// IdeLinkServer.mm — see IdeLinkServer.h; the protocol is IdeLink.h.
#import "IdeLinkServer.h"
#import "EditorController.h"
#import "AgentNotes.h"
#import "AppSettings.h"
#import "CompareView.h"
#import "Lsp.h"
#include "IdeLink.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using IdeLink::ToolResult;
using Respond = std::function<void(ToolResult)>;

static NSString *NSFromStd(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

static std::string JsonText(const Json &j) { return j.dump(); }

static std::string Utf8String(NSString *s) {
    const char *c = s.UTF8String;
    return c ? c : "";
}

static std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

// --------------------------------------------------------------- folders
NSString *MCIdeLinkFolder(void) {
    const char *dir = getenv("CLAUDE_CONFIG_DIR");
    NSString *config = dir && *dir ? [NSString stringWithUTF8String:dir]
                                   : [NSHomeDirectory() stringByAppendingPathComponent:@".claude"];
    return [config stringByAppendingPathComponent:@"ide"];
}

NSUInteger MCIdeLinkRemoveStaleLocks(NSString *folder) {
    NSUInteger removed = 0;
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:folder error:nil]) {
        if (IdeLink::portFromLockName(name.UTF8String ?: "") == 0) continue;
        NSString *path = [folder stringByAppendingPathComponent:name];
        struct stat st;
        if (lstat(path.fileSystemRepresentation, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_uid != geteuid() || st.st_size > 65536)
            continue;
        NSData *data = [NSData dataWithContentsOfFile:path];
        IdeLink::LockInfo info;
        if (!data || !IdeLink::parseLockFile(std::string((const char *)data.bytes, data.length), info))
            continue;
        const bool alive = info.pid > 0 && (kill(info.pid, 0) == 0 || errno != ESRCH);
        if (!IdeLink::isStaleOwnLock(info, getpid(), alive)) continue;
        if (unlink(path.fileSystemRepresentation) == 0) removed++;
    }
    return removed;
}

// Lock files of this process, removed however it ends.
static std::vector<std::string> &MCLockPaths() {
    static std::vector<std::string> paths;
    return paths;
}
static void MCRemoveLocksAtExit() {
    for (const std::string &p : MCLockPaths()) unlink(p.c_str());
}

static NSString *gProblem;
NSString *MCIdeLinkProblem(void) { return gProblem; }

// The comparisons a link offered for a decision: close_tab and
// closeAllDiffTabs close only these.
static NSHashTable<MCComparison *> *MCOfferedDiffs() {
    static NSHashTable *t;
    if (!t) t = [NSHashTable weakObjectsHashTable];
    return t;
}

// --------------------------------------------------------- a connection
struct IdeConn {
    int fd = -1;
    dispatch_source_t reader = nil, writer = nil;
    bool writerSuspended = true;
    bool closed = false, closing = false, upgraded = false;
    int sources = 2;
    std::string in, out;
    size_t outOff = 0;
    uint64_t progress = 0;
    IdeLink::FrameReader frames;
    std::unique_ptr<IdeLink::McpServer> mcp;
    std::string clientPid;
};

static Json MCDiagnostics(EditorController *c, const std::string &uri);
static NSUInteger MCCloseOfferedDiffs(EditorController *c, NSString *name);

@interface MCIdeLink ()
@property(nonatomic, weak) EditorController *controller;
- (instancetype)initWithController:(EditorController *)controller;
- (BOOL)start;
- (void)stop;
- (void)selectionMoved;
- (void)refreshLockIfFolderChanged;
@end

@implementation MCIdeLink {
    int _listenFD;
    dispatch_source_t _listenSource;
    std::vector<std::shared_ptr<IdeConn>> _conns;
    NSString *_writtenFolders;
    std::string _lastSelection;              // the last selection_changed sent
    Json _latestSelection;                   // getLatestSelection's
    NSTimer *_selectionTimer;
    // openDiff's comparisons still waiting, with the connection that asked.
    std::vector<std::pair<__weak MCComparison *, std::weak_ptr<IdeConn>>> _pendingConns;
}

- (instancetype)initWithController:(EditorController *)controller {
    if ((self = [super init])) {
        _controller = controller;
        _listenFD = -1;
        _latestSelection = Json();
    }
    return self;
}

- (NSUInteger)connectionCount {
    NSUInteger n = 0;
    for (auto &c : _conns) if (c->upgraded && !c->closed) n++;
    return n;
}

- (NSUInteger)clientCount {
    NSUInteger n = 0;
    for (auto &c : _conns) if (c->mcp && c->mcp->initialized() && !c->closed) n++;
    return n;
}

// ------------------------------------------------------------ listening
- (BOOL)start {
    if (_listenFD >= 0) return YES;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return [self fail:[NSString stringWithFormat:@"no socket: %s", strerror(errno)]];
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   // the system picks
    socklen_t len = sizeof addr;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 8) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return [self fail:[NSString stringWithFormat:@"cannot listen on 127.0.0.1: %s", strerror(errno)]];
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    _listenFD = fd;
    _port = ntohs(addr.sin_port);
    unsigned char raw[24];
    arc4random_buf(raw, sizeof raw);
    _authToken = NSFromStd(IdeLink::toHex(std::string((const char *)raw, sizeof raw)));
    if (![self writeLock]) {
        [self stop];
        return NO;
    }
    _listenSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0,
                                           dispatch_get_main_queue());
    __weak MCIdeLink *weakSelf = self;
    dispatch_source_set_event_handler(_listenSource, ^{ [weakSelf acceptConnections]; });
    dispatch_source_set_cancel_handler(_listenSource, ^{ close(fd); });
    dispatch_resume(_listenSource);
    gProblem = nil;
    return YES;
}

