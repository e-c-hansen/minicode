// FileTree.h — the left sidebar: a lazy, live-refreshing file browser rooted
// at a folder. Each folder is a list kept current from its own GFileMonitor (the
// Linux equivalent of the macOS FSEvents watcher), wrapped in a
// GtkTreeListModel for expand/collapse, shown in a GtkListView with
// GtkTreeExpander rows.
#pragma once

#include <gtk/gtk.h>

#include "ClickCount.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "EditorTabs.h"

class FileTree {
public:
    // Called when the user opens a regular file, and how (EditorTabs.h): a
    // single click on its row, as on the Mac (the preview tab), the second
    // click of a double-click on it or a Ctrl+click (a tab of its own), or
    // Enter (a tab of its own, after which the Mac moves the keyboard to the
    // editor).
    using OpenCb = void(*)(const std::string& path, EditorTabs::OpenFrom from, void* user);
    // Receives the name typed into askName's popover, trimmed and non-empty.
    using NameCb = std::function<void(const std::string& name)>;
    // Called when something is dropped on the tree: `paths` go into the
    // folder `dir`. `move` is true for a row dragged within mutible (any
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

    // The scrolled list of rows. container() is what the window shows: the
    // Focus on Folder breadcrumb row (hidden while the whole folder shows)
    // over widget().
    GtkWidget* widget() const { return scroller_; }
    GtkWidget* container() const { return box_; }

    void setOpenCallback(OpenCb cb, void* user) { openCb_ = cb; openUser_ = user; }

    // Rows can be dragged onto folders, as in the Mac's outline: onto a
    // folder row means into it, onto a file row into that file's folder, and
    // onto the empty space below the rows into the root. Without a callback
    // nothing is accepted.
    void setDropCallback(DropCb cb) { dropCb_ = std::move(cb); }

    // Re-root at a new directory (Open Folder). A focus belongs to its
    // folder, so the tree shows the whole of the new one.
    void setRoot(const std::string& rootDir);
    const std::string& root() const { return root_; }

