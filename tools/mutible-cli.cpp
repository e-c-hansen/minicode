// mutible-cli.cpp — the client half of the control channel. The `mutible`
// launcher (scripts/mutible-launcher) hands it every subcommand (`mutible
// open`, `state`, `term`, ...); it turns the command line into one request
// (ControlProtocol.h), sends it over the newest running mutible's Unix
// socket ($TMPDIR/Mutible/<pid>.sock, or the same in the user's own
// temporary folder), prints the answer, and exits 0 (done), 1 (the command
// failed), 2 (a usage error) or 3 (no mutible to talk to). With no mutible
// running it starts one the way the launcher does, then waits up to 10
// seconds for its socket; reads (state, selection, term-output, procs) exit 3 at
// once instead.
//
// On Linux (PARITY.md 3.1) the same file is the client half of the app's
// own binary: `mutible open|window|state|selection ...` runs mutibleCliMain
// (linux/src/main.cpp calls it before anything of GTK starts) over the socket
// folder of ControlPaths.h; the commands of later items (terminals, diffs,
// notifications, processes) answer "not on Linux yet" from the app, and the
// ones that need the Mac's frameworks are left out of this build.
//
// For tests: MUTIBLE_LAUNCHER_DRYRUN prints the launch command instead of
// running it, MUTIBLE_CLI_WAIT sets the wait in seconds, and
// MUTIBLE_CONTROL_DIR names the one folder to look for sockets in.
#include "ControlPaths.h"
#include "Rename.h"
#include "ControlProtocol.h"
#include "ClaudeHooks.h"
#include "ClaudeStatusLine.h"
#include "SessionServer.h"
#include "SessionWeb.h"
#ifdef __APPLE__
#include "ProcessTable.h"
#endif
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

extern char **environ;

using namespace ControlChannel;

