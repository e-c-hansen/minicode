// TextFind.cpp — see TextFind.h.
#include "TextFind.h"

#include <algorithm>
#include <chrono>
#include <regex>

static_assert(sizeof(wchar_t) == 4, "TextFind runs std::wregex on UTF-32 code points");

namespace TextFind {

char32_t foldCase(char32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x0100 && c <= 0x017F) {
        if ((c <= 0x012F) || (c >= 0x0132 && c <= 0x0137) || (c >= 0x014A && c <= 0x0177))
            return (c % 2 == 0) ? c + 1 : c;
        if ((c >= 0x0139 && c <= 0x0148) || (c >= 0x0179 && c <= 0x017E))
            return (c % 2 == 1) ? c + 1 : c;
        if (c == 0x0178) return 0x00FF;
        return c;
    }
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return c + 32;
    if (c == 0x0386) return 0x03AC;
    if (c >= 0x0388 && c <= 0x038A) return c + 37;
    if (c == 0x038C) return 0x03CC;
    if (c == 0x038E || c == 0x038F) return c + 63;
    if (c == 0x03C2) return 0x03C3;   // final sigma matches sigma
    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    if (c >= 0x0400 && c <= 0x040F) return c + 80;
    return c;
}

bool isWordChar(char32_t c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
        return true;
    if (c < 0x80) return false;
    // Past ASCII everything is a word character but the spaces.
    return !(c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
             c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF);
}

namespace {

// std::regex's case-insensitive matching asks the traits to fold; the
// standard ones ask the C locale, which folds ASCII only.
struct FoldTraits : std::regex_traits<wchar_t> {
    wchar_t translate_nocase(wchar_t c) const {
        return static_cast<wchar_t>(foldCase(static_cast<char32_t>(c)));
    }
};
using Regex = std::basic_regex<wchar_t, FoldTraits>;

// A line longer than this (code points) is searched one start position at
// a time, so a regex that backtracks over it can be stopped between them:
// after kRegexLineSeconds plus kRegexSecondsPerChar for each character
// (an ordinary regex takes well under that), or when the caller's cancel
// flag goes up. Shorter lines are searched in one call each.
constexpr std::size_t kSteppedLine = 2000;
constexpr double kRegexLineSeconds = 1.0;
constexpr double kRegexSecondsPerChar = 0.5e-6;

constexpr std::size_t npos = std::string::npos;

// ------------------------------------------------------------- decoding
// A line as code points, with each code point's offset in the source units
// (one more entry than code points: the line's end).
struct Line {
    std::wstring cps;
    std::vector<std::size_t> offs;
};

void decode16(const char16_t* s, std::size_t n, Line& out) {
    out.cps.clear();
    out.offs.clear();
    for (std::size_t i = 0; i < n;) {
        char32_t c = s[i];
        std::size_t len = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            len = 2;
        }
        out.cps.push_back(static_cast<wchar_t>(c));
        out.offs.push_back(i);
        i += len;
    }
    out.offs.push_back(n);
}

void decode8(const char* s, std::size_t n, Line& out) {
    out.cps.clear();
    out.offs.clear();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        char32_t cp;
        if (len == 1 || i + len > n) {
            cp = c;   // ASCII, or a stray byte taken as it is
            len = 1;
        } else {
            cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (std::size_t k = 1; k < len; ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.cps.push_back(static_cast<wchar_t>(cp));
        out.offs.push_back(i);
        i += len;
    }
    out.offs.push_back(n);
}

std::u32string toU32(const std::string& s) {
    Line l;
    decode8(s.data(), s.size(), l);
    return std::u32string(l.cps.begin(), l.cps.end());
}

std::u32string toU32(const std::u16string& s) {
    Line l;
    decode16(s.data(), s.size(), l);
    return std::u32string(l.cps.begin(), l.cps.end());
}

// The end of the line starting at `pos` and where the next one starts:
// \n, \r\n, \r and U+2029, as FolderSearch splits lines.
template <class Ch>
std::size_t lineEnd(const std::basic_string<Ch>& t, std::size_t pos, std::size_t* next);

template <>
std::size_t lineEnd(const std::u16string& t, std::size_t pos, std::size_t* next) {
    const std::size_t n = t.size();
    for (std::size_t e = pos; e < n; ++e) {
        const char16_t c = t[e];
        if (c == u'\n' || c == 0x2029) { *next = e + 1; return e; }
        if (c == u'\r') { *next = (e + 1 < n && t[e + 1] == u'\n') ? e + 2 : e + 1; return e; }
    }
    *next = n;
    return n;
}

template <>
std::size_t lineEnd(const std::string& t, std::size_t pos, std::size_t* next) {
    const std::size_t n = t.size();
    for (std::size_t e = pos; e < n; ++e) {
        const char c = t[e];
        if (c == '\n') { *next = e + 1; return e; }
        if (c == '\r') { *next = (e + 1 < n && t[e + 1] == '\n') ? e + 2 : e + 1; return e; }
        if (c == '\xE2' && e + 2 < n && t[e + 1] == '\x80' && t[e + 2] == '\xA9') {
            *next = e + 3;
            return e;
        }
    }
    *next = n;
    return n;
}

}  // namespace

