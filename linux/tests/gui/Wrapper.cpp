// Wrapper.cpp — the first stage of minicode-guitest (Harness.h): the scratch
// world, a private Xvfb, the run under dbus-run-session, and the cleaning up.
//
// A shell script could do most of this, but not the part that matters: the
// app's terminals and language servers start in sessions of their own, so
// neither a process group nor `dbus-run-session`'s exit accounts for them.
// PR_SET_CHILD_SUBREAPER keeps every process the run starts this program's
// descendant, whatever it does, so at the end all of them can be found by
// walking /proc from here, and only those are signalled. Nothing is ever
// signalled by name: the owner runs a packaged minicode on this machine.
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

extern char** environ;

namespace {

volatile sig_atomic_t gStopSignal = 0;
void onStopSignal(int sig) { gStopSignal = sig; }

std::string selfPath() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    return buf;
}

std::string dirOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

double nowSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

bool makeDir(const std::string& path) { return g_mkdir_with_parents(path.c_str(), 0700) == 0; }

void writeText(const std::string& path, const std::string& text) {
    g_file_set_contents(path.c_str(), text.c_str(), (gssize)text.size(), nullptr);
}

int removeOne(const char* path, const struct stat*, int, struct FTW*) { return remove(path); }

// Depth first, not through symlinks, and never onto another filesystem.
void removeTree(const std::string& path) {
    nftw(path.c_str(), removeOne, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

// ------------------------------------------------------------- descendants
struct Proc {
    pid_t pid;
    bool zombie;   // exited, waiting for its parent to collect it
};

// parent pid -> its children, from /proc/<pid>/stat.
std::map<pid_t, std::vector<Proc>> processTree() {
    std::map<pid_t, std::vector<Proc>> kids;
    GDir* proc = g_dir_open("/proc", 0, nullptr);
    if (!proc) return kids;
    while (const char* name = g_dir_read_name(proc)) {
        if (!g_ascii_isdigit(name[0])) continue;
        const std::string path = std::string("/proc/") + name + "/stat";
        gchar* contents = nullptr;
        if (!g_file_get_contents(path.c_str(), &contents, nullptr, nullptr)) continue;
        // pid (comm) state ppid ...; comm may hold spaces and parentheses.
        const char* close = strrchr(contents, ')');
        int ppid = 0;
        char state = 0;
        if (close && sscanf(close + 1, " %c %d", &state, &ppid) == 2)
            kids[ppid].push_back(Proc{(pid_t)atoi(name), state == 'Z'});
        g_free(contents);
    }
    g_dir_close(proc);
    return kids;
}

// Every process still running below this one, `except` and its children left
// out.
std::vector<pid_t> descendants(pid_t except) {
    std::vector<pid_t> out;
    const auto kids = processTree();
    std::vector<pid_t> todo = {getpid()};
    std::set<pid_t> seen;
    while (!todo.empty()) {
        pid_t p = todo.back();
        todo.pop_back();
        const auto it = kids.find(p);
        if (it == kids.end()) continue;
        for (const Proc& c : it->second) {
            if (c.pid == except || !seen.insert(c.pid).second) continue;
            if (!c.zombie) out.push_back(c.pid);
            todo.push_back(c.pid);
        }
    }
    return out;
}

// Collects this process's exited children, by PID, but never the ones in
// `keep`: their status is read where they are waited for, and a PID never
// waited for cannot be given to another process before it is signalled.
void reapChildren(std::initializer_list<pid_t> keep) {
    const auto kids = processTree();
    const auto it = kids.find(getpid());
    if (it == kids.end()) return;
    for (const Proc& c : it->second)
        if (c.zombie && std::find(keep.begin(), keep.end(), c.pid) == keep.end())
            waitpid(c.pid, nullptr, WNOHANG);
}

std::string commOf(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    gchar* text = nullptr;
    if (!g_file_get_contents(path.c_str(), &text, nullptr, nullptr)) return "?";
    std::string out = text;
    g_free(text);
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// SIGTERM to everything still below us but `except`, then SIGKILL to what is
// left a moment later. What had to be killed is named, since it would
// otherwise have outlived the run.
void stopDescendants(pid_t except) {
    std::vector<pid_t> left = descendants(except);
    if (left.empty()) return;
    for (pid_t p : left) kill(p, SIGTERM);
    const double until = nowSeconds() + 2;
    while (nowSeconds() < until) {
        reapChildren({except});
        if (descendants(except).empty()) return;
        usleep(20000);
    }
    left = descendants(except);
    for (pid_t p : left) {
        printf("Killing %d (%s), still running after the run\n", (int)p, commOf(p).c_str());
        kill(p, SIGKILL);
    }
    const double until2 = nowSeconds() + 2;
    while (nowSeconds() < until2 && !descendants(except).empty()) {
        reapChildren({except});
        usleep(20000);
    }
    reapChildren({except});
}

void stopProcess(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    const double until = nowSeconds() + 2;
    while (nowSeconds() < until) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) return;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

// --------------------------------------------------------------------- Xvfb
// Starts Xvfb with the display number left to the server (-displayfd), the
// one way two runs at the same moment cannot pick the same number. The server
// writes the number to the pipe once it is ready for clients.
pid_t startXvfb(const std::string& log, std::string* display) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);   // never outlive a wrapper that is killed
        if (getppid() != parent) _exit(127);
        close(fds[0]);
        int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) {
            dup2(out, 1);
            dup2(out, 2);
        }
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) dup2(in, 0);
        const std::string fd = std::to_string(fds[1]);
        execlp("Xvfb", "Xvfb", "-displayfd", fd.c_str(), "-screen", "0", "1280x800x24",
               "-nolisten", "tcp", "-noreset", (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return -1;
    }
    std::string number;
    const double until = nowSeconds() + 15;
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        const int ms = (int)((until - nowSeconds()) * 1000);
        if (ms <= 0 || poll(&p, 1, ms) <= 0) break;
        char c;
        if (read(fds[0], &c, 1) != 1 || c == '\n') break;
        number += c;
    }
    close(fds[0]);
    if (number.empty() ||
        !std::all_of(number.begin(), number.end(), [](char c) { return g_ascii_isdigit(c); })) {
        stopProcess(pid);
        return -1;
    }
    *display = ":" + number;
    return pid;
}

