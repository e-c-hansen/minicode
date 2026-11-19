// TextScan.cpp — see TextScan.h.
#include "TextScan.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace TextScan {

namespace {

constexpr std::size_t npos = std::string::npos;

inline unsigned char asciiLower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

void appendUtf16(std::u16string& out, char32_t c) {
    if (c < 0x10000) {
        out += static_cast<char16_t>(c);
    } else {
        c -= 0x10000;
        out += static_cast<char16_t>(0xD800 | (c >> 10));
        out += static_cast<char16_t>(0xDC00 | (c & 0x3FF));
    }
}

// UTF-16 units in n bytes of valid UTF-8: one per character, two for a
// four-byte one.
std::size_t unitsOf(const char* s, std::size_t n) {
    std::size_t u = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        u += (c & 0xC0) != 0x80;
        u += c >= 0xF0;
    }
    return u;
}

// The byte where unit `units` begins (clamped to n).
std::size_t byteAtUnit(const char* s, std::size_t n, std::size_t units) {
    std::size_t u = 0, i = 0;
    while (i < n && u < units) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const std::size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        u += len == 4 ? 2 : 1;
        i += len;
    }
    return std::min(i, n);
}

// A line of UTF-8 (valid) or UTF-16 as UTF-16 units, for TextFind.
void unitsOfLine(const char* s, std::size_t n, bool ascii, std::u16string& out) {
    out.clear();
    out.reserve(n);
    if (ascii) {
        for (std::size_t i = 0; i < n; ++i) out += static_cast<char16_t>(static_cast<unsigned char>(s[i]));
        return;
    }
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        std::size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; len = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        if (i + len > n) len = n - i;
        for (std::size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        appendUtf16(out, cp);
        i += len;
    }
}
void unitsOfLine(const char16_t* s, std::size_t n, bool, std::u16string& out) { out.assign(s, n); }

// Line breaks: \n, \r\n, \r and U+2029, as TextFind and FolderSearch end lines.
// Whether a line starts at s[p] (n the units there are).
bool lineStartsAt(const char* s, std::size_t n, std::size_t p) {
    if (p == 0) return true;
    const char c = s[p - 1];
    if (c == '\n') return true;
    if (c == '\r') return p >= n || s[p] != '\n';
    return p >= 3 && c == '\xA9' && s[p - 2] == '\x80' && s[p - 3] == '\xE2';
}
bool lineStartsAt(const char16_t* s, std::size_t n, std::size_t p) {
    if (p == 0) return true;
    const char16_t c = s[p - 1];
    if (c == u'\n' || c == 0x2029) return true;
    if (c == u'\r') return p >= n || s[p] != u'\n';
    return false;
}

// Where the line holding s[p] ends (its break left off) and the next begins.
template <class T>
void lineEndAt(const T* s, std::size_t n, std::size_t p, std::size_t* end, std::size_t* next) {
    for (std::size_t e = p; e < n; ++e) {
        const T c = s[e];
        if (c == T('\n')) { *end = e; *next = e + 1; return; }
        if (c == T('\r')) {
            *end = e;
            *next = (e + 1 < n && s[e + 1] == T('\n')) ? e + 2 : e + 1;
            return;
        }
        if constexpr (std::is_same<T, char16_t>::value) {
            if (c == 0x2029) { *end = e; *next = e + 1; return; }
        } else {
            if (c == '\xE2' && e + 2 < n && s[e + 1] == '\x80' && s[e + 2] == '\xA9') {
                *end = e;
                *next = e + 3;
                return;
            }
        }
    }
    *end = *next = n;
}

// The last place in (lo, hi] where a line starts, else 0; one past hi when
// hi falls between \r and \n.
template <class T>
std::size_t lastLineStart(const T* s, std::size_t lo, std::size_t hi, std::size_t n) {
    for (std::size_t e = hi; e > lo; --e) {
        if (s[e - 1] == T('\r') && e < n && s[e] == T('\n')) return e + 1;
        if (lineStartsAt(s, n, e)) return e;
    }
    return 0;
}

enum class Mode { Direct, Verify, Block };

// Matches in one block of whole lines (units T), shifted to the text's
// units and passed on while they start in [from, end).
template <class T>
struct BlockScan {
    const TextFind::Query& query;
    const Needle& needle;
    Mode mode;
    std::size_t from, end;   // end SIZE_MAX: through the text's end
    bool ascii;              // T is char and every byte is a unit
    const std::function<bool(const TextFind::Match&)>& found;
    const std::atomic<bool>* cancel;
    bool done = false, cancelled = false;
    std::u16string line;

