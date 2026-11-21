// ChangeReviewPage.h — the review of every uncommitted change in one tab
// (an EditorPage), the GTK counterpart of the Mac's MCChangeReviewPage
// (mac/src/ChangeReviewPage.mm): the staged files, then the changed ones,
// each a header line (its path, "+12 -3", and Side by Side, Open, Discard
// File and Stage File, or Unstage File) followed by its changes, every run
// of changed lines a block of its own with removed lines tinted red, added
// ones green, the changed words marked more strongly and the file's syntax
// colors on both; unchanged stretches fold to "14 unchanged lines", which a
// click opens. Each block has Stage and Discard beside its first line
// (Unstage in the staged list).
//
// Every rule is the core's (ChangeReview.h for the rows and what a block
// makes of the file, ChangeReviewRead.h for reading git and writing the
// index or the file); this shows them. Reading and each action run on the
// Source Control panel's git thread (GitPanel::runOnGitThread), so they
// stay in order with the panel's own commands; Discard asks through the
// panel's question, puts a copy in the Trash (GitTrash.h) and is remembered
// there, so Undo Discard (the page's button, the palette) puts it back. The
// page reads again when the panel reads the repository (an action, the
// window coming to the front, files changing: the panel keeps reading while
// the page is open) and keeps the place the eye was on.
//
// A worktree's page (the second constructor, the Mac's initForWorktree:,
// the tree's Compare with Main) is the same page over what the worktree
// changed since its branch left main: every file committed or not and the
// untracked ones, plain (Side by Side and Open on each, no Stage), against
// main at the merge base (ChangeReviewRead.h's readWorktree), with Apply to
// Main... at the top. It asks, naming the files, and refuses, naming them,
// when main has changes of its own in those; it brings the changes into the
// main checkout as uncommitted changes and says so on a line of its own with
// Show in Source Control beside it; the button then reads "Applied to Main"
// until the worktree's changes differ from those brought in.
//
// A conflicted file in Changes (the Mac's, 2026-11-16) is read from the
// work tree and laid out by ChangeReview::conflictFile: each block whole,
// its markers kept, its current side, ancestor and incoming side tinted in
// editor.merge-current, gray and editor.merge-incoming (the markers more
// strongly), with Keep Current, Keep Incoming and Keep Both on its first
// row. A choice writes the file through the atomic writer once its bytes
// are checked to be as read (no Trash copy; a tab with unsaved text for it
// refuses), and a tab open on it follows the disk. The header has Side by
// Side (every current side against every incoming one) and Open, and once
// no block is left, Mark Resolved, which stages the file without the
// panel's question about conflict marks.
//
// A click on a file in the Source Control panel opens the page at that file
// (revealFile), in the preview tab for a single click, as the Mac's #217.
// After Stage, Unstage or Discard (a block or a whole file) the first read
// started after the action keeps the place the eye was on: the row
// ChangeReview::rowAfterAction gives (the next block of that file, else
// what followed it) goes where the pressed row was on screen, though a
// staged list appears above it; other reads keep the top row (anchorAt).
//
// A binary picture (ChangeReview::showAsImages: png, jpg, gif, webp, tiff,
// bmp, heic, ico, icns, as far as GTK's image loaders read them) shows its
// versions, Before and After side by side, each labelled with its size in
// pixels and bytes, at its pixel size and never taller than 320 pixels: the
// sizes are read from the headers and the pictures decoded at the size they
// are drawn, on the git thread, and laid in the text over the file's first
// RowKind::Image row (the rest hidden). An animated GIF shows its first
// frame.
//
// A commit's page (the third constructor, the Mac's initForCommitsWithRoot:
// and showCommit:title:, a click on a commit in the Source Control graph):
// its subject, the byline under it (ChangeReview::commitByline, with the
// counts), the rest of the message, then every file it changed against its
// first parent (against nothing for a root commit), plain and read once by
// readCommit on the git thread: Side by Side and Open on each file, no
// Stage or Discard. Its key is "commit:<root>:<hash>", so a commit already
// kept shows that tab.
//
// F7 and Shift+F7 (the page's key controller, as the compare view's) step to
// the next or previous change block across files, wrapping, a quarter of the
// way down the pane (ChangeReview::stepBlock). A right click on selected
// changed lines of one file offers Stage Selected Lines and Discard Selected
// Lines... (Unstage Selected Lines in the staged list); on a conflict's rows,
// Keep Current, Keep Incoming and Keep Both. They run the block buttons' way
// (ChangeReview::linesResult, the same question and Trash copy for Discard);
// a commit's or a worktree's page, and a click with nothing selected, keep
// the text view's own menu (Copy).
#pragma once
#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ChangeReview.h"
#include "ChangeReviewRead.h"
#include "ComparePanes.h"
#include "EditorPage.h"
#include "Settings.h"

