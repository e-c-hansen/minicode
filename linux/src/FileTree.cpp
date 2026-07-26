// FileTree.cpp — see FileTree.h.
//
// Model shape:
//   GtkTreeListModel
//     root  = makeDirModel(rootDir)                        (top-level entries)
//     child = createChild(item) -> makeDirModel(item's GFile) for directories,
//             NULL for regular files (so files are leaves)
//   makeDirModel(dir) = GtkFilterListModel( GtkDirectoryList(dir), filter_ )
//   filter_ = GtkCustomFilter hiding dotfiles unless showHidden_.
//
// GtkDirectoryList monitors its directory with a GFileMonitor, so creations,
// deletions and renames on disk update the visible tree automatically.
//
// The items flowing through the model are GFileInfo. GtkDirectoryList records
// each entry's GFile on the info under the "standard::file" attribute; we read
// it back with g_file_info_get_attribute_object(). (Verify this attribute name
// on the target GTK — it is the documented idiom but a likely API-drift point.)
#include "FileTree.h"
#include "Palette.h"

#include <string>

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

FileTree::FileTree(const std::string& rootDir) : root_(rootDir) {
    scroller_ = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scroller_, 220, -1);
    gtk_widget_add_css_class(scroller_, "minicode-sidebar");
    build();
}

FileTree::~FileTree() = default;

// A GtkFilterListModel(GtkDirectoryList) for one directory, sharing filter_.
GListModel* FileTree::makeDirModel(GFile* dir) {
    GtkDirectoryList* dl = gtk_directory_list_new(kAttrs, dir);
    gtk_directory_list_set_monitored(dl, TRUE);  // live refresh via GFileMonitor
    // gtk_filter_list_model_new is (transfer full) on BOTH arguments. It takes
    // the directory list outright, but filter_ is shared across every directory
    // model, so hand it a ref of its own and keep ours.
    if (filter_) g_object_ref(filter_);
    GtkFilterListModel* fm =
        gtk_filter_list_model_new(G_LIST_MODEL(dl), filter_);

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

    GtkSingleSelection* sel =
        gtk_single_selection_new(G_LIST_MODEL(treeModel_));

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(onSetup), this);
    g_signal_connect(factory, "bind",  G_CALLBACK(onBind),  this);

    listView_ = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);
    gtk_widget_add_css_class(listView_, "minicode-tree");
    g_signal_connect(listView_, "activate", G_CALLBACK(onActivate), this);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), listView_);
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
    // Rebuild the model tree against the new root. filter_ and sorter_ are
    // recreated in build(); drop our old references first.
    if (filter_) { g_object_unref(filter_); filter_ = nullptr; }
    if (sorter_) { g_object_unref(sorter_); sorter_ = nullptr; }
    build();
}

void FileTree::toggleHidden() {
    showHidden_ = !showHidden_;
    if (filter_) gtk_filter_changed(filter_, GTK_FILTER_CHANGE_DIFFERENT);
}

void FileTree::focus() {
    if (listView_) gtk_widget_grab_focus(listView_);
}
