// BookmarkWorkspaceSuite.cpp — bookmarks per workspace
// (`./build/mutible-guitest bookmark_workspace`, HANDOFF item 111; the
// Mac's bookmarks suite since #206). Pages come from a server inside this
// process on 127.0.0.1; the shared folder is the scratch home's ~/Bookmarks,
// checked to be under MCGuiScratch() before anything is written, and the
// two workspaces are scratch folders, so nothing leaves the scratch world.
//   - Ctrl+D in a window on a folder writes the page to its
//     .mutible/bookmarks, made then with no .gitignore, and the status bar
//     says ".mutible/bookmarks"; the shared folder is untouched;
//   - the tree hides .mutible until hidden files are shown;
//   - the book and the menu bar's Bookmarks menu list the workspace's
//     bookmarks first, then "Shared" with ~/Bookmarks, whose folder and
//     each folder in it end with Move to This Workspace;
//   - "already bookmarked" finds the workspace's bookmark before a shared
//     one of the same page, and a page only in Shared counts as bookmarked;
//   - Move to This Workspace moves the file, by clicks through the book to
//     Shared > Reading > Move to This Workspace (its page, not Shared's of
//     the same label: no two submenus share a label), and by its item onto
//     a taken name (a number added); the menus follow;
//   - a second workspace lists none of the first's, and the menu bar's menu
//     follows the window in front;
//   - the window on ~/Bookmarks (`mutible https://...`) keeps the one
//     folder: no Shared part, and Ctrl+D saves in it.
#include "Harness.h"

#include "AppSeam.h"
#include "Bookmarks.h"
#include "Browser.h"
#include "BrowserPages.h"
#include "FileTree.h"
#include "WebBookmark.h"
#include "WindowLayout.h"

#include <ftw.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

GSocketService* gServer = nullptr;
int gPort = 0;
std::string gShared, gA, gB;          // ~/Bookmarks and the two workspaces
GtkWindow* gWinA = nullptr;
GtkWindow* gWinB = nullptr;
size_t gWindowsBefore = 0;
size_t gSharedCount = 0;
std::string gErrors;

// /alpha is "Alpha", /s1 "Shared One", any other /x "Page x".
gboolean onIncoming(GThreadedSocketService*, GSocketConnection* conn, GObject*, gpointer) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    gssize n = g_input_stream_read(in, buf, sizeof buf - 1, nullptr, nullptr);
    if (n <= 0) return TRUE;
    buf[n] = 0;
    const std::string req(buf);
    const size_t sp = req.find(' ');
    const std::string path = sp == std::string::npos ? "" : req.substr(sp + 1, req.find(' ', sp + 1) - sp - 1);
    std::string title = "Page " + (path.size() > 1 ? path.substr(1) : std::string("root"));
    if (path == "/alpha") title = "Alpha";
    else if (path == "/s1") title = "Shared One";
    const std::string body = "<title>" + title + "</title><p>" + title + "</p>";
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    const std::string all = head + body;
    g_output_stream_write_all(out, all.data(), all.size(), nullptr, nullptr, nullptr);
    g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
    return TRUE;
}

std::string url(const std::string& name) { return "http://127.0.0.1:" + std::to_string(gPort) + "/" + name; }
bool has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }

// Whether `path` is inside the scratch world: nothing is written or removed
// anywhere else.
bool scratch(const std::string& path) {
    const std::string root = MCGuiScratch() + "/";
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
           path.find("/../") == std::string::npos;
}
void writeBookmark(const std::string& path, const std::string& address) {
    GCHECK(scratch(path));
    if (scratch(path)) MCGuiWriteFile(path, WebBookmark::shortcutText(address));
}
// A scratch folder and everything in it.
void removeTree(const std::string& dir) {
    GCHECK(scratch(dir));
    if (!scratch(dir)) return;
    nftw(dir.c_str(), [](const char* p, const struct stat*, int, struct FTW*) { return g_remove(p); }, 16,
         FTW_DEPTH | FTW_PHYS);
}
size_t count(const std::string& dir) {
    size_t n = 0;
    if (GDir* d = g_dir_open(dir.c_str(), 0, nullptr)) {
        while (g_dir_read_name(d)) n++;
        g_dir_close(d);
    }
    return n;
}