- (BOOL)fail:(NSString *)problem {
    gProblem = problem;
    NSLog(@"minicode: Claude Code IDE link off: %@", problem);
    return NO;
}

- (void)stop {
    for (auto c : std::vector<std::shared_ptr<IdeConn>>(_conns)) [self closeConn:c];
    _conns.clear();
    if (_listenSource) {
        dispatch_source_cancel(_listenSource);
        _listenSource = nil;
    } else if (_listenFD >= 0) {
        close(_listenFD);
    }
    _listenFD = -1;
    [self removeLock];
    [_selectionTimer invalidate];
    _selectionTimer = nil;
}

- (void)dealloc {
    [self stop];
}

// The folders Claude Code matches its working folder against: the window's
// as the tree spells it, and resolved (/tmp is /private/tmp), since Claude
// Code compares without resolving links.
- (NSArray<NSString *> *)workspaceFolders {
    NSString *root = self.controller.rootPath;
    if (!root.length) return @[];
    NSString *real = root.stringByResolvingSymlinksInPath;
    char buf[PATH_MAX];
    NSString *rp = realpath(root.fileSystemRepresentation, buf) ? [NSString stringWithUTF8String:buf] : nil;
    NSMutableOrderedSet *set = [NSMutableOrderedSet orderedSetWithObject:root];
    if (real.length) [set addObject:real];
    if (rp.length) [set addObject:rp];
    return set.array;
}

- (BOOL)writeLock {
    NSString *folder = MCIdeLinkFolder();
    NSString *config = folder.stringByDeletingLastPathComponent;
    BOOL isDir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:config isDirectory:&isDir] || !isDir)
        return [self fail:[NSString stringWithFormat:
            @"%@ is not there (is Claude Code installed?)", config.stringByAbbreviatingWithTildeInPath]];
    const char *fs = folder.fileSystemRepresentation;
    if (mkdir(fs, 0700) != 0 && errno != EEXIST)
        return [self fail:[NSString stringWithFormat:@"cannot make %@: %s", folder, strerror(errno)]];
    struct stat st;
    if (lstat(fs, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        return [self fail:[NSString stringWithFormat:@"%@ is not a folder of yours", folder]];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        MCIdeLinkRemoveStaleLocks(folder);
        atexit(MCRemoveLocksAtExit);
    });
    IdeLink::LockInfo info;
    info.pid = getpid();
    NSArray<NSString *> *folders = [self workspaceFolders];
    for (NSString *f in folders) info.workspaceFolders.push_back(f.UTF8String ?: "");
    info.ideName = IdeLink::kIdeName;
    info.transport = "ws";
    info.authToken = _authToken.UTF8String;
    const std::string text = IdeLink::lockFileText(info);
    NSString *path = [folder stringByAppendingPathComponent:NSFromStd(IdeLink::lockFileName(_port))];
    NSString *tmp = [folder stringByAppendingPathComponent:
        [NSString stringWithFormat:@".%d.lock.%d.tmp", _port, (int)getpid()]];
    unlink(tmp.fileSystemRepresentation);
    int fd = open(tmp.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return [self fail:[NSString stringWithFormat:@"cannot write %@: %s", tmp, strerror(errno)]];
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    fchmod(fd, 0600);
    close(fd);
    if (done != text.size() || rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation) != 0) {
        unlink(tmp.fileSystemRepresentation);
        return [self fail:[NSString stringWithFormat:@"cannot write %@", path]];
    }
    if (!_lockPath) MCLockPaths().push_back(path.fileSystemRepresentation);
    _lockPath = path;
    _writtenFolders = [folders componentsJoinedByString:@"\n"];
    return YES;
}

- (void)removeLock {
    if (!_lockPath) return;
    const std::string p = _lockPath.fileSystemRepresentation;
    unlink(p.c_str());
    auto &paths = MCLockPaths();
    paths.erase(std::remove(paths.begin(), paths.end(), p), paths.end());
    _lockPath = nil;
}

- (void)refreshLockIfFolderChanged {
    if (_listenFD < 0 || !_lockPath) return;
    if (![[[self workspaceFolders] componentsJoinedByString:@"\n"] isEqualToString:_writtenFolders ?: @""])
        [self writeLock];
}

// ---------------------------------------------------------- connections
static const NSUInteger kMaxConnections = 8;

