// HostReattachSuite.cpp — closing a window detaches and opening one
// reattaches (`./build/mutible-guitest host_reattach`; SessionLink.h,
// docs/notes/session-host.md "GTK reattaches (stage 6, step 2)",
// linux/HANDOFF.md item 100).
// The harness pins MUTIBLE_HOST=inprocess; the first part sets it to
// `socket` through EnvLock, as host_link does, with the scratch world's
// socket folder, and ends the host it started by its pid.
//   - headless: a file edited and left unsaved, and a counter running in the
//     terminal; the window closes with Keep Unsaved; `mutible attach --dump`
//     (the real binary) still shows both on the host;
//   - a window opened on the folder: the tab is back under the same id with
//     the unsaved text, unsaved, its undo the host's; the terminal attached,
//     the same shell, its replay showing the counter's earlier output, and
//     new output arriving;
//   - closing that terminal ends its shell; Don't Save on the window's close
//     drops the file on the host;
//   - quit and relaunch: this app's windows closed with Keep Unsaved (its
//     connection gone), a second app process (_host_reattach_child, as the
//     _backups_ suites run one) opens the folder and finds the unsaved file
//     and the running counter, then quits keeping them;
//   - in-process: the same within this process, the host kept while the
//     app runs (SessionLink::keepWhileHolding).
#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"
#include "EnvLock.h"
#include "SessionLink.h"
#include "Terminal.h"
#include "TerminalDock.h"

#include <glib/gstdio.h>
#include <signal.h>

#include <climits>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::string kText = "first line\n";
const std::string kCounter =
    "i=0; while [ $i -lt 100000 ]; do i=$((i+1)); echo tick-$i; echo $i > counter.txt; sleep 0.25; done";

std::string gHostEnv;
bool gHadHostEnv = false;
std::string gDir, gDir2;
size_t gWindows = 0;
int gHostPid = 0;
uint64_t gDocId = 0, gTermId = 0;
int gShellPid = 0, gTicksBefore = 0;

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
std::string notes() { return gDir + "/notes.txt"; }
std::string expectFile() { return rp(MCGuiScratch()) + "/host_reattach/expect"; }

// The counter's last value, as it wrote it.
int ticks(const std::string& dir) { return std::atoi(MCGuiReadFile(dir + "/counter.txt").c_str()); }

// Whether `s` shows tick-n (not tick-n0).
bool showsTick(const std::string& s, int n) {
    const std::string want = "tick-" + std::to_string(n);
    for (size_t at = s.find(want); at != std::string::npos; at = s.find(want, at + 1)) {
        const size_t end = at + want.size();
        if (end >= s.size() || !isdigit((unsigned char)s[end])) return true;
    }
    return false;
}

// The highest tick-N a terminal's last lines show.
int lastTick(Terminal* t) {
    const std::string s = t ? t->recentText(200) : "";
    int best = 0;
    for (size_t at = s.find("tick-"); at != std::string::npos; at = s.find("tick-", at + 5))
        best = std::max(best, std::atoi(s.c_str() + at + 5));
    return best;
}

// `mutible attach --folder DIR --dump`, the real binary.
struct Dump {
    bool done = false;
    int status = -1;
    std::string out;
};
std::shared_ptr<Dump> gDump;
bool dumpUntil(const std::string& dir, const std::function<bool(const Dump&)>& ok) {
    if (gDump && gDump->done && ok(*gDump)) return true;
    if (!gDump || gDump->done) {
        auto d = std::make_shared<Dump>();
        gDump = d;
        MCGuiRunProcess({MCGuiBuildDir() + "/mutible", "attach", "--folder", dir, "--dump"}, {},
                        [d](int status, std::string out, std::string) {
                            d->status = status;
                            d->out = std::move(out);
                            d->done = true;
                        });
    }
    return false;
}

