package org.minicode.editor

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.SecureRandom

/**
 * Programs installed in Termux, run on MiniCode's behalf.
 *
 * Android lets no app run a binary out of another app's private storage, so
 * clangd or tectonic installed with `pkg` cannot be started from here. What
 * Termux offers instead is RUN_COMMAND: another app may ask it to run a
 * command, once the user has allowed that in two places (the permission, and
 * `allow-external-apps=true` in ~/.termux/termux.properties).
 *
 * RUN_COMMAND itself only starts things; it has no pipes. So the command it
 * runs connects back. MiniCode listens on a loopback port, and the command
 * Termux runs is bash that opens that port with /dev/tcp, sends a one-time
 * token, and then execs the program with the connection as its stdin and
 * stdout. MiniCode accepts one connection, checks the token, and from then on
 * the socket is the program's pipes, the same as a local process would give.
 * No package beyond bash is needed, and a stranger on the phone that finds
 * the port cannot use it without the token.
 *
 * Paths are the same on both sides for shared storage (/storage/emulated/0),
 * which is why projects meant for these tools live there.
 */
object Termux {
    const val PACKAGE = "com.termux"
    const val PERMISSION = "com.termux.permission.RUN_COMMAND"
    const val PREFIX = "/data/data/com.termux/files/usr"
    const val HOME = "/data/data/com.termux/files/home"
    private const val BASH = "$PREFIX/bin/bash"

    fun isInstalled(context: Context): Boolean = try {
        context.packageManager.getPackageInfo(PACKAGE, 0); true
    } catch (e: PackageManager.NameNotFoundException) { false }

    fun hasPermission(context: Context): Boolean =
        context.checkSelfPermission(PERMISSION) == PackageManager.PERMISSION_GRANTED

    /** What is missing, in words for the user, or null when it can run. */
    fun problem(context: Context): String? = when {
        !isInstalled(context) -> "Termux is not installed."
        !hasPermission(context) -> "MiniCode is not allowed to run commands " +
                "in Termux yet."
        else -> null
    }

    /**
     * A program running in Termux, with its stdin and stdout on a socket.
     * `output` ends when the program exits; closing the process closes the
     * socket, which a language server reads as end of input and exits.
     */
    class Process internal constructor(private val socket: Socket) {
        val input: InputStream get() = socket.getInputStream()
        val output: OutputStream get() = socket.getOutputStream()
        fun close() = try { socket.close() } catch (e: Exception) {}
    }

    private val random = SecureRandom()