    // Focus on Folder and Add to Focus (FileDetails.h's TreeFocus, the
    // Mac's EditorController+TreeFocus.mm). With one folder focused the
    // tree shows that folder inside the window's as its root, with a
    // breadcrumb row back up; with several, each is a root row of its own,
    // labelled with its path from root(), in the order added, under a row
    // of their labels, each with a × that takes it out. A focus is a
    // folder's path from root(), "mac/src"; none is the whole folder. Only
    // the tree's view narrows: root() and everything that covers it
    // (search, Quick Open, terminals, git) stay as they were.
    //
    // setFocusFolders shows `folders` (merged by the core's rule:
    // TreeFocus::merged), keeping the folders open in it and the selected
    // row where it still shows, and remembering the folders open outside
    // it, which open again when the tree widens; widening opens the old
    // root and selects its row, and a folder new among several roots shows
    // open. False, nothing changed, when a folder is not one the tree lists
    // (inTree). setFocus replaces the set with one folder ("" for none);
    // addFocus and removeFocus are TreeFocus::added and removed.
    bool setFocusFolders(const std::vector<std::string>& folders);
    bool setFocus(const std::string& focus);
    bool addFocus(const std::string& folder);
    bool removeFocus(const std::string& folder);
    const std::vector<std::string>& focusFolders() const { return foci_; }
    // The focused folders joined by ", " ("" for the whole folder), as the
    // Mac's treeFocus says them.
    std::string focusPath() const;
    // The folder the tree starts at: root(), the focused folder, or with
    // several the first (New File, Copy Path and drops go there by default,
    // the Mac's treeDefaultFolder).
    std::string top() const;
    // Whether `rel`, a path from root(), is a folder the tree lists: a
    // folder on disk, no dotfolder on the way while hidden files are off,
    // and not the Worktrees node's (.claude lists no "worktrees").
    bool inTree(const std::string& rel) const;
    // `path` from root() ("" for the root), false outside it.
    bool relative(const std::string& path, std::string* rel) const;
    // A saved set back (TreeFocus::restoredSet): those still in the tree,
    // else the whole folder. Calls no onFocusChanged.
    void restoreFocus(const std::vector<std::string>& saved);
    // Whether `path` shows as a row under the focus. With `widen`, a path
    // outside it widens one focused folder to the nearest folder above both,
    // or adds its folder to several (TreeFocus::revealed) first: reveals
    // widen, opens do not.
    bool showsPath(const std::string& path, bool widen);
    // The root row of the focused folder `rel` selected and scrolled to
    // (a label of the label row clicked).
    void selectRoot(const std::string& rel);
    // After every change of the focus but restoreFocus's (the window saves
    // its session), and a note for the status bar when a focused folder
    // went from the disk.
    std::function<void()> onFocusChanged;
    std::function<void(const std::string& note)> onNote;
    // The bar's + (Add Folder to Focus…, the window's picker); without it
    // the + is not shown.
    std::function<void()> onAddFolder;
    // For the checks: the bar, its parts' labels as shown (the breadcrumb's
    // parts, the ellipsis left out, or the label row's labels), a part's
    // label (null when left out), a label's ×, the + and the last ×.
    GtkWidget* focusBar() const { return bar_; }
    bool focusBarShowsFolders() const { return foci_.size() > 1; }
    std::vector<std::string> focusBarLabels() const;
    GtkWidget* focusBarPart(size_t index) const;
    GtkWidget* focusBarRemove(size_t index) const;
    GtkWidget* focusBarAdd() const { return barAdd_; }
    GtkWidget* focusBarClose() const { return barClose_; }
    // The breadcrumb's right click menu (Show Whole Folder in Tree), its
    // items naming window actions as the tree's own menu does.
    void setFocusBarMenu(GMenuModel* model);

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
    // of seconds, rather than failing on the first pass. Outside a focused
    // folder, `widen` widens the focus first (showsPath); without it the
    // tree is left as it is (the tree following the tab on show, a file
    // opened from Quick Open or a link). Found, the row is scrolled to the
    // middle unless it is wholly in view already (centerRow), so a row on
    // show never moves; the keyboard stays where it is.
    void revealPath(const std::string& path, bool widen = false);
    // The row for `path` selected when the tree lists it now, no folder
    // opened and nothing scrolled (the Mac's reselectCurrentFileInTree).
    void selectIfListed(const std::string& path);
    // The file a click or Enter in the tree is opening, while the open
    // callback runs ("" otherwise): its row is the one clicked, which the
    // tree following the tab selects without scrolling (the Mac's
    // treeOpensPath:), so a click on a row cut by the edge never jumps.
    const std::string& opening() const { return opening_; }
    // The tree's view for a window's record (TabSessions::Window): the
    // folders open, those remembered outside the focus too, and how far it
    // is scrolled.
    std::vector<std::string> openFolders() const;
    double scrolled() const;
    // A window's record back (the Mac's finishRestoringWindow): `open`
    // opened again as their listings arrive (those outside the focus
    // remembered for when it widens), `selected` selected, and the scroll
    // put back once the rows have stopped coming, as far as they go.
    void restoreView(const std::vector<std::string>& open, const std::string& selected,
                     double scroll);

    // Called on every change any of the tree's folder monitors reports, a
    // file's contents included, with the path that changed. The Source
    // Control panel refreshes on it, as the Mac's does on the tree's
    // FSEvents, and the git colors look again (TreeGitColors.h).
    using ActivityCb = std::function<void(const std::string& path)>;
    void setActivityCallback(ActivityCb cb) { *activity_ = std::move(cb); }

    // A row's git color (TreeGitColors.h): the stylesheet class its name
    // takes and its tooltip, for a path and whether it is a folder; null
    // for none. Asked as each row is bound, and by redecorate().
    using Decorator = std::function<const char*(const std::string& path, bool isDir,
                                                std::string* tooltip)>;
    void setDecorator(Decorator d) { decorator_ = std::move(d); }
    // The decorator's answers changed: the rows on show ask again, nothing
    // else of them changes.
    void redecorate();

    // The right-click menu. Its items name window actions ("win.rename"),
    // which resolve because the menu is parented inside the window.
    void setContextMenu(GMenuModel* model);
    // Called after a right click selected its row and before the menu pops
    // up, so the window can enable the items that depend on the row.
    std::function<void()> onContextMenu;
    // The Worktrees node (PARITY.md 3.5): the title and tooltip of the
    // worktree at `path` (a folder of .claude/worktrees), false for none;
    // a right click on one pops `model`. redecorate() makes the rows say it.
    using WorktreeTextCb = std::function<bool(const std::string& path, std::string* title,
                                              std::string* tip)>;
    void setWorktreeText(WorktreeTextCb cb) { worktreeTextCb_ = std::move(cb); }
    void setWorktreeMenu(GMenuModel* model) { worktreeModel_ = model; }
    bool isWorktreeRow(const std::string& path) const;