- (void)acceptConnections {
    for (;;) {
        int fd = accept(_listenFD, nullptr, nullptr);
        if (fd < 0) return;
        NSUInteger live = 0;
        for (auto &c : _conns) if (!c->closed) live++;
        if (live >= kMaxConnections) { close(fd); continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        [self serve:fd];
    }
}

- (void)serve:(int)fd {
    auto conn = std::make_shared<IdeConn>();
    conn->fd = fd;
    _conns.push_back(conn);
    IdeConn *raw = conn.get();
    auto cancelled = ^{ if (--raw->sources == 0) close(raw->fd); };
    conn->reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
    conn->writer = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, (uintptr_t)fd, 0, dispatch_get_main_queue());
    std::weak_ptr<IdeConn> weak = conn;
    __weak MCIdeLink *weakSelf = self;
    dispatch_source_set_event_handler(conn->reader, ^{
        auto c = weak.lock();
        if (c && !c->closed) [weakSelf readFrom:c];
    });
    dispatch_source_set_event_handler(conn->writer, ^{
        auto c = weak.lock();
        if (c && !c->closed) [weakSelf flush:c];
    });
    dispatch_source_set_cancel_handler(conn->reader, ^{ (void)conn; cancelled(); });
    dispatch_source_set_cancel_handler(conn->writer, ^{ (void)conn; cancelled(); });
    dispatch_resume(conn->reader);
    // A peer that has not finished its handshake in 10 s is let go.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        auto c = weak.lock();
        if (c && !c->closed && !c->upgraded) [weakSelf closeConn:c];
    });
}

- (void)closeConn:(const std::shared_ptr<IdeConn> &)c {
    if (c->closed) return;
    c->closed = true;
    if (c->writerSuspended) {   // a suspended source never runs its cancel handler
        c->writerSuspended = false;
        dispatch_resume(c->writer);
    }
    dispatch_source_cancel(c->reader);
    dispatch_source_cancel(c->writer);
    const bool wasClient = c->mcp && c->mcp->initialized();
    c->mcp.reset();   // an answer still to come goes nowhere
    _conns.erase(std::remove(_conns.begin(), _conns.end(), c), _conns.end());
    // A diff it asked about waits for nobody now.
    for (auto &p : _pendingConns) {
        auto owner = p.second.lock();
        if ((!owner || owner == c) && p.first)
            [p.first endDecision:@"Claude Code is no longer waiting for an answer."];
    }
    [self prunePending];
    if (wasClient) [self note:@"Claude Code left the IDE link"];
}

- (void)prunePending {
    _pendingConns.erase(std::remove_if(_pendingConns.begin(), _pendingConns.end(), [](auto &p) {
                            return !p.first || p.first.decision != 0 || p.second.expired();
                        }),
                        _pendingConns.end());
}

- (void)note:(NSString *)text {
    [self.controller showAgentStatusNote:text];
}

- (void)send:(const std::shared_ptr<IdeConn> &)c bytes:(const std::string &)bytes {
    if (c->closed) return;
    c->out += bytes;
    [self flush:c];
}

// Writes what the peer takes now; the rest waits for the write source. A
// peer that takes nothing for 30 s is let go.
- (void)flush:(const std::shared_ptr<IdeConn> &)c {
    while (c->outOff < c->out.size()) {
        ssize_t n = write(c->fd, c->out.data() + c->outOff, c->out.size() - c->outOff);
        if (n > 0) { c->outOff += (size_t)n; c->progress++; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            if (c->writerSuspended) {
                c->writerSuspended = false;
                dispatch_resume(c->writer);
                [self watchStall:c seen:c->progress];
            }
            return;
        }
        [self closeConn:c];
        return;
    }
    c->out.clear();
    c->outOff = 0;
    if (!c->writerSuspended) {
        c->writerSuspended = true;
        dispatch_suspend(c->writer);
    }
    if (c->closing) [self closeConn:c];
}

- (void)watchStall:(const std::shared_ptr<IdeConn> &)c seen:(uint64_t)seen {
    std::weak_ptr<IdeConn> weak = c;
    __weak MCIdeLink *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        auto s = weak.lock();
        if (!s || s->closed || s->writerSuspended) return;
        if (s->progress == seen) [weakSelf closeConn:s];
        else [weakSelf watchStall:s seen:s->progress];
    });
}

- (void)readFrom:(const std::shared_ptr<IdeConn> &)c {
    char buf[65536];
    ssize_t n = read(c->fd, buf, sizeof buf);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n <= 0) { [self closeConn:c]; return; }
    if (c->closing) return;   // said goodbye: the rest is not read
    if (!c->upgraded) {
        c->in.append(buf, (size_t)n);
        IdeLink::Handshake h = IdeLink::readHandshake(c->in, _authToken.UTF8String ?: "");
        if (h.state == IdeLink::Handshake::Incomplete) return;
        if (h.state == IdeLink::Handshake::Refused) {
            NSLog(@"minicode: IDE link refused a connection: %s", h.error.c_str());
            c->closing = true;
            [self send:c bytes:h.response];
            return;
        }
        c->upgraded = true;
        std::string rest = c->in.substr(h.consumed);
        c->in.clear();
        [self send:c bytes:h.response];
        [self startMcpOn:c];
        if (!rest.empty()) c->frames.feed(rest);
    } else {
        c->frames.feed(buf, (size_t)n);
    }
    [self readFrames:c];
}