    /**
     * Starts `command` (a shell command line, run by Termux's bash in
     * `workdir`) and waits up to `timeoutMs` for it to connect back. Throws
     * with a readable message when it does not: Termux missing, the
     * permission refused, or allow-external-apps not set, which Termux
     * reports only in its own notification.
     *
     * Must not be called on the main thread; it blocks.
     */
    fun start(context: Context, command: String, workdir: String? = null,
              stderrTo: String? = null, timeoutMs: Int = 15000): Process {
        problem(context)?.let { throw IllegalStateException(it) }
        // 127.0.0.1 by name: on Android getLoopbackAddress() is ::1, and
        // bash's /dev/tcp/127.0.0.1 is refused there.
        val server = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))
        try {
            val token = ByteArray(16).also(random::nextBytes)
                .joinToString("") { "%02x".format(it) }
            val errors = stderrTo?.let { shellQuote(it) } ?: "/dev/null"
            // exec 3<> opens the connection; the token goes first so
            // MiniCode knows who is calling; the program then gets the
            // socket as stdin and stdout.
            val script = "exec 3<>/dev/tcp/127.0.0.1/${server.localPort} || exit 97; " +
                    "echo $token >&3; exec 2>>$errors; exec <&3 >&3 3>&-; " +
                    "exec $command"
            val intent = Intent().apply {
                setClassName(PACKAGE, "$PACKAGE.app.RunCommandService")
                action = "$PACKAGE.RUN_COMMAND"
                putExtra("$PACKAGE.RUN_COMMAND_PATH", BASH)
                putExtra("$PACKAGE.RUN_COMMAND_ARGUMENTS",
                         arrayOf("-lc", script))
                putExtra("$PACKAGE.RUN_COMMAND_WORKDIR", workdir ?: HOME)
                putExtra("$PACKAGE.RUN_COMMAND_BACKGROUND", true)
            }
            // Termux reports how the command ended through a PendingIntent.
            // A working command ends much later (it is the server), so this
            // is only read when nothing connects: it is the one place a
            // refusal or a failing bash says why.
            val result = java.util.concurrent.LinkedBlockingQueue<String>()
            val action = "org.minicode.editor.TERMUX_RESULT." + token
            val receiver = object : android.content.BroadcastReceiver() {
                override fun onReceive(c: Context, i: Intent) {
                    val b = i.getBundleExtra("result")
                    result.offer(if (b == null) "no result" else
                        "exit ${b.getInt("exitCode", -1)}; err ${b.getInt("err")} " +
                        "${b.getString("errmsg").orEmpty()}; " +
                        "stderr: ${b.getString("stderr").orEmpty().trim()}; " +
                        "stdout: ${b.getString("stdout").orEmpty().trim().take(300)}")
                }
            }
            androidx.core.content.ContextCompat.registerReceiver(context, receiver,
                android.content.IntentFilter(action),
                androidx.core.content.ContextCompat.RECEIVER_NOT_EXPORTED)
            val pending = android.app.PendingIntent.getBroadcast(context, 0,
                Intent(action).setPackage(context.packageName),
                android.app.PendingIntent.FLAG_MUTABLE or
                        android.app.PendingIntent.FLAG_ONE_SHOT)
            intent.putExtra("$PACKAGE.RUN_COMMAND_PENDING_INTENT", pending)
            context.startService(intent)

            server.soTimeout = timeoutMs
            val socket = try {
                server.accept()
            } catch (e: SocketTimeoutException) {
                val why = result.poll(2, java.util.concurrent.TimeUnit.SECONDS)
                android.util.Log.w("MiniCodeTermux", "no connection; termux said: $why")
                throw IllegalStateException(
                    if (why != null) "Termux ran the command but it did not " +
                            "connect back ($why)."
                    else "Termux did not run the command. Check that " +
                    "~/.termux/termux.properties has allow-external-apps=true " +
                    "(then run termux-reload-settings), and look for a " +
                    "notification from Termux.")
            } finally {
                try { context.unregisterReceiver(receiver) } catch (e: Exception) {}
            }
            socket.soTimeout = 5000
            val line = readLine(socket.getInputStream())
            if (line != token) {
                socket.close()
                throw IllegalStateException("An unexpected program connected.")
            }
            socket.soTimeout = 0
            return Process(socket)
        } finally {
            server.close()
        }
    }

    /**
     * Runs `command` to the end and returns its exit status and everything
     * it printed (stdout and stderr together), for one-shot jobs such as a
     * tectonic run or checking what is installed.
     */
    fun run(context: Context, command: String, workdir: String? = null,
            timeoutMs: Int = 15000): Pair<Int, String> {
        val marker = "__minicode_exit_"
        val process = start(context,
            "bash -c ${shellQuote("$command 2>&1; echo $marker\$?")}",
            workdir, timeoutMs = timeoutMs)
        try {
            val text = process.input.readBytes().toString(Charsets.UTF_8)
            val at = text.lastIndexOf(marker)
            if (at < 0) return -1 to text
            val status = text.substring(at + marker.length).trim().toIntOrNull() ?: -1
            return status to text.substring(0, at)
        } finally {
            process.close()
        }
    }

    /** Where a Termux command finds a program, or null if it is not installed. */
    fun which(context: Context, program: String): String? {
        val (status, out) = run(context, "command -v ${shellQuote(program)}")
        return out.trim().takeIf { status == 0 && it.isNotEmpty() }
    }

    fun shellQuote(s: String) = "'" + s.replace("'", "'\\''") + "'"

    private fun readLine(input: InputStream): String {
        val out = StringBuilder()
        while (true) {
            val b = input.read()
            if (b < 0 || b == '\n'.code) break
            out.append(b.toChar())
            if (out.length > 200) break
        }
        return out.toString()
    }

    /** True for a path both MiniCode and Termux can reach. */
    fun isShared(path: File) = path.path.startsWith("/storage/")
}