// A window's parts by its GtkWindow, now.
MCGuiAppWindow win(GtkWindow* w) {
    for (const MCGuiAppWindow& a : MCGuiAppWindows())
        if (a.window == w) return a;
    return MCGuiAppWindow();
}
Browser* browserOf(GtkWindow* w) { return win(w).browser; }
std::string statusOf(GtkWindow* w) {
    GtkWidget* l = win(w).statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}
bool pageIs(GtkWindow* w, const std::string& title) {
    Browser* b = browserOf(w);
    return b && b->userProbe() && b->userProbe()->title() == title;
}

// A label as a person reads it: the zero-width spaces that keep two
// submenus' names apart (Bookmarks::menu) dropped.
std::string seen(const char* label) {
    std::string s = label ? label : "";
    for (size_t at; (at = s.find("\xE2\x80\x8B")) != std::string::npos;) s.erase(at, 3);
    return s;
}
// The labels of a menu model, a submenu as "Folder" and its rows as
// "Folder/Row"; sections are flattened (as the bookmark_menu suite reads).
// `raw` keeps every submenu's label as GTK names its page.
void walk(GMenuModel* m, const std::string& pre, std::vector<std::string>* out,
          std::vector<std::string>* raw = nullptr) {
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        char* label = nullptr;
        g_menu_model_get_item_attribute(m, i, "label", "s", &label);
        GMenuModel* sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SUBMENU);
        GMenuModel* sec = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION);
        if (sub) {
            out->push_back(pre + seen(label));
            if (raw) raw->push_back(label ? label : "");
            walk(sub, pre + seen(label) + "/", out, raw);
            g_object_unref(sub);
        } else if (sec) {
            walk(sec, pre, out, raw);
            g_object_unref(sec);
        } else if (label) {
            out->push_back(pre + seen(label));
        }
        g_free(label);
    }
}
std::vector<std::string> labels(GMenuModel* m) {
    std::vector<std::string> out;
    if (m) walk(m, "", &out);
    return out;
}
int indexOf(const std::vector<std::string>& v, const std::string& s) {
    const auto it = std::find(v.begin(), v.end(), s);
    return it == v.end() ? -1 : (int)(it - v.begin());
}
// The action and string target of the item at `path` ("Shared/Reading/Move
// to This Workspace/Deep") in a menu model.
bool itemAt(GMenuModel* m, const std::string& path, std::string* action, std::string* target) {
    const size_t slash = path.find('/');
    const std::string first = path.substr(0, slash);
    for (int i = 0; i < g_menu_model_get_n_items(m); i++) {
        GMenuModel* sec = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION);
        if (sec) {
            const bool found = itemAt(sec, path, action, target);
            g_object_unref(sec);
            if (found) return true;
            continue;
        }
        char* label = nullptr;
        g_menu_model_get_item_attribute(m, i, "label", "s", &label);
        const bool same = label && first == seen(label);
        g_free(label);
        if (!same) continue;
        if (slash != std::string::npos) {
            GMenuModel* sub = g_menu_model_get_item_link(m, i, G_MENU_LINK_SUBMENU);
            const bool found = sub && itemAt(sub, path.substr(slash + 1), action, target);
            if (sub) g_object_unref(sub);
            if (found) return true;
            continue;
        }
        char* a = nullptr;
        char* t = nullptr;
        g_menu_model_get_item_attribute(m, i, "action", "s", &a);
        g_menu_model_get_item_attribute(m, i, "target", "s", &t);
        *action = a ? a : "";
        *target = t ? t : "";
        g_free(a);
        g_free(t);
        return true;
    }
    return false;
}
// The window's book as it opens (the menu its button builds then).
std::vector<std::string> bookOf(GtkWindow* w) {
    Browser* b = browserOf(w);
    if (!b || !b->bookMenu) return {};
    GMenu* m = b->bookMenu();
    const std::vector<std::string> out = labels(G_MENU_MODEL(m));
    g_object_unref(m);
    return out;
}
std::vector<std::string> barMenu() { return labels(G_MENU_MODEL(MCGuiBookmarksMenu())); }
// Every submenu label of the window's book once each: GtkPopoverMenu names
// a submenu's page by its label, and a second alike opens the first.
bool submenuNamesUnique(GtkWindow* w) {
    Browser* b = browserOf(w);
    if (!b || !b->bookMenu) return false;
    GMenu* m = b->bookMenu();
    std::vector<std::string> l, raw;
    walk(G_MENU_MODEL(m), "", &l, &raw);
    g_object_unref(m);
    std::sort(raw.begin(), raw.end());
    return std::adjacent_find(raw.begin(), raw.end()) == raw.end();
}
// The mapped button of an open popover menu showing `text` (as read).
GtkWidget* modelButton(GtkWidget* w, const std::string& text) {
    if (!w) return nullptr;
    if (std::string(G_OBJECT_TYPE_NAME(w)) == "GtkModelButton" && gtk_widget_get_mapped(w)) {
        char* t = nullptr;
        g_object_get(w, "text", &t, nullptr);
        const bool same = t && text == seen(t);
        g_free(t);
        if (same) return w;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = modelButton(c, text)) return hit;
    return nullptr;
}
GtkWidget* bookPopover(GtkWindow* w) {
    return GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(browserOf(w)->bookButton())));
}
bool bookOpen(GtkWindow* w) {
    GtkWidget* p = bookPopover(w);
    return p && gtk_widget_get_mapped(p);
}
// A button of A's open book on screen, laid out (a frame or so after the
// page slides in: a click before went to a 0x0 button, #96, #97).
GtkWidget* bookButton(const std::string& text) {
    GtkWidget* b = bookOpen(gWinA) ? modelButton(bookPopover(gWinA), text) : nullptr;
    return b && MCGuiOnScreen(b) ? b : nullptr;
}
// A row of `w`'s tree by its path in the window's folder, never its name
// (docs/notes/tree.md).
GtkWidget* treeRow(GtkWindow* w, const std::string& name) {
    const MCGuiAppWindow a = win(w);
    return MCGuiTreeRow(a.tree, a.rootDir + "/" + name);
}
void setShowHidden(bool on) {
    g_action_group_change_action_state(G_ACTION_GROUP(MCGuiApplication()), "showhidden",
                                       g_variant_new_boolean(on));
}
// Ctrl+D with the page of `w` focused, as a person saves a page.
void saveWithKey(GtkWindow* w) {
    MCGuiSetKeyWindow(w);
    MCGuiFocusKeyWindow();
    if (Browser* b = browserOf(w)) gtk_widget_grab_focus(b->webView());
    MCGuiKey("ctrl+d");
}

}  // namespace

