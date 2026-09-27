package org.minicode.editor

import android.content.Context
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.security.MessageDigest
import java.util.zip.ZipFile

/**
 * Termux's bash in the terminal pane, on a real pseudo terminal.
 *
 * The phone's own shell (/system/bin/sh, in this app's sandbox) cannot reach
 * anything installed in Termux, so python, git and the compilers are not
 * there. Termux will run a command for MiniCode (Termux.start, which language
 * servers and git use too), but what it starts is connected to a socket, not
 * a terminal: no prompt editing, no Ctrl C, and no vim or less. So what
 * Termux is asked to run is a small helper, `termux_pty.c`, that opens a pty
 * inside Termux, starts bash on it, and relays between the pty and the
 * socket. The window size travels on the same socket in a frame of its own.
 *
 * The helper is built with the app and carried in the APK as
 * lib/arm64-v8a/libminicode_pty.so. Termux cannot run a file out of another
 * app's storage, but it can run its own files, so the first time (and after
 * each update that changes the helper) the bytes go over the socket into
 * Termux's temporary folder, named by their hash, and Termux runs them from
 * there. Nothing beyond Termux's base system is needed: bash, head, chmod.
 *
 * bash starts with an rc file (also written into Termux's temporary folder)
 * that enters the open folder, then runs Termux's usual bash.bashrc and the
 * user's ~/.bashrc, and adds a PROMPT_COMMAND that reports the directory
 * (OSC 7) and that a command finished (OSC 133;D). The prompt, PATH and HOME
 * are Termux's own.
 */
object TermuxShell {

    /** The helper's bytes and their hash, read from the APK once. */
    private var helper: Pair<ByteArray, String>? = null

    private fun helper(context: Context): Pair<ByteArray, String> {
        helper?.let { return it }
        val name = "libminicode_pty.so"
        // Extracted when the APK was installed that way, otherwise read
        // straight out of the APK, where it is stored uncompressed.
        val extracted = File(context.applicationInfo.nativeLibraryDir, name)
        val bytes = if (extracted.isFile) extracted.readBytes() else
            ZipFile(context.applicationInfo.sourceDir).use { zip ->
                val entry = zip.getEntry("lib/arm64-v8a/$name")
                    ?: throw IOException("The terminal helper is missing from the app.")
                zip.getInputStream(entry).use { it.readBytes() }
            }
        val hash = MessageDigest.getInstance("SHA-256").digest(bytes)
            .take(8).joinToString("") { "%02x".format(it) }
        return (bytes to hash).also { helper = it }
    }

    /**
     * The rc file bash starts with. Fixed text: the folder to enter comes in
     * MINICODE_DIR, so nothing of the user's is ever written into a script.
     */
    private val RC = """
        # Written by MiniCode for its terminal pane; rewritten on every start.
        if [ -n "${'$'}MINICODE_DIR" ]; then
          cd -- "${'$'}MINICODE_DIR" 2>/dev/null ||
            printf '\033[90mTermux cannot open %s. Run termux-setup-storage in Termux, then close and reopen this terminal.\033[0m\n' "${'$'}MINICODE_DIR"
        fi
        unset MINICODE_DIR
        [ -r "${'$'}PREFIX/etc/bash.bashrc" ] && . "${'$'}PREFIX/etc/bash.bashrc"
        [ -r "${'$'}HOME/.bashrc" ] && . "${'$'}HOME/.bashrc"
        __minicode_prompt() {
          local s=${'$'}?
          printf '\033]133;D;%s\007\033]7;file://localhost%s\007' "${'$'}s" "${'$'}{PWD//%/%25}"
          return ${'$'}s
        }
        PROMPT_COMMAND="__minicode_prompt${'$'}{PROMPT_COMMAND:+;${'$'}PROMPT_COMMAND}"
    """.trimIndent() + "\n"

    /**
     * Starts bash in Termux, in `dir` when it is given (a path in shared
     * storage), at `cols` by `rows`. Blocks for as long as Termux takes, so
     * never on the main thread. Throws with a message for the user when
     * Termux will not run it.
     */
    fun start(context: Context, dir: String?, cols: Int, rows: Int): Pty {
        val (bytes, hash) = helper(context)
        val q = Termux::shellQuote
        // The helper is installed only when this version of it is not there
        // yet, and older versions are removed. head -c reads exactly the
        // helper's bytes and no further, so nothing typed later is lost.
        val script = """
            d="${'$'}{TMPDIR:-${Termux.PREFIX}/tmp}"
            f="${'$'}d/minicode-pty-$hash"
            rc="${'$'}d/minicode-bashrc"
            if [ -x "${'$'}f" ]; then echo have; else
              echo need
              mkdir -p "${'$'}d" && head -c ${bytes.size} > "${'$'}f.${'$'}${'$'}" &&
                chmod 700 "${'$'}f.${'$'}${'$'}" && mv -f "${'$'}f.${'$'}${'$'}" "${'$'}f" || { echo fail; exit 1; }
              for old in "${'$'}d"/minicode-pty-*; do
                case "${'$'}old" in "${'$'}f"|*.*) ;; *) rm -f "${'$'}old" ;; esac
              done
            fi
            printf '%s' @RC@ >"${'$'}rc.${'$'}${'$'}" && mv -f "${'$'}rc.${'$'}${'$'}" "${'$'}rc" || { echo fail; exit 1; }
            echo go
            ${if (dir != null) "export MINICODE_DIR=${q(dir)}" else ":"}
            exec "${'$'}f" $cols $rows bash --rcfile "${'$'}rc" -i
        """.trimIndent().replace("@RC@", q(RC))
        val process = Termux.start(context, "bash -c ${q(script)}")
        try {
            process.setReadTimeout(15000)
            when (val first = readLine(process.input)) {
                "have" -> {}
                "need" -> {
                    process.output.write(bytes)
                    process.output.flush()
                }
                else -> throw IOException("Termux answered \"$first\".")
            }
            val ready = readLine(process.input)
            if (ready != "go") throw IOException(
                if (ready == "fail") "Termux could not write to its temporary folder."
                else "Termux answered \"$ready\".")
            val pty = Pty.attach(process.detachFd(), cols, rows)
                ?: throw IOException("The terminal could not take the connection.")
            return pty
        } catch (e: Exception) {
            process.close()
            throw e
        }
    }

    /** One line, read a byte at a time so nothing after it is taken. */
    private fun readLine(input: InputStream): String {
        val out = StringBuilder()
        while (true) {
            val b = input.read()
            if (b < 0) throw IOException("Termux closed the connection.")
            if (b == '\n'.code) break
            out.append(b.toChar())
            if (out.length > 200) throw IOException("Unexpected answer from Termux.")
        }
        return out.toString()
    }
}
