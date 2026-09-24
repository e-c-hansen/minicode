// FileTree.cpp — see FileTree.h.
//
// Model shape:
//   GtkTreeListModel
//     root  = makeDirModel(rootDir)                        (top-level entries)
//     child = createChild(item) -> makeDirModel(item's GFile) for directories,
//             NULL for regular files (so files are leaves)
//   makeDirModel(dir) = GtkSortListModel( GtkFilterListModel( store, filter_ ) )
//   store   = GListStore of the folder's GFileInfo, kept current by a
//             GFileMonitor (see "one folder" below for why not GtkDirectoryList)
//   filter_ = GtkCustomFilter hiding dotfiles unless showHidden_.
//
// The items flowing through the model are GFileInfo, each carrying its GFile
// under the "standard::file" attribute, as GtkDirectoryList's did; we read it
// back with g_file_info_get_attribute_object().
#include "FileTree.h"
#include "Palette.h"

#include <cstring>
#include <string>
#include <unordered_set>

// Attributes we request for every listing.
static const char* kAttrs =
    "standard::name,standard::type,standard::content-type,standard::icon";

// ---------------------------------------------------------------- helpers

static GFile* fileOfInfo(GFileInfo* info) {
    if (!info) return nullptr;
    GObject* obj = g_file_info_get_attribute_object(info, "standard::file");
    return obj ? G_FILE(obj) : nullptr;
}

static bool infoIsDir(GFileInfo* info) {
    return info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
}

// Row order, matching the macOS build (EditorController.mm -loadChildren):
// directories first, then files, each group compared case-insensitively by
// name. GtkDirectoryList hands entries back in raw enumeration order, which is
// whatever order the filesystem returns, so without this the tree looks random.
static int compareInfos(gconstpointer a, gconstpointer b, gpointer) {
    GFileInfo* ia = G_FILE_INFO(const_cast<gpointer>(a));
    GFileInfo* ib = G_FILE_INFO(const_cast<gpointer>(b));
    bool da = infoIsDir(ia), db = infoIsDir(ib);
    if (da != db) return da ? -1 : 1;

    const char* na = g_file_info_get_name(ia);
    const char* nb = g_file_info_get_name(ib);
    char* fa = g_utf8_casefold(na ? na : "", -1);
    char* fb = g_utf8_casefold(nb ? nb : "", -1);
    int r = g_utf8_collate(fa, fb);
    g_free(fa);
    g_free(fb);
    return r;
}

// ---------------------------------------------------------------- construction

FileTree::FileTree(const std::string& rootDir, bool showHidden)
    : root_(rootDir), showHidden_(showHidden) {
    scroller_ = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scroller_, 220, -1);
    gtk_widget_add_css_class(scroller_, "minicode-sidebar");
    build();
}

FileTree::~FileTree() {
    if (revealIdle_) g_source_remove(revealIdle_);
    revealIdle_ = 0;
    dropPopovers();
    if (listView_) {
        g_signal_handlers_disconnect_by_data(listView_, this);
        if (GtkListItemFactory* f = gtk_list_view_get_factory(GTK_LIST_VIEW(listView_)))
            g_signal_handlers_disconnect_by_data(f, this);
        GListModel* ctrls = gtk_widget_observe_controllers(listView_);
        for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
            GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
            g_signal_handlers_disconnect_by_data(c, this);
            g_object_unref(c);
        }
        g_object_unref(ctrls);
    }
    if (treeModel_) g_signal_handlers_disconnect_by_data(treeModel_, this);
    // The models call back into this object (the dotfile filter, a folder's
    // children), so they go now, with it, rather than whenever GTK drops the
    // list view: that also cancels every folder's monitor and listing.
    if (listView_) gtk_list_view_set_model(GTK_LIST_VIEW(listView_), nullptr);
    treeModel_ = nullptr;
    if (filter_) g_object_unref(filter_);
    if (sorter_) g_object_unref(sorter_);
    if (contextModel_) g_object_unref(contextModel_);
}

