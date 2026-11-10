// GitPanel.h — the Source Control panel on Linux, the GTK counterpart of the
// Mac's src/GitPanel.mm. Ctrl+Shift+G puts it in the sidebar in place of the
// file tree (main.cpp); it shows the branch, the staged and unstaged changes,
// a commit message box and the commit graph, and asks the window to show a
// file's diff or a commit in the editor's slot (GitDiffView, held by Editor).
//
// It is made with its window, as the Mac's is (EditorController+Git.mm's
// ensureGitPanel), so autofetch and the commands work before it is first
// shown. The commands (PARITY.md 2.9) are the Mac's View > Source Control:
// Fetch, Pull, Push and Sync; Stage All and Unstage All; Discard Changes and
// Undo Discard, with the file as it was in the Trash (GitTrash.h); Switch
// Branch, the window's palette with a list of branches (PalettePanel::
// showPick); Commit and Amend, Undo Last Commit, Stash and Pop Stash. Each
// reads the status now on the git thread, so it acts on the repository as it
// is whether the panel shows or not, asks when its plan asks, and says what
// happened under the Commit button (and in the status bar while the panel is
// hidden). The change lists take several rows (Ctrl and Shift clicks, Shift
// with the arrows), which Space, Stage, Unstage and Discard act on together.
// A file's diff gets the hunk actions (GitDiffView.h). The menus are built
// in one place, GitMenus.cpp.
//
// What it shows and the git commands it runs are worked out in GitModel.h,
// which is plain C++ and tested. Git runs off the main thread (GitRun.h),
// one command at a time in order, on a single-thread pool: an add and the
// status after it cannot pass each other. Results come back on the main loop
// and are dropped when the folder changed meanwhile, or, for diffs and the
// graph, when a newer one was asked for. The code is in GitPanel.cpp (the
// panel, its lists and drawing), GitVerbs.cpp (the commands) and
// GitMenus.cpp (its right click menus and the window's actions).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "GitDiffView.h"
#include "GitModel.h"
#include "GitRun.h"
#include "PalettePanel.h"
#include "Settings.h"

class GitPanel {
public:
    explicit GitPanel(const std::string& root);
    // Called while the window's widgets still exist (main.cpp's
    // onWindowRemoved). Git already running finishes on its own; its answer
    // is dropped.
    ~GitPanel();
    GitPanel(const GitPanel&) = delete;
    GitPanel& operator=(const GitPanel&) = delete;

    GtkWidget* widget() const { return root_; }

    // Open Folder: forget everything and look again.
    void setRoot(const std::string& root);
    const std::string& root() const { return rootDir_; }

    // Shown in the sidebar or not. Refreshes do nothing while it is hidden,
    // and showing it refreshes.
    void setActive(bool active);
    bool active() const { return active_; }

    // Look at the repository again, soon. Bursts (file monitor events, the
    // window coming to the front, an action) are gathered into one status.
    void refresh();

    // The keyboard to the change list, or the graph when there are no
    // changes, or the message box (Ctrl+0, and opening the panel).
    void focus();

    // ---- what the window gives it
    // A file's diff and a commit, shown in the editor's slot. `name` is the
    // file's name, `path` its absolute path; `title` is "<hash> <subject>".
    std::function<void(const std::string& name, const std::string& path,
                       const std::string& diff)> onShowDiff;
    std::function<void(const std::string& title, const std::string& show)> onShowCommit;
    // What it said while hidden, for the status bar.
    std::function<void(const std::string& text)> onNote;
    // A row's Open File and Reveal in File Tree, the path in the window's
    // folder's spelling.
    std::function<void(const std::string& path)> onOpenFile;
    std::function<void(const std::string& path)> onRevealFile;
    // The branch picker, in the window's palette.
    std::function<void(const PalettePanel::Pick& pick)> showPick;
    // Whether its window has the keyboard: autofetch waits for it, as VS
    // Code's does.
    std::function<bool()> isActive;
    // The file on show, "" for none: Discard Changes from the menu bar or the
    // palette acts on it.
    std::function<std::string()> fileOnShow;
    // The questions' window.
    GtkWindow* hostWindow = nullptr;

