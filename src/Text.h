// Text.h — a document's text as the core keeps it (SESSION-DESIGN.md,
// stage 1): UTF-16 in chunks of about kChunk units, each with its own line
// starts, and the chunks' lengths, line counts and code point counts in
// Fenwick trees, so an edit rewrites only the chunks it touches and every
// place is found in log time. It was the language server client's copy of
// each document (Lsp::SyncedText, 2026-11-08) and still is; it grew code
// points, so a port that counts characters (the GTK port's GtkTextIter)
// and one that counts UTF-16 units (the Mac's NSTextStorage, and LSP) can
// both address it, a version that every change moves on, and apply(), the
// multi-range edit in the shape of EditSpans.
//
// Each chunk's string is shared and never changed once made: an edit puts
// new strings in place of the ones it touches. So a copy of the text as it
// was (freeze(), a Frozen: a document's saved text, Document.h) costs a
// pointer a chunk, and the two hold one string for every chunk the edits
// since have not touched; comparing them skips those chunks.
//
// Lines end at \n, \r\n or \r, as LSP counts them (Lsp::PositionIndex);
// U+2029 is not a break here, unlike LineIndex. A code point is a unit
// that is not the low half of a surrogate pair, so a lone surrogate counts
// as one. Pure C++17, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EditSpans.h"

namespace Model {

// What an offset counts: UTF-16 units (the Mac, LSP) or code points (GTK).
enum class Unit { Utf16, CodePoint };

// A line and a column in UTF-16 units within it, as LSP's Position.
struct LinePos {
    int line = 0, character = 0;
    bool operator==(const LinePos& o) const { return line == o.line && character == o.character; }
    bool operator!=(const LinePos& o) const { return !(*this == o); }
};

// UTF-8 to UTF-16 and back: a byte that does not start or continue a
// character, and a lone surrogate, become U+FFFD.
std::u16string utf16FromUtf8(const std::string& utf8);
std::string utf8FromUtf16(const std::u16string& utf16);
// A surrogate without its other half: UTF-8 cannot carry it.
bool hasLoneSurrogate(const std::u16string& s);
// Code points in s (a lone surrogate is one).
std::size_t countCodePoints(const std::u16string& s);

// A text as it was when frozen (Text::freeze), never changed after: the
// chunks' strings it had then, shared with the text until its edits replace
// them. A document's saved text (Document.h).
class Frozen {
public:
    Frozen() = default;
    // A text of its own (not a Text's), cut into chunks as a Text is.
    explicit Frozen(const std::u16string& text);
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    // The whole text, copied.
    std::u16string str() const;
    bool operator==(const std::u16string& s) const;
    bool operator!=(const std::u16string& s) const { return !(*this == s); }

private:
    friend class Text;
    std::vector<std::shared_ptr<const std::u16string>> pieces_;   // none empty
    std::size_t size_ = 0;
};

class Text {
public:
    // A new chunk's size, in units: with its terminator 2,048 bytes in
    // libc++'s string (whose capacity rounds up to 1,023), as macOS's
    // allocator rounds 2,050 bytes up to 2,560.
    static constexpr std::size_t kChunk = 1020;

    // One replacement: `length` at `start`, both in the text before the
    // batch, replaced by `text`.
    struct Edit {
        std::size_t start = 0, length = 0;
        std::u16string text;
    };

    Text() = default;
    explicit Text(const std::u16string& text);
    // Decoded straight into chunks, as utf16FromUtf8 decodes it, with no
    // UTF-16 copy of the whole text on the way.
    static Text fromUtf8(const std::string& utf8);

    std::size_t size() const { return size_; }                 // UTF-16 units
    std::size_t codePoints() const { return pointCount_; }
    std::size_t length(Unit u) const { return u == Unit::Utf16 ? size_ : pointCount_; }
    std::size_t lines() const { return 1 + startCount_; }
    // Moves on with every change (never back), from 0 for a new text.
    std::uint64_t version() const { return version_; }

