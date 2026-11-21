// SessionServer.cpp — see SessionServer.h.
#include "SessionServer.h"

#include "TerminalScreen.h"
#include "Text.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Shared {

namespace {

// A connection's line, or what waits to be written to it, past which the
// other side is taken for broken: a text frame is a document's whole text.
constexpr std::size_t kMaxLine = std::size_t(512) << 20;
constexpr std::size_t kMaxPending = std::size_t(512) << 20;

void setFlags(int fd, bool nonBlocking) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonBlocking) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

ssize_t sendSome(int fd, const char* p, std::size_t n) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, p, n, MSG_NOSIGNAL);
#else
    return ::send(fd, p, n, 0);
#endif
}

// Whether the process at the other end of `fd` is this user's.
bool peerIsUs(int fd) {
#ifdef __APPLE__
    uid_t uid = 0;
    gid_t gid = 0;
    return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#else
    struct ucred cred;
    socklen_t len = sizeof cred;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == getuid();
#endif
}

bool socketAddress(const std::string& path, sockaddr_un* out) {
    std::memset(out, 0, sizeof *out);
    out->sun_family = AF_UNIX;
    if (path.size() >= sizeof out->sun_path) return false;
    std::memcpy(out->sun_path, path.c_str(), path.size() + 1);
    return true;
}

// `program` as given when it holds a '/', else the first executable of
// that name in the PATH `env` names (then /usr/bin and /bin).
std::string findInPath(const std::string& program, const std::vector<std::string>& env) {
    if (program.find('/') != std::string::npos) return program;
    std::string path;
    for (const std::string& e : env)
        if (e.compare(0, 5, "PATH=") == 0) path = e.substr(5);
    path += path.empty() ? "/usr/bin:/bin" : ":/usr/bin:/bin";
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t colon = path.find(':', at);
        const std::string dir = path.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (!dir.empty()) {
            const std::string p = dir + "/" + program;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0) return p;
        }
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    return program;
}

// All of `data` to a pipe, blocking; false once nobody reads it. Never
// SIGPIPE: on macOS the pipe says so (F_SETNOSIGPIPE), elsewhere the
// signal is held off this thread for the write and taken back if it came.
bool writeAllToPipe(int fd, const std::string& data) {
#ifndef F_SETNOSIGPIPE
    sigset_t pipeSig, old;
    sigemptyset(&pipeSig);
    sigaddset(&pipeSig, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSig, &old);
#endif
    bool ok = true;
    std::size_t at = 0;
    while (at < data.size()) {
        const ssize_t n = ::write(fd, data.data() + at, data.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        at += (std::size_t)n;
    }
#ifndef F_SETNOSIGPIPE
    if (!ok && errno == EPIPE) {
        const struct timespec zero = {0, 0};
        sigtimedwait(&pipeSig, nullptr, &zero);
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
#endif
    return ok;
}

// Complete lines at the end of `buf` (from `scanned` on) go to `each`, and
// are taken off it. False when a line is not a message.
bool takeLines(std::string& buf, std::size_t& scanned,
               const std::function<void(Message&&)>& each) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = buf.find('\n', std::max(start, scanned));
        if (nl == std::string::npos) break;
        Message m;
        if (!decode(buf.substr(start, nl - start), &m)) return false;
        each(std::move(m));
        start = nl + 1;
        scanned = start;
    }
    buf.erase(0, start);
    scanned = buf.size();
    return true;
}

}  // namespace

std::string resolveFolder(const std::string& dir) {
    char buf[PATH_MAX];
    if (dir.empty() || !realpath(dir.c_str(), buf)) return "";
    struct stat st;
    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return "";
    return buf;
}

bool stdioTarget(const std::string& path, StdioTarget* out, std::string* why) {
    *out = StdioTarget();
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    struct stat st;
    const bool there = !p.empty() && stat(p.c_str(), &st) == 0;
    if (there && S_ISDIR(st.st_mode)) {
        out->folder = resolveFolder(p);
        if (out->folder.empty() && why) *why = path + " is not a folder";
        return !out->folder.empty();
    }
    const std::size_t slash = p.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : p.substr(0, slash);
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") {
        if (why) *why = path + ": no such file or folder";
        return false;
    }
    out->folder = resolveFolder(dir);
    if (out->folder.empty()) {
        if (why) *why = path + (there ? " is not in a folder" : ": no such file or folder");
        return false;
    }
    out->file = (out->folder == "/" ? "" : out->folder) + "/" + name;
    out->isNew = !there;
    return true;
}

