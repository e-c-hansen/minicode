// Lsp.mm — Objective-C++. See Lsp.h. Server processes (MCLspServer), the
// editor's text view (CodeTextView), the completion list and its
// documentation (MCCompletionPopup), the signature hint (MCSignaturePopup)
// and the per-window glue (LspSession).
#import "Lsp.h"
#import "AppSettings.h"
#import "Terminal.h"   // MCTerminalCommandDidFinishNotification
#include "InstallHints.h"
#include "LspClient.h"
#include "MarkdownParser.h"
#include "PythonEnvs.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <memory>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

NSNotificationName const MCLspSessionDidChangeNotification = @"MCLspSessionDidChange";
NSNotificationName const MCPythonInterpreterDidChangeNotification =
    @"MCPythonInterpreterDidChange";

// ------------------------------------------------------------------ helpers
static NSColor *LHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static NSString *NS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

static std::string Std(NSString *s) {
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

static std::u16string U16Of(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}

// The canonical spelling of a path (/tmp -> /private/tmp), so paths from a
// server and from the file tree can be compared. The path itself if it does
// not exist.
static NSString *RealPath(NSString *p) {
    if (!p.length) return p;
    char buf[PATH_MAX];
    if (realpath(p.fileSystemRepresentation, buf)) return [NSString stringWithUTF8String:buf];
    return p.stringByStandardizingPath;
}

static BOOL IsIdentChar(unichar c) {
    return c == '_' || c == '$' ||
           [[NSCharacterSet alphanumericCharacterSet] characterIsMember:c];
}

static BOOL Contains(const std::vector<std::string> &list, const std::string &s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}

// The status dot's colors.
static NSColor *DotGreen(void) { return LHex(0x73C991); }
static NSColor *DotYellow(void) { return LHex(0xCCA700); }
static NSColor *DotRed(void) { return LHex(0xF14C4C); }
static NSColor *DotGray(void) { return LHex(0x9CA3AF); }

// ------------------------------------------------------ finding a server
// Folders inside `dir`, newest version first (v22.17.0 before v18.20.4,
// 3.13 before 3.9).
static NSArray<NSString *> *VersionedSubdirs(NSString *dir) {
    NSArray<NSString *> *names =
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
    names = [names sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        return [b compare:a options:NSNumericSearch];
    }];
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *n in names)
        if (![n hasPrefix:@"."]) [out addObject:[dir stringByAppendingPathComponent:n]];
    return out;
}

// npm's global prefix when ~/.npmrc sets one (prefix=~/.npm-global).
static NSString *NpmrcPrefix(NSString *home) {
    NSString *rc = [NSString stringWithContentsOfFile:[home stringByAppendingPathComponent:@".npmrc"]
                                             encoding:NSUTF8StringEncoding error:nil];
    NSCharacterSet *ws = [NSCharacterSet whitespaceCharacterSet];
    for (NSString *line in [rc componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [[line substringToIndex:eq.location] stringByTrimmingCharactersInSet:ws];
        if (![key isEqualToString:@"prefix"]) continue;
        NSString *v = [[line substringFromIndex:NSMaxRange(eq)] stringByTrimmingCharactersInSet:ws];
        v = [v stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\"'"]];
        v = [v stringByReplacingOccurrencesOfString:@"${HOME}" withString:home];
        if (v.length) return v.stringByExpandingTildeInPath;
    }
    return nil;
}

// A Finder-launched app gets a short PATH (/usr/bin:/bin:/usr/sbin:/sbin), so
// the places package managers put language servers are searched as well.
NSArray<NSString *> *MCSearchDirs(void) {
    NSMutableArray<NSString *> *dirs = [NSMutableArray array];
    NSString *path = NSProcessInfo.processInfo.environment[@"PATH"] ?: @"";
    NSString *home = NSHomeDirectory();
    NSString * (^h)(NSString *) = ^NSString *(NSString *sub) {
        return [home stringByAppendingPathComponent:sub];
    };
    NSMutableArray<NSString *> *extra =
        [NSMutableArray arrayWithObjects:@"/opt/homebrew/bin", @"/usr/local/bin",
                                         @"/opt/local/bin", nil];
    // npm's global installs: a prefix of the user's own, the folder npm's
    // docs suggest, and the Node versions nvm, fnm and volta manage.
    NSString *npm = NpmrcPrefix(home);
    if (npm) [extra addObject:[npm stringByAppendingPathComponent:@"bin"]];
    [extra addObject:h(@".npm-global/bin")];
    for (NSString *v in VersionedSubdirs(h(@".nvm/versions/node")))
        [extra addObject:[v stringByAppendingPathComponent:@"bin"]];
    for (NSString *v in VersionedSubdirs(h(@"Library/Application Support/fnm/node-versions")))
        [extra addObject:[v stringByAppendingPathComponent:@"installation/bin"]];
    [extra addObjectsFromArray:@[h(@".volta/bin"), h(@"Library/pnpm"), h(@".bun/bin"),
                                 h(@".cargo/bin"), h(@"go/bin"),
                                 h(@".local/bin")]];   // pipx, uv tool, pip --user on Linux
    // pip install --user on a Mac.
    for (NSString *v in VersionedSubdirs(h(@"Library/Python")))
        [extra addObject:[v stringByAppendingPathComponent:@"bin"]];
    [extra addObject:h(@".pyenv/shims")];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSString *> *existing = [NSMutableArray array];
    for (NSString *d in extra) {
        BOOL isDir = NO;
        if ([fm fileExistsAtPath:d isDirectory:&isDir] && isDir) [existing addObject:d];
    }
    [existing addObject:@"/usr/bin"];
    // Homebrew's llvm is not linked into bin; its clangd comes after the
    // developer tools' own.
    for (NSString *d in @[@"/opt/homebrew/opt/llvm/bin", @"/usr/local/opt/llvm/bin"])
        if ([fm fileExistsAtPath:d]) [existing addObject:d];
    for (NSString *d in [[path componentsSeparatedByString:@":"]
                            arrayByAddingObjectsFromArray:existing])
        if (d.length && ![dirs containsObject:d]) [dirs addObject:d];
    return dirs;
}

// The developer tools' own copy of a tool. /usr/bin/clangd is only a shim
// that runs it through xcrun, and on a Mac without the command line tools
// running the shim pops up the "install developer tools" dialog, which an
// editor opening a .c file must never do. So the shim is never run; the
// real binary is, when it exists.
NSString *MCDeveloperToolPath(NSString *name) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dev = [fm destinationOfSymbolicLinkAtPath:@"/var/db/xcode_select_link"
                                                  error:nil];
    NSMutableArray *roots = [NSMutableArray array];
    if (dev.length) [roots addObject:dev];
    [roots addObject:@"/Library/Developer/CommandLineTools"];
    for (NSString *root in roots) {
        for (NSString *sub in @[@"usr/bin",
                                @"Toolchains/XcodeDefault.xctoolchain/usr/bin"]) {
            NSString *p = [[root stringByAppendingPathComponent:sub]
                              stringByAppendingPathComponent:name];
            if ([fm isExecutableFileAtPath:p]) return p;
        }
    }
    return nil;
}

// `first` is searched before everything else (a Python environment's bin,
// where pylsp may be installed).
static NSString *FindProgram(NSString *name, NSArray<NSString *> *first) {
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([name containsString:@"/"]) {
        NSString *p = name.stringByExpandingTildeInPath;
        return [fm isExecutableFileAtPath:p] ? p : nil;
    }
    NSArray *dirs = first.count ? [first arrayByAddingObjectsFromArray:MCSearchDirs()]
                                : MCSearchDirs();
    for (NSString *dir in dirs) {
        if ([dir isEqualToString:@"/usr/bin"]) {
            NSString *real = MCDeveloperToolPath(name);
            if (real) return real;
            continue;
        }
        NSString *p = [dir stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        if ([fm isExecutableFileAtPath:p] && [fm fileExistsAtPath:p isDirectory:&isDir] && !isDir)
            return p;
    }
    return nil;
}

NSString *MCFindProgram(NSString *name) { return FindProgram(name, nil); }

// A command line from the settings (or the defaults) -> program path plus
// arguments, or nil when the program is not installed.
static NSArray<NSString *> *MCResolveCommand(const std::string &command,
                                             NSArray<NSString *> *first) {
    std::vector<std::string> words = Lsp::splitCommand(command);
    if (words.empty()) return nil;
    NSString *prog = FindProgram(NS(words[0]), first);
    if (!prog) return nil;
    NSMutableArray *argv = [NSMutableArray arrayWithObject:prog];
    for (size_t i = 1; i < words.size(); i++) [argv addObject:NS(words[i])];
    return argv;
}

static std::vector<std::string> ArgvWords(NSArray<NSString *> *argv) {
    std::vector<std::string> words;
    for (NSString *a in argv) words.push_back(Std(a));
    return words;
}

// How a server is named in the status bar: pyright, not pyright-langserver.
static NSString *ServerDisplayName(NSString *key, NSArray<NSString *> *argv) {
    if ([key isEqualToString:@"python"]) {
        Lsp::PythonServer kind = Lsp::pythonServerKind(ArgvWords(argv));
        if (kind != Lsp::PythonServer::Other) return NS(Lsp::pythonServerName(kind));
    }
    NSString *name = argv.firstObject.lastPathComponent;
    if ([name hasSuffix:@"-langserver"])
        name = [name substringToIndex:name.length - @"-langserver".length];
    return name;
}

// ------------------------------------------------------------- MCLspServer
// One language server process. The client state machine runs on the main
// thread; bytes are read on a background queue and handed over, and writes
// go through a serial queue so a server that is slow to read never blocks
// typing.
@class MCLspServer;
static NSMutableSet<MCLspServer *> *gServers;   // every running server

@interface MCLspServer : NSObject
@property(nonatomic, readonly, copy) NSString *key;
@property(nonatomic, readonly, copy) NSString *name;   // for the status bar: "pyright"
@property(nonatomic, readonly, copy) NSString *program;
@property(nonatomic, readonly, copy) NSArray<NSString *> *argv;
@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) BOOL stopping;
@property(nonatomic, readonly) pid_t pid;
@property(nonatomic, readonly) NSDate *started;
// Once it has ended: "exited with status 1", "killed by signal 9", with the
// last line it wrote to stderr.
@property(nonatomic, readonly, copy) NSString *exitDescription;
@property(nonatomic, readonly) BOOL processRunning;   // NSTask's view, any thread
@property(nonatomic, copy) void (^onExit)(MCLspServer *server);
- (instancetype)initWithKey:(NSString *)key argv:(NSArray<NSString *> *)argv
                       root:(NSString *)root;
- (Lsp::Client &)client;
- (void)stop;       // shutdown, exit, then signals if it lingers
- (void)stopNow;    // for app quit: no waiting on the event loop
@end

@implementation MCLspServer {
    NSTask *_task;
    NSFileHandle *_input;
    int _inputFd;
    BOOL _inputClosed;
    dispatch_queue_t _writeQueue;
    std::unique_ptr<Lsp::Client> _client;
    NSFileHandle *_log;
    NSMutableData *_errTail;   // the end of what it wrote to stderr
}

