// Carets.h — several carets in the editor's text view, and the line
// commands (linux/PARITY.md item 1.9; D5 and D7). The Mac's counterpart is
// MultiCaretView.{h,mm}; the text logic is the core's MultiCaret.h, the same
// on both, and this is the GTK glue.
//
// GtkTextBuffer holds one selection. The carets are kept here as character
// offsets (D7: a GtkTextMark each cost more and bought nothing, since the
// core says where every caret goes after an edit, and any edit or caret
// move not made here ends them, as on the Mac), the first of them as the
// buffer's own selection, which GTK draws and blinks, the others drawn by
// two CodeView passes: their selections under the text (BelowText 5), their
// carets over it (AboveText 10), only those on screen. The gutter numbers
// their lines in the active color (BufferLines::extraCarets).
//
// What makes several carets, by VS Code's Linux keys and mouse (D1):
//   - Alt+click adds a caret, or takes away one already there; Alt+drag, a
//     middle-button drag, and Shift+Alt+click or drag (from the caret, or
//     from where the last column began) make a column: a caret, or a
//     selection, on every line between the press and the pointer. A middle
//     click that does not drag pastes the primary selection, as GTK does.
//   - Ctrl+D selects the word at the caret, then adds the next place its
//     text occurs; Ctrl+Shift+L selects every place; Ctrl+Shift+Up and
//     Ctrl+Shift+Down add a caret on the line above or below each one
//     (GNOME keeps Ctrl+Alt+arrows); the find bar's Alt+Enter selects its
//     matches. At most 10,000 carets come of these, as in VS Code, with a
//     note in the status bar.
// What acts at every caret, as one undo step through MultiEdit::apply (D5):
// typing (never across a line break; brackets and quotes auto-closing at
// each, AutoCloseInput's rules), Backspace (a pair it put in going whole),
// Delete, Return (keeping each line's indentation), Tab, Ctrl+Backspace and
// Ctrl+Delete, Copy, Cut and Paste (a paste of as many lines as carets puts
// one at each). The arrows, Home and End, Ctrl+Left and Ctrl+Right move
// every caret, Shift selecting; Ctrl+Home and Ctrl+End leave one caret, or
// one selection from the farthest anchor; Escape goes back to the first.
// Anything else (a click, Page Down, Select All, an undo, a tab switch,
// Find) ends them, with the buffer's selection where GTK puts it.
//
// For one caret or several, in source text:
//   - Ctrl+L selects the line (and the next with each press), Ctrl+Shift+K
//     deletes the lines, Alt+Up and Alt+Down move them, Shift+Alt+Up and
//     Shift+Alt+Down copy them; Ctrl+C and Ctrl+X with nothing selected take
//     the whole lines, and such a copy, pasted with nothing selected, goes
//     in above the caret's line.
//   - Words are VS Code's default editor.wordSeparators (arr.size is two):
//     a double-click (and a drag after it), Ctrl+Left and Ctrl+Right (with
//     Shift), Ctrl+Backspace and Ctrl+Delete. A preview keeps GTK's words.
//   - Ctrl+Up and Ctrl+Down scroll a line, the caret staying, as VS Code's
//     scrollLineUp and scrollLineDown (GTK's move by paragraphs).
//
// Not done: an input method's composed text goes in at the first caret
// only (and ends the others), the extra carets do not blink, and a column
// drag does not scroll the view when the pointer leaves it.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "MultiCaret.h"

class CaretText;
class Editor;

class Carets {
public:
    explicit Carets(Editor* editor);
    ~Carets();
    Carets(const Carets&) = delete;
    Carets& operator=(const Carets&) = delete;

    // VS Code's editor.multiCursorLimit.
    static constexpr size_t kMaxCarets = 10000;