std::string hostSocketName(const std::string& folder) {
    // FNV-1a, 64 bits: a name, not a secret. Two folders sharing one would
    // find each other's host, which says which folder it serves (Welcome).
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : folder) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    return std::string("host-") + hex + ".sock";
}

bool trustedSocketFolder(const std::string& dir) {
    struct stat st;
    return lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
           (st.st_mode & 077) == 0;
}

bool makeSocketFolder(const std::string& dir, std::string* error) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        if (error) *error = dir + ": " + std::strerror(errno);
        return false;
    }
    if (!trustedSocketFolder(dir)) {
        if (error) *error = dir + " is not a folder of this user's that only they can enter";
        return false;
    }
    return true;
}

std::string findHostSocket(const std::string& folder, const std::vector<std::string>& dirs) {
    const std::string name = hostSocketName(folder);
    for (const std::string& dir : dirs) {
        if (!trustedSocketFolder(dir)) continue;
        const std::string path = dir + "/" + name;
        struct stat st;
        if (lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == getuid())
            return path;
    }
    return "";
}

// ---------------------------------------------------------------- HostServer

HostServer::HostServer() {
    if (pipe(wake_) == 0)
        for (int fd : wake_) setFlags(fd, true);
}

HostServer::~HostServer() {
    // The host first: its thread may still be sending to the connections.
    host_.reset();
    for (auto& [id, c] : conns_) {
        if (c->fd >= 0) ::close(c->fd);
        if (c->outFd >= 0) ::close(c->outFd);
    }
    conns_.clear();
    if (listen_ >= 0) {
        ::close(listen_);
        unlink(socketPath_.c_str());
    }
    // The lock file stays: removing it would let a host starting now lock a
    // new file while another holds the old one.
    if (lock_ >= 0) ::close(lock_);
    for (int fd : wake_)
        if (fd >= 0) ::close(fd);
}

HostServer::Start HostServer::start(const HostOptions& o, std::string* error) {
    opt_ = o;
    if (!makeSocketFolder(o.socketDir, error)) return Start::Failed;
    const std::string name = hostSocketName(o.folder);
    socketPath_ = o.socketDir + "/" + name;
    lockPath_ = o.socketDir + "/" + name.substr(0, name.size() - 5) + ".lock";
    sockaddr_un addr;
    if (!socketAddress(socketPath_, &addr)) {
        if (error) *error = socketPath_ + ": the path is too long for a socket";
        return Start::Failed;
    }
    lock_ = ::open(lockPath_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0) {
        if (error) *error = lockPath_ + ": " + std::strerror(errno);
        return Start::Failed;
    }
    if (flock(lock_, LOCK_EX | LOCK_NB) != 0) {
        const bool held = errno == EWOULDBLOCK;
        if (error) *error = held ? socketPath_ : lockPath_ + ": " + std::strerror(errno);
        ::close(lock_);
        lock_ = -1;
        return held ? Start::Running : Start::Failed;
    }
    // Holding the lock, nothing listens on a socket already there: a host
    // that died left it.
    unlink(socketPath_.c_str());
    listen_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_ < 0 || bind(listen_, (sockaddr*)&addr, sizeof addr) != 0 ||
        chmod(socketPath_.c_str(), 0600) != 0 || ::listen(listen_, 16) != 0) {
        if (error) *error = socketPath_ + ": " + std::strerror(errno);
        if (listen_ >= 0) ::close(listen_);
        listen_ = -1;
        unlink(socketPath_.c_str());
        return Start::Failed;
    }
    setFlags(listen_, true);
    makeHost(o);
    return Start::Ok;
}