- (instancetype)initWithKey:(NSString *)key argv:(NSArray<NSString *> *)argv
                       root:(NSString *)root {
    if (!(self = [super init])) return nil;
    _key = [key copy];
    _argv = [argv copy];
    _program = [argv.firstObject copy];
    _name = [ServerDisplayName(key, argv) copy];
    _errTail = [NSMutableData data];
    _task = [[NSTask alloc] init];
    _task.executableURL = [NSURL fileURLWithPath:argv.firstObject];
    _task.arguments = [argv subarrayWithRange:NSMakeRange(1, argv.count - 1)];
    BOOL isDir = NO;
    if (root && [[NSFileManager defaultManager] fileExistsAtPath:root isDirectory:&isDir] && isDir)
        _task.currentDirectoryURL = [NSURL fileURLWithPath:root];

    // Servers start helpers of their own (node, go, cargo), which need the
    // same wider PATH the server was found on. The server's own folder goes
    // first: a script that starts with "#!/usr/bin/env node" then runs the
    // node it was installed with (nvm keeps them side by side), not
    // whichever comes first on PATH.
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    NSMutableArray *path = [NSMutableArray arrayWithObject:
        _program.stringByDeletingLastPathComponent];
    for (NSString *d in MCSearchDirs()) if (![path containsObject:d]) [path addObject:d];
    env[@"PATH"] = [path componentsJoinedByString:@":"];
    _task.environment = env;

    NSPipe *toServer = [NSPipe pipe], *fromServer = [NSPipe pipe], *errors = [NSPipe pipe];
    _task.standardInput = toServer;
    _task.standardOutput = fromServer;
    // stderr is drained as it comes, so it never fills (clangd logs every
    // request there, and a full pipe would stall the server mid-reply). Its
    // last lines say why a server that dies died.
    _task.standardError = errors;
    NSString *logPath = NSProcessInfo.processInfo.environment[@"MINICODE_LSP_LOG"];
    if (logPath.length) {
        // O_APPEND, so several servers (and their stderr) share one file
        // without writing over each other.
        int fd = open(logPath.fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) _log = [[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];
    }

    __weak MCLspServer *weakSelf = self;
    fromServer.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *data = h.availableData;
        if (data.length == 0) { h.readabilityHandler = nil; return; }
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf received:data]; });
    };
    errors.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *data = h.availableData;
        if (data.length == 0) { h.readabilityHandler = nil; return; }
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf stderrArrived:data]; });
    };
    _task.terminationHandler = ^(NSTask *t) {
        int status = t.terminationStatus;
        NSTaskTerminationReason reason = t.terminationReason;
        // A moment later, so what it wrote to stderr as it died is in.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [weakSelf endedWithStatus:status reason:reason];
        });
    };

    NSError *err = nil;
    if (![_task launchAndReturnError:&err]) {
        NSLog(@"MiniCode: could not start %@: %@", _name, err.localizedDescription);
        return nil;
    }
    _running = YES;
    _started = [NSDate date];
    _pid = _task.processIdentifier;
    _input = toServer.fileHandleForWriting;
    _inputFd = _input.fileDescriptor;
    // A server that dies mid-write must not take the editor with it: with
    // NOSIGPIPE the write fails with EPIPE instead of raising SIGPIPE.
    fcntl(_inputFd, F_SETNOSIGPIPE, 1);
    _writeQueue = dispatch_queue_create("minicode.lsp.write", DISPATCH_QUEUE_SERIAL);
    _client = std::make_unique<Lsp::Client>([weakSelf](const std::string &bytes) {
        [weakSelf writeBytes:bytes];
    });
    if (!gServers) gServers = [NSMutableSet set];
    [gServers addObject:self];
    return self;
}

- (Lsp::Client &)client { return *_client; }
- (BOOL)processRunning { return _task.isRunning; }

- (void)logLine:(NSString *)prefix bytes:(const std::string &)bytes {
    if (!_log) return;
    NSMutableData *d = [NSMutableData data];
    [d appendData:[prefix dataUsingEncoding:NSUTF8StringEncoding]];
    [d appendBytes:bytes.data() length:bytes.size()];
    [d appendBytes:"\n" length:1];
    [_log writeData:d];
}

- (void)writeBytes:(const std::string &)bytes {
    if (_inputClosed) return;
    [self logLine:@"--> " bytes:bytes];
    NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    int fd = _inputFd;
    NSFileHandle *keep = _input;   // keeps the descriptor open until written
    dispatch_async(_writeQueue, ^{
        (void)keep;
        const char *p = (const char *)data.bytes;
        size_t left = data.length;
        while (left > 0) {
            ssize_t n = write(fd, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                return;   // EPIPE: the server is gone; its exit is handled elsewhere
            }
            p += n;
            left -= (size_t)n;
        }
    });
}

// EOF on the server's stdin, after anything already queued. Servers exit on
// it, which is the backstop if they ignore `exit`.
- (void)closeInput {
    if (_inputClosed) return;
    _inputClosed = YES;
    NSFileHandle *h = _input;
    dispatch_async(_writeQueue, ^{ [h closeFile]; });
}

- (void)received:(NSData *)data {
    if (_log) [self logLine:@"<-- " bytes:std::string((const char *)data.bytes, data.length)];
    _client->receive((const char *)data.bytes, data.length);
}

- (void)stderrArrived:(NSData *)data {
    if (_log) [_log writeData:data];
    [_errTail appendData:data];
    const NSUInteger keep = 4096;
    if (_errTail.length > keep)
        [_errTail replaceBytesInRange:NSMakeRange(0, _errTail.length - keep) withBytes:NULL
                               length:0];
}

// The last line the server wrote to stderr that says something.
- (NSString *)lastErrorLine {
    NSString *s = [[NSString alloc] initWithData:_errTail encoding:NSUTF8StringEncoding]
                      ?: [[NSString alloc] initWithData:_errTail encoding:NSISOLatin1StringEncoding];
    NSArray<NSString *> *lines = [s componentsSeparatedByCharactersInSet:
                                        [NSCharacterSet newlineCharacterSet]];
    for (NSString *line in lines.reverseObjectEnumerator) {
        NSString *t = [line stringByTrimmingCharactersInSet:
                                [NSCharacterSet whitespaceCharacterSet]];
        if (t.length) return t.length > 240 ? [[t substringToIndex:240] stringByAppendingString:@"…"] : t;
    }
    return nil;
}

- (void)endedWithStatus:(int)status reason:(NSTaskTerminationReason)reason {
    if (!_running) return;
    _running = NO;
    NSString *how = reason == NSTaskTerminationReasonUncaughtSignal
                        ? [NSString stringWithFormat:@"killed by signal %d", status]
                        : [NSString stringWithFormat:@"exited with status %d", status];
    NSString *last = [self lastErrorLine];
    _exitDescription = last ? [NSString stringWithFormat:@"%@: %@", how, last] : how;
    [self closeInput];
    [gServers removeObject:self];
    if (self.onExit) self.onExit(self);
}

- (void)stop {
    if (!_running || _stopping) return;
    _stopping = YES;
    __weak MCLspServer *weakSelf = self;
    _client->shutdown([weakSelf] { [weakSelf closeInput]; });
    // A server that doesn't answer shutdown within two seconds is told to
    // go, then made to.
    pid_t pid = _pid;
    MCLspServer *strong = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        if (!strong.running) return;
        [strong closeInput];
        kill(pid, SIGTERM);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            if (strong.running) kill(pid, SIGKILL);
        });
    });
}

- (void)stopNow {
    if (!_running) return;
    _stopping = YES;
    _client->exitNow();
    [self closeInput];
    dispatch_sync(_writeQueue, ^{});   // flushed and closed
}
@end

void MCLspTerminateAllServers(void) {
    NSArray<MCLspServer *> *servers = gServers.allObjects;
    if (!servers.count) return;
    for (MCLspServer *s in servers) [s stopNow];
    // The app is about to exit, so wait here rather than on the event loop:
    // half a second for a clean exit, then signals. NSTask updates isRunning
    // off the main thread, so polling it works while main is blocked.
    auto alive = [](MCLspServer *s) { return (bool)s.processRunning; };
    for (int i = 0; i < 50; i++) {
        BOOL any = NO;
        for (MCLspServer *s in servers) if (alive(s)) any = YES;
        if (!any) return;
        usleep(10000);
    }
    for (MCLspServer *s in servers) if (alive(s)) kill(s.pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        BOOL any = NO;
        for (MCLspServer *s in servers) if (alive(s)) any = YES;
        if (!any) return;
        usleep(10000);
    }
    for (MCLspServer *s in servers) if (alive(s)) kill(s.pid, SIGKILL);
}

NSUInteger MCLspRunningServerCount(void) { return gServers.count; }

// -------------------------------------------------------- CodeTextView
@implementation MCDiagnosticMark
@end

@implementation CodeTextView {
    BOOL _tooltipsDirty;
    NSArray<NSString *> *_tooltipMessages;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification
                 object:self.textStorage];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)setDiagnostics:(NSArray<MCDiagnosticMark *> *)diagnostics {
    _diagnostics = [diagnostics copy];
    // The old marks' tooltips go now: drawing rebuilds them only while there
    // are marks, so after a switch to a clean file the previous file's
    // messages stayed on its rects.
    [self removeAllToolTips];
    _tooltipMessages = nil;
    _tooltipsDirty = _diagnostics.count > 0;
    self.needsDisplay = YES;
}

- (BOOL)resignFirstResponder {
    BOOL ok = [super resignFirstResponder];
    if (ok && self.onFocusLost) self.onFocusLost();
    return ok;
}

// Keep the marks on their text while it is edited, until the server sends
// fresh ones: shift those after the edit, stretch those it touches.
- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || _diagnostics.count == 0) return;
    NSRange er = ts.editedRange;
    NSInteger delta = ts.changeInLength;
    NSInteger s = (NSInteger)er.location;
    NSInteger oldEnd = (NSInteger)NSMaxRange(er) - delta;
    NSInteger newEnd = (NSInteger)NSMaxRange(er);
    NSInteger length = (NSInteger)ts.length;
    NSMutableArray *kept = [NSMutableArray array];
    for (MCDiagnosticMark *m in _diagnostics) {
        NSInteger a = (NSInteger)m.range.location, b = (NSInteger)NSMaxRange(m.range);
        if (b <= s) {
            // before the edit: unchanged
        } else if (a >= oldEnd) {
            a += delta; b += delta;
        } else {
            a = MIN(a, s);
            b = MAX(newEnd, b + delta);
        }
        a = MAX(0, MIN(a, length));
        b = MAX(a, MIN(b, length));
        MCDiagnosticMark *n = [MCDiagnosticMark new];
        n.range = NSMakeRange((NSUInteger)a, (NSUInteger)(b - a));
        n.severity = m.severity;
        n.message = m.message;
        [kept addObject:n];
    }
    _diagnostics = kept;
    _tooltipsDirty = YES;
    self.needsDisplay = YES;
}

- (NSString *)diagnosticMessageAtIndex:(NSUInteger)index {
    NSMutableArray *msgs = [NSMutableArray array];
    for (MCDiagnosticMark *m in _diagnostics)
        if (NSLocationInRange(index, m.range) ||
            (m.range.length == 0 && index == m.range.location))
            [msgs addObject:m.message];
    return msgs.count ? [msgs componentsJoinedByString:@"\n"] : nil;
}

