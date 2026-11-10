// LspProcess.cpp — see LspProcess.h. Moved here from Lsp.cpp, unchanged,
// when the session was split by concern (PARITY.md item 2.0).
#include "LspProcess.h"

#include "Lsp.h"
#include "StartupLog.h"

#include <glib-unix.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace lspgtk {

std::vector<std::string> searchDirs() {
    std::vector<std::string> dirs;
    auto add = [&](const std::string& d) {
        if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
            dirs.push_back(d);
    };
    const char* path = g_getenv("PATH");
    std::stringstream ss(path ? path : "");
    std::string d;
    while (std::getline(ss, d, ':')) add(d);
    const std::string home = g_get_home_dir();
    add(home + "/.cargo/bin");
    add(home + "/go/bin");
    add(home + "/.local/bin");
    add("/usr/local/bin");
    add("/usr/bin");
    return dirs;
}

std::string findProgram(const std::string& name) {
    auto runnable = [](const std::string& p) {
        return g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE) &&
               !g_file_test(p.c_str(), G_FILE_TEST_IS_DIR);
    };
    if (name.find('/') != std::string::npos) {
        std::string p = name;
        if (p.rfind("~/", 0) == 0) p = std::string(g_get_home_dir()) + p.substr(1);
        return runnable(p) ? p : "";
    }
    for (const std::string& dir : searchDirs()) {
        std::string p = dir + "/" + name;
        if (runnable(p)) return p;
    }
    return "";
}

std::vector<std::string> resolveCommand(const std::string& command) {
    std::vector<std::string> words = Lsp::splitCommand(command);
    if (words.empty()) return {};
    std::string prog = findProgram(words[0]);
    if (prog.empty()) return {};
    words[0] = prog;
    return words;
}

namespace {

// A server that dies mid-write must not take the editor with it. A handler
// that does nothing (rather than SIG_IGN) makes write() fail with EPIPE, and
// unlike SIG_IGN it is reset to the default in every program the app starts,
// so the terminal's shell still gets ordinary SIGPIPE behaviour.
void ignoreSigpipeHere() {
    static bool done = false;
    if (done) return;
    done = true;
    struct sigaction old {};
    sigaction(SIGPIPE, nullptr, &old);
    if (old.sa_handler != SIG_DFL) return;   // someone already decided
    struct sigaction sa {};
    sa.sa_handler = [](int) {};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPIPE, &sa, nullptr);
}

// In the server, between fork and exec: if MiniCode dies without shutting
// it down (a crash, SIGKILL), the server gets SIGTERM. EOF on its stdin is
// the other backstop.
void childSetup(gpointer) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

}  // namespace

// Every server the app started and has not seen exit. Never freed: it must
// outlive anything that runs at exit.
static std::vector<std::shared_ptr<Server>>* gServers =
    new std::vector<std::shared_ptr<Server>>();

