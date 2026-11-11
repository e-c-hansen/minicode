// ControlServer.cpp — see ControlServer.h.
#include "ControlServer.h"

#include <glib-unix.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "ControlPaths.h"

namespace {

// A request line longer than this closes the connection: nothing the
// protocol has needs a megabyte.
const size_t kMaxLine = 1 << 20;
// A peer that takes nothing of an answer for this long is dropped.
const gint64 kStallMicros = 10 * G_USEC_PER_SEC;

char gSocketPath[sizeof(((struct sockaddr_un*)nullptr)->sun_path)];

void removeSocketAtExit() {
    if (gSocketPath[0]) unlink(gSocketPath);
}

// A folder the server may put its socket in: a real folder (not a link) of
// this user's that nobody else can enter.
bool privateFolderOfOurs(const std::string& dir, std::string* error) {
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            *error = dir + ": " + std::strerror(errno);
            return false;
        }
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            *error = "cannot make " + dir + ": " + std::strerror(errno);
            return false;
        }
        if (lstat(dir.c_str(), &st) != 0) {
            *error = dir + ": " + std::strerror(errno);
            return false;
        }
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
        *error = dir + " is not a private folder of yours (a real folder, mode 0700, "
                       "owned by you); the control channel is off";
        return false;
    }
    return true;
}

}  // namespace

namespace {
struct Conn;
}

struct ControlServer::Impl {
    ControlChannel::Dispatcher* dispatcher = nullptr;
    int listenFd = -1;
    guint listenId = 0;
    std::vector<std::shared_ptr<Conn>> conns;
};

namespace {

struct Conn {
    ControlServer::Impl* srv = nullptr;
    int fd = -1;
    std::string in;           // read, not yet handled
    std::string out;          // the answer being written
    size_t outOff = 0;
    guint readId = 0, writeId = 0, stallId = 0;
    bool busy = false;        // a request is with its handler
    bool eof = false;         // the peer sent all it will send
    bool closed = false;
    gint64 progress = 0;      // when a byte was last taken, in microseconds
};

void pump(const std::shared_ptr<Conn>& c);

void closeConn(const std::shared_ptr<Conn>& c) {
    if (c->closed) return;
    c->closed = true;
    if (c->readId) g_source_remove(c->readId);
    if (c->writeId) g_source_remove(c->writeId);
    if (c->stallId) g_source_remove(c->stallId);
    c->readId = c->writeId = c->stallId = 0;
    close(c->fd);
    auto& v = c->srv->conns;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == c) {
            v.erase(v.begin() + (long)i);
            break;
        }
}

gboolean onReadable(gint, GIOCondition, gpointer data);
gboolean onWritable(gint, GIOCondition, gpointer data);
gboolean onStall(gpointer data);

std::shared_ptr<Conn> find(ControlServer::Impl* srv, Conn* raw) {
    for (auto& c : srv->conns)
        if (c.get() == raw) return c;
    return nullptr;
}

// Reads only while no request is out: a source left on while the last
// answer is pending would spin on the bytes it does not take.
void setReading(const std::shared_ptr<Conn>& c, bool on) {
    if (on && !c->readId)
        c->readId = g_unix_fd_add(c->fd, (GIOCondition)(G_IO_IN | G_IO_HUP | G_IO_ERR), onReadable,
                                  c.get());
    if (!on && c->readId) {
        g_source_remove(c->readId);
        c->readId = 0;
    }
}

// Writes what it can; the rest waits for the loop. When all is out the
// connection takes its next request.
void flush(const std::shared_ptr<Conn>& c) {
    while (!c->closed && c->outOff < c->out.size()) {
        ssize_t n = send(c->fd, c->out.data() + c->outOff, c->out.size() - c->outOff,
                         MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!c->writeId)
                c->writeId = g_unix_fd_add(c->fd, G_IO_OUT, onWritable, c.get());
            if (!c->stallId) c->stallId = g_timeout_add_seconds(1, onStall, c.get());
            return;
        }
        if (n <= 0) {
            closeConn(c);
            return;
        }
        c->outOff += (size_t)n;
        c->progress = g_get_monotonic_time();
    }
    if (c->closed) return;
    c->out.clear();
    c->outOff = 0;
    if (c->writeId) { g_source_remove(c->writeId); c->writeId = 0; }
    if (c->stallId) { g_source_remove(c->stallId); c->stallId = 0; }
    c->busy = false;
    pump(c);
}

gboolean onWritable(gint, GIOCondition, gpointer data) {
    Conn* raw = static_cast<Conn*>(data);
    std::shared_ptr<Conn> c = find(raw->srv, raw);
    if (!c) return G_SOURCE_REMOVE;
    c->writeId = 0;   // this source ends by returning REMOVE; flush adds it again if needed
    flush(c);
    return G_SOURCE_REMOVE;
}

