// PageWords.cpp — see PageWords.h.
#include "PageWords.h"

#include "LatexDoc.h"
#include "SyncTex.h"

#include <limits>

namespace {

std::u32string decode(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        std::size_t len = 1;
        char32_t cp = c;
        if (c >= 0xF0)      { len = 4; cp = c & 0x07; }
        else if (c >= 0xE0) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xC0) { len = 2; cp = c & 0x1F; }
        else if (c >= 0x80) { cp = 0xFFFD; }   // a stray continuation byte
        if (i + len > s.size()) { out.push_back(0xFFFD); break; }
        for (std::size_t k = 1; k < len; ++k)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

void encode(char32_t cp, std::string* out) {
    if (cp < 0x80) {
        out->push_back((char)cp);
    } else if (cp < 0x800) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

std::string encode(const std::u32string& s, std::size_t a, std::size_t b) {
    std::string out;
    for (std::size_t i = a; i < b && i < s.size(); ++i) encode(s[i], &out);
    return out;
}

// A letter or digit, by the matcher's own definition: a character whose match
// key is not empty. So the page and the source agree on what a word is made
// of (ligatures, accented letters and other scripts included).
bool keyChar(char32_t cp) {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
               (cp >= 'A' && cp <= 'Z');
    }
    std::string one;
    encode(cp, &one);
    return !LatexDoc::matchKey(one).empty();
}

bool isApostrophe(char32_t cp) { return cp == '\'' || cp == 0x2019; }

bool isBreak(char32_t cp) {
    return cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C || cp == 0x85 ||
           cp == 0x2028 || cp == 0x2029;
}

bool isLetter(char32_t cp) { return keyChar(cp) && !(cp >= '0' && cp <= '9'); }

}  // namespace

// ---------------------------------------------------------------- PageText

PageText::PageText(const std::string& utf8, std::vector<PageBox> boxes)
    : boxes_(std::move(boxes)) {
    std::u32string s = decode(utf8);
    chars_.assign(s.begin(), s.end());
    if (boxes_.size() > chars_.size()) boxes_.resize(chars_.size());
    if (chars_.size() > boxes_.size()) chars_.resize(boxes_.size());
    word_.resize(chars_.size());
    for (std::size_t i = 0; i < chars_.size(); ++i) word_[i] = keyChar(chars_[i]) ? 1 : 0;
    // An apostrophe inside a word ("don't", "l’été") belongs to it.
    for (std::size_t i = 1; i + 1 < chars_.size(); ++i)
        if (isApostrophe(chars_[i]) && word_[i - 1] && keyChar(chars_[i + 1])) word_[i] = 1;
}

bool PageText::isWordChar(std::size_t i) const { return i < word_.size() && word_[i]; }

std::size_t PageText::charAt(double x, double y) const {
    std::size_t best = std::string::npos;
    double bestArea = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        if (isBreak(chars_[i])) continue;   // a newline's box is not a glyph
        const PageBox& b = boxes_[i];
        if (x < b.x1 || x > b.x2 || y < b.y1 || y > b.y2) continue;
        const double area = (b.x2 - b.x1) * (b.y2 - b.y1);
        if (area < bestArea) { bestArea = area; best = i; }
    }
    return best;
}

void PageText::wordRange(std::size_t i, std::size_t* start, std::size_t* end) const {
    if (!isWordChar(i)) { *start = *end = i; return; }
    std::size_t s = i, e = i + 1;
    while (s > 0 && word_[s - 1]) --s;
    while (e < word_.size() && word_[e]) ++e;
    // An apostrophe at either end is a quote mark, not part of the word.
    while (e > s && isApostrophe(chars_[e - 1])) --e;
    while (s < e && isApostrophe(chars_[s])) ++s;
    *start = s;
    *end = e;
}

std::vector<std::pair<std::size_t, std::size_t>> PageText::words() const {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (std::size_t i = 0; i < word_.size();) {
        if (!word_[i]) { ++i; continue; }
        std::size_t s, e;
        wordRange(i, &s, &e);
        if (e > s) out.emplace_back(s, e);
        std::size_t next = i + 1;
        while (next < word_.size() && word_[next]) ++next;
        i = next;
    }
    return out;
}

