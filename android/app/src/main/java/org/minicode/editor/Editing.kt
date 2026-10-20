package org.minicode.editor

/**
 * Indentation keys and line sorting, from the shared core through
 * app/src/main/cpp/editing_jni.cpp: src/AutoIndent.h decides what Enter,
 * Tab, Shift+Tab and Backspace do by the file's language, and
 * src/KeepSorted.h sorts lines the way Google's keep-sorted does. Offsets
 * are UTF-16 units, like the Editable's. Each result is one replacement, so
 * the text field's undo takes it back in one step.
 */
object Editing {
    init { System.loadLibrary("minicode") }

    private external fun nativeNewline(text: String, selStart: Int, selEnd: Int,
                                       filename: String): Array<Any>?
    private external fun nativeTyped(text: String, caret: Int, filename: String): Array<Any>?
    private external fun nativeKey(which: Int, text: String, selStart: Int, selEnd: Int,
                                   filename: String): Array<Any>?
    private external fun nativeSort(text: String, selStart: Int, selEnd: Int,
                                    all: Boolean): Array<Any>?

    private fun edit(parts: Array<Any>?): Core.TextEdit? {
        parts ?: return null
        val n = parts[0] as IntArray
        return Core.TextEdit(n[0], n[1], parts[1] as String, n[2], n[3])
    }

    /** Enter replacing [selStart, selEnd): the new line with its indentation. */
    fun newline(text: String, selStart: Int, selEnd: Int, filename: String) =
        edit(nativeNewline(text, selStart, selEnd, filename))

    /**
     * After a character was typed, with the caret just past it: a closer
     * alone on its line, or Python's ':' after else and its kin, moves the
     * line back. Null when nothing moves.
     */
    fun typed(text: String, caret: Int, filename: String) =
        edit(nativeTyped(text, caret, filename))

    /** Tab: indent the selected lines, or spaces to the next stop; null for a tab. */
    fun tab(text: String, selStart: Int, selEnd: Int, filename: String) =
        edit(nativeKey(0, text, selStart, selEnd, filename))

    /** Shift+Tab: outdent the lines the selection touches; null when none can. */
    fun backtab(text: String, selStart: Int, selEnd: Int, filename: String) =
        edit(nativeKey(1, text, selStart, selEnd, filename))

    /** Backspace in leading spaces: back to the tab stop; null for one character. */
    fun backspace(text: String, selStart: Int, selEnd: Int, filename: String) =
        edit(nativeKey(2, text, selStart, selEnd, filename))

    /** What sorting did: the edit (null when nothing changed) and what to say. */
    class Sorted(val edit: Core.TextEdit?, val message: String, val error: Boolean)

    /**
     * Sort Lines (`all` false): the selected lines, or with no selection the
     * keep-sorted block around the caret. With `all`, every keep-sorted
     * block in the file.
     */
    fun sort(text: String, selStart: Int, selEnd: Int, all: Boolean): Sorted {
        val parts = nativeSort(text, selStart, selEnd, all)
            ?: return Sorted(null, "", true)
        @Suppress("UNCHECKED_CAST")
        return Sorted(edit(parts[0] as Array<Any>?), parts[1] as String,
                      (parts[2] as IntArray)[0] != 0)
    }
}
