// Palette.h — the MiniCode dark color palette, mirrored from the macOS build
// (src/EditorController.mm, Hex() and ColorForStyle()). Keeping these values
// identical is what makes the Linux port look like the same editor.
#pragma once

#include "SyntaxHighlighter.h"

// Core chrome colors, as "#RRGGBB" strings (the form GTK CSS and GdkRGBA parse).
namespace pal {
    constexpr const char* EditorBg   = "#1E1E1E";  // editor background
    constexpr const char* EditorText = "#D4D4D4";  // default foreground
    constexpr const char* SidebarBg  = "#252526";  // file tree background
    constexpr const char* TreeText   = "#CCCCCC";  // file tree row text
    constexpr const char* DirIcon    = "#C09553";  // folder icon tint
    constexpr const char* FileIcon   = "#8A99A8";  // file icon tint
    constexpr const char* StatusBg   = "#007ACC";  // status bar (VS Code blue)
    constexpr const char* StatusText = "#FFFFFF";
    constexpr const char* Divider    = "#333333";  // pane dividers

    // Markdown preview colors.
    constexpr const char* MdHeading  = "#FFFFFF";
    constexpr const char* MdCode     = "#CE9178";
    constexpr const char* MdCodeBg   = "#2A2A2A";
    constexpr const char* MdQuote    = "#9CA3AF";
    constexpr const char* MdRule     = "#555555";
    constexpr const char* MdLink     = "#4EA1F7";
    constexpr const char* MdPlainMsg = "#9CA3AF";
}

// The concrete color for each syntax token style. Mirrors ColorForStyle().
inline const char* ColorForStyle(TokenStyle s) {
    switch (s) {
        case TokenStyle::Keyword:      return "#569CD6";
        case TokenStyle::Type:         return "#4EC9B0";
        case TokenStyle::String:       return "#CE9178";
        case TokenStyle::Comment:      return "#6A9955";
        case TokenStyle::Number:       return "#B5CEA8";
        case TokenStyle::Preprocessor: return "#C586C0";
        case TokenStyle::Function:     return "#DCDCAA";
        case TokenStyle::Plain:
        default:                       return "#D4D4D4";
    }
}

// A stable GtkTextTag name for each style, so we create the tag set once.
inline const char* TagNameForStyle(TokenStyle s) {
    switch (s) {
        case TokenStyle::Keyword:      return "kw";
        case TokenStyle::Type:         return "type";
        case TokenStyle::String:       return "str";
        case TokenStyle::Comment:      return "comment";
        case TokenStyle::Number:       return "num";
        case TokenStyle::Preprocessor: return "pre";
        case TokenStyle::Function:     return "func";
        case TokenStyle::Plain:
        default:                       return "plain";
    }
}
