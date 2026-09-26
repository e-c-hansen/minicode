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
        symbolRow = getSharedPreferences("minicode", MODE_PRIVATE).getBoolean("symbolRow", true)

        // A double tap in the LaTeX preview edits the source behind it; the
        // splice goes through the buffer, so it is highlighted, marked
        // unsaved and typeset again like any other edit.
        ui.latex.onEdit = { start, end, text -> ui.editor.text?.replace(start, end, text) }

        onBackPressedDispatcher.addCallback(this, back)
        TerminalKeys.fill(ui.termKeyRow, ui.terminal)
        ui.fileList.layoutManager = LinearLayoutManager(this)
        ui.fileList.adapter = files
        // The list takes the keyboard when the editor goes, and without this
        // Android paints its whole area grey to show that it has it.
        ui.fileList.defaultFocusHighlightEnabled = false
        ui.preview.defaultFocusHighlightEnabled = false
        ui.previewScroll.defaultFocusHighlightEnabled = false
        ui.up.setOnClickListener { goUp() }
        ui.menu.setOnClickListener { showMenu() }
        // Touch targets only. Focusable, ⋮ took the keyboard whenever a pane
        // was swapped under it, and the next Space opened the menu.
        ui.up.isFocusable = false
        ui.menu.isFocusable = false
        ui.keyboard.isFocusable = false
        ui.keyboard.setOnClickListener { toggleSoftKeyboard() }
        EditorKeys.fill(ui.editKeyRow, ui.editor)

        // Tapping a file reference or URL in the terminal opens it. Relative
        // paths are tried in the shell's folder, then the open one, then the
        // shell's home, which is last because `~/` means it.
        ui.terminal.linkDirs = {
            listOfNotNull(shellFolder, folderPath()?.path, filesDir.absolutePath).distinct()
        }
        ui.terminal.onLink = ::openTerminalLink

        // Source control takes the file list's place (leader V); a diff or a
        // commit it shows takes the editor's.
        ui.gitPanel.folder = ::gitFolder
        ui.gitPanel.onShow = ::showDiff

        ui.editor.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {
                if (!highlighting && s != null) highlighter.edited(s, a, b, c)
                if (!highlighting && s != null) lsp.edited(s, a, b, c)
            }
            override fun afterTextChanged(s: Editable?) {
                // No file, no buffer to mark: the editor is never shown then.
                if (highlighting || currentFile == null) return
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
        // `--ez start true` shows the start screen without forgetting the
        // saved folder, so it can be looked at on a phone already set up.
        val path = intent?.getStringExtra("folder")
        val startScreen = intent?.getBooleanExtra("start", false) == true
        val prefs = getSharedPreferences("minicode", MODE_PRIVATE)
        val saved = prefs.getString("folder", null)
        val savedPath = prefs.getString("folderPath", null)
        // The recent list began after folders were already being saved, so
        // it starts from the one that is.
        if (!prefs.contains("recent")) {
            val seed = savedPath?.let { "path:$it" } ?: saved?.let { "uri:$it" }
            prefs.edit().putString("recent", seed.orEmpty()).apply()
        }
        when {
            path != null -> usePath(File(path))
            startScreen -> showList(true)
            // Without "All files access" the folder's list says so and
            // offers the setting, rather than coming up empty.
            savedPath != null -> usePath(File(savedPath))
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
            addRecent("uri:$uri")
        }
        val tree = DocumentFile.fromTreeUri(this, uri) ?: return
        folder = tree
        followFolder()
        list(tree)
        sidebarIsGit = false
        ui.gitPanel.folderChanged()
        showList(true)
    }

    private fun usePath(dir: File, remember: Boolean = false) {
        if (remember) {
            getSharedPreferences("minicode", MODE_PRIVATE).edit()
                .putString("folderPath", dir.path).remove("folder").apply()
            addRecent("path:${dir.path}")
        }
        val tree = DocumentFile.fromFile(dir)
        folder = tree
        followFolder()
        list(tree)
        sidebarIsGit = false
        ui.gitPanel.folderChanged()
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
    private fun folderPath(): File? = pathOf(current ?: folder)

    /** A folder's path, when it has one; see folderPath. */
    private fun pathOf(tree: DocumentFile?): File? {
        tree ?: return null
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

    /**
     * The project for the source control panel, as a path Termux can use,
     * or null with the reason. Git runs in Termux, which sees shared storage
     * and nothing else, so a folder from the picker counts when it is on the
     * phone's storage, whether or not MiniCode itself may read paths there.
     */
    private fun gitFolder(): Pair<File?, String?> {
        if (folder == null) return null to "Open a folder first: leader O, Phone storage."
        val path = pathOf(folder)
        if (path == null || !Termux.isShared(path)) return null to GitPanel.NOT_SHARED
        return path to null
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
        // The recent folders follow, apart from the one already open.
        val open = folder?.let { recentKeyOf(it) }
        val recent = liveRecents().filter { it != open }
        val items = listOf("Phone storage (works in the terminal)",
                           "Another app or cloud (Drive, Termux, ...)") +
                recent.map { val (name, where) = describeRecent(it); "$name   ($where)" }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Open a folder")
            .setItems(items.toTypedArray()) { _, which ->
                when (which) {
                    0 -> openPhoneStorage()
                    1 -> pickFolder.launch(null)
                    else -> openRecent(recent[which - 2])
                }
            }
            .show()
    }

    /** The top of the phone's storage, browsed by path. */
    private fun openPhoneStorage() {
        if (!canReachPaths()) {
            wantsStorage = true
            askForPathAccess("Phone storage",
                "MiniCode browses the phone's storage by path, which Android " +
                "allows only with \"All files access\". The same permission lets " +
                "the terminal work in the folder you open. Open the setting?")
            return
        }
        confirmLeave { usePath(android.os.Environment.getExternalStorageDirectory(), remember = true) }
    }

    /** Set while the user is in Settings granting access to open storage. */
    private var wantsStorage = false

    // ------------------------------------------------------ recent folders

    /**
     * The last few folders opened, newest first, as "path:<path>" for one on
     * the phone's storage and "uri:<uri>" for one from the picker. Stored
     * newline-separated in the "recent" preference.
     */
    private fun recents(): List<String> =
        getSharedPreferences("minicode", MODE_PRIVATE).getString("recent", null)
            ?.split('\n')?.filter { it.isNotEmpty() } ?: emptyList()

    private fun saveRecents(list: List<String>) {
        getSharedPreferences("minicode", MODE_PRIVATE).edit()
            .putString("recent", list.joinToString("\n")).apply()
    }

    private fun addRecent(key: String) =
        saveRecents((listOf(key) + recents().filter { it != key }).take(MAX_RECENT))

    private fun recentKeyOf(dir: DocumentFile): String =
        if (dir.uri.scheme == "file") "path:${dir.uri.path}" else "uri:${dir.uri}"

    /**
     * The recent folders that can still be opened. One that is gone, or whose
     * permission was taken back, is dropped for good. A path is kept while
     * the app lacks "All files access", since it cannot tell then, and
     * opening it asks for the access.
     */
    private fun liveRecents(): List<String> {
        val granted = contentResolver.persistedUriPermissions
            .filter { it.isReadPermission }.map { it.uri.toString() }.toSet()
        val all = recents()
        val live = all.filter { key ->
            when {
                key.startsWith("path:") ->
                    !canReachPaths() || File(key.removePrefix("path:")).isDirectory
                key.startsWith("uri:") -> key.removePrefix("uri:") in granted && try {
                    DocumentFile.fromTreeUri(this, Uri.parse(key.removePrefix("uri:")))?.exists() == true
                } catch (e: Exception) { false }
                else -> false
            }
        }
        if (live.size != all.size) saveRecents(live)
        return live
    }

    /** A recent folder's name, and where it is. */
    private fun describeRecent(key: String): Pair<String, String> {
        if (key.startsWith("path:")) {
            val dir = File(key.removePrefix("path:"))
            val whole = describePath(dir)
            if (whole == PHONE_STORAGE) return PHONE_STORAGE to "All of it, from the top"
            return dir.name to whole
        }
        val uri = Uri.parse(key.removePrefix("uri:"))
        val name = try { DocumentFile.fromTreeUri(this, uri)?.name } catch (e: Exception) { null }
            ?: uri.lastPathSegment?.substringAfterLast(':')?.substringAfterLast('/') ?: "Folder"
        return name to providerName(uri.authority)
    }

    private fun openRecent(key: String) {
        if (key.startsWith("path:")) {
            if (!canReachPaths()) {
                askForPathAccess("Phone storage",
                    "Opening a folder on the phone's storage needs \"All files " +
                    "access\" for MiniCode. Open the setting?")
                return
            }
            confirmLeave { usePath(File(key.removePrefix("path:")), remember = true) }
        } else {
            confirmLeave { useFolder(Uri.parse(key.removePrefix("uri:")), remember = true) }
        }
    }

    // --------------------------------------------------- where things are

    /**
     * A path as the phone's user would say it: "Phone storage / mc-test /
     * docs" for shared storage, "MiniCode's own folder" for the app's
     * private one, and the path itself for anything else.
     */
    private fun describePath(f: File): String {
        val storage = android.os.Environment.getExternalStorageDirectory().path
        val own = filesDir.parentFile?.path ?: filesDir.path
        val p = f.path.trimEnd('/')
        fun under(root: String, label: String) =
            if (p == root) label else "$label / " + p.removePrefix("$root/").replace("/", " / ")
        return when {
            p == storage || p.startsWith("$storage/") -> under(storage, PHONE_STORAGE)
            p == "/sdcard" || p.startsWith("/sdcard/") -> under("/sdcard", PHONE_STORAGE)
            p == own || p.startsWith("$own/") -> under(own, "MiniCode's own folder")
            else -> p
        }
    }

    /** The app a picked folder comes from, by its provider's authority. */
    private fun providerName(authority: String?): String {
        authority ?: return "Another app"
        return when {
            authority.startsWith("com.termux") -> "Termux"
            authority.startsWith("com.google.android.apps.docs") -> "Google Drive"
            authority == "com.android.externalstorage.documents" -> PHONE_STORAGE
            else -> try {
                packageManager.resolveContentProvider(authority, 0)
                    ?.loadLabel(packageManager)?.toString()
            } catch (e: Exception) { null } ?: authority
        }
    }

    /**
     * Where a folder is: its path spelled as describePath does when it has
     * one, otherwise the app it comes from and the folders from the open
     * one down.
     */
    private fun where(dir: DocumentFile?): String? {
        dir ?: return null
        pathOf(dir)?.let { return describePath(it) }
        val names = mutableListOf<String>()
        var d: DocumentFile? = dir
        while (d != null) {
            names.add(0, d.name ?: "?")
            if (d.uri == folder?.uri) break
            d = d.parentFile
        }
        return (listOf(providerName(dir.uri.authority)) + names).joinToString(" / ")
    }

    /** The folder a file is in, as where() says it. */
    private fun whereFileIs(file: DocumentFile): String? =
        pathOf(file)?.parentFile?.let(::describePath)
            ?: where(file.parentFile) ?: providerName(file.uri.authority)

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
            shellFolder = cwd; updateTitle()
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
        // Back from Settings with "All files access": finish what asked for it.
        if (canReachPaths()) {
            if (wantsStorage) {
                wantsStorage = false
                confirmLeave { usePath(android.os.Environment.getExternalStorageDirectory(), remember = true) }
            } else if (lostAccess) {
                current?.let(::list)
            }
        }
        if (ui.start.visibility == View.VISIBLE) fillStart()
        // Back from Termux, where a commit may have been made.
        ui.gitPanel.refresh()
    }

    private fun askForPathAccess(
        title: String = "Terminal in this folder",
        message: String = "A shell reaches files by path, and Android allows that " +
                "outside the app only with \"All files access\". The editor " +
                "does not need it. Open the setting?",
    ) {
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
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
        val entries = readFolder(dir)?.sortedWith(
            compareBy({ !it.isDirectory }, { it.name?.lowercase() ?: "" }))
        lostAccess = entries == null
        files.submit(when {
            entries == null -> lostRows(dir)
            entries.isEmpty() -> listOf(
                ListRow.Note("This folder is empty."),
                ListRow.Action("New file") { newFile() })
            else -> entries.map { ListRow.Doc(it) }
        })
        ui.up.visibility = if (dir.uri == folder?.uri) View.GONE else View.VISIBLE
        updateTitle()
        if (entries.isNullOrEmpty() && ui.fileList.visibility == View.VISIBLE) focusFileList()
    }

    /**
     * A folder's entries, or null when the app cannot read it. A path with
     * no "All files access" lists as empty rather than failing, and so
     * does a folder that is gone, so both are checked first.
     */
    private fun readFolder(dir: DocumentFile): List<DocumentFile>? {
        if (dir.uri.scheme == "file") {
            val f = File(dir.uri.path ?: return null)
            if (!canReachPaths() && f.path != filesDir.path &&
                !f.path.startsWith(filesDir.parentFile?.path ?: filesDir.path)) return null
            if (!f.isDirectory) return null
        }
        return try {
            if (!dir.canRead()) null else dir.listFiles().toList()
        } catch (e: Exception) {
            null
        }
    }

    /** Set while the file list is saying it cannot read the folder. */
    private var lostAccess = false

    /** What to say, and offer, for a folder the app cannot read. */
    private fun lostRows(dir: DocumentFile): List<ListRow> {
        val name = (try { dir.name } catch (e: Exception) { null })
            ?: dir.uri.lastPathSegment?.substringAfterLast(':')?.substringAfterLast('/')
            ?: "This folder"
        val other = ListRow.Action("Open a different folder") { openFolder() }
        if (dir.uri.scheme == "file") {
            if (!canReachPaths()) return listOf(
                ListRow.Note("MiniCode cannot list $name without \"All files access\", " +
                             "which Android grants in Settings."),
                ListRow.Action("Allow access in Settings") {
                    askForPathAccess("Phone storage",
                        "Listing $name needs \"All files access\" for MiniCode. " +
                        "Open the setting?")
                },
                other)
            return listOf(
                ListRow.Note("$name is not there any more. It may have been moved or deleted."),
                other)
        }
        return listOf(
            ListRow.Note("MiniCode can no longer open $name. Its access was removed, " +
                         "or the app it comes from is gone."),
            ListRow.Action("Open it again") { pickFolder.launch(dir.uri) },
            other)
    }

    private fun focusFileList() {
        val at = files.firstAction()
        if (at < 0) return
        ui.fileList.post {
            ui.fileList.scrollToPosition(at)
            ui.fileList.post { ui.fileList.findViewHolderForAdapterPosition(at)?.itemView?.requestFocus() }
        }
    }

    /**
     * Makes a file in the folder being listed and opens it. The name is
     * asked for first, so a new file always has a place and a name before
     * anything is typed into it.
     */
    private fun newFile() {
        val dir = current ?: folder
        if (dir == null) { say("Open a folder first."); return }
        val field = android.widget.EditText(this).apply {
            hint = "name.txt"
            isSingleLine = true
            inputType = android.text.InputType.TYPE_CLASS_TEXT or
                    android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
            setTextColor(Palette.TEXT)
        }
        val box = android.widget.FrameLayout(this).apply {
            val pad = (20 * resources.displayMetrics.density).toInt()
            setPadding(pad, pad / 2, pad, 0)
            addView(field)
        }
        val dialog = androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("New file in ${where(dir) ?: dir.name}")
            .setView(box)
            .setPositiveButton("Create") { _, _ -> createFile(dir, field.text.toString().trim()) }
            .setNegativeButton("Cancel", null)
            .create()
        field.setOnEditorActionListener { _, _, _ ->
            dialog.dismiss()
            createFile(dir, field.text.toString().trim())
            true
        }
        dialog.show()
        field.requestFocus()
    }

    private fun createFile(dir: DocumentFile, name: String) {
        if (name.isEmpty() || name == "." || name == ".." || name.contains('/')) {
            say("A file name cannot be empty or contain /.")
            return
        }
        if (dir.findFile(name) != null) { say("$name already exists here."); return }
        val made: DocumentFile? = try {
            val path = dir.uri.takeIf { it.scheme == "file" }?.path
            if (path != null) File(path, name).takeIf { it.createNewFile() }?.let(DocumentFile::fromFile)
            // octet-stream, so the provider keeps the name as typed rather
            // than adding an extension of its own.
            else dir.createFile("application/octet-stream", name)
        } catch (e: Exception) {
            null
        }
        if (made == null) { say("Could not create $name here."); return }
        list(dir)
        confirmLeave { openFile(made, "") }
    }

    private fun say(text: String) =
        android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show()

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
        showingDiff = false
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
        showingDiff = false
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
                !sidebarShowing() && !terminalShowing
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
            if (!terminalShowing && !sidebarShowing() && !showingDiff && currentFile != null) {
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
        ui.gitPanel.shutdown()
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
        val file = currentFile ?: run {
            if (!showingDiff) say("Nothing to save: no file is open.")
            return true
        }
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
        ui.gitPanel.refresh()
        return true
    }

    /**
     * The list side (the start screen, the file list or source control) or
     * the editor side (the file, its preview, a picture, a diff).
     *
     * There is no editor without a file: with nothing open, asking for the
     * editor side shows the list side instead, so nothing can be typed into
     * a buffer that has no file to be saved to.
     */
    private fun showList(wanted: Boolean) {
        val show = wanted || !hasEditorContent()
        if (show) { terminalShowing = false; browserShowing = false }
        val start = show && folder == null
        if (start) fillStart()
        ui.start.visibility = if (start) View.VISIBLE else View.GONE
        ui.browser.visibility = View.GONE
        ui.fileList.visibility = if (show && !start && !sidebarIsGit) View.VISIBLE else View.GONE
        ui.gitPanel.visibility = if (show && !start && sidebarIsGit) View.VISIBLE else View.GONE
        setGitActive(show && !start && sidebarIsGit)
        ui.terminal.visibility = View.GONE; ui.termKeys.visibility = View.GONE
        val diff = !show && showingDiff
        ui.diffView.visibility = if (diff) View.VISIBLE else View.GONE
        val preview = !diff && !show && previewing && isPreviewable(currentFile?.name)
        val media = !diff && !show && showingMedia
        val pane = previewPane(currentFile?.name)
        ui.previewScroll.visibility =
            if (preview && pane == ui.previewScroll) View.VISIBLE else View.GONE
        ui.latex.visibility = if (preview && pane == ui.latex) View.VISIBLE else View.GONE
        ui.media.visibility = if (media) View.VISIBLE else View.GONE
        ui.editor.visibility =
            if (show || preview || media || diff) View.GONE else View.VISIBLE
        ui.up.visibility =
            if (show && !start && !sidebarIsGit && current?.uri != folder?.uri) View.VISIBLE else View.GONE
        if (!show) (if (diff) ui.diffView else ui.editor).let { v -> v.requestFocus(); v.post { v.requestFocus() } }
        else if (start) ui.start.post { ui.start.focusFirst() }
        else if (sidebarIsGit) ui.gitPanel.focusPanel()
        else if (lostAccess || files.firstAction() >= 0) focusFileList()
        updateTitle()
    }

    /** A file, picture or diff is open, so the editor side has something. */
    private fun hasEditorContent() = currentFile != null || showingDiff

    /**
     * The list side and the editor side, for the leader pressed alone and
     * for leader F. With no file open there is nothing to switch to, and
     * the title's second line already says so; a short note says it too.
     */
    private fun switchPanes() {
        if (sidebarShowing() && !hasEditorContent()) {
            say(if (folder == null) "No folder is open yet." else "No file is open. Pick one from the list.")
            return
        }
        showList(!sidebarShowing())
    }

    /** The start screen: ways to open a folder, recent ones, and the rest. */
    private fun fillStart() {
        val items = mutableListOf(
            StartScreen.Item("Open a folder on phone storage",
                "Browse the phone's storage. The terminal can work there too.") {
                openPhoneStorage()
            },
            StartScreen.Item("Open from another app or cloud",
                "Google Drive, Termux and others, through Android's picker") {
                pickFolder.launch(null)
            })
        val recent = liveRecents()
        if (recent.isNotEmpty()) {
            items += StartScreen.Item("Recent folders", heading = true)
            for (key in recent) {
                val (name, place) = describeRecent(key)
                items += StartScreen.Item(name, place) { openRecent(key) }
            }
        }
        items += StartScreen.Item("Also", heading = true)
        items += StartScreen.Item("Terminal", "A shell, in MiniCode's own folder until one is open") {
            toggleTerminal()
        }
        items += StartScreen.Item("Shortcuts", "What the leader key and the ⋮ menu do") {
            showShortcuts()
        }
        ui.start.show("No folder is open. Open one and its files are listed here; " +
                      "tap a file to edit it.", items)
    }

    /** The start screen, the file list or source control, whichever is up. */
    private fun sidebarShowing() =
        ui.fileList.visibility == View.VISIBLE || ui.gitPanel.visibility == View.VISIBLE ||
                ui.start.visibility == View.VISIBLE

    /** Which of the two the sidebar shows; leader V switches. */
    private var sidebarIsGit = false
    private var gitActive = false

    private fun setGitActive(on: Boolean) {
        if (on == gitActive) return
        gitActive = on
        ui.gitPanel.setActive(on)
    }

    /**
     * Leader V (Ctrl+Shift+G on a USB keyboard, as on the Mac and Linux):
     * the source control panel in the file list's place, and the file list
     * back when it is already up.
     */
    private fun toggleSourceControl() {
        sidebarIsGit = ui.gitPanel.visibility != View.VISIBLE
        showList(true)
    }

    /**
     * A diff or a commit from the source control panel, in the editor's
     * place, as on the Mac: whatever was open is closed first (asking about
     * unsaved changes), and nothing here can be saved. Opening any file
     * puts the editor back.
     */
    private fun showDiff(title: String, text: List<CharSequence>) {
        confirmLeave {
            currentFile = null
            dirty = false
            showingMedia = false
            ui.media.setImageDrawable(null)
            previewing = false
            ui.latex.close()
            highlighting = true
            ui.editor.setText("")
            highlighting = false
            lsp.opened(null, folder)
            showingDiff = true
            diffTitle = title
            ui.diffView.show(text)
            showList(false)
        }
    }

    private var showingDiff = false
    private var diffTitle = ""

    /**
     * The title bar: the pane's name on the first line (the folder while
     * listing, the file while editing, with a dot while it is unsaved), and
     * where it is on the second, so the screen always says what it shows.
     */
    private fun updateTitle() {
        val waiting = if (leaderArmed) "  …" else ""
        val showingList = ui.fileList.visibility == View.VISIBLE
        val (title, place) = when {
            terminalShowing -> "Terminal" to
                    (shellFolder?.let { describePath(File(it)) } ?: "MiniCode's own folder")
            browserShowing -> "Browser" to null
            ui.start.visibility == View.VISIBLE -> "MiniCode" to "No folder open"
            ui.gitPanel.visibility == View.VISIBLE -> "Source Control" to where(folder)
            showingList -> (current?.name ?: folder?.name ?: "MiniCode") to
                    (if (lostAccess) "Cannot open this folder" else where(current ?: folder))
            showingDiff -> diffTitle to "From source control, read only"
            else -> {
                val file = currentFile
                val extra = when {
                    !showingMedia -> ""
                    pdfPages > 0 -> "  ${pdfPages} page" + (if (pdfPages == 1) "" else "s")
                    else -> "  $mediaSize"
                }
                val mode = when {
                    file == null -> null
                    showingMedia -> "View only"
                    previewing && isPreviewable(file.name) -> "Preview"
                    isPreviewable(file.name) -> "Source"
                    else -> null
                }
                val state = if (dirty) "Unsaved" else null
                ((if (dirty) "● " else "") + (file?.name ?: "MiniCode") + extra) to
                        listOfNotNull(state, mode, file?.let(::whereFileIs)).joinToString(" · ")
            }
        }
        ui.title.text = title + waiting
        ui.subtitle.text = place.orEmpty()
        ui.subtitle.visibility = if (place.isNullOrEmpty()) View.GONE else View.VISIBLE
        // Every pane change ends here, so the editor's symbol row follows
        // the editor from here too.
        val editing = ui.editor.visibility == View.VISIBLE && currentFile != null && symbolRow
        ui.editKeys.visibility = if (editing) View.VISIBLE else View.GONE
    }

    /** Whether the symbol row under the editor is wanted (⋮ menu). */
    private var symbolRow = true

    private fun toggleSymbolRow() {
        symbolRow = !symbolRow
        getSharedPreferences("minicode", MODE_PRIVATE).edit()
            .putBoolean("symbolRow", symbolRow).apply()
        updateTitle()
    }

    /**
     * Whether a whole on-screen keyboard is up. With a hardware keyboard
     * attached, Android still "shows" the input method as a strip a few
     * dozen pixels tall (a hide arrow and a keyboard picker), and the
     * insets call that visible; a keyboard worth the name is far taller.
     */
    private fun imeShowing(): Boolean {
        val insets = androidx.core.view.ViewCompat.getRootWindowInsets(ui.root) ?: return false
        val type = androidx.core.view.WindowInsetsCompat.Type.ime()
        val nav = androidx.core.view.WindowInsetsCompat.Type.navigationBars()
        val height = insets.getInsets(type).bottom - insets.getInsets(nav).bottom
        return insets.isVisible(type) && height > 120 * resources.displayMetrics.density
    }

    /** A hardware keyboard is attached and in use. */
    private fun hardKeyboard(): Boolean {
        val c = resources.configuration
        return c.keyboard != android.content.res.Configuration.KEYBOARD_NOKEYS &&
                c.hardKeyboardHidden == android.content.res.Configuration.HARDKEYBOARDHIDDEN_NO
    }

    /**
     * Android's "Use on-screen keyboard" switch (show_ime_with_hard_keyboard):
     * 1 on, 0 off, -1 when the app may not read it.
     */
    private fun showImeWithHardKeyboard(): Int = try {
        android.provider.Settings.Secure.getInt(contentResolver, "show_ime_with_hard_keyboard", -1)
    } catch (e: Exception) { -1 }

    /**
     * The on-screen keyboard, on or off (the ⌨ button, leader Y). The phone's
     * own keyboard has no #, braces or backtick, and while it is attached
     * Android keeps the on-screen one hidden. No app can override that:
     * asked to show, the keyboard app draws only its thin strip. The switch
     * that allows it, "Use on-screen keyboard", is at the top of Android's
     * keyboard picker, so with the switch off this opens the picker (on a
     * Titan 2, turning the switch on brings the keyboard up at once). With
     * it on, the button shows and hides the keyboard directly.
     */
    private fun toggleSoftKeyboard() {
        val imm = getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager
        if (imeShowing()) {
            imm.hideSoftInputFromWindow(ui.root.windowToken, 0)
            return
        }
        val target: View = when {
            terminalShowing -> ui.terminal
            browserShowing -> currentFocus ?: ui.url
            ui.editor.visibility == View.VISIBLE -> ui.editor
            else -> currentFocus ?: run {
                say("Open a file to type into it.")
                return
            }
        }
        target.requestFocus()
        if (hardKeyboard() && showImeWithHardKeyboard() == 0) {
            pickKeyboard(imm)
            return
        }
        androidx.core.view.WindowCompat.getInsetsController(window, target)
            .show(androidx.core.view.WindowInsetsCompat.Type.ime())
        @Suppress("DEPRECATION")
        imm.showSoftInput(target, android.view.inputmethod.InputMethodManager.SHOW_FORCED)
        // The switch could not be read: if no keyboard came up, the picker
        // is still where it is.
        if (hardKeyboard()) ui.root.postDelayed({ if (!imeShowing()) pickKeyboard(imm) }, 700)
    }

    private fun pickKeyboard(imm: android.view.inputmethod.InputMethodManager) {
        android.widget.Toast.makeText(this, "Turn on \"Use on-screen keyboard\"",
                                      android.widget.Toast.LENGTH_LONG).show()
        keyboardAsked = true
        imm.showInputMethodPicker()
    }

    /** Set while Android's keyboard picker is up on the ⌨ button's behalf. */
    private var keyboardAsked = false

    /**
     * Back from the picker: if the switch was turned on, the keyboard comes
     * up, so one press of ⌨ is all it takes.
     */
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || !keyboardAsked) return
        keyboardAsked = false
        if (showImeWithHardKeyboard() == 1) ui.root.postDelayed({ toggleSoftKeyboard() }, 200)
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

    /** Leader F: the file list, or the editor when the list is up. */
    private fun showFiles() {
        if (ui.fileList.visibility == View.VISIBLE || ui.start.visibility == View.VISIBLE) {
            switchPanes(); return
        }
        sidebarIsGit = false
        showList(true)
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
            switchPanes()
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
        // The leader then Enter commits, from anywhere in the source control
        // panel: the phone has no Ctrl for Ctrl+Enter.
        if (letter == '\n' || letter == '\r') {
            if (ui.gitPanel.visibility != View.VISIBLE) return false
            ui.gitPanel.commit()
            return true
        }
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
        'f' to { showFiles() },
        // V for version control: G was already go-to-definition.
        'v' to { toggleSourceControl() },
        'p' to { togglePreview() },
        't' to { toggleTerminal() },
        // B is the browser, as Shift+Cmd+B is on the Mac.
        'b' to { toggleBrowser() },
        'o' to { openFolder() },
        'h' to { showShortcuts() },
        // Y: the on-screen keyboard, for the symbols the phone's lacks.
        'y' to { toggleSoftKeyboard() },
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
        // Ctrl+Shift+G is source control on the Mac and Linux.
        if (event.isCtrlPressed && event.isShiftPressed && event.keyCode == KeyEvent.KEYCODE_G) {
            handled.add(event.keyCode)
            toggleSourceControl()
            return true
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
                            "Shortcuts", "Termux tools", "Source control",
                            "New file", "On-screen keyboard",
                            if (symbolRow) "Hide the symbol row" else "Show the symbol row")
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> save()
                    1 -> showFiles()
                    2 -> togglePreview()
                    3 -> toggleTerminal()
                    4 -> toggleBrowser()
                    5 -> openFolder()
                    6 -> chooseTextSize()
                    7 -> showShortcuts()
                    8 -> termuxSetup()
                    9 -> toggleSourceControl()
                    10 -> newFile()
                    11 -> toggleSoftKeyboard()
                    12 -> toggleSymbolRow()
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
            ui.terminal.visibility = View.GONE; ui.termKeys.visibility = View.GONE
            ui.fileList.visibility = View.GONE
            ui.start.visibility = View.GONE
            ui.gitPanel.visibility = View.GONE
            setGitActive(false)
            ui.diffView.visibility = View.GONE
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
            showList(currentFile == null && !showingDiff)
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

    /**
     * Back walks the web history first, then closes whatever pane is up.
     *
     * Through the dispatcher rather than onBackPressed: on Android 16 an app
     * that targets API 36 never gets onBackPressed (predictive back), so the
     * old override left the app on the first press of Back from any pane.
     * With nothing left to close, the callback steps aside and Back does
     * what it does everywhere else.
     */
    private val back = object : androidx.activity.OnBackPressedCallback(true) {
        override fun handleOnBackPressed() {
            when {
                browserShowing && ui.web.canGoBack() -> ui.web.goBack()
                browserShowing -> toggleBrowser()
                terminalShowing -> toggleTerminal()
                !sidebarShowing() -> showList(true)
                // Back in a subfolder of the list goes up one, as ↑ does.
                ui.fileList.visibility == View.VISIBLE && current != null &&
                        current?.uri != folder?.uri -> goUp()
                // Back from source control is the file list, as V is.
                ui.gitPanel.visibility == View.VISIBLE -> toggleSourceControl()
                else -> {
                    isEnabled = false
                    onBackPressedDispatcher.onBackPressed()
                    isEnabled = true
                }
            }
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
            ui.start.visibility = View.GONE
            ui.gitPanel.visibility = View.GONE
            setGitActive(false)
            ui.diffView.visibility = View.GONE
            ui.editor.visibility = View.GONE
            ui.previewScroll.visibility = View.GONE
            ui.latex.visibility = View.GONE
            ui.media.visibility = View.GONE
            ui.browser.visibility = View.GONE
            browserShowing = false
            ui.terminal.visibility = View.VISIBLE; ui.termKeys.visibility = View.VISIBLE
            ui.terminal.onExit = {
                if (terminalShowing) toggleTerminal()
                ui.terminal.stop()
            }
            if (ui.terminal.isRunning) followFolder()
            if (!ui.terminal.isRunning) {
                val (cwd, why) = terminalDirectory()
                ui.terminal.post {
                    ui.terminal.start(filesDir.absolutePath, cwd ?: filesDir.absolutePath)
                    shellFolder = cwd; updateTitle()
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
            ui.terminal.visibility = View.GONE; ui.termKeys.visibility = View.GONE
            showList(currentFile == null && !showingDiff)
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
                                           "typescript-language-server", "tectonic",
                                           "git")
                        val (_, out) = Termux.run(this, tools.joinToString("; ") {
                            "printf '%s ' $it; command -v $it || echo -"
                        })
                        "Termux runs commands for MiniCode.\n\n" + out.trim() +
                                "\n\nInstall what is missing with pkg, for example " +
                                "pkg install git clang tectonic python."
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
            "Y      On-screen keyboard, for",
            "       symbols (or the ⌨ button)",
            "V      Source control, in place of",
            "       the files (V again: files)",
            "N      Complete (language server)",
            "K      What the symbol is",
            "G      Go to its definition",
            "",
            "In the terminal: C for Ctrl C,",
            "D for Ctrl D, E for Escape,",
            "I for Tab.",
            "",
            "In source control: Space stages or",
            "unstages, Enter shows the diff, the",
            "key then Enter commits.",
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

        /** How many recent folders the start screen and leader O offer. */
        private const val MAX_RECENT = 6

        /** What the shared storage is called on screen, as Android's Files app says. */
        private const val PHONE_STORAGE = "Phone storage"
    }
}

/**
 * One row of the file list: a file or folder, or, when there is nothing to
 * list, a line saying why and the rows that do something about it (an empty
 * folder offers New file, a folder the app lost access to offers to reopen).
 */
sealed class ListRow {
    class Doc(val file: DocumentFile) : ListRow()
    class Note(val text: String) : ListRow()
    class Action(val label: String, val run: () -> Unit) : ListRow()
}

/** The file list: one row per entry, folders marked by a trailing slash. */
class FileListAdapter(private val onClick: (DocumentFile) -> Unit,
                      private val onLongClick: (DocumentFile) -> Unit = {}) :
    RecyclerView.Adapter<FileListAdapter.Row>() {

    private var entries: List<ListRow> = emptyList()

    fun submit(list: List<ListRow>) {
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
            layoutParams = RecyclerView.LayoutParams(
                RecyclerView.LayoutParams.MATCH_PARENT, RecyclerView.LayoutParams.WRAP_CONTENT)
        }
        return Row(text)
    }

    override fun onBindViewHolder(row: Row, position: Int) {
        val t = row.text
        val dp = t.resources.displayMetrics.density
        t.setOnClickListener(null)
        t.setOnLongClickListener(null)
        t.background = null
        t.isFocusable = false
        t.minHeight = 0
        t.gravity = android.view.Gravity.CENTER_VERTICAL
        when (val entry = entries[position]) {
            is ListRow.Doc -> {
                val f = entry.file
                t.text = (f.name ?: "?") + if (f.isDirectory) "/" else ""
                t.setTextColor(if (f.isDirectory) Palette.ACCENT else Palette.TEXT)
                t.setOnClickListener { onClick(f) }
                t.setOnLongClickListener {
                    if (f.isDirectory) onLongClick(f)
                    f.isDirectory
                }
            }
            is ListRow.Note -> {
                t.text = entry.text
                t.setTextColor(Palette.MUTED)
            }
            is ListRow.Action -> {
                t.text = entry.label
                t.setTextColor(Palette.ACCENT)
                t.minHeight = (48 * dp).toInt()
                t.background = StartScreen.rowBackground()
                t.isFocusable = true
                t.setOnClickListener { entry.run() }
            }
        }
    }

    /** The first row that does something, for the keyboard's focus. */
    fun firstAction() = entries.indexOfFirst { it is ListRow.Action }

    override fun getItemCount() = entries.size
}
