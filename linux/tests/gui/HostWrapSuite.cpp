// HostWrapSuite.cpp — VTE at a smaller pty's grid, so plain output wraps
// where the pty wraps (`./build/mutible-guitest host_wrap`;
// docs/notes/session-host.md, "VTE pans", linux/HANDOFF.md item 105). A
// window's terminal on the folder's headless host (MUTIBLE_HOST=socket
// through EnvLock); a second renderer over the socket (a Client here)
// attaches with a view far smaller than the window's and types, so the pty
// takes its size (the size rule, #166). The window's VTE then has exactly
// the pty's columns and rows, laid out at the top left of its frame
// (TerminalFrame.h), and a line longer than the pty with no cursor
// positioning wraps at the pty's width, as does one the window printed
// before, at its own width (VTE rewraps); the rest of the view is the
// terminal's background, as VTE paints its own. VTE keeps its scrollback
// scrolling (the wheel over it and over the empty part), its scroll bar,
// selection and the keyboard (a click in the empty part gives it VTE).
// The window typing takes the pty back (a size it sent never holds VTE),
// and GTK warns of nothing meanwhile.
#include "Harness.h"

#include "Editor.h"
#include "EnvLock.h"
#include "SessionClient.h"
#include "SessionLink.h"
#include "SessionServer.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "TerminalFrame.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <vte/vte.h>

#include <climits>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

const int kCols = 40, kRows = 6;   // the other renderer's view
// The line printed: "Wrap", sixty digits, "End" (67 columns), none of which
// the command line itself shows.
const char* kCommand = "seq 1 40; printf '%s%060d%s\\n' Wrap 7 End\r";
const std::string kFirst = "Wrap" + std::string(36, '0');   // its first 40 columns
const std::string kRest = std::string(23, '0') + "7End";    // the 27 after them
// One the window prints first, while VTE has the view's width: 67 columns
// on one row then, rewrapped at the pty's width once VTE has its grid.
const std::string kEarly = "Early" + std::string(58, '0') + "7End";
const std::string kEarlyFirst = "Early" + std::string(35, '0');

std::string gEnv;
bool gHadEnv = false;
std::string gFolder;
int gHost = 0;
uint64_t gTerm = 0;
int gViewCols = 0, gViewRows = 0;
double gScrolledFrom = 0;

struct Other : Shared::Client::TermListener {
    Shared::Client* client = nullptr;
    std::string out;
    std::vector<std::pair<int, int>> sizes;   // the pty's, each change the host told
    void termOutput(Shared::TermId t, const std::string& b, long long, bool, bool replay) override {
        out += b;
        if (!replay && client) client->ackTerminal(t, b.size());
    }
    void termShell(Shared::TermId, const Shared::TermShell& s) override {
        if (s.cols && (sizes.empty() || sizes.back() != std::make_pair(s.cols, s.rows))) sizes.push_back({s.cols, s.rows});
    }
};
size_t gSizesBefore = 0;
std::unique_ptr<Shared::HostLink> gLink;
std::unique_ptr<Shared::Client> gClient;
Other gOther;

bool aliveP(int pid) {
    if (pid <= 0) return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return false;
    const size_t close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z';
}

VteTerminal* vte() {
    Terminal* t = MCGuiApp().terminal;
    return t && t->vteWidget() ? VTE_TERMINAL(t->vteWidget()) : nullptr;
}

// One row of VTE's buffer as text: "\n" at its end when the line ended
// there, none when it wrapped on to the next row.
std::string rowText(VteTerminal* v, long row) {
    gsize n = 0;
    char* s = vte_terminal_get_text_range_format(v, VTE_FORMAT_TEXT, row, 0, row + 1, 0, &n);
    std::string out = s ? std::string(s, n) : std::string();
    g_free(s);
    return out;
}

// The buffer row a printed line starts on ("Wrap0", "Early0"), or -1.
long rowStarting(VteTerminal* v, const std::string& start) {
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(v));
    if (!adj) return -1;
    const long upper = (long)gtk_adjustment_get_upper(adj);
    for (long r = std::max(0L, upper - 200); r < upper; r++)
        if (rowText(v, r).rfind(start, 0) == 0) return r;
    return -1;
}
long wrapRow(VteTerminal* v) { return rowStarting(v, "Wrap0"); }

