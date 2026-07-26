// FileTree.h — the left sidebar: a lazy, live-refreshing file browser rooted
// at a folder. Built on GtkDirectoryList (which watches each directory with a
// GFileMonitor internally, giving us the Linux equivalent of the macOS FSEvents
// watcher for free) wrapped in a GtkTreeListModel for expand/collapse, shown in
// a GtkListView with GtkTreeExpander rows.
#pragma once

#include <gtk/gtk.h>
#include <string>

class FileTree {
public:
    // Called when the user activates (single-click / Enter) a regular file.
    using OpenCb = void(*)(const std::string& path, void* user);

    explicit FileTree(const std::string& rootDir);
    ~FileTree();

    GtkWidget* widget() const { return scroller_; }

    void setOpenCallback(OpenCb cb, void* user) { openCb_ = cb; openUser_ = user; }

    // Re-root at a new directory (Open Folder).
    void setRoot(const std::string& rootDir);

    // Show/hide dotfiles (mirrors Shift+Cmd+. on macOS).
    void toggleHidden();
    bool showHidden() const { return showHidden_; }

    void focus();

private:
    GListModel* makeDirModel(GFile* dir);   // filtered GtkDirectoryList for a dir
    void build();

    // GTK callbacks (static trampolines).
    static GListModel* createChild(gpointer item, gpointer self);
    static gboolean    filterVisible(gpointer item, gpointer self);
    static void        onActivate(GtkListView* lv, guint pos, gpointer self);
    static void        onSetup(GtkSignalListItemFactory* f, GObject* obj, gpointer self);
    static void        onBind(GtkSignalListItemFactory* f, GObject* obj, gpointer self);

    std::string  root_;
    GtkWidget*   scroller_ = nullptr;
    GtkWidget*   listView_ = nullptr;
    GtkFilter*   filter_   = nullptr;     // shared dotfile filter
    GtkTreeListModel* treeModel_ = nullptr;

    OpenCb openCb_   = nullptr;
    void*  openUser_ = nullptr;
    bool   showHidden_ = false;
};
