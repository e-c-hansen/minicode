// TreeFocusSuite.cpp — `./build/mutible-guitest tree_focus`: Focus on
// Folder (HANDOFF.md item 94), the Mac's tree_focus suite
// (mac/tests/gui/TreeFocusSuite.mm) as far as GTK goes. A folder's right
// click offers it (a file's does not); taken, the tree's root is that
// folder, a breadcrumb row above it names the way down from the window's
// folder, and the window's folder is unchanged. A file made in the focused
// folder shows; a click on a part of the breadcrumb roots the tree there
// with the folders that were open still open, the first part or the ×
// shows the whole folder, and so does Show Whole Folder in Tree. An open
// outside the focus leaves it; a reveal widens it to the nearest folder
// above both, Reveal Active File in File Tree too. The focus comes back with
// its window's record and with its folder's session, a saved focus on a
// folder gone since comes back as the whole folder, and the focused folder
// deleted on disk gives the whole folder back with a note.
//
// Several folders (Add to Focus, HANDOFF.md item 98): Add to Focus in a
// folder's right click and from the menu bar takes the selected folder;
// the bar's + asks the folder chooser (answered by the check), starting
// in the window's folder, and adds what it chose, so the tree shows each
// folder as a root row labelled with its path, opened, under a row of
// their labels. A file made in a root shows; a folder inside a focused
// one adds nothing; a reveal outside every root adds its folder; Remove
// from Focus on a root row's right click and a label's × take one out (one
// left gives the breadcrumb back), a click on a label selects its root
// row; the chooser refuses a folder outside the window's; Add to Focus
// with nothing new to add asks the chooser. Both folders come back with
// the window's record (a folder gone since dropped) and with the folder's
// session, and a focused folder deleted on disk drops out alone.
//
// The tree follows the active tab (explorer.auto-reveal, HANDOFF.md item
// 107), in a folder of 6,000 files: a deep file in folders never listed,
// opened, is selected with its folders open and centered; another tab
// shows its row and back again; a row already in view is selected without
// the tree moving; a real click on a row cut by the bottom opens it without
// scrolling; the editor keeps the keyboard throughout; a file outside the
// focused folder leaves the tree alone; with the setting off nothing moves,
// and turning it on shows the file; with the sidebar closed nothing moves
// until it opens again; and a window brought back from its record has the
// record's open folders, selection and scroll, not moved to its file.
//
// The clicks on the breadcrumb, the × and the rows' right clicks are real
// (XTest); the command is the window's action, as the menu bar and the
// palette run it.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"
#include "Persistence.h"
#include "TabSessions.h"
#include "WindowLayout.h"
#include "FileDetails.h"   // TreeFocus::shortLabels

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string gRepo;
std::string F(const std::string& rel) { return gRepo + "/" + rel; }
size_t gWindowsBefore = 0;
std::string gRecord;

// The suite's window: the newest one on the folder.
MCGuiAppWindow Win() {
    MCGuiAppWindow found;
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == gRepo) found = w;
    return found;
}
FileTree* Tree() { return Win().tree; }

GListModel* Rows(FileTree* t) {
    GtkWidget* lv = t ? gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(t->widget())) : nullptr;
    return GTK_IS_LIST_VIEW(lv) ? G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(lv))) : nullptr;
}

std::string PathOf(GtkTreeListRow* r) {
    GFileInfo* info = G_FILE_INFO(gtk_tree_list_row_get_item(r));
    GObject* file = info ? g_file_info_get_attribute_object(info, "standard::file") : nullptr;
    char* p = file ? g_file_get_path(G_FILE(file)) : nullptr;
    std::string out = p ? p : "";
    g_free(p);
    if (info) g_object_unref(info);
    return out;
}

// The tree's row for `path`, or null; not a reference of its own.
GtkTreeListRow* RowObject(FileTree* t, const std::string& path) {
    GListModel* m = Rows(t);
    if (!m) return nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        const bool hit = PathOf(r) == path;
        g_object_unref(r);   // the model keeps it
        if (hit) return r;
    }
    return nullptr;
}
bool HasRow(FileTree* t, const std::string& path) { return RowObject(t, path) != nullptr; }
// A row the list does not show has no owner but the caller's reference:
// these two hold it while they use it.
bool Expanded(FileTree* t, const std::string& path) {
    GListModel* m = Rows(t);
    for (guint i = 0; m && i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        const bool hit = PathOf(r) == path, open = hit && gtk_tree_list_row_get_expanded(r);
        g_object_unref(r);
        if (hit) return open;
    }
    return false;
}
void SetExpanded(FileTree* t, const std::string& path, bool open) {
    GListModel* m = Rows(t);
    for (guint i = 0; m && i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        const bool hit = PathOf(r) == path;
        if (hit) gtk_tree_list_row_set_expanded(r, open);
        g_object_unref(r);
        if (hit) return;
    }
    GCHECK(!"no row to open or close");
}

// The names of the rows at the top of the tree, in order.
std::string TopRows(FileTree* t) {
    std::string out;
    GListModel* m = Rows(t);
    if (!m) return out;
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        if (gtk_tree_list_row_get_depth(r) == 0) {
            const std::string p = PathOf(r);
            out += (out.empty() ? "" : " ") + p.substr(p.find_last_of('/') + 1);
        }
        g_object_unref(r);
    }
    return out;
}

std::string Labels(FileTree* t) {
    std::string out;
    for (const std::string& l : t->focusBarLabels()) out += (out.empty() ? "" : " › ") + l;
    return out;
}

bool BarShown(FileTree* t) { return t && gtk_widget_get_visible(t->focusBar()); }

// A row's name label, on screen to be clicked.
GtkWidget* RowLabel(FileTree* t, const std::string& path, GtkWidget* root = nullptr) {
    if (!root) root = t ? t->widget() : nullptr;
    if (!root) return nullptr;
    if (GTK_IS_LABEL(root) && gtk_widget_has_css_class(root, "minicode-tree-label")) {
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(root), "minicode-path"));
        return p && path == p && MCGuiOnScreen(root) ? root : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = RowLabel(t, path, c)) return hit;
    return nullptr;
}

// The tree's right click menu while it is up.
GtkWidget* OpenMenu(FileTree* t) {
    GtkWidget* lv = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(t->widget()));
    for (GtkWidget* c = lv ? gtk_widget_get_first_child(lv) : nullptr; c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_POPOVER_MENU(c) && gtk_widget_get_mapped(c)) return c;
    return nullptr;
}
// The labels of the menu's items on show, in order.
std::vector<std::string> MenuLabels(GtkWidget* menu) {
    std::vector<std::string> out;
    std::vector<GtkWidget*> todo = {menu};
    while (!todo.empty()) {
        GtkWidget* w = todo.front();
        todo.erase(todo.begin());
        if (!w || !gtk_widget_get_mapped(w)) continue;
        if (GTK_IS_LABEL(w) && *gtk_label_get_text(GTK_LABEL(w))) out.push_back(gtk_label_get_text(GTK_LABEL(w)));
        std::vector<GtkWidget*> kids;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            kids.push_back(c);
        todo.insert(todo.begin(), kids.begin(), kids.end());
    }
    return out;
}
// The breadcrumb's right click menu while it is up, and the item showing
// `label` in a menu (the button a click goes to).
GtkWidget* BarMenu(FileTree* t) {
    for (GtkWidget* c = gtk_widget_get_first_child(t->focusBar()); c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_POPOVER_MENU(c) && gtk_widget_get_mapped(c)) return c;
    return nullptr;
}
GtkWidget* MenuButton(GtkWidget* menu, const std::string& label) {
    GtkWidget* l = menu ? MCGuiFindLabel(menu, label) : nullptr;
    for (GtkWidget* w = l; w && w != menu; w = gtk_widget_get_parent(w))
        if (std::string(G_OBJECT_TYPE_NAME(w)) == "GtkModelButton") return w;
    return l;
}
bool Has(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}