namespace {

const int kExitOK = 0, kExitFailed = 1, kExitUsage = 2, kExitNoApp = 3;

// The folders a mutible's socket may be in (ControlPaths.h).
std::vector<std::string> socketDirs() { return socketFolders(); }

// A folder the app made: a real folder (not a link) of this user's that
// nobody else can enter. Another user's socket in a shared folder could
// otherwise stand in for mutible.
bool trustedFolder(const std::string &dir) {
    struct stat st;
    return lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
           (st.st_mode & 077) == 0;
}

bool alive(int pid) {
    // EPERM is someone else's process, which is not ours to talk to.
    return pid > 0 && kill(pid, 0) == 0;
}

struct Candidate {
    int pid;
    std::string path;
};

// Every live mutible's socket, best first: `wantPid` alone when given.
std::vector<Candidate> findSockets(int wantPid) {
    std::vector<SocketEntry> found;
    std::vector<std::string> paths;
    for (const std::string &dir : socketDirs()) {
        if (!trustedFolder(dir)) continue;
        DIR *d = opendir(dir.c_str());
        if (!d) continue;
        while (struct dirent *e = readdir(d)) {
            int pid = pidFromSocketName(e->d_name);
            if (!pid) continue;
            struct stat st;
            std::string path = dir + "/" + e->d_name;
            if (lstat(path.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid())
                continue;
            #ifdef __APPLE__
            found.push_back({pid, (int64_t)st.st_mtimespec.tv_sec * 1000000000LL +
                                      st.st_mtimespec.tv_nsec});
#else
            found.push_back({pid, (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec});
#endif
            paths.push_back(path);
        }
        closedir(d);
    }
    std::vector<Candidate> out;
    for (int pid : rankSockets(found, wantPid, alive))
        for (size_t i = 0; i < found.size(); i++)
            if (found[i].pid == pid) out.push_back({pid, paths[i]});   // each of its folders
    return out;
}

// A mutible process of this user's that has no socket: one from before the
// control channel, which a launch would only bring forward.
#ifndef __APPLE__
bool appRunningWithoutSocket() { return false; }   // every Linux mutible has the channel
#else
bool appRunningWithoutSocket() {
    std::vector<pid_t> pids(4096);
    int bytes = proc_listallpids(pids.data(), (int)(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) return false;
    pids.resize((size_t)bytes / sizeof(pid_t));
    for (pid_t pid : pids) {
        char name[2 * MAXCOMLEN + 1] = {};
        if (pid <= 0 || proc_name(pid, name, sizeof name) <= 0) continue;
        if (std::strcmp(name, "Mutible") != 0) continue;
        struct proc_bsdshortinfo info;
        if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof info) == sizeof info &&
            info.pbsi_uid == getuid())
            return true;
    }
    return false;
}

// The app bundle this client was installed in (Contents/MacOS/mutible-cli),
// or "" when it runs from elsewhere (the build folder).
#ifdef __APPLE__
std::string bundlePath() {
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    char real[PATH_MAX];
    if (!realpath(buf, real)) return "";
    std::string p = real;
    for (int i = 0; i < 3; i++) {
        size_t slash = p.rfind('/');
        if (slash == std::string::npos) return "";
        p = p.substr(0, slash);
    }
    return p.size() > 4 && p.compare(p.size() - 4, 4, ".app") == 0 ? p : "";
}
#endif

#endif  // __APPLE__

std::string shellQuote(const std::string &s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

#ifndef __APPLE__
// Starts mutible on `folder` as `mutible <folder>` does: the app detaches
// from the terminal and returns at once (Detach.h), so this waits for that,
// and the caller waits for the socket. False when it could not be started.
bool launchApp(const std::string &folder) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    const char *dry = getenv("MUTIBLE_LAUNCHER_DRYRUN");
    if (dry && *dry) {
        std::printf("%s %s\n", shellQuote(exe).c_str(), shellQuote(folder).c_str());
        std::fflush(stdout);
        return true;
    }
    std::vector<const char *> argv = {exe};
    if (!folder.empty()) argv.push_back(folder.c_str());
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, exe, nullptr, nullptr, (char *const *)argv.data(), environ) != 0)
        return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#else
// Starts mutible the way the launcher does with a folder: Launch Services
// opens the bundle, which opens a window on the folder. False when it could
// not be started.
bool launchApp(const std::string &folder) {
    std::string bundle = bundlePath();
    const char *dry = getenv("MUTIBLE_LAUNCHER_DRYRUN");
    if (dry && *dry) {
        std::printf("open -a %s %s\n", shellQuote(bundle.empty() ? "Mutible.app" : bundle).c_str(),
                    shellQuote(folder).c_str());
        std::fflush(stdout);
        return true;
    }
    if (bundle.empty()) return false;
    const char *argv[] = {"/usr/bin/open", "-a", bundle.c_str(), folder.c_str(), nullptr};
    pid_t pid;
    if (posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, (char *const *)argv, environ) != 0)
        return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

double waitSeconds() {
    const char *w = getenv("MUTIBLE_CLI_WAIT");
    if (w && *w) {
        double d = std::atof(w);
        if (d >= 0) return d;
    }
    return 10;
}

double now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Sends one request and reads the one answer line. False with `error` set
// when the socket could not be used; *refused when nothing listens on it (a
// socket left by a mutible that died, its pid since reused).
bool exchange(const Candidate &to, const std::string &request, std::string &answer,
              std::string &error, bool *refused, int timeoutSeconds) {
    const int pid = to.pid;
    const std::string &path = to.path;
    *refused = false;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        error = "the socket's path is too long: " + path;
        return false;
    }
    std::strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    signal(SIGPIPE, SIG_IGN);   // a mutible that went away is an error, not a signal
#endif
    // A diff waits for git and browse --wait for a page (15 s at most);
    // nothing else takes more than a moment.
    struct timeval tv = {timeoutSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        *refused = errno == ECONNREFUSED || errno == ENOENT;
        error = "could not connect to mutible (process " + std::to_string(pid) + "): " +
                std::strerror(errno);
        close(fd);
        return false;
    }
    size_t off = 0;
    while (off < request.size()) {
        ssize_t n = write(fd, request.data() + off, request.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
            // mutible took the connection and closed it before the request
            // was written: what it does to another user's process, which
            // gets no answer at all. Which of the write and the close comes
            // first is timing, so this says the same as a close seen while
            // reading (the control suite's "state as another user" once
            // failed on it).
            error = "mutible closed the connection without answering";
            close(fd);
            return false;
        }
        if (n <= 0) {
            error = std::string("could not send the request: ") + std::strerror(errno);
            close(fd);
            return false;
        }
        off += (size_t)n;
    }
    answer.clear();
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        answer.append(buf, (size_t)n);
        if (answer.find('\n') != std::string::npos) break;
    }
    close(fd);
    size_t nl = answer.find('\n');
    if (nl == std::string::npos) {
        error = "mutible closed the connection without answering";
        return false;
    }
    answer.resize(nl);
    return true;
}

// All of stdin, up to `limit` bytes (a hook's JSON is small; the rest is
// read and dropped, so the writer never sees a broken pipe).
std::string readStdin(size_t limit) {
    std::string out;
    if (isatty(STDIN_FILENO)) return out;
    char buf[65536];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (out.size() < limit) out.append(buf, std::min((size_t)n, limit - out.size()));
    }
    return out;
}

