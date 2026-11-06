// ControlServer.mm — see ControlServer.h.
#import "ControlServer.h"
#import "ControlCommands.h"
#include "ControlProtocol.h"
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// A request line longer than this closes the connection: nothing the
// protocol has needs a megabyte.
const size_t kMaxLine = 1 << 20;

ControlChannel::Dispatcher *gDispatcher;
int gListenFD = -1;
dispatch_source_t gListenSource;
dispatch_queue_t gQueue;          // accepting, reading and writing
char gSocketPath[sizeof(((struct sockaddr_un *)nullptr)->sun_path)];
uid_t gPeerUID;
BOOL gPeerUIDSet;

void removeSocketAtExit() {
    if (gSocketPath[0]) unlink(gSocketPath);
}

// One connection. Requests are taken one at a time: the next line is read
// only once the answer to the last has been written, so a client cannot
// queue unbounded work on the main thread by sending many lines at once,
// and the kernel's buffer holds back a client that sends faster than
// minicode answers. Writes never block the queue every connection shares:
// the socket is non-blocking, an answer the peer is slow to take waits for
// a write source, and a peer that takes nothing for kStallSeconds is
// dropped.
const double kStallSeconds = 10;

struct Conn {
    int fd = -1;
    std::string in;              // read, not yet handled
    std::string out;             // the answer being written
    size_t outOff = 0;
    dispatch_source_t reader = nullptr, writer = nullptr;
    bool readerSuspended = false, writerSuspended = true;
    bool busy = false;           // a request is with the main thread
    bool eof = false;            // the peer sent all it will send
    bool closed = false;
    int sources = 0;             // cancel handlers still to run
    uint64_t progress = 0;       // bumps on every byte written
};

void closeConn(const std::shared_ptr<Conn> &c);
void pump(const std::shared_ptr<Conn> &c);

void setReading(const std::shared_ptr<Conn> &c, bool on) {
    if (c->closed || on == !c->readerSuspended) return;
    c->readerSuspended = !on;
    if (on) dispatch_resume(c->reader);
    else dispatch_suspend(c->reader);
}

void setWaitingToWrite(const std::shared_ptr<Conn> &c, bool on) {
    if (c->closed || on == !c->writerSuspended) return;
    c->writerSuspended = !on;
    if (on) dispatch_resume(c->writer);
    else dispatch_suspend(c->writer);
}

// While an answer waits for the peer: a peer that takes none of it for
// kStallSeconds is let go of; one that takes some is watched again.
void watchForStall(const std::shared_ptr<Conn> &c, uint64_t seen) {
    std::weak_ptr<Conn> weak = c;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kStallSeconds * NSEC_PER_SEC)),
                   gQueue, ^{
        auto s = weak.lock();
        if (!s || s->closed || s->writerSuspended) return;
        if (s->progress == seen) closeConn(s);
        else watchForStall(s, s->progress);
    });
}

// Writes what it can of the answer; true when all of it is out.
bool flush(const std::shared_ptr<Conn> &c) {
    while (c->outOff < c->out.size()) {
        ssize_t n = write(c->fd, c->out.data() + c->outOff, c->out.size() - c->outOff);
        if (n > 0) {
            c->outOff += (size_t)n;
            c->progress++;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            if (c->writerSuspended) {
                setWaitingToWrite(c, true);
                watchForStall(c, c->progress);
            }
            return false;
        }
        closeConn(c);   // the peer has gone
        return false;
    }
    c->out.clear();
    c->outOff = 0;
    setWaitingToWrite(c, false);
    return true;
}

// Hands the next request line to the main thread, when none is there.
void pump(const std::shared_ptr<Conn> &c) {
    while (!c->closed && !c->busy && c->out.empty()) {
        size_t nl = c->in.find('\n');
        if (nl == std::string::npos) {
            if (c->in.size() > kMaxLine) {
                c->out = ControlChannel::errorLine(Json(), "the request is too long");
                c->eof = true;
                c->in.clear();
                if (!flush(c)) return;
                closeConn(c);
                return;
            }
            if (c->eof) { closeConn(c); return; }
            setReading(c, true);
            return;
        }
        std::string line = c->in.substr(0, nl);
        c->in.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        c->busy = true;
        setReading(c, false);
        std::weak_ptr<Conn> weak = c;
        dispatch_async(dispatch_get_main_queue(), ^{
            gDispatcher->handle(line, [weak](const std::string &answer) {
                std::string copy = answer;
                dispatch_async(gQueue, ^{
                    auto s = weak.lock();
                    if (!s || s->closed) return;
                    s->busy = false;
                    s->out = copy;
                    s->outOff = 0;
                    if (flush(s)) pump(s);
                });
            });
        });
        return;
    }
}

