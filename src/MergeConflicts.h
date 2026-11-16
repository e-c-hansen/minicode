// MergeConflicts.h — pure C++, part of the portable core. The conflict
// blocks git leaves in a file when a merge, pull, rebase, cherry-pick or
// stash pop (or Apply to Main) stops on a conflict, and what keeping one
// side of a block, or both, makes of the text: VS Code's Accept Current
// Change, Accept Incoming Change and Accept Both Changes, and Zed's
// buttons over each conflict.
//
//     <<<<<<< HEAD              the current side (ours), labelled
//     a line of ours
//     ||||||| base              diff3 and zdiff3 only: the common ancestor
//     the line before
//     =======
//     a line of theirs          the incoming side
//     >>>>>>> feature
//
// A marker is its seven characters at a line's start, then the line's end
// or a space or tab before a label ("=======" takes only blanks after it);
// eight or more, or an indented marker, is text, as git's own longer
// markers for a conflict inside a conflict are. A block missing its
// separator or its end is left alone, and so is one a second "<<<<<<<"
// interrupts (that one starts again). Lines end at "\n", a "\r" before it
// part of the break, so CRLF files read as LF ones do.
//
// The Mac's editor (EditorConflicts.h) tints and offers the three choices
// over each block, and the review page (ChangeReview::conflictFile) shows a
// conflicted file's blocks with the same choices. Tested in
// tests/merge_conflicts_tests.inc (`conflicts:*`).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace MergeConflicts {

constexpr size_t npos = (size_t)-1;

// Offsets are in the text's own units (UTF-16 for the Mac, bytes for
// UTF-8), each a line's start or the text's end.
struct Block {
    size_t start = 0;            // the "<<<<<<<" line's start
    size_t end = 0;              // past the ">>>>>>>" line's break (the text's end without one)
    size_t currentStart = 0, currentEnd = 0;     // the current side's lines
    size_t baseMarker = npos;                    // the "|||||||" line, with diff3 or zdiff3
    size_t baseStart = npos, baseEnd = npos;     // the common ancestor's lines
    size_t separator = 0;                        // the "=======" line
    size_t incomingStart = 0, incomingEnd = 0;   // the incoming side's lines
    size_t endMarker = 0;                        // the ">>>>>>>" line
    // What follows each marker, UTF-8: "HEAD", "feature", "parent of 1a2b3c4 (...)".
    std::string currentLabel, baseLabel, incomingLabel;
    bool hasBase() const { return baseMarker != npos; }
};

std::vector<Block> parse(const std::u16string& text);
std::vector<Block> parse(const std::string& text);
// Whether the text could hold a block at all (a "<<<<<<<" anywhere), the
// cheap test before a parse.
bool mayHold(const std::u16string& text);

enum class Keep { Current, Incoming, Both };

// One replacement: `length` units at `start` become `text`.
template <class S>
struct BasicSplice {
    size_t start = 0, length = 0;
    S text;
};
using Splice = BasicSplice<std::u16string>;
using Splice8 = BasicSplice<std::string>;

// The block replaced by the side kept (Both: current, then incoming), its
// markers and the common ancestor gone. A block ending the text without a
// line break leaves the kept text without its last one too.
Splice resolve(const std::u16string& text, const Block& b, Keep keep);
Splice8 resolve(const std::string& text, const Block& b, Keep keep);
// The whole text with block `b` resolved.
std::u16string resolved(const std::u16string& text, const Block& b, Keep keep);
// The whole text with every block resolved the same way (the review page's
// Side by Side: every current side against every incoming one). `blocks`
// as parse gave them.
std::u16string resolvedAll(const std::u16string& text, const std::vector<Block>& blocks, Keep keep);

// The blocks moved through an edit made after they were read (`removed`
// units at `at` replaced by `inserted`): those after it move, those it
// touches are dropped until the text is parsed again.
void followEdit(std::vector<Block>& blocks, size_t at, size_t removed, size_t inserted);

// The block to go to from `offset` (a caret): forward, the first starting
// after it; backward, the last starting before the block holding it. Both
// wrap around; -1 with no blocks.
long step(const std::vector<Block>& blocks, size_t offset, bool forward);
// The block holding `offset`, or -1.
long blockAt(const std::vector<Block>& blocks, size_t offset);

// Words.
std::string buttonTitle(Keep keep);     // "Keep Current", "Keep Incoming", "Keep Both"
std::string actionName(Keep keep);      // the undo step: "Keep Current Change", ...
std::string tooltip(Keep keep, const Block& b);   // "Keep HEAD's lines and drop feature's"
std::string leftText(size_t left);      // "2 conflicts left", "1 conflict left", "No conflicts left"
std::string positionText(size_t index, size_t count);   // "Conflict 2 of 3"
// The status note after a choice in a file, "resolved.txt" its name.
std::string resolvedNote(const std::string& name, size_t left);

}  // namespace MergeConflicts
