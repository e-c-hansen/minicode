// AutoCloseInput.h — brackets and quotes that close themselves as they are
// typed, linux/PARITY.md item 1.8 (editor.auto-close, on by default): VS
// Code's rules at its defaults, each language's pairs from VS Code's
// language-configuration.json, all of it the core's AutoClose.h. The Mac's
// is EditingCommands.mm's MCAutoClose, asked by MultiCaretView.mm.
//
// - A character typed at the caret (the view's key handler, before the text
//   view or its input method takes the key) goes to AutoClose::type first:
//   over a closer auto-closing put there it only moves the caret; an opener
//   that closes puts its closer after the caret, one user action, so one
//   Ctrl+Z takes both; over a selection a surrounding pair wraps it (with
//   the setting off too, as on the Mac). Backspace between an empty pair
//   it put in deletes both.
// - Never while an input method composes (the view's preedit), so a dead
//   key or an IBus engine's text goes in as it composes; and never for text
//   an input method commits or replaces, which never comes through here.
// - The language server still hears the typed character last, by itself,
//   with the caret just after it: the closer goes in first, then the typed
//   character at the caret (AutoClose's two changes, applied last first),
//   as typing it would (Lsp.cpp's textEdited reads the character before the
//   caret for its trigger characters).
// - The Tracker remembers the closers each keystroke put in; it follows
//   every edit of the buffer on show and drops a keystroke's closers once
//   the caret leaves their pair or the pair spans lines (checked on every
//   caret move), and starts empty for each buffer the view shows.
//
// Offsets are characters (BufferText.h). The public calls take carets, so
// item 1.9's several carets can ask for every caret at once and apply the
// edit through D5's multi-range function; with one caret, keyPressed does
// it all.
#pragma once

#include <gtk/gtk.h>

#include <vector>

#include "AutoClose.h"
#include "MultiCaret.h"

class Editor;

class AutoCloseInput {
public:
    explicit AutoCloseInput(Editor* editor);
    ~AutoCloseInput();
    AutoCloseInput(const AutoCloseInput&) = delete;
    AutoCloseInput& operator=(const AutoCloseInput&) = delete;

    // From the view's key handler: true when the key was handled here (the
    // text view must not take it).
    bool keyPressed(guint keyval, GdkModifierType mods);

    // The core's answers for the carets (normalized, in characters) on the
    // buffer on show: what typing `ch` does, and Backspace's edit (empty
    // when it is not between pairs it put in at every caret).
    AutoClose::Typed typed(char16_t ch, const std::vector<MultiCaret::Caret>& carets);
    MultiCaret::Edit deletePair(const std::vector<MultiCaret::Caret>& carets);
    // After applying a Close: what the Tracker keeps.
    void remember(const std::vector<AutoClose::Tracker::Closed>& closed);
    // The carets moved (1.9's own moves; the insert mark's are heard here).
    void caretsAre(const std::vector<MultiCaret::Caret>& carets);

    // For the checks: the keystrokes whose closers are still remembered.
    size_t rememberedActions() const { return tracker_.actions(); }

private:
    bool applies() const;   // editable source on show, nothing composing
    bool closing() const;   // editor.auto-close
    const AutoClose::Language& language() const;
    std::vector<MultiCaret::Caret> carets() const;   // every caret (Carets.h)
    bool caretsApplying() const;
    // The edit as typing makes it: one user action, each change put in at a
    // caret placed there, last first; then the carets.
    void apply(const MultiCaret::Edit& e);
    void watchBuffer();

    static void onBuffer(GObject*, GParamSpec*, gpointer self);
    static void onInsert(GtkTextBuffer*, GtkTextIter* at, char* text, int len, gpointer self);
    static void onDelete(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer self);
    static void onMarkSet(GtkTextBuffer*, GtkTextIter*, GtkTextMark* mark, gpointer self);
    static void onCursor(GObject*, GParamSpec*, gpointer self);

    Editor* editor_;
    GtkWidget* view_;
    GtkTextBuffer* buffer_ = nullptr;
    AutoClose::Tracker tracker_;
    bool applying_ = false;
};
