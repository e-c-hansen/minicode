// FileTree.h — the left sidebar: a lazy, live-refreshing file browser rooted
// at a folder. Each folder is a list kept current by its own GFileMonitor (the
// Linux equivalent of the macOS FSEvents watcher), wrapped in a
// GtkTreeListModel for expand/collapse, shown in a GtkListView with
// GtkTreeExpander rows.
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class FileTree {
public:
    // Called when the user opens a regular file: a single click on its row,
    // as on the Mac, or Enter. `fromKeyboard` is true for Enter, after which
    // the Mac moves the keyboard to the editor.
    using OpenCb = void(*)(const std::string& path, bool fromKeyboard, void* user);
    // Receives the name typed into askName's popover, trimmed and non-empty.
    using NameCb = std::function<void(const std::string& name)>;
    // Called when something is dropped on the tree: `paths` go into the
    // folder `dir`. `move` is true for a row dragged within MiniCode (any
    // window's tree) and false for files dragged in from another program,
    // which are copied. The drop has already been checked: no folder into
    // itself or below itself, nothing into the folder it is already in.
    using DropCb = std::function<void(const std::vector<std::string>& paths,
                                      const std::string& dir, bool move)>;

    // The tree starts with dotfiles shown or hidden per showHidden.
    explicit FileTree(const std::string& rootDir, bool showHidden = false);
    // Called while the window's widgets still exist (main.cpp tears a window
    // down from GtkApplication's window-removed): takes every handler that
    // names this object off the models and the list view.
    ~FileTree();

    GtkWidget* widget() const { return scroller_; }

    void setOpenCallback(OpenCb cb, void* user) { openCb_ = cb; openUser_ = user; }

    // Rows can be dragged onto folders, as in the Mac's outline: onto a
    // folder row means into it, onto a file row into that file's folder, and
    // onto the empty space below the rows into the root. Without a callback
    // nothing is accepted.
    void setDropCallback(DropCb cb) { dropCb_ = std::move(cb); }

    // Re-root at a new directory (Open Folder).
    void setRoot(const std::string& rootDir);

    // Show or hide entries whose names start with a dot, the Mac's rule
    // (Shift+Cmd+. there, Ctrl+H or Ctrl+Shift+. here). The setting is the
    // application's: main.cpp sets it on every window's tree, as the Mac's is
    // one global flag. The expanded folders and the selection survive it; a
    // selected row that is hidden is no longer selected.
    void setShowHidden(bool show);
    bool showHidden() const { return showHidden_; }

    void focus();

    // The folder selected in the tree, or "" when nothing or a file is
    // selected. Find in Folder searches it, as on the Mac.
    std::string selectedDir() const;

    // The selected row's path, or "" when nothing is selected. A right-click
    // selects the row under the pointer (or clears the selection over empty
    // space), so the context menu's actions read the row that was clicked.
    std::string selectedPath() const;
    void clearSelection();

    // Select the row for `path`, expanding the folders above it. Folders load
    // asynchronously and a file just created appears only once the directory
    // monitor reports it, so this keeps retrying as rows arrive, for a couple
    // of seconds, rather than failing on the first pass.
    void revealPath(const std::string& path);

    // Called on every change any of the tree's folder monitors reports, a
    // file's contents included. The Source Control panel refreshes on it,
    // as the Mac's does on the tree's FSEvents.
    void setActivityCallback(std::function<void()> cb) { *activity_ = std::move(cb); }

    // The right-click menu. Its items name window actions ("win.rename"),
    // which resolve because the menu is parented inside the window.
    void setContextMenu(GMenuModel* model);

    // A small popover under the selected row (or the top of the tree) with an
    // entry holding `initial`. Enter calls `cb` with the trimmed text; Escape,
    // a click elsewhere or an empty entry cancels.
    void askName(const std::string& prompt, const std::string& initial, NameCb cb);

private:
    GListModel* makeDirModel(GFile* dir);   // one folder, filtered and sorted
    void build();
    void dropPopovers();                    // before the list view is replaced
    bool tryReveal();
    void tryExpand();
    bool rowBounds(guint pos, graphene_rect_t* out) const;
    static void onItemsChanged(GListModel* m, guint pos, guint removed, guint added,
                               gpointer self);
    static void onRightClick(GtkGestureClick* g, int n, double x, double y,
                             gpointer self);
    static void onPrimaryPressed(GtkGestureClick* g, int n, double x, double y,
                                 gpointer self);
    static void onPrimaryReleased(GtkGestureClick* g, int n, double x, double y,
                                  gpointer self);
    static gboolean onKey(GtkEventControllerKey* c, guint keyval, guint keycode,
                          GdkModifierType mods, gpointer self);
    // The row under a point in the list view's coordinates, or
    // GTK_INVALID_LIST_POSITION. `onArrow` says whether the point is on the
    // expander's arrow, which toggles the folder by itself.
    guint rowAt(double x, double y, bool* onArrow) const;
    // A click or Enter on a row: a folder opens or closes, a file opens.
    void actOnRow(guint pos, bool fromKeyboard);
    void selectRow(guint pos);

    // Drag and drop (see "drag and drop" in FileTree.cpp).
    static GdkContentProvider* onDragPrepare(GtkDragSource* s, double x, double y,
                                             gpointer self);
    static void onDragEnd(GtkDragSource* s, GdkDrag* drag, gboolean deleteData,
                          gpointer self);
    static GdkDragAction onDropMotion(GtkDropTarget* t, double x, double y, gpointer self);
    static void onDropLeave(GtkDropTarget* t, gpointer self);
    static gboolean onDrop(GtkDropTarget* t, const GValue* value, double x, double y,
                           gpointer self);
    // The folder a drop at (x, y) goes into, and the row that stands for it
    // (GTK_INVALID_LIST_POSITION for the root).
    std::string dropDirAt(double x, double y, guint* row) const;
    // Neither a folder into itself or below itself, nor anything into the
    // folder it is in already.
    static bool dropAllowed(const std::vector<std::string>& paths, const std::string& dir);
    // Checks the drag over the tree at dropX_, dropY_ and lights up where it
    // would land; the action to offer, or 0 to refuse. Run on every motion,
    // and again when the rows move under a pointer held still (scrolling, a
    // folder opening).
    GdkDragAction updateDrop();
    // Lights up row `row`, or the whole tree for the root, or nothing.
    void showDropTarget(bool on, guint row);
    // A closed folder held under the pointer opens; the list scrolls while
    // the pointer is near its top or bottom edge.
    void hoverFolder(guint row);
    void autoScroll(double y);
    void endDrop();             // the pointer left, or the drop happened
    GtkWidget* rowWidget(guint pos) const;   // the list's row widget, if on screen

    // GTK callbacks (static trampolines).
    static GListModel* createChild(gpointer item, gpointer self);
    static gboolean    filterVisible(gpointer item, gpointer self);
    static void        onSetup(GtkSignalListItemFactory* f, GObject* obj, gpointer self);
    static void        onBind(GtkSignalListItemFactory* f, GObject* obj, gpointer self);

    std::string  root_;
    GtkWidget*   scroller_ = nullptr;
    GtkWidget*   listView_ = nullptr;
    GtkFilter*   filter_   = nullptr;     // shared dotfile filter
    GtkSorter*   sorter_   = nullptr;     // shared dirs-first, name-order sorter
    GtkTreeListModel* treeModel_ = nullptr;

    OpenCb openCb_   = nullptr;
    void*  openUser_ = nullptr;
    bool   showHidden_ = false;

    GMenuModel* contextModel_ = nullptr;
    GtkWidget*  contextMenu_  = nullptr;   // GtkPopoverMenu, parented to listView_
    GtkWidget*  namePopover_  = nullptr;   // askName's popover while it is up

    guint       pressRow_ = GTK_INVALID_LIST_POSITION;   // where the click began

    DropCb      dropCb_;
    // The drag over the tree right now: what it carries (empty while files
    // from elsewhere are still being read), whether it moves, and where the
    // pointer is.
    std::vector<std::string> dropPaths_;
    bool        dropMove_ = false;
    double      dropX_ = 0, dropY_ = 0;
    GtkWidget*  dropRow_ = nullptr;        // the lit row widget (weak pointer)
    bool        dropRoot_ = false;         // the whole tree is lit
    GtkTreeListRow* hoverRow_ = nullptr;   // closed folder waiting to open (ref)
    guint       hoverTimer_ = 0;
    guint       scrollTimer_ = 0;
    double      scrollStep_ = 0;           // pixels per tick, signed

    std::string pendingReveal_;            // revealPath target still to be found
    gint64      revealDeadline_ = 0;       // monotonic µs; give up after this
    guint       revealIdle_ = 0;
    // Folders to open again after the dotfile filter changed (setShowHidden).
    std::unordered_set<std::string> pendingExpand_;
    // Shared with every folder's watch, which may outlive the tree briefly.
    std::shared_ptr<std::function<void()>> activity_ =
        std::make_shared<std::function<void()>>();
    gint64      expandDeadline_ = 0;
};