- (void)readFrames:(const std::shared_ptr<IdeConn> &)c {
    IdeLink::Message m;
    for (;;) {
        if (c->closed || c->closing) return;
        IdeLink::FrameReader::Result r = c->frames.next(m);
        if (r == IdeLink::FrameReader::NeedMore) return;
        if (r == IdeLink::FrameReader::Error) {
            c->closing = true;
            [self send:c bytes:IdeLink::encodeClose(c->frames.errorCode(), c->frames.error())];
            return;
        }
        switch (m.op) {
            case IdeLink::Opcode::Text:
                if (c->mcp) c->mcp->handle(m.payload);
                break;
            case IdeLink::Opcode::Ping:
                [self send:c bytes:IdeLink::encodeFrame(IdeLink::Opcode::Pong, m.payload)];
                break;
            case IdeLink::Opcode::Pong:
                break;
            case IdeLink::Opcode::Close:
                c->closing = true;
                [self send:c bytes:IdeLink::encodeClose(m.closeCode == 1005 ? 1000 : m.closeCode)];
                return;
            default:   // binary: MCP is text
                c->closing = true;
                [self send:c bytes:IdeLink::encodeClose(1003, "text frames only")];
                return;
        }
    }
}

- (void)broadcast:(const std::string &)message {
    const std::string frame = IdeLink::encodeFrame(IdeLink::Opcode::Text, message);
    for (auto c : std::vector<std::shared_ptr<IdeConn>>(_conns))
        if (c->mcp && c->mcp->initialized() && !c->closing) [self send:c bytes:frame];
}

// ----------------------------------------------------------------- MCP
- (void)startMcpOn:(const std::shared_ptr<IdeConn> &)c {
    NSString *version = [NSBundle mainBundle].infoDictionary[@"CFBundleShortVersionString"] ?: @"0";
    c->mcp.reset(new IdeLink::McpServer(IdeLink::kIdeName, version.UTF8String));
    std::weak_ptr<IdeConn> weak = c;
    __weak MCIdeLink *weakSelf = self;
    c->mcp->send = [weak, weakSelf](const std::string &text) {
        auto s = weak.lock();
        MCIdeLink *link = weakSelf;
        if (s && link) [link send:s bytes:IdeLink::encodeFrame(IdeLink::Opcode::Text, text)];
    };
    c->mcp->onNotification = [weak, weakSelf](const std::string &method, const Json &params) {
        MCIdeLink *link = weakSelf;
        auto s = weak.lock();
        if (!link || !s) return;
        if (method == "ide_connected") {
            s->clientPid = std::to_string(params["pid"].asInt(0));
            [link note:@"Claude Code joined through the IDE link"];
        } else if (method == "notifications/initialized") {
            // What the editor shows now, as VS Code sends it on connecting.
            [link sendSelectionTo:s];
        }
    };
    [self addToolsTo:*c->mcp conn:c];
}

- (void)sendSelectionTo:(const std::shared_ptr<IdeConn> &)c {
    Json sel = [self currentSelectionJson];
    if (sel.isNull() || !c->mcp || !c->mcp->initialized()) return;
    [self send:c bytes:IdeLink::encodeFrame(IdeLink::Opcode::Text,
                                            IdeLink::McpServer::notification("selection_changed", sel))];
}

// The editor's selection in VS Code's shape, or null when no text file is
// on show. A preview (Markdown, LaTeX) gives the file with an empty
// selection at its top, since its characters are not the source's.
- (Json)currentSelectionJson {
    EditorController *c = self.controller;
    NSDictionary *sel = [c controlSelection];
    if (!sel) return Json();
    IdeLink::Selection s;
    s.filePath = Utf8String(sel[@"path"]);
    if (![sel[@"preview"] boolValue]) {
        s.text = Utf8String(sel[@"text"]);
        s.startLine = [sel[@"start"][@"line"] intValue] - 1;
        s.startCharacter = [sel[@"start"][@"column"] intValue] - 1;
        s.endLine = [sel[@"end"][@"line"] intValue] - 1;
        s.endCharacter = [sel[@"end"][@"column"] intValue] - 1;
    }
    return IdeLink::selectionJson(s);
}

- (void)selectionMoved {
    [_selectionTimer invalidate];
    __weak MCIdeLink *weakSelf = self;
    _selectionTimer = [NSTimer scheduledTimerWithTimeInterval:0.3 repeats:NO block:^(NSTimer *t) {
        (void)t;
        [weakSelf sendSelectionIfChanged];
    }];
}

- (void)sendSelectionIfChanged {
    _selectionTimer = nil;
    [self refreshLockIfFolderChanged];
    Json sel = [self currentSelectionJson];
    if (sel.isNull()) return;
    _latestSelection = sel;
    const std::string text = sel.dump();
    if (text == _lastSelection) return;
    _lastSelection = text;
    [self broadcast:IdeLink::McpServer::notification("selection_changed", sel)];
}

// --------------------------------------------------------------- tools
static Json Obj(std::initializer_list<std::pair<std::string, Json>> kv) { return Json::object(kv); }
static ToolResult JsonResult(const Json &j) { return ToolResult::text(JsonText(j)); }

// `path` spelled from the window's folder as the tree spells it, when it is
// inside it (the tree finds it that way); else as it is.
static NSString *MCSpelledIn(EditorController *c, NSString *path) {
    NSString *root = c.rootPath.stringByResolvingSymlinksInPath;
    NSString *real = path.stringByResolvingSymlinksInPath;
    NSString *prefix = [root isEqualToString:@"/"] ? root : [root stringByAppendingString:@"/"];
    if (root.length && [real hasPrefix:prefix])
        return [c.rootPath stringByAppendingPathComponent:[real substringFromIndex:prefix.length]];
    return path;
}

static BOOL MCRegularFile(NSString *path) {
    struct stat st;
    return path.length && stat(path.fileSystemRepresentation, &st) == 0 && S_ISREG(st.st_mode);
}

