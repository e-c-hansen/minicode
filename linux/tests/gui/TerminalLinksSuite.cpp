// TerminalLinksSuite.cpp — `./build/mutible-guitest terminal_links`: links
// in the terminal's output (Terminal.h's Link; the Mac's terminal_links
// suite, its #225 part), with a real pointer (XTest) over a real shell's
// output, printed by printf:
//   - resting on a path for half a second shows "Ctrl+click to open" beside
//     the pointer, inside the terminal, never underlined without Ctrl; a
//     move off the link, a plain press, a key and leaving the terminal take
//     it away; a plain click selects and opens nothing;
//   - Ctrl held over the path underlines it, with no hint (its text says
//     where it goes);
//   - an OSC 8 hyperlink: a rest shows the hint with its address on a
//     second line, Ctrl held shows the address alone, and Ctrl+click opens
//     it, a web address in the browser pane and a file one in the editor;
//   - a javascript: hyperlink is no link at all, though its text is a URL.
#include "Harness.h"

#include "AppSeam.h"
#include "Browser.h"
#include "DocumentView.h"
#include "Editor.h"
#include "FileTree.h"
#include "Terminal.h"
#include "TermLinks.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <vte/vte.h>

#include <string>

namespace {

Terminal* term() { return MCGuiApp().terminal; }
GtkWidget* vte() { return term() ? term()->vteWidget() : nullptr; }
std::string hint() { return term() ? term()->linkHintText() : std::string("(no terminal)"); }

// Pages from a server inside this process on 127.0.0.1, so the page the
// hyperlink names loads (a refused one puts the view back on the page
// before it, sooner or later, and a check could miss the address).
GSocketService* gServer = nullptr;
int gPort = 0;

gboolean onIncoming(GThreadedSocketService*, GSocketConnection* conn, GObject*, gpointer) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[4096];
    const gssize n = g_input_stream_read(in, buf, sizeof buf - 1, nullptr, nullptr);
    if (n <= 0) return TRUE;
    const std::string body = "<title>Hyperlink</title><p>the hyperlink's page</p>";
    const std::string all = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n"
                            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_output_stream_write_all(out, all.data(), all.size(), nullptr, nullptr, nullptr);
    g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
    return TRUE;
}

std::string web() { return "http://127.0.0.1:" + std::to_string(gPort) + "/osc8.html"; }
const std::string kRest = TermLinks::restHint(false, "");

std::string target() { return MCGuiProjectFile("links-target.txt"); }

// Where `marker` is in the output (not on the command line that printed
// it): the buffer row and column of its first character, and a column
// `offset` characters on. False until it is on screen.
bool find(const std::string& marker, long* row, long* col) {
    if (!vte()) return false;
    VteTerminal* t = VTE_TERMINAL(vte());
    long ccol = 0, crow = 0;
    vte_terminal_get_cursor_position(t, &ccol, &crow);
    const long cols = vte_terminal_get_column_count(t);
    for (long r = crow; r >= 0 && r > crow - 200; r--) {
        gsize n = 0;
        char* s = vte_terminal_get_text_range_format(t, VTE_FORMAT_TEXT, r, 0, r, cols, &n);
        const std::string line = s ? std::string(s, n) : std::string();
        g_free(s);
        const size_t at = line.find(marker);
        if (at != std::string::npos) {
            *row = r;
            *col = (long)at;   // the lines printed are ASCII
            return true;
        }
    }
    return false;
}

// The middle of the cell `offset` characters into `marker`'s line.
bool point(const std::string& marker, long offset, double* x, double* y) {
    long row = 0, col = 0;
    return find(marker, &row, &col) && term()->cellCenter(row, col + offset, x, y);
}

void moveOnto(const std::string& marker, long offset) {
    double x = 0, y = 0;
    GCHECK(point(marker, offset, &x, &y));
    if (!point(marker, offset, &x, &y)) return;
    MCGuiMoveTo(vte(), x, y);
}

void clickOn(const std::string& modifiers, const std::string& marker, long offset) {
    double x = 0, y = 0;
    GCHECK(point(marker, offset, &x, &y));
    if (!point(marker, offset, &x, &y)) return;
    if (modifiers.empty()) MCGuiClick(vte(), x, y);
    else MCGuiClickWith(modifiers, vte(), x, y);
}

