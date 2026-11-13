// BookmarkSaveSuite.cpp — saving pages as bookmarks (`./build/minicode-guitest
// bookmark_save`, PARITY.md 4.8, second part; the Mac's bookmarks suite).
// Pages come from a server inside this process on 127.0.0.1 and the
// bookmark folder is a scratch one, so nothing leaves this machine and
// nothing lands in the real ~/Bookmarks.
//   - Ctrl+D with the page focused writes a .url named for the page's
//     title, holding its address;
//   - a second one writes nothing and says where the bookmark is;
//   - a page with a taken title gets a number, and never replaces a file;
//   - Ctrl+D in the editor is still Add Next Occurrence and saves nothing;
//   - File > Save Page as Bookmark (its action) works with the editor focused;
//   - File > Save All Tabs as Bookmarks writes a folder with one file a tab.
#include "CaretSuites.h"

#include "AppSeam.h"
#include "Browser.h"
#include "BrowserPages.h"
#include "WebBookmark.h"
#include "WindowLayout.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace caretsuite;

namespace {

GSocketService* gServer = nullptr;
int gPort = 0;
std::string gFolder;

gboolean onIncoming(GThreadedSocketService*, GSocketConnection* conn, GObject*, gpointer) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    gssize n = g_input_stream_read(in, buf, sizeof buf - 1, nullptr, nullptr);
    if (n <= 0) return TRUE;
    buf[n] = 0;
    const std::string req(buf);
    // /one and /two are both "Alpha", /three is "Gamma".
    const char* title = req.rfind("GET /three", 0) == 0 ? "Gamma" : "Alpha";
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

// The names in a folder, sorted.
std::vector<std::string> names(const std::string& dir) {
    std::vector<std::string> out;
    if (GDir* d = g_dir_open(dir.c_str(), 0, nullptr)) {
        while (const char* n = g_dir_read_name(d)) out.push_back(n);
        g_dir_close(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}
std::string addressOf(const std::string& path) {
    return WebBookmark::addressIn(path, MCGuiReadFile(path));
}
std::string tabsFolder() {
    for (const std::string& n : names(gFolder))
        if (n.compare(0, 5, "Tabs ") == 0) return gFolder + "/" + n;
    return "";
}
void focusPage() { gtk_widget_grab_focus(browser()->webView()); }

}  // namespace

MC_GUI_SUITE(bookmark_save) {
    MCGuiStep("a scratch bookmark folder, a server, the pane", [] {
        gFolder = MCGuiScratch() + "/bookmark-save";
        MCGuiSetSettings("lsp.enabled = false\nbrowser.bookmarks = " + gFolder + "\n");
        GCHECK(browser() != nullptr);
        if (!browser()) return;
        gServer = g_threaded_socket_service_new(4);
        gPort = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(gServer), nullptr, nullptr);
        GCHECK(gPort > 0);
        g_signal_connect(gServer, "run", G_CALLBACK(onIncoming), nullptr);
        g_socket_service_start(gServer);
        MCGuiWriteFile(MCGuiProjectFile("sel.py"), "x = arr + arr\n");
        MCGuiApp().layout->showBrowser();
    });
    MCGuiWait("the first page is made", 60, [] { return browser()->userProbe() != nullptr; });
    MCGuiStep("page one", [] { browser()->load(url("/one")); });
    MCGuiWait("it is loaded", 60, [] {
        return browser()->userProbe()->title() == "Alpha" && browser()->tabTitle(0) == "Alpha";
    });

    // ---- Ctrl+D with the page focused
    MCGuiStep("Ctrl+D with the page focused", [] {
        GCHECK(!MCGuiFileExists(gFolder + "/Alpha.url"));
        focusPage();
        MCGuiKey("ctrl+d");
    });
    MCGuiWait("Alpha.url is written", 20, [] { return MCGuiFileExists(gFolder + "/Alpha.url"); });
    MCGuiStep("it holds the page's address, and the status says where", [] {
        GCHECK_EQ_STR(addressOf(gFolder + "/Alpha.url"), url("/one"));
        GCHECK(names(gFolder).size() == 1);
        GCHECK(has(statusText(), "Saved") && has(statusText(), "Alpha.url"));
    });
    MCGuiStep("Ctrl+D again on the same page", [] {
        focusPage();
        MCGuiKey("ctrl+d");
    });
    MCGuiWait("the status says it is bookmarked already, and where", 20, [] {
        return has(statusText(), "Already bookmarked as") && has(statusText(), "Alpha.url");
    });
    MCGuiPause(0.3);   // a negative check: nothing more may be written
    MCGuiStep("nothing else was written", [] { GCHECK(names(gFolder).size() == 1); });

    // ---- a taken name
    MCGuiStep("another page with the same title in a new tab", [] {
        browser()->newTab();
        browser()->load(url("/two"));
    });
    MCGuiWait("it is loaded", 60, [] {
        return browser()->userProbe() && browser()->userProbe()->title() == "Alpha" &&
               has(browser()->tabUrl(1), "/two");
    });
    MCGuiStep("Ctrl+D", [] {
        focusPage();
        MCGuiKey("ctrl+d");
    });
    MCGuiWait("Alpha 2.url is written, Alpha.url untouched", 20,
              [] { return MCGuiFileExists(gFolder + "/Alpha 2.url"); });
    MCGuiStep("each holds its own address", [] {
        GCHECK_EQ_STR(addressOf(gFolder + "/Alpha 2.url"), url("/two"));
        GCHECK_EQ_STR(addressOf(gFolder + "/Alpha.url"), url("/one"));
        GCHECK(names(gFolder).size() == 2);
    });

    // ---- the editor keeps Ctrl+D
    MCGuiStep("a Python file, a click in the first arr", [] {
        MCGuiOpenFileKept(MCGuiProjectFile("sel.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == MCGuiProjectFile("sel.py"); });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });
    MCGuiStep("click in the first arr", [] { clickAt(5); });
    MCGuiWait("the editor has the keyboard", 5, [] { return caretsText() == "5"; });
    MCGuiStep("Ctrl+D", [] { MCGuiKey("ctrl+d"); });
    waitCarets("the word at the caret selected, as Add Next Occurrence does", "4>7");
    MCGuiStep("Ctrl+D again", [] { MCGuiKey("ctrl+d"); });
    waitCarets("the next arr is added", "4>7 10>13");
    MCGuiPause(0.3);   // a negative check: the key saves nothing there
    MCGuiStep("no bookmark was written by it", [] { GCHECK(names(gFolder).size() == 2); });

    // ---- File > Save Page as Bookmark, from the editor
    MCGuiStep("a third page in a new tab; the keyboard stays in the editor", [] {
        browser()->newTab(url("/three"), false);
        browser()->selectTab(2);
        clickAt(5);
    });
    MCGuiWait("it is loaded", 60, [] {
        return browser()->activeTab() == 2 && browser()->userProbe() &&
               browser()->userProbe()->title() == "Gamma";
    });
    MCGuiStep("File > Save Page as Bookmark with the editor focused", [] {
        GCHECK(!MCGuiFileExists(gFolder + "/Gamma.url"));
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "savebookmark", nullptr);
    });
    MCGuiWait("Gamma.url is written", 20, [] { return MCGuiFileExists(gFolder + "/Gamma.url"); });
    MCGuiStep("with the third page's address", [] {
        GCHECK_EQ_STR(addressOf(gFolder + "/Gamma.url"), url("/three"));
    });

    // ---- File > Save All Tabs as Bookmarks
    MCGuiStep("File > Save All Tabs as Bookmarks", [] {
        GCHECK(tabsFolder().empty());
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "savebrowsertabs", nullptr);
    });
    MCGuiWait("a Tabs folder is made", 20, [] { return !tabsFolder().empty(); });
    MCGuiStep("it holds a bookmark for each of the three tabs", [] {
        const std::string dir = tabsFolder();
        const std::vector<std::string> in = names(dir);
        GCHECK(in.size() == 3);
        std::vector<std::string> got;
        for (const std::string& n : in) got.push_back(addressOf(dir + "/" + n));
        std::sort(got.begin(), got.end());
        std::vector<std::string> want = {url("/one"), url("/three"), url("/two")};
        GCHECK(got == want);
        GCHECK(has(statusText(), "Saved 3 tabs"));
    });
    MCGuiStep("the server is stopped", [] {
        if (gServer) g_socket_service_stop(gServer);
        if (gServer) g_object_unref(gServer);
        gServer = nullptr;
    });
}
