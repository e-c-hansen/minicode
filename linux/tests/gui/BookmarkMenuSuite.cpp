// BookmarkMenuSuite.cpp — the toolbar's star and book, the Bookmarks menu in
// the menu bar and the palette's bookmarks (`./build/minicode-guitest
// bookmark_menu`, PARITY.md 4.8, last part; the Mac's bookmarks suite).
// Pages come from a server inside this process on 127.0.0.1, the bookmark
// folder is a scratch one and the Trash is a folder of the scratch world
// (GitTrash::useFolderForChecks), so nothing leaves this machine and nothing
// touches the real ~/Bookmarks or Trash.
//   - the star is filled for a bookmarked page (a bookmark in a subfolder
//     counts) and empty for another, and follows navigation, tab switches
//     and changes to the folder made outside;
//   - a click on the empty star saves the page; a click on the filled one
//     moves the bookmark to the Trash and the status bar says so;
//   - the book lists the folder when it opens (folders first, titles from
//     the file names, a file added meanwhile there), and an item loads in
//     the tab on show, with Ctrl in a new tab;
//   - the menu bar's Bookmarks menu lists the same;
//   - the palette has a row "Bookmarks > Name" that loads it.
#include "CaretSuites.h"

#include "AppSeam.h"
#include "Browser.h"
#include "BrowserPages.h"
#include "GitTrash.h"
#include "PalettePanel.h"
#include "WebBookmark.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace caretsuite;

namespace {

GSocketService* gServer = nullptr;
int gPort = 0;
std::string gFolder, gTrash;
int gTabs = 0;

gboolean onIncoming(GThreadedSocketService*, GSocketConnection* conn, GObject*, gpointer) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    gssize n = g_input_stream_read(in, buf, sizeof buf - 1, nullptr, nullptr);
    if (n <= 0) return TRUE;
    buf[n] = 0;
    const std::string req(buf);
    const char* title = "Alpha";
    if (req.rfind("GET /two", 0) == 0) title = "Beta";
    else if (req.rfind("GET /three", 0) == 0) title = "Gamma";
    else if (req.rfind("GET /four", 0) == 0) title = "Epsilon";
    const std::string body = std::string("<title>") + title + "</title><p>page</p>";
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n"
                             "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    const std::string all = head + body;
    g_output_stream_write_all(out, all.data(), all.size(), nullptr, nullptr, nullptr);
    g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
    return TRUE;
}

Browser* browser() { return MCGuiApp().browser; }
std::string url(const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(gPort) + path;
}
std::string statusText() {
    GtkWidget* l = MCGuiApp().statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}
bool has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }
void writeBookmark(const std::string& path, const std::string& address) {
    MCGuiWriteFile(path, WebBookmark::shortcutText(address));
}
size_t count(const std::string& dir) {
    size_t n = 0;
    if (GDir* d = g_dir_open(dir.c_str(), 0, nullptr)) {
        while (g_dir_read_name(d)) n++;
        g_dir_close(d);
    }
    return n;
}
// The files directly in gFolder whose address is `address`.
std::string fileWith(const std::string& address) {
    if (GDir* d = g_dir_open(gFolder.c_str(), 0, nullptr)) {
        std::string found;
        while (const char* n = g_dir_read_name(d))
            if (WebBookmark::addressIn(n, MCGuiReadFile(gFolder + "/" + n)) == address) found = n;
        g_dir_close(d);
        return found;
    }
    return "";
}
std::string icon() {
    const char* n = gtk_button_get_icon_name(GTK_BUTTON(browser()->starButton()));
    return n ? n : "";
}
bool starIs(bool filled) {
    return browser()->starFilled() == filled &&
           icon() == (filled ? "starred-symbolic" : "non-starred-symbolic");
}
bool pageIs(int tab, const std::string& title) {
    return browser()->activeTab() == tab && browser()->userProbe() &&
           browser()->userProbe()->title() == title;
}

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
int indexOf(const std::vector<std::string>& v, const std::string& s) {
    const auto it = std::find(v.begin(), v.end(), s);
    return it == v.end() ? -1 : (int)(it - v.begin());
}

GtkWidget* modelButton(GtkWidget* w, const std::string& text) {
    if (!w) return nullptr;
    if (std::string(G_OBJECT_TYPE_NAME(w)) == "GtkModelButton" && gtk_widget_get_mapped(w)) {
        char* t = nullptr;
        g_object_get(w, "text", &t, nullptr);
        const bool same = t && text == t;
        g_free(t);
        if (same) return w;
    }
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = modelButton(c, text)) return hit;
    return nullptr;
}
GtkWidget* bookPopover() {
    return GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(browser()->bookButton())));
}
bool bookOpen() { GtkWidget* p = bookPopover(); return p && gtk_widget_get_mapped(p); }
std::string activeUrl() { return browser()->tabUrl(browser()->activeTab()); }

}  // namespace