    // The window's commands (View > Source Control and the palette), added
    // to its action map as Keys.cpp names them ("gitfetch", ...).
    void installActions(GActionMap* window);
    // Their enabled state: Undo Discard with a discard to undo, Discard
    // Changes with a file on show or a file row chosen in the list while it
    // has the keyboard. The window calls it as the keyboard and tabs move.
    void updateActions();
    // The editor's diff view (made on the first diff): the panel's last
    // diff gets its hunk actions.
    void attachDiffView(GitDiffView* view);

    // Colors of the rows, from the settings (the sidebar's text and
    // background), and git.autofetch with its period.
    void applySettings(const Settings& s);

    // Places the children; called by the panel's layout manager.
    void layout(int width, int height);

    // ---- the commands (GitVerbs.cpp)
    void fetch();
    void pull();
    void push();
    void sync();
    void runSync(GitUi::SyncOp op, bool quietly = false);
    // git.autofetch: the panel's 30-second timer calls it with the monotonic
    // clock in seconds, and the checks with a time of their own. True when a
    // fetch started.
    bool autoFetchTickAt(double now);
    void stageAll();
    void unstageAll();
    // Discard Changes: the rows chosen with `row`, after one question.
    void discardRow(int row);
    // The file at `path` (absolute), if it has changes in the Changes list.
    void discardPath(const std::string& path);
    void undoDiscard();
    void chooseBranch();
    void commitAmend();
    void undoLastCommit();
    void stash();
    void popStash();
    void commit();
    // Space: the row, or the rows chosen with it, staged or unstaged.
    void toggleStage(int row);
    // A row's Stage Changes or Unstage Changes, for the rows chosen with it.
    void stageRows(int row, bool stage);
    // A hunk action from the diff view (GitDiffView::onHunk).
    void runHunkOp(GitUi::HunkOp op, size_t first, size_t last, bool lines);

    // ---- for the checks and the debug log (G_MESSAGES_DEBUG=minicode-git)
    std::vector<std::string> rowDescriptions() const;
    std::vector<std::string> graphDescriptions() const;
    std::string errorLine() const;
    bool inRepository() const { return inRepository_; }
    const std::string& branchText() const { return branchText_; }
    const std::string& graphSummary() const { return graphSummary_; }
    std::string syncButtonTitle() const;   // "" while hidden
    // "fetch ok", "push failed", "sync refused": the last fetch, pull, push
    // or sync, autofetch's too.
    const std::string& lastSync() const { return lastSync_; }
    bool syncing() const { return syncing_ || autoFetching_; }
    int autoFetches() const { return autoFetches_; }   // background fetches that ran
    std::string undoDiscardTitle() const;   // "" with nothing to undo
    int actionsDone() const { return actionsDone_; }  // commands that finished or refused
    std::string message() const;                      // the message box's text
    void setMessage(const std::string& text);
    std::vector<int> chosenRows() const;   // the change lists' selected rows
    int selectedRow() const;               // the row the keyboard is on
    GtkWidget* list() const { return list_; }
    GtkWidget* messageView() const { return message_; }
    GtkWidget* branchLabel() const { return branchLabel_; }
    GtkWidget* syncButton() const { return syncButton_; }
    GtkWidget* commitButton() const { return commitButton_; }
    // A row's widget while it is on screen, for a click; null otherwise.
    GtkWidget* rowWidget(int row) const;
    GtkWidget* menu() const { return menu_; }   // a right click menu, while it is up

private:
    // A Discard that Undo Discard can put back: where the file is, and where
    // the Trash put the copy ("" for a deleted file brought back).
    struct Discard {
        std::string path, rel, trashed;
        bool kept = false;   // a copy went to the Trash
    };
    // The diff the panel showed last, for its hunk actions.
    struct ShownDiff {
        bool valid = false;
        GitUi::Row row;
        std::string top, bytes;
    };

    void build();
    void post(std::function<void()> job);   // run on the git thread, in order
    // From the git thread back to the main loop, if the panel still lives.
    static void deliver(std::shared_ptr<bool> alive, GitPanel* panel,
                        std::function<void(GitPanel*)> fn);