    BlockScan(const TextFind::Query& q, const Needle& nd, Mode md, std::size_t f, std::size_t e,
              bool a, const std::function<bool(const TextFind::Match&)>& fn,
              const std::atomic<bool>* c)
        : query(q), needle(nd), mode(md), from(f), end(e), ascii(a), found(fn), cancel(c) {}

    // Units from the block's start to s[p], counted on from the last ask.
    const T* base = nullptr;
    std::size_t at = 0, atUnits = 0;
    std::size_t unitsTo(std::size_t p) {
        if constexpr (std::is_same<T, char16_t>::value) {
            return p;
        } else {
            if (ascii) return p;
            if (p < at) at = atUnits = 0;
            atUnits += unitsOf(base + at, p - at);
            at = p;
            return atUnits;
        }
    }

    bool stopNow() {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            cancelled = true;
            return true;
        }
        return false;
    }

    // False to stop the scan.
    bool emit(TextFind::Match m, std::size_t shift) {
        m.start += shift;
        if (m.start >= end) {
            done = true;
            return false;
        }
        if (m.start < from) return true;
        for (auto& g : m.groups)
            if (g.first != npos) g.first += shift;
        if (!found(m)) {
            done = true;
            return false;
        }
        return true;
    }

    // False when the scan ends (done, or cancelled).
    bool run(const T* s, std::size_t n, std::size_t unitStart, bool last) {
        base = s;
        at = atUnits = 0;
        std::size_t hits = 0;
        if (mode == Mode::Direct) {
            const std::size_t m =
                std::is_same<T, char16_t>::value ? needle.unitLength() : needle.byteLength();
            const std::size_t units = needle.unitLength();
            for (std::size_t p = needle.find(s, n, 0); p != npos; p = needle.find(s, n, p + m)) {
                const std::size_t u = unitsTo(p);
                TextFind::Match mt;
                mt.start = u;
                mt.length = units;
                mt.groups.push_back({u, units});
                if (!emit(std::move(mt), unitStart)) return false;
                if ((++hits & 4095) == 0 && stopNow()) return false;
            }
            return true;
        }
        if (mode == Mode::Verify) {
            std::size_t p = 0;   // a line's start
            while (p < n) {
                const std::size_t hit = needle.find(s, n, p);
                if (hit == npos) break;
                std::size_t ls = hit;
                while (ls > p && !lineStartsAt(s, n, ls)) --ls;
                std::size_t le, next;
                lineEndAt(s, n, hit, &le, &next);
                unitsOfLine(s + ls, le - ls, ascii, line);
                const std::size_t shift = unitStart + unitsTo(ls);
                for (TextFind::Match& mt : TextFind::findAll(line, query))
                    if (!emit(std::move(mt), shift)) return false;
                p = next;
                if ((++hits & 1023) == 0 && stopNow()) return false;
            }
            return true;
        }
        // No needle: every line of the block, through TextFind.
        unitsOfLine(s, n, ascii, line);
        bool stopped = false;
        std::vector<TextFind::Match> all = TextFind::findAll(line, query, 0, nullptr, cancel, &stopped);
        if (stopped) {
            cancelled = true;
            return false;
        }
        for (TextFind::Match& mt : all) {
            // After the block's last break findAll sees an empty line the
            // text has only at its very end (the next block starts there).
            if (!last && mt.start == line.size()) continue;
            if (!emit(std::move(mt), unitStart)) return false;
        }
        return true;
    }
};

}  // namespace

// ------------------------------------------------------------ the needle
Needle Needle::make(const std::u32string& text, bool caseSensitive) {
    Needle nd;
    for (char32_t c : text)
        if (c == U'\n' || c == U'\r' || c == 0x2029) {
            nd.never_ = true;
            return nd;
        }
    if (caseSensitive) {
        for (char32_t c : text) {
            appendUtf8(nd.bytes_, c);
            appendUtf16(nd.units_, c);
        }
        nd.whole_ = !text.empty();
    } else {
        std::u32string run, best;
        for (char32_t c : text) {
            if (c < 0x80) {
                run += c;
                if (run.size() > best.size()) best = run;
            } else {
                run.clear();
            }
        }
        bool letters = false;
        for (char32_t c : best) {
            const unsigned char b = asciiLower(static_cast<unsigned char>(c));
            letters = letters || (b >= 'a' && b <= 'z');
            nd.bytes_ += static_cast<char>(b);
            nd.units_ += static_cast<char16_t>(b);
        }
        nd.fold_ = letters;   // no letter: nothing to fold, memchr does
        nd.whole_ = !best.empty() && best.size() == text.size();
    }
    return nd;
}

