package org.minicode.editor

import android.content.Intent
import java.io.File
import android.net.Uri
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.KeyEvent
import android.view.View
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.minicode.editor.databinding.ActivityMainBinding

/**
 * The window: a file list that slides over the editor, and the editor itself.
 *
 * The screen this is built for is about 576 by 640 dp (a Unihertz Titan 2),
 * so panes take turns instead of sitting side by side. The file list covers
 * the editor and closes as soon as a file opens.
 *
 * Folders are opened through the system document picker, so the app needs no
 * storage permission and keeps access to what you picked across launches.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var ui: ActivityMainBinding
    private val files = FileListAdapter(::openEntry) { makeRoot(it) }

    private var folder: DocumentFile? = null
    private var current: DocumentFile? = null    // the folder being listed
    private var currentFile: DocumentFile? = null // the file in the editor
    private var dirty = false
    private var highlighting = false

    private val pickFolder =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri != null) confirmLeave { useFolder(uri, remember = true) }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ui = ActivityMainBinding.inflate(layoutInflater)
        setContentView(ui.root)

        // Ask the keyboard for plain keys: no autocorrect, no suggestions and
        // no composing region. Without this the keyboard composes words and
        // commits them in one go ("xys" for one keypress), which swallows
        // letters a shortcut needs and rewrites code as though it were prose.
        ui.editor.inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        ui.editor.typeface = android.graphics.Typeface.MONOSPACE
        ui.editor.privateImeOptions = "nm"   // numeric/no-prediction hint some IMEs honour

        setTextSize(getSharedPreferences("minicode", MODE_PRIVATE).getInt("textSize", 13))

        // A double tap in the LaTeX preview edits the source behind it; the
        // splice goes through the buffer, so it is highlighted, marked
        // unsaved and typeset again like any other edit.
        ui.latex.onEdit = { start, end, text -> ui.editor.text?.replace(start, end, text) }

        ui.fileList.layoutManager = LinearLayoutManager(this)
        ui.fileList.adapter = files
        ui.up.setOnClickListener { goUp() }
        ui.menu.setOnClickListener { showMenu() }

        // Tapping a file reference or URL in the terminal opens it. Relative
        // paths are tried in the shell's folder, then the open one, then the
        // shell's home, which is last because `~/` means it.
        ui.terminal.linkDirs = {
            listOfNotNull(shellFolder, folderPath()?.path, filesDir.absolutePath).distinct()
        }
        ui.terminal.onLink = ::openTerminalLink

        ui.editor.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {
                if (!highlighting && s != null) highlighter.edited(s, a, b, c)
                if (!highlighting && s != null) lsp.edited(s, a, b, c)
            }
            override fun afterTextChanged(s: Editable?) {
                if (highlighting) return
                dirty = true
                updateTitle()
                val name = currentFile?.name
                if (s != null && name != null) highlighter.paint(s, name)
                if (previewing) renderPreview()
            }
        })

        // A plain path can be passed in, which is how the development loop
        // drives the app over adb:
        //   am start -n org.minicode.editor/.MainActivity --es folder <path>
        // Everyday use goes through the document picker instead.
        val path = intent?.getStringExtra("folder")
        val prefs = getSharedPreferences("minicode", MODE_PRIVATE)
        val saved = prefs.getString("folder", null)
        val savedPath = prefs.getString("folderPath", null)
        when {
            path != null -> usePath(File(path))
            savedPath != null && canReachPaths() -> usePath(File(savedPath))
            saved != null -> useFolder(Uri.parse(saved), remember = false)
            else -> showList(true)
        }
    }

    // ------------------------------------------------------------ the folder

    private fun useFolder(uri: Uri, remember: Boolean) {
        if (remember) {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            getSharedPreferences("minicode", MODE_PRIVATE).edit()
                .putString("folder", uri.toString()).remove("folderPath").apply()
        }
        val tree = DocumentFile.fromTreeUri(this, uri) ?: return
        folder = tree
        followFolder()
        list(tree)
        showList(true)
    }

    private fun usePath(dir: File, remember: Boolean = false) {
        if (remember) {
            getSharedPreferences("minicode", MODE_PRIVATE).edit()
                .putString("folderPath", dir.path).remove("folder").apply()
        }
        val tree = DocumentFile.fromFile(dir)
        folder = tree
        followFolder()
        list(tree)
        showList(true)
    }

    /**
     * The open folder as a path a shell can use, or null when there is none.
     *
     * A folder picked in the document picker is a content URI. One on the
     * phone's own storage maps onto /storage/<volume>/<path>, and a shell can
     * go there once the app has "All files access". A folder from a cloud
     * provider (Google Drive and the like) has no path at all, so the
     * terminal cannot follow it there.
     */
    private fun folderPath(): File? {
        val tree = current ?: folder ?: return null
        val uri = tree.uri
        if (uri.scheme == "file") return uri.path?.let(::File)
        if (uri.authority != "com.android.externalstorage.documents") return null
        val id = try {
            if (android.provider.DocumentsContract.isDocumentUri(this, uri))
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

    private fun canReachPaths() =
        android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.R ||
                android.os.Environment.isExternalStorageManager()

    /** Where a new shell should start, and why not the folder if it cannot. */
    private fun terminalDirectory(): Pair<String?, String?> {
        val path = folderPath()
        val name = folder?.name ?: "this folder"
        return when {
            folder == null -> null to null
            path == null -> null to notReachable(name)
            !canReachPaths() -> null to "This is MiniCode's own private folder. " +
                    "Allow \"All files access\" for MiniCode in Settings and " +
                    "the terminal moves to $name."
            else -> path.path to null
        }
    }

    /**
     * Why the shell cannot enter a folder with no path, in terms of where the
     * folder actually lives. Termux's folders are the common case on a phone
     * set up for code, and the answer there is shared storage, which both
     * apps can reach.
     */
    private fun notReachable(name: String): String {
        val where = folder?.uri?.authority.orEmpty()
        val why = when {
            where.startsWith("com.termux") ->
                "$name is inside Termux, and Android keeps each app's files " +
                "private, so MiniCode's shell cannot enter it (nor can any " +
                "other app's). Keep the project in shared storage instead: in " +
                "Termux run termux-setup-storage and work under " +
                "~/storage/shared, then open that folder here with leader O, " +
                "Phone storage."
            where.contains("google") ->
                "$name is in Google Drive, which has no path a shell can use. " +
                "Open a folder on the phone's storage instead: leader O, " +
                "Phone storage."
            else ->
                "$name comes from another app's storage, which has no path a " +
                "shell can use. Open a folder on the phone's storage instead: " +
                "leader O, Phone storage."
        }
        return "$why This shell is in MiniCode's own private folder."
    }

    /**
     * Where to open a folder from. The phone's storage is browsed here, by
     * path, which is what lets the terminal follow it; Android's own picker
     * hides that storage behind a menu on many phones and cannot pick its
     * top level at all. The picker is still there for Drive, Termux and
     * other apps' folders, which the editor can use but a shell cannot.
     */
    private fun openFolder() {
        val storage = android.os.Environment.getExternalStorageDirectory()
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Open a folder")
            .setItems(arrayOf("Phone storage (works in the terminal)",
                              "Another app or cloud (Drive, Termux, ...)")) { _, which ->
                when {
                    which == 1 -> pickFolder.launch(null)
                    !canReachPaths() -> askForPathAccess()
                    else -> confirmLeave { usePath(storage, remember = true) }
                }
            }
            .show()
    }

    /**
     * Makes the folder being listed the project: its root in the file list,
     * and where the terminal goes. The file list's long press on a folder
     * does this, so a project deep in storage can be opened directly.
     */
    fun makeRoot(dir: DocumentFile) {
        val path = dir.uri.takeIf { it.scheme == "file" }?.path ?: return
        confirmLeave { usePath(File(path), remember = true) }
    }

    /** A running shell follows the folder when another is opened. */
    private fun followFolder() {
        if (!ui.terminal.isRunning) return
        val (cwd, why) = terminalDirectory()
        if (cwd != null && cwd != shellFolder) {
            ui.terminal.changeDirectory(cwd)
            shellFolder = cwd
        } else if (cwd == null && why != null) {
            ui.terminal.notice(why)
        }
    }

    /** The folder the shell was last put in, or null for the app's own. */
    private var shellFolder: String? = null

    /** Back from Settings with access granted, the shell moves at once. */
    override fun onResume() {
        super.onResume()
        if (ui.terminal.isRunning && shellFolder == null && canReachPaths()) followFolder()
    }

    private fun askForPathAccess() {
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Terminal in this folder")
            .setMessage("A shell reaches files by path, and Android allows that " +
                    "outside the app only with \"All files access\". The editor " +
                    "does not need it. Open the setting?")
            .setPositiveButton("Open settings") { _, _ ->
                startActivity(Intent(
                    android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:$packageName")))
            }
            .setNegativeButton("Not now", null)
            .show()
    }

    private fun list(dir: DocumentFile) {
        current = dir
        // Folders first, then files, each alphabetically, as the other ports do.
        val entries = dir.listFiles().sortedWith(
            compareBy({ !it.isDirectory }, { it.name?.lowercase() ?: "" }))
        files.submit(entries)
        ui.up.visibility = if (dir.uri == folder?.uri) View.GONE else View.VISIBLE
        updateTitle()
    }

    private fun goUp() {
        val parent = current?.parentFile ?: folder ?: return
        list(parent)
    }

    private fun openEntry(entry: DocumentFile) {
        if (entry.isDirectory) { list(entry); return }
        confirmLeave {
            // Pictures and PDFs are shown, not read as text, so an image never
            // reaches the editor and cannot be saved over.
            if (!showMedia(entry)) {
                val text = readText(entry)
                if (text == null) cannotDisplay(entry)
                else openFile(entry, text)
            }
        }
    }

    /**
     * The file as text, or null when it is not text the editor can hold:
     * too large, holding NUL bytes, or not valid UTF-8. The Mac app shows
     * "Cannot display" for the same files. Opening one as text and saving
     * would write the decoder's replacement characters over the original.
     */
    private fun readText(entry: DocumentFile): String? {
        if (entry.length() > MAX_TEXT_BYTES) return null
        val bytes = try {
            contentResolver.openInputStream(entry.uri)?.use { it.readBytes() }
        } catch (e: Exception) {
            null
        } ?: return null
        if (bytes.size > MAX_TEXT_BYTES) return null
        for (i in 0 until minOf(bytes.size, 8192)) if (bytes[i] == 0.toByte()) return null
        return try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
                .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT)
                .decode(java.nio.ByteBuffer.wrap(bytes)).toString()
        } catch (e: java.nio.charset.CharacterCodingException) {
            null
        }
    }

    private fun cannotDisplay(entry: DocumentFile) {
        android.widget.Toast.makeText(this, "Cannot display ${entry.name}",
                                      android.widget.Toast.LENGTH_SHORT).show()
    }

    /**
     * Runs `then` once the buffer may be replaced: at once when nothing is
     * unsaved, otherwise after the user chooses to save or discard. Cancel
     * leaves everything as it was, and so does a save that fails.
     */
    private fun confirmLeave(then: () -> Unit) {
        val file = currentFile
        if (!dirty || showingMedia || file == null) { then(); return }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Save changes to ${file.name}?")
            .setPositiveButton("Save") { _, _ -> if (save()) then() }
            .setNegativeButton("Discard") { _, _ -> dirty = false; then() }
            .setNeutralButton("Cancel", null)
            .show()
    }

    /**
     * An image or the first page of a PDF, in the editor's slot. Both are
     * decoded by Android itself, as the macOS app leaves them to AppKit and
     * PDFKit; there is nothing here for the shared core to do.
     */
    private fun showMedia(entry: DocumentFile): Boolean {
        val name = entry.name?.lowercase() ?: return false
        val isImage = listOf(".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp",
                             ".heic", ".heif").any { name.endsWith(it) }
        val isPdf = name.endsWith(".pdf")
        if (!isImage && !isPdf) return false

        val bitmap = if (isPdf) firstPdfPage(entry) else decodeImage(entry)
        if (bitmap == null) {
            cannotDisplay(entry)
            return true
        }
        currentFile = entry
        if (!entry.name.orEmpty().lowercase().endsWith(".pdf")) pdfPages = 0
        mediaSize = "${bitmap.width} × ${bitmap.height}"
        showingMedia = true
        previewing = false
        ui.latex.close()
        dirty = false
        ui.media.setImageBitmap(bitmap)
        // Scaled down to fit, never up past its real size, as on the Mac.
        ui.media.post {
            val fits = bitmap.width <= ui.media.width && bitmap.height <= ui.media.height
            ui.media.scaleType = if (fits) android.widget.ImageView.ScaleType.CENTER
                                 else android.widget.ImageView.ScaleType.FIT_CENTER
        }
        showList(false)
        return true
    }

    private fun decodeImage(entry: DocumentFile): android.graphics.Bitmap? =
        contentResolver.openInputStream(entry.uri)?.use {
            android.graphics.BitmapFactory.decodeStream(it)
        }

    /** The first page of a PDF, rendered at the width of the screen. */
    private fun firstPdfPage(entry: DocumentFile): android.graphics.Bitmap? {
        val descriptor = contentResolver.openFileDescriptor(entry.uri, "r") ?: return null
        descriptor.use { file ->
            android.graphics.pdf.PdfRenderer(file).use { pdf ->
                if (pdf.pageCount == 0) return null
                pdf.openPage(0).use { page ->
                    val width = resources.displayMetrics.widthPixels
                    val height = width * page.height / page.width
                    val bitmap = android.graphics.Bitmap.createBitmap(
                        width, height, android.graphics.Bitmap.Config.ARGB_8888)
                    bitmap.eraseColor(android.graphics.Color.WHITE)
                    page.render(bitmap, null, null,
                                android.graphics.pdf.PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY)
                    pdfPages = pdf.pageCount
                    return bitmap
                }
            }
        }
    }

    private var showingMedia = false
    private var mediaSize = ""
    private var pdfPages = 0

    // ------------------------------------------------------------ the editor

    private fun openFile(file: DocumentFile, text: String) {
        currentFile = file
        showingMedia = false
        ui.media.setImageDrawable(null)
        highlighting = true
        ui.editor.setText(text)
        highlighting = false
        dirty = false
        // Markdown and LaTeX open rendered, as they do in the other ports.
        previewing = isPreviewable(file.name)
        if (LatexPreview.isLatex(file.name)) ui.latex.open(file, text)
        else ui.latex.close()
        showList(false)
        updateTitle()
        rehighlight()
        renderPreview()
        lsp.opened(file, folder)
    }

    private fun isMarkdown(name: String?): Boolean {
        val lower = name?.lowercase() ?: return false
        return lower.endsWith(".md") || lower.endsWith(".markdown")
    }

    /** Files that open rendered: Markdown, and LaTeX (see LatexPreview). */
    private fun isPreviewable(name: String?) =
        isMarkdown(name) || LatexPreview.isLatex(name)

    /** The view the rendered form of `name` goes in. */
    private fun previewPane(name: String?): View =
        if (LatexPreview.isLatex(name)) ui.latex else ui.previewScroll

    /** Shift+Cmd+P on the Mac; the leader's P here. */
    private fun togglePreview() {
        if (!isPreviewable(currentFile?.name)) return
        previewing = !previewing
        renderPreview()
        updateTitle()
    }

    private fun renderPreview() {
        val name = currentFile?.name
        val showPreview = previewing && isPreviewable(name) &&
                ui.fileList.visibility != View.VISIBLE && !terminalShowing
        ui.previewScroll.visibility =
            if (showPreview && isMarkdown(name)) View.VISIBLE else View.GONE
        ui.latex.visibility =
            if (showPreview && LatexPreview.isLatex(name)) View.VISIBLE else View.GONE
        if (showPreview && LatexPreview.isLatex(name)) {
            ui.editor.visibility = View.GONE
            ui.latex.update(ui.editor.text.toString())
            return
        }
        if (!showPreview) {
            if (!terminalShowing && ui.fileList.visibility != View.VISIBLE) {
                ui.editor.visibility = View.VISIBLE
            }
            return
        }
        ui.editor.visibility = View.GONE
        ui.preview.text = Markdown.render(ui.editor.text.toString(),
                                          resources.displayMetrics.density)
    }

    private var previewing = false

    private val highlighter = Highlighter()

    /** Language servers, run in Termux; see LspSession.kt. */
    private val lsp by lazy { LspSession(this, ui.editor, ui.lspBar) }

    /** Opens a file by path, for go-to-definition into another file. */
    fun openPath(file: File) = openEntry(DocumentFile.fromFile(file))

    /**
     * A tapped terminal link: a URL goes to the browser pane, a file to the
     * editor with the caret on the line and column the compiler named.
     */
    private fun openTerminalLink(link: TerminalView.Link) {
        link.url?.let { url ->
            if (!browserShowing) toggleBrowser()
            navigate(url)
            return
        }
        val file = link.file ?: return
        confirmLeave {
            terminalShowing = false
            openPath(file)
            if (currentFile?.uri?.path == file.path && link.line > 0) {
                ui.editor.post { moveCaretTo(link.line, link.column) }
            }
            updateTitle()
        }
    }

    /** 1-based line and column; 0 for the column means the line's start. */
    private fun moveCaretTo(line: Int, column: Int) {
        val text = ui.editor.text ?: return
        var at = 0
        repeat(line - 1) {
            val nl = text.indexOf('\n', at)
            if (nl < 0) return@repeat
            at = nl + 1
        }
        val lineEnd = text.indexOf('\n', at).let { if (it < 0) text.length else it }
        ui.editor.requestFocus()
        ui.editor.setSelection((at + maxOf(column - 1, 0)).coerceAtMost(lineEnd))
    }

    override fun onDestroy() {
        lsp.shutdown()
        super.onDestroy()
    }

    /**
     * Colors the whole file from the core's tokens, on opening it. After
     * that the TextWatcher keeps the colors current an edit at a time.
     */
    private fun rehighlight() {
        val name = currentFile?.name ?: return
        val editable = ui.editor.text ?: return
        highlighter.open(editable, name)
    }

    /**
     * Writes the buffer back. Returns false, leaves the buffer marked
     * unsaved and says so when the write fails: a provider can refuse, the
     * folder can be gone, or the permission revoked.
     */
    private fun save(): Boolean {
        if (showingMedia) return true
        val file = currentFile ?: return true
        val written = try {
            contentResolver.openOutputStream(file.uri, "wt")?.use {
                it.write(ui.editor.text.toString().toByteArray(Charsets.UTF_8))
                true
            } ?: false
        } catch (e: Exception) {
            false
        }
        if (!written) {
            android.widget.Toast.makeText(this, "Could not save ${file.name}",
                                          android.widget.Toast.LENGTH_LONG).show()
            return false
        }
        dirty = false
        updateTitle()
        if (LatexPreview.isLatex(file.name)) ui.latex.typesetNow()
        lsp.saved()
        return true
    }

    private fun showList(show: Boolean) {
        if (show) { terminalShowing = false; browserShowing = false }
        ui.browser.visibility = View.GONE
        ui.fileList.visibility = if (show) View.VISIBLE else View.GONE
        ui.terminal.visibility = View.GONE
        val preview = !show && previewing && isPreviewable(currentFile?.name)
        val media = !show && showingMedia
        val pane = previewPane(currentFile?.name)
        ui.previewScroll.visibility =
            if (preview && pane == ui.previewScroll) View.VISIBLE else View.GONE
        ui.latex.visibility = if (preview && pane == ui.latex) View.VISIBLE else View.GONE
        ui.media.visibility = if (media) View.VISIBLE else View.GONE
        ui.editor.visibility =
            if (show || preview || media) View.GONE else View.VISIBLE
        ui.up.visibility =
            if (show && current?.uri != folder?.uri) View.VISIBLE else View.GONE
        if (!show) ui.editor.requestFocus()
        updateTitle()
    }

    /** One line of chrome: the folder while listing, the file while editing. */
    private fun updateTitle() {
        val waiting = if (leaderArmed) "  …" else ""
        if (terminalShowing) { ui.title.text = "Terminal"; return }
        if (browserShowing) { ui.title.text = "Browser"; return }
        val showingList = ui.fileList.visibility == View.VISIBLE
        val name = if (showingList) (current?.name ?: folder?.name ?: "MiniCode")
                   else (currentFile?.name ?: "MiniCode")
        val mark = if (!showingList && dirty) "● " else ""
        val extra = when {
            showingList || !showingMedia -> ""
            pdfPages > 0 -> "  ${pdfPages} page" + (if (pdfPages == 1) "" else "s")
            else -> "  $mediaSize"
        }
        ui.title.text = mark + name + extra + waiting
    }

    /**
     * Shortcuts are handled here, before the key reaches any view. The
     * editor is a text field and consumes every letter, so a shortcut left
     * to onKeyDown never fires while the cursor is in a file, which is
     * exactly when it is wanted.
     *
     * In a debug build each key is also logged, which is how the bindings
     * were worked out on a keyboard with no Ctrl: `adb logcat -s MiniCodeKeys`.
     * Only the key's code and modifiers are logged, never the character, so
     * nothing typed (a password in the browser) reaches the log.
     */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // The completion list, while it is up, takes arrows, Enter and Esc.
        if (event.action == KeyEvent.ACTION_DOWN && lsp.handleKey(event)) {
            handled.add(event.keyCode)
            return true
        }
        if (event.action == KeyEvent.ACTION_DOWN) {
            if (debuggable) {
                android.util.Log.d("MiniCodeKeys",
                    "code=${event.keyCode} (${KeyEvent.keyCodeToString(event.keyCode)})" +
                            " scan=${event.scanCode} meta=0x${event.metaState.toString(16)}" +
                            " alt=${event.isAltPressed} shift=${event.isShiftPressed}" +
                            " ctrl=${event.isCtrlPressed} sym=${event.isSymPressed}")
            }
            if (handleShortcut(event)) return true
        }
        // Swallow the release of a key whose press was a shortcut, or the
        // text field sees a stray key-up.
        if (event.action == KeyEvent.ACTION_UP) {
            if (event.keyCode in LEADER_KEYS) leaderHeld = false
            if (handled.remove(event.keyCode)) return true
        }
        return super.dispatchKeyEvent(event)
    }

    private val debuggable by lazy {
        applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE != 0
    }
    private val handled = mutableSetOf<Int>()
    private var lastLeaderPress = 0L
    private var leaderHeld = false
    private var leaderArmed = false
    private var whenWindowCloses: Runnable? = null

    /**
     * A leader press arms for a moment: a letter typed inside that window is
     * a shortcut rather than text, and nothing typed means the press was a
     * plain pane switch. The title bar shows the wait with an ellipsis.
     */
    private fun arm() {
        leaderArmed = true
        updateTitle()
        val close = Runnable {
            whenWindowCloses = null
            leaderArmed = false
            updateTitle()
            showList(ui.fileList.visibility != View.VISIBLE)
        }
        whenWindowCloses = close
        ui.title.postDelayed(close, LEADER_WINDOW)
    }

    private fun cancelLeader() {
        whenWindowCloses?.let { ui.title.removeCallbacks(it) }
        whenWindowCloses = null
        leaderArmed = false
        updateTitle()
    }

    /**
     * The letter after a leader press. On this class of phone the keyboard
     * reaches the editor through the input method, so the letter arrives as
     * committed text rather than as a key event; CodeEditText hands it here
     * before it can be inserted.
     */
    fun leaderLetter(letter: Char): Boolean {
        if (!leaderArmed) return false
        // A letter with no shortcut is typed as usual, and it ends the wait,
        // so the pane does not switch under it a moment later.
        cancelLeader()
        val action = leaderActions()[letter.lowercaseChar()] ?: return false
        action()
        return true
    }

    /**
     * Shortcuts on a phone keyboard, which has no Ctrl, Esc or Tab.
     *
     * Two things are ruled out on a Titan 2. Alt is the symbol layer, so
     * Alt+S types "4". And Sym, which looked promising because it sets a
     * modifier flag without changing the letter, is claimed by the system
     * for some letters: Sym+H opens the microphone and never reaches an app.
     *
     * So the unlabelled key beside Space is a leader instead: press it, then
     * a letter. Nothing else claims that key, and one press with no letter
     * after it still switches panes, which is the thing worth doing with a
     * single keystroke. Ctrl is accepted as well, for anyone on a USB or
     * Bluetooth keyboard.
     *
     * One table, keyed by letter, for both ways a shortcut can arrive.
     */
    private fun leaderActions(): Map<Char, () -> Unit> = mapOf(
        's' to { save() },
        'f' to { showList(ui.fileList.visibility != View.VISIBLE) },
        'p' to { togglePreview() },
        't' to { toggleTerminal() },
        // B is the browser, as Shift+Cmd+B is on the Mac.
        'b' to { toggleBrowser() },
        'o' to { openFolder() },
        'h' to { showShortcuts() },
        // A keyboard with no Ctrl or Escape still has to drive a shell.
        'c' to { ui.terminal.sendControl('c') },
        'd' to { ui.terminal.sendControl('d') },
        'e' to { ui.terminal.sendEscape() },
        'i' to { ui.terminal.sendTab() },
        // Language servers: N completes, K shows what the symbol is (as K
        // does in vim), G goes to its definition.
        'n' to { lsp.requestCompletion() },
        'k' to { lsp.hover() },
        'g' to { lsp.definition() },
    )

    private fun handleShortcut(event: KeyEvent): Boolean {
        val swap = { showList(ui.fileList.visibility != View.VISIBLE) }
        val letters = leaderActions()
        val actions = mapOf(
            KeyEvent.KEYCODE_S to letters.getValue('s'),
            KeyEvent.KEYCODE_B to letters.getValue('f'),   // Cmd+B on the Mac
            KeyEvent.KEYCODE_O to letters.getValue('o'),
            KeyEvent.KEYCODE_H to letters.getValue('h'),
        )

        if (event.keyCode in LEADER_KEYS) {
            // One press of this key sends a burst of key-downs on a Titan 2
            // (a hundred of them in the log). The release separates one
            // press from the next, so a down while the key is still held is
            // part of the same press, however fast the presses come.
            if (!leaderHeld) {
                leaderHeld = true
                val now = event.eventTime
                if (now - lastLeaderPress < LEADER_WINDOW) {
                    cancelLeader()
                    showMenu()
                } else {
                    arm()
                }
                lastLeaderPress = now
            }
            handled.add(event.keyCode)
            return true
        }
        // A letter after a leader press, when it arrives as a key event:
        // that is what happens in the file list and the terminal, where no
        // text field is taking the keyboard's output.
        if (leaderArmed && event.unicodeChar != 0) {
            val letter = event.unicodeChar.toChar()
            if (leaderLetter(letter)) {
                handled.add(event.keyCode)
                return true
            }
        }
        val act = when {
            event.isCtrlPressed -> actions[event.keyCode] ?: return false
            else -> return false
        }
        handled.add(event.keyCode)
        act()
        return true
    }

    /**
     * The same actions as the shortcuts, for when the keyboard cannot
     * provide them. Phone keyboards differ too much to rely on any key: the
     * Titan 2 has no Ctrl, its Alt types symbols, and the system claims Sym
     * for some letters.
     */
    private fun showMenu() {
        val items = arrayOf("Save", "Files or editor", "Markdown preview",
                            "Terminal", "Browser", "Open a folder", "Text size",
                            "Shortcuts", "Termux tools")
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> save()
                    1 -> showList(ui.fileList.visibility != View.VISIBLE)
                    2 -> togglePreview()
                    3 -> toggleTerminal()
                    4 -> toggleBrowser()
                    5 -> openFolder()
                    6 -> chooseTextSize()
                    7 -> showShortcuts()
                    8 -> termuxSetup()
                }
            }
            .show()
    }

    /**
     * The browser pane: the system web view with a URL bar, as on the Mac.
     * A word rather than an address goes to a search, as it does there.
     */
    private fun toggleBrowser() {
        browserShowing = !browserShowing
        if (browserShowing) {
            terminalShowing = false
            ui.terminal.visibility = View.GONE
            ui.fileList.visibility = View.GONE
            ui.editor.visibility = View.GONE
            ui.previewScroll.visibility = View.GONE
            ui.latex.visibility = View.GONE
            ui.media.visibility = View.GONE
            ui.browser.visibility = View.VISIBLE
            if (!browserReady) {
                browserReady = true
                ui.web.settings.javaScriptEnabled = true
                ui.web.settings.domStorageEnabled = true
                ui.web.webViewClient = android.webkit.WebViewClient()
                ui.url.setOnEditorActionListener { _, _, _ ->
                    navigate(ui.url.text.toString()); true
                }
                navigate("duckduckgo.com")
            }
            ui.url.requestFocus()
        } else {
            ui.browser.visibility = View.GONE
            showList(currentFile == null)
        }
        updateTitle()
    }

    private fun navigate(text: String) {
        val trimmed = text.trim()
        val url = when {
            trimmed.isEmpty() -> return
            trimmed.startsWith("http://") || trimmed.startsWith("https://") -> trimmed
            trimmed.contains(' ') || !trimmed.contains('.') ->
                "https://duckduckgo.com/?q=" + android.net.Uri.encode(trimmed)
            else -> "https://$trimmed"
        }
        ui.web.loadUrl(url)
        (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager)
            .hideSoftInputFromWindow(ui.root.windowToken, 0)
        ui.web.requestFocus()
    }

    /** Back walks the web history first, then closes whatever pane is up. */
    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        when {
            browserShowing && ui.web.canGoBack() -> ui.web.goBack()
            browserShowing -> toggleBrowser()
            terminalShowing -> toggleTerminal()
            ui.fileList.visibility != View.VISIBLE -> showList(true)
            else -> super.onBackPressed()
        }
    }

    private var browserShowing = false
    private var browserReady = false

    /**
     * The terminal pane: Android's own shell on a pty, drawn by the same
     * screen grid the macOS app uses. It runs in the app's own storage,
     * which is the only place it can write, and carries the toybox
     * utilities the system ships.
     */
    private fun toggleTerminal() {
        terminalShowing = !terminalShowing
        if (terminalShowing) {
            ui.fileList.visibility = View.GONE
            ui.editor.visibility = View.GONE
            ui.previewScroll.visibility = View.GONE
            ui.latex.visibility = View.GONE
            ui.media.visibility = View.GONE
            ui.browser.visibility = View.GONE
            browserShowing = false
            ui.terminal.visibility = View.VISIBLE
            ui.terminal.onExit = {
                if (terminalShowing) toggleTerminal()
                ui.terminal.stop()
            }
            if (ui.terminal.isRunning) followFolder()
            if (!ui.terminal.isRunning) {
                val (cwd, why) = terminalDirectory()
                ui.terminal.post {
                    ui.terminal.start(filesDir.absolutePath, cwd ?: filesDir.absolutePath)
                    shellFolder = cwd
                    // Said in the terminal itself, where the question
                    // "why am I here?" comes up, rather than in a toast.
                    if (why != null) ui.terminal.notice(why)
                }
                if (folderPath() != null && !canReachPaths()) askForPathAccess()
            }
            ui.terminal.requestFocus()
            (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager)
                .showSoftInput(ui.terminal, 0)
        } else {
            ui.terminal.visibility = View.GONE
            showList(currentFile == null)
        }
        updateTitle()
    }

    private var terminalShowing = false

    /**
     * How large the code is, which matters more on a phone than anywhere
     * else: at 13sp with the system font scale at 1.3, this screen fits
     * about 43 columns, and every step costs or buys a few.
     */
    private fun chooseTextSize() {
        val sizes = arrayOf(10, 11, 12, 13, 14, 16)
        val labels = sizes.map { "$it sp" }.toTypedArray()
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Text size")
            .setItems(labels) { _, which -> setTextSize(sizes[which]) }
            .show()
    }

    private fun setTextSize(sp: Int) {
        ui.editor.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, sp.toFloat())
        getSharedPreferences("minicode", MODE_PRIVATE).edit()
            .putInt("textSize", sp).apply()
    }

    private val askTermux =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) {
            termuxSetup()
        }

    /**
     * Where Termux integration stands, and the next step to take. Language
     * servers and tectonic run in Termux, so this is the one place that says
     * what is missing, and checks what is installed once it can.
     */
    private fun termuxSetup() {
        val dialog = androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Termux tools")
        when {
            !Termux.isInstalled(this) -> dialog.setMessage(
                "Language servers and the LaTeX preview run in Termux. Install " +
                "Termux from F-Droid, then come back here.")
            !Termux.hasPermission(this) -> {
                dialog.setMessage("MiniCode needs permission to run commands in " +
                        "Termux. Termux also has to allow it: in Termux, run\n\n" +
                        "echo allow-external-apps=true >> ~/.termux/termux.properties\n" +
                        "termux-reload-settings")
                dialog.setPositiveButton("Allow") { _, _ -> askTermux.launch(Termux.PERMISSION) }
            }
            else -> {
                dialog.setMessage("Checking Termux…")
                val shown = dialog.setPositiveButton("OK", null).show()
                Thread {
                    val text = try {
                        val tools = listOf("clangd", "pylsp", "pyright-langserver",
                                           "gopls", "rust-analyzer",
                                           "typescript-language-server", "tectonic")
                        val (_, out) = Termux.run(this, tools.joinToString("; ") {
                            "printf '%s ' $it; command -v $it || echo -"
                        })
                        "Termux runs commands for MiniCode.\n\n" + out.trim() +
                                "\n\nInstall what is missing with pkg, for example " +
                                "pkg install clang tectonic python."
                    } catch (e: Exception) {
                        e.message ?: e.toString()
                    }
                    android.util.Log.i("MiniCodeTermux", text)
                    runOnUiThread { shown.setMessage(text) }
                }.start()
                return
            }
        }
        dialog.setNegativeButton("Close", null).show()
    }

    /** The equivalent of the Mac app's Shift+Cmd+H panel, for a phone. */
    private fun showShortcuts() {
        val lines = listOf(
            "The key left of right Shift, then:",
            "S      Save",
            "F      Files or editor",
            "P      Markdown preview",
            "T      Terminal",
            "B      Browser",
            "O      Open a folder (long-press a",
            "       folder to make it the project)",
            "H      This list",
            "N      Complete (language server)",
            "K      What the symbol is",
            "G      Go to its definition",
            "",
            "In the terminal: C for Ctrl C,",
            "D for Ctrl D, E for Escape,",
            "I for Tab.",
            "",
            "That key alone switches panes,",
            "twice opens this menu, and the ⋮",
            "button does the same. On a USB or",
            "Bluetooth keyboard, Ctrl works too.")
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Shortcuts")
            .setMessage(lines.joinToString(System.lineSeparator()))
            .setPositiveButton("OK", null)
            .show()
    }

    companion object {
        /**
         * The leader keys. 403 is the unlabelled key beside Space on a
         * Titan 2: no standard name, and nothing else claims it. Menu and
         * the Function key are there for other keyboards, and because a
         * standard keycode is one a test can inject.
         */
        private val LEADER_KEYS = setOf(403, KeyEvent.KEYCODE_MENU,
                                        KeyEvent.KEYCODE_FUNCTION)

        /** How long a leader press waits for a letter, in milliseconds. */
        const val LEADER_WINDOW = 700L

        /** Larger files are not opened as text; an EditText would crawl. */
        private const val MAX_TEXT_BYTES = 4L * 1024 * 1024
    }
}