void Run(const char* action) {
    g_action_group_activate_action(G_ACTION_GROUP(Win().window), action, nullptr);
}
bool Enabled(const char* action) {
    return g_action_group_get_action_enabled(G_ACTION_GROUP(Win().window), action);
}

// The focus the folder's own session keeps ("(no session)" for none).
std::string StoredFocus() {
    const TabSessions::Sessions all =
        TabSessions::decode(MCGuiReadFile(MCGuiScratch() + "/state/mutible/sessions.json"));
    const auto it = all.find(Persistence::keyFor(gRepo));
    if (it == all.end()) return "(no session)";
    // A list since Add to Focus, joined by ", " as FileTree::focusPath.
    std::string out;
    for (const std::string& f : it->second.treeFocus) out += (out.empty() ? "" : ", ") + f;
    return out;
}

// The rows at the top of the tree, as paths from the folder.
std::string TopPaths(FileTree* t) {
    std::string out;
    GListModel* m = Rows(t);
    if (!m) return out;
    for (guint i = 0; i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        if (gtk_tree_list_row_get_depth(r) == 0) {
            const std::string p = PathOf(r);
            out += (out.empty() ? "" : " ") + p.substr(std::min(p.size(), gRepo.size() + 1));
        }
        g_object_unref(r);
    }
    return out;
}

// The folder chooser's answers (Add Folder to Focus…): what it is asked
// to start in, how often, and the folder it gives.
std::string gChooserStart, gChooserAnswer;
int gChooserAsked = 0;
void AnswerChooser(const std::string& folder) {
    gChooserAnswer = folder;
    MCGuiAnswerFolderChooser([](const std::string& start) {
        gChooserStart = start;
        gChooserAsked++;
        return gChooserAnswer;
    });
}

std::string StatusText() {
    GtkWidget* l = Win().statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}

// Whether the menu bar's File menu has an item running `action`.
bool MenuBarHas(GMenuModel* m, const std::string& action) {
    if (!m) return false;
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        char* a = nullptr;
        if (g_menu_model_get_item_attribute(m, i, G_MENU_ATTRIBUTE_ACTION, "s", &a)) {
            const bool hit = action == a;
            g_free(a);
            if (hit) return true;
        }
        for (const char* link : {G_MENU_LINK_SECTION, G_MENU_LINK_SUBMENU}) {
            GMenuModel* sub = g_menu_model_get_item_link(m, i, link);
            const bool hit = sub && MenuBarHas(sub, action);
            if (sub) g_object_unref(sub);
            if (hit) return true;
        }
    }
    return false;
}

void OpenRepoWindow(const std::string& what) {
    MCGuiStep(what, [] {
        const size_t before = MCGuiAppWindows().size();
        MCGuiOpenWindow(gRepo);
        GCHECK(MCGuiAppWindows().size() == before + 1);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("drawn", 10, [] { return Win().window && gtk_widget_get_mapped(GTK_WIDGET(Win().window)); });
}

// ---- the tree following the active tab
constexpr int kBigFiles = 3000;
const char* const kTarget = "m_deep/a/b/c/d/e/target.txt";
const char* const kNear = "m_deep/a/b/c/d/e/near.txt";
const char* const kCut = "z_big/f00040.txt";
std::string gSettingsBefore, gSelected;
double gTop = 0;

GtkAdjustment* Adj(FileTree* t) {
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(t->widget()));
}
GtkWidget* ListView(FileTree* t) { return gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(t->widget())); }
double TreeTop(FileTree* t) { return gtk_adjustment_get_value(Adj(t)); }
bool Near(double a, double b) { return std::fabs(a - b) < 1; }

// The label inside `w` naming `path`, mapped or not.
GtkWidget* LabelIn(GtkWidget* w, const std::string& path) {
    if (GTK_IS_LABEL(w) && gtk_widget_has_css_class(w, "minicode-tree-label")) {
        const char* p = static_cast<const char*>(g_object_get_data(G_OBJECT(w), "minicode-path"));
        return p && path == p ? w : nullptr;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = LabelIn(c, path)) return hit;
    return nullptr;
}
// The list's row widget for `path` while the list has one laid out, and
// any row widget it has.
GtkWidget* RowWidget(FileTree* t, const std::string& path) {
    for (GtkWidget* c = gtk_widget_get_first_child(ListView(t)); c; c = gtk_widget_get_next_sibling(c))
        if (gtk_widget_get_mapped(c) && gtk_widget_get_child_visible(c) && LabelIn(c, path)) return c;
    return nullptr;
}
// A row's whole height, its padding in (gtk_widget_get_height leaves it
// out): the rows' stride.
double RowHeight(FileTree* t) {
    graphene_rect_t b;
    for (GtkWidget* c = gtk_widget_get_first_child(ListView(t)); c; c = gtk_widget_get_next_sibling(c))
        if (gtk_widget_get_mapped(c) && gtk_widget_get_child_visible(c) && !GTK_IS_POPOVER(c) &&
            gtk_widget_compute_bounds(c, ListView(t), &b) && b.size.height > 0)
            return b.size.height;
    return 0;
}
// Where `path`'s row is drawn in the list view (its top and height).
bool RowRect(FileTree* t, const std::string& path, double* y, double* h) {
    GtkWidget* r = RowWidget(t, path);
    graphene_rect_t b;
    if (!r || !gtk_widget_compute_bounds(r, ListView(t), &b)) return false;
    *y = b.origin.y;
    *h = b.size.height;
    return *h > 0;
}
// How far a row's middle is from the tree's, in rows.
double RowsFromMiddle(FileTree* t, const std::string& path) {
    double y = 0, h = 0;
    if (!RowRect(t, path, &y, &h)) return 1e9;
    return std::fabs(y + h / 2 - gtk_adjustment_get_page_size(Adj(t)) / 2) / h;
}
bool WhollyInView(FileTree* t, const std::string& path) {
    double y = 0, h = 0;
    return RowRect(t, path, &y, &h) && y >= -0.5 && y + h <= gtk_adjustment_get_page_size(Adj(t)) + 0.5;
}
int PositionOf(FileTree* t, const std::string& path) {
    GListModel* m = Rows(t);
    for (guint i = 0; m && i < g_list_model_get_n_items(m); i++) {
        auto* r = GTK_TREE_LIST_ROW(g_list_model_get_item(m, i));
        const bool hit = PathOf(r) == path;
        g_object_unref(r);
        if (hit) return (int)i;
    }
    return -1;
}
// The top of `path`'s row in the tree's scroll, placed from `known`'s,
// which shows (the rows all have one height).
double RowTopFrom(FileTree* t, const std::string& known, const std::string& path) {
    double y = 0, h = 0;
    if (!RowRect(t, known, &y, &h)) return 0;
    return y + TreeTop(t) + (PositionOf(t, path) - PositionOf(t, known)) * h;
}
bool EditorHasKeyboard() {
    return gtk_root_get_focus(GTK_ROOT(Win().window)) == Win().editor->textView();
}
void Activate(const std::string& path) {
    Editor* e = Win().editor;
    GCHECK(e->documentFor(path) != nullptr);
    e->activate(e->documentFor(path));
}

