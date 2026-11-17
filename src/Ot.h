// Ot.h — edits that crossed in flight, made to agree (SESSION-DESIGN.md,
// section 2 and stage 4). A renderer edits its own copy of a document at
// once and sends the edit to the session's host; meanwhile the host may have
// applied someone else's (another renderer's typing, an undo). Each side
// then applies the other's edit moved past its own, `transform`, and both
// reach the same text. The host is the one sequencer, so only this one
// property is needed (TP1 in the literature), never the harder one a
// peer-to-peer scheme needs.
//
// An edit is a list of operations applied in order, each in the text as the
// ones before it left it. An operation removes `del` units at `pos`, then
// inserts `ins` there; transform splits it into its two halves, a deletion
// and an insertion (the primitives), which is all it has rules for:
//
//   - two insertions at one place: the one already applied (the host's,
//     the second argument) goes first, so its text ends up left;
//   - an insertion inside a range the other deletes survives, at the
//     deletion's start, and the deletion is split around it;
//   - two deletions delete their union once.
//
// Offsets and lengths are in one unit (Model::Unit): UTF-16 units (the Mac,
// LSP) or code points (GTK's GtkTextIter). In code points an operation
// never splits a surrogate pair. Pure C++17, no I/O.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "EditSpans.h"
#include "Text.h"

namespace Ot {

using Model::Unit;

struct Op {
    std::size_t pos = 0;
    std::size_t del = 0;    // units removed at pos
    std::u16string ins;     // then inserted at pos
    bool operator==(const Op& o) const { return pos == o.pos && del == o.del && ins == o.ins; }
    bool operator!=(const Op& o) const { return !(*this == o); }
};
// Applied in order, each in the text the ones before it left.
using Ops = std::vector<Op>;

// The length of `s` in `u`.
std::size_t lengthOf(const std::u16string& s, Unit u);

// Each operation as a deletion, then an insertion, with the empty ones left
// out: the form transform works in, and returns.
Ops primitives(const Ops& ops);

// Every operation inside the text as it is at its turn, starting from a text
// `length` units long (in `u`). The length after them, in *after.
bool valid(const Ops& ops, std::size_t length, Unit u, std::size_t* after = nullptr);

// The operations made on `text` (which they must fit: valid()).
void apply(std::u16string& text, const Ops& ops, Unit u);
// The same on a Model::Text, each operation one replace(). False, with the
// text as far as it got, at the first that does not fit.
bool apply(Model::Text& text, const Ops& ops, Unit u);

// `a` and `b` were both made on one text. *aAfterB is `a` moved past `b`
// (applied after b), *bAfterA `b` moved past `a`, so that a then bAfterA and
// b then aAfterB leave the same text. `b` wins ties: at one place, its
// insertion goes first. Either output may be null. Both come back as
// primitives.
void transform(const Ops& a, const Ops& b, Unit u, Ops* aAfterB, Ops* bAfterA);

// Where offset `at` of the text before `ops` is after them. An insertion at
// `at` itself moves it past the inserted text only with `afterInsert`; inside
// a deleted range it goes to the range's start.
std::size_t mapOffset(std::size_t at, const Ops& ops, Unit u, bool afterInsert = false);

// Spans as EditSpans takes them (ascending, apart, each replaced by
// texts[i]) as operations: last first, so each is still where it was.
Ops fromSpans(const std::vector<EditSpans::Span>& spans, const std::vector<std::u16string>& texts);

// The other way, when the operations allow it: each starting at or before
// where the one before it started (the shape fromSpans makes, and an undo's
// plan), so every one is also in the text before them all. Pairs a deletion
// with the insertion at its place. False, with nothing in the outputs, for
// any other shape.
struct Replacement {
    std::size_t start = 0, length = 0;
    std::u16string text;
};
bool asReplacements(const Ops& ops, Unit u, std::vector<Replacement>* out);

}  // namespace Ot