    // As Lsp::PositionIndex::position: the line and UTF-16 column.
    LinePos position(std::size_t offset) const;
    // Code point `point` as a unit offset (size() for codePoints() and past).
    std::size_t unitOf(std::size_t point) const;
    // Unit offset `unit` as a code point: inside a surrogate pair, the
    // pair's (as EditSpans::codePoints); codePoints() at the end and past.
    std::size_t codePointOf(std::size_t unit) const;
    // An offset counted in `u`, as UTF-16 units, and back.
    std::size_t toUnits(std::size_t offset, Unit u) const {
        return u == Unit::Utf16 ? offset : unitOf(offset);
    }
    std::size_t fromUnits(std::size_t units, Unit u) const {
        return u == Unit::Utf16 ? units : codePointOf(units);
    }

    // [a, b) replaced by `with` (both clamped to the text); only the chunks
    // it touches (and the ones either side, for a \r\n or a surrogate pair
    // across their edges) are looked at again.
    void replace(std::size_t a, std::size_t b, const std::u16string& with);
    // Several replacements as one change (one version): ascending, apart
    // (two may touch), inside the text, counted in `u`; applied last first.
    // False, and nothing changed, when they are not.
    bool apply(const std::vector<Edit>& edits, Unit u = Unit::Utf16);
    // The same in EditSpans' shape: span i's new text is texts[i], whose
    // length in `u` must be its newLength.
    bool apply(const std::vector<EditSpans::Span>& spans, const std::vector<std::u16string>& texts,
               Unit u = Unit::Utf16);

    // The unit at i (i < size()).
    char16_t at(std::size_t i) const;
    // Units [start, start + length), clamped to the text.
    std::u16string slice(std::size_t start, std::size_t length) const;
    // The whole text, copied.
    std::u16string str() const;
    std::string utf8() const { return utf8FromUtf16(str()); }
    // The whole text, copied once and shared until the next change.
    std::shared_ptr<const std::u16string> snapshot() const;
    bool operator==(const std::u16string& s) const;
    bool operator==(const Text& o) const;
    // Exact; a chunk still shared with `f` is not compared unit by unit.
    bool operator==(const Frozen& f) const;
    bool operator!=(const std::u16string& s) const { return !(*this == s); }
    std::size_t chunks() const { return chunks_.size(); }
    // The text as it is now, sharing its chunks (see the top of the file).
    Frozen freeze() const;
    // f(const char16_t* units, std::size_t count) for each chunk in order.
    template <class F>
    void eachChunk(F f) const {
        for (const Chunk& c : chunks_) f(c.text->data(), c.text->size());
    }

private:
    struct Chunk {
        std::shared_ptr<const std::u16string> text;   // never empty, never changed
        // Line starts s with the unit before them in this chunk, as
        // s - the chunk's start (1 to its length).
        std::vector<std::uint32_t> starts;
        // Units that start a code point (all but the low half of a pair,
        // whose high half may end the chunk before).
        std::size_t points = 0;
    };
    std::vector<Chunk> chunks_;   // none for an empty text; else none empty
    // Fenwick trees (1-based) over the chunks' lengths, start counts and
    // code point counts.
    std::vector<std::size_t> units_, startsIn_, pointsIn_;
    std::size_t size_ = 0, startCount_ = 0, pointCount_ = 0;
    std::uint64_t version_ = 0;
    mutable std::shared_ptr<const std::u16string> snapshot_;
    static void add(std::vector<std::size_t>& tree, std::size_t k, long delta);
    static std::size_t before(const std::vector<std::size_t>& tree, std::size_t k);   // chunks [0, k)
    // The last chunk k with before(tree, k) <= value (at most chunks() - 1).
    std::size_t find(const std::vector<std::size_t>& tree, std::size_t value) const;
    std::size_t chunkAt(std::size_t pos) const { return find(units_, pos); }
    char16_t unitBefore(std::size_t k) const;   // the last unit of chunk k - 1, or 0
    void rescan(std::size_t k);
    void rebuild();
    void replaceUnits(std::size_t a, std::size_t b, const std::u16string& with);
};

}  // namespace Model
