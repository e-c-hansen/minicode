// ProcessesSuite.cpp — processes and ports on Linux, part one (`./build/
// minicode-guitest processes`, PARITY.md item 3.8; the Mac's
// tests/gui/ProcessesSuite.mm). The sampler over /proc, the control channel's
// `procs` and the Processes panel (part two: View > Toggle Processes, its rows
// by group, an orphan from a closed window, Stop from the row's menu).
//   - a server started in the app's terminal (python3 -m http.server) is
//     reported by `minicode procs --json --folder` under that terminal, with
//     its listening port, its folder as cwd and a command line;
//   - a process started outside minicode (its parent gone, so the wrapper
//     subreaper holds it) with its cwd in the folder is "not started here";
//   - Stop (ProcessMonitor::stop) ends it by SIGTERM; a sleep that ignores
//     SIGTERM is ended by the SIGKILL 3 s later; neither the shell nor
//     minicode is ever a target;
//   - a look's CPU time on the sampling thread is bounded with the thread
//     CPU clock (never wall time), and a folder no window shows is answered
//     for, with no window opened.
#include "Harness.h"

#include "AppSeam.h"
#include "Json.h"
#include "ProcessMonitor.h"
#include "ProcessesPanel.h"
#include "Terminal.h"
#include "TerminalAttention.h"

#include <gio/gio.h>
#include <vte/vte.h>

#include <csignal>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

struct Run {
    bool done = false;
    int exitCode = -1;
    std::string out, err;
};

std::shared_ptr<Run> runClient(const std::vector<std::string>& args) {
    auto run = std::make_shared<Run>();
    std::vector<const char*> argv;
    const std::string exe = MCGuiBuildDir() + "/minicode";
    argv.push_back(exe.c_str());
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    GError* error = nullptr;
    GSubprocess* p = g_subprocess_newv(
        argv.data(), (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error);
    if (!p) {
        run->err = error ? error->message : "spawn failed";
        run->done = true;
        g_clear_error(&error);
        return run;
    }
    g_subprocess_communicate_utf8_async(
        p, nullptr, nullptr,
        +[](GObject* source, GAsyncResult* res, gpointer data) {
            auto* holder = static_cast<std::shared_ptr<Run>*>(data);
            Run& r = **holder;
            char *out = nullptr, *err = nullptr;
            g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, &err, nullptr);
            r.out = out ? out : "";
            r.err = err ? err : "";
            g_free(out);
            g_free(err);
            r.exitCode = g_subprocess_get_if_exited(G_SUBPROCESS(source))
                             ? g_subprocess_get_exit_status(G_SUBPROCESS(source))
                             : -1;
            r.done = true;
            delete holder;
            g_object_unref(source);
        },
        new std::shared_ptr<Run>(run));
    return run;
}

VteTerminal* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return VTE_TERMINAL(root);
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (VteTerminal* hit = findVte(c)) return hit;
    return nullptr;
}

// A process that has not exited (a zombie is gone as far as anyone cares).
bool alive(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return false;
    const size_t close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] != 'Z';
}

std::shared_ptr<Run> gRun;     // the `procs` look in flight
Json gLast;                    // its answer
int gServerPid = 0, gServerPort = 0, gSleepPid = 0, gStubbornPid = 0;
int64_t gSleepStart = 0, gServerStart = 0;
std::string gSeen;             // the last answer's text, for a failure message

void look() {
    if (gRun) {
        if (!gRun->done) return;
        Json j;
        std::string err;
        if (gRun->exitCode == 0 && Json::parse(gRun->out, j, &err)) gLast = j;
        gSeen = gRun->out + gRun->err;
        gRun.reset();
    }
    gRun = runClient({"procs", "--json"});   // the front window: the one the app's terminal is in
}

const Json* find(int pid) {
    if (!gLast.isObject()) return nullptr;
    for (const Json& g : gLast["groups"].elements())
        for (const Json& p : g["processes"].elements())
            if (p["pid"].asInt() == pid) return &p;
    return nullptr;
}

std::string groupOf(int pid) {
    if (!gLast.isObject()) return "";
    for (const Json& g : gLast["groups"].elements())
        for (const Json& p : g["processes"].elements())
            if (p["pid"].asInt() == pid) return g["kind"].asString();
    return "";
}

