// Lsp.mm — Objective-C++. See Lsp.h. Server processes (MCLspServer), the
// editor's text view (CodeTextView), the completion list and its
// documentation (MCCompletionPopup), the signature hint (MCSignaturePopup)
// and the per-window glue (LspSession).
#import "Lsp.h"
#import "EditorFont.h"
#import "AppSettings.h"
#import "Terminal.h"   // MCTerminalCommandDidFinishNotification
#import "MultiCaretView.h"
#import "LspPicker.h"
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

// A file URI with its path resolved, so one file named two ways (/tmp and
// /private/tmp) is one file; anything else as it was.
static std::string CanonicalURI(const std::string &uri) {
    NSString *path = NS(Lsp::pathFromUri(uri));
    if (!path.length) return uri;
    return Lsp::uriFromPath(Std(RealPath(path)));
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
    NSTrackingArea *_hoverArea;
    MCMultiCarets *_multi;
    NSEvent *_keyEvent;   // the key being interpreted, during keyDown:
}

- (MCMultiCarets *)multiCarets { return _multi; }
- (BOOL)multiCaretActive { return _multi.active; }

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _multi = [[MCMultiCarets alloc] initWithTextView:self];
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
    self.needsDisplay = YES;
    self.enclosingScrollView.verticalRulerView.needsDisplay = YES;   // the gutter's dots
}

- (BOOL)resignFirstResponder {
    BOOL ok = [super resignFirstResponder];
    if (ok && _multi.active) self.needsDisplay = YES;   // carets dimmed
    if (ok && self.onFocusLost) self.onFocusLost();
    if (ok && self.onBlur) self.onBlur();
    return ok;
}

// Keep the marks on their text while it is edited, until the server sends
// fresh ones: shift those after the edit, stretch those it touches.
- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    // A change made by anyone but the carets themselves (undo, a reload, a
    // formatter, a tab switch) leaves them pointing at the wrong text.
    if ((ts.editedMask & NSTextStorageEditedCharacters) && _multi.active && !_multi.applying)
        [_multi clear];
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

NSColor *MCDiagnosticColor(NSInteger severity) { return SeverityColor(severity); }

- (void)drawRect:(NSRect)dirty {
    [super drawRect:dirty];
    [_multi drawInRect:dirty];
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
}

// ---- the pointer, for hover info
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_hoverArea) [self removeTrackingArea:_hoverArea];
    _hoverArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:nil];
    [self addTrackingArea:_hoverArea];
}

- (NSUInteger)characterIndexUnderPoint:(NSPoint)point {
    NSString *text = self.string;
    NSUInteger n = text.length;
    if (n == 0) return NSNotFound;
    NSUInteger i = [self characterIndexForInsertionAtPoint:point];
    // The insertion point is before the character whose left half holds
    // the point, after it for its right half; either way it is one of these
    // two, and only if the point is inside its box (past a line's end the
    // nearest insertion point is the line's end).
    for (NSInteger k = (NSInteger)MIN(i, n - 1); k >= 0 && k >= (NSInteger)i - 1; k--) {
        unichar c = [text characterAtIndex:(NSUInteger)k];
        if (c == '\n' || c == '\r') continue;
        __block BOOL inside = NO;
        [self enumerateRectsForRange:NSMakeRange((NSUInteger)k, 1) block:^(NSRect rect) {
            if (NSPointInRect(point, rect)) inside = YES;
        }];
        if (inside) return (NSUInteger)k;
    }
    return NSNotFound;
}

- (void)mouseMoved:(NSEvent *)event {
    [super mouseMoved:event];
    if (!self.onMouseHover) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    self.onMouseHover([self characterIndexUnderPoint:p]);
}

- (void)mouseExited:(NSEvent *)event {
    [super mouseExited:event];
    if (self.onMouseHover) self.onMouseHover(NSNotFound);
}

- (void)scrollWheel:(NSEvent *)event {
    if (self.onScrolled) self.onScrolled();
    [super scrollWheel:event];
}

// ---- several carets: everything that types, moves or sets the selection
// goes to MCMultiCarets first while it has two or more.
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    if ([_multi insertText:string]) return;
    [super insertText:string replacementRange:replacementRange];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    if (_multi.active) [_multi clear];   // an input method composes at one place
    [super setMarkedText:string selectedRange:sel replacementRange:rep];
}

- (void)keyDown:(NSEvent *)event {
    // Cmd+Home / End (Fn+Cmd+Left / Right on a laptop) go to the start or
    // end of the document, with Shift selecting: Cmd+Up / Down move lines.
    const NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
         NSEventModifierFlagControl);
    if ((event.keyCode == 115 || event.keyCode == 119) &&
        (mods == NSEventModifierFlagCommand ||
         mods == (NSEventModifierFlagCommand | NSEventModifierFlagShift))) {
        const BOOL home = event.keyCode == 115, extend = (mods & NSEventModifierFlagShift) != 0;
        [self doCommandBySelector:home ? (extend ? @selector(moveToBeginningOfDocumentAndModifySelection:)
                                                 : @selector(moveToBeginningOfDocument:))
                                       : (extend ? @selector(moveToEndOfDocumentAndModifySelection:)
                                                 : @selector(moveToEndOfDocument:))];
        return;
    }
    // Option+Up / Down move the lines too, as in VS Code on the Mac, with or
    // without a selection (AppKit would move the caret by paragraph). Taken
    // by key code here, since AppKit sends two selectors for each.
    if ((event.keyCode == 125 || event.keyCode == 126) && mods == NSEventModifierFlagOption &&
        self.editable) {
        [_multi moveLinesDown:event.keyCode == 125];
        return;
    }
    NSEvent *outer = _keyEvent;
    _keyEvent = event;
    [super keyDown:event];
    _keyEvent = outer;
}

- (void)doCommandBySelector:(SEL)selector {
    // Cmd+Shift+Up/Down (select to the top or end of the file in AppKit) and
    // Shift+Option+Up/Down (select by paragraph) copy the lines instead, as
    // in VS Code. Only from the arrow keys: Shift+Home and Shift+End send the
    // same selectors and keep their meaning.
    NSEvent *ev = _keyEvent;
    if (self.editable && ev &&
        (ev.keyCode == 125 || ev.keyCode == 126) &&
        (selector == @selector(moveToEndOfDocumentAndModifySelection:) ||
         selector == @selector(moveToBeginningOfDocumentAndModifySelection:) ||
         selector == @selector(moveParagraphForwardAndModifySelection:) ||
         selector == @selector(moveParagraphBackwardAndModifySelection:))) {
        [_multi duplicateLinesDown:ev.keyCode == 125];
        return;
    }
    // Cmd+Down / Up move the lines, as the user expects; the system's move
    // to the end or start of the document is Cmd+End / Home (keyDown:).
    if (self.editable && ev && (ev.keyCode == 125 || ev.keyCode == 126) &&
        (selector == @selector(moveToEndOfDocument:) ||
         selector == @selector(moveToBeginningOfDocument:))) {
        [_multi moveLinesDown:ev.keyCode == 125];
        return;
    }
    if ([_multi doCommand:selector]) return;
    [super doCommandBySelector:selector];
}

- (void)copy:(id)sender {
    if (![_multi copy]) [super copy:sender];
}

- (void)cut:(id)sender {
    if (![_multi cut]) [super cut:sender];
}

- (void)paste:(id)sender {
    if (![_multi paste]) [super paste:sender];
}

// VS Code's multi-cursor keys, from the Edit menu (MultiCaretView.h).
- (IBAction)addNextOccurrence:(id)sender {
    if (!self.editable || ![_multi addNextOccurrence]) NSBeep();
}

- (IBAction)selectAllOccurrences:(id)sender {
    if (!self.editable || ![_multi selectAllOccurrences]) NSBeep();
}

- (IBAction)addCursorAbove:(id)sender {
    if (!self.editable || ![_multi addCursorVertically:-1]) NSBeep();
}

- (IBAction)addCursorBelow:(id)sender {
    if (!self.editable || ![_multi addCursorVertically:1]) NSBeep();
}

// Cmd+L and Ctrl+Shift+K, as in VS Code.
- (IBAction)expandLineSelection:(id)sender {
    if (!self.editable) { NSBeep(); return; }
    [_multi expandLineSelection];
}

- (IBAction)deleteLine:(id)sender {
    if (!self.editable) { NSBeep(); return; }
    [_multi deleteLines];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    SEL a = item.action;
    if (a == @selector(addNextOccurrence:) || a == @selector(selectAllOccurrences:) ||
        a == @selector(addCursorAbove:) || a == @selector(addCursorBelow:) ||
        a == @selector(expandLineSelection:) || a == @selector(deleteLine:))
        return self.editable;
    // With nothing selected, Copy and Cut take the line (MultiCaretView.h).
    if ((a == @selector(copy:) || a == @selector(cut:)) && self.editable) return YES;
    return [super validateMenuItem:item];
}

// ---- editor.ruler: drawn with the background, so the text covers it.
- (void)setRulerColumns:(NSArray<NSNumber *> *)columns {
    _rulerColumns = [columns copy];
    self.needsDisplay = YES;
}

- (void)setRulerColor:(NSColor *)color {
    _rulerColor = color;
    self.needsDisplay = YES;
}

// ---- bracket matching: the boxes are drawn with the background too.
- (void)setBracketRanges:(NSArray<NSValue *> *)ranges {
    if ([ranges isEqualToArray:_bracketRanges ?: @[]] ||
        (ranges.count == 0 && _bracketRanges.count == 0))
        return;
    for (NSValue *v in _bracketRanges) [self invalidateBracketBox:v.rangeValue];
    _bracketRanges = [ranges copy];
    for (NSValue *v in _bracketRanges) [self invalidateBracketBox:v.rangeValue];
}

- (void)setBracketMatchColor:(NSColor *)color {
    _bracketMatchColor = color;
    for (NSValue *v in _bracketRanges) [self invalidateBracketBox:v.rangeValue];
}