bool near(const int a[4], const int b[4]) {
    for (int i = 0; i < 4; i++)
        if (std::abs(a[i] - b[i]) > 2) return false;
    return true;
}

}  // namespace

MC_GUI_SUITE(host_wrap) {
    MCGuiStep("a window on a scratch folder's headless host, its terminal shown", [] {
        char buf[PATH_MAX];
        const std::string base = MCGuiScratch() + "/host_wrap";
        g_mkdir_with_parents(base.c_str(), 0700);
        gFolder = realpath(base.c_str(), buf) ? std::string(buf) : base;
        const char* host = getenv("MUTIBLE_HOST");
        gHadEnv = host != nullptr;
        gEnv = host ? host : "";
        EnvLock::envSet("MUTIBLE_HOST", "socket");
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "2");
        MCGuiOpenWindow(gFolder);
        MCGuiSetKeyWindow(nullptr);
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t != nullptr);
        if (t) t->focus();
    });
    MCGuiWait("its shell on the host", 30, [] {
        Terminal* t = MCGuiApp().terminal;
        return t && t->hostTerminal() && t->shellPid() > 0 && t->vteWidget() &&
               gtk_widget_get_mapped(t->vteWidget());
    });
    MCGuiStep("the view's own grid, larger than the other renderer's", [] {
        VteTerminal* v = vte();
        gViewCols = (int)vte_terminal_get_column_count(v);
        gViewRows = (int)vte_terminal_get_row_count(v);
        MCGuiNote("the view: %d x %d cells", gViewCols, gViewRows);
        GCHECK(gViewCols >= (int)kEarly.size() + 2 && gViewRows > kRows);
        GCHECK(!MCGuiApp().terminal->atPtySize());
        MCGuiType("printf '%s%059d%s\\n' Early 7 End\n");
    });
    MCGuiWait("the window's own long line, on one row at the view's width", 30, [] {
        VteTerminal* v = vte();
        const long r = rowStarting(v, "Early0");
        return r >= 0 && rowText(v, r) == kEarly + "\n";
    });
    MCGuiStep("another renderer attaches with a far smaller view and prints a long line", [] {
        MCGuiLogWatchStart();
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket);
        if (!l) return;
        gHost = (int)l->startedHost();
        gTerm = MCGuiApp().terminal->hostTerminal();
        std::string error;
        gLink = Shared::HostLink::open(l->socketPath(), &error);
        GCHECK(gLink != nullptr);
        if (!gLink) return;
        gClient = std::make_unique<Shared::Client>(Model::Unit::Utf16, "other");
        gLink->connect(*gClient);
        gOther.client = gClient.get();
        GCHECK(gClient->settle());
        gClient->attachTerminal(gTerm, &gOther);
        gClient->resizeTerminal(gTerm, kCols, kRows);
        gClient->terminalInput(gTerm, kCommand);
    });
    MCGuiWait("the other renderer sees the line", 30, [] {
        if (gClient) gClient->dispatch();
        return gOther.out.find("7End") != std::string::npos;
    });
    MCGuiWait("the window's VTE has exactly the pty's columns and rows", 30, [] {
        if (gClient) gClient->dispatch();
        VteTerminal* v = vte();
        return v && MCGuiApp().terminal->atPtySize() && vte_terminal_get_column_count(v) == kCols &&
               vte_terminal_get_row_count(v) == kRows;
    });
    MCGuiWait("the line wraps where the pty wraps", 30, [] {
        if (gClient) gClient->dispatch();
        VteTerminal* v = vte();
        const long r = v ? wrapRow(v) : -1;
        return r >= 0 && rowText(v, r) == kFirst && rowText(v, r + 1).rfind(kRest, 0) == 0;
    });
    MCGuiStep("the rows on either side of the wrap, as read", [] {
        VteTerminal* v = vte();
        const long r = wrapRow(v);
        MCGuiNote("row %ld: \"%s\" (%zu columns, no line end: wrapped)", r, rowText(v, r).c_str(),
                  rowText(v, r).size());
        std::string next = rowText(v, r + 1);
        while (!next.empty() && next.back() == '\n') next.pop_back();
        MCGuiNote("row %ld: \"%s\"", r + 1, next.c_str());
        GCHECK(rowText(v, r).size() == (size_t)kCols);
        // What VTE had before it took the pty's grid is rewrapped there too.
        const long e = rowStarting(v, "Early0");
        GCHECK_EQ_STR(rowText(v, e), kEarlyFirst);
    });
    MCGuiStep("VTE at the frame's top left, the rest of the view the terminal's background", [] {
        Terminal* t = MCGuiApp().terminal;
        GtkWidget* frame = t->frameWidget();
        const graphene_rect_t c = terminalFrameChildRect(frame);
        const int fw = gtk_widget_get_width(frame), fh = gtk_widget_get_height(frame);
        const int right = (int)(c.origin.x + c.size.width), bottom = (int)(c.origin.y + c.size.height);
        MCGuiNote("VTE %.0f x %.0f at (%.0f, %.0f) in a frame of %d x %d", c.size.width, c.size.height, c.origin.x,
                  c.origin.y, fw, fh);
        GCHECK(c.origin.x == 0 && c.origin.y == 0);
        GCHECK(right < fw && bottom < fh);
        GCHECK(gtk_widget_get_margin_start(t->vteWidget()) == 0 && gtk_widget_get_margin_top(t->vteWidget()) == 0);
        // VTE's own background (its bottom padding, below its last row),
        // the empty part to its right, and the empty part below it.
        int own[4] = {0}, beside[4] = {0}, below[4] = {0};
        GCHECK(MCGuiPixel(frame, 3, bottom - 3, own));
        GCHECK(MCGuiPixel(frame, (right + fw) / 2, fh / 2, beside));
        GCHECK(MCGuiPixel(frame, 10, (bottom + fh) / 2, below));
        MCGuiNote("VTE's background %d,%d,%d,%d; beside it %d,%d,%d,%d; below it %d,%d,%d,%d", own[0], own[1], own[2],
                  own[3], beside[0], beside[1], beside[2], beside[3], below[0], below[1], below[2], below[3]);
        GCHECK(own[3] > 0);
        GCHECK(near(own, beside));
        GCHECK(near(own, below));
    });
    MCGuiStep("a click in the empty part gives VTE the keyboard", [] {
        gtk_window_set_focus(MCGuiApp().window, nullptr);
        GtkWidget* frame = MCGuiApp().terminal->frameWidget();
        const graphene_rect_t c = terminalFrameChildRect(frame);
        const double right = c.origin.x + c.size.width;
        MCGuiClick(frame, (right + gtk_widget_get_width(frame)) / 2, gtk_widget_get_height(frame) / 2.0);
    });
    MCGuiWait("VTE has the keyboard", 10, [] {
        return gtk_window_get_focus(MCGuiApp().window) == MCGuiApp().terminal->vteWidget();
    });
    MCGuiStep("a drag over the wrapped line selects in VTE", [] {
        VteTerminal* v = vte();
        GtkWidget* w = GTK_WIDGET(v);
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(v));
        const long screenTop = (long)gtk_adjustment_get_value(adj);
        const long r = wrapRow(v);
        const double cw = (double)vte_terminal_get_char_width(v), ch = (double)vte_terminal_get_char_height(v);
        // The cells start inside VTE's padding (Terminal.cpp's cellInset).
        GtkBorder pad{};
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_style_context_get_padding(gtk_widget_get_style_context(w), &pad);
        G_GNUC_END_IGNORE_DEPRECATIONS
        const double y = pad.top + ((double)(r - screenTop) + 0.5) * ch;
        MCGuiNote("the line on screen row %ld of %d", r - screenTop, kRows);
        GCHECK(r - screenTop >= 0 && r - screenTop < kRows);
        vte_terminal_unselect_all(v);
        MCGuiDrag(w, pad.left + 0.5 * cw, y, pad.left + 10.5 * cw, y);
    });
    MCGuiWait("VTE has a selection", 10, [] { return vte_terminal_get_has_selection(vte()); });
    MCGuiStep("the wheel over VTE scrolls its scrollback", [] {
        VteTerminal* v = vte();
        vte_terminal_unselect_all(v);
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(v));
        gScrolledFrom = gtk_adjustment_get_value(adj);
        MCGuiNote("scrollback: value %.0f of %.0f, a page of %.0f", gScrolledFrom, gtk_adjustment_get_upper(adj),
                  gtk_adjustment_get_page_size(adj));
        GCHECK(gScrolledFrom > 0);
        MCGuiScroll(GTK_WIDGET(v), -3);
    });
    MCGuiWait("scrolled up", 10, [] {
        return gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte()))) < gScrolledFrom;
    });
    MCGuiStep("the wheel over the empty part scrolls it back down", [] {
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte()));
        gScrolledFrom = gtk_adjustment_get_value(adj);
        MCGuiNote("scrolled up to %.0f", gScrolledFrom);
        GtkWidget* frame = MCGuiApp().terminal->frameWidget();
        const graphene_rect_t c = terminalFrameChildRect(frame);
        GCHECK(gtk_widget_get_width(frame) / 2.0 > c.origin.x + c.size.width);   // its center is the empty part
        MCGuiScroll(frame, 3);
    });
    MCGuiWait("scrolled down", 10, [] {
        return gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte()))) > gScrolledFrom;
    });
    MCGuiStep("the window types: the pty takes its size back", [] {
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte()));
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
        MCGuiApp().terminal->focus();
        if (gClient) gClient->dispatch();
        gSizesBefore = gOther.sizes.size();
        GCHECK(gSizesBefore > 0 && gOther.sizes.back() == std::make_pair(kCols, kRows));
        MCGuiType("echo mine\n");
    });
    MCGuiWait("VTE at the view's own grid again, nothing held", 30, [] {
        if (gClient) gClient->dispatch();
        VteTerminal* v = vte();
        Terminal* t = MCGuiApp().terminal;
        if (!v || t->atPtySize()) return false;
        const graphene_rect_t c = terminalFrameChildRect(t->frameWidget());
        return vte_terminal_get_column_count(v) == gViewCols && vte_terminal_get_row_count(v) == gViewRows &&
               (int)c.size.width == gtk_widget_get_width(t->frameWidget());
    });
    MCGuiStep("the pty went straight to the view's own grid: the size it sent while held was VTE's own", [] {
        if (gClient) gClient->dispatch();
        std::string seen;
        for (size_t i = gSizesBefore; i < gOther.sizes.size(); i++)
            seen += std::to_string(gOther.sizes[i].first) + "x" + std::to_string(gOther.sizes[i].second) + " ";
        MCGuiNote("the pty's sizes since the window typed: %s", seen.c_str());
        GCHECK(gOther.sizes.size() == gSizesBefore + 1 &&
               gOther.sizes.back() == std::make_pair(gViewCols, gViewRows));
    });
    MCGuiStep("the other renderer goes", [] {
        if (gClient) gClient->detachTerminal(gTerm);
        if (gClient) gClient->settle();
        if (gLink) gLink->close();
        gClient.reset();
        gLink.reset();
    });
    MCGuiStep("no Gtk-WARNING or Gtk-CRITICAL meanwhile", [] {
        int gtk = 0;
        for (const std::string& line : MCGuiLogWatchStop()) {
            MCGuiNote("logged: %s", line.c_str());
            if (line.rfind("Gtk-", 0) == 0) gtk++;
        }
        GCHECK(gtk == 0);
    });
    MCGuiStep("the window closes; the host ended by its pid", [] {
        MCGuiApp().termDock->closePane(MCGuiApp().terminal, false);
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiStep("put back", [] {
        if (gHadEnv) EnvLock::envSet("MUTIBLE_HOST", gEnv.c_str());
        else EnvLock::envUnset("MUTIBLE_HOST");
        EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
        if (aliveP(gHost)) kill(gHost, SIGTERM);
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("the host ended", 15, [] { return !aliveP(gHost); });
}
