package org.minicode.editor

/**
 * The shared C++ core, reached through app/src/main/cpp/minicode_jni.cpp.
 * The same files build the macOS app and the GTK port; nothing is
 * reimplemented in Kotlin.
 */
object Core {
    init { System.loadLibrary("minicode") }

    /*
     * Incremental highlighting, through a native handle per open file; see
     * Highlighter.kt, the only caller. Offsets are UTF-16 units.
     */
    /** A highlighter over `text`, or 0 when the file has no grammar. */
    external fun hlOpen(text: String, filename: String): Long
    external fun hlClose(handle: Long)
    /** Units [pos, pos + oldLen) became `inserted`: the re-lexed {start, end}, or empty. */
    external fun hlEdit(handle: Long, pos: Int, oldLen: Int, inserted: String): IntArray
    /** {from, to} of the whole lines covering [start, end), then (start, length, style) triples. */
    external fun hlTokens(handle: Long, start: Int, end: Int): IntArray

    /** True when the core has a grammar for this file name. */
    external fun supports(filename: String): Boolean

    /** Markdown as styled runs; see minicode_jni.cpp for the four parts. */
    private external fun markdown(source: String): Array<Any>

    /**
     * The runs of a Markdown document, in parallel arrays: each run's text,
     * its style flags, where it came from and, for tables and pictures,
     * where it goes.
     */
    class MarkdownRuns(val text: Array<String>, val flags: IntArray,
                       private val extra: IntArray, private val targets: Array<String?>) {
        val size get() = text.size
        /** The 0-based source line the run came from, or -1. */
        fun line(i: Int) = extra[i * 7]
        /** Tables are numbered from 1; 0 is not a table. */
        fun tableId(i: Int) = extra[i * 7 + 1]
        /** 0 is the header row. */
        fun tableRow(i: Int) = extra[i * 7 + 2]
        /** -1 for the padding and rules only a monospace table needs. */
        fun tableCol(i: Int) = extra[i * 7 + 3]
        fun tableCols(i: Int) = extra[i * 7 + 4]
        /** 0 left, 1 center, 2 right. */
        fun tableAlign(i: Int) = extra[i * 7 + 5]
        fun isImage(i: Int) = extra[i * 7 + 6] != 0
        fun url(i: Int): String? = targets[i * 2]
        fun src(i: Int): String? = targets[i * 2 + 1]
    }

    @Suppress("UNCHECKED_CAST")
    fun markdownRuns(source: String): MarkdownRuns {
        val parts = markdown(source)
        return MarkdownRuns(parts[0] as Array<String>, parts[1] as IntArray,
                            parts[2] as IntArray, parts[3] as Array<String?>)
    }

    /** GitHub's anchor for a heading, what a "#section" link names. */
    external fun mdAnchor(heading: String): String

    /**
     * The block of Markdown on 0-based source `line`, or null: {kind, start,
     * end, firstLine, lastLine}, start and end in UTF-16 units of `source`.
     * `column` picks one cell of a table row. See src/MarkdownEdit.h.
     */
    external fun mdBlockAt(source: String, line: Int, column: Int): IntArray?

    /**
     * `source` once the block at (`line`, `column`) holds `text`, or with
     * `adding`, once a new list item holding `text` follows it. Null when
     * nothing would change.
     */
    external fun mdApply(source: String, line: Int, column: Int, text: String,
                         adding: Boolean): String?

    /** MarkdownEdit::Block::Kind, in its order. */
    const val MD_BLOCK_PARAGRAPH = 1
    const val MD_BLOCK_HEADING = 2
    const val MD_BLOCK_LIST_ITEM = 3
    const val MD_BLOCK_QUOTE = 4
    const val MD_BLOCK_CODE = 5
    const val MD_BLOCK_TABLE_CELL = 6
    const val MD_BLOCK_TABLE_ROW = 7
    const val MD_BLOCK_MATH = 8

    /** The bits markdownFlags packs, matching MdRun in the core. */
    const val MD_HEADING = 0x7
    const val MD_BOLD = 1 shl 3
    const val MD_ITALIC = 1 shl 4
    const val MD_CODE = 1 shl 5
    const val MD_CODE_BLOCK = 1 shl 6
    const val MD_QUOTE = 1 shl 7
    const val MD_RULE = 1 shl 8
    const val MD_TABLE = 1 shl 9
    const val MD_LINK = 1 shl 10
    const val MD_ORDERED = 1 shl 11
    fun mdListDepth(flags: Int) = (flags shr 12) and 0xF
    const val MD_STRIKE = 1 shl 16
    /**
     * Inline and display math. The run's text is the TeX, and MD_CODE is
     * set with them, so the preview shows formulas verbatim in the code
     * style; display math has lines of its own.
     */
    const val MD_MATH_INLINE = 1 shl 17
    const val MD_MATH_DISPLAY = 1 shl 18
    /** A list item's marker run ("  • ", "  3. ", or a task's ☐ or ☑). */
    const val MD_MARKER = 1 shl 19
    /**
     * A task item's box, on its marker and on the item's own text: 0 none,
     * 1 open, 2 checked (MdRun::task).
     */
    fun mdTask(flags: Int) = (flags shr 20) and 0x3

