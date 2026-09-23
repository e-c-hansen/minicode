// PageWords.h — the word under a point on a PDF page, from the page's text and
// one box per character. Pure C++17, no GTK and no poppler, so it is tested
// in linux/tests/run_tests.cpp and shared by the preview and the sweep.
//
// This is the Linux form of what the Mac gets from PDFKit's
// selectionForWordAtPoint: plus extendSelectionAtStart:/AtEnd: (src/Latex.mm),
// and Android from PdfRenderer's selection (LatexPreview.kt). poppler gives
// the page as text (poppler_page_get_text) and a rectangle for every character
// of it, newlines included (poppler_page_get_text_layout); a word is a run of
// the characters LatexDoc::matchKey keeps (letters and digits, in any script),
// with an apostrophe allowed between two of them.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct PageBox {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // points, from the page's top-left
};

// What a double-click at a point found.
struct PageClick {
    enum class Kind {
        Nothing,   // no text under the point: refuse, never guess
        Word,      // `word` is the word there; before/after its context
        Line,      // a glyph that is not a word (a bullet): `word` is its line
    };
    Kind kind = Kind::Nothing;
    std::string word, before, after;   // UTF-8
    std::size_t start = 0, end = 0;    // the word's character range
};

class PageText {
public:
    // `utf8` is the page's text and `boxes` one box per character of it (not
    // per byte). If the counts disagree, the extra boxes or characters are
    // ignored.
    PageText(const std::string& utf8, std::vector<PageBox> boxes);

    std::size_t size() const { return chars_.size(); }
    const PageBox& box(std::size_t i) const { return boxes_[i]; }
    bool isWordChar(std::size_t i) const;

    // The character whose box holds the point, or npos. Where boxes overlap
    // the smallest wins, since that is the most specific glyph.
    std::size_t charAt(double x, double y) const;
    // The word holding character i, as a character range [start, end); an
    // empty range when i is not part of a word.
    void wordRange(std::size_t i, std::size_t* start, std::size_t* end) const;
    // Every word on the page, in reading order (the sweep clicks them all).
    std::vector<std::pair<std::size_t, std::size_t>> words() const;
    std::string slice(std::size_t start, std::size_t end) const;

    // The whole double-click: the word under the point with 40 characters of
    // context each way, a word TeX hyphenated at a line end joined back
    // together, or the glyph's line when it is not a word.
    PageClick clickAt(double x, double y) const;

    static constexpr std::size_t kContext = 40;

private:
    std::vector<char32_t> chars_;
    std::vector<PageBox> boxes_;
    std::vector<unsigned char> word_;   // 1 when chars_[i] is a letter or digit
};

// The whole click-to-source path, as MCLatexSpanAtPoint is on the Mac:
// SyncTeX's candidate lines for the point (`tag` is the typeset file's input,
// 0 for any; `pageNumber` is 1-based), the word the page shows there, and
// LatexDoc's choice of span. Null when it refuses, including when there is no
// text under the point at all. `hits` and `click` report what was used.
struct LatexDoc;
struct LatexSpan;
struct SyncTexHit;
class SyncTexIndex;
const LatexSpan* latexSpanAtPoint(const LatexDoc& doc, const SyncTexIndex& sync,
                                  int tag, const PageText& page, int pageNumber,
                                  double x, double y,
                                  std::vector<SyncTexHit>* hits = nullptr,
                                  PageClick* click = nullptr);

// "counterrevolu-" + "\n" + "tionaries": TeX split a word at a line end, and
// the page holds two. When the word sits against a line-end hyphen, the other
// half is taken from the context (and out of it). The same rule as
// MCJoinHyphenation on the Mac and PageText.joinHyphenation on Android.
// Strings are UTF-8.
void joinHyphenation(std::string* word, std::string* before, std::string* after);
