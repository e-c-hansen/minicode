// BufferLines.h — what the gutter (Gutter.h) knows of one GtkTextBuffer,
// linux/PARITY.md item 1.4: where each of its lines starts, kept edit by
// edit in the core's LineIndex, and the per-file state of the gutter's
// other lanes: the change bars (QuickDiff.h), which move with the lines as
// the buffer is edited, the fold lane's source, and the carets besides the
// insert mark. One is attached to each document's source buffer when the
// buffer is made (Document.cpp) and lives as long as the buffer, as its
// object data, so a tab keeps its lines while another tab is on show and
// whatever shows the buffer finds them with BufferLines::of. The Mac keeps
// the same index in MCLineGutter, fed by NSTextStorage's edits.
//
// Offsets are characters, GtkTextIter's, not the Mac's UTF-16 units: the
// index is fed through LineIndex::editUtf8 from insert-text and
// delete-range, before the buffer changes, so an edit costs the length of
// the text it inserts and nothing is ever counted through the whole buffer
// again (a whole new text, from gtk_text_buffer_set_text, is one insertion).
//
// Lines end after '\n', a '\r' not before '\n', and U+2029, as on the Mac.
// GtkTextBuffer's own lines can differ after some edits: a '\n' typed right
// after a lone '\r' stays a line of its own there (an empty one in the
// view), and text typed between "\r\n" keeps the '\r' inside its line. The
// gutter numbers by this index, on the display line each line starts on, so
// such a display line has no number, and every number is the Mac's.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <vector>

#include "LineIndex.h"
#include "QuickDiff.h"

// The fold lane's source, for item 1.10 (Folding.h holds the regions and
// the folds). The gutter asks about the lines on screen while it draws: a
// chevron beside each line a region starts on while the pointer is over the
// gutter, and always beside a closed fold; a click on one calls toggle.
class GutterFolds {
public:
    virtual ~GutterFolds() = default;
    virtual bool hasClosedFolds() const = 0;
    virtual bool startsRegion(size_t line) const = 0;   // a region could fold here
    virtual bool startsClosedFold(size_t line) const = 0;
    virtual void toggle(size_t line) = 0;
};

class BufferLines {
public:
    // Made once for a buffer, which then owns it; `attach` on a buffer that
    // has one returns that one.
    static BufferLines* attach(GtkTextBuffer* buffer);
    // The buffer's, or null (a Markdown page, a message, the welcome text).
    static BufferLines* of(GtkTextBuffer* buffer);

    BufferLines(const BufferLines&) = delete;
    BufferLines& operator=(const BufferLines&) = delete;

    const LineIndex& index() const { return index_; }
    size_t lineCount() const { return index_.lineCount(); }
    size_t lineOf(size_t offset) const { return index_.lineOf(offset); }
    size_t lineStart(size_t line) const {
        return line < index_.lineCount() ? index_.lineStart(line) : index_.length();
    }
    // The end of a line, its break included (the text's end for the last).
    size_t lineEnd(size_t line) const {
        return line < index_.lineCount() ? index_.lineEnd(line) : index_.length();
    }
    bool isLineStart(size_t offset) const { return index_.isLineStart(offset); }

    // ---- the change lane (item 2.10 sets the bars from QuickDiff::bars)
    // Bars move with each edit (QuickDiff::shift) until the next set; a
    // whole new text clears them.
    void setChangeBars(std::vector<QuickDiff::Bar> bars);
    const std::vector<QuickDiff::Bar>& changeBars() const { return bars_; }

    // ---- the fold lane (item 1.10): not owned; null for none
    void setFolds(GutterFolds* folds);
    GutterFolds* folds() const { return folds_; }

    // ---- several carets (item 1.9): the carets besides the insert mark, as
    // character offsets; their lines are numbered in the active color. Call
    // changed() when they move.
    std::function<void(std::vector<size_t>& offsets)> extraCarets;

    // Something the gutter draws from this changed outside an edit (bars,
    // folds, carets): every gutter showing the buffer draws again.
    void changed();
    // A gutter's interest, while it shows the buffer.
    int listen(std::function<void()> onChange);
    void unlisten(int id);

private:
    explicit BufferLines(GtkTextBuffer* buffer);
    static void onInsert(GtkTextBuffer* b, GtkTextIter* at, char* text, int len, gpointer self);
    static void onDelete(GtkTextBuffer* b, GtkTextIter* from, GtkTextIter* to, gpointer self);
    // The change bars after an insertion of newLen characters at pos, on
    // what was line linesBefore.
    void shiftBars(size_t pos, size_t newLen, size_t linesBefore);

    LineIndex index_;
    std::vector<QuickDiff::Bar> bars_;
    GutterFolds* folds_ = nullptr;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    int nextListener_ = 0;
};

// CPU time spent in this file's handlers and in the gutter (Gutter.cpp), on
// the main thread, for the checks' bound on typing in a long file.
double gutterCpuMs();
void addGutterCpuMs(double ms);
double gutterThreadCpuMs();   // the calling thread's CPU clock, in ms
