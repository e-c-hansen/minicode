// PreviewScroll.cpp — see PreviewScroll.h.
#include "PreviewScroll.h"

#include <algorithm>

namespace PreviewScroll {

std::vector<Mark> marks(const std::vector<Mark>& runs) {
    std::vector<Mark> out;
    for (const Mark& m : runs) {
        if (m.line < 0) continue;
        if (!out.empty() && (m.line <= out.back().line || m.top < out.back().top)) continue;
        out.push_back(m);
    }
    return out;
}

// The last mark at or before `line`, -1 for none.
int markAt(const Page& page, double line) {
    const std::vector<Mark>& ms = page.marks;
    auto it = std::upper_bound(ms.begin(), ms.end(), line,
                               [](double l, const Mark& m) { return l < m.line; });
    return (int)(it - ms.begin()) - 1;
}

// The source line past the last mark's text: the end of the source.
static double endLine(const Page& page, const Mark& last) {
    return std::max((double)page.lines, (double)last.line + 1);
}

double yForLine(const Page& page, double line) {
    const std::vector<Mark>& ms = page.marks;
    if (ms.empty() || line <= 0) return 0;
    const int i = markAt(page, line);
    if (i < 0) {   // before the first mark: from the page's top
        const Mark& first = ms.front();
        return first.line > 0 ? first.top * line / first.line : first.top;
    }
    const Mark& m = ms[(size_t)i];
    if ((size_t)i + 1 < ms.size()) {
        const Mark& next = ms[(size_t)i + 1];
        return m.top + (line - m.line) / (next.line - m.line) * (next.top - m.top);
    }
    const double t = std::min(1.0, (line - m.line) / (endLine(page, m) - m.line));
    return m.top + t * std::max(0.0, page.bottom - m.top);
}

double lineForY(const Page& page, double y) {
    const std::vector<Mark>& ms = page.marks;
    if (ms.empty() || y <= 0) return 0;
    const Mark& first = ms.front();
    if (y < first.top) return first.line * y / first.top;
    // The last mark at or above y.
    auto it = std::upper_bound(ms.begin(), ms.end(), y,
                               [](double v, const Mark& m) { return v < m.top; });
    const size_t i = (size_t)(it - ms.begin()) - 1;
    const Mark& m = ms[i];
    if (i + 1 < ms.size()) {
        const Mark& next = ms[i + 1];   // next.top > y >= m.top
        return m.line + (y - m.top) / (next.top - m.top) * (next.line - m.line);
    }
    if (page.bottom <= m.top) return m.line;
    const double t = std::min(1.0, (y - m.top) / (page.bottom - m.top));
    return m.line + t * (endLine(page, m) - m.line);
}

Page around(const std::vector<Mark>& marks, int i, double bottom, int lines) {
    Page p;
    p.lines = lines;
    p.bottom = bottom;
    if (marks.empty()) return p;
    if (i < 0) {
        p.marks.push_back(marks.front());
        return p;
    }
    const size_t k = std::min((size_t)i, marks.size() - 1);
    p.marks.push_back(marks[k]);
    if (k + 1 < marks.size()) {
        Mark next = marks[k + 1];
        next.top = std::max(next.top, marks[k].top);
        p.marks.push_back(next);
    }
    p.bottom = std::max(p.bottom, p.marks.back().top);
    return p;
}

}  // namespace PreviewScroll
