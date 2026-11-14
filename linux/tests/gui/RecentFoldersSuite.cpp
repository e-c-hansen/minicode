// RecentFoldersSuite.cpp — recent folders and the dock icon's menu
// (`./build/minicode-guitest recent_folders`, linux/HANDOFF.md item 69; the
// Mac's File > Open Recent).
// Everything is in a scratch world: $XDG_STATE_HOME, $XDG_DATA_HOME and
// $XDG_DATA_DIRS (holding a fake installed desktop file) are set to folders
// under the scratch root for the run and put back, so the owner's real
// ~/.local/share/applications and state are never read or written, and the
// folder chooser is answered by the check (MCGuiAnswerFolderChooser).
//   - the list: newest first, one entry a folder by its resolved path (a
//     symlink to a folder is that folder), 20 at most, folders that are gone
//     left out, nothing noted under MINICODE_BENCH;
//   - File > Open Recent: the submenu lists them and ends with Clear
//     Recently Opened; an item, and Ctrl+R's pick, bring forward the window
//     on the folder, else move the window in front to it (asking about
//     unsaved tabs, Cancel leaving it), and a folder that is gone changes
//     nothing (the Mac's rule, HANDOFF.md item 71);
//   - the dock's menu: the generated desktop file's exact text (a folder
//     with a space quoted, a hostile name quoted per the spec), kept
//     current as the list changes, a file without the marker left alone,
//     window.dock-menu = false removing the copy, no installed file and
//     nothing written;
//   - `--open-folder` through the command line: the chooser's folder gets a
//     window (and no window for the current folder), a cancel gets nothing;
//   - `--new-window`: always a window, on a path, on the front window's
//     folder or with a file, and the dock's New Window action runs it.
#include "Harness.h"

#include "DockMenu.h"
#include "Editor.h"
#include "FileTree.h"
#include "PalettePanel.h"
#include "RecentList.h"

#include <glib/gstdio.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Saved { const char* name; std::string value; bool had; };
std::vector<Saved> gSaved;
std::string gBase, gDesktop, gSys;
size_t gWindows = 0;

std::string rp(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}
void setEnv(const char* name, const std::string& value) {
    const char* old = getenv(name);
    bool seen = false;
    for (const Saved& s : gSaved) seen = seen || std::string(s.name) == name;
    if (!seen) gSaved.push_back({name, old ? old : "", old != nullptr});
    setenv(name, value.c_str(), 1);
}
std::string dir(const std::string& name) {
    const std::string p = gBase + "/" + name;
    g_mkdir_with_parents(p.c_str(), 0755);
    return rp(p);
}
std::string home() { return g_get_home_dir(); }
std::string tilde(const std::string& p) { return RecentList::tilded(p, home()); }

// The labels of a menu model, a submenu as "Folder" and its rows as
// "Folder/Row"; sections are flattened.
void walk(GMenuModel* m, const std::string& pre, std::vector<std::string>* out) {
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        char* label = nullptr;
        g_menu_model_get_item_attribute(m, i, "label", "s", &label);
        GMenuModel* sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SUBMENU);
        GMenuModel* sec = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION);
        if (sub) {
            out->push_back(pre + label);
            walk(sub, pre + label + "/", out);
            g_object_unref(sub);
        } else if (sec) {
            walk(sec, pre, out);
            g_object_unref(sec);
        } else if (label) {
            out->push_back(pre + label);
        }
        g_free(label);
    }
}
std::vector<std::string> labels(GMenuModel* m) {
    std::vector<std::string> out;
    if (m) walk(m, "", &out);
    return out;
}
bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

const char* kInstalled =
    "[Desktop Entry]\n"
    "Type=Application\n"
    "Name=MiniCode\n"
    "Exec=/opt/mc/bin/minicode %f\n"
    "Icon=org.minicode.Editor\n"
    "StartupWMClass=org.minicode.Editor\n";

std::string expected(const std::vector<std::string>& folders) {
    std::string actions = "Actions=open-folder;new-window;";
    std::string tail;
    int n = 0;
    for (const std::string& f : folders) {
        n++;
        actions += "recent-" + std::to_string(n) + ";";
        tail += "\n[Desktop Action recent-" + std::to_string(n) + "]\nName=" + tilde(f) +
                "\nExec=/opt/mc/bin/minicode \"" + f + "\"\n";
    }
    return std::string(kInstalled) + actions + "\nX-Minicode-Generated=true\n\n"
           "[Desktop Action open-folder]\nName=Open Folder…\nExec=/opt/mc/bin/minicode --open-folder\n\n"
           "[Desktop Action new-window]\nName=New Window\nExec=/opt/mc/bin/minicode --new-window\n" + tail;
}

