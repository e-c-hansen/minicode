// Snippet.h — the LSP snippet syntax (a completion item whose
// insertTextFormat is 2, as clangd, rust-analyzer and gopls send for a
// function's arguments) and the tab stops it leaves in the text. Pure C++17,
// no I/O, in UTF-16 code units so offsets map straight onto NSRange.
//
// The grammar is the LSP specification's (VS Code's snippet syntax):
//
//   any         ::= tabstop | placeholder | choice | variable | text
//   tabstop     ::= '$' int | '${' int '}' | '${' int transform '}'
//   placeholder ::= '${' int ':' any* '}'
//   choice      ::= '${' int '|' text (',' text)* '|}'
//   variable    ::= '$' var | '${' var '}' | '${' var ':' any* '}'
//                 | '${' var transform '}'
//   transform   ::= '/' regex '/' (format | text)* '/' options
//   format      ::= '$' int | '${' int '}'
//                 | '${' int ':' ('/upcase' | '/downcase' | '/capitalize'
//                                | '/camelcase' | '/pascalcase') '}'
//                 | '${' int ':+' if '}' | '${' int ':?' if ':' else '}'
//                 | '${' int ':-' else '}' | '${' int ':' else '}'
//
// In text a backslash escapes $, } and \ (in a choice also , and |); before
// anything else it is itself. Whatever does not parse (an unclosed ${, a
// choice without its |}) is text as written, as in VS Code.
//
// Expanding resolves variables (VS Code's list: TM_FILENAME, CURRENT_YEAR,
// UUID and the rest; an unknown one becomes a placeholder holding its name),
// copies each stop's placeholder to its mirrors, indents every line after
// the first as the line the snippet goes on, makes a tab at a line's start
// the file's indentation unit and a line break the file's, and adds the
// final stop at the end when the snippet has no $0.
//
// A Session then follows the stops through edits: Tab and Shift+Tab move
// between them, the stop being typed in grows at its edges (and so do the
// stops around it), others do not, and a stop whose whole text was typed
// over (a nested placeholder) is skipped. The Mac's MCSnippetSession is the
// glue to the text view.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Snippet {

// A regex transform: /regex/format/options.
struct FormatItem {
    enum class Kind { Text, Group, Upcase, Downcase, Capitalize, Camelcase, Pascalcase,
                      Conditional };
    Kind kind = Kind::Text;
    std::string text;               // Text
    int group = 0;                  // the rest
    // Conditional: ${n:+if} (hasIf), ${n:?if:else} (both), ${n:-else} and
    // ${n:else} (hasElse): `if` when the group matched something, else
    // `else`; with no `if`, the group itself.
    std::string ifText, elseText;
    bool hasIf = false, hasElse = false;
};
struct Transform {
    std::string regex;              // as written, \/ unescaped
    std::vector<FormatItem> format;
    std::string options;            // g, i, m (m is accepted and changes nothing)
};

struct Node {
    enum class Kind { Text, Tabstop, Placeholder, Choice, Variable };
    Kind kind = Kind::Text;
    std::string text;               // Text: the text (escapes undone); Variable: its name
    int index = -1;                 // Tabstop, Placeholder, Choice
    std::vector<Node> children;     // Placeholder's contents; a Variable's default
    bool hasDefault = false;        // Variable: ${NAME:...} (children may be empty)
    std::vector<std::string> choices;
    std::shared_ptr<Transform> transform;   // Tabstop or Variable, or null
};

std::vector<Node> parse(const std::string& snippet);

// The text with the syntax taken out and nothing resolved (variables as
// their default, else empty; the first choice): what a completion list
// compares typing with.
std::string plainText(const std::vector<Node>& nodes);

// Applies a transform to a value, as JavaScript's replace would with the
// regex and the format; a regex the linear engine refuses leaves the value.
std::u16string applyTransform(const Transform& t, const std::u16string& value);