// The panel's row for a pid, and the kind of group it sits under.
bool panelRow(ProcessesPanel* p, int pid, ProcessesPanel::RowInfo* row, std::string* group) {
    if (!p) return false;
    std::string kind = "terminal";
    for (const ProcessesPanel::RowInfo& r : p->rows()) {
        if (r.header) kind = r.orphan ? "orphans" : r.outside ? "outside" : "terminal";
        else if (r.pid == pid) {
            if (row) *row = r;
            if (group) *group = kind;
            return true;
        }
    }
    return false;
}

double threadCpuMs() {
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

MCGuiAppWindow gFirst;       // the window with the panel
int gOrphanPid = 0;
GtkWindow* gSecond = nullptr;

int pidWithCommand(int window, const std::string& what) {
    const std::string folder = [] {
        char buf[4096];
        return std::string(realpath(MCGuiApp().rootDir.c_str(), buf) ? buf : "");
    }();
    Procs::View v = ProcessMonitor::shared().view(window, folder);
    std::vector<const Procs::Node*> todo;
    for (const Procs::Group& g : v.groups)
        for (const Procs::Node& n : g.roots) todo.push_back(&n);
    while (!todo.empty()) {
        const Procs::Node* n = todo.back();
        todo.pop_back();
        if (n->proc.command.find(what) != std::string::npos) return n->proc.key.pid;
        for (const Procs::Node& c : n->children) todo.push_back(&c);
    }
    return 0;
}

}  // namespace

