// PieceTable.cpp — see PieceTable.h.
#include "PieceTable.h"

#include <algorithm>
#include <cstring>

PieceTable::PieceTable(size_t originalLength)
    : length_(originalLength), originalLength_(originalLength) {
    if (originalLength) {
        pieces_.push_back({0, originalLength, kOriginal});
        at_.push_back(0);
    }
}

// A copy shares the blocks and never adds to the last one: the table it
// came from may go on filling it, past every unit the copy refers to.
PieceTable::PieceTable(const PieceTable& other)
    : pieces_(other.pieces_), at_(other.at_), blocks_(other.blocks_), tailUsed_(other.tailUsed_),
      tailOwned_(false), length_(other.length_), originalLength_(other.originalLength_) {}

PieceTable& PieceTable::operator=(const PieceTable& other) {
    if (this != &other) {
        pieces_ = other.pieces_;
        at_ = other.at_;
        blocks_ = other.blocks_;
        tailUsed_ = other.tailUsed_;
        tailOwned_ = false;
        length_ = other.length_;
        originalLength_ = other.originalLength_;
    }
    return *this;
}

bool PieceTable::isOriginal() const {
    if (pieces_.empty()) return originalLength_ == 0;
    return pieces_.size() == 1 && pieces_[0].block == kOriginal && pieces_[0].start == 0 &&
           pieces_[0].length == originalLength_;
}

size_t PieceTable::pieceAt(size_t pos) const {
    return (size_t)(std::upper_bound(at_.begin(), at_.end(), pos) - at_.begin()) - 1;
}

PieceTable::Piece PieceTable::store(const char16_t* units, size_t n) {
    if (n > kBlockUnits / 4) {   // a paste: a block of its own, exactly its size
        auto b = std::make_shared<Block>();
        b->data.reset(new char16_t[n]);
        b->capacity = n;
        std::memcpy(b->data.get(), units, n * sizeof(char16_t));
        blocks_.push_back(std::move(b));
        tailUsed_ = n;
        tailOwned_ = true;
        return {0, n, (uint32_t)(blocks_.size() - 1)};
    }
    if (!tailOwned_ || blocks_.empty() || blocks_.back()->capacity - tailUsed_ < n) {
        auto b = std::make_shared<Block>();
        b->data.reset(new char16_t[kBlockUnits]);
        b->capacity = kBlockUnits;
        blocks_.push_back(std::move(b));
        tailUsed_ = 0;
        tailOwned_ = true;
    }
    std::memcpy(blocks_.back()->data.get() + tailUsed_, units, n * sizeof(char16_t));
    const Piece p{tailUsed_, n, (uint32_t)(blocks_.size() - 1)};
    tailUsed_ += n;
    return p;
}

// `right` continues `left` where it came from: the same block (or both the
// original), and the very next unit.
bool PieceTable::extends(const Piece& left, const Piece& right) const {
    return left.block == right.block && left.start + left.length == right.start;
}

void PieceTable::reindexFrom(size_t i) {
    at_.resize(pieces_.size());
    for (size_t k = i; k < pieces_.size(); k++)
        at_[k] = k == 0 ? 0 : at_[k - 1] + pieces_[k - 1].length;
}

void PieceTable::replace(size_t pos, size_t oldLen, const char16_t* units, size_t n) {
    if (pos > length_) pos = length_;
    if (oldLen > length_ - pos) oldLen = length_ - pos;
    if (!oldLen && !n) return;
    const size_t end = pos + oldLen;
    // The pieces [first, last) the edit touches, and what stands in for them.
    const size_t first = pos < length_ ? pieceAt(pos) : pieces_.size();
    size_t last = first;
    Piece mid[3];
    size_t count = 0;
    if (first < pieces_.size() && pos > at_[first]) {
        const Piece& p = pieces_[first];
        mid[count++] = {p.start, pos - at_[first], p.block};
    }
    if (n) mid[count++] = store(units, n);
    if (end < length_) {
        const size_t j = pieceAt(end);
        const Piece& p = pieces_[j];
        const size_t skip = end - at_[j];
        mid[count++] = {p.start + skip, p.length - skip, p.block};
        last = j + 1;
    } else {
        last = pieces_.size();
    }
    pieces_.erase(pieces_.begin() + (long)first, pieces_.begin() + (long)last);
    pieces_.insert(pieces_.begin() + (long)first, mid, mid + count);
    // Join what meets again, from the piece before the edit to the one after.
    size_t from = first ? first - 1 : 0;
    size_t to = std::min(pieces_.size(), first + count + 1);
    for (size_t k = from; k + 1 < to;) {
        if (extends(pieces_[k], pieces_[k + 1])) {
            pieces_[k].length += pieces_[k + 1].length;
            pieces_.erase(pieces_.begin() + (long)k + 1);
            to--;
        } else {
            k++;
        }
    }
    length_ = length_ - oldLen + n;
    reindexFrom(from);
}

