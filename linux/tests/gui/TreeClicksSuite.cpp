// TreeClicksSuite.cpp — clicks in the file tree while a folder's listing
// goes in (`./build/mutible-guitest tree_clicks`), the follow-up to R1 in
// linux/PARITY.md.
//
// Since R1 a folder's rows change when a listing made on a worker comes in,
// at any moment, and two things went wrong, both seen as a GUI check that
// failed about one run in five:
//
//   - A new row's widgets are mapped as soon as the row is added and placed
//     only at the next layout. The harness counted a mapped label as on
//     show, and the save suite's first click, aimed at a label that had no
//     place yet, went to the window's top left corner and opened the File
//     menu. MCGuiFindLabel now hands out only widgets on screen
//     (MCGuiOnScreen: placed, and what GTK picks at their center, so not a
//     row scrolled just out of view either); checked here at the two
//     moments between a row being added and its first layout.
//   - The tree kept the pressed row, and the last click's row, as positions.
//     A listing that puts a row in above the pressed one while the button
//     is down moves the rows under a pointer held still, and the release
//     then opened the file now at that position, the one put in. The click
//     now acts on the row it was pressed on, kept as its row object.
//
// Rows are found by their paths (MCGuiTreeRow), never by their names: the
// suites before this one leave files of the same names in folders the tree
// has open. The last wait, for this folder's b.txt to be gone once the
// folder closed, found replace_in_folder's rif4/b.txt instead (the tree had
// opened rif4 following Find in Folder's results), which the closing moved
// up into view: a FLAKY line on CI from the run rif4 came in (2026-11-19),
// failing in the full order and passing alone, the folder closed all along.
//
// Real input throughout: the button is pressed and released with XTest; the
// files come from the disk, as a build or a terminal would make them.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"

#include <sys/stat.h>

#include <cmath>
#include <unistd.h>

namespace {

std::string dir() { return MCGuiProjectFile("0-tree-clicks"); }
std::string at(const std::string& name) { return dir() + "/" + name; }
// Files at the folder's top level, enough to scroll the tree.
std::string fill(int i) {
    char name[32];
    snprintf(name, sizeof name, "tc-fill-%02d.txt", i);
    return MCGuiProjectFile(name);
}
constexpr int kFill = 80;

GtkWidget* listView() {
    GtkWidget* w = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(MCGuiApp().tree->widget()));
    return GTK_IS_LIST_VIEW(w) ? w : nullptr;
}

GListModel* rows() {
    GtkWidget* lv = listView();
    return lv ? G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(lv))) : nullptr;
}

// A row on show, known by its path (see the top).
GtkWidget* row(const std::string& path) { return MCGuiTreeRow(path); }

// Any mapped row label for `path`, whether laid out or not: what the harness
// used to count as on show.
GtkWidget* anyLabel(GtkWidget* root, const std::string& path) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_LABEL(root)) {
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(root), "minicode-path"));
        return p && path == p ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = anyLabel(c, path)) return hit;
    return nullptr;
}

// The tree's row object for a path, or null; not a reference of its own.
GtkTreeListRow* rowObject(const std::string& path) {
    GListModel* m = rows();
    if (!m) return nullptr;
    GtkTreeListRow* found = nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(m) && !found; i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(r));
        GObject* file = info ? g_file_info_get_attribute_object(info, "standard::file") : nullptr;
        char* p = file ? g_file_get_path(G_FILE(file)) : nullptr;
        if (p && path == p) found = r;
        g_free(p);
        if (info) g_object_unref(info);
        g_object_unref(r);   // the list item and the model keep it
    }
    return found;
}

// The row's place in the tree's rows, or -1.
long rowIndex(const std::string& path) {
    GtkTreeListRow* r = rowObject(path);
    return r ? (long)gtk_tree_list_row_get_position(r) : -1;
}

double labelY(const std::string& path) {
    GtkWidget* label = row(path);
    if (!label) return -1;
    graphene_point_t p = GRAPHENE_POINT_INIT(0, 0), out;
    if (!gtk_widget_compute_point(label, GTK_WIDGET(MCGuiApp().window), &p, &out)) return -1;
    return out.y;
}