// ------------------------------------------------------------------ the query
struct Query::Impl {
    Options opt;
    std::wstring needle;   // plain text, folded unless case-sensitive
    std::unique_ptr<Regex> re;
};

static std::shared_ptr<const Query::Impl> compile(const std::u32string& pattern,
                                                  const Options& options, std::string* error) {
    if (pattern.empty()) return nullptr;
    auto impl = std::make_shared<Query::Impl>();
    impl->opt = options;
    if (options.regex) {
        auto flags = std::regex_constants::ECMAScript;
        if (!options.caseSensitive) flags |= std::regex_constants::icase;
        try {
            impl->re.reset(new Regex(std::wstring(pattern.begin(), pattern.end()), flags));
        } catch (const std::regex_error& e) {
            *error = std::string("Invalid regular expression: ") + e.what();
            return nullptr;
        }
    } else {
        impl->needle.reserve(pattern.size());
        for (char32_t c : pattern)
            impl->needle.push_back(static_cast<wchar_t>(options.caseSensitive ? c : foldCase(c)));
    }
    return impl;
}

Query Query::make(const std::string& pattern, const Options& options) {
    Query q;
    q.options_ = options;
    q.impl_ = compile(toU32(pattern), options, &q.error_);
    return q;
}

Query Query::make(const std::u16string& pattern, const Options& options) {
    Query q;
    q.options_ = options;
    q.impl_ = compile(toU32(pattern), options, &q.error_);
    return q;
}

