// SyntaxHighlighter.h — pure C++, no dependencies.
// Tokenizes source text into styled ranges. The GUI layer maps
// TokenStyle values onto concrete colors/fonts.
//
// The lexer works a line at a time: every line starts in the state the line
// above ended in (inside a block comment, a triple-quoted string, ...), and
// what it does to a line depends only on that state and the line's own text.
// That is what makes IncrementalHighlighter possible: after an edit, only the
// edited lines are lexed again, plus the lines below them until one ends in
// the same state as before.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class TokenStyle {
    Plain,
    Keyword,      // language keywords: if, def, return, ...
    Type,         // built-in types / capitalized identifiers
    String,       // "..." '...' `...`
    Comment,      // // ... or # ... or /* ... */
    Number,       // 42, 3.14, 0xFF
    Preprocessor, // #include, @decorator
    Function,     // identifier immediately followed by '('
};

struct Token {
    size_t start;   // offset into the source, in code units (bytes for UTF-8)
    size_t length;
    TokenStyle style;
    bool operator==(const Token& o) const {
        return start == o.start && length == o.length && style == o.style;
    }
};

// Where the lexer stands at the end of a line, i.e. what the next line starts
// inside of.
struct LexState {
    enum Kind : uint8_t {
        Normal,
        BlockComment,   // /* ... not closed yet
        TripleString,   // Python """ or ''' not closed yet (quote = the char)
        StringCont,     // "...\ : a backslash escaped the newline (quote = the char)
        TexMath,        // TeX math not closed yet; quote says what closes it:
                        // '$' for $, 'D' for $$, ')' for \), ']' for \]
        TexEnv,         // TeX verbatim, comment or math environment not ended
                        // yet; quote = 1 + its index in the lexer's table
        Unknown,       // never produced by the lexer; marks lines not lexed yet
    };
    uint8_t kind = Normal;
    char16_t quote = 0;
    bool operator==(const LexState& o) const { return kind == o.kind && quote == o.quote; }
    bool operator!=(const LexState& o) const { return !(*this == o); }
};

class SyntaxHighlighter {
public:
    // ext is a lowercase extension without the dot, e.g. "py", "cpp".
    // Byte offsets into the UTF-8 text. A block comment or string that spans
    // lines is one token.
    static std::vector<Token> highlight(const std::string& text,
                                        const std::string& ext);

    // True when we have a real grammar for this extension (vs. plain text).
    static bool supports(const std::string& ext);
};

// Read access to a document by range, so the incremental highlighter never
// keeps its own copy of the text. Ch is char (UTF-8) or char16_t (UTF-16, what
// NSString holds); tokens are in the same units.
template <class Ch>
class TextSource {
public:
    virtual ~TextSource() = default;
    virtual size_t length() const = 0;
    virtual void read(size_t pos, size_t len, Ch* out) const = 0;
};

// A TextSource over a string the caller owns.
template <class Ch>
class StringSource : public TextSource<Ch> {
public:
    explicit StringSource(const std::basic_string<Ch>& s) : s_(s) {}
    size_t length() const override { return s_.size(); }
    void read(size_t pos, size_t len, Ch* out) const override {
        s_.copy(out, len, pos);
    }
private:
    const std::basic_string<Ch>& s_;
};

struct SyntaxGrammar;   // one language's rules, private to SyntaxHighlighter.cpp

// Keeps each line's start offset and end state, and after an edit re-lexes
// only what the edit can have changed. Every line's tokens are exactly the
// tokens a full lex gives, cut at line boundaries: a token spanning lines
// comes out as one piece per line, each piece including its line's '\n'.
template <class Ch>
class IncrementalHighlighter {
public:
    explicit IncrementalHighlighter(const std::string& ext);
    ~IncrementalHighlighter();

    // Lex the whole document. Appends every token to *out if given.
    void reset(const TextSource<Ch>& src, std::vector<Token>* out);

    // What an edit re-lexed: lines [firstLine, endLine), which cover the
    // code units [start, end) of the new text. Everything outside keeps the
    // tokens it had (shifted by the edit's change in length).
    struct Range {
        size_t firstLine = 0, endLine = 0;
        size_t start = 0, end = 0;
    };

    // The code units [pos, pos + oldLen) were replaced by newLen units; src is
    // the text after the edit. Appends the re-lexed lines' tokens to out, in
    // absolute offsets. Several edits can be reported as one, as long as the
    // range covers all of them.
    Range edit(const TextSource<Ch>& src, size_t pos, size_t oldLen,
               size_t newLen, std::vector<Token>& out);

    // The tokens of lines [first, end) as they stand, lexed from the stored
    // states (no state changes). For a caller that applies an edit's colors
    // later than the edit itself, possibly after further edits.
    void lineTokens(const TextSource<Ch>& src, size_t first, size_t end,
                    std::vector<Token>& out);

    // The line holding offset pos (the last line for pos == length()).
    size_t lineOf(size_t pos) const;
    size_t lineCount() const { return starts_.size(); }
    size_t lineStart(size_t line) const { return starts_[line]; }
    LexState endState(size_t line) const { return states_[line]; }
    size_t length() const { return length_; }

private:
    size_t lineEnd(size_t line) const {
        return line + 1 < starts_.size() ? starts_[line + 1] : length_;
    }
    // Lex lines from `first` on, stopping after the first line at or past
    // `mustReach` whose end state did not change.
    size_t relex(const TextSource<Ch>& src, size_t first, size_t mustReach,
                 std::vector<Token>& out);

    std::unique_ptr<SyntaxGrammar> def_;
    std::vector<size_t> starts_;     // offset of each line's first unit
    std::vector<LexState> states_;   // state at the end of each line
    size_t length_ = 0;
    std::vector<Ch> buf_;            // scratch for one line's text
};

extern template class IncrementalHighlighter<char>;
extern template class IncrementalHighlighter<char16_t>;