HostServer::Start HostServer::startStdio(const HostOptions& o, int in, int out, std::string* error) {
    opt_ = o;
    if (in < 0 || out < 0) {
        if (error) *error = "no standard input or output";
        return Start::Failed;
    }
    stdio_ = true;
    for (int fd : {in, out}) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
#ifdef F_SETNOSIGPIPE
    fcntl(out, F_SETNOSIGPIPE, 1);   // a reader gone is EPIPE, not a signal (macOS)
#endif
    makeHost(o);
    auto c = std::make_shared<Conn>();
    c->fd = in;
    c->outFd = out;
    c->id = host_->connect();
    std::lock_guard<std::mutex> lk(mu_);
    conns_[c->id] = c;
    return Start::Ok;
}

bool HostServer::makeHost(const HostOptions& o) {
    host_ = std::make_unique<SessionHost>(
        [this](ClientId to, Message&& m) {
            std::string line = encode(m);
            line += '\n';
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = conns_.find(to);
                if (it == conns_.end() || it->second->dead) return;
                Conn& c = *it->second;
                if (c.out.size() + line.size() > kMaxPending) c.dead = true;
                else c.out += line;
            }
            wake();
        },
        o.delayUs);
    host_->setFolder(o.folder);
    if (o.watchDisk) host_->watchDisk(true);   // before any renderer: the Welcome says it
    if (!o.backupDir.empty()) {
        Host::BackupKeeping k;
        k.store.dir = o.backupDir;
        k.store.fs = Backups::posixFs();
        k.root = o.folder;
        k.ownerPid = (std::int64_t)getpid();
        k.ownerStart = Backups::processStart(k.ownerPid);
        host_->keepBackups(std::move(k));
    }
    if (!o.shellEnv.empty() || !o.loginShell.empty()) host_->setOwnShell(o.shellEnv, o.loginShell);
    if (!o.askedFile.empty()) host_->setAskedFile(o.askedFile, o.askedFileNew);
    return true;
}

void HostServer::wake() {
    const char b = 1;
    while (write(wake_[1], &b, 1) < 0 && errno == EINTR) {}
}

void HostServer::stop() {
    stop_ = true;
    wake();
}

std::size_t HostServer::renderers() {
    std::lock_guard<std::mutex> lk(mu_);
    return conns_.size();
}

void HostServer::accept() {
    for (;;) {
        const int fd = ::accept(listen_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;   // EAGAIN, or a connection that went before it was taken
        }
        if (!peerIsUs(fd)) {
            ::close(fd);
            continue;
        }
        setFlags(fd, true);
        auto c = std::make_shared<Conn>();
        c->fd = fd;
        c->id = host_->connect();   // never under mu_: the host's thread takes it to send
        std::lock_guard<std::mutex> lk(mu_);
        conns_[c->id] = c;
    }
}

void HostServer::readFrom(Conn& c) {
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(c.fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) break;
        c.in.append(buf, (std::size_t)n);
        const ClientId id = c.id;
        if (!takeLines(c.in, c.scanned, [&](Message&& m) { host_->post(id, std::move(m)); }) ||
            c.in.size() > kMaxLine)
            break;
    }
    std::lock_guard<std::mutex> lk(mu_);
    c.dead = true;
}

void HostServer::writeTo(Conn& c) {
    while (!c.out.empty() && !c.dead) {
        // A pipe (stdio) takes write(2), a socket send(2) without SIGPIPE.
        const ssize_t n = c.outFd >= 0 ? ::write(c.outFd, c.out.data(), c.out.size())
                                       : sendSome(c.fd, c.out.data(), c.out.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) {
            c.dead = true;
            return;
        }
        c.out.erase(0, (std::size_t)n);
    }
}

