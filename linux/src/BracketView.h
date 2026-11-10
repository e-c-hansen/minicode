// BracketView.h — the editor's brackets, linux/PARITY.md item 1.7: the
// bracket by the caret and its partner boxed, Go to Bracket, and the
// bracket-pair colors (BracketColors.h) painted as lines come on screen.
// The Mac's are EditorController+EditorView.mm's updateBracketMatch and
// goToBracket, and CodeTextView's drawBracketBoxesInRect:.
//
// - The match is the core's Brackets::match (BracketMatch.h) over a window
//   of the text around the caret, 20,001 characters either side, with the
//   strings and comments of those lines from the document's highlighter
//   (AutoClose::Utf8HighlighterContext, in characters as GtkTextIter
//   counts) skipped, as VS Code skips them. Nothing is read or lexed unless
//   the character before or after the caret is a bracket. It is worked out
//   once the caret or the text settles (an idle after the change, as the
//   Mac's dispatch to the next turn), with no selection, in editable source
//   only (never a Markdown page or a message), and a file without a
//   grammar matches every bracket.
// - The boxes are a CodeView drawing pass (BelowText, order 10, under the
//   text), in editor.bracket-match: a quarter-strength fill and a 1 px
//   border around each bracket's character, a little shorter than its line
//   as on the Mac.
// - Go to Bracket (VS Code's editor.action.jumpToBracket, Ctrl+Shift+\):
//   Brackets::jumpTarget from the end of the selection.
// - The active document's bracket colors are painted for the lines on
//   screen whenever the view scrolls or changes size, and when another
//   buffer comes on show (the document paints after its own edits).
//
// One per editor; the editor makes it once its CodeView exists and
// deletes it first in its destructor.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <vector>

class Editor;
class Settings;

class BracketView {
public:
    explicit BracketView(Editor* editor);
    ~BracketView();
    BracketView(const BracketView&) = delete;
    BracketView& operator=(const BracketView&) = delete;

    void applySettings(const Settings& s);
    // Go to Bracket: false (the caller rings the bell) when the view does
    // not show editable source or there is no bracket to go to.
    bool goToBracket();

    // For the checks: the characters boxed now (the pair, or none), the
    // boxes the last snapshot drew (buffer coordinates), how many times the
    // match was worked out, and how many of those read the text and its
    // strings and comments because a bracket touched the caret.
    const std::vector<size_t>& boxed() const { return boxed_; }
    const std::vector<GdkRectangle>& drawnBoxes() const { return drawn_; }
    int updates() const { return updates_; }
    int lookups() const { return lookups_; }
    const GdkRGBA& color() const { return color_; }

private:
    bool applies() const;
    void scheduleUpdate();
    void update();
    void setBoxed(std::vector<size_t> boxed);
    void draw(GtkSnapshot* s, const GdkRectangle& visible);
    void watchBuffer();
    void watchAdjustment();
    void paintOnScreen();
    // The window of the text around `caret` as one unit a character, and
    // its strings and comments, relative to `lo`.
    struct Window;
    bool window(size_t caret, Window& w);

    static void onBuffer(GObject*, GParamSpec*, gpointer self);
    static void onAdjustment(GObject*, GParamSpec*, gpointer self);
    static void onMarkSet(GtkTextBuffer*, GtkTextIter*, GtkTextMark* mark, gpointer self);
    static void onChanged(GtkTextBuffer*, gpointer self);
    static void onScrolled(GtkAdjustment*, gpointer self);
    static gboolean onIdle(gpointer self);

    Editor* editor_;
    GtkWidget* view_;
    GtkTextBuffer* buffer_ = nullptr;     // the one whose signals are watched
    GtkAdjustment* adjustment_ = nullptr;
    int pass_ = 0;
    guint idle_ = 0;
    std::vector<size_t> boxed_;
    std::vector<GdkRectangle> drawn_;
    GdkRGBA color_{0x88 / 255.0f, 0x88 / 255.0f, 0x88 / 255.0f, 1};
    int updates_ = 0, lookups_ = 0;
};
