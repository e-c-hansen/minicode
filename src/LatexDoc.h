// LatexDoc.h — pure C++. A structural (not typesetting) reader of LaTeX source.
//
// This is deliberately not a renderer: tectonic renders the document, and this
// only answers "which bytes of the source produced this piece of text?" so the
// preview can edit a field in place. Every span it reports is a byte range in
// the original source, and every edit it performs replaces exactly one range.
// Anything it does not understand is simply not reported, and so is never
// touched.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

enum class LatexSpanKind {
    Field,   // the braced argument of a structural command (\title, \section)
    Text,    // a run of ordinary text in the document body
    Math,    // the inside of $...$, \[...\] or an equation environment
};

struct LatexSpan {
    LatexSpanKind kind = LatexSpanKind::Text;
    std::string command;      // "title", "section", ... ; empty for Text/Math
    std::size_t start = 0;    // byte range of the editable source
    std::size_t end   = 0;
    int line = 1;             // 1-based line of `start`
    std::string display;      // text as it roughly appears, for matching
    int listIndex = -1;       // index into LatexDoc::lists, or -1
    int itemIndex = -1;       // index of the \item this span sits in, or -1
};

struct LatexList {
    std::string environment;            // itemize, enumerate, description
    std::size_t start = 0;              // offset of \begin
    std::size_t end   = 0;              // offset just past \end{...}
    std::vector<std::size_t> itemBodyStart;  // first byte of each item's body
    std::vector<std::size_t> itemBodyEnd;    // one past its last byte
    std::string indent;                 // leading whitespace to reuse
    int line = 1;
};

struct LatexDoc {
    std::vector<LatexSpan> spans;
    std::vector<LatexList> lists;

    static LatexDoc parse(const std::string &src);

    // Spans whose first line is `line`, nearest-first by source order.
    std::vector<const LatexSpan *> spansOnLine(int line) const;

    // Which span did a click land on? `lines` are the candidate source lines
    // SyncTeX offered, best first, and `word` is the word under the pointer.
    // TeX reports the line where a paragraph *closes*, which is often one or
    // two lines past the text itself, so nearby lines are searched too and the
    // word decides between them.
    const LatexSpan *spanForClick(const std::vector<int> &lines,
                                  const std::string &word) const;

    // ------------------------------------------------------------- editing
    // Both return the complete new source; the caller keeps the old one for
    // undo. `newRange` receives the range the replacement text now occupies.
    struct Edit {
        std::string source;
        std::size_t start = 0;
        std::size_t end   = 0;
    };
    static Edit replaceSpan(const std::string &src, const LatexSpan &span,
                            const std::string &replacement);
    // Insert "\item <text>" after item `afterItem` of `list` (-1 = at the end).
    static Edit addItem(const std::string &src, const LatexList &list,
                        int afterItem, const std::string &text);

    static int lineAt(const std::string &src, std::size_t offset);
    // Collapse whitespace and undo the common escapes, for text matching.
    static std::string displayText(const std::string &tex);
};
