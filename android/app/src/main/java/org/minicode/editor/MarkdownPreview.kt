package org.minicode.editor

import android.content.Context
import android.graphics.Rect
import android.graphics.Typeface
import android.graphics.drawable.AnimatedImageDrawable
import android.graphics.drawable.Drawable
import android.graphics.ImageDecoder
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.LeadingMarginSpan
import android.text.style.RelativeSizeSpan
import android.text.style.StyleSpan
import android.text.style.TypefaceSpan
import android.text.style.UnderlineSpan
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.documentfile.provider.DocumentFile

/**
 * The Markdown preview: the core's runs (MarkdownParser, through
 * minicode_jni.cpp) laid out as a column of views, as the Mac and Linux
 * previews lay them out as one text view with tables and pictures inside.
 *
 * - Text between tables and pictures is one TextView, styled with spans.
 * - A table is a grid of wrapping cells, the columns sharing the width
 *   equally, as the Linux port draws them.
 * - A local picture is shown at its own size or the pane's width, whichever
 *   is smaller; a GIF plays while the preview is on screen. Web pictures and
 *   files over 64 MB show their alt text instead.
 *
 * Every stretch of text remembers the source line it came from, which is
 * what a tapped link, a double tap to edit and the preview/source toggle all
 * go through. Parsing and deciding are the core's; only the look is here.
 */
class MarkdownPreview @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : ScrollView(context, attrs) {

    /** A tapped link's target, as written in the source. */
    var onLink: ((String) -> Unit)? = null

    /**
     * A double tap on a block: its 0-based source line, and for a table cell
     * its column (-1 otherwise). For a cell the line is the cell's row.
     */
    var onEditBlock: ((line: Int, column: Int) -> Unit)? = null

    /** A path as a picture's source names it (relative to the file) -> the file. */
    var resolve: ((String) -> DocumentFile?)? = null

    /** Text size of the body text, in sp. */
    var textSizeSp = 15f