// The characters on screen, so drawing never forces layout of a whole file.
- (NSRange)visibleCharacterRange {
    NSTextLayoutManager *tlm = self.textLayoutManager;
    if (tlm) {
        NSTextRange *vr = tlm.textViewportLayoutController.viewportRange;
        if (!vr) return NSMakeRange(0, self.string.length);
        NSTextContentManager *cm = tlm.textContentManager;
        NSInteger a = [cm offsetFromLocation:cm.documentRange.location toLocation:vr.location];
        NSInteger b = [cm offsetFromLocation:cm.documentRange.location
                                  toLocation:vr.endLocation];
        if (a < 0 || b < a) return NSMakeRange(0, self.string.length);
        return NSMakeRange((NSUInteger)a, (NSUInteger)(b - a));
    }
    NSLayoutManager *lm = self.layoutManager;
    NSRange glyphs = [lm glyphRangeForBoundingRect:self.visibleRect
                                   inTextContainer:self.textContainer];
    return [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
}

// Line fragments covering a character range, in view coordinates. TextKit 2
// (the default) draws no underline from rendering attributes, so the marks
// are drawn by hand from these rects; TextKit 1 is handled in case something
// ever switches the view over.
- (void)enumerateRectsForRange:(NSRange)r block:(void (^)(NSRect rect))block {
    NSPoint origin = self.textContainerOrigin;
    NSTextLayoutManager *tlm = self.textLayoutManager;
    if (tlm) {
        NSTextContentManager *cm = tlm.textContentManager;
        id<NSTextLocation> start = [cm locationFromLocation:cm.documentRange.location
                                                 withOffset:(NSInteger)r.location];
        id<NSTextLocation> end = start ? [cm locationFromLocation:start
                                                       withOffset:(NSInteger)r.length]
                                       : nil;
        if (!start || !end) return;
        NSTextRange *tr = [[NSTextRange alloc] initWithLocation:start endLocation:end];
        if (!tr) return;
        [tlm enumerateTextSegmentsInRange:tr
                                     type:NSTextLayoutManagerSegmentTypeStandard
                                  options:NSTextLayoutManagerSegmentOptionsRangeNotRequired
                               usingBlock:^BOOL(NSTextRange *sr, CGRect frame,
                                                CGFloat baseline, NSTextContainer *c) {
            (void)sr; (void)baseline; (void)c;
            block(NSOffsetRect(frame, origin.x, origin.y));
            return YES;
        }];
        return;
    }
    NSLayoutManager *lm = self.layoutManager;
    NSRange gr = [lm glyphRangeForCharacterRange:r actualCharacterRange:NULL];
    [lm enumerateEnclosingRectsForGlyphRange:gr
                    withinSelectedGlyphRange:NSMakeRange(NSNotFound, 0)
                             inTextContainer:self.textContainer
                                  usingBlock:^(NSRect rect, BOOL *stop) {
        (void)stop;
        block(NSOffsetRect(rect, origin.x, origin.y));
    }];
}

static NSColor *SeverityColor(NSInteger severity) {
    switch (severity) {
    case 1: return LHex(0xF14C4C);   // error
    case 2: return LHex(0xCCA700);   // warning
    case 3: return LHex(0x3794FF);   // information
    default: return LHex(0x9CA3AF);  // hint
    }
}

- (void)drawRect:(NSRect)dirty {
    [super drawRect:dirty];
    if (_diagnostics.count == 0) return;
    NSRange visible = [self visibleCharacterRange];
    // Errors last, so they draw over a warning on the same text.
    NSArray *ordered = [_diagnostics sortedArrayUsingComparator:
        ^NSComparisonResult(MCDiagnosticMark *a, MCDiagnosticMark *b) {
            return a.severity > b.severity ? NSOrderedAscending
                 : a.severity < b.severity ? NSOrderedDescending : NSOrderedSame;
        }];
    for (MCDiagnosticMark *m in ordered) {
        NSRange r = m.range;
        if (NSMaxRange(r) < visible.location || r.location > NSMaxRange(visible)) continue;
        NSColor *color = SeverityColor(m.severity);
        [self enumerateRectsForRange:r block:^(NSRect rect) {
            if (!NSIntersectsRect(NSInsetRect(rect, 0, -3), dirty)) return;
            CGFloat width = MAX(rect.size.width, 6);
            // A squiggle along the bottom of the line fragment (the view is
            // flipped, so the bottom is maxY).
            CGFloat y = NSMaxY(rect) - 2, amp = 1.5, step = 2;
            NSBezierPath *path = [NSBezierPath bezierPath];
            path.lineWidth = 1;
            [path moveToPoint:NSMakePoint(rect.origin.x, y)];
            BOOL up = YES;
            for (CGFloat x = rect.origin.x + step; x <= rect.origin.x + width + 0.1; x += step) {
                [path lineToPoint:NSMakePoint(x, y + (up ? -amp : amp))];
                up = !up;
            }
            [color set];
            [path stroke];
        }];
    }
    if (_tooltipsDirty) {
        _tooltipsDirty = NO;
        dispatch_async(dispatch_get_main_queue(), ^{ [self rebuildTooltips]; });
    }
}

// One tooltip rect per visible diagnostic, rebuilt after anything moves them.
- (void)rebuildTooltips {
    [self removeAllToolTips];
    NSMutableArray<NSString *> *messages = [NSMutableArray array];
    NSRange visible = [self visibleCharacterRange];
    for (MCDiagnosticMark *m in _diagnostics) {
        NSRange r = m.range;
        if (NSMaxRange(r) < visible.location || r.location > NSMaxRange(visible)) continue;
        NSUInteger index = messages.count;
        [messages addObject:m.message ?: @""];
        [self enumerateRectsForRange:r block:^(NSRect rect) {
            rect.size.width = MAX(rect.size.width, 6);
            [self addToolTipRect:rect owner:self userData:(void *)index];
        }];
    }
    _tooltipMessages = messages;
}

- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag
             point:(NSPoint)point userData:(void *)data {
    (void)view; (void)tag;
    // Every diagnostic under the pointer, not just the one this rect is for.
    NSUInteger i = [self characterIndexForInsertionAtPoint:point];
    NSString *all = [self diagnosticMessageAtIndex:i];
    if (!all && i > 0) all = [self diagnosticMessageAtIndex:i - 1];
    if (all) return all;
    NSUInteger index = (NSUInteger)data;
    return index < _tooltipMessages.count ? _tooltipMessages[index] : @"";
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    _tooltipsDirty = YES;
}

- (void)mouseDown:(NSEvent *)event {
    NSEventModifierFlags mods = event.modifierFlags &
        NSEventModifierFlagDeviceIndependentFlagsMask;
    if (mods == NSEventModifierFlagCommand && self.onCommandClick) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        NSUInteger i = [self characterIndexForInsertionAtPoint:p];
        if (self.onCommandClick(i)) return;
    }
    if (event.clickCount == 2 && !self.editable && self.onPreviewDoubleClick) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        NSUInteger i = [self characterIndexForInsertionAtPoint:p];
        if (self.onPreviewDoubleClick(i, p)) return;
    }
    [super mouseDown:event];
}
@end

// ------------------------------------------------------- documentation text
// Documentation from a server, drawn simply: code (spans, blocks and the
// signature) in the monospaced font, the rest as plain text, with Markdown
// read by the preview's own parser.
static NSFont *DocFont(BOOL mono, BOOL bold, BOOL italic, CGFloat size) {
    NSFont *f = mono ? [NSFont monospacedSystemFontOfSize:size
                                                   weight:bold ? NSFontWeightSemibold
                                                               : NSFontWeightRegular]
                     : (bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size]);
    if (italic) f = [[NSFontManager sharedFontManager] convertFont:f toHaveTrait:NSItalicFontMask];
    return f;
}

static void TrimTrailingSpace(NSMutableAttributedString *s) {
    NSCharacterSet *ws = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    while (s.length && [ws characterIsMember:[s.string characterAtIndex:s.length - 1]])
        [s deleteCharactersInRange:NSMakeRange(s.length - 1, 1)];
}

static NSAttributedString *MCDocText(NSString *text, BOOL markdown) {
    NSColor *body = LHex(0xCCCCCC);
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    if (!markdown) {
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:text attributes:@{NSFontAttributeName: DocFont(NO, NO, NO, 12),
                                              NSForegroundColorAttributeName: body}]];
        TrimTrailingSpace(s);
        return s;
    }
    for (const MdRun &r : MarkdownParser::parse(Std(text))) {
        if (r.gap) {   // between blocks: a short gap, not a whole line
            [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:5]}]];
            continue;
        }
        if (r.rule) continue;   // the gaps around it already part what it parts
        BOOL mono = r.code || r.codeBlock || r.math;
        NSString *text = NS(r.text);
        // pylsp escapes underscores even inside code spans (`object\_hook`),
        // where Markdown keeps the backslash; nobody means it there.
        if (r.code && !r.codeBlock)
            text = [text stringByReplacingOccurrencesOfString:@"\\_" withString:@"_"];
        NSFont *font = DocFont(mono, r.bold || r.heading, r.italic, mono ? 11.5 : 12);
        NSColor *color = r.codeBlock ? LHex(0xD4D4D4)
                       : r.code ? LHex(0xCE9178)
                       : r.link ? LHex(0x4EA1F7) : body;
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:text
                attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color}]];
    }
    TrimTrailingSpace(s);
    return s;
}

// The panel beside the completion list: the item's detail (its type or
// signature), then its documentation. Nil when there is neither.
static NSAttributedString *MCCompletionDoc(const Lsp::CompletionItem &item) {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    if (!item.detail.empty())
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:NS(item.detail)
                attributes:@{NSFontAttributeName: DocFont(YES, NO, NO, 11.5),
                             NSForegroundColorAttributeName: LHex(0xD4D4D4)}]];
    NSString *doc = [NS(item.documentation) stringByTrimmingCharactersInSet:
                                               [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (doc.length) {
        if (s.length)
            [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n\n"
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:5]}]];
        [s appendAttributedString:MCDocText(doc, item.docIsMarkdown)];
    }
    return s.length ? s : nil;
}

// ------------------------------------------------------ MCCompletionPopup
// A borderless child window with a one-column table, and a second one beside
// it for the selected item's documentation. Neither becomes key: the text
// view keeps focus and forwards arrows, Return, Tab and Esc.
@interface MCCompletionPanel : NSPanel
@end
@implementation MCCompletionPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

static MCCompletionPanel *MakePopupPanel(NSRect frame) {
    MCCompletionPanel *p = [[MCCompletionPanel alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered defer:YES];
    p.hasShadow = YES;
    p.releasedWhenClosed = NO;
    p.backgroundColor = LHex(0x252526);
    p.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    p.becomesKeyOnlyIfNeeded = YES;
    return p;
}

@interface MCCompletionPopup : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, copy) void (^onAccept)(NSInteger row);
@property(nonatomic, copy) void (^onSelectionChanged)(void);
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) NSArray<NSString *> *labels;
@property(nonatomic, readonly) NSString *docText;   // nil when the doc panel is hidden
// `select` is the row to highlight (the caller decides: the best match, or
// the item the arrows chose).
- (void)showLabels:(NSArray<NSString *> *)labels details:(NSArray<NSString *> *)details
               select:(NSInteger)select atScreenRect:(NSRect)caret parent:(NSWindow *)parent;
// The selected item's documentation beside the list; nil hides it.
- (void)showDoc:(NSAttributedString *)doc;
- (void)close;
- (void)moveSelection:(NSInteger)delta;
- (NSInteger)selectedRow;
- (NSRect)frame;
@end

@implementation MCCompletionPopup {
    MCCompletionPanel *_panel, *_docPanel;
    NSTableView *_table;
    NSTextView *_docView;
    NSScrollView *_docScroll;
    NSArray<NSString *> *_details;
    __weak NSWindow *_parent;
    BOOL _docVisible;
}
static const CGFloat kRowHeight = 20, kPopupWidth = 420, kDocWidth = 400, kDocMaxHeight = 320;
static const NSInteger kMaxRows = 10;

- (void)build {
    _panel = MakePopupPanel(NSMakeRect(0, 0, kPopupWidth, 100));
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:_panel.contentView.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.wantsLayer = YES;
    scroll.layer.borderWidth = 1;
    scroll.layer.borderColor = LHex(0x454545).CGColor;

    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"item"];
    col.width = kPopupWidth - 4;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = kRowHeight;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.backgroundColor = LHex(0x252526);
    _table.style = NSTableViewStylePlain;
    _table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.action = @selector(clicked:);
    _table.refusesFirstResponder = YES;
    scroll.documentView = _table;
    [_panel.contentView addSubview:scroll];
}

- (void)buildDoc {
    _docPanel = MakePopupPanel(NSMakeRect(0, 0, kDocWidth, 100));
    _docScroll = [[NSScrollView alloc] initWithFrame:_docPanel.contentView.bounds];
    _docScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _docScroll.hasVerticalScroller = YES;
    _docScroll.autohidesScrollers = YES;
    _docScroll.drawsBackground = NO;
    _docScroll.wantsLayer = YES;
    _docScroll.layer.borderWidth = 1;
    _docScroll.layer.borderColor = LHex(0x454545).CGColor;
    _docView = [[NSTextView alloc] initWithFrame:_docScroll.bounds];
    _docView.editable = NO;
    _docView.selectable = NO;
    _docView.drawsBackground = NO;
    _docView.textContainerInset = NSMakeSize(6, 6);
    _docView.verticallyResizable = YES;
    _docView.horizontallyResizable = NO;
    _docView.autoresizingMask = NSViewWidthSizable;
    _docView.textContainer.widthTracksTextView = YES;
    _docScroll.documentView = _docView;
    [_docPanel.contentView addSubview:_docScroll];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_labels.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
    NSTextField *f = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!f) {
        f = [NSTextField labelWithString:@""];
        f.identifier = @"cell";
        f.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    if (row < 0 || (NSUInteger)row >= _labels.count) {
        f.stringValue = @"";
        return f;
    }
    NSFont *mono = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:[@" " stringByAppendingString:_labels[(NSUInteger)row]]
            attributes:@{NSFontAttributeName: mono,
                         NSForegroundColorAttributeName: LHex(0xD4D4D4)}];
    NSString *detail = (NSUInteger)row < _details.count ? _details[(NSUInteger)row] : @"";
    if (detail.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"   " stringByAppendingString:detail]
                attributes:@{NSFontAttributeName: mono,
                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
    f.attributedStringValue = s;
    return f;
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
    (void)note;
    if (_visible && self.onSelectionChanged) self.onSelectionChanged();
}

