package org.minicode.editor

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Point
import android.graphics.Typeface
import android.graphics.pdf.PdfRenderer
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.documentfile.provider.DocumentFile
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import java.io.File
import java.util.concurrent.Executors

/** The click-to-source half of the core, through latex_jni.cpp. */
object LatexNative {
    init { System.loadLibrary("minicode") }

    external fun open(): Long
    external fun close(handle: Long)
    external fun loadSyncTex(handle: Long, path: String): Boolean
    external fun tagForPath(handle: Long, path: String): Int
    /** {start, end, kind, itemIndex} in UTF-16 units of `source`, or null. */
    external fun spanAt(handle: Long, source: String, tag: Int, page: Int,
                        x: Double, y: Double, word: String,
                        before: String, after: String): IntArray?
    external fun hasKey(text: String): Boolean

    /** LatexSpanKind, in the order of src/LatexDoc.h. */
    const val FIELD = 0
    const val TEXT = 1
    const val MATH = 2
}

/**
 * The LaTeX preview: the document typeset by tectonic, every page in a
 * scrolling list, and a double tap on the text to edit the source behind it.
 *
 * This follows the Mac app's LatexView (src/Latex.mm), with the differences
 * a phone forces:
 *
 * - tectonic runs in Termux (Termux.kt), since Android will not run a binary
 *   from another app's storage. So only a document on shared storage can be
 *   typeset: that is the one place both apps see at the same path.
 * - The buffer is written to a hidden sibling, `.<name>.minicode.tex`, in the
 *   document's own folder, so relative \input and \includegraphics resolve
 *   and the user's file is never written. It is deleted after each run.
 * - The PDF and its .synctex.gz go to /storage/emulated/0/.minicode/latex/,
 *   one folder per document. The Mac uses its temporary folder, but Termux
 *   cannot write MiniCode's and MiniCode cannot read Termux's, so the output
 *   has to be in shared storage too, and outside the project so it never
 *   shows up in the file list or in git.
 * - Finding the word under a tap needs the page's text, which PdfRenderer
 *   offers from Android 15. Below that the preview still shows the pages but
 *   says editing from it needs a newer Android.
 *
 * Typesetting is debounced and counted as on the Mac: one run at a time, a
 * change during a run queues one more, and a result from a document that has
 * since been closed is dropped.
 */
class LatexPreview @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : LinearLayout(context, attrs) {

    /** A tap's edit: replace [start, end) of the buffer with `text`. */
    var onEdit: ((start: Int, end: Int, text: String) -> Unit)? = null

    private val status = TextView(context).apply {
        setBackgroundColor(Palette.SIDEBAR)
        setTextColor(Palette.MUTED)
        textSize = 12f
        isSingleLine = true
        ellipsize = android.text.TextUtils.TruncateAt.END
        val dp = resources.displayMetrics.density
        setPadding((10 * dp).toInt(), (4 * dp).toInt(), (10 * dp).toInt(), (4 * dp).toInt())
    }
    private val pages = RecyclerView(context).apply {
        layoutManager = LinearLayoutManager(context)
        setBackgroundColor(Palette.BACKGROUND)
    }
    private val log = TextView(context).apply {
        setTextColor(Palette.TEXT)
        typeface = Typeface.MONOSPACE
        textSize = 11f
        setTextIsSelectable(true)
        val dp = resources.displayMetrics.density
        setPadding((10 * dp).toInt(), (8 * dp).toInt(), (10 * dp).toInt(), (8 * dp).toInt())
    }
    private val logScroll = ScrollView(context).apply {
        setBackgroundColor(Palette.BACKGROUND)
        addView(log)
        visibility = GONE
    }

    private val ui = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val adapter = PageAdapter()
    private val native = LatexNative.open()

    private var texPath: String? = null   // null: not typesettable
    private var source = ""
    private var typesetSource: String? = null   // what the last run was given
    private var generation = 0
    private var running = false
    private var queued = false

    private var renderer: PdfRenderer? = null
    private var descriptor: ParcelFileDescriptor? = null
    private var pageSizes: List<Pair<Int, Int>> = emptyList()   // points
    private var syncTag = 0
    private var hasSync = false