// The window on `dir` (the newest), and its editor's tab of notes.txt.
MCGuiAppWindow windowOn(const std::string& dir) {
    MCGuiAppWindow found;
    for (const MCGuiAppWindow& w : MCGuiAppWindows())
        if (w.rootDir == dir) found = w;
    return found;
}
Document* notesDoc(const std::string& dir) {
    const MCGuiAppWindow w = windowOn(dir);
    return w.editor ? w.editor->documentFor(dir + "/notes.txt") : nullptr;
}
std::string textOf(Document* d) {
    if (!d) return "";
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(d->buffer(), &s, &e);
    char* t = gtk_text_buffer_get_text(d->buffer(), &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

bool buttonUp(const std::string& label) {
    GtkWindow* top = MCGuiTopWindow();
    for (const MCGuiAppWindow& a : MCGuiAppWindows())
        if (top == a.window) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), label);
    return b && gtk_widget_get_width(b) > 0;
}
void answer(const std::string& label) {
    MCGuiSetKeyWindow(MCGuiTopWindow());
    MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), label));
}

// A window on `dir` with notes.txt typed into, unsaved, and the counter
// running in its terminal (the steps of the first and last parts).
void openEditAndCount(const std::string* dirp, const std::string& typed) {
    MCGuiStep("a window on the folder, notes.txt open", [dirp] {
        const std::string& dir = *dirp;
        MCGuiOpenWindow(dir);
        MCGuiSetKeyWindow(nullptr);
        MCGuiOpenFileKept(dir + "/notes.txt");
    });
    MCGuiWait("on show", 10, [dirp] { return textOf(notesDoc(*dirp)) == kText; });
    MCGuiStep("typed at its end", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiStep("typing", [typed] { MCGuiType(typed); });
    MCGuiWait("unsaved", 10, [dirp, typed] {
        Document* d = notesDoc(*dirp);
        return d && textOf(d) == kText + typed && d->dirty();
    });
    MCGuiStep("the counter started in the terminal", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t != nullptr);
        if (t) t->runLine(kCounter, [](const std::string& why) { GCHECK_EQ_STR(why, ""); });
    });
    MCGuiWait("counting", 30, [dirp] { return lastTick(MCGuiApp().terminal) >= 3 && ticks(*dirp) >= 3; });
}

void putBack() {
    if (gHadHostEnv) EnvLock::envSet("MUTIBLE_HOST", gHostEnv.c_str());
    else EnvLock::envUnset("MUTIBLE_HOST");
    EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
}

}  // namespace