gboolean onStall(gpointer data) {
    Conn* raw = static_cast<Conn*>(data);
    std::shared_ptr<Conn> c = find(raw->srv, raw);
    if (!c) return G_SOURCE_REMOVE;
    if (g_get_monotonic_time() - c->progress > kStallMicros) {
        c->stallId = 0;
        closeConn(c);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// Takes the next whole line, if the connection is free for it.
void pump(const std::shared_ptr<Conn>& c) {
    if (c->closed || c->busy) return;
    size_t nl = c->in.find('\n');
    if (nl == std::string::npos) {
        if (c->eof) { closeConn(c); return; }
        setReading(c, true);
        return;
    }
    std::string line = c->in.substr(0, nl);
    c->in.erase(0, nl + 1);
    c->busy = true;
    c->progress = g_get_monotonic_time();
    setReading(c, false);
    std::weak_ptr<Conn> weak = c;
    c->srv->dispatcher->handle(line, [weak](const std::string& answer) {
        std::shared_ptr<Conn> conn = weak.lock();
        if (!conn || conn->closed) return;
        conn->out = answer;
        conn->outOff = 0;
        conn->progress = g_get_monotonic_time();
        flush(conn);
    });
}

gboolean onReadable(gint, GIOCondition, gpointer data) {
    Conn* raw = static_cast<Conn*>(data);
    std::shared_ptr<Conn> c = find(raw->srv, raw);
    if (!c) return G_SOURCE_REMOVE;
    char buf[65536];
    for (;;) {
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) { c->eof = true; break; }
        c->in.append(buf, (size_t)n);
        if (c->in.size() > kMaxLine && c->in.find('\n') == std::string::npos) {
            c->readId = 0;
            closeConn(c);
            return G_SOURCE_REMOVE;
        }
        if (c->in.find('\n') != std::string::npos) break;   // one request at a time
    }
    c->readId = 0;   // this source ends here; pump makes another when it wants one
    pump(c);
    return G_SOURCE_REMOVE;
}

gboolean onAccept(gint fd, GIOCondition, gpointer data) {
    ControlServer::Impl* srv = static_cast<ControlServer::Impl*>(data);
    for (;;) {
        int cfd = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) break;
        struct ucred cred;
        socklen_t len = sizeof cred;
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
            close(cfd);   // another user's peer is never served
            continue;
        }
        auto c = std::make_shared<Conn>();
        c->srv = srv;
        c->fd = cfd;
        c->progress = g_get_monotonic_time();
        srv->conns.push_back(c);
        setReading(c, true);
    }
    return G_SOURCE_CONTINUE;
}

}  // namespace

ControlServer::ControlServer() {
    impl_ = new Impl;
    impl_->dispatcher = &dispatcher_;
}

ControlServer::~ControlServer() {
    stop();
    delete impl_;
}

int ControlServer::removeStale(const std::string& folder, int self) {
    DIR* d = opendir(folder.c_str());
    if (!d) return 0;
    int removed = 0;
    while (struct dirent* e = readdir(d)) {
        const int pid = ControlChannel::pidFromSocketName(e->d_name);
        if (!pid || pid == self) continue;
        const std::string path = folder + "/" + e->d_name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid())
            continue;
        bool stale = false;
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            stale = true;
        } else {
            // A live pid, which may be another process's since: stale when
            // nothing listens.
            struct sockaddr_un addr;
            std::memset(&addr, 0, sizeof addr);
            addr.sun_family = AF_UNIX;
            if (path.size() < sizeof addr.sun_path) {
                std::strcpy(addr.sun_path, path.c_str());
                int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                if (fd >= 0) {
                    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) != 0)
                        stale = errno == ECONNREFUSED || errno == ENOENT;
                    close(fd);
                }
            }
        }
        if (stale && unlink(path.c_str()) == 0) removed++;
    }
    closedir(d);
    return removed;
}

bool ControlServer::start(std::string* error) {
    std::string ignored;
    if (!error) error = &ignored;
    if (impl_->listenFd >= 0) return true;
    const std::string folder = ControlChannel::socketFolder(
        getenv("MINICODE_CONTROL_DIR"), getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getuid());
    if (!privateFolderOfOurs(folder, error)) return false;
    removeStale(folder, (int)getpid());
    const std::string path = folder + "/" + ControlChannel::socketName((int)getpid());
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        *error = "the socket's path is too long: " + path;
        return false;
    }
    std::strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    unlink(path.c_str());
    const mode_t old = umask(077);
    const int bound = bind(fd, (struct sockaddr*)&addr, sizeof addr);
    umask(old);
    if (bound != 0 || chmod(path.c_str(), 0600) != 0 || listen(fd, 16) != 0) {
        *error = "cannot listen on " + path + ": " + std::strerror(errno);
        close(fd);
        unlink(path.c_str());
        return false;
    }
    impl_->listenFd = fd;
    impl_->listenId = g_unix_fd_add(fd, G_IO_IN, onAccept, impl_);
    path_ = path;
    std::strcpy(gSocketPath, path.c_str());
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit(removeSocketAtExit);
    }
    return true;
}

void ControlServer::stop() {
    while (!impl_->conns.empty()) closeConn(impl_->conns.back());
    if (impl_->listenId) g_source_remove(impl_->listenId);
    impl_->listenId = 0;
    if (impl_->listenFd >= 0) close(impl_->listenFd);
    impl_->listenFd = -1;
    if (!path_.empty()) {
        unlink(path_.c_str());
        if (path_ == gSocketPath) gSocketPath[0] = 0;
    }
    path_.clear();
}
