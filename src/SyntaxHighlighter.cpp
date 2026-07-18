// SyntaxHighlighter.cpp — hand-rolled lexer. Pure C++ standard library only.
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace {

struct LanguageDef {
    std::vector<std::string> lineComments;   // e.g. {"//"} or {"#"}
    std::string blockStart;                  // e.g. "/*"  ("" = none)
    std::string blockEnd;                    // e.g. "*/"
    std::string stringDelims;                // characters that open strings
    bool tripleQuotes = false;               // Python-style """ / '''
    bool preprocHash = false;                // '#' at line start is preprocessor (C)
    std::unordered_set<std::string> keywords;
    std::unordered_set<std::string> types;
};

const std::unordered_set<std::string> kCLike = {
    "if","else","for","while","do","switch","case","default","break","continue",
    "return","goto","struct","class","enum","union","namespace","template",
    "typename","public","private","protected","virtual","override","static",
    "const","constexpr","inline","new","delete","this","using","typedef",
    "sizeof","operator","friend","explicit","volatile","mutable","try","catch",
    "throw","nullptr","true","false","auto","extern","register"};
const std::unordered_set<std::string> kCTypes = {
    "int","char","bool","void","float","double","long","short","unsigned",
    "signed","size_t","wchar_t","string","vector","map","uint8_t","uint32_t",
    "int64_t","std"};

const std::unordered_set<std::string> kPy = {
    "def","class","return","if","elif","else","for","while","break","continue",
    "import","from","as","with","try","except","finally","raise","pass","yield",
    "lambda","global","nonlocal","assert","del","in","is","not","and","or",
    "None","True","False","async","await","self"};
const std::unordered_set<std::string> kPyTypes = {
    "int","str","float","bool","list","dict","set","tuple","bytes","object"};

const std::unordered_set<std::string> kJs = {
    "function","return","if","else","for","while","do","switch","case","break",
    "continue","var","let","const","new","delete","typeof","instanceof","this",
    "class","extends","super","import","export","from","default","try","catch",
    "finally","throw","async","await","yield","null","undefined","true","false",
    "of","in","void"};

LanguageDef makeDef(const std::string& ext) {
    LanguageDef d;
    if (ext == "py") {
        d.lineComments = {"#"}; d.stringDelims = "\"'"; d.tripleQuotes = true;
        d.keywords = kPy; d.types = kPyTypes;
    } else if (ext == "js" || ext == "ts" || ext == "jsx" || ext == "tsx" ||
               ext == "json") {
        d.lineComments = {"//"}; d.blockStart = "/*"; d.blockEnd = "*/";
        d.stringDelims = "\"'`"; d.keywords = kJs; d.types = kCTypes;
    } else if (ext == "sh" || ext == "bash" || ext == "zsh" || ext == "yml" ||
               ext == "yaml" || ext == "toml") {
        d.lineComments = {"#"}; d.stringDelims = "\"'";
    } else { // c, cpp, cc, h, hpp, m, mm, java, go, rs, ...
        d.lineComments = {"//"}; d.blockStart = "/*"; d.blockEnd = "*/";
        d.stringDelims = "\"'"; d.preprocHash = true;
        d.keywords = kCLike; d.types = kCTypes;
    }
    return d;
}

bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdentChar(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }

bool matchesAt(const std::string& s, size_t i, const std::string& tok) {
    return tok.size() && s.compare(i, tok.size(), tok) == 0;
}

} // namespace

bool SyntaxHighlighter::supports(const std::string& ext) {
    static const std::unordered_set<std::string> known = {
        "py","js","ts","jsx","tsx","json","c","cpp","cc","cxx","h","hpp","hxx",
        "m","mm","java","go","rs","sh","bash","zsh","yml","yaml","toml"};
    return known.count(ext) > 0;
}

std::vector<Token> SyntaxHighlighter::highlight(const std::string& text,
                                                const std::string& ext) {
    std::vector<Token> out;
    LanguageDef d = makeDef(ext);
    const size_t n = text.size();
    size_t i = 0;
    bool atLineStart = true;

    auto push = [&](size_t start, size_t len, TokenStyle st) {
        if (len) out.push_back({start, len, st});
    };

    while (i < n) {
        char c = text[i];

        if (c == '\n') { i++; atLineStart = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        // Preprocessor / decorators at line start.
        if (atLineStart && ((d.preprocHash && c == '#') ||
                            (ext == "py" && c == '@'))) {
            size_t s = i;
            while (i < n && text[i] != '\n') i++;
            push(s, i - s, TokenStyle::Preprocessor);
            atLineStart = false;
            continue;
        }

        // Line comments.
        bool handled = false;
        for (const auto& lc : d.lineComments) {
            if (matchesAt(text, i, lc)) {
                size_t s = i;
                while (i < n && text[i] != '\n') i++;
                push(s, i - s, TokenStyle::Comment);
                handled = true; break;
            }
        }
        if (handled) { atLineStart = false; continue; }

        // Block comments.
        if (matchesAt(text, i, d.blockStart)) {
            size_t s = i;
            i += d.blockStart.size();
            while (i < n && !matchesAt(text, i, d.blockEnd)) i++;
            if (i < n) i += d.blockEnd.size();
            push(s, i - s, TokenStyle::Comment);
            atLineStart = false;
            continue;
        }

        // Strings.
        if (d.stringDelims.find(c) != std::string::npos) {
            size_t s = i;
            char q = c;
            // Triple-quoted (Python).
            if (d.tripleQuotes && i + 2 < n && text[i+1] == q && text[i+2] == q) {
                i += 3;
                while (i < n && !(text[i] == q && i + 2 < n &&
                                  text[i+1] == q && text[i+2] == q)) i++;
                if (i < n) i += 3;
            } else {
                i++;
                while (i < n && text[i] != q && text[i] != '\n') {
                    if (text[i] == '\\' && i + 1 < n) i++; // skip escape
                    i++;
                }
                if (i < n && text[i] == q) i++;
            }
            push(s, i - s, TokenStyle::String);
            atLineStart = false;
            continue;
        }

        // Numbers.
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < n && std::isdigit((unsigned char)text[i+1]))) {
            size_t s = i;
            while (i < n && (std::isalnum((unsigned char)text[i]) ||
                             text[i] == '.' || text[i] == 'x' || text[i] == 'X')) i++;
            push(s, i - s, TokenStyle::Number);
            atLineStart = false;
            continue;
        }

        // Identifiers / keywords.
        if (isIdentStart(c)) {
            size_t s = i;
            while (i < n && isIdentChar(text[i])) i++;
            std::string word = text.substr(s, i - s);
            TokenStyle st = TokenStyle::Plain;
            if (d.keywords.count(word)) st = TokenStyle::Keyword;
            else if (d.types.count(word)) st = TokenStyle::Type;
            else if (!word.empty() && std::isupper((unsigned char)word[0]))
                st = TokenStyle::Type;
            else {
                size_t j = i;
                while (j < n && (text[j] == ' ' || text[j] == '\t')) j++;
                if (j < n && text[j] == '(') st = TokenStyle::Function;
            }
            if (st != TokenStyle::Plain) push(s, i - s, st);
            atLineStart = false;
            continue;
        }

        i++;
        atLineStart = false;
    }
    return out;
}