// ---------------------------------------------------------------- one folder
//
// A folder's entries, kept current. This used to be a GtkDirectoryList with
// monitoring on, but on GTK 4.22 that never reports a change: a GFileMonitor
// on the same folder sees every create, delete and rename while the list sits
// unchanged, so files made in the terminal, or by New File, never appeared.
// So the listing is a GListStore that this code fills and edits itself:
// enumerated asynchronously (a huge folder must not stall the window), then
// patched entry by entry from its own GFileMonitor. Patching one entry at a
// time, rather than re-listing, is what keeps expanded folders expanded,
// because the tree model only forgets a row's children when its item goes.
namespace {
struct DirWatch {
    GFile* dir = nullptr;
    GListStore* store = nullptr;         // not owned; this struct hangs off it
    GFileMonitor* monitor = nullptr;
    GCancellable* cancel = nullptr;
    std::unordered_set<std::string> names;
};

// The info a row needs, tagged with its GFile the way GtkDirectoryList did it,
// so fileOfInfo reads both the same.
void storeInfo(DirWatch* w, GFile* file, GFileInfo* info) {
    const char* name = g_file_info_get_name(info);
    if (!name || !w->names.insert(name).second) return;   // already listed
    g_file_info_set_attribute_object(info, "standard::file", G_OBJECT(file));
    g_list_store_append(w->store, info);
}

void removeName(DirWatch* w, const char* name) {
    if (!name || !w->names.erase(name)) return;
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(w->store));
    for (guint i = 0; i < n; ++i) {
        GFileInfo* info = G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(w->store), i));
        const bool match = g_strcmp0(g_file_info_get_name(info), name) == 0;
        g_object_unref(info);
        if (match) { g_list_store_remove(w->store, i); return; }
    }
}

// Each async step holds a ref on the store, so the DirWatch hanging off it
// outlives the call; a cancelled step (the folder was collapsed away or the
// tree re-rooted) just drops out.
void addFile(DirWatch* w, GFile* file) {
    g_file_query_info_async(file, kAttrs, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT, w->cancel,
        [](GObject* src, GAsyncResult* res, gpointer storep) {
            GListStore* store = G_LIST_STORE(storep);
            GFileInfo* info = g_file_query_info_finish(G_FILE(src), res, nullptr);
            auto* dw = static_cast<DirWatch*>(g_object_get_data(G_OBJECT(store), "minicode-watch"));
            // Gone again already: the temporary file of an atomic save, say.
            if (info && dw && !g_cancellable_is_cancelled(dw->cancel))
                storeInfo(dw, G_FILE(src), info);
            if (info) g_object_unref(info);
            g_object_unref(store);
        }, g_object_ref(w->store));
}

void onDirChanged(GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent ev,
                  gpointer wp) {
    auto* w = static_cast<DirWatch*>(wp);
    char* name = g_file_get_basename(file);
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        addFile(w, file);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        removeName(w, name);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:   // both names are in this folder
        removeName(w, name);
        if (other) addFile(w, other);
        break;
    default:
        break;
    }
    g_free(name);
}

void nextFiles(GFileEnumerator* en, DirWatch* w);

void gotFiles(GObject* src, GAsyncResult* res, gpointer storep) {
    GListStore* store = G_LIST_STORE(storep);
    GFileEnumerator* en = G_FILE_ENUMERATOR(src);
    GList* files = g_file_enumerator_next_files_finish(en, res, nullptr);
    auto* w = static_cast<DirWatch*>(g_object_get_data(G_OBJECT(store), "minicode-watch"));
    if (w && files && !g_cancellable_is_cancelled(w->cancel)) {
        for (GList* l = files; l; l = l->next) {
            GFileInfo* info = G_FILE_INFO(l->data);
            GFile* child = g_file_enumerator_get_child(en, info);
            storeInfo(w, child, info);
            g_object_unref(child);
        }
        nextFiles(en, w);
    } else {
        g_file_enumerator_close_async(en, G_PRIORITY_DEFAULT, nullptr, nullptr, nullptr);
    }
    g_list_free_full(files, g_object_unref);
    g_object_unref(store);
}