void PieceTable::segments(size_t pos, size_t len,
                          const std::function<void(const Segment&)>& f) const {
    if (pos >= length_ || !len) return;
    if (len > length_ - pos) len = length_ - pos;
    const size_t end = pos + len;
    for (size_t i = pieceAt(pos); i < pieces_.size() && at_[i] < end; i++) {
        const Piece& p = pieces_[i];
        const size_t a = std::max(pos, at_[i]), b = std::min(end, at_[i] + p.length);
        Segment s;
        s.at = a;
        s.length = b - a;
        s.start = p.start + (a - at_[i]);
        s.added = p.block == kOriginal ? nullptr : blocks_[p.block]->data.get() + s.start;
        f(s);
    }
}

void PieceTable::read(size_t pos, size_t len, char16_t* out, const Reader& original) const {
    segments(pos, len, [&](const Segment& s) {
        char16_t* to = out + (s.at - pos);
        if (s.added) std::memcpy(to, s.added, s.length * sizeof(char16_t));
        else original(s.start, s.length, to);
    });
}

char16_t PieceTable::at(size_t pos, const Reader& original) const {
    char16_t c = 0;
    if (pos < length_) read(pos, 1, &c, original);
    return c;
}

bool PieceTable::sameAsOriginal(const Reader& original) const {
    if (length_ != originalLength_) return false;
    bool same = true;
    char16_t a[4096], b[4096];
    segments(0, length_, [&](const Segment& s) {
        if (!same || (!s.added && s.start == s.at)) return;   // in its own place
        for (size_t done = 0; same && done < s.length;) {
            const size_t k = std::min(s.length - done, sizeof a / sizeof a[0]);
            original(s.at + done, k, a);
            if (s.added) std::memcpy(b, s.added + done, k * sizeof(char16_t));
            else original(s.start + done, k, b);
            same = std::memcmp(a, b, k * sizeof(char16_t)) == 0;
            done += k;
        }
    });
    return same;
}

bool PieceTable::sameAs(const PieceTable& other, const Reader& original) const {
    if (length_ != other.length_) return false;
    std::vector<Segment> a, b;
    segments(0, length_, [&](const Segment& s) { a.push_back(s); });
    other.segments(0, other.length_, [&](const Segment& s) { b.push_back(s); });
    char16_t x[4096], y[4096];
    auto fetch = [&](const Segment& s, size_t at, size_t n, char16_t* out) {
        if (s.added) std::memcpy(out, s.added + (at - s.at), n * sizeof(char16_t));
        else original(s.start + (at - s.at), n, out);
    };
    size_t i = 0, j = 0;
    for (size_t at = 0; at < length_;) {
        while (a[i].at + a[i].length <= at) i++;
        while (b[j].at + b[j].length <= at) j++;
        const Segment &p = a[i], &q = b[j];
        const size_t end = std::min(p.at + p.length, q.at + q.length);
        const bool shared = p.added ? q.added && p.added + (at - p.at) == q.added + (at - q.at)
                                    : !q.added && p.start + (at - p.at) == q.start + (at - q.at);
        if (!shared) {
            for (size_t k = at; k < end;) {
                const size_t n = std::min(end - k, sizeof x / sizeof x[0]);
                fetch(p, k, n, x);
                fetch(q, k, n, y);
                if (std::memcmp(x, y, n * sizeof(char16_t)) != 0) return false;
                k += n;
            }
        }
        at = end;
    }
    return true;
}

size_t PieceTable::addedUnits() const {
    size_t n = 0;
    for (const auto& b : blocks_) n += b->capacity;
    return n;
}
