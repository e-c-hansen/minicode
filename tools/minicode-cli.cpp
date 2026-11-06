// minicode-cli.cpp — the client half of the control channel. The `minicode`
// launcher (scripts/minicode-launcher) hands it every subcommand (`minicode
// open`, `state`, `term`, ...); it turns the command line into one request
// (ControlProtocol.h), sends it over the newest running minicode's Unix
// socket ($TMPDIR/MiniCode/<pid>.sock, or the same in the user's own
// temporary folder), prints the answer, and exits 0 (done), 1 (the command
// failed), 2 (a usage error) or 3 (no minicode to talk to). With no minicode
// running it starts one the way the launcher does, then waits up to 10
// seconds for its socket; reads (state, selection, term-output) exit 3 at
// once instead.
//
// For tests: MINICODE_LAUNCHER_DRYRUN prints the launch command instead of
// running it, MINICODE_CLI_WAIT sets the wait in seconds, and
// MINICODE_CONTROL_DIR names the one folder to look for sockets in.
#include "ControlProtocol.h"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <libproc.h>
#include <mach-o/dyld.h>
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

std::string trimmed(std::string dir) {
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
            found.push_back({pid, (int64_t)st.st_mtimespec.tv_sec * 1000000000LL +
                                      st.st_mtimespec.tv_nsec});
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

std::string shellQuote(const std::string &s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

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
              std::string &error, bool *refused) {
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
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    // A diff waits for git; nothing else takes more than a moment.
    struct timeval tv = {120, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
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

int usageError(const std::string &message) {
    std::fprintf(stderr, "minicode: %s\nTry 'minicode help'.\n", message.c_str());
    return kExitUsage;
}

}  // namespace

int main(int argc, char **argv) {
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
    if (!c.error.empty()) return usageError(c.error);

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
        if (exchange(to, request, answer, error, &refused)) {
            sent = true;
            break;
        }
        if (!refused) break;
    }
    if (!sent) {
        std::fprintf(stderr, "minicode: %s\n", error.c_str());
        return kExitNoApp;
    }
    Json reply;
    if (!Json::parse(answer, reply, &error) || !reply.isObject()) {
        std::fprintf(stderr, "minicode: an answer that is not JSON: %s\n", answer.c_str());
        return kExitFailed;
    }
    if (!reply["ok"].asBool()) {
        std::fprintf(stderr, "minicode: %s\n", reply["error"].asString("failed").c_str());
        return kExitFailed;
    }
    const Json &result = reply["result"];
    if (result["background"].asBool())
        std::fprintf(stderr, "minicode: the user is typing in the editor, so this opened in a tab "
                             "beside theirs; it shows when they switch to it\n");
    switch (c.output) {
    case Output::None: break;
    case Output::Id: std::printf("%s\n", result[c.outputField].dump().c_str()); break;
    case Output::Json:
        std::printf("%s\n", c.pretty ? prettyJson(result).c_str() : result.dump().c_str());
        break;
    case Output::Text: {
        const std::string &text = result[c.outputField].asString();
        std::fwrite(text.data(), 1, text.size(), stdout);
        if (!text.empty() && text.back() != '\n' && isatty(STDOUT_FILENO)) std::fputc('\n', stdout);
        break;
    }
    }
    return kExitOK;
}