void nextFiles(GFileEnumerator* en, DirWatch* w) {
    g_file_enumerator_next_files_async(en, 200, G_PRIORITY_DEFAULT, w->cancel,
                                       gotFiles, g_object_ref(w->store));
}
}  // namespace

// The folder's entries, then a filter (dotfiles) and a sort (folders first),
// both shared by every folder in the tree.
GListModel* FileTree::makeDirModel(GFile* dir) {
    GListStore* store = g_list_store_new(G_TYPE_FILE_INFO);
    auto* w = new DirWatch;
    w->dir = G_FILE(g_object_ref(dir));
    w->store = store;
    w->cancel = g_cancellable_new();
    // Watch before listing, so nothing created in between is missed; the name
    // set keeps an entry from being listed twice.
    w->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (w->monitor) g_signal_connect(w->monitor, "changed", G_CALLBACK(onDirChanged), w);
    g_object_set_data_full(G_OBJECT(store), "minicode-watch", w, [](gpointer p) {
        auto* dw = static_cast<DirWatch*>(p);
        g_cancellable_cancel(dw->cancel);
        if (dw->monitor) {
            g_signal_handlers_disconnect_by_data(dw->monitor, dw);
            g_file_monitor_cancel(dw->monitor);
            g_object_unref(dw->monitor);
        }
        g_object_unref(dw->cancel);
        g_object_unref(dw->dir);
        delete dw;
    });
    g_file_enumerate_children_async(dir, kAttrs, G_FILE_QUERY_INFO_NONE,
        G_PRIORITY_DEFAULT, w->cancel,
        [](GObject* src, GAsyncResult* res, gpointer storep) {
            GListStore* st = G_LIST_STORE(storep);
            GFileEnumerator* en = g_file_enumerate_children_finish(G_FILE(src), res, nullptr);
            auto* dw = static_cast<DirWatch*>(g_object_get_data(G_OBJECT(st), "minicode-watch"));
            if (en && dw && !g_cancellable_is_cancelled(dw->cancel)) nextFiles(en, dw);
            if (en) g_object_unref(en);   // the pending next_files call holds its own ref
            g_object_unref(st);
        }, g_object_ref(store));

    // gtk_filter_list_model_new is (transfer full) on BOTH arguments. It takes
    // the store outright, but filter_ is shared across every directory model,
    // so hand it a ref of its own and keep ours.
    if (filter_) g_object_ref(filter_);
    GtkFilterListModel* fm =
        gtk_filter_list_model_new(G_LIST_MODEL(store), filter_);

    // Same sharing rule for the sorter: transfer full, so hand over a ref.
    if (sorter_) g_object_ref(sorter_);
    GtkSortListModel* sm =
        gtk_sort_list_model_new(G_LIST_MODEL(fm), sorter_);
    return G_LIST_MODEL(sm);
}

