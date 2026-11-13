// BookmarksSuite.cpp — opening web bookmarks and the browser window
// (`./build/minicode-guitest bookmarks`, PARITY.md 4.8, the Mac's bookmarks
// suite). Pages come from a server inside this process on 127.0.0.1; nothing
// leaves the machine.
//   - a .url clicked in the tree loads in a tab of the browser pane, the
//     keyboard staying in the tree; the same address again shows that tab;
//     a double-click gives the page the keyboard;
//   - a file: or javascript: address, and a .webloc that is not XML, open
//     as text;
//   - the tree's Open as Text shows the bookmark file itself, and Open All
//     in Browser Tabs opens every bookmark directly in a folder, in order;
//   - `minicode https://...`: two addresses make two tabs in the window on
//     the browser.bookmarks folder, made when missing; a later address
//     joins it, and a path is still a path.
#include "Harness.h"

#include "AppSeam.h"
#include "Bookmarks.h"
#include "Browser.h"
#include "BrowserPages.h"
#include "Editor.h"
#include "FileTree.h"
#include "LaunchOptions.h"
#include "WindowLayout.h"

#include <string>

namespace {

GSocketService* gServer = nullptr;
int gPort = 0;

gboolean onIncoming(GThreadedSocketService*, GSocketConnection* conn, GObject*, gpointer) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    gssize n = g_input_stream_read(in, buf, sizeof buf - 1, nullptr, nullptr);
    if (n <= 0) return TRUE;
    buf[n] = 0;
    const std::string req(buf);
    const size_t sp = req.find(' ');
    const std::string path = sp == std::string::npos ? "" : req.substr(sp + 1, req.find(' ', sp + 1) - sp - 1);
    const std::string name = path.size() > 1 ? path.substr(1) : "root";
    const std::string body = "<title>Page " + name + "</title><p>page " + name + "</p>";
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    const std::string all = head + body;
    g_output_stream_write_all(out, all.data(), all.size(), nullptr, nullptr, nullptr);
    g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
    return TRUE;
}

std::string url(const std::string& name) { return "http://127.0.0.1:" + std::to_string(gPort) + "/" + name; }
Browser* browser() { return MCGuiApp().browser; }
Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& n) { return MCGuiProjectFile(n); }
std::string gBm;                      // the bookmark folder
GtkWidget* treeRow(const std::string& name) { return MCGuiFindLabel(MCGuiApp().tree->widget(), name); }
bool treeHasKeyboard() {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
    return f && gtk_widget_is_ancestor(f, MCGuiApp().tree->widget());
}
bool pageHasKeyboard() {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
    return f && browser()->webView() &&
           (f == browser()->webView() || gtk_widget_is_ancestor(f, browser()->webView()));
}
int tabAt(const std::string& name) {
    for (int i = 0; i < browser()->tabCount(); i++)
        if (browser()->tabUrl(i).find("/" + name) != std::string::npos) return i;
    return -1;
}
bool actionEnabled(const char* name) {
    GAction* a = g_action_map_lookup_action(G_ACTION_MAP(MCGuiApp().window), name);
    return a && g_action_get_enabled(a);
}
// The row's menu pops up and stays until it is dismissed, taking the next
// click: popovers under the tree are closed in a step of their own first.
bool anyMenuOpen(GtkWidget* w) {
    if (GTK_IS_POPOVER(w) && gtk_widget_get_mapped(w)) return true;
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        if (anyMenuOpen(c)) return true;
    return false;
}
void closeMenus(GtkWidget* w) {
    if (GTK_IS_POPOVER(w)) gtk_popover_popdown(GTK_POPOVER(w));
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        closeMenus(c);
}
void activate(const char* name) {
    g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), name, nullptr);
}
void rightClickRow(const std::string& name) {
    GCHECK(treeRow(name) != nullptr);
    MCGuiClick(treeRow(name), -1, -1, 3);
}
int gTabs = 0, gWindows = 0;
std::string gErrors;

}  // namespace

