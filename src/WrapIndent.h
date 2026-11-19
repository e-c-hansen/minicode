// WrapIndent.h — pure C++, header only. Where a wrapped line's
// continuation starts (VS Code's editor.wrappingIndent).
//
// editor.wrapping-indent:
//   same    (the default, as VS Code's) under the line's first non-blank
//           character;
//   indent  one level further in than that;
//   none    at the left edge, as before 2026-11-18.
// Whatever the mode, an indent wider than half the room a line has falls
// back to none, so a deeply indented line still gets most of the width
// (VS Code does the same).
//
// The width of a line's leading whitespace is worked out here in a unit
// the caller picks: columns for a grid (a space is 1, a tab runs to the
// next multiple of the tab size: leadingColumns), or points for a view
// whose tabs stop where its paragraph style says (leadingWidth, with the
// stops and the interval after them). The Mac's editor sets a continuation
// line's line fragment in by the answer (WrapIndentContainer.h); nothing in
// the text, its attributes or its carets changes.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace WrapIndent {

enum class Mode { None, Same, Indent };

// "none", "same", "indent" (any case); false for anything else.
inline bool parseMode(const std::string &value, Mode &out) {
    std::string v;
    for (char c : value) v += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    if (v == "none") out = Mode::None;
    else if (v == "same") out = Mode::Same;
    else if (v == "indent") out = Mode::Indent;
    else return false;
    return true;
}

inline const char *modeName(Mode m) {
    return m == Mode::None ? "none" : m == Mode::Indent ? "indent" : "same";
}

// How many of `s` (a line's characters from its start, `n` of them) are its
// leading whitespace: spaces and tabs.
inline size_t leadingLength(const char16_t *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == u' ' || s[i] == u'\t')) i++;
    return i;
}

// The width of a line's leading whitespace, in the unit of `space` (one
// space's width): each tab runs to the first of `tabStops` (ascending)
// past where it starts, then to the next multiple of `tabInterval` past
// it; with neither left, a tab is one space wide. Stops at `limit` (the
// answer is then at least `limit`), so a line of thousands of spaces is
// not read to its end when anything past the limit falls back anyway.
inline double leadingWidth(const char16_t *s, size_t n, double space,
                           const std::vector<double> &tabStops, double tabInterval,
                           double limit = -1) {
    double x = 0;
    const size_t len = leadingLength(s, n);
    for (size_t i = 0; i < len; i++) {
        if (limit >= 0 && x >= limit) break;
        if (s[i] == u' ') {
            x += space;
            continue;
        }
        double next = -1;
        for (double stop : tabStops)
            if (stop > x + 1e-6) {
                next = stop;
                break;
            }
        if (next < 0 && tabInterval > 0) {
            const double k = (double)(long long)(x / tabInterval + 1e-9) + 1;
            next = k * tabInterval;
        }
        x = next < 0 ? x + space : next;
    }
    return x;
}

// Columns: a space is one, a tab runs to the next multiple of `tabSize`.
inline int leadingColumns(const char16_t *s, size_t n, int tabSize) {
    return (int)(leadingWidth(s, n, 1, {}, tabSize > 0 ? tabSize : 1) + 0.5);
}
inline int leadingColumns(const std::u16string &line, int tabSize) {
    return leadingColumns(line.data(), line.size(), tabSize);
}

// Where a continuation line starts, in the unit of `leading` (the line's
// leading whitespace width): 0 for None, `leading` for Same, one `level`
// more for Indent; 0 when that is more than half of `room` (the width a
// line has), as VS Code falls back.
inline double continuationIndent(double leading, Mode mode, double level, double room) {
    if (mode == Mode::None) return 0;
    const double indent = mode == Mode::Indent ? leading + level : leading;
    if (indent <= 0 || indent > room / 2) return 0;
    return indent;
}

}  // namespace WrapIndent