bool HostServer::run() {
    using Clock = std::chrono::steady_clock;
    const long long checkMs = std::max(5LL, std::min(1000LL, opt_.idleMs / 4));
    Clock::time_point lastLeft = Clock::now();
    std::vector<std::shared_ptr<Conn>> polled;
    std::vector<pollfd> polls;
    // Each polled connection's entries in `polls`: what it is read from,
    // and what it is written to when that is another descriptor (stdio's
    // two pipes), else -1.
    std::vector<std::pair<size_t, long>> at;
    for (;;) {
        if (stop_) return false;
        polled.clear();
        at.clear();
        polls.assign({pollfd{wake_[0], POLLIN, 0}, pollfd{listen_, POLLIN, 0}});
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [id, c] : conns_) {
                if (c->dead) continue;
                polled.push_back(c);
                const short out = c->out.empty() ? 0 : POLLOUT;
                if (c->outFd >= 0) {
                    at.push_back({polls.size(), (long)polls.size() + 1});
                    polls.push_back(pollfd{c->fd, POLLIN, 0});
                    polls.push_back(pollfd{c->outFd, out, 0});
                } else {
                    at.push_back({polls.size(), -1});
                    polls.push_back(pollfd{c->fd, (short)(POLLIN | out), 0});
                }
            }
        }
        int n;
        do n = poll(polls.data(), (nfds_t)polls.size(), polled.empty() ? (int)checkMs : 1000);
        while (n < 0 && errno == EINTR);
        char buf[64];
        while (read(wake_[0], buf, sizeof buf) > 0) {}
        if (n > 0) {
            if (polls[1].revents) accept();
            for (size_t i = 0; i < polled.size(); i++) {
                const short r = polls[at[i].first].revents;
                const short w = at[i].second >= 0 ? polls[(size_t)at[i].second].revents : r;
                if (r & (POLLIN | POLLHUP | POLLERR)) readFrom(*polled[i]);
                if (at[i].second >= 0 && (w & (POLLHUP | POLLERR))) {
                    std::lock_guard<std::mutex> lk(mu_);
                    polled[i]->dead = true;   // nobody reads what it writes
                }
                if (w & POLLOUT) {
                    std::lock_guard<std::mutex> lk(mu_);
                    writeTo(*polled[i]);
                }
            }
        }
        // Connections gone: their renderers detach, everything else stays.
        std::vector<std::shared_ptr<Conn>> gone;
        bool none;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = conns_.begin(); it != conns_.end();) {
                if (it->second->dead) {
                    gone.push_back(it->second);
                    it = conns_.erase(it);
                } else {
                    ++it;
                }
            }
            none = conns_.empty();
        }
        for (auto& c : gone) {
            ::close(c->fd);
            c->fd = -1;
            if (c->outFd >= 0) ::close(c->outFd);
            c->outFd = -1;
            host_->disconnect(c->id);
        }
        // Over stdio the one renderer is the host's reason to be: its input
        // closed (the connection carrying it went), so the host ends.
        if (stdio_ && !gone.empty()) return true;
        if (!none) continue;
        if (!gone.empty()) lastLeft = Clock::now();
        const long long quiet =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastLeft).count();
        if (quiet < opt_.idleMs) continue;
        // Busy, too, when the host's thread does not answer.
        std::string why = "the host's thread did not answer";
        host_->inspect([&why](const Host& h) { why = h.busyReason(); });
        if (why.empty()) return true;
    }
}

// ---------------------------------------------------------------- HostLink

std::unique_ptr<HostLink> HostLink::open(const std::string& path, std::string* error,
                                         bool* refused) {
    if (refused) *refused = false;
    sockaddr_un addr;
    if (!socketAddress(path, &addr)) {
        if (error) *error = path + ": the path is too long for a socket";
        return nullptr;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = std::strerror(errno);
        return nullptr;
    }
    setFlags(fd, false);
    int r;
    do r = ::connect(fd, (sockaddr*)&addr, sizeof addr);
    while (r != 0 && errno == EINTR);
    if (r != 0) {
        const int e = errno;
        if (refused) *refused = e == ECONNREFUSED || e == ENOENT;
        if (error) *error = path + ": " + std::strerror(e);
        ::close(fd);
        return nullptr;
    }
    if (!peerIsUs(fd)) {
        if (error) *error = path + ": the host is another user's";
        ::close(fd);
        return nullptr;
    }
    return std::unique_ptr<HostLink>(new HostLink(fd));
}

HostLink::~HostLink() { close(); }

