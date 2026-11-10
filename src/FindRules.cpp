// FindRules.cpp — see FindRules.h.
#include "FindRules.h"

#include "EditSpans.h"
#include "MultiCaret.h"

#include <algorithm>
#include <cstring>

namespace FindRules {

namespace {

bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// UTF-16 as UTF-8; a lone surrogate becomes U+FFFD.
std::string utf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (isHigh(s[i]) && i + 1 < s.size() && isLow(s[i + 1])) {
            c = 0x10000 + (((char32_t)s[i] - 0xD800) << 10) + ((char32_t)s[i + 1] - 0xDC00);
            i++;
        } else if (isHigh(s[i]) || isLow(s[i])) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Code points in a UTF-16 string.
std::size_t pointsIn(const std::u16string& s) {
    std::size_t n = s.size();
    for (std::size_t i = 0; i + 1 < s.size(); i++)
        if (isHigh(s[i]) && isLow(s[i + 1])) {
            n--;
            i++;
        }
    return n;
}

// NSCharacterSet's newlineCharacterSet, which the Mac's seed looks for.
bool isLineBreak(char16_t c) {
    return (c >= 0x0A && c <= 0x0D) || c == 0x85 || c == 0x2028 || c == 0x2029;
}

bool inside(std::size_t start, std::size_t length, std::size_t scopeStart, std::size_t scopeEnd) {
    return start >= scopeStart && start + length <= scopeEnd;
}

}  // namespace

// ------------------------------------------------------------ the query

std::u16string escapeRegex(const std::u16string& text) {
    static const char16_t kSpecial[] = u"\\{}*+?|^$.[]()";
    std::u16string out;
    out.reserve(text.size() + 8);
    for (char16_t c : text) {
        if (std::char_traits<char16_t>::find(kSpecial, 14, c)) out += u'\\';
        out += c;
    }
    return out;
}

Seed seed(const std::u16string& text, std::size_t selStart, std::size_t selEnd, bool regex) {
    Seed s;
    if (selStart > selEnd) std::swap(selStart, selEnd);
    if (selEnd > text.size()) return s;
    if (selStart == selEnd) {
        const MultiCaret::Caret c = MultiCaret::wordAt(text, selStart);
        if (c.empty()) return s;
        s.start = c.lo();
        s.end = c.hi();
        s.word = true;
    } else {
        if (selEnd - selStart >= kMaxSeed) return s;
        for (std::size_t i = selStart; i < selEnd; i++)
            if (isLineBreak(text[i])) return s;
        s.start = selStart;
        s.end = selEnd;
    }
    s.query = text.substr(s.start, s.end - s.start);
    if (regex) s.query = escapeRegex(s.query);
    s.found = true;
    return s;
}

// ------------------------------------------------------------ the count

Count count(const CountState& st) {
    Count c;
    if (!st.hasQuery) {
        c.text = "No results";
        return c;
    }
    if (!st.queryError.empty()) {
        c.text = "No results";
        c.error = true;
        c.tip = st.queryError;
        return c;
    }
    if (!st.current) {
        // The old count stands while the edited text is searched again; a
        // new query has no count yet.
        if (st.searched) c.keep = true;
        else c.text = "…";
        return c;
    }
    const std::size_t n = st.matches;
    c.navigable = n > 0;
    if (n == 0 && st.tooSlow) {
        c.text = "Too slow";
        c.error = true;
        c.tip = "This pattern took too long. One with a backreference or a lookahead runs on a "
                "slower engine, which stops after two seconds.";
        return c;
    }
    if (n == 0) {
        c.text = "No results";
        c.noResults = true;
        return c;
    }
    const std::string total = std::to_string(n) + (st.truncated ? "+" : "");
    c.text = st.currentIndex >= 0 ? std::to_string(st.currentIndex + 1) + " of " + total
                                  : "? of " + total;
    if (st.truncated) c.tip = "Only the first 20,000 matches are counted and highlighted";
    return c;
}

// ------------------------------------------------------------ searching

std::size_t scopeEnd(std::size_t start, std::size_t end, char32_t last) {
    if (end < start) return start;
    if (end - start > 1 && last == U'\n') end--;
    return end;
}

std::size_t scopeEnd(const std::u16string& text, std::size_t start, std::size_t end) {
    if (end > text.size() || end <= start) return start;
    return scopeEnd(start, end, text[end - 1]);
}

Found findAll(const std::u16string& text, const TextFind::Query& query, std::size_t scopeStart,
              std::size_t scopeEnd, const std::atomic<bool>* cancel) {
    Found f;
    if (!query.usable()) return f;
    const bool scoped = scopeEnd > scopeStart;
    bool truncated = false, cancelled = false;
    std::vector<TextFind::Match> units = TextFind::findAll(
        text, query, scoped ? 0 : kMatchLimit, &truncated, cancel, &cancelled);
    if (cancelled) {
        f.cancelled = true;
        return f;
    }
    // Starts and ends, ascending: matches never overlap and come in order.
    std::vector<std::size_t> at;
    at.reserve(units.size() * 2);
    for (const TextFind::Match& m : units) {
        at.push_back(m.start);
        at.push_back(m.start + m.length);
    }
    const std::vector<std::size_t> cp = EditSpans::codePoints(text, at);
    f.units.reserve(units.size());
    f.points.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); i++) {
        TextFind::Match p;
        p.start = cp[2 * i];
        p.length = cp[2 * i + 1] - cp[2 * i];
        if (scoped && !inside(p.start, p.length, scopeStart, scopeEnd)) continue;
        if (f.points.size() == kMatchLimit) {
            truncated = true;
            break;
        }
        f.units.push_back(std::move(units[i]));
        f.points.push_back(p);
    }
    f.truncated = truncated;
    return f;
}