// A file's text: the buffer of the tab on show, else the disk's (UTF-8, at
// most 16 MB); nil when there is none to read.
static NSString *MCFileText(EditorController *c, NSString *path) {
    NSString *buffer = [c agentBufferOfShownFile:path];
    if (buffer) return buffer;
    struct stat st;
    if (stat(path.fileSystemRepresentation, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size > 16 * 1024 * 1024)
        return nil;
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

// 1-based line and column (in characters as drawn, as notes count them) of
// a UTF-16 offset.
static void MCNoteLineColumn(NSString *s, NSUInteger offset, int &line, int &column) {
    offset = MIN(offset, s.length);
    line = 1;
    NSUInteger start = 0;
    for (NSUInteger i = 0; i < offset; i++)
        if ([s characterAtIndex:i] == '\n') { line++; start = i + 1; }
    column = 1;
    for (NSUInteger i = start; i < offset; column++)
        i = NSMaxRange([s rangeOfComposedCharacterSequenceAtIndex:i]);
}

- (void)addToolsTo:(IdeLink::McpServer &)mcp conn:(const std::shared_ptr<IdeConn> &)conn {
    using IdeLink::objectSchema;
    using IdeLink::schemaProperty;
    using IdeLink::Tool;
    __weak MCIdeLink *weakSelf = self;
    std::weak_ptr<IdeConn> weakConn = conn;
    auto add = [&](const char *name, const char *description, Json schema,
                   void (^run)(MCIdeLink *link, EditorController *c, const Json &args, Respond respond)) {
        Tool t;
        t.name = name;
        t.description = description;
        t.inputSchema = schema;
        t.run = [weakSelf, run](const Json &args, Respond respond) {
            MCIdeLink *link = weakSelf;
            EditorController *c = link.controller;
            if (!link || !c) { respond(ToolResult::failure("the window has closed")); return; }
            [link refreshLockIfFolderChanged];
            run(link, c, args, respond);
        };
        mcp.addTool(std::move(t));
    };

    add("openFile", "Open a file in the editor and optionally select a range of text",
        objectSchema({{"filePath", schemaProperty("string", "Path to the file to open")},
                      {"preview", schemaProperty("boolean", "Whether to open the file in preview mode")},
                      {"startText", schemaProperty("string", "Text pattern to find the start of the selection range. Selects from the beginning of this match.")},
                      {"endText", schemaProperty("string", "Text pattern to find the end of the selection range. Selects up to the end of this match. If not provided, only the startText match will be selected.")},
                      {"selectToEndOfLine", schemaProperty("boolean", "If true, selection will extend to the end of the line containing the endText match.")},
                      {"makeFrontmost", schemaProperty("boolean", "Whether to make the file the active editor tab. If false, the file will be opened in the background without changing focus.")}},
                     {"filePath"}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)link;
        NSString *path = NSFromStd(IdeLink::resolvePath(args["filePath"].asString(), Utf8String(c.rootPath)));
        if (!MCRegularFile(path)) {
            respond(ToolResult::failure("File not found: " + Utf8String(path)));
            return;
        }
        NSString *spelled = MCSpelledIn(c, path);
        id tab = [c openTabForAgent:spelled preview:args["preview"].asBool(false) quiet:YES];
        if (!tab) {
            respond(ToolResult::failure("Could not open " + Utf8String(path)));
            return;
        }
        std::string message = "Opened file: " + Utf8String(path);
        const std::string start = args["startText"].asString();
        if (!start.empty()) {
            NSString *text = MCFileText(c, path);
            IdeLink::TextRange r = text ? IdeLink::findTextRange(U16(text), U16(NSFromStd(start)),
                                                                U16(NSFromStd(args["endText"].asString())),
                                                                args["selectToEndOfLine"].asBool(false))
                                        : IdeLink::TextRange();
            const std::string end = args["endText"].asString();
            if (r.found) {
                int line, col, toLine, toCol;
                MCNoteLineColumn(text, r.start, line, col);
                MCNoteLineColumn(text, r.end, toLine, toCol);
                [c.agentNotes showNote:@"" from:@"Claude Code" line:line column:col
                                toLine:toLine toColumn:toCol inTab:tab];
                if (end.empty()) message = "Opened file and selected text \"" + start + "\"";
                else if (r.endFound)
                    message = "Opened file and selected text from \"" + start + "\" to \"" + end + "\"";
                else
                    message = "Opened file and positioned at \"" + start + "\" (end text \"" + end +
                              "\" not found)";
            } else {
                message = "Opened file, but text \"" + start + "\" not found";
            }
            respond(ToolResult::text(message));
            return;
        }
        if (!args["makeFrontmost"].asBool(true)) {
            respond(JsonResult(Obj({{"success", true}, {"filePath", Utf8String(path)},
                                    {"fileUrl", IdeLink::fileUri(Utf8String(path))},
                                    {"message", message},
                                    {"isDirty", [link dirtyPath:path]}})));
            return;
        }
        respond(ToolResult::text(message));
    });

    add("openDiff", "Open a git diff for the file",
        objectSchema({{"old_file_path", schemaProperty("string", "Path to the file to show diff for. If not provided, uses active editor.")},
                      {"new_file_path", schemaProperty("string", "Path to the file to show diff for. If not provided, uses active editor.")},
                      {"new_file_contents", schemaProperty("string", "Contents of the new file. If not provided then the current file contents of new_file_path will be used.")},
                      {"tab_name", schemaProperty("string", "Path to the file to show diff for. If not provided, uses active editor.")}}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        [link openDiff:args in:c conn:weakConn respond:respond];
    });

    add("getDiagnostics", "Get language diagnostics from VS Code",
        objectSchema({{"uri", schemaProperty("string", "Optional file URI to get diagnostics for. If not provided, gets diagnostics for all files.")}}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)link;
        respond(JsonResult(MCDiagnostics(c, args["uri"].asString())));
    });

    add("close_tab", "Close a diff tab Claude Code opened, by its name",
        objectSchema({{"tab_name", schemaProperty("string", "The tab's name")}}, {"tab_name"}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)link;
        MCCloseOfferedDiffs(c, NSFromStd(args["tab_name"].asString()));
        respond(ToolResult::text("TAB_CLOSED"));
    });

    add("closeAllDiffTabs", "Close all diff tabs in the editor", objectSchema({}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)link;
        (void)args;
        const NSUInteger n = MCCloseOfferedDiffs(c, nil);
        respond(ToolResult::text("CLOSED_" + std::to_string(n) + "_DIFF_TABS"));
    });

    add("getCurrentSelection", "Get the current text selection in the active editor", objectSchema({}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)c;
        (void)args;
        Json sel = [link currentSelectionJson];
        if (sel.isNull()) {
            respond(JsonResult(Obj({{"success", false}, {"message", "No active editor found"}})));
            return;
        }
        sel.set("success", true);
        respond(JsonResult(sel));
    });

    add("getLatestSelection", "Get the most recent text selection (even if not in the active editor)",
        objectSchema({}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)c;
        (void)args;
        Json sel = [link currentSelectionJson];
        if (sel.isNull()) sel = link->_latestSelection;
        if (sel.isNull()) {
            respond(JsonResult(Obj({{"success", false}, {"message", "No selection available"}})));
            return;
        }
        sel.set("success", true);
        respond(JsonResult(sel));
    });

    add("getOpenEditors", "Get information about currently open editors", objectSchema({}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)link;
        (void)args;
        Json tabs = Json::array();
        for (NSDictionary *t in [c controlTabs]) {
            NSString *path = [t[@"path"] isKindOfClass:NSString.class] ? t[@"path"] : nil;
            if (!path.length || ![t[@"kind"] isEqualToString:@"text"]) continue;
            tabs.push(Obj({{"uri", IdeLink::fileUri(Utf8String(path))},
                           {"isActive", (bool)[t[@"active"] boolValue]},
                           {"isPinned", false},
                           {"isPreview", (bool)[t[@"preview"] boolValue]},
                           {"isDirty", (bool)[t[@"dirty"] boolValue]},
                           {"label", Utf8String(t[@"title"])},
                           {"groupIndex", 0},
                           {"viewColumn", 1},
                           {"isGroupActive", true},
                           {"fileName", Utf8String(path)}}));
        }
        respond(JsonResult(Obj({{"tabs", tabs}})));
    });

    add("getWorkspaceFolders", "Get all workspace folders currently open in the IDE", objectSchema({}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        (void)link;
        (void)args;
        const std::string root = Utf8String(c.rootPath);
        Json folders = Json::array();
        if (!root.empty())
            folders.push(Obj({{"name", Utf8String(c.rootPath.lastPathComponent)},
                              {"uri", IdeLink::fileUri(root)},
                              {"path", root},
                              {"index", 0}}));
        respond(JsonResult(Obj({{"success", true}, {"folders", folders},
                                {"rootPath", root.empty() ? Json() : Json(root)},
                                {"workspaceFile", Json()}})));
    });

    add("checkDocumentDirty", "Check if a document has unsaved changes (is dirty)",
        objectSchema({{"filePath", schemaProperty("string", "Path to the file to check")}}, {"filePath"}),
        ^(MCIdeLink *link, EditorController *c, const Json &args, Respond respond) {
        NSString *path = NSFromStd(IdeLink::resolvePath(args["filePath"].asString(), Utf8String(c.rootPath)));
        NSNumber *dirty = [link tabDirtiness:path];
        if (!dirty) {
            respond(JsonResult(Obj({{"success", false},
                                    {"message", "Document not open: " + Utf8String(path)}})));
            return;
        }
        respond(JsonResult(Obj({{"success", true}, {"filePath", Utf8String(path)},
                                {"isDirty", (bool)dirty.boolValue}, {"isUntitled", false}})));
    });
}

