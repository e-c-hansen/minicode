// SyntaxHighlighter.cpp — hand-rolled lexer. Pure C++ standard library only.
//
// The lexer runs one line at a time (lexLine). A line is its text up to and
// including its '\n'; the last line has none. Nothing in a line's lexing looks
// past its own '\n', so a line's tokens depend only on its text and the state
// the line above ended in. SyntaxHighlighter::highlight lexes every line and
// joins the pieces of tokens that cross lines; IncrementalHighlighter keeps
// the per-line states and re-lexes only what an edit can have changed.
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <cctype>
#include <type_traits>
#include <unordered_set>

struct SyntaxGrammar {
    std::vector<std::string> lineComments;   // e.g. {"//"} or {"#"}
    std::string blockStart;                  // e.g. "/*"  ("" = none)
    std::string blockEnd;                    // e.g. "*/"
    std::string stringDelims;                // characters that open strings
    bool tripleQuotes = false;               // Python-style """ / '''
    bool preprocHash = false;                // '#' at line start is preprocessor (C)
    bool decorators = false;                 // '@' at line start is a decorator (Python)
    std::unordered_set<std::string> keywords;
    std::unordered_set<std::string> types;
};

namespace {

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

SyntaxGrammar makeGrammar(const std::string& ext) {
    SyntaxGrammar d;
    if (ext == "py") {
        d.lineComments = {"#"}; d.stringDelims = "\"'"; d.tripleQuotes = true;
        d.decorators = true;
        d.keywords = kPy; d.types = kPyTypes;
    } else if (ext == "js" || ext == "ts" || ext == "jsx" || ext == "tsx" ||
               ext == "json") {
        d.lineComments = {"//"}; d.blockStart = "/*"; d.blockEnd = "*/";
        d.stringDelims = "\"'`"; d.keywords = kJs; d.types = kCTypes;
    } else if (ext == "sh" || ext == "bash" || ext == "zsh" || ext == "yml" ||
               ext == "yaml" || ext == "toml" || ext == "conf") {
        d.lineComments = {"#"}; d.stringDelims = "\"'";
    } else { // c, cpp, cc, h, hpp, m, mm, java, go, rs, ...
        d.lineComments = {"//"}; d.blockStart = "/*"; d.blockEnd = "*/";
        d.stringDelims = "\"'"; d.preprocHash = true;
        d.keywords = kCLike; d.types = kCTypes;
    }
    return d;
}

// Character classes, ASCII only: a byte of a UTF-8 sequence or a non-ASCII
// UTF-16 unit is never a letter or digit here, which is what the C locale's
// isalpha said of those bytes all along.
template <class Ch> inline unsigned code(Ch c) {
    return (unsigned)(typename std::make_unsigned<Ch>::type)c;
}
template <class Ch> inline bool isAlpha(Ch c) { unsigned u = code(c); return u < 128 && std::isalpha((int)u); }
template <class Ch> inline bool isDigit(Ch c) { unsigned u = code(c); return u < 128 && std::isdigit((int)u); }
template <class Ch> inline bool isAlnum(Ch c) { unsigned u = code(c); return u < 128 && std::isalnum((int)u); }
template <class Ch> inline bool isIdentStart(Ch c) { return isAlpha(c) || c == Ch('_'); }
template <class Ch> inline bool isIdentChar(Ch c)  { return isAlnum(c) || c == Ch('_'); }

template <class Ch>
bool matchesAt(const Ch* s, size_t n, size_t i, const std::string& tok) {
    if (tok.empty() || i + tok.size() > n) return false;
    for (size_t k = 0; k < tok.size(); ++k)
        if (code(s[i + k]) != (unsigned char)tok[k]) return false;
    return true;
}

template <class Ch>
bool isDelim(const SyntaxGrammar& d, Ch c) {
    unsigned u = code(c);
    return u < 128 && u != 0 && d.stringDelims.find((char)u) != std::string::npos;
}

// Scan a single-quoted string body from i: up to the closing quote or the end
// of the line. A backslash skips the next unit, so an escaped newline carries
// the string onto the next line. Returns the end; *continues says whether it
// ran on past this line.
template <class Ch>
size_t scanString(const Ch* s, size_t n, size_t i, Ch q, bool* continues) {
    while (i < n && s[i] != q && s[i] != Ch('\n')) {
        if (s[i] == Ch('\\') && i + 1 < n) i++;   // skip escape
        i++;
    }
    // Stopping at n with a '\n' last means the escape swallowed it.
    *continues = i >= n && n > 0 && s[n - 1] == Ch('\n');
    if (i < n && s[i] == q) i++;
    return i;
}

// Scan for the end of a triple-quoted string from i. Returns the end, or n if
// the string is still open at the end of the line.
template <class Ch>
size_t scanTriple(const Ch* s, size_t n, size_t i, Ch q, bool* closed) {
    while (i < n && !(s[i] == q && i + 2 < n && s[i+1] == q && s[i+2] == q)) i++;
    *closed = i < n;
    return *closed ? i + 3 : n;
}

// Lex one line: s[0, n), ending in '\n' unless it is the last line. `in` is
// the state the line above ended in. Tokens go to out at offset `base`.
// Returns the state this line ends in.
template <class Ch>
LexState lexLine(const SyntaxGrammar& d, const Ch* s, size_t n, LexState in,
                 size_t base, std::vector<Token>& out, std::string& word) {
    size_t i = 0;
    bool atLineStart = true;
    auto push = [&](size_t start, size_t len, TokenStyle st) {
        if (len) out.push_back({base + start, len, st});
    };

    // Finish whatever the line above left open.
    switch (in.kind) {
    case LexState::BlockComment: {
        while (i < n && !matchesAt(s, n, i, d.blockEnd)) i++;
        if (i >= n) { push(0, n, TokenStyle::Comment); return in; }
        i += d.blockEnd.size();
        push(0, i, TokenStyle::Comment);
        atLineStart = false;
        break;
    }
    case LexState::TripleString: {
        bool closed;
        i = scanTriple(s, n, 0, (Ch)in.quote, &closed);
        push(0, i, TokenStyle::String);
        if (!closed) return in;
        atLineStart = false;
        break;
    }
    case LexState::StringCont: {
        bool continues;
        i = scanString(s, n, 0, (Ch)in.quote, &continues);
        push(0, i, TokenStyle::String);
        if (continues) return in;
        atLineStart = false;
        break;
    }
    default:
        break;
    }

    while (i < n) {
        Ch c = s[i];

        if (c == Ch('\n')) { i++; atLineStart = true; continue; }
        if (c == Ch(' ') || c == Ch('\t') || c == Ch('\r')) { i++; continue; }

        // Preprocessor / decorators at line start.
        if (atLineStart && ((d.preprocHash && c == Ch('#')) ||
                            (d.decorators && c == Ch('@')))) {
            size_t st = i;
            while (i < n && s[i] != Ch('\n')) i++;
            push(st, i - st, TokenStyle::Preprocessor);
            atLineStart = false;
            continue;
        }

        // Line comments.
        bool handled = false;
        for (const auto& lc : d.lineComments) {
            if (matchesAt(s, n, i, lc)) {
                size_t st = i;
                while (i < n && s[i] != Ch('\n')) i++;
                push(st, i - st, TokenStyle::Comment);
                handled = true; break;
            }
        }
        if (handled) { atLineStart = false; continue; }

        // Block comments.
        if (matchesAt(s, n, i, d.blockStart)) {
            size_t st = i;
            i += d.blockStart.size();
            while (i < n && !matchesAt(s, n, i, d.blockEnd)) i++;
            if (i >= n) {
                push(st, n - st, TokenStyle::Comment);
                return LexState{LexState::BlockComment, 0};
            }
            i += d.blockEnd.size();
            push(st, i - st, TokenStyle::Comment);
            atLineStart = false;
            continue;
        }

        // Strings.
        if (isDelim(d, c)) {
            size_t st = i;
            Ch q = c;
            if (d.tripleQuotes && i + 2 < n && s[i+1] == q && s[i+2] == q) {
                bool closed;
                i = scanTriple(s, n, i + 3, q, &closed);
                push(st, i - st, TokenStyle::String);
                if (!closed) return LexState{LexState::TripleString, (char16_t)q};
            } else {
                bool continues;
                i = scanString(s, n, i + 1, q, &continues);
                push(st, i - st, TokenStyle::String);
                if (continues) return LexState{LexState::StringCont, (char16_t)q};
            }
            atLineStart = false;
            continue;
        }

        // Numbers.
        if (isDigit(c) || (c == Ch('.') && i + 1 < n && isDigit(s[i+1]))) {
            size_t st = i;
            while (i < n && (isAlnum(s[i]) || s[i] == Ch('.') ||
                             s[i] == Ch('x') || s[i] == Ch('X'))) i++;
            push(st, i - st, TokenStyle::Number);
            atLineStart = false;
            continue;
        }

        // Identifiers / keywords.
        if (isIdentStart(c)) {
            size_t st = i;
            while (i < n && isIdentChar(s[i])) i++;
            word.assign(i - st, ' ');
            for (size_t k = st; k < i; ++k) word[k - st] = (char)code(s[k]);
            TokenStyle sty = TokenStyle::Plain;
            if (d.keywords.count(word)) sty = TokenStyle::Keyword;
            else if (d.types.count(word)) sty = TokenStyle::Type;
            else if (std::isupper((unsigned char)word[0]))
                sty = TokenStyle::Type;
            else {
                size_t j = i;
                while (j < n && (s[j] == Ch(' ') || s[j] == Ch('\t'))) j++;
                if (j < n && s[j] == Ch('(')) sty = TokenStyle::Function;
            }
            if (sty != TokenStyle::Plain) push(st, i - st, sty);
            atLineStart = false;
            continue;
        }

        i++;
        atLineStart = false;
    }
    return LexState{};
}

} // namespace

