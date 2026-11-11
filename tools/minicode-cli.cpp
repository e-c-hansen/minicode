// minicode-cli.cpp — the client half of the control channel. The `minicode`
// launcher (scripts/minicode-launcher) hands it every subcommand (`minicode
// open`, `state`, `term`, ...); it turns the command line into one request
// (ControlProtocol.h), sends it over the newest running minicode's Unix
// socket ($TMPDIR/MiniCode/<pid>.sock, or the same in the user's own
// temporary folder), prints the answer, and exits 0 (done), 1 (the command
// failed), 2 (a usage error) or 3 (no minicode to talk to). With no minicode
// running it starts one the way the launcher does, then waits up to 10
// seconds for its socket; reads (state, selection, term-output, procs) exit 3 at
// once instead.
//
// On Linux (PARITY.md 3.1) the same file is the client half of the app's
// own binary: `minicode open|window|state|selection ...` runs minicodeCliMain
// (linux/src/main.cpp calls it before anything of GTK starts) over the socket
// folder of ControlPaths.h; the commands of later items (terminals, diffs,
// notifications, processes) answer "not on Linux yet" from the app, and the
// ones that need the Mac's frameworks are left out of this build.
//
// For tests: MINICODE_LAUNCHER_DRYRUN prints the launch command instead of
// running it, MINICODE_CLI_WAIT sets the wait in seconds, and
// MINICODE_CONTROL_DIR names the one folder to look for sockets in.
#include "ControlPaths.h"
#include "ControlProtocol.h"
#ifdef __APPLE__
#include "ClaudeHooks.h"
#include "ProcessTable.h"
#endif
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern char **environ;

using namespace ControlChannel;

namespace {

const int kExitOK = 0, kExitFailed = 1, kExitUsage = 2, kExitNoApp = 3;

[[maybe_unused]] std::string trimmed(std::string dir) {
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir;
}

// The folders a minicode's socket may be in, in the order looked at. The
// app makes its socket in $TMPDIR/MiniCode, and Launch Services starts it
// with TMPDIR set to the user's own temporary folder, which confstr gives
// whatever the shell's TMPDIR says (a nix shell, a dotfile's TMPDIR=/tmp).
// So that folder is always looked in, after the shell's TMPDIR when that
// is another one. MINICODE_CONTROL_DIR names the one folder to use
// instead (tests).
#ifndef __APPLE__
std::vector<std::string> socketDirs() {
    return {socketFolder(getenv("MINICODE_CONTROL_DIR"), getenv("XDG_RUNTIME_DIR"),
                         getenv("TMPDIR"), (unsigned)getuid())};
}
#else
std::vector<std::string> socketDirs() {
    const char *forced = getenv("MINICODE_CONTROL_DIR");
    if (forced && *forced) return {trimmed(forced)};
    std::vector<std::string> out;
    const char *t = getenv("TMPDIR");
    if (t && *t) out.push_back(trimmed(t) + "/MiniCode");
    char buf[PATH_MAX];
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
    std::string user = n > 0 && n <= sizeof buf ? trimmed(buf) + "/MiniCode" : "";
    if (!user.empty()) {
        char a[PATH_MAX], b[PATH_MAX];
        bool same = false;
        for (const std::string &o : out)
            same = same || o == user ||
                   (realpath(o.c_str(), a) && realpath(user.c_str(), b) && strcmp(a, b) == 0);
        if (!same) out.push_back(user);
    }
    if (out.empty()) out.push_back("/tmp/MiniCode");
    return out;
}
#endif

// A folder the app made: a real folder (not a link) of this user's that
// nobody else can enter. Another user's socket in a shared folder could
// otherwise stand in for minicode.
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

// Every live minicode's socket, best first: `wantPid` alone when given.
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

// A minicode process of this user's that has no socket: one from before the
// control channel, which a launch would only bring forward.
#ifndef __APPLE__
bool appRunningWithoutSocket() { return false; }   // every Linux minicode has the channel
#else
bool appRunningWithoutSocket() {
    std::vector<pid_t> pids(4096);
    int bytes = proc_listallpids(pids.data(), (int)(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) return false;
    pids.resize((size_t)bytes / sizeof(pid_t));
    for (pid_t pid : pids) {
        char name[2 * MAXCOMLEN + 1] = {};
        if (pid <= 0 || proc_name(pid, name, sizeof name) <= 0) continue;
        if (std::strcmp(name, "MiniCode") != 0) continue;
        struct proc_bsdshortinfo info;
        if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof info) == sizeof info &&
            info.pbsi_uid == getuid())
            return true;
    }
    return false;
}

