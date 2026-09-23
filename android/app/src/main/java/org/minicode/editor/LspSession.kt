package org.minicode.editor

import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.NoCopySpan
import android.text.Spanned
import android.view.KeyEvent
import android.view.View
import android.widget.ArrayAdapter
import android.widget.ListPopupWindow
import android.widget.TextView
import androidx.documentfile.provider.DocumentFile
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors

/**
 * The shared LSP client (../../../src/LspClient.cpp) behind JNI; see
 * lsp_jni.cpp. Nothing here does I/O: LspSession owns the sockets.
 */
object LspNative {
    init { System.loadLibrary("minicode") }

    external fun create(): Long
    external fun destroy(h: Long)
    external fun initialize(h: Long, root: String, pid: Int)
    external fun didOpen(h: Long, uri: String, languageId: String, text: String)
    external fun didChange(h: Long, uri: String, text: String)
    external fun didSave(h: Long, uri: String, text: String)
    external fun didClose(h: Long, uri: String)
    external fun completion(h: Long, uri: String, line: Int, character: Int, trigger: String): Int
    /** The last completion list narrowed to `prefix`, as a JSON array. */
    external fun filter(h: Long, prefix: String): String
    external fun hover(h: Long, uri: String, line: Int, character: Int): Int
    external fun definition(h: Long, uri: String, line: Int, character: Int): Int
    external fun cancel(h: Long, id: Int)
    external fun shutdown(h: Long)
    external fun receive(h: Long, data: ByteArray, length: Int)
    external fun takeOutgoing(h: Long): ByteArray?
    external fun nextEvent(h: Long): String?

    /** {server key, languageId}, or null when no server handles `ext`. */
    external fun language(ext: String): Array<String>?
    external fun commands(server: String): Array<String>
    external fun displayName(server: String): String
    external fun uriFromPath(path: String): String
    /** (line, character) pairs to UTF-16 offsets into `text`. */
    external fun offsets(text: String, positions: IntArray): IntArray
    /** An offset into `text` as {line, character}. */
    external fun position(text: String, offset: Int): IntArray
}

/**
 * A diagnostic's range in the editor. It is only a marker: CodeEditText draws
 * the squiggle under it, and the span moves with edits by itself until the
 * server publishes again. NoCopySpan keeps it out of copied text.
 */
class DiagnosticSpan(val severity: Int, val message: String) : NoCopySpan

/**
 * Language servers for the Android editor, the phone's counterpart of the
 * LspSession in Lsp.mm.
 *
 * Servers run in Termux (see Termux.kt), one per language per project,
 * started the first time a file of that language opens. Termux sees only
 * shared storage, so only files there get a server; the paths are the same
 * on both sides. MainActivity's hooks are few on purpose: [opened], [edited],
 * [saved], [handleKey], [shutdown], plus three leader letters.
 */