MC_GUI_SUITE(host_reattach) {
    MCGuiStep("scratch folders, and MUTIBLE_HOST=socket", [] {
        gWindows = MCGuiAppWindows().size();
        const std::string base = rp(MCGuiScratch()) + "/host_reattach";
        gDir = base + "/one";
        gDir2 = base + "/two";
        for (const std::string& d : {gDir, gDir2}) g_mkdir_with_parents(d.c_str(), 0700);
        MCGuiWriteFile(gDir + "/notes.txt", kText);
        MCGuiWriteFile(gDir2 + "/notes.txt", kText);
        gDir = rp(gDir);
        gDir2 = rp(gDir2);
        const char* host = getenv("MUTIBLE_HOST");
        gHadHostEnv = host != nullptr;
        gHostEnv = host ? host : "";
        const char* runtime = getenv("XDG_RUNTIME_DIR");
        GCHECK(runtime && std::string(runtime).rfind("/tmp/mcgui-", 0) == 0);
        EnvLock::envSet("MUTIBLE_HOST", "socket");
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "2");
    });

    // ---- headless: closed with Keep Unsaved, then opened again
    openEditAndCount(&gDir, "unsaved");
    MCGuiStep("the host, the document and the shell noted", [] {
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket);
        if (!l) return;
        gHostPid = (int)l->startedHost();
        GCHECK(gHostPid > 0 && alive(gHostPid));
        gDocId = notesDoc(gDir)->id();
        gTermId = MCGuiApp().terminal->hostTerminal();
        gShellPid = MCGuiApp().terminal->shellPid();
        GCHECK(gDocId && gTermId && gShellPid > 0);
    });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked, with Keep Unsaved", 10, [] { return buttonUp("Keep Unsaved"); });
    MCGuiStep("Keep Unsaved", [] { answer("Keep Unsaved"); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiWait("`attach --dump`: the file unsaved and the counter running, on the host", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.find(" " + notes() + " (") != std::string::npos &&
                   d.out.find(", unsaved)\n" + kText + "unsaved") != std::string::npos &&
                   d.out.find("== terminal " + std::to_string(gTermId) + " (") != std::string::npos &&
                   d.out.find("shell " + std::to_string(gShellPid)) != std::string::npos &&
                   d.out.find("tick-") != std::string::npos;
        });
    });
    MCGuiStep("the host and the shell run on", [] {
        gDump.reset();
        MCGuiSetKeyWindow(nullptr);
        GCHECK(alive(gHostPid) && alive(gShellPid));
        gTicksBefore = ticks(gDir);
    });
    MCGuiWait("counting with no window", 15, [] { return ticks(gDir) > gTicksBefore + 2; });

    MCGuiStep("a window opened on the folder", [] {
        gTicksBefore = ticks(gDir);
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
    });
    MCGuiStep("the tab is back: the host's document, its unsaved text, its undo", [] {
        Document* d = notesDoc(gDir);
        GCHECK(d != nullptr);
        if (!d) return;
        GCHECK(d->id() == gDocId);
        GCHECK_EQ_STR(textOf(d), kText + "unsaved");
        GCHECK(d->dirty());
        GCHECK(MCGuiApp().editor->active() == d);
        GCHECK(d->canUndo());
    });
    MCGuiStep("the terminal is the host's, the same shell; shown", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t && t->hostTerminal() == gTermId && t->shellPid() == gShellPid);
        if (t) t->focus();   // made: attached, its replay fed
    });
    MCGuiWait("its replay shows the output from before it attached", 30, [] {
        Terminal* t = MCGuiApp().terminal;
        return t && showsTick(t->recentText(200), std::max(1, gTicksBefore - 1));
    });
    MCGuiWait("new output, past what the counter had written as it opened", 30, [] {
        return lastTick(MCGuiApp().terminal) > gTicksBefore + 2;
    });
    MCGuiStep("still the same shell, running", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t->shellPid() == gShellPid && alive(gShellPid) && t->busy());
        // Undo is the host's, through the history the first window made.
        GCHECK(notesDoc(gDir)->undo() == Document::UndoResult::Done);
    });
    MCGuiWait("the typing undone", 10, [] { return textOf(notesDoc(gDir)) == kText; });
    MCGuiStep("typed again", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("again");
    });
    MCGuiWait("unsaved again", 10, [] { return textOf(notesDoc(gDir)) == kText + "again"; });

    MCGuiStep("the terminal closed: its shell ends", [] {
        MCGuiApp().termDock->closePane(MCGuiApp().terminal, false);
    });
    MCGuiWait("ended", 15, [] { return !alive(gShellPid); });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked", 10, [] { return buttonUp("Don't Save"); });
    MCGuiStep("Don't Save", [] { answer("Don't Save"); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiWait("`attach --dump`: no document left on the host", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.rfind("== host " + gDir + "\n", 0) == 0 &&
                   d.out.find("== document ") == std::string::npos &&
                   d.out.find("== terminal " + std::to_string(gTermId) + " ") == std::string::npos;
        });
    });

    // ---- quit and relaunch: a second app process reattaches
    MCGuiStep("the file left as it was on disk", [] {
        gDump.reset();
        MCGuiSetKeyWindow(nullptr);
        GCHECK_EQ_STR(MCGuiReadFile(notes()), kText);
    });
    openEditAndCount(&gDir, "kept");
    MCGuiStep("what the second app should find", [] {
        Document* d = notesDoc(gDir);
        Terminal* t = MCGuiApp().terminal;
        GCHECK(d && t);
        if (!d || !t) return;
        gDocId = d->id();
        gTermId = t->hostTerminal();
        gShellPid = t->shellPid();
        std::ostringstream o;
        o << gDir << "\n" << gDocId << "\n" << gTermId << "\n" << gShellPid << "\n";
        MCGuiWriteFile(expectFile(), o.str());
    });
    MCGuiStep("this app's window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked", 10, [] { return buttonUp("Keep Unsaved"); });
    MCGuiStep("Keep Unsaved", [] { answer("Keep Unsaved"); });
    MCGuiWait("gone, and this app no longer on the host", 15, [] {
        if (MCGuiAppWindows().size() != gWindows) return false;
        for (const auto& l : SessionLink::all())
            if (l->folder() == gDir) return false;
        return true;
    });
    MCGuiAsync("a second app opens the folder and finds both", 120, [](MCGuiDone done) {
        MCGuiSetKeyWindow(nullptr);
        MCGuiRunChild("_host_reattach_child", gDir, [done](int status, std::string out) {
            GCHECK(status == 0);
            if (status != 0) MCGuiNote("the second app (%d):\n%s", status, out.c_str());
            done();
        });
    });
    MCGuiWait("it quit keeping them: still on the host", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.find(", unsaved)\n" + kText + "kept") != std::string::npos &&
                   d.out.find("== terminal " + std::to_string(gTermId) + " (") != std::string::npos;
        });
    });
    MCGuiStep("the host ended by its pid", [] {
        gDump.reset();
        GCHECK(alive(gHostPid));
        kill(gHostPid, SIGTERM);
    });
    MCGuiWait("ended, and its shells with it", 15, [] { return !alive(gHostPid) && !alive(gShellPid); });

    // ---- in-process: within this app
    MCGuiStep("MUTIBLE_HOST=inprocess", [] {
        putBack();
        EnvLock::envSet("MUTIBLE_HOST", "inprocess");
        MCGuiOpenWindow(gDir2);
        MCGuiSetKeyWindow(nullptr);
        MCGuiOpenFileKept(gDir2 + "/notes.txt");
    });
    MCGuiWait("on show", 10, [] { return textOf(notesDoc(gDir2)) == kText; });
    MCGuiStep("typed, then saved", [] {
        GCHECK(MCGuiApp().editor->sessionLink()->kind() == SessionLink::Link::Kind::InProcess);
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
        MCGuiType("inside");
    });
    MCGuiWait("typed", 10, [] { return textOf(notesDoc(gDir2)) == kText + "inside"; });
    MCGuiStep("saved", [] { MCGuiKey("ctrl+s"); });
    MCGuiWait("clean", 10, [] { return !notesDoc(gDir2)->dirty(); });
    MCGuiStep("the counter started", [] {
        gDocId = notesDoc(gDir2)->id();
        MCGuiApp().terminal->runLine(kCounter, [](const std::string&) {});
    });
    MCGuiWait("counting", 30, [] { return lastTick(MCGuiApp().terminal) >= 3; });
    MCGuiStep("the window closes, nothing asked", [] {
        gTermId = MCGuiApp().terminal->hostTerminal();
        gShellPid = MCGuiApp().terminal->shellPid();
        GCHECK(gShellPid > 0);
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the in-process host kept, holding both", [] {
        MCGuiSetKeyWindow(nullptr);
        std::shared_ptr<SessionLink::Link> link;
        for (const auto& l : SessionLink::all())
            if (l->folder() == gDir2) link = l;
        GCHECK(link != nullptr);
        if (!link) return;
        const Json st = link->client().sessionState();
        GCHECK(st["documents"].size() == 1 && st["terminals"].size() >= 1);
        GCHECK(alive(gShellPid));
        gTicksBefore = ticks(gDir2);
    });
    MCGuiStep("a window opened on the folder again", [] {
        MCGuiOpenWindow(gDir2);
        MCGuiSetKeyWindow(nullptr);
        Document* d = notesDoc(gDir2);
        GCHECK(d && d->id() == gDocId && !d->dirty() && d->canUndo());
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t && t->hostTerminal() == gTermId && t->shellPid() == gShellPid);
        if (t) t->focus();
    });
    MCGuiWait("its terminal attached, counting on", 30, [] {
        return lastTick(MCGuiApp().terminal) > gTicksBefore + 2;
    });
    MCGuiStep("closed by hand: the shell ends", [] {
        MCGuiApp().termDock->closePane(MCGuiApp().terminal, false);
    });
    MCGuiWait("ended", 15, [] { return !alive(gShellPid); });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });

    MCGuiStep("everything put back", [] {
        putBack();
        if (alive(gHostPid)) kill(gHostPid, SIGKILL);
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        gDump.reset();
    });
}

