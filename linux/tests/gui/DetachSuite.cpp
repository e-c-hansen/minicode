// DetachSuite.cpp — `minicode` started from a terminal gives the terminal
// back and is not tied to it (`./build/minicode-guitest detach`,
// linux/PARITY.md R2, src/Detach.h).
//
// Not this app: the real build/minicode, run on pseudo-terminals the suite
// opens, each with /bin/sh as the terminal's shell (it runs the command,
// prints "[minicode exited N]" when the command returns, then waits), on
// the checks' Xvfb, in a scratch home of its own:
//   - `minicode --version` and `minicode -h` print and return 0;
//   - `minicode <folder>` returns 0 while its window stays up, in a process
//     in a session of its own with no controlling terminal and /dev/null on
//     its standard streams; it asked systemd for a scope of its own, and a
//     refusal did not stop it; the terminal hanging up (its master closed,
//     SIGHUP to its session) leaves it running;
//   - a second `minicode <file>` is handed to it and returns 0, and its
//     window shows the file;
//   - with its streams sent elsewhere (`</dev/null >log 2>&1`), still the
//     terminal's job, it leaves as well, keeping the log file;
//   - with no display to open, it returns 1 with GTK's message shown in
//     the terminal;
//   - started as the desktop starts it (no terminal), it does not fork:
//     its window is the process started;
//   - `minicode --foreground <folder>` holds the terminal while it runs, in
//     the terminal's session, and goes when the terminal hangs up: the old
//     behaviour, now asked for.
//
// The real minicode gets a session bus of its own, a dbus-daemon started
// with the harness's dbus.conf: on the harness's bus this app owns
// org.minicode.Editor, and the real binary would hand its command line to
// it and exit. On that bus the suite answers as org.freedesktop.systemd1,
// recording StartTransientUnit and refusing it. The real binary's browser
// pane loads DuckDuckGo (the checks' own build has about:blank), so its
// http and https proxies are a listener here that answers nothing, and no
// check reaches the network.
//
// Every process the real minicode runs as, or starts, carries a token in
// its environment; the suite stops them all by PID at the end, with the
// shells, the bus and anything below the minicodes.
#include "Harness.h"

#include "LaunchOptions.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern char** environ;

#ifndef MINICODE_VERSION
#define MINICODE_VERSION ""
#endif

