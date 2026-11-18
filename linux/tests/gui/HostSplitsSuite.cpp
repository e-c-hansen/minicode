// HostSplitsSuite.cpp — host_splits: a terminal dock's split panes come back
// as splits when a window reattaches (`./build/mutible-guitest host_splits`;
// the core's TermDockRecord.h, the record's "termLayout" beside "terms";
// docs/notes/session-host.md, linux/HANDOFF.md item 106). On the folder's
// headless host (MUTIBLE_HOST=socket through EnvLock, as host_reattach;
// ended by its pid at the end):
//   - one tab split right, then down in the new column, both boundaries
//     moved from the keyboard, the top right pane given the keyboard, a
//     third boundary move reaching the host's record by itself (the dock
//     tells it a third of a second after the last move), and a second tab
//     on show; the window closes (a plain detach: nothing unsaved) and the
//     folder's next window has the same two tabs: the same terminals (host
//     ids and shells) in the same columns and rows, the shares as they were,
//     the same pane active in each tab and the same tab on show; laid out at
//     those shares, and the bottom right pane's replay;
//   - closed again and that pane's shell killed while no window shows it:
//     the next window has the tab without it, the right column one pane,
//     the widths as they were;
//   - closed again and the record rewritten without its layout, as an app
//     from before it writes one (through the link the window had, kept a
//     moment): each terminal comes back as a tab of its own, in the
//     record's order.
#include "Harness.h"
#include "AppSeam.h"
#include "Editor.h"
#include "EnvLock.h"
#include "SessionLink.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <glib/gstdio.h>
#include <signal.h>

#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using Cols = std::vector<std::vector<uint64_t>>;

std::string gHostEnv;
bool gHadHostEnv = false;
std::string gDir;
size_t gWindows = 0;
int gHostPid = 0;
// The terminals: A, B right of it, C below B, and D in a second tab.
struct Pane {
    uint64_t id = 0;
    int pid = 0;
};
Pane gA, gB, gC, gD;
TerminalLayout::Layout gShares;   // the split tab's, as the window closed
std::shared_ptr<SessionLink::Link> gKeep;

std::string rp(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}
bool alive(int pid) {
    if (pid <= 0) return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return false;
    const size_t close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z';
}

TerminalDock* dock() { return MCGuiApp().termDock; }
TerminalGroup* group(int i) { return dock() ? dock()->group(i) : nullptr; }
bool started(Terminal* t) { return t && t->hostTerminal() && t->shellPid() > 0; }
Pane paneOf(Terminal* t) { return t ? Pane{t->hostTerminal(), t->shellPid()} : Pane{}; }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// Tab i's panes as host ids, column by column.
Cols idsOf(int tab) {
    Cols out;
    if (TerminalGroup* g = group(tab))
        for (const auto& column : g->columns()) {
            std::vector<uint64_t> c;
            for (Terminal* t : column) c.push_back(t->hostTerminal());
            out.push_back(c);
        }
    return out;
}
std::string text(const Cols& cols) {
    std::string s = "[";
    for (size_t c = 0; c < cols.size(); c++) {
        s += c ? ",[" : "[";
        for (size_t r = 0; r < cols[c].size(); r++) s += (r ? "," : "") + std::to_string(cols[c][r]);
        s += "]";
    }
    return s + "]";
}
// Every pane of the dock with the shell it had.
bool sameShells(const std::vector<Pane>& want) {
    if (!dock()) return false;
    for (const Pane& p : want) {
        bool found = false;
        for (Terminal* t : dock()->all())
            if (t->hostTerminal() == p.id) found = t->shellPid() == p.pid;
        if (!found) return false;
    }
    return true;
}
bool sharesNear(const TerminalLayout::Fractions& a, const TerminalLayout::Fractions& b, double tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!near(a[i], b[i], tol)) return false;
    return true;
}

// The record on the window's host naming terminal `term` (its tabs).
Json recordNaming(uint64_t term) {
    SessionLink::Link* l = MCGuiApp().editor ? MCGuiApp().editor->sessionLink() : nullptr;
    if (!l) return Json();
    const Json st = l->client().sessionState();
    for (const Json& w : st["windows"].elements())
        for (const Json& t : w["tabs"]["terms"].elements())
            if ((uint64_t)t.asInt() == term) return w;
    return Json();
}

