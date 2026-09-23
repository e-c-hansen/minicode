package org.minicode.editor

/**
 * The shared C++ core, reached through app/src/main/cpp/minicode_jni.cpp.
 * The same files build the macOS app and the GTK port; nothing is
 * reimplemented in Kotlin.
 */
object Core {
    init { System.loadLibrary("minicode") }

    /** Tokens as flat triples: start, length, style, in UTF-16 units. */
    external fun highlight(text: String, filename: String): IntArray

    /** True when the core has a grammar for this file name. */
    external fun supports(filename: String): Boolean

    /** Markdown as styled runs: [0] is Array<String>, [1] is IntArray. */
    private external fun markdown(source: String): Array<Any>

    class MarkdownRuns(val text: Array<String>, val flags: IntArray)

    @Suppress("UNCHECKED_CAST")
    fun markdownRuns(source: String): MarkdownRuns {
        val parts = markdown(source)
        return MarkdownRuns(parts[0] as Array<String>, parts[1] as IntArray)
    }

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