void FileTree::build() {
    // Shared dotfile filter.
    filter_ = GTK_FILTER(gtk_custom_filter_new(
        [](gpointer item, gpointer self) -> gboolean {
            return static_cast<FileTree*>(self)->filterVisible(item, self);
        }, this, nullptr));

    // Shared row sorter (dirs first, then case-insensitive name order).
    sorter_ = GTK_SORTER(gtk_custom_sorter_new(compareInfos, nullptr, nullptr));

    GFile* rootFile = g_file_new_for_path(root_.c_str());
    GListModel* rootModel = makeDirModel(rootFile);
    g_object_unref(rootFile);

    treeModel_ = gtk_tree_list_model_new(
        rootModel,          // takes ownership
        FALSE,              // passthrough
        FALSE,              // autoexpand
        [](gpointer item, gpointer self) -> GListModel* {
            return static_cast<FileTree*>(self)->createChild(item, self);
        }, this, nullptr);

    g_signal_connect(treeModel_, "items-changed", G_CALLBACK(onItemsChanged), this);

    GtkSingleSelection* sel =
        gtk_single_selection_new(G_LIST_MODEL(treeModel_));
    // No row is selected until the user picks one, as in the macOS outline.
    // With GTK's default autoselect the first row (often a folder) counted as
    // selected from startup, and Find in Folder searched only that folder.
    // New File likewise goes into the root until a row is picked.
    gtk_single_selection_set_autoselect(sel, FALSE);
    gtk_single_selection_set_can_unselect(sel, TRUE);

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(onSetup), this);
    g_signal_connect(factory, "bind",  G_CALLBACK(onBind),  this);

    listView_ = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);
    gtk_widget_add_css_class(listView_, "minicode-tree");
    g_signal_connect(listView_, "activate", G_CALLBACK(onActivate), this);

    // Right-click: capture phase, so it sees the press before the rows' own
    // click handling does.
    GtkGesture* rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(rclick),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(rclick, "pressed", G_CALLBACK(onRightClick), this);
    gtk_widget_add_controller(listView_, GTK_EVENT_CONTROLLER(rclick));

    if (contextModel_) {
        contextMenu_ = gtk_popover_menu_new_from_model(contextModel_);
        gtk_popover_set_has_arrow(GTK_POPOVER(contextMenu_), FALSE);
        gtk_widget_set_halign(contextMenu_, GTK_ALIGN_START);
        gtk_widget_set_parent(contextMenu_, listView_);
    }
    // The window going away takes the list view with it; the popovers have
    // to come off first (setRoot does the same before replacing the view).
    g_signal_connect(listView_, "destroy", G_CALLBACK(+[](GtkWidget* w, gpointer selfp) {
        FileTree* self = static_cast<FileTree*>(selfp);
        if (self->listView_ == w) self->dropPopovers();
    }), this);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), listView_);
}

// Popovers are children of the list view, and a widget must not be finalized
// with children still attached, so they come off before the view is replaced.
void FileTree::dropPopovers() {
    if (contextMenu_) {
        gtk_widget_unparent(contextMenu_);
        contextMenu_ = nullptr;
    }
    if (namePopover_) {
        GtkWidget* pop = namePopover_;
        namePopover_ = nullptr;
        gtk_popover_popdown(GTK_POPOVER(pop));
        if (gtk_widget_get_parent(pop)) gtk_widget_unparent(pop);
    }
}

// ---------------------------------------------------------------- model funcs

GListModel* FileTree::createChild(gpointer item, gpointer selfp) {
    FileTree* self = static_cast<FileTree*>(selfp);
    GFileInfo* info = G_FILE_INFO(item);   // the row's underlying item
    if (!infoIsDir(info)) return nullptr;  // files are leaves
    GFile* dir = fileOfInfo(info);
    if (!dir) return nullptr;
    return self->makeDirModel(dir);        // lazy: only built when expanded
}

gboolean FileTree::filterVisible(gpointer item, gpointer selfp) {
    FileTree* self = static_cast<FileTree*>(selfp);
    if (self->showHidden_) return TRUE;
    GFileInfo* info = G_FILE_INFO(item);
    const char* name = info ? g_file_info_get_name(info) : nullptr;
    if (name && name[0] == '.') return FALSE;
    return TRUE;
}

// ---------------------------------------------------------------- row widgets

void FileTree::onSetup(GtkSignalListItemFactory* /*f*/, GObject* obj,
                       gpointer /*self*/) {
    GtkListItem* li = GTK_LIST_ITEM(obj);
    GtkWidget* expander = gtk_tree_expander_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* icon = gtk_image_new();
    GtkWidget* label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_add_css_class(label, "minicode-tree-label");
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(li, expander);
    // Lets a widget found under the pointer be traced back to its row.
    g_object_set_data(G_OBJECT(expander), "minicode-list-item", li);
}

