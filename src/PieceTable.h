// PieceTable.h — pure C++17, no I/O. A text kept as pieces of an original
// that never changes and of a store of what edits put in, so a large file's
// text is held once: the original is the tab's text as read (on the Mac an
// NSString of the file, 8-bit for ASCII), and the editor's storage edits
// over it without a copy (mac/src/PieceString.h). VS Code's text model is a
// piece tree of the same idea.
//
// Units are UTF-16 code units, as NSString counts. The original is read
// through a Reader the owner gives each call (the table never holds it), and
// edits' units are kept in blocks that only grow at their end, never move,
// and are shared by copies of the table: a copy is a snapshot that costs the
// pieces and a reference to each block, and stays valid while the table it
// came from goes on editing (the table writes only past what any copy
// refers to; a copy itself starts a fresh block at its first edit). So a
// copy may be read on another thread while the table it came from is
// edited on its own, the way the Mac's search queue reads the tab's text.
//
// An edit costs the number of pieces, never the length of the text: a
// character typed after the last one typed extends that piece, and pieces
// that meet again (text typed, then deleted) are joined, so the table is
// the original again (isOriginal) and compares with it in no time.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class PieceTable {
public:
    // Reads `length` units of the original from `start` into `out`.
    using Reader = std::function<void(size_t start, size_t length, char16_t* out)>;

    explicit PieceTable(size_t originalLength = 0);
    PieceTable(const PieceTable& other);
    PieceTable& operator=(const PieceTable& other);
    PieceTable(PieceTable&&) = default;
    PieceTable& operator=(PieceTable&&) = default;

    size_t length() const { return length_; }
    size_t originalLength() const { return originalLength_; }
    size_t pieceCount() const { return pieces_.size(); }
    // The text is the original, whole and unmoved: one piece of it (none for
    // an empty original left empty).
    bool isOriginal() const;

    // The units [pos, pos + oldLen) become the `n` units at `units`.
    void replace(size_t pos, size_t oldLen, const char16_t* units, size_t n);

    // Units [pos, pos + len) into `out` (clamped to the text).
    void read(size_t pos, size_t len, char16_t* out, const Reader& original) const;
    char16_t at(size_t pos, const Reader& original) const;

    // Whether the text equals the original. Pieces of the original standing
    // where they stood in it compare without being read; the rest are read.
    bool sameAsOriginal(const Reader& original) const;
    // Whether two tables over the same original hold the same text (a tab's
    // text against the text it last saved, both pieces of the file read):
    // what both take from the same place, in the original or in a shared
    // block, compares without being read.
    bool sameAs(const PieceTable& other, const Reader& original) const;

    // The pieces over [pos, pos + len) in order: `added` is null for a piece
    // of the original (`start` is where in the original it begins), else
    // the piece's units; `at` is where the piece begins in the text.
    struct Segment {
        size_t at = 0;
        size_t length = 0;
        size_t start = 0;
        const char16_t* added = nullptr;
    };
    void segments(size_t pos, size_t len, const std::function<void(const Segment&)>& f) const;

    // Units of edits kept (memory beyond the original and the pieces).
    size_t addedUnits() const;

private:
    static constexpr uint32_t kOriginal = UINT32_MAX;
    static constexpr size_t kBlockUnits = 1 << 16;
    struct Block {
        std::unique_ptr<char16_t[]> data;
        size_t capacity = 0;
    };
    struct Piece {
        size_t start = 0;    // in the original, or in its block
        size_t length = 0;
        uint32_t block = kOriginal;
    };

    std::vector<Piece> pieces_;
    std::vector<size_t> at_;   // where each piece begins in the text
    std::vector<std::shared_ptr<Block>> blocks_;
    size_t tailUsed_ = 0;      // units used of the last block, while this table may add to it
    bool tailOwned_ = false;   // this table alone appends to the last block
    size_t length_ = 0;
    size_t originalLength_ = 0;

    size_t pieceAt(size_t pos) const;   // the piece holding pos (< length)
    const char16_t* units(const Piece& p) const { return blocks_[p.block]->data.get() + p.start; }
    Piece store(const char16_t* units, size_t n);
    bool extends(const Piece& left, const Piece& right) const;
    void reindexFrom(size_t i);
};