    // ---- state
    // Two or more carets are in play.
    bool active() const { return carets_.size() > 1; }
    // While this edits the text or moves the selection itself.
    bool applying() const { return applying_ > 0; }
    // Every caret in the text's order (one: the buffer's selection), in
    // characters, head where the caret is drawn.
    std::vector<MultiCaret::Caret> current() const;
    // Shows these carets (normalized); one goes back to being the buffer's
    // selection.
    void set(std::vector<MultiCaret::Caret> carets);
    // Back to the buffer's own selection as it is.
    void clear();
    // Text put in at every caret over its selection, as one undo step (a
    // snippet's choice taken at a stop and its mirrors, SnippetSession.h);
    // false, doing nothing, with fewer than two carets.
    bool insertText(const std::string& utf8);

    // ---- input, from Editor's key handler (capture phase): true when the
    // key was taken here.
    bool keyPressed(guint keyval, GdkModifierType mods);

    // ---- commands (the window's actions, addActions); false when there
    // was nothing to do (the action rings the bell).
    bool addNextOccurrence();
    bool selectAllOccurrences();
    bool addCursorVertically(int direction);
    bool expandLineSelection();
    bool deleteLines();
    bool moveLines(bool down);
    bool duplicateLines(bool down);
    // The find bar's Alt+Enter: a selection at each of these (start and
    // length in characters), the 10,000 around `current` when there are
    // more.
    bool selectRanges(const std::vector<std::pair<size_t, size_t>>& ranges, size_t current);

    // The window's actions for the commands above (Keys.cpp's table):
    // win.addnextoccurrence, win.selectalloccurrences, win.addcursorabove,
    // win.addcursorbelow, win.selectline, win.deleteline, win.movelinesup,
    // win.movelinesdown, win.copylinesup, win.copylinesdown. Each acts while
    // the editor's view has the keyboard, else rings the bell.
    static void addActions(GActionMap* window, Editor* editor);

    // A line for the status bar ("The number of cursors has been limited to
    // 10000").
    std::function<void(const std::string&)> onNote;

    // ---- for the checks
    // The heads of the extra carets the last frame drew, and how many extra
    // selections it drew.
    const std::vector<size_t>& drawnCarets() const { return drawnCarets_; }
    size_t drawnSelections() const { return drawnSelections_; }
    // The main thread's CPU time the last edit at the carets took here, in
    // milliseconds (the edit itself, through MultiEdit and every buffer
    // signal).
    double lastEditMs() const { return lastEditMs_; }

private:
    using Caret = MultiCaret::Caret;

    GtkTextView* view() const;
    GtkTextBuffer* buffer() const { return buffer_; }
    bool applies() const;   // editable source on show
    bool inSource() const;  // source on show, editable or not
    size_t length() const;
    std::vector<Caret> selectionCarets() const;   // the buffer's selection as one caret
    void selectInBuffer(const Caret& c);
    void showChanged();   // redraw, the gutter's lines, the auto-close tracker

    // Edits (CaretEdits.cpp): one MultiEdit, one undo step, then the carets.
    // `text` turns the core's units back into the text (CaretText.h); null
    // for edits whose text is ASCII (the auto-closing rules').
    bool applyEdit(const MultiCaret::Edit& e, const CaretText* text);
    void insertStrings(const std::vector<std::string>& texts);
    void typeChar(gunichar c);
    void backspace();
    void deleteForward();
    void deleteBy(MultiCaret::Move m);
    void newline();
    void tab();
    void moveAll(MultiCaret::Move m, bool extend);
    void moveVertically(int direction, bool extend);
    void collapse();
    bool findStateHolds() const;
    void rememberFind(const MultiCaret::Found& f);
    bool nothingSelected() const;
    // Copy, Cut and Paste (the view's signals); true when handled here.
    bool copy(bool cut);
    bool paste();
    void pasteText(const std::string& utf8, bool lines);
    // The primary clipboard off the buffer while the selection changes
    // (null: it was not on), and back.
    GdkClipboard* takePrimary();
    void givePrimary(GdkClipboard* primary);
    // Scrolling: an offset, the buffer's insert mark, a line up or down.
    void reveal(size_t offset);
    void revealCaret();
    void scrollLines(int count);