    // A small popover under the selected row (or the top of the tree) with an
    // entry holding `initial`. Enter calls `cb` with the trimmed text; Escape,
    // a click elsewhere or an empty entry cancels.
    void askName(const std::string& prompt, const std::string& initial, NameCb cb);

private:
    GListModel* makeDirModel(GFile* dir, bool root = false);   // one folder, sorted, then filtered
    void build();
    // The tree's model from top(), and the list view put on a fresh one
    // (Focus on Folder): the rows and their folders' watches start over.
    GtkTreeListModel* makeTreeModel();
    void replaceModel();
    // The paths of the folders open now.
    std::unordered_set<std::string> expandedNow() const;
    // Several focused folders: a row each, in the set's order (onBind
    // labels them with their paths).
    GListModel* makeRootsModel();
    // The focus set to `set` (merged, each in the tree; empty for the whole
    // folder): the model replaced, the folders open again, the bar redrawn.
    // `lost`: focused folders went, so nothing widens or is selected.
    void applyFocus(const std::vector<std::string>& set, bool lost);
    // Focused folders gone from the disk, renamed, or hidden now: each
    // drops out, the others stay; none left is the whole folder, with a
    // note.
    void dropLostFocus();
    // The bar: made once, its parts made again on every focus.
    void buildFocusBar();
    void updateFocusBar();
    // The label row's labels: full paths, or the core's shortLabels when
    // those do not fit (barShort_), switched from an idle after a layout.
    void setBarShort(bool shortLabels);
    static void measureParts(GtkWidget* w, GtkOrientation o, int forSize, int* min, int* nat,
                             int* minBase, int* natBase);
    static void allocateParts(GtkWidget* w, int width, int height, int baseline);
    static void onBarPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onBarRightClick(GtkGestureClick* g, int n, double x, double y, gpointer self);
    // Watches each folder from root() down to the focused folder's parent,
    // so the focused folder deleted or renamed, or one above it, is seen
    // (the tree's own watches start at the focused folder).
    void watchFocusPath();
    void unwatchFocusPath();
    static void onFocusPathChanged(GFileMonitor* m, GFile* file, GFile* other,
                                   GFileMonitorEvent ev, gpointer self);
    void dropPopovers();                    // before the list view is replaced
    // revealPath, with the row centered (`center`) or only selected, as
    // the tree's own reveals keep it (a focus changed, hidden files shown,
    // a root's label clicked: the Mac's scrollRowToVisible there).
    void reveal(const std::string& path, bool widen, bool center);
    bool tryReveal();
    // Scrolls `row` to the middle unless it is wholly in view
    // (TreeReveal::scrollTop), measured once the list is laid out with it
    // (afterLayout, which also puts a waiting restoreView's scroll back).
    void centerRow(GtkTreeListRow* row);
    // The frame clock's after-paint, asked for once (scheduleFrame): the
    // rows' places are those of the model as it is then.
    void scheduleFrame();
    void stopFrames();
    static void onAfterPaint(GdkFrameClock* clock, gpointer self);
    void afterLayout();
    // A row the list shows now, and its position: `pos` itself when it
    // shows, else the first.
    GtkWidget* shownRow(guint pos, guint* shownPos) const;
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
    // The row drawn under a point, as its row object (its list item's), or
    // null: what the user sees there, even when a listing has just moved the
    // rows and the list is not laid out again yet.
    GtkTreeListRow* rowObjectAt(double x, double y, bool* onArrow) const;
    GtkListItem* listItemAt(double x, double y, bool* onArrow) const;
    // A click or Enter on a row: a folder opens or closes, a file opens.
    void actOnRow(GtkTreeListRow* row, EditorTabs::OpenFrom from);
    void selectRow(guint pos);
    // Keeps `row` (a reference of its own) in one of the click slots below.
    static void holdRow(GtkTreeListRow** slot, GtkTreeListRow* row);

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
    GtkWidget*   box_      = nullptr;     // the bar over the scroller
    GtkWidget*   scroller_ = nullptr;
    GtkWidget*   listView_ = nullptr;
    GtkFilter*   filter_   = nullptr;     // shared dotfile filter
    GtkTreeListModel* treeModel_ = nullptr;

