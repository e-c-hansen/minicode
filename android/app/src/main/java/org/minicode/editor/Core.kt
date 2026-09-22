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
