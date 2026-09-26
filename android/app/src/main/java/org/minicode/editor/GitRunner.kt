package org.minicode.editor

import android.content.Context
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.IOException

/**
 * Runs git in Termux for the Source Control panel: an argument vector in a
 * directory in, the exit status, stdout and stderr out, each kept apart.
 *
 * Starting a program through Termux's RUN_COMMAND takes a moment, and one
 * refresh of the panel runs up to five commands, so this keeps one small bash
 * loop running in Termux (started through Termux.start, like a language
 * server) and sends it one request at a time over the socket:
 *
 *     count NUL directory NUL arg1 NUL ... argN NUL
 *
 * bash reads each field with `read -r -d ''`, which keeps every byte but NUL,
 * so paths and commit messages never pass through a shell's word splitting,
 * globbing or quoting: they go straight into an array and from there to git
 * as its argv. The script itself holds nothing from the user. It answers
 *
 *     status SPACE stdout length SPACE stderr length LF, stdout, stderr
 *
 * after git has finished writing both into files in Termux's temporary
 * folder (so neither pipe can fill and stall git). Right after connecting it
 * says "ok", or "nogit" when Termux has no git, which becomes [Missing].
 *
 * Calls block and must be made off the main thread, one at a time; the
 * panel's single worker thread is the only caller.
 */
class GitRunner(private val context: Context) {

    class Result(val status: Int, val out: ByteArray, val err: ByteArray) {
        val ok get() = status == 0
        fun errText() = err.toString(Charsets.UTF_8)
    }

    /** Termux works but has no git. */
    class Missing : Exception("Install git in Termux: pkg install git")

    private var process: Termux.Process? = null
    private var input: DataInputStream? = null
    private var lastUse = 0L

    @Synchronized
    fun run(dir: String, args: List<ByteArray>): Result {
        // A connection that has sat idle may have been closed by Termux (or
        // the phone's memory manager) without a word; the first write or read
        // then fails before git ever ran, and one retry on a new connection
        // is safe. A failure on a new connection is reported.
        for (attempt in 0..1) {
            val reused = process != null
            val p = process ?: connect()
            val stream = input ?: DataInputStream(p.input.buffered()).also { input = it }
            var answered = false
            try {
                p.output.write(request(dir, args))
                p.output.flush()
                val header = readLine(stream)
                answered = true
                val parts = header.trim().split(' ')
                if (parts.size != 3) throw IOException("Unexpected answer from Termux: $header")
                val status = parts[0].toInt()
                val out = ByteArray(parts[1].toInt()).also { stream.readFully(it) }
                val err = ByteArray(parts[2].toInt()).also { stream.readFully(it) }
                lastUse = System.currentTimeMillis()
                return Result(status, out, err)
            } catch (e: IOException) {
                close()
                if (!reused || answered || attempt == 1) throw e
            }
        }
        throw IOException("unreachable")
    }

    /** Stops the loop in Termux; the next call starts it again. */
    @Synchronized
    fun close() {
        process?.close()
        process = null
        input = null
    }

    /** Milliseconds since the last command, for closing an idle loop. */
    @Synchronized
    fun idleFor() = if (process == null) 0L else System.currentTimeMillis() - lastUse

    private fun connect(): Termux.Process {
        val env = GitNative.environment().joinToString(" ") { Termux.shellQuote(it) }
        val loop = """
            t=${'$'}(mktemp -d "${'$'}{TMPDIR:-${Termux.PREFIX}/tmp}/minicode-git.XXXXXX") || exit 96
            trap 'rm -rf "${'$'}t"' EXIT
            export $env
            if command -v git >/dev/null 2>&1; then echo ok; else echo nogit; exit 0; fi
            while IFS= read -r -d '' n; do
              IFS= read -r -d '' dir || break
              args=()
              i=0
              while [ "${'$'}i" -lt "${'$'}n" ]; do
                IFS= read -r -d '' a || exit 0
                args+=("${'$'}a")
                i=${'$'}((i + 1))
              done
              ( cd -- "${'$'}dir" && exec git "${'$'}{args[@]}" ) </dev/null >"${'$'}t/o" 2>"${'$'}t/e"
              s=${'$'}?
              printf '%s %s %s\n' "${'$'}s" "${'$'}(wc -c <"${'$'}t/o")" "${'$'}(wc -c <"${'$'}t/e")"
              cat "${'$'}t/o" "${'$'}t/e"
            done
        """.trimIndent()
        val p = Termux.start(context, "bash -c ${Termux.shellQuote(loop)}")
        val stream = DataInputStream(p.input.buffered())
        // Git on a slow phone, a large history or a commit hook can take a
        // while; a minute without a byte means something is stuck.
        p.setReadTimeout(60_000)
        val hello = try { readLine(stream) } catch (e: IOException) { p.close(); throw e }
        if (hello != "ok") {
            p.close()
            if (hello == "nogit") throw Missing()
            throw IOException("Termux did not start the git helper ($hello).")
        }
        process = p
        input = stream
        lastUse = System.currentTimeMillis()
        return p
    }

    private fun request(dir: String, args: List<ByteArray>): ByteArray {
        val out = ByteArrayOutputStream()
        fun field(b: ByteArray) {
            // A NUL would end the field early; git's arguments are C strings
            // and cannot hold one anyway.
            for (x in b) if (x != 0.toByte()) out.write(x.toInt())
            out.write(0)
        }
        field(args.size.toString().toByteArray())
        field(dir.toByteArray(Charsets.UTF_8))
        args.forEach(::field)
        return out.toByteArray()
    }

    private fun readLine(input: DataInputStream): String {
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
