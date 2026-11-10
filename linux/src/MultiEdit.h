// MultiEdit.h — edits with several ranges, as one user action
// (linux/PARITY.md, D5): the GTK port's counterpart of the Mac's
// replaceRanges:withStrings:actionName: (CodeTextView, src/Lsp.h). Replace
// All uses it now; several carets (item 1.9), rename and formatting
// (Stage 2) and the agents' edits are to use it too, so that every edit
// with more than one range behaves the same.
//
// What apply() does, and why:
//   - The changes come in ascending order and apart (EditSpans::valid), in
//     characters, as GtkTextIter counts them, with their new text in UTF-8.
//     Anything else is refused whole and nothing changes.
//   - They go in as one user action (gtk_text_buffer_begin_user_action), so
//     one Ctrl+Z takes all of them back, and the dirty mark, the Markdown
//     task count and the LaTeX preview look once, at its end.
//   - Last range first, so every earlier offset is still true, and each
//     range is its own buffer edit (a delete and an insert), never one
//     replacement from the first range to the last: the highlighter, the
//     language server, and later the folds and change bars follow each one,
//     as they follow typing. (On the Mac, one storage edit over all of them
//     relexed the whole span and opened every fold between.) Document's
//     highlighter takes the changes of an apply() of 8 ranges or more
//     together at its end (applyingChanges(), EditBatch.h), each re-lexing
//     only its own lines; any other user action of more than 64 edits it
//     finds whole at its end (Document.cpp, "bulk").
//   - With `keepCaret`, the caret keeps its place in the text around it
//     (EditSpans::mapThrough) and the selection goes, as the Mac's find
//     bar leaves it after Replace All; without, the marks are where GTK's
//     gravity left them, for a caller that places the carets itself.
//   - applying() is true while it runs, for anything that reacts to one
//     typed character (the language server's completion triggers) and
//     must not take a replacement for typing.
//
// Cost: a delete and an insert per range, each O(log n) in the buffer, so
// Replace All of 20,000 matches in a 100,000-line file is a fraction of a
// second (the find_bar suite bounds it); typing is untouched.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <string>
#include <vector>

namespace MultiEdit {

struct Change {
    std::size_t start = 0;    // characters from the buffer's start
    std::size_t length = 0;   // characters replaced
    std::string text;         // what replaces them, UTF-8
};

// The changes in `buffer` as one user action, last first. False, changing
// nothing, when they are not ascending and apart or reach past the end.
bool apply(GtkTextBuffer* buffer, const std::vector<Change>& changes, bool keepCaret = true);

// True while apply() is putting changes in.
bool applying();

// While apply() runs: the buffer it edits and its changes, for a listener
// that would rather take them all at once than edit by edit (Document's
// highlighter, which hears begin-user-action before the first edit and
// end-user-action after the last, both inside apply()). Null otherwise.
GtkTextBuffer* applyingTo();
const std::vector<Change>* applyingChanges();

}  // namespace MultiEdit