- (void)clicked:(id)sender {
    (void)sender;
    NSInteger row = _table.clickedRow;
    if (row >= 0 && self.onAccept) self.onAccept(row);
}

- (NSRect)frame { return _panel.frame; }

- (void)showLabels:(NSArray<NSString *> *)labels details:(NSArray<NSString *> *)details
               select:(NSInteger)select atScreenRect:(NSRect)caret parent:(NSWindow *)parent {
    if (!_panel) [self build];
    _labels = [labels copy];
    _details = [details copy];
    BOOL wasVisible = _visible;
    _visible = NO;   // no selection callbacks while the rows are replaced
    [_table reloadData];
    NSInteger row = select >= 0 && select < (NSInteger)labels.count ? select : 0;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];

    CGFloat h = MIN((NSInteger)labels.count, kMaxRows) * kRowHeight + 2;
    NSRect frame = NSMakeRect(caret.origin.x - 6, caret.origin.y - h - 2, kPopupWidth, h);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    if (NSMinY(frame) < NSMinY(vis)) frame.origin.y = NSMaxY(caret) + 2;   // above
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - kPopupWidth;
    [_panel setFrame:frame display:YES];
    if (!wasVisible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
    if (_docVisible) [self placeDoc];
    if (self.onSelectionChanged) self.onSelectionChanged();
}

- (void)placeDoc {
    NSRect list = _panel.frame;
    NSScreen *screen = _parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    NSRect f = _docPanel.frame;
    f.origin.x = NSMaxX(list) + 2;
    if (NSMaxX(f) > NSMaxX(vis)) f.origin.x = NSMinX(list) - f.size.width - 2;   // left of it
    f.origin.y = NSMaxY(list) - f.size.height;   // tops level
    if (NSMinY(f) < NSMinY(vis)) f.origin.y = NSMinY(vis);
    [_docPanel setFrame:f display:YES];
}

- (void)showDoc:(NSAttributedString *)doc {
    if (!doc.length || !_visible) { [self hideDoc]; return; }
    if (!_docPanel) [self buildDoc];
    [_docPanel setContentSize:NSMakeSize(kDocWidth, kDocMaxHeight)];
    [_docView.textStorage setAttributedString:doc];
    NSLayoutManager *lm = _docView.layoutManager;
    [lm ensureLayoutForTextContainer:_docView.textContainer];
    CGFloat h = ceil([lm usedRectForTextContainer:_docView.textContainer].size.height) +
                2 * _docView.textContainerInset.height + 2;
    h = MIN(MAX(h, kRowHeight + 2), kDocMaxHeight);
    NSRect f = _docPanel.frame;
    f.size = NSMakeSize(kDocWidth, h);
    [_docPanel setFrame:f display:NO];
    [self placeDoc];
    [_docView scrollRangeToVisible:NSMakeRange(0, 0)];
    if (!_docVisible) {
        [_parent addChildWindow:_docPanel ordered:NSWindowAbove];
        [_docPanel orderFront:nil];
        _docVisible = YES;
    }
}

- (void)hideDoc {
    if (!_docVisible) return;
    _docVisible = NO;
    [_parent removeChildWindow:_docPanel];
    [_docPanel orderOut:nil];
}

- (NSString *)docText { return _docVisible ? _docView.string : nil; }

- (void)close {
    [self hideDoc];
    if (!_visible) return;
    _visible = NO;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
    _labels = @[];
    _details = @[];
    // The table must hear the rows are gone, or a later layout pass asks
    // for one of them.
    [_table reloadData];
}

- (NSInteger)selectedRow { return _table ? _table.selectedRow : -1; }

- (void)moveSelection:(NSInteger)delta {
    NSInteger n = (NSInteger)_labels.count;
    if (n == 0) return;
    NSInteger row = ([self selectedRow] + delta + n) % n;   // wraps around
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];
}
@end

// ------------------------------------------------------- MCSignaturePopup
// The signature of the call being typed, above the caret's line, with the
// argument being typed in bold.
@interface MCSignaturePopup : NSObject
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) NSString *text;
- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)caret parent:(NSWindow *)parent;
- (void)close;
@end

@implementation MCSignaturePopup {
    MCCompletionPanel *_panel;
    NSTextField *_label;
    __weak NSWindow *_parent;
}
static const CGFloat kSignatureMaxWidth = 600;

- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)caret parent:(NSWindow *)parent {
    if (!_panel) {
        _panel = MakePopupPanel(NSMakeRect(0, 0, 200, 24));
        NSView *box = _panel.contentView;
        box.wantsLayer = YES;
        box.layer.borderWidth = 1;
        box.layer.borderColor = LHex(0x454545).CGColor;
        _label = [NSTextField wrappingLabelWithString:@""];
        _label.selectable = NO;
        [box addSubview:_label];
    }
    _label.attributedStringValue = text;
    NSSize fit = [_label sizeThatFits:NSMakeSize(kSignatureMaxWidth, CGFLOAT_MAX)];
    fit.width = MIN(ceil(fit.width), kSignatureMaxWidth);
    fit.height = MIN(ceil(fit.height), 200);
    NSSize size = NSMakeSize(fit.width + 16, fit.height + 10);
    _label.frame = NSMakeRect(8, 5, fit.width, fit.height);
    NSRect frame = NSMakeRect(caret.origin.x - 8, NSMaxY(caret) + 2, size.width, size.height);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    // Below the line when there is no room above it.
    if (NSMaxY(frame) > NSMaxY(vis)) frame.origin.y = caret.origin.y - size.height - 2;
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - size.width;
    if (NSMinX(frame) < NSMinX(vis)) frame.origin.x = NSMinX(vis);
    [_panel setFrame:frame display:YES];
    if (!_visible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
    _text = [text.string copy];
}

- (void)close {
    if (!_visible) return;
    _visible = NO;
    _text = nil;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
}
@end

// The first paragraph of some documentation, as one plain line.
static NSString *FirstParagraph(const std::string &doc, bool markdown) {
    NSString *s = markdown ? MCDocText(NS(doc), YES).string : NS(doc);
    s = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSRange gap = [s rangeOfString:@"\n\n"];
    if (gap.location != NSNotFound) s = [s substringToIndex:gap.location];
    s = [s stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
    return s.length > 300 ? [[s substringToIndex:300] stringByAppendingString:@"…"] : s;
}

static NSAttributedString *MCSignatureText(const Lsp::SignatureHelp &help) {
    const Lsp::SignatureInfo &sig = help.signatures[(size_t)help.activeSignature];
    NSString *label = NS(sig.label);
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:label
            attributes:@{NSFontAttributeName: DocFont(YES, NO, NO, 12),
                         NSForegroundColorAttributeName: LHex(0xD4D4D4)}];
    int ap = help.activeParameter;
    if (ap >= 0 && (size_t)ap < sig.params.size()) {
        std::pair<int, int> r = sig.params[(size_t)ap];
        if (r.first >= 0 && r.second > r.first && (NSUInteger)r.second <= label.length) {
            NSRange range = NSMakeRange((NSUInteger)r.first, (NSUInteger)(r.second - r.first));
            [s addAttributes:@{NSFontAttributeName: DocFont(YES, YES, NO, 12),
                               NSForegroundColorAttributeName: LHex(0x4EA1F7)}
                       range:range];
        }
    }
    if (help.signatures.size() > 1)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[NSString stringWithFormat:@"   %d of %zu",
                               help.activeSignature + 1, help.signatures.size()]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
    NSString *doc = nil;
    if (ap >= 0 && (size_t)ap < sig.paramDocs.size() && !sig.paramDocs[(size_t)ap].empty())
        doc = FirstParagraph(sig.paramDocs[(size_t)ap], true);
    else if (!sig.documentation.empty())
        doc = FirstParagraph(sig.documentation, sig.docIsMarkdown);
    if (doc.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"\n" stringByAppendingString:doc]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
    return s;
}

// ------------------------------------------------------------ LspSession
@implementation MCLspServerInfo
@end
@implementation MCPythonCandidate
@end

static NSString *StateText(MCLspState state) {
    switch (state) {
    case MCLspStateIdle: return @"not started";
    case MCLspStateStarting: return @"starting";
    case MCLspStateRunning: return @"running";
    case MCLspStateNotInstalled: return @"not installed";
    case MCLspStateOff: return @"off";
    case MCLspStateStopped: return @"stopped";
    case MCLspStateFailed: return @"failed to start";
    case MCLspStateRestarting: return @"restarting";
    case MCLspStateCrashed: return @"crashed";
    }
    return @"";
}

// One language's server in one window: the process when there is one, and
// what the status bar and the popover say about it when there is not.
@interface MCLspEntry : NSObject
@property(nonatomic, copy) NSString *key;
@property(nonatomic, strong) MCLspServer *server;
@property(nonatomic, assign) MCLspState state;
@property(nonatomic, copy) NSString *message;   // why it failed or crashed
@property(nonatomic, copy) NSString *command;   // the command line it runs
@property(nonatomic, copy) NSString *serverName;   // "pyright", kept after it exits
@property(nonatomic, assign) NSInteger crashes;
// Bumped by every start and stop, so a restart scheduled after a crash can
// tell it has been overtaken.
@property(nonatomic, assign) NSUInteger generation;
// Python: the interpreter the running server was given, where it came from,
// and which server it is.
@property(nonatomic, copy) NSString *interpreter, *interpreterSource;
@property(nonatomic, assign) Lsp::PythonServer pythonKind;
@end
@implementation MCLspEntry
@end

// A server that crashes is started again after 1, 2, then 4 seconds; the
// fourth crash in a row leaves it stopped, so a server that cannot start
// never spins. Three minutes of running clears the count.
static const NSInteger kMaxAutomaticRestarts = 3;
static const NSTimeInterval kCrashForgiven = 180;
static NSString *const kInterpretersDefaultsKey = @"MCPythonInterpreters";

@implementation LspSession {
    __weak CodeTextView *_tv;
    NSMutableDictionary<NSString *, MCLspEntry *> *_entries;
    NSString *_path;          // the file in the view, even with no server
    NSString *_langKey;       // its language's server key, or nil
    NSString *_languageId;
    NSString *_uri;           // its URI, when a server has it open
    MCLspServer *_server;     // the server for it, or nil
    NSString *_transient;     // a brief message ("No definition found")
    std::map<std::string, std::vector<Lsp::Diagnostic>> _diags;
    NSString *_settingsSignature, *_pythonSetting;

    MCCompletionPopup *_popup;
    std::vector<Lsp::CompletionItem> _items, _shown;
    NSUInteger _anchor;       // where the word being completed starts
    BOOL _completing;         // a request is out or the list is showing
    BOOL _haveItems;          // the server has answered it
    BOOL _incomplete;         // ...with a list it says will change as you type
    Lsp::CompletionOrigin _origin;   // how the list came up (what Return does)
    BOOL _navigated;          // the arrow keys moved through it
    NSRange _definitionSelection;    // the selection when F12 was asked
    int _completionGen, _definitionGen, _hoverGen, _resolveGen, _signatureGen;
    int _resolveId;
    MCLspServer *_resolveServer;
    NSPopover *_hover;
    MCSignaturePopup *_signature;
}

- (instancetype)initWithTextView:(CodeTextView *)textView root:(NSString *)root {
    if (!(self = [super init])) return nil;
    _tv = textView;
    _root = [root copy];
    _entries = [NSMutableDictionary dictionary];
    _settingsSignature = [self currentSettingsSignature];
    _pythonSetting = NS([AppSettings shared].settings.pythonInterpreter());
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(storageEdited:)
               name:NSTextStorageDidProcessEditingNotification object:textView.textStorage];
    [nc addObserver:self selector:@selector(selectionChanged:)
               name:NSTextViewDidChangeSelectionNotification object:textView];
    [nc addObserver:self selector:@selector(settingsChanged:)
               name:MCSettingsDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(interpreterChosen:)
               name:MCPythonInterpreterDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(windowResignedKey:)
               name:NSWindowDidResignKeyNotification object:nil];
    // A server installed meanwhile (in the terminal, or anywhere while the
    // window was in the background) is picked up without a restart.
    [nc addObserver:self selector:@selector(windowBecameKey:)
               name:NSWindowDidBecomeKeyNotification object:nil];
    [nc addObserver:self selector:@selector(terminalCommandFinished:)
               name:MCTerminalCommandDidFinishNotification object:nil];
    __weak LspSession *weakSelf = self;
    // The list and the signature hint belong to the caret: they go when the
    // keyboard leaves the editor (Cmd+2, a click in the tree) or the window.
    textView.onFocusLost = ^{
        [weakSelf closeCompletion];
        [weakSelf closeSignature];
    };
    textView.onCommandClick = ^BOOL(NSUInteger index) {
        LspSession *s = weakSelf;
        if (!s || !s->_server) return NO;
        [s->_tv setSelectedRange:NSMakeRange(index, 0)];
        [s definitionAtIndex:index];
        return YES;
    };
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self shutdown];
}