void closeConn(const std::shared_ptr<Conn> &c) {
    if (c->closed) return;
    // Resumed first: a suspended source never runs its cancel handler.
    setReading(c, true);
    setWaitingToWrite(c, true);
    c->closed = true;
    dispatch_source_cancel(c->reader);
    dispatch_source_cancel(c->writer);
}

void serve(int fd) {
    auto conn = std::make_shared<Conn>();
    conn->fd = fd;
    conn->sources = 2;
    Conn *raw = conn.get();
    auto cancelled = ^{
        // The descriptor goes once neither source can touch it.
        if (--raw->sources == 0) close(raw->fd);
    };
    conn->reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, gQueue);
    conn->writer = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, (uintptr_t)fd, 0, gQueue);
    std::weak_ptr<Conn> weak = conn;
    dispatch_source_set_event_handler(conn->reader, ^{
        auto c = weak.lock();
        if (!c || c->closed) return;
        char buf[16384];
        ssize_t n = read(c->fd, buf, sizeof buf);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (n <= 0) {
            // The peer is done sending (or gone): what it sent is still
            // answered, then the connection closes.
            c->eof = true;
            setReading(c, false);
        } else {
            c->in.append(buf, (size_t)n);
        }
        pump(c);
    });
    dispatch_source_set_event_handler(conn->writer, ^{
        auto c = weak.lock();
        if (!c || c->closed) return;
        if (flush(c)) pump(c);
    });
    // Each handler holds the connection until it has run, so the last one
    // to run frees it.
    dispatch_source_set_cancel_handler(conn->reader, ^{ (void)conn; cancelled(); });
    dispatch_source_set_cancel_handler(conn->writer, ^{ (void)conn; cancelled(); });
    conn->readerSuspended = false;
    dispatch_resume(conn->reader);   // the writer starts suspended, until a write would block
}

void acceptConnections() {
    for (;;) {
        int fd = accept(gListenFD, nullptr, nullptr);
        if (fd < 0) return;   // EAGAIN: none left
        uid_t uid = (uid_t)-1;
        gid_t gid;
        const uid_t want = gPeerUIDSet ? gPeerUID : geteuid();
        if (getpeereid(fd, &uid, &gid) != 0 || uid != want) {
            close(fd);   // another user: no answer at all
            continue;
        }
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(fd);
            continue;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        serve(fd);
    }
}

