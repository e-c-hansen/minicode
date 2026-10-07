package org.minicode.editor

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextUtils
import android.text.style.ForegroundColorSpan
import android.util.AttributeSet
import android.util.Log
import android.view.KeyEvent
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import java.io.File
import java.util.concurrent.Executors

/**
 * The TODO list, in the file list's place as source control is: every TODO,
 * FIXME, HACK, XXX and BUG comment in the project, and every open task
 * ("- [ ] ...") in its Markdown files, found by the core's
 * FolderSearch::findTodos (the walk, limits and order of Find in Folder).
 *
 * The folder is read again each time the pane is shown, on one worker
 * thread; a scan still running when another starts, or when the pane goes,
 * is cancelled, and its answer dropped. Rows read `path:line  text`. Up and
 * Down move, Enter (or a tap) opens the file at that line.
 */
class TodoPane @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : LinearLayout(context, attrs) {

    /** A row chosen: open its file at its line. */
    var onOpen: ((Core.Todo) -> Unit)? = null

    private val dp = resources.displayMetrics.density
    private fun px(v: Int) = (v * dp).toInt()

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "MiniCodeTodos").apply { isDaemon = true }
    }
    /** Bumped by every scan and by stop(), so a late answer is dropped. */
    private var generation = 0
    private var items: List<Core.Todo> = emptyList()
    private var sel = -1

    private val status = TextView(context).apply {
        textSize = 12f
        setTextColor(Palette.MUTED)
        setPadding(px(10), px(6), px(10), px(6))
    }
    private val list = RecyclerView(context).apply {
        layoutManager = LinearLayoutManager(context)
        isFocusable = false
        descendantFocusability = FOCUS_BLOCK_DESCENDANTS
        itemAnimator = null
    }

    init {
        orientation = VERTICAL
        setBackgroundColor(Palette.SIDEBAR)
        isFocusable = true
        isFocusableInTouchMode = true
        // The pane holds the keyboard itself (the rows are not focused), and
        // Android would grey a focused view with no focused look of its own.
        defaultFocusHighlightEnabled = false
        addView(status, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        addView(list, LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))
        list.adapter = Rows()
    }

    /**
     * Reads `root` again, or says `why` not when there is no root. The rows
     * already shown stay until the answer comes, so the list does not
     * flicker empty on every visit.
     */
    fun scan(root: File?, why: String?) {
        val gen = ++generation
        Core.cancelTodos()
        if (root == null) {
            show(emptyList(), why ?: "Open a folder first.")
            return
        }
        status.text = if (items.isEmpty()) "Looking for TODOs…" else status.text
        val started = android.os.SystemClock.uptimeMillis()
        worker.execute {
            val found = try { Core.todosIn(root.path) } catch (e: Exception) { null }
            main.post {
                if (gen != generation) return@post
                val ms = android.os.SystemClock.uptimeMillis() - started
                if (Log.isLoggable(TAG, Log.DEBUG))
                    Log.d(TAG, "${root.path}: ${found?.items?.size} in ${found?.filesMatched} of " +
                            "${found?.filesRead} files, $ms ms")
                when {
                    found == null -> show(emptyList(), "Could not read ${root.name}.")
                    found.items.isEmpty() -> show(emptyList(),
                        "Nothing to do: no TODO, FIXME, HACK, XXX or BUG comments, " +
                                "and no open tasks in Markdown files.")
                    else -> show(found.items, summary(found))
                }
            }
        }
    }

    /** The pane went away: any scan still running stops. */
    fun stop() {
        generation++
        Core.cancelTodos()
    }

    fun shutdown() {
        stop()
        worker.shutdownNow()
    }

    private fun summary(t: Core.Todos): String {
        val n = t.items.size
        val files = t.filesMatched
        var s = "$n " + (if (n == 1) "item" else "items") + " in $files " +
                (if (files == 1) "file" else "files")
        if (t.truncated) s += ", stopped at the first $n"
        return s
    }

    private fun show(found: List<Core.Todo>, message: String) {
        // The selection stays on the same item if it is still there.
        val was = items.getOrNull(sel)
        items = found
        sel = when {
            found.isEmpty() -> -1
            was != null -> found.indexOfFirst { it.path == was.path && it.line == was.line }
                .let { if (it >= 0) it else sel.coerceIn(0, found.size - 1) }
            else -> 0
        }
        status.text = message
        list.adapter?.notifyDataSetChanged()
        if (sel >= 0) list.scrollToPosition(sel)
    }

    /** Showing the pane: the keyboard comes here. */
    fun focusPanel() {
        requestFocus()
        post { requestFocus() }
    }

    override fun onFocusChanged(gained: Boolean, direction: Int, previous: android.graphics.Rect?) {
        super.onFocusChanged(gained, direction, previous)
        list.adapter?.notifyDataSetChanged()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action != KeyEvent.ACTION_DOWN) {
            return if (swallow.remove(event.keyCode)) true else super.dispatchKeyEvent(event)
        }
        if (key(event)) { swallow.add(event.keyCode); return true }
        return super.dispatchKeyEvent(event)
    }
    private val swallow = mutableSetOf<Int>()

    private fun key(e: KeyEvent): Boolean {
        if (e.isCtrlPressed || e.isAltPressed || e.isMetaPressed) return false
        val n = items.size
        val page = maxOf(1, list.height / maxOf(1, px(ROW_H)) - 1)
        val to = when (e.keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> sel - 1
            KeyEvent.KEYCODE_DPAD_DOWN -> sel + 1
            KeyEvent.KEYCODE_PAGE_UP -> sel - page
            KeyEvent.KEYCODE_PAGE_DOWN, KeyEvent.KEYCODE_SPACE -> sel + page
            KeyEvent.KEYCODE_MOVE_HOME -> 0
            KeyEvent.KEYCODE_MOVE_END -> n - 1
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER, KeyEvent.KEYCODE_DPAD_CENTER -> {
                items.getOrNull(sel)?.let { onOpen?.invoke(it) }
                return true
            }
            else -> return false
        }
        if (n == 0) return true
        select(to.coerceIn(0, n - 1))
        return true
    }

    private fun select(row: Int) {
        val old = sel
        sel = row
        list.adapter?.notifyItemChanged(old)
        list.adapter?.notifyItemChanged(row)
        list.scrollToPosition(row)
    }

    /** `path:line` muted, then the line's text. */
    private fun rowText(t: Core.Todo): CharSequence {
        val s = SpannableStringBuilder()
        s.append("${t.path}:${t.line}")
        s.setSpan(ForegroundColorSpan(Palette.MUTED), 0, s.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        s.append("  ")
        s.append(t.text)
        return s
    }

    private inner class Rows : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        override fun getItemCount() = items.size
        override fun onCreateViewHolder(parent: ViewGroup, type: Int): RecyclerView.ViewHolder {
            val v = TextView(context).apply {
                textSize = 13f
                setTextColor(Palette.TEXT)
                maxLines = 2
                ellipsize = TextUtils.TruncateAt.END
                minHeight = px(ROW_H)
                gravity = android.view.Gravity.CENTER_VERTICAL
                setPadding(px(10), px(4), px(10), px(4))
                layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                         ViewGroup.LayoutParams.WRAP_CONTENT)
            }
            val holder = object : RecyclerView.ViewHolder(v) {}
            v.setOnClickListener {
                val pos = holder.bindingAdapterPosition
                val t = items.getOrNull(pos) ?: return@setOnClickListener
                select(pos)
                requestFocus()
                onOpen?.invoke(t)
            }
            v.isFocusable = false
            return holder
        }
        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            val v = holder.itemView as TextView
            v.text = rowText(items[position])
            v.setBackgroundColor(when {
                position != sel -> Palette.SIDEBAR
                hasFocus() -> SELECTED
                else -> SELECTED_INACTIVE
            })
        }
    }

    companion object {
        private const val TAG = "MiniCodeTodos"
        private const val ROW_H = 36
        // The source control panel's selection colors.
        private val SELECTED = 0xFF04395E.toInt()
        private val SELECTED_INACTIVE = 0xFF37373D.toInt()
    }
}
