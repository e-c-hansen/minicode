// Search.h — Find in Folder (Ctrl+Shift+F): the GTK front end for the shared
// FolderSearch core (../src/FolderSearch.h). A separate window, like the macOS
// SearchPanel (src/Search.mm): a scope row naming the folder, a search entry,
// a status line and a list of matches. Activating a match (a click, or
// Enter) hands it to the open callback, which opens the file at that line.
//
// Searching runs on a GTask worker thread. Every new search bumps a generation
// counter and raises the previous search's cancel flag, and a result that comes
// back from a superseded generation is dropped, so typing quickly never shows a
// stale list.
#pragma once

#include <gtk/gtk.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "FolderSearch.h"

class SearchPanel {
public:
    // Called on the main thread when the user activates a match.
    using OpenCb = void (*)(const FolderSearchMatch& match, void* user);

    // One panel per editor window, created on first use and hidden, not
    // destroyed, when its own window is closed.
    explicit SearchPanel(GtkWindow* parent);
    // The editor window is closing: the panel's window goes, and a search
    // still running is stopped and its result dropped.
    ~SearchPanel();
    SearchPanel(const SearchPanel&) = delete;
    SearchPanel& operator=(const SearchPanel&) = delete;

    void setOpenCallback(OpenCb cb, void* user) { openCb_ = cb; openUser_ = user; }

    // The open folder. Relative paths typed into the scope field resolve
    // against it, and when it changes (Open Folder) the scope resets to it.
    void setRoot(const std::string& root);

    // Search a folder, re-running the current query if the folder changed.
    void setScope(const std::string& dir);
    const std::string& scope() const { return scope_; }

    // Present the window with the query field focused.
    void show();

    // For the headless test hook and for anyone who needs to look inside.
    GtkWidget* window() const { return window_; }
    GtkWidget* queryEntry() const { return entry_; }
    GtkWidget* listView() const { return listView_; }
    const std::vector<FolderSearchMatch>& matches() const { return hits_; }
    bool searching() const { return searching_; }
    std::string statusText() const;

    // Run the query now, without waiting for the typing pause.
    void runSearch();

private:
    struct Job;

    void build();
    void setStatus(const std::string& s);
    void clearResults();
    void finish(Job* job);
    void scopeEntered();
    void chooseScope();
    void updateScopeField();

    static void onSearchChanged(GtkSearchEntry* e, gpointer self);
    static void onQueryActivate(GtkSearchEntry* e, gpointer self);
    static void onStopSearch(GtkSearchEntry* e, gpointer self);
    static gboolean onQueryKey(GtkEventControllerKey* c, guint keyval, guint code,
                               GdkModifierType mods, gpointer self);
    static void onRowPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onRowReleased(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static gboolean onListKey(GtkEventControllerKey* c, guint keyval, guint keycode,
                              GdkModifierType mods, gpointer self);
    guint rowAt(double x, double y) const;
    void openRow(guint pos);
    void focusFirstResult();
    static void onSetup(GtkSignalListItemFactory* f, GObject* item, gpointer self);
    static void onBind(GtkSignalListItemFactory* f, GObject* item, gpointer self);
    static void worker(GTask* task, gpointer source, gpointer data, GCancellable* c);
    static void onDone(GObject* source, GAsyncResult* res, gpointer self);

    GtkWindow*     parent_     = nullptr;
    GtkWidget*     window_     = nullptr;
    GtkWidget*     scopeEntry_ = nullptr;
    GtkWidget*     entry_      = nullptr;
    GtkWidget*     status_     = nullptr;
    GtkWidget*     listView_   = nullptr;
    GtkStringList* rows_       = nullptr;   // one display string per match
    guint          pressRow_   = GTK_INVALID_LIST_POSITION;   // where a click began

    std::vector<FolderSearchMatch> hits_;   // row i of the list is hits_[i]
    std::string root_;
    std::string scope_;
    std::string lastQuery_;                 // the query the current list is for
    unsigned    generation_ = 0;            // bumped by every search
    bool        searching_  = false;
    std::shared_ptr<std::atomic<bool>> cancel_;   // the running search's flag

    OpenCb openCb_   = nullptr;
    void*  openUser_ = nullptr;
    // A finished search checks this before touching the panel, which may
    // have been deleted while it ran.
    std::shared_ptr<int> life_ = std::make_shared<int>(0);
};
