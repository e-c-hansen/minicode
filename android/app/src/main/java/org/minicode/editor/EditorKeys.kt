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

    /**
     * `newline` puts a New line key first, for the Markdown preview's edit
     * box, where Enter saves: Shift+Enter is the other way, but not every
     * phone keyboard lets an app see Shift.
     */
    fun fill(row: LinearLayout, editor: EditText, newline: Boolean = false) {
        val dp = row.resources.displayMetrics.density
        row.setBackgroundColor(Palette.SIDEBAR)
        // A word: the ↵ glyph draws tiny in the phone's fonts.
        val all = if (newline) listOf("New line" to "\n") + keys else keys
        for ((label, text) in all) {
            row.addView(TextView(row.context).apply {
                this.text = label
                textSize = 15f
                typeface = android.graphics.Typeface.MONOSPACE
                setTextColor(Palette.TEXT)
                gravity = Gravity.CENTER
                minWidth = (36 * dp).toInt()
                setPadding((6 * dp).toInt(), (KEY_PADDING_DP * dp).toInt(),
                           (6 * dp).toInt(), (KEY_PADDING_DP * dp).toInt())
                isFocusable = false
                isFocusableInTouchMode = false
                setOnClickListener { insert(editor, text) }
            })
        }
    }

    /** A key's padding above and below its label. */
    const val KEY_PADDING_DP = 9f

    /** Replaces the selection, or inserts at the caret, as typing would. */
    private fun insert(editor: EditText, text: String) {
        val buffer = editor.text ?: return
        val a = minOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0)
        val b = maxOf(editor.selectionStart, editor.selectionEnd).coerceAtLeast(0)
        buffer.replace(a, b, text)
        editor.setSelection((a + text.length).coerceAtMost(buffer.length))
    }
}

/**
 * Keeps a row of keys along the bottom of the screen clear of its rounded
 * corners. The Titan 2's display is rounded with a 100 pixel radius, and a
 * row flush with the bottom edge lost the outer halves of its first and
 * last keys to the curve.
 *
 * The row is lifted a few dp, and its ends are padded by as much as the
 * corner cuts in at the height of the keys' text, worked out from the
 * corners the system reports (WindowInsets.getRoundedCorner, Android 12 and
 * later), so a phone with square corners loses nothing. Without that
 * information a small fixed margin is used. The padding is inside the
 * scrolling row, so the first and last keys can still be scrolled to.
 */
object CurvedEdges {
    fun keepClear(scroll: android.view.View, row: LinearLayout, keyPaddingDp: Float) {
        val dp = row.resources.displayMetrics.density
        val lift = (LIFT_DP * dp).toInt()
        row.setPadding(row.paddingLeft, row.paddingTop, row.paddingRight, lift)
        scroll.addOnLayoutChangeListener { v, _, _, _, _, _, _, _, _ ->
            val (left, right) = margins(v, lift + keyPaddingDp * dp, dp)
            if (left != row.paddingLeft || right != row.paddingRight) {
                // Not in the middle of this layout pass.
                row.post { row.setPadding(left, row.paddingTop, right, lift) }
            }
        }
    }

    /** Left and right padding for a row whose text ends `above` px over its bottom. */
    private fun margins(v: android.view.View, above: Float, dp: Float): Pair<Int, Int> {
        val spare = (2 * dp).toInt()
        val insets = v.rootWindowInsets
        if (android.os.Build.VERSION.SDK_INT < 31 || insets == null) {
            val fixed = (FALLBACK_DP * dp).toInt()
            return fixed to fixed
        }
        val at = IntArray(2)
        v.getLocationInWindow(at)
        val y = at[1] + v.height - above
        var left = 0
        var right = 0
        fun cut(position: Int): Pair<Float, android.view.RoundedCorner>? {
            val c = insets.getRoundedCorner(position) ?: return null
            val dy = y - c.center.y
            if (dy <= 0 || c.radius <= 0) return null
            val r = c.radius.toFloat()
            val x = if (dy >= r) r else r - kotlin.math.sqrt(r * r - dy * dy)
            return x to c
        }
        cut(android.view.RoundedCorner.POSITION_BOTTOM_LEFT)?.let { (x, c) ->
            val edge = c.center.x - c.radius
            left = maxOf(0, (edge + x - at[0]).toInt() + spare)
        }
        cut(android.view.RoundedCorner.POSITION_BOTTOM_RIGHT)?.let { (x, c) ->
            val edge = c.center.x + c.radius
            right = maxOf(0, (at[0] + v.width - (edge - x)).toInt() + spare)
        }
        return left to right
    }

    private const val LIFT_DP = 4f
    private const val FALLBACK_DP = 12f
}