- (void)invalidateBracketBox:(NSRange)r {
    if (NSMaxRange(r) > self.textStorage.length) return;
    [self enumerateRectsForRange:r block:^(NSRect rect) {
        [self setNeedsDisplayInRect:NSInsetRect(rect, -2, -2)];
    }];
}

- (void)drawBracketBoxesInRect:(NSRect)dirty {
    if (_bracketRanges.count == 0 || !_bracketMatchColor) return;
    const NSUInteger n = self.textStorage.length;
    NSColor *fill = [_bracketMatchColor colorWithAlphaComponent:_bracketMatchColor.alphaComponent * 0.25];
    for (NSValue *v in _bracketRanges) {
        const NSRange r = v.rangeValue;
        if (NSMaxRange(r) > n) continue;
        [self enumerateRectsForRange:r block:^(NSRect rect) {
            if (!NSIntersectsRect(rect, dirty)) return;
            // The glyph's box, a little shorter than its line fragment,
            // which holds the line spacing below the text.
            NSRect box = NSInsetRect([self backingAlignedRect:rect
                                                      options:NSAlignAllEdgesNearest], 0.5, 0.5);
            box.size.height = MAX(4, NSHeight(box) - 2);
            [fill set];
            NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
            [self->_bracketMatchColor set];
            NSBezierPath *p = [NSBezierPath bezierPathWithRect:box];
            p.lineWidth = 1;
            [p stroke];
        }];
    }
}

- (void)drawViewBackgroundInRect:(NSRect)rect {
    [super drawViewBackgroundInRect:rect];
    if (self.editable) [self drawBracketBoxesInRect:rect];
    if (!self.editable || _rulerColumns.count == 0 || !_rulerColor) return;
    NSFont *font = self.typingAttributes[NSFontAttributeName];
    if (!font.fixedPitch) font = MCEditorFont();
    const CGFloat cw = [@"0" sizeWithAttributes:@{NSFontAttributeName: font}].width;
    const CGFloat x0 = self.textContainerOrigin.x + self.textContainer.lineFragmentPadding;
    [_rulerColor set];
    for (NSNumber *col in _rulerColumns) {
        NSRect line = [self backingAlignedRect:NSMakeRect(x0 + col.doubleValue * cw,
                                                          NSMinY(rect), 1, NSHeight(rect))
                                       options:NSAlignAllEdgesNearest];
        if (NSMaxX(line) > NSWidth(self.bounds) || !NSIntersectsRect(line, rect)) continue;
        NSRectFillUsingOperation(line, NSCompositingOperationSourceOver);
    }
}

- (void)setSelectedRanges:(NSArray<NSValue *> *)ranges
                 affinity:(NSSelectionAffinity)affinity
           stillSelecting:(BOOL)still {
    if (_multi.active && !_multi.applying) [_multi clear];
    [super setSelectedRanges:ranges affinity:affinity stillSelecting:still];
}

- (BOOL)shouldDrawInsertionPoint {
    return _multi.active ? NO : [super shouldDrawInsertionPoint];
}

- (BOOL)becomeFirstResponder {
    BOOL ok = [super becomeFirstResponder];
    if (_multi.active) self.needsDisplay = YES;   // carets back to full color
    return ok;
}

- (void)mouseDown:(NSEvent *)event {
    if ([_multi mouseDown:event]) return;
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
    size = round(size * MCEditorScale() * 2) / 2;   // follows the editor's zoom
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
// Sizes at the editor's 13 points, scaled with its font (editor.font-size
// and the zoom), as the list's text is.
static CGFloat Zoomed(CGFloat size) { return round(size * MCEditorScale()); }
static CGFloat RowHeight(void) { return Zoomed(20); }
static CGFloat PopupWidth(void) { return Zoomed(420); }
static CGFloat DocWidth(void) { return Zoomed(400); }
static CGFloat DocMaxHeight(void) { return Zoomed(320); }
static const NSInteger kMaxRows = 10;

- (void)build {
    _panel = MakePopupPanel(NSMakeRect(0, 0, PopupWidth(), 100));
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
    col.width = PopupWidth() - 4;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = RowHeight();
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
    _docPanel = MakePopupPanel(NSMakeRect(0, 0, DocWidth(), 100));
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
    NSFont *mono = [NSFont monospacedSystemFontOfSize:MCEditorFontSize() - 1
                                               weight:NSFontWeightRegular];
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
    // The zoom may have changed since the list was last shown.
    _table.rowHeight = RowHeight();
    _table.tableColumns.firstObject.width = PopupWidth() - 4;
    BOOL wasVisible = _visible;
    _visible = NO;   // no selection callbacks while the rows are replaced
    [_table reloadData];
    NSInteger row = select >= 0 && select < (NSInteger)labels.count ? select : 0;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];

    CGFloat h = MIN((NSInteger)labels.count, kMaxRows) * RowHeight() + 2;
    NSRect frame = NSMakeRect(caret.origin.x - 6, caret.origin.y - h - 2, PopupWidth(), h);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    if (NSMinY(frame) < NSMinY(vis)) frame.origin.y = NSMaxY(caret) + 2;   // above
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - PopupWidth();
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
    [_docPanel setContentSize:NSMakeSize(DocWidth(), DocMaxHeight())];
    [_docView.textStorage setAttributedString:doc];
    NSLayoutManager *lm = _docView.layoutManager;
    [lm ensureLayoutForTextContainer:_docView.textContainer];
    CGFloat h = ceil([lm usedRectForTextContainer:_docView.textContainer].size.height) +
                2 * _docView.textContainerInset.height + 2;
    h = MIN(MAX(h, RowHeight() + 2), DocMaxHeight());
    NSRect f = _docPanel.frame;
    f.size = NSMakeSize(DocWidth(), h);
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

// ----------------------------------------------------------- MCHoverPopup
// Hover info: what the server says about the symbol under the pointer (or
// the caret, for Cmd+I), with any diagnostic there first. Above its line, as
// in VS Code, below when there is no room. The pointer may move into it to
// scroll a long docstring; it says when the pointer leaves.
@interface MCHoverView : NSView
@property(nonatomic, copy) void (^onExit)(void);
@end
@implementation MCHoverView {
    NSTrackingArea *_area;
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:self userInfo:nil];
    [self addTrackingArea:_area];
}
- (void)mouseExited:(NSEvent *)event {
    (void)event;
    if (self.onExit) self.onExit();
}
@end

@interface MCHoverPopup : NSObject
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, copy) void (^onExit)(void);
- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)line parent:(NSWindow *)parent;
- (void)close;
- (NSRect)frame;
@end

@implementation MCHoverPopup {
    MCCompletionPanel *_panel;
    NSScrollView *_scroll;
    NSTextView *_view;
    __weak NSWindow *_parent;
}
static const CGFloat kHoverMinWidth = 60;
#define kHoverMaxWidth Zoomed(520)
#define kHoverMaxHeight Zoomed(320)

- (void)build {
    _panel = MakePopupPanel(NSMakeRect(0, 0, kHoverMaxWidth, 100));
    MCHoverView *box = [[MCHoverView alloc] initWithFrame:_panel.contentView.bounds];
    __weak MCHoverPopup *weakSelf = self;
    box.onExit = ^{
        MCHoverPopup *p = weakSelf;
        if (p && p.onExit) p.onExit();
    };
    _panel.contentView = box;
    _scroll = [[NSScrollView alloc] initWithFrame:box.bounds];
    _scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.drawsBackground = NO;
    _scroll.wantsLayer = YES;
    _scroll.layer.borderWidth = 1;
    _scroll.layer.borderColor = LHex(0x454545).CGColor;
    _view = [[NSTextView alloc] initWithFrame:_scroll.bounds];
    _view.editable = NO;
    _view.selectable = NO;
    _view.drawsBackground = NO;
    _view.textContainerInset = NSMakeSize(8, 6);
    _view.verticallyResizable = YES;
    _view.horizontallyResizable = NO;
    _view.autoresizingMask = NSViewWidthSizable;
    _view.textContainer.widthTracksTextView = YES;
    _scroll.documentView = _view;
    [box addSubview:_scroll];
}

- (NSRect)frame { return _visible ? _panel.frame : NSZeroRect; }

- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)line parent:(NSWindow *)parent {
    if (!_panel) [self build];
    // Laid out at the widest first, then narrowed to the longest line, so a
    // one-line answer gets a small box.
    [_panel setContentSize:NSMakeSize(kHoverMaxWidth, kHoverMaxHeight)];
    [_view.textStorage setAttributedString:text];
    NSLayoutManager *lm = _view.layoutManager;
    [lm ensureLayoutForTextContainer:_view.textContainer];
    NSSize used = [lm usedRectForTextContainer:_view.textContainer].size;
    NSSize inset = _view.textContainerInset;
    CGFloat pad = _view.textContainer.lineFragmentPadding;
    CGFloat w = MIN(MAX(ceil(used.width) + 2 * (inset.width + pad) + 2, kHoverMinWidth),
                    kHoverMaxWidth);
    if (w < kHoverMaxWidth) {
        [_panel setContentSize:NSMakeSize(w, kHoverMaxHeight)];
        [lm ensureLayoutForTextContainer:_view.textContainer];
        used = [lm usedRectForTextContainer:_view.textContainer].size;
    }
    CGFloat h = MIN(ceil(used.height) + 2 * inset.height + 2, kHoverMaxHeight);
    NSRect frame = NSMakeRect(line.origin.x - 8, NSMaxY(line) + 2, w, h);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    if (NSMaxY(frame) > NSMaxY(vis)) frame.origin.y = line.origin.y - h - 2;   // below
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - w;
    if (NSMinX(frame) < NSMinX(vis)) frame.origin.x = NSMinX(vis);
    [_panel setFrame:frame display:YES];
    [_view scrollRangeToVisible:NSMakeRange(0, 0)];
    if (!_visible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
}

- (void)close {
    if (!_visible) return;
    _visible = NO;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
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

// A file open in a tab of this window, open on its language's server so the
// server sees every tab's unsaved text (rename, references), not only the
// one on show. Keyed by its canonical URI.
@interface MCLspDoc : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, strong) MCLspServer *server;
@end
@implementation MCLspDoc
@end

// A document's diagnostics as last published, with the text the server had
// been sent when it computed them, so they are drawn on the text as it is
// when they are shown (typed in since, perhaps) rather than as it was.
struct MCDiagSet {
    std::vector<Lsp::Diagnostic> list;
    std::shared_ptr<const std::u16string> base;   // null: not known
    // For a file no tab has open: the server that reported on it, compared
    // only (never followed), so its reports go when it does.
    const void *server = nullptr;
};

@implementation MCProblem
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
    std::map<std::string, MCDiagSet> _diags;   // by canonical URI
    // What servers publish for files no tab has open (a server checking the
    // whole folder): listed in Problems only.
    std::map<std::string, MCDiagSet> _otherDiags;
    // F8: the mark the last step went to, in the marks it was one of.
    NSArray<MCDiagnosticMark *> *_stepMarks;
    long _stepIndex;
    NSMutableDictionary<NSString *, MCLspDoc *> *_docs;   // by canonical URI
    NSUInteger _edits;        // counts edits to the text on show
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
    MCHoverPopup *_hover;
    BOOL _hoverByMouse;       // shown for the pointer (else for Cmd+I)
    NSRange _hoverRange;      // the text it is about
    NSUInteger _pointerIndex; // the character under the pointer, or NSNotFound
    MCSignaturePopup *_signature;

    // Rename, references and quick fixes.
    MCPickerPopup *_picker;
    NSString *_pickerKind;                    // "references", "fixes", or nil
    std::vector<Lsp::Location> _refRows;      // per row; a heading's has no uri
    std::vector<Lsp::CodeAction> _actions;    // per row of the quick fix list
    MCLspServer *_actionServer;               // the server the actions came from
    NSRange _askedSelection;                  // the selection when one was asked for
    int _referencesGen, _renameGen, _actionGen;
    MCRenameBox *_rename;
    NSRange _renameRange;                     // the name being renamed
    NSString *_renameText;                    // the text when the field opened
    BOOL _applyingEdit;                       // a WorkspaceEdit is going into the text
    NSString *_commandTitle;                  // the quick fix whose command is running
}

- (instancetype)initWithTextView:(CodeTextView *)textView root:(NSString *)root {
    if (!(self = [super init])) return nil;
    _tv = textView;
    _root = [root copy];
    _entries = [NSMutableDictionary dictionary];
    _docs = [NSMutableDictionary dictionary];
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
        [weakSelf closeHoverUnlessPointer];
        [weakSelf closePicker];
    };
    _pointerIndex = NSNotFound;
    textView.onMouseHover = ^(NSUInteger index) { [weakSelf pointerAt:index]; };
    textView.onScrolled = ^{ [weakSelf closeHover]; };
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
                for (const Lsp::Diagnostic &d : it->second.list) {
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
        MCLspServer *s = weakServer;
        if (s && !s.stopping)
            [weakSelf diagnosticsArrived:uri list:list base:s.client.textFor(uri) server:s];
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
    // An edit the server makes itself, usually while running a quick fix's
    // command. Applied once the reply has been read, never inside it.
    c.onApplyEdit = [weakSelf, weakServer](const Json &params,
                                           Lsp::Client::ApplyEditReply reply) {
        MCLspServer *s = weakServer;
        if (!s || s.stopping) { reply(false, "the server is stopping"); return; }
        Json p = params;
        dispatch_async(dispatch_get_main_queue(), ^{
            (void)s;   // keeps the client the reply goes through alive
            LspSession *me = weakSelf;
            if (!me) { reply(false, "the window closed"); return; }
            NSString *failure = nil;
            NSString *label = NS(p["label"].asString());
            NSInteger n = [me applyWorkspaceEdit:Lsp::parseWorkspaceEdit(p["edit"], CanonicalURI)
                                      actionName:label.length ? label
                                                 : me->_commandTitle ?: @"Edit"
                                         failure:&failure];
            if (n < 0) [me note:failure];
            reply(n >= 0, n < 0 ? Std(failure) : "");
        });
    };
    c.onProtocolError = [weakServer](const std::string &problem) {
        NSLog(@"MiniCode: %@: %s", weakServer.name, problem.c_str());
    };
    server.onExit = ^(MCLspServer *s) { [weakSelf serverExited:s]; };
    // The folder as the server sees it is spelled as didOpen spells files
    // (resolved), or it takes /tmp/x/a.py and /private/tmp/x/a.py for two
    // files and finds every reference twice. Paths it sends back are mapped
    // to the tree's spelling (treePathForURI:).
    c.initialize(Std(RealPath(_root) ?: _root), (int)getpid());
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
    [self forgetDocumentsOf:server];
    [server stop];   // its client has already exited, so this just ends its input
    [self changed];
}

- (void)serverExited:(MCLspServer *)server {
    if (_server == server) [self detachDocument];
    [self forgetDocumentsOf:server];
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
    [self forgetDocumentsOf:s];
    e.server = nil;
    [s stop];
}

- (void)stopAllServers {
    for (MCLspEntry *e in _entries.allValues) [self stopEntry:e];
    [_entries removeAllObjects];
    [_docs removeAllObjects];
    _diags.clear();
    _otherDiags.clear();
    [self problemsChanged];
}

// A server going away takes its documents with it: nothing to close, and
// what it said about them no longer holds. A restart opens them again.
- (void)forgetDocumentsOf:(MCLspServer *)server {
    for (NSString *uri in _docs.allKeys) {
        if (_docs[uri].server != server) continue;
        [_docs removeObjectForKey:uri];
        _diags.erase(Std(uri));
    }
    const void *gone = (__bridge const void *)server;
    for (auto it = _otherDiags.begin(); it != _otherDiags.end();)
        it = it->second.server == gone ? _otherDiags.erase(it) : std::next(it);
    [self problemsChanged];
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
    [self closeHover];
    [self closePicker];
    if (!_applyingEdit) [_rename close];
    // Answers still on their way were for the document leaving the view.
    _completionGen++;
    _definitionGen++;
    _hoverGen++;
    _referencesGen++;
    _actionGen++;
    if (!_applyingEdit) _renameGen++;
    // The document stays open on its server while its tab is open (see
    // syncOpenDocuments), and so do its diagnostics, for when it is back.
    _server = nil;
    _uri = nil;
    _tv.diagnostics = @[];
}

// Open the current file on its language's server, starting it if need be,
// and with it every other tab's file that server handles.
- (void)attachDocument {
    if (!_server && _path && _langKey && [AppSettings shared].settings.lspEnabled()) {
        MCLspServer *server = [self ensureServer:_langKey];
        if (server) {
            _server = server;
            _uri = NS(Lsp::uriFromPath(Std(RealPath(_path))));
        }
    }
    [self syncOpenDocuments];
    if (_server && _uri) [self paintDiagnostics];
}

// Leaves the file on its server: the tab is still open. syncOpenDocuments
// closes it there once its tab goes.
- (void)closeDocument {
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
    } else {
        [self syncOpenDocuments];   // a tab that closed or stopped showing text
    }
    [self changed];
}

- (void)tabsChanged {
    [self syncOpenDocuments];
}

// Every tab's file stays open on its language's server, with the tab's text,
// so rename and references see unsaved changes in tabs not on show, and a
// cross-file rename's edits (unsaved) are what the server reads, not the
// disk. The file on show is the text view's; the others come from
// bufferText. Only servers already running (or starting) for a file on show
// take files from background tabs: a tab never starts a server unseen.
// Files whose tabs have gone are closed. Cheap when nothing changed: an
// unchanged text sends nothing (Client::didChange compares).
- (void)syncOpenDocuments {
    NSMutableDictionary<NSString *, MCLspDoc *> *wanted = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSString *> *texts = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSString *> *languageIds = [NSMutableDictionary dictionary];
    if ([AppSettings shared].settings.lspEnabled()) {
        if (_server && _uri) {
            MCLspDoc *d = [MCLspDoc new];
            d.path = _path;
            d.server = _server;
            wanted[_uri] = d;
            texts[_uri] = _tv.string;
            languageIds[_uri] = _languageId;
        }
        NSArray<NSString *> *paths = self.openPaths ? self.openPaths() : @[];
        for (NSString *path in paths) {
            Lsp::Language lang;
            if (!Lsp::languageForExtension(Std(path.pathExtension.lowercaseString), lang))
                continue;
            MCLspServer *server = _entries[NS(lang.server)].server;
            if (!server || server.stopping) continue;
            NSString *uri = NS(Lsp::uriFromPath(Std(RealPath(path))));
            if (wanted[uri]) continue;   // the file on show
            NSString *text = self.bufferText ? self.bufferText(path) : nil;
            if (!text) continue;         // not read yet, or not shown as text
            MCLspDoc *d = [MCLspDoc new];
            d.path = path;
            d.server = server;
            wanted[uri] = d;
            texts[uri] = text;
            languageIds[uri] = NS(lang.languageId);
        }
    }
    // Closed first, so a file that moved to another server is closed on
    // the old one before the new one opens it.
    for (NSString *uri in _docs.allKeys) {
        MCLspDoc *had = _docs[uri];
        if (wanted[uri].server == had.server) continue;
        if (!had.server.stopping) had.server.client.didClose(Std(uri));
        [_docs removeObjectForKey:uri];
        _diags.erase(Std(uri));
        [self problemsChanged];
    }
    for (NSString *uri in wanted) {
        MCLspDoc *d = wanted[uri];
        // didOpen of an open document is a didChange (and nothing at all
        // when the text is what the server has).
        d.server.client.didOpen(Std(uri), Std(languageIds[uri]), Std(texts[uri]));
        _docs[uri] = d;
    }
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
    // Typing closes hover info and the lists; recoloring alone does not.
    if (ts.editedMask & NSTextStorageEditedCharacters) {
        _edits++;
        [self closeHover];
        [self closePicker];
    }
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || !_server || !_uri) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    [self performSelector:@selector(flushChanges) withObject:nil afterDelay:0.3];
    // A rename or a fix going in is not typing: no completion or signature.
    if (_applyingEdit) return;

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
    [self closeHover];
    [self closePicker];
}

- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    [self closeHover];
    if (!_applyingEdit) [self closePicker];
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

// `base` is the text the server had been sent for the file when it
// published (Client::textFor), or null when it does not have it open.
- (void)diagnosticsArrived:(const std::string &)uri
                      list:(const std::vector<Lsp::Diagnostic> &)list
                      base:(const std::string *)base
                    server:(MCLspServer *)server {
    std::string key = CanonicalURI(uri);
    if (!base && !_docs[NS(key)]) {
        // A file no tab has open (a server reporting on the whole folder):
        // only listed in Problems, and opening it brings fresh ones.
        if (list.empty()) {
            if (!_otherDiags.erase(key)) return;
        } else {
            MCDiagSet set;
            set.list = list;
            set.server = (__bridge const void *)server;
            _otherDiags[key] = std::move(set);
        }
        [self problemsChanged];
        return;
    }
    _otherDiags.erase(key);
    MCDiagSet set;
    set.list = list;
    if (base) set.base = std::make_shared<const std::u16string>(Lsp::toUtf16(*base));
    // Kept for another tab's file too, and drawn when that tab is shown.
    _diags[key] = std::move(set);
    if ([self uri:uri isSameFileAs:_uri]) [self paintDiagnostics];
    [self problemsChanged];
}

// ---- the Problems list
- (void)problemsChanged {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(reportProblems)
                                               object:nil];
    [self performSelector:@selector(reportProblems) withObject:nil afterDelay:0.05];
}

- (void)reportProblems {
    if (self.onProblemsChanged) self.onProblemsChanged();
}

// Every published diagnostic as an Lsp::Problem, placed in the text as it
// is now for files open in a tab (Lsp::diagnosticSpans, leaving out ones
// about text typed over since), as the server said for the others. Hints
// are left out, as VS Code leaves them out of its Problems: pyright sends
// one for every name not used yet, which would drown the rest.
- (std::vector<Lsp::Problem>)collectProblems {
    std::vector<Lsp::Problem> out;
    auto add = [&](NSString *path, const Lsp::Diagnostic &d, Lsp::Position a,
                   Lsp::Position b) {
        if (d.severity == Lsp::Severity::Hint) return;
        Lsp::Problem p;
        p.path = Std(path);
        p.line = a.line;
        p.column = a.character;
        p.endLine = b.line;
        p.endColumn = b.character;
        p.severity = d.severity;
        p.message = d.message;
        p.source = d.source;
        p.code = d.code;
        out.push_back(p);
    };
    for (auto &[key, set] : _diags) {
        NSString *uri = NS(key);
        BOOL onShow = _uri && [self uri:key isSameFileAs:_uri];
        // Listed under the tree's spelling of the path, as the files no tab
        // has are, so they sort together; the tab's own spelling finds its text.
        NSString *tabPath = onShow ? _path : _docs[uri].path;
        NSString *path = [self treePathForURI:key];
        if (!path.length) continue;
        NSString *text = onShow ? _tv.string
                       : tabPath && self.bufferText ? self.bufferText(tabPath) : nil;
        std::u16string now = text ? U16Of(text)
                                  : (set.base ? *set.base : std::u16string());
        for (const Lsp::DiagnosticSpan &sp :
             Lsp::diagnosticSpans(set.base.get(), now, set.list))
            add(path, set.list[sp.index], Lsp::positionForOffset(now, sp.start),
                Lsp::positionForOffset(now, sp.end));
    }
    for (auto &[key, set] : _otherDiags) {
        NSString *path = [self treePathForURI:key];
        if (!path.length) continue;
        for (const Lsp::Diagnostic &d : set.list) add(path, d, d.range.start, d.range.end);
    }
    return out;
}

- (NSArray<MCProblem *> *)problemRows {
    std::vector<Lsp::Problem> problems = [self collectProblems];
    NSMutableArray<MCProblem *> *rows = [NSMutableArray array];
    for (const Lsp::ProblemRow &r : Lsp::problemRows(problems)) {
        MCProblem *p = [MCProblem new];
        p.heading = r.heading;
        p.path = NS(r.path);
        if (r.heading) {
            p.errors = r.errors;
            p.warnings = r.warnings;
            p.others = r.others;
        } else {
            const Lsp::Problem &q = problems[r.problem];
            p.line = q.line;
            p.column = q.column;
            p.endLine = q.endLine;
            p.endColumn = q.endColumn;
            p.severity = (NSInteger)q.severity;
            p.fullMessage = NS(q.message);
            p.message = NS(Lsp::problemOneLine(q.message));
            p.source = NS(q.source);
            p.code = NS(q.code);
        }
        [rows addObject:p];
    }
    return rows;
}

- (Lsp::ProblemCounts)problemCounts {
    std::vector<Lsp::Problem> problems;
    for (auto *m : {&_diags, &_otherDiags})
        for (auto &[key, set] : *m)
            for (const Lsp::Diagnostic &d : set.list) {
                if (d.severity == Lsp::Severity::Hint) continue;
                Lsp::Problem p;
                p.path = key;
                p.severity = d.severity;
                problems.push_back(p);
            }
    return Lsp::countProblems(problems);
}

- (NSString *)problemCountsText { return NS(Lsp::problemCountsText([self problemCounts])); }
- (NSInteger)problemErrors { return [self problemCounts].errors; }
- (NSInteger)problemWarnings { return [self problemCounts].warnings; }

- (BOOL)problemsKnown {
    if (!_diags.empty() || !_otherDiags.empty()) return YES;
    for (MCLspEntry *e in _entries.allValues)
        if (e.server) return YES;
    return NO;
}

- (void)openProblem:(MCProblem *)problem {
    if (problem.heading || !problem.path.length) return;
    Lsp::Location loc;
    loc.uri = Lsp::uriFromPath(Std(RealPath(problem.path)));
    loc.range.start = Lsp::Position{(int)problem.line, (int)problem.column};
    loc.range.end = Lsp::Position{(int)problem.endLine, (int)problem.endColumn};
    [self revealLocation:loc];
}

- (void)goToProblem:(BOOL)forward {
    // The marks drawn now (they follow typing), hints left out as in the list.
    NSArray<MCDiagnosticMark *> *all = _tv.diagnostics;
    NSMutableArray<MCDiagnosticMark *> *marks = [NSMutableArray array];
    for (MCDiagnosticMark *m in all) if (m.severity != 4) [marks addObject:m];
    if (!_uri || marks.count == 0) {
        NSBeep();
        [self flash:@"No problems in this file"];
        return;
    }
    std::vector<std::pair<size_t, size_t>> spans;
    for (MCDiagnosticMark *m in marks) spans.push_back({m.range.location, NSMaxRange(m.range)});
    long last = all == _stepMarks ? _stepIndex : -1;
    long i = Lsp::stepProblem(spans, _tv.selectedRange.location, last, forward);
    if (i < 0) return;
    MCDiagnosticMark *m = marks[(NSUInteger)i];
    NSRange caret = NSMakeRange(m.range.location, 0);
    [_tv setSelectedRange:caret];   // closes what was showing
    [_tv scrollRangeToVisible:m.range];
    _stepMarks = all;
    _stepIndex = i;
    // Its message (and any other problem there) in the hover box at the
    // caret, which the next caret move closes.
    [self closeHover];
    [self showHover:Json() gen:_hoverGen uri:_uri index:caret.location byMouse:NO];
}

// The file on show's diagnostics, drawn on its text as it is now: those
// computed on text typed over since are left out until the server
// publishes for the new text (Lsp::diagnosticSpans).
- (void)paintDiagnostics {
    if (!_uri) return;
    auto it = _diags.find(Std(_uri));
    if (it == _diags.end()) {
        _tv.diagnostics = @[];
        [self updateStatus];
        return;
    }
    const std::vector<Lsp::Diagnostic> &list = it->second.list;
    std::u16string text = U16Of(_tv.string);
    NSMutableArray *marks = [NSMutableArray array];
    for (const Lsp::DiagnosticSpan &span :
         Lsp::diagnosticSpans(it->second.base.get(), text, list)) {
        const Lsp::Diagnostic &d = list[span.index];
        size_t a = span.start;
        size_t b = span.end;
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

// How long accepting an item waits for completionItem/resolve to bring its
// additional edits (an auto-import). Keys pressed meanwhile wait in the
// event queue and arrive after the completion, in order.
static const NSTimeInterval kResolveBeforeAccept = 0.4;

// completionItem/resolve for an item about to be accepted, waited for here,
// at most kResolveBeforeAccept. YES when the item may still be accepted: the
// server, the file and its text are as they were (the wait runs the main
// queue, so a server's workspace/applyEdit could have got in).
- (BOOL)resolveBeforeAccepting:(Lsp::CompletionItem &)item {
    MCLspServer *server = _server;
    if (!server || !Lsp::completionWantsResolve(item, server.client.canResolveCompletion()))
        return YES;
    struct Answer { bool done = false; Json result; };
    auto answer = std::make_shared<Answer>();
    int rid = server.client.resolveCompletion(item, [answer](const Json &r, const Json &e) {
        answer->done = true;
        if (e.isNull()) answer->result = r;
    });
    if (!rid) return YES;
    NSString *uri = _uri;
    const NSUInteger edits = _edits;
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:kResolveBeforeAccept];
    while (!answer->done && until.timeIntervalSinceNow > 0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
    if (!answer->done) {
        server.client.cancel(rid);   // too slow: the word goes in without it
        return _server == server && [_uri isEqualToString:uri] && _edits == edits;
    }
    Lsp::applyResolved(item, answer->result);
    return _server == server && [_uri isEqualToString:uri] && _edits == edits;
}

- (void)acceptCompletion:(NSInteger)row {
    if (row < 0 || (size_t)row >= _shown.size()) { [self closeCompletion]; return; }
    Lsp::CompletionItem item = _shown[(size_t)row];
    NSUInteger cursor = [self cursor];
    NSUInteger start = _anchor;
    [self closeCompletion];
    // Answers for the list still on their way must not bring it back.
    _completionGen++;
    if (![self resolveBeforeAccepting:item]) return;
    std::u16string text = U16Of(_tv.string);
    if (item.hasEdit) {
        // The edit's range was computed on the text as it was when asked;
        // everything before the word is unchanged since, so its start holds.
        size_t editStart = Lsp::offsetForPosition(text, item.editRange.start);
        if (editStart <= cursor) start = editStart;
    }
    // The word, and the edits that go with it (an import at the top, an
    // #include), as one replacement: one undo step, one change for the
    // highlighter and the server.
    Lsp::CompletionEdits ce = Lsp::completionEdits(text, start, cursor,
                                                   Lsp::toUtf16(item.textToInsert()),
                                                   item.additionalEdits);
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSMutableArray<NSString *> *strings = [NSMutableArray array];
    for (const Lsp::OffsetEdit &e : ce.edits) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(e.start, e.length)]];
        [strings addObject:[NSString stringWithCharacters:(const unichar *)e.text.data()
                                                   length:e.text.size()]];
    }
    [_tv breakUndoCoalescing];
    if (![_tv shouldChangeTextInRanges:ranges replacementStrings:strings]) return;
    NSTextStorage *ts = _tv.textStorage;
    [ts beginEditing];
    for (NSInteger i = (NSInteger)ranges.count - 1; i >= 0; i--)
        [ts replaceCharactersInRange:ranges[(NSUInteger)i].rangeValue
                          withString:strings[(NSUInteger)i]];
    [ts endEditing];
    [_tv didChangeText];
    [_tv breakUndoCoalescing];
    if (ce.caret <= _tv.string.length) [_tv setSelectedRange:NSMakeRange(ce.caret, 0)];
    [_tv scrollRangeToVisible:_tv.selectedRange];
}

