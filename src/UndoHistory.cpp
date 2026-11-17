// UndoHistory.cpp — see UndoHistory.h.
//
// A step keeps its replacements as pieces: ascending and apart in the text
// before the step, each with what it removed and inserted, as Text::apply
// takes a batch. Edits come in one at a time (a GTK buffer's insert and
// delete signals, a typed letter joining the step before), each in the text
// as it is then, and are composed into the pieces: an edit touching pieces
// becomes one piece with them, built from their texts and the edit's own
// removed text for whatever lies between. A step recorded last first (a
// multi-range edit applied from its end, the shape EditSpans gives every
// port) or first to last with its shifts is taken whole in one pass.
#include "UndoHistory.h"

#include <algorithm>

namespace Model {

namespace {

std::vector<EditSpans::Span> spansOf(const std::vector<UndoHistory::Change>& batch,
                                     const std::function<std::size_t(const std::u16string&)>& len) {
    std::vector<EditSpans::Span> spans;
    spans.reserve(batch.size());
    for (const UndoHistory::Change& c : batch) spans.push_back({c.start, len(c.removed), len(c.inserted)});
    return spans;
}

void mapSelection(UndoHistory::Selection& sel, const std::vector<EditSpans::Span>& spans) {
    if (spans.empty()) return;
    for (UndoHistory::Range& r : sel) {
        r.anchor = EditSpans::mapThrough(r.anchor, spans);
        r.caret = EditSpans::mapThrough(r.caret, spans);
    }
}

}  // namespace

// ---------------------------------------------------------------- units

std::size_t UndoHistory::len(const std::u16string& s) const {
    return unit_ == Unit::Utf16 ? s.size() : countCodePoints(s);
}

std::size_t UndoHistory::index(const std::u16string& s, std::size_t offset) const {
    if (unit_ == Unit::Utf16) return std::min(offset, s.size());
    return EditSpans::unitsAt(s, offset);
}

std::u16string UndoHistory::sub(const std::u16string& s, std::size_t from, std::size_t length) const {
    const std::size_t a = index(s, from);
    const std::size_t b = unit_ == Unit::Utf16 ? std::min(s.size(), a + length)
                                               : a + EditSpans::unitsAt(s.substr(a), length);
    return s.substr(a, b - a);
}

UndoHistory::Piece UndoHistory::piece(std::size_t start, std::u16string removed,
                                      std::u16string inserted) const {
    Piece p;
    p.start = start;
    p.removedLen = len(removed);
    p.insertedLen = len(inserted);
    p.removed = std::move(removed);
    p.inserted = std::move(inserted);
    return p;
}

// ---------------------------------------------------------------- pieces

// Two pieces that touch are one: [s, s+r1) and then [s+r1, ...) replaced
// side by side.
void UndoHistory::mergeTouching(std::vector<Piece>& pieces) {
    if (pieces.size() < 2) return;
    std::size_t out = 0;
    for (std::size_t i = 1; i < pieces.size(); i++) {
        Piece& last = pieces[out];
        Piece& p = pieces[i];
        if (last.start + last.removedLen == p.start) {
            last.removed += p.removed;
            last.inserted += p.inserted;
            last.removedLen += p.removedLen;
            last.insertedLen += p.insertedLen;
        } else {
            if (++out != i) pieces[out] = std::move(p);
        }
    }
    pieces.resize(out + 1);
}

void UndoHistory::dropNoOps(std::vector<Piece>& pieces) {
    pieces.erase(std::remove_if(pieces.begin(), pieces.end(),
                                [](const Piece& p) { return p.removed == p.inserted; }),
                 pieces.end());
}

// One edit, in the text after the pieces, composed into them.
void UndoHistory::compose(std::vector<Piece>& pieces, const Change& c) const {
    const std::size_t cRem = len(c.removed), cIns = len(c.inserted);
    const std::size_t cEnd = c.start + cRem;
    // The first piece whose text (as it is now) ends at or after the edit's
    // start, with the shift of those before it.
    long delta = 0;
    std::size_t j = 0;
    for (; j < pieces.size(); j++) {
        const std::size_t ns = (std::size_t)((long)pieces[j].start + delta);
        if (ns + pieces[j].insertedLen >= c.start) break;
        delta += (long)pieces[j].insertedLen - (long)pieces[j].removedLen;
    }
    // The pieces from j on that touch the edit.
    std::size_t k = j;
    long deltaK = delta;
    std::vector<std::size_t> starts;   // each touching piece's start now
    for (; k < pieces.size(); k++) {
        const std::size_t ns = (std::size_t)((long)pieces[k].start + deltaK);
        if (ns > cEnd) break;
        starts.push_back(ns);
        deltaK += (long)pieces[k].insertedLen - (long)pieces[k].removedLen;
    }
    if (j == k) {
        pieces.insert(pieces.begin() + (long)j,
                      piece((std::size_t)((long)c.start - delta), c.removed, c.inserted));
        return;
    }
    // Typing at the end of a piece, or within what it put in: only its text.
    if (k == j + 1 && starts[0] <= c.start && cEnd <= starts[0] + pieces[j].insertedLen) {
        Piece& p = pieces[j];
        const std::size_t into = c.start - starts[0];
        const std::size_t a = into == p.insertedLen ? p.inserted.size() : index(p.inserted, into);
        const std::size_t b = cRem == 0 ? a : a + index(p.inserted.substr(a), cRem);
        p.inserted.replace(a, b - a, c.inserted);
        p.insertedLen = p.insertedLen - cRem + cIns;
        if (p.removed == p.inserted) pieces.erase(pieces.begin() + (long)j);
        return;
    }
    // The region from the first touching piece or the edit, whichever starts
    // first, to the last or the edit's end: as it is now, and as it was
    // before the step. What lies between pieces is unchanged text inside
    // the edit's removed range.
    const std::size_t n0 = std::min(c.start, starts[0]);
    std::u16string now, before;
    std::size_t pos = n0;
    auto gap = [&](std::size_t to) {
        if (pos >= to) return;
        const std::u16string g = sub(c.removed, pos - c.start, to - pos);
        now += g;
        before += g;
        pos = to;
    };
    for (std::size_t m = j; m < k; m++) {
        gap(starts[m - j]);
        now += pieces[m].inserted;
        before += pieces[m].removed;
        pos = starts[m - j] + pieces[m].insertedLen;
    }
    gap(std::max(pos, cEnd));
    const std::size_t at = index(now, c.start - n0);
    const std::size_t atEnd = at + index(now.substr(at), cRem);
    now.replace(at, atEnd - at, c.inserted);
    Piece merged = piece((std::size_t)((long)n0 - delta), std::move(before), std::move(now));
    pieces.erase(pieces.begin() + (long)j, pieces.begin() + (long)k);
    if (merged.removed != merged.inserted) pieces.insert(pieces.begin() + (long)j, std::move(merged));
}

// The edits of a step, as made, as pieces.
std::vector<UndoHistory::Piece> UndoHistory::normalize(const std::vector<Change>& raw) const {
    std::vector<Piece> pieces;
    if (raw.empty()) return pieces;
    pieces.reserve(raw.size());
    // Last first: each ends at or before the start of the one before, so
    // every start is in the text before them all.
    bool descending = true;
    for (std::size_t k = 1; k < raw.size() && descending; k++)
        descending = raw[k].start + len(raw[k].removed) <= raw[k - 1].start;
    if (descending) {
        for (std::size_t k = raw.size(); k-- > 0;)
            pieces.push_back(piece(raw[k].start, raw[k].removed, raw[k].inserted));
        mergeTouching(pieces);
        dropNoOps(pieces);
        return pieces;
    }
    // First to last: each starts at or after where the one before ended, so
    // each start less the shifts before it is in the text before them all.
    bool ascending = true;
    std::vector<std::size_t> insLens(raw.size());
    for (std::size_t k = 0; k < raw.size(); k++) insLens[k] = len(raw[k].inserted);
    for (std::size_t k = 1; k < raw.size() && ascending; k++)
        ascending = raw[k].start >= raw[k - 1].start + insLens[k - 1];
    if (ascending) {
        long shift = 0;
        for (std::size_t k = 0; k < raw.size(); k++) {
            Piece p = piece((std::size_t)((long)raw[k].start - shift), raw[k].removed, raw[k].inserted);
            shift += (long)p.insertedLen - (long)p.removedLen;
            pieces.push_back(std::move(p));
        }
        mergeTouching(pieces);
        dropNoOps(pieces);
        return pieces;
    }
    for (const Change& c : raw) compose(pieces, c);
    dropNoOps(pieces);
    return pieces;
}

// ---------------------------------------------------------------- recording

void UndoHistory::open(const StepInfo& info, const Selection& before) {
    if (depth_++ > 0) return;
    openInfo_ = info;
    openBefore_ = before;
    raw_.clear();
}

void UndoHistory::rename(const StepInfo& info) {
    if (depth_ > 0) openInfo_ = info;
}

void UndoHistory::record(const Change& c) {
    if (c.removed.empty() && c.inserted.empty()) return;
    if (depth_ == 0) {
        Step s;
        s.pieces = normalize({c});
        commit(std::move(s));
        return;
    }
    raw_.push_back(c);
}

void UndoHistory::close(const Selection& after) {
    if (depth_ == 0 || --depth_ > 0) return;
    Step s;
    s.info = openInfo_;
    s.before = std::move(openBefore_);
    s.after = after;
    s.pieces = normalize(raw_);
    raw_.clear();
    commit(std::move(s));
}

bool UndoHistory::add(const std::vector<Change>& batch, const StepInfo& info, const Selection& before,
                      const Selection& after) {
    std::vector<Piece> pieces;
    pieces.reserve(batch.size());
    std::size_t end = 0;
    for (const Change& c : batch) {
        if (!pieces.empty() && c.start < end) return false;
        pieces.push_back(piece(c.start, c.removed, c.inserted));
        end = c.start + pieces.back().removedLen;
    }
    mergeTouching(pieces);
    dropNoOps(pieces);
    Step s;
    s.info = info;
    s.before = before;
    s.after = after;
    s.pieces = std::move(pieces);
    commit(std::move(s));
    return true;
}

// Typing at the caret the step before left, as one replacement touching it.
bool UndoHistory::joins(const Step& s) const {
    if (broken_ || undo_.empty() || !s.info.coalesce) return false;
    const Step& top = undo_.back();
    if (!top.info.coalesce || top.info.name != s.info.name) return false;
    if (top.after.size() != 1 || s.before != top.after || s.pieces.size() != 1) return false;
    const Range& r = top.after[0];
    const Piece& p = s.pieces[0];
    return r.anchor == r.caret && p.start <= r.caret && r.caret <= p.start + p.removedLen;
}

void UndoHistory::commit(Step s) {
    if (s.pieces.empty()) return;
    redo_.clear();
    if (joins(s)) {
        Step& top = undo_.back();
        for (std::size_t k = s.pieces.size(); k-- > 0;) {
            const Piece& p = s.pieces[k];
            compose(top.pieces, Change{p.start, p.removed, p.inserted});
        }
        top.after = std::move(s.after);
        // Typed and taken back to nothing: no step at all.
        if (top.pieces.empty()) {
            undo_.pop_back();
            broken_ = true;
        }
        return;
    }
    undo_.push_back(std::move(s));
    broken_ = false;
    if (limit_ && undo_.size() > limit_) undo_.erase(undo_.begin(), undo_.end() - (long)limit_);
}

void UndoHistory::noteSelection(const Selection& now) {
    if (depth_ > 0 || undo_.empty()) return;
    if (now != undo_.back().after) broken_ = true;
}

void UndoHistory::clear() {
    undo_.clear();
    redo_.clear();
    broken_ = true;
}

void UndoHistory::setLimit(std::size_t steps) {
    limit_ = steps;
    if (limit_ && undo_.size() > limit_) undo_.erase(undo_.begin(), undo_.end() - (long)limit_);
}

// ---------------------------------------------------------------- undo and redo

std::vector<Text::Edit> UndoHistory::Plan::edits() const {
    std::vector<Text::Edit> out;
    out.reserve(changes.size());
    for (const Change& c : changes)
        out.push_back({c.start, unit == Unit::Utf16 ? c.removed.size() : countCodePoints(c.removed),
                       c.inserted});
    return out;
}

UndoHistory::Outcome UndoHistory::take(std::vector<Step>& from, std::vector<Step>& to, bool undoing,
                                       const Reader& read, Plan* plan) {
    if (from.empty()) return Outcome::Nothing;
    Step& s = from.back();
    Plan p;
    p.unit = unit_;
    p.name = s.info.name;
    p.changes.reserve(s.pieces.size());
    long delta = 0;
    for (const Piece& pc : s.pieces) {
        // Undoing: the piece's inserted text, where it is now; redoing: what
        // it removed, in the text before the step, which is the text now.
        const std::size_t at = undoing ? (std::size_t)((long)pc.start + delta) : pc.start;
        const std::u16string& there = undoing ? pc.inserted : pc.removed;
        const std::size_t length = undoing ? pc.insertedLen : pc.removedLen;
        if (read && read(at, length) != there) {
            clear();
            return Outcome::Refused;
        }
        p.changes.push_back(undoing ? Change{at, pc.inserted, pc.removed}
                                    : Change{at, pc.removed, pc.inserted});
        delta += (long)pc.insertedLen - (long)pc.removedLen;
    }
    p.select = undoing ? s.before : s.after;
    to.push_back(std::move(s));
    from.pop_back();
    broken_ = true;
    if (plan) *plan = std::move(p);
    return Outcome::Done;
}

UndoHistory::Outcome UndoHistory::undo(const Reader& read, Plan* plan) {
    return take(undo_, redo_, true, read, plan);
}

UndoHistory::Outcome UndoHistory::redo(const Reader& read, Plan* plan) {
    return take(redo_, undo_, false, read, plan);
}

UndoHistory::Reader UndoHistory::readerOf(const Text& text) const {
    const Unit u = unit_;
    return [&text, u](std::size_t start, std::size_t length) {
        const std::size_t a = text.toUnits(start, u);
        const std::size_t b = text.toUnits(start + length, u);
        return b > a ? text.slice(a, b - a) : std::u16string();
    };
}

UndoHistory::Outcome UndoHistory::undo(const Text& text, Plan* plan) {
    return undo(readerOf(text), plan);
}

UndoHistory::Outcome UndoHistory::redo(const Text& text, Plan* plan) {
    return redo(readerOf(text), plan);
}

// ---------------------------------------------------------------- foreign changes

bool UndoHistory::transform(Step& step, std::vector<Change>& batch, bool fromAfter) const {
    const auto lenFn = [this](const std::u16string& s) { return len(s); };
    const std::vector<EditSpans::Span> in = spansOf(batch, lenFn);
    std::vector<Change> out;
    out.reserve(batch.size());
    std::vector<Piece> pieces = step.pieces;
    long dP = 0, dF = 0;
    std::size_t i = 0, j = 0;
    while (i < pieces.size() || j < batch.size()) {
        if (i < pieces.size()) {
            Piece& pc = pieces[i];
            // The piece's range in the batch's text.
            const std::size_t a = fromAfter ? (std::size_t)((long)pc.start + dP) : pc.start;
            const std::size_t b = a + (fromAfter ? pc.insertedLen : pc.removedLen);
            const bool fBefore = j < batch.size() && batch[j].start + in[j].length <= a;
            if (!fBefore) {
                if (j < batch.size() && batch[j].start < b) return false;   // they overlap
                pc.start = (std::size_t)((long)pc.start + dF);
                dP += (long)pc.insertedLen - (long)pc.removedLen;
                i++;
                continue;
            }
        }
        Change f = batch[j];
        f.start = (std::size_t)((long)f.start + (fromAfter ? -dP : dP));
        dF += (long)in[j].newLength - (long)in[j].length;
        out.push_back(std::move(f));
        j++;
    }
    step.pieces = std::move(pieces);
    const std::vector<EditSpans::Span> moved = spansOf(out, lenFn);
    mapSelection(fromAfter ? step.after : step.before, in);
    mapSelection(fromAfter ? step.before : step.after, moved);
    batch = std::move(out);
    return true;
}

void UndoHistory::foreign(const std::vector<Change>& batch) {
    if (batch.empty()) return;
    broken_ = true;
    // The undo steps from the top down (each from the text after it to the
    // text before), then the redo steps from the top down (the other way).
    for (int side = 0; side < 2; side++) {
        std::vector<Step>& steps = side == 0 ? undo_ : redo_;
        std::vector<Change> moving = batch;
        for (std::size_t k = steps.size(); k-- > 0;) {
            if (!transform(steps[k], moving, side == 0)) {
                steps.erase(steps.begin(), steps.begin() + (long)k + 1);
                break;
            }
        }
    }
}

}  // namespace Model
