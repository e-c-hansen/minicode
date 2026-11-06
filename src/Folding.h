// Folding.h — pure C++. Code folding for the editor: where a file's foldable
// regions are, and which of them are folded while the text is edited.
//
// A region is a pair of 0-based lines, start < end: folded, the start line
// stays in view and lines start+1 to end are hidden. Regions come from the
// language server's textDocument/foldingRange when it offers one (lines
// the same way, as LSP's lineFoldingOnly asks), else from `compute`:
//   brackets      C, C++, Java, JS, TS, Go, Rust, JSON, CSS and the rest of
//                 AutoIndent's C-like languages: ( [ { opened on one line and
//                 closed on a later one, the closer's line staying in view
//                 (`} else {` closes one region and opens the next); brackets
//                 in strings and comments, from the highlighter's tokens, do
//                 not count. Block comments over several lines fold whole.
//   indentation   Python, YAML and every other language: a line followed by
//                 more deeply indented lines, up to the last of them (blank
//                 lines inside belong to it, blank lines after it do not),
//                 as VS Code folds by indentation. Lines that start inside a
//                 string or comment opened on an earlier line (a docstring)
//                 neither open nor close a region.
//   Markdown      a heading down to the next heading of its level or above;
//                 a fenced code block.
//   LaTeX         \part to \subparagraph down to the next one of the same
//                 level or above (or \end{document}); \begin{x} to its
//                 \end{x}, the \end line staying in view.
// Everywhere, comment lines naming a region fold from the start marker to
// the end marker: `#region` / `#endregion` after any line comment (`// `,
// `# `, `-- `, `% `, `; `), `#pragma region`, `<!-- #region -->`, and
// `// region` / `// endregion` as Java and Kotlin write them.
//
// Regions are nested properly (`sanitize`): a range that starts inside
// another and ends after it is dropped, as is a second one starting on the
// same line.
//
// Folded regions (Collapsed) are kept as UTF-16 offsets into the text, so
// they follow edits without counting lines: a fold hides [start, end), from
// the line break that ends its first line to the line break that ends its
// last line (or the text's end). An edit before or after a fold moves it; an
// edit that touches what it hides opens it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Fold {

enum class Kind : uint8_t { Block, Comment, Region, Heading, Section, Environment, Imports };

struct Range {
    size_t start = 0, end = 0;   // lines; start < end
    Kind kind = Kind::Block;
    bool operator==(const Range& o) const {
        return start == o.start && end == o.end && kind == o.kind;
    }
};

enum class Method { Indentation, Brackets, Markdown, Latex };
Method methodFor(const std::string& filename);

// The regions of a text, by the file's name (a path is fine). Sanitized.
std::vector<Range> compute(const char16_t* text, size_t length, const std::string& filename);
inline std::vector<Range> compute(const std::u16string& t, const std::string& filename) {
    return compute(t.data(), t.size(), filename);
}

// Ranges from anywhere (a language server) made fit to use: inside the
// text's `lineCount` lines, start < end, sorted by start (outer first), one
// per start line (the first one given), properly nested.
std::vector<Range> sanitize(std::vector<Range> ranges, size_t lineCount);

// LSP's FoldingRangeKind ("comment", "imports", "region"); Block otherwise.
Kind kindFromLsp(const std::string& kind);

// How deep each of a sanitized list's ranges is: 1 for one inside no other.
std::vector<int> depths(const std::vector<Range>& ranges);

// The ranges holding `line` (start <= line <= end), innermost first, as
// indexes into a sanitized list.
std::vector<size_t> containing(const std::vector<Range>& ranges, size_t line);

// The range starting on `line`, as an index, or -1.
long startingAt(const std::vector<Range>& ranges, size_t line);

// Where a fold of lines [startLine, endLine] hides text, given each line's
// start and end (the end including its line break), and the text: from its
// first line's line break ("\r\n" counts from the \r) to its last line's.
struct Span {
    size_t start = 0, end = 0;   // UTF-16 offsets: [start, end) is hidden
    bool operator==(const Span& o) const { return start == o.start && end == o.end; }
};
// lineEnd is a line's end without its line break.
Span spanOf(size_t firstLineContentEnd, size_t lastLineContentEnd);
// The end of a line's content: before "\n", "\r\n", "\r" or U+2029 (lineEnd is where the
// next line starts, or the text's length).
size_t contentEnd(const char16_t* text, size_t lineStart, size_t lineEnd);

// The folds that are closed, following edits.
class Collapsed {
public:
    const std::vector<Span>& all() const { return folds_; }   // sorted by start
    bool empty() const { return folds_.empty(); }
    size_t size() const { return folds_.size(); }
    void clear() { folds_.clear(); }
    // Adds a fold; false when it is empty or already there.
    bool add(Span s);
    // Adds many at once (Fold All on a long file), in O(n log n); returns
    // how many were new. A span replaces a fold starting where it starts.
    size_t addAll(const std::vector<Span>& spans);
    // Removes a fold that starts at `start`; false when there is none.
    bool remove(size_t start);
    bool contains(Span s) const;

    // The units [pos, pos + oldLen) were replaced by the newLen units of
    // `inserted`, and `following` is the unit after them in the new text (0
    // at its end). A fold the edit does not touch moves with the text; one
    // it touches opens (is removed), and where it was, in the new text's
    // offsets, is appended to *opened. Touching is replacing any hidden
    // unit, inserting strictly inside the hidden part, putting a line break
    // where it starts (at the end of its first line), or an edit at its end
    // that leaves anything but a line break there (typing after the
    // placeholder, or joining the next line onto the hidden one; Return
    // there makes a line below the fold, and undoing that leaves it closed).
    void edit(size_t pos, size_t oldLen, const char16_t* inserted, size_t newLen,
              char16_t following, std::vector<Span>* opened = nullptr);

    // The folds inside no other fold: what the view shows as placeholders.
    // Their spans together are everything hidden.
    std::vector<Span> outermost() const;
    // The outermost fold hiding offset pos (start <= pos < end), or null.
    const Span* hiding(size_t pos) const;
    // Every fold whose hidden part holds a position strictly inside it
    // (start < pos < end), outermost first: those a caret there needs open.
    std::vector<Span> around(size_t pos) const;

private:
    std::vector<Span> folds_;
};

}  // namespace Fold
