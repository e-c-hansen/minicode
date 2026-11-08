// MarkdownPage.h — pure C++. A rendered Markdown page in pieces, so a page
// on show can be brought up to date by building again only what changed,
// as the Mac's preview to the side does at every pause in typing.
//
// The parser's runs are split where a page builder's state starts over: at
// the start of a line (after a run ending in "\n", not a line break inside
// a paragraph) outside any quote, code block or table, since a quote's bar
// and a code block's box are one text block carried from paragraph to
// paragraph, and a table is built whole. Each piece renders from its own
// runs alone, so a piece whose runs are the same as last time's, its lines
// counted from its own first line, is the same text, needing only its line
// numbers moved when lines were added or taken out above it.
//
// match() pairs the pieces of a new parse with the last one's: the longest
// runs of equal pieces from the top and from the bottom, so an edit in one
// place builds again only its own pieces (edits in two places, everything
// between them).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "MarkdownParser.h"

namespace MarkdownPage {

struct Piece {
    size_t begin = 0, end = 0;   // its runs, [begin, end)
    int base = 0;                // the first source line of its runs (0 when none has one)
    int firstTable = 0;          // the first tableId among them, which tables count from
    // Two hashes of its runs, lines and tables counted from those (the
    // second with its run count in), which is all a page keeps of them.
    uint64_t key = 0, check = 0;
};

std::vector<Piece> split(const std::vector<MdRun>& runs);

// Whether piece `pa` of `a` and piece `pb` of `b` hold the same runs, their
// lines counted from each piece's base and tables from each one's first:
// alike, and then run for run.
bool same(const std::vector<MdRun>& a, const Piece& pa, const std::vector<MdRun>& b,
          const Piece& pb);
// Whether two pieces' 128 bits of hash and run counts agree, which is what
// match() goes by, so a page need not keep its runs.
bool alike(const Piece& a, const Piece& b);

// For each new piece, the old piece it is alike and may be kept in place of
// (its index), or -1 to build it. Kept pieces keep their order; `keep[i]`
// false (a piece still waiting for a picture or a formula) builds old piece
// i again even when it is alike.
std::vector<long> match(const std::vector<Piece>& oldPieces, const std::vector<bool>& keep,
                        const std::vector<Piece>& newPieces);

}  // namespace MarkdownPage
