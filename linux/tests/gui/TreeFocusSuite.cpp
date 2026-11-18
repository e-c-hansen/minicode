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
// The clicks on the breadcrumb, the × and the rows' right clicks are real
// (XTest); the command is the window's action, as the menu bar and the
// palette run it. The GTK port keeps no open folders in a window's record
// (the Mac does), so a window brought back opens none.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"
#include "Persistence.h"
#include "TabSessions.h"

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
bool Expanded(FileTree* t, const std::string& path) {
    GtkTreeListRow* r = RowObject(t, path);
    return r && gtk_tree_list_row_get_expanded(r);
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
    // A list since the Mac's Add to Focus; the GTK tree keeps one folder.
    return it->second.treeFocus.empty() ? std::string() : it->second.treeFocus.front();
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
}