// `mutible hooks --print` and `--install` (R7.5). The settings file is
// ClaudeHooks::settingsPath's: $CLAUDE_CONFIG_DIR/settings.json when that is
// set (Claude Code reads its settings from there then), else
// ~/.claude/settings.json. The app's Install Claude Code Hooks… does the
// same through ClaudeHooks::install.
int runHooks(const std::string &action) {
    if (action == "print") {
        std::printf("%s\n", prettyJson(ClaudeHooks::configuration()).c_str());
        return kExitOK;
    }
    const std::string path = ClaudeHooks::settingsPath(getenv("CLAUDE_CONFIG_DIR"), getenv("HOME"));
    ClaudeHooks::InstallResult r =
        ClaudeHooks::install(path, ClaudeHooks::kCommand, (long)time(nullptr));
    if (!r.ok) {
        std::fprintf(stderr, "mutible: %s\n", r.error.c_str());
        return kExitFailed;
    }
    if (!r.changed) {
        std::printf("The hooks are already in %s; nothing changed.\n", path.c_str());
        return kExitOK;
    }
    if (!r.backup.empty()) std::printf("Backed up %s to %s\n", path.c_str(), r.backup.c_str());
    std::printf("Added `%s` to the %s hooks in %s.\n"
                "Claude Code sessions started from now on in mutible's terminals mark "
                "their tabs.\n",
                ClaudeHooks::kCommand, ClaudeHooks::eventList(r.added).c_str(), path.c_str());
    return kExitOK;
}

// `mutible statusline [--then COMMAND]`: Claude Code's status line, as
// mutible's Agents panel installs it (ClaudeStatusLine.h). Notes the plan
// usage on stdin in mutible's own log, then runs COMMAND (the user's own
// status line before) with the same stdin and lets it print, or prints a
// short line of its own. Whatever happens it exits 0 and says nothing on
// stderr: a status line must never break.
pid_t gStatusChild = 0;

void forwardSignal(int sig) {
    if (gStatusChild > 0) kill(gStatusChild, sig);
    _exit(0);
}

