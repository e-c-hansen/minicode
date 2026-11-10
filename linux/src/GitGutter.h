// GitGutter.h — the editor's change bars on Linux, linux/PARITY.md item
// 2.10: the GTK counterpart of the Mac's MCGitGutter (src/GitGutter.mm),
// over the core's QuickDiff.h, as VS Code's quick diff draws them. Which
// lines of the file on show were added or changed against the version in
// git's index, and where lines were deleted, in the gutter's change lane
// (Gutter.h, a lane each buffer's BufferLines feeds).
//
// The original is the file's index entry (`git ls-files --stage`, then
// `git cat-file blob` of its object), read again only when the file or the
// index file's stat changed. The diff runs off the main thread, 0.3 s after
// typing pauses (one timer, not one a key), and its answer is dropped when
// an edit came meanwhile; until then the bars move with each edit
// (QuickDiff::shift, in BufferLines). Each tab's buffer keeps its own bars,
// so a tab shown again has them at once, and a look follows soon.
//
// The repository's folder is watched (GitDirWatch.h), so a stage, commit
// or checkout made anywhere moves the bars with no key pressed; the Source
// Control panel's refresh and the window coming to the front look again too.
//
// A click on a bar shows a popover with what the lines were, Revert Change
// (the buffer edit putting them back, through MultiEdit::apply: one undo
// step) and Stage Change (VS Code's `hash-object -w --stdin --path` and
// `update-index --cacheinfo`, of the buffer's text, saved or not; refused
// when the index entry no longer names the object the bars came from).
// Navigate has Go to Next Change and Go to Previous Change (VS Code's
// Alt+F5 and Shift+Alt+F5), View > Source Control Revert Change and Stage
// Change for the bar on the caret's line.
//
// No bars for a file git does not track, a large file with its features
// off (LargeFileView.h), a preview, a diff, or with editor.git-gutter off,
// which also takes the lane away; editor.git-added, editor.git-modified and
// editor.git-deleted are the bars' colors.
//
// git runs through GitRun.h on a thread of its own, one command at a time.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "GitDirWatch.h"
#include "GitModel.h"
#include "QuickDiff.h"

class Editor;
class Settings;
class Gutter;

class GitGutter {
public:
    explicit GitGutter(Editor* editor);
    // Called while the window's widgets still exist. git already running
    // finishes on its own thread; its answer is dropped.
    ~GitGutter();
    GitGutter(const GitGutter&) = delete;
    GitGutter& operator=(const GitGutter&) = delete;

    // editor.git-gutter and the bars' colors.
    void applySettings(const Settings& s);
    // The index may have changed (the panel refreshed, the window came to
    // the front): look again soon.
    void refreshSoon();

    // The window's actions (Keys.cpp names them): nextchange,
    // previouschange, revertchange and stagechange.
    void installActions(GActionMap* window);

    // Next Change and Previous Change: the caret to the next or previous
    // bar, wrapping; false (and a bell) with none.
    bool goToNextChange(bool forward);
    // The bar on the caret's line, or -1.
    int barAtCaret() const;
    // Revert Change and Stage Change for bar `index` (barAtCaret() for the
    // commands); false, with a bell, when there is no such bar.
    bool revertChange(int index);
    bool stageChange(int index);

    // The popover a click on bar `index` shows, its rect in the gutter's
    // coordinates.
    void showChange(int index, const graphene_rect_t& rect);
    void closePopover();

    // ---- what the window gives it
    // What it says (Stage Change's answer), for the status bar.
    std::function<void(const std::string& text)> onNote;
    // It changed the index (Stage Change): the Source Control panel looks
    // again.
    std::function<void()> onIndexChanged;
    // For the checks: Stage Change's end, with its failure ("" for none).
    std::function<void(const std::string& failure)> onStaged;

    // ---- for the checks
    bool on() const { return on_; }
    unsigned looks() const { return looks_; }           // looks begun
    unsigned diffs() const { return diffs_; }           // bars set from a look
    unsigned indexChanges() const { return indexChanges_; }   // the watch saw one
    // "modified 2+1 (was 2+1)", as the Mac's suite spells them.
    std::vector<std::string> barDescriptions() const;
    size_t barCount() const;
    GtkWidget* popover() const { return popover_; }
    bool popoverShown() const;
    const std::string& popoverTitle() const { return popoverTitle_; }
    const std::string& popoverText() const { return popoverText_; }
    GtkWidget* revertButton() const { return revertButton_; }
    GtkWidget* stageButton() const { return stageButton_; }
    // Looks wait while held, and the one wanted runs on release: a check
    // makes the index move on under bars already drawn, as it can between
    // a look and a click, to see Stage Change refuse.
    void holdLooksForChecks(bool hold);

    // What the worker knows of the file last looked at (GitGutter.cpp).
    struct Source;

private:
    // The path to look at now: the file on show while its editable source
    // is, and the bars are on; "" otherwise.
    std::string pathToLook() const;
    Gutter* gutter() const;
    GtkTextBuffer* viewBuffer() const;
    const std::vector<QuickDiff::Bar>& bars() const;
    size_t caretLine() const;

    void attachBuffer();                 // the view's buffer changed
    void detachBuffer();
    void edited();                       // the shown buffer's text changed
    void scheduleAfter(int ms);
    void look();
    void setBars(GtkTextBuffer* buffer, std::vector<QuickDiff::Bar> bars);
    void watch(const std::shared_ptr<const Source>& src);
    void post(std::function<void()> job);
    void updateActions();
    void enable(const char* action, bool on);

    static void onViewBuffer(GObject*, GParamSpec*, gpointer self);
    static void onUnrealize(GtkWidget*, gpointer self);

    Editor* editor_ = nullptr;
    GtkWidget* view_ = nullptr;
    GThreadPool* pool_ = nullptr;
    std::shared_ptr<bool> alive_;

    bool on_ = false;
    GtkTextBuffer* buffer_ = nullptr;    // the view's, held while listened to
    unsigned generation_ = 0;            // every edit, switch and look
    guint timer_ = 0;
    gint64 lastEdit_ = 0;                // monotonic µs
    bool pauseWanted_ = false;
    std::shared_ptr<const Source> source_;   // the worker's last
    std::unique_ptr<GitDirWatch> watch_;
    // What the bars on show were made from, for Revert and Stage.
    GtkTextBuffer* shownBuffer_ = nullptr;   // compared, never followed
    std::string shownPath_;
    QuickDiff::Original original_;
    std::string top_, rel_;
    GitUi::IndexEntry entry_;

    GActionMap* actions_ = nullptr;

    GtkWidget* popover_ = nullptr;
    GtkWidget* titleLabel_ = nullptr;
    GtkWidget* revertButton_ = nullptr;
    GtkWidget* stageButton_ = nullptr;
    GtkWidget* oldScroll_ = nullptr;
    GtkWidget* oldView_ = nullptr;
    int popoverBar_ = -1;
    std::string popoverTitle_, popoverText_;
    GdkRGBA deletedColor_{0.945f, 0.298f, 0.298f, 1};

    unsigned looks_ = 0, diffs_ = 0, indexChanges_ = 0;
    bool held_ = false, lookWanted_ = false;
};