// `mutible attach --folder DIR --dump`, the real binary.
struct Dump {
    bool done = false;
    int status = -1;
    std::string out;
};
std::shared_ptr<Dump> gDump;
bool dumpUntil(const std::function<bool(const Dump&)>& ok) {
    if (gDump && gDump->done && ok(*gDump)) return true;
    if (!gDump || gDump->done) {
        auto d = std::make_shared<Dump>();
        gDump = d;
        MCGuiRunProcess({MCGuiBuildDir() + "/mutible", "attach", "--folder", gDir, "--dump"}, {},
                        [d](int status, std::string out, std::string) {
                            d->status = status;
                            d->out = std::move(out);
                            d->done = true;
                        });
    }
    return false;
}
bool dumpShows(const Dump& d, uint64_t term) {
    return d.out.find("== terminal " + std::to_string(term) + " (") != std::string::npos;
}

void showDock() {
    if (MCGuiApp().layout && !MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
}
void closeWindow() { gtk_window_close(MCGuiApp().window); }
bool closed() { return MCGuiAppWindows().size() == gWindows; }
void openAgain() {
    MCGuiOpenWindow(gDir);
    MCGuiSetKeyWindow(nullptr);
}

void putBack() {
    if (gHadHostEnv) EnvLock::envSet("MUTIBLE_HOST", gHostEnv.c_str());
    else EnvLock::envUnset("MUTIBLE_HOST");
    EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
}

}  // namespace

