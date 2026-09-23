package org.minicode.editor

import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * A shell on a pseudo terminal, with the shared TerminalScreen reading its
 * output. See app/src/main/cpp/terminal_jni.cpp.
 *
 * The shell is Android's own, /system/bin/sh, with the toybox utilities: ls,
 * cat, grep, ps, and enough to move around a project. Anything richer (git,
 * python, a compiler) lives in Termux, whose files this app cannot reach, so
 * pointing the terminal at a Termux shell is a later job.
 *
 * Threads: everything here is for the UI thread except the reader, which
 * startReading owns. The native session is freed only once both the owner
 * (close) and the reader have let go of it, so closing while output is
 * streaming cannot pull the screen out from under the reader.
 */
class Pty private constructor(private val session: Long) {

    companion object {
        init { System.loadLibrary("minicode") }

        /** Null when no shell could be started. */
        fun start(shell: String, home: String, cwd: String, cols: Int, rows: Int): Pty? {
            val handle = nativeOpen(shell, home, cwd, cols, rows)
            return if (handle == 0L) null else Pty(handle)
        }

        /**
         * The ordinals of TermKey in src/TerminalScreen.h, in its order:
         * Up, Down, Right, Left, Home, End, PageUp, PageDown, Insert,
         * Delete, F1 to F12, Enter, KeypadEnter, Backspace, Tab, Escape.
         */
        const val KEY_UP = 0
        const val KEY_DOWN = 1
        const val KEY_RIGHT = 2
        const val KEY_LEFT = 3
        const val KEY_HOME = 4
        const val KEY_END = 5
        const val KEY_PAGE_UP = 6
        const val KEY_PAGE_DOWN = 7
        const val KEY_INSERT = 8
        const val KEY_DELETE = 9
        const val KEY_ENTER = 22
        const val KEY_KEYPAD_ENTER = 23
        const val KEY_BACKSPACE = 24
        const val KEY_TAB = 25
        const val KEY_ESCAPE = 26

        /** TermMod in src/TerminalScreen.h. */
        const val MOD_SHIFT = 1
        const val MOD_ALT = 2
        const val MOD_CTRL = 4

        /** The flag bits of a cell in a Screen, from nativeSnapshot. */
        const val CELL_BOLD = 1
        const val CELL_INVERSE = 2
        const val CELL_WIDE = 4

        @JvmStatic private external fun nativeOpen(
            shell: String, home: String, cwd: String, cols: Int, rows: Int): Long
        @JvmStatic private external fun nativePump(handle: Long, timeoutMs: Int): Int
        @JvmStatic private external fun nativeReaderDone(handle: Long)
        @JvmStatic private external fun nativeWrite(handle: Long, data: ByteArray)
        @JvmStatic private external fun nativeEncodeChar(codePoint: Int, mods: Int): ByteArray
        @JvmStatic private external fun nativeEncodeKey(handle: Long, key: Int, mods: Int): ByteArray
        @JvmStatic private external fun nativeResize(handle: Long, cols: Int, rows: Int)
        @JvmStatic private external fun nativeSnapshot(handle: Long): IntArray
        @JvmStatic private external fun nativeClose(handle: Long)
    }

    /**
     * One frame of the screen, copied in a single call. `cells` holds four
     * ints per cell, row by row: code point (0 for the right half of a wide
     * character), foreground, background (0xAARRGGBB, 0 for the terminal's
     * default) and CELL_* flags.
     */
    class Screen(data: IntArray) {
        val rows = if (data.size >= 5) data[0] else 0
        val cols = if (data.size >= 5) data[1] else 0
        val cursorRow = if (data.size >= 5) data[2] else 0
        val cursorCol = if (data.size >= 5) data[3] else 0
        val cursorVisible = data.size >= 5 && data[4] != 0
        private val cells = data

        private fun at(row: Int, col: Int) = 5 + (row * cols + col) * 4
        fun codePoint(row: Int, col: Int) = cells[at(row, col)]
        fun foreground(row: Int, col: Int) = cells[at(row, col) + 1]
        fun background(row: Int, col: Int) = cells[at(row, col) + 2]
        fun flags(row: Int, col: Int) = cells[at(row, col) + 3]
    }

    @Volatile private var open = true
    private var reader: Thread? = null
    private val readerReleased = AtomicBoolean(false)

    val isOpen: Boolean get() = open

    /**
     * Reads output on a thread of its own until the shell exits or the Pty
     * is closed. `onChange` and `onExit` run on that thread; `onExit` only
     * when the shell ended by itself, not after close().
     */
    fun startReading(onChange: () -> Unit, onExit: () -> Unit) {
        if (reader != null || !open) return
        reader = thread(name = "minicode-pty", isDaemon = true) {
            while (open) {
                when (nativePump(session, 200)) {
                    -1 -> break
                    1 -> onChange()
                }
            }
            val closedByOwner = !open
            releaseReader()
            if (!closedByOwner) onExit()
        }
    }

    private fun releaseReader() {
        if (readerReleased.compareAndSet(false, true)) nativeReaderDone(session)
    }

    fun write(data: ByteArray) { if (open) nativeWrite(session, data) }

    fun type(codePoint: Int, mods: Int = 0) =
        write(nativeEncodeChar(codePoint, mods))

    fun press(key: Int, mods: Int = 0) {
        if (open) write(nativeEncodeKey(session, key, mods))
    }

    fun resize(cols: Int, rows: Int) { if (open) nativeResize(session, cols, rows) }

    fun snapshot(): Screen = Screen(if (open) nativeSnapshot(session) else IntArray(0))

    fun close() {
        if (!open) return
        open = false
        nativeClose(session)
        // A reader that never started still holds its reference.
        if (reader == null) releaseReader()
    }
}
