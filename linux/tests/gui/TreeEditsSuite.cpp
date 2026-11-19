// TreeEditsSuite.cpp — the tree keeps its place when rows go and come
// (`./build/mutible-guitest tree_edits`, HANDOFF item 110). The owner: "If I
// delete a file in the filetree panel, rather than simply removing it from
// my current view, the view resets and I am brought to the top".
//
// A folder of about 120 rows (three folders open, files at the top level),
// scrolled down, a file near the top open in a tab of its own:
//   - Move to Trash with the Delete key on a row clicked in view (it opens
//     in the preview tab, which the trash closes): the row goes, the tree's
//     scroll stays within a row, every folder stays open, the next row is
//     selected and has the keyboard, and the other tab's file is not
//     revealed. Delete again trashes that one the same way. The trash is the
//     scratch world's ($XDG_DATA_HOME/Trash, checked before anything is
//     trashed; a plain delete stands in when it would not be).
//   - A selected file deleted on disk by something else: the same.
//   - Files deleted and made above the view: the rows on show stay where
//     they are.
//   - Rename (F2) to a name that sorts in place: the scroll stays, the
//     renamed row is selected.
//   - The last row of the tree trashed: the row before it is selected.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/stat.h>

namespace {

std::string gRepo;
std::string F(const std::string& rel) { return gRepo + "/" + rel; }
bool gScratchTrash = false;   // Move to Trash goes to the scratch world's trash
double gTop = 0, gY = 0, gH = 0;

MCGuiAppWindow Win() {
    MCGuiAppWindow found;
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == gRepo) found = w;
    return found;
}
FileTree* Tree() { return Win().tree; }
GtkWidget* ListView() { return gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(Tree()->widget())); }
GtkAdjustment* Adj() { return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(Tree()->widget())); }
double TreeTop() { return gtk_adjustment_get_value(Adj()); }
GListModel* Rows() { return G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(ListView()))); }

std::string PathOf(GtkTreeListRow* r) {
    GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(r));
    GObject* file = info ? g_file_info_get_attribute_object(info, "standard::file") : nullptr;
    char* p = file ? g_file_get_path(G_FILE(file)) : nullptr;
    std::string out = p ? p : "";
    g_free(p);
    if (info) g_object_unref(info);
    return out;
}
int PositionOf(const std::string& path) {
    GListModel* m = Rows();
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        const bool hit = PathOf(r) == path;
        g_object_unref(r);
        if (hit) return (int)i;
    }
    return -1;
}
bool Expanded(const std::string& path) {
    GListModel* m = Rows();
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        const bool hit = PathOf(r) == path, open = hit && gtk_tree_list_row_get_expanded(r);
        g_object_unref(r);
        if (hit) return open;
    }
    return false;
}
void Expand(const std::string& path) {
    GListModel* m = Rows();
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        if (PathOf(r) == path) gtk_tree_list_row_set_expanded(r, TRUE);
        g_object_unref(r);
    }
}
bool AllOpen() { return Expanded(F("a_dir")) && Expanded(F("b_dir")) && Expanded(F("c_dir")); }

// The row's name label, on screen.
GtkWidget* RowLabel(const std::string& path, GtkWidget* root = nullptr) {
    if (!root) root = ListView();
    if (GTK_IS_LABEL(root) && gtk_widget_has_css_class(root, "minicode-tree-label")) {
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(root), "minicode-path"));
        return p && path == p && MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = RowLabel(path, c)) return hit;
    return nullptr;
}
// Where a row is drawn in the list view: its row widget's top and height.
bool RowRect(const std::string& path, double* y, double* h) {
    GtkWidget* label = RowLabel(path);
    GtkWidget* row = label;
    while (row && gtk_widget_get_parent(row) != ListView()) row = gtk_widget_get_parent(row);
    graphene_rect_t b;
    if (!row || !gtk_widget_compute_bounds(row, ListView(), &b)) return false;
    *y = b.origin.y;
    *h = b.size.height;
    return *h > 0;
}
bool TreeHasKeyboard() {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(Win().window));
    return f && (f == ListView() || gtk_widget_is_ancestor(f, ListView()));
}
std::string Rel(const std::string& path) {
    return path.compare(0, gRepo.size() + 1, gRepo + "/") == 0 ? path.substr(gRepo.size() + 1) : path;
}