MC_GUI_SUITE(bookmark_workspace) {
    MCGuiStep("the shared folder is the scratch home's ~/Bookmarks; a server; shared bookmarks", [] {
        gWindowsBefore = MCGuiAppWindows().size();
        MCGuiSetSettings("lsp.enabled = false\n");   // browser.bookmarks unset: ~/Bookmarks
        gShared = Bookmarks::folder("");
        // Never the owner's: the scratch home's, or a scratch folder named
        // in the settings when the home is not the scratch one.
        const bool home = scratch(gShared);
        GCHECK(home);
        if (!home) {
            gShared = MCGuiScratch() + "/shared-bookmarks";
            MCGuiSetSettings("lsp.enabled = false\nbrowser.bookmarks = " + gShared + "\n");
        }
        gA = MCGuiScratch() + "/ws-a";
        gB = MCGuiScratch() + "/ws-b";
        GCHECK(scratch(gShared) && scratch(gA) && scratch(gB));

        gServer = g_threaded_socket_service_new(4);
        gPort = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(gServer), nullptr, nullptr);
        GCHECK(gPort > 0);
        g_signal_connect(gServer, "run", G_CALLBACK(onIncoming), nullptr);
        g_socket_service_start(gServer);

        writeBookmark(gShared + "/Shared One.url", url("s1"));
        writeBookmark(gShared + "/Reading/Deep.url", url("deep"));
        MCGuiWriteFile(gA + "/main.py", "print('a')\n");
        MCGuiWriteFile(gB + "/main.py", "print('b')\n");
        gSharedCount = count(gShared);
        MCGuiOpenWindow(gA);
    });
    MCGuiWait("a window on workspace A", 30, [] {
        const MCGuiAppWindow w = MCGuiApp();
        return MCGuiAppWindows().size() == gWindowsBefore + 1 && w.rootDir == gA && w.browser;
    });
    MCGuiStep("its browser on a page", [] {
        gWinA = MCGuiApp().window;
        MCGuiSetKeyWindow(gWinA);
        MCGuiFocusKeyWindow();
        win(gWinA).layout->showBrowser();
        browserOf(gWinA)->newTab(url("alpha"));
    });
    MCGuiWait("Alpha is loaded", 60, [] { return pageIs(gWinA, "Alpha"); });

    // ---- saving in the workspace
    MCGuiStep("Ctrl+D with the page focused", [] {
        GCHECK(!MCGuiFileExists(gA + "/.mutible"));
        saveWithKey(gWinA);
    });
    MCGuiWait("Alpha.url is in A's .mutible/bookmarks", 20,
              [] { return MCGuiFileExists(gA + "/.mutible/bookmarks/Alpha.url"); });
    MCGuiStep("its address; no .gitignore; nothing in the shared folder; the status says where", [] {
        GCHECK_EQ_STR(WebBookmark::addressIn("Alpha.url", MCGuiReadFile(gA + "/.mutible/bookmarks/Alpha.url")),
                      url("alpha"));
        GCHECK(!MCGuiFileExists(gA + "/.gitignore"));
        GCHECK(!MCGuiFileExists(gA + "/.mutible/.gitignore"));
        GCHECK(!MCGuiFileExists(gA + "/.mutible/bookmarks/.gitignore"));
        GCHECK(count(gA + "/.mutible/bookmarks") == 1);
        GCHECK(count(gShared) == gSharedCount);
        GCHECK(!MCGuiFileExists(gShared + "/Alpha.url"));
        GCHECK(has(statusOf(gWinA), "Saved “Alpha.url” in .mutible/bookmarks"));
        if (!has(statusOf(gWinA), "in .mutible/bookmarks")) MCGuiNote("status: %s", statusOf(gWinA).c_str());
    });

    // ---- the tree hides .mutible
    MCGuiWait("the tree lists main.py", 20, [] { return treeRow(gWinA, "main.py") != nullptr; });
    MCGuiStep("hidden files shown", [] {
        GCHECK(treeRow(gWinA, ".mutible") == nullptr);
        GCHECK(!win(gWinA).tree->showHidden());
        setShowHidden(true);
    });
    MCGuiWait("the tree shows .mutible: it was there, and hidden", 20,
              [] { return treeRow(gWinA, ".mutible") != nullptr; });
    MCGuiStep("hidden files hidden again", [] { setShowHidden(false); });
    MCGuiWait(".mutible is hidden again", 20, [] { return treeRow(gWinA, ".mutible") == nullptr; });
    MCGuiStep("the setting is off, as it was", [] { GCHECK(!win(gWinA).tree->showHidden()); });

    // ---- the menus: the workspace's first, then Shared
    MCGuiStep("the book: A's bookmark, then Shared with ~/Bookmarks and Move to This Workspace", [] {
        const std::vector<std::string> l = bookOf(gWinA);
        GCHECK(indexOf(l, "Alpha") == 0);
        GCHECK(indexOf(l, "No bookmarks") < 0);
        GCHECK(indexOf(l, "Shared") > indexOf(l, "Alpha"));
        GCHECK(indexOf(l, "Shared/Reading") > indexOf(l, "Shared"));
        GCHECK(indexOf(l, "Shared/Reading/Open All in Tabs") > indexOf(l, "Shared/Reading"));
        GCHECK(indexOf(l, "Shared/Reading/Deep") > indexOf(l, "Shared/Reading/Open All in Tabs"));
        GCHECK(indexOf(l, "Shared/Reading/Move to This Workspace/Deep") > indexOf(l, "Shared/Reading/Deep"));
        GCHECK(indexOf(l, "Shared/Shared One") > indexOf(l, "Shared/Reading/Move to This Workspace/Deep"));
        GCHECK(indexOf(l, "Shared/Move to This Workspace/Shared One") > indexOf(l, "Shared/Shared One"));
        GCHECK(indexOf(l, "Shared/Alpha") < 0);
        std::string text;
        for (const std::string& s : l) text += s + " | ";
        if (indexOf(l, "Shared/Shared One") < 0) MCGuiNote("book: %s", text.c_str());
    });
    MCGuiStep("the book's button opens on the same", [] { MCGuiClick(browserOf(gWinA)->bookButton()); });
    MCGuiWait("open", 10, [] {
        GtkWidget* p = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(browserOf(gWinA)->bookButton())));
        return p && gtk_widget_get_mapped(p);
    });
    MCGuiStep("its model lists Alpha first and Shared", [] {
        const std::vector<std::string> l =
            labels(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(browserOf(gWinA)->bookButton())));
        GCHECK(indexOf(l, "Alpha") == 0);
        GCHECK(indexOf(l, "Shared/Shared One") > 0);
        gtk_menu_button_popdown(GTK_MENU_BUTTON(browserOf(gWinA)->bookButton()));
    });
    MCGuiWait("the book is closed", 10, [] {
        GtkWidget* p = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(browserOf(gWinA)->bookButton())));
        return !p || !gtk_widget_get_mapped(p);
    });
    MCGuiWait("the menu bar's Bookmarks menu, A in front, has the same", 20, [] {
        const std::vector<std::string> l = barMenu();
        return indexOf(l, "Bookmark This Page") == 0 && indexOf(l, "Alpha") == 1 &&
               indexOf(l, "Shared/Shared One") > 1 && indexOf(l, "Shared/Reading/Deep") > 1;
    });
    // ---- "already bookmarked": the workspace first
    MCGuiStep("the same page bookmarked in the shared folder too; Ctrl+D", [] {
        writeBookmark(gShared + "/Alpha elsewhere.url", url("alpha"));
        saveWithKey(gWinA);
    });
    MCGuiWait("it is already bookmarked, as the workspace's file", 20, [] {
        return has(statusOf(gWinA), "Already bookmarked as .mutible/bookmarks/Alpha.url");
    });
    MCGuiStep("not in Shared's words", [] {
        GCHECK(!has(statusOf(gWinA), "in Shared"));
        GCHECK(count(gA + "/.mutible/bookmarks") == 1);
        browserOf(gWinA)->load(url("s1"));
    });
    MCGuiWait("a page bookmarked only in Shared: the star is filled", 60, [] {
        return pageIs(gWinA, "Shared One") && browserOf(gWinA)->starFilled();
    });
    MCGuiStep("Ctrl+D on it", [] { saveWithKey(gWinA); });
    MCGuiWait("it is already bookmarked in Shared", 20, [] {
        return has(statusOf(gWinA), "Already bookmarked in Shared as") &&
               has(statusOf(gWinA), "Shared One.url");
    });
    MCGuiPause(0.3);   // a negative check: nothing may be written
    MCGuiStep("and nothing was written in the workspace", [] {
        GCHECK(count(gA + "/.mutible/bookmarks") == 1);
    });

    // ---- Move to This Workspace, by clicks through the book: Reading's,
    // whose label Shared's own has too. GTK names a submenu's page by its
    // label and opened Shared's list for Reading's item (the
    // "duplicate child name" warning) until the labels were kept apart.
    MCGuiStep("the book opened", [] {
        GCHECK(submenuNamesUnique(gWinA));
        GCHECK(!bookOpen(gWinA));
        MCGuiClick(browserOf(gWinA)->bookButton());
    });
    MCGuiWait("Shared on screen", 10, [] { return bookButton("Shared") != nullptr; });
    MCGuiStep("a click on Shared", [] { MCGuiClick(bookButton("Shared")); });
    MCGuiWait("Reading on screen", 10, [] { return bookButton("Reading") != nullptr; });
    MCGuiStep("a click on Reading", [] { MCGuiClick(bookButton("Reading")); });
    MCGuiWait("Reading's page: Open All in Tabs and Move to This Workspace on screen", 10, [] {
        return bookButton("Open All in Tabs") != nullptr && bookButton("Move to This Workspace") != nullptr &&
               bookButton("Shared One") == nullptr;
    });
    MCGuiStep("a click on Reading's Move to This Workspace", [] { MCGuiClick(bookButton("Move to This Workspace")); });
    MCGuiWait("its page is on screen", 10, [] {
        return bookButton("Open All in Tabs") == nullptr && (bookButton("Deep") || bookButton("Shared One"));
    });
    MCGuiStep("it lists Reading's bookmark, not Shared's; a click on Deep", [] {
        GCHECK(bookButton("Deep") != nullptr);
        GCHECK(bookButton("Shared One") == nullptr);
        GCHECK(bookButton("Alpha elsewhere") == nullptr);
        if (GtkWidget* b = bookButton("Deep")) MCGuiClick(b);
    });
    MCGuiWait("the file moved into A's .mutible/bookmarks", 20, [] {
        return MCGuiFileExists(gA + "/.mutible/bookmarks/Deep.url") && !MCGuiFileExists(gShared + "/Reading/Deep.url");
    });
    MCGuiWait("the book closed", 10, [] { return !bookOpen(gWinA); });
    MCGuiStep("its address kept, the status says where", [] {
        GCHECK_EQ_STR(WebBookmark::addressIn("Deep.url", MCGuiReadFile(gA + "/.mutible/bookmarks/Deep.url")),
                      url("deep"));
        GCHECK(has(statusOf(gWinA), "Moved “Deep.url” to .mutible/bookmarks"));
        GCHECK(MCGuiFileExists(gShared + "/Reading"));   // the folder stays
    });
    MCGuiWait("the menus follow: Deep is A's own, gone from Shared", 20, [] {
        const std::vector<std::string> l = bookOf(gWinA), bar = barMenu();
        return indexOf(l, "Deep") >= 0 && indexOf(l, "Deep") < indexOf(l, "Shared") &&
               indexOf(l, "Shared/Reading/Deep") < 0 && indexOf(bar, "Deep") > 0 &&
               indexOf(bar, "Shared/Reading/Deep") < 0;
    });

    // ---- Shared's own Move to This Workspace, by its item, onto a taken name
    MCGuiStep("A has a Shared One.url of its own; Shared > Move to This Workspace > Shared One", [] {
        writeBookmark(gA + "/.mutible/bookmarks/Shared One.url", url("mine"));
        GMenu* m = browserOf(gWinA)->bookMenu();
        std::string action, target;
        GCHECK(itemAt(G_MENU_MODEL(m), "Shared/Move to This Workspace/Shared One", &action, &target));
        g_object_unref(m);
        GCHECK_EQ_STR(action, "win.movebookmark");
        GCHECK_EQ_STR(target, gShared + "/Shared One.url");
        if (action == "win.movebookmark")
            g_action_group_activate_action(G_ACTION_GROUP(gWinA), "movebookmark",
                                           g_variant_new_string(target.c_str()));
    });
    MCGuiWait("it is Shared One 2.url in A, gone from Shared", 20, [] {
        return MCGuiFileExists(gA + "/.mutible/bookmarks/Shared One 2.url") &&
               !MCGuiFileExists(gShared + "/Shared One.url");
    });
    MCGuiStep("A's own file untouched; a file outside Shared is refused", [] {
        GCHECK_EQ_STR(WebBookmark::addressIn("x.url", MCGuiReadFile(gA + "/.mutible/bookmarks/Shared One.url")),
                      url("mine"));
        GCHECK_EQ_STR(WebBookmark::addressIn("x.url", MCGuiReadFile(gA + "/.mutible/bookmarks/Shared One 2.url")),
                      url("s1"));
        g_action_group_activate_action(G_ACTION_GROUP(gWinA), "movebookmark",
                                       g_variant_new_string((gA + "/.mutible/bookmarks/Alpha.url").c_str()));
    });
    MCGuiWait("the status says it was not moved", 20,
              [] { return has(statusOf(gWinA), "The bookmark was not moved"); });
    MCGuiStep("and it is where it was", [] { GCHECK(MCGuiFileExists(gA + "/.mutible/bookmarks/Alpha.url")); });
    // ---- a second workspace
    MCGuiStep("a window on workspace B", [] { MCGuiOpenWindow(gB); });
    MCGuiWait("it is open", 30, [] {
        const MCGuiAppWindow w = MCGuiApp();
        return MCGuiAppWindows().size() == gWindowsBefore + 2 && w.rootDir == gB && w.browser;
    });
    MCGuiStep("B in front", [] {
        gWinB = MCGuiApp().window;
        MCGuiSetKeyWindow(gWinB);
        MCGuiFocusKeyWindow();
    });
    MCGuiStep("B's book: none of A's, the shared ones under Shared", [] {
        const std::vector<std::string> l = bookOf(gWinB);
        GCHECK(indexOf(l, "No bookmarks") == 0);
        GCHECK(indexOf(l, "Alpha") < 0);
        GCHECK(indexOf(l, "Deep") < 0);
        GCHECK(indexOf(l, "Shared/Alpha elsewhere") > 0);
        GCHECK(indexOf(l, "Shared/Move to This Workspace/Alpha elsewhere") > 0);
    });
    MCGuiWait("the menu bar's Bookmarks menu follows B", 20, [] {
        const std::vector<std::string> l = barMenu();
        return indexOf(l, "No bookmarks") == 1 && indexOf(l, "Alpha") < 0 && indexOf(l, "Deep") < 0 &&
               indexOf(l, "Shared/Alpha elsewhere") > 1;
    });
    MCGuiStep("B's .mutible is not made by looking", [] { GCHECK(!MCGuiFileExists(gB + "/.mutible")); });
    MCGuiStep("A in front again", [] {
        MCGuiSetKeyWindow(gWinA);
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("the menu bar's menu is A's again", 20, [] {
        const std::vector<std::string> l = barMenu();
        return indexOf(l, "Alpha") == 1 && indexOf(l, "No bookmarks") < 0;
    });

    // ---- the window on ~/Bookmarks keeps the one folder
    MCGuiStep("mutible https://...", [] {
        MCGuiSetKeyWindow(nullptr);
        gErrors.clear();
        MCGuiCommandLine({url("w1")}, gA, &gErrors);
    });
    MCGuiWait("a window on the shared folder shows the page", 60, [] {
        const MCGuiAppWindow w = MCGuiApp();
        return MCGuiAppWindows().size() == gWindowsBefore + 3 && w.rootDir == gShared && pageIs(w.window, "Page w1");
    });
    MCGuiStep("its book is the folder itself, no Shared part", [] {
        const std::vector<std::string> l = bookOf(MCGuiApp().window);
        GCHECK(indexOf(l, "Reading") == 0);
        GCHECK(indexOf(l, "Alpha elsewhere") > 0);
        GCHECK(indexOf(l, "Shared") < 0);
        GCHECK(indexOf(l, "Move to This Workspace") < 0);
        GCHECK(gErrors.empty());
        gSharedCount = count(gShared);
        saveWithKey(MCGuiApp().window);
    });
    MCGuiWait("Ctrl+D there saves in the folder itself", 20,
              [] { return MCGuiFileExists(gShared + "/Page w1.url"); });
    MCGuiStep("not in a .mutible of it", [] {
        GCHECK(!MCGuiFileExists(gShared + "/.mutible"));
        GCHECK(count(gShared) == gSharedCount + 1);
    });

    // ---- put away
    MCGuiStep("the windows the suite made are closed; the server stopped", [] {
        MCGuiSetKeyWindow(nullptr);
        const auto all = MCGuiAppWindows();
        for (size_t i = gWindowsBefore; i < all.size(); i++) gtk_window_close(all[i].window);
        if (gServer) {
            g_socket_service_stop(gServer);
            g_socket_listener_close(G_SOCKET_LISTENER(gServer));
            g_object_unref(gServer);
            gServer = nullptr;
        }
    });
    MCGuiWait("gone", 10, [] { return MCGuiAppWindows().size() == gWindowsBefore; });
    MCGuiStep("the first window has the keyboard; the scratch folders go", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        removeTree(gA);
        removeTree(gB);
        removeTree(gShared);
        gWinA = gWinB = nullptr;
    });
}