    /*
     * Task lists (src/MarkdownTasks.h). The preview's box works on a source
     * line and gives back the whole source, spliced like any preview edit;
     * the editor's two edits work in UTF-16 units, like the Editable.
     */
    /** `source` with the box on 0-based `line` ticked or cleared, or null. */
    external fun mdToggleBox(source: String, line: Int): String?
    /** {done, total} over the document's task items. */
    external fun mdTaskCount(source: String): IntArray
    private external fun mdTaskToggle(text: String, selStart: Int, selEnd: Int): Array<Any>?
    private external fun mdTaskNewline(text: String, selStart: Int, selEnd: Int): Array<Any>?

    /** One replacement, [start, start + length) -> `text`, and the selection after it. */
    class TextEdit(val start: Int, val length: Int, val text: String,
                   val selStart: Int, val selEnd: Int) {
        /** Applies it to `editable` as one replacement, returning the new selection. */
        fun applyTo(editable: android.text.Editable): Pair<Int, Int> {
            editable.replace(start, start + length, text)
            return selStart.coerceIn(0, editable.length) to selEnd.coerceIn(0, editable.length)
        }
    }

    private fun textEdit(parts: Array<Any>?): TextEdit? {
        parts ?: return null
        val n = parts[0] as IntArray
        return TextEdit(n[0], n[1], parts[1] as String, n[2], n[3])
    }

    /**
     * The task key on the lines the selection touches: each becomes a task,
     * or all are checked, or all cleared. Null when nothing changes.
     */
    fun taskToggle(text: String, selStart: Int, selEnd: Int) =
        textEdit(mdTaskToggle(text, selStart, selEnd))

    /** Return continuing a list item, or null for the usual Return. */
    fun taskNewline(text: String, selStart: Int, selEnd: Int) =
        textEdit(mdTaskNewline(text, selStart, selEnd))

    /**
     * The TODO list over a folder (FolderSearch::findTodos), blocking: TODO,
     * FIXME, HACK, XXX and BUG after a comment opener, and open tasks in
     * Markdown files.
     */
    private external fun findTodos(root: String): Array<Any>
    /** Stops a findTodos under way; it then returns what it had. */
    external fun cancelTodos()

    class Todo(val path: String, val line: Int, val column: Int, val text: String)
    class Todos(val items: List<Todo>, val filesMatched: Int, val filesRead: Int,
                val truncated: Boolean, val cancelled: Boolean)

    fun todosIn(root: String): Todos {
        val parts = findTodos(root)
        @Suppress("UNCHECKED_CAST") val paths = parts[0] as Array<String>
        val where = parts[1] as IntArray
        @Suppress("UNCHECKED_CAST") val texts = parts[2] as Array<String>
        val info = parts[3] as IntArray
        return Todos(paths.indices.map { Todo(paths[it], where[it * 2], where[it * 2 + 1], texts[it]) },
                     info[0], info[1], info[2] != 0, info[3] != 0)
    }

    /**
     * What a tap can open in one terminal row: `cells` is a code point per
     * column (-1 for the right half of a wide character). See links_jni.cpp.
     */
    private external fun termLinks(cells: IntArray): Array<Any>

    class TermLink(val isFile: Boolean, val first: Int, val end: Int,
                   val target: String, val line: Int, val column: Int)

    fun termLinksIn(cells: IntArray): List<TermLink> {
        val parts = termLinks(cells)
        val spans = parts[0] as IntArray
        @Suppress("UNCHECKED_CAST") val targets = parts[1] as Array<String>
        return targets.indices.map { i ->
            TermLink(spans[i * 5] == 1, spans[i * 5 + 1], spans[i * 5 + 2],
                     targets[i], spans[i * 5 + 3], spans[i * 5 + 4])
        }
    }

    /** Styles in the order of TokenStyle in src/SyntaxHighlighter.h. */
    const val PLAIN = 0
    const val KEYWORD = 1
    const val TYPE = 2
    const val STRING = 3
    const val COMMENT = 4
    const val NUMBER = 5
    const val PREPROCESSOR = 6
    const val FUNCTION = 7
}

/** The dark palette the other two ports use (VS Code Dark+ values). */
object Palette {
    const val BACKGROUND = 0xFF1E1E1E.toInt()
    const val SIDEBAR = 0xFF252526.toInt()
    const val TEXT = 0xFFD4D4D4.toInt()
    const val MUTED = 0xFF9CA3AF.toInt()
    const val ACCENT = 0xFF4EA1F7.toInt()
    const val DIVIDER = 0xFF333333.toInt()

    // Markdown, from the same defaults the other ports use.
    const val MD_HEADING = 0xFF4EA1F7.toInt()
    const val MD_LINK = 0xFF4EA1F7.toInt()
    const val MD_CODE = 0xFFCE9178.toInt()
    const val MD_QUOTE = 0xFF9CA3AF.toInt()
    /** A checked task's text, struck through too (the desktop's markdown.done). */
    const val MD_DONE = 0xFF858585.toInt()

    val styles = intArrayOf(
        0xFFD4D4D4.toInt(),   // plain
        0xFF569CD6.toInt(),   // keyword
        0xFF4EC9B0.toInt(),   // type
        0xFFCE9178.toInt(),   // string
        0xFF6A9955.toInt(),   // comment
        0xFFB5CEA8.toInt(),   // number
        0xFFC586C0.toInt(),   // preprocessor
        0xFFDCDCAA.toInt(),   // function
    )
}
