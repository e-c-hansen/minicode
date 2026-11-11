// ProcessesSuite.cpp — processes and ports on Linux, part one (`./build/
// minicode-guitest processes`, PARITY.md item 3.8; the Mac's
// tests/gui/ProcessesSuite.mm). No panel yet: what is checked is the sampler
// over /proc and the control channel's `procs`.
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
#include "Terminal.h"

#include <gio/gio.h>
#include <vte/vte.h>

#include <csignal>
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

}  // namespace

MC_GUI_SUITE(processes) {
    MCGuiStep("a server typed into the app's terminal and run", [] {
        Terminal* t = MCGuiApp().terminal;
        GCHECK(t != nullptr);
        if (!t) return;
        // Other suites leave a typed, unrun line at the prompt (install
        // hints' "uv tool install ..."): make the terminal, then clear the line.
        t->typeText("true");
    });
    MCGuiWait("the terminal has its shell", 30, [] {
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        if (!vte || MCGuiApp().terminal->shellPid() <= 0) return false;
        char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
        const bool there = text && std::string(text).find("true") != std::string::npos;
        g_free(text);
        return there;
    });
    MCGuiStep("the line is cleared and the server typed", [] {
        vte_terminal_feed_child(findVte(MCGuiApp().terminal->widget()), "\x15", 1);   // Ctrl+U
        MCGuiApp().terminal->typeText("python3 -m http.server 0 --bind 127.0.0.1");
    });
    MCGuiWait("the command is at the prompt", 30, [] {
        VteTerminal* vte = findVte(MCGuiApp().terminal->widget());
        if (!vte) return false;
        char* text = vte_terminal_get_text_format(vte, VTE_FORMAT_TEXT);
        const bool there = text && std::string(text).find("http.server 0") != std::string::npos;
        g_free(text);
        return there;
    });
    MCGuiStep("Return", [] {
        vte_terminal_feed_child(findVte(MCGuiApp().terminal->widget()), "\n", 1);
    });
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
    MCGuiStep("a look costs little CPU on the sampling thread", [] {
        ProcessMonitor& m = ProcessMonitor::shared();
        GCHECK(m.samples() > 0 && m.lastListed() > 0);
        GCHECK_TIME(m.lastSampleCpuMilliseconds(), 100, "a /proc sample");
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