void HostLink::start(std::function<void(Message&&)> deliver, std::function<void()> closed) {
    reader_ = std::thread([this, deliver = std::move(deliver), closed = std::move(closed)] {
        std::string in;
        std::size_t scanned = 0;
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            in.append(buf, (std::size_t)n);
            if (!takeLines(in, scanned, deliver) || in.size() > kMaxLine) break;
        }
        closed_ = true;
        // A command's end (ssh's): its status and last words, for whoever
        // says why the host went.
        if (pid_ > 0) {
            reap(2000);
            std::unique_lock<std::mutex> lk(errMu_);
            errDone_.wait_for(lk, std::chrono::milliseconds(500), [this] { return errEnded_; });
        }
        if (closed) closed();
    });
}

std::unique_ptr<HostLink> HostLink::spawn(const std::vector<std::string>& argvIn,
                                          const std::vector<std::string>& env, std::string* error) {
    if (argvIn.empty() || argvIn[0].empty()) {
        if (error) *error = "no command to run";
        return nullptr;
    }
    const std::string program = findInPath(argvIn[0], env);
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    auto closeAll = [&] {
        for (int* p : {in, out, err})
            for (int i = 0; i < 2; i++)
                if (p[i] >= 0) ::close(p[i]);
    };
    if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0) {
        if (error) *error = std::string("no pipe: ") + std::strerror(errno);
        closeAll();
        return nullptr;
    }
    for (int* p : {in, out, err})
        for (int i = 0; i < 2; i++) fcntl(p[i], F_SETFD, FD_CLOEXEC);
    std::vector<std::string> args = argvIn, envCopy = env;
    std::vector<char*> argv, envp;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    for (std::string& e : envCopy) envp.push_back(&e[0]);
    envp.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;   // a group of its own: hung up as one (close)
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;   // nothing else of this process's
#endif
    posix_spawnattr_setflags(&at, flags);
    pid_t pid = 0;
    const int r = posix_spawn(&pid, program.c_str(), &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (r != 0) {
        if (error) *error = "could not run " + argvIn[0] + ": " + std::strerror(r);
        ::close(in[1]);
        ::close(out[0]);
        ::close(err[0]);
        return nullptr;
    }
#ifdef F_SETNOSIGPIPE
    fcntl(in[1], F_SETNOSIGPIPE, 1);   // the command gone is EPIPE, not a signal (macOS)
#endif
    std::unique_ptr<HostLink> link(new HostLink(out[0]));
    link->out_ = in[1];
    link->err_ = err[0];
    link->pid_ = (int)pid;
    HostLink* self = link.get();
    link->errReader_ = std::thread([self] {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(self->err_, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            std::lock_guard<std::mutex> lk(self->errMu_);
            self->errText_.append(buf, (std::size_t)n);
            if (self->errText_.size() > 8192) self->errText_.erase(0, self->errText_.size() - 8192);
        }
        std::lock_guard<std::mutex> lk(self->errMu_);
        self->errEnded_ = true;
        self->errDone_.notify_all();
    });
    return link;
}

std::string HostLink::stderrText() {
    std::lock_guard<std::mutex> lk(errMu_);
    return errText_;
}

// Waits up to `waitMs` for the command to end, taking its status.
void HostLink::reap(int waitMs) {
    if (pid_ <= 0 || status_.load() != -1) return;
    // A deadline by the clock, not a count of naps: on a loaded machine
    // each nap can last far longer than asked, which stretched the waits
    // counted that way well past what they say.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    for (;;) {
        int st = 0;
        const pid_t r = waitpid(pid_, &st, WNOHANG);
        if (r == pid_) {
            status_ = st;
            return;
        }
        if (r < 0 && errno != EINTR) {
            status_ = 0;   // reaped elsewhere (a subreaper): nothing to say
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) return;
        usleep(10 * 1000);
    }
}

void HostLink::connect(Client& client) {
    start([&client](Message&& m) { client.deliver(std::move(m)); }, [&client] { client.hostLost(); });
    client.connect([this](Message&& m) { send(m); });
}