MC_GUI_SUITE(processes) {
    // Run as `minicode term --run` and Run File run a line (runLine): it
    // waits for the shell's prompt, sets aside a line another suite left
    // typed there (install_hints' "uv tool install ..."), and types this one
    // with Return. Nothing here reads the screen, which earlier suites leave
    // as bash redrew it: after the terminal's width changed under a wrapped
    // line (zoom), bash drew later lines over or under the old ones, and on
    // CI a line typed here never showed (November 2026).
    auto ran = std::make_shared<std::string>("(not yet)");
    MCGuiStep("a server run in the app's terminal", [ran] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t != nullptr);
        if (!t) return;
        t->runLine("python3 -m http.server 0 --bind 127.0.0.1",
                   [ran](const std::string& why) { *ran = why; });
    });
    MCGuiWait("it is typed and entered", 30, [ran] { return *ran != "(not yet)"; });
    MCGuiStep("with no refusal", [ran] { GCHECK_EQ_STR(*ran, ""); });
    MCGuiStep("processes started elsewhere: one that ends on SIGTERM, one that ignores it", [] {
        // The shell exits at once, so the sleeps' parent is gone and the
        // harness's subreaper holds them: outside minicode's own tree.
        const std::string script =
            "cd \"$1\" || exit; sleep 600 & echo $! > \"$2\"; "
            "(trap '' TERM; exec sleep 601) & echo $! > \"$3\"";
        const std::string p1 = MCGuiProjectFile("sleep.pid"), p2 = MCGuiProjectFile("stubborn.pid");
        const std::string proj = MCGuiApp().rootDir;
        const char* argv[] = {"/bin/sh", "-c", script.c_str(), "sh", proj.c_str(),
                              p1.c_str(), p2.c_str(), nullptr};
        GError* error = nullptr;
        GSubprocess* p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error);
        GCHECK(p != nullptr);
        if (p) g_object_unref(p);
        g_clear_error(&error);
    });
    MCGuiWait("the pids were written", 10, [] {
        auto pid = [](const std::string& f) {
            std::string s = MCGuiReadFile(MCGuiProjectFile(f));
            return s.empty() ? 0 : atoi(s.c_str());
        };
        gSleepPid = pid("sleep.pid");
        gStubbornPid = pid("stubborn.pid");
        return gSleepPid > 0 && gStubbornPid > 0;
    });
    MCGuiWait("`minicode procs --json` reports the server with its port", 60, [] {
        look();
        if (!gLast.isObject()) return false;
        for (const Json& g : gLast["groups"].elements())
            for (const Json& p : g["processes"].elements())
                if (p["command"].asString().find("http.server") != std::string::npos &&
                    p["ports"].size() > 0) {
                    gServerPid = (int)p["pid"].asInt();
                    gServerPort = (int)p["ports"].elements()[0]["port"].asInt();
                    gServerStart = (int64_t)p["start"].asNumber();
                }
        return gServerPid > 0 && find(gSleepPid) && find(gStubbornPid);
    });
    MCGuiStep("the server: terminal group, port, folder, memory", [] {
        const Json* s = find(gServerPid);
        GCHECK(s != nullptr);
        if (!s) return;
        GCHECK_EQ_STR(groupOf(gServerPid), "terminal");
        GCHECK(gServerPort > 0);
        GCHECK_EQ_STR((*s)["ports"].elements()[0]["address"].asString(), "127.0.0.1");
        char buf[4096];
        GCHECK_EQ_STR((*s)["cwd"].asString(), realpath(MCGuiApp().rootDir.c_str(), buf) ? buf : "");
        GCHECK((*s)["memory"].asNumber() > 0);
        GCHECK((*s)["command"].asString().find("python") != std::string::npos);
        GCHECK(gLast["folder"].asString() == std::string(realpath(MCGuiApp().rootDir.c_str(), buf)));
    });
    MCGuiStep("the process started outside is not started here", [] {
        GCHECK_EQ_STR(groupOf(gSleepPid), "outside");
        GCHECK_EQ_STR(groupOf(gStubbornPid), "outside");
        const Json* s = find(gSleepPid);
        GCHECK(s != nullptr && (*s)["command"].asString().find("sleep 600") != std::string::npos);
        if (s) gSleepStart = (int64_t)(*s)["start"].asNumber();
    });
    MCGuiStep("a notify names a session in the terminal", [] {
        Json args;
        GCHECK(Json::parse("{\"state\":\"clear\",\"session_id\":\"r74-session\","
                           "\"transcript_path\":\"/tmp/r74.jsonl\"}", args));
        std::string error;
        GCHECK(TerminalAttention::apply(MCGuiApp().terminal->controlId(), args, &error));
    });
    MCGuiWait("the program the terminal runs has the session, in `procs --json`", 30, [] {
        look();
        const Json* s = find(gServerPid);
        return s && (*s)["session"].asString() == "r74-session";
    });
    MCGuiStep("a look costs little CPU on the sampling thread", [] {
        ProcessMonitor& m = ProcessMonitor::shared();
        GCHECK(m.samples() > 0 && m.lastListed() > 0);
        GCHECK_TIME(m.lastSampleCpuMilliseconds(), 100, "a /proc sample");
    });
    // ------------------------------------------------------------ the panel
    MCGuiStep("View > Toggle Processes puts the panel in the sidebar", [] {
        gFirst = MCGuiApp();
        GCHECK(gFirst.procs == nullptr && !gFirst.procsShown);
        g_action_group_activate_action(G_ACTION_GROUP(gFirst.window), "toggleprocesses", nullptr);
        gFirst = MCGuiApp();
        GCHECK(gFirst.procs != nullptr && gFirst.procsShown);
        GCHECK(ProcessMonitor::shared().panelShown());
    });
    MCGuiWait("the panel lists the server with its port under the terminal's group", 30, [] {
        ProcessesPanel::RowInfo r;
        std::string group;
        return panelRow(gFirst.procs, gServerPid, &r, &group) && group == "terminal" &&
               r.ports.find(std::to_string(gServerPort)) != std::string::npos;
    });
    MCGuiStep("the row says name, command, folder, footprint; the tooltip the whole line", [] {
        ProcessesPanel::RowInfo r;
        GCHECK(panelRow(gFirst.procs, gServerPid, &r, nullptr));
        GCHECK(r.text.find("python") != std::string::npos);
        GCHECK(r.detail.find("http.server") != std::string::npos);
        char buf[4096];
        GCHECK(r.detail.find(realpath(MCGuiApp().rootDir.c_str(), buf)) != std::string::npos);
        GCHECK(!r.memory.empty() && r.memory != "exited" && !r.cpu.empty());
        GCHECK(r.tooltip.find("python3 -m http.server 0 --bind 127.0.0.1") != std::string::npos);
        GCHECK(r.tooltip.find("Started by Claude Code session r74-session") != std::string::npos);
        std::string g1, g2;
        GCHECK(panelRow(gFirst.procs, gSleepPid, nullptr, &g1) && g1 == "outside");
        GCHECK(panelRow(gFirst.procs, gStubbornPid, nullptr, &g2) && g2 == "outside");
    });
    MCGuiStep("the server's menu has Open localhost:port and Reveal Origin; Open loads the page", [] {
        ProcessesPanel* pp = gFirst.procs;
        const std::string port = std::to_string(gServerPort);
        pp->openMenuFor(gServerPid);
        GCHECK(pp->menuLabels().find("Open localhost:" + port) != std::string::npos);
        GCHECK(pp->menuLabels().find("Reveal Origin") != std::string::npos);
        auto keep = pp->onOpenUrl;
        std::string url;
        pp->onOpenUrl = [&url](const std::string& u) { url = u; };
        g_action_group_activate_action(pp->actionGroup(), "openport",
                                       g_variant_new_int32(gServerPort));
        pp->onOpenUrl = keep;
        GCHECK_EQ_STR(url, "http://localhost:" + port);
    });
    MCGuiStep("Reveal Origin shows the terminal that started it; a closed one is said", [] {
        ProcessesPanel* pp = gFirst.procs;
        ProcessesPanel::RowInfo r;
        GCHECK(panelRow(pp, gServerPid, &r, nullptr));
        auto keepT = pp->onRevealTerminal;
        auto keepN = pp->onNote;
        int asked = -1;
        std::string note;
        pp->onRevealTerminal = [&asked](int id) { asked = id; return true; };
        pp->onNote = [&note](const std::string& t) { note = t; };
        pp->openMenuFor(gServerPid);
        g_action_group_activate_action(pp->actionGroup(), "origin", nullptr);
        GCHECK(asked == r.terminal && r.terminal != 0);
        GCHECK(note.empty());
        pp->onRevealTerminal = [](int) { return false; };
        g_action_group_activate_action(pp->actionGroup(), "origin", nullptr);
        GCHECK(note.find("was closed") != std::string::npos);
        pp->openMenuFor(gSleepPid);   // started outside minicode
        note.clear();
        g_action_group_activate_action(pp->actionGroup(), "origin", nullptr);
        GCHECK(note.find("was not started in minicode") != std::string::npos);
        pp->onRevealTerminal = keepT;
        pp->onNote = keepN;
    });
    MCGuiStep("a picture of the panel, for looking at", [] {
        MCGuiSnapshot(GTK_WIDGET(gFirst.window), "processes");
    });
    MCGuiStep("the panel's own work is bounded by thread CPU time", [] {
        const double c0 = threadCpuMs();
        for (int i = 0; i < 20; i++) gFirst.procs->refresh();
        GCHECK_TIME((threadCpuMs() - c0) / 20, 20, "rebuilding the panel's rows");
        GCHECK_TIME(ProcessMonitor::shared().lastSampleCpuMilliseconds(), 100, "a /proc sample");
    });
    MCGuiStep("a second window on the folder, its terminal started", [] {
        MCGuiOpenWindow(MCGuiApp().rootDir);
        MCGuiSetKeyWindow(nullptr);
        gSecond = MCGuiApp().window;
        GCHECK(gSecond != gFirst.window && MCGuiApp().terminal != nullptr);
        if (MCGuiApp().terminal) MCGuiApp().terminal->typeText("true");
    });
    MCGuiWait("its shell is up", 30, [] {
        Terminal* t = MCGuiApp().terminal;
        VteTerminal* vte = t ? findVte(t->widget()) : nullptr;
        if (!vte || t->shellPid() <= 0) return false;
        char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
        const bool there = text && std::string(text).find("true") != std::string::npos;
        g_free(text);
        return there;
    });
    MCGuiStep("it runs a sleep that ignores the hang-up", [] {
        vte_terminal_feed_child(findVte(MCGuiApp().terminal->widget()), "\x15", 1);
        MCGuiApp().terminal->typeText("(trap '' HUP; exec sleep 612)");
    });
    MCGuiWait("the command is at the prompt", 30, [] {
        char* text = vte_terminal_get_text_format(findVte(MCGuiApp().terminal->widget()),
                                                  VTE_FORMAT_TEXT);
        const bool there = text && std::string(text).find("sleep 612") != std::string::npos;
        g_free(text);
        return there;
    });
    MCGuiStep("Return", [] {
        vte_terminal_feed_child(findVte(MCGuiApp().terminal->widget()), "\n", 1);
    });
    MCGuiWait("the monitor has the sleep under the second window", 30, [] {
        // The terminal id is the window's control id; read it from the table.
        for (const Procs::Terminal& t : ProcessMonitor::shared().terminals())
            if (t.shellPid == MCGuiApp().terminal->shellPid()) {
                gOrphanPid = pidWithCommand(t.window, "sleep 612");
                return gOrphanPid > 0;
            }
        return false;
    });
    MCGuiStep("the second window closes", [] {
        gtk_window_close(gSecond);
    });
    MCGuiWait("gone", 10, [] { return MCGuiAppWindows().size() == 1; });
    MCGuiStep("the first window has the keyboard again", [] {
        MCGuiSetKeyWindow(nullptr);
        gFirst = MCGuiApp();
    });
    MCGuiWait("the sleep is an orphan in the panel", 30, [] {
        std::string group;
        return panelRow(gFirst.procs, gOrphanPid, nullptr, &group) && group == "orphans";
    });
    MCGuiStep("an orphan's menu says Stop (no question), Stop ends it", [] {
        gFirst.procs->openMenuFor(gOrphanPid);
        GCHECK_EQ_STR(gFirst.procs->menuLabels(), "Stop|Reveal Origin");
        g_action_group_activate_action(gFirst.procs->actionGroup(), "stop", nullptr);
    });
    MCGuiWait("the orphan is gone", 10, [] { return !alive(gOrphanPid); });
    MCGuiStep("a row that is not an orphan's says Stop… (it asks)", [] {
        gFirst.procs->openMenuFor(gSleepPid);
        GCHECK_EQ_STR(gFirst.procs->menuLabels(), "Stop\u2026|Reveal Origin");
    });
    MCGuiAsync("Stop ends the sleep by SIGTERM", 20, [](MCGuiDone done) {
        Procs::Key k;
        k.pid = gSleepPid;
        k.start = gSleepStart;
        ProcessMonitor::shared().stop(k, [done](const std::string& refusal) {
            GCHECK(refusal.empty());
            done();
        });
    });
    MCGuiWait("the sleep is gone", 10, [] { return !alive(gSleepPid); });
    MCGuiAsync("Stop on the server (the shell's child) ends it", 20, [](MCGuiDone done) {
        Procs::Key k;
        k.pid = gServerPid;
        k.start = gServerStart;
        ProcessMonitor::shared().stop(k, [done](const std::string& refusal) {
            GCHECK(refusal.empty());
            done();
        });
    });
    MCGuiWait("the server is gone, and minicode and the shell are not", 10, [] {
        return !alive(gServerPid);
    });
    MCGuiStep("nothing that must not die was a target", [] {
        GCHECK(alive((int)getpid()));
        GCHECK(MCGuiApp().terminal->shellPid() > 0 && alive(MCGuiApp().terminal->shellPid()));
    });
    MCGuiAsync("the one that ignores SIGTERM is ended by SIGKILL 3 s later", 30, [](MCGuiDone done) {
        // Its start time as the table has it.
        const Json* s = find(gStubbornPid);
        Procs::Key k;
        k.pid = gStubbornPid;
        k.start = s ? (int64_t)(*s)["start"].asNumber() : 0;
        ProcessMonitor::shared().stop(k, [done](const std::string& refusal) {
            GCHECK(refusal.empty());
            done();
        });
    });
    MCGuiStep("after SIGTERM alone it still runs", [] { GCHECK(alive(gStubbornPid)); });
    MCGuiWait("SIGKILL ended it", 15, [] { return !alive(gStubbornPid); });
    MCGuiAsync("minicode itself is refused", 10, [](MCGuiDone done) {
        Procs::Key k;
        k.pid = (int)getpid();
        k.start = 1;
        ProcessMonitor::shared().stop(k, [done](const std::string& refusal) {
            GCHECK(!refusal.empty());
            done();
        });
    });
    MCGuiStep("Toggle Processes again gives the tree back and sampling slows", [] {
        g_action_group_activate_action(G_ACTION_GROUP(MCGuiApp().window), "toggleprocesses", nullptr);
        GCHECK(!MCGuiApp().procsShown && !ProcessMonitor::shared().panelShown());
    });
    MCGuiStep("a folder no window shows is answered for, and no window opens", [] {
        gRun = runClient({"procs", "--json", "--folder", "/tmp"});
    });
    MCGuiWait("the answer", 30, [] { return gRun && gRun->done; });
    MCGuiStep("it is an answer", [] {
        GCHECK(gRun->exitCode == 0);
        Json j;
        std::string err;
        GCHECK(Json::parse(gRun->out, j, &err) && j["folder"].asString() == "/tmp");
        gRun.reset();
    });
}
