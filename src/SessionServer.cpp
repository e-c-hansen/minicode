// SessionServer.cpp — see SessionServer.h.
#include "SessionServer.h"

#include "TerminalScreen.h"
#include "Text.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
    for (auto& [id, c] : conns_)
        if (c->fd >= 0) ::close(c->fd);
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
    return Start::Ok;
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
        const ssize_t n = sendSome(c.fd, c.out.data(), c.out.size());
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
    for (;;) {
        if (stop_) return false;
        polled.clear();
        polls.assign({pollfd{wake_[0], POLLIN, 0}, pollfd{listen_, POLLIN, 0}});
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [id, c] : conns_) {
                if (c->dead) continue;
                polled.push_back(c);
                polls.push_back(pollfd{c->fd, (short)(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
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
                const short r = polls[i + 2].revents;
                if (r & (POLLIN | POLLHUP | POLLERR)) readFrom(*polled[i]);
                if (r & POLLOUT) {
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
            host_->disconnect(c->id);
        }
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
        if (closed) closed();
    });
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

}  // namespace Shared
