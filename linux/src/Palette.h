// Palette.h — mutible's default dark colors, VS Code's Dark Modern as the
// core's Settings resolves them with no theme named (src/Settings.cpp,
// kThemes), for what is drawn before the settings apply and what they do not
// color. Keeping these values identical is what makes the Linux port look
// like the same editor.
#pragma once

#include "SyntaxHighlighter.h"

// Core chrome colors, as "#RRGGBB" strings (the form GTK CSS and GdkRGBA parse).
namespace pal {
    constexpr const char* EditorBg   = "#1F1F1F";  // editor background
    constexpr const char* EditorText = "#CCCCCC";  // default foreground
    constexpr const char* SidebarBg  = "#181818";  // file tree background
    constexpr const char* TreeText   = "#CCCCCC";  // file tree row text
    constexpr const char* DirIcon    = "#C09553";  // folder icon tint
    constexpr const char* FileIcon   = "#8A99A8";  // file icon tint
    constexpr const char* StatusBg   = "#181818";  // status bar
    constexpr const char* StatusText = "#CCCCCC";
    constexpr const char* Divider    = "#333333";  // pane dividers
    constexpr const char* Selection  = "#264F78";  // selected text, editor and terminal

    // Terminal panel. The background is a shade darker than the editor so the
    // panel reads as a separate surface, as the theme's panel is.
    constexpr const char* TermBg     = "#181818";
    constexpr const char* TermText   = "#CCCCCC";
    constexpr const char* TermCursor = "#AEAFAD";

    // Markdown preview colors.
    constexpr const char* MdHeading  = "#FFFFFF";
    constexpr const char* MdCode     = "#CE9178";
    constexpr const char* MdCodeBg   = "#2B2B2B";
    constexpr const char* MdQuote    = "#9D9D9D";
    constexpr const char* MdRule     = "#555555";
    constexpr const char* MdLink     = "#4DAAFC";
    constexpr const char* MdPlainMsg = "#9D9D9D";
    constexpr const char* MdDone     = "#6E7681";  // a checked task's text
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
        case TokenStyle::Escape:       return "#D7BA7D";
        case TokenStyle::Self:         return "#569CD6";
        case TokenStyle::Tag:          return "#569CD6";
        case TokenStyle::Attribute:    return "#9CDCFE";
        case TokenStyle::TagBracket:   return "#808080";
        case TokenStyle::Plain:
        default:                       return "#CCCCCC";
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
        case TokenStyle::Escape:       return "esc";
        case TokenStyle::Self:         return "self";
        case TokenStyle::Tag:          return "tag";
        case TokenStyle::Attribute:    return "attr";
        case TokenStyle::TagBracket:   return "tagbracket";
        case TokenStyle::Plain:
        default:                       return "plain";
    }
}
