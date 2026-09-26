package org.minicode.editor

import android.content.Context
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.StateListDrawable
import android.util.AttributeSet
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * What the window shows while no folder is open: a line saying so, then one
 * large row per thing that can be done from here (open a folder, a recent
 * folder, the terminal, the shortcuts).
 *
 * Before it existed the app opened on an empty file list titled "MiniCode",
 * with nothing to tap, and a leader press turned that into an empty editor
 * whose typing went nowhere. Every row is a touch target and also takes the
 * keyboard's focus, so the arrows move between rows and Enter picks one.
 */
class StartScreen @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null,
) : ScrollView(context, attrs) {

    /** A row: a label, an optional muted second line, and what it does. */
    class Item(val label: String, val detail: String? = null,
               val heading: Boolean = false, val action: (() -> Unit)? = null)

    private val column = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private val dp = resources.displayMetrics.density

    init {
        isFillViewport = true
        addView(column, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
    }

    fun show(message: String, items: List<Item>) {
        column.removeAllViews()
        column.addView(TextView(context).apply {
            text = message
            textSize = 14f
            setTextColor(Palette.MUTED)
            setPadding(px(16), px(16), px(16), px(10))
        })
        for (item in items) column.addView(if (item.heading) heading(item) else row(item))
    }

    /** Puts the keyboard on the first row, so Enter works straight away. */
    fun focusFirst() {
        for (i in 0 until column.childCount) {
            val v = column.getChildAt(i)
            if (v.isFocusable) { v.requestFocus(); return }
        }
    }

    private fun heading(item: Item) = TextView(context).apply {
        text = item.label
        textSize = 12f
        setTextColor(Palette.MUTED)
        setPadding(px(16), px(14), px(16), px(4))
    }

    private fun row(item: Item): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(px(16), px(10), px(16), px(10))
        minimumHeight = px(52)
        gravity = android.view.Gravity.CENTER_VERTICAL
        background = rowBackground()
        isFocusable = true
        isClickable = true
        defaultFocusHighlightEnabled = false
        setOnClickListener { item.action?.invoke() }
        addView(TextView(context).apply {
            text = item.label
            textSize = 16f
            setTextColor(Palette.ACCENT)
        })
        item.detail?.let { d ->
            addView(TextView(context).apply {
                text = d
                textSize = 12f
                setTextColor(Palette.MUTED)
                isSingleLine = true
                ellipsize = android.text.TextUtils.TruncateAt.START
            })
        }
    }

    private fun px(v: Int) = (v * dp).toInt()

    companion object {
        /** The look of a focused or pressed row, shared with the file list. */
        fun rowBackground() = StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_pressed), ColorDrawable(0xFF37373D.toInt()))
            addState(intArrayOf(android.R.attr.state_focused), ColorDrawable(0xFF04395E.toInt()))
            addState(intArrayOf(), ColorDrawable(0))
        }
    }
}
