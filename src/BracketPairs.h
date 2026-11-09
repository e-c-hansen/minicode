// BracketPairs.h — pure C++. Bracket-pair colors, as VS Code's
// editor.bracketPairColorization: every bracket in code, ( ) [ ] { }, is
// colored by how deeply it is nested, in a cycle of three colors, and a
// closer with nothing open before it is "unexpected" and gets a color of
// its own. Brackets in strings, escapes and comments (the highlighter's
// tokens) are not brackets; nor is anything on a line too long to lex.
//
// All kinds count together, as VS Code's default does (nestingLevel, not
// independentColorPoolPerBracketType): ([)] colors as if it were ([]).
// VS Code also calls a closer of the wrong kind unexpected; this does not.
//
// The depth is not in the lexer's state, which would re-lex every line
// below a typed bracket. Each line instead has a summary of what it does
// to the depth (LineSum), and Depths keeps those in blocks of lines with a
// summary per block, so the depth at a line's start costs a pass over the
// blocks before it and the lines before it in its block, and an edit
// changes only its own lines' entries (and their block's summary).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "SyntaxHighlighter.h"

namespace BracketPairs {

// What a line does to the depth: walked from depth D, an opener adds one, a
// closer takes one, and a closer at depth 0 (unexpected) leaves it at 0. The
// depth after it is max(D + net, floor): net is what the line adds
// overall, floor what it leaves when it starts at 0 (floor >= max(net, 0)).
// Lines compose (then): that is the whole of the bookkeeping.
struct LineSum {
    int32_t net = 0, floor = 0;
    int32_t after(int32_t depth) const { return depth + net > floor ? depth + net : floor; }
    // This line, then `next`.
    LineSum then(const LineSum& next) const;
    bool operator==(const LineSum& o) const { return net == o.net && floor == o.floor; }
    bool operator!=(const LineSum& o) const { return !(*this == o); }
};

// A bracket to color: its offset (as the line's text was given, plus base),
// and its depth (0 for the outermost pair), or -1 for an unexpected closer.
struct Bracket {
    size_t at;
    int32_t depth;
};

// The color a bracket of `depth` takes: 0, 1 or 2 in turn (VS Code's
// editorBracketHighlight.foreground1 to 3), or 3 for an unexpected closer.
inline int colorIndex(int32_t depth) { return depth < 0 ? 3 : (int)(depth % 3); }

// One line's text s[0, n), which starts at offset `base` of the document,
// and its tokens (absolute offsets, any order; only strings, escapes and
// comments matter). summarize gives the line's LineSum; color appends its
// brackets to *out, walking from `depth`, and returns the depth after it.
LineSum summarize(const char16_t* s, size_t n, size_t base, const Token* tokens,
                  size_t count);
int32_t color(const char16_t* s, size_t n, size_t base, const Token* tokens, size_t count,
              int32_t depth, std::vector<Bracket>* out);

// The same over a document the highlighter keeps: the sums of lines
// [first, end) of src, from those lines' tokens in the order the
// highlighter gives them (reset, edit or lineTokens: line by line). A line
// too long to lex sums to nothing. colorLines appends the brackets of
// lines [first, end), starting at `depth`, and returns the depth after.
std::vector<LineSum> summarizeLines(const TextSource<char16_t>& src,
                                    const IncrementalHighlighter<char16_t>& hl, size_t first,
                                    size_t end, const std::vector<Token>& tokens);
int32_t colorLines(const TextSource<char16_t>& src, const IncrementalHighlighter<char16_t>& hl,
                   size_t first, size_t end, const std::vector<Token>& tokens, int32_t depth,
                   std::vector<Bracket>* out);

// Every line's LineSum, in blocks.
class Depths {
public:
    Depths();
    // Start over with these lines.
    void assign(const std::vector<LineSum>& lines);
    // Lines [first, first + oldCount) became `now` (any count). Returns
    // whether the lines after them start at different depths than before
    // (what the replaced lines did to the depth changed).
    bool replace(size_t first, size_t oldCount, const std::vector<LineSum>& now);
    // The depth at the start of `line` (lineCount() for the end).
    int32_t depthAt(size_t line) const;
    size_t lineCount() const { return count_; }
    LineSum line(size_t i) const;

private:
    struct Block {
        std::vector<LineSum> lines;
        LineSum sum;
    };
    void resum(Block& b);
    // The block holding line i (the last block for i == count_), and the
    // line its first one is.
    size_t blockOf(size_t i, size_t* firstLine) const;
    std::vector<Block> blocks_;
    size_t count_ = 0;
};

// Which characters' bracket colors are current (the Mac keeps them as
// ascending, disjoint [start, end) spans) follows the text's edits. Moving
// the spans at every edit is quadratic in a multi-range edit: its edits come
// last first (CodeTextView replaceRanges:), each leaves a hole, and every
// later edit moved all the holes after it again (0.44 s of the main thread
// for a letter typed at 10,000 carets). Edits instead gather here, each
// wholly before or wholly after those already here, as such an edit's are
// (descending, or ascending as an undo may give them), kept in the
// coordinates of the text before the first; apply then maps the spans
// through all of them in one pass.
class PaintedEdits {
public:
    // [at, at + oldLen) of the text as it is now became newLen characters.
    // `cut`: nothing from `at` on stays current (the edit changed the depth
    // the lines after it start at). Returns false, recording nothing, when
    // the edit overlaps or falls between those gathered: apply them first,
    // then add it again.
    bool add(size_t at, size_t oldLen, size_t newLen, bool cut);
    bool empty() const { return front_.empty() && back_.empty(); }
    void clear();
    // `spans` (ascending, disjoint [start, end) of the text before the
    // edits) as they are after them: an edited character is no longer
    // current, every other one moves with its text, and nothing at or after
    // a cut is. Adjacent results are joined. Linear in spans and edits.
    std::vector<std::pair<size_t, size_t>> apply(
        const std::vector<std::pair<size_t, size_t>>& spans) const;

private:
    struct Edit {
        size_t at, oldLen, newLen;   // `at` in the text before the first edit
    };
    std::vector<Edit> front_;   // each before the last (descending)
    std::vector<Edit> back_;    // each after the last (ascending)
    size_t low_ = 0;            // where the lowest edit starts (either text)
    size_t highEnd_ = 0;        // where the highest one's new text ends, now
    long long delta_ = 0;       // the change in length so far
    size_t cut_ = SIZE_MAX;     // nothing from here (the old text) on is current
};

}  // namespace BracketPairs