int runStatusLine(const std::vector<std::string> &args) {
    std::string previous;
    for (size_t i = 1; i + 1 < args.size(); i++)
        if (args[i] == "--then") previous = args[i + 1];
    const std::string input = readStdin(1 << 20);
    ClaudeUsage::Sample sample;
    if (ClaudeUsage::sampleFromStatusJson(input, (double)time(nullptr), &sample)) {
#ifdef __APPLE__
        const bool mac = true;
#else
        const bool mac = false;
#endif
        const std::string dir = ClaudeStatusLine::usageDir(getenv("MUTIBLE_USAGE_DIR"), getenv("HOME"),
                                                           getenv("XDG_STATE_HOME"), mac);
        ClaudeStatusLine::appendSample(ClaudeStatusLine::logPath(dir), sample);
    }
    if (previous.empty()) {
        std::printf("%s\n", ClaudeStatusLine::defaultLine(input).c_str());
        std::fflush(stdout);
        return kExitOK;
    }
    // The user's command, through sh as Claude Code runs it, stdin fed from
    // a pipe on a thread of its own (a command that never reads it must not
    // hold us), stdout and stderr ours.
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    if (pipe(fds) != 0) return kExitOK;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    const char *argv[] = {"/bin/sh", "-c", previous.c_str(), nullptr};
    pid_t pid = 0;
    const int err = posix_spawn(&pid, "/bin/sh", &fa, nullptr, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[0]);
    if (err != 0) {
        close(fds[1]);
        return kExitOK;
    }
    gStatusChild = pid;
    signal(SIGTERM, forwardSignal);
    signal(SIGINT, forwardSignal);
    signal(SIGHUP, forwardSignal);
    const int writeFd = fds[1];
    std::thread feeder([writeFd, input] {
        size_t off = 0;
        while (off < input.size()) {
            const ssize_t n = write(writeFd, input.data() + off, input.size() - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(writeFd);
    });
    feeder.detach();
    // Ten seconds at most: Claude Code has moved on by then anyway.
    for (int waited = 0; waited < 1000; waited++) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return kExitOK;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    kill(pid, SIGTERM);
    return kExitOK;
}

// open-url's way out: the address goes to the default browser through
// /usr/bin/open, or xdg-open on Linux ($MUTIBLE_OPEN_FALLBACK in the GUI
// checks, which record instead of opening anything).
int openInDefaultBrowser(const std::string &url) {
    if (url.empty() || url[0] == '-') {
        std::fprintf(stderr, "mutible: not an address: %s\n", url.c_str());
        return kExitUsage;
    }
    const char *fallback = getenv("MUTIBLE_OPEN_FALLBACK");
#ifdef __APPLE__
    const std::string program = fallback && *fallback ? fallback : "/usr/bin/open";
#else
    const std::string program = fallback && *fallback ? fallback : "xdg-open";
#endif
    std::vector<char *> argv = {const_cast<char *>(program.c_str()), const_cast<char *>(url.c_str()),
                                nullptr};
    execvp(program.c_str(), argv.data());
    std::fprintf(stderr, "mutible: could not run %s: %s\n", program.c_str(), std::strerror(errno));
    return kExitFailed;
}

// SIGINT, SIGTERM and SIGHUP end `host` and `attach --web` as the host's
// lifetime would: taken by a thread of their own, so they are blocked
// before any other starts (shells get a clean mask: HostPty.cpp).
std::mutex gStopMu;
Shared::HostServer *gHost = nullptr;
Shared::Web::Server *gWeb = nullptr;

void takeStopSignals() {
    signal(SIGPIPE, SIG_IGN);
    sigset_t stops;
    sigemptyset(&stops);
    sigaddset(&stops, SIGINT);
    sigaddset(&stops, SIGTERM);
    sigaddset(&stops, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stops, nullptr);
    std::thread([stops] {
        int sig = 0;
        sigwait(&stops, &sig);
        std::lock_guard<std::mutex> lk(gStopMu);
        if (gHost) gHost->stop();
        if (gWeb) gWeb->stop();
    }).detach();
}

// The browser's view (SessionWeb.h) of the host at `socket`: started, its
// address printed with a link's token, and the next address printed each
// time one is used. False, with the reason printed, when
// the port cannot be had.
bool startWeb(Shared::Web::Server &web, const std::string &socket, const std::string &folder,
              int port, bool edit) {
    Shared::Web::Options w;
    w.hostSocket = socket;
    w.folder = folder;
    w.port = port;
    w.editable = edit;
    // A link is good for one browser: the next one's address after each
    // use. Nobody is here to approve a browser, so the link alone admits.
    w.nextLink = [](const std::string &url) {
        std::fprintf(stderr, "mutible: that address has been used; the next browser opens:\n");
        std::printf("%s\n", url.c_str());
        std::fflush(stdout);
    };
    std::string error;
    if (!web.start(w, &error)) {
        std::fprintf(stderr, "mutible: the browser's view could not listen: %s\n", error.c_str());
        return false;
    }
    std::fprintf(stderr, "mutible: %s of %s for a browser on this machine:\n",
                 edit ? "a view that can edit" : "a read-only view", folder.c_str());
    std::printf("%s\n", web.url().c_str());
    std::fflush(stdout);
    return true;
}

// `mutible host --folder DIR [--web PORT]`: the session's host for one
// folder, with no window (SessionServer.h), in the foreground until its
// lifetime ends or a signal stops it; with --web, also its view for a
// browser, a renderer of it attached while a browser looks.
int runHostStdio(const Json &args);

int runHost(const Json &args) {
    if (args.has("stdio")) return runHostStdio(args);
    const std::string asked = args["folder"].asString();
    const std::string folder = Shared::resolveFolder(asked);
    if (folder.empty()) {
        std::fprintf(stderr, "mutible: %s is not a folder\n", asked.c_str());
        return kExitFailed;
    }
    Shared::HostOptions o;
    o.folder = folder;
    o.socketDir = socketDirs()[0];
    if (args.has("idle_minutes")) o.idleMs = (long long)(args["idle_minutes"].asNumber() * 60000.0);
    // The apps' backup store: what no window shows, unsaved, is kept there
    // under this process's name (SessionServer.h, "Backups").
#ifdef __APPLE__
    const bool mac = true;
#else
    const bool mac = false;
#endif
    o.backupDir = Backups::defaultDir(getenv("MUTIBLE_BACKUP_DIR"), getenv("HOME"), getenv("XDG_DATA_HOME"), mac);
    // The disk watched (DiskWatch.h): a page's tree and Source Control
    // follow it, and a clean document no window shows follows its file.
    o.watchDisk = true;
    takeStopSignals();
    Shared::HostServer server;
    std::string error;
    switch (server.start(o, &error)) {
    case Shared::HostServer::Start::Running:
        std::fprintf(stderr, "mutible: a host for %s is running already (%s)\n", folder.c_str(),
                     error.c_str());
        if (args.has("web"))
            std::fprintf(stderr, "mutible: for a browser's view of it, run `mutible attach "
                                 "--folder %s --web %d`\n",
                         shellQuote(folder).c_str(), (int)args["web"].asInt());
        return kExitOK;
    case Shared::HostServer::Start::Failed:
        std::fprintf(stderr, "mutible: %s\n", error.c_str());
        return kExitFailed;
    case Shared::HostServer::Start::Ok: break;
    }
    std::fprintf(stderr, "mutible: hosting %s at %s\n", folder.c_str(), server.socketPath().c_str());
    Shared::Web::Server web;
    std::thread webThread;
    if (args.has("web")) {
        if (!startWeb(web, server.socketPath(), folder, (int)args["web"].asInt(),
                      args["edit"].asBool()))
            return kExitFailed;
        webThread = std::thread([&web] { web.run(); });
    }
    {
        std::lock_guard<std::mutex> lk(gStopMu);
        gHost = &server;
    }
    if (server.run())
        std::fprintf(stderr, "mutible: the host for %s had no renderer and nothing unsaved or "
                             "running, so it stopped\n", folder.c_str());
    {
        std::lock_guard<std::mutex> lk(gStopMu);
        gHost = nullptr;
    }
    if (webThread.joinable()) {
        web.stop();
        webThread.join();
    }
    return kExitOK;
}

// `mutible host --stdio PATH`: the session's host for one renderer on this
// process's standard input and output (SessionServer.h, startStdio), which
// is what a Mac window over SSH runs here (stage 8). PATH is a folder, a
// file whose folder it serves, or a new file's name in a folder that is
// there (Shared::stdioTarget). It watches the disk, so the window's tree
// follows it. Ends when its input closes. Nothing but the
// protocol goes to standard output: what a library or a child might print
// there goes to standard error instead (fd 1 is moved off the protocol's
// pipe), and children read /dev/null.
int runHostStdio(const Json &args) {
    const std::string asked = args["stdio"].asString();
    // A folder, a file, or a file not there yet in a folder that is (the
    // window opens it empty and its first save makes it).
    Shared::StdioTarget target;
    std::string why;
    if (!Shared::stdioTarget(asked, &target, &why)) {
        std::fprintf(stderr, "mutible: %s\n", why.c_str());
        return kExitFailed;
    }
    const std::string &folder = target.folder;
    const int in = dup(0), out = dup(1);
    const int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (in < 0 || out < 0 || null < 0) {
        std::fprintf(stderr, "mutible: no standard input or output: %s\n", std::strerror(errno));
        return kExitFailed;
    }
    dup2(null, 0);
    dup2(2, 1);
    ::close(null);
    Shared::HostOptions o;
    o.folder = folder;
    // A file: named in the Welcome, spelled under the folder as resolved.
    o.askedFile = target.file;
    o.askedFileNew = target.isNew;
    // The disk watched (DiskWatch.h): the window's tree follows the folders
    // it has open (FsChanged), as `mutible host` does for a page.
    o.watchDisk = true;
    // The terminals a renderer on another machine asks for: this machine's
    // login shell (SHELL, as sshd sets it) in this process's environment.
    for (char **e = environ; e && *e; e++) o.shellEnv.push_back(*e);
    takeStopSignals();
    Shared::HostServer server;
    std::string error;
    if (server.startStdio(o, in, out, &error) != Shared::HostServer::Start::Ok) {
        std::fprintf(stderr, "mutible: %s\n", error.c_str());
        return kExitFailed;
    }
    {
        std::lock_guard<std::mutex> lk(gStopMu);
        gHost = &server;
    }
    server.run();
    {
        std::lock_guard<std::mutex> lk(gStopMu);
        gHost = nullptr;
    }
    return kExitOK;
}

// `mutible attach --folder DIR --dump`: the folder's session as text, then
// detach (SessionServer.h's dumpSession). `--web PORT`: a browser's view of
// it until the host ends.
int runAttach(const Json &args) {
    const std::string asked = args["folder"].asString();
    const std::string folder = Shared::resolveFolder(asked);
    if (folder.empty()) {
        std::fprintf(stderr, "mutible: %s is not a folder\n", asked.c_str());
        return kExitFailed;
    }
    signal(SIGPIPE, SIG_IGN);
    const std::string path = Shared::findHostSocket(folder, socketDirs());
    std::string error;
    bool refused = false;
    std::unique_ptr<Shared::HostLink> link;
    if (!path.empty()) link = Shared::HostLink::open(path, &error, &refused);
    if (!link) {
        if (path.empty() || refused)
            std::fprintf(stderr, "mutible: no host serves %s (start one with `mutible host "
                                 "--folder %s`)\n", folder.c_str(), shellQuote(folder).c_str());
        else
            std::fprintf(stderr, "mutible: %s\n", error.c_str());
        return kExitNoApp;
    }
    if (args.has("web")) {
        // The view attaches itself while a browser looks; this link only
        // showed that a host answers.
        link->close();
        link.reset();
        takeStopSignals();
        Shared::Web::Server web;
        if (!startWeb(web, path, folder, (int)args["web"].asInt(), args["edit"].asBool()))
            return kExitFailed;
        {
            std::lock_guard<std::mutex> lk(gStopMu);
            gWeb = &web;
        }
        if (web.run()) std::fprintf(stderr, "mutible: the host for %s stopped\n", folder.c_str());
        std::lock_guard<std::mutex> lk(gStopMu);
        gWeb = nullptr;
        return kExitOK;
    }
    Shared::Client client(Model::Unit::Utf16, "mutible attach");
    link->connect(client);
    const std::string text = Shared::dumpSession(client);
    link->close();
    if (text.empty()) {
        std::fprintf(stderr, "mutible: the host at %s did not answer\n", path.c_str());
        return kExitFailed;
    }
    if (text.compare(0, 8 + folder.size() + 1, "== host " + folder + "\n") != 0) {
        std::fprintf(stderr, "mutible: the host at %s serves another folder\n", path.c_str());
        return kExitFailed;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    return kExitOK;
}

int usageError(const std::string &message) {
    std::fprintf(stderr, "mutible: %s\nTry 'mutible help'.\n", message.c_str());
    return kExitUsage;
}

}  // namespace

#ifndef __APPLE__
// `mutible install-skill [--dir DIR]` on Linux, what scripts/mutible-launcher
// does on the Mac (PARITY.md 3.1): the skill that ships with the package, or
// the build tree's copy, goes to $CLAUDE_CONFIG_DIR/skills/mutible (else
// ~/.claude/skills/mutible). $MUTIBLE_SKILL_FILE names the source in checks.
static bool readWholeFile(const std::string &path, std::string &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

static bool makeFolders(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') {
            const std::string part = dir.substr(0, i);
            if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    return true;
}

int mutibleInstallSkill(int argc, char **argv) {
    std::string to;
    const char *config = getenv("CLAUDE_CONFIG_DIR");
    const char *home = getenv("HOME");
    const std::string skills = config && *config
                                   ? std::string(config) + "/skills"
                                   : std::string(home && *home ? home : "/") + "/.claude/skills";
    to = skills + "/mutible";
    bool givenDir = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--dir") {
            if (i + 1 >= argc || !*argv[i + 1]) return usageError("--dir needs a folder");
            to = argv[++i];
            givenDir = true;
        } else if (a == "-h" || a == "--help") {
            std::printf("Usage: mutible install-skill [--dir DIR]\n");
            return kExitOK;
        } else {
            return usageError("install-skill: unexpected " + a);
        }
    }
    std::vector<std::string> candidates;
    if (const char *own = getenv("MUTIBLE_SKILL_FILE")) candidates.push_back(own);
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        std::string dir(exe, (size_t)n);
        dir.erase(dir.rfind('/'));
        candidates.push_back(dir + "/SKILL.md");                                  // build tree
        candidates.push_back(dir + "/../share/mutible/skills/mutible/SKILL.md");  // installed
        candidates.push_back(dir + "/../../../skills/mutible/SKILL.md");       // linux/build in a checkout
    }
    std::string skill;
    bool found = false;
    for (const std::string &c : candidates)
        if (!c.empty() && readWholeFile(c, skill)) {
            found = true;
            break;
        }
    if (!found) {
        std::fprintf(stderr, "mutible: this copy has no skill to install\n");
        return 3;
    }
    std::string have;
    if (readWholeFile(to + "/SKILL.md", have) && have == skill) {
        std::printf("The mutible skill in %s is up to date\n", to.c_str());
        return kExitOK;
    }
    FILE *out = makeFolders(to) ? std::fopen((to + "/SKILL.md").c_str(), "wb") : nullptr;
    if (!out || std::fwrite(skill.data(), 1, skill.size(), out) != skill.size()) {
        std::fprintf(stderr, "mutible: could not write %s/SKILL.md: %s\n", to.c_str(),
                     std::strerror(errno));
        if (out) std::fclose(out);
        return kExitFailed;
    }
    std::fclose(out);
    std::printf("Installed the mutible skill in %s\n", to.c_str());
    // The skill as it was installed before the rename is left alone; say
    // where it is, so Claude Code does not load both (src/Rename.h).
    const std::string oldSkill = skills + "/" + Rename::kOldName;
    struct stat st;
    if (!givenDir && oldSkill != to && stat((oldSkill + "/SKILL.md").c_str(), &st) == 0)
        std::printf("The skill from before the rename is still in %s; remove that folder so "
                    "Claude Code loads only the new one\n", oldSkill.c_str());
    return kExitOK;
}
#endif

