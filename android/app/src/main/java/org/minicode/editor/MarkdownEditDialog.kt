package org.minicode.editor

import android.content.Context
import android.graphics.Typeface
import android.util.AttributeSet
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputConnectionWrapper
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.AppCompatEditText

/**
 * The box a double tap in the Markdown preview opens: the Markdown source of
 * one block (a heading's text, a list item, a paragraph, a quote, a code
 * block or a table cell), as the Mac's popover and the Linux one hold it.
 *
 * Enter saves and Shift+Enter starts a new line, as on the desktop. A phone
 * keyboard may send Enter as text rather than as a key, and may not let an
 * app see Shift at all, so the row of symbols under the box starts with a
 * New line key that always works. A list item also offers Add item,
 * which turns the box into one for a new item below it.
 *
 * Nothing is edited here: `save` gets the text and whether it is a new item,
 * and the caller splices it into the source.
 */
object MarkdownEditDialog {

    fun show(context: Context, what: String, text: String, listItem: Boolean,
             save: (text: String, adding: Boolean) -> Unit) {
        val dp = context.resources.displayMetrics.density
        var adding = false

        val field = BlockField(context).apply {
            setText(text)
            typeface = Typeface.MONOSPACE
            textSize = 14f
            setTextColor(Palette.TEXT)
            setBackgroundColor(Palette.BACKGROUND)
            minLines = 2
            maxLines = 8
            isVerticalScrollBarEnabled = true
            gravity = android.view.Gravity.TOP or android.view.Gravity.START
            val pad = (8 * dp).toInt()
            setPadding(pad, pad, pad, pad)
        }
        val hint = TextView(context).apply {
            this.text = "Markdown. Enter saves; Shift+Enter or New line below starts a new line."
            setTextColor(Palette.MUTED)
            textSize = 12f
            setPadding(0, 0, 0, (6 * dp).toInt())
        }
        val keys = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        EditorKeys.fill(keys, field, newline = true)
        val keyScroll = HorizontalScrollView(context).apply {
            isHorizontalScrollBarEnabled = false
            addView(keys)
        }
        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            val side = (16 * dp).toInt()
            setPadding(side, (4 * dp).toInt(), side, 0)
            addView(hint)
            addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                      LinearLayout.LayoutParams.WRAP_CONTENT))
            addView(keyScroll, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = (4 * dp).toInt() })
        }

        val builder = AlertDialog.Builder(context)
            .setTitle("Editing $what")
            .setView(box)
            .setPositiveButton("Save", null)
            .setNegativeButton("Cancel", null)
        if (listItem) builder.setNeutralButton("Add item", null)
        val dialog = builder.create()

        fun commit() {
            val t = field.text?.toString().orEmpty()
            dialog.dismiss()
            save(t, adding)
        }
        field.onEnter = { commit() }

        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener { commit() }
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL)?.setOnClickListener { button ->
                // The same box, now for a new entry below this one.
                adding = true
                dialog.setTitle("New list item")
                field.setText("")
                button.visibility = View.GONE
                field.requestFocus()
            }
            field.requestFocus()
            field.setSelection(field.text?.length ?: 0)
        }
        dialog.window?.setSoftInputMode(
            WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE or
                    WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)
        dialog.show()
    }

    /**
     * The edit box. Enter saves, whether it arrives as a key or as a newline
     * the keyboard commits; with Shift held (as far as the box can tell) it
     * is a new line. Composition is refused, as in the editor (CodeEditText),
     * so a keyboard cannot rewrite what was typed.
     */
    class BlockField @JvmOverloads constructor(
        context: Context, attrs: AttributeSet? = null,
        defStyleAttr: Int = androidx.appcompat.R.attr.editTextStyle
    ) : AppCompatEditText(context, attrs, defStyleAttr) {

        var onEnter: (() -> Unit)? = null
        private var shift = false

        init {
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                    android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                    android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                    android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        }

        override fun dispatchKeyEvent(event: KeyEvent): Boolean {
            if (event.keyCode == KeyEvent.KEYCODE_SHIFT_LEFT ||
                event.keyCode == KeyEvent.KEYCODE_SHIFT_RIGHT) {
                shift = event.action == KeyEvent.ACTION_DOWN
            } else {
                shift = event.isShiftPressed
            }
            val enter = event.keyCode == KeyEvent.KEYCODE_ENTER ||
                    event.keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER
            if (enter && !event.isShiftPressed) {
                if (event.action == KeyEvent.ACTION_UP) onEnter?.invoke()
                return true
            }
            return super.dispatchKeyEvent(event)
        }

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
            val inner = super.onCreateInputConnection(outAttrs) ?: return null
            outAttrs.imeOptions = outAttrs.imeOptions or
                    EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING or
                    EditorInfo.IME_FLAG_NO_EXTRACT_UI
            return object : InputConnectionWrapper(inner, false) {
                override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                    if (text != null && text.toString() == "\n" && !shift) {
                        onEnter?.invoke()
                        return true
                    }
                    return super.commitText(text, newCursorPosition)
                }

                override fun setComposingText(text: CharSequence?, newCursorPosition: Int) =
                    super.commitText(text, newCursorPosition)

                override fun setComposingRegion(start: Int, end: Int) = true
            }
        }
    }
}