class GitPanel;

class ChangeReviewPage : public EditorPage {
public:
    ChangeReviewPage(GitPanel* panel, const std::string& root);
    // A worktree against main (Compare with Main): its folder, its name for
    // the title, its branch, and the main branch (or commit) to compare with.
    ChangeReviewPage(GitPanel* panel, const std::string& worktree, const std::string& label,
                     const std::string& branch, const std::string& mainRef);
    // A commit (the hash as the graph gives it, hex digits) and the subject
    // the graph showed, for the tab's title.
    ChangeReviewPage(GitPanel* panel, const std::string& root, const std::string& hash,
                     const std::string& subject);
    ~ChangeReviewPage() override;
    GtkWidget* widget() override { return root_; }
    GtkWidget* focusWidget() override { return view_; }
    std::string key() const override {
        return !commit_.empty() ? "commit:" + root_s_ + ":" + commit_
               : worktree_.empty() ? "review:" + root_s_ : "worktree:" + worktree_;
    }
    bool isCommit() const { return !commit_.empty(); }
    const std::string& commit() const { return commit_; }
    std::string title() const override;

    // The window's hooks: open a file at a 1-based line (0: wherever it
    // was), show two versions side by side, whether a tab holds unsaved text
    // for a path (a discard then leaves the file alone), and a syntax color.
    std::function<void(const std::string& path, int line)> onOpenFile;
    std::function<void(const CompareSpec&)> onShowComparison;
    std::function<bool(const std::string& path)> hasUnsavedText;
    std::function<Rgba(int style)> syntaxColor;
    // editor.merge-current (false) and editor.merge-incoming (true): a
    // conflict block's tints.
    std::function<Rgba(bool incoming)> mergeColor;
    // After Apply to Main: show the main checkout's changes (the path of the
    // main checkout).
    std::function<void(const std::string& mainPath)> onShowSourceControl;
    // The colors after a settings change.
    void applySettings();

    // Read everything again, keeping the place.
    void reload();
    // Scroll to a file's header (its path from the top level, in the staged
    // list or the changes) once read, or to the top for "" (the Mac's
    // revealFile:staged:); reads again.
    void revealFile(const std::string& rel, bool staged);
    // The changes' own: Undo Discard.
    void undoDiscard();

    // A worktree's page: Apply to Main..., as the button does (asks first).
    void applyToMain();
    bool isWorktree() const { return !worktree_.empty(); }
    const std::string& worktree() const { return worktree_; }
    bool applied() const { return applied_; }
    bool applying() const { return applying_; }
    // The title of the Apply button, and the Show in Source Control one.
    std::string applyTitle() const;
    bool applyEnabled() const;
    bool sourceButtonShown() const;
    GtkWidget* applyButton() const { return apply_; }
    GtkWidget* sourceButton() const { return source_; }
    void pressSourceButton();
    // The files the page shows, as "A agent.txt" (letter, path).
    std::vector<std::string> fileLines() const;

