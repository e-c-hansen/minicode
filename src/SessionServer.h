// SessionServer.h — a headless session host on a Unix socket, and a
// renderer's connection to it (SESSION-DESIGN.md, stage 6). `mutible host
// --folder <dir>` runs a HostServer: a SessionHost with no window, which
// renderers attach to and detach from over the socket, speaking the
// session's protocol (SessionProtocol.h) one JSON line a message. Several
// renderers at once; detaching leaves documents, windows and terminals as
// they are, and a terminal nobody shows keeps running.
//
// One host per folder: the socket is named for the folder's resolved path
// (host-<16 hex>.sock) in the control channel's socket folder
// (ControlPaths.h: a 0700 folder of the user's own), and a lock file beside
// it is held for the host's life, so a second host for the folder finds the
// first and stops. A socket left by a host that died is removed by the next
// one (it holds the lock, so nothing listens there). Peers of another user
// are refused, both ways.
//
// Lifetime (the owner's answer, 2026-11-16): the host ends once it has had
// no renderer for `idleMs` (10 minutes) and nothing is unsaved or running
// (Host::busyReason); until then it lives on, however long that is.
//
// Pure C++17 and POSIX (Linux and macOS): the server's own thread polls the
// listening socket and the connections; the host's thread is SessionHost's.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "SessionClient.h"
#include "SessionHost.h"

namespace Shared {

// The folder's resolved path (realpath), "" when it is not a folder.
std::string resolveFolder(const std::string& dir);
// The host socket's file name for a resolved folder: host-<16 hex>.sock.
std::string hostSocketName(const std::string& folder);
// A folder the server made: a real folder (not a link) of this user's that
// nobody else can enter.
bool trustedSocketFolder(const std::string& dir);
// Makes `dir` (0700) when missing; false, with why, unless it is then
// trusted.
bool makeSocketFolder(const std::string& dir, std::string* error);
// The socket of the host serving `folder` (resolved) in the first of
// `dirs` holding one of this user's: "" when there is none.
std::string findHostSocket(const std::string& folder, const std::vector<std::string>& dirs);

struct HostOptions {
    std::string folder;                 // resolved
    std::string socketDir;              // where the socket goes
    long long idleMs = 10 * 60 * 1000;  // with no renderer and nothing to lose
    unsigned delayUs = 0;               // SessionHost's, for checks
};

class HostServer {
public:
    enum class Start { Ok, Running, Failed };
    HostServer();
    ~HostServer();   // the host ends: its shells hung up, the socket removed
    HostServer(const HostServer&) = delete;
    HostServer& operator=(const HostServer&) = delete;

    // Takes the folder's lock and listens: Running (with the other host's
    // socket in `error`) when a host serves the folder already.
    Start start(const HostOptions& o, std::string* error);
    // Serves renderers until the lifetime ends (true) or stop() (false).
    bool run();
    // Any thread: run() returns at its next turn.
    void stop();
    const std::string& socketPath() const { return socketPath_; }
    SessionHost& host() { return *host_; }
    // Renderers connected now (any thread).
    std::size_t renderers();

private:
    struct Conn {
        int fd = -1;
        ClientId id = 0;
        std::string in;            // the server's thread only
        std::size_t scanned = 0;   // of `in`, no newline before
        std::string out;           // under mu_
        bool dead = false;         // under mu_
    };
    HostOptions opt_;
    std::string socketPath_, lockPath_;
    int listen_ = -1, lock_ = -1;
    int wake_[2] = {-1, -1};
    std::unique_ptr<SessionHost> host_;
    std::mutex mu_;
    std::map<ClientId, std::shared_ptr<Conn>> conns_;
    std::atomic<bool> stop_{false};
    void wake();
    void accept();
    void readFrom(Conn& c);
    void writeTo(Conn& c);   // under mu_
};

// A renderer's connection to a host's socket: a Client's Send writes here,
// and a reader thread of its own delivers the host's messages.
class HostLink {
public:
    // Connects to `path`: null, with why, when it cannot; `refused` when
    // nothing listens there (a socket a dead host left).
    static std::unique_ptr<HostLink> open(const std::string& path, std::string* error,
                                          bool* refused = nullptr);
    ~HostLink();   // close()
    HostLink(const HostLink&) = delete;
    HostLink& operator=(const HostLink&) = delete;

    // Starts reading: each message to `deliver` (on the reader thread), and
    // `closed` once when the host goes or sends what is not a message.
    void start(std::function<void(Message&&)> deliver, std::function<void()> closed = {});
    // `client` speaks through this link: its sends written here, the host's
    // messages delivered to it, then its Hello; when the link closes, the
    // client hears the host is gone (Client::hostLost), so it waits no more.
    void connect(Client& client);
    // One message, written whole (any thread). False once the host is gone.
    bool send(const Message& m);
    // Detaches: the host forgets this renderer, and keeps everything else.
    void close();
    bool closed() const { return closed_.load(); }

private:
    explicit HostLink(int fd) : fd_(fd) {}
    int fd_ = -1;
    std::mutex writeMu_;
    std::thread reader_;
    std::atomic<bool> closed_{false};
};

// What `mutible attach --dump` prints: the session `client` is connected
// to, as text. The folder, then each document (path, version, unsaved or
// not) with its text, then each terminal (size, shell, what runs) with its
// screen as a renderer attaching would draw it from the host's replay. The
// terminals are attached to for that, so the client is left showing them.
// "" when the host does not answer.
std::string dumpSession(Client& client);

}  // namespace Shared
