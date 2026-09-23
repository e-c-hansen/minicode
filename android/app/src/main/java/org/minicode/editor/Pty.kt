package org.minicode.editor

/**
 * A shell on a pseudo terminal, with the shared TerminalScreen reading its
 * output. See app/src/main/cpp/terminal_jni.cpp.
 *
 * The shell is Android's own, /system/bin/sh, with the toybox utilities: ls,
 * cat, grep, ps, and enough to move around a project. Anything richer (git,
 * python, a compiler) lives in Termux, whose files this app cannot reach, so
 * pointing the terminal at a Termux shell is a later job.
 */
class Pty private constructor(private var handle: Long) {

    companion object {
        init { System.loadLibrary("minicode") }

        /** Null when no shell could be started. */
        fun start(shell: String, home: String, cols: Int, rows: Int): Pty? {
            val handle = nativeOpen(shell, home, cols, rows)
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

        @JvmStatic private external fun nativeOpen(
            shell: String, home: String, cols: Int, rows: Int): Long
        @JvmStatic private external fun nativePump(handle: Long, timeoutMs: Int): Int
        @JvmStatic private external fun nativeWrite(handle: Long, data: ByteArray)
        @JvmStatic private external fun nativeEncodeChar(codePoint: Int, mods: Int): ByteArray
        @JvmStatic private external fun nativeEncodeKey(handle: Long, key: Int, mods: Int): ByteArray
        @JvmStatic private external fun nativeResize(handle: Long, cols: Int, rows: Int)
        @JvmStatic private external fun nativeText(handle: Long): String
        @JvmStatic private external fun nativeColors(handle: Long): IntArray
        @JvmStatic private external fun nativeCursorRow(handle: Long): Int
        @JvmStatic private external fun nativeCursorCol(handle: Long): Int
        @JvmStatic private external fun nativeClose(handle: Long)
    }

    val isOpen: Boolean get() = handle != 0L

    /** Waits for output and feeds it to the screen. -1 once the shell exits. */
    fun pump(timeoutMs: Int): Int = if (handle == 0L) -1 else nativePump(handle, timeoutMs)

    fun write(data: ByteArray) { if (handle != 0L) nativeWrite(handle, data) }

    fun type(codePoint: Int, mods: Int = 0) =
        write(nativeEncodeChar(codePoint, mods))

    fun press(key: Int, mods: Int = 0) {
        if (handle != 0L) write(nativeEncodeKey(handle, key, mods))
    }

    fun resize(cols: Int, rows: Int) { if (handle != 0L) nativeResize(handle, cols, rows) }

    fun text(): String = if (handle == 0L) "" else nativeText(handle)
    fun colors(): IntArray = if (handle == 0L) IntArray(0) else nativeColors(handle)
    fun cursorRow(): Int = if (handle == 0L) 0 else nativeCursorRow(handle)
    fun cursorCol(): Int = if (handle == 0L) 0 else nativeCursorCol(handle)

    fun close() {
        if (handle != 0L) { nativeClose(handle); handle = 0 }
    }
}