std::string PageText::slice(std::size_t start, std::size_t end) const {
    std::string out;
    for (std::size_t i = start; i < end && i < chars_.size(); ++i) encode(chars_[i], &out);
    return out;
}

PageClick PageText::clickAt(double x, double y) const {
    PageClick c;
    const std::size_t i = charAt(x, y);
    if (i == std::string::npos) return c;   // margin, gap, picture: nothing
    std::size_t s, e;
    wordRange(i, &s, &e);
    if (e > s) {
        c.kind = PageClick::Kind::Word;
        c.start = s;
        c.end = e;
        c.word = slice(s, e);
        c.before = slice(s >= kContext ? s - kContext : 0, s);
        c.after = slice(e, e + kContext);
        joinHyphenation(&c.word, &c.before, &c.after);
        return c;
    }
    // A glyph that is not a word (a bullet, a dash, a space between words):
    // its line, which the matcher can still place, as PDFKit's
    // selectionForLineAtPoint: does on the Mac.
    std::size_t a = i, b = i;
    while (a > 0 && !isBreak(chars_[a - 1])) --a;
    while (b < chars_.size() && !isBreak(chars_[b])) ++b;
    c.kind = PageClick::Kind::Line;
    c.start = a;
    c.end = b;
    c.word = slice(a, b);
    return c;
}

// ---------------------------------------------------------------- hyphens

void joinHyphenation(std::string* wordp, std::string* beforep, std::string* afterp) {
    static const char32_t kHyphens[] = {'-', 0x2010, 0x00AD};
    std::u32string word = decode(*wordp), before = decode(*beforep), after = decode(*afterp);
    bool changed = false;

    // "-\n" right after the word: the rest of it starts the next line.
    for (char32_t h : kHyphens) {
        if (after.size() < 2 || after[0] != h) continue;
        std::size_t k = 1;
        if (!isBreak(after[k])) break;
        while (k < after.size() && isBreak(after[k])) ++k;
        std::size_t e = k;
        while (e < after.size() && isLetter(after[e])) ++e;
        if (e > k) {
            word += after.substr(k, e - k);
            after = after.substr(e);
            changed = true;
        }
        break;
    }
    // "-\n" right before it: the word began at the end of the previous line.
    std::size_t k = before.size();
    while (k > 0 && isBreak(before[k - 1])) --k;
    if (k != before.size() && k != 0) {
        for (char32_t h : kHyphens) {
            if (before[k - 1] != h) continue;
            const std::size_t e = k - 1;
            std::size_t s = e;
            while (s > 0 && isLetter(before[s - 1])) --s;
            if (e > s) {
                word = before.substr(s, e - s) + word;
                before = before.substr(0, s);
                changed = true;
            }
            break;
        }
    }
    if (!changed) return;
    *wordp = encode(word, 0, word.size());
    *beforep = encode(before, 0, before.size());
    *afterp = encode(after, 0, after.size());
}

// ---------------------------------------------------------------- the click

const LatexSpan* latexSpanAtPoint(const LatexDoc& doc, const SyncTexIndex& sync,
                                  int tag, const PageText& page, int pageNumber,
                                  double x, double y, std::vector<SyncTexHit>* hitsOut,
                                  PageClick* clickOut) {
    std::vector<SyncTexHit> hits = sync.textHitsAtPoint(pageNumber, x, y, 8);
    std::vector<int> lines;
    for (const SyncTexHit& h : hits)
        if (tag == 0 || h.tag == tag) lines.push_back(h.line);
    if (hitsOut) *hitsOut = hits;

    PageClick click = page.clickAt(x, y);
    if (clickOut) *clickOut = click;
    // The Mac falls back to the nearest span when a click finds no text at
    // all. Here that refuses: a click in a margin or on a picture names
    // nothing, and opening some text anyway could open the wrong text.
    if (click.kind == PageClick::Kind::Nothing) return nullptr;
    // A glyph that is not a word: its whole line, which the matcher can still
    // place (or, with no letters in it at all, the nearest span, as on the Mac).
    if (click.kind == PageClick::Kind::Line) return doc.spanForClick(lines, click.word);
    return doc.spanForClick(lines, click.word, click.before, click.after);
}
