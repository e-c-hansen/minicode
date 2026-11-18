// HostPanSuite.cpp — VTE pans a larger pty to the cursor, as tmux does
// (`./build/mutible-guitest host_pan`; docs/notes/session-host.md, "The host's
// own backups, untitled buffers, one path twice, panning", linux/HANDOFF.md
// item 102). A window's
// terminal on the folder's headless host (MUTIBLE_HOST=socket through
// EnvLock); a second renderer over the socket (a Client here) attaches to
// it with a view far larger than the window's and types, so the pty takes
// its size (the size rule, #166) and the window's VTE is drawn at it,
// clipped. Lines printed to the bottom of that larger grid: the part on
// show follows the cursor (Terminal::panRow), VTE moved up under the
// scroller by as much; the other renderer gone, the pty is the window's
// again and nothing is panned.
#include "Harness.h"

#include "Editor.h"
#include "EnvLock.h"
#include "SessionClient.h"
#include "SessionLink.h"
#include "SessionServer.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <vte/vte.h>

#include <climits>
#include <fstream>
#include <memory>
#include <string>

namespace {

std::string gEnv;
bool gHadEnv = false;
std::string gFolder;
int gPanHost = 0;
uint64_t gTerm = 0;

struct Other : Shared::Client::TermListener {
    Shared::Client* client = nullptr;
    std::string out;
    void termOutput(Shared::TermId t, const std::string& b, long long, bool, bool replay) override {
        out += b;
        if (!replay && client) client->ackTerminal(t, b.size());
    }
};
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

// The cursor's row on the program's screen, and whether it is inside the
// part of VTE the scroller shows.
bool cursorOnShow(Terminal* t) {
    GtkWidget* vte = t ? t->vteWidget() : nullptr;
    if (!vte) return false;
    VteTerminal* v = VTE_TERMINAL(vte);
    long col = 0, row = 0;
    vte_terminal_get_cursor_position(v, &col, &row);
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte));
    const long top = adj ? (long)gtk_adjustment_get_upper(adj) - vte_terminal_get_row_count(v) : 0;
    const double ch = (double)vte_terminal_get_char_height(v);
    graphene_rect_t b;
    GtkWidget* scroller = gtk_widget_get_parent(vte);
    if (!gtk_widget_compute_bounds(vte, scroller, &b)) return false;
    // The cursor's cell, in the scroller's coordinates.
    const double y = b.origin.y + (double)(row - top) * ch;
    return y >= -1 && y + ch <= gtk_widget_get_height(scroller) + 1;
}

}  // namespace

MC_GUI_SUITE(host_pan) {
    MCGuiStep("a window on a scratch folder's headless host, its terminal shown", [] {
        char buf[PATH_MAX];
        const std::string base = MCGuiScratch() + "/host_pan";
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
    MCGuiStep("another renderer attaches with a far larger view and types", [] {
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket);
        if (!l) return;
        gPanHost = (int)l->startedHost();
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
        gClient->resizeTerminal(gTerm, 300, 120);
        gClient->terminalInput(gTerm, "for i in $(seq 1 200); do echo pan-line-$i; done\r");
    });
    MCGuiWait("the other renderer sees the lines", 30, [] {
        if (gClient) gClient->dispatch();
        return gOther.out.find("pan-line-200") != std::string::npos;
    });
    MCGuiWait("the window's VTE drawn at the pty's size, clipped", 30, [] {
        if (gClient) gClient->dispatch();
        Terminal* t = MCGuiApp().terminal;
        VteTerminal* v = t ? VTE_TERMINAL(t->vteWidget()) : nullptr;
        return v && t->clipped() && vte_terminal_get_row_count(v) == 120 &&
               vte_terminal_get_column_count(v) == 300;
    });
    MCGuiWait("the lines printed, the part on show following the cursor down", 30, [] {
        if (gClient) gClient->dispatch();
        Terminal* t = MCGuiApp().terminal;
        return t && t->recentText(400).find("pan-line-200") != std::string::npos && t->panRow() > 0 &&
               cursorOnShow(t);
    });
    MCGuiStep("VTE moved up under the scroller by the rows panned", [] {
        Terminal* t = MCGuiApp().terminal;
        GtkWidget* vte = t->vteWidget();
        GCHECK(gtk_widget_get_margin_top(vte) < 0);
        graphene_rect_t b;
        GCHECK(gtk_widget_compute_bounds(vte, gtk_widget_get_parent(vte), &b));
        const double ch = (double)vte_terminal_get_char_height(VTE_TERMINAL(vte));
        GCHECK(b.origin.y <= -(t->panRow() * ch) + 1 && b.origin.y >= -(t->panRow() * ch) - 1);
    });
    MCGuiStep("the other renderer goes", [] {
        if (gClient) gClient->detachTerminal(gTerm);
        if (gClient) gClient->settle();
        if (gLink) gLink->close();
        gClient.reset();
        gLink.reset();
    });
    MCGuiWait("the pty the window's again: nothing clipped or panned", 30, [] {
        Terminal* t = MCGuiApp().terminal;
        return t && !t->clipped() && t->panRow() == 0 && t->panCol() == 0 &&
               gtk_widget_get_margin_top(t->vteWidget()) == 0;
    });
    MCGuiStep("the window closes; the host ended by its pid", [] {
        MCGuiApp().termDock->closePane(MCGuiApp().terminal, false);
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiStep("put back", [] {
        if (gHadEnv) EnvLock::envSet("MUTIBLE_HOST", gEnv.c_str());
        else EnvLock::envUnset("MUTIBLE_HOST");
        EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
        if (aliveP(gPanHost)) kill(gPanHost, SIGTERM);
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("the host ended", 15, [] { return !aliveP(gPanHost); });
}
