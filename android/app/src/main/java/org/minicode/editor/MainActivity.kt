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
            if (uri != null) useFolder(uri, remember = true)
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

        ui.fileList.layoutManager = LinearLayoutManager(this)
        ui.fileList.adapter = files
        ui.openFolder.setOnClickListener { pickFolder.launch(null) }
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
        ui.path.text = dir.name ?: "/"
        ui.up.visibility = if (dir.uri == folder?.uri) View.GONE else View.VISIBLE
    }

    private fun goUp() {
        val parent = current?.parentFile ?: folder ?: return
        list(parent)
    }

    private fun openEntry(entry: DocumentFile) {
        if (entry.isDirectory) { list(entry); return }
        val text = contentResolver.openInputStream(entry.uri)?.use {
            it.readBytes().toString(Charsets.UTF_8)
        } ?: return
        openFile(entry, text)
    }

    // ------------------------------------------------------------ the editor

    private fun openFile(file: DocumentFile, text: String) {
        currentFile = file
        highlighting = true
        ui.editor.setText(text)
        highlighting = false
        dirty = false
        showList(false)
        updateTitle()
        rehighlight()
    }

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

    private fun save() {
        val file = currentFile ?: return
        contentResolver.openOutputStream(file.uri, "wt")?.use {
            it.write(ui.editor.text.toString().toByteArray(Charsets.UTF_8))
        }
        dirty = false
        updateTitle()
    }

    private fun showList(show: Boolean) {
        ui.fileList.visibility = if (show) View.VISIBLE else View.GONE
        ui.listBar.visibility = if (show) View.VISIBLE else View.GONE
        ui.editor.visibility = if (show) View.GONE else View.VISIBLE
        if (!show) ui.editor.requestFocus()
    }

    private fun updateTitle() {
        val waiting = if (leaderArmed) "  …" else ""
        ui.title.text = (if (dirty) "● " else "") +
                (currentFile?.name ?: "MiniCode") + waiting
    }

    /**
     * Shortcuts are handled here, before the key reaches any view. The
     * editor is a text field and consumes every letter, so a shortcut left
     * to onKeyDown never fires while the cursor is in a file, which is
     * exactly when it is wanted.
     *
     * Each key is also logged, which is how the bindings were worked out on
     * a keyboard with no Ctrl: `adb logcat -s MiniCodeKeys`.
     */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            android.util.Log.i("MiniCodeKeys",
                "code=${event.keyCode} (${KeyEvent.keyCodeToString(event.keyCode)})" +
                        " scan=${event.scanCode} meta=0x${event.metaState.toString(16)}" +
                        " char='${event.unicodeChar.toChar()}'" +
                        " alt=${event.isAltPressed} shift=${event.isShiftPressed}" +
                        " ctrl=${event.isCtrlPressed} sym=${event.isSymPressed}")
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
        cancelLeader()
        leaderActions()[letter.lowercaseChar()]?.invoke()
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
     */
    /** One table, keyed by letter, for both ways a shortcut can arrive. */
    private fun leaderActions(): Map<Char, () -> Unit> = mapOf(
        's' to { save() },
        'b' to { showList(ui.fileList.visibility != View.VISIBLE) },
        'o' to { pickFolder.launch(null) },
        'h' to { showShortcuts() },
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
        val act = when {
            event.isCtrlPressed -> actions[event.keyCode] ?: return false
            else -> return false
        }
        handled.add(event.keyCode)
        act()
        return true
    }

    private var pendingSwap: Runnable? = null

    /**
     * The same actions as the shortcuts, for when the keyboard cannot
     * provide them. Phone keyboards differ too much to rely on any key: the
     * Titan 2 has no Ctrl, its Alt types symbols, and the system claims Sym
     * for some letters.
     */
    private fun showMenu() {
        val items = arrayOf("Save", "Files or editor", "Open a folder", "Shortcuts")
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> save()
                    1 -> showList(ui.fileList.visibility != View.VISIBLE)
                    2 -> pickFolder.launch(null)
                    3 -> showShortcuts()
                }
            }
            .show()
    }

    /** The equivalent of the Mac app's Shift+Cmd+H panel, for a phone. */
    private fun showShortcuts() {
        val lines = listOf(
            "The key left of right Shift, then:",
            "S      Save",
            "B      Files or editor",
            "O      Open a folder",
            "H      This list",
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
        val text = TextView(parent.context).apply {
            setPadding(32, 24, 32, 24)
            textSize = 16f
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
