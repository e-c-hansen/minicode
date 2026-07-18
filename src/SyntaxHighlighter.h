// SyntaxHighlighter.h — pure C++, no dependencies.
// Tokenizes source text into styled ranges. The GUI layer maps
// TokenStyle values onto concrete colors/fonts.
#pragma once
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
    size_t start;   // byte offset into the source
    size_t length;
    TokenStyle style;
};

class SyntaxHighlighter {
public:
    // ext is a lowercase extension without the dot, e.g. "py", "cpp".
    static std::vector<Token> highlight(const std::string& text,
                                        const std::string& ext);

    // True when we have a real grammar for this extension (vs. plain text).
    static bool supports(const std::string& ext);
};