// The app bundle this client was installed in (Contents/MacOS/minicode-cli),
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
// Starts minicode on `folder` as `minicode <folder>` does: the app detaches
// from the terminal and returns at once (Detach.h), so this waits for that,
// and the caller waits for the socket. False when it could not be started.
bool launchApp(const std::string &folder) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return false;
    exe[n] = 0;
    const char *dry = getenv("MINICODE_LAUNCHER_DRYRUN");
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
// Starts minicode the way the launcher does with a folder: Launch Services
// opens the bundle, which opens a window on the folder. False when it could
// not be started.
bool launchApp(const std::string &folder) {
    std::string bundle = bundlePath();
    const char *dry = getenv("MINICODE_LAUNCHER_DRYRUN");
    if (dry && *dry) {
        std::printf("open -a %s %s\n", shellQuote(bundle.empty() ? "MiniCode.app" : bundle).c_str(),
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
    const char *w = getenv("MINICODE_CLI_WAIT");
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
// socket left by a minicode that died, its pid since reused).
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
    signal(SIGPIPE, SIG_IGN);   // a minicode that went away is an error, not a signal
#endif
    // A diff waits for git and browse --wait for a page (15 s at most);
    // nothing else takes more than a moment.
    struct timeval tv = {timeoutSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        *refused = errno == ECONNREFUSED || errno == ENOENT;
        error = "could not connect to minicode (process " + std::to_string(pid) + "): " +
                std::strerror(errno);
        close(fd);
        return false;
    }
    size_t off = 0;
    while (off < request.size()) {
        ssize_t n = write(fd, request.data() + off, request.size() - off);
        if (n < 0 && errno == EINTR) continue;
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
        error = "minicode closed the connection without answering";
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

#ifdef __APPLE__
// Claude Code's settings file: $CLAUDE_CONFIG_DIR/settings.json when that is
// set (Claude Code reads its settings from there then), else
// ~/.claude/settings.json.
std::string claudeSettingsPath() {
    const char *dir = getenv("CLAUDE_CONFIG_DIR");
    if (dir && *dir) return trimmed(dir) + "/settings.json";
    const char *home = getenv("HOME");
    return std::string(home && *home ? home : "/") + "/.claude/settings.json";
}

bool readFile(const std::string &path, std::string &text, bool &exists, std::string &error) {
    text.clear();
    exists = false;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        error = path + ": " + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error = path + " is not a plain file";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    exists = true;
    return true;
}

// `minicode hooks --print` and `--install` (R7.5).
int runHooks(const std::string &action) {
    if (action == "print") {
        std::printf("%s\n", prettyJson(ClaudeHooks::configuration()).c_str());
        return kExitOK;
    }
    const std::string path = claudeSettingsPath();
    std::string text, error;
    bool exists = false;
    if (!readFile(path, text, exists, error)) {
        std::fprintf(stderr, "minicode: %s\n", error.c_str());
        return kExitFailed;
    }
    ClaudeHooks::MergeResult r = ClaudeHooks::merge(text);
    if (!r.ok) {
        std::fprintf(stderr, "minicode: %s was left alone: %s\n", path.c_str(), r.error.c_str());
        return kExitFailed;
    }
    if (!r.changed) {
        std::printf("The hooks are already in %s; nothing changed.\n", path.c_str());
        return kExitOK;
    }
    mode_t mode = 0600;
    if (exists) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;
        // A backup of the file as it was, named for the time, never over
        // an earlier one.
        char stamp[32];
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        std::string backup = path + ".minicode-backup-" + stamp;
        for (int n = 2; access(backup.c_str(), F_OK) == 0; n++)
            backup = path + ".minicode-backup-" + stamp + "-" + std::to_string(n);
        if (!ClaudeHooks::writeSettingsFile(backup, text, mode, error)) {
            std::fprintf(stderr, "minicode: could not back up %s, so it was left alone: %s\n",
                         path.c_str(), error.c_str());
            return kExitFailed;
        }
        std::printf("Backed up %s to %s\n", path.c_str(), backup.c_str());
    } else {
        std::string dir = path.substr(0, path.rfind('/'));
        if (!dir.empty()) mkdir(dir.c_str(), 0700);
    }
    if (!ClaudeHooks::writeSettingsFile(path, r.text, mode, error)) {
        std::fprintf(stderr, "minicode: %s\n", error.c_str());
        return kExitFailed;
    }
    std::string added;
    for (size_t i = 0; i < r.added.size(); i++)
        added += (i ? (i + 1 == r.added.size() ? ", and " : ", ") : "") + r.added[i];
    if (r.added.size() == 2) added = r.added[0] + " and " + r.added[1];
    std::printf("Added `%s` to the %s hooks in %s.\n"
                "Claude Code sessions started from now on in minicode's terminals mark "
                "their tabs.\n",
                ClaudeHooks::kCommand, added.c_str(), path.c_str());
    return kExitOK;
}

// open-url's way out: the address goes to the default browser through
// /usr/bin/open ($MINICODE_OPEN_FALLBACK in the GUI checks, which record
// instead of opening anything).
int openInDefaultBrowser(const std::string &url) {
    if (url.empty() || url[0] == '-') {
        std::fprintf(stderr, "minicode: not an address: %s\n", url.c_str());
        return kExitUsage;
    }
    const char *fallback = getenv("MINICODE_OPEN_FALLBACK");
    const std::string program = fallback && *fallback ? fallback : "/usr/bin/open";
    std::vector<char *> argv = {const_cast<char *>(program.c_str()), const_cast<char *>(url.c_str()),
                                nullptr};
    execv(program.c_str(), argv.data());
    std::fprintf(stderr, "minicode: could not run %s: %s\n", program.c_str(), std::strerror(errno));
    return kExitFailed;
}
#endif  // __APPLE__

#ifndef __APPLE__
int openInDefaultBrowser(const std::string &) {
    std::fprintf(stderr, "minicode: open-url is not on Linux yet\n");
    return kExitFailed;
}
#endif

int usageError(const std::string &message) {
    std::fprintf(stderr, "minicode: %s\nTry 'minicode help'.\n", message.c_str());
    return kExitUsage;
}

}  // namespace

#ifdef __APPLE__
int main(int argc, char **argv) {
#else
int minicodeCliMain(int argc, char **argv) {
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    char cwdBuf[PATH_MAX];
    std::string cwd = getcwd(cwdBuf, sizeof cwdBuf) ? cwdBuf : "/";
    const char *home = getenv("HOME");
    auto exists = [](const std::string &p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0;
    };
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
#ifdef __APPLE__
    if (!c.hooksAction.empty()) return runHooks(c.hooksAction);
#else
    // What the Linux app has so far (PARITY.md 3.1); the app answers the
    // rest of the table the same way, for a client that is not this one.
    if (!c.hooksAction.empty() || (c.cmd != "open" && c.cmd != "window" && c.cmd != "state" &&
                                   c.cmd != "selection" && c.cmd != "ping")) {
        const std::string what = c.hooksAction.empty() ? c.cmd : "hooks";
        // A hook never fails noisily.
        if (c.fromHook) return kExitOK;
        std::fprintf(stderr, "minicode: %s is not on Linux yet\n", what.c_str());
        return kExitFailed;
    }
#endif

#ifdef __APPLE__
    // notify: never launches minicode and never complains that there is
    // none; with --from-hook nothing at all is ever reported (R7.2).
    const bool notify = c.cmd == "notify";
    const bool quiet = c.fromHook;
    if (notify && c.fromHook) {
        int pid = 0;
        Json hookArgs;
        if (!ClaudeHooks::notifyArgsFromHook(readStdin(1 << 20), getenv("MINICODE_TERMINAL_ID"),
                                             getenv("MINICODE_PID"), hookArgs, pid))
            return kExitOK;
        c.args = hookArgs;
        if (!c.pid) c.pid = pid;
    } else if (notify && !c.pid) {
        c.pid = ClaudeHooks::parsePositive(getenv("MINICODE_PID"));
    }

    // open-url, which BROWSER runs in minicode's terminals: a page that is
    // not local goes to the default browser, and so does any page when the
    // minicode the terminal belongs to does not take it. It never starts
    // minicode, and prints nothing when the page opened.
    const bool openUrl = c.cmd == "open-url";
    const std::string url = openUrl ? c.args["url"].asString() : "";
    if (openUrl) {
        if (!opensInPane(url)) return openInDefaultBrowser(url);
        if (!c.pid) c.pid = ClaudeHooks::parsePositive(getenv("MINICODE_PID"));
        const int terminal = ClaudeHooks::parsePositive(getenv("MINICODE_TERMINAL_ID"));
        if (!c.args.has("terminal") && terminal > 0) c.args.set("terminal", terminal);
    }
#else
    const bool notify = false, quiet = false, openUrl = false;
    const std::string url;
#endif

    if (!c.tourFile.empty()) {
        std::ifstream in(c.tourFile);
        if (!in) {
            std::fprintf(stderr, "minicode: %s: %s\n", c.tourFile.c_str(), std::strerror(errno));
            return kExitFailed;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        Json stops;
        std::string error;
        if (!tourStops(ss.str(), cwd, home ? home : "/", stops, error)) {
            std::fprintf(stderr, "minicode: %s\n", error.c_str());
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
            std::fprintf(stderr, "minicode: no minicode is running as process %d\n", c.pid);
            return kExitNoApp;
        }
        if (appRunningWithoutSocket()) {
            std::fprintf(stderr, "minicode: the minicode that is running does not answer (a version "
                                 "from before the command channel?); quit and open it again\n");
            return kExitNoApp;
        }
        if (!launchesApp(c.cmd)) {
            // Nothing to read from a minicode that is not there.
            std::fprintf(stderr, "minicode: minicode is not running\n");
            return kExitNoApp;
        }
        if (!launchApp(c.launchFolder)) {
            std::fprintf(stderr, "minicode: minicode is not running and could not be started\n");
            return kExitNoApp;
        }
        const double until = now() + waitSeconds();
        while ((sockets = findSockets(0)).empty() && now() < until) usleep(100000);
        if (sockets.empty()) {
            std::fprintf(stderr, "minicode: minicode did not start within %g seconds\n",
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
        std::fprintf(stderr, "minicode: %s\n", error.c_str());
        return kExitNoApp;
    }
    Json reply;
    if (quiet) return kExitOK;
    if (!Json::parse(answer, reply, &error) || !reply.isObject()) {
        std::fprintf(stderr, "minicode: an answer that is not JSON: %s\n", answer.c_str());
        return kExitFailed;
    }
    if (!reply["ok"].asBool()) {
        std::fprintf(stderr, "minicode: %s\n", reply["error"].asString("failed").c_str());
        if (openUrl) return openInDefaultBrowser(url);
        return kExitFailed;
    }
    const Json &result = reply["result"];
    if (result["background"].asBool())
        std::fprintf(stderr, "minicode: the user is typing in the editor, so this opened in a tab "
                             "beside theirs; it shows when they switch to it\n");
    if (result["pane_busy"].asBool())
        std::fprintf(stderr, "minicode: the user is using the browser pane, so the agent page "
                             "loaded without being shown; it shows when they pick Agent there\n");
    if (result["timed_out"].asBool())
        std::fprintf(stderr, "minicode: the page was still loading after 15 seconds\n");
    if (result["dropped"].asInt() > 0)
        std::fprintf(stderr, "minicode: %lld earlier messages of this page were dropped (the "
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
