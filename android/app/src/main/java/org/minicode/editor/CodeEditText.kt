package org.minicode.editor

import android.content.Context
import android.util.AttributeSet
import android.view.KeyEvent
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputConnectionWrapper
import androidx.appcompat.widget.AppCompatEditText

/**
 * The editor's text field, with two changes for code on a phone.
 *
 * Composition is refused, so the keyboard cannot gather letters into a word
 * and rewrite it: on a Titan 2 that turned one keypress into "xys" and made
 * code unreadable as it was typed.
 *
 * Committed text is offered to the activity first, which is how a shortcut's
 * letter is caught. A phone keyboard reaches this field through the input
 * method, so the letter never arrives as a key event while the cursor is
 * here, however the focus and the soft keyboard are arranged.
 *
 * With [autoIndent], Enter starts the new line with the indentation of the
 * one it splits, whether it comes as a key or as committed text.
 */
class CodeEditText @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null,
    defStyleAttr: Int = androidx.appcompat.R.attr.editTextStyle
) : AppCompatEditText(context, attrs, defStyleAttr) {

    /** Told when the caret moves; the language-server bar follows it. */
    var onSelection: ((Int) -> Unit)? = null

    /**
     * Offered committed text before it is inserted: the completion list
     * takes a newline as "accept" when the keyboard sends Enter as text.
     */
    var interceptCommit: ((CharSequence) -> Boolean)? = null

    /** Enter keeps the current line's indentation (the code editor's field). */
    var autoIndent = false

    private val squiggle = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
        style = android.graphics.Paint.Style.STROKE
        strokeWidth = resources.displayMetrics.density
    }

    override fun onSelectionChanged(selStart: Int, selEnd: Int) {
        super.onSelectionChanged(selStart, selEnd)
        onSelection?.invoke(selEnd)
    }

    /** The text, then language-server squiggles over it. */
    override fun onDraw(canvas: android.graphics.Canvas) {
        super.onDraw(canvas)
        LspSession.drawDiagnostics(this, canvas, squiggle)
    }

    /** "\n" plus the spaces and tabs that begin the caret's line, up to the caret. */
    private fun newlineWithIndent(): String {
        val t = text ?: return "\n"
        val caret = minOf(selectionStart, selectionEnd).coerceIn(0, t.length)
        var start = caret
        while (start > 0 && t[start - 1] != '\n') start--
        var end = start
        while (end < caret && (t[end] == ' ' || t[end] == '\t')) end++
        return "\n" + t.subSequence(start, end)
    }

    /** Replaces the selection with a newline and the indentation. */
    private fun insertNewline() {
        val t = text ?: return
        val a = minOf(selectionStart, selectionEnd).coerceAtLeast(0)
        val b = maxOf(selectionStart, selectionEnd).coerceAtLeast(0)
        val nl = newlineWithIndent()
        t.replace(a, b, nl)
        setSelection(a + nl.length)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (autoIndent && (keyCode == KeyEvent.KEYCODE_ENTER ||
                    keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER) &&
            !event.isCtrlPressed && !event.isAltPressed && !event.isMetaPressed) {
            insertNewline()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        val inner = super.onCreateInputConnection(outAttrs) ?: return null
        outAttrs.inputType = codeInputType(outAttrs.inputType)
        outAttrs.imeOptions = outAttrs.imeOptions or
                EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING or
                EditorInfo.IME_FLAG_NO_EXTRACT_UI
        return object : InputConnectionWrapper(inner, false) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                if (text?.length == 1) {
                    val activity = context as? MainActivity
                    if (activity?.leaderLetter(text[0]) == true) return true
                }
                if (text != null && interceptCommit?.invoke(text) == true) return true
                if (autoIndent && text?.toString() == "\n")
                    return super.commitText(newlineWithIndent(), newCursorPosition)
                return super.commitText(text, newCursorPosition)
            }

            // No composing region: a keyboard that cannot compose cannot
            // replace what was already typed.
            override fun setComposingText(text: CharSequence?, newCursorPosition: Int) =
                super.commitText(text, newCursorPosition)

            override fun setComposingRegion(start: Int, end: Int) = true
        }
    }

    companion object {
        /**
         * What the keyboard is told this field is, whatever the layout or
         * the caller set: plain text in the visible-password variation,
         * multi-line if the field is, with no suggestions and no automatic
         * capitals.
         *
         * NO_SUGGESTIONS alone is not enough. Physical-keyboard input
         * methods such as Pastiera keep auto-capitals, double space to
         * period and autocorrect on in every text field except password,
         * URI, email and filter ones (its InputContextState), so `if` at
         * the start of a line became `If`. The visible-password variation is
         * the least odd of those for code: nothing is hidden, the Titan 2's
         * own keyboard shows its usual layout and symbol panel, and the
         * field stays multi-line.
         */
        fun codeInputType(current: Int): Int {
            val keep = current and android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
            return keep or android.text.InputType.TYPE_CLASS_TEXT or
                    android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
                    android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }
    }
}