    // Geometry: an offset for a point in buffer coordinates, a caret's x,
    // and where an offset goes a display line up or down at x (-1: none).
    size_t offsetAt(int bx, int by) const;
    int xOf(size_t offset) const;
    long acrossLine(size_t offset, int direction, int x) const;
    std::vector<Caret> column(size_t a0, int x0, size_t a1, int x1) const;

    // Drawing.
    void drawSelections(GtkSnapshot* s, const GdkRectangle& visible);
    void drawCarets(GtkSnapshot* s, const GdkRectangle& visible);
    std::pair<size_t, size_t> visibleChars(const GdkRectangle& visible) const;

    // Signals.
    void watchBuffer();
    static void onBuffer(GObject*, GParamSpec*, gpointer self);
    static void onInsert(GtkTextBuffer*, GtkTextIter*, char*, int, gpointer self);
    static void onDelete(GtkTextBuffer*, GtkTextIter*, GtkTextIter*, gpointer self);
    static void onMarkSet(GtkTextBuffer*, GtkTextIter*, GtkTextMark*, gpointer self);
    static void onMoveCursor(GtkTextView*, GtkMovementStep, int, gboolean, gpointer self);
    static void onDeleteFrom(GtkTextView*, GtkDeleteType, int, gpointer self);
    static void onCopy(GtkTextView*, gpointer self);
    static void onCut(GtkTextView*, gpointer self);
    static void onPaste(GtkTextView*, gpointer self);
    static void onPressed(GtkGestureClick*, int n, double x, double y, gpointer self);
    static void onDragUpdate(GtkGestureDrag*, double dx, double dy, gpointer self);
    static void onDragEnd(GtkGestureDrag*, double dx, double dy, gpointer self);

    Editor* editor_;
    GtkWidget* view_;
    GtkTextBuffer* buffer_ = nullptr;
    std::vector<Caret> carets_;   // two or more while active, else empty
    std::vector<int> goalX_;      // for Up and Down in a row; empty otherwise
    int applying_ = 0;
    bool primaryTaken_ = false;
    int belowPass_ = 0, abovePass_ = 0;
    std::vector<size_t> drawnCarets_;
    size_t drawnSelections_ = 0;
    double lastEditMs_ = 0;

    // Where the last column began (its character and x), and the carets it
    // left: a Shift+Alt press goes on from it while those still stand.
    bool hasAnchor_ = false;
    size_t anchorIndex_ = 0, anchorLength_ = 0;
    int anchorX_ = 0;
    std::vector<Caret> anchorCarets_;

    // What the last Ctrl+D or Ctrl+Shift+L left: the selection it added,
    // whether it matches whole words, and the carets and length then.
    bool hasFind_ = false;
    Caret findLast_;
    bool findWholeWord_ = false;
    std::vector<Caret> findCarets_;
    size_t findLength_ = 0;

    // A press being followed (onPressed to onDragEnd).
    enum class Press { None, Word, Line, Column, AltClick, Middle };
    Press press_ = Press::None;
    size_t pressIndex_ = 0;
    int pressX_ = 0, pressY_ = 0;   // buffer coordinates
    double pressWx_ = 0, pressWy_ = 0;   // the widget's, for the drag's offsets
    bool dragged_ = false;
    Caret wordAnchor_;   // the word a double-click selected
    size_t colAnchor_ = 0;
    int colAnchorX_ = 0;
    GtkGesture* click_ = nullptr;
    GtkGesture* drag_ = nullptr;
    // The last press, to count clicks (onPressed).
    int clicks_ = 0;
    guint clickButton_ = 0;
    guint32 clickTime_ = 0;
    double clickX_ = 0, clickY_ = 0;
    // Paste's late answers find the object gone once this goes.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};
