// HostLinkSuite.cpp — a window attached to its folder's headless session
// host over the socket (`./build/mutible-guitest host_link`; SessionLink.h,
// docs/notes/session-host.md "GTK attaches (stage 6, step 1)",
// linux/HANDOFF.md item 99).
// The harness pins MUTIBLE_HOST=inprocess for every suite; this one sets it
// to `socket` (and once unsets it under editor.session-host = headless)
// through EnvLock, with MUTIBLE_HOST_IDLE_MINUTES small, and puts both back.
// The socket folder is the scratch world's ($XDG_RUNTIME_DIR under
// /tmp/mcgui-*), never the session's own. Every host started here is ended
// by its pid before the suite ends (the last step makes sure); the wrapper's
// subreaper reaps them.
//   - a window on a scratch folder starts `mutible host --folder` (this
//     program, detached, its parent not the app) and attaches: its socket
//     is there, in the scratch socket folder;
//   - typing goes through it: `mutible attach --folder DIR --dump` (the real
//     binary, a subprocess) prints the typed text as unsaved; undo is the
//     host's; a terminal's shell is the host's child, and what it printed
//     is in the dump;
//   - a second window on the folder shares the link (no second host);
//   - closing the windows leaves the host running, and a new window finds
//     it rather than starting another;
//   - the host killed: the window's status bar and the terminal say so,
//     typing, undo and a new tab neither crash nor wait;
//   - editor.session-host = headless picks the headless host with
//     MUTIBLE_HOST unset; `on` keeps it in the app.
#include "Harness.h"

#include "DocumentView.h"
#include "Editor.h"
#include "EnvLock.h"
#include "SessionLink.h"
#include "Terminal.h"

#include <glib/gstdio.h>
#include <signal.h>
#include <sys/stat.h>

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

std::string gHostEnv, gSettings;
bool gHadHostEnv = false;
std::string gDir, gDir2, gDir3;
size_t gWindows = 0;
int gHostPid = 0, gHostPid2 = 0;
std::string gSocket;
GtkWindow* gWin1 = nullptr;
SessionLink::Link* gLink1 = nullptr;

