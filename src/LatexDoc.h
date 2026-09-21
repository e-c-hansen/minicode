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
    int endLine = 1;          // 1-based line of the span's last byte
    std::string display;      // text as it roughly appears, for matching
    // The match keys (see matchKey) of the text just before and after this
    // span, from its neighbouring spans: what the page shows around it.
    std::string lead, trail;
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
    // Runs of spans that the PDF shows as one word, because a font change
    // falls inside the word: foo\emph{bar}baz is three spans but one word on
    // the page. Each join covers the whole chain as a single byte range, and
    // exists only when that range has balanced braces, so it is as safe to
    // replace as any span.
    std::vector<LatexSpan> joins;
    // Lines holding \maketitle: text there comes from \title, \author and
    // \date, which usually sit in the preamble, far from where they appear.
    std::vector<int> titleLines;

    static LatexDoc parse(const std::string &src);

    // Spans whose first line is `line`, nearest-first by source order.
    std::vector<const LatexSpan *> spansOnLine(int line) const;

    // Which span did a click land on? `lines` are the candidate source lines
    // SyncTeX offered, best first, and `word` is the word under the pointer.
    // TeX reports the line where a paragraph *closes*, which is often one or
    // two lines past the text itself, so nearby lines are searched too and the
    // word decides between them. When the word matches nothing, the answer is
    // null: opening the wrong text for editing is worse than opening none.
    //
    // `before` and `after` are the text the page shows on either side of the
    // word, when known. They decide between several nearby spans that all
    // hold a common word: the one whose neighbours read the same wins.
    const LatexSpan *spanForClick(const std::vector<int> &lines,
                                  const std::string &word,
                                  const std::string &before = std::string(),
                                  const std::string &after = std::string()) const;

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
    // The form both sides are compared in: letters and digits only, lowercase,
    // with ligatures split (U+FB03 -> ffi) and accents dropped (é -> e, ß ->
    // ss), so the PDF's "eﬃcient" and "résumé" meet the source's "efficient"
    // and "r\'esum\'e". Letters with no ASCII form (Greek, CJK) are kept.
    static std::string matchKey(const std::string &text);
};