    OpenCb openCb_   = nullptr;
    void*  openUser_ = nullptr;
    bool   showHidden_ = false;

    GMenuModel* contextModel_ = nullptr;
    GtkWidget*  contextMenu_  = nullptr;   // GtkPopoverMenu, parented to listView_
    GtkWidget*  namePopover_  = nullptr;   // askName's popover while it is up

    // The row a click began on and where, and the last click's row, held as
    // row objects: a folder's listing can move the rows between a press and
    // its release, or between the two clicks of a double-click, and a
    // position would then name another row (R1's follow-up). A row that
    // stays keeps its object.
    GtkTreeListRow* pressRow_ = nullptr;
    double          pressX_ = 0, pressY_ = 0;
    GtkTreeListRow* lastReleaseRow_ = nullptr;
    guint32         pressTime_ = 0;   // the last press's event time
    ClickCount      clicks_;

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
    bool        revealCenters_ = true;     // and whether its row is centered
    gint64      revealDeadline_ = 0;       // monotonic µs; give up after this
    guint       revealIdle_ = 0;
    std::string opening_;                  // opening(): the row a click opens
    // The row to center after the next layout (a reference), till when.
    GtkTreeListRow* centerRow_ = nullptr;
    gint64      centerDeadline_ = 0;
    // restoreView's selection and scroll waiting for the rows (scroll < 0:
    // none), till when, and when the rows last changed.
    std::string restoreSelect_;
    double      restoreScroll_ = -1;
    gint64      restoreDeadline_ = 0;
    gint64      rowsChanged_ = 0;
    // The frame clock asked for an after-paint (a reference), its handler,
    // and the list view's map, waited for while it has no clock.
    GdkFrameClock* frameClock_ = nullptr;
    gulong      afterPaintId_ = 0;
    gulong      mapId_ = 0;
    // Folders to open again after the dotfile filter changed (setShowHidden).
    std::unordered_set<std::string> pendingExpand_;
    // Shared with every folder's watch, which may outlive the tree briefly.
    std::shared_ptr<ActivityCb> activity_ = std::make_shared<ActivityCb>();
    Decorator decorator_;
    void decorate(GtkWidget* label) const;
    bool worktreeText(const std::string& path, std::string* text, std::string* tip) const;
    WorktreeTextCb worktreeTextCb_;
    GMenuModel* worktreeModel_ = nullptr;
    GtkWidget*  worktreeMenu_ = nullptr;
    gint64      expandDeadline_ = 0;

    // Focus on Folder and Add to Focus.
    std::vector<std::string> foci_;        // none for the whole folder
    // The folders open outside the focused folder, for when it widens.
    std::unordered_set<std::string> focusExpanded_;
    GtkWidget*  bar_ = nullptr;            // the breadcrumb row
    GtkWidget*  barParts_ = nullptr;       // its parts, laid out by allocateParts
    GtkWidget*  barClose_ = nullptr;       // the ×
    GtkWidget*  barEllipsis_ = nullptr;
    GtkWidget*  barMenu_ = nullptr;        // its right click menu
    GMenuModel* barMenuModel_ = nullptr;
    std::vector<GtkWidget*> barLabels_;    // by part
    std::vector<GtkWidget*> barSeps_;      // one more than enough
    std::vector<std::string> barFoci_;     // the focus each part gives
    GtkWidget*  barAdd_ = nullptr;         // the + (onAddFolder)
    // The label row (several folders): one chip a folder, each its label
    // and ×; the full and short labels by folder.
    std::vector<GtkWidget*> barChips_;
    std::vector<GtkWidget*> barChipLabels_;
    std::vector<GtkWidget*> barChipRemoves_;
    std::vector<std::string> barFull_, barShortText_;
    bool        barShort_ = false;
    guint       barShortIdle_ = 0;
    guint       barRemoveIdle_ = 0;        // a label's × acts from an idle
    std::string barRemove_;
    std::vector<GFileMonitor*> focusWatch_;
    guint       focusCheck_ = 0;           // the check after an event, coalesced
};
