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
    private val files = FileListAdapter(::openEntry, ::makeRoot, ::startMove)

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
        // The password variation is what turns off Pastiera's capitals and
        // autocorrect; CodeEditText.codeInputType says so to the keyboard
        // whatever is set here.
        ui.editor.inputType = android.text.InputType.TYPE_CLASS_TEXT or
                android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        ui.editor.typeface = android.graphics.Typeface.MONOSPACE
        ui.editor.privateImeOptions = "nm"   // numeric/no-prediction hint some IMEs honour
        ui.editor.autoIndent = true   // Enter keeps the line's indentation

        setTextSize(getSharedPreferences("minicode", MODE_PRIVATE).getInt("textSize", 13))
        symbolRow = getSharedPreferences("minicode", MODE_PRIVATE).getBoolean("symbolRow", true)
        // The desktop's markdown.web-images, as a preference (⋮ menu); on by default.
        WebImages.enabled = getSharedPreferences("minicode", MODE_PRIVATE).getBoolean("webImages", true)

        // A double tap in the LaTeX preview edits the source behind it; the
        // splice goes through the buffer, so it is highlighted, marked
        // unsaved and typeset again like any other edit.
        ui.latex.onEdit = { start, end, text -> ui.editor.text?.replace(start, end, text) }

        onBackPressedDispatcher.addCallback(this, back)
        TerminalKeys.fill(ui.termKeyRow, ui.terminal)
        // Both key rows sit on the bottom edge, where the screen's corners
        // are rounded; the number is each key's own vertical padding.
        CurvedEdges.keepClear(ui.termKeys, ui.termKeyRow, TerminalKeys.KEY_PADDING_DP)
        ui.fileList.layoutManager = LinearLayoutManager(this)
        ui.fileList.adapter = files
        // The list takes the keyboard when the editor goes, and without this
        // Android paints its whole area grey to show that it has it.
        ui.fileList.defaultFocusHighlightEnabled = false
        ui.previewScroll.defaultFocusHighlightEnabled = false
        // The Markdown preview: a tapped link opens, a double tap edits the
        // block behind it, and pictures are found beside the file.
        ui.previewScroll.onLink = ::openMarkdownLink
        ui.previewScroll.onEditBlock = ::editMarkdownBlock
        ui.previewScroll.resolve = ::resolveRelative
        // A tap on a task's box ticks or clears it, as a preview edit.
        ui.previewScroll.onToggleTask = ::toggleTaskBox
        // The TODO list (leader W) opens a file at the line a row names.
        ui.todos.onOpen = ::openTodo
        ui.up.setOnClickListener { goUp() }
        // A row dragged from the list drops on a folder row, on the listed
        // folder (anywhere else in the list), or on ↑ for the folder above.
        ui.fileList.setOnDragListener { _, e -> listDrag(e) }
        ui.up.setOnDragListener { _, e -> upDrag(e) }
        ui.menu.setOnClickListener { showMenu() }
        // Touch targets only. Focusable, ⋮ took the keyboard whenever a pane
        // was swapped under it, and the next Space opened the menu.
        ui.up.isFocusable = false
        ui.menu.isFocusable = false
        ui.keyboard.isFocusable = false
        ui.keyboard.setOnClickListener { toggleSoftKeyboard() }
        EditorKeys.fill(ui.editKeyRow, ui.editor)
        CurvedEdges.keepClear(ui.editKeys, ui.editKeyRow, EditorKeys.KEY_PADDING_DP)

        // Tapping a file reference or URL in the terminal opens it. Relative
        // paths are tried in the shell's folder, then the open one, then the
        // shell's home, which is last because `~/` means it.
        ui.terminal.linkDirs = {
            listOfNotNull(shellFolder, folderPath()?.path, filesDir.absolutePath).distinct()
        }
        ui.terminal.onLink = ::openTerminalLink
        // bash in Termux says where it is (OSC 7) and when a command ends
        // (OSC 133;D): the title follows it, and the file list looks again,
        // which is the only way a folder from the picker hears of a change.
        ui.terminal.onDirectory = { dir -> shellFolder = dir; updateTitle() }
        ui.terminal.onCommandEnded = { scheduleListRefresh() }

        // Source control takes the file list's place (leader V); a diff or a
        // commit it shows takes the editor's.
        ui.gitPanel.folder = ::gitFolder
        ui.gitPanel.onShow = ::showDiff

        // Video and audio: the title follows the size and length once known.
        ui.player.onChanged = { updateTitle() }

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
                if (isMarkdown(name)) scheduleTaskCount()
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
        sidebarIsTodos = false
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
        sidebarIsTodos = false
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
    private fun notReachable(name: String,
                             shellHome: String = "MiniCode's own private folder"): String {
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
        return "$why This shell is in $shellHome."
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
            p == Termux.HOME || p.startsWith("${Termux.HOME}/") -> under(Termux.HOME, "Termux home")
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

    // ------------------------------------------------- moving by dragging

    /**
     * A drag from the file list, from the long press that started it to the
     * drop. `from` is the folder the entry is in; `key` and `fromKey` are
     * FileMove.keyOf for the two.
     */
    private class Drag(val item: DocumentFile, val from: DocumentFile, val name: String,
                       val key: String, val fromKey: String)

    private var drag: Drag? = null

    /**
     * Starts dragging `entry` from its row: the file list's long press,
     * then a move of the finger. The shadow is a small label with the name,
     * since the whole row is as wide as the screen.
     */
    private fun startMove(row: View, entry: DocumentFile) {
        val from = current ?: return
        if (lostAccess) return
        val name = entry.name ?: return
        val d = Drag(entry, from, name, FileMove.keyOf(entry), FileMove.keyOf(from))
        val dp = resources.displayMetrics.density
        val label = TextView(this).apply {
            text = if (files.docAt(ui.fileList.getChildAdapterPosition(row))?.isDir == true)
                "$name/" else name
            textSize = 15f
            setTextColor(Palette.TEXT)
            setPadding((12 * dp).toInt(), (6 * dp).toInt(), (12 * dp).toInt(), (6 * dp).toInt())
            background = android.graphics.drawable.GradientDrawable().apply {
                setColor(DROP_COLOR)
                cornerRadius = 6 * dp
            }
            val any = View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED)
            measure(any, any)
            layout(0, 0, measuredWidth, measuredHeight)
        }
        drag = d
        dragMoved = false
        val started = row.startDragAndDrop(android.content.ClipData.newPlainText(name, name),
                                           View.DragShadowBuilder(label), d, 0)
        if (!started) drag = null
    }

    /**
     * What a drop at (x, y) in the list would move into: the folder row
     * there, or else the folder being listed. The row is returned too, to
     * be lit up.
     */
    private fun dropTargetAt(x: Float, y: Float): Pair<DocumentFile?, View?> {
        val row = ui.fileList.findChildViewUnder(x, y)
        val doc = row?.let { files.docAt(ui.fileList.getChildAdapterPosition(it)) }
        if (doc != null && doc.isDir) return doc.file to row
        return (if (lostAccess) null else current) to null
    }

    /** Why `into` cannot take the dragged entry, or null when it can. */
    private fun refusal(d: Drag, into: DocumentFile): FileMove.Refusal? =
        FileMove.refusal(d.key, d.fromKey, FileMove.keyOf(into))

    private fun listDrag(e: android.view.DragEvent): Boolean {
        if (e.action == android.view.DragEvent.ACTION_DRAG_ENDED) { endDrag(); return true }
        val d = e.localState as? Drag ?: return false
        when (e.action) {
            android.view.DragEvent.ACTION_DRAG_STARTED -> return true
            android.view.DragEvent.ACTION_DRAG_LOCATION -> {
                dragX = e.x; dragY = e.y
                hoverAt(d, e.x, e.y)
                val edge = 48 * resources.displayMetrics.density
                val dir = when {
                    e.y < edge -> -1
                    e.y > ui.fileList.height - edge -> 1
                    else -> 0
                }
                if (dir != edgeScrollDir) {
                    edgeScrollDir = dir
                    ui.root.removeCallbacks(edgeScroll)
                    if (dir != 0) ui.root.post(edgeScroll)
                }
            }
            android.view.DragEvent.ACTION_DRAG_EXITED -> clearListHover()
            android.view.DragEvent.ACTION_DROP -> {
                val (into, _) = dropTargetAt(e.x, e.y)
                clearListHover()
                return into != null && moveInto(d, into)
            }
        }
        return true
    }

    /** The drag is over the list at (x, y): light up where it would go. */
    private fun hoverAt(d: Drag, x: Float, y: Float) {
        val (into, row) = dropTargetAt(x, y)
        val ok = into != null && refusal(d, into) == null
        val lit = if (ok) row ?: ui.fileList else null
        if (lit !== hoverView) unlight()
        hoverView = lit
        // Set every time: a row scrolled away and back is bound afresh,
        // which clears its background.
        when (lit) {
            null -> {}
            ui.fileList -> ui.fileList.setBackgroundColor(DROP_LIST_COLOR)
            else -> lit.setBackgroundColor(DROP_COLOR)
        }
        // Held over a folder row, the list opens that folder, so a drop can
        // go deeper than the folders on screen. Never the dragged folder
        // itself: nothing can be moved into it, and the list never being
        // inside it is what keeps every drop out of it.
        val spring = if (row == null) null else into?.takeIf { it.uri != d.item.uri }
        if (spring?.uri != springTarget?.uri) {
            ui.root.removeCallbacks(springOpen)
            springTarget = spring
            if (spring != null) ui.root.postDelayed(springOpen, SPRING_MS)
        }
    }

    private var hoverView: View? = null
    private var springTarget: DocumentFile? = null
    private var dragX = 0f
    private var dragY = 0f
    private var edgeScrollDir = 0
    private var dragMoved = false

    /** The folder held over during a drag opens in the list. */
    private val springOpen = Runnable {
        val dir = springTarget ?: return@Runnable
        clearListHover()
        list(dir)
    }

    /** Held near the top or bottom of the list during a drag, it scrolls. */
    private val edgeScroll = object : Runnable {
        override fun run() {
            val d = drag ?: return
            if (edgeScrollDir == 0) return
            ui.fileList.scrollBy(0, (edgeScrollDir * 10 * resources.displayMetrics.density).toInt())
            hoverAt(d, dragX, dragY)
            ui.root.postDelayed(this, 25)
        }
    }

    private fun unlight() {
        when (val v = hoverView) {
            null -> {}
            ui.fileList -> ui.fileList.setBackgroundColor(Palette.SIDEBAR)
            else -> v.background = null
        }
        hoverView = null
    }

    private fun clearListHover() {
        unlight()
        ui.root.removeCallbacks(springOpen)
        springTarget = null
        edgeScrollDir = 0
        ui.root.removeCallbacks(edgeScroll)
    }

    /** ↑ during a drag: the folder above the listed one. */
    private fun upDrag(e: android.view.DragEvent): Boolean {
        if (e.action == android.view.DragEvent.ACTION_DRAG_ENDED) { endDrag(); return true }
        val d = e.localState as? Drag ?: return false
        val into = current?.takeIf { it.uri != folder?.uri }?.parentFile
        when (e.action) {
            android.view.DragEvent.ACTION_DRAG_STARTED -> return true
            android.view.DragEvent.ACTION_DRAG_ENTERED -> { upHovered = true; lightUp(d) }
            android.view.DragEvent.ACTION_DRAG_EXITED -> { upHovered = false; clearUpHover() }
            android.view.DragEvent.ACTION_DROP -> {
                upHovered = false
                clearUpHover()
                return into != null && moveInto(d, into)
            }
        }
        return true
    }

    /** The folder above the listed one: lit when it can take the drag, and gone to when held. */
    private fun lightUp(d: Drag) {
        val into = current?.takeIf { it.uri != folder?.uri }?.parentFile ?: return
        if (refusal(d, into) == null) ui.up.setBackgroundColor(DROP_COLOR)
        ui.root.removeCallbacks(springUp)
        ui.root.postDelayed(springUp, SPRING_MS)
    }

    private var upHovered = false

    /** Held on ↑, the list goes up a level, and on up while it stays held. */
    private val springUp = Runnable {
        clearUpHover()
        val d = drag ?: return@Runnable
        if (current?.uri == folder?.uri) return@Runnable
        goUp()
        if (upHovered && ui.up.visibility == View.VISIBLE) lightUp(d)
    }

    private fun clearUpHover() {
        ui.up.background = null
        ui.root.removeCallbacks(springUp)
    }

    /**
     * Every listening view hears the end of a drag; the first to does the
     * tidying. A drag that moved nothing puts the list back on the folder
     * it started from, if holding over folders had taken it elsewhere.
     */
    private fun endDrag() {
        val d = drag ?: return
        drag = null
        upHovered = false
        clearListHover()
        clearUpHover()
        if (!dragMoved && current?.uri != d.from.uri && ui.fileList.visibility == View.VISIBLE)
            list(d.from)
    }

    /**
     * Moves the dragged entry into `into`, unless that is refused. Then the
     * open file follows it, and the list shows the result. True when it
     * moved.
     */
    private fun moveInto(d: Drag, into: DocumentFile): Boolean {
        when (refusal(d, into)) {
            // Lifting the finger over the row itself, or over the folder it
            // is already in, is how a drag is let go of: nothing to say.
            FileMove.Refusal.SELF, FileMove.Refusal.ALREADY_THERE -> return false
            FileMove.Refusal.INSIDE -> { say("${d.name} cannot go inside itself."); return false }
            null -> {}
        }
        // Where the open file is below the moved entry, if it is, while the
        // old names can still be read.
        val trail = currentFile?.let { FileMove.trail(it, d.item) }
        val result = FileMove.move(this, d.item, d.from, into, providerName(into.uri.authority))
        if (result is FileMove.Result.Failed) {
            android.widget.Toast.makeText(this, result.message, android.widget.Toast.LENGTH_LONG).show()
            return false
        }
        val moved = (result as FileMove.Result.Moved).file
        dragMoved = true
        if (trail != null) followMove(moved, trail)
        // The top of shared storage is a folder called "0".
        val place = if (where(into) == PHONE_STORAGE) PHONE_STORAGE else into.name ?: "the folder"
        say("Moved ${d.name} to $place")
        current?.let { list(it, keepPlace = true) }
        ui.gitPanel.refresh()
        return true
    }

    /**
     * The open file, or the folder it is in, was moved: the buffer stays as
     * it is (unsaved edits too) and from now on belongs to the new place,
     * for saving, watching, the title, the language server and the LaTeX
     * and Markdown previews.
     */
    private fun followMove(moved: DocumentFile, trail: List<String>) {
        val open = currentFile ?: return
        val now = FileMove.follow(moved, trail) ?: run {
            say("${open.name} moved, and MiniCode lost track of it. Open it again from its new folder.")
            return
        }
        currentFile = now
        diskStamp = stampOf(now)
        watchOpenFile()
        when {
            showingPlayer -> ui.player.moved(now)
            showingMedia -> {}
            else -> {
                val text = ui.editor.text?.toString().orEmpty()
                if (LatexPreview.isLatex(now.name)) ui.latex.open(now, text)
                lsp.opened(now, folder)
                if (previewing && isMarkdown(now.name)) renderPreview()
            }
        }
        updateTitle()
    }

    /** A running shell follows the folder when another is opened. */
    private fun followFolder() {
        if (!ui.terminal.isRunning) return
        val (cwd, why) = if (ui.terminal.isTermux) termuxDirectory() else terminalDirectory()
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
        // Files may have come and gone while MiniCode was away (in Termux,
        // say): the list and the open file are read again, and watched.
        resumed = true
        watchFolder()
        watchOpenFile()
        scheduleListRefresh()
        checkOpenFile()
        if (ui.terminal.isRunning && !ui.terminal.isTermux && shellFolder == null &&
            canReachPaths()) followFolder()
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

    /**
     * Lists `dir`. With `keepPlace` (a refresh of the folder already shown)
     * nothing happens unless the entries changed, and then the scroll
     * position and the focused row stay where they were.
     */
    private fun list(dir: DocumentFile, keepPlace: Boolean = false) {
        current = dir
        // Folders first, then files, each alphabetically, as the other ports do.
        val entries = readFolder(dir)?.map { Entry(it, it.name ?: "", it.isDirectory) }
            ?.sortedWith(compareBy({ !it.isDir }, { it.name.lowercase() }))
        val shape = entries?.map { (if (it.isDir) "d:" else "f:") + it.name }
        if (keepPlace && shape == listed && (entries == null) == lostAccess) return
        listed = shape
        lostAccess = entries == null
        val state = if (keepPlace) ui.fileList.layoutManager?.onSaveInstanceState() else null
        val focused = if (keepPlace) focusedRowLabel() else null
        files.submit(when {
            entries == null -> lostRows(dir)
            entries.isEmpty() -> listOf(
                ListRow.Note("This folder is empty."),
                ListRow.Action("New file") { newFile() })
            else -> entries.map { ListRow.Doc(it.file, it.isDir) }
        })
        state?.let { ui.fileList.layoutManager?.onRestoreInstanceState(it) }
        if (focused != null) ui.fileList.post { focusRowLabelled(focused) }
        ui.up.visibility = if (dir.uri == folder?.uri || ui.fileList.visibility != View.VISIBLE)
            View.GONE else View.VISIBLE
        updateTitle()
        watchFolder()
        if (!keepPlace && entries.isNullOrEmpty() && ui.fileList.visibility == View.VISIBLE) focusFileList()
    }

    /** A listed entry, with the name and kind read once for sorting. */
    private class Entry(val file: DocumentFile, val name: String, val isDir: Boolean)

    /** What the list shows, as "d:name" and "f:name", or null when unreadable. */
    private var listed: List<String>? = null

    private fun focusedRowLabel(): CharSequence? {
        val v = ui.fileList.focusedChild as? TextView ?: return null
        return v.text
    }

    private fun focusRowLabelled(label: CharSequence) {
        for (i in 0 until ui.fileList.childCount) {
            val v = ui.fileList.getChildAt(i) as? TextView ?: continue
            if (v.text.toString() == label.toString() && v.isFocusable) { v.requestFocus(); return }
        }
    }

    // ------------------------------------------- following changes on disk

    /**
     * The file list follows the disk. A folder with a path MiniCode may read
     * (Phone storage, or a picked folder on the phone's storage once "All
     * files access" is granted) is watched with a FileObserver, so a file
     * made by `touch` in either terminal, by Termux or over adb shows up
     * within a moment. A folder from another app or the cloud has no path to
     * watch; it is read again on returning to the app, on showing the list,
     * and whenever a command finishes in a Termux shell.
     *
     * The open file's folder is watched the same way, for writes to the file.
     * One observer per folder, whatever it is watched for: Android keeps one
     * inotify watch per path, so a second FileObserver on the same folder
     * replaces the first one's events instead of adding to them (the list
     * stopped seeing deletions once a file in it was opened).
     *
     * Events come in bursts (an unzip, a git checkout), so they only start a
     * short timer, and the refresh reads the folder once.
     */
    private fun updateWatches() {
        val listDir = current?.let(::pathOf)?.takeIf { resumed && readablePath(it) }?.path
        val file = currentFile?.let(::pathOf)?.takeIf { resumed && readablePath(it) }
        watchedListDir = listDir
        watchedFileDir = file?.parentFile?.path
        watchedFileName = file?.name
        val wanted = setOfNotNull(listDir, watchedFileDir)
        for ((dir, observer) in observers.toList()) {
            if (dir !in wanted) { observer.stopWatching(); observers.remove(dir) }
        }
        for (dir in wanted) {
            if (dir in observers) continue
            observers[dir] = object : android.os.FileObserver(File(dir), WATCHED) {
                // On FileObserver's own thread; only what matters is posted.
                override fun onEvent(event: Int, name: String?) {
                    val what = event and ALL_EVENTS
                    if (dir == watchedListDir && what and LIST_EVENTS != 0)
                        ui.root.post { scheduleListRefresh() }
                    if (dir == watchedFileDir && name == watchedFileName && what and FILE_EVENTS != 0)
                        ui.root.post {
                            ui.root.removeCallbacks(fileCheck)
                            ui.root.postDelayed(fileCheck, 300)
                        }
                }
            }.also { it.startWatching() }
        }
    }

    private fun watchFolder() = updateWatches()
    private fun watchOpenFile() = updateWatches()

    private val observers = mutableMapOf<String, android.os.FileObserver>()
    @Volatile private var watchedListDir: String? = null
    @Volatile private var watchedFileDir: String? = null
    @Volatile private var watchedFileName: String? = null
    private var resumed = false

    /** MiniCode itself may read this path (not only Termux). */
    private fun readablePath(f: File) =
        canReachPaths() || f.path.startsWith(filesDir.parentFile?.path ?: filesDir.path)

    private val listRefresh = Runnable { refreshList() }

    private fun scheduleListRefresh() {
        ui.root.removeCallbacks(listRefresh)
        ui.root.postDelayed(listRefresh, 250)
    }

    /**
     * Reads the listed folder again, keeping the place. Only while the list
     * is on screen; showing it later refreshes it then. A subfolder that was
     * deleted gives way to the nearest folder above it that is still there.
     */
    private fun refreshList() {
        if (ui.fileList.visibility != View.VISIBLE) return
        var dir = current ?: return
        if (dir.uri != folder?.uri && !stillThere(dir)) {
            var up = dir.parentFile
            while (up != null && up.uri != folder?.uri && !stillThere(up)) up = up.parentFile
            dir = up ?: folder ?: return
            list(dir)
            return
        }
        list(dir, keepPlace = true)
    }

    private fun stillThere(dir: DocumentFile) = try { dir.isDirectory } catch (e: Exception) { false }

    /**
     * When the open file was last read or written, as its modification time
     * and length, to notice another program changing it.
     */
    private var diskStamp: Pair<Long, Long>? = null
    /** Set once the user has been told the unsaved file changed on disk. */
    private var diskConflictSaid = false

    private fun stampOf(file: DocumentFile): Pair<Long, Long>? = try {
        if (file.exists()) file.lastModified() to file.length() else null
    } catch (e: Exception) { null }

    /**
     * The open file changed on disk: a buffer with no unsaved edits takes
     * the new text (as one edit, so undo can go back), keeping the caret; an
     * unsaved one is kept, the user is told once, and saving asks before
     * writing over the other program's change.
     */
    private fun checkOpenFile() {
        val file = currentFile ?: return
        if (showingPlayer) { checkPlayedFile(file); return }
        if (showingMedia || showingDiff) return
        val now = stampOf(file) ?: return   // gone: saving writes it again
        if (now == diskStamp) return
        if (dirty) {
            if (!diskConflictSaid) {
                diskConflictSaid = true
                say("${file.name} changed on disk. Your unsaved edits are kept; saving asks first.")
            }
            return
        }
        val text = readText(file) ?: return
        val caret = ui.editor.selectionStart
        applyPreviewSource(text, undoable = false)
        mdUndo.clear(); mdRedo.clear()
        dirty = false
        diskStamp = now
        ui.editor.setSelection(caret.coerceIn(0, ui.editor.text?.length ?: 0))
        updateTitle()
    }

    private val fileCheck = Runnable { checkOpenFile() }

    override fun onPause() {
        super.onPause()
        resumed = false
        // Nothing is watched while MiniCode is out of sight; returning reads
        // everything again.
        updateWatches()
        // No sound from a window the user has left.
        ui.player.pause()
    }

    /**
     * Out of sight, the player gives up its decoder and keeps its place;
     * back in sight it opens the file again, paused there (PlayerPane).
     */
    override fun onStop() {
        super.onStop()
        ui.player.suspend()
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
            // Pictures, PDFs, video and audio are shown, not read as text, so
            // they never reach the editor and cannot be saved over.
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
     * An image, the first page of a PDF, or a video or audio file, in the
     * editor's slot. All are decoded by Android itself, as the macOS app
     * leaves them to AppKit, PDFKit and AVKit; there is nothing here for the
     * shared core to do.
     */
    private fun showMedia(entry: DocumentFile): Boolean {
        val name = entry.name?.lowercase() ?: return false
        val ext = name.substringAfterLast('.', "")
        if (ext in PlayerPane.VIDEO_EXTENSIONS || ext in PlayerPane.AUDIO_EXTENSIONS) {
            showPlayer(entry, video = ext in PlayerPane.VIDEO_EXTENSIONS)
            return true
        }
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
        showingPlayer = false
        ui.player.close()
        showingDiff = false
        previewing = false
        ui.latex.close()
        ui.previewScroll.close()
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

    /**
     * A video or audio file in the editor's slot (PlayerPane), paused on its
     * first frame, as on the Mac and Linux. It is view only like a picture:
     * nothing to save, and the player lets go of the file whenever the pane
     * is out of sight, so nothing plays from a hidden pane.
     */
    private fun showPlayer(entry: DocumentFile, video: Boolean) {
        currentFile = entry
        diskStamp = stampOf(entry)
        watchOpenFile()
        pdfPages = 0
        showingMedia = true
        showingPlayer = true
        showingDiff = false
        previewing = false
        ui.latex.close()
        ui.previewScroll.close()
        dirty = false
        ui.media.setImageDrawable(null)
        lsp.opened(null, folder)
        ui.player.open(entry, video)
        showList(false)
    }

    /**
     * The played file changed on disk: the player loads it again, keeping
     * its place and whether it was playing. One that is gone stops, and says
     * so.
     */
    private fun checkPlayedFile(file: DocumentFile) {
        val now = stampOf(file)
        if (now == diskStamp) return
        diskStamp = now
        if (now == null) ui.player.gone() else ui.player.reload()
    }

    /** Set with showingMedia while the file is a video or audio one. */
    private var showingPlayer = false
    private var showingMedia = false
    private var mediaSize = ""
    private var pdfPages = 0

    // ------------------------------------------------------------ the editor

    private fun openFile(file: DocumentFile, text: String) {
        currentFile = file
        diskStamp = stampOf(file)
        diskConflictSaid = false
        watchOpenFile()
        showingMedia = false
        showingPlayer = false
        ui.player.close()
        showingDiff = false
        ui.media.setImageDrawable(null)
        highlighting = true
        ui.editor.setText(text)
        highlighting = false
        dirty = false
        mdUndo.clear()
        mdRedo.clear()
        // Return continues a Markdown list, and the title counts its tasks.
        ui.editor.markdownLists = isMarkdown(file.name)
        countTasks()
        // Markdown and LaTeX open rendered, as they do in the other ports,
        // unless the file is empty: a blank preview looks like nothing opened.
        previewing = isPreviewable(file.name) && text.isNotBlank()
        if (LatexPreview.isLatex(file.name)) ui.latex.open(file, text)
        else ui.latex.close()
        // Another Markdown file replaces the page when it renders; anything
        // else lets go of it, and of any web picture still on its way.
        if (!isMarkdown(file.name)) ui.previewScroll.close()
        showList(false)
        updateTitle()
        rehighlight()
        renderPreview(keepScroll = false)
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

    /**
     * Shift+Cmd+P on the Mac; the leader's P here. For Markdown the place
     * is kept both ways, through the source line of each rendered stretch:
     * the preview opens with the caret's part of the page a third of the way
     * down, and the source opens at what the preview had at its top if the
     * preview was scrolled, or else with the caret where it was.
     */
    private fun togglePreview() {
        val name = currentFile?.name
        if (!isPreviewable(name)) return
        val md = isMarkdown(name)
        val caretLine = if (md && !previewing) lineAt(ui.editor.selectionStart) else -1
        val topLine = if (md && previewing && ui.previewScroll.isShown &&
                          ui.previewScroll.scrolledByUser()) ui.previewScroll.topLine() else -1
        previewing = !previewing
        // Preview edits are undone from snapshots; the source has its own undo.
        if (md && !previewing) { mdUndo.clear(); mdRedo.clear() }
        renderPreview(keepScroll = false)
        if (md && previewing && ui.previewScroll.isShown) {
            ui.previewScroll.scrollToLine(caretLine, 0.33f)
            ui.previewScroll.requestFocus()
            // Nothing to type into on a page; the keyboard's strip would stay.
            (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager)
                .hideSoftInputFromWindow(ui.root.windowToken, 0)
        } else if (md && !previewing && ui.editor.isShown) {
            ui.editor.requestFocus()
            if (topLine >= 0) showSourceLine(topLine)
        }
        updateTitle()
    }

    /** The 0-based line of `offset` in the buffer. */
    private fun lineAt(offset: Int): Int {
        val text = ui.editor.text ?: return 0
        var n = 0
        for (i in 0 until offset.coerceIn(0, text.length)) if (text[i] == '\n') n++
        return n
    }

    /** Puts the caret at the start of 0-based `line` and that line at the top. */
    private fun showSourceLine(line: Int) {
        val text = ui.editor.text ?: return
        var at = 0
        for (k in 0 until line) {
            val nl = text.indexOf('\n', at)
            if (nl < 0) break
            at = nl + 1
        }
        ui.editor.setSelection(at)
        ui.editor.post {
            val l = ui.editor.layout ?: return@post
            val visible = ui.editor.height - ui.editor.totalPaddingTop - ui.editor.totalPaddingBottom
            val max = maxOf(0, l.height - visible)
            ui.editor.scrollTo(0, l.getLineTop(l.getLineForOffset(at)).coerceIn(0, max))
        }
    }

    /**
     * Renders the preview if it is the pane showing. `keepScroll` leaves a
     * Markdown page where it was, which is what an edit made from the preview
     * wants; opening a file starts at the top.
     */
    private fun renderPreview(keepScroll: Boolean = true) {
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
        ui.previewScroll.show(ui.editor.text.toString(), keepScroll)
    }

    // ------------------------------------------------------------ Markdown preview

    /** Preview edits, as whole-source snapshots (capped), for leader U and R. */
    private val mdUndo = ArrayList<String>()
    private val mdRedo = ArrayList<String>()

    private fun markdownPreviewShowing() = ui.previewScroll.visibility == View.VISIBLE

    /**
     * A path as a Markdown file writes it (relative to the file, or
     * absolute) -> the file, or null. A file opened by path resolves by
     * path; one from the document picker walks the picked tree from its
     * folder, since it has no path.
     */
    private fun resolveRelative(written: String): DocumentFile? {
        val file = currentFile ?: return null
        if (written.startsWith("/")) {
            val f = File(written)
            return if (f.exists()) DocumentFile.fromFile(f) else null
        }
        if (file.uri.scheme == "file") {
            val base = File(file.uri.path ?: return null).parentFile ?: return null
            val f = try { File(base, written).canonicalFile } catch (e: Exception) { return null }
            return if (f.exists()) DocumentFile.fromFile(f) else null
        }
        var at: DocumentFile = file.parentFile ?: return null
        for (part in written.split('/')) {
            at = when (part) {
                "", "." -> at
                ".." -> at.parentFile ?: return null
                else -> at.findFile(part) ?: return null
            }
        }
        return at
    }

    /**
     * A tapped link in the preview, as on the Mac and Linux: "#section"
     * scrolls to that heading (GitHub's spelling of the anchor), a web
     * address opens in the browser pane, and a path relative to the file
     * opens it, at the line "#L12" names or the heading another anchor names.
     */
    private fun openMarkdownLink(url: String) {
        if (url.startsWith("#")) {
            if (!ui.previewScroll.scrollToAnchor(url.substring(1))) say("There is no heading $url here.")
            return
        }
        val uri = Uri.parse(url)
        val scheme = uri.scheme?.lowercase()
        if (scheme == "http" || scheme == "https") {
            if (!browserShowing) toggleBrowser()
            navigate(url)
            return
        }
        if (scheme != null && scheme != "file") {
            try {
                startActivity(Intent(Intent.ACTION_VIEW, uri))
            } catch (e: Exception) {
                say("Nothing on this phone opens $scheme: links.")
            }
            return
        }
        var path = if (scheme == "file") uri.path.orEmpty() else url
        var fragment = ""
        val hash = path.indexOf('#')
        if (hash >= 0) {
            fragment = path.substring(hash + 1)
            path = path.substring(0, hash)
        }
        if (scheme != "file") path = Uri.decode(path)
        if (path.isEmpty()) {
            if (!ui.previewScroll.scrollToAnchor(fragment)) say("There is no heading #$fragment here.")
            return
        }
        val target = resolveRelative(path)
        if (target == null) {
            say("$path was not found.")
            return
        }
        if (target.isDirectory) {
            confirmLeave { list(target); showList(true) }
            return
        }
        val line = Regex("L(\\d+).*").matchEntire(fragment)?.groupValues?.get(1)?.toIntOrNull() ?: 0
        if (target.uri == currentFile?.uri) {
            if (line > 0) { if (previewing) togglePreview(); ui.editor.post { moveCaretTo(line, 0) } }
            else if (fragment.isNotEmpty()) ui.previewScroll.scrollToAnchor(fragment)
            return
        }
        confirmLeave {
            openEntry(target)
            if (currentFile?.uri != target.uri) return@confirmLeave
            if (line > 0) {
                // A line is a place in the source, so the source it is.
                if (previewing) togglePreview()
                ui.editor.post { moveCaretTo(line, 0) }
            } else if (fragment.isNotEmpty() && previewing) {
                ui.previewScroll.scrollToAnchor(fragment)
            }
        }
    }

    /**
     * A double tap in the preview: the block of Markdown behind it in a box
     * (MarkdownEditDialog), and on Save the new text spliced over exactly
     * that block by the core's MarkdownEdit. The buffer is then unsaved, as
     * typing leaves it. An edit is dropped if the buffer changed meanwhile.
     */
    private fun editMarkdownBlock(line: Int, column: Int) {
        val file = currentFile ?: return
        val source = ui.editor.text.toString()
        val block = Core.mdBlockAt(source, line, column) ?: return
        val what = when (block[0]) {
            Core.MD_BLOCK_PARAGRAPH -> "a paragraph"
            Core.MD_BLOCK_HEADING -> "a heading"
            Core.MD_BLOCK_LIST_ITEM -> "a list item"
            Core.MD_BLOCK_QUOTE -> "a quote"
            Core.MD_BLOCK_CODE -> "a code block"
            Core.MD_BLOCK_TABLE_CELL -> "a table cell"
            Core.MD_BLOCK_MATH -> "a formula"
            else -> "a table row"
        }
        val start = block[1].coerceIn(0, source.length)
        val end = block[2].coerceIn(start, source.length)
        MarkdownEditDialog.show(this, what, source.substring(start, end),
                                block[0] == Core.MD_BLOCK_LIST_ITEM) { text, adding ->
            if (currentFile?.uri != file.uri || ui.editor.text.toString() != source ||
                !markdownPreviewShowing()) {
                say("The file changed while the box was open, so the edit was not applied.")
                return@show
            }
            val edited = Core.mdApply(source, line, column, text, adding) ?: return@show
            applyPreviewSource(edited, undoable = true)
        }
    }

    /**
     * Makes the buffer `source`, as the smallest splice that gets there, so
     * the highlighter, the language server and the title see one ordinary
     * edit. The preview re-renders from the buffer and keeps its place.
     */
    private fun applyPreviewSource(source: String, undoable: Boolean) {
        val editable = ui.editor.text ?: return
        val old = editable.toString()
        if (old == source) return
        if (undoable) {
            mdUndo.add(old)
            if (mdUndo.size > MD_UNDO_LIMIT) mdUndo.removeAt(0)
            mdRedo.clear()
        }
        val n = minOf(old.length, source.length)
        var p = 0
        while (p < n && old[p] == source[p]) p++
        if (p > 0 && Character.isHighSurrogate(old[p - 1])) p--
        var q = 0
        while (q < n - p && old[old.length - 1 - q] == source[source.length - 1 - q]) q++
        if (q > 0 && Character.isLowSurrogate(old[old.length - q])) q--
        editable.replace(p, old.length - q, source, p, source.length - q)
    }

    // ------------------------------------------------------------ task lists

    /**
     * A tap on a task's box in the preview: the core flips the one character
     * between its brackets (MarkdownTasks::toggleBox), and the new source
     * reaches the buffer as any preview edit does, the smallest splice, with
     * a snapshot for leader U. The file is unsaved until it is saved.
     */
    private fun toggleTaskBox(line: Int) {
        if (currentFile == null || !markdownPreviewShowing()) return
        val edited = Core.mdToggleBox(ui.editor.text.toString(), line) ?: return
        applyPreviewSource(edited, undoable = true)
        countTasks()
    }

    /**
     * Leader L, Ctrl+L: the task key in a Markdown file's source, on every
     * line the caret or selection touches (MarkdownTasks::toggle). A line
     * becomes a task, or all of them are ticked, or all cleared. One
     * replacement, so the text field's undo takes it back in one step.
     */
    private fun toggleTaskInSource() {
        val file = currentFile
        if (file == null || showingMedia || showingDiff || !isMarkdown(file.name)) {
            say("Task lists are for Markdown files.")
            return
        }
        if (!ui.editor.isShown) {
            say(if (markdownPreviewShowing()) "In the preview, tap a task's box to tick it."
                else "Task lists are toggled in the editor.")
            return
        }
        val t = ui.editor.text ?: return
        val a = maxOf(0, minOf(ui.editor.selectionStart, ui.editor.selectionEnd))
        val b = maxOf(0, maxOf(ui.editor.selectionStart, ui.editor.selectionEnd))
        val edit = Core.taskToggle(t.toString(), a, b) ?: return
        val (s, e) = edit.applyTo(t)
        ui.editor.setSelection(s, e)
        countTasks()
    }

    /** Done and total tasks in the open Markdown file, or null: the title shows them. */
    private var taskCount: IntArray? = null
    private val recountTasks = Runnable { countTasks() }

    /** Counts again a moment after typing stops. */
    private fun scheduleTaskCount() {
        ui.title.removeCallbacks(recountTasks)
        ui.title.postDelayed(recountTasks, TASK_COUNT_DELAY)
    }

    private fun countTasks() {
        ui.title.removeCallbacks(recountTasks)
        val file = currentFile
        val before = taskCount
        taskCount = if (file != null && !showingMedia && isMarkdown(file.name))
            Core.mdTaskCount(ui.editor.text.toString()).takeIf { it[1] > 0 } else null
        if (!before.contentEquals(taskCount)) updateTitle()
    }

    /**
     * Leader W, Ctrl+Shift+L: the project's TODOs and open tasks in the file
     * list's place, as source control is; W again (or Back) is the list.
     */
    private fun toggleTodos() {
        if (folder == null) { say("Open a folder first: leader O, Phone storage."); return }
        sidebarIsTodos = ui.todos.visibility != View.VISIBLE
        if (sidebarIsTodos) sidebarIsGit = false
        showList(true)
    }

    /**
     * The folder the TODO list reads, as a path, or why there is none. The
     * core walks the folder with the file system, so a folder from the
     * cloud or another app, which has no path, cannot be read.
     */
    private fun todoRoot(): Pair<File?, String?> {
        val tree = folder ?: return null to "Open a folder first: leader O, Phone storage."
        val path = pathOf(tree)
        if (path == null) return null to "The TODO list reads folders on the phone's storage. " +
                "${tree.name ?: "This folder"} comes from another app or the cloud."
        if (tree.uri.scheme != "file" && !canReachPaths())
            return null to "The TODO list needs \"All files access\" to read this folder. " +
                    "Open the terminal once to be asked for it."
        if (!path.isDirectory || !path.canRead()) return null to "Cannot read ${path.path}."
        return path to null
    }

    /** A row of the TODO list: its file, in the source, at its line and column. */
    private fun openTodo(todo: Core.Todo) {
        val tree = folder ?: return
        val target = entryUnder(tree, todo.path)
        if (target == null || !target.isFile) {
            say("${todo.path} is not there any more.")
            return
        }
        val go = go@{
            if (currentFile?.uri != target.uri) openEntry(target)
            if (currentFile?.uri != target.uri) return@go
            if (sidebarShowing()) showList(false)
            // A line is a place in the source, so the source it is.
            if (previewing) togglePreview()
            ui.editor.post { moveCaretTo(todo.line, todo.column) }
        }
        // The open file itself needs no question about its unsaved edits.
        if (currentFile?.uri == target.uri) go() else confirmLeave(go)
    }

    /** The entry at `relative` ('/' separated) under `tree`, or null. */
    private fun entryUnder(tree: DocumentFile, relative: String): DocumentFile? {
        if (tree.uri.scheme == "file") {
            val base = tree.uri.path ?: return null
            val f = File(base, relative)
            return if (f.exists()) DocumentFile.fromFile(f) else null
        }
        var at = tree
        for (part in relative.split('/')) {
            if (part.isEmpty()) continue
            at = at.findFile(part) ?: return null
        }
        return at
    }

    /**
     * Leader U and R (Ctrl+Z and Ctrl+Shift+Z on a keyboard that has Ctrl):
     * in the Markdown preview, the preview's own edits, a snapshot each; in
     * the editor, the text field's own undo, which a phone keyboard has no
     * other way to reach.
     */
    private fun undo(redo: Boolean) {
        if (markdownPreviewShowing() && currentFile != null) {
            val from = if (redo) mdRedo else mdUndo
            val to = if (redo) mdUndo else mdRedo
            if (from.isEmpty()) {
                say(if (redo) "Nothing to redo in the preview." else "Nothing to undo in the preview.")
                return
            }
            to.add(ui.editor.text.toString())
            applyPreviewSource(from.removeAt(from.lastIndex), undoable = false)
            return
        }
        if (ui.editor.isShown && currentFile != null) {
            ui.editor.onTextContextMenuItem(if (redo) android.R.id.redo else android.R.id.undo)
        }
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
        // A Termux shell lives in Termux's process: without this it would
        // outlive the window until MiniCode's own process ended.
        ui.terminal.stop()
        lsp.shutdown()
        ui.gitPanel.shutdown()
        ui.todos.shutdown()
        ui.player.close()
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
    private fun save(force: Boolean = false): Boolean {
        if (showingMedia) return true
        val file = currentFile ?: run {
            if (!showingDiff) say("Nothing to save: no file is open.")
            return true
        }
        // Another program wrote the file since it was read: ask rather than
        // overwrite its change without a word.
        val onDisk = stampOf(file)
        if (!force && onDisk != null && diskStamp != null && onDisk != diskStamp) {
            androidx.appcompat.app.AlertDialog.Builder(this)
                .setTitle("${file.name} changed on disk")
                .setMessage("Another program changed this file since it was opened. " +
                        "Save over its change, or load the file as it is on disk " +
                        "(your unsaved edits are dropped)?")
                .setPositiveButton("Save over it") { _, _ -> save(force = true) }
                .setNegativeButton("Load from disk") { _, _ ->
                    dirty = false
                    checkOpenFile()
                }
                .setNeutralButton("Cancel", null)
                .show()
            return false
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
        diskStamp = stampOf(file)
        diskConflictSaid = false
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
        val listWasShowing = ui.fileList.visibility == View.VISIBLE
        if (show) { terminalShowing = false; browserShowing = false }
        val start = show && folder == null
        if (start) fillStart()
        ui.start.visibility = if (start) View.VISIBLE else View.GONE
        ui.browser.visibility = View.GONE
        val filesKind = !sidebarIsGit && !sidebarIsTodos
        ui.fileList.visibility = if (show && !start && filesKind) View.VISIBLE else View.GONE
        ui.gitPanel.visibility = if (show && !start && sidebarIsGit) View.VISIBLE else View.GONE
        setGitActive(show && !start && sidebarIsGit)
        setTodosShowing(show && !start && sidebarIsTodos)
        ui.terminal.visibility = View.GONE; ui.termKeys.visibility = View.GONE
        val diff = !show && showingDiff
        ui.diffView.visibility = if (diff) View.VISIBLE else View.GONE
        val preview = !diff && !show && previewing && isPreviewable(currentFile?.name)
        val media = !diff && !show && showingMedia
        val pane = previewPane(currentFile?.name)
        ui.previewScroll.visibility =
            if (preview && pane == ui.previewScroll) View.VISIBLE else View.GONE
        ui.latex.visibility = if (preview && pane == ui.latex) View.VISIBLE else View.GONE
        ui.media.visibility = if (media && !showingPlayer) View.VISIBLE else View.GONE
        // Hiding the player releases it (PlayerPane), so nothing plays behind
        // the list, and showing it again opens the file paused where it was.
        ui.player.visibility = if (media && showingPlayer) View.VISIBLE else View.GONE
        ui.editor.visibility =
            if (show || preview || media || diff) View.GONE else View.VISIBLE
        ui.up.visibility =
            if (show && !start && filesKind && current?.uri != folder?.uri) View.VISIBLE else View.GONE
        if (!show) (if (diff) ui.diffView else if (media && showingPlayer) ui.player else ui.editor)
            .let { v -> v.requestFocus(); v.post { v.requestFocus() } }
        else if (start) ui.start.post { ui.start.focusFirst() }
        else if (sidebarIsGit) ui.gitPanel.focusPanel()
        else if (sidebarIsTodos) ui.todos.focusPanel()
        else if (lostAccess || files.firstAction() >= 0) focusFileList()
        // Nothing to type into in a player; the keyboard strip the terminal
        // raised would otherwise stay under it.
        if (!show && media && showingPlayer)
            (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager)
                .hideSoftInputFromWindow(ui.root.windowToken, 0)
        updateTitle()
        // Coming back to the list (from the terminal, say) reads it again,
        // and coming back to the file checks it was not changed meanwhile.
        if (ui.fileList.visibility == View.VISIBLE && !listWasShowing) scheduleListRefresh()
        if (!show) checkOpenFile()
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
        items += StartScreen.Item("Terminal", if (wantsTermuxShell())
                "Termux's bash, in its home folder until one is open"
            else "A shell, in MiniCode's own folder until one is open") {
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
                ui.todos.visibility == View.VISIBLE || ui.start.visibility == View.VISIBLE

    /**
     * Which the sidebar shows: source control (leader V), the TODO list
     * (leader W), or with neither, the file list.
     */
    private var sidebarIsGit = false
    private var sidebarIsTodos = false
    private var gitActive = false

    /**
     * The TODO list on or off screen. Each time it comes on, the folder is
     * read again; going off stops a scan still under way.
     */
    private fun setTodosShowing(on: Boolean) {
        val was = ui.todos.visibility == View.VISIBLE
        ui.todos.visibility = if (on) View.VISIBLE else View.GONE
        if (on && !was) todoRoot().let { (root, why) -> ui.todos.scan(root, why) }
        else if (!on && was) ui.todos.stop()
    }

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
        if (sidebarIsGit) sidebarIsTodos = false
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
            showingPlayer = false
            ui.player.close()
            ui.media.setImageDrawable(null)
            previewing = false
            ui.latex.close()
            ui.previewScroll.close()
            highlighting = true
            ui.editor.setText("")
            highlighting = false
            lsp.opened(null, folder)
            watchOpenFile()
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
            terminalShowing -> (if (ui.terminal.isTermux) "Terminal (Termux)" else "Terminal") to
                    (shellFolder?.let { describePath(File(it)) }
                        ?: if (ui.terminal.isTermux || wantsTermuxShell()) "Termux home"
                           else "MiniCode's own folder")
            browserShowing -> "Browser" to null
            ui.start.visibility == View.VISIBLE -> "MiniCode" to "No folder open"
            ui.gitPanel.visibility == View.VISIBLE -> "Source Control" to where(folder)
            ui.todos.visibility == View.VISIBLE -> "TODOs" to where(folder)
            showingList -> {
                val dir = current ?: folder
                // The top of shared storage is a folder called "0".
                val top = dir != null && where(dir) == PHONE_STORAGE
                (if (top) PHONE_STORAGE else dir?.name ?: "MiniCode") to when {
                    lostAccess -> "Cannot open this folder"
                    top -> "The top of the phone's storage"
                    else -> where(dir)
                }
            }
            showingDiff -> diffTitle to "From source control, read only"
            else -> {
                val file = currentFile
                val extra = when {
                    !showingMedia -> ""
                    // "640 × 360  0:05" for a video, the length for audio.
                    showingPlayer -> ui.player.describe().let { if (it.isEmpty()) "" else "  $it" }
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
                // A Markdown file with tasks: "notes.md — 3 of 7 done".
                val tasks = taskCount?.takeIf { !showingMedia && isMarkdown(file?.name) }
                    ?.let { " — ${it[0]} of ${it[1]} done" }.orEmpty()
                ((if (dirty) "● " else "") + (file?.name ?: "MiniCode") + extra + tasks) to
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
        sidebarIsTodos = false
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
        // Undo and redo: the phone has no Ctrl for Ctrl+Z.
        'u' to { undo(redo = false) },
        'r' to { undo(redo = true) },
        // Task lists: L toggles the task on the caret's lines in a Markdown
        // file (Ctrl+L, as on the desktop); W lists what is left to do.
        'l' to { toggleTaskInSource() },
        'w' to { toggleTodos() },
    )

    /**
     * The action a held Ctrl gives a letter key, or null to leave the key
     * alone. Ctrl+letter is the leader's letter, with these exceptions:
     *
     * - C, V, X, A, Z and Y belong to the text field (copy, paste, cut,
     *   select all, undo, redo), so they are never taken, and leader Y (the
     *   on-screen keyboard) has no Ctrl form.
     * - C, D, E and I are the leader's stand-ins for keys a real Ctrl already
     *   sends to the shell (Ctrl C, Ctrl D, Ctrl [ and Ctrl I), so they stay
     *   the shell's.
     * - B is the file list, as Cmd+B is on the Mac; Ctrl+Shift+B is the
     *   browser, as on Linux. G is go to definition, and Ctrl+Shift+G source
     *   control, as on the Mac and Linux (V is paste).
     * - L toggles a task, as on the desktop, and Ctrl+Shift+L is the TODO
     *   list. Leader W, the TODO list, has no Ctrl form: Ctrl+W closes
     *   things everywhere else, and is left alone.
     * - While the terminal has the keyboard, Ctrl+letter is the shell's
     *   (Ctrl R searches history, Ctrl U kills the line, and so on). Only
     *   S, B, O and H are taken there, as they always were; with Shift held,
     *   every letter here is taken, the way a desktop terminal keeps
     *   Ctrl+Shift for itself.
     *
     * The key may come from the hardware or, with an input method such as
     * Pastiera that passes a held Ctrl on, through InputConnection.sendKeyEvent;
     * both reach dispatchKeyEvent with the Ctrl meta state set.
     */
    private fun ctrlAction(event: KeyEvent): (() -> Unit)? {
        if (!event.isCtrlPressed || event.isAltPressed || event.isMetaPressed) return null
        val code = event.keyCode
        if (code < KeyEvent.KEYCODE_A || code > KeyEvent.KEYCODE_Z) return null
        val letter = 'a' + (code - KeyEvent.KEYCODE_A)
        val shift = event.isShiftPressed
        if (letter in "cvxazydei") return null
        if (!shift && ui.terminal.hasFocus() && letter !in "sboh") return null
        val letters = leaderActions()
        return when {
            letter == 'b' -> letters.getValue(if (shift) 'b' else 'f')
            letter == 'g' && shift -> letters.getValue('v')
            letter == 'l' && shift -> letters.getValue('w')
            letter == 'w' -> null
            else -> letters[letter]
        }
    }

    private fun handleShortcut(event: KeyEvent): Boolean {
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
        // Play or pause a video or audio file, as Ctrl+Shift+Space does on
        // Linux (Shift+Cmd+Space on the Mac), whatever has the keyboard.
        // With no player on screen the key goes on as usual.
        if (event.keyCode == KeyEvent.KEYCODE_SPACE && event.isCtrlPressed &&
            event.isShiftPressed && ui.player.visibility == View.VISIBLE) {
            handled.add(event.keyCode)
            if (event.repeatCount == 0) ui.player.togglePlay()
            return true
        }
        // Undo and redo in the Markdown preview, as Ctrl+Z does on Linux.
        // The editor's own field handles them itself.
        if (event.isCtrlPressed && markdownPreviewShowing() &&
            (event.keyCode == KeyEvent.KEYCODE_Z || event.keyCode == KeyEvent.KEYCODE_Y)) {
            handled.add(event.keyCode)
            undo(redo = event.keyCode == KeyEvent.KEYCODE_Y || event.isShiftPressed)
            return true
        }
        val act = ctrlAction(event) ?: return false
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
                            if (symbolRow) "Hide the symbol row" else "Show the symbol row",
                            "Undo", "Redo", "Shell: " + when (shellChoice()) {
                                "termux" -> "Termux"; "system" -> "Android"; else -> "automatic" },
                            "Web images in Markdown: " + if (WebImages.enabled) "on" else "off",
                            "Toggle task", "TODOs")
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
                    13 -> undo(redo = false)
                    14 -> undo(redo = true)
                    15 -> chooseShell()
                    16 -> toggleWebImages()
                    17 -> toggleTaskInSource()
                    18 -> toggleTodos()
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
            setTodosShowing(false)
            ui.diffView.visibility = View.GONE
            ui.editor.visibility = View.GONE
            ui.previewScroll.visibility = View.GONE
            ui.latex.visibility = View.GONE
            ui.media.visibility = View.GONE
            ui.player.visibility = View.GONE
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
                // Back from the TODO list is the file list, as W is.
                ui.todos.visibility == View.VISIBLE -> toggleTodos()
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
     * The terminal pane, drawn by the same screen grid the macOS app uses:
     * bash in Termux when it is set up (TermuxShell), with python, git and
     * the rest, or else Android's own sh with the toybox utilities. See
     * startShell and ⋮ Shell.
     */
    private fun toggleTerminal() {
        terminalShowing = !terminalShowing
        if (terminalShowing) {
            ui.fileList.visibility = View.GONE
            ui.start.visibility = View.GONE
            ui.gitPanel.visibility = View.GONE
            setGitActive(false)
            setTodosShowing(false)
            ui.diffView.visibility = View.GONE
            ui.editor.visibility = View.GONE
            ui.previewScroll.visibility = View.GONE
            ui.latex.visibility = View.GONE
            ui.media.visibility = View.GONE
            ui.player.visibility = View.GONE
            ui.browser.visibility = View.GONE
            browserShowing = false
            ui.terminal.visibility = View.VISIBLE; ui.termKeys.visibility = View.VISIBLE
            if (ui.terminal.isRunning) followFolder() else startShell()
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
     * Which shell the terminal runs: "termux" (bash in Termux, with python,
     * git and whatever else is installed there), "system" (Android's own sh
     * and toybox), or "auto", the default, which is Termux's whenever Termux
     * is installed and MiniCode may run commands in it.
     */
    private fun shellChoice() =
        getSharedPreferences("minicode", MODE_PRIVATE).getString("shell", "auto") ?: "auto"

    private fun wantsTermuxShell() = shellChoice() != "system" && Termux.problem(this) == null

    /**
     * Starts the chosen shell, once the pane has a size. A Termux shell that
     * cannot be started falls back to Android's, saying why in the pane.
     */
    private fun startShell() {
        ui.terminal.onExit = { quick ->
            if (quick && ui.terminal.isTermux && terminalShowing) {
                // bash in Termux ended as it began: whatever it printed is
                // gone with the pane, so say so and use Android's shell.
                startSystemShell("bash in Termux ended as soon as it started. " +
                        "This is Android's own shell; try again from ⋮ Shell.")
            } else {
                if (terminalShowing) toggleTerminal()
                ui.terminal.stop()
            }
        }
        if (!wantsTermuxShell()) {
            val note = if (shellChoice() != "system") termuxShellNote() else null
            startSystemShell(note)
            return
        }
        val (cwd, why) = termuxDirectory()
        ui.terminal.post {
            ui.terminal.startTermux(cwd) { reason ->
                if (terminalShowing) startSystemShell(
                    "Termux did not start a shell ($reason) This is Android's own shell.")
            }
            shellFolder = cwd; updateTitle()
            if (why != null) ui.terminal.notice(why)
        }
    }

    private fun startSystemShell(note: String? = null) {
        ui.terminal.stop()
        val (cwd, why) = terminalDirectory()
        ui.terminal.post {
            ui.terminal.start(filesDir.absolutePath, cwd ?: filesDir.absolutePath)
            shellFolder = cwd; updateTitle()
            // Said in the terminal itself, where the question "why am I
            // here?" comes up, rather than in a toast.
            if (note != null) ui.terminal.notice(note)
            if (why != null) ui.terminal.notice(why)
        }
        if (folderPath() != null && !canReachPaths()) askForPathAccess()
    }

    /** One line on why this is not Termux's shell, and how to get it. */
    private fun termuxShellNote(): String = when {
        !Termux.isInstalled(this) -> "Android's own shell, without python or git. " +
                "For Termux's bash here, install Termux from F-Droid."
        else -> "Android's own shell, without python or git. For Termux's " +
                "bash here, allow MiniCode in ⋮ Termux tools."
    }

    /**
     * Where a Termux shell starts, and why not the folder if it cannot. Termux
     * sees shared storage at the same paths MiniCode does, and nothing of
     * MiniCode's own, so a folder anywhere else starts it in Termux's home.
     */
    private fun termuxDirectory(): Pair<String?, String?> {
        val path = folderPath()
        val name = folder?.name ?: "this folder"
        return when {
            folder == null -> null to null
            path == null -> null to notReachable(name, "Termux's home folder")
            !Termux.isShared(path) -> null to "$name is in MiniCode's own storage, " +
                    "which Termux cannot enter. This shell is in Termux's home folder."
            else -> path.path to null
        }
    }

    /**
     * ⋮ Web images in Markdown, the desktop's markdown.web-images: whether
     * the preview fetches pictures from https addresses. Off, they show
     * their alt text and opening a file contacts nothing it links to.
     */
    private fun toggleWebImages() {
        WebImages.enabled = !WebImages.enabled
        getSharedPreferences("minicode", MODE_PRIVATE).edit()
            .putBoolean("webImages", WebImages.enabled).apply()
        say(if (WebImages.enabled) "Web images in Markdown are on."
            else "Web images in Markdown are off: they show their alt text.")
        if (markdownPreviewShowing()) renderPreview()
    }

    /** ⋮ Shell: Termux's bash or Android's sh, and the shell restarts. */
    private fun chooseShell() {
        val choices = arrayOf("auto", "termux", "system")
        val labels = arrayOf(
            "Automatic: Termux's when it is set up",
            "Termux: bash, python, git and your packages",
            "Android: the phone's sh and toybox, no Termux")
        val at = choices.indexOf(shellChoice()).coerceAtLeast(0)
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Shell")
            .setSingleChoiceItems(labels, at) { dialog, which ->
                dialog.dismiss()
                getSharedPreferences("minicode", MODE_PRIVATE).edit()
                    .putString("shell", choices[which]).apply()
                if (choices[which] == "termux" && Termux.problem(this) != null) {
                    termuxSetup()
                    return@setSingleChoiceItems
                }
                // A new shell of the chosen kind, in the pane.
                ui.terminal.stop()
                if (!terminalShowing) toggleTerminal() else startShell()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

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
            "U      Undo",
            "R      Redo",
            "L      Toggle a task (- [ ]) on the",
            "       caret's lines, in Markdown",
            "W      TODOs and open tasks in the",
            "       project (W again: files)",
            "",
            "In the Markdown preview: tap a link",
            "to follow it, tap a task's box to",
            "tick it, double-tap a block to",
            "edit it (Enter saves; New line in",
            "the row under the box for a new line).",
            "",
            "In the terminal: C for Ctrl C,",
            "D for Ctrl D, E for Escape,",
            "I for Tab.",
            "",
            "In source control: Space stages or",
            "unstages, Enter shows the diff, the",
            "key then Enter commits.",
            "",
            "In a video or audio file: Space",
            "plays or pauses, Left and Right go",
            "back or on 5 seconds; tap the",
            "picture for the controls.",
            "",
            "That key alone switches panes,",
            "twice opens this menu, and the ⋮",
            "button does the same.",
            "",
            "A held Ctrl works too: Ctrl and the",
            "letter, except Ctrl B for files,",
            "Ctrl Shift B for the browser and",
            "Ctrl Shift G for source control,",
            "Ctrl Shift L for TODOs (W has no",
            "Ctrl form),",
            "and Ctrl Shift Space plays or pauses.",
            "In the terminal, Ctrl and a letter",
            "go to the shell; add Shift there.")
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
        /** Preview edits kept for undo, as on the Mac. */
        private const val MD_UNDO_LIMIT = 50
        /** The title's task count is redone this long after typing stops. */
        private const val TASK_COUNT_DELAY = 300L

        /** What the file list and the open file are watched for. */
        private const val LIST_EVENTS = android.os.FileObserver.CREATE or
                android.os.FileObserver.DELETE or android.os.FileObserver.MOVED_FROM or
                android.os.FileObserver.MOVED_TO or android.os.FileObserver.DELETE_SELF or
                android.os.FileObserver.MOVE_SELF
        // Deleting or moving the open file away matters to the player, which
        // stops; a text buffer is kept (saving writes it again).
        private const val FILE_EVENTS = android.os.FileObserver.CLOSE_WRITE or
                android.os.FileObserver.MOVED_TO or android.os.FileObserver.CREATE or
                android.os.FileObserver.MODIFY or android.os.FileObserver.DELETE or
                android.os.FileObserver.MOVED_FROM
        private const val WATCHED = LIST_EVENTS or FILE_EVENTS
        private const val ALL_EVENTS = android.os.FileObserver.ALL_EVENTS

        /** Larger files are not opened as text; an EditText would crawl. */
        private const val MAX_TEXT_BYTES = 4L * 1024 * 1024

        /** How many recent folders the start screen and leader O offer. */
        private const val MAX_RECENT = 6

        /** A drop target under a drag: a folder row, ↑, the drag's label. */
        private const val DROP_COLOR = 0xFF04395E.toInt()
        /** The whole list, while a drop there goes into the listed folder. */
        private const val DROP_LIST_COLOR = 0xFF1C2B3A.toInt()
        /** How long a drag rests on a folder or ↑ before the list goes there. */
        private const val SPRING_MS = 900L

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
    class Doc(val file: DocumentFile, val isDir: Boolean = file.isDirectory) : ListRow()
    class Note(val text: String) : ListRow()
    class Action(val label: String, val run: () -> Unit) : ListRow()
}

/**
 * The file list: one row per entry, folders marked by a trailing slash.
 *
 * A long press arms a row. Lifted without moving, it does what a long press
 * always did (`onLongClick`: a folder on Phone storage becomes the project);
 * moved after the press, the row is dragged instead (`onDrag`), to be
 * dropped on a folder (MainActivity.startMove).
 */
class FileListAdapter(private val onClick: (DocumentFile) -> Unit,
                      private val onLongClick: (DocumentFile) -> Unit = {},
                      private val onDrag: (View, DocumentFile) -> Unit = { _, _ -> }) :
    RecyclerView.Adapter<FileListAdapter.Row>() {

    private var entries: List<ListRow> = emptyList()

    fun submit(list: List<ListRow>) {
        entries = list
        notifyDataSetChanged()
    }

    /** The row at `position` when it is a file or folder, else null. */
    fun docAt(position: Int): ListRow.Doc? = entries.getOrNull(position) as? ListRow.Doc

    class Row(val text: TextView) : RecyclerView.ViewHolder(text) {
        /** Set by a long press, until the finger lifts or a drag starts. */
        var held = false
        var lastX = 0f
        var lastY = 0f
        var heldX = 0f
        var heldY = 0f
    }

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

    @android.annotation.SuppressLint("ClickableViewAccessibility")  // clicks still go through performClick
    override fun onBindViewHolder(row: Row, position: Int) {
        val t = row.text
        val dp = t.resources.displayMetrics.density
        t.setOnClickListener(null)
        t.setOnLongClickListener(null)
        t.setOnTouchListener(null)
        row.held = false
        t.background = null
        t.isFocusable = false
        t.minHeight = 0
        t.gravity = android.view.Gravity.CENTER_VERTICAL
        when (val entry = entries[position]) {
            is ListRow.Doc -> {
                val f = entry.file
                t.text = (f.name ?: "?") + if (entry.isDir) "/" else ""
                t.setTextColor(if (entry.isDir) Palette.ACCENT else Palette.TEXT)
                t.setOnClickListener { onClick(f) }
                t.setOnLongClickListener { v ->
                    row.held = true
                    row.heldX = row.lastX
                    row.heldY = row.lastY
                    // The list would take the next move as a scroll.
                    v.parent?.requestDisallowInterceptTouchEvent(true)
                    true
                }
                val slop = android.view.ViewConfiguration.get(t.context).scaledTouchSlop
                t.setOnTouchListener { v, e ->
                    when (e.actionMasked) {
                        android.view.MotionEvent.ACTION_DOWN,
                        android.view.MotionEvent.ACTION_MOVE -> {
                            row.lastX = e.x; row.lastY = e.y
                            if (row.held && Math.hypot((e.x - row.heldX).toDouble(),
                                                       (e.y - row.heldY).toDouble()) > slop) {
                                row.held = false
                                onDrag(v, f)
                            }
                        }
                        android.view.MotionEvent.ACTION_UP -> {
                            if (row.held && entry.isDir) onLongClick(f)
                            row.held = false
                        }
                        android.view.MotionEvent.ACTION_CANCEL -> row.held = false
                    }
                    false
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
