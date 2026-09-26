package org.minicode.editor

import android.view.Gravity
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView

/**
 * A row of characters under the editor that a phone keyboard cannot type.
 *
 * The Titan 2's Alt layer has no #, backtick, braces, pipe or backslash, and
 * Android hides the on-screen keyboard (which has them) while a hardware
 * keyboard is attached, so a Markdown heading could not be written at all.
 * This is the terminal's key row (TerminalKeys) for the editor.
 *
 * A tap inserts at the caret through the editor's own text, so the edit is
 * highlighted, marks the file unsaved, updates a preview and can be undone
 * like typing. The keys never take focus, so the caret and the keyboard stay
 * in the editor.
 */
object EditorKeys {
    private val keys = listOf(
        "Tab" to "\t",
        "#" to "#", "*" to "*", "`" to "`", "_" to "_", "-" to "-",
        "[" to "[", "]" to "]", "(" to "(", ")" to ")", "{" to "{", "}" to "}",
        "<" to "<", ">" to ">", "|" to "|", "\\" to "\\", "/" to "/", "~" to "~",
        "=" to "=", "+" to "+", "\"" to "\"", "'" to "'", "!" to "!", "?" to "?",
        "@" to "@", "$" to "$", "%" to "%", "^" to "^", "&" to "&",
        ";" to ";", ":" to ":",
    )

    fun fill(row: LinearLayout, editor: EditText) {
        val dp = row.resources.displayMetrics.density
        row.setBackgroundColor(Palette.SIDEBAR)
        for ((label, text) in keys) {
            row.addView(TextView(row.context).apply {
                this.text = label
                textSize = 15f
                typeface = android.graphics.Typeface.MONOSPACE
                setTextColor(Palette.TEXT)
                gravity = Gravity.CENTER
                minWidth = (36 * dp).toInt()
                setPadding((6 * dp).toInt(), (9 * dp).toInt(),
                           (6 * dp).toInt(), (9 * dp).toInt())
                isFocusable = false
                isFocusableInTouchMode = false
                setOnClickListener { insert(editor, text) }
            })
        }
    }

    /** Replaces the selection, or inserts at the caret, as typing would. */
    private fun insert(editor: EditText, text: String) {
        val buffer = editor.text ?: return
        val a = minOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0)
        val b = maxOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0)
        buffer.replace(a, b, text)
        editor.setSelection((a + text.length).coerceAtMost(buffer.length))
    }
}