MC_GUI_SUITE(bookmarks) {
    MCGuiStep("a server, bookmarks and a settings file naming the bookmark folder", [] {
        gServer = g_threaded_socket_service_new(4);
        g_signal_connect(gServer, "run", G_CALLBACK(onIncoming), nullptr);
        GError* error = nullptr;
        GInetAddress* lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
        GSocketAddress* at = g_inet_socket_address_new(lo, 0);
        GSocketAddress* eff = nullptr;
        g_socket_listener_add_address(G_SOCKET_LISTENER(gServer), at, G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_TCP, nullptr, &eff, &error);
        GCHECK(error == nullptr);
        if (eff) gPort = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(eff));
        g_clear_object(&eff);
        g_object_unref(at);
        g_object_unref(lo);
        g_socket_service_start(gServer);
        GCHECK(gPort > 0);

        gBm = MCGuiScratch() + "/bm-folder";   // not made: the command line makes it
        MCGuiSetSettings("lsp.enabled = false\nbrowser.bookmarks = " + gBm + "\n");

        MCGuiWriteFile(F("a.url"), "[InternetShortcut]\r\nURL=" + url("a") + "\r\n");
        MCGuiWriteFile(F("a-again.url"), "[InternetShortcut]\nURL=" + url("a") + "\n");
        MCGuiWriteFile(F("file.url"), "[InternetShortcut]\nURL=file:///etc/hostname\n");
        MCGuiWriteFile(F("js.url"), "[InternetShortcut]\nURL=javascript:alert(1)\n");
        MCGuiWriteFile(F("binary.webloc"), std::string("bplist00\x01\x02", 10));
        MCGuiWriteFile(F("folder/1-first.url"), "[InternetShortcut]\nURL=" + url("one") + "\n");
        MCGuiWriteFile(F("folder/2-second.url"), "[InternetShortcut]\nURL=" + url("two") + "\n");
        MCGuiWriteFile(F("folder/3-third.url"), "[InternetShortcut]\nURL=" + url("three") + "\n");
        MCGuiWriteFile(F("folder/4-bad.url"), "[InternetShortcut]\nURL=file:///etc/hostname\n");
        MCGuiWriteFile(F("folder/notes.txt"), "not a bookmark\n");
        MCGuiWriteFile(F("empty/notes.txt"), "no bookmark here\n");
        MCGuiWriteFile(F("notes.txt"), "plain\n");
        MCGuiApp().layout->hideBrowser();   // an earlier suite may have left it showing
        GCHECK(!MCGuiApp().layout->browserShown());
        GCHECK(MCGuiFileExists(gBm) == false);
    });
    MCGuiWait("the tree lists them", 30, [] { return treeRow("a.url") && treeRow("js.url") && treeRow("binary.webloc") && treeRow("folder") && treeRow("empty") && treeRow("notes.txt"); });

    // ---- opening from the tree
    MCGuiStep("a single click on a .url", [] {
        GCHECK(!MCGuiApp().layout->browserShown());
        MCGuiClick(treeRow("a.url"));
    });
    MCGuiWait("the pane shows a tab at its address", 60, [] {
        return MCGuiApp().layout->browserShown() && tabAt("a") >= 0 && browser()->userProbe() &&
               browser()->userProbe()->title() == "Page a";
    });
    MCGuiStep("it took the home tab; the keyboard stayed in the tree; no editor tab was made", [] {
        GCHECK(browser()->tabCount() == 1);
        GCHECK(treeHasKeyboard());
        GCHECK(ed()->currentPath().find("a.url") == std::string::npos);
        GCHECK(ed()->tabCount() == 0 || ed()->currentPath() != F("a.url"));
        gTabs = browser()->tabCount();
        browser()->newTab(url("other"));   // another tab on show, so the first must be brought back
    });
    MCGuiWait("the other tab is on show", 60, [] { return browser()->activeTab() == 1 && browser()->userProbe(); });
    MCGuiStep("another bookmark of the same address", [] { MCGuiClick(treeRow("a-again.url")); });
    MCGuiWait("the first tab is shown again", 30, [] { return browser()->activeTab() == 0; });
    MCGuiStep("no new tab was made", [] { GCHECK(browser()->tabCount() == gTabs + 1); });
    MCGuiStep("a double-click on a bookmark gives the page the keyboard", [] {
        MCGuiApp().layout->toggleBrowser();   // hidden, to see it come back
        GCHECK(!MCGuiApp().layout->browserShown());
        MCGuiDoubleClick(treeRow("a.url"));
    });
    MCGuiWait("the page is shown and has the keyboard", 30, [] {
        return MCGuiApp().layout->browserShown() && pageHasKeyboard() && browser()->activeTab() == 0;
    });

    // ---- what is not a web address opens as text
    MCGuiStep("a file: bookmark", [] { MCGuiClick(treeRow("file.url")); });
    MCGuiWait("it is text in the editor", 30, [] { return ed()->currentPath() == F("file.url"); });
    MCGuiStep("a javascript: bookmark", [] {
        GCHECK(MCGuiEditorText().find("file:///etc/hostname") != std::string::npos);
        GCHECK(browser()->tabCount() == gTabs + 1);
        MCGuiClick(treeRow("js.url"));
    });
    MCGuiWait("it is text too", 30, [] { return ed()->currentPath() == F("js.url"); });
    MCGuiStep("a binary .webloc", [] {
        GCHECK(MCGuiEditorText().find("javascript:alert(1)") != std::string::npos);
        MCGuiClick(treeRow("binary.webloc"));
    });
    MCGuiWait("it is not sent to the browser", 30, [] { return ed()->currentPath() != F("js.url"); });
    MCGuiStep("no tab was added for any of them", [] { GCHECK(browser()->tabCount() == gTabs + 1); });

    // ---- the right click menu
    MCGuiStep("a right click on a bookmark enables Open as Text only", [] { rightClickRow("a.url"); });
    MCGuiWait("the menu items follow the row", 10, [] {
        return actionEnabled("openbookmarkastext") && !actionEnabled("openbookmarkstabs");
    });
    MCGuiStep("Open as Text", [] {
        gTabs = browser()->tabCount();
        activate("openbookmarkastext");
    });
    MCGuiWait("the file itself is on show", 30, [] { return ed()->currentPath() == F("a.url"); });
    MCGuiStep("its text is the shortcut, the browser untouched, the editor has the keyboard", [] {
        GCHECK(MCGuiEditorText().find("[InternetShortcut]") != std::string::npos);
        GCHECK(browser()->tabCount() == gTabs);
        GCHECK(gtk_widget_has_focus(ed()->textView()));
        closeMenus(MCGuiApp().tree->widget());
    });
    MCGuiWait("the menu is closed", 10, [] { return !anyMenuOpen(MCGuiApp().tree->widget()); });
    MCGuiStep("a right click on a plain file", [] { rightClickRow("notes.txt"); });
    MCGuiWait("a plain file enables neither", 10, [] {
        return !actionEnabled("openbookmarkastext") && !actionEnabled("openbookmarkstabs");
    });
    MCGuiStep("close the menu", [] { closeMenus(MCGuiApp().tree->widget()); });
    MCGuiWait("it is closed", 10, [] { return !anyMenuOpen(MCGuiApp().tree->widget()); });
    MCGuiStep("a folder with no bookmark enables neither", [] { rightClickRow("empty"); });
    MCGuiWait("still neither", 10, [] { return !actionEnabled("openbookmarkastext") && !actionEnabled("openbookmarkstabs"); });
    MCGuiStep("close the menu again", [] { closeMenus(MCGuiApp().tree->widget()); });
    MCGuiWait("it is closed again", 10, [] { return !anyMenuOpen(MCGuiApp().tree->widget()); });
    MCGuiStep("a folder with bookmarks enables Open All", [] { rightClickRow("folder"); });
    MCGuiWait("Open All in Browser Tabs is enabled", 10, [] {
        return actionEnabled("openbookmarkstabs") && !actionEnabled("openbookmarkastext");
    });
    MCGuiStep("Open All in Browser Tabs", [] {
        gTabs = browser()->tabCount();
        activate("openbookmarkstabs");
    });
    MCGuiWait("three tabs, one a bookmark, in order; the bad one skipped", 60, [] {
        return browser()->tabCount() == gTabs + 3 && tabAt("one") >= 0 && tabAt("two") == tabAt("one") + 1 &&
               tabAt("three") == tabAt("two") + 1 && browser()->activeTab() == tabAt("one");
    });
    MCGuiStep("their keyboard is the page's", [] { GCHECK(pageHasKeyboard()); });

    // ---- minicode https://...
    MCGuiStep("the command line keeps an address an address", [] {
        const auto p = LaunchOptions::parse({"minicode", "https://example.test/x"});
        GCHECK(p.error.empty());
        GCHECK(p.args.size() == 2 && p.args[1] == "https://example.test/x");
        gWindows = (int)MCGuiAppWindows().size();
        GCHECK(!MCGuiFileExists(gBm));
        gErrors.clear();
        MCGuiCommandLine({url("w1"), url("w2")}, MCGuiProject(), &gErrors);
    });
    MCGuiWait("a window on the bookmark folder, made with it, shows two tabs", 60, [] {
        if ((int)MCGuiAppWindows().size() != gWindows + 1) return false;
        const MCGuiAppWindow w = MCGuiApp();
        return w.rootDir == gBm && w.browser && w.browser->tabCount() == 2 && w.layout->browserShown();
    });
    MCGuiStep("the folder exists, nothing was said, the page has the side, the tabs are the addresses", [] {
        GCHECK(MCGuiFileExists(gBm));
        GCHECK(gErrors.empty());
        GCHECK(!MCGuiApp().layout->editorShown());
        GCHECK(browser()->tabUrl(0).find("/w1") != std::string::npos);
        GCHECK(browser()->tabUrl(1).find("/w2") != std::string::npos);
        GCHECK(MCGuiAppWindows().size() == (size_t)gWindows + 1);
        MCGuiSetKeyWindow(nullptr);
        MCGuiCommandLine({url("w3")}, MCGuiProject(), &gErrors);
    });
    MCGuiWait("a later address joins that window, no new one", 60, [] {
        return browser()->tabCount() == 3 && tabAt("w3") >= 0;
    });
    MCGuiStep("one window more only, and a path argument still opens as a path", [] {
        GCHECK(MCGuiAppWindows().size() == (size_t)gWindows + 1);
        GCHECK(MCGuiApp().rootDir == gBm);
        gWindows = (int)MCGuiAppWindows().size();
        MCGuiCommandLine({F("notes.txt")}, MCGuiProject(), &gErrors);
    });
    MCGuiWait("the first window shows the file", 30, [] {
        const auto all = MCGuiAppWindows();
        return all.size() == (size_t)gWindows && all[0].editor->currentPath() == F("notes.txt");
    });
    MCGuiStep("the bookmark window was left alone; cleanup", [] {
        GCHECK(MCGuiApp().browser->tabCount() == 3);
        GCHECK(gErrors.empty());
        if (gServer) {
            g_socket_service_stop(gServer);
            g_socket_listener_close(G_SOCKET_LISTENER(gServer));
            g_object_unref(gServer);
            gServer = nullptr;
        }
    });
}
