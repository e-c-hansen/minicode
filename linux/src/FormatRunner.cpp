// FormatRunner.cpp — see FormatRunner.h.
#include "FormatRunner.h"

#include "Lsp.h"   // LspSearchDirs

#include <glib.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace FormatRunner {

namespace {

std::atomic<int> gRunning{0};
std::atomic<int> gLastPid{0};

bool runnable(const std::string& p) {
    return !p.empty() && g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE) &&
           !g_file_test(p.c_str(), G_FILE_TEST_IS_DIR);
}

std::string realPathOf(const std::string& p) {
    if (p.empty()) return p;
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

std::string dirOf(const std::string& p) {
    gchar* d = g_path_get_dirname(p.c_str());
    std::string out = d;
    g_free(d);
    return out;
}

double now() { return (double)g_get_monotonic_time() / 1e6; }

struct Job {
    std::string text, path, root, setting, consent;
    int firstLine = 0, lastLine = 0;
    double timeout = 10;
    // Read on the main thread: where programs are looked for, the home
    // folder, $VIRTUAL_ENV, and the environment the program gets.
    std::vector<std::string> searchDirs;
    std::string home, virtualEnv;
    std::vector<std::string> environ;
    std::function<void(Result)> done;
    Result result;
};

// In the formatter, between fork and exec: the signals this thread blocks
// are not the program's, and it gets SIGTERM if minicode dies first.
void childSetup(gpointer) {
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, nullptr);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

// A SIGPIPE this thread caused by writing to a formatter that stopped
// reading is blocked here (write() said EPIPE instead); take it off.
void dropPendingSigpipe() {
    sigset_t pipe;
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    struct timespec zero = {0, 0};
    while (sigtimedwait(&pipe, nullptr, &zero) == SIGPIPE) {
    }
}

bool reap(pid_t pid, int* wstatus, bool block) {
    for (;;) {
        const pid_t r = waitpid(pid, wstatus, block ? 0 : WNOHANG);
        if (r == pid) return true;
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
}

// Waits up to `seconds` for the program to end.
bool reapWithin(pid_t pid, int* wstatus, double seconds) {
    const double until = now() + seconds;
    while (true) {
        if (reap(pid, wstatus, false)) return true;
        if (now() >= until) return false;
        g_usleep(10000);
    }
}

// Runs argv with `input` on stdin, collecting stdout and stderr, on this
// (worker) thread. Writing stdin and draining both outputs at once, so a
// large file never stalls the formatter on a full pipe.
void runProcess(const std::vector<std::string>& argv, const std::string& cwd,
                const std::vector<std::string>& environ, const std::string& input,
                double timeout, int* status, bool* timedOut, std::string* out, std::string* err,
                std::string* launchError) {
    *status = -1;
    *timedOut = false;
    std::vector<const char*> av, ev;
    for (const std::string& a : argv) av.push_back(a.c_str());
    av.push_back(nullptr);
    for (const std::string& e : environ) ev.push_back(e.c_str());
    ev.push_back(nullptr);
    GPid pid = 0;
    int inFd = -1, outFd = -1, errFd = -1;
    GError* gerr = nullptr;
    if (!g_spawn_async_with_pipes_and_fds(cwd.c_str(), av.data(), ev.data(),
                                          G_SPAWN_DO_NOT_REAP_CHILD, childSetup, nullptr, -1, -1,
                                          -1, nullptr, nullptr, 0, &pid, &inFd, &outFd, &errFd,
                                          &gerr)) {
        *launchError = "could not be started";
        if (gerr) *launchError += std::string(" (") + gerr->message + ")";
        g_clear_error(&gerr);
        return;
    }
    gLastPid = (int)pid;
    fcntl(inFd, F_SETFL, fcntl(inFd, F_GETFL) | O_NONBLOCK);
    fcntl(outFd, F_SETFL, fcntl(outFd, F_GETFL) | O_NONBLOCK);
    fcntl(errFd, F_SETFL, fcntl(errFd, F_GETFL) | O_NONBLOCK);

    size_t written = 0;
    if (input.empty()) {
        close(inFd);
        inFd = -1;
    }
    const double deadline = now() + timeout;
    double exitedAt = 0;
    int wstatus = 0;
    bool exited = false;
    char buf[65536];
    while (inFd >= 0 || outFd >= 0 || errFd >= 0) {
        if (!exited && reap(pid, &wstatus, false)) {
            exited = true;
            exitedAt = now();
        }
        // Exited, but something it started holds its output: let it go.
        if (exited && now() - exitedAt > 2) {
            *launchError = "left its output open after exiting";
            break;
        }
        if (!exited && now() >= deadline) {
            *timedOut = true;
            break;
        }
        struct pollfd fds[3];
        int n = 0, inAt = -1, outAt = -1, errAt = -1;
        if (inFd >= 0) {
            inAt = n;
            fds[n++] = {inFd, POLLOUT, 0};
        }
        if (outFd >= 0) {
            outAt = n;
            fds[n++] = {outFd, POLLIN, 0};
        }
        if (errFd >= 0) {
            errAt = n;
            fds[n++] = {errFd, POLLIN, 0};
        }
        // A tenth of a second at most, so the program's end and the
        // deadline are seen in time.
        const int r = poll(fds, (nfds_t)n, 100);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (inAt >= 0 && fds[inAt].revents) {
            if (fds[inAt].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(inFd);   // it stopped reading
                inFd = -1;
            } else {
                const ssize_t w = write(inFd, input.data() + written, input.size() - written);
                if (w > 0) written += (size_t)w;
                else if (w < 0 && errno != EAGAIN && errno != EINTR) written = input.size();
                if (written >= input.size()) {
                    close(inFd);
                    inFd = -1;
                }
            }
        }
        for (int* fd : {&outFd, &errFd}) {
            const int at = fd == &outFd ? outAt : errAt;
            if (at < 0 || !fds[at].revents) continue;
            const ssize_t got = read(*fd, buf, sizeof buf);
            if (got > 0) (fd == &outFd ? out : err)->append(buf, (size_t)got);
            else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                close(*fd);
                *fd = -1;
            }
        }
    }
    for (int fd : {inFd, outFd, errFd})
        if (fd >= 0) close(fd);
    dropPendingSigpipe();
    if (*timedOut || (!exited && !reapWithin(pid, &wstatus, std::max(0.0, deadline - now())))) {
        // Past the limit: SIGTERM, a second, then SIGKILL.
        *timedOut = true;
        kill(pid, SIGTERM);
        if (!reapWithin(pid, &wstatus, 1)) {
            kill(pid, SIGKILL);
            reapWithin(pid, &wstatus, 2);
        }
        g_spawn_close_pid(pid);
        return;
    }
    g_spawn_close_pid(pid);
    if (!launchError->empty()) return;
    *status = WIFEXITED(wstatus)   ? WEXITSTATUS(wstatus)
              : WIFSIGNALED(wstatus) ? 128 + WTERMSIG(wstatus)
                                     : -1;
}

void work(Job* job) {
    // write() to a formatter that stopped reading says EPIPE here; the
    // signal is never the app's.
    sigset_t pipe;
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe, nullptr);

    FormatRun::Env env;
    env.fs.exists = [](const std::string& p) {
        return g_file_test(p.c_str(), G_FILE_TEST_EXISTS) != 0;
    };
    env.fs.read = [](const std::string& p) {
        gchar* text = nullptr;
        gsize len = 0;
        std::string out;
        if (g_file_get_contents(p.c_str(), &text, &len, nullptr)) out.assign(text, len);
        g_free(text);
        return out;
    };
    env.executable = runnable;
    const std::vector<std::string> dirs = job->searchDirs;
    const std::string home = job->home;
    env.find = [dirs, home](const std::string& name) -> std::string {
        if (name.find('/') != std::string::npos) {
            std::string p = name;
            if (p.rfind("~/", 0) == 0) p = home + p.substr(1);
            return runnable(p) ? p : "";
        }
        for (const std::string& d : dirs) {
            const std::string p = d + "/" + name;
            if (runnable(p)) return p;
        }
        return "";
    };
    env.realPath = realPathOf;
    env.virtualEnv = job->virtualEnv;
    env.home = home;
    env.askAgainMenu = "Edit";

    Result& r = job->result;
    r.decision = FormatRun::decide(job->path, job->root, job->setting, job->consent,
                                   job->firstLine, job->lastLine, env);
    if (r.decision.kind != FormatRun::Kind::Run) {
        r.message = r.decision.message;
        return;
    }
    std::string cwd = r.decision.cwd;
    if (!g_file_test(cwd.c_str(), G_FILE_TEST_IS_DIR))
        cwd = g_file_test(job->root.c_str(), G_FILE_TEST_IS_DIR) ? job->root : home;
    // The program's own folder first on PATH (a script's #!/usr/bin/env
    // node then gets the node beside it), then the usual folders.
    std::string path = dirOf(r.decision.argv[0]);
    for (const std::string& d : dirs) path += ":" + d;
    std::vector<std::string> environ;
    for (const std::string& e : job->environ)
        if (e.rfind("PATH=", 0) != 0 && e.rfind("PYTHONUTF8=", 0) != 0) environ.push_back(e);
    environ.push_back("PATH=" + path);
    environ.push_back("PYTHONUTF8=1");

    int status = -1;
    bool timedOut = false;
    std::string out, err, launchError;
    runProcess(r.decision.argv, cwd, environ, job->text, job->timeout, &status, &timedOut, &out,
               &err, &launchError);
    const FormatRun::Outcome o = FormatRun::outcome(r.decision.tool, job->path, timedOut,
                                                    job->timeout, launchError, status, out, err,
                                                    job->text.size());
    r.ok = o.ok;
    r.text = o.text;
    r.message = o.message;
    // A NUL is UTF-8 but no text a GtkTextBuffer holds.
    if (r.ok && r.text.find('\0') != std::string::npos) {
        r.ok = false;
        r.text.clear();
        r.message = r.decision.tool +
                    " wrote something that is not UTF-8 text. The file was not changed.";
    }
}

}  // namespace

void format(const std::string& text, const std::string& path, const std::string& root,
            const std::string& setting, const std::string& consent, int firstLine,
            int lastLine, double timeoutSeconds, std::function<void(Result)> done) {
    Job* job = new Job;
    job->text = text;
    job->path = path;
    job->root = root;
    job->setting = setting;
    job->consent = consent;
    job->firstLine = firstLine;
    job->lastLine = lastLine;
    job->timeout = timeoutSeconds;
    job->searchDirs = LspSearchDirs();
    job->home = g_get_home_dir();
    const char* venv = g_getenv("VIRTUAL_ENV");
    job->virtualEnv = venv ? venv : "";
    gchar** envp = g_get_environ();
    for (gchar** e = envp; e && *e; e++) job->environ.push_back(*e);
    g_strfreev(envp);
    job->done = std::move(done);
    gRunning++;
    std::thread([job] {
        work(job);
        g_idle_add([](gpointer p) -> gboolean {
            Job* job = static_cast<Job*>(p);
            gRunning--;
            if (job->done) job->done(std::move(job->result));
            delete job;
            return G_SOURCE_REMOVE;
        }, job);
    }).detach();
}

int running() { return gRunning; }

int lastPid() { return gLastPid; }

}  // namespace FormatRunner