// Whether the file's tab has unsaved changes; nil when no tab has it.
- (NSNumber *)tabDirtiness:(NSString *)path {
    NSString *want = path.stringByResolvingSymlinksInPath;
    for (NSDictionary *t in [self.controller controlTabs]) {
        NSString *p = [t[@"path"] isKindOfClass:NSString.class] ? t[@"path"] : nil;
        if (p && [p.stringByResolvingSymlinksInPath isEqualToString:want]) return t[@"dirty"];
    }
    return nil;
}

- (bool)dirtyPath:(NSString *)path {
    return [[self tabDirtiness:path] boolValue];
}

// The language servers' problems, for one file (as Claude Code named it,
// and answered under that name even when it has none) or every file.
static Json MCDiagnostics(EditorController *c, const std::string &uri) {
    NSString *only = nil;
    if (!uri.empty()) {
        const std::string p = IdeLink::pathFromUri(uri);
        if (p.empty()) return Json::array();
        only = NSFromStd(p);
    }
    NSString *onlyReal = only.stringByResolvingSymlinksInPath;
    std::vector<IdeLink::FileDiagnostics> files;
    NSMutableDictionary<NSString *, NSNumber *> *index = [NSMutableDictionary dictionary];
    for (MCProblem *p in [c agentProblems]) {
        if (p.heading || !p.path) continue;
        NSString *name = p.path;
        if (only) {
            if (![p.path.stringByResolvingSymlinksInPath isEqualToString:onlyReal]) continue;
            name = only;
        }
        NSNumber *at = index[name];
        if (!at) {
            at = @(files.size());
            index[name] = at;
            IdeLink::FileDiagnostics f;
            f.path = Utf8String(name);
            files.push_back(f);
        }
        IdeLink::Diagnostic d;
        d.line = (int)p.line;
        d.character = (int)p.column;
        d.endLine = (int)p.endLine;
        d.endCharacter = (int)p.endColumn;
        d.severity = (int)p.severity;
        d.message = Utf8String(p.fullMessage ?: p.message ?: @"");
        d.source = Utf8String(p.source ?: @"");
        d.code = Utf8String(p.code ?: @"");
        files[at.unsignedIntegerValue].diagnostics.push_back(d);
    }
    if (only && files.empty()) {
        IdeLink::FileDiagnostics f;
        f.path = Utf8String(only);
        files.push_back(f);
    }
    return IdeLink::diagnosticsJson(files);
}

