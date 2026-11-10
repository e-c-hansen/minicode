// SnippetSession.h — the tab stops of a snippet completion in the editor's
// text view (linux/PARITY.md item 2.5), the GTK port's counterpart of the
// Mac's MCSnippetSession (src/SnippetSession.mm). The text logic is the
// core's Snippet::Session (src/Snippet.h), the same on both, here in
// characters (Snippet::inCodePoints), which is what GtkTextIter counts.
//
// The language server session (LspSessionCompletion.cpp) puts a snippet's
// expansion into the buffer as one undo step and starts a session for its
// stops; from then on:
//   - Tab goes to the next stop and Shift+Tab to the one before, each
//     selected; a stop's mirrors are carets of their own (Carets.h), so
//     typing goes in at every place; a stop whose copies a transform makes
//     has them written when Tab leaves it. The keys come from Editor's key
//     handler first, before the several carets and the indentation, but
//     after a list showing by the caret (the completion list, a choice
//     stop's options, the signature hint), which keeps its own keys.
//   - A choice stop (${1|a,b,c|}) shows its options in the completion list
//     (onChoices).
//   - It ends at the final stop ($0), on Escape, on an undo or redo, when
//     the selection leaves the current stop (a click, an arrow past its
//     edge; looked at once the carets have settled, in an idle), when a
//     replacement swallows the stop being typed in, when the view shows
//     another buffer, or when another snippet starts.
// Typing in a stop is ordinary typing, each its own undo step; the
// insertion was one, the completion's.
//
// The stops are drawn as faint boxes under the text (a CodeView pass,
// BelowText order 6, just over the several carets' selections): the
// current stop filled and outlined in the accent, the others outlined, an
// empty stop and the final one a thin bar. Nothing is put in the buffer
// (D6).
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "EditSpans.h"
#include "Snippet.h"

class Editor;

class SnippetSession {
public:
    explicit SnippetSession(Editor* editor);
    ~SnippetSession();
    SnippetSession(const SnippetSession&) = delete;
    SnippetSession& operator=(const SnippetSession&) = delete;

    bool active() const { return active_; }
    // A session for an expansion (in UTF-16 units, as Snippet::expand made
    // it) whose text was just put into the buffer on show at character
    // `base`, its first stop selected. With no stop but the final one it
    // does nothing but end a running session; the caller then puts the
    // caret at the final stop.
    void start(const Snippet::Expansion& expansion, size_t base);
    void end();

    // Tab, Shift+Tab and Escape while a session runs (from Editor's key
    // handler); true when the key was taken.
    bool keyPressed(guint keyval, GdkModifierType mods);

    // A choice stop was reached: its options, and the stop's place in
    // characters (selected).
    std::function<void(const std::vector<std::string>& choices, size_t start, size_t end)> onChoices;

    // While held, the session lets edits, undo and selection changes pass
    // it by (an accepted completion put in again with an import that came
    // later, as one undo step); release moves it by `moved`, edits made
    // meanwhile in characters of the text as it was when held, ascending.
    void hold() { held_++; }
    void release(const std::vector<EditSpans::Span>& moved);

    // ---- for the checks
    // The snippet's index of the current stop (0 the final), -1 with no
    // session; the current stop's places (where the carets go) and every
    // living stop's, in characters, the current one's first.
    int currentStopIndex() const;
    std::vector<std::pair<size_t, size_t>> currentRanges() const;
    std::vector<std::pair<size_t, size_t>> allRanges() const;
    // How many boxes and bars the last frame drew.
    size_t drawnBoxes() const { return drawnBoxes_; }

private:
    GtkTextView* view() const { return GTK_TEXT_VIEW(view_); }
    void watchBuffer();
    void selectRanges(const std::vector<Snippet::Range>& ranges);
    void selectCurrent();
    void writeTransforms();
    void edited(size_t start, size_t removed, size_t inserted);
    void scheduleCheck();
    void checkSelection();
    void draw(GtkSnapshot* s, const GdkRectangle& visible);
    void drawRange(GtkSnapshot* s, const GdkRectangle& visible, size_t a, size_t b,
                   const GdkRGBA& fill, const GdkRGBA& line);
    void drawBar(GtkSnapshot* s, const GdkRectangle& visible, size_t at, const GdkRGBA& color);

    static void onBuffer(GObject*, GParamSpec*, gpointer self);
    static void onInsert(GtkTextBuffer*, GtkTextIter* at, char* text, int len, gpointer self);
    static void onDelete(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer self);
    static void onUndo(GtkTextBuffer*, gpointer self);
    static void onMarkSet(GtkTextBuffer*, GtkTextIter*, GtkTextMark* mark, gpointer self);

    Editor* editor_;
    GtkWidget* view_;
    GtkTextBuffer* buffer_ = nullptr;   // the view's, held while watched
    Snippet::Session s_;
    bool active_ = false;
    int applying_ = 0;   // the session is selecting or writing copies itself
    int held_ = 0;
    guint checkId_ = 0;
    int pass_ = 0;
    size_t drawnBoxes_ = 0;
};