bool SyntaxHighlighter::supports(const std::string& ext) {
    static const std::unordered_set<std::string> known = {
        "py","js","ts","jsx","tsx","json","c","cpp","cc","cxx","h","hpp","hxx",
        "m","mm","java","go","rs","sh","bash","zsh","yml","yaml","toml","conf"};
    return known.count(ext) > 0;
}

std::vector<Token> SyntaxHighlighter::highlight(const std::string& text,
                                                const std::string& ext) {
    std::vector<Token> out;
    SyntaxGrammar d = makeGrammar(ext);
    std::string word;
    LexState st;
    size_t ls = 0;
    const size_t n = text.size();
    while (true) {
        size_t nl = text.find('\n', ls);
        size_t le = nl == std::string::npos ? n : nl + 1;
        size_t before = out.size();
        LexState in = st;
        st = lexLine(d, text.data() + ls, le - ls, in, ls, out, word);
        // A line that starts inside a token continues the previous line's
        // last piece: join them, so a multi-line comment is one token.
        if (in.kind != LexState::Normal && out.size() > before && before > 0 &&
            out[before].start == ls &&
            out[before - 1].start + out[before - 1].length == ls) {
            out[before - 1].length += out[before].length;
            out.erase(out.begin() + before);
        }
        if (le >= n) break;
        ls = le;
    }
    return out;
}