class LspSession(private val activity: MainActivity,
                 private val editor: CodeEditText,
                 private val bar: TextView) {

    private val ui = Handler(Looper.getMainLooper())

    /** One server process and the client that speaks to it. */
    private inner class Server(val key: String, val root: String) {
        val handle = LspNative.create()
        val name = LspNative.displayName(key)
        @Volatile var process: Termux.Process? = null
        var ready = false
        var gone = false
        var failure: String? = null
        var triggers: List<String> = emptyList()
        @Volatile var closed = false
        private val writer = Executors.newSingleThreadExecutor()

        /**
         * Sends whatever the client has queued, in order. Before the server
         * has connected the bytes stay queued in the client.
         */
        fun flush() {
            if (process == null) return
            val bytes = LspNative.takeOutgoing(handle) ?: return
            writer.execute {
                try {
                    process?.output?.let { it.write(bytes); it.flush() }
                } catch (e: Exception) {}
            }
        }

        fun start() {
            LspNative.initialize(handle, root, android.os.Process.myPid())
            Thread({
                val p = try {
                    Termux.start(activity, launchCommand(key), root,
                                 stderrTo = if (debuggable)
                                     "${Termux.HOME}/.minicode-lsp.log" else null)
                } catch (e: Exception) {
                    ui.post { failed(e.message ?: e.toString()) }
                    return@Thread
                }
                process = p
                // Closed while Termux was still starting it: end it now.
                if (closed) p.close()
                flush()
                val buffer = ByteArray(16384)
                val input = p.input
                while (true) {
                    val n = try { input.read(buffer) } catch (e: Exception) { -1 }
                    if (n < 0) break
                    LspNative.receive(handle, buffer, n)
                    flush()
                    ui.post { drain(this) }
                }
                ui.post { exited() }
            }, "minicode-lsp-$key").start()
        }

        private fun failed(message: String) {
            failure = message
            gone = true
            if (closed) release() else status()
        }

        private fun exited() {
            gone = true
            if (!ready && failure == null) {
                failure = "No ${name} language server in Termux " +
                        "(${installHint(key)})"
            } else if (failure == null) {
                failure = "The ${name} language server stopped"
            }
            if (closed) { release(); return }
            clearMarks()
            status()
        }

        /**
         * Asks the server to stop. It exits on the reply or when its input
         * closes, which happens here after a moment regardless. The native
         * client is freed once the reader has seen the end (in [exited]), so
         * nothing can reach it after it is gone.
         */
        fun close() {
            if (closed) return
            closed = true
            if (gone) { release(); return }
            LspNative.shutdown(handle)
            flush()
            ui.postDelayed({ process?.close() }, 1500)
        }

        private fun release() {
            writer.shutdown()
            LspNative.destroy(handle)
        }
    }

    private val servers = mutableMapOf<String, Server>()
    private var server: Server? = null
    private var uri: String? = null
    private var path: String? = null
    private var note: String? = null           // why there is no server
    private val diagnostics = mutableMapOf<String, JSONArray>()
    private var pendingJump: Triple<String, Int, Int>? = null

    init {
        editor.onSelection = { showStatusOrDiagnostic() }
        editor.interceptCommit = { text -> text.toString() == "\n" && acceptSelected() }
        // The bar lives under the editor and goes wherever it goes.
        editor.viewTreeObserver.addOnGlobalLayoutListener { syncBar() }
    }

    // ------------------------------------------------------------ documents

    /**
     * A file opened in the editor (`file` null for anything that is not
     * source: an image, a PDF). `projectRoot` is the open folder.
     */
    fun opened(file: DocumentFile?, projectRoot: DocumentFile?) {
        closeCompletion()
        server?.let { s -> uri?.let { LspNative.didClose(s.handle, it); s.flush() } }
        server = null
        uri = null
        path = null
        note = null
        clearMarks()
        val name = file?.name
        if (file == null || name == null) { status(); return }
        val ext = name.substringAfterLast('.', "").lowercase()
        val lang = LspNative.language(ext)
        if (lang == null) { status(); return }
        val (key, languageId) = lang[0] to lang[1]
        val filePath = pathOf(file)
        if (filePath == null || !Termux.isShared(filePath)) {
            note = "Language servers need the file in phone storage"
            status()
            return
        }
        Termux.problem(activity)?.let {
            note = "Termux not set up: open ⋮ Termux tools"
            status()
            return
        }
        val root = projectRoot?.let(::pathOf)
            ?.takeIf { Termux.isShared(it) && filePath.path.startsWith(it.path) }
            ?: filePath.parentFile ?: return
        // Servers for another project are done with.
        for ((k, s) in servers.entries.toList()) {
            if (s.root != root.path) { s.close(); servers.remove(k) }
        }
        val s = servers.getOrPut("$key|${root.path}") {
            Server(key, root.path).also { it.start() }
        }
        server = s
        path = filePath.path
        uri = LspNative.uriFromPath(filePath.path)
        if (!s.gone) {
            LspNative.didOpen(s.handle, uri!!, languageId, text())
            s.flush()
        }
        diagnostics[uri!!]?.let { applyDiagnostics(it) }
        pendingJump?.let { (p, line, ch) ->
            if (p == filePath.path) {
                pendingJump = null
                editor.post { moveCaret(line, ch) }
            }
        }
        status()
    }

    /** From TextWatcher.onTextChanged: `count` units replaced `before` at `start`. */
    fun edited(text: CharSequence, start: Int, before: Int, count: Int) {
        val s = server ?: return
        if (s.gone) return
        ui.removeCallbacks(flushChange)
        ui.postDelayed(flushChange, 300)
        // One character typed: is it the end of ".", "->" or "::"?
        if (before == 0 && count == 1) {
            val c = text[start]
            val prev = if (start > 0) text[start - 1] else ' '
            val trigger = when {
                c == '.' -> "."
                c == '>' && prev == '-' -> ">"
                c == ':' && prev == ':' -> ":"
                else -> null
            }
            if (trigger != null && (s.triggers.isEmpty() || trigger in s.triggers)) {
                editor.post { requestCompletion(trigger) }
                return
            }
        }
        if (completing) editor.post { refilter() }
    }

    private val flushChange = Runnable { flushChanges() }

    private fun flushChanges() {
        ui.removeCallbacks(flushChange)
        val s = server ?: return
        val u = uri ?: return
        if (s.gone) return
        LspNative.didChange(s.handle, u, text())
        s.flush()
    }

    fun saved() {
        val s = server ?: return
        val u = uri ?: return
        if (s.gone) return
        flushChanges()
        LspNative.didSave(s.handle, u, text())
        s.flush()
    }

    /** Stops every server; the activity is going away. */
    fun shutdown() {
        for (s in servers.values) s.close()
        servers.clear()
        server = null
    }

    private fun text() = editor.text?.toString().orEmpty()

    private val debuggable = activity.applicationInfo.flags and
            android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE != 0

    // --------------------------------------------------------------- events

    private fun drain(s: Server) {
        if (s.closed) return
        while (true) {
            val raw = LspNative.nextEvent(s.handle) ?: break
            val e = try { JSONObject(raw) } catch (x: Exception) { continue }
            when (e.optString("type")) {
                "ready" -> {
                    s.ready = true
                    val t = e.optJSONArray("triggers")
                    s.triggers = if (t == null) emptyList()
                                 else (0 until t.length()).map { t.optString(it) }
                    status()
                }
                "diagnostics" -> {
                    val items = e.optJSONArray("items") ?: JSONArray()
                    val u = e.optString("uri")
                    diagnostics[u] = items
                    if (s === server && samePath(u, uri)) applyDiagnostics(items)
                }
                "completion" -> if (s === server) refilter()
                "hover" -> if (s === server) showHover(e.optString("text"))
                "definition" -> if (s === server) jumpTo(e.optJSONArray("locations"))
                "message" -> if (s === server && e.optInt("level") <= 2)
                    flash(e.optString("text"))
                "error" -> android.util.Log.w("MiniCodeLsp", e.optString("text"))
            }
        }
    }

    private fun samePath(a: String, b: String?): Boolean {
        if (b == null) return false
        if (a == b) return true
        return try { File(java.net.URI(a)).canonicalPath == File(java.net.URI(b)).canonicalPath }
        catch (e: Exception) { false }
    }

    // ---------------------------------------------------------- diagnostics

    private fun clearMarks() {
        val editable = editor.text ?: return
        for (span in editable.getSpans(0, editable.length, DiagnosticSpan::class.java))
            editable.removeSpan(span)
        editor.invalidate()
    }

    private fun applyDiagnostics(items: JSONArray) {
        clearMarks()
        val editable = editor.text ?: return
        val text = editable.toString()
        val positions = IntArray(items.length() * 4)
        for (i in 0 until items.length()) {
            val r = items.getJSONObject(i).getJSONArray("range")
            for (k in 0 until 4) positions[i * 4 + k] = r.getInt(k)
        }
        val offsets = LspNative.offsets(text, positions)
        for (i in 0 until items.length()) {
            val d = items.getJSONObject(i)
            var a = offsets[i * 2]
            var b = offsets[i * 2 + 1]
            if (b <= a) {
                // An empty range: mark the word there, or one character.
                b = a
                while (b < text.length && isIdent(text[b])) b++
                if (b == a && a < text.length && text[a] != '\n') b = a + 1
                else if (b == a && a > 0) a--
            }
            if (a < 0 || b > text.length || b <= a) continue
            val severity = d.optInt("severity", 1)
            val kind = when (severity) { 1 -> "Error"; 2 -> "Warning"; 3 -> "Info"; else -> "Hint" }
            editable.setSpan(DiagnosticSpan(severity, "$kind: ${d.optString("message")}"),
                             a, b, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        editor.invalidate()
        status()
    }

    // ------------------------------------------------------------ status bar

    private var flashText: String? = null
    private val endFlash = Runnable { flashText = null; showStatusOrDiagnostic() }

    private fun flash(text: String) {
        flashText = text
        ui.removeCallbacks(endFlash)
        ui.postDelayed(endFlash, 4000)
        showStatusOrDiagnostic()
    }

    private fun status() = showStatusOrDiagnostic()

    /**
     * One line under the editor: the diagnostic at the caret when there is
     * one (the phone's stand-in for the Mac's tooltip), else what the server
     * is doing, else nothing and the bar goes away.
     */
    private fun showStatusOrDiagnostic() {
        val caret = editor.selectionStart
        val editable = editor.text
        val here = editable?.getSpans(caret, caret, DiagnosticSpan::class.java)
            ?.minByOrNull { it.severity }
        val s = server
        val text = when {
            flashText != null -> flashText
            here != null -> here.message
            note != null -> note
            s == null -> null
            s.failure != null -> s.failure
            !s.ready -> "${s.name}: starting in Termux…"
            else -> {
                val all = editable?.getSpans(0, editable.length, DiagnosticSpan::class.java)
                    .orEmpty()
                val errors = all.count { it.severity == 1 }
                val warnings = all.count { it.severity == 2 }
                when {
                    errors + warnings == 0 -> null
                    else -> listOfNotNull(
                        errors.takeIf { it > 0 }?.let { "$it error" + if (it == 1) "" else "s" },
                        warnings.takeIf { it > 0 }?.let { "$it warning" + if (it == 1) "" else "s" }
                    ).joinToString(", ")
                }
            }
        }
        // Set only on a change: this runs on every caret move.
        if (bar.text.toString() != text.orEmpty()) bar.text = text.orEmpty()
        bar.setTextColor(when (here?.severity) {
            1 -> ERROR; 2 -> WARNING; else -> Palette.MUTED
        })
        syncBar()
    }

    private fun syncBar() {
        val want = if (editor.visibility == View.VISIBLE && bar.text.isNotEmpty())
            View.VISIBLE else View.GONE
        if (bar.visibility != want) bar.visibility = want
    }

    // ------------------------------------------------------------ completion

    private var completing = false
    private var anchor = 0
    private var shown = JSONArray()
    private var popup: ListPopupWindow? = null

    /** The leader's N, or a trigger character. */
    fun requestCompletion(trigger: String = "") {
        val s = server
        if (s == null || s.gone) {
            flash(note ?: s?.failure ?: "No language server for this file")
            return
        }
        val u = uri ?: return
        flushChanges()
        val text = text()
        val cursor = editor.selectionEnd
        var a = cursor
        while (a > 0 && isIdent(text[a - 1])) a--
        anchor = a
        completing = true
        val pos = LspNative.position(text, cursor)
        LspNative.completion(s.handle, u, pos[0], pos[1], trigger)
        s.flush()
    }

    private fun refilter() {
        if (!completing) return
        val s = server ?: return closeCompletion()
        val text = text()
        val cursor = editor.selectionEnd
        if (editor.selectionStart != cursor || cursor < anchor || cursor > text.length ||
            !editor.hasFocus()) return closeCompletion()
        for (i in anchor until cursor) if (!isIdent(text[i])) return closeCompletion()
        val prefix = text.substring(anchor, cursor)
        shown = try { JSONArray(LspNative.filter(s.handle, prefix)) } catch (e: Exception) { JSONArray() }
        if (shown.length() == 0) {
            // Still waiting for the server, or nothing matches.
            popup?.dismiss()
            return
        }
        if (shown.length() == 1 && shown.getJSONObject(0).optString("insert") == prefix)
            return closeCompletion()
        showPopup()
    }

    private fun showPopup() {
        val labels = (0 until shown.length()).map {
            val item = shown.getJSONObject(it)
            val detail = item.optString("detail")
            if (detail.isEmpty()) item.optString("label")
            else "${item.optString("label")}  ${detail}"
        }
        val p = popup ?: ListPopupWindow(activity).also { p ->
            popup = p
            p.anchorView = editor
            p.isModal = false
            p.inputMethodMode = ListPopupWindow.INPUT_METHOD_NEEDED
            p.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Palette.SIDEBAR))
            p.setOnItemClickListener { _, _, position, _ -> accept(position) }
        }
        val adapter = object : ArrayAdapter<String>(activity,
            android.R.layout.simple_list_item_1, labels) {
            override fun getView(position: Int, convertView: View?,
                                 parent: android.view.ViewGroup): View {
                val v = super.getView(position, convertView, parent) as TextView
                v.setTextColor(Palette.TEXT)
                v.typeface = android.graphics.Typeface.MONOSPACE
                v.textSize = 13f
                v.isSingleLine = true
                v.ellipsize = android.text.TextUtils.TruncateAt.END
                v.minHeight = 0
                val pad = (4 * activity.resources.displayMetrics.density).toInt()
                v.setPadding(pad * 2, pad, pad * 2, pad)
                return v
            }
        }
        p.setAdapter(adapter)
        // Below the caret's line, or above it when there is no room.
        val layout = editor.layout
        val density = activity.resources.displayMetrics.density
        val rowHeight = (26 * density).toInt()
        val height = minOf(labels.size, 6) * rowHeight
        p.width = (editor.width * 0.85).toInt()
        p.height = height
        if (layout != null) {
            val line = layout.getLineForOffset(anchor)
            val x = layout.getPrimaryHorizontal(anchor).toInt() +
                    editor.totalPaddingLeft - editor.scrollX
            val below = layout.getLineBottom(line) + editor.totalPaddingTop - editor.scrollY
            val above = layout.getLineTop(line) + editor.totalPaddingTop - editor.scrollY
            p.horizontalOffset = x.coerceIn(0, maxOf(0, editor.width - p.width))
            p.verticalOffset = if (below + height <= editor.height) below - editor.height
                               else above - editor.height - height
        }
        if (!p.isShowing) p.show()
        p.setSelection(0)
        p.listView?.choiceMode = android.widget.AbsListView.CHOICE_MODE_SINGLE
        p.listView?.setItemChecked(0, true)
    }

    private fun closeCompletion() {
        completing = false
        shown = JSONArray()
        popup?.dismiss()
    }

    private fun selectedRow(): Int {
        val p = popup ?: return -1
        val row = p.listView?.checkedItemPosition ?: -1
        return if (row >= 0) row else p.selectedItemPosition.coerceAtLeast(0)
    }

    private fun acceptSelected(): Boolean {
        if (!completing || popup?.isShowing != true || shown.length() == 0) return false
        accept(selectedRow())
        return true
    }

    private fun accept(row: Int) {
        if (row < 0 || row >= shown.length()) return closeCompletion()
        val item = shown.getJSONObject(row)
        val editable = editor.text ?: return closeCompletion()
        val cursor = editor.selectionEnd
        var start = anchor
        item.optJSONArray("edit")?.let { e ->
            // The edit's range was worked out on the text as it was when
            // asked; everything before the word is unchanged since.
            val at = LspNative.offsets(editable.toString(),
                                       intArrayOf(e.getInt(0), e.getInt(1)))[0]
            if (at <= cursor) start = at
        }
        closeCompletion()
        val insert = item.optString("insert")
        editable.replace(start, cursor, insert)
        editor.setSelection((start + insert.length).coerceAtMost(editable.length))
    }

    /**
     * Keys while the completion list is up: arrows move through it, Enter
     * and Tab accept, Escape and Back close it. Anything else goes on to
     * the editor, and typing narrows the list.
     */
    fun handleKey(event: KeyEvent): Boolean {
        val p = popup
        if (!completing || p == null || !p.isShowing) return false
        val count = shown.length()
        val row = selectedRow()
        when (event.keyCode) {
            KeyEvent.KEYCODE_DPAD_DOWN -> select((row + 1).coerceAtMost(count - 1))
            KeyEvent.KEYCODE_DPAD_UP -> select((row - 1).coerceAtLeast(0))
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER,
            KeyEvent.KEYCODE_TAB, KeyEvent.KEYCODE_DPAD_CENTER -> accept(row)
            KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_BACK -> closeCompletion()
            else -> return false
        }
        return true
    }

    private fun select(row: Int) {
        val list = popup?.listView ?: return
        list.setItemChecked(row, true)
        list.setSelection(row)
    }

    // --------------------------------------------------- hover and definition

    /** The leader's K: what the server says about the symbol at the caret. */
    fun hover() {
        val s = readyServer() ?: return
        flushChanges()
        val pos = LspNative.position(text(), editor.selectionStart)
        LspNative.hover(s.handle, uri!!, pos[0], pos[1])
        s.flush()
    }

    private fun showHover(text: String) {
        if (text.isBlank()) { flash("Nothing to show here"); return }
        androidx.appcompat.app.AlertDialog.Builder(activity)
            .setMessage(text.trim())
            .setPositiveButton("OK", null)
            .show()
            .findViewById<TextView>(android.R.id.message)
            ?.typeface = android.graphics.Typeface.MONOSPACE
    }

    /** The leader's G: go to where the symbol at the caret is defined. */
    fun definition() {
        val s = readyServer() ?: return
        flushChanges()
        val pos = LspNative.position(text(), editor.selectionStart)
        LspNative.definition(s.handle, uri!!, pos[0], pos[1])
        s.flush()
    }

    private fun jumpTo(locations: JSONArray?) {
        val first = locations?.optJSONObject(0)
        if (first == null) { flash("No definition found"); return }
        val target = first.optString("path")
        val r = first.getJSONArray("range")
        val line = r.getInt(0)
        val ch = r.getInt(1)
        if (target.isEmpty()) return
        if (path != null && File(target).canonicalPath == File(path!!).canonicalPath) {
            moveCaret(line, ch)
            return
        }
        val f = File(target)
        if (!f.canRead()) { flash("Cannot open ${f.name}"); return }
        pendingJump = Triple(f.path, line, ch)
        activity.openPath(f)
    }

    private fun moveCaret(line: Int, ch: Int) {
        val text = text()
        val at = LspNative.offsets(text, intArrayOf(line, ch))[0].coerceIn(0, text.length)
        editor.requestFocus()
        editor.setSelection(at)
    }

    private fun readyServer(): Server? {
        val s = server
        if (s == null || s.gone || uri == null) {
            flash(note ?: s?.failure ?: "No language server for this file")
            return null
        }
        return s
    }

    // ---------------------------------------------------------------- helpers

    private fun isIdent(c: Char) = c.isLetterOrDigit() || c == '_' || c == '$'

    /** A document's path on the phone, or null when it has none. */
    private fun pathOf(file: DocumentFile): File? {
        val uri = file.uri
        if (uri.scheme == "file") return uri.path?.let(::File)
        if (uri.authority != "com.android.externalstorage.documents") return null
        val id = try {
            if (android.provider.DocumentsContract.isDocumentUri(activity, uri))
                android.provider.DocumentsContract.getDocumentId(uri)
            else android.provider.DocumentsContract.getTreeDocumentId(uri)
        } catch (e: IllegalArgumentException) { return null }
        val volume = id.substringBefore(':')
        val rest = id.substringAfter(':', "")
        val root = if (volume == "primary")
            android.os.Environment.getExternalStorageDirectory().path
        else "/storage/$volume"
        return File(if (rest.isEmpty()) root else "$root/$rest")
    }

    companion object {
        const val ERROR = 0xFFF14C4C.toInt()
        const val WARNING = 0xFFCCA700.toInt()
        const val INFO = 0xFF3794FF.toInt()

        /**
         * The command Termux runs: the first of the core's default commands
         * that is installed. When none is, bash exits without speaking, and
         * the session reports the server as missing.
         */
        fun launchCommand(server: String): String {
            val tries = LspNative.commands(server).joinToString(" ") { cmd ->
                val program = cmd.substringBefore(' ')
                "if command -v $program >/dev/null 2>&1; then exec $cmd; fi;"
            }
            return "bash -c ${Termux.shellQuote("$tries exit 3")}"
        }

        fun installHint(server: String) = when (server) {
            "cpp" -> "pkg install clang"
            "python" -> "pip install python-lsp-server"
            "go" -> "pkg install gopls"
            "rust" -> "pkg install rust-analyzer"
            "typescript" -> "npm install -g typescript-language-server typescript"
            else -> "install one with pkg"
        }

        /**
         * The squiggles, drawn by CodeEditText after its text: a wave under
         * each marked range on the lines in view, like the Mac editor's.
         */
        fun drawDiagnostics(view: CodeEditText, canvas: Canvas, paint: Paint) {
            val editable = view.text ?: return
            val layout = view.layout ?: return
            val spans = editable.getSpans(0, editable.length, DiagnosticSpan::class.java)
            if (spans.isEmpty()) return
            val density = view.resources.displayMetrics.density
            val amp = 1.5f * density
            val step = 3f * density
            val firstLine = layout.getLineForVertical(view.scrollY)
            val lastLine = layout.getLineForVertical(view.scrollY + view.height)
            canvas.save()
            canvas.translate(view.totalPaddingLeft.toFloat(), view.totalPaddingTop.toFloat())
            val path = Path()
            for (span in spans.sortedByDescending { it.severity }) {
                val a = editable.getSpanStart(span)
                val b = editable.getSpanEnd(span)
                if (a < 0 || b <= a) continue
                paint.color = when (span.severity) { 1 -> ERROR; 2 -> WARNING; else -> INFO }
                val l0 = maxOf(layout.getLineForOffset(a), firstLine)
                val l1 = minOf(layout.getLineForOffset(b), lastLine)
                for (line in l0..l1) {
                    val s = maxOf(a, layout.getLineStart(line))
                    val e = minOf(b, layout.getLineEnd(line))
                    if (e <= s) continue
                    val x0 = layout.getPrimaryHorizontal(s)
                    var x1 = if (e >= layout.getLineEnd(line)) layout.getLineRight(line)
                             else layout.getPrimaryHorizontal(e)
                    if (x1 <= x0) x1 = x0 + step * 2
                    val y = layout.getLineBaseline(line) + 2.5f * density
                    path.reset()
                    path.moveTo(x0, y)
                    var x = x0
                    var up = true
                    while (x < x1) {
                        x = minOf(x + step, x1)
                        path.lineTo(x, if (up) y - amp else y)
                        up = !up
                    }
                    canvas.drawPath(path, paint)
                }
            }
            canvas.restore()
        }
    }
}