// ---- status
- (BOOL)isSettingsFile:(NSString *)path {
    NSString *settings = [AppSettings shared].path;
    return path && settings && [RealPath(path) isEqualToString:RealPath(settings)];
}

- (void)updateStatus {
    NSString *s = @"";
    NSColor *dot = nil;
    MCLspEntry *e = _langKey ? _entries[_langKey] : nil;
    NSString *language = _langKey ? NS(Lsp::serverDisplayName(Std(_langKey))) : nil;
    if (_transient) {
        s = _transient;
    } else if (_langKey && ![AppSettings shared].settings.lspEnabled()) {
        s = @"Language servers off";
        dot = DotGray();
    } else if (_langKey && e) {
        NSString *name = e.server.name ?: e.serverName ?: language;
        switch (e.state) {
        case MCLspStateRunning: {
            int errors = 0, warnings = 0;
            auto it = _uri ? _diags.find(Std(_uri)) : _diags.end();
            if (it != _diags.end())
                for (const Lsp::Diagnostic &d : it->second) {
                    if (d.severity == Lsp::Severity::Error) errors++;
                    else if (d.severity == Lsp::Severity::Warning) warnings++;
                }
            NSMutableArray *parts = [NSMutableArray array];
            if (errors) [parts addObject:[NSString stringWithFormat:@"%d error%@", errors,
                                          errors == 1 ? @"" : @"s"]];
            if (warnings) [parts addObject:[NSString stringWithFormat:@"%d warning%@",
                                            warnings, warnings == 1 ? @"" : @"s"]];
            s = [NSString stringWithFormat:@"%@: %@", name,
                 parts.count ? [parts componentsJoinedByString:@", "] : @"no problems"];
            dot = DotGreen();
            break;
        }
        case MCLspStateStarting:
            s = [NSString stringWithFormat:@"%@ starting…", name];
            dot = DotYellow();
            break;
        case MCLspStateRestarting:
            s = [NSString stringWithFormat:@"%@ crashed, restarting…", name];
            dot = DotYellow();
            break;
        case MCLspStateNotInstalled:
            // Clicking it shows how to install one.
            s = [NSString stringWithFormat:@"Install a %@ language server…", language];
            dot = DotGray();
            break;
        case MCLspStateOff:
            s = [NSString stringWithFormat:@"%@ language server off", language];
            dot = DotGray();
            break;
        case MCLspStateStopped:
            s = [NSString stringWithFormat:@"%@ stopped", name];
            dot = DotGray();
            break;
        case MCLspStateFailed:
            s = [NSString stringWithFormat:@"%@ failed to start", name];
            dot = DotRed();
            break;
        case MCLspStateCrashed:
            s = [NSString stringWithFormat:@"%@ crashed", name];
            dot = DotRed();
            break;
        case MCLspStateIdle:
            break;
        }
    } else if ([self isSettingsFile:_path]) {
        // The settings file names the servers, so say where to see them.
        NSInteger running = 0;
        for (MCLspEntry *x in _entries.allValues) if (x.state == MCLspStateRunning) running++;
        s = running ? [NSString stringWithFormat:@"Language servers: %ld running", (long)running]
                    : @"Language servers";
        dot = running ? DotGreen() : DotGray();
    }
    _statusText = s;
    _statusDot = dot;
    if (self.onStatus) self.onStatus(s, dot);
}

// Something the status bar or the popover shows has changed.
- (void)changed {
    [self updateStatus];
    [[NSNotificationCenter defaultCenter] postNotificationName:MCLspSessionDidChangeNotification
                                                        object:self];
}

- (void)flash:(NSString *)message {
    _transient = [message copy];
    [self updateStatus];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(endFlash)
                                               object:nil];
    [self performSelector:@selector(endFlash) withObject:nil afterDelay:3];
}
- (void)endFlash {
    _transient = nil;
    [self updateStatus];
}

- (NSString *)currentLanguage { return _langKey; }

- (NSString *)missingServer {
    if (!_langKey || !_path || ![AppSettings shared].settings.lspEnabled()) return nil;
    MCLspEntry *e = _entries[_langKey];
    return e.state == MCLspStateNotInstalled ? _langKey : nil;
}

- (void)lookAgainForMissingServer {
    if (!self.missingServer) return;
    [self attachDocument];   // ensureServer looks again for a missing one
    if (!self.missingServer) [self changed];
}

- (BOOL)hasServer { return _server != nil; }

- (void)windowBecameKey:(NSNotification *)note {
    if (_tv.window && note.object == _tv.window) [self lookAgainForMissingServer];
}

- (void)terminalCommandFinished:(NSNotification *)note {
    (void)note;
    [self lookAgainForMissingServer];
}

// ---- servers
- (NSString *)currentSettingsSignature {
    const Settings &st = [AppSettings shared].settings;
    NSMutableString *sig = [NSMutableString stringWithFormat:@"%d", st.lspEnabled()];
    for (const std::string &k : Settings::lspServers())
        [sig appendFormat:@"|%s", st.lspCommand(k).c_str()];
    return sig;
}

- (MCLspEntry *)entryForKey:(NSString *)key {
    MCLspEntry *e = _entries[key];
    if (!e) {
        e = [MCLspEntry new];
        e.key = key;
        e.state = MCLspStateIdle;
        _entries[key] = e;
    }
    return e;
}

// The command lines to try for a key: the settings' own, else the defaults.
- (std::vector<std::string>)commandsForKey:(const std::string &)key {
    const Settings &st = [AppSettings shared].settings;
    if (!st.lspCommand(key).empty()) return {st.lspCommand(key)};
    return Lsp::defaultCommands(key);
}

// Where a server is looked for before the usual folders: for Python, the
// interpreter's own bin, where pylsp may be installed.
- (NSArray<NSString *> *)firstDirsForKey:(NSString *)key {
    if (![key isEqualToString:@"python"]) return nil;
    NSString *source = nil;
    NSString *interp = [self pythonInterpreterWithSource:&source];
    return interp ? @[interp.stringByDeletingLastPathComponent] : nil;
}

// The server for a key, started if it is not running and nothing says it
// should not be.
- (MCLspServer *)ensureServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    if (e.server) return e.server;
    // Not installed is looked for again: it may have been installed since.
    if (e.state != MCLspStateIdle && e.state != MCLspStateNotInstalled) return nil;
    return [self startEntry:e];
}

- (MCLspServer *)startEntry:(MCLspEntry *)e {
    e.generation++;
    e.message = nil;
    const Settings &st = [AppSettings shared].settings;
    std::string key = Std(e.key);
    if (!st.lspEnabled() || st.lspOff(key)) {
        e.state = MCLspStateOff;
        return nil;
    }
    NSArray *first = [self firstDirsForKey:e.key];
    MCLspServer *server = nil;
    NSArray *argv = nil;
    for (const std::string &cmd : [self commandsForKey:key]) {
        argv = MCResolveCommand(cmd, first);
        if (!argv) continue;
        e.command = NS(cmd);
        server = [[MCLspServer alloc] initWithKey:e.key argv:argv root:_root];
        break;
    }
    if (!argv) {
        e.state = MCLspStateNotInstalled;
        return nil;
    }
    if (!server) {
        e.state = MCLspStateFailed;
        e.message = [NSString stringWithFormat:@"%@ could not be run", argv.firstObject];
        return nil;
    }
    e.server = server;
    e.serverName = server.name;
    e.state = MCLspStateStarting;
    Lsp::Client &c = server.client;
    if ([e.key isEqualToString:@"python"]) {
        NSString *source = nil;
        NSString *interp = [self pythonInterpreterWithSource:&source];
        Lsp::PythonServer kind = Lsp::pythonServerKind(ArgvWords(argv));
        e.pythonKind = kind;
        e.interpreter = interp;
        e.interpreterSource = source;
        c.setInitializationOptions(Lsp::pythonInitializationOptions(kind, Std(interp)));
        c.setSettings(Lsp::pythonSettings(kind, Std(interp)));
    }
    __weak LspSession *weakSelf = self;
    __weak MCLspServer *weakServer = server;
    // A server being stopped (a new folder, a settings change) can still
    // answer or publish before it goes, when its replacement may have the
    // same file open under the same URI; what it sends then is dropped.
    c.onReady = [weakSelf, weakServer] {
        MCLspServer *s = weakServer;
        if (s && !s.stopping) [weakSelf serverReady:s];
    };
    c.onDiagnostics = [weakSelf, weakServer](const std::string &uri,
                                             const std::vector<Lsp::Diagnostic> &list) {
        if (!weakServer.stopping) [weakSelf diagnosticsArrived:uri list:list];
    };
    // A server that refuses the handshake would otherwise read "starting…"
    // for good while every request is dropped: stop it and say so.
    c.onInitializeFailed = [weakSelf, weakServer](const std::string &message) {
        MCLspServer *s = weakServer;
        if (s && !s.stopping) [weakSelf server:s failedToStart:message];
    };
    // Errors the server shows (pyright saying it cannot find the Python it
    // was given) are kept for the popover.
    c.onShowMessage = [weakSelf, weakServer](int type, const std::string &message) {
        MCLspServer *s = weakServer;
        if (s && !s.stopping && type == 1) [weakSelf server:s said:NS(message)];
    };
    c.onProtocolError = [weakServer](const std::string &problem) {
        NSLog(@"MiniCode: %@: %s", weakServer.name, problem.c_str());
    };
    server.onExit = ^(MCLspServer *s) { [weakSelf serverExited:s]; };
    c.initialize(Std(_root), (int)getpid());
    return server;
}

- (MCLspEntry *)entryOf:(MCLspServer *)server {
    MCLspEntry *e = _entries[server.key];
    return e.server == server ? e : nil;
}

- (void)serverReady:(MCLspServer *)server {
    MCLspEntry *e = [self entryOf:server];
    if (!e) return;
    e.state = MCLspStateRunning;
    [self changed];
}

- (void)server:(MCLspServer *)server said:(NSString *)message {
    MCLspEntry *e = [self entryOf:server];
    if (!e) return;
    e.message = message;
    [self changed];
}

- (void)server:(MCLspServer *)server failedToStart:(const std::string &)message {
    MCLspEntry *e = [self entryOf:server];
    if (e) {
        // Not tried again until it is started by hand, or the folder or the
        // settings change.
        e.state = MCLspStateFailed;
        e.message = message.empty() ? nil : NS(message);
        e.server = nil;
        e.generation++;
    }
    if (_server == server) [self detachDocument];
    [server stop];   // its client has already exited, so this just ends its input
    [self changed];
}

- (void)serverExited:(MCLspServer *)server {
    if (_server == server) [self detachDocument];
    MCLspEntry *e = [self entryOf:server];
    if (!e || server.stopping) { [self changed]; return; }
    // It went on its own: a crash. Start it again a few times, waiting
    // longer each time, then leave it to the popover's Start.
    e.server = nil;
    if (-[server.started timeIntervalSinceNow] > kCrashForgiven) e.crashes = 0;
    e.crashes++;
    e.message = server.exitDescription;
    NSLog(@"MiniCode: %@ %@", server.name, server.exitDescription);
    if (e.crashes <= kMaxAutomaticRestarts) {
        e.state = MCLspStateRestarting;
        NSUInteger gen = ++e.generation;
        double delay = pow(2.0, (double)(e.crashes - 1));
        __weak LspSession *weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            LspSession *s = weakSelf;
            if (!s || s->_entries[e.key] != e || e.generation != gen ||
                e.state != MCLspStateRestarting)
                return;
            [s startEntry:e];
            [s attachDocument];
            [s changed];
        });
    } else {
        e.state = MCLspStateCrashed;
    }
    [self changed];
}