#ifdef __APPLE__
int main(int argc, char **argv) {
#else
int mutibleCliMain(int argc, char **argv) {
#endif
    Rename::adoptOldEnvironment();   // MUTIBLE_* still read (src/Rename.h)
    std::vector<std::string> args(argv + 1, argv + argc);
    char cwdBuf[PATH_MAX];
    std::string cwd = getcwd(cwdBuf, sizeof cwdBuf) ? cwdBuf : "/";
    const char *home = getenv("HOME");
    auto exists = [](const std::string &p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0;
    };
    // The status line before anything else: it runs every few seconds.
    if (!args.empty() && args[0] == "statusline" &&
        std::find(args.begin(), args.end(), "--help") == args.end() &&
        std::find(args.begin(), args.end(), "-h") == args.end())
        return runStatusLine(args);
    CliCommand c = parseCommandLine(args, cwd, home ? home : "/", exists);
    if (c.help) {
        std::string text = usageText(c.helpTopic);
        if (text.empty()) return usageError("no help for " + c.helpTopic);
        std::fputs(text.c_str(), stdout);
        return kExitOK;
    }
    if (!c.error.empty()) {
        // A hook never fails noisily, whatever it was given.
        if (!args.empty() && args[0] == "notify" &&
            std::find(args.begin(), args.end(), "--from-hook") != args.end()) {
            readStdin(1 << 20);
            return kExitOK;
        }
        return usageError(c.error);
    }
    if (!c.hooksAction.empty()) return runHooks(c.hooksAction);
    if (c.cmd == "host") return runHost(c.args);
    if (c.cmd == "attach") return runAttach(c.args);
#ifndef __APPLE__
    // What the Linux app has so far (PARITY.md 3.1, 3.6, 3.8); the app answers
    // the rest of the table the same way, for a client that is not this one.
    if (c.cmd != "open" && c.cmd != "window" && c.cmd != "state" &&
        c.cmd != "selection" && c.cmd != "ping" && c.cmd != "procs" && c.cmd != "diff" &&
        c.cmd != "tour" && c.cmd != "notify" && c.cmd != "browse" && c.cmd != "page" &&
        c.cmd != "open-url" && c.cmd != "term" && c.cmd != "term-output") {
        // A hook never fails noisily.
        if (c.fromHook) return kExitOK;
        std::fprintf(stderr, "mutible: %s is not on Linux yet\n", c.cmd.c_str());
        return kExitFailed;
    }
#endif

    // notify: never launches mutible and never complains that there is
    // none; with --from-hook nothing at all is ever reported (R7.2).
    const bool notify = c.cmd == "notify";
    const bool quiet = c.fromHook;
    if (notify && c.fromHook) {
        int pid = 0;
        Json hookArgs;
        if (!ClaudeHooks::notifyArgsFromHook(readStdin(1 << 20), getenv("MUTIBLE_TERMINAL_ID"),
                                             getenv("MUTIBLE_PID"), hookArgs, pid))
            return kExitOK;
        c.args = hookArgs;
        if (!c.pid) c.pid = pid;
    } else if (notify && !c.pid) {
        c.pid = ClaudeHooks::parsePositive(getenv("MUTIBLE_PID"));
    }

    // open-url, which BROWSER runs in mutible's terminals: a page that is
    // not local goes to the default browser, and so does any page when the
    // mutible the terminal belongs to does not take it. It never starts
    // mutible, and prints nothing when the page opened.
    const bool openUrl = c.cmd == "open-url";
    const std::string url = openUrl ? c.args["url"].asString() : "";
    if (openUrl) {
        if (!opensInPane(url)) return openInDefaultBrowser(url);
        if (!c.pid) c.pid = ClaudeHooks::parsePositive(getenv("MUTIBLE_PID"));
        const int terminal = ClaudeHooks::parsePositive(getenv("MUTIBLE_TERMINAL_ID"));
        if (!c.args.has("terminal") && terminal > 0) c.args.set("terminal", terminal);
    }

    if (!c.tourFile.empty()) {
        std::ifstream in(c.tourFile);
        if (!in) {
            std::fprintf(stderr, "mutible: %s: %s\n", c.tourFile.c_str(), std::strerror(errno));
            return kExitFailed;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        Json stops;
        std::string error;
        if (!tourStops(ss.str(), cwd, home ? home : "/", stops, error)) {
            std::fprintf(stderr, "mutible: %s\n", error.c_str());
            return kExitFailed;
        }
        c.args.set("stops", stops);
    }
    std::string error;
    if (const CommandSpec *spec = findCommand(c.cmd)) {
        if (!validateArgs(*spec, c.args, error)) return usageError(error);
    }

    std::vector<Candidate> sockets = findSockets(c.pid);
    if (sockets.empty() && notify) return kExitOK;
    if (sockets.empty() && openUrl) return openInDefaultBrowser(url);
    if (sockets.empty()) {
        if (c.pid) {
            std::fprintf(stderr, "mutible: no mutible is running as process %d\n", c.pid);
            return kExitNoApp;
        }
        if (appRunningWithoutSocket()) {
            std::fprintf(stderr, "mutible: the mutible that is running does not answer (a version "
                                 "from before the command channel?); quit and open it again\n");
            return kExitNoApp;
        }
        if (!launchesApp(c.cmd)) {
            // Nothing to read from a mutible that is not there.
            std::fprintf(stderr, "mutible: mutible is not running\n");
            return kExitNoApp;
        }
        if (!launchApp(c.launchFolder)) {
            std::fprintf(stderr, "mutible: mutible is not running and could not be started\n");
            return kExitNoApp;
        }
        const double until = now() + waitSeconds();
        while ((sockets = findSockets(0)).empty() && now() < until) usleep(100000);
        if (sockets.empty()) {
            std::fprintf(stderr, "mutible: mutible did not start within %g seconds\n",
                         waitSeconds());
            return kExitNoApp;
        }
    }

    // The newest first; one that refuses the connection is a leftover, and
    // the next is tried.
    std::string answer;
    bool sent = false;
    const std::string request = requestLine(Json(1), c.cmd, c.args);
    for (const Candidate &to : sockets) {
        bool refused = false;
        if (exchange(to, request, answer, error, &refused, notify ? 5 : openUrl ? 10 : 120)) {
            sent = true;
            break;
        }
        if (!refused) break;
    }
    if (!sent) {
        if (notify) return kExitOK;
        if (openUrl) return openInDefaultBrowser(url);
        std::fprintf(stderr, "mutible: %s\n", error.c_str());
        return kExitNoApp;
    }
    Json reply;
    if (quiet) return kExitOK;
    if (!Json::parse(answer, reply, &error) || !reply.isObject()) {
        std::fprintf(stderr, "mutible: an answer that is not JSON: %s\n", answer.c_str());
        return kExitFailed;
    }
    if (!reply["ok"].asBool()) {
        std::fprintf(stderr, "mutible: %s\n", reply["error"].asString("failed").c_str());
        if (openUrl) return openInDefaultBrowser(url);
        return kExitFailed;
    }
    const Json &result = reply["result"];
    if (result["background"].asBool())
        std::fprintf(stderr, "mutible: the user is typing in the editor, so this opened in a tab "
                             "beside theirs; it shows when they switch to it\n");
    if (result["pane_busy"].asBool())
        std::fprintf(stderr, "mutible: the user is using the browser pane, so the agent page "
                             "loaded without being shown; it shows when they pick Agent there\n");
    if (result["timed_out"].asBool())
        std::fprintf(stderr, "mutible: the page was still loading after 15 seconds\n");
    if (result["dropped"].asInt() > 0)
        std::fprintf(stderr, "mutible: %lld earlier messages of this page were dropped (the "
                             "last 500 are kept)\n", (long long)result["dropped"].asInt());
    switch (c.output) {
    case Output::None: break;
    case Output::Line: std::printf("%s\n", result[c.outputField].asString().c_str()); break;
    case Output::JsonLines: {
        const Json &list = result[c.outputField];
        for (size_t i = 0; i < list.size(); i++) std::printf("%s\n", list[i].dump().c_str());
        break;
    }
    case Output::Id: std::printf("%s\n", result[c.outputField].dump().c_str()); break;
    case Output::Json:
        std::printf("%s\n", c.pretty ? prettyJson(result).c_str() : result.dump().c_str());
        break;
    case Output::Procs: {
#ifdef __APPLE__
        const std::string text = Procs::formatTable(result);
        std::fwrite(text.data(), 1, text.size(), stdout);
#endif
        break;
    }
    case Output::Text: {
        const std::string &text = result[c.outputField].asString();
        std::fwrite(text.data(), 1, text.size(), stdout);
        if (!text.empty() && text.back() != '\n' && isatty(STDOUT_FILENO)) std::fputc('\n', stdout);
        break;
    }
    }
    return kExitOK;
}