// `rel` scrolled to about the middle of the tree.
void ScrollToMiddle(const std::string& rel) {
    MCGuiStep(rel + " scrolled to the middle", [rel] {
        const int pos = PositionOf(F(rel));
        GCHECK(pos >= 0);
        double y = 0, h = 0;
        // Any row on show gives the stride: the rows are of one height.
        for (GtkWidget* c = gtk_widget_get_first_child(ListView()); c && h == 0; c = gtk_widget_get_next_sibling(c)) {
            graphene_rect_t b;
            if (gtk_widget_get_mapped(c) && !GTK_IS_POPOVER(c) && gtk_widget_compute_bounds(c, ListView(), &b))
                h = b.size.height;
        }
        GCHECK(h > 0);
        gH = h;
        const double page = gtk_adjustment_get_page_size(Adj());
        gtk_adjustment_set_value(Adj(), std::max(0.0, pos * h - page / 2));
        (void)y;
    });
    MCGuiWait(rel + " on screen", 10, [rel] { return RowLabel(F(rel)) != nullptr; });
}

// A click on the row (it opens in the preview tab and the tree has the
// keyboard), then where things stand noted.
void ClickRow(const std::string& rel) {
    MCGuiStep("a click on " + rel, [rel] { MCGuiClick(RowLabel(F(rel))); });
    MCGuiWait(rel + " selected and on show", 10, [rel] {
        return Tree()->selectedPath() == F(rel) && Win().editor->currentPath() == F(rel) && TreeHasKeyboard();
    });
}

void Note(const std::string& rel) {
    MCGuiStep("where " + rel + " is noted", [rel] {
        gTop = TreeTop();
        GCHECK(RowRect(F(rel), &gY, &gH));
        MCGuiNote("%s at y %.0f, the tree scrolled to %.0f, rows %.0f high", rel.c_str(), gY, gTop, gH);
    });
}

// Move to Trash on the selected row with the Delete key, answered; with no
// scratch trash, a plain delete of the same file instead.
void TrashSelected(const std::string& rel) {
    MCGuiStep("Delete on " + rel, [rel] {
        if (gScratchTrash) {
            MCGuiKey("delete");
        } else {
            MCGuiNote("no scratch trash: %s deleted instead", rel.c_str());
            std::filesystem::remove(F(rel));
        }
    });
    if (gScratchTrash) {
        MCGuiClickWhenReady("the question's Move to Trash", [] {
            GtkWindow* top = MCGuiTopWindow();
            return top && top != Win().window ? MCGuiFindButton(GTK_WIDGET(top), "Move to Trash") : nullptr;
        });
    }
    MCGuiWait(rel + " gone from the disk and the tree", 10, [rel] {
        return !MCGuiFileExists(F(rel)) && PositionOf(F(rel)) < 0;
    });
    if (gScratchTrash)
        MCGuiStep(rel + " in the scratch trash", [rel] {
            const std::string name = rel.substr(rel.find_last_of('/') + 1);
            GCHECK(MCGuiFileExists(std::string(g_get_user_data_dir()) + "/Trash/files/" + name));
        });
}

// After a row went: the tree where it was, every folder open, `next`
// selected (and with the keyboard when `keys`), the rows on show in place.
void Kept(const std::string& next, bool keys) {
    // Anything moving would move in the next frame or two: a pause for a
    // negative check.
    MCGuiPause(0.4);
    MCGuiStep("the tree kept its place; " + next + " selected", [next, keys] {
        double y = 0, h = 0;
        const bool shown = RowRect(F(next), &y, &h);
        MCGuiNote("the tree at %.0f (was %.0f); %s at y %.0f (the row that went was at %.0f); selected %s",
                  TreeTop(), gTop, next.c_str(), shown ? y : -1, gY, Rel(Tree()->selectedPath()).c_str());
        GCHECK(std::fabs(TreeTop() - gTop) < gH);
        GCHECK(AllOpen());
        GCHECK_EQ_STR(Rel(Tree()->selectedPath()), next);
        GCHECK(shown && std::fabs(y - gY) < gH);
        if (keys) GCHECK(TreeHasKeyboard());
    });
}

}  // namespace

