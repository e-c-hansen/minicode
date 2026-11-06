// SideBySide.cpp — see SideBySide.h.
#include "SideBySide.h"
#include "LineDiff.h"

namespace SideBySide {

size_t lineCount(const std::u16string& text) {
    if (text.empty()) return 0;
    size_t n = 0;
    for (char16_t c : text)
        if (c == u'\n') n++;
    if (text.back() != u'\n') n++;
    return n;
}

std::vector<Row> align(const std::u16string& a, const std::u16string& b, size_t maxEdits) {
    std::vector<Row> rows;
    const size_t na = lineCount(a), nb = lineCount(b);
    // LineDiff counts a line with its "\n"; a last line without one is still
    // a line, and one side ending in "\n" where the other does not would make
    // the last lines differ, so both are compared as if they ended in "\n".
    std::u16string aa = a, bb = b;
    if (!aa.empty() && aa.back() != u'\n') aa += u'\n';
    if (!bb.empty() && bb.back() != u'\n') bb += u'\n';
    std::vector<LineDiff::Hunk> hunks = LineDiff::diff(aa, bb, maxEdits);
    size_t i = 0, j = 0;
    for (const LineDiff::Hunk& h : hunks) {
        while (i < h.oldLine && j < h.newLine) rows.push_back({Kind::Same, (int)i++, (int)j++});
        const size_t pairs = h.oldLines < h.newLines ? h.oldLines : h.newLines;
        for (size_t k = 0; k < pairs; k++) rows.push_back({Kind::Changed, (int)i++, (int)j++});
        for (size_t k = pairs; k < h.oldLines; k++) rows.push_back({Kind::Removed, (int)i++, -1});
        for (size_t k = pairs; k < h.newLines; k++) rows.push_back({Kind::Added, -1, (int)j++});
    }
    while (i < na && j < nb) rows.push_back({Kind::Same, (int)i++, (int)j++});
    // Only when the hunks left lines over (they never should).
    while (i < na) rows.push_back({Kind::Removed, (int)i++, -1});
    while (j < nb) rows.push_back({Kind::Added, -1, (int)j++});
    return rows;
}

std::vector<size_t> changeStarts(const std::vector<Row>& rows) {
    std::vector<size_t> out;
    for (size_t r = 0; r < rows.size(); r++)
        if (rows[r].kind != Kind::Same && (r == 0 || rows[r - 1].kind == Kind::Same))
            out.push_back(r);
    return out;
}

}  // namespace SideBySide