// ------------------------------------------------------------ replacing

std::vector<Change> replaceAll(const std::u16string& text, const TextFind::Query& query,
                               const std::u16string& replacement, std::size_t scopeStart,
                               std::size_t scopeEnd) {
    std::vector<Change> out;
    if (!query.usable()) return out;
    const std::vector<TextFind::Edit16> edits = TextFind::replaceAll(text, query, replacement);
    std::vector<std::size_t> at;
    at.reserve(edits.size() * 2);
    for (const TextFind::Edit16& e : edits) {
        at.push_back(e.start);
        at.push_back(e.start + e.length);
    }
    const std::vector<std::size_t> cp = EditSpans::codePoints(text, at);
    const bool scoped = scopeEnd > scopeStart;
    out.reserve(edits.size());
    for (std::size_t i = 0; i < edits.size(); i++) {
        Change c;
        c.start = cp[2 * i];
        c.length = cp[2 * i + 1] - cp[2 * i];
        if (scoped && !inside(c.start, c.length, scopeStart, scopeEnd)) continue;
        c.text = utf8(edits[i].text);
        c.newLength = pointsIn(edits[i].text);
        out.push_back(std::move(c));
    }
    return out;
}

bool replaceOne(const std::u16string& text, const Found& found, std::size_t index,
                const TextFind::Query& query, const std::u16string& replacement, Change* out) {
    if (index >= found.units.size() || index >= found.points.size() || !query.usable())
        return false;
    const TextFind::Match& m = found.units[index];
    if (m.start + m.length > text.size()) return false;
    const std::u16string with = TextFind::expand(replacement, text, m, query);
    out->start = found.points[index].start;
    out->length = found.points[index].length;
    out->text = utf8(with);
    out->newLength = pointsIn(with);
    return true;
}

// ------------------------------------------------------------ colors

