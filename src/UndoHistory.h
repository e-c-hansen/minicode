// UndoHistory.h — a document's undo and redo (SESSION-DESIGN.md, stage 2):
// steps of edits with the selection before and after each, a name
// ("Typing", "Format Document", "Reload from Disk"), and typing joined into
// one step until something breaks it. It is the Mac's NSUndoManager rules
// (docs/notes/editor.md, "Programmatic edits", and docs/notes/sessions.md) as a
// core object any port drives from its edit path:
//
//   - A step is what was opened and closed around edits (a GTK user action,
//     a replaceRanges: call), or a whole batch given at once. A batch of
//     replacements at many places (several carets, Replace All, a
//     formatter's hunks) is one step, undone as one multi-range edit.
//   - Typing coalesces: a step opened with `coalesce` joins the step before
//     when that one was also coalescing typing of the same name, nothing
//     broke coalescing since (breakCoalescing, as breakUndoCoalescing; a
//     selection moved by anything but an edit; an undo or redo), it starts
//     at the selection the step before left, and it is one replacement
//     touching that caret.
//   - Reload from Disk is an ordinary named step: the steps before it stay.
//   - An undo first checks that every range still holds what the step put
//     there (a redo, what it took away). If one does not, nothing is
//     changed and the history is cleared: an older step would be put over
//     text it was not made for. A change made outside the history that the
//     port reports (foreign) moves every step instead, and drops only the
//     steps whose ranges it touched, with everything older.
//
// The history never holds the text: steps keep what they removed and
// inserted, so a port that owns its text (the GTK buffer, the Mac's
// NSTextStorage) records its edits as it makes them and applies the plan
// undo() hands back. Offsets and lengths are in one unit, chosen when the
// history is made: UTF-16 units (the Mac, LSP) or code points (GTK's
// GtkTextIter), as Model::Text counts them. Pure C++17, no I/O.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Text.h"

namespace Model {

class UndoHistory {
public:
    // A selection: the anchor and the caret (equal for an empty one).
    struct Range {
        std::size_t anchor = 0, caret = 0;
        bool operator==(const Range& o) const { return anchor == o.anchor && caret == o.caret; }
        bool operator!=(const Range& o) const { return !(*this == o); }
    };
    // Every caret's selection, the first the main one.
    using Selection = std::vector<Range>;

    // One replacement: `removed` at `start` replaced by `inserted`. As
    // recorded, `start` is in the text as it was just before this change;
    // in a batch or a plan, in the text before the whole batch.
    struct Change {
        std::size_t start = 0;
        std::u16string removed, inserted;
    };

    // What a step is: its name, and whether it may join the step before
    // (typing at the caret).
    struct StepInfo {
        std::string name = "Typing";
        bool coalesce = false;
    };

    explicit UndoHistory(Unit unit = Unit::Utf16) : unit_(unit) {}
    Unit unit() const { return unit_; }

    // ---- recording
    // A step's edits as they are made: open() before the first, record()
    // each in order, close() after the last. Nested pairs are one step (the
    // outermost's info and selections); rename() changes the open step's.
    // A step that changed nothing is dropped. A record() with no step open
    // is a step of its own.
    void open(const StepInfo& info, const Selection& before);
    void record(const Change& c);
    void close(const Selection& after);
    bool isOpen() const { return depth_ > 0; }
    void rename(const StepInfo& info);
    // A whole step at once: replacements ascending and apart in the text
    // before them (Text::apply's rule), each with the text it removed.
    // False, recording nothing, when they are not.
    bool add(const std::vector<Change>& batch, const StepInfo& info, const Selection& before,
             const Selection& after);
    // The next step starts anew whatever its `coalesce` says.
    void breakCoalescing() { broken_ = true; }
    // The selection now: when it is not where the last step left it (a
    // click, an arrow), typing starts a new step. Ignored while a step is
    // open.
    void noteSelection(const Selection& now);
    // Everything forgotten (the text was set whole, as a file read anew).
    void clear();

    // ---- undo and redo
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // The step an undo or redo would take ("" when none), for "Undo Typing".
    std::string undoName() const { return undo_.empty() ? std::string() : undo_.back().info.name; }
    std::string redoName() const { return redo_.empty() ? std::string() : redo_.back().info.name; }
    std::size_t undoSteps() const { return undo_.size(); }
    std::size_t redoSteps() const { return redo_.size(); }

    // What an undo or redo does: replacements ascending and apart in the
    // text as it is now (counted in `unit`), and the selection to show.
    struct Plan {
        Unit unit = Unit::Utf16;
        std::vector<Change> changes;
        Selection select;
        std::string name;
        // As Text::apply takes them.
        std::vector<Text::Edit> edits() const;
    };
    enum class Outcome {
        Done,      // *plan is to be applied (with recording off); the step moved over
        Nothing,   // nothing to undo or redo
        Refused,   // a range no longer holds the step's text: nothing to apply,
                   // and the history is cleared
    };
    // Reads `length` units of the text as it is now, from `start`.
    using Reader = std::function<std::u16string(std::size_t start, std::size_t length)>;
    Outcome undo(const Reader& read, Plan* plan);
    Outcome redo(const Reader& read, Plan* plan);
    Outcome undo(const Text& text, Plan* plan);
    Outcome redo(const Text& text, Plan* plan);

    // ---- changes not recorded
    // A change made outside the history (another renderer's, one a port
    // made with recording off): replacements ascending and apart in the
    // text as it is now. Every step moves through it; a step whose ranges
    // it touches is dropped, and every step older than it on its side.
    void foreign(const std::vector<Change>& batch);

    // At most `steps` undo steps (0, the default, for no limit): the oldest
    // go first.
    void setLimit(std::size_t steps);

private:
    // A replacement in a step: `start` in the text before the step.
    struct Piece {
        std::size_t start = 0;
        std::u16string removed, inserted;
        std::size_t removedLen = 0, insertedLen = 0;   // in the unit
    };
    struct Step {
        StepInfo info;
        Selection before, after;
        std::vector<Piece> pieces;   // ascending and apart, in the text before
    };

    Unit unit_;
    std::vector<Step> undo_, redo_;   // the top at the back
    // The step being recorded: its edits as made, normalized at close().
    int depth_ = 0;
    StepInfo openInfo_;
    Selection openBefore_;
    std::vector<Change> raw_;
    bool broken_ = true;
    std::size_t limit_ = 0;

    std::size_t len(const std::u16string& s) const;
    std::size_t index(const std::u16string& s, std::size_t offset) const;   // offset in units of s
    std::u16string sub(const std::u16string& s, std::size_t from, std::size_t length) const;
    Piece piece(std::size_t start, std::u16string removed, std::u16string inserted) const;
    std::vector<Piece> normalize(const std::vector<Change>& raw) const;
    void compose(std::vector<Piece>& pieces, const Change& c) const;
    static void mergeTouching(std::vector<Piece>& pieces);
    static void dropNoOps(std::vector<Piece>& pieces);
    void commit(Step step);
    bool joins(const Step& step) const;
    Outcome take(std::vector<Step>& from, std::vector<Step>& to, bool undoing, const Reader& read,
                 Plan* plan);
    Reader readerOf(const Text& text) const;
    // Moves `batch` through `step`: from the text after it to the text
    // before (`fromAfter`), or the other way. False when they overlap.
    bool transform(Step& step, std::vector<Change>& batch, bool fromAfter) const;
};

}  // namespace Model