// ---- watching a row go in
// While armed, each change to the tree's rows looks for `gWatched`, a path,
// at once and again at the start of the next frame, before GTK lays it out.
std::string gWatched;
bool gArmed = false;
int gLooks = 0;
bool gSawUnplaced = false;     // a mapped label with no size: the window the flake fell in
bool gHandedUnplaced = false;  // MCGuiFindLabel returned such a label
gulong gChangedId = 0;

void look() {
    gLooks++;
    GtkWidget* tree = MCGuiApp().tree->widget();
    GtkWidget* any = anyLabel(tree, gWatched);
    if (any && !MCGuiOnScreen(any)) gSawUnplaced = true;
    // Both ways a check finds a row: by its name (another file's placed
    // label never counts) and by its path.
    const std::string name = gWatched.substr(gWatched.rfind('/') + 1);
    for (GtkWidget* handed : {MCGuiFindLabel(tree, name), row(gWatched)})
        if (handed && (gtk_widget_get_width(handed) <= 0 || gtk_widget_get_height(handed) <= 0))
            gHandedUnplaced = true;
}

void onRowsChanged(GListModel*, guint, guint, guint added, gpointer) {
    if (!gArmed || added == 0) return;
    look();
    GdkFrameClock* clock = gtk_widget_get_frame_clock(listView());
    if (!clock) return;
    // GTK lays out in the frame's layout phase; update comes just before it.
    auto* id = new gulong(0);
    *id = g_signal_connect(clock, "update", G_CALLBACK(+[](GdkFrameClock* c, gpointer p) {
        auto* handler = static_cast<gulong*>(p);
        look();
        g_signal_handler_disconnect(c, *handler);
        delete handler;
    }), id);
    gdk_frame_clock_request_phase(clock, GDK_FRAME_CLOCK_PHASE_UPDATE);
}

GtkTreeListRow* gHeldB = nullptr;
GtkTreeListRow* gHeldD = nullptr;
double gPressedY = 0;
long gPressedIndex = -1;

}  // namespace

