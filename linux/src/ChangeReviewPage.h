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
// Left to the Mac for now: the right click on a selection's lines (and the
// Keep choices in it), F7 and Shift+F7, a commit's page.
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
    ~ChangeReviewPage() override;
    GtkWidget* widget() override { return root_; }
    GtkWidget* focusWidget() override { return view_; }
    std::string key() const override {
        return worktree_.empty() ? "review:" + root_s_ : "worktree:" + worktree_;
    }
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
    GtkTextView* textView() const { return GTK_TEXT_VIEW(view_); }
    std::string rowText(size_t row) const;
    // The tint (paragraph background) a row is drawn with, "" for none.
    std::string rowTint(size_t row) const;
    // The block a row is in, or -1.
    int rowBlock(size_t row) const;
    size_t rowCount() const;

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
    static void onPressed(GtkGestureClick*, int n, double x, double y, gpointer self);

    GitPanel* panel_;
    std::string root_s_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::shared_ptr<bool> panelAlive_;
    std::shared_ptr<Data> data_;
    ChangeReview::Opened opened_;
    std::string worktree_, label_, branch_, mainRef_;   // a worktree's page
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

    GtkWidget *root_ = nullptr, *title_ = nullptr, *summary_ = nullptr, *messageLabel_ = nullptr,
              *undo_ = nullptr, *scroll_ = nullptr, *view_ = nullptr, *apply_ = nullptr,
              *source_ = nullptr, *noteRow_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
};
