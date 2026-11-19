// WrapIndentTags.h — wrapped lines keep their indentation in the GTK port
// (the Mac's #214, WrapIndentContainer.{h,mm}; editor.wrapping-indent, the
// core's WrapIndent.h): a wrapped line's continuation rows start under its
// first non-blank character (same, the default, as VS Code's
// editor.wrappingIndent), one level further in (indent), or at the left
// edge (none); a line indented more than half the room a line has goes on
// at the edge whatever the setting says.
//
// GtkTextView has no say per line fragment, as TextKit's container has, but
// a tag's indent is a paragraph's, and a negative one is a hanging indent:
// the first row keeps the full width and every later row is set in by it.
// So every line of a source buffer that has leading spaces or tabs and
// something after them carries one tag, by its leading width in columns (a
// tab to the next multiple of 8, Pango's default stops):
// "mutible-wrap-<columns>". The tags are its editor's tag table's, shared by
// every document (WrapIndentTable), and their indents come from the core's
// WrapIndent::continuationIndent for the mode, a space's width in the
// editor's font and the room a line has, so a zoom, a resize or the setting
// changes a few dozen tags and not a line of any buffer. Nothing is in the
// text, its undo, a copy or what a server gets.
//
// Each source buffer's lines are kept tagged here (WrapIndentTags): an edit
// retags the lines it touched, at once (one line for a typed character,
// nothing at all when its leading width is as before); a big change (a
// file opening, a paste of thousands of lines) gets the lines on screen at
// once and the rest in idle slices of 5 ms, as highlighting does. Only the
// tags at a line's first character count (GTK takes a paragraph's indent
// from there), so a stale one further in is harmless. A paste from another
// buffer brings its tags, which are refused here as the highlighter's are.
// In large-file mode (no wrapping) and with the setting none, no line is
// tagged; tags left from before keep an indent of 0.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>

#include "WrapIndent.h"

namespace WrapIndentTable {
// Sets the mode, a space's width and the room a line has (both in pixels;
// room 0 or less: not known yet, no fall back) for `table`'s tags, setting
// the indent of each tag whose indent changes. True when any did.
bool configure(GtkTextTagTable* table, WrapIndent::Mode mode, double space, double room);
WrapIndent::Mode mode(GtkTextTagTable* table);
// Whether `tag` is one of the wrap tags.
bool isTag(GtkTextTag* tag);
// The indent, in pixels, of a continuation of a line `columns` columns in,
// as configured now (0 for none, or a fall back).
int indentOf(GtkTextTagTable* table, int columns);
}  // namespace WrapIndentTable

class WrapIndentTags {
public:
    // `visibleLines` gives the lines on screen (or around the caret) for
    // a big retag's first part.
    WrapIndentTags(GtkTextBuffer* buffer, std::function<void(size_t&, size_t&)> visibleLines);
    ~WrapIndentTags();
    WrapIndentTags(const WrapIndentTags&) = delete;
    WrapIndentTags& operator=(const WrapIndentTags&) = delete;

    // Keep the lines tagged (every line retagged when it comes on, the
    // lines on screen at once), or stop.
    void setOn(bool on);
    bool on() const { return on_; }

    // For the checks: whether lines wait for an idle retag; the lines
    // retagged since the buffer was made; and the main thread's CPU time
    // (ms) the last edit's retag took.
    bool settled() const { return pendFirst_ >= pendEnd_; }
    size_t linesRetagged() const { return retagged_; }
    double lastEditMs() const { return lastEditMs_; }

private:
    void retagLine(int line);
    // Lines [first, end): at once up to kSyncLines of them, else the lines
    // on screen at once and the rest in idle time.
    void retag(int first, int end);
    void schedule(int first, int end);
    // An edit at `line` added (or, negative, removed) `added` lines: the
    // lines waiting follow it.
    void noteLines(int line, int added);
    void stopIdle();

    static void onInsertAfter(GtkTextBuffer*, GtkTextIter* end, char* text, int len, gpointer self);
    static void onDeleteBefore(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer self);
    static void onDeleteAfter(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer self);
    static void onApplyTag(GtkTextBuffer*, GtkTextTag* tag, GtkTextIter*, GtkTextIter*, gpointer self);
    static gboolean onIdle(gpointer self);

    GtkTextBuffer* buffer_;
    GtkTextTagTable* table_;
    std::function<void(size_t&, size_t&)> visibleLines_;
    bool on_ = false;
    int applying_ = 0;
    int deleting_ = 0;            // lines the delete under way removes
    int pendFirst_ = 0, pendEnd_ = 0;   // lines waiting for the idle retag
    guint idle_ = 0;
    size_t retagged_ = 0;
    double lastEditMs_ = 0;
};