    // ---- for the checks
    bool loaded() const { return reads_ > 0 && !loading_; }
    int reads() const { return reads_; }
    bool shows(const std::string& title) const;   // a header or section row with these words
    std::string pageText() const;
    std::string headerDetail() const;
    std::string headerTitle() const;   // the header's first line: a commit's subject
    std::string bodyText() const;      // a commit's message after its subject ("" on other pages)
    std::string messageText() const;
    bool messageOk() const { return messageOk_; }
    size_t blockCount() const;
    size_t fileCount() const;
    // A row's buttons, left to right.
    std::vector<std::string> buttonsOfRow(size_t row) const;
    // The row of a block (page order), of a file by path and list, or -1.
    long rowOfBlock(size_t block) const;
    long rowOfFile(const std::string& path, bool staged) const;
    // Presses the button of that title on that row, as a click does.
    bool press(const std::string& title, size_t row);
    // Presses the click on a folded row.
    bool openGap(size_t row);
    GtkWidget* undoButton() const { return undo_; }
    GtkWidget* buttonWidget(const std::string& title, size_t row) const;
    void scrollToRow(size_t row);
    // F7 (forward) or Shift+F7: the next or previous block; false with no
    // blocks. The block's row after a step, or -1.
    bool stepBlock(bool forward);
    long currentBlockRow() const { return currentBlockRow_; }
    // The right click's items for a click on `row` (-1: none) and the
    // selection now, by title; runMenuItem runs one as a click on it does.
    std::vector<std::string> menuItems(long row);
    bool runMenuItem(const std::string& title);
    // The row's top `y` pixels below the pane's top: a scroll to a mark now
    // (GTK waits for the lines around it to be laid out), then put right on
    // each frame until it holds (onSettle), since a page just made and not
    // yet given its size, or still being measured, left it elsewhere. A
    // reveal and the place an action keeps go this way.
    void scrollRowTo(size_t row, double y);
    GtkTextView* textView() const { return GTK_TEXT_VIEW(view_); }
    std::string rowText(size_t row) const;
    // The tint (paragraph background) a row is drawn with, "" for none.
    std::string rowTint(size_t row) const;
    // The block a row is in, or -1.
    int rowBlock(size_t row) const;
    size_t rowCount() const;
    // Where a row's top is, in pixels below the top of the pane, and how
    // tall the pane is.
    double rowScreenY(size_t row) const;
    double paneHeight() const;
    long firstVisibleRow() const { return topRow(); }
    // For the checks only: the row the caret is on, and a selection of one
    // row's text (the caret at its end), as a drag over it makes.
    long caretRow() const;
    void selectRowText(size_t row);
    // The pictures on show, in page order: each one's label ("Before: 4 ×
    // 3 pixels, 75 bytes"), its widget, and the size it is drawn at.
    std::vector<std::string> imageLabels() const;
    std::vector<GtkWidget*> imageWidgets() const;
    std::vector<std::pair<int, int>> imageSizes() const;

private:
    struct Data;
    enum class Act { Stage, Unstage, Discard, Side, Open, DiscardFile, StageFile, UnstageFile,
                     KeepCurrent, KeepIncoming, KeepBoth, MarkResolved };
    struct Button {
        Act act;
        size_t row;
        std::string title;
        GtkWidget* widget;
    };