- (void)stopEntry:(MCLspEntry *)e {
    e.generation++;
    MCLspServer *s = e.server;
    if (!s) return;
    if (_server == s) [self detachDocument];
    e.server = nil;
    [s stop];
}

- (void)stopAllServers {
    for (MCLspEntry *e in _entries.allValues) [self stopEntry:e];
    [_entries removeAllObjects];
    _diags.clear();
}

- (void)restartServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    [self stopEntry:e];
    e.crashes = 0;
    e.state = MCLspStateIdle;
    [self startEntry:e];
    [self attachDocument];
    [self changed];
}

- (void)stopServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    [self stopEntry:e];
    e.crashes = 0;
    e.message = nil;
    e.state = MCLspStateStopped;
    [self changed];
}

- (void)startServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    if (e.server) return;
    e.crashes = 0;
    e.state = MCLspStateIdle;
    [self startEntry:e];
    [self attachDocument];
    [self changed];
}

- (void)restartCurrentServer {
    if (!_langKey) {
        NSBeep();
        [self flash:@"No language server for this file"];
        return;
    }
    [self restartServer:_langKey];
}

- (NSArray<MCLspServerInfo *> *)serverInfos {
    const Settings &st = [AppSettings shared].settings;
    NSMutableArray<NSString *> *keys = [NSMutableArray array];
    if (_langKey) [keys addObject:_langKey];
    for (const std::string &k : Settings::lspServers())
        if (![keys containsObject:NS(k)]) [keys addObject:NS(k)];
    NSMutableArray<MCLspServerInfo *> *out = [NSMutableArray array];
    for (NSString *key in keys) {
        std::string k = Std(key);
        MCLspEntry *e = _entries[key];
        MCLspServerInfo *info = [MCLspServerInfo new];
        info.key = key;
        info.language = NS(Lsp::serverDisplayName(k));
        info.current = [key isEqualToString:_langKey];
        info.state = e ? e.state : MCLspStateIdle;
        info.message = e.message;
        if (e.server) {
            info.serverName = e.server.name;
            info.program = e.server.program;
            info.command = e.command;
            info.pid = e.server.pid;
        } else {
            // What would run: the first command that is installed.
            std::vector<std::string> commands = [self commandsForKey:k];
            NSArray *first = [self firstDirsForKey:key];
            for (const std::string &cmd : commands) {
                NSArray *argv = MCResolveCommand(cmd, first);
                if (!argv) continue;
                info.command = NS(cmd);
                info.program = argv.firstObject;
                info.serverName = ServerDisplayName(key, argv);
                break;
            }
            if (!info.command) {
                NSMutableArray *all = [NSMutableArray array];
                for (const std::string &cmd : commands) [all addObject:NS(cmd)];
                info.command = [all componentsJoinedByString:@", or "];
                if (info.state == MCLspStateIdle) info.state = MCLspStateNotInstalled;
            }
        }
        if (!st.lspEnabled() || st.lspOff(k)) info.state = MCLspStateOff;
        if (info.state == MCLspStateNotInstalled) {
            const Install::Tool *tool = Install::server(k);
            if (tool && !tool->ways.empty()) info.installHint = NS(tool->ways.front().command);
        }
        if ([key isEqualToString:@"python"]) {
            if (e.server) {
                info.interpreter = e.interpreter;
                info.interpreterSource = e.interpreterSource;
            } else {
                NSString *source = nil;
                info.interpreter = [self pythonInterpreterWithSource:&source];
                info.interpreterSource = source;
            }
        }
        info.stateText = StateText(info.state);
        [out addObject:info];
    }
    return out;
}

- (void)settingsChanged:(NSNotification *)note {
    (void)note;
    NSString *sig = [self currentSettingsSignature];
    NSString *python = NS([AppSettings shared].settings.pythonInterpreter());
    if (![sig isEqualToString:_settingsSignature]) {
        _settingsSignature = sig;
        _pythonSetting = python;
        NSString *path = _path;
        [self closeDocument];
        [self stopAllServers];
        [self documentOpened:path];
        return;
    }
    if (![python isEqualToString:_pythonSetting]) {
        _pythonSetting = python;
        [self applyPythonInterpreter];
    }
}

// ---- the Python interpreter
- (NSString *)rootKey { return RealPath(_root) ?: @""; }

- (NSString *)chosenPythonInterpreter {
    NSDictionary *all = [[NSUserDefaults standardUserDefaults]
                            dictionaryForKey:kInterpretersDefaultsKey];
    id p = all[[self rootKey]];
    return [p isKindOfClass:[NSString class]] && [p length] ? p : nil;
}

- (void)choosePythonInterpreter:(NSString *)path {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary *all = [[d dictionaryForKey:kInterpretersDefaultsKey] mutableCopy]
                                   ?: [NSMutableDictionary dictionary];
    if (path.length) all[[self rootKey]] = path;
    else [all removeObjectForKey:[self rootKey]];
    [d setObject:all forKey:kInterpretersDefaultsKey];
    // Every window on this folder follows, this one included.
    [[NSNotificationCenter defaultCenter]
        postNotificationName:MCPythonInterpreterDidChangeNotification object:[self rootKey]];
}

- (void)interpreterChosen:(NSNotification *)note {
    if ([note.object isEqual:[self rootKey]]) [self applyPythonInterpreter];
}

- (PythonEnvs::Context)pythonContext {
    PythonEnvs::Context ctx;
    BOOL pythonFile = [_langKey isEqualToString:@"python"] && _path;
    ctx.fileDir = Std(pythonFile ? RealPath(_path.stringByDeletingLastPathComponent)
                                 : RealPath(_root));
    ctx.root = Std(RealPath(_root));
    ctx.home = Std(NSHomeDirectory());
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    for (NSString *k in @[@"VIRTUAL_ENV", @"CONDA_PREFIX", @"PYENV_ROOT", @"WORKON_HOME",
                          @"CONDA_ENVS_PATH", @"POETRY_VIRTUALENVS_PATH", @"XDG_DATA_HOME",
                          @"XDG_CACHE_HOME"])
        if ([env[k] length]) ctx.env[Std(k)] = Std(env[k]);
    for (NSString *d in MCSearchDirs())
        if (![d isEqualToString:@"/usr/bin"]) ctx.pathDirs.push_back(Std(d));
    // /usr/bin/python3 is the developer tools' shim; their real Python is
    // offered instead, when they are installed.
    ctx.skip = {"/usr/bin/python3", "/usr/bin/python"};
    ctx.mac = true;
    return ctx;
}

static BOOL IsRunnable(NSString *path) {
    BOOL isDir = NO;
    return path.length && [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] &&
           !isDir && [[NSFileManager defaultManager] isExecutableFileAtPath:path];
}

- (NSString *)pythonInterpreterWithSource:(NSString **)source {
    return [self pythonInterpreterWithSource:source folderChoice:YES];
}

- (NSString *)defaultPythonInterpreterWithSource:(NSString **)source {
    return [self pythonInterpreterWithSource:source folderChoice:NO];
}

- (NSString *)pythonInterpreterWithSource:(NSString **)source folderChoice:(BOOL)useChoice {
    NSString *missing = nil;
    NSString *chosen = useChoice ? [self chosenPythonInterpreter].stringByExpandingTildeInPath
                                 : nil;
    if (chosen) {
        if (IsRunnable(chosen)) { if (source) *source = @"this folder"; return chosen; }
        missing = @"this folder's choice is missing";
    }
    std::string setting = [AppSettings shared].settings.pythonInterpreter();
    if (!setting.empty()) {
        NSString *p = NS(setting).stringByExpandingTildeInPath;
        if (IsRunnable(p)) { if (source) *source = @"settings"; return p; }
        if (!missing) missing = @"python.interpreter is missing";
    }
    std::string automatic = PythonEnvs::automatic([self pythonContext]);
    if (source) *source = missing ? [@"automatic; " stringByAppendingString:missing]
                                  : @"automatic";
    return automatic.empty() ? nil : NS(automatic);
}

- (NSArray<MCPythonCandidate *> *)pythonCandidates {
    NSMutableArray<MCPythonCandidate *> *out = [NSMutableArray array];
    NSMutableSet *seen = [NSMutableSet set];
    auto add = [&](NSString *path, NSString *kind, NSString *name, NSString *version) {
        if (!path.length || [seen containsObject:path]) return;
        [seen addObject:path];
        MCPythonCandidate *c = [MCPythonCandidate new];
        c.path = path;
        c.kind = kind;
        c.name = name;
        c.version = version ?: @"";
        [out addObject:c];
    };
    for (const PythonEnvs::Interpreter &i : PythonEnvs::find([self pythonContext]))
        add(NS(i.path), NS(i.kind), NS(i.name), NS(i.version));
    NSString *xcode = MCDeveloperToolPath(@"python3");
    if (xcode) add(xcode, @"Xcode tools", @"python3", NS(PythonEnvs::versionOf(Std(xcode))));
    // A choice that is none of these (picked with Other…) stays on the list.
    NSString *chosen = [self chosenPythonInterpreter].stringByExpandingTildeInPath;
    if (chosen && ![seen containsObject:chosen] && IsRunnable(chosen))
        add(chosen, @"chosen", chosen.lastPathComponent, NS(PythonEnvs::versionOf(Std(chosen))));
    return out;
}

// The interpreter may have changed (a choice, the setting, or in automatic
// mode a file in another environment): tell the server, the way it listens.
- (void)applyPythonInterpreter {
    [self applyPythonInterpreterForSwitch:NO];
}

// A switch to another file (forSwitch) changes the server only when that
// file resolves to another interpreter: a file in no environment keeps the
// one the server has, where it used to send "none" (and restart pylsp,
// which cannot be told to forget one) on every switch to such a file.
- (void)applyPythonInterpreterForSwitch:(BOOL)forSwitch {
    MCLspEntry *e = _entries[@"python"];
    NSString *source = nil;
    NSString *interp = [self pythonInterpreterWithSource:&source];
    if (!e.server) { [self changed]; return; }
    if (interp == e.interpreter || [interp isEqualToString:e.interpreter] ||
        (forSwitch && !interp && e.interpreter)) {
        if (interp) e.interpreterSource = source;
        [self changed];
        return;
    }
    Lsp::PythonServer kind = e.pythonKind;
    // jedi-language-server reads it only when starting, and pylsp cannot be
    // told to forget one.
    if (Lsp::pythonInterpreterNeedsRestart(kind) || (kind == Lsp::PythonServer::Pylsp && !interp)) {
        [self restartServer:@"python"];
        return;
    }
    e.interpreter = interp;
    e.interpreterSource = source;
    e.server.client.didChangeConfiguration(Lsp::pythonSettings(kind, Std(interp)));
    [self changed];
}

// ---- documents
- (void)detachDocument {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    [self closeCompletion];
    [self closeSignature];
    [_hover close];
    // Answers still on their way were for the document leaving the view.
    _completionGen++;
    _definitionGen++;
    _hoverGen++;
    if (_uri) _diags.erase(Std(_uri));
    _server = nil;
    _uri = nil;
    _tv.diagnostics = @[];
}

// Open the current file on its language's server, starting it if need be.
- (void)attachDocument {
    if (_server || !_path || !_langKey || ![AppSettings shared].settings.lspEnabled()) return;
    MCLspServer *server = [self ensureServer:_langKey];
    if (!server) return;
    _server = server;
    _uri = NS(Lsp::uriFromPath(Std(RealPath(_path))));
    server.client.didOpen(Std(_uri), Std(_languageId), Std(_tv.string));
}

- (void)closeDocument {
    if (_server && _uri) _server.client.didClose(Std(_uri));
    [self detachDocument];
    _path = nil;
    _langKey = nil;
    _languageId = nil;
}