void FileTree::onBind(GtkSignalListItemFactory* /*f*/, GObject* obj,
                      gpointer /*self*/) {
    GtkListItem* li = GTK_LIST_ITEM(obj);
    GtkTreeExpander* expander =
        GTK_TREE_EXPANDER(gtk_list_item_get_child(li));
    GtkWidget* box = gtk_tree_expander_get_child(expander);
    GtkWidget* icon = gtk_widget_get_first_child(box);
    GtkWidget* label = gtk_widget_get_next_sibling(icon);

    GtkTreeListRow* row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(li));
    if (!row) return;
    gtk_tree_expander_set_list_row(expander, row);

    // gtk_tree_list_row_get_item is (transfer full): we own this reference.
    GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(row));
    const char* name = info ? g_file_info_get_name(info) : "?";
    gtk_label_set_text(GTK_LABEL(label), name ? name : "?");

    // Folder vs file icon (named icons from the theme; tinted via CSS class).
    if (infoIsDir(info)) {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "folder-symbolic");
        gtk_widget_remove_css_class(icon, "minicode-file-icon");
        gtk_widget_add_css_class(icon, "minicode-dir-icon");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "text-x-generic-symbolic");
        gtk_widget_remove_css_class(icon, "minicode-dir-icon");
        gtk_widget_add_css_class(icon, "minicode-file-icon");
    }
    if (info) g_object_unref(info);
}

// ---------------------------------------------------------------- activation

void FileTree::onActivate(GtkListView* /*lv*/, guint pos, gpointer selfp) {
    FileTree* self = static_cast<FileTree*>(selfp);
    GtkSelectionModel* model =
        gtk_list_view_get_model(GTK_LIST_VIEW(self->listView_));
    GtkTreeListRow* row =
        GTK_TREE_LIST_ROW(g_list_model_get_item(G_LIST_MODEL(model), pos));
    if (!row) return;

    // Both gtk_tree_list_row_get_item and g_list_model_get_item are
    // (transfer full): we own both references.
    GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(row));
    if (infoIsDir(info)) {
        // Expand/collapse directories on activation.
        gboolean expanded = gtk_tree_list_row_get_expanded(row);
        gtk_tree_list_row_set_expanded(row, !expanded);
    } else if (self->openCb_) {
        GFile* file = fileOfInfo(info);
        if (file) {
            char* path = g_file_get_path(file);
            if (path) { self->openCb_(path, self->openUser_); g_free(path); }
        }
    }
    if (info) g_object_unref(info);
    g_object_unref(row);
}

// ---------------------------------------------------------------- public ops

void FileTree::setRoot(const std::string& rootDir) {
    root_ = rootDir;
    pendingReveal_.clear();
    dropPopovers();
    // Rebuild the model tree against the new root. filter_ and sorter_ are
    // recreated in build(); drop our old references first.
    if (filter_) { g_object_unref(filter_); filter_ = nullptr; }
    if (sorter_) { g_object_unref(sorter_); sorter_ = nullptr; }
    build();
}

void FileTree::setShowHidden(bool show) {
    if (show == showHidden_) return;
    showHidden_ = show;
    // One filter serves every folder's list, so this refilters all of them,
    // expanded ones included, without re-reading anything from disk.
    if (filter_)
        gtk_filter_changed(filter_, show ? GTK_FILTER_CHANGE_LESS_STRICT
                                         : GTK_FILTER_CHANGE_MORE_STRICT);
}

void FileTree::focus() {
    if (listView_) gtk_widget_grab_focus(listView_);
}

std::string FileTree::selectedDir() const {
    if (!listView_) return "";
    GtkSelectionModel* model = gtk_list_view_get_model(GTK_LIST_VIEW(listView_));
    if (!GTK_IS_SINGLE_SELECTION(model)) return "";
    // Both are (transfer none) here: the selection and the row own them.
    GtkTreeListRow* row = GTK_TREE_LIST_ROW(
        gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(model)));
    if (!row) return "";
    GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(row));   // transfer full
    std::string out;
    if (infoIsDir(info)) {
        if (GFile* file = fileOfInfo(info)) {
            char* path = g_file_get_path(file);
            if (path) { out = path; g_free(path); }
        }
    }
    if (info) g_object_unref(info);
    return out;
}

// ---------------------------------------------------------------- selection

static std::string pathOfRow(GtkTreeListRow* row) {
    std::string out;
    GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(row));   // transfer full
    if (GFile* f = fileOfInfo(info)) {
        char* p = g_file_get_path(f);
        if (p) { out = p; g_free(p); }
    }
    if (info) g_object_unref(info);
    return out;
}

std::string FileTree::selectedPath() const {
    GtkSelectionModel* model = gtk_list_view_get_model(GTK_LIST_VIEW(listView_));
    gpointer item = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(model));
    return item ? pathOfRow(GTK_TREE_LIST_ROW(item)) : std::string();
}

