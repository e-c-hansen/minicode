package org.minicode.editor

import android.content.Context
import android.util.AttributeSet
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

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        val inner = super.onCreateInputConnection(outAttrs) ?: return null
        outAttrs.inputType = outAttrs.inputType or
                android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
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
                return super.commitText(text, newCursorPosition)
            }

            // No composing region: a keyboard that cannot compose cannot
            // replace what was already typed.
            override fun setComposingText(text: CharSequence?, newCursorPosition: Int) =
                super.commitText(text, newCursorPosition)

            override fun setComposingRegion(start: Int, end: Int) = true
        }
    }
}