// The hint on screen, inside the terminal, and big enough to read.
bool hintShows(const std::string& text) {
    GtkWidget* h = term() ? term()->linkHintWidget() : nullptr;
    if (hint() != text || !h || !gtk_widget_get_mapped(h)) return false;
    graphene_rect_t b, area;
    GtkWidget* root = gtk_widget_get_parent(h);   // the terminal's overlay
    if (!gtk_widget_compute_bounds(h, root, &b) || !gtk_widget_compute_bounds(root, root, &area))
        return false;
    return b.size.width > 20 && b.size.height > 8 && graphene_rect_contains_rect(&area, &b);
}

bool gShownTerminal = false;
std::string gPathBefore;

}  // namespace

MC_GUI_SUITE(terminal_links) {
    MCGuiHideBrowserPane();
    MCGuiStep("a server for the hyperlink's page", [] {
        gServer = g_threaded_socket_service_new(2);
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
        g_clear_error(&error);
        g_socket_service_start(gServer);
        GCHECK(gPort > 0);
    });
    MCGuiStep("a file to link to; the terminal shown", [] {
        MCGuiWriteFile(target(), "the target\n");
        gShownTerminal = !MCGuiApp().layout->terminalShown();
        if (gShownTerminal) MCGuiApp().layout->toggleTerminal();
    });
    MCGuiWait("the terminal made", 15, [] { return term() && term()->made() && gtk_widget_get_mapped(vte()); });
    auto ran = std::make_shared<std::string>("(not yet)");
    MCGuiStep("printf prints a path, two hyperlinks and a javascript: one", [ran] {
        // The markers are split on the command line (%s), so only the
        // output has them whole.
        const std::string line =
            "printf 'LINKS-%s see links-target.txt:1 here\\n' A; "
            "printf '\\033]8;;" + web() + "\\033\\\\HYPER%s text\\033]8;;\\033\\\\ end\\n' LINK; "
            "printf '\\033]8;;file://" + target() + "\\033\\\\FILE%s\\033]8;;\\033\\\\\\n' LINK; "
            "printf '\\033]8;;javascript:alert(1)\\033\\\\NOTA%s https://example.com/x\\033]8;;\\033\\\\\\n' LINK";
        term()->runLine(line, [ran](const std::string& why) { *ran = why; });
    });
    MCGuiWait("typed and entered", 30, [ran] { return *ran != "(not yet)"; });
    MCGuiStep("with no refusal", [ran] { GCHECK_EQ_STR(*ran, ""); });
    MCGuiWait("the output on screen", 15, [] {
        double x, y;
        return point("LINKS-A see", 0, &x, &y) && point("HYPERLINK text", 0, &x, &y) &&
               point("FILELINK", 0, &x, &y) && point("NOTALINK", 0, &x, &y);
    });

    // ---- a path read from the text
    MCGuiStep("the pointer rests on links-target.txt:1", [] {
        GCHECK_EQ_STR(hint(), "");
        gPathBefore = MCGuiApp().editor->currentPath();
        moveOnto("LINKS-A see", 14);   // "links-target.txt:1" starts at 12
    });
    MCGuiWait("\"Ctrl+click to open\" beside it, inside the terminal", 5, [] { return hintShows(kRest); });
    MCGuiStep("no underline without Ctrl; a move off the link takes it away", [] {
        GCHECK_EQ_STR(kRest, "Ctrl+click to open");
        GCHECK(!term()->linkUnderlined());
        moveOnto("LINKS-A see", 2);   // "LINKS-A", no link
    });
    MCGuiWait("gone", 5, [] { return hint().empty(); });
    MCGuiPause(1.0);   // a negative check: a rest there shows nothing
    MCGuiStep("still none off a link; back onto it", [] {
        GCHECK_EQ_STR(hint(), "");
        moveOnto("LINKS-A see", 16);
    });
    MCGuiWait("the hint again", 5, [] { return hintShows(kRest); });
    MCGuiStep("a plain click", [] { clickOn("", "LINKS-A see", 16); });
    MCGuiWait("the press takes the hint away", 5, [] { return hint().empty(); });
    MCGuiPause(0.6);   // a negative check: nothing opens from a plain click
    MCGuiStep("nothing opened; a rest again, then a key", [] {
        GCHECK_EQ_STR(MCGuiApp().editor->currentPath(), gPathBefore);
        moveOnto("LINKS-A see", 15);
    });
    MCGuiWait("the hint", 5, [] { return hintShows(kRest); });
    MCGuiStep("Shift pressed and let go", [] {
        MCGuiHoldModifier("shift", true);
        MCGuiHoldModifier("shift", false);
    });
    MCGuiWait("a key takes it away", 5, [] { return hint().empty(); });
    MCGuiStep("Ctrl held, the pointer onto the path", [] {
        MCGuiHoldModifier("ctrl", true);
        moveOnto("LINKS-A see", 18);
    });
    MCGuiWait("underlined", 5, [] { return term()->linkUnderlined(); });
    MCGuiStep("and no hint: its text says where it goes", [] {
        GCHECK_EQ_STR(hint(), "");
        MCGuiHoldModifier("ctrl", false);
    });
    MCGuiWait("Ctrl let go: no underline", 5, [] { return !term()->linkUnderlined(); });

    // ---- an OSC 8 hyperlink to a web address
    MCGuiStep("the pointer rests on the hyperlink's text", [] { moveOnto("HYPERLINK text", 3); });
    MCGuiWait("the hint, its address on a second line", 5, [] {
        return hintShows(kRest + "\n" + web());
    });
    MCGuiStep("Ctrl held over it", [] {
        // A picture to look at (MC_GUI_SNAPSHOTS): the hint over the output.
        MCGuiSnapshot(gtk_widget_get_parent(term()->linkHintWidget()), "terminal-link-hint");
        GCHECK_EQ_STR(TermLinks::restHint(false, web()), "Ctrl+click to open\n" + web());
        MCGuiHoldModifier("ctrl", true);
        moveOnto("HYPERLINK text", 5);
    });
    MCGuiWait("the address alone, at once", 5, [] { return hintShows(web()); });
    MCGuiStep("Ctrl let go", [] { MCGuiHoldModifier("ctrl", false); });
    MCGuiWait("the address goes", 5, [] { return hint().empty(); });
    MCGuiStep("Ctrl+click on it", [] { clickOn("ctrl", "HYPERLINK text", 6); });
    MCGuiWait("its page loads in the browser pane", 15, [] {
        const MCGuiAppWindow w = MCGuiApp();
        return w.layout->browserShown() && w.browser &&
               w.browser->tabUrl(w.browser->activeTab()) == web() &&
               w.browser->tabTitle(w.browser->activeTab()) == "Hyperlink";
    });
    MCGuiStep("no hint left after the click", [] {
        const MCGuiAppWindow w = MCGuiApp();
        if (w.browser)
            MCGuiNote("browser %s, tab %d of %d: %s \"%s\"", w.layout->browserShown() ? "shown" : "hidden",
                      w.browser->activeTab(), w.browser->tabCount(),
                      w.browser->tabUrl(w.browser->activeTab()).c_str(),
                      w.browser->tabTitle(w.browser->activeTab()).c_str());
        GCHECK_EQ_STR(hint(), "");
    });
    MCGuiHideBrowserPane();

    // ---- an OSC 8 hyperlink to a file
    MCGuiStep("Ctrl+click on a file hyperlink", [] { clickOn("ctrl", "FILELINK", 2); });
    MCGuiWait("the file opens in the editor", 10, [] {
        return MCGuiApp().editor->currentPath() == target();
    });

    // ---- javascript: is no link, though its text is a web address
    MCGuiStep("Ctrl held over the javascript: hyperlink's URL text", [] {
        MCGuiHoldModifier("ctrl", true);
        moveOnto("NOTALINK", 12);   // in "https://example.com/x"
    });
    MCGuiPause(0.6);   // a negative check: nothing to underline or show
    MCGuiStep("no underline, no address", [] {
        GCHECK(!term()->linkUnderlined());
        GCHECK_EQ_STR(hint(), "");
        MCGuiHoldModifier("ctrl", false);
        moveOnto("NOTALINK", 14);
    });
    MCGuiPause(1.0);   // a negative check: a rest there shows no hint
    MCGuiStep("no hint at rest either", [] { GCHECK_EQ_STR(hint(), ""); });

    // ---- leaving the terminal
    MCGuiStep("back on the path", [] { moveOnto("LINKS-A see", 14); });
    MCGuiWait("the hint", 5, [] { return hintShows(kRest); });
    MCGuiStep("the pointer leaves for the tree", [] { MCGuiMoveTo(MCGuiApp().tree->widget()); });
    MCGuiWait("leaving takes it away", 5, [] { return hint().empty(); });

    MCGuiStep("put back: the file's tab closed, the file gone, the terminal as it was", [] {
        Editor* e = MCGuiApp().editor;
        for (Document* d : e->documents())
            if (d->path() == target()) {
                e->closeTab(d);
                break;
            }
        g_unlink(target().c_str());
        if (gShownTerminal && MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        if (gServer) {
            g_socket_service_stop(gServer);
            g_socket_listener_close(G_SOCKET_LISTENER(gServer));
            g_object_unref(gServer);
            gServer = nullptr;
        }
    });
}