    void build();
    void show(std::shared_ptr<Data> d);
    void fill();
    void updateSummary();
    std::vector<Act> actsFor(size_t row) const;
    static const char* titleOf(Act a);
    void act(Act a, size_t row);
    void runOp(ChangeReview::Op op, size_t fi, long block);
    // The same on a read: a block, or (block < 0) the chosen lines.
    void runOpOn(std::shared_ptr<Data> d, ChangeReview::Op op, size_t fi, long block,
                 std::vector<int> oldLines, std::vector<int> newLines);
    static gboolean onKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                          gpointer self);
    static void onRightPressed(GtkGestureClick*, int, double x, double y, gpointer self);
    void installMenu();
    void writeIndex(std::u16string text, size_t fi, std::shared_ptr<Data> d,
                    const GitUi::ActionPlan& plan);
    void writeDiscard(std::u16string text, size_t fi, std::shared_ptr<Data> d,
                      const GitUi::ActionPlan& plan);
    // `done`: what to say once staged ("" for "Staged x.").
    void stageFile(size_t fi, const std::string& done = std::string());
    void keepConflict(MergeConflicts::Keep keep, size_t fi, long block);
    void markResolved(size_t fi);
    void sideBySide(size_t fi);
    void openFile(size_t fi);
    void runApply(std::shared_ptr<Data> d, const std::string& mainName);
    void say(const std::string& text, bool ok);
    void finish(const std::string& text, bool ok);
    // From the git thread back to the main loop, if the page still lives.
    static void onMain(std::shared_ptr<bool> alive, ChangeReviewPage* page,
                       std::function<void(ChangeReviewPage*)> fn);
    size_t rowAtOffset(int offset) const;
    long topRow() const;
    void applyReveal();
    static gboolean onSettle(GtkWidget*, GdkFrameClock*, gpointer self);
    // An action on `row`: where the eye goes once its result is read.
    void rememberActedRow(size_t row, ChangeReview::Op op);
    // The action's work goes to the git thread now: reads started from now
    // on show its result.
    void actionPosted() { actedFrom_ = readsStarted_ + 1; }
    GtkWidget* pictureBox(size_t fi);
    static void onPressed(GtkGestureClick*, int n, double x, double y, gpointer self);

    long currentBlockRow_ = -1;
    struct MenuItem {
        std::string title;
        std::function<void()> run;
    };
    std::vector<MenuItem> menu_;
    GMenuModel* baseExtra_ = nullptr;
    bool baseKept_ = false;

    GitPanel* panel_;
    std::string root_s_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::shared_ptr<bool> panelAlive_;
    std::shared_ptr<Data> data_;
    ChangeReview::Opened opened_;
    std::string worktree_, label_, branch_, mainRef_;   // a worktree's page
    std::string commit_, subject_;                       // a commit's page
    bool applying_ = false, applied_ = false;
    size_t appliedPrint_ = 0;
    std::string appliedMain_;
    bool loading_ = false, again_ = false;
    int reads_ = 0;
    std::string message_;
    bool messageOk_ = true, messageSettled_ = false;
    size_t messagePrint_ = 0;
    std::vector<Button> buttons_;
    std::vector<int> rowChar_;   // the buffer offset each row starts at
    std::vector<std::string> tints_;
    // revealFile's file (a File::key), or the top.
    std::string revealKey_;
    bool revealTop_ = false;
    // The place an action keeps (ChangeReview::ActedPlace), the pressed
    // row's height on screen, and the first read that shows its result.
    ChangeReview::ActedPlace acted_;
    double actedY_ = 0;
    unsigned actedFrom_ = 0, readsStarted_ = 0;
    bool actedPending_ = false;
    double rowHeight_ = 17;      // a plain row's height, for the layout's picture rows
    struct Picture {
        std::string label;
        GtkWidget* widget;
        int width, height;
    };
    std::vector<Picture> pictures_;   // on show
    GtkTextMark* scrollMark_ = nullptr;
    long settleRow_ = -1;        // scrollRowTo's row while it settles
    double settleY_ = 0;
    int settleFrames_ = 0, settleHeld_ = 0;
    guint settleTick_ = 0;

    GtkWidget *root_ = nullptr, *title_ = nullptr, *summary_ = nullptr, *bodyLabel_ = nullptr, *messageLabel_ = nullptr,
              *undo_ = nullptr, *scroll_ = nullptr, *view_ = nullptr, *apply_ = nullptr,
              *source_ = nullptr, *noteRow_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
};
