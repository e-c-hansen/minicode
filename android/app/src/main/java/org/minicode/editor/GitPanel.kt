package org.minicode.editor

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.text.SpannableString
import android.text.Spanned
import android.text.TextPaint
import android.text.TextUtils
import android.text.style.ForegroundColorSpan
import android.text.style.StyleSpan
import android.util.AttributeSet
import android.util.Log
import android.view.GestureDetector
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import java.io.File
import java.util.concurrent.Executors

/**
 * The Source Control panel, in the file list's place: the branch, a commit
 * message box, the staged and unstaged changes, and under them the commit
 * graph. The Mac's src/GitPanel.mm and the GTK port's linux/src/GitPanel.cpp
 * are the models; the rules, the commands, the wording and the colors are
 * theirs, and all of the deciding is their shared C++ (GitNative).
 *
 * Git runs in Termux (GitRunner) on one worker thread, so an add and the
 * status after it run in order. Results come back to the main thread and are
 * dropped when the folder changed or a newer diff or graph was asked for.
 *
 * Native handles (a snapshot, a graph) are freed on the worker thread, after
 * whatever job was still using them, so the two threads never race on one.
 *
 * Keys, for a hardware keyboard: Up and Down move through the files (headings
 * skipped) and on into the graph; Up from the first file goes to the message
 * box, Down from the message box's last line comes back. Space stages or
 * unstages, Enter shows the diff or the commit. Tab and Shift+Tab work as on
 * the desktop for a keyboard that has them, and Ctrl+Enter or the leader then
 * Enter commits. Touch: tap a file for its diff, tap + or − at its right to
 * stage or unstage, tap a commit for its diff, long-press either for details.
 */
class GitPanel @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : LinearLayout(context, attrs) {

    /** The folder to look at, as a path, or null with the reason why not. */
    var folder: () -> Pair<File?, String?> = { null to "Open a folder first." }
    /** A diff or a commit to show in the editor's place: title and text. */
    var onShow: ((String, List<CharSequence>) -> Unit)? = null

    private val dp = resources.displayMetrics.density
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor { r ->
        Thread(r, "MiniCodeGit").apply { isDaemon = true }
    }
    private val runner = GitRunner(context)

    // What is shown. `snap` and `graph` are native handles owned here.
    private var root: String? = null
    private var snap = 0L
    private var rows: List<RowData> = emptyList()
    private var inRepository = false
    private var initial = false
    private var graph = 0L
    private var graphRows: List<CommitRow> = emptyList()
    private var graphHasMore = false
    private var graphMaxWidth = 1
    private var graphShown = false
    private var graphLimit = GitNative.GRAPH_BATCH
    private var allBranches = false
    private var sel = -1
    private var graphSel = -1
    private var inGraph = false          // which list the keyboard is in
    private var errorFromStatus = false
    private var fullError = ""
    private var active = false
    private var focusPending = false
    private var graphGeneration = 0
    private var diffGeneration = 0
    private var refreshQueued = false
    /** The safe.directory given to every git: the repository's top level. */
    @Volatile private var safeDirectory = ""