- (BOOL)handleCommand:(SEL)sel {
    // The references or quick fix list: the arrows move through it, Return
    // takes a row, Esc closes it. Any other key goes to the editor, and the
    // list closes when the text or the caret changes.
    if (_picker.visible) {
        if (sel == @selector(moveUp:)) { [_picker moveSelection:-1]; return YES; }
        if (sel == @selector(moveDown:)) { [_picker moveSelection:1]; return YES; }
        if (sel == @selector(scrollPageUp:) || sel == @selector(pageUp:)) {
            [_picker moveSelection:-10];
            return YES;
        }
        if (sel == @selector(scrollPageDown:) || sel == @selector(pageDown:)) {
            [_picker moveSelection:10];
            return YES;
        }
        if (sel == @selector(insertNewline:)) {
            [self acceptPickerRow:_picker.selectedRow];
            return YES;
        }
        if (sel == @selector(cancelOperation:)) { [self closePicker]; return YES; }
    }
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
    if (_hover.visible && sel == @selector(cancelOperation:)) {
        [self closeHover];
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
    [self revealLocation:locs[0]];
}

// A file the server named, under the tree's own spelling of the path when it
// is inside the folder (the server may say /private/tmp for a /tmp folder),
// so the tree can select it. Nil when the URI is not a file.
- (NSString *)treePathForURI:(const std::string &)uri {
    NSString *target = NS(Lsp::pathFromUri(uri));
    if (!target.length) return nil;
    NSString *real = RealPath(target), *rootReal = RealPath(_root);
    if (rootReal.length && [real hasPrefix:[rootReal stringByAppendingString:@"/"]])
        return [_root stringByAppendingPathComponent:[real substringFromIndex:rootReal.length + 1]];
    return target;
}

// Open a location's file (go to definition, a reference) and select its range.
- (void)revealLocation:(const Lsp::Location &)loc {
    if (![self uri:loc.uri isSameFileAs:_uri]) {
        NSString *path = [self treePathForURI:loc.uri];
        if (!path.length || !self.openFile || !self.openFile(path)) return;
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
// Cmd+I: about the caret, until the caret moves or Esc.
- (void)showHoverInfo {
    if (!_server || !_uri) {
        NSBeep();
        [self flash:_statusText.length ? _statusText : @"No language server for this file"];
        return;
    }
    [self requestHoverAt:_tv.selectedRange.location byMouse:NO];
}

// The pointer rests on a character for a moment: ask about it, as VS Code
// does. Moving within what the box is about keeps it; moving off it closes
// it a moment later unless the pointer went into the box.
static const NSTimeInterval kHoverDelay = 0.4, kHoverGrace = 0.3;

- (void)pointerAt:(NSUInteger)index {
    _pointerIndex = index;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(pointerRested)
                                               object:nil];
    if (_hover.visible && _hoverByMouse && index != NSNotFound &&
        NSLocationInRange(index, _hoverRange))
        return;
    if (_hover.visible && _hoverByMouse) [self closeHoverSoon];
    if (index != NSNotFound && [self hoverWantedAt:index])
        [self performSelector:@selector(pointerRested) withObject:nil afterDelay:kHoverDelay];
}

- (BOOL)hoverWantedAt:(NSUInteger)index {
    if (!_tv.editable || index >= _tv.string.length) return NO;
    if (_popup.visible) return NO;   // the completion list has the stage
    unichar c = [_tv.string characterAtIndex:index];
    if ([[NSCharacterSet whitespaceAndNewlineCharacterSet] characterIsMember:c]) return NO;
    return (_server && _uri) || [_tv diagnosticMessageAtIndex:index] != nil;
}

- (BOOL)pointerInHover {
    return _hover.visible &&
           NSPointInRect([NSEvent mouseLocation], NSInsetRect(_hover.frame, -4, -4));
}

- (void)pointerRested {
    NSUInteger index = _pointerIndex;
    if (index == NSNotFound || ![self hoverWantedAt:index] || [self pointerInHover]) return;
    if (!_server || !_uri) {   // only a diagnostic to show
        [self showHover:Json() gen:_hoverGen uri:_uri index:index byMouse:YES];
        return;
    }
    [self requestHoverAt:index byMouse:YES];
}

- (void)requestHoverAt:(NSUInteger)index byMouse:(BOOL)byMouse {
    [self flushChanges];
    int gen = ++_hoverGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    _server.client.hover(Std(uri), pos, [weakSelf, gen, uri, index, byMouse](const Json &r,
                                                                              const Json &e) {
        (void)e;
        [weakSelf showHover:r gen:gen uri:uri index:index byMouse:byMouse];
    });
}

// The word around a character, for a server that names no range.
- (NSRange)wordAt:(NSUInteger)index {
    NSString *t = _tv.string;
    if (index >= t.length) return NSMakeRange(index, 0);
    if (!IsIdentChar([t characterAtIndex:index])) return NSMakeRange(index, 1);
    NSUInteger a = index, b = index + 1;
    while (a > 0 && IsIdentChar([t characterAtIndex:a - 1])) a--;
    while (b < t.length && IsIdentChar([t characterAtIndex:b])) b++;
    return NSMakeRange(a, b - a);
}

- (void)showHover:(const Json &)result gen:(int)gen uri:(NSString *)uri
            index:(NSUInteger)index byMouse:(BOOL)byMouse {
    if (gen != _hoverGen) return;
    if (uri != _uri && ![uri isEqualToString:_uri]) return;
    if (index > _tv.string.length) return;
    NSRange range = [self wordAt:index];
    Lsp::HoverDoc doc = Lsp::parseHoverDoc(result);
    if (doc.hasRange) {
        std::u16string text = U16Of(_tv.string);
        size_t a = Lsp::offsetForPosition(text, doc.range.start);
        size_t b = Lsp::offsetForPosition(text, doc.range.end);
        if (a <= index && index < b) range = NSMakeRange(a, b - a);
    }
    // The pointer moved on while the server thought.
    if (byMouse && (_pointerIndex == NSNotFound ||
                    !(NSLocationInRange(_pointerIndex, range) || _pointerIndex == index)))
        return;

    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSAttributedString *gap = [[NSAttributedString alloc] initWithString:@"\n\n"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:5]}];
    for (MCDiagnosticMark *m in _tv.diagnostics) {
        if (!(NSLocationInRange(index, m.range) ||
              (m.range.length == 0 && index == m.range.location)))
            continue;
        if (s.length) [s appendAttributedString:gap];
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:m.message ?: @""
                attributes:@{NSFontAttributeName: DocFont(NO, NO, NO, 12),
                             NSForegroundColorAttributeName: SeverityColor(m.severity)}]];
        range = NSUnionRange(range, m.range);
    }
    for (const Lsp::HoverDoc::Part &part : doc.parts) {
        NSAttributedString *t = MCDocText(NS(part.text), part.markdown);
        if (!t.length) continue;
        if (s.length) [s appendAttributedString:gap];
        [s appendAttributedString:t];
    }
    _lastHoverText = s.length ? [s.string copy] : @"";
    if (!s.length) {
        if (!byMouse) [self flash:@"No information here"];
        return;
    }

    // Anchored to the line the text starts on, at the text (or the caret).
    __block NSRect line = NSZeroRect;
    NSRange anchor = byMouse ? NSMakeRange(range.location, MAX(range.length, 1))
                             : NSMakeRange(index, 0);
    if (anchor.length) {
        [_tv enumerateRectsForRange:anchor block:^(NSRect r) {
            if (NSIsEmptyRect(line)) line = r;
        }];
    }
    if (NSIsEmptyRect(line)) {
        NSRect screen = [_tv firstRectForCharacterRange:NSMakeRange(index, 0) actualRange:NULL];
        line = [_tv.window convertRectFromScreen:screen];
    } else {
        line = [_tv convertRect:line toView:nil];
    }
    NSRect screenLine = [_tv.window convertRectToScreen:line];
    screenLine.size.width = MAX(screenLine.size.width, 1);

    if (!_hover) {
        _hover = [MCHoverPopup new];
        __weak LspSession *weakSelf = self;
        _hover.onExit = ^{ [weakSelf closeHoverSoon]; };
    }
    _hoverByMouse = byMouse;
    _hoverRange = range;
    [_hover show:s atScreenRect:screenLine parent:_tv.window];
}