    void runRefresh();
    void applySnapshot(const GitUi::Snapshot& s);
    void applyGraph(std::shared_ptr<const GitUi::Graph> g);
    // One git that changes the index or makes a commit, in `dir` (the top
    // level), then a refresh; `done` gets the result on the main loop first.
    void runAction(std::vector<std::string> args, const std::string& dir,
                   std::function<void(const GitResult&)> done);
    void setError(const std::string& text, bool info);
    void showGraph(bool show);
    void showMessageArea(bool show);
    void watch(const GitUi::Snapshot& s);   // monitors on the repository
    void unwatch();

    // Rows and keys.
    void reloadList();
    void reloadGraph();
    int  selectedGraphRow() const;
    void selectRow(int row, bool focus);
    void selectGraphRow(int row, bool focus);
    void chooseRange(int from, int to);   // Shift with an arrow or a click
    const GitUi::Row* fileRow(int row) const;
    int listRowAt(double x, double y) const;   // in the list's coordinates; -1 on none
    // The rows an action on `row` is for: every row chosen when `row` is one
    // of several chosen, else that row alone.
    std::vector<GitUi::Row> rowsActingWith(int row) const;
    void showDiff(int row);
    void showDiffOf(const GitUi::Row& row, const std::string& top);
    void showCommit(int row);
    void showMore();
    void focusList();
    void focusGraph();
    void focusMessage();
    void focusListFromGraph(bool wrap);
    void focusMessageOrList();
    bool graphHasRows() const;
    bool panelHasFocus() const;
    bool listHasKeyboard() const;
    int  graphRowCount() const;
    std::string inRoot(const std::string& path) const;   // a top-level path, the root's way

    // The commands' pieces (GitVerbs.cpp).
    void say(const std::string& text, bool info);   // under the button; the status bar while hidden
    void refuse(const GitUi::Snapshot& snap, const std::string& refusal);
    void actionDone();
    void ask(const GitUi::ActionPlan& plan, std::function<void()> go);
    void runAllStaging(bool stage);
    void runVerb(std::function<GitUi::ActionPlan(const GitUi::Snapshot&, std::string* extra)> plan,
                 std::function<void(const GitResult&, const std::string& extra)> after);
    void discardRows(const std::vector<GitUi::Row>& rows, const std::string& top);
    void runDiscards(const std::vector<GitUi::DiscardPlan>& plans,
                     const std::vector<std::string>& rels, const std::string& top,
                     const std::string& doneText);
    void runHunkPlan(const GitUi::HunkPlan& plan, bool discard, const ShownDiff& d);
    void switchToBranch(const GitUi::BranchItem& b, const GitUi::Snapshot& snap);
    void createBranch(const std::string& typed, const GitUi::Snapshot& snap);
    void finishSync(GitUi::SyncOp op, const GitUi::SyncPlan& plan, const GitResult* r,
                    const std::string& top, const std::string& root, bool quietly);
    void updateSyncButton();
    void discardFromCommand();   // git.clean

    // The menus (GitMenus.cpp).
    void buildActions();
    void showRowMenu(int row, double x, double y);   // x, y in the panel's coordinates
    void showPanelMenu(double x, double y);
    void popupMenu(GMenu* menu, double x, double y);
    void dropMenu();
    void enable(const char* action, bool on);

    static gboolean onListKey(GtkEventControllerKey*, guint key, guint code,
                              GdkModifierType mods, gpointer self);
    static gboolean onGraphKey(GtkEventControllerKey*, guint key, guint code,
                               GdkModifierType mods, gpointer self);
    static gboolean onMessageKey(GtkEventControllerKey*, guint key, guint code,
                                 GdkModifierType mods, gpointer self);
    static void onMenuPressed(GtkGestureClick*, int n, double x, double y, gpointer self);
    static void onMenuAction(GSimpleAction*, GVariant*, gpointer self);
    static void onWindowAction(GSimpleAction*, GVariant*, gpointer self);
    static void onSelectionChanged(GtkSelectionModel*, guint pos, guint n, gpointer self);
    static void drawRow(GtkDrawingArea*, cairo_t*, int w, int h, gpointer self);
    static void drawGraphRow(GtkDrawingArea*, cairo_t*, int w, int h, gpointer self);

