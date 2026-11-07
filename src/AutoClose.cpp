// AutoClose.cpp — see AutoClose.h.
#include "AutoClose.h"
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace AutoClose {

namespace {

bool isBreak(char16_t c) { return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029; }

bool isQuote(char16_t c) { return c == u'"' || c == u'\'' || c == u'`'; }

// VS Code's default editor.wordSeparators; anything else but a blank is a
// word character.
bool isWordChar(char16_t c) {
    static const std::u16string kSeparators = u"`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?";
    if (c == u' ' || c == u'\t' || isBreak(c)) return false;
    return kSeparators.find(c) == std::u16string::npos;
}

size_t lineStartOf(const Text& t, size_t p) {
    while (p > 0 && !isBreak(t[p - 1])) p--;
    return p;
}

size_t lineEndOf(const Text& t, size_t p) {
    const size_t n = t.size();
    while (p < n && !isBreak(t[p])) p++;
    return p;
}

bool textAt(const Text& t, size_t at, const std::u16string& s) {
    if (at + s.size() > t.size()) return false;
    for (size_t i = 0; i < s.size(); i++)
        if (t[at + i] != s[i]) return false;
    return true;
}

bool startsWith(const std::u16string& s, const std::u16string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::u16string& s, const std::u16string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---- the languages

enum : int { NS = 1, NC = 2, BeforeOpen = 4 };

Pair P(const char16_t* open, const char16_t* close, int flags = 0) {
    Pair p;
    p.open = open;
    p.close = close;
    p.notInString = flags & NS;
    p.notInComment = flags & NC;
    p.scopeBeforeOpen = flags & BeforeOpen;
    return p;
}

const char16_t* const kQuotesBefore = u";:.,=}])> \n\t";
const char16_t* const kBracketsBefore = u"'\"`;:.,=}])> \n\t";

Language L(const char* id, std::vector<Pair> pairs, const char16_t* surround,
           const char16_t* blockComment = u"", const char16_t* before = nullptr) {
    Language l;
    l.id = id;
    l.pairs = std::move(pairs);
    // Surrounding pairs as open-close couples; a closer of \x01 is none
    // (the opener alone goes before the selection).
    for (const char16_t* s = surround; s[0]; s += 2)
        l.surround.push_back({s[0], s[1] == 1 ? char16_t(0) : s[1]});
    l.closeBeforeQuotes = before ? before : kQuotesBefore;
    l.closeBeforeBrackets = before ? before : kBracketsBefore;
    l.blockCommentStart = blockComment;
    return l;
}

std::vector<Pair> BracketPairs() { return {P(u"{", u"}"), P(u"[", u"]"), P(u"(", u")")}; }

std::vector<Pair> With(std::vector<Pair> a, std::vector<Pair> b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

struct Languages {
    Language plain, markdown, latex, python, js, cpp, objc, rust, go, shell, json, yaml, css,
        html, xml, make, sql, docker, ini, properties, dotenv, diff, lisp, code;
    Languages() {
        // VS Code's plaintext: its brackets pair; more characters surround.
        plain = L("plaintext", BracketPairs(), u"{}[]()<>\"\"''``");
        markdown = L("markdown", With(BracketPairs(), {P(u"<", u">", NS)}),
                     u"()[]``__**{}''\"\"<>~~$$", u"<!--");
        latex = L("latex", BracketPairs(), u"{}[]()$$");
        python = L("python",
                   With(BracketPairs(),
                        {P(u"\"", u"\"", NS), P(u"r\"", u"\"", NS | NC), P(u"R\"", u"\"", NS | NC),
                         P(u"u\"", u"\"", NS | NC), P(u"U\"", u"\"", NS | NC),
                         P(u"f\"", u"\"", NS | NC), P(u"F\"", u"\"", NS | NC),
                         P(u"b\"", u"\"", NS | NC), P(u"B\"", u"\"", NS | NC),
                         P(u"'", u"'", NS | NC), P(u"r'", u"'", NS | NC), P(u"R'", u"'", NS | NC),
                         P(u"u'", u"'", NS | NC), P(u"U'", u"'", NS | NC), P(u"f'", u"'", NS | NC),
                         P(u"F'", u"'", NS | NC), P(u"b'", u"'", NS | NC), P(u"B'", u"'", NS | NC),
                         P(u"`", u"`", NS),
                         // Not VS Code's: the triple quote closes.
                         P(u"\"\"\"", u"\"\"\"", NS | NC | BeforeOpen),
                         P(u"'''", u"'''", NS | NC | BeforeOpen)}),
                   u"{}[]()\"\"''``", u"\"\"\"");
        js = L("javascript",
               {P(u"${", u"}"), P(u"{", u"}"), P(u"[", u"]"), P(u"(", u")"),
                P(u"'", u"'", NS | NC), P(u"\"", u"\"", NS), P(u"`", u"`", NS | NC),
                P(u"/**", u" */", NS)},
               u"$\x01{}[]()''\"\"``<>", u"/*", u";:.,=}])>` \n\t");
        cpp = L("cpp",
                With(BracketPairs(), {P(u"'", u"'", NS | NC), P(u"\"", u"\"", NS),
                                  P(u"/*", u"*/", NS | NC), P(u"/**", u" */", NS)}),
                u"{}[]()\"\"''<>", u"/*");
        objc = L("objective-c", With(BracketPairs(), {P(u"\"", u"\"", NS), P(u"'", u"'", NS)}),
                 u"{}[]()\"\"''", u"/*");
        rust = L("rust", With(BracketPairs(), {P(u"\"", u"\"", NS)}), u"{}[]()\"\"<>", u"/*");
        go = L("go",
               With(BracketPairs(), {P(u"`", u"`", NS), P(u"\"", u"\"", NS), P(u"'", u"'", NS | NC)}),
               u"{}[]()\"\"''``", u"/*");
        shell = L("shellscript",
                  With(BracketPairs(), {P(u"\"", u"\"", NS), P(u"'", u"'", NS), P(u"`", u"`", NS)}),
                  u"{}[]()\"\"''``");
        json = L("json",
                 {P(u"{", u"}", NS), P(u"[", u"]", NS), P(u"(", u")", NS), P(u"'", u"'", NS),
                  P(u"\"", u"\"", NS | NC), P(u"`", u"`", NS | NC)},
                 u"", u"/*");
        yaml = L("yaml", With(BracketPairs(), {P(u"\"", u"\""), P(u"'", u"'")}), u"{}[]()\"\"''");
        css = L("css",
                {P(u"{", u"}", NS | NC), P(u"[", u"]", NS | NC), P(u"(", u")", NS | NC),
                 P(u"\"", u"\"", NS | NC), P(u"'", u"'", NS | NC)},
                u"{}[]()\"\"''", u"/*");
        html = L("html",
                 With(BracketPairs(), {P(u"'", u"'"), P(u"\"", u"\""), P(u"<!--", u"-->", NS | NC)}),
                 u"''\"\"{}[]()<>", u"<!--");
        xml = L("xml",
                With(BracketPairs(),
                     {P(u"\"", u"\"", NS), P(u"'", u"'", NS), P(u"<!--", u"-->", NS | NC)}),
                u"''\"\"{}[]()<>", u"<!--");
        make = L("makefile", With(BracketPairs(), {P(u"'", u"'", NS | NC), P(u"\"", u"\"", NS | NC)}),
                 u"");
        sql = L("sql",
                With(BracketPairs(),
                     {P(u"\"", u"\"", NS), P(u"N'", u"'", NS | NC), P(u"'", u"'", NS | NC)}),
                u"{}[]()\"\"''``", u"/*");
        docker = L("dockerfile", With(BracketPairs(), {P(u"\"", u"\""), P(u"'", u"'")}),
                   u"{}[]()\"\"''");
        ini = L("ini", With(BracketPairs(), {P(u"\"", u"\"", NS), P(u"'", u"'", NS)}),
                u"{}[]()\"\"''");
        properties = L("properties", With(BracketPairs(), {P(u"\"", u"\"", NS)}), u"{}[]()\"\"''");
        dotenv = L("dotenv", With(BracketPairs(), {P(u"\"", u"\""), P(u"'", u"'")}), u"{}[]()\"\"''");
        diff = L("diff", {}, u"");
        // Lisps quote with ', so only the double quote pairs (VS Code's
        // clojure).
        lisp = L("lisp", With(BracketPairs(), {P(u"\"", u"\"", NS)}), u"{}[]()\"\"");
        // A language VS Code configures that has no table here: the pairs
        // nearly all of them share.
        code = L("code", With(BracketPairs(), {P(u"\"", u"\"", NS), P(u"'", u"'", NS | NC)}),
                 u"{}[]()\"\"''``");
    }
};

const Languages& languages() {
    static const Languages l;
    return l;
}

std::string lowerName(const std::string& path) {
    std::string name = path.substr(path.find_last_of('/') == std::string::npos
                                       ? 0
                                       : path.find_last_of('/') + 1);
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    return name;
}

}  // namespace

const Language& languageFor(const std::string& path) {
    const Languages& l = languages();
    const std::string name = lowerName(path);
    static const std::unordered_map<std::string, const Language Languages::*> byName = {
        {"makefile", &Languages::make},       {"gnumakefile", &Languages::make},
        {"dockerfile", &Languages::docker},   {".env", &Languages::dotenv},
        {".zshrc", &Languages::shell},        {".zshenv", &Languages::shell},
        {".zprofile", &Languages::shell},     {".bashrc", &Languages::shell},
        {".bash_profile", &Languages::shell}, {".profile", &Languages::shell},
        {".gitconfig", &Languages::ini},      {".editorconfig", &Languages::ini},
    };
    auto n = byName.find(name);
    if (n != byName.end()) return l.*(n->second);
    if (name.rfind(".env.", 0) == 0) return l.dotenv;
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 == name.size()) return l.plain;
    const std::string ext = name.substr(dot + 1);
    static const std::unordered_map<std::string, const Language Languages::*> byExt = {
        {"py", &Languages::python},      {"pyw", &Languages::python},
        {"pyi", &Languages::python},     {"md", &Languages::markdown},
        {"markdown", &Languages::markdown}, {"mdown", &Languages::markdown},
        {"mkd", &Languages::markdown},   {"tex", &Languages::latex},
        {"sty", &Languages::latex},      {"cls", &Languages::latex},
        {"ltx", &Languages::latex},      {"dtx", &Languages::latex},
        {"bib", &Languages::latex},      {"js", &Languages::js},
        {"mjs", &Languages::js},         {"cjs", &Languages::js},
        {"jsx", &Languages::js},         {"ts", &Languages::js},
        {"mts", &Languages::js},         {"cts", &Languages::js},
        {"tsx", &Languages::js},         {"c", &Languages::cpp},
        {"h", &Languages::cpp},          {"cc", &Languages::cpp},
        {"cpp", &Languages::cpp},        {"cxx", &Languages::cpp},
        {"c++", &Languages::cpp},        {"hpp", &Languages::cpp},
        {"hh", &Languages::cpp},         {"hxx", &Languages::cpp},
        {"h++", &Languages::cpp},        {"ino", &Languages::cpp},
        {"inl", &Languages::cpp},        {"ipp", &Languages::cpp},
        {"m", &Languages::objc},         {"mm", &Languages::objc},
        {"rs", &Languages::rust},        {"go", &Languages::go},
        {"sh", &Languages::shell},       {"bash", &Languages::shell},
        {"zsh", &Languages::shell},      {"ksh", &Languages::shell},
        {"fish", &Languages::shell},     {"json", &Languages::json},
        {"jsonc", &Languages::json},     {"json5", &Languages::json},
        {"code-workspace", &Languages::json}, {"yml", &Languages::yaml},
        {"yaml", &Languages::yaml},      {"css", &Languages::css},
        {"scss", &Languages::css},       {"less", &Languages::css},
        {"html", &Languages::html},      {"htm", &Languages::html},
        {"xhtml", &Languages::html},     {"xml", &Languages::xml},
        {"svg", &Languages::xml},        {"xsd", &Languages::xml},
        {"xsl", &Languages::xml},        {"xslt", &Languages::xml},
        {"plist", &Languages::xml},      {"mk", &Languages::make},
        {"sql", &Languages::sql},        {"ini", &Languages::ini},
        {"properties", &Languages::properties}, {"env", &Languages::dotenv},
        {"diff", &Languages::diff},      {"patch", &Languages::diff},
        {"clj", &Languages::lisp},       {"cljs", &Languages::lisp},
        {"cljc", &Languages::lisp},      {"edn", &Languages::lisp},
        {"el", &Languages::lisp},        {"lisp", &Languages::lisp},
        {"scm", &Languages::lisp},       {"rkt", &Languages::lisp},
        {"txt", &Languages::plain},      {"text", &Languages::plain},
        {"log", &Languages::plain},
    };
    auto e = byExt.find(ext);
    if (e != byExt.end()) return l.*(e->second);
    return SyntaxHighlighter::supports(ext) ? l.code : l.plain;
}

// ---- the highlighter as a Context

namespace {

Scope scopeOf(TokenStyle s) {
    return s == TokenStyle::String ? Scope::String
         : s == TokenStyle::Comment ? Scope::Comment
                                    : Scope::Code;
}

}  // namespace

HighlighterContext::HighlighterContext(IncrementalHighlighter<char16_t>& hl,
                                       std::unique_ptr<TextSource<char16_t>> source)
    : hl_(hl), src_(std::move(source)) {}

HighlighterContext::~HighlighterContext() = default;

Scope HighlighterContext::at(size_t i) const {
    if (i >= hl_.length()) return Scope::Code;
    const size_t line = hl_.lineOf(i);
    if (line != line_) {
        line_ = line;
        tokens_.clear();
        scopes_.clear();
        std::vector<Token> out;
        hl_.lineTokens(*src_, line, line + 1, out);
        for (const Token& t : out) {
            tokens_.push_back({t.start, t.start + t.length});
            scopes_.push_back(scopeOf(t.style));
        }
    }
    for (size_t k = 0; k < tokens_.size(); k++)
        if (tokens_[k].first <= i && i < tokens_[k].second) return scopes_[k];
    return Scope::Code;
}

Scope HighlighterContext::ifTyped(size_t i, char16_t c) const {
    return scopeOf(hl_.styleIfInserted(*src_, i, c));
}

bool HighlighterContext::lexed(size_t i) const {
    if (i > hl_.length()) return false;
    const size_t line = hl_.lineOf(i);
    const size_t end = line + 1 < hl_.lineCount() ? hl_.lineStart(line + 1) : hl_.length();
    return end - hl_.lineStart(line) <= kMaxTokenizationLineLength + 1;
}

std::vector<Span> HighlighterContext::nonCode(size_t lo, size_t hi) const {
    std::vector<Span> spans;
    hi = std::min(hi, hl_.length());
    if (hi <= lo) return spans;
    std::vector<Token> tokens;
    hl_.lineTokens(*src_, hl_.lineOf(lo), hl_.lineOf(hi - 1) + 1, tokens);
    for (const Token& t : tokens) {
        const Scope s = scopeOf(t.style);
        if (s == Scope::Code) continue;
        const size_t a = std::max(lo, t.start), b = std::min(hi, t.start + t.length);
        if (b <= a) continue;
        if (!spans.empty() && spans.back().scope == s && spans.back().end >= a)
            spans.back().end = std::max(spans.back().end, b);
        else
            spans.push_back({a, b, s});
    }
    return spans;
}

// ---- the tracker

void Tracker::add(const std::vector<Closed>& closed) {
    if (closed.empty()) return;
    Action a;
    for (const Closed& c : closed) {
        a.closers.push_back({c.closeStart, c.closeEnd});
        a.pairs.push_back({c.pairStart, c.closeEnd});
    }
    actions_.push_back(std::move(a));
}

namespace {

// A range that never grows at its edges (VS Code's decorations of
// auto-closed characters), as text [pos, pos + oldLength) becomes newLength
// units: text put in at its start moves it, at its end leaves it, inside it
// grows it, and what it loses shrinks it. False when nothing of it is left.
bool follow(size_t& start, size_t& end, size_t pos, size_t oldLength, size_t newLength) {
    const size_t oldEnd = pos + oldLength;
    if (oldEnd <= start) {   // before it, or put in at its start
        start = start - oldLength + newLength;
        end = end - oldLength + newLength;
        return true;
    }
    if (pos >= end) return true;   // after it, or put in at its end
    if (pos <= start && end <= oldEnd) return false;   // all of it went
    const size_t s = pos <= start ? pos + newLength : start;
    const size_t e = end > oldEnd ? end - oldLength + newLength : pos;
    start = s;
    end = e;
    return e > s;
}

}  // namespace

void Tracker::edited(size_t pos, size_t oldLength, size_t newLength) {
    for (Action& a : actions_) {
        std::vector<Range> closers, pairs;
        for (size_t k = 0; k < a.closers.size(); k++) {
            Range c = a.closers[k], p = a.pairs[k];
            // A closer typed over, deleted or replaced is gone, and its pair
            // with it.
            const bool touched = oldLength && pos < c.end && pos + oldLength > c.start;
            if (touched || !follow(c.start, c.end, pos, oldLength, newLength) ||
                !follow(p.start, p.end, pos, oldLength, newLength))
                continue;
            closers.push_back(c);
            pairs.push_back(p);
        }
        a.closers = std::move(closers);
        a.pairs = std::move(pairs);
    }
    actions_.erase(std::remove_if(actions_.begin(), actions_.end(),
                                  [](const Action& a) { return a.closers.empty(); }),
                   actions_.end());
}

void Tracker::validate(const Text& text, const std::vector<Caret>& carets) {
    auto valid = [&](const Action& a) {
        std::vector<Range> pairs = a.pairs;
        for (const Range& p : pairs) {
            if (p.end > text.size()) return false;
            for (size_t i = p.start; i < p.end; i++)
                if (isBreak(text[i])) return false;   // the pair spans lines
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](const Range& x, const Range& y) { return x.start < y.start; });
        if (carets.size() > pairs.size()) return false;
        for (size_t i = 0; i < carets.size(); i++)
            if (!(pairs[i].start < carets[i].lo() && carets[i].hi() < pairs[i].end)) return false;
        return true;
    };
    actions_.erase(std::remove_if(actions_.begin(), actions_.end(),
                                  [&](const Action& a) { return !valid(a); }),
                   actions_.end());
}

bool Tracker::closerAt(size_t at) const {
    for (const Action& a : actions_)
        for (const Range& c : a.closers)
            if (c.start == at) return true;
    return false;
}

// ---- typing

namespace {

bool allEmpty(const std::vector<Caret>& carets) {
    for (const Caret& c : carets)
        if (!c.empty()) return false;
    return !carets.empty();
}

// VS Code's isAutoClosingOvertype: ch closes one of the language's pairs,
// is the character after every caret, not escaped by a backslash if it is
// a quote, and was put there by auto-closing.
bool overtypes(const Text& t, const std::vector<Caret>& carets, char16_t ch, const Language& lang,
               const Tracker& tracker) {
    bool closes = false;
    for (const Pair& p : lang.pairs)
        if (p.close.size() == 1 && p.close[0] == ch) closes = true;
    if (!closes || !allEmpty(carets)) return false;
    for (const Caret& c : carets) {
        const size_t p = c.head;
        if (p >= t.size() || t[p] != ch) return false;
        if (isQuote(ch) && p > lineStartOf(t, p) && t[p - 1] == u'\\') return false;
        if (!tracker.closerAt(p)) return false;
    }
    return true;
}

// The bracket kind a pair's opener opens, for the balance check (VS Code's
// hasUnmatchedClosingBracketAfter, counted per kind as Brackets does).
char16_t bracketCloser(const std::u16string& open) {
    switch (open.empty() ? 0 : open.back()) {
    case u'(': return u')';
    case u'[': return u']';
    case u'{': return u'}';
    default: return 0;
    }
}

// Whether a closer of `opener`'s kind after any caret has no opener to
// match it: the openers left open before the caret (within kBalanceReach)
// are counted, each closer after it takes one, and one with none left is
// unmatched. Strings and comments do not count. Carets whose reaches
// overlap share one window, so a column of carets lexes its stretch once.
bool unmatchedCloserAfter(const Text& t, const std::vector<Caret>& carets, char16_t opener,
                          char16_t closer, const Context& ctx) {
    const size_t n = t.size();
    auto reachFrom = [](size_t p) { return p > kBalanceReach ? p - kBalanceReach : 0; };
    size_t k = 0;
    while (k < carets.size()) {
        const size_t lo = reachFrom(carets[k].head);
        size_t hi = std::min(n, carets[k].head + kBalanceReach), last = k;
        while (last + 1 < carets.size() && reachFrom(carets[last + 1].head) <= hi)
            hi = std::min(n, carets[++last].head + kBalanceReach);
        // The brackets of this kind in code, and at each caret `net`
        // (openers minus closers since lo) and `open` (openers still open).
        const std::vector<Span> skip = ctx.nonCode(lo, hi);
        std::vector<std::pair<size_t, int>> brackets;
        std::vector<long> netAt, openAt;
        long net = 0, open = 0;
        size_t s = 0, j = k;
        for (size_t i = lo; i < hi; i++) {
            while (j <= last && carets[j].head == i) {
                netAt.push_back(net);
                openAt.push_back(open);
                j++;
            }
            const char16_t c = t[i];
            if (c != opener && c != closer) continue;
            while (s < skip.size() && skip[s].end <= i) s++;
            if (s < skip.size() && skip[s].start <= i) continue;
            brackets.push_back({i, c == opener ? 1 : -1});
            net += c == opener ? 1 : -1;
            open = c == opener ? open + 1 : std::max(0L, open - 1);
        }
        for (; j <= last; j++) {   // carets at hi
            netAt.push_back(net);
            openAt.push_back(open);
        }
        // From the end back: the lowest net after each caret, which falls
        // below its net minus its open openers exactly when a closer after
        // it has none to take.
        long low = 0;
        bool any = false;
        size_t c = last + 1;
        auto unmatched = [&](size_t idx) {
            return any && low < netAt[idx - k] - openAt[idx - k];
        };
        for (size_t e = brackets.size(); e-- > 0;) {
            while (c > k && carets[c - 1].head > brackets[e].first)
                if (unmatched(--c)) return true;
            low = any ? std::min(low, net) : net;
            any = true;
            net -= brackets[e].second;
        }
        while (c > k)
            if (unmatched(--c)) return true;
        k = last + 1;
    }
    return false;
}

bool scopeAllows(const Pair& pair, Scope s) {
    return !((s == Scope::String && pair.notInString) || (s == Scope::Comment && pair.notInComment));
}

// VS Code's _isBeforeClosingBrace: what follows the caret starts with a
// closer of some pair and with no opener.
bool beforeClosingBrace(const Language& lang, const std::u16string& after) {
    bool opening = false, closing = false;
    for (const Pair& p : lang.pairs) {
        if (!p.open.empty() && startsWith(after, p.open)) opening = true;
        if (!p.close.empty() && startsWith(after, p.close)) closing = true;
    }
    return !opening && closing;
}

char16_t neutralFor(const Pair& p) {
    auto in = [&](char16_t c) {
        return p.open.find(c) != std::u16string::npos || p.close.find(c) != std::u16string::npos;
    };
    for (char16_t c = u'0'; c <= u'9'; c++)
        if (!in(c)) return c;
    for (char16_t c = u'a'; c <= u'z'; c++)
        if (!in(c)) return c;
    for (char16_t c = u'A'; c <= u'Z'; c++)
        if (!in(c)) return c;
    return 0;
}

// VS Code's getAutoClosingPair: the pair typing ch at every caret closes,
// and the closer to put in (shorter when a pair it contains is closed
// already: "/*" then "*" gives "/** */"). False when it closes nothing.
bool closingPair(const Text& t, const std::vector<Caret>& carets, char16_t ch,
                 const Language& lang, const Context& ctx, const Pair** found,
                 std::u16string* closeOut) {
    if (!allEmpty(carets)) return false;
    // The longest opener ending in ch that the text before every caret
    // completes.
    const Pair* pair = nullptr;
    for (const Pair& cand : lang.pairs) {
        if (cand.open.empty() || cand.open.back() != ch) continue;
        if (pair && cand.open.size() <= pair->open.size()) continue;
        const size_t before = cand.open.size() - 1;
        bool all = true;
        for (const Caret& c : carets) {
            const size_t p = c.head;
            if (p < before || p - before < lineStartOf(t, p) ||
                !textAt(t, p - before, cand.open.substr(0, before))) {
                all = false;
                break;
            }
        }
        if (all) pair = &cand;
    }
    if (!pair) return false;
    const bool quote = isQuote(ch);
    const bool comment =
        !quote && !lang.blockCommentStart.empty() &&
        pair->open.find(lang.blockCommentStart) != std::u16string::npos;
    const std::u16string& before = quote ? lang.closeBeforeQuotes : lang.closeBeforeBrackets;
    const bool checkBalance = !quote && !comment;
    // A pair it contains ("/*" in "/**"): its closer may be there already.
    const Pair* contained = nullptr;
    if (pair->open.size() > 1)
        for (const Pair& cand : lang.pairs)
            if (&cand != pair && cand.open != pair->open &&
                pair->open.find(cand.open) != std::u16string::npos &&
                endsWith(pair->close, cand.close) &&
                (!contained || cand.open.size() > contained->open.size()))
                contained = &cand;
    const std::u16string containedClose = contained ? contained->close : u"";
    bool containedThere = true;
    for (const Caret& c : carets) {
        const size_t p = c.head;
        const size_t ls = lineStartOf(t, p);
        const size_t le = lineEndOf(t, p);
        const std::u16string after = t.substr(p, std::min(le - p, (size_t)64));
        if (!startsWith(after, containedClose)) containedThere = false;
        // Only before a space, the line's end, a listed character or a
        // closer.
        if (!after.empty() && !beforeClosingBrace(lang, after) &&
            before.find(after[0]) == std::u16string::npos)
            return false;
        // Not a quote after a word character ("don't").
        if (pair->open.size() == 1 && (ch == u'\'' || ch == u'"') && p > ls &&
            isWordChar(t[p - 1]))
            return false;
        if (!ctx.lexed(p)) return false;
        // Where the pair belongs: the character before the caret (or the
        // first of the line), and what a neutral character typed at the
        // caret would be part of (a quote there may end a string).
        const size_t from = pair->scopeBeforeOpen ? p - (pair->open.size() - 1) : p;
        if (from > ls) {
            if (!scopeAllows(*pair, ctx.at(from - 1))) return false;
        } else if (!pair->scopeBeforeOpen && p < le) {
            if (!scopeAllows(*pair, ctx.at(p))) return false;
        }
        if (const char16_t neutral = neutralFor(*pair))
            if (!scopeAllows(*pair, ctx.ifTyped(p, neutral))) return false;
    }
    // A bracket with an unmatched closer after it already gets none.
    if (checkBalance)
        if (const char16_t closer = bracketCloser(pair->open))
            if (unmatchedCloserAfter(t, carets, pair->open.back(), closer, ctx)) return false;
    *found = pair;
    *closeOut = containedThere && !containedClose.empty()
                    ? pair->close.substr(0, pair->close.size() - containedClose.size())
                    : pair->close;
    return true;
}

// VS Code's _isSurroundSelectionType: every caret has a selection with
// something besides blanks in it, and a quote is not typed over a quote.
bool surrounds(const Text& t, const std::vector<Caret>& carets, char16_t ch,
               const Language& lang, char16_t* closer) {
    bool listed = false;
    for (const auto& s : lang.surround)
        if (s.first == ch) {
            listed = true;
            *closer = s.second;
        }
    if (!listed || carets.empty()) return false;
    for (const Caret& c : carets) {
        if (c.empty()) return false;
        bool blank = true;
        for (size_t i = c.lo(); i < c.hi() && blank; i++)
            if (t[i] != u' ' && t[i] != u'\t' && !isBreak(t[i])) blank = false;
        if (blank) return false;
        if (isQuote(ch) && c.hi() - c.lo() == 1 && isQuote(t[c.lo()])) return false;
    }
    return true;
}

}  // namespace

Typed type(const Text& text, const std::vector<Caret>& carets, char16_t ch,
           const Language& lang, const Context& context, const Tracker& tracker, bool closing) {
    Typed r;
    if (carets.empty()) return r;
    if (closing && overtypes(text, carets, ch, lang, tracker)) {
        r.kind = Typed::Kind::Overtype;
        for (const Caret& c : carets) r.edit.carets.push_back({c.head + 1, c.head + 1});
        return r;
    }
    const Pair* pair = nullptr;
    std::u16string close;
    if (closing && closingPair(text, carets, ch, lang, context, &pair, &close)) {
        r.kind = Typed::Kind::Close;
        size_t shift = 0;
        for (const Caret& c : carets) {
            const size_t p = c.head;
            r.edit.changes.push_back({p, 0, std::u16string(1, ch)});
            if (!close.empty()) r.edit.changes.push_back({p, 0, close});
            const size_t caret = p + shift + 1;
            r.edit.carets.push_back({caret, caret});
            if (!close.empty())
                r.closed.push_back({caret - pair->open.size(), caret, caret + close.size()});
            shift += 1 + close.size();
        }
        return r;
    }
    char16_t closer = 0;
    if (surrounds(text, carets, ch, lang, &closer)) {
        r.kind = Typed::Kind::Surround;
        r.edit = MultiCaret::surround(carets, ch, closer);
        return r;
    }
    return r;
}

Edit deletePair(const Text& t, const std::vector<Caret>& carets, const Language& lang,
                const Tracker& tracker) {
    Edit e;
    if (!allEmpty(carets)) return e;
    size_t shift = 0;
    std::vector<MultiCaret::Change> changes;
    for (const Caret& c : carets) {
        const size_t p = c.head;
        const size_t ls = lineStartOf(t, p), le = lineEndOf(t, p);
        if (p <= ls || p >= le) return {};
        bool pairs = false;
        for (const Pair& pr : lang.pairs)
            if (pr.open.size() == 1 && pr.close.size() == 1 && pr.open[0] == t[p - 1] &&
                pr.close[0] == t[p])
                pairs = true;
        if (!pairs || !tracker.closerAt(p)) return {};
        changes.push_back({p - 1, 2, u""});
        e.carets.push_back({p - 1 - shift, p - 1 - shift});
        shift += 2;
    }
    e.changes = std::move(changes);
    return e;
}

}  // namespace AutoClose