std::shared_ptr<Server> Server::start(const std::string& key,
                                      const std::vector<std::string>& argv,
                                      const std::string& root) {
    std::shared_ptr<Server> s(new Server());
    s->key_ = key;
    char* base = g_path_get_basename(argv[0].c_str());
    s->name_ = base;
    g_free(base);

    int toServer[2], fromServer[2];
    if (pipe2(toServer, O_CLOEXEC) != 0) return nullptr;
    if (pipe2(fromServer, O_CLOEXEC) != 0) {
        close(toServer[0]);
        close(toServer[1]);
        return nullptr;
    }

    // stderr must go somewhere that never fills up: clangd logs every request
    // there, and a full pipe would stall it mid-reply. MINICODE_LSP_LOG gets
    // it instead, along with all the traffic, opened O_APPEND so several
    // servers can share the file.
    int errFd = -1;
    const char* logPath = g_getenv("MINICODE_LSP_LOG");
    if (logPath && *logPath) {
        errFd = open(logPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        s->logFd_ = open(logPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    GSubprocessLauncher* l = g_subprocess_launcher_new(
        errFd >= 0 ? G_SUBPROCESS_FLAGS_NONE : G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_take_stdin_fd(l, toServer[0]);
    g_subprocess_launcher_take_stdout_fd(l, fromServer[1]);
    if (errFd >= 0) g_subprocess_launcher_take_stderr_fd(l, errFd);
    if (!root.empty() && g_file_test(root.c_str(), G_FILE_TEST_IS_DIR))
        g_subprocess_launcher_set_cwd(l, root.c_str());
    // Servers start helpers of their own (node, go, cargo), which need the
    // same wider PATH the server was found on.
    std::string path;
    for (const std::string& d : searchDirs()) path += (path.empty() ? "" : ":") + d;
    g_subprocess_launcher_setenv(l, "PATH", path.c_str(), TRUE);
    g_subprocess_launcher_set_child_setup(l, childSetup, nullptr, nullptr);

    std::vector<const char*> av;
    for (const std::string& a : argv) av.push_back(a.c_str());
    av.push_back(nullptr);
    GError* err = nullptr;
    s->proc_ = g_subprocess_launcher_spawnv(l, av.data(), &err);
    g_object_unref(l);   // closes the child's ends of the pipes
    if (!s->proc_) {
        g_printerr("MiniCode: could not start %s: %s\n", argv[0].c_str(),
                   err ? err->message : "?");
        g_clear_error(&err);
        close(toServer[1]);
        close(fromServer[0]);
        return nullptr;
    }
    ignoreSigpipeHere();
    const char* id = g_subprocess_get_identifier(s->proc_);
    s->pid_ = id ? atoi(id) : 0;
    StartupLog::once(("lsp " + s->name_).c_str(), "language server started: %s", s->name_.c_str());
    s->inFd_ = toServer[1];
    s->outFd_ = fromServer[0];
    g_unix_set_fd_nonblocking(s->inFd_, TRUE, nullptr);
    g_unix_set_fd_nonblocking(s->outFd_, TRUE, nullptr);
    s->running_ = true;
    s->readId_ = g_unix_fd_add(s->outFd_, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR),
                               onReadable, s.get());
    g_subprocess_wait_async(s->proc_, nullptr, onWaited, new std::shared_ptr<Server>(s));
    Server* raw = s.get();
    s->client_ = std::make_unique<Lsp::Client>([raw](const std::string& bytes) {
        raw->send(bytes);
    });
    gServers->push_back(s);
    return s;
}

Server::~Server() {
    if (readId_) g_source_remove(readId_);
    if (writeId_) g_source_remove(writeId_);
    if (inFd_ >= 0) close(inFd_);
    if (outFd_ >= 0) close(outFd_);
    if (logFd_ >= 0) close(logFd_);
    if (proc_) g_object_unref(proc_);
}

void Server::log(const char* prefix, const char* data, size_t n) {
    if (logFd_ < 0) return;
    std::string line = std::string(prefix) + std::string(data, n) + "\n";
    ssize_t w = write(logFd_, line.data(), line.size());
    (void)w;
}

void Server::send(const std::string& bytes) {
    if (closeWanted_ || inputClosed_) return;
    log("--> ", bytes.data(), bytes.size());
    out_ += bytes;
    flushWrites();
}

void Server::flushWrites() {
    while (!out_.empty() && !inputClosed_) {
        ssize_t n = write(inFd_, out_.data(), out_.size());
        if (n > 0) {
            out_.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!writeId_) writeId_ = g_unix_fd_add(inFd_, G_IO_OUT, onWritable, this);
            return;
        }
        out_.clear();   // EPIPE: the server is gone; its exit is handled elsewhere
    }
    if (writeId_) {
        g_source_remove(writeId_);
        writeId_ = 0;
    }
    if (closeWanted_ && !inputClosed_) doClose();
}

gboolean Server::onWritable(int, GIOCondition, gpointer selfp) {
    Server* s = static_cast<Server*>(selfp);
    s->writeId_ = 0;   // flushWrites adds a new watch if it is still needed
    s->flushWrites();
    return G_SOURCE_REMOVE;
}

void Server::closeInput() {
    closeWanted_ = true;
    if (out_.empty() && !inputClosed_) doClose();
}

void Server::doClose() {
    inputClosed_ = true;
    out_.clear();
    if (writeId_) {
        g_source_remove(writeId_);
        writeId_ = 0;
    }
    if (inFd_ >= 0) close(inFd_);
    inFd_ = -1;
}

bool Server::readAvailable() {
    if (outFd_ < 0) return true;
    char buf[65536];
    for (;;) {
        ssize_t n = read(outFd_, buf, sizeof buf);
        if (n > 0) {
            log("<-- ", buf, (size_t)n);
            client_->receive(buf, (size_t)n);
            continue;
        }
        if (n == 0) return true;
        if (errno == EINTR) continue;
        return !(errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

gboolean Server::onReadable(int, GIOCondition, gpointer selfp) {
    Server* s = static_cast<Server*>(selfp);
    std::shared_ptr<Server> keep = s->shared_from_this();
    if (s->readAvailable()) {
        s->readId_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void Server::onWaited(GObject* src, GAsyncResult* res, gpointer data) {
    auto* holder = static_cast<std::shared_ptr<Server>*>(data);
    std::shared_ptr<Server> s = *holder;
    delete holder;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, nullptr);
    s->ended();
}

void Server::ended() {
    if (!running_) return;
    std::shared_ptr<Server> keep = shared_from_this();
    if (readId_) readAvailable();   // whatever it wrote before going
    running_ = false;
    if (readId_) {
        g_source_remove(readId_);
        readId_ = 0;
    }
    doClose();
    auto& all = *gServers;
    all.erase(std::remove(all.begin(), all.end(), keep), all.end());
    if (onExit) {
        auto cb = onExit;
        cb(this);
    }
}

void Server::stop() {
    if (!running_ || stopping_) return;
    stopping_ = true;
    std::weak_ptr<Server> weak = shared_from_this();
    client_->shutdown([weak] {
        if (auto s = weak.lock()) s->closeInput();
    });
    // A server that does not answer shutdown within two seconds is told to
    // go, then made to.
    auto release = [](gpointer d) { delete static_cast<std::shared_ptr<Server>*>(d); };
    g_timeout_add_full(G_PRIORITY_DEFAULT, 2000, [](gpointer d) -> gboolean {
        std::shared_ptr<Server> s = *static_cast<std::shared_ptr<Server>*>(d);
        if (!s->running_) return G_SOURCE_REMOVE;
        s->closeInput();
        g_subprocess_send_signal(s->proc_, SIGTERM);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, [](gpointer d2) -> gboolean {
            auto& s2 = *static_cast<std::shared_ptr<Server>*>(d2);
            if (s2->running_) g_subprocess_force_exit(s2->proc_);
            return G_SOURCE_REMOVE;
        }, new std::shared_ptr<Server>(s),
           [](gpointer d2) { delete static_cast<std::shared_ptr<Server>*>(d2); });
        return G_SOURCE_REMOVE;
    }, new std::shared_ptr<Server>(shared_from_this()), release);
}

void Server::stopNow() {
    if (!running_) return;
    stopping_ = true;
    client_->exitNow();
    closeWanted_ = true;
    // Written out before returning, since the main loop will not run again.
    const gint64 deadline = g_get_monotonic_time() + 200000;
    while (!out_.empty() && !inputClosed_ && g_get_monotonic_time() < deadline) {
        struct pollfd p = {inFd_, POLLOUT, 0};
        poll(&p, 1, 20);
        flushWrites();
    }
    if (!inputClosed_) doClose();
}

}  // namespace lspgtk

using lspgtk::Server;

// Wait up to `ms` for every listed server to exit, without the main loop
// (which has stopped by the time the app quits). A pidfd turns readable when
// its process exits, and waiting on one does not reap it, so GLib's own child
// watch is left alone. Returns true when all have exited.
static bool waitForExit(const std::vector<std::shared_ptr<Server>>& list, int ms) {
#ifdef SYS_pidfd_open
    std::vector<struct pollfd> fds;
    for (const auto& s : list) {
        if (!s->running() || s->pid() <= 0) continue;
        int fd = (int)syscall(SYS_pidfd_open, s->pid(), 0);
        if (fd >= 0) fds.push_back({fd, POLLIN, 0});
    }
    const gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
    size_t pending = fds.size();
    while (pending > 0) {
        const gint64 left = (deadline - g_get_monotonic_time()) / 1000;
        if (left <= 0) break;
        if (poll(fds.data(), fds.size(), (int)left) < 0 && errno != EINTR) break;
        for (auto& p : fds)
            if (p.fd >= 0 && p.revents) {
                close(p.fd);
                p.fd = -1;   // poll ignores negative descriptors
                --pending;
            }
    }
    for (auto& p : fds) if (p.fd >= 0) close(p.fd);
    return pending == 0;
#else
    g_usleep((gulong)ms * 1000);
    return false;
#endif
}

void LspTerminateAllServers() {
    std::vector<std::shared_ptr<Server>> list = *lspgtk::gServers;
    if (list.empty()) return;
    for (auto& s : list) s->stopNow();
    // Half a second for a clean exit, then signals.
    if (waitForExit(list, 500)) return;
    for (auto& s : list) s->signal(SIGTERM);
    if (waitForExit(list, 300)) return;
    for (auto& s : list) s->signal(SIGKILL);
    waitForExit(list, 300);
}

std::size_t LspRunningServerCount() {
    std::size_t n = 0;
    for (auto& s : *lspgtk::gServers) if (s->running()) ++n;
    return n;
}

std::vector<int> LspServerPids() {
    std::vector<int> out;
    for (auto& s : *lspgtk::gServers) if (s->running()) out.push_back(s->pid());
    return out;
}