// The folder exists, is a real folder (not a link) owned by this user, and
// nobody else can enter it.
BOOL prepareFolder(NSString *folder) {
    const char *p = folder.fileSystemRepresentation;
    if (mkdir(p, 0700) != 0 && errno != EEXIST) return NO;
    struct stat st;
    if (lstat(p, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return NO;
    if ((st.st_mode & 0777) != 0700 && chmod(p, 0700) != 0) return NO;
    return YES;
}

}  // namespace

// $TMPDIR, as the command line reads it (a process Launch Services starts
// has it set to the user's own temporary folder), else the same folder from
// the system; NSTemporaryDirectory() does not read TMPDIR.
NSString *MCControlSocketFolder(void) {
    const char *forced = getenv("MINICODE_CONTROL_DIR");
    if (forced && *forced) return [NSString stringWithUTF8String:forced];
    const char *t = getenv("TMPDIR");
    NSString *tmp = t && *t ? [NSString stringWithUTF8String:t] : nil;
    if (!tmp) {
        char buf[PATH_MAX];
        size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
        tmp = n > 0 && n <= sizeof buf ? [NSString stringWithUTF8String:buf] : NSTemporaryDirectory();
    }
    return [tmp stringByAppendingPathComponent:@"MiniCode"];
}

// Whether something listens on the socket at `path` (a live pid may be a
// later process that reused a dead minicode's number).
static BOOL MCSocketAnswers(NSString *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    const char *fs = path.fileSystemRepresentation;
    if (!fs || strlen(fs) >= sizeof addr.sun_path) return YES;   // can't tell: leave it
    strlcpy(addr.sun_path, fs, sizeof addr.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return YES;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    const BOOL refused = connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0 &&
                         errno == ECONNREFUSED;
    close(fd);
    return !refused;
}

NSUInteger MCControlRemoveStaleSockets(NSString *folder) {
    NSUInteger removed = 0;
    NSArray<NSString *> *names =
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil];
    for (NSString *name in names) {
        int pid = ControlChannel::pidFromSocketName(name.UTF8String ?: "");
        if (pid <= 0 || pid == getpid()) continue;
        NSString *path = [folder stringByAppendingPathComponent:name];
        if (kill(pid, 0) == 0) {
            // Alive, but perhaps a later process with a dead minicode's pid:
            // then nothing listens.
            if (MCSocketAnswers(path)) continue;
        } else if (errno != ESRCH) {
            continue;   // not ours to judge
        }
        struct stat st;
        if (lstat(path.fileSystemRepresentation, &st) != 0 || !S_ISSOCK(st.st_mode) ||
            st.st_uid != geteuid())
            continue;
        if (unlink(path.fileSystemRepresentation) == 0) removed++;
    }
    return removed;
}

void MCControlServerStart(id<MCControlAppHost> host) {
    if (gListenFD >= 0) return;
    if (!gDispatcher) {
        gDispatcher = new ControlChannel::Dispatcher();
        MCControlRegisterCommands(*gDispatcher, host);
    }
    const char *off = getenv("MINICODE_CONTROL");
    if (off && strcmp(off, "off") == 0) return;
    NSString *folder = MCControlSocketFolder();
    if (!prepareFolder(folder)) {
        NSLog(@"minicode: control channel off: cannot use %@", folder);
        return;
    }
    MCControlRemoveStaleSockets(folder);
    NSString *path = [folder stringByAppendingPathComponent:
        [NSString stringWithUTF8String:ControlChannel::socketName(getpid()).c_str()]];
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    const char *fs = path.fileSystemRepresentation;
    if (strlen(fs) >= sizeof addr.sun_path) {
        NSLog(@"minicode: control channel off: the socket path is too long: %@", path);
        return;
    }
    strlcpy(addr.sun_path, fs, sizeof addr.sun_path);
    unlink(fs);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || chmod(fs, 0600) != 0 ||
        listen(fd, 16) != 0) {
        NSLog(@"minicode: control channel off: %s", strerror(errno));
        close(fd);
        unlink(fs);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    gListenFD = fd;
    strlcpy(gSocketPath, fs, sizeof gSocketPath);
    static dispatch_once_t once;
    dispatch_once(&once, ^{ atexit(removeSocketAtExit); });
    gQueue = dispatch_queue_create("minicode.control", DISPATCH_QUEUE_SERIAL);
    gListenSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, gQueue);
    dispatch_source_set_event_handler(gListenSource, ^{ acceptConnections(); });
    dispatch_source_set_cancel_handler(gListenSource, ^{ close(fd); });
    dispatch_resume(gListenSource);
}

void MCControlServerStop(void) {
    if (gListenFD < 0) return;
    dispatch_source_cancel(gListenSource);
    gListenSource = nil;
    gListenFD = -1;
    if (gSocketPath[0]) unlink(gSocketPath);
    gSocketPath[0] = 0;
}

NSString *MCControlSocketPath(void) {
    return gSocketPath[0] ? [NSString stringWithUTF8String:gSocketPath] : nil;
}

void MCControlHandleLine(NSString *line, void (^done)(NSString *answer)) {
    if (!gDispatcher) {
        done(@"{\"id\":null,\"ok\":false,\"error\":\"the control channel is off\"}\n");
        return;
    }
    gDispatcher->handle(line.UTF8String ?: "", [done](const std::string &answer) {
        NSString *s = [NSString stringWithUTF8String:answer.c_str()] ?: @"";
        if ([NSThread isMainThread]) done(s);
        else dispatch_async(dispatch_get_main_queue(), ^{ done(s); });
    });
}

void MCControlServerSetPeerUIDForTesting(uid_t uid) {
    gPeerUID = uid;
    gPeerUIDSet = YES;
}