MC_GUI_SUITE(bookmark_menu) {
    MCGuiStep("a scratch bookmark folder with three bookmarks, a scratch Trash, a server", [] {
        gFolder = MCGuiScratch() + "/bookmark-menu";
        gTrash = MCGuiScratch() + "/bookmark-menu-trash";
        GitTrash::useFolderForChecks(gTrash);
        MCGuiSetSettings("lsp.enabled = false\nbrowser.bookmarks = " + gFolder + "\n");
        GCHECK(browser() != nullptr);
        if (!browser()) return;
        gServer = g_threaded_socket_service_new(4);
        gPort = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(gServer), nullptr, nullptr);
        GCHECK(gPort > 0);
        g_signal_connect(gServer, "run", G_CALLBACK(onIncoming), nullptr);
        g_socket_service_start(gServer);
        writeBookmark(gFolder + "/Alpha.url", url("/one"));
        writeBookmark(gFolder + "/Beta_x.url", url("/two"));
        writeBookmark(gFolder + "/Work/Delta.url", url("/three"));
        MCGuiWriteFile(gTrash + "/keep", "");
        // An earlier suite may have left tabs: tab 0 must be the page loaded below.
        while (browser()->tabCount() > 0) browser()->closeTab(browser()->tabCount() - 1);
        MCGuiApp().layout->showBrowser();
    });
    MCGuiWait("the first page is made", 60, [] { return browser()->userProbe() != nullptr; });

    // ---- the star's state
    MCGuiStep("a bookmarked page", [] { browser()->load(url("/one")); });
    MCGuiWait("it is loaded and the star is filled", 60, [] {
        return pageIs(0, "Alpha") && starIs(true);
    });
    MCGuiStep("the star says what a click does", [] {
        const char* t = gtk_widget_get_tooltip_text(browser()->starButton());
        GCHECK(t && has(t, "Trash"));
    });
    MCGuiStep("a page that is not bookmarked", [] { browser()->load(url("/four")); });
    MCGuiWait("it is loaded and the star is empty", 60, [] {
        return pageIs(0, "Epsilon") && starIs(false);
    });
    MCGuiStep("a page bookmarked in a subfolder", [] { browser()->load(url("/three")); });
    MCGuiWait("the star is filled: any bookmark in the folder counts", 60, [] {
        return pageIs(0, "Gamma") && starIs(true);
    });
    MCGuiStep("the unbookmarked page again", [] { browser()->load(url("/four")); });
    MCGuiWait("empty", 60, [] { return pageIs(0, "Epsilon") && starIs(false); });

    // ---- a click saves
    MCGuiStep("a click on the empty star", [] {
        GCHECK(fileWith(url("/four")).empty());
        MCGuiClick(browser()->starButton());
    });
    MCGuiWait("the bookmark is written", 20, [] { return !fileWith(url("/four")).empty(); });
    MCGuiWait("the star is filled and the status says saved", 20, [] {
        return starIs(true) && has(statusText(), "Saved");
    });

    // ---- a click on the filled star trashes
    MCGuiStep("a click on the filled star", [] {
        GCHECK(!fileWith(url("/four")).empty());
        MCGuiClick(browser()->starButton());
    });
    MCGuiWait("the bookmark is gone from the folder and in the Trash", 20, [] {
        return fileWith(url("/four")).empty() && count(gTrash) == 2;
    });
    MCGuiWait("the star is empty and the status says where it went", 20, [] {
        return starIs(false) && has(statusText(), "Trash");
    });
    MCGuiStep("the other bookmarks are untouched", [] {
        GCHECK(MCGuiFileExists(gFolder + "/Alpha.url"));
        GCHECK(MCGuiFileExists(gFolder + "/Work/Delta.url"));
    });

    // ---- tabs and the folder
    MCGuiStep("a second tab on a bookmarked page", [] { browser()->newTab(url("/one")); });
    MCGuiWait("it is loaded, the star filled", 60, [] { return pageIs(1, "Alpha") && starIs(true); });
    MCGuiStep("back to the first tab", [] { browser()->selectTab(0); });
    MCGuiWait("the star follows the tab: empty", 60, [] { return pageIs(0, "Epsilon") && starIs(false); });
    MCGuiStep("a bookmark for its page is made outside minicode", [] {
        writeBookmark(gFolder + "/Epsilon.url", url("/four"));
    });
    MCGuiWait("the star follows the folder: filled", 20, [] { return starIs(true); });
    MCGuiStep("and it is deleted outside", [] {
        g_unlink((gFolder + "/Epsilon.url").c_str());
    });
    MCGuiWait("empty again", 20, [] { return starIs(false); });

    // ---- the book
    MCGuiStep("a bookmark made since the last look, then the book opened", [] {
        writeBookmark(gFolder + "/Zed.url", url("/two"));
        MCGuiClick(browser()->bookButton());
    });
    MCGuiWait("the book is open", 10, bookOpen);
    MCGuiStep("it lists the folder: folders first, Open All in Tabs, titles", [] {
        const std::vector<std::string> l =
            labels(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(browser()->bookButton())));
        GCHECK(indexOf(l, "Work") == 0);
        GCHECK(indexOf(l, "Work/Open All in Tabs") == 1);
        GCHECK(indexOf(l, "Work/Delta") == 2);
        GCHECK(indexOf(l, "Alpha") > 2);
        GCHECK(indexOf(l, "Beta__x") > indexOf(l, "Alpha"));   // an underscore is doubled: no mnemonic
        GCHECK(indexOf(l, "Zed") > indexOf(l, "Beta__x"));     // read when it opened
        GCHECK(indexOf(l, "Alpha.url") < 0);
        GCHECK(gtk_widget_get_mapped(browser()->bookButton()));
    });
    MCGuiStep("a click on Beta_x", [] {
        GtkWidget* b = modelButton(bookPopover(), "Beta_x");
        GCHECK(b != nullptr);
        gTabs = browser()->tabCount();
        if (b) MCGuiClick(b);
    });
    MCGuiWait("it loads in the tab on show", 60, [] {
        return pageIs(0, "Beta") && browser()->tabCount() == gTabs;
    });
    MCGuiStep("the book again, Ctrl held for Alpha", [] {
        GCHECK(!bookOpen());
        MCGuiClick(browser()->bookButton());
    });
    MCGuiWait("open", 10, bookOpen);
    MCGuiStep("a Ctrl+click on Zed", [] {
        GtkWidget* b = modelButton(bookPopover(), "Zed");
        GCHECK(b != nullptr);
        gTabs = browser()->tabCount();
        if (b) MCGuiClickWith("ctrl", b);
    });
    MCGuiWait("it loads in a new tab", 60, [] {
        return browser()->tabCount() == gTabs + 1 && has(activeUrl(), "/two");
    });
    MCGuiStep("a folder's Open All in Tabs", [] {
        gTabs = browser()->tabCount();
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "openbookmarkfolder",
                                       g_variant_new_string((gFolder + "/Work").c_str()));
    });
    MCGuiWait("its page is open", 60, [] { return has(activeUrl(), "/three"); });

    // ---- the menu bar's Bookmarks menu
    MCGuiStep("the Bookmarks menu has the same", [] {
        const std::vector<std::string> l = labels(G_MENU_MODEL(MCGuiBookmarksMenu()));
        GCHECK(indexOf(l, "Bookmark This Page") == 0);
        GCHECK(indexOf(l, "Work") > 0);
        GCHECK(indexOf(l, "Work/Open All in Tabs") > indexOf(l, "Work"));
        GCHECK(indexOf(l, "Work/Delta") > indexOf(l, "Work/Open All in Tabs"));
        GCHECK(indexOf(l, "Alpha") > indexOf(l, "Work/Delta"));
        GCHECK(indexOf(l, "Beta__x") > indexOf(l, "Alpha"));
        GCHECK(indexOf(l, "Zed") > indexOf(l, "Beta__x"));
    });
    MCGuiStep("a bookmark added and one trashed outside", [] {
        writeBookmark(gFolder + "/Omega.url", url("/one"));
        g_unlink((gFolder + "/Zed.url").c_str());
    });
    MCGuiWait("the menu follows the folder", 20, [] {
        const std::vector<std::string> l = labels(G_MENU_MODEL(MCGuiBookmarksMenu()));
        return indexOf(l, "Omega") > 0 && indexOf(l, "Zed") < 0;
    });
    MCGuiStep("an item of the menu loads its page in the tab on show", [] {
        gTabs = browser()->tabCount();
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "openbookmark",
                                       g_variant_new_string((gFolder + "/Beta_x.url").c_str()));
    });
    MCGuiWait("loaded without a new tab", 60, [] {
        return has(activeUrl(), "/two") && browser()->tabCount() == gTabs;
    });

    // ---- the palette
    MCGuiStep("the palette's commands", [] {
        ed()->closeAllTabs();
        MCGuiFocusKeyWindow();
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+shift+p");
    });
    MCGuiWait("it is open", 10, [] { return MCGuiApp().palette->isOpen(); });
    MCGuiStep("the bookmarks are rows, Folder/Name for a subfolder", [] {
        const std::vector<std::string> l = MCGuiApp().palette->labels();
        GCHECK(indexOf(l, "Bookmarks > Alpha") >= 0);
        GCHECK(indexOf(l, "Bookmarks > Work/Delta") >= 0);
        GCHECK(indexOf(l, "Bookmarks > Zed") < 0);
        MCGuiType("Work/Delta");
    });
    MCGuiWait("one row matches", 10, [] {
        const std::vector<std::string> l = MCGuiApp().palette->labels();
        return !l.empty() && l[0] == "Bookmarks > Work/Delta";
    });
    MCGuiStep("Return", [] {
        gTabs = browser()->tabCount();
        MCGuiKey("enter");
    });
    MCGuiWait("the page loads in the browser", 60, [] { return has(activeUrl(), "/three"); });

    MCGuiStep("the server and the Trash are put away", [] {
        GitTrash::useFolderForChecks("");
        if (gServer) g_socket_service_stop(gServer);
        if (gServer) g_object_unref(gServer);
        gServer = nullptr;
    });
}