void closeExtraWindows() {
    const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
    for (size_t i = gWindows; i < all.size(); i++) gtk_window_close(all[i].window);
}

// The question about unsaved tabs (a GtkAlertDialog window of its own).
bool questionUp() {
    GtkWindow* top = MCGuiTopWindow();
    if (top == MCGuiApp().window) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), "Cancel");
    return b && gtk_widget_get_width(b) > 0 && gtk_widget_get_height(b) > 0;
}
bool questionGone() { return MCGuiTopWindow() == MCGuiApp().window; }
void answer(const std::string& button) {
    MCGuiSetKeyWindow(MCGuiTopWindow());
    MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), button));
}
void openRecent(const std::string& folder) {
    g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "openrecentfolder",
                                   g_variant_new_string(folder.c_str()));
}
// The newest window has the keyboard, so it is the one in front.
void front() {
    MCGuiSetKeyWindow(nullptr);
    MCGuiFocusKeyWindow();
}
bool newestOn(const std::string& folder) {
    return MCGuiApp().rootDir == folder && MCGuiApp().tree && MCGuiApp().tree->root() == folder;
}
std::string gX, gY, gLater;
int gWant = 0;

}  // namespace

MC_GUI_SUITE(recent_folders) {
    MCGuiStep("a scratch world: state, data and a fake installed desktop file", [] {
        gBase = rp(MCGuiScratch()) + "/recent";
        gDesktop = gBase + "/data/applications/org.minicode.Editor.desktop";
        gSys = gBase + "/sys/applications/org.minicode.Editor.desktop";
        setEnv("XDG_STATE_HOME", gBase + "/state");
        setEnv("XDG_DATA_HOME", gBase + "/data");
        setEnv("XDG_DATA_DIRS", gBase + "/sys");
        setEnv("MINICODE_BENCH", "");
        unsetenv("MINICODE_BENCH");
        MCGuiWriteFile(gSys, kInstalled);
        MCGuiSetSettings("lsp.enabled = false\n");
        gWindows = MCGuiAppWindows().size();
        RecentList::clear();
        GCHECK(RecentList::file() == gBase + "/state/minicode/recent-folders.json");
        GCHECK(RecentList::folders().empty());
    });

    // ---- the list
    MCGuiStep("newest first, a folder once, a symlink is its folder", [] {
        const std::string a = dir("a"), b = dir("b"), c = dir("c");
        RecentList::note(a);
        RecentList::note(b);
        RecentList::note(c);
        GCHECK((RecentList::folders() == std::vector<std::string>{c, b, a}));
        RecentList::note(a);
        GCHECK((RecentList::folders() == std::vector<std::string>{a, c, b}));
        // The same folder spelled through a link, and with a trailing slash.
        g_remove((gBase + "/link").c_str());
        GCHECK(symlink(b.c_str(), (gBase + "/link").c_str()) == 0);
        RecentList::note(gBase + "/link");
        GCHECK((RecentList::folders() == std::vector<std::string>{b, a, c}));
        RecentList::note(c + "/");
        GCHECK((RecentList::folders() == std::vector<std::string>{c, b, a}));
        GCHECK(MCGuiReadFile(RecentList::file()).find("\"" + c + "\"") == 1);
    });
    MCGuiStep("20 at most", [] {
        std::string last, dropped;
        for (int i = 1; i <= 25; i++) {
            last = dir("n" + std::to_string(i));
            if (i == 5) dropped = last;
            RecentList::note(last);
        }
        const std::vector<std::string> l = RecentList::folders();
        GCHECK(l.size() == 20);
        GCHECK(!l.empty() && l[0] == last);
        GCHECK(!has(l, dropped));
        GCHECK(has(l, dir("n6")) && !has(l, dir("n1")) && !has(l, dir("a")));
    });
    MCGuiStep("a folder that is gone is left out", [] {
        const std::string gone = dir("gone");
        RecentList::note(gone);
        GCHECK(RecentList::folders()[0] == gone);
        g_rmdir(gone.c_str());
        const std::vector<std::string> l = RecentList::folders();
        GCHECK(!has(l, gone) && l.size() == 19);   // 20 kept in the file, one gone
        // A file is no folder either.
        MCGuiWriteFile(gBase + "/file.txt", "x\n");
        RecentList::note(gBase + "/file.txt");
        GCHECK(!has(RecentList::folders(), gBase + "/file.txt"));
    });
    MCGuiStep("nothing is noted under MINICODE_BENCH", [] {
        const std::vector<std::string> before = RecentList::folders();
        setenv("MINICODE_BENCH", "1", 1);
        RecentList::note(dir("bench"));
        GCHECK(RecentList::folders().empty());   // and none listed
        unsetenv("MINICODE_BENCH");
        GCHECK(RecentList::folders() == before);   // the file was not written
    });

    // ---- the dock's menu
    MCGuiStep("quoting follows the Desktop Entry spec", [] {
        GCHECK_EQ_STR(DockMenu::execArg("/a b"), "\"/a b\"");
        GCHECK_EQ_STR(DockMenu::execArg("a b\"c$d`e%f\\g"),
                      "\"a b\\\\\"c\\\\$d\\\\`e%%f\\\\\\\\g\"");
        // A name with a line break cannot be written, so it is left out.
        const std::string text = DockMenu::render(kInstalled, {"/x\ny", "/ok"}, "/home/u");
        GCHECK(text.find("recent-2") == std::string::npos && text.find("Name=/ok") != std::string::npos);
        GCHECK(DockMenu::render("[Desktop Entry]\nName=x\n", {"/ok"}, "/home/u").empty());
    });
    MCGuiStep("the desktop file is written with the list, a folder with a space quoted", [] {
        RecentList::clear();
        RecentList::note(dir("a"));
        RecentList::note(dir("sp ace"));
        GCHECK(DockMenu::installedFile() == gSys);
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), expected({dir("sp ace"), dir("a")}));
        GCHECK(MCGuiReadFile(gDesktop).find("Exec=/opt/mc/bin/minicode \"" + dir("sp ace") + "\"\n") !=
               std::string::npos);
    });
    MCGuiStep("it follows the list, ten folders at most, Clear empties the recents", [] {
        for (int i = 1; i <= 12; i++) RecentList::note(dir("n" + std::to_string(i)));
        const std::string text = MCGuiReadFile(gDesktop);
        GCHECK(text.find("[Desktop Action recent-10]") != std::string::npos);
        GCHECK(text.find("[Desktop Action recent-11]") == std::string::npos);
        GCHECK(text.find("Name=" + tilde(dir("n12")) + "\n") != std::string::npos);
        GCHECK(text.find("Actions=open-folder;new-window;recent-1;") == text.find("Actions="));
        RecentList::clear();
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), expected({}));
    });
    MCGuiStep("a file without the marker is the user's own and stays", [] {
        MCGuiWriteFile(gDesktop, "[Desktop Entry]\nName=Mine\nExec=/mine\n");
        RecentList::note(dir("a"));
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), "[Desktop Entry]\nName=Mine\nExec=/mine\n");
        MCGuiSetSettings("lsp.enabled = false\nwindow.dock-menu = false\n");
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), "[Desktop Entry]\nName=Mine\nExec=/mine\n");
        MCGuiSetSettings("lsp.enabled = false\n");
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), "[Desktop Entry]\nName=Mine\nExec=/mine\n");
        g_remove(gDesktop.c_str());
        RecentList::note(dir("b"));
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), expected({dir("b"), dir("a")}));
    });
    MCGuiStep("window.dock-menu = false removes the copy, true brings it back", [] {
        MCGuiSetSettings("lsp.enabled = false\nwindow.dock-menu = false\n");
        GCHECK(!MCGuiFileExists(gDesktop));
        RecentList::note(dir("c"));
        GCHECK(!MCGuiFileExists(gDesktop));
        MCGuiSetSettings("lsp.enabled = false\nwindow.dock-menu = true\n");
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), expected({dir("c"), dir("b"), dir("a")}));
    });
    MCGuiStep("with no installed desktop file nothing is written", [] {
        g_remove(gDesktop.c_str());
        setEnv("XDG_DATA_DIRS", gBase + "/nothing-here");
        GCHECK(DockMenu::installedFile().empty());
        RecentList::note(dir("a"));
        GCHECK(!MCGuiFileExists(gDesktop));
        setEnv("XDG_DATA_DIRS", gBase + "/sys");
        RecentList::note(dir("b"));
        GCHECK(MCGuiFileExists(gDesktop));
    });

    // ---- File > Open Recent
    MCGuiStep("the submenu lists the folders and ends with Clear", [] {
        RecentList::clear();
        RecentList::note(dir("a"));
        RecentList::note(dir("b"));
        const std::vector<std::string> l = labels(G_MENU_MODEL(MCGuiRecentMenu()));
        GCHECK((l == std::vector<std::string>{tilde(dir("b")), tilde(dir("a")), "Clear Recently Opened"}));
        const std::vector<std::string> bar =
            labels(gtk_application_get_menubar(MCGuiApplication()));
        GCHECK(has(bar, "File/Open Recent"));
        GCHECK(has(bar, "File/Open Recent/" + tilde(dir("b"))));
        GCHECK(has(bar, "File/Open Recent/Clear Recently Opened"));
        GCHECK(has(bar, "File/Open Recent…"));   // the registry's command, with the key
    });
    // The Mac's rule: the window on the folder comes forward, else the
    // window in front moves to it, asking about unsaved tabs first.
    MCGuiStep("a window on a folder of its own, in front", [] {
        gX = dir("x");
        gY = dir("y");
        MCGuiWriteFile(gY + "/note.txt", "note\n");
        MCGuiOpenWindow(gX);
    });
    MCGuiWait("it shows x", 20, [] { return MCGuiAppWindows().size() == gWindows + 1 && newestOn(gX); });
    MCGuiStep("Open Recent to y", [] {
        front();
        openRecent(gY);
    });
    MCGuiWait("the same window shows y", 20, [] { return newestOn(gY); });
    MCGuiStep("no window was made, and y is the newest recent folder", [] {
        GCHECK(MCGuiAppWindows().size() == gWindows + 1);
        GCHECK(RecentList::folders()[0] == gY);
    });
    MCGuiStep("a tab with unsaved text in it", [] {
        MCGuiOpenFileKept(gY + "/note.txt");
    });
    MCGuiWait("note.txt on show", 10, [] { return MCGuiApp().editor->currentPath() == gY + "/note.txt"; });
    MCGuiStep("typed into", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+Home");
        MCGuiType("edit ");
    });
    MCGuiWait("unsaved", 10, [] { return MCGuiApp().editor->dirty(); });
    MCGuiStep("Open Recent to x asks first", [] { openRecent(gX); });
    MCGuiWait("the question is up", 10, [] { return questionUp(); });
    MCGuiStep("Cancel", [] { answer("Cancel"); });
    MCGuiWait("the question is gone", 10, [] { return questionGone(); });
    MCGuiStep("the window stays on y with its edit", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(newestOn(gY));
        GCHECK(MCGuiAppWindows().size() == gWindows + 1);
        GCHECK(MCGuiApp().editor->dirty());
    });
    MCGuiStep("again, and Don't Save lets it go", [] {
        openRecent(gX);
    });
    MCGuiWait("the question is up again", 10, [] { return questionUp(); });
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("the window shows x", 20, [] { return newestOn(gX); });
    MCGuiStep("with no tab of y's, and the file unchanged", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(!MCGuiApp().editor->dirty());
        GCHECK_EQ_STR(MCGuiReadFile(gY + "/note.txt"), "note\n");
        GCHECK(MCGuiAppWindows().size() == gWindows + 1);
    });
    MCGuiStep("a second window, in front, on another folder", [] {
        gLater = dir("later");
        MCGuiOpenWindow(gLater);
    });
    MCGuiWait("it shows later", 20, [] { return MCGuiAppWindows().size() == gWindows + 2 && newestOn(gLater); });
    MCGuiStep("Open Recent to x, which the first window has", [] {
        front();
        openRecent(gX);
    });
    MCGuiPause(0.5);   // a negative check: neither window moves, no third opens
    MCGuiStep("it is that window that comes forward, and nothing else changes", [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        GCHECK(all.size() == gWindows + 2);
        GCHECK(all[gWindows].rootDir == gX && all[gWindows + 1].rootDir == gLater);
    });
    MCGuiStep("a folder that is gone changes nothing", [] {
        front();
        const std::string gone = dir("vanished");
        RecentList::note(gone);
        g_rmdir(gone.c_str());
        openRecent(gone);
    });
    MCGuiPause(0.5);   // a negative check: no window opens and none moves
    MCGuiStep("both windows as they were", [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        GCHECK(all.size() == gWindows + 2);
        GCHECK(all[gWindows].rootDir == gX && all[gWindows + 1].rootDir == gLater);
    });
    // The palette's pick is the same rule, for the window it was opened in.
    MCGuiStep("Ctrl+R lists them as a pick list", [] {
        front();
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+r");
    });
    MCGuiWait("the pick list is open", 10, [] { return MCGuiApp().palette && MCGuiApp().palette->isPick(); });
    MCGuiStep("with the folders as rows, y chosen", [] {
        const std::vector<std::string> l = MCGuiApp().palette->labels();
        GCHECK(has(l, tilde(gY)) && has(l, tilde(gX)) && has(l, tilde(gLater)));
        GCHECK(!has(l, tilde(dir("vanished"))));
        gWant = (int)(std::find(l.begin(), l.end(), tilde(gY)) - l.begin());
        for (int i = MCGuiApp().palette->selected(); i < gWant; i++) MCGuiKey("down");
    });
    MCGuiWait("y is selected", 10, [] { return MCGuiApp().palette->selected() == gWant; });
    MCGuiStep("and taken", [] { MCGuiKey("return"); });
    MCGuiWait("the window in front shows y", 20, [] { return newestOn(gY); });
    MCGuiStep("no window was made, and the first one stayed on x", [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        GCHECK(all.size() == gWindows + 2);
        GCHECK(all[gWindows].rootDir == gX);
        GCHECK(!MCGuiApp().palette->isOpen());
    });
    MCGuiStep("a pick of a folder another window has raises it", [] {
        front();
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+r");
    });
    MCGuiWait("the pick list is open again", 10, [] { return MCGuiApp().palette && MCGuiApp().palette->isPick(); });
    MCGuiStep("x chosen", [] {
        const std::vector<std::string> l = MCGuiApp().palette->labels();
        gWant = (int)(std::find(l.begin(), l.end(), tilde(gX)) - l.begin());
        for (int i = MCGuiApp().palette->selected(); i < gWant; i++) MCGuiKey("down");
    });
    MCGuiWait("x is selected", 10, [] { return MCGuiApp().palette->selected() == gWant; });
    MCGuiStep("and taken", [] { MCGuiKey("return"); });
    MCGuiPause(0.5);   // a negative check: the pick opens nothing and moves nothing
    MCGuiStep("both windows as they were", [] {
        const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
        GCHECK(all.size() == gWindows + 2);
        GCHECK(all[gWindows].rootDir == gX && all[gWindows + 1].rootDir == gY);
    });
    MCGuiStep("Clear Recently Opened empties the list and the menu", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApplication()), "clearrecent", nullptr);
        GCHECK(RecentList::folders().empty());
        GCHECK((labels(G_MENU_MODEL(MCGuiRecentMenu())) ==
                std::vector<std::string>{"No Recent Folders", "Clear Recently Opened"}));
        GCHECK_EQ_STR(MCGuiReadFile(gDesktop), expected({}));
    });

    // ---- minicode --open-folder
    MCGuiStep("--open-folder asks the chooser, which starts in the home folder", [] {
        closeExtraWindows();
    });
    MCGuiWait("the window from before is closed", 20, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the folder chosen gets a window", [] {
        MCGuiAnswerFolderChooser([](const std::string& start) {
            GCHECK_EQ_STR(start, home());
            return dir("chosen");
        });
        std::string errors;
        MCGuiCommandLine({"--open-folder"}, gBase, &errors);
        GCHECK(errors.empty());
        GCHECK(MCGuiAppWindows().size() == gWindows);   // not before it is answered, not for the cwd
    });
    MCGuiWait("a window shows it", 20, [] {
        return MCGuiAppWindows().size() == gWindows + 1 && MCGuiApp().rootDir == dir("chosen");
    });
    MCGuiStep("it is the newest recent folder", [] {
        GCHECK(RecentList::folders()[0] == dir("chosen"));
        // Asked again, the window that shows it comes forward.
        std::string errors;
        MCGuiCommandLine({"--open-folder"}, gBase, &errors);
    });
    MCGuiPause(0.5);   // a negative check: no second window on the folder
    MCGuiStep("a cancel opens nothing", [] {
        GCHECK(MCGuiAppWindows().size() == gWindows + 1);
        MCGuiAnswerFolderChooser([](const std::string&) { return std::string(); });
        std::string errors;
        MCGuiCommandLine({"--open-folder"}, gBase, &errors);
    });
    MCGuiPause(0.5);   // a negative check: the cancel is answered, nothing opens
    MCGuiStep("still the one window", [] {
        GCHECK(MCGuiAppWindows().size() == gWindows + 1);
        GCHECK(RecentList::folders()[0] == dir("chosen"));
    });

    // ---- minicode --new-window
    MCGuiStep("--new-window adds a window on the folder a window already shows", [] {
        closeExtraWindows();
    });
    MCGuiWait("the others are closed", 20, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("one window on the chosen folder, in front", [] {
        MCGuiOpenWindow(dir("chosen"));
    });
    MCGuiWait("it shows chosen", 20, [] { return MCGuiAppWindows().size() == gWindows + 1 && newestOn(dir("chosen")); });
    MCGuiStep("with the path of that folder", [] {
        front();
        std::string errors;
        MCGuiCommandLine({"--new-window", dir("chosen")}, gBase, &errors);
        GCHECK(errors.empty());
    });
    MCGuiWait("a second window on it", 20, [] {
        return MCGuiAppWindows().size() == gWindows + 2 && newestOn(dir("chosen"));
    });
    MCGuiStep("with no path: the front window's folder", [] {
        front();
        std::string errors;
        MCGuiCommandLine({"--new-window"}, gBase, &errors);
        GCHECK(errors.empty());
    });
    MCGuiWait("a third", 20, [] { return MCGuiAppWindows().size() == gWindows + 3 && newestOn(dir("chosen")); });
    MCGuiStep("with a file of that folder: a window of its own, the file in it", [] {
        MCGuiWriteFile(dir("chosen") + "/f.txt", "f\n");
        std::string errors;
        MCGuiCommandLine({dir("chosen") + "/f.txt", "--new-window"}, gBase, &errors);
        GCHECK(errors.empty());
    });
    MCGuiWait("a fourth, with the file", 20, [] {
        return MCGuiAppWindows().size() == gWindows + 4 &&
               MCGuiApp().editor->currentPath() == dir("chosen") + "/f.txt";
    });
    MCGuiStep("without the option the same path only raises", [] {
        std::string errors;
        MCGuiCommandLine({dir("chosen")}, gBase, &errors);
    });
    MCGuiPause(0.5);   // a negative check: no window is made
    MCGuiStep("still four", [] { GCHECK(MCGuiAppWindows().size() == gWindows + 4); });
    MCGuiStep("the dock's New Window runs it, also from an installed file's own action", [] {
        const std::string text = MCGuiReadFile(gDesktop);
        GCHECK(text.find("[Desktop Action new-window]\nName=New Window\nExec=/opt/mc/bin/minicode "
                         "--new-window\n") != std::string::npos);
        const std::string own = std::string(kInstalled) +
            "Actions=new-window;\n\n[Desktop Action new-window]\nName=Nouvelle\nExec=/opt/mc/bin/minicode\n";
        const std::string out = DockMenu::render(own, {}, "/home/u");
        GCHECK(out.find("Exec=/opt/mc/bin/minicode --new-window\n") != std::string::npos);
        GCHECK(out.find("Name=Nouvelle") == std::string::npos);
        GCHECK(out.find("[Desktop Action new-window]") == out.rfind("[Desktop Action new-window]"));
        GCHECK(out.find("Actions=open-folder;new-window;\n") != std::string::npos);
    });

    MCGuiStep("put away: the chooser, the windows, the environment, the keyboard", [] {
        MCGuiAnswerFolderChooser(nullptr);
        closeExtraWindows();
        for (const Saved& s : gSaved) {
            if (s.had) setenv(s.name, s.value.c_str(), 1);
            else unsetenv(s.name);
        }
        MCGuiSetSettings("lsp.enabled = false\n");
    });
    MCGuiWait("the windows it opened are closed", 20, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the keyboard goes back", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}