MC_GUI_SUITE(tree_edits) {
    // Move to Trash moves into $XDG_DATA_HOME/Trash only on the same file
    // system (else to a .Trash-uid at that system's top): the scratch
    // world's data folder, never the real one. Decided here, as the steps
    // are queued, since which steps run depends on it.
    {
        const std::string data = g_get_user_data_dir();
        struct stat a, b;
        gScratchTrash = data.compare(0, MCGuiScratch().size(), MCGuiScratch()) == 0 &&
                        stat(data.c_str(), &a) == 0 && stat(MCGuiScratch().c_str(), &b) == 0 &&
                        a.st_dev == b.st_dev;
    }
    MCGuiStep("a folder: a_dir (5), b_dir (80), c_dir (5) and 30 files at the top", [] {
        gRepo = MCGuiScratch() + "/editsrepo";
        auto make = [](const std::string& rel) {
            std::filesystem::create_directories(std::filesystem::path(F(rel)).parent_path());
            if (FILE* f = fopen(F(rel).c_str(), "w")) fclose(f);
        };
        char name[64];
        for (int i = 0; i < 5; i++) {
            snprintf(name, sizeof name, "a_dir/a%02d.txt", i);
            make(name);
            snprintf(name, sizeof name, "c_dir/c%02d.txt", i);
            make(name);
        }
        for (int i = 0; i < 80; i++) {
            snprintf(name, sizeof name, "b_dir/f%03d.txt", i);
            make(name);
        }
        for (int i = 0; i < 30; i++) {
            snprintf(name, sizeof name, "r%02d.txt", i);
            make(name);
        }
        const std::string data = g_get_user_data_dir();
        MCGuiNote("XDG_DATA_HOME %s: %s", data.c_str(), gScratchTrash ? "the scratch trash" : "no trash used");
        GCHECK(data.compare(0, MCGuiScratch().size(), MCGuiScratch()) == 0);
    });
    MCGuiStep("a window on it", [] {
        MCGuiOpenWindow(gRepo);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("drawn, its top rows listed", 10, [] {
        return Win().window && gtk_widget_get_mapped(GTK_WIDGET(Win().window)) && PositionOf(F("r29.txt")) >= 0;
    });
    MCGuiStep("the three folders open", [] {
        for (const char* d : {"a_dir", "b_dir", "c_dir"}) Expand(F(d));
    });
    MCGuiWait("their rows", 10, [] { return g_list_model_get_n_items(Rows()) == 3 + 5 + 80 + 5 + 30; });
    MCGuiStep("a_dir/a00.txt open in a tab of its own", [] { MCGuiOpenFileKept(F("a_dir/a00.txt")); });
    MCGuiWait("on show", 10, [] { return Win().editor->currentPath() == F("a_dir/a00.txt"); });

    // ---- Move to Trash, twice, by the key
    ScrollToMiddle("b_dir/f040.txt");
    ClickRow("b_dir/f040.txt");
    Note("b_dir/f040.txt");
    TrashSelected("b_dir/f040.txt");
    Kept("b_dir/f041.txt", gScratchTrash);
    MCGuiStep("its preview tab closed; a00.txt on show, not revealed", [] {
        GCHECK(Win().editor->documentFor(F("b_dir/f040.txt")) == nullptr);
        GCHECK_EQ_STR(Win().editor->currentPath(), F("a_dir/a00.txt"));
        MCGuiSnapshot(Tree()->widget(), "tree-edits-trashed");
    });
    Note("b_dir/f041.txt");
    TrashSelected("b_dir/f041.txt");
    Kept("b_dir/f042.txt", gScratchTrash);

    // ---- deleted on disk by something else
    Note("b_dir/f042.txt");
    MCGuiStep("b_dir/f042.txt, selected, deleted on disk", [] { std::filesystem::remove(F("b_dir/f042.txt")); });
    MCGuiWait("its row gone", 10, [] { return PositionOf(F("b_dir/f042.txt")) < 0; });
    Kept("b_dir/f043.txt", false);

    // ---- rows going and coming above the view
    Note("b_dir/f043.txt");
    MCGuiStep("b_dir/f000.txt to f002.txt deleted, then f000a.txt and f000b.txt made", [] {
        for (const char* n : {"f000.txt", "f001.txt", "f002.txt"}) std::filesystem::remove(F(std::string("b_dir/") + n));
    });
    MCGuiWait("their rows gone", 10, [] { return PositionOf(F("b_dir/f002.txt")) < 0; });
    MCGuiStep("two made", [] {
        for (const char* n : {"f000a.txt", "f000b.txt"})
            if (FILE* f = fopen(F(std::string("b_dir/") + n).c_str(), "w")) fclose(f);
    });
    MCGuiWait("their rows", 10, [] { return PositionOf(F("b_dir/f000b.txt")) >= 0; });
    MCGuiPause(0.4);   // a negative check: nothing on show moved
    MCGuiStep("b_dir/f043.txt where it was on screen", [] {
        double y = 0, h = 0;
        const bool shown = RowRect(F("b_dir/f043.txt"), &y, &h);
        MCGuiNote("f043 at y %.0f (was %.0f), the tree at %.0f (was %.0f)", shown ? y : -1, gY, TreeTop(), gTop);
        GCHECK(shown && std::fabs(y - gY) < gH);
        GCHECK(AllOpen());
        GCHECK_EQ_STR(Rel(Tree()->selectedPath()), "b_dir/f043.txt");
    });

    // ---- Rename to a name that sorts in place
    ClickRow("b_dir/f050.txt");
    Note("b_dir/f050.txt");
    MCGuiStep("F2", [] { MCGuiKey("f2"); });
    MCGuiWait("it asks", 10, [] { return MCGuiFindLabel(GTK_WIDGET(Win().window), "Rename to") != nullptr; });
    MCGuiStep("f050b.txt", [] {
        MCGuiKey("ctrl+a");
        MCGuiType("f050b.txt\n");
    });
    MCGuiWait("renamed, its row in", 10, [] {
        return MCGuiFileExists(F("b_dir/f050b.txt")) && PositionOf(F("b_dir/f050.txt")) < 0 &&
               Tree()->selectedPath() == F("b_dir/f050b.txt");
    });
    Kept("b_dir/f050b.txt", false);

    // ---- the last row: the one before it is selected
    MCGuiStep("scrolled to the end", [] {
        gtk_adjustment_set_value(Adj(), gtk_adjustment_get_upper(Adj()) - gtk_adjustment_get_page_size(Adj()));
    });
    MCGuiWait("r29.txt on screen", 10, [] { return RowLabel(F("r29.txt")) != nullptr; });
    ClickRow("r29.txt");
    Note("r29.txt");
    TrashSelected("r29.txt");
    MCGuiPause(0.4);   // a negative check, as in Kept
    MCGuiStep("r28.txt selected; the tree at its end still", [] {
        MCGuiNote("the tree at %.0f (was %.0f, the end %.0f), selected %s", TreeTop(), gTop,
                  gtk_adjustment_get_upper(Adj()) - gtk_adjustment_get_page_size(Adj()),
                  Rel(Tree()->selectedPath()).c_str());
        GCHECK_EQ_STR(Rel(Tree()->selectedPath()), "r28.txt");
        GCHECK(std::fabs(TreeTop() - gTop) < gH + 1);
        GCHECK(AllOpen());
        if (gScratchTrash) GCHECK(TreeHasKeyboard());
    });

    MCGuiStep("the window closes", [] { gtk_window_close(Win().window); });
    MCGuiWait("gone", 10, [] { return Win().window == nullptr; });
    MCGuiStep("the first window has the keyboard again; the folder goes", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();   // the X focus was on the window that closed
        std::filesystem::remove_all(gRepo);
    });
}