// What the variables are.
struct Context {
    std::string filePath;           // TM_FILEPATH; TM_FILENAME, _BASE, TM_DIRECTORY from it
    std::string workspaceFolder;    // WORKSPACE_FOLDER (WORKSPACE_NAME, RELATIVE_FILEPATH)
    std::string selectedText;       // TM_SELECTED_TEXT
    std::string currentLine;        // TM_CURRENT_LINE
    std::string currentWord;        // TM_CURRENT_WORD
    long lineIndex = 0;             // TM_LINE_INDEX (0-based); TM_LINE_NUMBER is one more
    std::string clipboard;          // CLIPBOARD
    std::string lineComment;        // LINE_COMMENT
    std::string blockCommentStart, blockCommentEnd;
    std::tm time{};                 // the CURRENT_* variables, local time
    long timezoneOffsetSeconds = 0; // east of UTC, for CURRENT_TIMEZONE_OFFSET
    int64_t unixSeconds = 0;        // CURRENT_SECONDS_UNIX
    std::function<uint32_t()> random;   // RANDOM, RANDOM_HEX, UUID (std::random_device when unset)

    // Layout of the text the snippet goes into.
    std::u16string indent;          // the line's leading whitespace, after every line break
    std::u16string unit = u"\t";    // what a tab at a line's start becomes
    std::u16string eol = u"\n";     // the file's line break
};

// The value of a variable, or false when it is unknown. An empty value is
// known (VS Code then takes the default, if any).
bool variable(const Context& ctx, const std::string& name, std::string& value);

// The comment markers for LINE_COMMENT and BLOCK_COMMENT_*, by file name.
void commentsFor(const std::string& filename, std::string& line, std::string& blockStart,
                 std::string& blockEnd);

struct Range {
    size_t start = 0, end = 0;
    bool operator==(const Range& o) const { return start == o.start && end == o.end; }
};

struct Stop {
    int index = 0;                  // 0 is the final stop
    // Every place it is, in text order; the placeholder's own first.
    std::vector<Range> ranges;
    // Per range: a transform makes that place a copy of the stop's text,
    // transformed, written when Tab leaves the stop; null is a plain mirror,
    // which gets a caret of its own.
    std::vector<std::shared_ptr<Transform>> transforms;
    std::vector<std::u16string> choices;
};

struct Expansion {
    std::u16string text;
    // In the order Tab visits them: 1, 2, ... (by index), then the final.
    std::vector<Stop> stops;
    // Whether there is anything to visit besides the final stop.
    bool hasStops() const { return stops.size() > 1; }
};

Expansion expand(const std::vector<Node>& nodes, const Context& ctx);
inline Expansion expand(const std::string& snippet, const Context& ctx) {
    return expand(parse(snippet), ctx);
}

// The stops of an expansion put into a text at `base`, followed through
// edits. Offsets are in the text as it is.
class Session {
public:
    Session() = default;
    Session(const Expansion& e, size_t base);

    bool active() const { return active_; }
    void end() { active_ = false; }
    // The stop Tab last went to (an index into stops()).
    size_t current() const { return current_; }
    const std::vector<Stop>& stops() const { return stops_; }
    const Stop& currentStop() const { return stops_[current_]; }
    bool atFinal() const { return stops_[current_].index == 0; }
    // Whether every place of a stop was typed over from around it (Tab
    // skips it): a placeholder nested in one whose text was replaced.
    bool dead(size_t stop) const;
    bool dead(size_t stop, size_t range) const { return dead_[stop][range]; }
    // The whole snippet's span, as it is now.
    Range span() const { return span_; }

    // The places carets go for the current stop (its ranges without a
    // transform).
    std::vector<Range> caretRanges() const;
    // Moves to the next living stop (true), the final one included; false
    // when there is none (the caller ends the session, the caret at the
    // span's end). Moving onto the final stop leaves the session to end
    // once the caret is there.
    bool next();
    // The previous living stop that is not the final one; false (staying)
    // when there is none.
    bool prev();
    // Every selection (lo, hi) within one of the current stop's caret ranges.
    bool holds(const std::vector<Range>& selections) const;

    // [start, start + removed) replaced by `inserted` characters.
    void edited(size_t start, size_t removed, size_t inserted);

    // The transformed copies of the current stop, given the stop's text:
    // replacements (start, length, text) in the text as it is, ascending.
    struct Replacement {
        size_t start = 0, length = 0;
        std::u16string text;
    };
    std::vector<Replacement> transformsFor(const std::u16string& value) const;

private:
    bool sticky(size_t stop, size_t range) const;

    std::vector<Stop> stops_;
    std::vector<std::vector<size_t>> order_;   // each range's place in the expansion's text order
    std::vector<std::vector<bool>> dead_;      // per range
    Range span_;
    size_t current_ = 0;
    bool active_ = false;
};

}  // namespace Snippet
