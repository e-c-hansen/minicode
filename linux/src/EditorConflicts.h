// EditorConflicts.h — the merge conflicts in the editor's text on Linux,
// the GTK counterpart of the Mac's MCEditorConflicts (mac/src/
// EditorConflicts.mm), over the core's MergeConflicts.h, as VS Code and
// Zed offer them: each block's current side (ours) and incoming side
// (theirs) tinted behind the text in editor.merge-current and
// editor.merge-incoming, the markers' lines more strongly (VS Code's
// header backgrounds are its content colors at two and a half times the
// alpha), the common ancestor (diff3, zdiff3) in gray, and on the
// "<<<<<<<" line, after its text, Keep Current, Keep Incoming and Keep
// Both, then "Conflict 2 of 3". A press on one replaces the block with the
// side kept (Both: current, then incoming) as one undo step ("Keep Current
// Change"), through MultiEdit::apply, so the highlighter, the language
// server and the change bars follow it as any edit, and onResolved says
// how many are left (main.cpp puts MergeConflicts::resolvedNote in the
// status bar). Next Change and Previous Change (Alt+F5, Shift+Alt+F5;
// GitGutter.h) step between the blocks of a file that has them: the change
// bars skip a conflicted file, whose index entry is no single blob.
//
// Two drawing passes of the view (CodeView.h): the tints below the text,
// the buttons above it, each only for what is on screen. A click in the
// capture phase takes a press on a button before the text view's own
// gestures, as a fold's placeholder does (Folds.h).
//
// The blocks are in characters, as GtkTextIter counts them. They move with
// each edit at once (MergeConflicts::followEdit: an edit touching a block,
// or ending at its start, drops it) and are read again from the text 0.2 s
// after typing pauses, or at once (on idle) when the view shows another
// buffer or turns editable, or the whole text is replaced (a reload).
// Nothing is read unless the view shows the active file's editable source
// with a path: not a Markdown page, a diff, a file still loading,
// large-file mode with its features off (the conditions the change bars
// look under, GitGutter.h, without their setting), or past 20 MB.
#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "MergeConflicts.h"

class Editor;
class Settings;

class EditorConflicts {
public:
    explicit EditorConflicts(Editor* editor);
    ~EditorConflicts();
    EditorConflicts(const EditorConflicts&) = delete;
    EditorConflicts& operator=(const EditorConflicts&) = delete;

    void applySettings(const Settings& s);

    // Read the text again now; soon (0.2 s, restarted by each call).
    void rescanNow();
    void rescanSoon();

    // After a choice: how many blocks are left.
    std::function<void(size_t left)> onResolved;
    // The blocks changed in number (found, resolved, dropped by an edit):
    // Next and Previous Change follow (GitGutter.h).
    std::function<void()> onBlocksChanged;

    size_t count() const { return blocks_.size(); }
    const std::vector<MergeConflicts::Block>& blocks() const { return blocks_; }
    // "current HEAD 1, incoming topic 1, base" per block, as the Mac's
    // suite spells them.
    std::vector<std::string> blockDescriptions() const;

    // A button's rect, in the view's widget coordinates (keep 0 current, 1
    // incoming, 2 both); empty when that block's "<<<<<<<" line is not on
    // screen. positionRect is "Conflict 2 of 3"'s.
    graphene_rect_t buttonRect(int keep, size_t block) const;
    graphene_rect_t positionRect(size_t block) const;
    // The choice made directly (the checks); false, with a bell, when there
    // is no such block or the view cannot be edited.
    bool keep(int keep, size_t block);
    // A press at a point of the view's widget: true when it was on a button.
    bool pressAt(double x, double y);

    // Next and Previous Change: the caret to the next or previous block's
    // "<<<<<<<" line, wrapping. False without blocks.
    bool goToNext(bool forward);

    // The tint colors as drawn (checks read pixels against them).
    const GdkRGBA& currentColor() const { return current_; }
    const GdkRGBA& incomingColor() const { return incoming_; }
    static GdkRGBA headerOf(const GdkRGBA& c);

private:
    struct Button {
        MergeConflicts::Keep keep;
        size_t blockStart;   // which block, by where it starts
        GdkRectangle rect;   // buffer coordinates
    };
    struct Label {
        std::string text;
        GdkRectangle rect;
    };

    bool reads() const;
    void watchBuffer();
    void edited(size_t at, size_t removed, size_t inserted, bool mayMakeMarker, bool whole);
    void setBlocks(std::vector<MergeConflicts::Block> found);
    void visibleChars(const GdkRectangle& visible, size_t* from, size_t* to) const;
    GdkRectangle visibleRect() const;
    std::vector<Button> buttonsIn(const GdkRectangle& visible, std::vector<Label>* labels) const;
    PangoLayout* layout(const std::string& text) const;
    void drawBehind(GtkSnapshot* s, const GdkRectangle& visible);
    void drawOver(GtkSnapshot* s, const GdkRectangle& visible);
    bool keepAt(MergeConflicts::Keep keep, size_t start);
    graphene_rect_t toWidget(const GdkRectangle& r) const;

    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);

    Editor* editor_;
    GtkWidget* view_;
    GtkTextBuffer* buffer_ = nullptr;   // the view's, held while listened to
    std::vector<MergeConflicts::Block> blocks_;
    int behindPass_ = 0, overPass_ = 0;
    guint timer_ = 0, idle_ = 0;
    GtkGesture* click_ = nullptr;
    GdkRGBA current_{0x40 / 255.0f, 0xC8 / 255.0f, 0xAE / 255.0f, 0.2f};
    GdkRGBA incoming_{0x40 / 255.0f, 0xA6 / 255.0f, 1.0f, 0.2f};
    GdkRGBA background_{0x1F / 255.0f, 0x1F / 255.0f, 0x1F / 255.0f, 1};
    GdkRGBA ink_{0xCC / 255.0f, 0xCC / 255.0f, 0xCC / 255.0f, 1};
};