- (void)documentOpened:(NSString *)path {
    [self closeDocument];
    _path = [path copy];
    Lsp::Language lang;
    if (path && Lsp::languageForExtension(Std(path.pathExtension.lowercaseString), lang)) {
        _langKey = NS(lang.server);
        _languageId = NS(lang.languageId);
        BOOL wasRunning = _entries[_langKey].server != nil;
        [self attachDocument];
        // In automatic mode another file may sit in another environment.
        // Only a real change reaches the server (same environment: nothing).
        if (wasRunning && [_langKey isEqualToString:@"python"])
            [self applyPythonInterpreterForSwitch:YES];
    }
    [self changed];
}

- (void)documentSaved {
    if (!_server || !_uri) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    _server.client.didSave(Std(_uri), Std(_tv.string));
}

- (void)setRoot:(NSString *)root {
    [self closeDocument];
    [self stopAllServers];
    _root = [root copy];
    [self changed];
}

- (void)shutdown {
    [self closeDocument];
    [self stopAllServers];
}

// Full-document sync, a moment after typing pauses. Anything that asks the
// server about the text flushes first.
- (void)flushChanges {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    if (_server && _uri) _server.client.didChange(Std(_uri), Std(_tv.string));
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || !_server || !_uri) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    [self performSelector:@selector(flushChanges) withObject:nil afterDelay:0.3];

    NSRange er = ts.editedRange;
    NSString *s = ts.string;
    BOOL typedOne = ts.changeInLength == 1 && er.length == 1;
    unichar c = typedOne ? [s characterAtIndex:er.location] : 0;
    unichar prev = typedOne && er.location > 0 ? [s characterAtIndex:er.location - 1] : 0;
    Lsp::Client &client = _server.client;

    // The signature hint: opened by the server's trigger characters ("(" and
    // ","), and brought up to date after any other edit while it shows.
    if (client.hasSignatureHelp()) {
        std::string ch = typedOne ? Std([NSString stringWithCharacters:&c length:1]) : "";
        if (typedOne && (Contains(client.signatureTriggers(), ch) ||
                         (_signature.visible && Contains(client.signatureRetriggers(), ch)))) {
            NSString *trigger = NS(ch);
            dispatch_async(dispatch_get_main_queue(), ^{ [self requestSignature:trigger]; });
        } else if (_signature.visible) {
            [self scheduleSignatureUpdate];
        }
    }

    if (typedOne) {
        // The end of ".", "->" or "::"?
        NSString *trigger = nil;
        if (c == '.') trigger = @".";
        else if (c == '>' && prev == '-') trigger = @">";
        else if (c == ':' && prev == ':') trigger = @":";
        if (trigger) {
            std::vector<std::string> triggers = client.completionTriggers();
            if (triggers.empty() || Contains(triggers, Std(trigger))) {
                // After the text view has finished the edit and moved the caret.
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self requestCompletion:trigger origin:Lsp::CompletionOrigin::Trigger];
                });
                return;
            }
        }
        // A word being typed: the list opens by itself on the first letter
        // after ".", "->" or "::" (so "json.lo" typed quickly, or after Esc,
        // still lists json's members), and on the second letter of any
        // other word.
        if (!_completing && IsIdentChar(c)) {
            NSUInteger start = er.location;
            while (start > 0 && IsIdentChar([s characterAtIndex:start - 1])) start--;
            NSUInteger length = er.location + 1 - start;
            unichar b1 = start > 0 ? [s characterAtIndex:start - 1] : 0;
            unichar b2 = start > 1 ? [s characterAtIndex:start - 2] : 0;
            BOOL afterAccess = b1 == '.' || (b1 == '>' && b2 == '-') || (b1 == ':' && b2 == ':');
            unichar first = [s characterAtIndex:start];
            BOOL number = first >= '0' && first <= '9';
            if (!number && (afterAccess || length == 2)) {
                // A member after "." counts as after the trigger character
                // (Return takes it); any other word's list leaves Return a
                // new line until the arrows are used.
                Lsp::CompletionOrigin origin = afterAccess ? Lsp::CompletionOrigin::Trigger
                                                           : Lsp::CompletionOrigin::Typing;
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self requestCompletion:nil origin:origin];
                });
                return;
            }
        }
    }
    if (_completing) {
        // A list the server called incomplete is asked for again as the word
        // grows; the old one stays up, filtered, until the new one is in.
        if (_incomplete && _haveItems && typedOne && IsIdentChar(c))
            dispatch_async(dispatch_get_main_queue(), ^{
                [self requestCompletion:nil origin:Lsp::CompletionOrigin::Typing];
            });
        dispatch_async(dispatch_get_main_queue(), ^{ [self refilterCompletion]; });
    }
}

- (void)windowResignedKey:(NSNotification *)note {
    if (!_tv.window || note.object != _tv.window) return;
    [self closeCompletion];
    [self closeSignature];
}

- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    if (_completing)
        dispatch_async(dispatch_get_main_queue(), ^{ [self refilterCompletion]; });
    if (_signature.visible) {
        if (_tv.window.firstResponder != _tv) [self closeSignature];
        else [self scheduleSignatureUpdate];
    }
}

// ---- diagnostics
- (BOOL)uri:(const std::string &)a isSameFileAs:(NSString *)b {
    if (!b) return NO;
    if (NS(a) && [NS(a) isEqualToString:b]) return YES;
    NSString *pa = NS(Lsp::pathFromUri(a)), *pb = NS(Lsp::pathFromUri(Std(b)));
    return pa.length && [RealPath(pa) isEqualToString:RealPath(pb)];
}

- (void)diagnosticsArrived:(const std::string &)uri
                      list:(const std::vector<Lsp::Diagnostic> &)list {
    if (![self uri:uri isSameFileAs:_uri]) {
        _diags[uri] = list;   // another file; shown if it is opened while cached
        return;
    }
    _diags[Std(_uri)] = list;
    std::u16string text = U16Of(_tv.string);
    NSMutableArray *marks = [NSMutableArray array];
    for (const Lsp::Diagnostic &d : list) {
        size_t a = Lsp::offsetForPosition(text, d.range.start);
        size_t b = Lsp::offsetForPosition(text, d.range.end);
        if (b <= a) {
            // An empty range: mark the word there, or at least one character.
            b = a;
            while (b < text.size() && IsIdentChar(text[b])) b++;
            if (b == a && a < text.size() && text[a] != u'\n') b = a + 1;
            else if (b == a && a > 0) a--;
        }
        MCDiagnosticMark *m = [MCDiagnosticMark new];
        m.range = NSMakeRange(a, b - a);
        m.severity = (NSInteger)d.severity;
        NSString *kind = d.severity == Lsp::Severity::Error ? @"Error"
                       : d.severity == Lsp::Severity::Warning ? @"Warning"
                       : d.severity == Lsp::Severity::Information ? @"Info" : @"Hint";
        m.message = [NSString stringWithFormat:@"%@: %@", kind, NS(d.message)];
        [marks addObject:m];
    }
    _tv.diagnostics = marks;
    [self updateStatus];
}

// ---- completion
- (NSUInteger)cursor {
    NSRange sel = _tv.selectedRange;
    return NSMaxRange(sel);
}

- (void)triggerCompletion {
    if (!_server) {
        NSBeep();
        [self flash:_statusText.length ? _statusText : @"No language server for this file"];
        return;
    }
    [self requestCompletion:nil origin:Lsp::CompletionOrigin::Explicit];
}

- (void)requestCompletion:(NSString *)trigger origin:(Lsp::CompletionOrigin)origin {
    if (!_server || !_uri) return;
    [self flushChanges];
    NSString *s = _tv.string;
    NSUInteger cursor = [self cursor];
    NSUInteger anchor = cursor;
    while (anchor > 0 && IsIdentChar([s characterAtIndex:anchor - 1])) anchor--;
    if (!_completing || anchor != _anchor) {
        // A new word: forget the old list. The same word asked again (an
        // incomplete list) keeps it showing until the answer, and keeps how
        // it first came up unless it is now asked for.
        _items.clear();
        _haveItems = NO;
        _origin = origin;
        _navigated = NO;
    } else if (origin == Lsp::CompletionOrigin::Explicit) {
        _origin = origin;
    }
    _anchor = anchor;
    _completing = YES;
    int gen = ++_completionGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(s), cursor);
    _server.client.completion(Std(uri), pos,
        [weakSelf, gen, uri](const Json &result, const Json &error) {
            (void)error;
            [weakSelf completionArrived:result gen:gen uri:uri];
        }, trigger ? Std(trigger) : "");
}

- (void)completionArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri {
    if (gen != _completionGen || ![uri isEqualToString:_uri] || !_completing) return;
    bool incomplete = false;
    _items = Lsp::parseCompletion(result, &incomplete);
    _incomplete = incomplete;
    _haveItems = YES;
    if (_items.empty()) { [self closeCompletion]; return; }
    [self refilterCompletion];
}

- (NSString *)typedPrefix {
    NSUInteger cursor = [self cursor];
    if (cursor < _anchor || cursor > _tv.string.length) return @"";
    return [_tv.string substringWithRange:NSMakeRange(_anchor, cursor - _anchor)];
}

- (void)refilterCompletion {
    if (!_completing) return;
    NSString *s = _tv.string;
    NSUInteger cursor = [self cursor];
    if (_tv.selectedRange.length > 0 || cursor < _anchor || cursor > s.length ||
        _tv.window.firstResponder != _tv) {
        [self closeCompletion];
        return;
    }
    for (NSUInteger i = _anchor; i < cursor; i++)
        if (!IsIdentChar([s characterAtIndex:i])) { [self closeCompletion]; return; }
    if (!_haveItems) return;   // still waiting for the server
    NSString *prefix = [self typedPrefix];
    // The item highlighted before, by label and text (an incomplete list
    // asked for again numbers its items afresh).
    NSInteger prevRow = _popup.visible ? _popup.selectedRow : -1;
    std::string prevLabel, prevText;
    if (prevRow >= 0 && (size_t)prevRow < _shown.size()) {
        prevLabel = _shown[(size_t)prevRow].label;
        prevText = _shown[(size_t)prevRow].textToInsert();
    }
    _shown = Lsp::filterCompletions(_items, Std(prefix));
    // Nothing to offer, or only exactly what is already typed.
    if (_shown.empty() ||
        (_shown.size() == 1 && _shown[0].textToInsert() == Std(prefix))) {
        [self closeCompletion];
        return;
    }
    if (!_popup) {
        _popup = [MCCompletionPopup new];
        __weak LspSession *weakSelf = self;
        _popup.onAccept = ^(NSInteger row) { [weakSelf acceptCompletion:row]; };
        _popup.onSelectionChanged = ^{ [weakSelf completionSelectionMoved]; };
    }
    NSMutableArray *labels = [NSMutableArray array], *details = [NSMutableArray array];
    size_t limit = std::min<size_t>(_shown.size(), 200);
    _shown.resize(limit);
    // The best match (filterCompletions puts it first), unless the arrows
    // chose an item that is still listed. A stale highlight from a wider
    // list used to stay ("os.pa" kept POSIX_SPAWN_CLOSE from "os.p").
    NSInteger select = 0;
    if (_navigated) {
        NSInteger kept = -1;
        for (size_t i = 0; i < limit && kept < 0; i++)
            if (_shown[i].label == prevLabel && _shown[i].textToInsert() == prevText)
                kept = (NSInteger)i;
        if (kept >= 0) select = kept;
        else _navigated = NO;   // the chosen item went: back to choosing for the user
    }
    for (size_t i = 0; i < limit; i++) {
        [labels addObject:NS(_shown[i].label)];
        [details addObject:NS(_shown[i].detail)];
    }
    NSRect caret = [_tv firstRectForCharacterRange:NSMakeRange(_anchor, 0) actualRange:NULL];
    [_popup showLabels:labels details:details select:select atScreenRect:caret
                parent:_tv.window];
}