void CloseRepoWindows(const std::string& what) {
    MCGuiStep(what, [] {
        for (const MCGuiAppWindow& w : MCGuiAppWindows())
            if (w.rootDir == gRepo) gtk_window_close(w.window);
    });
    MCGuiWait("gone", 10, [] { return Win().window == nullptr; });
    MCGuiStep("the first window has the keyboard again", [] { MCGuiSetKeyWindow(nullptr); });
}

}  // namespace

MC_GUI_SUITE(tree_focus) {
    MCGuiStep("a folder: mac/src (a.mm, deep/d.txt), mac/tests/t.mm, src/core.cpp, README.md", [] {
        gRepo = MCGuiScratch() + "/focusrepo";
        gWindowsBefore = MCGuiAppWindows().size();
        MCGuiWriteFile(F("mac/src/a.mm"), "// a\n");
        MCGuiWriteFile(F("mac/src/deep/d.txt"), "d\n");
        MCGuiWriteFile(F("mac/tests/t.mm"), "// t\n");
        MCGuiWriteFile(F("src/core.cpp"), "// core\n");
        MCGuiWriteFile(F("README.md"), "# focus\n");
    });
    OpenRepoWindow("a window on it");
    MCGuiWait("its rows", 10, [] { return TopRows(Tree()) == "mac src README.md"; });
    MCGuiStep("mac, mac/tests and src open", [] {
        Tree()->revealPath(F("src/core.cpp"));
        Tree()->revealPath(F("mac/tests/t.mm"));
    });
    MCGuiWait("their rows", 10, [] {
        return HasRow(Tree(), F("src/core.cpp")) && HasRow(Tree(), F("mac/tests/t.mm"));
    });
    MCGuiStep("the whole folder: no bar, nothing to widen, both commands in the File menu", [] {
        GCHECK_EQ_STR(Tree()->focusPath(), "");
        GCHECK(!BarShown(Tree()));
        GCHECK(!Enabled("showwholefolder"));
        GMenuModel* bar = gtk_application_get_menubar(MCGuiApplication());
        GCHECK(MenuBarHas(bar, "win.focusfolder"));
        GCHECK(MenuBarHas(bar, "win.showwholefolder"));
    });

    // ---- the right click menu
    MCGuiStep("mac/src selected", [] { Tree()->revealPath(F("mac/src")); });
    MCGuiWait("selected", 10, [] { return Tree()->selectedPath() == F("mac/src"); });
    MCGuiWait("its row on screen", 10, [] { return RowLabel(Tree(), F("mac/src")) != nullptr; });
    MCGuiStep("a right click on mac/src's row", [] {
        MCGuiClick(RowLabel(Tree(), F("mac/src")), -1, -1, 3);
    });
    MCGuiWait("its menu", 10, [] { return OpenMenu(Tree()) != nullptr; });
    MCGuiStep("a folder's right click offers Focus on Folder, not Show Whole Folder", [] {
        const std::vector<std::string> items = MenuLabels(OpenMenu(Tree()));
        GCHECK(Has(items, "Focus on Folder"));
        GCHECK(!Has(items, "Show Whole Folder in Tree"));
        GCHECK(Has(items, "Rename…"));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return OpenMenu(Tree()) == nullptr; });
    MCGuiWait("README.md's row on screen", 10, [] { return RowLabel(Tree(), F("README.md")) != nullptr; });
    MCGuiStep("a right click on README.md's row", [] {
        MCGuiClick(RowLabel(Tree(), F("README.md")), -1, -1, 3);
    });
    MCGuiWait("its menu", 10, [] { return OpenMenu(Tree()) != nullptr; });
    MCGuiStep("a file's right click does not offer it", [] {
        const std::vector<std::string> items = MenuLabels(OpenMenu(Tree()));
        GCHECK(!items.empty());
        GCHECK(!Has(items, "Focus on Folder"));
        GCHECK(!Has(items, "Show Whole Folder in Tree"));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return OpenMenu(Tree()) == nullptr; });

    // ---- focused on mac/src
    MCGuiStep("mac/src selected again", [] { Tree()->revealPath(F("mac/src")); });
    MCGuiWait("selected", 10, [] { return Tree()->selectedPath() == F("mac/src"); });
    MCGuiStep("Focus on Folder", [] { Run("focusfolder"); });
    MCGuiWait("the tree starts at mac/src", 10, [] { return TopRows(Tree()) == "deep a.mm"; });
    MCGuiStep("the breadcrumb says where; the window's folder is unchanged", [] {
        GCHECK_EQ_STR(Tree()->focusPath(), "mac/src");
        GCHECK_EQ_STR(Tree()->root(), gRepo);
        GCHECK_EQ_STR(Win().rootDir, gRepo);
        GCHECK_EQ_STR(Tree()->top(), F("mac/src"));
        GCHECK(BarShown(Tree()));
        GCHECK_EQ_STR(Labels(Tree()), "focusrepo › mac › src");
        GCHECK(Enabled("showwholefolder"));
        // The bar is over the rows, never on them.
        graphene_rect_t bar, rows;
        GCHECK(gtk_widget_compute_bounds(Tree()->focusBar(), Tree()->container(), &bar));
        GCHECK(gtk_widget_compute_bounds(Tree()->widget(), Tree()->container(), &rows));
        MCGuiNote("bar %.0f+%.0f, rows from %.0f", bar.origin.y, bar.size.height, rows.origin.y);
        GCHECK(bar.size.height >= 20 && bar.origin.y + bar.size.height <= rows.origin.y + 0.5);
        // Kept in the folder's session at once.
        GCHECK_EQ_STR(StoredFocus(), "mac/src");
        Tree()->revealPath(F("mac/src/deep/d.txt"));   // deep open
    });
    MCGuiWait("the bar's parts drawn", 10, [] {
        return MCGuiOnScreen(Tree()->focusBarPart(0)) && MCGuiOnScreen(Tree()->focusBarPart(2)) &&
               Tree()->selectedPath() == F("mac/src/deep/d.txt");
    });
    MCGuiStep("a picture of it; a file made in the focused folder", [] {
        Tree()->clearSelection();   // as the Mac suite has it: nothing selected
        MCGuiSnapshot(Tree()->container(), "tree_focus-bar");
        MCGuiWriteFile(F("mac/src/new.txt"), "n\n");
    });
    MCGuiWait("it shows", 10, [] { return HasRow(Tree(), F("mac/src/new.txt")); });

    // ---- the breadcrumb
    MCGuiClickWhenReady("the breadcrumb's mac", [] { return Tree()->focusBarPart(1); });
    MCGuiWait("the tree starts at mac", 10, [] { return Tree()->focusPath() == "mac"; });
    MCGuiWait("what was open stays open, mac/tests too; mac/src selected", 10, [] {
        return TopRows(Tree()) == "src tests" && Expanded(Tree(), F("mac/src")) &&
               Expanded(Tree(), F("mac/src/deep")) && Expanded(Tree(), F("mac/tests")) &&
               Tree()->selectedPath() == F("mac/src");
    });
    MCGuiStep("the breadcrumb is shorter", [] {
        MCGuiNote("rows %s; open: mac/src %d, deep %d, tests %d; selected %s", TopRows(Tree()).c_str(),
                  Expanded(Tree(), F("mac/src")), Expanded(Tree(), F("mac/src/deep")),
                  Expanded(Tree(), F("mac/tests")), Tree()->selectedPath().c_str());
        GCHECK_EQ_STR(Labels(Tree()), "focusrepo › mac");
        GCHECK_EQ_STR(StoredFocus(), "mac");
    });
    MCGuiClickWhenReady("its first part", [] { return Tree()->focusBarPart(0); });
    MCGuiWait("the whole folder, as it was left", 10, [] {
        return Tree()->focusPath().empty() && TopRows(Tree()) == "mac src README.md" &&
               Expanded(Tree(), F("mac")) && Expanded(Tree(), F("src")) &&
               Expanded(Tree(), F("mac/src")) && HasRow(Tree(), F("src/core.cpp"));
    });
    MCGuiStep("no bar; the folder's session keeps no focus", [] {
        GCHECK(!BarShown(Tree()));
        const std::string stored = StoredFocus();
        GCHECK(stored.empty() || stored == "(no session)");
        GCHECK(Tree()->setFocus("mac/src"));
    });
    MCGuiWait("focused again", 10, [] { return TopRows(Tree()) == "deep a.mm new.txt"; });
    MCGuiClickWhenReady("the ×", [] { return Tree()->focusBarClose(); });
    MCGuiWait("the whole folder", 10, [] {
        return Tree()->focusPath().empty() && TopRows(Tree()) == "mac src README.md";
    });
    MCGuiStep("focused again", [] { GCHECK(Tree()->setFocus("mac/src")); });
    MCGuiWait("the bar on screen", 10, [] { return MCGuiOnScreen(Tree()->focusBarPart(2)); });
    MCGuiStep("a right click on the bar", [] { MCGuiClick(Tree()->focusBarPart(2), -1, -1, 3); });
    MCGuiWait("its menu", 10, [] { return BarMenu(Tree()) != nullptr; });
    MCGuiClickWhenReady("its Show Whole Folder in Tree", [] {
        return MenuButton(BarMenu(Tree()), "Show Whole Folder in Tree");
    });
    MCGuiWait("the whole folder", 10, [] {
        return Tree()->focusPath().empty() && TopRows(Tree()) == "mac src README.md";
    });
    MCGuiStep("Show Whole Folder in Tree does the same; a folder not there is refused", [] {
        GCHECK(Tree()->setFocus("mac/src"));
        GCHECK_EQ_STR(Tree()->focusPath(), "mac/src");
        Run("showwholefolder");
        GCHECK_EQ_STR(Tree()->focusPath(), "");
        GCHECK(!BarShown(Tree()));
        GCHECK(!Tree()->setFocus("mac/nope"));
        GCHECK(!Tree()->setFocus(".claude/worktrees"));
        GCHECK_EQ_STR(Tree()->focusPath(), "");
    });
    MCGuiStep("a focus too long for the bar", [] {
        MCGuiWriteFile(F("mac/src/a-folder-with-a-rather-long-name/and-another-long-one/x.txt"), "x\n");
        GCHECK(Tree()->setFocus("mac/src/a-folder-with-a-rather-long-name/and-another-long-one"));
    });
    MCGuiWait("drawn", 10, [] {
        return MCGuiOnScreen(Tree()->focusBarPart(0)) && MCGuiOnScreen(Tree()->focusBarPart(4));
    });
    MCGuiStep("the first part, an ellipsis, and the last ones that fit (TreeFocus::fit)", [] {
        MCGuiSnapshot(Tree()->container(), "tree_focus-long");
        GCHECK(gtk_widget_get_child_visible(Tree()->focusBarPart(0)));
        GCHECK(!gtk_widget_get_child_visible(Tree()->focusBarPart(1)));
        GCHECK(gtk_widget_get_child_visible(Tree()->focusBarPart(4)));
        // Every part shown sits inside the bar.
        graphene_rect_t bar, last;
        GCHECK(gtk_widget_compute_bounds(Tree()->focusBar(), Tree()->container(), &bar));
        GCHECK(gtk_widget_compute_bounds(Tree()->focusBarPart(4), Tree()->container(), &last));
        GCHECK(last.origin.x + last.size.width <= bar.origin.x + bar.size.width + 0.5);
        GCHECK(Tree()->setFocus(""));
        std::filesystem::remove_all(F("mac/src/a-folder-with-a-rather-long-name"));
    });

    // ---- opens and reveals
    MCGuiStep("focused on mac/src, src/core.cpp opened", [] {
        GCHECK(Tree()->setFocus("mac/src"));
        MCGuiOpenFileKept(F("src/core.cpp"));
    });
    MCGuiWait("it shows", 10, [] { return Win().editor->currentPath() == F("src/core.cpp"); });
    MCGuiStep("an open outside the focus leaves it", [] {
        GCHECK_EQ_STR(Tree()->focusPath(), "mac/src");
        Tree()->revealPath(F("mac/src/a.mm"), true);   // inside: selected as before
        GCHECK_EQ_STR(Tree()->focusPath(), "mac/src");
    });
    MCGuiWait("a.mm selected", 10, [] { return Tree()->selectedPath() == F("mac/src/a.mm"); });
    MCGuiStep("a reveal widens to the nearest folder above both", [] {
        Tree()->revealPath(F("mac/tests/t.mm"), true);
        GCHECK_EQ_STR(Tree()->focusPath(), "mac");
    });
    MCGuiWait("t.mm selected", 10, [] { return Tree()->selectedPath() == F("mac/tests/t.mm"); });
    MCGuiStep("Reveal Active File in File Tree: core.cpp is beside mac", [] {
        Run("revealactiveintree");
        GCHECK_EQ_STR(Tree()->focusPath(), "");
    });
    MCGuiWait("core.cpp selected", 10, [] { return Tree()->selectedPath() == F("src/core.cpp"); });
    MCGuiStep("t.mm selected", [] { Tree()->revealPath(F("mac/tests/t.mm")); });
    MCGuiWait("selected", 10, [] { return Tree()->selectedPath() == F("mac/tests/t.mm"); });
    MCGuiStep("with a file selected, the command focuses its folder", [] { Run("focusfolder"); });
    MCGuiWait("mac/tests", 10, [] {
        return Tree()->focusPath() == "mac/tests" && TopRows(Tree()) == "t.mm";
    });

    // ---- brought back
    MCGuiStep("focused on mac/src: the window's record keeps it", [] {
        GCHECK(Tree()->setFocus("mac/src"));
        gRecord = MCGuiWindowRecord(Win().window);
        const std::vector<TabSessions::Window> w = TabSessions::decodeWindows(gRecord);
        GCHECK(w.size() == 1 && w[0].tabs.treeFocus == std::vector<std::string>{"mac/src"});
        MCGuiOpenWindowRestoring(gRepo, gRecord);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("a window brought back from it, focused there", 10, [] {
        FileTree* back = MCGuiApp().tree;
        return MCGuiAppWindows().size() == gWindowsBefore + 2 && back->focusPath() == "mac/src" &&
               TopRows(back) == "deep a.mm new.txt";
    });
    MCGuiStep("its breadcrumb", [] {
        FileTree* back = MCGuiApp().tree;
        GCHECK(BarShown(back));
        GCHECK_EQ_STR(Labels(back), "focusrepo › mac › src");
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("closed", 10, [] { return MCGuiAppWindows().size() == gWindowsBefore + 1; });
    MCGuiStep("a record focused on a folder gone since", [] {
        MCGuiSetKeyWindow(nullptr);
        std::vector<TabSessions::Window> w = TabSessions::decodeWindows(gRecord);
        GCHECK(!w.empty());
        if (w.empty()) return;
        w[0].tabs.treeFocus = {"mac/gone"};
        MCGuiOpenWindowRestoring(gRepo, TabSessions::encodeWindows(w));
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("brings its window back on the whole folder", 10, [] {
        FileTree* back = MCGuiApp().tree;
        return MCGuiAppWindows().size() == gWindowsBefore + 2 && back->focusPath().empty() &&
               TopRows(back) == "mac src README.md" && !BarShown(back);
    });
    MCGuiStep("that window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("closed", 10, [] { return MCGuiAppWindows().size() == gWindowsBefore + 1; });
    MCGuiStep("the focus still mac/src; the window closes", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(Tree()->focusPath(), "mac/src");
    });
    CloseRepoWindows("the window closes");
    MCGuiStep("the folder's session keeps the focus", [] { GCHECK_EQ_STR(StoredFocus(), "mac/src"); });
    OpenRepoWindow("the folder opened again");
    MCGuiWait("focused as it was", 10, [] {
        return Tree()->focusPath() == "mac/src" && TopRows(Tree()) == "deep a.mm new.txt" &&
               BarShown(Tree());
    });

    // ---- gone from the disk
    MCGuiStep("focused on deep", [] { GCHECK(Tree()->setFocus("mac/src/deep")); });
    MCGuiWait("its rows", 10, [] { return TopRows(Tree()) == "d.txt"; });
    MCGuiStep("deep deleted on disk", [] { std::filesystem::remove_all(F("mac/src/deep")); });
    MCGuiWait("the whole folder again", 10, [] { return Tree()->focusPath().empty(); });
    MCGuiWait("its rows, with no deep", 10, [] {
        return TopRows(Tree()) == "mac src README.md" && !HasRow(Tree(), F("mac/src/deep"));
    });
    MCGuiStep("the bar is gone, the status bar says why, the session keeps no focus", [] {
        GCHECK(!BarShown(Tree()));
        MCGuiNote("status: %s", StatusText().c_str());
        GCHECK(StatusText().find("focused folder is gone") != std::string::npos);
        const std::string stored = StoredFocus();
        GCHECK(stored.empty() || stored == "(no session)");
    });
    MCGuiStep("focused on mac/tests, the window closed, mac/tests renamed", [] {
        GCHECK(Tree()->setFocus("mac/tests"));
    });
    CloseRepoWindows("the window closes");
    MCGuiStep("mac/tests renamed mac/checks", [] {
        GCHECK_EQ_STR(StoredFocus(), "mac/tests");
        std::filesystem::rename(F("mac/tests"), F("mac/checks"));
    });
    OpenRepoWindow("the folder opened again");
    MCGuiWait("on the whole folder", 10, [] {
        return Tree()->focusPath().empty() && TopRows(Tree()) == "mac src README.md" &&
               !BarShown(Tree());
    });
    CloseRepoWindows("the window closes");

    // ---- several folders (Add to Focus)
    OpenRepoWindow("the folder opened again, for several folders");
    MCGuiWait("on the whole folder", 10, [] {
        return Tree()->focusPath().empty() && TopRows(Tree()) == "mac src README.md";
    });
    MCGuiStep("both commands in the File menu; src selected", [] {
        GMenuModel* bar = gtk_application_get_menubar(MCGuiApplication());
        GCHECK(MenuBarHas(bar, "win.addtofocus"));
        GCHECK(MenuBarHas(bar, "win.addfoldertofocus"));
        Tree()->revealPath(F("src"));
    });
    MCGuiWait("selected", 10, [] { return Tree()->selectedPath() == F("src"); });
    MCGuiWait("its row on screen", 10, [] { return RowLabel(Tree(), F("src")) != nullptr; });
    MCGuiStep("a right click on src's row", [] { MCGuiClick(RowLabel(Tree(), F("src")), -1, -1, 3); });
    MCGuiWait("its menu", 10, [] { return OpenMenu(Tree()) != nullptr; });
    MCGuiStep("a folder's right click offers Add to Focus, not Remove from Focus", [] {
        const std::vector<std::string> items = MenuLabels(OpenMenu(Tree()));
        GCHECK(Has(items, "Focus on Folder"));
        GCHECK(Has(items, "Add to Focus"));
        GCHECK(!Has(items, "Remove from Focus"));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return OpenMenu(Tree()) == nullptr; });
    MCGuiStep("Add to Focus from the menu bar takes the selected folder", [] { Run("addtofocus"); });
    MCGuiWait("one folder: src, with its breadcrumb", 10, [] {
        return Tree()->focusPath() == "src" && TopRows(Tree()) == "core.cpp";
    });
    MCGuiStep("the breadcrumb and its +; the chooser answers mac/src", [] {
        GCHECK(BarShown(Tree()));
        GCHECK(!Tree()->focusBarShowsFolders());
        GCHECK_EQ_STR(Labels(Tree()), "focusrepo › src");
        GCHECK(gtk_widget_get_visible(Tree()->focusBarAdd()));
        AnswerChooser(F("mac/src"));
        gChooserAsked = 0;
    });
    MCGuiClickWhenReady("the bar's +", [] { return Tree()->focusBarAdd(); });
    MCGuiWait("two roots in the order added, each open", 10, [] {
        return TopPaths(Tree()) == "src mac/src" && Expanded(Tree(), F("src")) &&
               Expanded(Tree(), F("mac/src")) && HasRow(Tree(), F("src/core.cpp")) &&
               HasRow(Tree(), F("mac/src/a.mm"));
    });
    MCGuiWait("the label row drawn", 10, [] {
        return MCGuiOnScreen(Tree()->focusBarPart(0)) && MCGuiOnScreen(Tree()->focusBarPart(1)) &&
               MCGuiOnScreen(Tree()->focusBarRemove(1)) && RowLabel(Tree(), F("mac/src")) != nullptr;
    });
    MCGuiStep("the chooser started in the window's folder; labels, rows and session say both", [] {
        MCGuiNote("asked %d, from %s; labels %s; session %s", gChooserAsked, gChooserStart.c_str(),
                  Labels(Tree()).c_str(), StoredFocus().c_str());
        GCHECK(gChooserAsked == 1);
        GCHECK_EQ_STR(gChooserStart, gRepo);
        GCHECK_EQ_STR(Tree()->focusPath(), "src, mac/src");
        GCHECK_EQ_STR(Tree()->root(), gRepo);
        GCHECK(Tree()->focusBarShowsFolders());
        GCHECK_EQ_STR(Labels(Tree()), "src › mac/src");
        GCHECK(gtk_widget_get_visible(Tree()->focusBarAdd()));
        // A root row says its path from the window's folder.
        GCHECK_EQ_STR(gtk_label_get_text(GTK_LABEL(RowLabel(Tree(), F("mac/src")))), "mac/src");
        GCHECK_EQ_STR(StoredFocus(), "src, mac/src");
        // New File's folder, Copy Path's: the first root.
        GCHECK_EQ_STR(Tree()->top(), F("src"));
        GCHECK(Enabled("showwholefolder"));
        MCGuiSnapshot(Tree()->container(), "tree_focus-roots");
        MCGuiWriteFile(F("mac/src/b.txt"), "b\n");
    });
    MCGuiWait("a file made in a root shows", 10, [] { return HasRow(Tree(), F("mac/src/b.txt")); });
    MCGuiStep("a folder already shown adds nothing; a reveal outside every root adds its folder", [] {
        GCHECK(Tree()->addFocus("mac/src"));
        GCHECK(Tree()->addFocus("src"));
        GCHECK_EQ_STR(Tree()->focusPath(), "src, mac/src");
        Tree()->revealPath(F("mac/checks/t.mm"), true);
        GCHECK_EQ_STR(Tree()->focusPath(), "src, mac/src, mac/checks");
    });
    MCGuiWait("t.mm selected under its new root", 10, [] {
        return TopPaths(Tree()) == "src mac/src mac/checks" &&
               Tree()->selectedPath() == F("mac/checks/t.mm");
    });
    MCGuiWait("three labels drawn", 10, [] {
        return MCGuiOnScreen(Tree()->focusBarPart(2)) && MCGuiOnScreen(Tree()->focusBarRemove(2));
    });
    MCGuiStep("the full paths, or the core's short labels where they do not fit, inside the bar", [] {
        MCGuiSnapshot(Tree()->container(), "tree_focus-three");
        const std::string labels = Labels(Tree());
        MCGuiNote("labels %s", labels.c_str());
        std::string brief;
        for (const std::string& l : TreeFocus::shortLabels({"src", "mac/src", "mac/checks"}))
            brief += (brief.empty() ? "" : " › ") + l;
        GCHECK(labels == "src › mac/src › mac/checks" || labels == brief);
        graphene_rect_t bar, last;
        GCHECK(gtk_widget_compute_bounds(Tree()->focusBar(), Tree()->container(), &bar));
        GCHECK(gtk_widget_compute_bounds(Tree()->focusBarRemove(2), Tree()->container(), &last));
        GCHECK(last.origin.x + last.size.width <= bar.origin.x + bar.size.width + 0.5);
    });
    MCGuiWait("mac/checks's row on screen", 10, [] {
        return RowLabel(Tree(), F("mac/checks")) != nullptr;
    });
    MCGuiStep("a right click on the mac/checks root row", [] {
        MCGuiClick(RowLabel(Tree(), F("mac/checks")), -1, -1, 3);
    });
    MCGuiWait("its menu", 10, [] { return OpenMenu(Tree()) != nullptr; });
    MCGuiStep("a root row offers Remove from Focus, not Add to Focus", [] {
        const std::vector<std::string> items = MenuLabels(OpenMenu(Tree()));
        GCHECK(Has(items, "Remove from Focus"));
        GCHECK(!Has(items, "Add to Focus"));
        GCHECK(Has(items, "Show Whole Folder in Tree"));
    });
    MCGuiClickWhenReady("its Remove from Focus", [] {
        return MenuButton(OpenMenu(Tree()), "Remove from Focus");
    });
    MCGuiWait("mac/checks taken out", 10, [] {
        return Tree()->focusPath() == "src, mac/src" && TopPaths(Tree()) == "src mac/src";
    });
    MCGuiWait("the labels drawn", 10, [] {
        return MCGuiOnScreen(Tree()->focusBarPart(1)) && Tree()->focusBarLabels().size() == 2;
    });
    MCGuiClickWhenReady("mac/src's label", [] { return Tree()->focusBarPart(1); });
    MCGuiWait("its root row selected", 10, [] { return Tree()->selectedPath() == F("mac/src"); });
    MCGuiClickWhenReady("src's ×", [] { return Tree()->focusBarRemove(0); });
    MCGuiWait("one folder left: mac/src, with its breadcrumb", 10, [] {
        return Tree()->focusPath() == "mac/src" && !Tree()->focusBarShowsFolders() &&
               TopRows(Tree()) == "a.mm b.txt new.txt";
    });
    MCGuiStep("the breadcrumb again; the chooser answers a folder outside the window's", [] {
        GCHECK_EQ_STR(Labels(Tree()), "focusrepo › mac › src");
        GCHECK_EQ_STR(StoredFocus(), "mac/src");
        AnswerChooser(MCGuiScratch());
        Run("addfoldertofocus");
    });
    MCGuiWait("refused, with a note", 10, [] {
        return StatusText().find("Only folders inside") != std::string::npos;
    });
    MCGuiStep("the chooser answers the window's folder itself", [] {
        MCGuiNote("status: %s", StatusText().c_str());
        GCHECK_EQ_STR(Tree()->focusPath(), "mac/src");
        AnswerChooser(gRepo);
        gChooserAsked = 0;
        Run("addfoldertofocus");
        GCHECK(gChooserAsked == 1);
    });
    // The answer comes from an idle, run before the next step.
    MCGuiStep("refused too: nothing changed", [] {
        GCHECK_EQ_STR(Tree()->focusPath(), "mac/src");
        GCHECK(StatusText().find("Only folders inside") != std::string::npos);
    });
    MCGuiStep("a.mm opened and selected: nothing new to add", [] {
        MCGuiOpenFileKept(F("mac/src/a.mm"));
        Tree()->revealPath(F("mac/src/a.mm"));
    });
    MCGuiWait("selected", 10, [] {
        return Win().editor->currentPath() == F("mac/src/a.mm") &&
               Tree()->selectedPath() == F("mac/src/a.mm");
    });
    MCGuiStep("Add to Focus asks the chooser instead, which gives src", [] {
        AnswerChooser(F("src"));
        gChooserAsked = 0;
        Run("addtofocus");
        GCHECK(gChooserAsked == 1);
    });
    MCGuiWait("mac/src and src", 10, [] {
        return Tree()->focusPath() == "mac/src, src" && TopPaths(Tree()) == "mac/src src";
    });
    MCGuiClickWhenReady("the last ×", [] { return Tree()->focusBarClose(); });
    MCGuiWait("the whole folder", 10, [] {
        return Tree()->focusPath().empty() && TopRows(Tree()) == "mac src README.md" &&
               !BarShown(Tree());
    });

    // ---- several brought back
    MCGuiStep("mac/src and src again: the window's record keeps both", [] {
        GCHECK(Tree()->setFocusFolders({"mac/src", "src"}));
        gRecord = MCGuiWindowRecord(Win().window);
        std::vector<TabSessions::Window> w = TabSessions::decodeWindows(gRecord);
        GCHECK(w.size() == 1 && w[0].tabs.treeFocus == (std::vector<std::string>{"mac/src", "src"}));
        if (w.empty()) return;
        // One gone since, between them.
        w[0].tabs.treeFocus = {"mac/src", "mac/gone", "src"};
        MCGuiOpenWindowRestoring(gRepo, TabSessions::encodeWindows(w));
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("a window brought back with the two still there", 10, [] {
        FileTree* back = MCGuiApp().tree;
        return MCGuiAppWindows().size() == gWindowsBefore + 2 && back->focusPath() == "mac/src, src" &&
               TopPaths(back) == "mac/src src" && back->focusBarShowsFolders() && BarShown(back);
    });
    MCGuiStep("that window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("closed", 10, [] { return MCGuiAppWindows().size() == gWindowsBefore + 1; });
    CloseRepoWindows("the window closes");
    MCGuiStep("the folder's session keeps both", [] { GCHECK_EQ_STR(StoredFocus(), "mac/src, src"); });
    OpenRepoWindow("the folder opened again");
    MCGuiWait("both roots, as they were", 10, [] {
        return Tree()->focusPath() == "mac/src, src" && TopPaths(Tree()) == "mac/src src" &&
               Tree()->focusBarShowsFolders() && BarShown(Tree());
    });

    // ---- one of several gone from the disk
    MCGuiStep("src deleted on disk", [] { std::filesystem::remove_all(F("src")); });
    MCGuiWait("it drops out alone", 10, [] {
        return Tree()->focusPath() == "mac/src" && TopRows(Tree()) == "a.mm b.txt new.txt";
    });
    MCGuiStep("the breadcrumb again, the status bar says why, the session keeps mac/src", [] {
        GCHECK(BarShown(Tree()));
        GCHECK_EQ_STR(Labels(Tree()), "focusrepo › mac › src");
        MCGuiNote("status: %s", StatusText().c_str());
        GCHECK(StatusText().find("A focused folder is gone") != std::string::npos);
        GCHECK_EQ_STR(StoredFocus(), "mac/src");
        MCGuiAnswerFolderChooser(nullptr);
    });
    CloseRepoWindows("the window closes");

    // ---- the tree follows the active tab (explorer.auto-reveal)
    MCGuiStep("a folder of 6,000 files: a_big and z_big, m_deep/a/b/c/d/e between", [] {
        gRepo = MCGuiScratch() + "/revealrepo";
        for (const char* big : {"a_big", "z_big"}) {
            std::filesystem::create_directories(F(big));
            for (int i = 0; i < kBigFiles; i++) {
                char name[32];
                snprintf(name, sizeof name, "/f%05d.txt", i);
                if (FILE* f = fopen((F(big) + name).c_str(), "w")) fclose(f);
            }
        }
        MCGuiWriteFile(F(kTarget), "target\n");
        MCGuiWriteFile(F(kNear), "near\n");
        MCGuiWriteFile(F("readme.txt"), "readme\n");
        gSettingsBefore = MCGuiReadFile(g_getenv("MUTIBLE_SETTINGS"));
    });
    OpenRepoWindow("a window on it");
    MCGuiWait("its rows", 10, [] { return TopRows(Tree()) == "a_big m_deep z_big readme.txt"; });
    MCGuiStep("a_big and z_big open", [] {
        for (const char* big : {"a_big", "z_big"})
            SetExpanded(Tree(), F(big), true);
    });
    MCGuiWait("their rows", 20, [] { return g_list_model_get_n_items(Rows(Tree())) == 2 * kBigFiles + 4; });
    MCGuiStep("m_deep never listed; readme.txt opened first", [] {
        GCHECK(!Expanded(Tree(), F("m_deep")));
        GCHECK(gtk_adjustment_get_page_size(Adj(Tree())) > 100);
        MCGuiOpenFileKept(F("readme.txt"));
        MCGuiOpenFileKept(F("a_big/f00001.txt"));
        Win().editor->focusActiveDocument();
    });
    MCGuiWait("a_big/f00001.txt selected, in view", 10, [] {
        return Tree()->selectedPath() == F("a_big/f00001.txt") && WhollyInView(Tree(), F("a_big/f00001.txt"));
    });
    MCGuiStep("a deep file opened", [] {
        GCHECK(EditorHasKeyboard());
        MCGuiOpenFileKept(F(kTarget));
    });
    MCGuiWait("its folders open, its row selected and centered", 10, [] {
        return Tree()->selectedPath() == F(kTarget) && Expanded(Tree(), F("m_deep/a/b/c/d/e")) &&
               RowsFromMiddle(Tree(), F(kTarget)) <= 1;
    });
    MCGuiStep("the editor kept the keyboard; nothing else closed", [] {
        MCGuiNote("rows from the middle: %.2f, the tree at %.0f", RowsFromMiddle(Tree(), F(kTarget)),
                  TreeTop(Tree()));
        GCHECK_EQ_STR(Win().editor->currentPath(), F(kTarget));
        GCHECK(EditorHasKeyboard());
        GCHECK(Expanded(Tree(), F("a_big")) && Expanded(Tree(), F("z_big")));
        MCGuiSnapshot(Tree()->widget(), "tree-follow-centered");
    });
    MCGuiStep("another tab", [] { Activate(F("a_big/f00001.txt")); });
    MCGuiWait("the selection follows and the row shows", 10, [] {
        return Tree()->selectedPath() == F("a_big/f00001.txt") && WhollyInView(Tree(), F("a_big/f00001.txt"));
    });
    MCGuiStep("back again", [] {
        GCHECK(!WhollyInView(Tree(), F(kTarget)));
        Activate(F(kTarget));
    });
    MCGuiWait("centered again", 10, [] {
        return Tree()->selectedPath() == F(kTarget) && RowsFromMiddle(Tree(), F(kTarget)) <= 1;
    });
    MCGuiStep("a row in view: near.txt opened", [] {
        GCHECK(EditorHasKeyboard());
        GCHECK(WhollyInView(Tree(), F(kNear)));
        gTop = TreeTop(Tree());
        MCGuiOpenFileKept(F(kNear));
    });
    MCGuiWait("selected", 10, [] { return Tree()->selectedPath() == F(kNear); });
    // The scroll, if any, comes a frame later: a pause for a negative check.
    MCGuiPause(0.3);
    MCGuiStep("the tree did not move; back to the target", [] {
        GCHECK(Near(TreeTop(Tree()), gTop));
        Activate(F(kTarget));
    });
    MCGuiWait("selected", 10, [] { return Tree()->selectedPath() == F(kTarget); });
    MCGuiPause(0.3);   // a negative check, as above
    MCGuiStep("the tree did not move; the editor kept the keyboard", [] {
        GCHECK(Near(TreeTop(Tree()), gTop));
        GCHECK(EditorHasKeyboard());
    });

    MCGuiStep("z_big/f00040.txt's row cut by the bottom of the tree", [] {
        const double h = RowHeight(Tree());
        const double top = RowTopFrom(Tree(), F(kTarget), F(kCut));
        GCHECK(h > 0);
        // Two thirds of it on show, its label's top among them.
        gtk_adjustment_set_value(Adj(Tree()), top + 2 * h / 3 - gtk_adjustment_get_page_size(Adj(Tree())));
    });
    MCGuiWait("on screen, cut", 10, [] {
        return RowWidget(Tree(), F(kCut)) && !WhollyInView(Tree(), F(kCut)) && RowLabel(Tree(), F(kCut));
    });
    MCGuiStep("a click on it", [] {
        gTop = TreeTop(Tree());
        double y = 0, h = 0;
        RowRect(Tree(), F(kCut), &y, &h);
        MCGuiNote("the row at %.1f, %.1f tall, the tree %.1f tall", y, h,
                  gtk_adjustment_get_page_size(Adj(Tree())));
        MCGuiClick(RowLabel(Tree(), F(kCut)), -1, 3);
    });
    MCGuiWait("it opens", 10, [] {
        return Win().editor->currentPath() == F(kCut) && Tree()->selectedPath() == F(kCut);
    });
    MCGuiPause(0.3);   // a negative check: no scroll a frame later
    MCGuiStep("the tree was not scrolled to the row clicked", [] {
        // GtkListView itself brings the row a click focuses wholly into
        // view (here the third of it under the edge); the tree following
        // the tab adds nothing, where centering it would move it half the
        // tree's height.
        MCGuiNote("the tree at %.0f, was %.0f; a row is %.0f", TreeTop(Tree()), gTop, RowHeight(Tree()));
        GCHECK(std::fabs(TreeTop(Tree()) - gTop) < RowHeight(Tree()) / 2);
        GCHECK(WhollyInView(Tree(), F(kCut)));
        GCHECK(RowsFromMiddle(Tree(), F(kCut)) > 3);
    });

    MCGuiStep("focused on z_big", [] { GCHECK(Tree()->setFocus("z_big")); });
    MCGuiWait("its rows, z_big/f00040.txt still selected and on show", 10, [] {
        return g_list_model_get_n_items(Rows(Tree())) == kBigFiles &&
               Tree()->selectedPath() == F(kCut) && WhollyInView(Tree(), F(kCut));
    });
    MCGuiStep("the target's tab", [] {
        Win().editor->focusActiveDocument();
        gSelected = Tree()->selectedPath();
        gTop = TreeTop(Tree());
        Activate(F(kTarget));
    });
    MCGuiPause(0.3);   // a negative check: nothing moves
    MCGuiStep("a file outside the focused folder leaves the tree as it is", [] {
        GCHECK_EQ_STR(Win().editor->currentPath(), F(kTarget));
        GCHECK_EQ_STR(Tree()->focusPath(), "z_big");   // never widened
        GCHECK_EQ_STR(Tree()->selectedPath(), gSelected);
        GCHECK(Near(TreeTop(Tree()), gTop));
        GCHECK(EditorHasKeyboard());
        MCGuiOpenFileKept(F("z_big/f02000.txt"));   // inside it, the tree follows
    });
    MCGuiWait("z_big/f02000.txt selected, centered", 10, [] {
        return Tree()->selectedPath() == F("z_big/f02000.txt") &&
               RowsFromMiddle(Tree(), F("z_big/f02000.txt")) <= 1;
    });
    MCGuiStep("the whole folder again", [] { GCHECK(Tree()->setFocus("")); });
    // The folders open before the focus open again from an idle as their
    // rows come (FileTree::tryExpand), after the top rows are in: closed
    // before that idle, m_deep was not open yet, and it and its folders
    // opened after the close (1 run in 5 beside hints_panel).
    MCGuiWait("its rows, the folders open before open again", 10, [] {
        return TopRows(Tree()) == "a_big m_deep z_big readme.txt" && Expanded(Tree(), F("a_big")) &&
               Expanded(Tree(), F("z_big")) && Expanded(Tree(), F("m_deep/a/b/c/d/e"));
    });

    MCGuiStep("explorer.auto-reveal = off, m_deep closed, near.txt's tab", [] {
        MCGuiSetSettings(gSettingsBefore + "\nexplorer.auto-reveal = off\n");
        SetExpanded(Tree(), F("m_deep"), false);
        GCHECK(!Expanded(Tree(), F("m_deep")) && !HasRow(Tree(), F(kNear)));   // a real close
        Win().editor->focusActiveDocument();
        Activate(F(kNear));
    });
    MCGuiPause(0.3);   // a negative check: nothing opens
    MCGuiStep("nothing moved", [] {
        GCHECK_EQ_STR(Win().editor->currentPath(), F(kNear));
        GCHECK(!Expanded(Tree(), F("m_deep")));
        GCHECK(!HasRow(Tree(), F(kNear)));
        MCGuiSetSettings(gSettingsBefore);
    });
    MCGuiWait("turned on, the file shows, centered", 10, [] {
        return Expanded(Tree(), F("m_deep/a/b/c/d/e")) && Tree()->selectedPath() == F(kNear) &&
               RowsFromMiddle(Tree(), F(kNear)) <= 1;
    });
    MCGuiStep("the editor kept the keyboard", [] { GCHECK(EditorHasKeyboard()); });

    MCGuiStep("the tree at its top, m_deep closed, the sidebar closed, the target's tab", [] {
        SetExpanded(Tree(), F("m_deep"), false);
        gtk_adjustment_set_value(Adj(Tree()), 0);
        Run("togglesidebar");
        GCHECK(!Win().layout->sidebarShown());
        Activate(F(kTarget));
    });
    MCGuiPause(0.3);   // a negative check: nothing opens while hidden
    MCGuiStep("nothing moved; the sidebar opened", [] {
        GCHECK(!Expanded(Tree(), F("m_deep")));
        GCHECK(Near(TreeTop(Tree()), 0));
        Run("togglesidebar");
        GCHECK(Win().layout->sidebarShown());
    });
    MCGuiWait("the file shows, centered (it was out of view)", 10, [] {
        return Expanded(Tree(), F("m_deep/a/b/c/d/e")) && Tree()->selectedPath() == F(kTarget) &&
               RowsFromMiddle(Tree(), F(kTarget)) <= 1;
    });
    MCGuiStep("the editor kept the keyboard", [] {
        MCGuiNote("open %d, selected %s, rows from the middle %.2f", Expanded(Tree(), F("m_deep/a/b/c/d/e")),
                  Tree()->selectedPath().c_str(), RowsFromMiddle(Tree(), F(kTarget)));
        GCHECK(EditorHasKeyboard());
    });

    // ---- brought back: the record's tree, not moved to the file on show
    MCGuiStep("the tree scrolled into a_big, the target out of view: the window's record", [] {
        gtk_adjustment_set_value(Adj(Tree()), 50 * RowHeight(Tree()));
    });
    MCGuiWait("scrolled", 10, [] { return !RowWidget(Tree(), F(kTarget)) || !WhollyInView(Tree(), F(kTarget)); });
    MCGuiStep("the record keeps the open folders, the selection and the scroll", [] {
        gTop = TreeTop(Tree());
        gRecord = MCGuiWindowRecord(Win().window);
        const std::vector<TabSessions::Window> w = TabSessions::decodeWindows(gRecord);
        GCHECK(w.size() == 1);
        if (w.size() != 1) return;
        MCGuiNote("the record: scroll %.0f, %zu folders open, %s selected", w[0].treeScroll,
                  w[0].expanded.size(), w[0].selected.c_str());
        GCHECK(Near(w[0].treeScroll, gTop));
        GCHECK(Has(w[0].expanded, F("a_big")) && Has(w[0].expanded, F("m_deep/a/b/c/d/e")));
        GCHECK_EQ_STR(w[0].selected, F(kTarget));
        const EditorTabs::Session tabs = TabSessions::files(w[0].tabs);
        GCHECK(tabs.active >= 0 && tabs.files[(size_t)tabs.active] == F(kTarget));
        MCGuiOpenWindowRestoring(gRepo, gRecord);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiWait("a window brought back from it, its tree as it was", 20, [] {
        FileTree* back = MCGuiApp().tree;
        return MCGuiAppWindows().size() == gWindowsBefore + 2 && back->selectedPath() == F(kTarget) &&
               Expanded(back, F("m_deep/a/b/c/d/e")) && Near(TreeTop(back), gTop);
    });
    MCGuiPause(0.5);   // a negative check: no reveal jumps it afterwards
    MCGuiStep("its scroll kept: the target's tab on show, its row not centered", [] {
        FileTree* back = MCGuiApp().tree;
        MCGuiNote("the tree at %.0f, the record's %.0f", TreeTop(back), gTop);
        GCHECK_EQ_STR(MCGuiApp().editor->currentPath(), F(kTarget));
        GCHECK(Near(TreeTop(back), gTop));
        GCHECK(!RowWidget(back, F(kTarget)) || RowsFromMiddle(back, F(kTarget)) > 1);
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("closed", 10, [] { return MCGuiAppWindows().size() == gWindowsBefore + 1; });
    CloseRepoWindows("the folder of 6,000 files closes");
    MCGuiStep("its files go", [] { std::filesystem::remove_all(gRepo); });
}