void FileTree::clearSelection() {
    pendingReveal_.clear();
    gtk_selection_model_unselect_all(gtk_list_view_get_model(GTK_LIST_VIEW(listView_)));
}

void FileTree::revealPath(const std::string& path) {
    pendingReveal_ = path;
    revealDeadline_ = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    tryReveal();
}

// One pass over the visible rows: select the target if it is there, and
// expand each folder on the way down to it. Expanding a folder whose listing
// is already loaded inserts its rows at once, so the same pass carries on
// into them; otherwise the rows arrive later and onItemsChanged tries again.
bool FileTree::tryReveal() {
    if (pendingReveal_.empty()) return true;
    if (g_get_monotonic_time() > revealDeadline_) { pendingReveal_.clear(); return false; }
    const std::string target = pendingReveal_;
    GListModel* rows = G_LIST_MODEL(treeModel_);
    for (guint i = 0; i < g_list_model_get_n_items(rows); ++i) {
        GtkTreeListRow* row = gtk_tree_list_model_get_row(treeModel_, i);
        if (!row) continue;
        const std::string p = pathOfRow(row);
        if (p == target) {
            pendingReveal_.clear();
            g_object_unref(row);
            gtk_list_view_scroll_to(GTK_LIST_VIEW(listView_), i,
                                    GTK_LIST_SCROLL_SELECT, nullptr);
            return true;
        }
        if (!p.empty() && target.compare(0, p.size() + 1, p + "/") == 0 &&
            gtk_tree_list_row_is_expandable(row) && !gtk_tree_list_row_get_expanded(row))
            gtk_tree_list_row_set_expanded(row, TRUE);
        g_object_unref(row);
    }
    return false;
}

// Rows come and go while folders load and the directory monitors report
// changes. A reveal still waiting is retried from an idle, not from inside
// the signal, since selecting while the model is mid-update is asking for
// trouble.
void FileTree::onItemsChanged(GListModel*, guint, guint, guint, gpointer selfp) {
    FileTree* self = static_cast<FileTree*>(selfp);
    if (self->pendingReveal_.empty() || self->revealIdle_) return;
    self->revealIdle_ = g_idle_add([](gpointer p) -> gboolean {
        FileTree* t = static_cast<FileTree*>(p);
        t->revealIdle_ = 0;
        t->tryReveal();
        return G_SOURCE_REMOVE;
    }, self);
}

// Where row `pos` is drawn, in the list view's coordinates, if it is on screen.
bool FileTree::rowBounds(guint pos, graphene_rect_t* out) const {
    for (GtkWidget* c = gtk_widget_get_first_child(listView_); c;
         c = gtk_widget_get_next_sibling(c)) {
        GtkWidget* inner = gtk_widget_get_first_child(c);
        if (!inner) continue;
        auto* li = static_cast<GtkListItem*>(
            g_object_get_data(G_OBJECT(inner), "minicode-list-item"));
        if (li && gtk_list_item_get_position(li) == pos &&
            gtk_widget_get_mapped(inner))
            return gtk_widget_compute_bounds(inner, listView_, out);
    }
    return false;
}

// ---------------------------------------------------------------- context menu

void FileTree::setContextMenu(GMenuModel* model) {
    if (contextModel_) g_object_unref(contextModel_);
    contextModel_ = model ? G_MENU_MODEL(g_object_ref(model)) : nullptr;
    if (contextMenu_) { gtk_widget_unparent(contextMenu_); contextMenu_ = nullptr; }
    if (contextModel_ && listView_) {
        contextMenu_ = gtk_popover_menu_new_from_model(contextModel_);
        gtk_popover_set_has_arrow(GTK_POPOVER(contextMenu_), FALSE);
        gtk_widget_set_halign(contextMenu_, GTK_ALIGN_START);
        gtk_widget_set_parent(contextMenu_, listView_);
    }
}