// ------------------------------------------ the second app: finds them back
namespace {
std::vector<std::string> gExpect;
Terminal* heldTerminal() {
    const MCGuiAppWindow w = windowOn(gExpect.size() > 0 ? gExpect[0] : "");
    if (!w.termDock) return nullptr;
    for (Terminal* t : w.termDock->all())
        if (gExpect.size() > 2 && t->hostTerminal() == std::strtoull(gExpect[2].c_str(), nullptr, 10)) return t;
    return nullptr;
}
}  // namespace

MC_GUI_SUITE(_host_reattach_child) {
    MCGuiStep("what to find", [] {
        std::istringstream in(MCGuiReadFile(expectFile()));
        for (std::string line; std::getline(in, line);) gExpect.push_back(line);
        GCHECK(gExpect.size() == 4);
        gDir = gExpect.size() > 0 ? gExpect[0] : "";
    });
    MCGuiWait("a window on the folder", 30, [] {
        const MCGuiAppWindow w = windowOn(gDir);
        return w.window && gtk_widget_get_mapped(GTK_WIDGET(w.window));
    });
    MCGuiStep("on the folder's headless host, the file's tab back with its unsaved text", [] {
        const MCGuiAppWindow w = windowOn(gDir);
        MCGuiSetKeyWindow(w.window);
        GCHECK(w.editor->sessionLink() && w.editor->sessionLink()->kind() == SessionLink::Link::Kind::Socket);
        Document* d = notesDoc(gDir);
        GCHECK(d != nullptr);
        if (!d) return;
        GCHECK(std::to_string(d->id()) == gExpect[1]);
        GCHECK_EQ_STR(textOf(d), kText + "kept");
        GCHECK(d->dirty());
        Terminal* t = heldTerminal();
        GCHECK(t != nullptr);
        if (!t) return;
        GCHECK(std::to_string(t->shellPid()) == gExpect[3]);
        gTicksBefore = ticks(gDir);
        w.termDock->focusPane(t);
        t->focus();
    });
    MCGuiWait("its terminal attached: the replay of what came before", 30, [] {
        Terminal* t = heldTerminal();
        return t && showsTick(t->recentText(200), std::max(1, gTicksBefore - 1));
    });
    MCGuiWait("then new output", 30, [] { return lastTick(heldTerminal()) > gTicksBefore + 2; });
    // The app's Quit (the terminal has the keyboard, and Ctrl+Q is its).
    MCGuiStep("quit", [] {
        g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", nullptr);
    });
    MCGuiWait("asked", 15, [] { return buttonUp("Keep Unsaved"); });
    MCGuiStep("Keep Unsaved", [] { answer("Keep Unsaved"); });
    MCGuiWait("every window closes", 15, [] { return windowOn(gDir).window == nullptr; });
}
