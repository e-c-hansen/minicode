// PreviewScroll.h — pure C++. Scrolling a rendered Markdown page and its
// source together, as VS Code's preview does with
// markdown.preview.scrollPreviewWithEditor and scrollEditorWithPreview (both
// on by default there): the source's top line, a fraction of a line
// included, puts the page's text from that line at the page's top, and the
// page's top puts that line at the source's top.
//
// The page is known by its marks: where the first rendered text of each
// source line is (every run carries its line, kMarkdownSourceLine on the
// Mac). VS Code's preview knows a block per element instead
// (preview-src/scroll-sync.ts); between two marks both directions here
// interpolate from one mark's top to the next one's, which is VS Code's
// page-to-editor rule (getEditorLineNumberForPageOffset) and, for blocks
// with no space between them, its editor-to-page rule too. Using the one
// rule both ways makes the two maps each other's inverse, so a page that
// moved the source is where the source would move it, and the guard
// against the two feeding back never has to absorb a drift.
//
// Before the first mark the map runs from line 0 at the page's top; after
// the last it runs to the page's bottom over the source lines left.
#pragma once
#include <vector>

namespace PreviewScroll {

// The first rendered text of source line `line` (0-based), at `top`
// points down the page.
struct Mark {
    int line;
    double top;
    long at = -1;   // the caller's: where the text is (the page's character)
};

// The page's marks from its runs in page order: the first run of each
// line, keeping lines growing and tops never going back up (a heading's
// runs all carry its line, a table's cells the header's, and a line that
// renders above an earlier one would make the map fold over), so that
// both directions can be searched.
std::vector<Mark> marks(const std::vector<Mark>& runs);

struct Page {
    std::vector<Mark> marks;   // from marks()
    double bottom = 0;         // where the page's text ends
    int lines = 0;             // the source's line count
};

// Where on the page source line `line` (a fraction of a line included)
// is: the y to put at the page's top.
double yForLine(const Page& page, double line);

// The source line, with its fraction, shown at page y.
double lineForY(const Page& page, double y);

// The mark whose text holds source line `line`: the last at or before it
// (VS Code's ActiveLineMarker takes the same element), -1 when the line
// comes before every mark.
int markAt(const Page& page, double line);

// The two marks a question about mark `i`'s stretch needs: mark i and the
// one after it (mark 0 alone for i < 0), and the page's bottom and line
// count when there is no mark after it. yForLine for a line from mark i's
// to the next one's, and lineForY for a y from mark i's top to the next
// one's, answer from it as from the whole page, so a long page needs the
// tops of only the marks asked about, and its layout made only that far.
// A top above the one before it is taken as that one.
Page around(const std::vector<Mark>& marks, int i, double bottom, int lines);

}  // namespace PreviewScroll