    std::string rootDir_;
    bool active_ = false;
    std::shared_ptr<bool> alive_;        // false once the panel is gone
    std::shared_ptr<GitPanel*> ref_;     // the rows' draw functions hold this
    bool focusPending_ = false;          // focus() came before the first answer
    GThreadPool* pool_ = nullptr;
    guint refreshTimer_ = 0;
    guint autoTimer_ = 0;
    unsigned diffGeneration_ = 0;
    unsigned graphGeneration_ = 0;

    // What is shown.
    bool inRepository_ = false;
    bool initial_ = false;
    std::string topLevel_;
    std::string branchText_, graphSummary_;
    std::vector<GitUi::Row> rows_;
    GitUi::Snapshot shown_;              // the last status applied
    bool haveShown_ = false;
    std::shared_ptr<const GitUi::Graph> graph_;
    int graphLimit_ = GitUi::kGraphBatch;
    bool allBranches_ = false;
    bool graphShown_ = false;
    bool errorIsInfo_ = false;
    bool errorFromStatus_ = false;
    std::string errorText_;
    std::string watchedTop_, watchedGitDir_;
    std::vector<GFileMonitor*> monitors_;
    int cursor_ = -1;                    // the row the keyboard is on
    int anchor_ = -1;                    // where Shift extends from
    bool fixingSelection_ = false;

    // The commands.
    bool syncing_ = false;               // a fetch, pull, push or sync asked for runs
    bool autoFetching_ = false;          // a background fetch runs (no button, no words)
    std::string autoKey_;                // the repository's top level, once known
    double autoNow_ = 0;                 // the tick that started the autofetch running
    int autoFetches_ = 0;
    bool autoFetchOn_ = false;
    int autoFetchPeriod_ = GitUi::kAutoFetchMinPeriod;
    std::string lastSync_;
    std::vector<Discard> discards_;      // the last one last, 20 at most
    ShownDiff shownDiff_;
    GitDiffView* diffView_ = nullptr;
    int actionsDone_ = 0;
    GActionMap* windowActions_ = nullptr;
    std::vector<GSimpleAction*> ownActions_;   // the window's, for taking them off

    // The menus.
    GSimpleActionGroup* menuActions_ = nullptr;   // "gitpanel."
    GtkWidget* menu_ = nullptr;
    int menuRow_ = -1;                   // the row a row menu is for

    // Widgets, in the order the layout places them.
    GtkWidget* root_ = nullptr;          // a box with a hand-written layout
    GtkWidget* branchLabel_ = nullptr;
    GtkWidget* messageFrame_ = nullptr;  // overlay: scroller + placeholder
    GtkWidget* message_ = nullptr;       // GtkTextView
    GtkWidget* placeholder_ = nullptr;
    GtkWidget* commitButton_ = nullptr;
    GtkWidget* syncButton_ = nullptr;    // "Sync Changes ↓ 1 ↑ 2", "Publish Branch"
    GtkWidget* errorLabel_ = nullptr;
    GtkWidget* notice_ = nullptr;
    GtkWidget* listScroll_ = nullptr;
    GtkWidget* list_ = nullptr;          // GtkListView
    GtkStringList* listModel_ = nullptr;
    GtkMultiSelection* listSel_ = nullptr;
    GtkWidget* graphLine_ = nullptr;
    GtkWidget* graphHeading_ = nullptr;
    GtkWidget* allToggle_ = nullptr;
    GtkWidget* summaryLabel_ = nullptr;
    GtkWidget* graphScroll_ = nullptr;
    GtkWidget* graphList_ = nullptr;     // GtkListView
    GtkStringList* graphModel_ = nullptr;
    GtkSingleSelection* graphSel_ = nullptr;
    int pressRow_ = -1;                  // where a click began, in its list
    Rgba text_ = Rgba::hex(0xCCCCCC);
    Rgba panelBg_ = Rgba::hex(0x252526);
};