/** The file list: one row per entry, folders marked by a trailing slash. */
class FileListAdapter(private val onClick: (DocumentFile) -> Unit,
                      private val onLongClick: (DocumentFile) -> Unit = {}) :
    RecyclerView.Adapter<FileListAdapter.Row>() {

    private var entries: List<DocumentFile> = emptyList()

    fun submit(list: List<DocumentFile>) {
        entries = list
        notifyDataSetChanged()
    }

    class Row(val text: TextView) : RecyclerView.ViewHolder(text)

    override fun onCreateViewHolder(parent: android.view.ViewGroup, type: Int): Row {
        val dp = parent.resources.displayMetrics.density
        val text = TextView(parent.context).apply {
            setPadding((12 * dp).toInt(), (8 * dp).toInt(),
                       (12 * dp).toInt(), (8 * dp).toInt())
            textSize = 15f
            setTextColor(Palette.TEXT)
        }
        return Row(text)
    }

    override fun onBindViewHolder(row: Row, position: Int) {
        val entry = entries[position]
        row.text.text = (entry.name ?: "?") + if (entry.isDirectory) "/" else ""
        row.text.setTextColor(if (entry.isDirectory) Palette.ACCENT else Palette.TEXT)
        row.text.setOnClickListener { onClick(entry) }
        row.text.setOnLongClickListener {
            if (entry.isDirectory) onLongClick(entry)
            entry.isDirectory
        }
    }

    override fun getItemCount() = entries.size
}
