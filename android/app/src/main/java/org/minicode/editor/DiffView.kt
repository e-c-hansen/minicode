package org.minicode.editor

import android.content.Context
import android.graphics.Typeface
import android.util.AttributeSet
import android.view.KeyEvent
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView

/**
 * A diff or a commit from the source control panel, in the editor's place:
 * one row per line, so only the lines on screen are laid out. A single
 * TextView holding a 500 KB diff took several seconds to lay out on the
 * phone, with the window frozen meanwhile; this shows the core's full 4 MB
 * at once.
 *
 * Keys, since it holds the keyboard while shown: Up and Down scroll by a
 * line, Space and Page Down by a screen (Shift+Space and Page Up back), and
 * Home and End go to either end.
 */
class DiffView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : RecyclerView(context, attrs) {

    private var lines: List<CharSequence> = emptyList()
    private val dp = resources.displayMetrics.density

    init {
        layoutManager = LinearLayoutManager(context)
        adapter = Lines()
        itemAnimator = null
        isFocusable = true
        isFocusableInTouchMode = true
        defaultFocusHighlightEnabled = false
        setBackgroundColor(Palette.BACKGROUND)
        setPadding((8 * dp).toInt(), (8 * dp).toInt(), (8 * dp).toInt(), (8 * dp).toInt())
        clipToPadding = false
        isVerticalScrollBarEnabled = true
    }

    /** Shows `text`, split into lines (the spans come along), from the top. */
    fun show(text: List<CharSequence>) {
        lines = text
        adapter?.notifyDataSetChanged()
        scrollToPosition(0)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val line = (17 * dp).toInt()
        val page = (height * 0.9f).toInt()
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_DOWN -> scrollBy(0, line)
            KeyEvent.KEYCODE_DPAD_UP -> scrollBy(0, -line)
            KeyEvent.KEYCODE_SPACE -> scrollBy(0, if (event.isShiftPressed) -page else page)
            KeyEvent.KEYCODE_PAGE_DOWN -> scrollBy(0, page)
            KeyEvent.KEYCODE_PAGE_UP -> scrollBy(0, -page)
            KeyEvent.KEYCODE_MOVE_HOME -> scrollToPosition(0)
            KeyEvent.KEYCODE_MOVE_END -> scrollToPosition(maxOf(0, lines.size - 1))
            else -> return super.onKeyDown(keyCode, event)
        }
        return true
    }

    private inner class Lines : Adapter<ViewHolder>() {
        override fun getItemCount() = lines.size
        override fun onCreateViewHolder(parent: ViewGroup, type: Int): ViewHolder {
            val t = TextView(context).apply {
                typeface = Typeface.MONOSPACE
                textSize = 12f
                includeFontPadding = false
                setLineSpacing(2 * dp, 1f)
                setPadding(0, dp.toInt(), 0, dp.toInt())
                setTextColor(Palette.TEXT)
                layoutParams = LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                            ViewGroup.LayoutParams.WRAP_CONTENT)
            }
            return object : ViewHolder(t) {}
        }
        override fun onBindViewHolder(holder: ViewHolder, position: Int) {
            (holder.itemView as TextView).text = lines[position]
        }
    }
}