- (void)closeHoverSoon {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(closeHoverUnlessPointer)
                                               object:nil];
    [self performSelector:@selector(closeHoverUnlessPointer) withObject:nil
               afterDelay:kHoverGrace];
}

// Kept while the pointer is in the box or on what it is about.
- (void)closeHoverUnlessPointer {
    if (!_hover.visible) return;
    if (_hoverByMouse) {
        if ([self pointerInHover]) return;
        NSWindow *w = _tv.window;
        if (w) {
            NSPoint p = [_tv convertPoint:[w convertPointFromScreen:[NSEvent mouseLocation]]
                                 fromView:nil];
            NSUInteger i = [_tv characterIndexUnderPoint:p];
            if (i != NSNotFound && NSLocationInRange(i, _hoverRange)) return;
        }
    }
    [self closeHover];
}

- (void)closeHover {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(pointerRested)
                                               object:nil];
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(closeHoverUnlessPointer)
                                               object:nil];
    _hoverGen++;
    [_hover close];
}

- (BOOL)hoverVisible { return _hover.visible; }

// ---- rename, references and quick fixes
- (void)note:(NSString *)message {
    if (!message.length) return;
    _lastNote = [message copy];
    if (self.showNote) self.showNote(message);
    else [self flash:message];
}

// No server for the file, or it does not do this: beep and say so.
- (BOOL)serverCan:(BOOL (^)(Lsp::Client &))can missing:(NSString *)what {
    if (!_server || !_uri) {
        NSBeep();
        [self flash:_statusText.length ? _statusText : @"No language server for this file"];
        return NO;
    }
    if (!can(_server.client)) {
        NSBeep();
        [self note:[NSString stringWithFormat:@"%@ does not %@", _server.name, what]];
        return NO;
    }
    return YES;
}

// The lists and the rename field make way for one another.
- (void)closeTransients {
    [self closeCompletion];
    [self closeSignature];
    [self closeHover];
    [self closePicker];
    [_rename close];
}

// A server's error answer as a sentence, or nil when there was none.
static NSString *ErrorText(const Json &error) {
    if (error.isNull()) return nil;
    std::string m = error["message"].asString();
    return m.empty() ? @"The language server refused" : NS(m);
}

// The text of a file as the editor has it: the text view's for the file on
// show, a tab's buffer for another open file, else the disk's.
- (NSString *)textOfPath:(NSString *)path uri:(const std::string &)uri {
    if ([self uri:uri isSameFileAs:_uri]) return _tv.string;
    NSString *buffer = self.bufferText ? self.bufferText(path) : nil;
    if (buffer) return buffer;
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

// The text a server's positions in a file refer to: what it was last sent
// for the file when a server has it open (every tab's file is), else the
// disk's.
- (NSString *)serverTextOfPath:(NSString *)path uri:(const std::string &)uri {
    for (MCLspEntry *e in _entries.allValues) {
        const std::string *t = e.server ? e.server.client.textFor(uri) : nullptr;
        if (t) return NS(*t);
    }
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

- (BOOL)showsPath:(NSString *)path {
    return _path && [RealPath(path) isEqualToString:RealPath(_path)];
}

// Show a file an edit is about to change in the editor, opening its tab if
// need be (and noting a tab it had to make in `opened`). NO, with *why, when
// it does not show as text to edit, holding `text`.
- (BOOL)showEditFile:(NSString *)path text:(NSString *)text
              opened:(NSMutableArray<NSString *> *)opened failure:(NSString **)why {
    if (![self showsPath:path]) {
        BOOL hadTab = NO;
        for (NSString *p in self.openPaths ? self.openPaths() : @[])
            if ([RealPath(p) isEqualToString:RealPath(path)]) hadTab = YES;
        if (!self.openFile(path)) {
            *why = [NSString stringWithFormat:@"Could not open %@", path.lastPathComponent];
            return NO;
        }
        if (!hadTab && ![opened containsObject:path]) [opened addObject:path];
    }
    if (!_tv.editable || ![_tv.string isEqualToString:text]) {
        *why = [NSString stringWithFormat:@"%@ is not open as text to edit",
                                          path.lastPathComponent];
        return NO;
    }
    return YES;
}

// Apply a WorkspaceEdit (rename, a quick fix, workspace/applyEdit). Each
// file's edits go into its buffer as one undo step, through the text view,
// so highlighting, the dirty mark and didChange follow as for typing. A file
// not open gets a tab; nothing is saved, so the user reviews and saves.
// All or nothing: every file is read and checked first, then opened in its
// tab and checked again, and only then edited; a file that cannot be edited
// after others were puts those back (their undo step undone), and tabs the
// edit opened close again. Returns how many files changed, or -1 with
// *failure saying why.
- (NSInteger)applyWorkspaceEdit:(const Lsp::WorkspaceEdit &)we
                     actionName:(NSString *)name
                        failure:(NSString **)failure {
    __autoreleasing NSString *dummy = nil;
    if (!failure) failure = &dummy;
    *failure = nil;
    if (!we.problem.empty()) { *failure = NS(we.problem); return -1; }
    if (we.files.empty()) return 0;
    if (!self.openFile) { *failure = @"No window to edit in"; return -1; }

    // 1. Read and check every file.
    NSMutableArray<NSString *> *paths = [NSMutableArray array], *texts = [NSMutableArray array];
    for (const Lsp::FileEdit &f : we.files) {
        NSString *path = [self treePathForURI:f.uri];
        if (!path.length) {
            *failure = [NSString stringWithFormat:@"The edit names %s, which is not a file",
                                                  f.uri.c_str()];
            return -1;
        }
        NSString *text = [self textOfPath:path uri:f.uri];
        if (!text) {
            *failure = [NSString stringWithFormat:@"Could not read %@", path.lastPathComponent];
            return -1;
        }
        // The server's positions are in the text it has: a tab's text as
        // last sent, or the disk's for a file no tab has open. Text that
        // differs from that (typed since, or a tab the server was never
        // sent) is edited only when every range holds the same text in
        // both; otherwise the edit would land in the wrong places.
        NSString *base = [self serverTextOfPath:path uri:f.uri];
        if (base && ![base isEqualToString:text]) {
            std::u16string a = U16Of(text), b = U16Of(base);
            for (const Lsp::TextEdit &e : f.edits) {
                size_t a0 = Lsp::offsetForPosition(a, e.range.start);
                size_t a1 = Lsp::offsetForPosition(a, e.range.end);
                size_t b0 = Lsp::offsetForPosition(b, e.range.start);
                size_t b1 = Lsp::offsetForPosition(b, e.range.end);
                if (a.compare(a0, a1 - a0, b, b0, b1 - b0) != 0 ||
                    Lsp::positionForOffset(a, a0).line != e.range.start.line) {
                    *failure = [NSString stringWithFormat:
                        @"%@ has changes the language server has not seen; "
                         "save it and try again", path.lastPathComponent];
                    return -1;
                }
            }
        }
        [paths addObject:path];
        [texts addObject:text];
    }

    NSString *home = _path;
    NSMutableArray<NSString *> *opened = [NSMutableArray array];   // tabs this edit made
    NSMutableArray<NSNumber *> *applied = [NSMutableArray array];
    NSInteger changed = 0;
    NSString *why = nil;
    _applyingEdit = YES;

    // 2. Open every file in its tab, before anything is edited.
    BOOL ok = YES;
    for (NSUInteger i = 0; i < paths.count && ok; i++)
        ok = [self showEditFile:paths[i] text:texts[i] opened:opened failure:&why];

    // 3. Edit them, each in one multi-range replacement: one undo step, and
    // the text view takes all of it or none.
    for (NSUInteger i = 0; i < paths.count && ok; i++) {
        ok = [self showEditFile:paths[i] text:texts[i] opened:opened failure:&why];
        if (!ok) break;
        std::vector<Lsp::OffsetEdit> edits = Lsp::offsetEdits(U16Of(_tv.string), we.files[i].edits);
        NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
        NSMutableArray<NSString *> *strings = [NSMutableArray array];
        for (auto e = edits.rbegin(); e != edits.rend(); ++e) {   // ascending
            [ranges addObject:[NSValue valueWithRange:NSMakeRange(e->start, e->length)]];
            [strings addObject:[NSString stringWithCharacters:(const unichar *)e->text.data()
                                                       length:e->text.size()]];
        }
        NSRange sel = _tv.selectedRange;
        NSUndoManager *um = _tv.undoManager;
        [_tv breakUndoCoalescing];
        [um beginUndoGrouping];
        BOOL may = [_tv shouldChangeTextInRanges:ranges replacementStrings:strings];
        if (may) {
            NSTextStorage *ts = _tv.textStorage;
            [ts beginEditing];
            for (NSInteger k = (NSInteger)ranges.count - 1; k >= 0; k--)
                [ts replaceCharactersInRange:ranges[(NSUInteger)k].rangeValue
                                  withString:strings[(NSUInteger)k]];
            [ts endEditing];
            [_tv didChangeText];
            // The caret keeps its place in the text around it.
            for (const Lsp::OffsetEdit &e : edits) {   // last first
                NSUInteger at = sel.location, end = e.start + e.length;
                if (at >= end) at = at - e.length + e.text.size();
                else if (at > e.start) at = e.start + MIN(at - e.start, e.text.size());
                sel = NSMakeRange(at, 0);
            }
        }
        [um setActionName:name ?: @"Edit"];
        [um endUndoGrouping];
        [_tv breakUndoCoalescing];
        if (!may) {
            why = [NSString stringWithFormat:@"%@ could not be edited",
                                             paths[i].lastPathComponent];
            ok = NO;
            break;
        }
        if (NSMaxRange(sel) <= _tv.string.length) [_tv setSelectedRange:sel];
        [applied addObject:@(i)];
        changed++;
    }

    // 4. A failure part way: the files already edited go back as they were.
    if (!ok) {
        for (NSNumber *n in applied.reverseObjectEnumerator) {
            NSUInteger i = n.unsignedIntegerValue;
            if (![self showsPath:paths[i]] && !self.openFile(paths[i])) continue;
            // Exactly the group this edit made, even inside the event's own
            // group, which is still open while this runs.
            NSUndoManager *um = _tv.undoManager;
            if (um.canUndo) [um undoNestedGroup];
            if (![_tv.string isEqualToString:texts[i]])
                NSLog(@"MiniCode: could not put %@ back after a failed edit", paths[i]);
        }
        changed = 0;
    }
    // Back to the file the edit was asked from.
    if (home && ![self showsPath:home]) self.openFile(home);
    if (!ok && self.closeFile)
        for (NSString *p in opened)
            if (![self showsPath:p]) self.closeFile(p);
    _applyingEdit = NO;
    [self flushChanges];
    [self syncOpenDocuments];
    if (!ok) *failure = why ?: @"The edit could not be made";
    return ok ? changed : -1;
}

// ---- rename
- (void)renameSymbol {
    if (![self serverCan:^BOOL(Lsp::Client &c) { return c.hasRename(); } missing:@"rename"])
        return;
    [self closeTransients];
    [self flushChanges];
    int gen = ++_renameGen;
    _askedSelection = _tv.selectedRange;
    NSUInteger index = _tv.selectedRange.location;
    NSString *uri = _uri;
    if (!_server.client.canPrepareRename()) {
        [self openRenameAt:[self nameRangeAt:index] gen:gen placeholder:nil];
        return;
    }
    __weak LspSession *weakSelf = self;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    _server.client.prepareRename(Std(uri), pos, [weakSelf, gen, uri, index](const Json &r,
                                                                        const Json &e) {
        [weakSelf prepareRenameArrived:r error:e gen:gen uri:uri index:index];
    });
}

// The identifier at or just before a character, or an empty range.
- (NSRange)nameRangeAt:(NSUInteger)index {
    NSString *t = _tv.string;
    if (index < t.length && IsIdentChar([t characterAtIndex:index])) return [self wordAt:index];
    if (index > 0 && index <= t.length && IsIdentChar([t characterAtIndex:index - 1]))
        return [self wordAt:index - 1];
    return NSMakeRange(index, 0);
}

- (void)prepareRenameArrived:(const Json &)result error:(const Json &)error gen:(int)gen
                         uri:(NSString *)uri index:(NSUInteger)index {
    if (gen != _renameGen || ![uri isEqualToString:_uri] ||
        !NSEqualRanges(_tv.selectedRange, _askedSelection))
        return;
    Lsp::PrepareRename p = Lsp::parsePrepareRename(result);
    if (!p.ok) {
        // Null for a keyword, a literal, or a name from a library.
        NSBeep();
        [self note:ErrorText(error) ?: @"This can't be renamed"];
        return;
    }
    NSRange range = [self nameRangeAt:index];
    if (p.hasRange) {
        std::u16string text = U16Of(_tv.string);
        size_t a = Lsp::offsetForPosition(text, p.range.start);
        size_t b = Lsp::offsetForPosition(text, p.range.end);
        if (b > a) range = NSMakeRange(a, b - a);
    }
    [self openRenameAt:range gen:gen placeholder:p.placeholder.empty() ? nil : NS(p.placeholder)];
}

- (void)openRenameAt:(NSRange)range gen:(int)gen placeholder:(NSString *)placeholder {
    if (range.length == 0 || NSMaxRange(range) > _tv.string.length) {
        NSBeep();
        [self note:@"No name at the cursor to rename"];
        return;
    }
    if (!_tv.window) return;
    _renameRange = range;
    _renameText = [_tv.string copy];
    __block NSRect rect = NSZeroRect;
    [_tv enumerateRectsForRange:range block:^(NSRect r) {
        if (NSIsEmptyRect(rect)) rect = r;
    }];
    if (NSIsEmptyRect(rect)) return;
    if (!_rename) {
        _rename = [MCRenameBox new];
        __weak LspSession *weakSelf = self;
        _rename.onClose = ^{
            LspSession *s = weakSelf;
            CodeTextView *tv = s ? s->_tv : nil;
            if (tv.window && tv.window.firstResponder != tv) [tv.window makeFirstResponder:tv];
        };
    }
    __weak LspSession *weakSelf = self;
    _rename.onCommit = ^(NSString *name) { [weakSelf commitRename:name gen:gen]; };
    [_rename showWithName:placeholder ?: [_tv.string substringWithRange:range]
           relativeToRect:rect ofView:_tv];
}

- (void)commitRename:(NSString *)name gen:(int)gen {
    if (gen != _renameGen || !_server || !_uri || ![_tv.string isEqualToString:_renameText])
        return;
    NSString *old = [_tv.string substringWithRange:_renameRange];
    if ([name isEqualToString:old]) return;
    // Every tab's text as it is, so uses in unsaved tabs are renamed too.
    [self flushChanges];
    [self syncOpenDocuments];
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    MCLspServer *server = _server;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), _renameRange.location);
    _server.client.rename(Std(uri), pos, Std(name), [weakSelf, gen, uri, server](const Json &r,
                                                                            const Json &e) {
        (void)server;
        Json result = r, error = e;
        // Applied once the reply has been read: applying switches tabs,
        // which talks to the server.
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf renameArrived:result error:error gen:gen uri:uri];
        });
    });
}