namespace {

std::string World() { return MCGuiScratch() + "/detach"; }
std::string Project() { return World() + "/project"; }
std::string Binary() { return MCGuiBuildDir() + "/minicode"; }
double Now() { return MCGuiNowMs() / 1000; }

// ------------------------------------------------------------------ /proc
struct Stat {
    bool ok = false;
    char state = 0;
    pid_t ppid = 0, pgrp = 0, session = 0;
    int ttyNr = 0;
    unsigned long long start = 0;
};

Stat statOf(pid_t pid) {
    Stat s;
    const std::string text = MCGuiReadFile("/proc/" + std::to_string(pid) + "/stat");
    const size_t close = text.rfind(')');
    if (close == std::string::npos || close + 2 > text.size()) return s;
    // state ppid pgrp session tty_nr ... starttime, the 22nd field.
    std::istringstream in(text.substr(close + 2));
    std::vector<std::string> f;
    for (std::string w; f.size() < 20 && in >> w;) f.push_back(w);
    if (f.size() < 20) return s;
    s.state = f[0][0];
    s.ppid = atoi(f[1].c_str());
    s.pgrp = atoi(f[2].c_str());
    s.session = atoi(f[3].c_str());
    s.ttyNr = atoi(f[4].c_str());
    s.start = strtoull(f[19].c_str(), nullptr, 10);
    s.ok = true;
    return s;
}

struct Proc {
    pid_t pid = 0;
    unsigned long long start = 0;
};

// The same process as when it was noted (a PID can be given again), and
// not a zombie.
bool alive(const Proc& p) {
    const Stat s = statOf(p.pid);
    return s.ok && s.state != 'Z' && s.start == p.start;
}

bool alive(pid_t pid) {
    const Stat s = statOf(pid);
    return s.ok && s.state != 'Z';
}

std::string linkOf(const std::string& path) {
    char buf[PATH_MAX];
    const ssize_t n = readlink(path.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    return buf;
}

std::vector<pid_t> allPids() {
    std::vector<pid_t> out;
    GDir* dir = g_dir_open("/proc", 0, nullptr);
    if (!dir) return out;
    while (const char* name = g_dir_read_name(dir))
        if (g_ascii_isdigit(name[0])) out.push_back((pid_t)atoi(name));
    g_dir_close(dir);
    return out;
}

std::string gToken;

// The live processes whose environment holds this run's token: the real
// minicodes, the launchers, and what they started.
std::vector<pid_t> withToken() {
    std::vector<pid_t> out;
    const std::string want = "MC_GUI_DETACH_TOKEN=" + gToken;
    for (pid_t pid : allPids()) {
        gchar* env = nullptr;
        gsize len = 0;
        const std::string path = "/proc/" + std::to_string(pid) + "/environ";
        if (!g_file_get_contents(path.c_str(), &env, &len, nullptr)) continue;
        bool found = false;
        for (gsize i = 0; i < len && !found;) {
            const size_t n = strnlen(env + i, len - i);
            found = n == want.size() && memcmp(env + i, want.data(), n) == 0;
            i += n + 1;
        }
        g_free(env);
        if (found && alive(pid)) out.push_back(pid);
    }
    return out;
}

// The real minicodes among them, by their program.
std::vector<pid_t> minicodes() {
    char real[PATH_MAX];
    const std::string binary = realpath(Binary().c_str(), real) ? real : Binary();
    std::vector<pid_t> out;
    for (pid_t pid : withToken())
        if (linkOf("/proc/" + std::to_string(pid) + "/exe") == binary) out.push_back(pid);
    return out;
}

// Everything below `root` now: WebKit's sandboxed processes keep no token.
std::vector<pid_t> descendantsOf(pid_t root) {
    std::map<pid_t, std::vector<pid_t>> kids;
    for (pid_t pid : allPids()) {
        const Stat s = statOf(pid);
        if (s.ok && s.state != 'Z') kids[s.ppid].push_back(pid);
    }
    std::vector<pid_t> out, todo = {root};
    std::set<pid_t> seen;
    while (!todo.empty()) {
        const pid_t p = todo.back();
        todo.pop_back();
        for (pid_t c : kids[p])
            if (seen.insert(c).second) {
                out.push_back(c);
                todo.push_back(c);
            }
    }
    return out;
}

// What the suite has started or found, to be stopped by PID.
std::vector<Proc> gStarted;

void remember(pid_t pid) {
    const Stat s = statOf(pid);
    if (!s.ok) return;
    for (const Proc& p : gStarted)
        if (p.pid == pid && p.start == s.start) return;
    gStarted.push_back(Proc{pid, s.start});
}

// The minicodes and everything theirs, noted before any is signalled.
void rememberMinicodesAndTheirs() {
    for (pid_t pid : withToken()) remember(pid);
    for (pid_t m : minicodes())
        for (pid_t d : descendantsOf(m)) remember(d);
}

void signalRemembered(int sig) {
    for (const Proc& p : gStarted)
        if (alive(p)) kill(p.pid, sig);
}

bool anyRememberedAlive() {
    for (const Proc& p : gStarted)
        if (alive(p)) return true;
    return false;
}

// Collects this process's own children among them (the shells, the bus);
// the rest belong to the wrapper, which adopts orphans and reaps them.
void reapRemembered() {
    for (const Proc& p : gStarted) waitpid(p.pid, nullptr, WNOHANG);
}

// ------------------------------------------------------- the real binary's
std::vector<std::string> gEnv;

void makeEnvironment(const std::string& busAddress, int proxyPort) {
    const std::string proxy = "http://127.0.0.1:" + std::to_string(proxyPort);
    const std::map<std::string, std::string> set = {
        {"HOME", World() + "/home"},
        {"XDG_CONFIG_HOME", World() + "/config"},
        {"XDG_CACHE_HOME", World() + "/cache"},
        {"XDG_DATA_HOME", World() + "/data"},
        {"XDG_STATE_HOME", World() + "/state"},
        {"MINICODE_SETTINGS", World() + "/settings.conf"},
        {"MINICODE_KEYS", World() + "/keys.conf"},
        {"MINICODE_BACKUP_DIR", World() + "/backups"},
        {"DBUS_SESSION_BUS_ADDRESS", busAddress},
        {"MC_GUI_DETACH_TOKEN", gToken},
        {"http_proxy", proxy},
        {"https_proxy", proxy},
        {"HTTP_PROXY", proxy},
        {"HTTPS_PROXY", proxy},
        {"all_proxy", proxy},
        {"ALL_PROXY", proxy},
    };
    const std::set<std::string> drop = {"MC_GUI_CHILD", "no_proxy", "NO_PROXY", "MINICODE_FOREGROUND",
                                        "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE"};
    gEnv.clear();
    for (char** e = environ; *e; e++) {
        const std::string kv = *e;
        const std::string key = kv.substr(0, kv.find('='));
        if (!set.count(key) && !drop.count(key)) gEnv.push_back(kv);
    }
    for (const auto& kv : set) gEnv.push_back(kv.first + "=" + kv.second);
}

// ---------------------------------------------------------- the terminals
struct PtyShell {
    pid_t shell = -1;    // the session's leader, as a terminal's shell is
    int master = -1;
    std::string slave;   // /dev/pts/N
    std::string out;
    double started = 0, returned = 0;
};

// What the terminal's shell runs: the command, as typed, then a line when
// it returns. `redirect` follows the command, as in `minicode x >log`.
std::string shellScript(const std::string& redirect) {
    return "\"$0\" \"$@\" " + redirect + "; echo \"[minicode exited $?]\"; exec sleep 300";
}

std::shared_ptr<PtyShell> onTerminal(const std::vector<std::string>& args,
                                     const std::map<std::string, std::string>& env = {},
                                     const std::string& redirect = "") {
    auto t = std::make_shared<PtyShell>();
    std::vector<std::string> argv = {"/bin/sh", "-c", shellScript(redirect), Binary()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<std::string> environment;
    for (const std::string& kv : gEnv)
        if (!env.count(kv.substr(0, kv.find('=')))) environment.push_back(kv);
    for (const auto& kv : env) environment.push_back(kv.first + "=" + kv.second);
    std::vector<char*> av, ev;
    for (std::string& a : argv) av.push_back(&a[0]);
    av.push_back(nullptr);
    for (std::string& e : environment) ev.push_back(&e[0]);
    ev.push_back(nullptr);
    struct winsize size{};
    size.ws_row = 24;
    size.ws_col = 80;
    char slave[256] = "";
    int master = -1;
    const pid_t pid = forkpty(&master, slave, nullptr, &size);
    if (pid == 0) {
        // As a terminal starts its shell: default signal handling and an
        // empty mask (a SIGHUP ignored here would be ignored in minicode
        // too, and pass the hang-up check without the fix), and nothing of
        // this process but the terminal.
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        for (int s = 1; s < NSIG; s++) sigaction(s, &dfl, nullptr);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        close_range(3, ~0U, 0);
        execve(av[0], av.data(), ev.data());
        _exit(127);
    }
    if (pid < 0) {
        MCGuiCheck(false, "a pseudo-terminal for the real minicode", __FILE__, __LINE__);
        return t;
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    t->shell = pid;
    t->master = master;
    t->slave = slave;
    t->started = Now();
    remember(pid);
    return t;
}

void pump(PtyShell& t) {
    if (t.master >= 0) {
        char buf[4096];
        for (;;) {
            const ssize_t n = read(t.master, buf, sizeof buf);
            if (n <= 0) break;
            t.out.append(buf, (size_t)n);
        }
    }
    if (t.returned == 0 && t.out.find("[minicode exited ") != std::string::npos) t.returned = Now();
}

bool returned(PtyShell& t) {
    pump(t);
    return t.returned > 0;
}

// "[minicode exited N]", or "" while the command runs.
std::string exitLine(PtyShell& t) {
    pump(t);
    const size_t at = t.out.find("[minicode exited ");
    if (at == std::string::npos) return "";
    const size_t end = t.out.find(']', at);
    return end == std::string::npos ? "" : t.out.substr(at, end + 1 - at);
}

// What closing a terminal window does: the master goes, the kernel hangs
// the terminal up (SIGHUP to its session's leader, and to the foreground
// group when the leader exits), and a shell sends SIGHUP on to its jobs.
void hangUp(PtyShell& t) {
    pump(t);
    if (t.master >= 0) close(t.master);
    t.master = -1;
    if (t.shell > 0) kill(-t.shell, SIGHUP);
}

bool shellGone(PtyShell& t) {
    if (t.shell <= 0) return true;
    const pid_t got = waitpid(t.shell, nullptr, WNOHANG);
    return got == t.shell || (got < 0 && errno == ECHILD);
}

// As the desktop starts the desktop entry's Exec line: no controlling
// terminal, nothing read, the output to a file (the journal's place).
pid_t startWithoutTerminal(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {Binary()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> av, ev;
    for (std::string& a : argv) av.push_back(&a[0]);
    av.push_back(nullptr);
    for (std::string& e : gEnv) ev.push_back(&e[0]);
    ev.push_back(nullptr);
    const std::string log = World() + "/desktop.log";
    const int in = open("/dev/null", O_RDONLY | O_CLOEXEC);
    const int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (in < 0 || out < 0) {
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        return -1;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        dup2(in, 0);
        dup2(out, 1);
        dup2(out, 2);
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        for (int s = 1; s < NSIG; s++) sigaction(s, &dfl, nullptr);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        close_range(3, ~0U, 0);
        execve(av[0], av.data(), ev.data());
        _exit(127);
    }
    close(in);
    close(out);
    if (pid > 0) remember(pid);
    return pid;
}

// ---------------------------------------------------------- the bus
pid_t gBusPid = -1;
std::string gBusAddress;
GDBusConnection* gBus = nullptr;
guint gSystemdObject = 0;

struct ScopeCall {
    std::string name, mode, description, collect;
    std::vector<guint32> pids;
};
std::vector<ScopeCall> gScopeCalls;

// The scopes minicode asked for itself. The rest are VTE's, one for each
// shell its terminal panel starts (vte-spawn-<uuid>.scope).
std::vector<ScopeCall> appScopes() {
    std::vector<ScopeCall> out;
    for (const ScopeCall& c : gScopeCalls)
        if (c.name.rfind("app-", 0) == 0) out.push_back(c);
    return out;
}

const char kSystemdXml[] =
    "<node><interface name='org.freedesktop.systemd1.Manager'>"
    "<method name='StartTransientUnit'>"
    "<arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='a(sv)' direction='in'/><arg type='a(sa(sv))' direction='in'/>"
    "<arg type='o' direction='out'/></method>"
    "</interface></node>";

void onSystemdCall(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                   GVariant* params, GDBusMethodInvocation* invocation, gpointer) {
    if (g_strcmp0(method, "StartTransientUnit") != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod",
                                                   "only StartTransientUnit");
        return;
    }
    ScopeCall c;
    const gchar* name = nullptr;
    const gchar* mode = nullptr;
    GVariant* props = nullptr;
    GVariant* aux = nullptr;
    g_variant_get(params, "(&s&s@a(sv)@a(sa(sv)))", &name, &mode, &props, &aux);
    c.name = name;
    c.mode = mode;
    GVariantIter it;
    g_variant_iter_init(&it, props);
    const gchar* key = nullptr;
    GVariant* value = nullptr;
    while (g_variant_iter_next(&it, "(&sv)", &key, &value)) {
        if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
            if (!strcmp(key, "Description")) c.description = g_variant_get_string(value, nullptr);
            if (!strcmp(key, "CollectMode")) c.collect = g_variant_get_string(value, nullptr);
        } else if (!strcmp(key, "PIDs") && g_variant_is_of_type(value, G_VARIANT_TYPE("au"))) {
            gsize n = 0;
            const guint32* p = static_cast<const guint32*>(g_variant_get_fixed_array(value, &n, sizeof(guint32)));
            c.pids.assign(p, p + n);
        }
        g_variant_unref(value);
    }
    g_variant_unref(props);
    g_variant_unref(aux);
    gScopeCalls.push_back(c);
    // Refused, as a session with no systemd user manager would be: the app
    // must carry on without a scope of its own.
    g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.AccessDenied",
                                               "refused by the detach check");
}

// A dbus-daemon of the suite's own, as MCGuiRunChild's runs have: the
// harness's configuration, which activates nothing. It dies with this
// process (PR_SET_PDEATHSIG) and is stopped by PID at the end.
bool startBus() {
    gchar* daemon = g_find_program_in_path("dbus-daemon");
    if (!daemon) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        g_free(daemon);
        return false;
    }
    std::vector<std::string> argv = {daemon, "--nofork",
                                     "--config-file=" + MCGuiScratch() + "/dbus.conf",
                                     "--print-address=3"};
    g_free(daemon);
    std::vector<char*> av;
    for (std::string& a : argv) av.push_back(&a[0]);
    av.push_back(nullptr);
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(127);
        if (fds[1] == 3) fcntl(3, F_SETFD, 0);
        else dup2(fds[1], 3);
        close_range(4, ~0U, 0);
        execv(av[0], av.data());
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return false;
    }
    gBusPid = pid;
    remember(pid);
    std::string line;
    const double until = Now() + 10;
    while (Now() < until) {
        pollfd p{fds[0], POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        char c;
        if (read(fds[0], &c, 1) != 1 || c == '\n') break;
        line += c;
    }
    close(fds[0]);
    gBusAddress = line;
    return !line.empty();
}

bool connectAsSystemd() {
    GError* error = nullptr;
    gBus = g_dbus_connection_new_for_address_sync(
        gBusAddress.c_str(),
        (GDBusConnectionFlags)(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    if (!gBus) {
        MCGuiNote("cannot reach the suite's bus: %s", error ? error->message : "?");
        g_clear_error(&error);
        return false;
    }
    g_dbus_connection_set_exit_on_close(gBus, FALSE);
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kSystemdXml, nullptr);
    static GDBusInterfaceVTable vtable{};
    vtable.method_call = onSystemdCall;
    gSystemdObject = g_dbus_connection_register_object(gBus, "/org/freedesktop/systemd1", info->interfaces[0],
                                                       &vtable, nullptr, nullptr, nullptr);
    g_dbus_node_info_unref(info);
    GVariant* reply = g_dbus_connection_call_sync(
        gBus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.freedesktop.systemd1", 4u /* DO_NOT_QUEUE */), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    guint32 result = 0;
    if (reply) {
        g_variant_get(reply, "(u)", &result);
        g_variant_unref(reply);
    }
    return gSystemdObject != 0 && result == 1;   // the primary owner
}

// The PID of the process owning `name` on the suite's bus, or -1.
pid_t ownerOf(const char* name) {
    if (!gBus) return -1;
    GVariant* reply = g_dbus_connection_call_sync(
        gBus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "GetConnectionUnixProcessID", g_variant_new("(s)", name), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (!reply) return -1;
    guint32 pid = 0;
    g_variant_get(reply, "(u)", &pid);
    g_variant_unref(reply);
    return (pid_t)pid;
}

// ---------------------------------------------------------- the proxy
GSocketService* gProxy = nullptr;
int gProxyPort = 0;
std::vector<std::string> gProxyAsked;

gboolean onProxyClient(GSocketService*, GSocketConnection* connection, GObject*, gpointer) {
    GSocket* socket = g_socket_connection_get_socket(connection);
    g_socket_set_timeout(socket, 2);
    char buf[512];
    const gssize n = g_socket_receive(socket, buf, sizeof buf, nullptr, nullptr);
    std::string first = n > 0 ? std::string(buf, (size_t)n) : "";
    first = first.substr(0, first.find_first_of("\r\n"));
    gProxyAsked.push_back(first);
    g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
    return TRUE;
}

bool startProxy() {
    gProxy = g_socket_service_new();
    GInetAddress* loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress* address = g_inet_socket_address_new(loopback, 0);
    GSocketAddress* effective = nullptr;
    const bool ok = g_socket_listener_add_address(G_SOCKET_LISTENER(gProxy), address, G_SOCKET_TYPE_STREAM,
                                                  G_SOCKET_PROTOCOL_TCP, nullptr, &effective, nullptr);
    g_object_unref(address);
    g_object_unref(loopback);
    if (!ok || !effective) return false;
    gProxyPort = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
    g_object_unref(effective);
    g_signal_connect(gProxy, "incoming", G_CALLBACK(onProxyClient), nullptr);
    g_socket_service_start(gProxy);
    return gProxyPort > 0;
}

// ---------------------------------------------------------- the suite
bool gReady = false;
pid_t gApp = -1;   // the detached minicode
pid_t gForeground = -1;
std::shared_ptr<PtyShell> gVersion, gHelp, gDetached, gSecond, gRedirected, gNoDisplay, gHeld;
pid_t gRedirectedApp = -1;
pid_t gDesktop = -1;   // started as the desktop starts it
bool gDesktopReaped = false;

// The one real minicode with a window up, or -1.
pid_t windowedMinicode() {
    const std::vector<pid_t> found = minicodes();
    if (found.size() != 1) return -1;
    for (const MCGuiOtherWindow& w : MCGuiWindowsOfProcess(found[0]))
        if (w.viewable) return found[0];
    return -1;
}

// A display number nothing listens on.
std::string NoDisplay() {
    for (int n = 4999; n > 4000; n--)
        if (!MCGuiFileExists("/tmp/.X11-unix/X" + std::to_string(n))) return ":" + std::to_string(n);
    return ":4999";
}

std::string realPath(const std::string& path) {
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) ? buf : path;
}

bool hasWindow(pid_t pid, const std::string& titlePart = "") {
    for (const MCGuiOtherWindow& w : MCGuiWindowsOfProcess(pid))
        if (w.viewable && w.title.find(titlePart) != std::string::npos) return true;
    return false;
}

std::string titlesOf(pid_t pid) {
    std::string out;
    for (const MCGuiOtherWindow& w : MCGuiWindowsOfProcess(pid))
        out += (out.empty() ? "\"" : ", \"") + w.title + "\"" + (w.viewable ? "" : " (not shown)");
    return out;
}

// The real minicodes running and their windows, for a wait that failed.
std::string describeMinicodes() {
    std::string out;
    for (pid_t pid : minicodes()) {
        const Stat s = statOf(pid);
        out += (out.empty() ? "" : "; ") + std::to_string(pid) + " (parent " + std::to_string(s.ppid) +
               ", session " + std::to_string(s.session) + ", windows " + titlesOf(pid) + ")";
    }
    return out.empty() ? "none" : out;
}

}  // namespace

MC_GUI_SUITE(detach) {
    MCGuiStep("makes the real minicode's world, its bus, a systemd that refuses and a proxy", [] {
        gReady = GCHECK(access(Binary().c_str(), X_OK) == 0);
        if (!gReady) return;
        gToken = std::to_string(getpid()) + "-" + std::to_string(g_random_int());
        for (const char* d : {"home", "config", "cache", "data", "state", "backups", "project"})
            g_mkdir_with_parents((World() + "/" + d).c_str(), 0700);
        MCGuiWriteFile(World() + "/settings.conf", "lsp.enabled = false\n");
        MCGuiWriteFile(Project() + "/notes.txt", "Notes for the detach check.\n");
        gReady = GCHECK(startBus()) && GCHECK(connectAsSystemd()) && GCHECK(startProxy());
        if (gReady) makeEnvironment(gBusAddress, gProxyPort);
    });

    // ---- printing and returning
    MCGuiStep("runs minicode --version on a terminal", [] {
        if (gReady) gVersion = onTerminal({"--version"});
    });
    MCGuiWait("it prints the version and returns", 20, [] { return !gVersion || returned(*gVersion); });
    MCGuiStep("checks what it printed", [] {
        if (!gVersion) return;
        GCHECK_EQ_STR(exitLine(*gVersion), "[minicode exited 0]");
        const std::string want = std::string("minicode ") + MINICODE_VERSION + "\r\n";
        if (!GCHECK(gVersion->out.find(want) != std::string::npos))
            MCGuiNote("it printed: %s", gVersion->out.c_str());
        hangUp(*gVersion);
    });
    MCGuiStep("runs minicode -h on a terminal", [] {
        if (gReady) gHelp = onTerminal({"-h"});
    });
    MCGuiWait("it prints the help and returns", 20, [] { return !gHelp || returned(*gHelp); });
    MCGuiStep("checks the help", [] {
        if (!gHelp) return;
        GCHECK_EQ_STR(exitLine(*gHelp), "[minicode exited 0]");
        GCHECK(gHelp->out.find("Usage: minicode [options] [path]") != std::string::npos);
        GCHECK(gHelp->out.find("--foreground") != std::string::npos);
        hangUp(*gHelp);
    });

    // ---- the terminal given back
    MCGuiStep("runs minicode <folder> on a terminal", [] {
        if (gReady) gDetached = onTerminal({Project()});
    });
    // The product stops waiting for the window after 30 s and returns 0
    // with a message; this bound is under that, and the message is checked
    // for below.
    MCGuiWait("the command returns", 25, [] { return !gDetached || returned(*gDetached); });
    MCGuiStep("it returned 0, with nothing printed", [] {
        if (!gDetached) return;
        GCHECK_EQ_STR(exitLine(*gDetached), "[minicode exited 0]");
        GCHECK(gDetached->out.find("still starting") == std::string::npos);
        if (gDetached->returned > 0)
            MCGuiNote("returned %.2f s after it started", gDetached->returned - gDetached->started);
        // What the app printed before its window, which the terminal shows
        // (Mesa's DRI3 complaint on this display); nothing of the launcher's.
        const std::string before = gDetached->out.substr(0, gDetached->out.find("[minicode exited "));
        if (!before.empty()) {
            gchar* shown = g_strescape(before.c_str(), nullptr);
            MCGuiNote("the terminal showed before it returned: %s", shown);
            g_free(shown);
        }
    });
    MCGuiWait("a minicode window is up", 20, [] {
        if (!gDetached) return true;
        gApp = windowedMinicode();
        return gApp > 0;
    });
    MCGuiStep("it runs in a session of its own, with nothing of the terminal", [] {
        if (gDetached && gApp <= 0) MCGuiNote("running: %s", describeMinicodes().c_str());
        if (gApp <= 0) return;
        const Stat s = statOf(gApp);
        GCHECK(s.ok);
        GCHECK(s.session == gApp);   // setsid: the leader of its own session
        GCHECK(s.pgrp == gApp);
        GCHECK(s.ttyNr == 0);        // no controlling terminal
        GCHECK(s.ppid != gDetached->shell);
        GCHECK(getsid(gApp) == gApp);
        for (int fd = 0; fd <= 2; fd++) {
            const std::string to = linkOf("/proc/" + std::to_string(gApp) + "/fd/" + std::to_string(fd));
            GCHECK(to != gDetached->slave);
            GCHECK_EQ_STR(to, "/dev/null");
        }
        // The primary instance on its bus: what a second `minicode` finds.
        GCHECK(ownerOf("org.minicode.Editor") == gApp);
    });
    MCGuiWait("it asked systemd for a scope of its own", 10, [] { return gApp <= 0 || !appScopes().empty(); });
    MCGuiStep("the scope it asked for", [] {
        if (gApp <= 0 || appScopes().empty()) return;
        GCHECK(appScopes().size() == 1);
        const ScopeCall c = appScopes()[0];
        GCHECK_EQ_STR(c.name, LaunchOptions::scopeName("org.minicode.Editor", gApp));
        GCHECK_EQ_STR(c.mode, "fail");
        GCHECK(c.pids.size() == 1 && c.pids[0] == (guint32)gApp);
        GCHECK_EQ_STR(c.collect, "inactive-or-failed");
        GCHECK(!c.description.empty());
    });
    MCGuiStep("the terminal hangs up", [] {
        if (gDetached) hangUp(*gDetached);
    });
    MCGuiWait("the terminal's shell goes", 10, [] { return !gDetached || shellGone(*gDetached); });
    // A negative check: the hang-up and the SIGHUP to the session have
    // reached whatever they were going to reach well within this time.
    MCGuiPause(1.5);
    MCGuiStep("minicode is still running, its window up", [] {
        if (gApp <= 0) return;
        GCHECK(alive(gApp));
        GCHECK(hasWindow(gApp));
        GCHECK(ownerOf("org.minicode.Editor") == gApp);
    });

    // ---- a second minicode hands its path over
    MCGuiStep("runs minicode notes.txt on a second terminal", [] {
        if (gApp > 0) gSecond = onTerminal({Project() + "/notes.txt"});
    });
    MCGuiWait("it returns", 25, [] { return !gSecond || returned(*gSecond); });
    MCGuiStep("it returned 0", [] {
        if (gSecond) GCHECK_EQ_STR(exitLine(*gSecond), "[minicode exited 0]");
    });
    MCGuiWait("the running minicode shows the file", 10, [] {
        return !gSecond || hasWindow(gApp, "notes.txt");
    });
    MCGuiStep("no second minicode stayed, and it asked for no scope", [] {
        if (!gSecond) return;
        if (!hasWindow(gApp, "notes.txt")) MCGuiNote("its windows: %s", titlesOf(gApp).c_str());
        const std::vector<pid_t> found = minicodes();
        GCHECK(found.size() == 1 && found[0] == gApp);
        GCHECK(appScopes().size() == 1);
        hangUp(*gSecond);
    });

    // ---- the detached minicode is stopped, by its PID
    MCGuiStep("stops the detached minicode and what it started", [] {
        rememberMinicodesAndTheirs();
        if (gApp > 0) kill(gApp, SIGTERM);
    });
    MCGuiWait("it has gone", 10, [] { return gApp <= 0 || !alive(gApp); });
    // The next minicode must find no primary instance: the bus lets go of
    // a dead process's name a moment after it has gone.
    MCGuiWait("its name on the bus is free", 10, [] { return !gReady || ownerOf("org.minicode.Editor") < 0; });

    // ---- its streams sent elsewhere: still the terminal's job, so it leaves
    MCGuiStep("runs minicode <folder> </dev/null >log 2>&1 on a terminal", [] {
        if (gReady) gRedirected = onTerminal({Project()}, {}, "</dev/null >\"$HOME/out.log\" 2>&1");
    });
    MCGuiWait("the command returns", 25, [] { return !gRedirected || returned(*gRedirected); });
    MCGuiWait("its window is up", 20, [] {
        if (!gRedirected) return true;
        gRedirectedApp = windowedMinicode();
        return gRedirectedApp > 0;
    });
    MCGuiWait("it asked for a scope of its own too", 10, [] {
        return gRedirectedApp <= 0 || appScopes().size() >= 2;
    });
    MCGuiStep("it left the terminal and kept its log file", [] {
        if (gRedirected && gRedirectedApp <= 0)
            MCGuiNote("%s; running: %s", exitLine(*gRedirected).c_str(), describeMinicodes().c_str());
        if (gRedirectedApp <= 0) return;
        GCHECK(appScopes().size() == 2 && appScopes()[1].pids == std::vector<guint32>{(guint32)gRedirectedApp});
        GCHECK_EQ_STR(exitLine(*gRedirected), "[minicode exited 0]");
        const Stat s = statOf(gRedirectedApp);
        GCHECK(s.session == gRedirectedApp);
        GCHECK(s.ttyNr == 0);
        const std::string fd = "/proc/" + std::to_string(gRedirectedApp) + "/fd/";
        const std::string log = realPath(World() + "/home/out.log");
        GCHECK_EQ_STR(linkOf(fd + "0"), "/dev/null");
        GCHECK_EQ_STR(linkOf(fd + "1"), log);   // a regular file the user chose stays
        GCHECK_EQ_STR(linkOf(fd + "2"), log);
        rememberMinicodesAndTheirs();
        kill(gRedirectedApp, SIGTERM);
        hangUp(*gRedirected);
    });
    MCGuiWait("it has gone", 10, [] { return gRedirectedApp <= 0 || !alive(gRedirectedApp); });
    MCGuiWait("its name on the bus is free", 10, [] { return !gReady || ownerOf("org.minicode.Editor") < 0; });

    // ---- a failure before the window is said in the terminal
    MCGuiStep("runs minicode <folder> on a terminal, with no display to open", [] {
        if (gReady) gNoDisplay = onTerminal({Project()}, {{"DISPLAY", NoDisplay()}});
    });
    MCGuiWait("the command returns", 25, [] { return !gNoDisplay || returned(*gNoDisplay); });
    MCGuiStep("with minicode's status, and GTK's message in the terminal", [] {
        if (!gNoDisplay) return;
        GCHECK_EQ_STR(exitLine(*gNoDisplay), "[minicode exited 1]");
        // GTK 4.22 says "Failed to open display", older ones "cannot open display".
        if (!GCHECK(gNoDisplay->out.find("open display") != std::string::npos))
            MCGuiNote("it printed: %s", gNoDisplay->out.c_str());
        GCHECK(minicodes().empty());
        hangUp(*gNoDisplay);
    });

    // ---- the desktop's launch: no terminal, so no fork
    MCGuiStep("starts minicode <folder> as the desktop does, with no terminal", [] {
        if (gReady) GCHECK((gDesktop = startWithoutTerminal({Project()})) > 0);
    });
    // The window's _NET_WM_PID is the very process started: it did not fork.
    MCGuiWait("its window is up", 25, [] { return gDesktop <= 0 || hasWindow(gDesktop); });
    MCGuiStep("it is the process started, still running, with no scope asked for", [] {
        if (gDesktop > 0 && !hasWindow(gDesktop)) MCGuiNote("running: %s", describeMinicodes().c_str());
        if (gDesktop <= 0) return;
        GCHECK(waitpid(gDesktop, nullptr, WNOHANG) == 0);   // still running, as this process's child
        const std::vector<pid_t> found = minicodes();
        GCHECK(found.size() == 1 && found[0] == gDesktop);
        GCHECK(appScopes().size() == 2);   // none for this one
        rememberMinicodesAndTheirs();
        kill(gDesktop, SIGTERM);
    });
    MCGuiWait("it has gone", 10, [] {
        if (gDesktop <= 0 || gDesktopReaped) return true;
        gDesktopReaped = waitpid(gDesktop, nullptr, WNOHANG) == gDesktop;
        return gDesktopReaped;
    });
    MCGuiWait("its name on the bus is free", 10, [] { return !gReady || ownerOf("org.minicode.Editor") < 0; });

    // ---- --foreground: the old way, asked for
    MCGuiStep("runs minicode --foreground <folder> on a third terminal", [] {
        if (gReady) gHeld = onTerminal({"--foreground", Project()});
    });
    MCGuiWait("its window is up", 25, [] {
        if (!gHeld) return true;
        gForeground = windowedMinicode();
        return gForeground > 0;
    });
    MCGuiStep("it runs in the terminal's session, on the terminal", [] {
        if (gHeld && gForeground <= 0) MCGuiNote("running: %s", describeMinicodes().c_str());
        if (gForeground <= 0) return;
        const Stat s = statOf(gForeground);
        GCHECK(s.session == gHeld->shell);
        GCHECK(s.ppid == gHeld->shell);
        GCHECK(s.ttyNr != 0);
        GCHECK_EQ_STR(linkOf("/proc/" + std::to_string(gForeground) + "/fd/1"), gHeld->slave);
        GCHECK(appScopes().size() == 2);   // none for this one
    });
    // A negative check: the command has had time to return if it would.
    MCGuiPause(1.5);
    MCGuiStep("the terminal is still held", [] {
        if (!gHeld) return;
        GCHECK(!returned(*gHeld));
        GCHECK(alive(gForeground));
        rememberMinicodesAndTheirs();
        hangUp(*gHeld);
    });
    MCGuiWait("minicode goes with its terminal", 10, [] { return gForeground <= 0 || !alive(gForeground); });

    // ---- the end: everything started here is stopped by PID
    MCGuiStep("stops every process the suite started", [] {
        rememberMinicodesAndTheirs();
        signalRemembered(SIGTERM);
    });
    MCGuiWait("they have all gone", 10, [] {
        reapRemembered();
        return !anyRememberedAlive();
    });
    MCGuiStep("and nothing is left", [] {
        if (anyRememberedAlive()) {
            for (const Proc& p : gStarted)
                if (alive(p)) MCGuiNote("still running after SIGTERM: %d", (int)p.pid);
            signalRemembered(SIGKILL);
        }
        GCHECK(withToken().empty());
        if (gBus) {
            if (gSystemdObject) g_dbus_connection_unregister_object(gBus, gSystemdObject);
            g_dbus_connection_close_sync(gBus, nullptr, nullptr);
            g_clear_object(&gBus);
        }
        if (gBusPid > 0) {
            kill(gBusPid, SIGTERM);
            waitpid(gBusPid, nullptr, 0);
        }
        if (gProxy) {
            g_socket_service_stop(gProxy);
            g_socket_listener_close(G_SOCKET_LISTENER(gProxy));
            g_clear_object(&gProxy);
        }
        // Where the real minicode's browser pane went instead of the network.
        if (!gProxyAsked.empty())
            MCGuiNote("the proxy was asked %d times, first: %s", (int)gProxyAsked.size(),
                      gProxyAsked[0].c_str());
        std::string scopes;
        for (const ScopeCall& c : gScopeCalls) scopes += (scopes.empty() ? "" : ", ") + c.name;
        MCGuiNote("systemd was asked for %s", scopes.c_str());
        for (const auto& t : {gVersion, gHelp, gDetached, gSecond, gRedirected, gNoDisplay, gHeld})
            if (t && t->master >= 0) close(t->master);
        reapRemembered();
    });
}
