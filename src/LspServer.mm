// LspServer.mm — Objective-C++. Finding a language server's program and
// running it (MCLspServer); see LspServer.h.
#import "LspShared.h"
#import "LspServer.h"
#include <atomic>
#include <memory>

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
NSArray<NSString *> *MCResolveCommand(const std::string &command,
                                      NSArray<NSString *> *first) {
    std::vector<std::string> words = Lsp::splitCommand(command);
    if (words.empty()) return nil;
    NSString *prog = FindProgram(NS(words[0]), first);
    if (!prog) return nil;
    NSMutableArray *argv = [NSMutableArray arrayWithObject:prog];
    for (size_t i = 1; i < words.size(); i++) [argv addObject:NS(words[i])];
    return argv;
}

std::vector<std::string> ArgvWords(NSArray<NSString *> *argv) {
    std::vector<std::string> words;
    for (NSString *a in argv) words.push_back(Std(a));
    return words;
}

// How a server is named in the status bar: pyright, not pyright-langserver.
NSString *ServerDisplayName(NSString *key, NSArray<NSString *> *argv) {
    if ([key isEqualToString:@"python"]) {
        Lsp::PythonServer kind = Lsp::pythonServerKind(ArgvWords(argv));
        if (kind != Lsp::PythonServer::Other) return NS(Lsp::pythonServerName(kind));
    }
    return NS(Lsp::programDisplayName(Std(argv.firstObject)));
}


static NSMutableSet<MCLspServer *> *gServers;   // every running server

@implementation MCLspServer {
    NSTask *_task;
    NSFileHandle *_input;
    int _inputFd;
    BOOL _inputClosed;
    dispatch_queue_t _writeQueue;
    std::unique_ptr<Lsp::Client> _client;
    NSFileHandle *_log;
    NSMutableData *_errTail;   // the end of what it wrote to stderr
    // Bytes handed to the write queue and not yet written. A server that
    // stops reading (hung, not crashed) blocks the queue's write(), and
    // every didChange after that waited in memory for ever.
    std::shared_ptr<std::atomic<size_t>> _queued;
    NSString *_stopReason;     // why minicode ended it, for exitDescription
}

// More than this waiting for a server to read, and it is taken as hung.
static size_t gMaxQueuedBytes = 64u << 20;
void MCLspSetMaxQueuedBytes(size_t bytes) { gMaxQueuedBytes = bytes ? bytes : 64u << 20; }

- (instancetype)initWithKey:(NSString *)key argv:(NSArray<NSString *> *)argv
                       root:(NSString *)root {
    if (!(self = [super init])) return nil;
    _key = [key copy];
    static NSUInteger serial;
    _serverId = [NSString stringWithFormat:@"%@#%lu", key, (unsigned long)++serial];
    _argv = [argv copy];
    _program = [argv.firstObject copy];
    _name = [ServerDisplayName(key, argv) copy];
    _errTail = [NSMutableData data];
    _queued = std::make_shared<std::atomic<size_t>>(0);
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
    if (_inputClosed || _unresponsive) return;
    [self logLine:@"--> " bytes:bytes];
    // Only while earlier messages still wait: one big message (a huge
    // file's didChange) to a server that reads is no sign of a hang.
    const size_t waiting = _queued->load();
    if (waiting > 0 && waiting + bytes.size() > gMaxQueuedBytes) {
        [self stopUnresponsive:waiting];
        return;
    }
    NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    int fd = _inputFd;
    NSFileHandle *keep = _input;   // keeps the descriptor open until written
    std::shared_ptr<std::atomic<size_t>> queued = _queued;
    *queued += data.length;
    dispatch_async(_writeQueue, ^{
        (void)keep;
        const char *p = (const char *)data.bytes;
        size_t left = data.length;
        while (left > 0) {
            ssize_t n = write(fd, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;   // EPIPE: the server is gone; its exit is handled elsewhere
            }
            p += n;
            left -= (size_t)n;
        }
        *queued -= data.length;
    });
}

// The server has not read what it was sent for a long while: nothing more
// is queued, and it is killed, so it goes through the usual exit (and the
// automatic restart) with the reason in its exit description.
- (void)stopUnresponsive:(size_t)waiting {
    if (_unresponsive) return;
    _unresponsive = YES;
    _stopReason = [NSString stringWithFormat:
        @"stopped reading what minicode sent (%@ waiting), so it was stopped",
        [NSByteCountFormatter stringFromByteCount:(long long)waiting
                                       countStyle:NSByteCountFormatterCountStyleFile]];
    NSLog(@"MiniCode: %@ %@", _name, _stopReason);
    if (_running && _pid > 0) kill(_pid, SIGKILL);
}

- (size_t)queuedBytes { return _queued->load(); }

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
    if (_stopReason) how = _stopReason;
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