- (void)renameArrived:(const Json &)result error:(const Json &)error gen:(int)gen
                  uri:(NSString *)uri {
    // Only onto the text the name was read from.
    if (gen != _renameGen || ![uri isEqualToString:_uri] ||
        ![_tv.string isEqualToString:_renameText])
        return;
    if (NSString *why = ErrorText(error)) { NSBeep(); [self note:why]; return; }
    Lsp::WorkspaceEdit we = Lsp::parseWorkspaceEdit(result, CanonicalURI);
    if (we.editCount() == 0 && we.problem.empty()) {
        NSBeep();
        [self note:@"Nothing to rename"];
        return;
    }
    NSString *failure = nil;
    NSInteger files = [self applyWorkspaceEdit:we actionName:@"Rename" failure:&failure];
    if (files < 0) { NSBeep(); [self note:failure]; return; }
    size_t n = we.editCount();
    [self note:[NSString stringWithFormat:@"Renamed %zu occurrence%@ in %ld file%@", n,
                                          n == 1 ? @"" : @"s", (long)files,
                                          files == 1 ? @"" : @"s"]];
}

// ---- references
- (void)findReferences {
    if (![self serverCan:^BOOL(Lsp::Client &c) { return c.hasReferences(); }
                 missing:@"find references"])
        return;
    [self closeTransients];
    [self flushChanges];
    [self syncOpenDocuments];
    int gen = ++_referencesGen;
    _askedSelection = _tv.selectedRange;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), _tv.selectedRange.location);
    _server.client.references(Std(uri), pos, true, [weakSelf, gen, uri](const Json &r,
                                                                    const Json &e) {
        [weakSelf referencesArrived:r error:e gen:gen uri:uri];
    });
}