namespace {

// A match in code points of one line.
struct CpMatch {
    std::size_t start, length;
    std::vector<std::pair<std::size_t, std::size_t>> groups;
};

bool wholeWordAt(const std::wstring& l, std::size_t s, std::size_t len) {
    const std::size_t e = s + len;
    auto word = [&](std::size_t i) { return isWordChar(static_cast<char32_t>(l[i])); };
    if (s > 0 && word(s - 1) && (len == 0 || word(s))) return false;
    if (e < l.size() && word(e) && (len == 0 || word(e - 1))) return false;
    return true;
}

// Every match of `q` in one line of code points, at most `limit` (0 = all).
// A regex on a line longer than kSteppedLine gives up after about a second,
// or as soon as `cancel` is raised, keeping what it found.
void matchLine(const Query::Impl& q, const std::wstring& line, std::vector<CpMatch>& out,
               std::size_t limit, const std::atomic<bool>* cancel = nullptr) {
    const std::size_t n = line.size();
    auto full = [&] { return limit && out.size() >= limit; };
    if (!q.re) {
        const std::wstring& needle = q.needle;
        const std::size_t m = needle.size();
        if (m == 0 || n < m) return;
        std::wstring folded;
        const std::wstring* hay = &line;
        if (!q.opt.caseSensitive) {
            folded.resize(n);
            for (std::size_t i = 0; i < n; ++i)
                folded[i] = static_cast<wchar_t>(foldCase(static_cast<char32_t>(line[i])));
            hay = &folded;
        }
        std::size_t pos = 0;
        while (pos + m <= n && !full()) {
            const std::size_t at = hay->find(needle, pos);
            if (at == std::wstring::npos) break;
            if (q.opt.wholeWord && !wholeWordAt(line, at, m)) {
                pos = at + 1;
                continue;
            }
            out.push_back({at, m, {{at, m}}});
            pos = at + m;
        }
        return;
    }
    // Matches one after another, stepping past an empty match as
    // std::regex_iterator and JavaScript do: the next search starts where a
    // match ended, and after an empty one first asks for a non-empty match
    // there, then searches from one further on. Over pointers, not string
    // iterators: libc++ copies the rest of the line for every search through
    // any other iterator type, which made a line with many matches
    // quadratic. A search from inside the line has the character before it
    // (\b sees it) and is not at the line's start (^ does not match there).
    // Ignoring case, the regex runs over the line folded to lower case
    // (one code point for one, so positions hold). libstdc++ tests a
    // bracket range such as [а-я] through the locale's ctype, not the
    // traits, and the C locale folds only ASCII; folded text puts the
    // character itself in the lower-case range.
    namespace rc = std::regex_constants;
    using Results = std::match_results<const wchar_t*>;
    std::wstring foldedLine;
    const std::wstring* subject = &line;
    if (!q.opt.caseSensitive) {
        foldedLine.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            foldedLine[i] = static_cast<wchar_t>(foldCase(static_cast<char32_t>(line[i])));
        subject = &foldedLine;
    }
    const wchar_t* const b = subject->data();
    const wchar_t* const e = b + n;
    const bool stepped = n > kSteppedLine;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(kRegexLineSeconds +
                                                            kRegexSecondsPerChar * (double)n));
    bool stop = false;
    // From inside the line: match_prev_avail (\b looks at the character
    // before), and ^ must not match. libstdc++ takes that from
    // match_prev_avail itself (or match_not_bol); libc++ ignores
    // match_not_bol beside it and lets ^ match at the start of any search,
    // except one flagged with its own __no_update_pos, which is how its
    // regex_iterator searches on from a match.
    auto flagsAt = [](std::size_t p) {
        if (p == 0) return rc::match_default;
#ifdef _LIBCPP_VERSION
        return rc::match_prev_avail | rc::match_not_bol | rc::__no_update_pos;
#else
        return rc::match_prev_avail | rc::match_not_bol;
#endif
    };
    // The first match starting at or after `from` (not an empty one at
    // `from` when notNull), into m.
    auto next = [&](std::size_t from, bool notNull, Results& m) -> bool {
        if (!stepped) {
            if (notNull) {
                if (std::regex_search(b + from, e, m, *q.re,
                                      flagsAt(from) | rc::match_not_null | rc::match_continuous))
                    return true;
                if (from >= n) return false;
                ++from;
            }
            return std::regex_search(b + from, e, m, *q.re, flagsAt(from));
        }
        for (std::size_t p = from; p <= n; ++p) {
            // Before every attempt: one can itself cost a pass over the line.
            if ((cancel && cancel->load(std::memory_order_relaxed)) ||
                std::chrono::steady_clock::now() > deadline) {
                stop = true;
                return false;
            }
            auto f = flagsAt(p) | rc::match_continuous;
            if (p == from && notNull) f |= rc::match_not_null;
            if (std::regex_search(b + p, e, m, *q.re, f)) return true;
        }
        return false;
    };
    try {
        Results mr;
        std::size_t from = 0;
        bool notNull = false;
        while (from <= n && !full() && !stop && next(from, notNull, mr)) {
            const std::size_t s = static_cast<std::size_t>(mr[0].first - b);
            const std::size_t len = static_cast<std::size_t>(mr[0].second - mr[0].first);
            from = s + len;
            notNull = len == 0;
            if (q.opt.wholeWord && !wholeWordAt(line, s, len)) continue;
            CpMatch cm{s, len, {}};
            for (std::size_t k = 0; k < mr.size(); ++k) {
                if (mr[k].matched)
                    cm.groups.push_back({static_cast<std::size_t>(mr[k].first - b),
                                         static_cast<std::size_t>(mr[k].second - mr[k].first)});
                else
                    cm.groups.push_back({npos, 0});
            }
            out.push_back(std::move(cm));
        }
    } catch (const std::exception&) {
        // Too complex for std::regex: this line has no more.
    }
}

// Code point matches to matches in the line's units, offset by `base`.
void toUnits(const std::vector<CpMatch>& cms, const Line& l, std::size_t base,
             std::vector<Match>& out) {
    for (const CpMatch& c : cms) {
        Match m;
        m.start = base + l.offs[c.start];
        m.length = l.offs[c.start + c.length] - l.offs[c.start];
        for (const auto& g : c.groups) {
            if (g.first == npos) m.groups.push_back({npos, 0});
            else
                m.groups.push_back({base + l.offs[g.first],
                                    l.offs[g.first + g.second] - l.offs[g.first]});
        }
        out.push_back(std::move(m));
    }
}