    init {
        orientation = VERTICAL
        setBackgroundColor(Palette.BACKGROUND)
        addView(status, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        addView(pages, LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))
        addView(logScroll, LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))
        pages.adapter = adapter
        val taps = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onDoubleTap(e: MotionEvent): Boolean {
                tapped(e.x, e.y)
                return true
            }
        })
        pages.addOnItemTouchListener(object : RecyclerView.SimpleOnItemTouchListener() {
            override fun onInterceptTouchEvent(rv: RecyclerView, e: MotionEvent): Boolean {
                taps.onTouchEvent(e)
                return false
            }
        })
    }

    // ------------------------------------------------------------ the document

    /**
     * Shows `file`, whose text is `text`. Typesets at once when it can, and
     * otherwise says why not.
     */
    fun open(file: DocumentFile?, text: String) {
        generation++
        running = false
        queued = false
        ui.removeCallbacks(debounced)
        closeRenderer()
        adapter.notifyDataSetChanged()
        source = text
        typesetSource = null
        texPath = file?.let { pathOf(context, it) }
        val path = texPath
        if (path == null || !path.startsWith("/storage/")) {
            texPath = null
            showMessage("The preview typesets with tectonic in Termux, which can " +
                    "only reach files on the phone's storage. Open this document " +
                    "from Phone storage (leader O) to see it typeset.")
            return
        }
        typeset()
    }

    /** The buffer changed; typeset it once typing pauses. */
    fun update(text: String) {
        source = text
        if (texPath == null || text == typesetSource) return
        ui.removeCallbacks(debounced)
        ui.postDelayed(debounced, DEBOUNCE_MS)
    }

    /** Typeset now, skipping the pause (on save). */
    fun typesetNow() {
        if (texPath == null) return
        ui.removeCallbacks(debounced)
        typeset()
    }

    /** Lets go of the PDF when another file is opened. */
    fun close() {
        generation++
        ui.removeCallbacks(debounced)
        texPath = null
        closeRenderer()
        adapter.notifyDataSetChanged()
    }

    private val debounced = Runnable { typeset() }

    // ------------------------------------------------------------ typesetting

    private fun typeset() {
        val path = texPath ?: return
        Termux.problem(context)?.let {
            showMessage("$it The preview runs tectonic in Termux; ⋮ → Termux " +
                    "tools walks through setting it up.")
            return
        }
        if (running) { queued = true; return }
        running = true
        queued = false
        val gen = generation
        val text = source
        typesetSource = text
        setStatus("Typesetting…")

        worker.execute {
            val result = runTectonic(path, text) { line ->
                ui.post { if (gen == generation && running) setStatus("Typesetting… $line") }
            }
            ui.post {
                if (gen != generation) return@post
                running = false
                if (result.pdf != null) show(result.pdf, result.synctex, result.scratch)
                else showFailure(result.log)
                if (queued || source != typesetSource) typeset()
            }
        }
    }

    private class Result(val pdf: File?, val synctex: File?, val scratch: String,
                         val log: String)

    /**
     * One tectonic run in Termux, on the worker thread. `progress` gets each
     * line tectonic prints, which is how the first run's downloads show.
     */
    private fun runTectonic(path: String, text: String, progress: (String) -> Unit): Result {
        val doc = File(path)
        val dir = doc.parentFile ?: File("/")
        val scratch = File(dir, "." + doc.nameWithoutExtension + ".minicode.tex")
        val out = File(OUTPUT_ROOT, Integer.toHexString(path.hashCode()))
        val stem = scratch.nameWithoutExtension
        val marker = "__minicode_exit_"
        val log = StringBuilder()
        try {
            out.mkdirs()
            File(out, "$stem.pdf").delete()
            scratch.writeText(text)
        } catch (e: Exception) {
            return Result(null, null, scratch.path,
                "Could not write the preview copy next to the document:\n${e.message}")
        }
        var status = -1
        try {
            val command = "tectonic --synctex --chatter minimal --color never " +
                    "--outdir ${Termux.shellQuote(out.path)} " +
                    "${Termux.shellQuote(scratch.path)} 2>&1; echo $marker\$?"
            val process = Termux.start(context, "bash -c ${Termux.shellQuote(command)}",
                                       workdir = dir.path)
            try {
                process.input.bufferedReader().forEachLine { line ->
                    if (line.startsWith(marker)) {
                        status = line.removePrefix(marker).trim().toIntOrNull() ?: -1
                    } else {
                        log.append(line).append('\n')
                        if (line.isNotBlank()) progress(line.trim())
                    }
                }
            } finally {
                process.close()
            }
        } catch (e: Exception) {
            log.append(e.message ?: e.toString())
        } finally {
            scratch.delete()
        }
        if (status == 127 || log.contains("tectonic: command not found")) {
            return Result(null, null, scratch.path,
                "tectonic is not installed in Termux. In Termux, run\n\n" +
                        "    pkg install tectonic\n\nand the preview will typeset.")
        }
        val pdf = File(out, "$stem.pdf")
        if (status != 0 || !pdf.exists()) {
            return Result(null, null, scratch.path,
                log.toString().ifBlank { "tectonic did not run." })
        }
        // A copy the renderer can hold open while the next run rewrites the
        // original.
        val copy = File(context.cacheDir, "latex-preview.pdf")
        pdf.copyTo(copy, overwrite = true)
        val sync = File(out, "$stem.synctex.gz").takeIf { it.exists() }
        return Result(copy, sync, scratch.path, log.toString())
    }

    private fun show(pdf: File, synctex: File?, scratch: String) {
        // Where the reader was, so a re-typeset does not jump to the top.
        val manager = pages.layoutManager as LinearLayoutManager
        val first = manager.findFirstVisibleItemPosition()
        val offset = manager.findViewByPosition(first)?.top ?: 0

        closeRenderer()
        try {
            val fd = ParcelFileDescriptor.open(pdf, ParcelFileDescriptor.MODE_READ_ONLY)
            val r = PdfRenderer(fd)
            descriptor = fd
            renderer = r
            pageSizes = (0 until r.pageCount).map { i ->
                r.openPage(i).use { it.width to it.height }
            }
        } catch (e: Exception) {
            showFailure("The PDF tectonic wrote could not be opened:\n${e.message}")
            return
        }
        hasSync = synctex != null && LatexNative.loadSyncTex(native, synctex.path)
        syncTag = if (hasSync) LatexNative.tagForPath(native, scratch) else 0

        logScroll.visibility = GONE
        pages.visibility = VISIBLE
        adapter.notifyDataSetChanged()
        if (first >= 0 && first < pageSizes.size) manager.scrollToPositionWithOffset(first, offset)
        val count = pageSizes.size
        val what = if (count == 1) "1 page" else "$count pages"
        setStatus(when {
            !hasSync -> "$what · no SyncTeX data, so the preview cannot be edited"
            Build.VERSION.SDK_INT < 35 -> "$what · editing from the preview needs Android 15"
            else -> "$what · double-tap text to edit it"
        })
    }

    private fun showFailure(text: String) {
        // The end of the log is where TeX says what went wrong.
        val lines = text.trimEnd().lines()
        log.text = lines.takeLast(60).joinToString("\n")
        pages.visibility = GONE
        logScroll.visibility = VISIBLE
        logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) }
        setStatus("The document did not typeset")
    }

    private fun showMessage(text: String) {
        log.text = text
        pages.visibility = GONE
        logScroll.visibility = VISIBLE
        setStatus("LaTeX preview")
    }

    private fun setStatus(text: String) { status.text = text }

    private fun closeRenderer() {
        try { renderer?.close() } catch (e: Exception) {}
        try { descriptor?.close() } catch (e: Exception) {}
        renderer = null
        descriptor = null
        pageSizes = emptyList()
        hasSync = false
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        closeRenderer()
    }

    // ------------------------------------------------------------ the pages

    /** One page per row, rendered at the list's width when it scrolls in. */
    private inner class PageAdapter : RecyclerView.Adapter<PageAdapter.Row>() {
        inner class Row(val image: ImageView) : RecyclerView.ViewHolder(image)

        override fun getItemCount() = pageSizes.size

        override fun onCreateViewHolder(parent: ViewGroup, type: Int): Row {
            val image = ImageView(parent.context).apply {
                scaleType = ImageView.ScaleType.FIT_XY
                setBackgroundColor(Color.WHITE)
            }
            return Row(image)
        }

        override fun onBindViewHolder(row: Row, position: Int) {
            val width = pages.width.takeIf { it > 0 } ?: resources.displayMetrics.widthPixels
            val (pw, ph) = pageSizes[position]
            val height = (width.toLong() * ph / pw).toInt()
            val gap = (4 * resources.displayMetrics.density).toInt()
            row.image.layoutParams = RecyclerView.LayoutParams(width, height).apply {
                bottomMargin = gap
            }
            row.image.setImageBitmap(render(position, width, height))
        }

        override fun onViewRecycled(row: Row) {
            row.image.setImageDrawable(null)
        }
    }

    private fun render(index: Int, width: Int, height: Int): Bitmap? {
        val r = renderer ?: return null
        if (width <= 0 || height <= 0) return null
        return try {
            val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            bitmap.eraseColor(Color.WHITE)
            r.openPage(index).use {
                it.render(bitmap, null, null, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY)
            }
            bitmap
        } catch (e: Exception) {
            null
        }
    }

    // ------------------------------------------------------------ tap -> source

    /**
     * A double tap at (x, y) in the list. Finds the page and the point on it
     * in PDF points, then the span behind it, and offers that span's source
     * for editing. A tap that matches nothing says so and edits nothing.
     */
    private fun tapped(x: Float, y: Float) {
        val child = pages.findChildViewUnder(x, y) ?: return
        val index = pages.getChildAdapterPosition(child)
        if (index < 0 || index >= pageSizes.size) return
        if (!hasSync) return
        if (Build.VERSION.SDK_INT < 35) {
            setStatus("Editing from the preview needs Android 15 or later")
            return
        }
        val (pw, ph) = pageSizes[index]
        val px = (x - child.left) * pw / child.width
        val py = (y - child.top) * ph / child.height
        val found = PageText.around(renderer ?: return, index, px, py)
        if (found == null) {
            setStatus("There is no text there")
            return
        }
        val span = LatexNative.spanAt(native, source, syncTag, index + 1,
                                      px.toDouble(), py.toDouble(),
                                      found.word, found.before, found.after)
        if (span == null || span[1] > source.length || span[0] > span[1]) {
            setStatus("That is not text MiniCode can trace back to the source")
            return
        }
        showEditor(span[0], span[1], span[2], span[3])
    }

    private fun showEditor(start: Int, end: Int, kind: Int, itemIndex: Int) {
        val what = when (kind) {
            LatexNative.FIELD -> "a field"
            LatexNative.MATH -> "math"
            else -> if (itemIndex >= 0) "a list item" else "text"
        }
        val original = source.substring(start, end)
        val field = android.widget.EditText(context).apply {
            setText(original)
            typeface = Typeface.MONOSPACE
            textSize = 13f
            setTextColor(Palette.TEXT)
            setBackgroundColor(Palette.SIDEBAR)
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                    android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                    android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            val dp = resources.displayMetrics.density
            setPadding((10 * dp).toInt(), (8 * dp).toInt(), (10 * dp).toInt(), (8 * dp).toInt())
        }
        // The source as written, commands and all: nothing is escaped or
        // re-serialized, so what comes back replaces exactly those bytes.
        val expected = source
        androidx.appcompat.app.AlertDialog.Builder(context)
            .setTitle("Editing $what")
            .setView(field)
            .setPositiveButton("Done") { _, _ ->
                val replacement = field.text.toString()
                // The buffer moved on while the dialog was up; the range may
                // no longer be the span's, so nothing is applied.
                if (source != expected) {
                    setStatus("The document changed while editing, so the edit was not applied")
                } else if (replacement != original) {
                    onEdit?.invoke(start, end, replacement)
                }
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    companion object {
        const val DEBOUNCE_MS = 800L
        val OUTPUT_ROOT = File("/storage/emulated/0/.minicode/latex")

        fun isLatex(name: String?): Boolean {
            val lower = name?.lowercase() ?: return false
            return lower.endsWith(".tex") || lower.endsWith(".ltx") ||
                    lower.endsWith(".latex")
        }

        /**
         * A document's path, when it has one: a file opened from Phone
         * storage, or a picker document on the phone's own storage. Null for
         * a cloud or another app's document.
         */
        fun pathOf(context: Context, doc: DocumentFile): String? {
            val uri = doc.uri
            if (uri.scheme == "file") return uri.path
            if (uri.authority != "com.android.externalstorage.documents") return null
            val id = try {
                android.provider.DocumentsContract.getDocumentId(uri)
            } catch (e: IllegalArgumentException) { return null }
            val volume = id.substringBefore(':')
            val rest = id.substringAfter(':', "")
            val root = if (volume == "primary")
                android.os.Environment.getExternalStorageDirectory().path
            else "/storage/$volume"
            return if (rest.isEmpty()) root else "$root/$rest"
        }
    }
}

/**
 * The word under a point on a PDF page, and the text either side of it, from
 * PdfRenderer's text selection (Android 15 and later). This is the Android
 * form of what the Mac gets from PDFKit's selectionForWordAtPoint and
 * extendSelection: 40 characters of context each way, and a word TeX
 * hyphenated at a line end joined back together.
 */
object PageText {
    class Found(val word: String, val before: String, val after: String)

    private const val CONTEXT = 40

    fun around(renderer: PdfRenderer, index: Int, x: Float, y: Float): Found? {
        if (Build.VERSION.SDK_INT < 35) return null
        return try {
            renderer.openPage(index).use { page -> find(page, x, y) }
        } catch (e: Exception) {
            null
        }
    }

    @androidx.annotation.RequiresApi(35)
    private fun find(page: PdfRenderer.Page, x: Float, y: Float): Found? {
        val point = Point(x.toInt(), y.toInt())
        // A selection from a point to itself is the word there.
        val word = try {
            page.selectContent(
                android.graphics.pdf.models.selection.SelectionBoundary(point),
                android.graphics.pdf.models.selection.SelectionBoundary(point))
        } catch (e: Exception) { null }
        val text = word?.selectedTextContents?.joinToString("") { it.text }.orEmpty()
        if (word != null && LatexNative.hasKey(text) && text.trim().none { it.isWhitespace() }) {
            val start = word.start.index
            val stop = word.stop.index
            var before = if (start > 0) range(page, maxOf(0, start - CONTEXT), start) else ""
            var after = if (stop >= 0) range(page, stop, stop + CONTEXT) else ""
            var clicked = text.trim()
            joinHyphenation(clicked, before, after).let {
                clicked = it.first; before = it.second; after = it.third
            }
            return Found(clicked, before, after)
        }
        // No word (a bullet, a logo, or a selection that did not resolve):
        // the line under the point, which the matcher can still place.
        val line = page.textContents.firstOrNull { content ->
            content.bounds.any { it.contains(x, y) }
        }?.text ?: return null
        return Found(line, "", "")
    }

    @androidx.annotation.RequiresApi(35)
    private fun range(page: PdfRenderer.Page, from: Int, to: Int): String = try {
        page.selectContent(
            android.graphics.pdf.models.selection.SelectionBoundary(from),
            android.graphics.pdf.models.selection.SelectionBoundary(to))
            ?.selectedTextContents?.joinToString("") { it.text }.orEmpty()
    } catch (e: Exception) { "" }

    /**
     * TeX hyphenates at the end of a line, so the page holds "counterrevolu-"
     * and "tionaries" where the source has one word. When the tapped word
     * sits against a line-end hyphen, the other half comes from the context,
     * as MCJoinHyphenation does on the Mac.
     */
    fun joinHyphenation(word: String, before: String, after: String): Triple<String, String, String> {
        val hyphens = listOf("-", "‐", "­")
        var w = word
        var b = before
        var a = after
        for (h in hyphens) {
            if (!a.startsWith(h) || a.length <= h.length) continue
            var k = h.length
            if (a[k] != '\n' && a[k] != '\r') continue
            while (k < a.length && (a[k] == '\n' || a[k] == '\r')) k++
            var e = k
            while (e < a.length && a[e].isLetter()) e++
            if (e > k) { w += a.substring(k, e); a = a.substring(e) }
            break
        }
        var k = b.length
        while (k > 0 && (b[k - 1] == '\n' || b[k - 1] == '\r')) k--
        if (k == b.length || k == 0) return Triple(w, b, a)
        for (h in hyphens) {
            if (k < h.length || b.substring(k - h.length, k) != h) continue
            val e = k - h.length
            var s = e
            while (s > 0 && b[s - 1].isLetter()) s--
            if (e > s) { w = b.substring(s, e) + w; b = b.substring(0, s) }
            break
        }
        return Triple(w, b, a)
    }
}