// Closes the diff tabs the link offered (those named `name`, or all of them),
// never another tab; returns how many.
static NSUInteger MCCloseOfferedDiffs(EditorController *c, NSString *name) {
    return [c closeComparisonTabsMatching:^BOOL(MCComparison *cmp) {
        return [MCOfferedDiffs() containsObject:cmp] && (!name || [cmp.title isEqualToString:name]);
    }];
}

// The text of the file on disk for the left side of a diff, off the main
// thread: "" for a file that is not there yet (*exists NO), nil (with
// *error) for one that cannot be shown.
static NSString *MCDiffBaseText(NSString *path, BOOL *exists, std::string &error) {
    struct stat st;
    *exists = NO;
    if (stat(path.fileSystemRepresentation, &st) != 0) return @"";
    *exists = YES;
    if (!S_ISREG(st.st_mode)) { error = "not a plain file: " + Utf8String(path); return nil; }
    if (st.st_size > 16 * 1024 * 1024) { error = Utf8String(path.lastPathComponent) + " is larger than 16 MB"; return nil; }
    NSData *data = [NSData dataWithContentsOfFile:path];
    NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
    if (!text) error = Utf8String(path.lastPathComponent) + " is not UTF-8 text";
    return text;
}

- (void)openDiff:(const Json &)args in:(EditorController *)c conn:(std::weak_ptr<IdeConn>)conn
         respond:(Respond)respond {
    const std::string root = Utf8String(c.rootPath);
    NSString *newPath = NSFromStd(IdeLink::resolvePath(args["new_file_path"].asString(), root));
    NSString *oldPath = args.has("old_file_path")
        ? NSFromStd(IdeLink::resolvePath(args["old_file_path"].asString(), root)) : newPath;
    if (!newPath.length) {
        respond(ToolResult::failure("openDiff needs new_file_path"));
        return;
    }
    const bool haveContents = args["new_file_contents"].isString();
    NSString *contents = haveContents ? NSFromStd(args["new_file_contents"].asString()) : nil;
    NSString *tabName = args["tab_name"].isString() ? NSFromStd(args["tab_name"].asString())
        : [NSString stringWithFormat:@"%@ (Claude Code)", newPath.lastPathComponent];
    NSString *shortPath = [self shortPath:newPath];
    __weak MCIdeLink *weakSelf = self;
    __weak EditorController *weakC = c;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        BOOL exists = NO;
        std::string error;
        NSString *left = MCDiffBaseText(oldPath, &exists, error);
        NSString *right = contents;
        if (left && !right) {   // no new text: the file as it is now
            BOOL there = NO;
            right = MCDiffBaseText(newPath, &there, error) ?: @"";
        }
        MCComparison *cmp = nil;
        if (left) {
            cmp = [MCComparison new];
            cmp.leftText = left;
            cmp.rightText = right;
            cmp.leftLabel = exists ? [NSString stringWithFormat:@"%@ (on disk)", shortPath]
                                   : [NSString stringWithFormat:@"%@ (a new file)", shortPath];
            cmp.rightLabel = [NSString stringWithFormat:@"%@ (Claude Code's change)", shortPath];
            cmp.title = tabName;
            cmp.path = newPath;
            [cmp prepare];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            MCIdeLink *link = weakSelf;
            EditorController *win = weakC;
            if (!cmp) { respond(ToolResult::failure(error.empty() ? "could not read the file" : error)); return; }
            if (!link || !win) { respond(ToolResult::failure("the window has closed")); return; }
            NSString *answer = right;
            cmp.decisionPrompt = [NSString stringWithFormat:@"Claude Code wants to %@ %@.",
                                  exists ? @"change" : @"create", newPath.lastPathComponent];
            // Accepted is the text made of the hunks the user accepted (all
            // of Claude's when the whole change was), which Claude Code
            // writes as it is.
            __weak MCComparison *weakCmp = cmp;
            cmp.onDecision = ^(BOOL accepted) {
                NSString *text = weakCmp.acceptedText ?: answer;
                respond(accepted ? IdeLink::diffAccepted(Utf8String(text))
                                 : IdeLink::diffRejected(Utf8String(tabName)));
                [weakSelf prunePending];
            };
            [MCOfferedDiffs() addObject:cmp];
            link->_pendingConns.push_back({cmp, conn});
            [win showComparison:cmp preview:NO quiet:YES];
        });
    });
}

