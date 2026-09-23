package org.minicode.editor

import android.content.Intent
import java.io.File
import android.net.Uri
import android.os.Bundle
import android.text.Editable
import android.text.Spannable
import android.text.TextWatcher
import android.text.style.ForegroundColorSpan
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
    private val files = FileListAdapter(::openEntry)

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

        ui.fileList.layoutManager = LinearLayoutManager(this)
        ui.fileList.adapter = files
        ui.up.setOnClickListener { goUp() }
        ui.menu.setOnClickListener { showMenu() }

        ui.editor.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable?) {
                if (highlighting) return
                dirty = true
                updateTitle()
                rehighlight()
                if (previewing) renderPreview()
            }
        })

        // A plain path can be passed in, which is how the development loop
        // drives the app over adb:
        //   am start -n org.minicode.editor/.MainActivity --es folder <path>
        // Everyday use goes through the document picker instead.
        val path = intent?.getStringExtra("folder")
        val saved = getSharedPreferences("minicode", MODE_PRIVATE).getString("folder", null)
        when {
            path != null -> usePath(File(path))
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
                .putString("folder", uri.toString()).apply()
        }
        val tree = DocumentFile.fromTreeUri(this, uri) ?: return
        folder = tree
        list(tree)
        showList(true)
    }

    private fun usePath(dir: File) {
        val tree = DocumentFile.fromFile(dir)
        folder = tree
        list(tree)
        showList(true)
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
        // Markdown opens rendered, as it does in the other ports.
        previewing = isMarkdown(file.name)
        showList(false)
        updateTitle()
        rehighlight()
        renderPreview()
    }

    private fun isMarkdown(name: String?): Boolean {
        val lower = name?.lowercase() ?: return false
        return lower.endsWith(".md") || lower.endsWith(".markdown")
    }

    /** Shift+Cmd+P on the Mac; the leader's P here. */
    private fun togglePreview() {
        if (!isMarkdown(currentFile?.name)) return
        previewing = !previewing
        renderPreview()
        updateTitle()
    }

    private fun renderPreview() {
        val showPreview = previewing && isMarkdown(currentFile?.name) &&
                ui.fileList.visibility != View.VISIBLE && !terminalShowing
        ui.previewScroll.visibility = if (showPreview) View.VISIBLE else View.GONE
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

    /**
     * Colors the whole file from the core's tokens. This is the full pass the
     * other ports run when a file opens; the incremental path (only the lines
     * an edit can change) is the next thing to bring over.
     */
    private fun rehighlight() {
        val name = currentFile?.name ?: return
        val editable = ui.editor.text ?: return
        val spans = editable.getSpans(0, editable.length, ForegroundColorSpan::class.java)
        for (span in spans) editable.removeSpan(span)
        if (!Core.supports(name)) return
        val tokens = Core.highlight(editable.toString(), name)
        highlighting = true
        var i = 0
        while (i + 2 < tokens.size) {
            val start = tokens[i]
            val end = start + tokens[i + 1]
            val style = tokens[i + 2]
            if (end <= editable.length && style in Palette.styles.indices) {
                editable.setSpan(ForegroundColorSpan(Palette.styles[style]), start, end,
                    Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
            i += 3
        }
        highlighting = false
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
        return true
    }

    private fun showList(show: Boolean) {
        if (show) { terminalShowing = false; browserShowing = false }
        ui.browser.visibility = View.GONE
        ui.fileList.visibility = if (show) View.VISIBLE else View.GONE
        ui.terminal.visibility = View.GONE
        val preview = !show && previewing && isMarkdown(currentFile?.name)
        val media = !show && showingMedia
        ui.previewScroll.visibility = if (preview) View.VISIBLE else View.GONE
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
        'b' to { showList(ui.fileList.visibility != View.VISIBLE) },
        'p' to { togglePreview() },
        't' to { toggleTerminal() },
        'w' to { toggleBrowser() },
        'o' to { pickFolder.launch(null) },
        'h' to { showShortcuts() },
        // A keyboard with no Ctrl or Escape still has to drive a shell.
        'c' to { ui.terminal.sendControl('c') },
        'd' to { ui.terminal.sendControl('d') },
        'e' to { ui.terminal.sendEscape() },
        'i' to { ui.terminal.sendTab() },
    )

    private fun handleShortcut(event: KeyEvent): Boolean {
        val swap = { showList(ui.fileList.visibility != View.VISIBLE) }
        val letters = leaderActions()
        val actions = mapOf(
            KeyEvent.KEYCODE_S to letters.getValue('s'),
            KeyEvent.KEYCODE_B to letters.getValue('b'),
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
                            "Shortcuts")
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> save()
                    1 -> showList(ui.fileList.visibility != View.VISIBLE)
                    2 -> togglePreview()
                    3 -> toggleTerminal()
                    4 -> toggleBrowser()
                    5 -> pickFolder.launch(null)
                    6 -> chooseTextSize()
                    7 -> showShortcuts()
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
            ui.media.visibility = View.GONE
            ui.browser.visibility = View.GONE
            browserShowing = false
            ui.terminal.visibility = View.VISIBLE
            ui.terminal.onExit = {
                if (terminalShowing) toggleTerminal()
                ui.terminal.stop()
            }
            ui.terminal.post { ui.terminal.start(filesDir.absolutePath) }
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

    /** The equivalent of the Mac app's Shift+Cmd+H panel, for a phone. */
    private fun showShortcuts() {
        val lines = listOf(
            "The key left of right Shift, then:",
            "S      Save",
            "B      Files or editor",
            "P      Markdown preview",
            "T      Terminal",
            "W      Browser",
            "O      Open a folder",
            "H      This list",
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
class FileListAdapter(private val onClick: (DocumentFile) -> Unit) :
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
    }

    override fun getItemCount() = entries.size
}