static NSFont *PickerMono(void) {
    return [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
}

- (void)referencesArrived:(const Json &)result error:(const Json &)error gen:(int)gen
                      uri:(NSString *)uri {
    if (gen != _referencesGen || ![uri isEqualToString:_uri] ||
        !NSEqualRanges(_tv.selectedRange, _askedSelection))
        return;
    if (NSString *why = ErrorText(error)) { NSBeep(); [self note:why]; return; }
    std::vector<Lsp::ReferenceFile> files =
        Lsp::groupReferences(Lsp::parseLocations(result), Std(_uri), CanonicalURI);
    if (files.empty()) { NSBeep(); [self note:@"No references found"]; return; }

    NSMutableArray<NSAttributedString *> *rows = [NSMutableArray array];
    NSMutableIndexSet *headers = [NSMutableIndexSet indexSet];
    std::vector<Lsp::Location> where;
    NSInteger select = -1;
    NSUInteger caret = _tv.selectedRange.location;
    NSDictionary *muted = @{NSFontAttributeName: PickerMono(),
                            NSForegroundColorAttributeName: LHex(0x9CA3AF)};
    NSDictionary *plain = @{NSFontAttributeName: PickerMono(),
                            NSForegroundColorAttributeName: LHex(0xD4D4D4)};
    NSString *rootReal = RealPath(_root);
    size_t total = 0;
    for (const Lsp::ReferenceFile &f : files) {
        NSString *path = [self treePathForURI:f.uri] ?: NS(f.uri);
        NSString *text = [self textOfPath:path uri:f.uri] ?: @"";
        std::u16string u = U16Of(text);
        BOOL current = [self uri:f.uri isSameFileAs:_uri];
        // The heading: the file's name, its folder within the project, and
        // how many references it holds.
        NSString *real = RealPath(path), *folder = real.stringByDeletingLastPathComponent;
        if (rootReal.length && [folder hasPrefix:rootReal])
            folder = [folder substringFromIndex:rootReal.length];
        if ([folder hasPrefix:@"/"] && rootReal.length) folder = [folder substringFromIndex:1];
        NSMutableAttributedString *h = [[NSMutableAttributedString alloc]
            initWithString:[NSString stringWithFormat:@" %@", path.lastPathComponent]
                attributes:@{NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                             NSForegroundColorAttributeName: LHex(0xE5E5E5)}];
        NSString *tail = [NSString stringWithFormat:@"  %@%@%zu", folder,
                                                    folder.length ? @"  " : @"",
                                                    f.ranges.size()];
        [h appendAttributedString:[[NSAttributedString alloc]
            initWithString:tail attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
        [headers addIndex:rows.count];
        [rows addObject:h];
        where.push_back(Lsp::Location());
        for (const Lsp::Range &r : f.ranges) {
            std::u16string lu = Lsp::lineOf(u, r.start.line);
            if (lu.size() > 400) lu.resize(400);   // a minified line
            NSString *line = [NSString stringWithCharacters:(const unichar *)lu.data()
                                                     length:lu.size()];
            // Leading space goes; the match is highlighted where it is.
            NSUInteger indent = 0;
            while (indent < line.length &&
                   [[NSCharacterSet whitespaceCharacterSet]
                       characterIsMember:[line characterAtIndex:indent]])
                indent++;
            NSUInteger len = line.length;
            NSUInteger a = MIN((NSUInteger)MAX(r.start.character, 0), len);
            NSUInteger b = r.end.line == r.start.line ? MIN((NSUInteger)MAX(r.end.character, 0), len)
                                                      : len;
            NSString *shown = [line substringFromIndex:indent];
            NSMutableAttributedString *row = [[NSMutableAttributedString alloc]
                initWithString:[NSString stringWithFormat:@"  %5d  ", r.start.line + 1]
                    attributes:muted];
            NSUInteger start = row.length;
            [row appendAttributedString:[[NSAttributedString alloc] initWithString:shown
                                                                        attributes:plain]];
            if (b > a && a >= indent) {
                NSRange hl = NSMakeRange(start + a - indent, b - a);
                if (NSMaxRange(hl) <= row.length)
                    [row addAttributes:@{NSBackgroundColorAttributeName:
                                             [LHex(0x4EA1F7) colorWithAlphaComponent:0.35],
                                         NSForegroundColorAttributeName: LHex(0xFFFFFF)}
                                 range:hl];
            }
            // The reference under the caret starts selected.
            if (current && select < 0) {
                size_t r0 = Lsp::offsetForPosition(u, r.start), r1 = Lsp::offsetForPosition(u, r.end);
                if (caret >= r0 && caret <= r1) select = (NSInteger)rows.count;
            }
            [rows addObject:row];
            where.push_back(Lsp::Location{f.uri, r});
            total++;
        }
    }
    _refRows = where;
    _pickerKind = @"references";
    [self showPickerRows:rows headers:headers select:select width:620 maxRows:14];
    [self flash:[NSString stringWithFormat:@"%zu reference%@ in %zu file%@", total,
                                           total == 1 ? @"" : @"s", files.size(),
                                           files.size() == 1 ? @"" : @"s"]];
}

- (void)showPickerRows:(NSArray<NSAttributedString *> *)rows headers:(NSIndexSet *)headers
                select:(NSInteger)select width:(CGFloat)width maxRows:(NSInteger)maxRows {
    if (!_picker) {
        _picker = [MCPickerPopup new];
        __weak LspSession *weakSelf = self;
        _picker.onAccept = ^(NSInteger row) { [weakSelf acceptPickerRow:row]; };
    }
    NSUInteger caret = _tv.selectedRange.location;
    NSRect at = [_tv firstRectForCharacterRange:NSMakeRange(caret, 0) actualRange:NULL];
    [_picker showRows:rows headers:headers select:select width:width maxRows:maxRows
         atScreenRect:at parent:_tv.window];
}

- (void)closePicker {
    if (!_picker.visible && !_pickerKind) return;
    [_picker close];
    _pickerKind = nil;
    _refRows.clear();
    _actions.clear();
    _actionServer = nil;
}

- (void)acceptPickerRow:(NSInteger)row {
    if (!_picker.visible || row < 0) return;
    if ([_pickerKind isEqualToString:@"references"] && (size_t)row < _refRows.size() &&
        !_refRows[(size_t)row].uri.empty()) {
        Lsp::Location loc = _refRows[(size_t)row];
        [self closePicker];
        [self revealLocation:loc];
    } else if ([_pickerKind isEqualToString:@"fixes"] && (size_t)row < _actions.size()) {
        Lsp::CodeAction action = _actions[(size_t)row];
        MCLspServer *server = _actionServer;
        [self closePicker];
        [self runCodeAction:action server:server];
    }
}

- (NSArray<NSString *> *)pickerRows {
    NSMutableArray *out = [NSMutableArray array];
    if (_picker.visible)
        for (NSAttributedString *r in _picker.rows) [out addObject:r.string];
    return out;
}
- (NSString *)pickerKind { return _picker.visible ? _pickerKind : nil; }
- (NSInteger)pickerSelectedRow { return _picker.visible ? _picker.selectedRow : -1; }
- (NSView *)pickerView { return _picker.visible ? _picker.contentView : nil; }
- (NSTextField *)renameField { return _rename.visible ? _rename.field : nil; }
- (NSView *)renameView { return _rename.visible ? _rename.contentView : nil; }

// ---- quick fixes
- (void)showQuickFixes {
    if (![self serverCan:^BOOL(Lsp::Client &c) { return c.hasCodeActions(); }
                 missing:@"offer quick fixes"])
        return;
    [self closeTransients];
    [self flushChanges];
    int gen = ++_actionGen;
    NSRange sel = _tv.selectedRange;
    _askedSelection = sel;
    std::u16string text = U16Of(_tv.string);
    Lsp::Range range{Lsp::positionForOffset(text, sel.location),
                     Lsp::positionForOffset(text, NSMaxRange(sel))};
    auto it = _diags.find(Std(_uri));
    Json diags = it != _diags.end() ? Lsp::diagnosticsInRange(it->second.list, range)
                                    : Json::array();
    // A caret on a line with a problem that is not under it (clang reports
    // a missing ';' at the next token): the line's problems, at the first.
    if (diags.size() == 0 && sel.length == 0 && it != _diags.end()) {
        Lsp::Range line{{range.start.line, 0}, {range.start.line, INT_MAX}};
        Json onLine = Lsp::diagnosticsInRange(it->second.list, line);
        if (onLine.size() > 0) {
            diags = onLine;
            range = Lsp::parseRange(onLine[0]["range"]);
        }
    }
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    MCLspServer *server = _server;
    _server.client.codeActions(Std(uri), range, diags,
        [weakSelf, gen, uri, server](const Json &r, const Json &e) {
            [weakSelf codeActionsArrived:r error:e gen:gen uri:uri server:server];
        });
}

static NSString *ActionKindName(const std::string &kind) {
    if (kind.empty()) return @"";
    if (kind.compare(0, 8, "quickfix") == 0) return @"fix";
    if (kind.compare(0, 22, "source.organizeImports") == 0) return @"imports";
    if (kind.compare(0, 6, "source") == 0) return @"source";
    if (kind.compare(0, 8, "refactor") == 0) return @"refactor";
    return NS(kind);
}

- (void)codeActionsArrived:(const Json &)result error:(const Json &)error gen:(int)gen
                       uri:(NSString *)uri server:(MCLspServer *)server {
    if (gen != _actionGen || ![uri isEqualToString:_uri] || server != _server ||
        !NSEqualRanges(_tv.selectedRange, _askedSelection))
        return;
    if (NSString *why = ErrorText(error)) { NSBeep(); [self note:why]; return; }
    std::vector<Lsp::CodeAction> actions =
        Lsp::parseCodeActions(result, server.client.canResolveCodeAction());
    if (actions.empty()) { [self note:@"No quick fixes"]; return; }
    NSMutableArray<NSAttributedString *> *rows = [NSMutableArray array];
    for (const Lsp::CodeAction &a : actions) {
        NSMutableAttributedString *row = [[NSMutableAttributedString alloc]
            initWithString:[NSString stringWithFormat:@" %@", NS(a.title)]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5],
                             NSForegroundColorAttributeName: LHex(0xD4D4D4)}];
        NSString *kind = ActionKindName(a.kind);
        if (a.preferred) kind = kind.length ? [kind stringByAppendingString:@", preferred"]
                                            : @"preferred";
        if (kind.length)
            [row appendAttributedString:[[NSAttributedString alloc]
                initWithString:[@"   " stringByAppendingString:kind]
                    attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                                 NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
        [rows addObject:row];
    }
    _actions = actions;
    _actionServer = server;
    _pickerKind = @"fixes";
    [self showPickerRows:rows headers:[NSIndexSet indexSet] select:0 width:440 maxRows:10];
}

// A chosen action: resolved first when it came without its edit and the
// server can fill it in, then its edit applied and its command run.
- (void)runCodeAction:(Lsp::CodeAction)action server:(MCLspServer *)server {
    if (!server || server.stopping) return;
    __weak LspSession *weakSelf = self;
    if (!action.hasEdit() && server.client.canResolveCodeAction() && action.raw) {
        server.client.resolveCodeAction(action, [weakSelf, action, server](const Json &r,
                                                                      const Json &e) {
            Lsp::CodeAction done = action;
            if (e.isNull()) {
                Lsp::CodeAction resolved = Lsp::parseCodeAction(r);
                if (resolved.hasEdit()) done.edit = resolved.edit;
                if (!done.hasCommand() && resolved.hasCommand()) done.command = resolved.command;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf performCodeAction:done server:server];
            });
        });
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf performCodeAction:action server:server];
    });
}

- (void)performCodeAction:(const Lsp::CodeAction &)action server:(MCLspServer *)server {
    if (action.hasEdit()) {
        NSString *failure = nil;
        NSInteger n = [self applyWorkspaceEdit:Lsp::parseWorkspaceEdit(action.edit, CanonicalURI)
                                    actionName:NS(action.title)
                                       failure:&failure];
        if (n < 0) { NSBeep(); [self note:failure]; return; }
    }
    if (action.hasCommand() && server && !server.stopping) {
        // The server makes its own edits with workspace/applyEdit while it
        // runs the command (onApplyEdit).
        // Its edits are undone under the fix's title.
        __weak LspSession *weakSelf = self;
        NSString *title = NS(action.title);
        _commandTitle = title;
        server.client.executeCommand(action.command, [weakSelf, title](const Json &r,
                                                                       const Json &e) {
            (void)r;
            NSString *why = ErrorText(e);
            dispatch_async(dispatch_get_main_queue(), ^{
                LspSession *me = weakSelf;
                if (!me) return;
                if (me->_commandTitle == title) me->_commandTitle = nil;
                if (why) [me note:why];
            });
        });
    } else if (!action.hasEdit()) {
        [self note:@"The language server gave nothing to apply"];
    }
}
@end