MC_GUI_SUITE(tree_clicks) {
    MCGuiStep("a folder of its own, first in the tree; files enough to scroll it", [] {
        MCGuiWriteFile(at("b.txt"), "b\n");
        MCGuiWriteFile(at("d.txt"), "d\n");
        for (int i = 0; i < kFill; i++) MCGuiWriteFile(fill(i), "x\n");
    });
    MCGuiWait("listed", 20, [] { return rowObject(dir()) && rowObject(fill(kFill - 1)); });
    // After earlier suites the tree may be scrolled, and a row put in above
    // the top one stays above the view (GTK keeps the rows on show still).
    // Rows gone just before (session_browser's last step removes files at
    // the folder's top level) leave the tree keeping its place for a moment
    // (FileTree::keepRows: 300 ms and four layouts, up to 5 s on a loaded
    // machine), and the keep undoes a scroll to the top the tree did not
    // ask for: this suite scrolled the list itself and found its folder
    // never on show (the soak, November 2026). The tree's own reveal ends
    // the keep; a keep is made here on purpose to check that.
    MCGuiStep("the tree scrolled to its end", [] {
        GtkAdjustment* a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(MCGuiApp().tree->widget()));
        gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a));
    });
    MCGuiWait("scrolled", 10, [] {
        GtkAdjustment* a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(MCGuiApp().tree->widget()));
        return gtk_adjustment_get_value(a) > 100 && !row(dir());
    });
    MCGuiStep("four of the files go", [] {
        for (int i = 0; i < 4; i++) unlink(fill(i).c_str());
    });
    MCGuiWait("the tree keeps its place; then it reveals the folder", 10, [] {
        if (!MCGuiApp().tree->keeping()) return false;
        MCGuiApp().tree->revealPath(dir());
        return true;
    });
    MCGuiWait("its row on show", 20, [] { return row(dir()) != nullptr; });
    MCGuiStep("a click opens it", [] { MCGuiClick(row(dir())); });
    MCGuiWait("its files are on show", 20, [] { return row(at("b.txt")) && row(at("d.txt")); });

    // A row listed while the tree is on show is never handed to a check
    // before its first layout.
    MCGuiStep("watching, a file appears in the folder", [] {
        gWatched = at("new.txt");
        gArmed = true;
        gChangedId = g_signal_connect(rows(), "items-changed", G_CALLBACK(onRowsChanged), nullptr);
        MCGuiWriteFile(at("new.txt"), "new\n");
    });
    MCGuiWait("its row is on show", 20, [] { return row(at("new.txt")) != nullptr; });
    MCGuiPause(0.2);   // time is the condition: the frame after it, looked at too
    MCGuiStep("the harness never handed out the row before it had a place", [] {
        gArmed = false;
        g_signal_handler_disconnect(rows(), gChangedId);
        MCGuiNote("looked %d times; the row was mapped before it was placed: %s", gLooks,
                  gSawUnplaced ? "yes" : "no");
        GCHECK(gLooks >= 2);
        GCHECK(gSawUnplaced);   // the moment the flake fell in did come
        GCHECK(!gHandedUnplaced);
        GCHECK(MCGuiOnScreen(row(at("new.txt"))));
    });

    // A press on d.txt, and while the button is down a listing puts c.txt in
    // above it, so d.txt is one position further down when the button comes
    // up. GTK then either keeps the focused row, d.txt, under the pointer
    // (with the tree scrolled) or draws c.txt in its place (at the top); kept
    // by position, the click opened nothing in the first case and c.txt in
    // the second.
    MCGuiStep("the button goes down on d.txt", [] {
        gHeldB = GTK_TREE_LIST_ROW(g_object_ref(rowObject(at("b.txt"))));
        gHeldD = GTK_TREE_LIST_ROW(g_object_ref(rowObject(at("d.txt"))));
        gPressedIndex = rowIndex(at("d.txt"));
        gPressedY = labelY(at("d.txt"));
        MCGuiPress(row(at("d.txt")));
    });
    MCGuiPause(0.2);   // time is the condition: the press is in before the file appears
    MCGuiStep("another program writes c.txt, which sorts into d's place", [] {
        MCGuiWriteFile(at("c.txt"), "c\n");
    });
    MCGuiWait("c.txt is drawn above d.txt, d.txt a position further down", 20, [] {
        const double c = labelY(at("c.txt")), d = labelY(at("d.txt"));
        return c >= 0 && d > c && rowIndex(at("d.txt")) == gPressedIndex + 1;
    });
    MCGuiStep("", [] {
        const double c = labelY(at("c.txt")), d = labelY(at("d.txt"));
        MCGuiNote("under the pointer now: %s", std::abs(c - gPressedY) < 5   ? "c.txt"
                                               : std::abs(d - gPressedY) < 5 ? "d.txt, kept by GTK"
                                                                             : "another row");
    });
    MCGuiStep("the rows that stayed are the same row objects", [] {
        GCHECK(rowObject(at("b.txt")) == gHeldB);
        GCHECK(rowObject(at("d.txt")) == gHeldD);
        g_clear_object(&gHeldB);
        g_clear_object(&gHeldD);
    });
    MCGuiStep("the button comes up, the pointer not moved", [] { MCGuiRelease(); });
    MCGuiWait("the file pressed opens, d.txt", 10, [] {
        return MCGuiApp().editor->currentPath() == at("d.txt");
    });
    MCGuiStep("not c.txt", [] { GCHECK(MCGuiApp().editor->currentPath() != at("c.txt")); });
    MCGuiPause(0.5);   // the double-click interval: the next click is a new one
    MCGuiStep("an ordinary click on c.txt opens it", [] { MCGuiClick(row(at("c.txt"))); });
    MCGuiWait("c.txt opens", 10, [] { return MCGuiApp().editor->currentPath() == at("c.txt"); });
    MCGuiPause(0.5);   // the double-click interval
    MCGuiStep("the tree reveals the folder", [] { MCGuiApp().tree->revealPath(dir()); });
    MCGuiWait("the folder's row on show", 10, [] { return row(dir()) != nullptr; });
    MCGuiStep("the folder closes", [] { MCGuiClick(row(dir())); });
    MCGuiWait("closed", 10, [] { return rowIndex(at("b.txt")) < 0 && !row(at("b.txt")); });
    MCGuiStep("the files made to scroll the tree go", [] {
        for (int i = 4; i < kFill; i++) unlink(fill(i).c_str());
    });
    // Not left keeping its place for the next suite (see the top).
    MCGuiWait("gone from the tree, which keeps no place", 20, [] {
        return !rowObject(fill(kFill - 1)) && !MCGuiApp().tree->keeping();
    });
}
