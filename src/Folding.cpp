// Folding.cpp — see Folding.h.
#include "Folding.h"
#include "AutoIndent.h"
#include "MarkdownParser.h"
#include "SyntaxHighlighter.h"
#include <algorithm>
#include <cctype>

namespace Fold {

namespace {

std::string lower(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += (char)std::tolower((unsigned char)c);
    return o;
}

std::string extOf(const std::string& filename) {
    std::string base = lower(filename);
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? "" : base.substr(dot + 1);
}

bool isBlank(char16_t c) { return c == u' ' || c == u'\t' || c == u'\r' || c == u'\f'; }
bool isWordChar(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') ||
           c == u'_';
}

class PtrSource : public TextSource<char16_t> {
public:
    PtrSource(const char16_t* t, size_t n) : t_(t), n_(n) {}
    size_t length() const override { return n_; }
    void read(size_t pos, size_t len, char16_t* out) const override {
        std::copy(t_ + pos, t_ + pos + len, out);
    }

private:
    const char16_t* t_;
    size_t n_;
};

// The text's lines: where each starts. A line ends after '\n'.
struct Lines {
    const char16_t* t;
    size_t n;
    std::vector<size_t> starts;
    Lines(const char16_t* text, size_t length) : t(text), n(length) {
        starts.push_back(0);
        for (size_t i = 0; i < n; i++)
            if (t[i] == u'\n') starts.push_back(i + 1);
    }
    size_t count() const { return starts.size(); }
    size_t start(size_t l) const { return starts[l]; }
    size_t end(size_t l) const { return l + 1 < starts.size() ? starts[l + 1] : n; }
    // The first non-blank unit of a line, or its end (its '\n' or the text's end).
    size_t firstText(size_t l) const {
        size_t i = start(l), e = end(l);
        while (i < e && isBlank(t[i])) i++;
        return i;
    }
    bool blank(size_t l) const {
        size_t i = firstText(l);
        return i >= end(l) || t[i] == u'\n';
    }
    size_t lineOf(size_t pos) const {
        return (size_t)(std::upper_bound(starts.begin(), starts.end(), pos) - starts.begin()) - 1;
    }
    // Columns of indentation, a tab to the next multiple of 4.
    int indent(size_t l) const {
        int col = 0;
        for (size_t i = start(l), e = end(l); i < e; i++) {
            if (t[i] == u' ') col++;
            else if (t[i] == u'\t') col = (col / 4 + 1) * 4;
            else break;
        }
        return col;
    }
    // Whether the line's text, after its indentation, starts with `s`.
    bool startsWith(size_t l, size_t from, const char16_t* s) const {
        size_t i = from, e = end(l);
        for (; *s; s++, i++)
            if (i >= e || t[i] != *s) return false;
        return true;
    }
};

// The highlighter's reading of a text: its tokens, and the state each line
// ends in (whether a string or comment runs on into the next line).
struct Lexed {
    std::vector<Token> tokens;
    std::vector<LexState> ends;   // one per line; empty without a grammar
};

Lexed lex(const char16_t* text, size_t length, const std::string& ext) {
    Lexed out;
    if (!SyntaxHighlighter::supports(ext)) return out;
    IncrementalHighlighter<char16_t> h(ext);
    h.reset(PtrSource(text, length), &out.tokens);
    out.ends.reserve(h.lineCount());
    for (size_t l = 0; l < h.lineCount(); l++) out.ends.push_back(h.endState(l));
    return out;
}

bool notCode(TokenStyle s) { return s == TokenStyle::String || s == TokenStyle::Comment; }

// ---- region markers: #region / #endregion and their spellings
// 1 for a start marker, -1 for an end marker, 0 for neither.
int markerAt(const Lines& L, size_t l) {
    size_t i = L.firstText(l), e = L.end(l);
    const char16_t* t = L.t;
    auto rest = [&](size_t at, const char16_t* word) {
        size_t k = at;
        for (const char16_t* w = word; *w; w++, k++)
            if (k >= e || t[k] != *w) return (size_t)0;
        if (k < e && isWordChar(t[k])) return (size_t)0;
        return k;
    };
    auto keyword = [&](size_t at) -> int {
        while (at < e && isBlank(t[at])) at++;
        if (at < e && t[at] == u'#') at++;
        if (rest(at, u"region")) return 1;
        if (rest(at, u"endregion")) return -1;
        return 0;
    };
    static const char16_t* openers[] = {u"<!--", u"//", u"/*", u"--", u"#", u";", u"%"};
    if (size_t k = rest(i, u"#pragma")) return keyword(k);
    for (const char16_t* o : openers) {
        if (!L.startsWith(l, i, o)) continue;
        size_t k = i + std::char_traits<char16_t>::length(o);
        if (o[0] == u'#') {
            // "#region" itself (C#, Python), or "# region".
            if (rest(k, u"region")) return 1;
            if (rest(k, u"endregion")) return -1;
        }
        return keyword(k);
    }
    return 0;
}

void addMarkers(const Lines& L, const std::vector<bool>& inString, std::vector<Range>& out) {
    std::vector<size_t> open;
    for (size_t l = 0; l < L.count(); l++) {
        if (inString[l]) continue;
        int m = markerAt(L, l);
        if (m > 0) {
            open.push_back(l);
        } else if (m < 0 && !open.empty()) {
            size_t s = open.back();
            open.pop_back();
            if (l > s) out.push_back({s, l, Kind::Region});
        }
    }
}

// Lines whose start lies inside a string or comment that began on an
// earlier line, and the regions of block comments (and, with `docstrings`,
// of strings that start a line) over several lines.
void readStates(const Lines& L, const Lexed& lx, std::vector<bool>& inside,
                std::vector<Range>& out, bool docstrings) {
    inside.assign(L.count(), false);
    const size_t n = std::min(L.count(), lx.ends.size());
    for (size_t l = 0; l < n;) {
        if (lx.ends[l].kind == LexState::Normal) { l++; continue; }
        size_t e = l + 1;
        while (e < n && lx.ends[e].kind != LexState::Normal) e++;
        if (e >= n) e = n - 1;
        for (size_t k = l + 1; k <= e; k++) inside[k] = true;
        if (e > l) {
            const bool comment = lx.ends[l].kind == LexState::BlockComment;
            bool starts = false;   // a string token that is the line's first text
            if (!comment && docstrings) {
                const size_t first = L.firstText(l);
                auto it = std::lower_bound(lx.tokens.begin(), lx.tokens.end(), first,
                                           [](const Token& t, size_t at) { return t.start < at; });
                starts = it != lx.tokens.end() && it->start == first &&
                         it->style == TokenStyle::String;
            }
            if (comment || starts) out.push_back({l, e, Kind::Comment});
        }
        l = e + 1;
    }
}

// Two or more lines in a row holding nothing but a line comment.
void addCommentRuns(const Lines& L, const std::vector<Token>& tokens,
                    const std::vector<bool>& inside, std::vector<Range>& out) {
    std::vector<bool> only(L.count(), false);
    for (const Token& tk : tokens) {
        if (tk.style != TokenStyle::Comment) continue;
        size_t l = L.lineOf(tk.start);
        if (inside[l] || L.firstText(l) != tk.start) continue;
        size_t end = tk.start + tk.length, le = L.end(l);
        while (end < le && isBlank(L.t[end])) end++;
        if (end < le && L.t[end] != u'\n') continue;   // code after it, or it runs on
        if (markerAt(L, l)) continue;
        only[l] = true;
    }
    for (size_t l = 0; l < L.count();) {
        if (!only[l]) { l++; continue; }
        size_t e = l;
        while (e + 1 < L.count() && only[e + 1]) e++;
        if (e > l) out.push_back({l, e, Kind::Comment});
        l = e + 1;
    }
}

// ---- by indentation
void byIndentation(const Lines& L, const std::vector<bool>& inside, std::vector<Range>& out) {
    struct Open { size_t line; int indent; };
    std::vector<Open> stack;
    long lastText = -1;
    auto close = [&]() {
        Open o = stack.back();
        stack.pop_back();
        if (lastText > (long)o.line) out.push_back({o.line, (size_t)lastText, Kind::Block});
    };
    for (size_t l = 0; l < L.count(); l++) {
        if (inside[l]) { lastText = (long)l; continue; }
        if (L.blank(l)) continue;
        const int in = L.indent(l);
        while (!stack.empty() && stack.back().indent >= in) close();
        stack.push_back({l, in});
        lastText = (long)l;
    }
    while (!stack.empty()) close();
}

// ---- by brackets
void byBrackets(const Lines& L, const std::vector<Token>& tokens, std::vector<Range>& out) {
    struct Open { char16_t c; size_t line; };
    std::vector<Open> stack;
    size_t tk = 0;   // the next token that may hold the position
    size_t line = 0;
    for (size_t i = 0; i < L.n; i++) {
        const char16_t c = L.t[i];
        if (c == u'\n') { line++; continue; }
        if (c != u'(' && c != u'[' && c != u'{' && c != u')' && c != u']' && c != u'}') continue;
        while (tk < tokens.size() && tokens[tk].start + tokens[tk].length <= i) tk++;
        // Tokens never overlap, so only this one can hold i.
        bool skip = false;
        for (size_t k = tk; k < tokens.size() && tokens[k].start <= i; k++)
            if (notCode(tokens[k].style) && i < tokens[k].start + tokens[k].length) { skip = true; break; }
        if (skip) continue;
        if (c == u'(' || c == u'[' || c == u'{') {
            stack.push_back({c, line});
            continue;
        }
        const char16_t want = c == u')' ? u'(' : c == u']' ? u'[' : u'{';
        size_t k = stack.size();
        while (k > 0 && stack[k - 1].c != want) k--;
        if (k == 0) continue;   // a closer with no opener: ignored
        const size_t s = stack[k - 1].line;
        stack.resize(k - 1);
        // The closer's line stays in view when only closers come before it
        // there (`}`, `});`, `} else {`).
        bool leads = true;
        for (size_t k = L.firstText(line); k < i && leads; k++) {
            const char16_t p = L.t[k];
            leads = p == u')' || p == u']' || p == u'}' || isBlank(p);
        }
        const size_t e = leads && line > 0 ? line - 1 : line;
        if (e > s) out.push_back({s, e, Kind::Block});
    }
}

// ---- Markdown
std::string toUtf8(const char16_t* t, size_t n) {
    std::string o;
    o.reserve(n);
    for (size_t i = 0; i < n; i++) {
        uint32_t c = t[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && t[i + 1] >= 0xDC00 && t[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (t[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) {
            o += (char)c;
        } else if (c < 0x800) {
            o += (char)(0xC0 | (c >> 6));
            o += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            o += (char)(0xE0 | (c >> 12));
            o += (char)(0x80 | ((c >> 6) & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        } else {
            o += (char)(0xF0 | (c >> 18));
            o += (char)(0x80 | ((c >> 12) & 0x3F));
            o += (char)(0x80 | ((c >> 6) & 0x3F));
            o += (char)(0x80 | (c & 0x3F));
        }
    }
    return o;
}

void markdown(const Lines& L, std::vector<Range>& out) {
    const std::string src = toUtf8(L.t, L.n);
    const std::vector<MdLine> md = MarkdownParser::lines(src);
    const size_t n = std::min(md.size(), L.count());
    struct Heading { size_t first, last; int level; };
    std::vector<Heading> heads;
    for (size_t l = 0; l < n; l++) {
        if (md[l].kind != MdLine::Heading) continue;
        if (l > 0 && md[l - 1].kind == MdLine::Heading && md[l - 1].block == md[l].block)
            continue;   // a setext heading's further text lines
        size_t last = l;
        while (last + 1 < n && md[last + 1].kind == MdLine::Heading &&
               md[last + 1].block == md[l].block)
            last++;
        int level = 0;
        size_t i = md[l].lineStart;
        while (i < md[l].lineEnd && src[i] == ' ') i++;
        while (i < md[l].lineEnd && src[i] == '#') { level++; i++; }
        if (level == 0 || level > 6) {
            level = 2;
            if (last + 1 < n && md[last + 1].kind == MdLine::SetextUnderline) {
                last++;
                size_t u = md[last].lineStart;
                while (u < md[last].lineEnd && src[u] == ' ') u++;
                if (u < md[last].lineEnd && src[u] == '=') level = 1;
            }
        }
        heads.push_back({l, last, level});
    }
    for (size_t h = 0; h < heads.size(); h++) {
        size_t end = n - 1;
        for (size_t k = h + 1; k < heads.size(); k++)
            if (heads[k].level <= heads[h].level) { end = heads[k].first - 1; break; }
        while (end > heads[h].last && L.blank(end)) end--;
        if (end > heads[h].last) out.push_back({heads[h].first, end, Kind::Heading});
    }
    // Fenced code (its closing fence folded with it) and display math.
    for (size_t l = 0; l < n;) {
        if (md[l].kind == MdLine::Fence) {
            size_t e = l + 1;
            while (e < n && md[e].kind == MdLine::Code) e++;
            if (e < n && md[e].kind == MdLine::Fence) {
                if (e > l) out.push_back({l, e, Kind::Block});
                l = e + 1;
            } else {
                if (e - 1 > l) out.push_back({l, e - 1, Kind::Block});
                l = e;
            }
            continue;
        }
        if (md[l].kind == MdLine::Math) {
            size_t e = l;
            while (e + 1 < n && md[e + 1].kind == MdLine::Math && md[e + 1].block == md[l].block)
                e++;
            if (e > l) out.push_back({l, e, Kind::Block});
            l = e + 1;
            continue;
        }
        l++;
    }
}

// ---- LaTeX
int sectionLevel(const std::u16string& name) {
    static const char16_t* names[] = {u"part", u"chapter", u"section", u"subsection",
                                      u"subsubsection", u"paragraph", u"subparagraph"};
    for (int i = 0; i < 7; i++)
        if (name == names[i]) return i;
    return -1;
}

void latex(const Lines& L, std::vector<Range>& out) {
    struct Section { size_t line; int level; };
    struct Env { std::u16string name; size_t line; };
    std::vector<Section> sections;
    std::vector<Env> envs;
    auto lastText = [&](size_t before, size_t floor) {
        // The last line before `before` that is not blank, at least `floor`.
        size_t e = before;
        while (e > floor && L.blank(e - 1)) e--;
        return e == 0 ? 0 : e - 1;
    };
    auto closeSections = [&](size_t before, int level) {
        while (!sections.empty() && sections.back().level >= level) {
            Section s = sections.back();
            sections.pop_back();
            size_t e = lastText(before, s.line);
            if (before > 0 && e > s.line) out.push_back({s.line, e, Kind::Section});
        }
    };
    for (size_t l = 0; l < L.count(); l++) {
        // The line without its comment: up to a % no backslash escapes.
        size_t a = L.start(l), b = L.end(l);
        for (size_t i = a; i < b; i++) {
            if (L.t[i] == u'\\') { i++; continue; }
            if (L.t[i] == u'%' || L.t[i] == u'\n') { b = i; break; }
        }
        size_t first = L.firstText(l);
        for (size_t i = a; i < b; i++) {
            if (L.t[i] != u'\\') continue;
            size_t j = i + 1;
            while (j < b && ((L.t[j] >= u'a' && L.t[j] <= u'z') || (L.t[j] >= u'A' && L.t[j] <= u'Z')))
                j++;
            std::u16string name(L.t + i + 1, L.t + j);
            if (j == i + 1) { i = j; continue; }   // \\, \%, ...
            if (name == u"begin" || name == u"end") {
                size_t k = j;
                while (k < b && isBlank(L.t[k])) k++;
                if (k >= b || L.t[k] != u'{') { i = j - 1; continue; }
                size_t close = k + 1;
                while (close < b && L.t[close] != u'}') close++;
                if (close >= b) { i = j - 1; continue; }
                std::u16string env(L.t + k + 1, L.t + close);
                if (name == u"begin") {
                    envs.push_back({env, l});
                } else {
                    if (env == u"document") closeSections(l, 0);
                    size_t m = envs.size();
                    while (m > 0 && envs[m - 1].name != env) m--;
                    if (m > 0) {
                        size_t s = envs[m - 1].line;
                        envs.resize(m - 1);
                        size_t e = first == i ? (l > 0 ? l - 1 : 0) : l;
                        if (e > s) out.push_back({s, e, Kind::Environment});
                    }
                }
                i = close;
                continue;
            }
            const int level = sectionLevel(name);
            if (level >= 0 && i == first) {
                size_t k = j;
                if (k < b && L.t[k] == u'*') k++;
                while (k < b && isBlank(L.t[k])) k++;
                if (k < b && (L.t[k] == u'{' || L.t[k] == u'[')) {
                    closeSections(l, level);
                    sections.push_back({l, level});
                }
            }
            i = j - 1;
        }
    }
    closeSections(L.count(), 0);
}

}  // namespace

Method methodFor(const std::string& filename) {
    const std::string ext = extOf(filename);
    if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd" || ext == "mdx")
        return Method::Markdown;
    if (ext == "tex" || ext == "ltx" || ext == "latex" || ext == "sty" || ext == "cls")
        return Method::Latex;
    if (ext == "bib" || AutoIndent::languageFor(filename) == AutoIndent::Lang::CLike)
        return Method::Brackets;
    return Method::Indentation;
}

std::vector<Range> compute(const char16_t* text, size_t length, const std::string& filename) {
    const Lines L(text, length);
    const Method m = methodFor(filename);
    std::vector<Range> out;
    Lexed lx;
    std::vector<bool> inside(L.count(), false);
    const std::vector<Token>& tokens = lx.tokens;
    if (m == Method::Brackets || m == Method::Indentation) {
        const std::string ext = extOf(filename);
        lx = lex(text, length, ext);
        const bool python = ext == "py" || ext == "pyw" || ext == "pyi";
        readStates(L, lx, inside, out, python);
        addCommentRuns(L, tokens, inside, out);
    }
    switch (m) {
    case Method::Brackets: byBrackets(L, tokens, out); break;
    case Method::Indentation: byIndentation(L, inside, out); break;
    case Method::Markdown: markdown(L, out); break;
    case Method::Latex: latex(L, out); break;
    }
    addMarkers(L, inside, out);
    return sanitize(std::move(out), L.count());
}

std::vector<Range> sanitize(std::vector<Range> ranges, size_t lineCount) {
    std::vector<Range> in;
    in.reserve(ranges.size());
    for (Range r : ranges) {
        if (lineCount == 0) break;
        if (r.end >= lineCount) r.end = lineCount - 1;
        if (r.start < r.end) in.push_back(r);
    }
    // Outer first among those starting on one line; stable, so of two equal
    // ranges the first given stays.
    std::stable_sort(in.begin(), in.end(), [](const Range& a, const Range& b) {
        return a.start != b.start ? a.start < b.start : a.end > b.end;
    });
    std::vector<Range> out;
    out.reserve(in.size());
    std::vector<size_t> stack;   // indexes into out
    for (const Range& r : in) {
        if (!out.empty() && out.back().start == r.start) continue;
        while (!stack.empty() && out[stack.back()].end < r.start) stack.pop_back();
        if (!stack.empty() && r.end > out[stack.back()].end) continue;   // crosses it
        out.push_back(r);
        stack.push_back(out.size() - 1);
    }
    return out;
}

Kind kindFromLsp(const std::string& kind) {
    if (kind == "comment") return Kind::Comment;
    if (kind == "imports") return Kind::Imports;
    if (kind == "region") return Kind::Region;
    return Kind::Block;
}

std::vector<int> depths(const std::vector<Range>& ranges) {
    std::vector<int> out(ranges.size(), 1);
    std::vector<size_t> stack;
    for (size_t i = 0; i < ranges.size(); i++) {
        while (!stack.empty() && ranges[stack.back()].end < ranges[i].start) stack.pop_back();
        out[i] = (int)stack.size() + 1;
        stack.push_back(i);
    }
    return out;
}

std::vector<size_t> containing(const std::vector<Range>& ranges, size_t line) {
    std::vector<size_t> out;
    // Ranges start in order; those starting after the line cannot hold it.
    auto endIt = std::upper_bound(ranges.begin(), ranges.end(), line,
                                  [](size_t l, const Range& r) { return l < r.start; });
    for (auto it = ranges.begin(); it != endIt; ++it)
        if (it->end >= line) out.push_back((size_t)(it - ranges.begin()));
    std::reverse(out.begin(), out.end());   // nested: later starts are inner
    return out;
}

long startingAt(const std::vector<Range>& ranges, size_t line) {
    auto it = std::lower_bound(ranges.begin(), ranges.end(), line,
                               [](const Range& r, size_t l) { return r.start < l; });
    return it != ranges.end() && it->start == line ? (long)(it - ranges.begin()) : -1;
}

Span spanOf(size_t firstLineContentEnd, size_t lastLineContentEnd) {
    return {firstLineContentEnd, std::max(firstLineContentEnd, lastLineContentEnd)};
}

size_t contentEnd(const char16_t* text, size_t lineStart, size_t lineEnd) {
    size_t e = lineEnd;
    if (e > lineStart && text[e - 1] == u'\n') {
        e--;
        if (e > lineStart && text[e - 1] == u'\r') e--;
    }
    return e;
}

// ---- Collapsed
static bool before(const Span& a, const Span& b) {
    return a.start != b.start ? a.start < b.start : a.end > b.end;
}

bool Collapsed::add(Span s) {
    if (s.end <= s.start) return false;
    for (Span& f : folds_) {
        if (f.start != s.start) continue;
        if (f.end == s.end) return false;
        f = s;   // one fold per first line
        std::sort(folds_.begin(), folds_.end(), before);
        return true;
    }
    folds_.insert(std::upper_bound(folds_.begin(), folds_.end(), s, before), s);
    return true;
}

bool Collapsed::remove(size_t start) {
    for (auto it = folds_.begin(); it != folds_.end(); ++it)
        if (it->start == start) { folds_.erase(it); return true; }
    return false;
}

bool Collapsed::contains(Span s) const {
    return std::find(folds_.begin(), folds_.end(), s) != folds_.end();
}

void Collapsed::edit(size_t pos, size_t oldLen, size_t newLen, bool insertsLineBreak,
                     std::vector<Span>* opened) {
    const size_t end = pos + oldLen;
    const long delta = (long)newLen - (long)oldLen;
    std::vector<Span> kept;
    kept.reserve(folds_.size());
    for (Span f : folds_) {
        bool touches;
        if (oldLen > 0)
            touches = (pos < f.end && end > f.start) || pos == f.end ||
                      (end == f.start && insertsLineBreak);
        else
            touches = (pos > f.start && pos <= f.end) || (pos == f.start && insertsLineBreak);
        if (touches) {
            if (opened) {
                // Where it was, in the new text's offsets: the edit's own
                // text stands where the edit was.
                size_t a = std::min(f.start, pos);
                long b = std::max((long)f.end + delta, (long)(pos + newLen));
                opened->push_back({a, (size_t)std::max<long>((long)a, b)});
            }
            continue;
        }
        if (f.start >= end) {
            f.start = (size_t)((long)f.start + delta);
            f.end = (size_t)((long)f.end + delta);
        }
        kept.push_back(f);
    }
    folds_.swap(kept);
}

std::vector<Span> Collapsed::outermost() const {
    std::vector<Span> out;
    for (const Span& f : folds_) {
        if (!out.empty() && f.start < out.back().end) continue;   // hidden by it
        out.push_back(f);
    }
    return out;
}

const Span* Collapsed::hiding(size_t pos) const {
    const Span* best = nullptr;
    for (const Span& f : folds_) {
        if (f.start > pos) break;
        if (pos < f.end && (!best || f.start < best->start)) best = &f;
        if (best) break;   // sorted by start: the first one holding pos is outermost
    }
    return best;
}

std::vector<Span> Collapsed::around(size_t pos) const {
    std::vector<Span> out;
    for (const Span& f : folds_) {
        if (f.start >= pos) break;
        if (pos < f.end) out.push_back(f);
    }
    return out;
}

}  // namespace Fold
