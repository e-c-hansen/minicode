// TermDrop.h — what a drop of files on a terminal types: each path as one
// shell word, as Terminal.app and iTerm drop them, separated by spaces and
// followed by one, so the next word can be typed or dropped at once. The
// app sends the result as a paste (TerminalScreen::encodePaste, in brackets
// when the program asked), which is how Claude Code, among others, sees a
// dropped image's path and attaches the picture. Header only, no
// dependencies, UTF-8. Tested in tests/terminal_paste_tests.inc
// (term:drop).
//
// The quoting is Terminal.app's: a backslash before every character the
// shell would read as more than itself, so "/a b/it's.png" drops as
// /a\ b/it\'s.png. Claude Code reads a pasted path the same way (it takes
// one outer pair of quotes off, then every backslash before a character),
// and splits several at a space before a "/", which single quotes around
// each would hide. Letters, digits, "/", ".", "_", "-", "+", ",", ":",
// "@", "%" and every byte of a non-ASCII character go as they are. A name
// holding a control character (a line break, a Tab) is quoted as $'...'
// instead, zsh's and bash's ANSI-C quoting, since a backslash before a line
// break is a line continuation, which the shell drops.
#pragma once

#include <string>
#include <vector>

namespace TermDrop {

// One path as a single shell word.
inline std::string quotePath(const std::string& path) {
    if (path.empty()) return "''";
    bool controls = false;
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7F) controls = true;
    std::string out;
    out.reserve(path.size() + 8);
    if (controls) {
        static const char hex[] = "0123456789abcdef";
        out += "$'";
        for (unsigned char c : path) {
            switch (c) {
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\\': out += "\\\\"; break;
                case '\'': out += "\\'"; break;
                default:
                    if (c < 0x20 || c == 0x7F) {
                        out += "\\x";
                        out += hex[c >> 4];
                        out += hex[c & 15];
                    } else {
                        out += (char)c;
                    }
            }
        }
        out += '\'';
        return out;
    }
    for (unsigned char c : path) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c >= 0x80 || c == '/' || c == '.' ||
                           c == '_' || c == '-' || c == '+' || c == ',' || c == ':' ||
                           c == '@' || c == '%';
        if (!plain) out += '\\';
        out += (char)c;
    }
    return out;
}

// Paths dropped together: each quoted, a space after each. Nothing for none.
inline std::string paths(const std::vector<std::string>& paths) {
    std::string out;
    for (const std::string& p : paths) {
        out += quotePath(p);
        out += ' ';
    }
    return out;
}

}  // namespace TermDrop