    private val dp = resources.displayMetrics.density
    private val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }

    /** Heading anchor -> the text view and the character it starts at. */
    private val anchors = HashMap<String, Pair<PieceText, Int>>()
    /** The first source line of each table and picture, for place keeping. */
    private val blockLines = HashMap<View, Int>()
    private val playing = ArrayList<AnimatedImageDrawable>()
    private val pictures = HashMap<String, Picture>()
    /** Where the preview was left after it was placed; a scroll after that is the user's. */
    private var entryScroll = -1
    /**
     * False until construction is done: View's constructor already reports
     * the visibility from the layout file, before these fields exist.
     */
    private var ready = false

    /** A stretch of rendered text: [start, end) of its view's text. */
    private class Piece(val start: Int, val end: Int, val line: Int, val url: String?)

    /** A table cell: the source line of its row, and its column. */
    private class Cell(val line: Int, val column: Int)

    private class Picture(val drawable: Drawable, val width: Int, val height: Int)

    init {
        setBackgroundColor(Palette.BACKGROUND)
        isFillViewport = true
        val pad = (10 * dp).toInt()
        body.setPadding(pad, pad, pad, pad)
        addView(body, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        ready = true
    }

    // ------------------------------------------------------------ rendering

    /**
     * Shows `source`. With `keepScroll` the page stays where it was, which is
     * what an edit made from the preview wants; otherwise it starts at the top.
     */
    fun show(source: String, keepScroll: Boolean) {
        val keep = scrollY
        render(source)
        afterLayout {
            scrollTo(0, if (keepScroll) keep else 0)
            // An edit keeps the page where the reader took it, so a scroll
            // made before the edit still counts as theirs.
            if (!keepScroll) entryScroll = scrollY
        }
    }

    private fun render(source: String) {
        stopAll()
        playing.clear()
        body.removeAllViews()
        anchors.clear()
        blockLines.clear()
        val used = HashMap<String, Picture>()
        val runs = Core.markdownRuns(source)

        var text = SpannableStringBuilder()
        var view = PieceText(null)
        var heading = StringBuilder()
        var headingLine = -1
        var headingAt = 0

        fun endHeading() {
            if (headingLine < 0) return
            val key = Core.mdAnchor(heading.toString())
            var unique = key
            var n = 1
            while (anchors.containsKey(unique)) unique = "$key-${n++}"
            if (unique.isNotEmpty()) anchors[unique] = view to headingAt
            headingLine = -1
            heading = StringBuilder()
        }

        // The text gathered so far becomes a view, and a fresh one starts.
        fun flush() {
            endHeading()
            // A block's own newline at the end would show as an empty line
            // above the table or picture that follows; the margins space them.
            var end = text.length
            while (end > 0 && text[end - 1] == '\n') end--
            if (end > 0) {
                text.delete(end, text.length)
                view.text = text
                view.pieces.removeAll { it.start >= end }
                addBlock(view)
            }
            text = SpannableStringBuilder()
            view = PieceText(null)
        }

        var i = 0
        while (i < runs.size) {
            val flags = runs.flags[i]
            val line = runs.line(i)
            val level = flags and Core.MD_HEADING
            if (level > 0 && line != headingLine) {
                endHeading()
                headingLine = line
                headingAt = text.length
            } else if (level == 0) {
                endHeading()
            }
            if (level > 0) heading.append(runs.text[i])

            // A table: every run with its id, as one grid.
            val table = runs.tableId(i)
            if (table > 0) {
                var end = i
                while (end < runs.size && runs.tableId(end) == table) end++
                flush()
                val grid = buildTable(runs, i, end)
                blockLines[grid] = runs.line(i)
                addBlock(grid)
                i = end
                continue
            }
            // A picture that can be shown; one that cannot shows its alt text.
            if (runs.isImage(i)) {
                val picture = runs.src(i)?.let { picture(it, used) }
                if (picture != null) {
                    flush()
                    val image = pictureView(picture, runs.url(i), line)
                    blockLines[image] = line
                    addBlock(image)
                    i++
                    continue
                }
            }

            var piece = runs.text[i]
            if (runs.isImage(i) && piece.isEmpty()) piece = runs.src(i).orEmpty()
            if (flags and Core.MD_RULE != 0) piece = "————————\n"
            // Space between blocks is the views' margins, not blank lines.
            if (text.isEmpty()) piece = piece.trimStart('\n')
            if (piece.isNotEmpty()) {
                val start = text.length
                text.append(piece)
                style(text, start, text.length, flags, runs.isImage(i))
                view.pieces.add(Piece(start, text.length, line, runs.url(i)))
            }
            i++
        }
        flush()
        pictures.clear()
        pictures.putAll(used)
    }

    private fun addBlock(v: View) {
        val lp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                           LinearLayout.LayoutParams.WRAP_CONTENT)
        if (body.childCount > 0) lp.topMargin = (8 * dp).toInt()
        if (v is ImageView) lp.width = LinearLayout.LayoutParams.WRAP_CONTENT
        body.addView(v, lp)
    }

    /** One run's look, from its flags. */
    private fun style(out: SpannableStringBuilder, start: Int, end: Int, flags: Int, image: Boolean) {
        if (end <= start) return
        fun span(what: Any) = out.setSpan(what, start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        val heading = flags and Core.MD_HEADING
        val depth = Core.mdListDepth(flags)
        if (flags and Core.MD_RULE != 0) {
            span(ForegroundColorSpan(Palette.DIVIDER))
            return
        }
        if (heading > 0) {
            // 1.6x down to 1.0x, so a phone keeps its lines readable.
            span(RelativeSizeSpan(1f + (0.6f - 0.1f * (heading - 1)).coerceAtLeast(0f)))
            span(StyleSpan(Typeface.BOLD))
            span(ForegroundColorSpan(Palette.MD_HEADING))
        }
        if (flags and Core.MD_BOLD != 0) span(StyleSpan(Typeface.BOLD))
        if (flags and Core.MD_ITALIC != 0) span(StyleSpan(Typeface.ITALIC))
        if (flags and (Core.MD_CODE or Core.MD_CODE_BLOCK) != 0) {
            span(TypefaceSpan("monospace"))
            span(ForegroundColorSpan(Palette.MD_CODE))
            span(RelativeSizeSpan(0.9f))
        }
        if (flags and Core.MD_QUOTE != 0) {
            span(ForegroundColorSpan(Palette.MD_QUOTE))
            span(StyleSpan(Typeface.ITALIC))
        }
        if (image && flags and Core.MD_LINK == 0) span(ForegroundColorSpan(Palette.MUTED))
        if (flags and Core.MD_LINK != 0) {
            span(ForegroundColorSpan(Palette.MD_LINK))
            span(UnderlineSpan())
        }
        if (depth > 0) span(LeadingMarginSpan.Standard((depth * 16 * dp).toInt()))
    }

    // ------------------------------------------------------------ tables

    /**
     * Runs [first, end) of one table as a grid: equal columns across the
     * pane, each cell wrapping inside its own, the header row shaded. The
     * padding and rule runs a monospace table needs (column -1) are left out.
     */
    private fun buildTable(runs: Core.MarkdownRuns, first: Int, end: Int): View {
        val columns = maxOf(1, runs.tableCols(first))
        var rows = 0
        for (k in first until end) rows = maxOf(rows, runs.tableRow(k) + 1)
        val line = runs.line(first)
        val px = maxOf(1, dp.toInt())
        val grid = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Palette.DIVIDER)   // shows between the cells
            setPadding(px, px, px, px)
        }
        for (row in 0 until rows) {
            val rowView = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            for (col in 0 until columns) {
                // The separator row sits between the header and the first body row.
                val cell = PieceText(Cell(line + row + (if (row > 0) 1 else 0), col))
                val text = SpannableStringBuilder()
                var align = 0
                for (k in first until end) {
                    if (runs.tableRow(k) != row || runs.tableCol(k) != col) continue
                    align = runs.tableAlign(k)
                    var t = runs.text[k]
                    if (runs.isImage(k) && t.isEmpty()) t = runs.src(k).orEmpty()
                    val s = text.length
                    text.append(t)
                    style(text, s, text.length, runs.flags[k], runs.isImage(k))
                    cell.pieces.add(Piece(s, text.length, cell.cell!!.line, runs.url(k)))
                }
                cell.text = text
                cell.gravity = Gravity.TOP or when (align) {
                    2 -> Gravity.END
                    1 -> Gravity.CENTER_HORIZONTAL
                    else -> Gravity.START
                }
                val h = (4 * dp).toInt()
                val w = (6 * dp).toInt()
                cell.setPadding(w, h, w, h)
                cell.setBackgroundColor(if (row == 0) Palette.SIDEBAR else Palette.BACKGROUND)
                if (row == 0) cell.setTypeface(cell.typeface, Typeface.BOLD)
                val lp = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f)
                if (col < columns - 1) lp.rightMargin = px
                rowView.addView(cell, lp)
            }
            val lp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                               LinearLayout.LayoutParams.WRAP_CONTENT)
            if (row < rows - 1) lp.bottomMargin = px
            grid.addView(rowView, lp)
        }
        return grid
    }

    // ------------------------------------------------------------ pictures

    /**
     * The picture a Markdown source names, decoded no wider than the pane:
     * a path relative to the file, an absolute path or a file:// address.
     * Web pictures are not fetched; they, files over 64 MB and files that do
     * not decode give null, and the alt text is shown. Decoded pictures are
     * kept for the next render (an edit re-renders), keyed by file, date
     * and width; `used` collects this render's.
     */
    private fun picture(srcIn: String, used: HashMap<String, Picture>): Picture? {
        var src = srcIn.trim()
        if (src.startsWith("file://")) {
            src = android.net.Uri.parse(src).path ?: return null
        } else {
            if (src.contains("://") || src.startsWith("data:")) return null
            src = src.substringBefore('#').substringBefore('?')
            src = android.net.Uri.decode(src)
        }
        if (src.isEmpty()) return null
        val file = try { resolve?.invoke(src) } catch (e: Exception) { null } ?: return null
        if (!file.isFile || file.length() > MAX_PICTURE_BYTES) return null
        val maxWidth = maxOf(1, availableWidth())
        var key = "${file.uri}|${file.lastModified()}|$maxWidth"
        // The same picture twice on a page needs two drawables.
        var n = 0
        while (used.containsKey("$key|$n")) n++
        key = "$key|$n"
        pictures[key]?.let { used[key] = it; return it }
        return try {
            var w = 0
            var h = 0
            val source = ImageDecoder.createSource(context.contentResolver, file.uri)
            val drawable = ImageDecoder.decodeDrawable(source) { decoder, info, _ ->
                w = info.size.width
                h = info.size.height
                // Never larger than the pane, and never blown up past its own size.
                if (w > maxWidth) {
                    h = maxOf(1, (h.toLong() * maxWidth / w).toInt())
                    w = maxWidth
                    decoder.setTargetSize(w, h)
                }
            }
            if (w <= 0 || h <= 0) null
            else Picture(drawable, w, h).also { used[key] = it }
        } catch (e: Exception) {
            null
        } catch (e: OutOfMemoryError) {
            null
        }
    }

    private fun pictureView(picture: Picture, url: String?, line: Int): View {
        val image = ImageView(context)
        image.scaleType = ImageView.ScaleType.FIT_XY
        image.setImageDrawable(picture.drawable)
        image.layoutParams = LinearLayout.LayoutParams(picture.width, picture.height)
        (picture.drawable as? AnimatedImageDrawable)?.let {
            playing.add(it)
            if (isShown) it.start()
        }
        val taps = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                url?.let { onLink?.invoke(it) }
                return true
            }
            override fun onDoubleTap(e: MotionEvent): Boolean {
                if (line >= 0) onEditBlock?.invoke(line, -1)
                return true
            }
        })
        @Suppress("ClickableViewAccessibility")
        image.setOnTouchListener { _, e -> taps.onTouchEvent(e) }
        image.contentDescription = "Picture"
        return image
    }

    private fun availableWidth(): Int {
        val w = if (width > 0) width else resources.displayMetrics.widthPixels
        return w - body.paddingLeft - body.paddingRight
    }

    /** GIFs play only while the preview can be seen. */
    override fun onVisibilityChanged(changedView: View, visibility: Int) {
        super.onVisibilityChanged(changedView, visibility)
        if (!ready) return
        if (isShown) playing.forEach { it.start() } else stopAll()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        super.onWindowVisibilityChanged(visibility)
        if (!ready) return
        if (visibility == View.VISIBLE && isShown) playing.forEach { it.start() } else stopAll()
    }

    private fun stopAll() = playing.forEach { it.stop() }

    // ------------------------------------------------------------ taps

    /**
     * A block of text that knows which source line each stretch came from:
     * a single tap on a link opens it, a double tap edits the block (or the
     * table cell) under it. Not selectable, because a selectable TextView
     * takes a double tap to select a word.
     */
    private inner class PieceText(val cell: Cell?) : TextView(context) {
        val pieces = ArrayList<Piece>()

        private val taps = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                linkAt(e.x, e.y)?.let { onLink?.invoke(it) }
                return true
            }
            override fun onDoubleTap(e: MotionEvent): Boolean {
                if (cell != null) {
                    onEditBlock?.invoke(cell.line, cell.column)
                } else {
                    val line = lineAt(e.x, e.y)
                    if (line >= 0) onEditBlock?.invoke(line, -1)
                }
                return true
            }
        })

        init {
            setTextColor(Palette.TEXT)
            setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, textSizeSp)
            setLineSpacing(0f, 1.15f)
        }

        @Suppress("ClickableViewAccessibility")
        override fun onTouchEvent(event: MotionEvent): Boolean {
            taps.onTouchEvent(event)
            return true
        }

        /** The character under (x, y), or -1 past the end of a line. */
        fun charAt(x: Float, y: Float, strict: Boolean): Int {
            val l = layout ?: return -1
            val line = l.getLineForVertical((y - totalPaddingTop + scrollY).toInt())
            val fx = x - totalPaddingLeft + scrollX
            if (strict && (fx < l.getLineLeft(line) || fx > l.getLineRight(line))) return -1
            var off = l.getOffsetForHorizontal(line, fx)
            // The nearest boundary may be the one after the character tapped.
            if (off > l.getLineStart(line) && l.getPrimaryHorizontal(off) > fx) off--
            return off
        }

        fun linkAt(x: Float, y: Float): String? {
            val off = charAt(x, y, strict = true)
            if (off < 0) return null
            return pieces.firstOrNull { off >= it.start && off < it.end }?.url
        }

        /** The source line under (x, y): the stretch there, or the nearest before it. */
        fun lineAt(x: Float, y: Float): Int {
            val off = charAt(x, y, strict = false)
            if (off < 0) return -1
            val at = pieces.lastOrNull { it.start <= off && it.line >= 0 }
                ?: pieces.firstOrNull { it.line >= 0 }
            return at?.line ?: -1
        }

        /** The source line of the first stretch at or after `offset`. */
        fun lineFrom(offset: Int): Int =
            (pieces.firstOrNull { it.end > offset && it.line >= 0 }
                ?: pieces.lastOrNull { it.line >= 0 })?.line ?: -1
    }

    // ------------------------------------------------------------ place keeping

    /** The y, in this view's scrolling coordinates, of `offset` in `view`. */
    private fun yOf(view: PieceText, offset: Int): Int {
        val r = Rect()
        offsetDescendantRectToMyCoords(view, r)
        val l = view.layout ?: return r.top
        return r.top + view.totalPaddingTop + l.getLineTop(l.getLineForOffset(offset))
    }

    private fun topOf(view: View): Int {
        val r = Rect()
        offsetDescendantRectToMyCoords(view, r)
        return r.top
    }

    /**
     * The source line of what is at the top of the pane, read a little below
     * the edge, since a jump to a heading leaves a sliver of the line above.
     */
    fun topLine(): Int {
        val top = scrollY + (8 * dp).toInt()
        for (k in 0 until body.childCount) {
            val child = body.getChildAt(k)
            val y = topOf(child)
            if (y + child.height <= top) continue
            if (child is PieceText) {
                val l = child.layout ?: return child.lineFrom(0)
                val line = l.getLineForVertical(maxOf(0, top - y - child.totalPaddingTop))
                return child.lineFrom(l.getLineStart(line))
            }
            return blockLines[child] ?: continue
        }
        return -1
    }

    /**
     * Scrolls so the rendering of source `line` (or what follows it) sits
     * `fraction` of the way down the pane; runs once the page is laid out.
     */
    fun scrollToLine(line: Int, fraction: Float) {
        afterLayout {
            var y = 0
            if (line > 0) {
                y = -1
                for (k in 0 until body.childCount) {
                    val child = body.getChildAt(k)
                    if (child is PieceText) {
                        val p = child.pieces.firstOrNull { it.line >= line } ?: continue
                        y = yOf(child, p.start)
                    } else {
                        if ((blockLines[child] ?: -1) < line) continue
                        y = topOf(child)
                    }
                    break
                }
                if (y < 0) y = body.height
                y = (y - fraction * height).toInt()
            }
            scrollTo(0, maxOf(0, y))
            entryScroll = scrollY
        }
    }

    /**
     * "#section": scrolls to the heading with that GitHub anchor, once the
     * page is laid out (a link from another file asks before it is).
     */
    fun scrollToAnchor(anchorIn: String): Boolean {
        val key = android.net.Uri.decode(anchorIn).lowercase()
        if (!anchors.containsKey(key)) return false
        afterLayout {
            val (view, offset) = anchors[key] ?: return@afterLayout
            smoothScrollTo(0, maxOf(0, yOf(view, offset) - (4 * dp).toInt()))
        }
        return true
    }

    /** Whether the reader has scrolled since the preview was placed. */
    fun scrolledByUser() = entryScroll >= 0 && kotlin.math.abs(scrollY - entryScroll) > 2 * dp

    /** Runs `action` once the page's views have their sizes. */
    private fun afterLayout(action: () -> Unit) {
        if (!body.isLayoutRequested && body.isLaidOut && !isLayoutRequested) {
            action()
            return
        }
        body.addOnLayoutChangeListener(object : OnLayoutChangeListener {
            override fun onLayoutChange(v: View, l: Int, t: Int, r: Int, b: Int,
                                        ol: Int, ot: Int, or: Int, ob: Int) {
                body.removeOnLayoutChangeListener(this)
                // The scroll view clamps to its content only after its own layout.
                post(action)
            }
        })
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        if (ready) stopAll()
    }

    companion object {
        const val MAX_PICTURE_BYTES = 64L * 1024 * 1024
    }
}