template <class S>
S expandT(const S& tmpl, const S& text, const Match& m, bool regex) {
    using Ch = typename S::value_type;
    if (!regex) return tmpl;
    S out;
    auto group = [&](std::size_t k) {
        if (k >= m.groups.size() || m.groups[k].first == npos) return;
        out.append(text, m.groups[k].first, m.groups[k].second);
    };
    const std::size_t n = tmpl.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Ch c = tmpl[i];
        if (c == Ch('$') && i + 1 < n) {
            const Ch d = tmpl[i + 1];
            if (d == Ch('$')) { out.push_back(Ch('$')); ++i; continue; }
            if (d == Ch('&')) { group(0); ++i; continue; }
            if (d >= Ch('0') && d <= Ch('9')) {
                std::size_t k = static_cast<std::size_t>(d - Ch('0'));
                std::size_t used = 1;
                if (i + 2 < n && tmpl[i + 2] >= Ch('0') && tmpl[i + 2] <= Ch('9')) {
                    const std::size_t two = k * 10 + static_cast<std::size_t>(tmpl[i + 2] - Ch('0'));
                    if (two < m.groups.size()) { k = two; used = 2; }
                }
                if (k < m.groups.size()) {
                    group(k);
                    i += used;
                    continue;
                }
            }
        } else if (c == Ch('\\') && i + 1 < n) {
            const Ch d = tmpl[i + 1];
            if (d == Ch('n')) { out.push_back(Ch('\n')); ++i; continue; }
            if (d == Ch('t')) { out.push_back(Ch('\t')); ++i; continue; }
            if (d == Ch('\\')) { out.push_back(Ch('\\')); ++i; continue; }
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

// ----------------------------------------------------------------- UTF-16
std::vector<Match> findAll(const std::u16string& text, const Query& query, std::size_t limit,
                           bool* truncated, const std::atomic<bool>* cancel, bool* cancelled) {
    std::vector<Match> out;
    if (truncated) *truncated = false;
    if (cancelled) *cancelled = false;
    const Query::Impl* q = query.impl();
    if (!q) return out;
    Line l;
    std::vector<CpMatch> cms;
    std::size_t pos = 0, lines = 0;
    const std::size_t n = text.size();
    while (true) {
        if (cancel && (++lines & 255) == 0 && cancel->load(std::memory_order_relaxed)) {
            if (cancelled) *cancelled = true;
            break;
        }
        std::size_t next;
        const std::size_t end = lineEnd(text, pos, &next);
        decode16(text.data() + pos, end - pos, l);
        cms.clear();
        matchLine(*q, l.cps, cms, limit ? limit - out.size() : 0, cancel);
        toUnits(cms, l, pos, out);
        if (cancel && l.cps.size() > kSteppedLine && cancel->load(std::memory_order_relaxed)) {
            if (cancelled) *cancelled = true;
            break;
        }
        if (limit && out.size() >= limit) {
            if (truncated) *truncated = true;
            break;
        }
        if (next >= n) {
            // A text ending in a line break has an empty last line (where ^
            // and $ still match).
            if (end < n) {
                pos = n;
                continue;
            }
            break;
        }
        pos = next;
    }
    return out;
}

std::u16string expand(const std::u16string& replacement, const std::u16string& text,
                      const Match& match, const Query& query) {
    return expandT(replacement, text, match, query.options().regex);
}

std::vector<Edit16> replaceAll(const std::u16string& text, const Query& query,
                               const std::u16string& replacement) {
    std::vector<Edit16> edits;
    for (const Match& m : findAll(text, query))
        edits.push_back({m.start, m.length, expand(replacement, text, m, query)});
    return edits;
}

std::u16string applyEdits(const std::u16string& text, const std::vector<Edit16>& edits) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t at = 0;
    for (const Edit16& e : edits) {
        if (e.start < at || e.start + e.length > text.size()) continue;   // overlapping
        out.append(text, at, e.start - at);
        out += e.text;
        at = e.start + e.length;
    }
    out.append(text, at, std::u16string::npos);
    return out;
}

long nextIndex(const std::vector<Match>& matches, std::size_t selStart, std::size_t selEnd,
               bool forward) {
    const long n = static_cast<long>(matches.size());
    if (n == 0) return -1;
    const long here = indexOf(matches, selStart, selEnd - selStart);
    if (here >= 0) return forward ? (here + 1) % n : (here - 1 + n) % n;
    if (forward) {
        for (long i = 0; i < n; ++i)
            if (matches[static_cast<std::size_t>(i)].start >= selEnd) return i;
        return 0;
    }
    for (long i = n - 1; i >= 0; --i)
        if (matches[static_cast<std::size_t>(i)].start < selStart) return i;
    return n - 1;
}

long indexOf(const std::vector<Match>& matches, std::size_t start, std::size_t length) {
    auto it = std::lower_bound(matches.begin(), matches.end(), start,
                               [](const Match& m, std::size_t s) { return m.start < s; });
    for (; it != matches.end() && it->start == start; ++it)
        if (it->length == length) return static_cast<long>(it - matches.begin());
    return -1;
}

bool replaceOneAt(const std::u16string& text, const Query& query,
                  const std::u16string& replacement, int line, std::size_t byteColumn,
                  Edit16* out) {
    if (!query.usable() || line < 1) return false;
    std::size_t pos = 0, next = 0, end = 0;
    for (int k = 1;; ++k) {
        end = lineEnd(text, pos, &next);
        if (k == line) break;
        if (next >= text.size()) {
            if (end < text.size() && k + 1 == line) {
                pos = end = next = text.size();
                break;
            }
            return false;
        }
        pos = next;
    }
    // The byte column in UTF-16 units: UTF-8 lengths summed along the line.
    std::size_t at = pos, bytes = 0;
    while (bytes < byteColumn && at < end) {
        const char16_t c = text[at];
        if (c >= 0xD800 && c <= 0xDBFF && at + 1 < end) { bytes += 4; at += 2; }
        else { bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : 3; at += 1; }
    }
    if (bytes != byteColumn) return false;
    const std::u16string lineText = text.substr(pos, end - pos);
    for (const Match& m : findAll(lineText, query)) {
        if (m.start + pos != at) continue;
        out->start = at;
        out->length = m.length;
        out->text = expand(replacement, lineText, m, query);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ UTF-8
std::vector<Match> findAllInLine(const std::string& line, const Query& query) {
    std::vector<Match> out;
    const Query::Impl* q = query.impl();
    if (!q) return out;
    Line l;
    decode8(line.data(), line.size(), l);
    std::vector<CpMatch> cms;
    matchLine(*q, l.cps, cms, 0);
    toUnits(cms, l, 0, out);
    return out;
}

bool findInLine(const std::string& line, const Query& query, std::size_t* byteColumn,
                std::size_t* byteLength) {
    const Query::Impl* q = query.impl();
    if (!q) return false;
    Line l;
    decode8(line.data(), line.size(), l);
    std::vector<CpMatch> cms;
    matchLine(*q, l.cps, cms, 1);
    if (cms.empty()) return false;
    *byteColumn = l.offs[cms[0].start];
    *byteLength = l.offs[cms[0].start + cms[0].length] - *byteColumn;
    return true;
}

std::string replaceInText(const std::string& content, const Query& query,
                          const std::string& replacement, std::size_t* count) {
    std::size_t total = 0;
    std::string out;
    if (!query.usable()) {
        if (count) *count = 0;
        return content;
    }
    out.reserve(content.size());
    const std::size_t n = content.size();
    std::size_t pos = 0;
    while (true) {
        std::size_t next;
        const std::size_t end = lineEnd(content, pos, &next);
        const std::string line = content.substr(pos, end - pos);
        std::size_t at = 0;
        for (const Match& m : findAllInLine(line, query)) {
            out.append(line, at, m.start - at);
            out += expandT(replacement, line, m, query.options().regex);
            at = m.start + m.length;
            ++total;
        }
        out.append(line, at, std::string::npos);
        out.append(content, end, next - end);   // the line break as it was
        if (next >= n) {
            if (end < n) {   // the empty line after a final line break
                pos = n;
                continue;
            }
            break;
        }
        pos = next;
    }
    if (count) *count = total;
    return out;
}

bool replaceOneInText(const std::string& content, const Query& query,
                      const std::string& replacement, int line, std::size_t byteColumn,
                      std::string* out) {
    if (!query.usable() || line < 1) return false;
    std::size_t pos = 0, next = 0, end = 0;
    for (int k = 1;; ++k) {
        end = lineEnd(content, pos, &next);
        if (k == line) break;
        if (next >= content.size()) {
            if (end < content.size() && k + 1 == line) {   // the empty last line
                pos = end = next = content.size();
                break;
            }
            return false;
        }
        pos = next;
    }
    const std::string text = content.substr(pos, end - pos);
    for (const Match& m : findAllInLine(text, query)) {
        if (m.start != byteColumn) continue;
        std::string r = content.substr(0, pos + m.start);
        r += expandT(replacement, text, m, query.options().regex);
        r.append(content, pos + m.start + m.length, std::string::npos);
        *out = std::move(r);
        return true;
    }
    return false;
}

}  // namespace TextFind