std::string rp(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

// A process that has not exited (a zombie is gone as far as anyone cares;
// the harness's subreaper reaps the hosts when it gets to them).
bool alive(int pid) {
    if (pid <= 0) return false;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return false;
    const size_t close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z';
}
int parentOf(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return 0;
    const size_t close = line.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream rest(line.substr(close + 2));
    std::string state;
    int ppid = 0;
    rest >> state >> ppid;
    return ppid;
}
std::string cmdline(int pid) {
    std::string s = MCGuiReadFile("/proc/" + std::to_string(pid) + "/cmdline");
    for (char& c : s)
        if (c == '\0') c = ' ';
    return s;
}
bool isSocket(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

// The terminal's last lines, scrollback included: the panel is hidden, so
// VTE's screen is a row or two.
std::string terminalText(Terminal* t) { return t ? t->recentText(100) : ""; }

// `mutible attach --folder DIR --dump`, the real binary, as a subprocess.
struct Dump {
    bool done = false;
    int status = -1;
    std::string out, err;
};
std::shared_ptr<Dump> gDump;
void dump(const std::string& dir) {
    auto d = std::make_shared<Dump>();
    gDump = d;
    MCGuiRunProcess({MCGuiBuildDir() + "/mutible", "attach", "--folder", dir, "--dump"}, {},
                    [d](int status, std::string out, std::string err) {
                        d->status = status;
                        d->out = std::move(out);
                        d->err = std::move(err);
                        d->done = true;
                    });
}
// Waits for a dump that `ok` accepts, asking again while it does not.
bool dumpUntil(const std::string& dir, const std::function<bool(const Dump&)>& ok) {
    if (!gDump || !gDump->done) {
        if (!gDump) dump(dir);
        return false;
    }
    if (ok(*gDump)) return true;
    dump(dir);
    return false;
}

Document* doc() { return MCGuiApp().editor ? MCGuiApp().editor->active() : nullptr; }
std::string bufferText() {
    Document* d = doc();
    if (!d) return "";
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(d->buffer(), &s, &e);
    char* t = gtk_text_buffer_get_text(d->buffer(), &s, &e, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}
std::string statusText() {
    GtkWidget* l = MCGuiApp().statusLabel;
    return l ? gtk_label_get_text(GTK_LABEL(l)) : "";
}

bool questionUp() {
    GtkWindow* top = MCGuiTopWindow();
    for (const MCGuiAppWindow& a : MCGuiAppWindows())
        if (top == a.window) return false;
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(top), "Don't Save");
    return b && gtk_widget_get_width(b) > 0;
}
void closeExtraWindows() {
    const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
    for (size_t i = gWindows; i < all.size(); i++) gtk_window_close(all[i].window);
}
void endHost(int& pid) {
    if (alive(pid)) kill(pid, SIGKILL);
}

}  // namespace

MC_GUI_SUITE(host_link) {
    MCGuiStep("scratch folders, and MUTIBLE_HOST=socket with a short idle time", [] {
        gWindows = MCGuiAppWindows().size();
        const std::string base = rp(MCGuiScratch()) + "/host_link";
        gDir = base + "/one";
        gDir2 = base + "/two";
        gDir3 = base + "/three";
        for (const std::string& d : {gDir, gDir2, gDir3}) g_mkdir_with_parents(d.c_str(), 0700);
        MCGuiWriteFile(gDir + "/notes.txt", kText);
        gDir = rp(gDir);
        gDir2 = rp(gDir2);
        gDir3 = rp(gDir3);
        const char* host = getenv("MUTIBLE_HOST");
        gHadHostEnv = host != nullptr;
        gHostEnv = host ? host : "";
        const char* settings = getenv("MUTIBLE_SETTINGS");
        gSettings = settings ? MCGuiReadFile(settings) : "";
        // The socket folder is the scratch world's, never the session's.
        const char* runtime = getenv("XDG_RUNTIME_DIR");
        GCHECK(runtime && std::string(runtime).rfind("/tmp/mcgui-", 0) == 0);
        GCHECK(!getenv("MUTIBLE_CONTROL_DIR"));
        EnvLock::envSet("MUTIBLE_HOST", "socket");
        EnvLock::envSet("MUTIBLE_HOST_IDLE_MINUTES", "2");
    });
    MCGuiStep("a window on the folder", [] {
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
        gWin1 = MCGuiApp().window;
        GCHECK(MCGuiAppWindows().size() == gWindows + 1 && MCGuiApp().rootDir == gDir);
    });
    MCGuiStep("it started the folder's headless host and attached over its socket", [] {
        SessionLink::Link* l = MCGuiApp().editor ? MCGuiApp().editor->sessionLink() : nullptr;
        GCHECK(l != nullptr);
        if (!l) return;
        gLink1 = l;
        GCHECK(l->kind() == SessionLink::Link::Kind::Socket);
        GCHECK_EQ_STR(l->folder(), gDir);
        gHostPid = (int)l->startedHost();
        gSocket = l->socketPath();
        GCHECK(gHostPid > 0 && alive(gHostPid));
        GCHECK(isSocket(gSocket));
        GCHECK(gSocket.rfind(std::string(getenv("XDG_RUNTIME_DIR")) + "/mutible/host-", 0) == 0);
        // This program, run as `host --folder`, detached: not the app's child.
        const std::string cmd = cmdline(gHostPid);
        GCHECK(cmd.find(" host --folder " + gDir) != std::string::npos);
        GCHECK(cmd.find("--idle-minutes 2") != std::string::npos);
        GCHECK(parentOf(gHostPid) != (int)getpid());
        GCHECK(l->client().idBase() >= Shared::kIdRange && !l->lost());
    });

    // ---- a file edited through it
    MCGuiStep("its file opened", [] { MCGuiOpenFileKept(gDir + "/notes.txt"); });
    MCGuiWait("on show", 10, [] { return doc() && doc()->path() == gDir + "/notes.txt" && bufferText() == kText; });
    MCGuiStep("the document is the link's; the caret at the end", [] {
        GCHECK(doc()->sessionLink() == gLink1);
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiStep("typed", [] { MCGuiType("typed here"); });
    MCGuiWait("in the buffer", 10, [] { return bufferText() == kText + "typed here"; });
    MCGuiWait("`mutible attach --dump` prints it as unsaved", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.find(kText + "typed here") != std::string::npos;
        });
    });
    MCGuiStep("the host's folder, the document unsaved", [] {
        GCHECK(gDump->out.rfind("== host " + gDir + "\n", 0) == 0);
        GCHECK(gDump->out.find(", unsaved)\n" + kText + "typed here") != std::string::npos);
        gDump.reset();
    });
    MCGuiStep("Ctrl+Z: the host's undo", [] {
        GCHECK(doc()->canUndo());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("the typing gone", 10, [] { return bufferText() == kText; });
    MCGuiWait("from the host's text too", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.find("typed here") == std::string::npos &&
                   d.out.find(")\n" + kText) != std::string::npos;
        });
    });
    MCGuiStep("and redo is there", [] {
        GCHECK(doc()->canRedo() && !doc()->dirty());
        gDump.reset();
    });

    // ---- a terminal on it
    MCGuiStep("the window's terminal runs a command", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t != nullptr);
        if (!t) return;
        GCHECK(t->sessionLink() == gLink1);
        t->runLine("echo host-link-$((6*7))", [](const std::string& why) { GCHECK_EQ_STR(why, ""); });
    });
    MCGuiWait("its output on the screen", 30, [] {
        return terminalText(MCGuiApp().terminal).find("host-link-42") != std::string::npos;
    });
    MCGuiStep("its shell is the host's child", [] {
        const int shell = MCGuiApp().terminal->shellPid();
        GCHECK(shell > 0 && parentOf(shell) == gHostPid);
    });
    MCGuiWait("the output is in `attach --dump`", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            const size_t term = d.out.find("== terminal ");
            return d.status == 0 && term != std::string::npos &&
                   d.out.find("host-link-42", term) != std::string::npos;
        });
    });

    // ---- a second window on the folder
    MCGuiStep("a second window on the folder shares the link", [] {
        gDump.reset();
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
        GCHECK(MCGuiAppWindows().size() == gWindows + 2 && MCGuiApp().window != gWin1);
        GCHECK(MCGuiApp().editor->sessionLink() == gLink1);
        int links = 0;
        for (const auto& l : SessionLink::all()) links += l->folder() == gDir;
        GCHECK(links == 1);
        GCHECK(gLink1->startedHost() == gHostPid);
    });
    MCGuiStep("it closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("gone", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("the first window closes too", [] {
        MCGuiSetKeyWindow(nullptr);
        GCHECK(MCGuiApp().window == gWin1 && !gLink1->lost());
        gtk_window_close(gWin1);
    });
    MCGuiWait("gone, with nothing asked", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the host runs on, its socket there; the link let go", [] {
        gLink1 = nullptr;
        gWin1 = nullptr;
        GCHECK(alive(gHostPid) && isSocket(gSocket));
        int links = 0;
        for (const auto& l : SessionLink::all()) links += l->folder() == gDir;
        GCHECK(links == 0);
    });
    MCGuiWait("`attach --dump` still reaches it", 30, [] {
        return dumpUntil(gDir, [](const Dump& d) {
            return d.status == 0 && d.out.rfind("== host " + gDir + "\n", 0) == 0;
        });
    });

    // ---- found again, then killed
    MCGuiStep("a new window on the folder finds the running host", [] {
        gDump.reset();
        MCGuiOpenWindow(gDir);
        MCGuiSetKeyWindow(nullptr);
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket);
        if (!l) return;
        GCHECK(l->startedHost() == 0 && l->socketPath() == gSocket);
        gLink1 = l;
        MCGuiApp().terminal->runLine("echo before-$((2+3))", [](const std::string&) {});
    });
    MCGuiWait("its shell up", 30, [] {
        return terminalText(MCGuiApp().terminal).find("before-5") != std::string::npos;
    });
    MCGuiStep("the file opened in it", [] { MCGuiOpenFileKept(gDir + "/notes.txt"); });
    MCGuiWait("on show", 10, [] { return doc() && bufferText() == kText; });
    MCGuiStep("the host killed, by the pid this suite saw it start with", [] {
        GCHECK(alive(gHostPid));
        kill(gHostPid, SIGKILL);
    });
    MCGuiWait("the link hears it", 10, [] { return gLink1->lost(); });
    MCGuiWait("the window's status bar says so", 10, [] {
        return statusText().find("session's host") != std::string::npos &&
               statusText().find("went away") != std::string::npos;
    });
    MCGuiWait("and the terminal", 10, [] {
        return terminalText(MCGuiApp().terminal).find("host went away") != std::string::npos;
    });
    MCGuiStep("typing, undo and a new tab go on without waiting for it", [] {
        const gint64 t0 = g_get_monotonic_time();
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        GCHECK(!doc()->canUndo());
        GCHECK(doc()->undo() == Document::UndoResult::Nothing);
        MCGuiWriteFile(gDir + "/other.txt", "other\n");
        MCGuiOpenFileKept(gDir + "/other.txt");
        GCHECK(MCGuiApp().terminal->shellPid() == 0);
        GCHECK_TIME((g_get_monotonic_time() - t0) / 1000.0, 2000, "the calls that waited for the host");
    });
    MCGuiStep("typing in it", [] { MCGuiType("x"); });
    MCGuiWait("typed", 10, [] { return bufferText() == "xother\n" || bufferText() == "other\nx"; });
    MCGuiStep("the window closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("asked about the unsaved tab", 10, questionUp);
    MCGuiStep("Don't Save", [] {
        MCGuiSetKeyWindow(MCGuiTopWindow());
        MCGuiClick(MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), "Don't Save"));
    });
    MCGuiWait("gone", 15, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("nothing left of it", [] {
        MCGuiSetKeyWindow(nullptr);
        gLink1 = nullptr;
        GCHECK(!alive(gHostPid));
    });

    // ---- the setting
    MCGuiStep("MUTIBLE_HOST unset, editor.session-host = headless", [] {
        EnvLock::envUnset("MUTIBLE_HOST");
        MCGuiSetSettings(gSettings + "\neditor.session-host = headless\n");
        MCGuiOpenWindow(gDir2);
        MCGuiSetKeyWindow(nullptr);
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::Socket && l->startedHost() > 0);
        if (!l) return;
        gHostPid2 = (int)l->startedHost();
        GCHECK(alive(gHostPid2) && isSocket(l->socketPath()) && l->socketPath() != gSocket);
    });
    MCGuiStep("it closes", [] { gtk_window_close(MCGuiApp().window); });
    MCGuiWait("gone", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("its host ended by its pid (SIGTERM: it stops as its lifetime would)", [] {
        GCHECK(alive(gHostPid2));
        kill(gHostPid2, SIGTERM);
    });
    MCGuiWait("ended", 10, [] { return !alive(gHostPid2); });
    MCGuiStep("editor.session-host = on keeps the host in the app", [] {
        MCGuiSetSettings(gSettings + "\neditor.session-host = on\n");
        MCGuiOpenWindow(gDir3);
        MCGuiSetKeyWindow(nullptr);
        SessionLink::Link* l = MCGuiApp().editor->sessionLink();
        GCHECK(l && l->kind() == SessionLink::Link::Kind::InProcess && l->startedHost() == 0);
        gtk_window_close(MCGuiApp().window);
    });
    MCGuiWait("gone", 10, [] { return MCGuiAppWindows().size() == gWindows; });

    MCGuiStep("everything put back; no host of this suite's left", [] {
        closeExtraWindows();
        endHost(gHostPid);
        endHost(gHostPid2);
        if (gHadHostEnv) EnvLock::envSet("MUTIBLE_HOST", gHostEnv.c_str());
        else EnvLock::envUnset("MUTIBLE_HOST");
        EnvLock::envUnset("MUTIBLE_HOST_IDLE_MINUTES");
        MCGuiSetSettings(gSettings);
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();   // the X focus back on the app's window
        gDump.reset();
    });
    MCGuiWait("the hosts gone", 10, [] { return !alive(gHostPid) && !alive(gHostPid2); });
}
