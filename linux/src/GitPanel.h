// GitPanel.h — the Source Control panel on Linux, the GTK counterpart of the
// Mac's src/GitPanel.mm. Ctrl+Shift+G puts it in the sidebar in place of the
// file tree (main.cpp); it shows the branch, the staged and unstaged changes,
// a commit message box and the commit graph, and asks the window to show a
// file's diff or a commit in the editor's slot (GitDiffView, held by Editor).
//
// What it shows and the git commands it runs are worked out in GitModel.h,
// which is plain C++ and tested. Git runs off the main thread, one command at
// a time in order, on a single-thread pool: an add and the status after it
// cannot pass each other. Results come back on the main loop and are dropped
// when the folder changed meanwhile, or, for diffs and the graph, when a
// newer one was asked for.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "GitModel.h"
#include "Settings.h"

// A diff or a commit, read-only and colored, in the editor's slot.
class GitDiffView {
public:
    GitDiffView();
    GtkWidget* widget() const { return scroller_; }
    // `commit`: `git show` output (header, message, stat and diff); else a
    // plain diff.
    void show(const std::string& bytes, bool commit);
    GtkTextBuffer* buffer() const { return buffer_; }
    GtkWidget* textView() const { return view_; }

private:
    GtkWidget* scroller_ = nullptr;
    GtkWidget* view_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
};

class GitPanel {
public:
    explicit GitPanel(const std::string& root);
    // Called while the window's widgets still exist (main.cpp's
    // onWindowRemoved). Git already running finishes on its own; its answer
    // is dropped.
    ~GitPanel();

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

    // The window shows these in the editor's slot. `name` is the file's
    // name, `path` its absolute path; `title` is "<short hash> <subject>".
    std::function<void(const std::string& name, const std::string& path,
                       const std::string& diff)> onShowDiff;
    std::function<void(const std::string& title, const std::string& show)> onShowCommit;

    // Colors of the rows, from the settings (the sidebar's text and
    // background); the rest of the panel is colored by the stylesheet.
    void applySettings(const Settings& s);

    // Places the children; called by the panel's layout manager.
    void layout(int width, int height);

    // For tests and the debug log (G_MESSAGES_DEBUG=minicode-git).
    std::vector<std::string> rowDescriptions() const;
    std::vector<std::string> graphDescriptions() const;
    std::string errorLine() const;

private:
    void build();
    void post(std::function<void()> job);   // run on the git thread, in order

    void runRefresh();
    void applySnapshot(const GitUi::Snapshot& s);
    void applyGraph(std::shared_ptr<const GitUi::Graph> g);
    void runAction(std::vector<std::string> args,
                   std::function<void(int status, const std::string& out)> done);
    void setError(const std::string& text, bool info);
    void showGraph(bool show);
    void showMessageArea(bool show);
    void watch(const GitUi::Snapshot& s);   // monitors on the repository
    void unwatch();

    // Rows and keys.
    void reloadList();
    void reloadGraph();
    int  selectedRow() const;
    int  selectedGraphRow() const;
    void selectRow(int row, bool focus);
    void selectGraphRow(int row, bool focus);
    void toggleStage(int row);
    void showDiff(int row);
    void showCommit(int row);
    void showMore();
    void commit();
    void focusList();
    void focusGraph();
    void focusMessage();
    void focusListFromGraph(bool wrap);
    void focusMessageOrList();
    bool graphHasRows() const;
    bool panelHasFocus() const;
    int  graphRowCount() const;

    static gboolean onListKey(GtkEventControllerKey*, guint key, guint code,
                              GdkModifierType mods, gpointer self);
    static gboolean onGraphKey(GtkEventControllerKey*, guint key, guint code,
                               GdkModifierType mods, gpointer self);
    static gboolean onMessageKey(GtkEventControllerKey*, guint key, guint code,
                                 GdkModifierType mods, gpointer self);
    static void drawRow(GtkDrawingArea*, cairo_t*, int w, int h, gpointer self);
    static void drawGraphRow(GtkDrawingArea*, cairo_t*, int w, int h, gpointer self);

    std::string rootDir_;
    bool active_ = false;
    std::shared_ptr<bool> alive_;        // false once the panel is gone
    std::shared_ptr<GitPanel*> ref_;     // the rows' draw functions hold this
    bool focusPending_ = false;          // focus() came before the first answer
    GThreadPool* pool_ = nullptr;
    guint refreshTimer_ = 0;
    unsigned diffGeneration_ = 0;
    unsigned graphGeneration_ = 0;

    // What is shown.
    bool inRepository_ = false;
    bool initial_ = false;
    std::string topLevel_;
    std::vector<GitUi::Row> rows_;
    std::shared_ptr<const GitUi::Graph> graph_;
    int graphLimit_ = GitUi::kGraphBatch;
    bool allBranches_ = false;
    bool graphShown_ = false;
    bool errorIsInfo_ = false;
    bool errorFromStatus_ = false;
    std::string errorText_;
    std::string watchedTop_, watchedGitDir_;
    std::vector<GFileMonitor*> monitors_;

    // Widgets, in the order the layout places them.
    GtkWidget* root_ = nullptr;          // a box with a hand-written layout
    GtkWidget* branchLabel_ = nullptr;
    GtkWidget* messageFrame_ = nullptr;  // overlay: scroller + placeholder
    GtkWidget* message_ = nullptr;       // GtkTextView
    GtkWidget* placeholder_ = nullptr;
    GtkWidget* commitButton_ = nullptr;
    GtkWidget* errorLabel_ = nullptr;
    GtkWidget* notice_ = nullptr;
    GtkWidget* listScroll_ = nullptr;
    GtkWidget* list_ = nullptr;          // GtkListView
    GtkStringList* listModel_ = nullptr;
    GtkSingleSelection* listSel_ = nullptr;
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

// Where git is, found once on PATH. Empty when it is not installed.
const std::string& gitExecutable();
