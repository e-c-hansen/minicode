// Text.cpp — see Text.h. The chunked text (Lsp::SyncedText until stage 1
// of the shared session, moved here with code points, versions and
// apply()), and UTF-8 to UTF-16 and back.
#include "Text.h"

#include <algorithm>
#include <iterator>

namespace Model {

namespace {
bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// UTF-8 to UTF-16, each unit given to put(char16_t) in order: the one
// decoder of utf16FromUtf8 and Text::fromUtf8.
template <class Put>
void decodeUtf8(const std::string& s, Put put) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { put((char16_t)0xFFFD); i++; continue; }
        if (i + n > s.size()) { put((char16_t)0xFFFD); break; }
        bool bad = false;
        for (size_t k = 1; k < n; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { put((char16_t)0xFFFD); i++; continue; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put((char16_t)(0xD800 + (cp >> 10)));
            put((char16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            put((char16_t)cp);
        }
    }
}
}  // namespace

std::u16string utf16FromUtf8(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    decodeUtf8(s, [&out](char16_t u) { out += u; });
    return out;
}

std::string utf8FromUtf16(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = s[i];
        if (isHigh((char16_t)cp) && i + 1 < s.size() && isLow(s[i + 1])) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

bool hasLoneSurrogate(const std::u16string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (isHigh(s[i])) {
            if (i + 1 < s.size() && isLow(s[i + 1])) i++;
            else return true;
        } else if (isLow(s[i])) {
            return true;
        }
    }
    return false;
}

size_t countCodePoints(const std::u16string& s) {
    size_t n = s.size();
    for (size_t i = 1; i < s.size(); i++)
        if (isLow(s[i]) && isHigh(s[i - 1])) n--;
    return n;
}

// ------------------------------------------------------------------- Text
namespace {
std::shared_ptr<const std::u16string> shared(std::u16string s) {
    return std::make_shared<const std::u16string>(std::move(s));
}
}  // namespace

Text::Text(const std::u16string& text) : size_(text.size()) {
    chunks_.reserve(text.size() / kChunk + 1);
    for (size_t at = 0; at < text.size(); at += kChunk) {
        Chunk c;
        c.text = shared(text.substr(at, kChunk));
        chunks_.push_back(std::move(c));
    }
    for (size_t k = 0; k < chunks_.size(); k++) rescan(k);
    rebuild();
}

Text Text::fromUtf8(const std::string& utf8) {
    Text t;
    t.chunks_.reserve(utf8.size() / kChunk + 1);
    std::u16string piece;
    piece.reserve(kChunk);
    auto flush = [&t, &piece]() {
        Chunk c;
        t.size_ += piece.size();
        c.text = shared(std::move(piece));
        t.chunks_.push_back(std::move(c));
        piece = std::u16string();
        piece.reserve(kChunk);
    };
    decodeUtf8(utf8, [&](char16_t u) {
        piece += u;
        if (piece.size() == kChunk) flush();
    });
    if (!piece.empty()) flush();
    for (size_t k = 0; k < t.chunks_.size(); k++) t.rescan(k);
    t.rebuild();
    return t;
}

void Text::add(std::vector<size_t>& tree, size_t k, long delta) {
    if (!delta) return;
    for (size_t i = k + 1; i < tree.size(); i += i & (~i + 1))
        tree[i] = (size_t)((long)tree[i] + delta);
}

size_t Text::before(const std::vector<size_t>& tree, size_t k) {
    size_t sum = 0;
    for (size_t i = k; i > 0; i -= i & (~i + 1)) sum += tree[i];
    return sum;
}

size_t Text::find(const std::vector<size_t>& tree, size_t value) const {
    const size_t n = chunks_.size();
    if (n == 0) return 0;
    size_t step = 1;
    while (step * 2 <= n) step *= 2;
    size_t pos = 0, rest = value;
    for (; step; step /= 2)
        if (pos + step <= n && tree[pos + step] <= rest) {
            pos += step;
            rest -= tree[pos];
        }
    return std::min(pos, n - 1);
}

void Text::rebuild() {
    const size_t n = chunks_.size();
    units_.assign(n + 1, 0);
    startsIn_.assign(n + 1, 0);
    pointsIn_.assign(n + 1, 0);
    startCount_ = 0;
    pointCount_ = 0;
    for (size_t k = 0; k < n; k++) {
        units_[k + 1] += chunks_[k].text->size();
        startsIn_[k + 1] += chunks_[k].starts.size();
        pointsIn_[k + 1] += chunks_[k].points;
        startCount_ += chunks_[k].starts.size();
        pointCount_ += chunks_[k].points;
        const size_t up = (k + 1) + ((k + 1) & (~(k + 1) + 1));
        if (up <= n) {
            units_[up] += units_[k + 1];
            startsIn_[up] += startsIn_[k + 1];
            pointsIn_[up] += pointsIn_[k + 1];
        }
    }
}

char16_t Text::unitBefore(size_t k) const {
    return k > 0 && k - 1 < chunks_.size() ? chunks_[k - 1].text->back() : 0;
}

// A line starts after \n, and after a \r not followed by \n (the unit after
// a chunk's last is the next chunk's first). A code point starts at every
// unit but the low half of a pair (whose high half may end the chunk
// before).
void Text::rescan(size_t k) {
    Chunk& c = chunks_[k];
    c.starts.clear();
    const size_t n = c.text->size();
    const char16_t* t = c.text->data();
    char16_t prev = unitBefore(k);
    size_t points = 0;
    for (size_t i = 0; i < n; i++) {
        if (t[i] == u'\n') {
            c.starts.push_back((uint32_t)(i + 1));
        } else if (t[i] == u'\r') {
            const bool nextIsNewline = i + 1 < n ? t[i + 1] == u'\n'
                                       : k + 1 < chunks_.size() && (*chunks_[k + 1].text)[0] == u'\n';
            if (!nextIsNewline) c.starts.push_back((uint32_t)(i + 1));
        }
        if (!(isLow(t[i]) && isHigh(prev))) points++;
        prev = t[i];
    }
    c.points = points;
}

LinePos Text::position(size_t offset) const {
    offset = std::min(offset, size_);
    if (offset == 0) return LinePos{0, 0};
    // The starts at or before `offset` are those of the chunks before the
    // one holding the unit before it, and that chunk's own up to it.
    const size_t k = chunkAt(offset - 1);
    const Chunk& c = chunks_[k];
    const size_t chunkStart = before(units_, k);
    auto it = std::upper_bound(c.starts.begin(), c.starts.end(), (uint32_t)(offset - chunkStart));
    const size_t inChunk = (size_t)(it - c.starts.begin());
    const size_t earlier = before(startsIn_, k);
    size_t start = 0;
    if (inChunk) {
        start = chunkStart + c.starts[inChunk - 1];
    } else if (earlier) {
        // The chunk holding the last start before this one.
        const size_t j = find(startsIn_, earlier - 1);
        start = before(units_, j) + chunks_[j].starts.back();
    }
    return LinePos{(int)(earlier + inChunk), (int)(offset - start)};
}

size_t Text::unitOf(size_t point) const {
    if (point >= pointCount_) return size_;
    // The chunk holding that code point's start: the points before it are
    // at most `point`, and with its own more than that.
    const size_t k = find(pointsIn_, point);
    size_t rest = point - before(pointsIn_, k);
    const std::u16string& c = *chunks_[k].text;
    const size_t chunkStart = before(units_, k);
    // No pair ends in this chunk: every unit is a code point.
    if (chunks_[k].points == c.size()) return chunkStart + rest;
    char16_t prev = unitBefore(k);
    for (size_t i = 0; i < c.size(); i++) {
        const char16_t u = c[i];
        if (!(isLow(u) && isHigh(prev))) {
            if (rest == 0) return chunkStart + i;
            rest--;
        }
        prev = u;
    }
    return chunkStart + c.size();
}

size_t Text::codePointOf(size_t unit) const {
    if (unit >= size_) return pointCount_;
    const size_t k = chunkAt(unit);
    const std::u16string& c = *chunks_[k].text;
    const size_t at = unit - before(units_, k);
    size_t count = before(pointsIn_, k);
    if (chunks_[k].points == c.size()) return count + at;
    char16_t prev = unitBefore(k);
    for (size_t i = 0; i < at; i++) {
        if (!(isLow(c[i]) && isHigh(prev))) count++;
        prev = c[i];
    }
    // The low half of a pair: the pair's own code point, counted already.
    if (isLow(c[at]) && isHigh(prev)) count--;
    return count;
}

void Text::replace(size_t a, size_t b, const std::u16string& with) {
    a = std::min(a, size_);
    b = std::min(std::max(a, b), size_);
    if (a == b && with.empty()) return;
    replaceUnits(a, b, with);
    version_++;
}

void Text::replaceUnits(size_t a, size_t b, const std::u16string& with) {
    if (a == b && with.empty()) return;
    snapshot_.reset();
    if (chunks_.empty()) {
        const uint64_t v = version_;
        *this = Text(with);
        version_ = v;
        return;
    }
    size_t ka = chunkAt(a), kb = chunkAt(b);
    std::u16string merged = chunks_[ka].text->substr(0, a - before(units_, ka));
    merged += with;
    merged.append(*chunks_[kb].text, b - before(units_, kb), std::u16string::npos);
    // A small piece joins a neighbour, so chunks stay a quarter full at
    // least and their number follows the text's size.
    const size_t kMin = kChunk / 4, kMax = kChunk * 2;
    if (merged.size() < kMin && kb + 1 < chunks_.size()) {
        merged += *chunks_[++kb].text;
    } else if (merged.size() < kMin && ka > 0) {
        merged.insert(0, *chunks_[--ka].text);
    }
    const size_t count = merged.empty()        ? 0
                         : merged.size() <= kMax ? 1
                                                 : (merged.size() + kChunk - 1) / kChunk;
    size_ = size_ - (b - a) + with.size();
    // New strings, never the old ones changed: a Frozen may share those.
    auto piece = [&](size_t i, size_t& at) {
        const size_t len = (merged.size() - at) / (count - i);
        std::shared_ptr<const std::u16string> p = shared(merged.substr(at, len));
        at += len;
        return p;
    };
    // The chunk before may end in a \r meeting another unit; the chunk
    // after may start with the low half of a pair whose high half changed.
    const size_t first = ka > 0 ? ka - 1 : 0;
    if (count == kb - ka + 1) {
        // As many chunks as before: rewritten in place, the trees adjusted.
        size_t at = 0;
        for (size_t i = 0; i < count; i++) {
            Chunk& c = chunks_[ka + i];
            const long was = (long)c.text->size();
            c.text = piece(i, at);
            add(units_, ka + i, (long)c.text->size() - was);
        }
        for (size_t k = first; k <= ka + count && k < chunks_.size(); k++) {
            const long was = (long)chunks_[k].starts.size(), wasPoints = (long)chunks_[k].points;
            rescan(k);
            const long delta = (long)chunks_[k].starts.size() - was;
            const long pointDelta = (long)chunks_[k].points - wasPoints;
            add(startsIn_, k, delta);
            add(pointsIn_, k, pointDelta);
            startCount_ = (size_t)((long)startCount_ + delta);
            pointCount_ = (size_t)((long)pointCount_ + pointDelta);
        }
        return;
    }
    std::vector<Chunk> pieces(count);
    size_t at = 0;
    for (size_t i = 0; i < count; i++) pieces[i].text = piece(i, at);
    chunks_.erase(chunks_.begin() + (long)ka, chunks_.begin() + (long)kb + 1);
    chunks_.insert(chunks_.begin() + (long)ka, std::make_move_iterator(pieces.begin()),
                   std::make_move_iterator(pieces.end()));
    for (size_t k = first; k <= ka + count && k < chunks_.size(); k++) rescan(k);
    rebuild();
}

bool Text::apply(const std::vector<Edit>& edits, Unit u) {
    const size_t total = length(u);
    size_t last = 0;
    for (const Edit& e : edits) {
        if (e.start < last || e.length > total || e.start > total - e.length) return false;
        last = e.start + e.length;
    }
    // Every place in units, all taken from the text before any change.
    std::vector<std::pair<size_t, size_t>> at(edits.size());
    bool any = false;
    for (size_t i = 0; i < edits.size(); i++) {
        const size_t a = toUnits(edits[i].start, u);
        at[i] = {a, toUnits(edits[i].start + edits[i].length, u)};
        any = any || at[i].first != at[i].second || !edits[i].text.empty();
    }
    if (!any) return true;
    for (size_t i = edits.size(); i-- > 0;) replaceUnits(at[i].first, at[i].second, edits[i].text);
    version_++;
    return true;
}

bool Text::apply(const std::vector<EditSpans::Span>& spans, const std::vector<std::u16string>& texts,
                 Unit u) {
    if (spans.size() != texts.size() || !EditSpans::valid(spans, length(u))) return false;
    std::vector<Edit> edits;
    edits.reserve(spans.size());
    for (size_t i = 0; i < spans.size(); i++) {
        const size_t n = u == Unit::Utf16 ? texts[i].size() : countCodePoints(texts[i]);
        if (n != spans[i].newLength) return false;
        edits.push_back(Edit{spans[i].start, spans[i].length, texts[i]});
    }
    return apply(edits, u);
}

char16_t Text::at(size_t i) const {
    const size_t k = chunkAt(i);
    return (*chunks_[k].text)[i - before(units_, k)];
}

std::u16string Text::slice(size_t start, size_t length) const {
    std::u16string out;
    start = std::min(start, size_);
    length = std::min(length, size_ - start);
    if (!length) return out;
    out.reserve(length);
    size_t k = chunkAt(start), off = start - before(units_, k);
    while (out.size() < length && k < chunks_.size()) {
        const std::u16string& t = *chunks_[k].text;
        const size_t n = std::min(t.size() - off, length - out.size());
        out.append(t, off, n);
        k++;
        off = 0;
    }
    return out;
}

std::u16string Text::str() const {
    std::u16string out;
    out.reserve(size_);
    for (const Chunk& c : chunks_) out += *c.text;
    return out;
}

std::shared_ptr<const std::u16string> Text::snapshot() const {
    if (!snapshot_) snapshot_ = std::make_shared<const std::u16string>(str());
    return snapshot_;
}

bool Text::operator==(const std::u16string& s) const {
    if (s.size() != size_) return false;
    size_t at = 0;
    for (const Chunk& c : chunks_) {
        if (s.compare(at, c.text->size(), *c.text) != 0) return false;
        at += c.text->size();
    }
    return true;
}

namespace {
// Two texts of the same length as strings in order, cut in different
// places (piece(i) the i-th string's shared pointer): equal unit for unit.
// A string both hold at the same place is the same units, not compared.
template <class A, class B>
bool sameUnits(size_t na, A pieceA, size_t nb, B pieceB) {
    size_t i = 0, j = 0, oi = 0, oj = 0;
    while (i < na && j < nb) {
        const std::shared_ptr<const std::u16string>& pa = pieceA(i);
        const std::shared_ptr<const std::u16string>& pb = pieceB(j);
        if (oi == 0 && oj == 0 && pa == pb) {
            i++, j++;
            continue;
        }
        const std::u16string& a = *pa;
        const std::u16string& b = *pb;
        const size_t n = std::min(a.size() - oi, b.size() - oj);
        if (a.compare(oi, n, b, oj, n) != 0) return false;
        oi += n, oj += n;
        if (oi == a.size()) i++, oi = 0;
        if (oj == b.size()) j++, oj = 0;
    }
    return true;
}
}  // namespace

bool Text::operator==(const Text& o) const {
    if (o.size_ != size_ || o.pointCount_ != pointCount_ || o.startCount_ != startCount_)
        return false;
    return sameUnits(
        chunks_.size(), [this](size_t i) -> const auto& { return chunks_[i].text; },
        o.chunks_.size(), [&o](size_t j) -> const auto& { return o.chunks_[j].text; });
}

bool Text::operator==(const Frozen& f) const {
    if (f.size_ != size_) return false;
    return sameUnits(
        chunks_.size(), [this](size_t i) -> const auto& { return chunks_[i].text; },
        f.pieces_.size(), [&f](size_t j) -> const auto& { return f.pieces_[j]; });
}

Frozen Text::freeze() const {
    Frozen f;
    f.pieces_.reserve(chunks_.size());
    for (const Chunk& c : chunks_) f.pieces_.push_back(c.text);
    f.size_ = size_;
    return f;
}

// ----------------------------------------------------------------- Frozen
Frozen::Frozen(const std::u16string& text) : size_(text.size()) {
    pieces_.reserve(text.size() / Text::kChunk + 1);
    for (size_t at = 0; at < text.size(); at += Text::kChunk)
        pieces_.push_back(shared(text.substr(at, Text::kChunk)));
}

std::u16string Frozen::str() const {
    std::u16string out;
    out.reserve(size_);
    for (const auto& p : pieces_) out += *p;
    return out;
}

bool Frozen::operator==(const std::u16string& s) const {
    if (s.size() != size_) return false;
    size_t at = 0;
    for (const auto& p : pieces_) {
        if (s.compare(at, p->size(), *p) != 0) return false;
        at += p->size();
    }
    return true;
}

}  // namespace Model
