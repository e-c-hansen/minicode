// ControlServer.mm — see ControlServer.h.
#import "ControlServer.h"
#import "ControlCommands.h"
#include "ControlProtocol.h"
#include <cerrno>
#include <fcntl.h>
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

// Writes the whole answer, giving up on a peer that stopped reading.
void writeAll(int fd, const std::string &s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(fd, s.data() + off, s.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        off += (size_t)n;
    }
}

// One connection: lines in, an answer for each, in order.
void serve(int fd) {
    struct Conn {
        int fd;
        std::string buffer;
        dispatch_source_t source;
        bool closed = false;
    };
    auto conn = std::make_shared<Conn>();
    conn->fd = fd;
    conn->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, gQueue);
    dispatch_source_set_event_handler(conn->source, ^{
        char buf[16384];
        ssize_t n = read(conn->fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            dispatch_source_cancel(conn->source);
            return;
        }
        conn->buffer.append(buf, (size_t)n);
        size_t nl;
        while ((nl = conn->buffer.find('\n')) != std::string::npos) {
            std::string line = conn->buffer.substr(0, nl);
            conn->buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            // Handled on the main thread; the answer is written from here,
            // so a slow reader never holds the main thread up.
            dispatch_async(dispatch_get_main_queue(), ^{
                gDispatcher->handle(line, [conn](const std::string &answer) {
                    std::string copy = answer;
                    dispatch_async(gQueue, ^{
                        if (!conn->closed) writeAll(conn->fd, copy);
                    });
                });
            });
        }
        if (conn->buffer.size() > kMaxLine) {
            writeAll(conn->fd, ControlChannel::errorLine(Json(), "the request is too long"));
            dispatch_source_cancel(conn->source);
        }
    });
    dispatch_source_set_cancel_handler(conn->source, ^{
        // Answers still on their way are dropped once the peer has gone.
        conn->closed = true;
        close(conn->fd);
    });
    dispatch_resume(conn->source);
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
        if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
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
    const char *t = getenv("TMPDIR");
    NSString *tmp = t && *t ? [NSString stringWithUTF8String:t] : NSTemporaryDirectory();
    return [tmp stringByAppendingPathComponent:@"MiniCode"];
}

NSUInteger MCControlRemoveStaleSockets(NSString *folder) {
    NSUInteger removed = 0;
    NSArray<NSString *> *names =
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil];
    for (NSString *name in names) {
        int pid = ControlChannel::pidFromSocketName(name.UTF8String ?: "");
        if (pid <= 0 || pid == getpid()) continue;
        if (kill(pid, 0) == 0 || errno != ESRCH) continue;   // alive (or not ours to judge)
        NSString *path = [folder stringByAppendingPathComponent:name];
        struct stat st;
        if (lstat(path.fileSystemRepresentation, &st) != 0 || !S_ISSOCK(st.st_mode)) continue;
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