// Select the row under the pointer, or clear the selection over empty space
// (so New File there goes into the root), then open the menu at the pointer.
void FileTree::onRightClick(GtkGestureClick* g, int, double x, double y,
                            gpointer selfp) {
    FileTree* self = static_cast<FileTree*>(selfp);
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    self->pendingReveal_.clear();

    GtkListItem* li = nullptr;
    for (GtkWidget* w = gtk_widget_pick(self->listView_, x, y, GTK_PICK_DEFAULT);
         w && w != self->listView_; w = gtk_widget_get_parent(w)) {
        li = static_cast<GtkListItem*>(g_object_get_data(G_OBJECT(w), "minicode-list-item"));
        if (li) break;
    }
    GtkSelectionModel* model = gtk_list_view_get_model(GTK_LIST_VIEW(self->listView_));
    if (li && gtk_list_item_get_position(li) != GTK_INVALID_LIST_POSITION)
        gtk_selection_model_select_item(model, gtk_list_item_get_position(li), TRUE);
    else
        gtk_selection_model_unselect_all(model);

    if (!self->contextMenu_) return;
    GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(self->contextMenu_), &at);
    gtk_popover_popup(GTK_POPOVER(self->contextMenu_));
}

// ---------------------------------------------------------------- name entry

namespace {
struct NameRequest {
    FileTree::NameCb cb;
    GtkWidget* entry = nullptr;
};
}  // namespace

void FileTree::askName(const std::string& prompt, const std::string& initial,
                       NameCb cb) {
    if (namePopover_) {   // one at a time; a second request replaces the first
        GtkWidget* old = namePopover_;
        namePopover_ = nullptr;
        if (gtk_widget_get_visible(old)) gtk_popover_popdown(GTK_POPOVER(old));
        else gtk_widget_unparent(old);   // never shown, so no "closed" to clean up
    }

    GtkWidget* pop = gtk_popover_new();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* label = gtk_label_new(prompt.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 28);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), entry);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_widget_set_parent(pop, listView_);

    // Under the selected row when it is on screen, else at the top of the tree.
    graphene_rect_t b;
    GtkSelectionModel* model = gtk_list_view_get_model(GTK_LIST_VIEW(listView_));
    guint pos = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(model));
    GdkRectangle at = {8, 0, 1, 1};
    if (pos != GTK_INVALID_LIST_POSITION && rowBounds(pos, &b))
        at = {(int)b.origin.x + 24, (int)b.origin.y, 1, (int)b.size.height};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &at);

    auto* req = new NameRequest{std::move(cb), entry};
    g_object_set_data_full(G_OBJECT(pop), "minicode-name-request", req,
                           [](gpointer p) { delete static_cast<NameRequest*>(p); });

    g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry* e, gpointer popp) {
        GtkWidget* popover = GTK_WIDGET(popp);
        auto* r = static_cast<NameRequest*>(
            g_object_get_data(G_OBJECT(popover), "minicode-name-request"));
        if (!r || !r->cb) return;
        char* text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
        std::string name = text;
        g_free(text);
        FileTree::NameCb done = std::move(r->cb);   // at most once
        r->cb = nullptr;
        gtk_popover_popdown(GTK_POPOVER(popover));
        if (!name.empty()) done(name);
    }), pop);

    // A closed popover is taken off the list view from an idle, since it is
    // still emitting its own signal when "closed" arrives.
    g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer selfp) {
        FileTree* self = static_cast<FileTree*>(selfp);
        if (self->namePopover_ == GTK_WIDGET(p)) self->namePopover_ = nullptr;
        g_idle_add([](gpointer w) -> gboolean {
            GtkWidget* widget = GTK_WIDGET(w);
            if (gtk_widget_get_parent(widget)) gtk_widget_unparent(widget);
            g_object_unref(widget);
            return G_SOURCE_REMOVE;
        }, g_object_ref(p));
    }), this);

    namePopover_ = pop;
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_widget_grab_focus(entry);
    // Select the name without its extension, so typing replaces just that.
    const char* dot = strrchr(initial.c_str(), '.');
    int stem = (dot && dot != initial.c_str())
                   ? (int)g_utf8_pointer_to_offset(initial.c_str(), dot) : -1;
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, stem);
}