// ------------------------------------------------------------ the world
// What the app reads at startup, none of it the user's. The project and the
// home are on the build folder's filesystem (g_file_trash refuses a tmpfs);
// the temporary folder and XDG_RUNTIME_DIR are short paths under /tmp, since
// Unix socket paths are limited to 108 bytes.
void clearEnvironment() {
    std::vector<std::string> names;
    for (char** e = environ; *e; e++) {
        const char* eq = strchr(*e, '=');
        if (!eq) continue;
        std::string name(*e, (size_t)(eq - *e));
        // Anything a parent set for a real minicode, or an earlier run.
        if (name.rfind("MINICODE_", 0) == 0 || name.rfind("MC_GUI_", 0) == 0) names.push_back(name);
    }
    for (const std::string& n : names)
        if (n != "MC_GUI_KEEP" && n != "MC_GUI_VERBOSE" && n != "MC_GUI_SNAPSHOTS" &&
            n != "MC_GUI_TOTAL_SECONDS" && n != "MC_GUI_STEP_SECONDS" && n != "MC_GUI_SUITES")
            unsetenv(n.c_str());
    for (const char* n :
         {"WAYLAND_DISPLAY", "WAYLAND_SOCKET", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS",
          "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "SESSION_MANAGER", "XDG_SESSION_TYPE",
          "XDG_SESSION_ID", "XDG_SESSION_CLASS", "XDG_SESSION_DESKTOP", "XDG_CURRENT_DESKTOP",
          "DESKTOP_SESSION", "GDK_DISPLAY", "GTK_MODULES", "GTK_THEME", "GDK_SCALE", "GDK_DPI_SCALE",
          "GTK_IM_MODULE", "XMODIFIERS", "LANGUAGE", "VIRTUAL_ENV", "CONDA_PREFIX",
          "CLAUDE_CODE_SSE_PORT"})
        unsetenv(n);
}