const std::vector<Palette>& palettes() {
    // RRGGBBAA throughout; the same rows as FindWidget.mm's kPalettes.
    static const std::vector<Palette> table = {
        {"dark-modern", 0x202020FF, 0x313131FF, 0xCCCCCCFF, 0x0000005C,
         0x313131FF, 0x3C3C3CFF, 0xCCCCCCFF, 0x989898FF, 0x0078D4FF,
         0x2489DB82, 0x2488DBFF, 0xFFFFFFFF,
         0xF85149FF, 0x616161FF, 0xF8F8F8FF, 0x181818FF, 0x04395EFF},
        {"dark-plus", 0x252526FF, 0x303031FF, 0xCCCCCCFF, 0x0000005C,
         0x3C3C3CFF, 0, 0xCCCCCCFF, 0xA6A6A6FF, 0x007FD4FF,
         0x007FD466, 0x007ACCFF, 0xFFFFFFFF,
         0xF48771FF, 0x4D4D4DFF, 0xFFFFFFFF, 0x252526FF, 0x04395EFF},
        {"monokai", 0x1E1F1CFF, 0, 0xF8F8F2FF, 0x00000098,
         0x414339FF, 0, 0xF8F8F2FF, 0x90908AFF, 0x99947CFF,
         0x99947C66, 0x75715EFF, 0xFFFFFFFF,
         0xF48771FF, 0x75715EFF, 0xF8F8F2FF, 0x1E1F1CFF, 0x75715EFF},
        {"solarized-dark", 0x00212BFF, 0, 0x93A1A1FF, 0x0000005C,
         0x003847FF, 0, 0x93A1A1FF, 0x93A1A1AA, 0x2AA19999,
         0x2AA19944, 0x2AA19999, 0xFFFFFFFF,
         0xFFEAEAFF, 0x047AA6FF, 0xFFFFFFFF, 0x00212BFF, 0x005A6FFF},
        {"tomorrow-night-blue", 0x001C40FF, 0, 0xFFFFFFFF, 0x0000005C,
         0x001733FF, 0, 0xFFFFFFFF, 0xFFFFFF80, 0xBBDAFFFF,
         0xBBDAFF44, 0xBBDAFFFF, 0xFFFFFFFF,
         0xA92049FF, 0xBBDAFFCC, 0x001733FF, 0x001C40FF, 0xFFFFFF60},
        {"light-modern", 0xF8F8F8FF, 0xE5E5E5FF, 0x3B3B3BFF, 0x00000029,
         0xFFFFFFFF, 0xCECECEFF, 0x3B3B3BFF, 0x767676FF, 0x005FB8FF,
         0xBED6EDFF, 0x005FB8FF, 0x000000FF,
         0xF85149FF, 0xCCCCCCFF, 0x3B3B3BFF, 0xF8F8F8FF, 0xE8E8E8FF},
        {"light-plus", 0xF3F3F3FF, 0xD4D4D4FF, 0x616161FF, 0x00000029,
         0xFFFFFFFF, 0, 0x616161FF, 0x767676FF, 0x0090F1FF,
         0x0090F133, 0x007ACCFF, 0x000000FF,
         0xA1260DFF, 0xC4C4C4FF, 0x333333FF, 0xF3F3F3FF, 0xE4E6F1FF},
        {"quiet-light", 0xF3F3F3FF, 0, 0x333333FF, 0x00000029,
         0xFFFFFFFF, 0, 0x333333FF, 0x767676FF, 0x9769DCFF,
         0x9769DC33, 0xADAFB7FF, 0x000000FF,
         0xA1260DFF, 0x705697AAU, 0xFFFFFFFF, 0xF2F2F2FF, 0xC4D9B1FF},
        {"solarized-light", 0xEEE8D5FF, 0, 0x586E75FF, 0x00000029,
         0xDDD6C1FF, 0, 0x586E75FF, 0x586E75AA, 0xB49471FF,
         0xB4947133, 0xD3AF86FF, 0x000000FF,
         0xA1260DFF, 0xB58900AA, 0xFFFFFFFF, 0xEEE8D5FF, 0xDFCA88FF},
    };
    return table;
}

const Palette& palette(const std::string& theme, bool dark) {
    const std::vector<Palette>& all = palettes();
    for (const Palette& p : all)
        if (theme == p.theme) return p;
    return all[dark ? 1 : 6];   // dark-plus or light-plus
}

// The Mac draws them with these colors (EditorFind.mm, drawHighlightsInRect).
std::uint32_t matchHighlight(bool) { return 0xEA5C0054; }
std::uint32_t currentMatch(bool dark) { return dark ? 0x515C6AFF : 0xA8AC94FF; }
std::uint32_t scopeHighlight(bool dark) { return dark ? 0x3A3D4166 : 0xB4B4B44D; }

}  // namespace FindRules