    private val branchLine = label(14f, Palette.TEXT).apply {
        typeface = Typeface.DEFAULT_BOLD
        setPadding(px(10), px(6), px(10), px(4))
        isSingleLine = true
        ellipsize = TextUtils.TruncateAt.END
        text = "Source Control"
        // A tap looks again, for changes made where no refresh could see
        // them (Termux's own shell, another app).
        setOnClickListener { refresh() }
    }
    // The editor's own field and keyboard settings (see MainActivity), so
    // the phone's keyboard neither composes nor corrects words here either.
    val message = CodeEditText(context).apply {
        inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        privateImeOptions = "nm"
        typeface = Typeface.DEFAULT   // the password variation set monospace
        setTextColor(Palette.TEXT)
        setHintTextColor(Palette.MUTED)
        hint = "Message (Commit, or leader then Enter)"
        textSize = 13f
        minLines = 2
        maxLines = 4
        gravity = Gravity.TOP or Gravity.START
        setBackgroundColor(Palette.BACKGROUND)
        setPadding(px(6), px(4), px(6), px(4))
        importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
    }
    private val commitButton = label(14f, 0xFFFFFFFF.toInt()).apply {
        text = "Commit"
        gravity = Gravity.CENTER
        setBackgroundColor(ACCENT_DARK)
        setPadding(px(12), 0, px(12), 0)
        setOnClickListener { commit() }
    }
    private val messageRow = LinearLayout(context).apply {
        orientation = HORIZONTAL
        setPadding(px(8), px(2), px(8), px(4))
        addView(message, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
        addView(commitButton, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT)
            .apply { leftMargin = px(6) })
    }
    private val errorLine = label(12f, ERROR).apply {
        setPadding(px(10), px(2), px(10), px(4))
        maxLines = 6
        ellipsize = TextUtils.TruncateAt.END
        visibility = GONE
        // Six lines at most here; the whole of git's message on a tap.
        setOnClickListener {
            if (fullError.isNotEmpty()) androidx.appcompat.app.AlertDialog.Builder(context)
                .setMessage(fullError).setPositiveButton("OK", null).show()
        }
    }
    private val notice = label(13f, Palette.MUTED).apply {
        setPadding(px(10), px(8), px(10), px(8))
        visibility = GONE
    }
    private val changes = RecyclerView(context).apply {
        layoutManager = LinearLayoutManager(context)
        isFocusable = false
        descendantFocusability = FOCUS_BLOCK_DESCENDANTS
        itemAnimator = null
    }
    private val rule = View(context).apply { setBackgroundColor(Palette.DIVIDER) }
    private val allToggle = CheckBox(context).apply {
        text = "All branches"
        textSize = 12f
        setTextColor(Palette.MUTED)
        isFocusable = false
        buttonTintList = android.content.res.ColorStateList.valueOf(Palette.ACCENT)
        setOnCheckedChangeListener { _, on ->
            if (on == allBranches) return@setOnCheckedChangeListener
            allBranches = on
            graphLimit = GitNative.GRAPH_BATCH
            runRefresh()
        }
    }
    private val graphHeading = LinearLayout(context).apply {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(px(10), 0, px(4), 0)
        addView(label(11f, Palette.MUTED).apply {
            text = "GRAPH"
            letterSpacing = 0.06f
            typeface = Typeface.DEFAULT_BOLD
        }, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f))
        addView(allToggle)
    }
    private val summary = label(12f, Palette.MUTED).apply {
        setPadding(px(10), 0, px(10), px(3))
        isSingleLine = true
        ellipsize = TextUtils.TruncateAt.END
    }
    private val graphList = RecyclerView(context).apply {
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
        descendantFocusability = FOCUS_BEFORE_DESCENDANTS
        // The panel holds the keyboard itself (the rows are drawn, not
        // focused), and Android would grey the whole of a focused view
        // that has no focused look of its own.
        defaultFocusHighlightEnabled = false
        addView(branchLine, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        addView(messageRow, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        addView(errorLine, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        addView(notice, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        addView(changes, LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))
        addView(rule, LayoutParams(LayoutParams.MATCH_PARENT, 1))
        addView(graphHeading, LayoutParams(LayoutParams.MATCH_PARENT, px(34)))
        addView(summary, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        addView(graphList, LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))
        changes.adapter = ChangeAdapter()
        graphList.adapter = GraphAdapter()
        message.onFocusChangeListener = OnFocusChangeListener { _, _ -> redrawLists() }
        showGraph(false)
        showMessageArea(false)
    }

    override fun onFocusChanged(gained: Boolean, direction: Int, previous: android.graphics.Rect?) {
        super.onFocusChanged(gained, direction, previous)
        redrawLists()
    }

    // ------------------------------------------------------------ showing

    /** The panel came on screen (true) or left it. */
    fun setActive(on: Boolean) {
        active = on
        if (on) {
            idleCheck?.let { main.removeCallbacks(it) }
            val (dir, _) = folder()
            if (dir?.path != root) setRoot(dir?.path)
            refresh()
        } else {
            // The loop in Termux is cheap, but there is no reason to keep it
            // while nobody looks at the panel for a while.
            val check = Runnable { worker.execute { if (!active) runner.close() } }
            idleCheck = check
            main.postDelayed(check, 120_000)
        }
    }
    private var idleCheck: Runnable? = null

    /** A folder was opened: start over there. */
    fun folderChanged() {
        val (dir, _) = folder()
        if (dir?.path == root) return
        setRoot(dir?.path)
        if (active) refresh()
    }

    private fun setRoot(path: String?) {
        root = path
        replaceSnapshot(0L)
        rows = emptyList()
        inRepository = false
        replaceGraph(0L)
        graphRows = emptyList()
        graphLimit = GitNative.GRAPH_BATCH
        sel = -1; graphSel = -1; inGraph = false
        showGraph(false)
        showMessageArea(false)
        setError("", false)
        reload()
    }

    /** Opening the panel: the keyboard goes to the first file, or the graph. */
    fun focusPanel() {
        focusPending = rows.isEmpty() && graphRows.isEmpty()
        // Back from a commit it showed, the keyboard returns to the graph.
        if (inGraph && graphSel >= 0 && graphCount() > 0) focusGraph() else focusList()
    }

    fun shutdown() {
        active = false
        worker.execute { runner.close() }
        freeLater(snap, graph)
        snap = 0L; graph = 0L
        worker.shutdown()
    }

    /** Looks again, soon: a burst of requests makes one status. */
    fun refresh() {
        if (!active || refreshQueued) return
        refreshQueued = true
        main.postDelayed({ refreshQueued = false; runRefresh() }, 50)
    }

    // The status first, shown as soon as it is read; then the graph, which on
    // a long history takes longer, rebuilt only when HEAD, the refs, the
    // limit or the "All branches" switch changed.
    private fun runRefresh() {
        val (dir, why) = folder()
        if (dir?.path != root) setRoot(dir?.path)
        val rootNow = root
        val gen = ++graphGeneration
        val limit = graphLimit
        val all = allBranches
        val previous = graph
        worker.execute {
            val started = System.nanoTime()
            log { "refresh: $rootNow" }
            val s = if (rootNow == null) GitNative.snapshotEmpty(why, null)
                    else snapshotAt(rootNow)
            val data = SnapData.of(s)
            log { "status in ${(System.nanoTime() - started) / 1_000_000} ms: ${data.text[0]} | " +
                  "${data.text[3]} | error: ${data.text[2]} | notice: ${data.text[1]}" }
            log { GitNative.rowDescriptions(s).joinToString("\n") { "  $it" } }
            main.post {
                if (rootNow != root) { freeLater(s); return@post }
                applySnapshot(s, data)
            }
            if (data.flags[2] == 0) return@execute
            val g = graphFor(s, data, limit, all, previous)
            if (g == previous) return@execute
            val gd = GraphData.of(g)
            log { "graph in ${(System.nanoTime() - started) / 1_000_000} ms: " +
                  "${gd.rows.size} commits, more: ${gd.hasMore}" }
            log { GitNative.graphDescriptions(g).take(40).joinToString("\n") { "  $it" } }
            main.post {
                if (rootNow != root || gen != graphGeneration) { freeLater(g); return@post }
                applyGraph(g, gd)
            }
        }
    }

    /** Runs one git, in `dir`, trusting that repository only. */
    private fun git(dir: String, args: Array<ByteArray>, safe: String = safeDirectory):
            GitRunner.Result {
        val all = ArrayList<ByteArray>(args.size + 2)
        if (safe.isNotEmpty()) {
            // Termux's user does not own files in shared storage, and git
            // refuses such a repository ("dubious ownership") unless it is
            // named in safe.directory. Given here, for this one repository,
            // on each command; nothing is written to any config.
            all.add("-c".toByteArray())
            all.add("safe.directory=$safe".toByteArray())
        }
        all.addAll(args)
        val r = runner.run(dir, all)
        log { "git " + all.joinToString(" ") { String(it) } + " -> ${r.status}" +
              (if (r.ok) "" else ": " + r.errText().trim().take(300)) }
        return r
    }

    private fun failure(r: GitRunner.Result) = GitNative.failureText(r.status, r.out, r.err)

    /** What the lists show about `dir`, read on the worker thread. */
    private fun snapshotAt(dir: String): Long {
        if (!Termux.isShared(File(dir))) return GitNative.snapshotEmpty(NOT_SHARED, null)
        Termux.problem(context)?.let {
            return GitNative.snapshotEmpty("Git runs in Termux. $it Open ⋮, Termux tools.", null)
        }
        return try {
            var top = git(dir, GitNative.topLevelArgs(), dir)
            val owner = Regex("dubious ownership in repository at '([^']+)'")
                .find(top.errText())?.groupValues?.get(1)
            if (!top.ok && owner != null) top = git(dir, GitNative.topLevelArgs(), owner)
            if (!top.ok) {
                return if (top.errText().contains("not a git repository"))
                    GitNative.snapshotEmpty("This folder is not in a git repository.", null)
                else GitNative.snapshotEmpty(null, failure(top))
            }
            val paths = GitNative.parseTopLevel(top.out)
                ?: return GitNative.snapshotEmpty(null, failure(top))
            safeDirectory = paths[0]
            val st = git(paths[0], GitNative.statusArgs())
            if (!st.ok) GitNative.snapshotOf(paths[0], paths[1], null, failure(st))
            else GitNative.snapshotOf(paths[0], paths[1], st.out, null)
        } catch (e: GitRunner.Missing) {
            GitNative.snapshotEmpty(e.message, null)
        } catch (e: Exception) {
            GitNative.snapshotEmpty(null, e.message ?: e.toString())
        }
    }

    /** The graph, or `previous` when nothing it depends on changed. */
    private fun graphFor(s: Long, data: SnapData, limit: Int, all: Boolean, previous: Long): Long {
        val top = data.top
        return try {
            val refs = git(top, GitNative.refArgs())
            if (GitNative.graphSameKey(previous, s, limit, all, refs.out)) return previous
            val compare = data.flags[3] != 0
            val log = git(top, GitNative.logArgs(limit, all, compare))
            if (!log.ok) return GitNative.graphBuild(s, limit, all, refs.out, null, failure(log))
            val g = GitNative.graphBuild(s, limit, all, refs.out, log.out, null)
            if (compare && (data.flags[4] != 0 || data.flags[5] != 0)) {
                val lr = git(top, GitNative.leftRightArgs())
                if (lr.ok) GitNative.graphSetLeftRight(g, lr.out)
            }
            g
        } catch (e: Exception) {
            GitNative.graphBuild(s, limit, all, ByteArray(0), null, e.message ?: e.toString())
        }
    }

    private fun applySnapshot(s: Long, data: SnapData) {
        val old = snap
        val was = sel
        snap = s
        rows = data.rows
        inRepository = data.flags[0] != 0
        initial = data.flags[1] != 0
        branchLine.text = data.text[0]
        showMessageArea(inRepository)
        notice.text = data.text[1]
        notice.visibility = if (data.text[1].isEmpty()) GONE else VISIBLE
        // A failing status says why until one succeeds; a failed action's
        // message stays until the next action.
        if (data.text[2].isNotEmpty()) {
            setError(data.text[2], false)
            errorFromStatus = true
        } else if (errorFromStatus) {
            setError("", false)
        }
        val wanted = data.flags[2] != 0
        showGraph(wanted)
        summary.text = data.text[3]
        if (!wanted && graph != 0L) {
            replaceGraph(0L)
            graphRows = emptyList()
            graphSel = -1
        }
        sel = GitNative.pickAfterRefresh(old, was, s)
        replaceSnapshot(s, free = old)
        if (sel < 0 && !inGraph && hasFocus() && !message.hasFocus())
            sel = GitNative.nextFileRow(s, -1, 1)
        if (focusPending && (sel >= 0 || !wanted)) {
            focusPending = false
            if (hasFocus()) focusList()
        }
        if (inGraph && !wanted) inGraph = false
        reload()
        scrollToSelection()
    }

    private fun applyGraph(g: Long, data: GraphData) {
        if (g == graph) return
        val wasHash = graphRows.getOrNull(graphSel)?.hash
        val wasMoreRow = graphSel >= 0 && graphSel == graphRows.size
        val oldSel = graphSel
        replaceGraph(g)
        graphRows = data.rows
        graphHasMore = data.hasMore
        graphMaxWidth = data.maxWidth
        graphSel = graphRows.indexOfFirst { it.hash == wasHash && wasHash != null }
        // After "Show more", the first of the new commits.
        if (graphSel < 0 && wasMoreRow && oldSel < graphRows.size) graphSel = oldSel
        if (focusPending) {
            focusPending = false
            if (hasFocus()) focusList()
        }
        if (data.error.isNotEmpty()) setError(data.error, false)
        graphList.adapter?.notifyDataSetChanged()
        if (inGraph && graphSel >= 0) graphList.scrollToPosition(graphSel)
    }

    private fun replaceSnapshot(s: Long, free: Long = snap) {
        snap = s
        if (free != 0L && free != s) freeLater(free)
    }

    private fun replaceGraph(g: Long) {
        val old = graph
        graph = g
        if (old != 0L && old != g) freeLater(0L, old)
    }

    /** Frees native handles once every job before this one is done. */
    private fun freeLater(s: Long, g: Long = 0L) {
        if (s == 0L && g == 0L) return
        try {
            worker.execute {
                if (s != 0L) GitNative.snapshotFree(s)
                if (g != 0L) GitNative.graphFree(g)
            }
        } catch (e: java.util.concurrent.RejectedExecutionException) {}
    }

    private fun showGraph(show: Boolean) {
        graphShown = show
        for (v in listOf(rule, graphHeading, summary, graphList))
            v.visibility = if (show) VISIBLE else GONE
        layoutLists()
    }

    private fun showMessageArea(show: Boolean) {
        messageRow.visibility = if (show) VISIBLE else GONE
    }

    private fun setError(text: String, info: Boolean) {
        errorFromStatus = false
        fullError = text
        errorLine.text = text
        errorLine.setTextColor(if (info) Palette.MUTED else ERROR)
        errorLine.visibility = if (text.isEmpty()) GONE else VISIBLE
        if (text.isNotEmpty()) log { "message (${if (info) "info" else "error"}): $text" }
    }

    // The change lists take what their rows need, up to about half of what
    // is left, and the graph the rest, as on the Mac.
    private fun layoutLists() {
        val lp = changes.layoutParams as LayoutParams
        if (!graphShown) {
            lp.height = 0; lp.weight = 1f
        } else {
            var content = 0
            for (r in rows) content += if (r.header) HEADER_H else ROW_H
            val used = branchLine.height + (if (messageRow.visibility == VISIBLE) messageRow.height else 0) +
                    (if (errorLine.visibility == VISIBLE) errorLine.height else 0) +
                    (if (notice.visibility == VISIBLE) notice.height else 0) +
                    px(34) + summary.height + 1
            val half = maxOf(px(70), (height - used) / 2)
            lp.height = minOf(px(content) + px(4), half)
            lp.weight = 0f
        }
        changes.layoutParams = lp
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        post { layoutLists() }
    }

    private fun reload() {
        changes.adapter?.notifyDataSetChanged()
        graphList.adapter?.notifyDataSetChanged()
        post { layoutLists() }
    }

    private fun redrawLists() {
        for (list in listOf(changes, graphList))
            for (i in 0 until list.childCount) list.getChildAt(i).invalidate()
    }

    // ------------------------------------------------------------ actions

    /** Runs one git that changes the index or makes a commit, then refreshes. */
    private fun runAction(args: Array<ByteArray>, done: ((GitRunner.Result) -> Unit)? = null) {
        val dir = safeDirectory.takeIf { inRepository } ?: return
        val rootNow = root
        log { "action: " + args.joinToString(" ") { String(it) } }
        worker.execute {
            val r = try { git(dir, args) } catch (e: Exception) {
                GitRunner.Result(-1, ByteArray(0), (e.message ?: e.toString()).toByteArray())
            }
            val text = if (r.ok) "" else failure(r)
            main.post {
                if (rootNow != root) return@post
                setError(text, false)
                done?.invoke(r)
                runRefresh()
            }
        }
    }

    private fun toggleStage(row: Int) {
        val args = if (snap != 0L) GitNative.stageArgs(snap, row) else null
        if (args == null) return
        runAction(args)
    }

    /** The Commit button, Ctrl+Enter, or the leader then Enter. */
    fun commit() {
        if (!inRepository) return
        val text = message.text?.toString().orEmpty()
        runAction(GitNative.commitArgs(text)) { r ->
            if (!r.ok) return@runAction   // the message is kept when a commit fails
            message.setText("")
            setError(GitNative.firstLine(r.out), true)   // "[main 1a2b3c4] Subject"
        }
    }

    private fun showDiff(row: Int) {
        val r = rows.getOrNull(row) ?: return
        if (r.header || snap == 0L) return
        val args = GitNative.diffArgs(snap, row) ?: return
        val gen = ++diffGeneration
        val dir = safeDirectory
        val rootNow = root
        val title = "${r.name} (diff)"
        worker.execute {
            val res = try { git(dir, args) } catch (e: Exception) {
                GitRunner.Result(-1, ByteArray(0), (e.message ?: "").toByteArray())
            }
            // Exit status 1 from --no-index means "differs".
            val ok = res.ok || (r.untracked && res.status == 1)
            val text = if (ok) styled(res.out, false) else null
            val why = if (ok) "" else failure(res)
            log { "diff: ${r.name}, ${res.out.size} bytes" }
            main.post {
                if (gen != diffGeneration || rootNow != root) return@post
                if (text == null) setError(why, false) else onShow?.invoke(title, text)
            }
        }
    }

    // A commit's header, message and diff (against its first parent for a
    // merge, which is what the merge brought in).
    private fun showCommit(row: Int) {
        if (graph == 0L || row < 0) return
        if (row >= graphRows.size) {
            if (graphHasMore) showMore()
            return
        }
        val (hash, title) = GitNative.graphCommit(graph, row)?.let { it[0] to it[1] } ?: return
        val gen = ++diffGeneration
        val dir = safeDirectory
        val rootNow = root
        worker.execute {
            val res = try { git(dir, GitNative.showArgs(hash)) } catch (e: Exception) {
                GitRunner.Result(-1, ByteArray(0), (e.message ?: "").toByteArray())
            }
            val text = if (res.ok) styled(res.out, true) else null
            val why = if (res.ok) "" else failure(res)
            log { "commit view: $title, ${res.out.size} bytes" }
            main.post {
                if (gen != diffGeneration || rootNow != root) return@post
                if (text == null) setError(why, false) else onShow?.invoke(title, text)
            }
        }
    }

    // "Show more" raises the limit and loads the whole window again:
    // topological order has to be worked out from the top anyway.
    private fun showMore() {
        graphLimit += GitNative.GRAPH_BATCH
        runRefresh()
    }

    /** Diff or commit text, colored as on the Mac, a line at a time for DiffView. */
    private fun styled(bytes: ByteArray, commit: Boolean): List<CharSequence> {
        val parts = GitNative.styledText(bytes, commit, System.currentTimeMillis() / 1000)
        val text = SpannableString(parts[0] as String)
        val runs = parts[1] as IntArray
        var i = 0
        while (i + 2 < runs.size) {
            val a = runs[i]; val b = a + runs[i + 1]
            val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
            when (runs[i + 2]) {
                GitNative.ADDED -> text.setSpan(ForegroundColorSpan(ADDED), a, b, flags)
                GitNative.REMOVED -> text.setSpan(ForegroundColorSpan(ERROR), a, b, flags)
                GitNative.MUTED -> text.setSpan(ForegroundColorSpan(Palette.MUTED), a, b, flags)
                GitNative.FILE_HEADER -> {
                    text.setSpan(ForegroundColorSpan(Palette.MUTED), a, b, flags)
                    text.setSpan(StyleSpan(Typeface.BOLD), a, b, flags)
                }
                GitNative.HASH -> text.setSpan(ForegroundColorSpan(HASH), a, b, flags)
                GitNative.BOLD -> text.setSpan(StyleSpan(Typeface.BOLD), a, b, flags)
            }
            i += 3
        }
        val lines = ArrayList<CharSequence>()
        var start = 0
        val whole = text.toString()
        while (start <= whole.length) {
            var end = whole.indexOf('\n', start)
            if (end < 0) end = whole.length
            if (end > start || end < whole.length) lines.add(text.subSequence(start, end))
            start = end + 1
        }
        return lines
    }

    // ---------------------------------------------------------------- keys

    private fun focusList() {
        inGraph = false
        if (sel < 0 && snap != 0L) sel = GitNative.nextFileRow(snap, -1, 1)
        if (sel < 0) {
            when {
                graphShown && graphCount() > 0 -> { focusGraph(); return }
                messageRow.visibility == VISIBLE -> { message.requestFocus(); return }
            }
        }
        requestFocus()
        redrawLists()
        scrollToSelection()
    }

    private fun focusGraph() {
        if (!graphShown || graphCount() == 0) {
            if (messageRow.visibility == VISIBLE) message.requestFocus()
            return
        }
        inGraph = true
        if (graphSel < 0) graphSel = 0
        requestFocus()
        redrawLists()
        scrollToSelection()
    }

    /** Up from the graph's first row, or Shift+Tab: the last file. */
    private fun focusListFromGraph(wrap: Boolean) {
        if (sel < 0 && snap != 0L) sel = GitNative.lastFileRow(snap)
        if (sel < 0) {
            if (wrap && messageRow.visibility == VISIBLE) message.requestFocus()
            return
        }
        inGraph = false
        requestFocus()
        redrawLists()
        scrollToSelection()
    }

    /** Brings a file row into view, with its heading when it is the first. */
    private fun scrollChangesTo(row: Int) {
        val first = if (row > 0 && rows.getOrNull(row - 1)?.header == true) row - 1 else row
        changes.scrollToPosition(first)
        // Only once that is laid out: a second call now would replace it.
        if (first != row) changes.post { changes.scrollToPosition(row) }
    }

    private fun graphCount() = graphRows.size + if (graphHasMore) 1 else 0

    private fun scrollToSelection() {
        if (sel >= 0) scrollChangesTo(sel)
        if (graphSel >= 0) graphList.scrollToPosition(graphSel)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action != KeyEvent.ACTION_DOWN) {
            return if (swallow.remove(event.keyCode)) true else super.dispatchKeyEvent(event)
        }
        val handled = if (message.hasFocus()) messageKey(event) else listKey(event)
        if (handled) { swallow.add(event.keyCode); return true }
        return super.dispatchKeyEvent(event)
    }
    private val swallow = mutableSetOf<Int>()

    private fun isEnter(code: Int) = code == KeyEvent.KEYCODE_ENTER ||
            code == KeyEvent.KEYCODE_NUMPAD_ENTER || code == KeyEvent.KEYCODE_DPAD_CENTER

    private fun messageKey(e: KeyEvent): Boolean {
        val code = e.keyCode
        if (isEnter(code) && e.isCtrlPressed) { commit(); return true }
        if (e.isCtrlPressed || e.isAltPressed || e.isMetaPressed) return false
        if (code == KeyEvent.KEYCODE_TAB) {
            if (e.isShiftPressed) focusGraph() else focusList()
            return true
        }
        // Down on the message's last line goes on to the files.
        if (code == KeyEvent.KEYCODE_DPAD_DOWN) {
            val layout = message.layout ?: return false
            val line = layout.getLineForOffset(message.selectionEnd)
            if (line >= layout.lineCount - 1) { focusList(); return true }
        }
        return false
    }

    private fun listKey(e: KeyEvent): Boolean {
        if (e.isCtrlPressed || e.isAltPressed || e.isMetaPressed) return false
        val code = e.keyCode
        val shift = e.isShiftPressed
        if (code == KeyEvent.KEYCODE_TAB) {
            if (inGraph) { if (shift) focusListFromGraph(true) else focusMessageOrList() }
            else { if (shift) message.requestFocus() else focusGraph() }
            return true
        }
        if (inGraph) {
            when {
                isEnter(code) -> { showCommit(graphSel); return true }
                code == KeyEvent.KEYCODE_SPACE -> return true
                code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN -> {
                    val down = code == KeyEvent.KEYCODE_DPAD_DOWN
                    val n = graphCount()
                    val r = if (graphSel < 0) (if (down) 0 else n - 1)
                            else graphSel + if (down) 1 else -1
                    if (r in 0 until n) {
                        graphSel = r
                        graphList.scrollToPosition(r)
                        redrawLists()
                    } else if (!down) focusListFromGraph(false)
                    return true
                }
            }
            return false
        }
        when {
            isEnter(code) -> { showDiff(sel); return true }
            code == KeyEvent.KEYCODE_SPACE -> { toggleStage(sel); return true }
            code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN -> {
                val step = if (code == KeyEvent.KEYCODE_DPAD_UP) -1 else 1
                val next = if (snap != 0L) GitNative.nextFileRow(snap, sel, step) else -1
                when {
                    next >= 0 -> { sel = next; scrollChangesTo(next); redrawLists() }
                    step > 0 -> focusGraph()
                    messageRow.visibility == VISIBLE -> message.requestFocus()
                }
                return true
            }
        }
        return false
    }

    private fun focusMessageOrList() {
        if (messageRow.visibility == VISIBLE) message.requestFocus() else focusList()
    }

    // ------------------------------------------------------------- drawing

    private class RowData(val header: Boolean, val staged: Boolean, val untracked: Boolean,
                          val unmerged: Boolean, val letter: Char, val letterColor: Int,
                          val name: String, val dir: String, val tip: String)

    private class SnapData(val text: Array<String>, val flags: IntArray,
                           val rows: List<RowData>, val top: String) {
        companion object {
            fun of(s: Long): SnapData {
                val parts = GitNative.snapshotRows(s)
                val f = parts[0] as IntArray
                @Suppress("UNCHECKED_CAST") val t = parts[1] as Array<String>
                val rows = (0 until t.size / 3).map { i ->
                    RowData(f[i * 6] != 0, f[i * 6 + 1] != 0, f[i * 6 + 2] != 0, f[i * 6 + 3] != 0,
                            f[i * 6 + 4].toChar(), f[i * 6 + 5] or 0xFF000000.toInt(),
                            t[i * 3], t[i * 3 + 1], t[i * 3 + 2])
                }
                val text = GitNative.snapshotText(s)
                return SnapData(text, GitNative.snapshotFlags(s), rows, text[4])
            }
        }
    }

    private class Label(val kind: Int, val current: Boolean, val name: String)
    private class Edge(val kind: Int, val from: Int, val to: Int, val color: Int)
    private class CommitRow(val hash: String?, val lane: Int, val color: Int, val width: Int,
                            val flags: Int, val edges: List<Edge>, val labels: List<Label>,
                            val subject: String)

    private class GraphData(val rows: List<CommitRow>, val hasMore: Boolean, val maxWidth: Int,
                            val error: String) {
        companion object {
            fun of(g: Long): GraphData {
                val parts = GitNative.graphRows(g)
                val v = parts[0] as IntArray
                @Suppress("UNCHECKED_CAST") val t = parts[1] as Array<String>
                val n = v[0]
                var at = 3
                var ti = 0
                val rows = ArrayList<CommitRow>(n)
                for (i in 0 until n) {
                    val lane = v[at]; val color = v[at + 1]; val width = v[at + 2]
                    val flags = v[at + 3]; val ne = v[at + 4]; val nl = v[at + 5]
                    at += 6
                    val edges = (0 until ne).map {
                        Edge(v[at + it * 4], v[at + it * 4 + 1], v[at + it * 4 + 2], v[at + it * 4 + 3])
                    }
                    at += ne * 4
                    val subject = t[ti++]
                    val labels = (0 until nl).map {
                        Label(v[at + it * 2], v[at + it * 2 + 1] != 0, t[ti + it])
                    }
                    at += nl * 2
                    ti += nl
                    val hash = GitNative.graphCommit(g, i)?.get(0)
                    rows.add(CommitRow(hash, lane, color, width, flags, edges, labels, subject))
                }
                return GraphData(rows, v[1] != 0, maxOf(1, v[2]), GitNative.graphError(g))
            }
        }
    }

    private val laneColors = IntArray(7) { GitNative.laneColor(it) or 0xFF000000.toInt() }
    private fun laneColor(i: Int) = laneColors[((i % 7) + 7) % 7]

    private val textPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 13.5f * dp }
    private val smallPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 11.5f * dp }
    private val letterPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 13f * dp
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
    }
    private val pillPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 10.5f * dp
        typeface = Typeface.DEFAULT_BOLD
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
    }

    private fun selectionColor(focused: Boolean) = if (focused) SELECTED else SELECTED_INACTIVE
    private fun listFocused() = hasFocus() && !message.hasFocus() && !inGraph
    private fun graphFocused() = hasFocus() && !message.hasFocus() && inGraph

    private fun baseline(paint: Paint, mid: Float) = mid - (paint.descent() + paint.ascent()) / 2

    /** One row of the change lists, drawn by hand as on the Mac. */
    private inner class ChangeRow : View(context) {
        var position = -1

        override fun onDraw(canvas: Canvas) {
            val r = rows.getOrNull(position) ?: return
            val w = width.toFloat()
            val mid = height / 2f
            if (r.header) {
                smallPaint.color = Palette.MUTED
                smallPaint.typeface = Typeface.DEFAULT_BOLD
                smallPaint.letterSpacing = 0.06f
                canvas.drawText(r.name.uppercase(), 10 * dp, baseline(smallPaint, mid), smallPaint)
                smallPaint.typeface = Typeface.DEFAULT
                smallPaint.letterSpacing = 0f
                return
            }
            if (position == sel) {
                fill.color = selectionColor(listFocused())
                canvas.drawRect(0f, 0f, w, height.toFloat(), fill)
            }
            // At the right: the stage (+) or unstage (−) button, then the letter.
            val letterX = w - 20 * dp
            val buttonMid = w - STAGE_W * dp + 16 * dp
            letterPaint.color = r.letterColor
            canvas.drawText(r.letter.toString(), letterX, baseline(letterPaint, mid), letterPaint)
            textPaint.color = Palette.MUTED
            val sign = if (r.staged) "−" else "+"
            canvas.drawText(sign, buttonMid - textPaint.measureText(sign) / 2,
                            baseline(textPaint, mid), textPaint)
            val room = w - STAGE_W * dp - 14 * dp
            textPaint.color = Palette.TEXT
            val name = TextUtils.ellipsize(r.name, textPaint, room, TextUtils.TruncateAt.END)
            canvas.drawText(name, 0, name.length, 14 * dp, baseline(textPaint, mid), textPaint)
            val nameW = textPaint.measureText(name, 0, name.length)
            val dirRoom = room - nameW - 8 * dp
            if (r.dir.isNotEmpty() && dirRoom > 16 * dp) {
                smallPaint.color = Palette.MUTED
                val dir = TextUtils.ellipsize(r.dir, smallPaint, dirRoom, TextUtils.TruncateAt.START)
                canvas.drawText(dir, 0, dir.length, 14 * dp + nameW + 8 * dp,
                                baseline(textPaint, mid), smallPaint)
            }
        }
    }

    private inner class ChangeAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        override fun getItemCount() = rows.size
        override fun getItemViewType(position: Int) = if (rows[position].header) 1 else 0
        override fun onCreateViewHolder(parent: ViewGroup, type: Int): RecyclerView.ViewHolder {
            val v = ChangeRow()
            v.layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                       px(if (type == 1) HEADER_H else ROW_H))
            val detector = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
                override fun onDown(e: MotionEvent) = true
                override fun onSingleTapUp(e: MotionEvent): Boolean {
                    val pos = v.position
                    val r = rows.getOrNull(pos) ?: return true
                    if (r.header) return true
                    sel = pos
                    inGraph = false
                    requestFocus()
                    redrawLists()
                    if (e.x > v.width - STAGE_W * dp) toggleStage(pos) else showDiff(pos)
                    return true
                }
                override fun onLongPress(e: MotionEvent) {
                    rows.getOrNull(v.position)?.tip?.takeIf { it.isNotEmpty() }?.let {
                        android.widget.Toast.makeText(context, it,
                            android.widget.Toast.LENGTH_LONG).show()
                    }
                }
            })
            v.setOnTouchListener { _, e -> detector.onTouchEvent(e) }
            return object : RecyclerView.ViewHolder(v) {}
        }
        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            (holder.itemView as ChangeRow).apply { this.position = position; invalidate() }
        }
    }

    /** One commit: its lines and dot, ref pills, subject, push or pull arrow. */
    private inner class GraphRowView : View(context) {
        var position = -1

        override fun onDraw(canvas: Canvas) {
            val w = width.toFloat()
            val h = height.toFloat()
            val mid = kotlin.math.floor(h / 2)
            if (position == graphSel) {
                fill.color = selectionColor(graphFocused())
                canvas.drawRect(0f, 0f, w, h, fill)
            }
            if (position >= graphRows.size) {
                textPaint.color = Palette.ACCENT
                canvas.drawText("Show ${GitNative.GRAPH_BATCH} more…", 18 * dp,
                                baseline(textPaint, mid), textPaint)
                return
            }
            val c = graphRows.getOrNull(position) ?: return
            val outgoing = c.flags and 1 != 0
            val incoming = c.flags and 2 != 0
            val lw = (GitNative.laneWidth((w / dp).toDouble(), graphMaxWidth) * dp).toFloat()
            fun x(lane: Int) = 8 * dp + lane * lw + lw / 2

            if ((outgoing || incoming) && position != graphSel) {
                fill.color = ((if (outgoing) Palette.ACCENT else ADDED) and 0x00FFFFFF) or 0x17000000
                canvas.drawRect(0f, 0f, w, h, fill)
            }
            // Lines: straight within a lane, curves between lanes, meeting the
            // dot sideways the way VS Code's graph draws them.
            stroke.strokeWidth = 1.5f * dp
            val path = Path()
            for (e in c.edges) {
                var x0 = 0f; var y0 = 0f; var x1 = 0f; var y1 = 0f
                when (e.kind) {
                    0 -> { x0 = x(e.from); y0 = 0f; x1 = x(e.to); y1 = h }
                    1 -> { x0 = x(e.from); y0 = 0f; x1 = x(c.lane); y1 = mid }
                    else -> { x0 = x(c.lane); y0 = mid; x1 = x(e.to); y1 = h }
                }
                path.reset()
                path.moveTo(x0, y0)
                when {
                    x0 == x1 -> path.lineTo(x1, y1)
                    e.kind == 0 -> path.cubicTo(x0, mid, x1, mid, x1, y1)
                    e.kind == 1 -> path.cubicTo(x0, y1, x0, y1, x1, y1)
                    else -> path.cubicTo(x1, y0, x1, y0, x1, y1)
                }
                stroke.color = laneColor(e.color)
                canvas.drawPath(path, stroke)
            }
            // The dot: filled; hollow when not pushed or pulled yet; a merge
            // has a hole; HEAD gets a ring.
            val dc = laneColor(c.color)
            val cx = x(c.lane)
            val merge = c.flags and 8 != 0
            val bg = if (position == graphSel) selectionColor(graphFocused()) else Palette.SIDEBAR
            if (c.flags and 4 != 0) {
                fill.color = bg
                canvas.drawCircle(cx, mid, 6.5f * dp, fill)
                stroke.strokeWidth = 1.3f * dp
                stroke.color = dc
                canvas.drawCircle(cx, mid, 6f * dp, stroke)
            }
            fill.color = bg
            canvas.drawCircle(cx, mid, 5f * dp, fill)
            if (outgoing || incoming) {
                stroke.strokeWidth = 1.6f * dp
                stroke.color = dc
                canvas.drawCircle(cx, mid, 3.4f * dp, stroke)
                if (merge) { fill.color = dc; canvas.drawCircle(cx, mid, 1.4f * dp, fill) }
            } else {
                fill.color = dc
                canvas.drawCircle(cx, mid, 4f * dp, fill)
                if (merge) { fill.color = bg; canvas.drawCircle(cx, mid, 1.6f * dp, fill) }
            }

            // Labels, then the subject.
            var at = 8 * dp + c.width * lw + 6 * dp
            val right = w - (if (outgoing || incoming) 22 else 6) * dp
            var shown = 0
            for (l in c.labels) {
                val room = minOf(120 * dp, right - at - 40 * dp)
                if (room < 30 * dp) break
                val pw = drawPill(canvas, l, at, mid, room)
                if (pw <= 0) break
                at += pw + 4 * dp
                shown++
            }
            if (shown < c.labels.size && right - at > 20 * dp) {
                smallPaint.color = Palette.MUTED
                val more = "+${c.labels.size - shown}"
                canvas.drawText(more, at, baseline(smallPaint, mid), smallPaint)
                at += smallPaint.measureText(more) + 4 * dp
            }
            if (right - at > 8 * dp) {
                textPaint.color = if (incoming) Palette.MUTED else Palette.TEXT
                val s = TextUtils.ellipsize(c.subject, textPaint, right - at, TextUtils.TruncateAt.END)
                canvas.drawText(s, 0, s.length, at, baseline(textPaint, mid), textPaint)
            }
            if (outgoing || incoming) {
                textPaint.color = if (outgoing) Palette.ACCENT else ADDED
                val arrow = if (outgoing) "↑" else "↓"
                canvas.drawText(arrow, w - 16 * dp, baseline(textPaint, mid), textPaint)
            }
        }

        /** A label: filled for HEAD's branch, outlined for the rest. */
        private fun drawPill(canvas: Canvas, l: Label, x: Float, mid: Float, maxW: Float): Float {
            val color = GitNative.pillColor(l.kind) or 0xFF000000.toInt()
            val name = if (l.kind == 3) "tag ${l.name}" else l.name
            val full = pillPaint.measureText(name)
            val textW = minOf(full, maxOf(0f, maxW - 10 * dp))
            if (textW < 12 * dp) return 0f
            val shownText = if (textW < full)
                TextUtils.ellipsize(name, pillPaint, textW, TextUtils.TruncateAt.MIDDLE) else name
            val bw = textW + 10 * dp
            val rect = RectF(x + 0.5f, mid - 8 * dp + 0.5f, x + bw - 0.5f, mid + 8 * dp - 0.5f)
            val radius = 7.5f * dp
            if (l.current) {
                fill.color = color
                canvas.drawRoundRect(rect, radius, radius, fill)
                pillPaint.color = Palette.BACKGROUND
            } else {
                fill.color = (color and 0x00FFFFFF) or 0x24000000
                canvas.drawRoundRect(rect, radius, radius, fill)
                stroke.strokeWidth = 1f * dp
                stroke.color = (color and 0x00FFFFFF) or 0xB3000000.toInt()
                canvas.drawRoundRect(rect, radius, radius, stroke)
                pillPaint.color = color
            }
            canvas.drawText(shownText, 0, shownText.length, x + 5 * dp,
                            baseline(pillPaint, mid), pillPaint)
            return bw
        }
    }

    private inner class GraphAdapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {
        override fun getItemCount() = if (graphShown) graphCount() else 0
        override fun onCreateViewHolder(parent: ViewGroup, type: Int): RecyclerView.ViewHolder {
            val v = GraphRowView()
            v.layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                       px(GRAPH_H))
            val detector = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
                override fun onDown(e: MotionEvent) = true
                override fun onSingleTapUp(e: MotionEvent): Boolean {
                    val pos = v.position
                    graphSel = pos
                    inGraph = true
                    requestFocus()
                    redrawLists()
                    showCommit(pos)
                    return true
                }
                override fun onLongPress(e: MotionEvent) {
                    if (graph == 0L) return
                    val tip = GitNative.graphToolTip(graph, v.position,
                                                     System.currentTimeMillis() / 1000)
                    if (tip.isNotEmpty()) androidx.appcompat.app.AlertDialog.Builder(context)
                        .setMessage(tip).setPositiveButton("OK", null).show()
                }
            })
            v.setOnTouchListener { _, e -> detector.onTouchEvent(e) }
            return object : RecyclerView.ViewHolder(v) {}
        }
        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            (holder.itemView as GraphRowView).apply { this.position = position; invalidate() }
        }
    }

    // --------------------------------------------------------------- misc

    private fun label(sp: Float, color: Int) = TextView(context).apply {
        textSize = sp
        setTextColor(color)
    }

    private fun px(dpValue: Int) = (dpValue * dp).toInt()

    private inline fun log(text: () -> String) {
        if (Log.isLoggable(TAG, Log.DEBUG)) Log.d(TAG, text())
    }

    companion object {
        /** `adb shell setprop log.tag.MiniCodeGit DEBUG` turns the log on. */
        const val TAG = "MiniCodeGit"
        const val NOT_SHARED = "Source control works on folders in phone storage: git " +
                "runs in Termux, which cannot see into other apps' folders. Open one " +
                "with leader O, Phone storage."
        private const val ROW_H = 34
        private const val HEADER_H = 26
        private const val GRAPH_H = 30
        private const val STAGE_W = 56   // the + or − and the letter, in dp
        private val ADDED = 0xFF73C991.toInt()
        private val ERROR = 0xFFF14C4C.toInt()
        private val HASH = 0xFFE2C08D.toInt()
        private val SELECTED = 0xFF04395E.toInt()
        private val SELECTED_INACTIVE = 0xFF37373D.toInt()
        private val ACCENT_DARK = 0xFF0E639C.toInt()
    }
}