// ------------------------------------------------------ IncrementalHighlighter

template <class Ch>
IncrementalHighlighter<Ch>::IncrementalHighlighter(const std::string& ext)
    : def_(new SyntaxGrammar(makeGrammar(ext))) {
    starts_.assign(1, 0);
    states_.assign(1, LexState{});
}

template <class Ch>
IncrementalHighlighter<Ch>::~IncrementalHighlighter() = default;

template <class Ch>
void IncrementalHighlighter<Ch>::reset(const TextSource<Ch>& src,
                                       std::vector<Token>* out) {
    length_ = src.length();
    std::vector<Ch> text(length_);
    if (length_) src.read(0, length_, text.data());
    starts_.assign(1, 0);
    for (size_t i = 0; i < length_; ++i)
        if (text[i] == Ch('\n')) starts_.push_back(i + 1);
    states_.assign(starts_.size(), LexState{});

    std::vector<Token> scratch;
    std::vector<Token>& sink = out ? *out : scratch;
    std::string word;
    LexState st;
    for (size_t k = 0; k < starts_.size(); ++k) {
        size_t a = starts_[k];
        st = lexLine(*def_, text.data() + a, lineEnd(k) - a, st, a, sink, word);
        states_[k] = st;
        if (!out) scratch.clear();
    }
}