// The selection in the list moved (or the list was rebuilt): show the
// item's documentation, asking the server for it when the list came
// without.
- (void)completionSelectionMoved {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(resolveSelected)
                                               object:nil];
    NSInteger row = _popup.selectedRow;
    if (row < 0 || (size_t)row >= _shown.size()) { [_popup showDoc:nil]; return; }
    const Lsp::CompletionItem &item = _shown[(size_t)row];
    BOOL willResolve = !item.resolved && _server && _server.client.canResolveCompletion();
    NSAttributedString *doc = MCCompletionDoc(item);
    // While the documentation is on its way, what shows stays rather than
    // flickering away and back as the arrows move.
    if (doc || !willResolve) [_popup showDoc:doc];
    // Asked a moment after the selection settles, so holding an arrow key
    // down does not ask for every item it passes.
    if (willResolve)
        [self performSelector:@selector(resolveSelected) withObject:nil afterDelay:0.08];
}

- (void)resolveSelected {
    if (!_server || !_completing) return;
    NSInteger row = _popup.selectedRow;
    if (row < 0 || (size_t)row >= _shown.size() || _shown[(size_t)row].resolved) return;
    if (_resolveId && _resolveServer) _resolveServer.client.cancel(_resolveId);
    int gen = ++_resolveGen;
    int index = _shown[(size_t)row].index;
    __weak LspSession *weakSelf = self;
    _resolveServer = _server;
    _resolveId = _server.client.resolveCompletion(_shown[(size_t)row],
        [weakSelf, gen, index](const Json &result, const Json &error) {
            [weakSelf completionResolved:error.isNull() ? result : Json() gen:gen index:index];
        });
}

- (void)completionResolved:(const Json &)result gen:(int)gen index:(int)index {
    if (gen != _resolveGen) return;
    _resolveId = 0;
    _resolveServer = nil;
    if (!_completing) return;
    if (index >= 0 && (size_t)index < _items.size() && _items[(size_t)index].index == index)
        Lsp::applyResolved(_items[(size_t)index], result);
    for (Lsp::CompletionItem &it : _shown)
        if (it.index == index) Lsp::applyResolved(it, result);
    NSInteger row = _popup.selectedRow;
    if (row >= 0 && (size_t)row < _shown.size() && _shown[(size_t)row].index == index)
        [_popup showDoc:MCCompletionDoc(_shown[(size_t)row])];
}

- (void)closeCompletion {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(resolveSelected)
                                               object:nil];
    if (_resolveId && _resolveServer) _resolveServer.client.cancel(_resolveId);
    _resolveId = 0;
    _resolveServer = nil;
    _completing = NO;
    _haveItems = NO;
    _incomplete = NO;
    _navigated = NO;
    _items.clear();
    _shown.clear();
    [_popup close];
}

- (void)acceptCompletion:(NSInteger)row {
    if (row < 0 || (size_t)row >= _shown.size()) { [self closeCompletion]; return; }
    Lsp::CompletionItem item = _shown[(size_t)row];
    NSUInteger cursor = [self cursor];
    NSUInteger start = _anchor;
    if (item.hasEdit) {
        // The edit's range was computed on the text as it was when asked;
        // everything before the word is unchanged since, so its start holds.
        size_t editStart = Lsp::offsetForPosition(U16Of(_tv.string), item.editRange.start);
        if (editStart <= cursor) start = editStart;
    }
    [self closeCompletion];
    NSString *insert = NS(item.textToInsert());
    NSRange range = NSMakeRange(start, cursor - start);
    if (![_tv shouldChangeTextInRange:range replacementString:insert]) return;
    [_tv.textStorage replaceCharactersInRange:range withString:insert];
    [_tv didChangeText];
    [_tv setSelectedRange:NSMakeRange(start + insert.length, 0)];
    [_tv scrollRangeToVisible:_tv.selectedRange];
}

- (BOOL)handleCommand:(SEL)sel {
    if (sel == @selector(complete:)) {
        if (!_server) return NO;   // the text view's own word completion
        [self requestCompletion:nil origin:Lsp::CompletionOrigin::Explicit];
        return YES;
    }
    if (_popup.visible) {
        if (sel == @selector(moveUp:)) { _navigated = YES; [_popup moveSelection:-1]; return YES; }
        if (sel == @selector(moveDown:)) { _navigated = YES; [_popup moveSelection:1]; return YES; }
        BOOL ret = sel == @selector(insertNewline:);
        BOOL tab = sel == @selector(insertTab:);
        BOOL backTab = sel == @selector(insertBacktab:);
        if (ret || tab || backTab) {
            // Tab takes the item; Return does when the list was asked for,
            // came after ".", or was moved through, and is otherwise a new
            // line (Lsp::completionKeyAccepts). A key that does not take it
            // closes the list and goes on to the editor (AutoIndent).
            NSInteger row = _popup.selectedRow;
            if (row < 0 || (size_t)row >= _shown.size()) {
                // Nothing selected: the key is the editor's (Tab indents).
                [self closeCompletion];
                return NO;
            }
            // The word typed out exactly as some item (case and all) is a
            // new line, unless the arrows chose another item: "os.path"
            // used to become os.PathLike.
            std::string typed = Std([self typedPrefix]);
            BOOL exact = _shown[(size_t)row].textToInsert() == typed ||
                         (!_navigated && Lsp::exactCompletion(_shown, typed) >= 0);
            Lsp::CompletionKey key = ret ? Lsp::CompletionKey::Return
                                   : tab ? Lsp::CompletionKey::Tab
                                         : Lsp::CompletionKey::ShiftTab;
            if (!Lsp::completionKeyAccepts(_origin, _navigated, key, exact)) {
                [self closeCompletion];
                return NO;
            }
            [self acceptCompletion:row];
            return YES;
        }
        if (sel == @selector(cancelOperation:)) { [self closeCompletion]; return YES; }
        return NO;
    }
    if (_signature.visible && sel == @selector(cancelOperation:)) {
        [self closeSignature];
        return YES;
    }
    return NO;
}

- (BOOL)completionVisible { return _popup.visible; }
- (NSArray<NSString *> *)completionLabels { return _popup.visible ? _popup.labels : @[]; }
- (NSString *)completionSelectedLabel {
    NSInteger row = _popup.visible ? _popup.selectedRow : -1;
    return row >= 0 && (NSUInteger)row < _popup.labels.count ? _popup.labels[(NSUInteger)row] : nil;
}
- (NSString *)completionDocText { return _popup.visible ? _popup.docText : nil; }

// ---- signature help
- (void)scheduleSignatureUpdate {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(updateSignature)
                                               object:nil];
    [self performSelector:@selector(updateSignature) withObject:nil afterDelay:0.15];
}

- (void)updateSignature {
    if (_signature.visible) [self requestSignature:nil];
}

- (void)requestSignature:(NSString *)trigger {
    if (!_server || !_uri || !_server.client.hasSignatureHelp()) return;
    [self flushChanges];
    int gen = ++_signatureGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    NSUInteger cursor = [self cursor];
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), cursor);
    _server.client.signatureHelp(Std(uri), pos,
        [weakSelf, gen, uri, cursor](const Json &result, const Json &error) {
            (void)error;
            [weakSelf signatureArrived:result gen:gen uri:uri cursor:cursor];
        }, trigger ? Std(trigger) : "", _signature.visible);
}

- (void)signatureArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri
                  cursor:(NSUInteger)cursor {
    if (gen != _signatureGen || ![uri isEqualToString:_uri]) return;
    Lsp::SignatureHelp help = Lsp::parseSignatureHelp(result);
    if (help.signatures.empty() || _tv.window.firstResponder != _tv ||
        cursor > _tv.string.length) {
        [self closeSignature];
        return;
    }
    if (!_signature) _signature = [MCSignaturePopup new];
    NSRect caret = [_tv firstRectForCharacterRange:NSMakeRange(cursor, 0) actualRange:NULL];
    [_signature show:MCSignatureText(help) atScreenRect:caret parent:_tv.window];
}

- (void)closeSignature {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(updateSignature)
                                               object:nil];
    _signatureGen++;
    [_signature close];
}

- (NSString *)signatureText { return _signature.visible ? _signature.text : nil; }

// ---- go to definition
- (void)goToDefinition {
    if (!_server) {
        NSBeep();
        [self flash:_statusText.length ? _statusText : @"No language server for this file"];
        return;
    }
    [self definitionAtIndex:_tv.selectedRange.location];
}

- (void)definitionAtIndex:(NSUInteger)index {
    if (!_server || !_uri) return;
    [self flushChanges];
    int gen = ++_definitionGen;
    _definitionSelection = _tv.selectedRange;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    _server.client.definition(Std(uri), pos, [weakSelf, gen, uri](const Json &r, const Json &e) {
        (void)e;
        [weakSelf definitionArrived:r gen:gen uri:uri];
    });
}

- (void)definitionArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri {
    // A late answer jumps only if the user is still where they asked: the
    // same document, never left since (detachDocument bumps the generation,
    // so A, B and back to A drops it), and the selection unmoved.
    if (gen != _definitionGen || ![uri isEqualToString:_uri] ||
        !NSEqualRanges(_tv.selectedRange, _definitionSelection))
        return;
    std::vector<Lsp::Location> locs = Lsp::parseLocations(result);
    if (locs.empty()) { NSBeep(); [self flash:@"No definition found"]; return; }
    Lsp::Location loc = locs[0];
    NSString *target = NS(Lsp::pathFromUri(loc.uri));
    if (!target.length) return;
    if (![self uri:loc.uri isSameFileAs:_uri]) {
        // Open it under the tree's own spelling of the path when it is inside
        // the folder, so the tree can select it.
        NSString *real = RealPath(target), *rootReal = RealPath(_root);
        NSString *path = target;
        if (rootReal.length && [real hasPrefix:[rootReal stringByAppendingString:@"/"]])
            path = [_root stringByAppendingPathComponent:
                        [real substringFromIndex:rootReal.length + 1]];
        if (!self.openFile || !self.openFile(path)) return;
    }
    std::u16string text = U16Of(_tv.string);
    size_t a = Lsp::offsetForPosition(text, loc.range.start);
    size_t b = Lsp::offsetForPosition(text, loc.range.end);
    NSRange r = NSMakeRange(a, b >= a ? b - a : 0);
    [_tv.window makeFirstResponder:_tv];
    [_tv setSelectedRange:r];
    [_tv scrollRangeToVisible:r];
    [_tv showFindIndicatorForRange:r];
}

// ---- hover
- (void)showHoverInfo {
    if (!_server || !_uri) {
        NSBeep();
        [self flash:_statusText.length ? _statusText : @"No language server for this file"];
        return;
    }
    [self flushChanges];
    NSUInteger index = _tv.selectedRange.location;
    int gen = ++_hoverGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    _server.client.hover(Std(uri), pos, [weakSelf, gen, uri, index](const Json &r, const Json &e) {
        (void)e;
        [weakSelf hoverArrived:r gen:gen uri:uri index:index];
    });
}

- (void)hoverArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri
               index:(NSUInteger)index {
    if (gen != _hoverGen || ![uri isEqualToString:_uri]) return;
    NSString *text = NS(Lsp::parseHover(result));
    NSString *diag = [_tv diagnosticMessageAtIndex:index];
    if (diag) text = text.length ? [NSString stringWithFormat:@"%@\n\n%@", diag, text] : diag;
    _lastHoverText = text;
    if (!text.length) { [self flash:@"No information here"]; return; }

    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    label.textColor = LHex(0xD4D4D4);
    label.selectable = YES;
    NSSize fit = [label sizeThatFits:NSMakeSize(520, CGFLOAT_MAX)];
    fit.width = MIN(ceil(fit.width), 520);
    fit.height = MIN(ceil(fit.height), 360);
    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, fit.width + 20, fit.height + 16)];
    label.frame = NSMakeRect(10, 8, fit.width, fit.height);
    [box addSubview:label];
    NSViewController *vc = [NSViewController new];
    vc.view = box;

    [_hover close];
    _hover = [NSPopover new];
    _hover.behavior = NSPopoverBehaviorTransient;
    _hover.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _hover.contentViewController = vc;
    NSRect screen = [_tv firstRectForCharacterRange:NSMakeRange(index, 0) actualRange:NULL];
    NSRect inWindow = [_tv.window convertRectFromScreen:screen];
    NSRect inView = [_tv convertRect:inWindow fromView:nil];
    inView.size.width = MAX(inView.size.width, 1);
    [_hover showRelativeToRect:inView ofView:_tv preferredEdge:NSRectEdgeMaxY];
}
@end