void setWorld(const std::string& root, const std::string& shortRoot) {
    const auto at = [&](const char* d) { return root + "/" + d; };
    setenv("HOME", at("home").c_str(), 1);
    setenv("XDG_CONFIG_HOME", at("config").c_str(), 1);
    setenv("XDG_CACHE_HOME", at("cache").c_str(), 1);
    setenv("XDG_DATA_HOME", at("data").c_str(), 1);
    setenv("XDG_STATE_HOME", at("state").c_str(), 1);
    setenv("XDG_RUNTIME_DIR", (shortRoot + "/run").c_str(), 1);
    setenv("TMPDIR", (shortRoot + "/t").c_str(), 1);
    setenv("ZDOTDIR", at("zdot").c_str(), 1);
    setenv("MINICODE_SETTINGS", at("settings.conf").c_str(), 1);
    setenv("MINICODE_BACKUP_DIR", at("backups").c_str(), 1);
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("GIT_AUTHOR_NAME", "GUI Check", 1);
    setenv("GIT_AUTHOR_EMAIL", "gui@example.invalid", 1);
    setenv("GIT_COMMITTER_NAME", "GUI Check", 1);
    setenv("GIT_COMMITTER_EMAIL", "gui@example.invalid", 1);
    setenv("MC_GUI_SCRATCH", root.c_str(), 1);
    // GTK's own: the X11 backend only, no accessibility bus, and the
    // software renderer, since Xvfb has no GPU and every agent shares the CPU.
    setenv("GDK_BACKEND", "x11", 1);
    setenv("GTK_A11Y", "none", 1);
    setenv("NO_AT_BRIDGE", "1", 1);
    setenv("GSK_RENDERER", "cairo", 1);
    // The color chooser and the like save to GSettings: not the user's dconf.
    setenv("GSETTINGS_BACKEND", "memory", 1);
    // The same text and the same input method on every machine.
    setenv("LANG", "C.UTF-8", 1);
    setenv("LC_ALL", "C.UTF-8", 1);
}

bool makeWorld(const std::string& root, const std::string& shortRoot) {
    for (const char* d : {"home", "config", "cache", "data", "state", "zdot", "backups", "project/src"})
        if (!makeDir(root + "/" + d)) return false;
    for (const char* d : {"run", "t"})
        if (!makeDir(shortRoot + "/" + d)) return false;
    writeText(root + "/zdot/.zshrc", "HISTFILE=$ZDOTDIR/.zsh_history\nPS1='%% '\n");
    writeText(root + "/settings.conf", "lsp.enabled = false\n");
    // A session bus with nothing to activate. Under the standard one, the
    // first look at a portal or the accessibility bus starts
    // xdg-desktop-portal and at-spi inside the run: seconds of CPU, a page of
    // warnings, and processes whose backends are the user's desktop's.
    writeText(root + "/dbus.conf",
              "<busconfig><type>session</type><keep_umask/>"
              "<listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth>"
              "<policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/>"
              "<allow eavesdrop=\"true\"/><allow own=\"*\"/></policy></busconfig>\n");
    std::string big;
    for (int i = 1; i <= 400; i++) big += "line " + std::to_string(i) + " of big.txt\n";
    writeText(root + "/project/big.txt", big);
    writeText(root + "/project/README.md", "# Project\n\nA scratch project for the GUI checks.\n");
    return true;
}

std::string suitesFromCommandLine(int argc, char** argv) {
    std::string wanted;
    for (int i = 1; i < argc; i++) wanted += std::string(i > 1 ? " " : "") + argv[i];
    if (wanted.empty() && getenv("SUITES")) wanted = getenv("SUITES");
    if (wanted.empty() && getenv("MC_GUI_SUITES")) wanted = getenv("MC_GUI_SUITES");
    return wanted;
}

}  // namespace

// dbus-run-session starts its dbus-daemon with this program in its place
// (--dbus-daemon). dbus-run-session stops the daemon when the run ends, but
// if it is itself killed (the wrapper gets a SIGKILL: every other process
// has a parent-death signal), a plain dbus-daemon would stay forever, the
// trap `timeout -s KILL` once left behind. So it is run with one too.
bool MCGuiIsDbusDaemonShim(int argc, char** argv) {
    return argc > 1 && strcmp(argv[1], "--nofork") == 0;
}

int MCGuiDbusDaemonShim(char** argv) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    gchar* daemon = g_find_program_in_path("dbus-daemon");
    if (!daemon) return 127;
    argv[0] = daemon;
    execv(daemon, argv);
    perror("minicode-guitest: dbus-daemon");
    return 127;
}