bool HostLink::send(const Message& m) {
    std::string line = encode(m);
    line += '\n';
    std::lock_guard<std::mutex> lk(writeMu_);
    if (fd_ < 0 || closed_) return false;
    if (pid_ > 0) return out_ >= 0 && writeAllToPipe(out_, line);
    std::size_t at = 0;
    while (at < line.size()) {
        const ssize_t n = sendSome(fd_, line.data() + at, line.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        at += (std::size_t)n;
    }
    return true;
}

void HostLink::close() {
    if (pid_ > 0) {
        // A command: its input closed, which ends a `mutible host --stdio`
        // and so the ssh carrying it; one that stays is hung up, then
        // killed, as a group (its own session).
        {
            std::lock_guard<std::mutex> lk(writeMu_);
            if (out_ >= 0) ::close(out_);
            out_ = -1;
        }
        // Deadlines by the clock (reap's note).
        auto ended = [this](int ms) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (!closed_.load() && std::chrono::steady_clock::now() < deadline) usleep(10 * 1000);
            return closed_.load() || !reader_.joinable();
        };
        if (reader_.joinable() && !ended(1500)) {
            kill(-pid_, SIGHUP);
            kill(pid_, SIGHUP);
            if (!ended(1000)) {
                kill(-pid_, SIGKILL);
                kill(pid_, SIGKILL);
            }
        }
        if (reader_.joinable()) reader_.join();
        if (status_.load() == -1) {
            // Never started reading, or the command outlived its output.
            reap(200);
            if (status_.load() == -1) {
                kill(-pid_, SIGKILL);
                kill(pid_, SIGKILL);
                int st = 0;
                while (waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
                status_ = st;
            }
        }
        if (errReader_.joinable()) errReader_.join();
        std::lock_guard<std::mutex> lk(writeMu_);
        if (fd_ >= 0) ::close(fd_);
        if (err_ >= 0) ::close(err_);
        fd_ = err_ = -1;
        closed_ = true;
        return;
    }
    int fd;
    {
        std::lock_guard<std::mutex> lk(writeMu_);
        fd = fd_;
        if (fd >= 0) shutdown(fd, SHUT_RDWR);   // the reader sees the end
    }
    if (reader_.joinable()) reader_.join();
    std::lock_guard<std::mutex> lk(writeMu_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    closed_ = true;
}

// ---------------------------------------------------------------- the dump

namespace {

// A terminal's replay, collected.
struct Collect : Client::TermListener {
    std::string bytes;
    bool replayed = false, exited = false;
    void termOutput(TermId, const std::string& b, long long, bool, bool replay) override {
        if (replay) {
            bytes = b;
            replayed = true;
        } else if (replayed) {
            bytes += b;
        }
    }
    void termExited(TermId, int, const std::string&) override { exited = true; }
};

}  // namespace

std::string dumpSession(Client& client) {
    const Json state = client.sessionState();
    if (!state.isObject()) return "";
    std::string out = "== host " + state["folder"].asString() + "\n";
    const Json& docs = state["documents"];
    for (size_t i = 0; i < docs.size(); i++) {
        const Json& d = docs[i];
        const DocId id = (DocId)d["doc"].asNumber();
        const Client::HostDoc h = client.hostDocument(id);
        out += "== document " + std::to_string(id) + " " + d["path"].asString() + " (version " +
               std::to_string(h.version) + (h.dirty ? ", unsaved" : ", saved") + ")\n";
        std::string text = Model::utf8FromUtf16(h.text);
        if (!text.empty() && text.back() != '\n') text += '\n';
        out += text;
    }
    const Json& terms = state["terminals"];
    std::vector<std::unique_ptr<Collect>> seen;
    for (size_t i = 0; i < terms.size(); i++) {
        seen.push_back(std::make_unique<Collect>());
        client.attachTerminal((TermId)terms[i]["term"].asNumber(), seen.back().get());
    }
    client.settle();   // the replays come before the answer to it
    for (size_t i = 0; i < terms.size(); i++) {
        const Json& t = terms[i];
        const int cols = std::max(2, (int)t["cols"].asNumber(80));
        const int rows = std::max(1, (int)t["rows"].asNumber(24));
        const TermShell sh = termShellFrom(t["shell"]);
        out += "== terminal " + std::to_string((long long)t["term"].asNumber()) + " (" +
               std::to_string(cols) + "x" + std::to_string(rows) + ", shell " +
               std::to_string((long long)t["pid"].asNumber());
        if (sh.busy) out += ", running " + (sh.command.empty() ? std::string("a command") : sh.command);
        if (!sh.cwd.empty()) out += ", in " + sh.cwd;
        out += seen[i]->exited ? ", exited)\n" : ")\n";
        TerminalScreen screen(cols, rows);
        screen.feed(seen[i]->bytes);
        std::vector<std::string> lines;
        for (int r = 0; r < screen.rows(); r++) lines.push_back(screen.lineText(r));
        while (!lines.empty() && lines.back().find_first_not_of(' ') == std::string::npos)
            lines.pop_back();
        for (std::string& l : lines) {
            while (!l.empty() && l.back() == ' ') l.pop_back();
            out += l + "\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------- ssh://

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX decoded; false for a '%' not followed by two hex digits.
bool percentDecode(const std::string& in, std::string* out) {
    out->clear();
    for (std::size_t i = 0; i < in.size(); i++) {
        if (in[i] != '%') {
            *out += in[i];
            continue;
        }
        if (i + 2 >= in.size() || hexValue(in[i + 1]) < 0 || hexValue(in[i + 2]) < 0) return false;
        *out += (char)(hexValue(in[i + 1]) * 16 + hexValue(in[i + 2]));
        i += 2;
    }
    return true;
}

bool hostNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_';
}

// The destination as a person types it to ssh: [-p PORT ][user@]host.
std::string sshAsTyped(const SshTarget& t) {
    std::string s = "ssh ";
    if (t.port) s += "-p " + std::to_string(t.port) + " ";
    if (!t.user.empty()) s += t.user + "@";
    return s + t.host;
}

// The last line with something on it.
std::string lastLine(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0) {
        std::size_t start = text.rfind('\n', end - 1);
        start = start == std::string::npos ? 0 : start + 1;
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) return line;
        if (start == 0) break;
        end = start - 1;
    }
    return "";
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

bool parseSshUrl(const std::string& url, SshTarget* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    SshTarget t;
    std::string lower = url.substr(0, std::min<std::size_t>(6, url.size()));
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower != "ssh://") return fail("not an ssh:// address: " + url);
    const std::string rest = url.substr(6);
    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    const std::string rawPath = slash == std::string::npos ? "" : rest.substr(slash);
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        if (!percentDecode(authority.substr(0, at), &t.user) || t.user.empty())
            return fail("no user before the @ in " + url);
        for (char c : t.user)
            if ((unsigned char)c <= ' ' || c == 0x7f || c == ':' || c == '@' || c == '/')
                return fail("the user in " + url + " is not a user name");
        if (t.user[0] == '-') return fail("the user in " + url + " would read as an option to ssh");
        authority = authority.substr(at + 1);
    }
    std::string portText;
    if (!authority.empty() && authority[0] == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return fail("no ] after the address in " + url);
        t.host = authority.substr(1, close - 1);
        const std::string after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return fail("what follows the ] in " + url + " is not a port");
            portText = after.substr(1);
        }
        if (t.host.empty()) return fail("no host in " + url);
        for (char c : t.host)
            if (!(std::isxdigit((unsigned char)c) || c == ':' || c == '.' || c == '%' || hostNameChar(c)))
                return fail("the address in " + url + " is not an IPv6 address");
    } else {
        const std::size_t colon = authority.find(':');
        t.host = authority.substr(0, colon);
        if (colon != std::string::npos) portText = authority.substr(colon + 1);
        if (t.host.empty()) return fail("no host in " + url);
        for (char c : t.host)
            if (!hostNameChar(c)) return fail("the host in " + url + " is not a host name");
    }
    if (t.host[0] == '-') return fail("the host in " + url + " would read as an option to ssh");
    if (!portText.empty() || (authority.find(':') != std::string::npos && authority[0] != '[')) {
        if (portText.empty() || portText.size() > 5 ||
            portText.find_first_not_of("0123456789") != std::string::npos)
            return fail("the port in " + url + " is not a number from 1 to 65535");
        t.port = std::atoi(portText.c_str());
        if (t.port < 1 || t.port > 65535) return fail("the port in " + url + " is not a number from 1 to 65535");
    }
    std::string path;
    if (!percentDecode(rawPath, &path)) return fail("a % in " + url + " is not followed by two hex digits");
    for (char c : path)
        if (c == '\0' || c == '\n' || c == '\r') return fail("the path in " + url + " holds a line break or a NUL");
    if (path.empty() || path == "/") path = "~";
    else if (path == "/~") path = "~";
    else if (path.compare(0, 3, "/~/") == 0) path = path.substr(1);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    t.path = path;
    *out = t;
    return true;
}