namespace {

// Sixteen bytes compared at once (GCC's and Clang's vector extensions:
// NEON on Apple Silicon and ARM, SSE2 on x86, plain code elsewhere).
typedef unsigned char V8x16 __attribute__((vector_size(16)));
typedef unsigned short V16x8 __attribute__((vector_size(16)));

template <class V, class T>
inline V loadVec(const T* p) {
    V v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <class V, class T>
inline V splat(T x) {
    V v;
    for (std::size_t k = 0; k < sizeof v / sizeof(T); ++k) v[k] = x;
    return v;
}

// A unit as matched: ASCII capitals to small when folding.
template <class T>
inline T unitAt(const T* s, std::size_t i, bool fold) {
    const T c = s[i];
    return (fold && c >= T('A') && c <= T('Z')) ? T(c + 32) : c;
}

// The first occurrence of b[0, m) (m >= 2; folded to small letters when
// `fold`) in s[from, n): sixteen bytes at a time, the places where both the
// needle's first and last units are found, then the rest compared (the
// generic SIMD substring search). A letter is matched in either case by
// setting 0x20, which only its own capital shares with it.
template <class V, class T>
std::size_t findWide(const T* s, std::size_t n, std::size_t from, const T* b, std::size_t m,
                     bool fold) {
    constexpr std::size_t lanes = sizeof(V) / sizeof(T);
    auto letter = [](T c) { return c >= T('a') && c <= T('z'); };
    const T f = b[0], l = b[m - 1];
    const T orF = fold && letter(f) ? T(0x20) : T(0), orL = fold && letter(l) ? T(0x20) : T(0);
    const V vf = splat<V>(f), vl = splat<V>(l), of = splat<V>(orF), ol = splat<V>(orL);
    auto rest = [&](std::size_t i) {
        for (std::size_t k = 1; k + 1 < m; ++k)
            if (unitAt(s, i + k, fold) != b[k]) return false;
        return true;
    };
    std::size_t i = from;
    for (; i + m - 1 + lanes <= n; i += lanes) {
        const V first = loadVec<V>(s + i), last = loadVec<V>(s + i + m - 1);
        const auto hit = ((first | of) == vf) & ((last | ol) == vl);
        std::uint64_t w[2];
        std::memcpy(w, &hit, sizeof w);
        if (!(w[0] | w[1])) continue;
        for (std::size_t k = 0; k < lanes; ++k)
            if (hit[k] && rest(i + k)) return i + k;
    }
    for (; i + m <= n; ++i)
        if (unitAt(s, i, fold) == f && unitAt(s, i + m - 1, fold) == l && rest(i)) return i;
    return npos;
}

}  // namespace

std::size_t Needle::find(const char* s, std::size_t n, std::size_t from) const {
    const std::size_t m = bytes_.size();
    if (m == 0 || from > n || n - from < m) return npos;
    const unsigned char* u = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* b = reinterpret_cast<const unsigned char*>(bytes_.data());
    if (m == 1) {
        if (!fold_) {
            const void* p = std::memchr(s + from, b[0], n - from);
            return p ? static_cast<std::size_t>(static_cast<const char*>(p) - s) : npos;
        }
        for (std::size_t i = from; i < n; ++i)
            if (asciiLower(u[i]) == b[0]) return i;
        return npos;
    }
    return findWide<V8x16>(u, n, from, b, m, fold_);
}

std::size_t Needle::find(const char16_t* s, std::size_t n, std::size_t from) const {
    const std::size_t m = units_.size();
    if (m == 0 || from > n || n - from < m) return npos;
    const unsigned short* u = reinterpret_cast<const unsigned short*>(s);
    const unsigned short* b = reinterpret_cast<const unsigned short*>(units_.data());
    if (m == 1) {
        for (std::size_t i = from; i < n; ++i)
            if (unitAt(u, i, fold_) == b[0]) return i;
        return npos;
    }
    return findWide<V16x8>(u, n, from, b, m, fold_);
}

// ------------------------------------------------------------ the text
Text Text::utf8(const char* bytes, std::size_t n, bool ascii) {
    Text t;
    t.bytes = bytes;
    t.byteCount = n;
    t.ascii = ascii;
    t.length = ascii ? n : unitsOf(bytes, n);
    return t;
}

Text Text::utf16(std::size_t length, TextFind::Reader read) {
    Text t;
    t.length = length;
    t.read = std::move(read);
    return t;
}

// ------------------------------------------------------------ scanning
bool usesNeedle(const TextFind::Query& query) {
    if (!query.usable()) return false;
    const Needle nd = Needle::make(TextFind::requiredText(query), query.options().caseSensitive);
    return !nd.empty();
}

namespace {

// Where the line holding unit `pos` of a text read through `read` begins.
std::size_t lineStartOf(const Text& t, std::size_t pos) {
    std::u16string buf;
    const std::size_t len = t.length;
    pos = std::min(pos, len);
    while (pos > 0) {
        const std::size_t lo = pos > 4096 ? pos - 4096 : 0;
        const std::size_t hi = std::min(pos + 1, len);   // the unit after: \r\n
        buf.resize(hi - lo);
        t.read(lo, hi - lo, &buf[0]);
        for (std::size_t p = pos; p > lo; --p) {
            const char16_t c = buf[p - 1 - lo];
            if (c == u'\n' || c == 0x2029) return p;
            if (c == u'\r' && (p >= len || buf[p - lo] != u'\n')) return p;
        }
        pos = lo;
    }
    return 0;
}

}  // namespace

bool scan(const Text& text, const TextFind::Query& query, std::size_t from, std::size_t to,
          const std::function<bool(const TextFind::Match&)>& found, const Options& options) {
    if (!query.usable()) return true;
    const Needle needle =
        Needle::make(TextFind::requiredText(query), query.options().caseSensitive);
    if (needle.never()) return true;
    const TextFind::Options& o = query.options();
    const Mode mode = needle.empty()                                   ? Mode::Block
                      : (!o.regex && !o.wholeWord && needle.whole()) ? Mode::Direct
                                                                       : Mode::Verify;
    const std::size_t len = text.length;
    const std::size_t end = to >= len ? SIZE_MAX : to;
    if (from > len || from >= end) return true;
    const std::size_t block = std::max<std::size_t>(options.blockUnits, 1);

    if (text.bytes || len == 0) {
        const char* s = text.bytes ? text.bytes : "";
        const std::size_t n = text.bytes ? text.byteCount : 0;
        BlockScan<char> bs(query, needle, mode, from, end, text.ascii || n == 0, found,
                           options.cancel);
        std::size_t p = text.ascii ? std::min(from, n) : byteAtUnit(s, n, from);
        while (!lineStartsAt(s, n, p)) --p;
        std::size_t units = text.ascii ? p : unitsOf(s, p);
        for (;;) {
            std::size_t cut = n;
            if (p < n && n - p > block) {
                cut = lastLineStart(s, p, p + block, n);
                if (cut == 0) {   // a line longer than a block: all of it
                    std::size_t e;
                    lineEndAt(s, n, p, &e, &cut);
                }
            }
            const bool last = cut >= n;
            if (units >= end) return true;
            if (!bs.run(s + p, cut - p, units, last)) return !bs.cancelled;
            if (bs.stopNow()) return false;
            units += text.ascii ? cut - p : unitsOf(s + p, cut - p);
            p = cut;
            if (last) return true;
        }
    }

    BlockScan<char16_t> bs(query, needle, mode, from, end, true, found, options.cancel);
    std::size_t p = lineStartOf(text, from);
    std::u16string buf;
    for (;;) {
        std::size_t cut = len;
        if (p < len) {
            std::size_t want = std::min(block, len - p);
            for (;;) {
                const std::size_t got = std::min(want + 1, len - p);
                buf.resize(got);
                text.read(p, got, &buf[0]);
                if (p + want >= len) {
                    cut = len;
                    break;
                }
                const std::size_t e = lastLineStart(buf.data(), 0, want, got);
                if (e) {
                    cut = p + e;
                    break;
                }
                want = std::min(want * 2, len - p);
            }
            buf.resize(cut - p);
        } else {
            buf.clear();
        }
        const bool last = cut >= len;
        if (p >= end) return true;
        if (!bs.run(buf.data(), buf.size(), p, last)) return !bs.cancelled;
        if (bs.stopNow()) return false;
        p = cut;
        if (last) return true;
    }
}

bool findFirst(const Text& text, const TextFind::Query& query, std::size_t from, std::size_t to,
               TextFind::Match* out, const Options& options, bool* cancelled) {
    bool got = false;
    const bool finished = scan(text, query, from, to, [&](const TextFind::Match& m) {
        *out = m;
        got = true;
        return false;
    }, options);
    if (cancelled) *cancelled = !finished;
    return got;
}

bool findLast(const Text& text, const TextFind::Query& query, std::size_t from, std::size_t to,
              TextFind::Match* out, const Options& options, bool* cancelled) {
    if (cancelled) *cancelled = false;
    const std::size_t block = std::max<std::size_t>(options.blockUnits, 1);
    std::size_t e = to;   // past the end on the first round: a match at the very end counts
    std::size_t hi = std::min(to, text.length);
    while (hi >= from) {
        const std::size_t lo = hi - from > block ? hi - block : from;
        bool got = false;
        const bool finished = scan(text, query, lo, e, [&](const TextFind::Match& m) {
            *out = m;
            got = true;
            return true;
        }, options);
        if (!finished) {
            if (cancelled) *cancelled = true;
            return false;
        }
        if (got) return true;
        if (lo == from) return false;
        e = hi = lo;
    }
    return false;
}

}  // namespace TextScan