- (NSString *)shortPath:(NSString *)path {
    NSString *root = self.controller.rootPath.stringByResolvingSymlinksInPath;
    NSString *real = path.stringByResolvingSymlinksInPath;
    if (root.length > 1 && [real hasPrefix:[root stringByAppendingString:@"/"]])
        return [real substringFromIndex:root.length + 1];
    return path.stringByAbbreviatingWithTildeInPath;
}

@end

// --------------------------------------------------------- the windows
static NSMapTable<EditorController *, MCIdeLink *> *gLinks;
static NSHashTable<EditorController *> *gWindows;

static void MCIdeLinkSync(EditorController *c) {
    const BOOL want = [AppSettings shared].settings.ideLink() && [gWindows containsObject:c];
    MCIdeLink *link = [gLinks objectForKey:c];
    if (want && !link) {
        link = [[MCIdeLink alloc] initWithController:c];
        if ([link start]) [gLinks setObject:link forKey:c];
    } else if (!want && link) {
        [link stop];
        [gLinks removeObjectForKey:c];
    }
}

static void MCIdeLinkSetUp(void) {
    if (gLinks) return;
    gLinks = [NSMapTable weakToStrongObjectsMapTable];
    gWindows = [NSHashTable weakObjectsHashTable];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:MCSettingsDidChangeNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
        (void)n;
        for (EditorController *c in gWindows.allObjects) MCIdeLinkSync(c);
    }];
    [nc addObserverForName:NSWindowWillCloseNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
        for (EditorController *c in gWindows.allObjects) {
            if (c.window != n.object) continue;
            [gWindows removeObject:c];
            MCIdeLinkSync(c);
        }
    }];
    [nc addObserverForName:NSTextViewDidChangeSelectionNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
        if (gLinks.count == 0) return;
        for (EditorController *c in gLinks.keyEnumerator.allObjects)
            if ([c isEditorTextView:n.object]) [[gLinks objectForKey:c] selectionMoved];
    }];
}

void MCIdeLinkWindowOpened(EditorController *controller) {
    MCIdeLinkSetUp();
    [gWindows addObject:controller];
    // After the window's init has finished.
    __weak EditorController *weak = controller;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (weak) MCIdeLinkSync(weak);
    });
}

MCIdeLink *MCIdeLinkFor(EditorController *controller) {
    return controller ? [gLinks objectForKey:controller] : nil;
}

NSArray<NSString *> *MCIdeLinkTerminalEnvironment(NSString *dir) {
    if (!gLinks.count || !dir.length) return @[];
    NSString *real = dir.stringByResolvingSymlinksInPath;
    MCIdeLink *best = nil;
    NSUInteger bestLength = 0;
    for (EditorController *c in gLinks.keyEnumerator.allObjects) {
        NSString *root = c.rootPath.stringByResolvingSymlinksInPath;
        if (!root.length) continue;
        const BOOL inside = [real isEqualToString:root] ||
                            [real hasPrefix:[root isEqualToString:@"/"] ? root : [root stringByAppendingString:@"/"]];
        if (inside && root.length >= bestLength) {
            best = [gLinks objectForKey:c];
            bestLength = root.length;
        }
    }
    if (!best.port) return @[];
    return @[[NSString stringWithFormat:@"CLAUDE_CODE_SSE_PORT=%d", best.port]];
}

// ------------------------------------------------------------- the menu
@interface MCIdeLinkMenuTarget : NSObject
@end
@implementation MCIdeLinkMenuTarget
- (void)toggleIdeLink:(id)sender {
    (void)sender;
    const BOOL on = [AppSettings shared].settings.ideLink();
    if (![[AppSettings shared] writeValue:on ? @"off" : @"on" forKey:@"agent.ide-link"]) {
        NSBeep();
        return;
    }
    if (!on && MCIdeLinkProblem()) {
        NSBeep();
        NSLog(@"minicode: Claude Code IDE link: %@", MCIdeLinkProblem());
    }
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(toggleIdeLink:))
        item.state = [AppSettings shared].settings.ideLink() ? NSControlStateValueOn
                                                             : NSControlStateValueOff;
    return YES;
}
@end

NSMenuItem *MCIdeLinkMenuItem(void) {
    static MCIdeLinkMenuTarget *target;
    if (!target) target = [MCIdeLinkMenuTarget new];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Claude Code IDE Link"
                                                  action:@selector(toggleIdeLink:)
                                           keyEquivalent:@""];
    item.target = target;
    item.toolTip = @"Lets Claude Code's /ide see this window's selection and problems, open files "
                   @"and offer diffs to accept or reject (agent.ide-link).";
    return item;
}