std::string sshLabel(const SshTarget& t) {
    return (t.user.empty() ? "" : t.user + "@") + t.host + ":" + t.path;
}

std::string sshMachineKey(const SshTarget& t) {
    std::string host = t.host.find(':') != std::string::npos ? "[" + t.host + "]" : t.host;
    return "ssh://" + (t.user.empty() ? "" : t.user + "@") + host +
           (t.port ? ":" + std::to_string(t.port) : "");
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::vector<std::string> sshCommand(const SshTarget& t, const std::string& ssh) {
    std::vector<std::string> a = {ssh.empty() ? std::string("ssh") : ssh,
                                  "-T",
                                  "-o", "BatchMode=yes",
                                  "-o", "ConnectTimeout=20",
                                  "-o", "ServerAliveInterval=15",
                                  "-o", "ServerAliveCountMax=3",
                                  "-e", "none"};
    if (t.port) {
        a.push_back("-p");
        a.push_back(std::to_string(t.port));
    }
    if (!t.user.empty()) {
        a.push_back("-l");
        a.push_back(t.user);
    }
    a.push_back("--");
    a.push_back(t.host);
    a.push_back("mutible host --stdio " + shellQuote(t.path));
    return a;
}

std::string explainSshFailure(const SshTarget& t, int status, const std::string& err) {
    const std::string who = (t.user.empty() ? "" : t.user + "@") + t.host;
    const std::string typed = sshAsTyped(t);
    const int code = status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const std::string last = lastLine(err);
    if (contains(err, "Host key verification failed") || contains(err, "IDENTIFICATION HAS CHANGED") ||
        contains(err, "authenticity of host") || contains(err, "No ED25519 host key is known") ||
        contains(err, "host key is known"))
        return "ssh does not know " + who + "'s host key yet, or it changed, and a window cannot ask " +
               "you to confirm it. Run `" + typed + "` once in a terminal to check and accept it, " +
               "then open the address again.";
    if (contains(err, "Permission denied") || contains(err, "passphrase") ||
        contains(err, "password") || contains(err, "Too many authentication failures"))
        return "ssh to " + who + " needs a password or a key's passphrase, which a window cannot " +
               "ask for. Run `" + typed + "` once in a terminal (with your key added to the agent: " +
               "ssh-add), then open the address again.";
    if (contains(err, "Could not resolve hostname") || contains(err, "nodename nor servname"))
        return "ssh could not find " + t.host + ": " + last;
    if (code == 127 || contains(err, "mutible: command not found") || contains(err, "mutible: not found"))
        return "mutible is not installed on " + who + ", or not on the PATH a command over ssh " +
               "gets there. Install it from the PPA (ppa:echansen/minicode) or a build, then open " +
               "the address again.";
    if (contains(err, "unknown option --stdio") || contains(err, "unknown command host") ||
        contains(err, "unknown option --stdio for host"))
        return "mutible on " + who + " is too old for a window over SSH (it has no `mutible host " +
               "--stdio`). Upgrade it there, then open the address again.";
    if (code == 255)
        return "ssh to " + who + " failed" + (last.empty() ? std::string(".") : ": " + last);
    if (!last.empty()) return "The host on " + who + " ended: " + last;
    if (code >= 0) return "The host on " + who + " ended (exit status " + std::to_string(code) + ").";
    if (status >= 0 && WIFSIGNALED(status))
        return "The connection to " + who + " ended (signal " + std::to_string(WTERMSIG(status)) + ").";
    return "The connection to " + who + " closed.";
}

}  // namespace Shared