template <class Ch>
size_t IncrementalHighlighter<Ch>::relex(const TextSource<Ch>& src, size_t first,
                                         size_t mustReach, std::vector<Token>& out) {
    std::string word;
    LexState st = first == 0 ? LexState{} : states_[first - 1];
    size_t k = first;
    for (; k < starts_.size(); ++k) {
        size_t a = starts_[k], len = lineEnd(k) - a;
        buf_.resize(len);
        if (len) src.read(a, len, buf_.data());
        st = lexLine(*def_, buf_.data(), len, st, a, out, word);
        bool same = st == states_[k];
        states_[k] = st;
        // Past the edit, a line that ends the way it used to leaves every
        // line below it exactly as it was.
        if (k >= mustReach && same) { ++k; break; }
    }
    if (buf_.capacity() > 1 << 16) std::vector<Ch>().swap(buf_);
    return k;
}

template <class Ch>
void IncrementalHighlighter<Ch>::lineTokens(const TextSource<Ch>& src, size_t first,
                                            size_t end, std::vector<Token>& out) {
    std::string word;
    end = std::min(end, starts_.size());
    for (size_t k = first; k < end; ++k) {
        size_t a = starts_[k], len = lineEnd(k) - a;
        buf_.resize(len);
        if (len) src.read(a, len, buf_.data());
        lexLine(*def_, buf_.data(), len, k == 0 ? LexState{} : states_[k - 1], a, out,
                word);
    }
    if (buf_.capacity() > 1 << 16) std::vector<Ch>().swap(buf_);
}

template <class Ch>
size_t IncrementalHighlighter<Ch>::lineOf(size_t pos) const {
    return (size_t)(std::upper_bound(starts_.begin(), starts_.end(), pos) -
                    starts_.begin()) - 1;
}

template <class Ch>
typename IncrementalHighlighter<Ch>::Range
IncrementalHighlighter<Ch>::edit(const TextSource<Ch>& src, size_t pos,
                                 size_t oldLen, size_t newLen,
                                 std::vector<Token>& out) {
    const size_t total = src.length();
    if (pos > length_ || oldLen > length_ - pos ||
        total != length_ - oldLen + newLen) {
        // The edit does not describe how we got here; start over.
        reset(src, &out);
        Range r;
        r.firstLine = 0; r.endLine = starts_.size();
        r.start = 0; r.end = length_;
        return r;
    }

    // Line a holds the edit's start. Lines whose start lies in
    // (pos, pos + oldLen] lost the '\n' before them and are gone.
    auto firstGone = std::upper_bound(starts_.begin(), starts_.end(), pos);
    auto keep = std::upper_bound(firstGone, starts_.end(), pos + oldLen);
    const size_t a = (size_t)(firstGone - starts_.begin()) - 1;
    const size_t removed = (size_t)(keep - firstGone);
    // The last edited line ends where the last touched old line ended, so it
    // is compared against that line's old end state.
    const LexState tailState = states_[a + removed];

    // Line starts inside the inserted text.
    std::vector<size_t> added;
    if (newLen) {
        std::vector<Ch> ins(newLen);
        src.read(pos, newLen, ins.data());
        for (size_t j = 0; j < newLen; ++j)
            if (ins[j] == Ch('\n')) added.push_back(pos + j + 1);
    }

    // Shift the untouched lines below, then splice the edited ones in.
    for (auto it = keep; it != starts_.end(); ++it) *it = *it - oldLen + newLen;
    size_t gi = (size_t)(firstGone - starts_.begin());
    starts_.erase(starts_.begin() + gi, starts_.begin() + gi + removed);
    starts_.insert(starts_.begin() + gi, added.begin(), added.end());

    std::vector<LexState> fresh(added.size() + 1, LexState{LexState::Unknown, 0});
    fresh.back() = tailState;
    states_.erase(states_.begin() + a, states_.begin() + a + removed + 1);
    states_.insert(states_.begin() + a, fresh.begin(), fresh.end());
    length_ = total;

    Range r;
    r.firstLine = a;
    r.endLine = relex(src, a, a + added.size(), out);
    r.start = starts_[a];
    r.end = lineEnd(r.endLine - 1);
    return r;
}

template class IncrementalHighlighter<char>;
template class IncrementalHighlighter<char16_t>;