MC_GUI_SUITE(host_splits) {
    MCGuiStep("a scratch folder, and MUTIBLE_HOST=socket", [] {
        gWindows = MCGuiAppWindows().size();
        gDir = rp(MCGuiScratch()) + "/host_splits/one";
        g_mkdir_with_parents(gDir.c_str(), 0700);
        GCHECK(gDir.rfind(rp(MCGuiScratch()), 0) == 0);   // never the user's own
        MCGuiWriteFile(gDir + "/notes.txt", "notes\n");
        gDir = rp(gDir);
        const char* host = getenv("MUTIBLE_HOST");
        gHadHostEnv = host != nullptr;
        gHostEnv = host ? host : "";
        EnvLock::envSet("MUTIBLE_HOST", "socket");
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "2");
    });

    // ---- two splits and a second tab, closed and opened again
    MCGuiStep("a window on the folder, its dock shown", [] {
        openAgain();
        showDock();
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket);
        gHostPid = l ? (int)l->startedHost() : 0;
        GCHECK(gHostPid > 0 && alive(gHostPid));
        GCHECK(dock() && dock()->count() == 1);
    });
    MCGuiWait("its shell on the host", 30, [] { return started(dock()->at(0)); });
    MCGuiStep("split right, then down in the new column", [] {
        Terminal* b = dock()->split(false, false);
        GCHECK(b != nullptr);
        Terminal* c = b ? dock()->split(true, false) : nullptr;
        GCHECK(c != nullptr);
    });
    MCGuiWait("three shells on the host", 30, [] {
        TerminalGroup* g = group(0);
        if (!g || g->count() != 3) return false;
        for (Terminal* t : g->panes())
            if (!started(t)) return false;
        return true;
    });
    MCGuiStep("both boundaries moved from the keyboard; a line in the bottom right pane", [] {
        TerminalGroup* g = group(0);
        const auto& cols = g->columns();
        GCHECK(cols.size() == 2 && cols[0].size() == 1 && cols[1].size() == 2);
        gA = paneOf(cols[0][0]);
        gB = paneOf(cols[1][0]);
        gC = paneOf(cols[1][1]);
        GCHECK(g->active() == cols[1][1]);
        // The right column wider, the bottom right pane taller.
        GCHECK(dock()->resizeActive(TerminalGroup::Dir::Left));
        GCHECK(dock()->resizeActive(TerminalGroup::Dir::Up));
        // A short prompt first: the pane is a few rows, and a prompt
        // naming a long scratch path (CI's) wraps over them, scrolling the
        // line off the host's screen before the replay (the integration's
        // marks are PROMPT_COMMAND's, not the prompt's).
        cols[1][1]->runLine("PS1='$ '; echo pane-c-$((6*7))", [](const std::string& why) { GCHECK_EQ_STR(why, ""); });
    });
    MCGuiWait("its output", 30, [] {
        TerminalGroup* g = group(0);
        return g && g->columns().size() == 2 && g->columns()[1].size() == 2 &&
               g->columns()[1][1]->recentText(50).find("pane-c-42") != std::string::npos;
    });
    MCGuiStep("a second tab", [] { dock()->newTerminal(); });
    MCGuiWait("its shell on the host", 30, [] { return dock()->count() == 2 && started(dock()->at(1)); });
    // The keyboard, as a click gives it (the dock's toggle gave it to the
    // first pane, which would take the tab's active place back as its tab
    // is shown again).
    MCGuiStep("the top right pane given the keyboard", [] {
        gD = paneOf(dock()->at(1));
        dock()->focusPane(group(0)->columns()[1][0]);
    });
    MCGuiWait("it is the tab's active pane, on show", 10, [] {
        TerminalGroup* g = group(0);
        return dock()->activeIndex() == 0 && g->active() == g->columns()[1][0] &&
               MCGuiApp().terminal == g->columns()[1][0];
    });
    MCGuiStep("its boundary moved", [] {
        TerminalGroup* g = group(0);
        // The right column wider by 4 cells more: told to the host by
        // the dock's own report, a moment after (no other change sends it).
        GCHECK(dock()->resizeActive(TerminalGroup::Dir::Left));
        gShares = g->layout();
        MCGuiNote("columns %.4f %.4f, right column's rows %.4f %.4f", gShares.columns[0], gShares.columns[1],
                  gShares.rows[1][0], gShares.rows[1][1]);
        GCHECK(!near(gShares.columns[0], 0.5, 0.005) && !near(gShares.rows[1][0], 0.5, 0.005));
    });
    MCGuiWait("the host's record has the shares", 10, [] {
        const Json w = recordNaming(gA.id);
        const Json& t = w["tabs"]["termLayout"]["tabs"][0];
        return near(t["columnShares"][0].asNumber(), gShares.columns[0], 1e-4) &&
               near(t["rowShares"][1][0].asNumber(), gShares.rows[1][0], 1e-4) &&
               t["activeColumn"].asInt() == 1 && t["activeRow"].asInt() == 0;
    });
    MCGuiStep("the second tab on show; the window closes (nothing unsaved: no question)", [] {
        const Json w = recordNaming(gA.id);
        MCGuiNote("the record: %s", w["tabs"].dump().c_str());
        GCHECK(w["tabs"]["terms"].dump() ==
               "[" + std::to_string(gA.id) + "," + std::to_string(gB.id) + "," + std::to_string(gC.id) + "," +
                   std::to_string(gD.id) + "]");
        dock()->select(1, false);
        closeWindow();
    });
    MCGuiWait("gone", 15, closed);
    MCGuiStep("the host and the four shells run on", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(alive(gHostPid));
        for (const Pane& p : {gA, gB, gC, gD}) GCHECK(alive(p.pid));
    });
    // Also the host has heard this app's connection go before the next
    // window asks it what is free.
    MCGuiWait("`attach --dump`: the four terminals on the host", 30, [] {
        return dumpUntil([](const Dump& d) {
            return d.status == 0 && dumpShows(d, gA.id) && dumpShows(d, gB.id) && dumpShows(d, gC.id) &&
                   dumpShows(d, gD.id);
        });
    });
    MCGuiStep("a window opened on the folder: the same tabs and panes", [] {
        gDump.reset();
        openAgain();
        GCHECK(dock() && dock()->count() == 2);
        if (!dock() || dock()->count() != 2) return;
        MCGuiNote("tabs %s %s", text(idsOf(0)).c_str(), text(idsOf(1)).c_str());
        GCHECK((idsOf(0) == Cols{{gA.id}, {gB.id, gC.id}}));
        GCHECK((idsOf(1) == Cols{{gD.id}}));
        GCHECK(sameShells({gA, gB, gC, gD}));
        const TerminalLayout::Layout& l = group(0)->layout();
        GCHECK(sharesNear(l.columns, gShares.columns, 2e-4));
        GCHECK(l.rows.size() == 2 && sharesNear(l.rows[0], {1.0}, 1e-9) &&
               sharesNear(l.rows[1], gShares.rows[1], 2e-4));
        // The pane that had the keyboard's place in each tab, the tab on show.
        GCHECK(group(0)->active()->hostTerminal() == gB.id);
        GCHECK(dock()->activeIndex() == 1);
        GCHECK(MCGuiApp().terminal && MCGuiApp().terminal->hostTerminal() == gD.id);
        // Shown with its panes laid out.
        showDock();
        dock()->select(0, false);
        GCHECK(MCGuiApp().terminal && MCGuiApp().terminal->hostTerminal() == gB.id);
    });
    MCGuiWait("laid out at those shares", 15, [] {
        TerminalGroup* g = group(0);
        if (!g || g->columns().size() != 2 || g->columns()[1].size() != 2) return false;
        TerminalLayout::Rect a, b, c;
        if (!g->frameOf(g->columns()[0][0], &a) || !g->frameOf(g->columns()[1][0], &b) ||
            !g->frameOf(g->columns()[1][1], &c) || a.w < 50 || b.h < 20 || c.h < 20)
            return false;
        return near(a.w / (a.w + b.w), gShares.columns[0], 0.03) &&
               near(b.h / (b.h + c.h), gShares.rows[1][0], 0.05) && near(b.x, a.w + TerminalGroup::kGap, 2);
    });
    MCGuiWait("the bottom right pane attached: its replay", 30, [] {
        TerminalGroup* g = group(0);
        return g && g->columns().size() == 2 && g->columns()[1].size() == 2 &&
               g->columns()[1][1]->recentText(50).find("pane-c-42") != std::string::npos;
    });

    // ---- a shell that ends while no window shows it
    MCGuiStep("the window closes", [] { closeWindow(); });
    MCGuiWait("gone", 15, closed);
    MCGuiStep("the bottom right pane's shell killed by its pid", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(alive(gC.pid));
        kill(gC.pid, SIGKILL);
    });
    MCGuiWait("`attach --dump`: the host no longer has it, and has the rest", 30, [] {
        return dumpUntil([](const Dump& d) {
            return d.status == 0 && !dumpShows(d, gC.id) && dumpShows(d, gA.id) && dumpShows(d, gB.id) &&
                   dumpShows(d, gD.id);
        });
    });
    MCGuiStep("a window opened on the folder: the tab without that pane", [] {
        gDump.reset();
        openAgain();
        GCHECK(dock() && dock()->count() == 2);
        if (!dock() || dock()->count() != 2) return;
        MCGuiNote("tabs %s %s", text(idsOf(0)).c_str(), text(idsOf(1)).c_str());
        GCHECK((idsOf(0) == Cols{{gA.id}, {gB.id}}));
        GCHECK((idsOf(1) == Cols{{gD.id}}));
        GCHECK(sameShells({gA, gB, gD}));
        const TerminalLayout::Layout& l = group(0)->layout();
        GCHECK(sharesNear(l.columns, gShares.columns, 2e-4));
        GCHECK(l.rows.size() == 2 && sharesNear(l.rows[1], {1.0}, 1e-9));
        GCHECK(group(0)->active()->hostTerminal() == gB.id);
        GCHECK(dock()->activeIndex() == 0);
    });

    // ---- a record from an app before the layout: a tab each
    MCGuiStep("the window closes, its link kept here a moment", [] {
        for (const auto& l : SessionLink::all())
            if (l->folder() == gDir) gKeep = l;
        GCHECK(gKeep != nullptr);
        closeWindow();
    });
    MCGuiWait("gone", 15, closed);
    MCGuiStep("its record rewritten without the layout, as an older app writes it", [] {
        MCGuiSetKeyWindow(nullptr);
        if (!gKeep) return;
        const Json st = gKeep->client().sessionState();
        uint64_t record = 0;
        Json tabs;
        for (const Json& w : st["windows"].elements())
            if (w["tabs"].has("termLayout")) {
                record = (uint64_t)w["window"].asInt();
                tabs = w["tabs"];
            }
        GCHECK(record != 0);
        Json old = Json::object();
        for (const auto& [key, value] : tabs.items())
            if (key != "termLayout") old.set(key, value);
        MCGuiNote("record %llu, as an older app: %s", (unsigned long long)record, old.dump().c_str());
        GCHECK(old["terms"].size() == 3);
        gKeep->client().windowTabs(record, old);
        GCHECK(gKeep->client().settle());
        bool stripped = false;
        const Json now = gKeep->client().sessionState();
        for (const Json& w : now["windows"].elements())
            if ((uint64_t)w["window"].asInt() == record) stripped = !w["tabs"].has("termLayout");
        GCHECK(stripped);
    });
    // On the kept link (the record is this app's own, so free for it at
    // once), which the window then holds.
    MCGuiStep("a window opened on the folder: a tab each, in the record's order", [] {
        openAgain();
        GCHECK(MCGuiApp().editor->sessionLink() == gKeep.get());
        gKeep.reset();
        GCHECK(dock() && dock()->count() == 3);
        if (!dock() || dock()->count() != 3) return;
        MCGuiNote("tabs %s %s %s", text(idsOf(0)).c_str(), text(idsOf(1)).c_str(), text(idsOf(2)).c_str());
        GCHECK((idsOf(0) == Cols{{gA.id}}));
        GCHECK((idsOf(1) == Cols{{gB.id}}));
        GCHECK((idsOf(2) == Cols{{gD.id}}));
        GCHECK(sameShells({gA, gB, gD}));
        GCHECK(dock()->activeIndex() == 0);
    });
    MCGuiStep("the window closes", [] { closeWindow(); });
    MCGuiWait("gone", 15, closed);
    MCGuiStep("the host ended by its pid", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(alive(gHostPid));
        kill(gHostPid, SIGTERM);
    });
    MCGuiWait("ended, and its shells with it", 15, [] {
        return !alive(gHostPid) && !alive(gA.pid) && !alive(gB.pid) && !alive(gD.pid);
    });
    MCGuiStep("everything put back", [] {
        putBack();
        if (alive(gHostPid)) kill(gHostPid, SIGKILL);
        gKeep.reset();
        gDump.reset();
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}