int MCGuiWrapperMain(int argc, char** argv) {
    const std::string self = selfPath();
    if (self.empty()) {
        fprintf(stderr, "minicode-guitest: cannot find this program\n");
        return 2;
    }
    // Asked for explicitly, the checks fail rather than skip when a tool is
    // missing, so a green run always means they ran.
    gchar* xvfb = g_find_program_in_path("Xvfb");
    gchar* dbusRun = g_find_program_in_path("dbus-run-session");
    if (!xvfb || !dbusRun) {
        fprintf(stderr, "minicode-guitest: %s is not installed (see PARITY.md, Environment)\n",
                xvfb ? "dbus-run-session" : "Xvfb");
        return 2;
    }
    g_free(xvfb);

    const std::string wanted = suitesFromCommandLine(argc, argv);
    prctl(PR_SET_CHILD_SUBREAPER, 1);

    std::string rootTemplate = dirOf(self) + "/guitest-XXXXXX";
    char shortTemplate[] = "/tmp/mcgui-XXXXXX";
    std::vector<char> rootBuf(rootTemplate.begin(), rootTemplate.end());
    rootBuf.push_back(0);
    if (!mkdtemp(rootBuf.data()) || !mkdtemp(shortTemplate)) {
        perror("minicode-guitest: mkdtemp");
        return 2;
    }
    const std::string root = rootBuf.data(), shortRoot = shortTemplate;
    if (!makeWorld(root, shortRoot)) {
        fprintf(stderr, "minicode-guitest: could not make the scratch folders\n");
        return 2;
    }

    clearEnvironment();
    setWorld(root, shortRoot);
    if (!wanted.empty()) setenv("MC_GUI_SUITES", wanted.c_str(), 1);

    struct sigaction sa {};
    sa.sa_handler = onStopSignal;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGINT, SIGTERM, SIGHUP}) sigaction(s, &sa, nullptr);

    int status = 2;
    std::string display;
    const pid_t xvfbPid = startXvfb(root + "/xvfb.log", &display);
    if (xvfbPid <= 0) {
        // The log goes with the scratch folder, so it is printed here: on CI
        // this is all there is to go on.
        fprintf(stderr, "minicode-guitest: Xvfb did not start. Its log:\n");
        gchar* log = nullptr;
        if (g_file_get_contents((root + "/xvfb.log").c_str(), &log, nullptr, nullptr))
            fprintf(stderr, "%s\n", log);
        g_free(log);
    } else {
        setenv("DISPLAY", display.c_str(), 1);
        setenv("MC_GUI_XVFB_PID", std::to_string(xvfbPid).c_str(), 1);
        setenv("MC_GUI_CHILD", "1", 1);
        const std::string project = root + "/project";
        const double total = getenv("MC_GUI_TOTAL_SECONDS") ? atof(getenv("MC_GUI_TOTAL_SECONDS")) : 1200;
        const double deadline = nowSeconds() + (total > 0 ? total : 1200) + 60;

        const pid_t parent = getpid();
        pid_t run = fork();
        if (run == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(127);
            const std::string config = "--config-file=" + root + "/dbus.conf";
            const std::string daemon = "--dbus-daemon=" + self;   // see MCGuiDbusDaemonShim
            execl(dbusRun, "dbus-run-session", config.c_str(), daemon.c_str(), "--", self.c_str(),
                  project.c_str(), (char*)nullptr);
            _exit(127);
        }
        if (run > 0) {
            for (;;) {
                int st = 0;
                const pid_t got = waitpid(run, &st, WNOHANG);
                if (got == run) {
                    status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
                    break;
                }
                if (gStopSignal || nowSeconds() > deadline) {
                    printf("\nminicode-guitest: %s; stopping the run\n",
                           gStopSignal ? "interrupted" : "the whole run overran its limit");
                    fflush(stdout);
                    status = 2;
                    break;
                }
                reapChildren({run, xvfbPid});   // orphans adopted while the run goes on
                usleep(50000);
            }
        }
    }
    g_free(dbusRun);

    // Everything the run left is stopped by PID, and the display goes last,
    // so nothing dies of a lost display and leaves a core behind.
    stopDescendants(xvfbPid);
    stopProcess(xvfbPid);
    reapChildren({});

    if (status != 0 && status != 1)
        printf("minicode-guitest: the checks ended abnormally (status %d)\n", status);
    if (getenv("MC_GUI_KEEP") && *getenv("MC_GUI_KEEP")) {
        printf("Scratch kept in %s and %s\n", root.c_str(), shortRoot.c_str());
    } else {
        removeTree(root);
        removeTree(shortRoot);
    }
    return status == 0 ? 0 : 1;
}
