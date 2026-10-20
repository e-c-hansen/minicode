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
 * With [autoIndent], Enter, Tab, Shift+Tab and Backspace follow the open
 * file's language ([fileName]) through the core's AutoIndent (Editing.kt):
 * Enter indents after a Python colon or an opening bracket and opens a
 * bracket pair, Tab inserts spaces where the file indents with them, and a
 * closer typed alone on a line moves back to its opener. Enter does this
 * whether it comes as a key or as committed text. With [markdownLists] as
 * well, Enter in a list item first asks the core (MarkdownTasks::newline) to
 * continue the list: the next item's marker ("- [ ] ", "4. "), or the list
 * ended on an empty item.
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

    /** Enter, Tab and Backspace follow the indentation rules (the code editor's field). */
    var autoIndent = false

    /** The open file's name: its language decides the indentation rules. */
    var fileName = ""

    /** Enter continues a Markdown list (set while a Markdown file is open). */
    var markdownLists = false

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

    /**
     * Enter in a Markdown list item, as the core decides it: one
     * replacement and the caret after it. False when the line is not one,
     * and the usual Enter should run.
     */
    private fun continueList(): Boolean {
        if (!markdownLists) return false
        val t = text ?: return false
        val edit = Core.taskNewline(t.toString(), selectionStart, selectionEnd) ?: return false
        val (a, b) = edit.applyTo(t)
        setSelection(a, b)
        return true
    }

    private fun selA() = minOf(selectionStart, selectionEnd).coerceAtLeast(0)
    private fun selB() = maxOf(selectionStart, selectionEnd).coerceAtLeast(0)

    /** One replacement from the core and the selection after it; false for none. */
    private fun apply(edit: Core.TextEdit?): Boolean {
        edit ?: return false
        val t = text ?: return false
        val (a, b) = edit.applyTo(t)
        setSelection(a, b)
        return true
    }

    /** Replaces the selection with a newline and the indentation. */
    private fun insertNewline() {
        if (continueList()) return
        val t = text ?: return
        if (apply(Editing.newline(t.toString(), selA(), selB(), fileName))) return
        val a = selA()
        val nl = newlineWithIndent()
        t.replace(a, selB(), nl)
        setSelection(a + nl.length)
    }

    /** Tab, or Shift+Tab with `outdent`: true when the core did it. */
    private fun indentKey(outdent: Boolean): Boolean {
        val t = text?.toString() ?: return false
        return apply(if (outdent) Editing.backtab(t, selA(), selB(), fileName)
                     else Editing.tab(t, selA(), selB(), fileName))
    }

    /** Backspace in leading spaces: back to the tab stop. False for one character. */
    private fun softBackspace(): Boolean {
        val t = text ?: return false
        val caret = selectionStart
        if (caret != selectionEnd || caret <= 0 || t[caret - 1] != ' ') return false
        return apply(Editing.backspace(t.toString(), caret, caret, fileName))
    }

    /** After `c` was typed: a closer alone on its line, or Python's ':' ending else, moves back. */
    private fun electric(c: Char) {
        if (c != ':' && c != ')' && c != ']' && c != '}') return
        val t = text ?: return
        val caret = selectionStart
        if (caret != selectionEnd || caret <= 0 || caret > t.length || t[caret - 1] != c) return
        apply(Editing.typed(t.toString(), caret, fileName))
    }

    /**
     * A key of the symbol row (EditorKeys): Tab as the Tab key, anything
     * else typed at the caret, a closer then moving its line if it should.
     * False when the field has no indentation rules, for a plain insert.
     */
    fun keyRowInsert(key: String): Boolean {
        if (!autoIndent) return false
        if (key == "\t" && indentKey(false)) return true
        val t = text ?: return false
        val a = selA()
        t.replace(a, selB(), key)
        setSelection((a + key.length).coerceAtMost(t.length))
        if (key.length == 1) electric(key[0])
        return true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val plain = !event.isCtrlPressed && !event.isAltPressed && !event.isMetaPressed
        if (autoIndent && plain) {
            when (keyCode) {
                KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> {
                    insertNewline()
                    return true
                }
                // Shift+Tab never moves the focus out of the editor.
                KeyEvent.KEYCODE_TAB ->
                    if (indentKey(event.isShiftPressed) || event.isShiftPressed) return true
                KeyEvent.KEYCODE_DEL -> if (softBackspace()) return true
            }
        }
        val handled = super.onKeyDown(keyCode, event)
        if (autoIndent && handled) {
            val c = event.getUnicodeChar(event.metaState)
            if (c > 0) electric(c.toChar())
        }
        return handled
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
                if (autoIndent && text?.toString() == "\n") {
                    beginBatchEdit()
                    try { insertNewline() } finally { endBatchEdit() }
                    return true
                }
                if (autoIndent && text?.toString() == "\t") {
                    beginBatchEdit()
                    val done = try { indentKey(false) } finally { endBatchEdit() }
                    if (done) return true
                }
                val committed = super.commitText(text, newCursorPosition)
                if (autoIndent && text?.length == 1) electric(text[0])
                return committed
            }

            // Backspace from a keyboard that deletes rather than sending the key.
            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                if (autoIndent && beforeLength == 1 && afterLength == 0) {
                    beginBatchEdit()
                    val done = try { softBackspace() } finally { endBatchEdit() }
                    if (done) return true
                }
                return super.deleteSurroundingText(beforeLength, afterLength)
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
