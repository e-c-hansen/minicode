package org.minicode.editor

import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView

/**
 * A row of keys under the terminal for what a phone keyboard cannot type.
 *
 * The Titan 2 has no Esc, Tab or Ctrl, and its Alt layer has no pipe,
 * backslash or backtick, which a shell needs constantly. The keyboard app's
 * own symbol picker does not help either: the terminal declares TYPE_NULL
 * (see TerminalView), so there is no text field for the picker to type into.
 * Termux answers this with the same kind of row, and so does this.
 *
 * Ctrl is sticky: tap it, then a letter. It lights up while it waits. The
 * keys never take focus, so the keyboard stays with the terminal.
 */
object TerminalKeys {
    private sealed class Key(val label: String) {
        class Text(label: String, val text: String = label) : Key(label)
        class Special(label: String, val code: Int) : Key(label)
        object Ctrl : Key("Ctrl")
    }

    private val keys = listOf(
        Key.Special("Esc", Pty.KEY_ESCAPE),
        Key.Special("Tab", Pty.KEY_TAB),
        Key.Ctrl,
        Key.Text("|"), Key.Text("~"), Key.Text("/"), Key.Text("-"),
        Key.Text("\\"), Key.Text("`"), Key.Text("&"), Key.Text(">"),
        Key.Text("<"), Key.Text("{"), Key.Text("}"), Key.Text("["), Key.Text("]"),
        Key.Special("←", Pty.KEY_LEFT), Key.Special("↓", Pty.KEY_DOWN),
        Key.Special("↑", Pty.KEY_UP), Key.Special("→", Pty.KEY_RIGHT),
    )

    fun fill(row: LinearLayout, terminal: TerminalView) {
        val dp = row.resources.displayMetrics.density
        row.setBackgroundColor(Palette.SIDEBAR)
        var ctrlView: TextView? = null
        for (key in keys) {
            val view = TextView(row.context).apply {
                text = key.label
                textSize = 15f
                typeface = android.graphics.Typeface.MONOSPACE
                setTextColor(Palette.TEXT)
                gravity = Gravity.CENTER
                minWidth = (40 * dp).toInt()
                setPadding((8 * dp).toInt(), (10 * dp).toInt(),
                           (8 * dp).toInt(), (10 * dp).toInt())
                isFocusable = false
                isFocusableInTouchMode = false
                setOnClickListener {
                    when (key) {
                        is Key.Text -> terminal.typeText(key.text)
                        is Key.Special -> terminal.pressKey(key.code)
                        Key.Ctrl -> terminal.ctrlArmed = !terminal.ctrlArmed
                    }
                }
            }
            if (key == Key.Ctrl) ctrlView = view
            row.addView(view)
        }
        terminal.onCtrlChanged = { armed ->
            ctrlView?.setTextColor(if (armed) Palette.BACKGROUND else Palette.TEXT)
            ctrlView?.setBackgroundColor(if (armed) Palette.ACCENT else 0)
        }
    }

}
