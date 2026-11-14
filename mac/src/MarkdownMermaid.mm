// MarkdownMermaid.mm — see MarkdownMermaid.h; the decisions are in
// MermaidDiagram.h.
#import "MarkdownMermaid.h"
#import "AppSettings.h"
#import "Lsp.h"            // MCFindProgram, MCSearchDirs
#import "MarkdownImage.h"  // MCMarkdownImage
#include "MermaidDiagram.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

NSAttributedStringKey const MCMarkdownMermaidAttribute = @"MCMarkdownMermaid";
NSNotificationName const MCMermaidDrawnNotification = @"MCMermaidDrawnNotification";
NSString *const MCMermaidKeysKey = @"keys";

static NSString *NSFromStd(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

NSString *MCMermaidPath(void) {
    const char *forced = getenv("MINICODE_MMDC");
    if (forced && *forced) {
        NSString *p = [NSString stringWithUTF8String:forced];
        return [[NSFileManager defaultManager] isExecutableFileAtPath:p] ? p : nil;
    }
    return MCFindProgram(@"mmdc");
}

NSString *MCMermaidCacheDirectory(void) {
    const char *forced = getenv("MINICODE_MERMAID_CACHE");
    if (forced && *forced) return [NSString stringWithUTF8String:forced];
    NSString *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    return [[caches stringByAppendingPathComponent:@"MiniCode"] stringByAppendingPathComponent:@"mermaid"];
}

// --------------------------------------------------------------- the store
// What is known of a diagram: its PNG, or why mmdc could not draw it.
// Failures are kept in memory only (a missing browser can look like a bad
// diagram), pictures on disk too.
static NSMutableDictionary<NSString *, NSData *> *gPictures;
static NSMutableDictionary<NSString *, NSString *> *gFailures;
static NSMutableSet<NSString *> *gUnderWay;
static NSMutableArray<NSArray<NSString *> *> *gQueue;   // @[key, source, theme]
static dispatch_queue_t gRunQueue;
static BOOL gRunning;

static void MCMermaidSetUp(void) {
    if (gPictures) return;
    gPictures = [NSMutableDictionary dictionary];
    gFailures = [NSMutableDictionary dictionary];
    gUnderWay = [NSMutableSet set];
    gQueue = [NSMutableArray array];
    gRunQueue = dispatch_queue_create("minicode.mermaid", DISPATCH_QUEUE_SERIAL);
}

static NSString *MCMermaidCacheFile(NSString *key) {
    return [MCMermaidCacheDirectory() stringByAppendingPathComponent:[key stringByAppendingPathExtension:@"png"]];
}

// The PNG for `key` from memory, else from disk (into memory); nil if none.
static NSData *MCMermaidPicture(NSString *key) {
    MCMermaidSetUp();
    NSData *d = gPictures[key];
    if (d) return d;
    d = [NSData dataWithContentsOfFile:MCMermaidCacheFile(key)];
    if (d.length) gPictures[key] = d;
    return d.length ? d : nil;
}

BOOL MCMermaidIsReady(NSString *key) {
    return key && MCMermaidPicture(key) != nil;
}

// A picture of the PNG at its size in points (it is drawn at kScale).
static NSImage *MCMermaidImage(NSData *png) {
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:png];
    if (!rep || rep.pixelsWide <= 0 || rep.pixelsHigh <= 0) return nil;
    const CGFloat scale = MermaidDiagram::kScale;
    rep.size = NSMakeSize(rep.pixelsWide / scale, rep.pixelsHigh / scale);
    NSImage *image = [[NSImage alloc] initWithSize:rep.size];
    [image addRepresentation:rep];
    return image;
}

// Keeps the folder under 50 MB, the pictures used longest ago going first,
// once a launch.
static void MCMermaidTrimCache(void) {
    static BOOL done;
    if (done) return;
    done = YES;
    NSString *dir = MCMermaidCacheDirectory();
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSArray *> *files = [NSMutableArray array];
    unsigned long long total = 0;
    for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
        if (![name.pathExtension isEqualToString:@"png"]) continue;
        NSString *path = [dir stringByAppendingPathComponent:name];
        NSDictionary *a = [fm attributesOfItemAtPath:path error:nil];
        total += a.fileSize;
        [files addObject:@[a.fileModificationDate ?: NSDate.distantPast, path, @(a.fileSize)]];
    }
    if (total <= 50ull * 1024 * 1024) return;
    [files sortUsingComparator:^NSComparisonResult(NSArray *x, NSArray *y) { return [x[0] compare:y[0]]; }];
    for (NSArray *f in files) {
        if (total <= 40ull * 1024 * 1024) break;
        if ([fm removeItemAtPath:f[1] error:nil]) total -= [f[2] unsignedLongLongValue];
    }
}

// ---------------------------------------------------------------- the run
// Runs mmdc on one diagram, off the main thread: its PNG, or nil with
// *error. Its own process group, so a browser it starts goes with it at the
// time limit (90 s: the first run of a new install may set one up).
static NSData *MCMermaidRunOne(NSString *mmdc, NSString *source, NSString *theme, NSString **error) {
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"MiniCode-mermaid-%d-%@", (int)getpid(), NSUUID.UUID.UUIDString]];
    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES
                        attributes:@{NSFilePosixPermissions: @0700} error:nil]) {
        *error = @"could not make a scratch folder";
        return nil;
    }
    NSString *in = [dir stringByAppendingPathComponent:@"diagram.mmd"];
    NSString *out = [dir stringByAppendingPathComponent:@"diagram.png"];
    [source writeToFile:in atomically:NO encoding:NSUTF8StringEncoding error:nil];
    std::vector<std::string> args = {mmdc.fileSystemRepresentation};
    for (const std::string &a : MermaidDiagram::arguments(in.fileSystemRepresentation,
                                                          out.fileSystemRepresentation,
                                                          theme.UTF8String))
        args.push_back(a);
    std::vector<char *> argv;
    for (std::string &a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    // The environment, with PATH widened (mmdc is a node script, and the
    // app launched from Finder has a short PATH).
    std::vector<std::string> env;
    std::string path = "/usr/bin:/bin:/usr/sbin:/sbin";
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, "PATH=", 5) == 0) path = *e + 5;
        else env.push_back(*e);
    }
    for (NSString *d in MCSearchDirs()) path += std::string(":") + d.fileSystemRepresentation;
    path = std::string(mmdc.stringByDeletingLastPathComponent.fileSystemRepresentation) + ":" + path;
    env.push_back("PATH=" + path);
    std::vector<char *> envp;
    for (std::string &e : env) envp.push_back(&e[0]);
    envp.push_back(nullptr);

    int pipeFD[2];
    if (pipe(pipeFD) != 0) { *error = @"could not start mmdc"; return nil; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipeFD[1], 2);
    posix_spawn_file_actions_addclose(&fa, pipeFD[0]);
    posix_spawn_file_actions_addclose(&fa, pipeFD[1]);
    posix_spawn_file_actions_addchdir_np(&fa, dir.fileSystemRepresentation);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(pipeFD[1]);
    if (rc != 0) {
        close(pipeFD[0]);
        [fm removeItemAtPath:dir error:nil];
        *error = [NSString stringWithFormat:@"could not start mmdc: %s", strerror(rc)];
        return nil;
    }
    std::string err;
    const NSTimeInterval deadline = NSDate.timeIntervalSinceReferenceDate + 90;
    BOOL timedOut = NO;
    for (;;) {
        const NSTimeInterval left = deadline - NSDate.timeIntervalSinceReferenceDate;
        if (left <= 0) { timedOut = YES; break; }
        struct pollfd p = {pipeFD[0], POLLIN, 0};
        const int r = poll(&p, 1, (int)MIN(left * 1000, 1000.0));
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) continue;
        char buf[4096];
        ssize_t n = read(pipeFD[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;   // mmdc (and all it started) closed stderr
        if (err.size() < 65536) err.append(buf, (size_t)n);
    }
    close(pipeFD[0]);
    int status = 0;
    if (timedOut) {
        kill(-pid, SIGTERM);
        for (int i = 0; i < 20 && waitpid(pid, &status, WNOHANG) == 0; i++) usleep(100000);
        kill(-pid, SIGKILL);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    kill(-pid, SIGKILL);   // anything of its group still about
    NSData *png = nil;
    if (timedOut) {
        *error = @"mmdc did not finish within 90 seconds";
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        NSString *why = [NSString stringWithFormat:@"mmdc failed (exit status %d)",
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1];
        *error = NSFromStd(MermaidDiagram::errorSummary(err, why.UTF8String));
    } else {
        png = [NSData dataWithContentsOfFile:out];
        if (!png.length || !MCMermaidImage(png)) {
            png = nil;
            *error = @"mmdc wrote no picture";
        }
    }
    [fm removeItemAtPath:dir error:nil];
    return png;
}

static void MCMermaidRunNext(void);

static void MCMermaidRunNext(void) {
    if (gRunning || gQueue.count == 0) return;
    NSString *mmdc = MCMermaidPath();
    if (!mmdc) {
        NSMutableSet *keys = [NSMutableSet set];
        for (NSArray *job in gQueue) [keys addObject:job[0]];
        [gQueue removeAllObjects];
        [gUnderWay minusSet:keys];
        return;
    }
    NSArray<NSString *> *job = gQueue.firstObject;
    [gQueue removeObjectAtIndex:0];
    gRunning = YES;
    dispatch_async(gRunQueue, ^{
        NSString *error = nil;
        NSData *png = MCMermaidRunOne(mmdc, job[1], job[2], &error);
        if (png) {
            NSString *dir = MCMermaidCacheDirectory();
            [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                       attributes:nil error:nil];
            [png writeToFile:MCMermaidCacheFile(job[0]) atomically:YES];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            gRunning = NO;
            [gUnderWay removeObject:job[0]];
            if (png) gPictures[job[0]] = png;
            else gFailures[job[0]] = error ?: @"mmdc failed";
            MCMermaidTrimCache();
            [[NSNotificationCenter defaultCenter] postNotificationName:MCMermaidDrawnNotification
                                                                object:nil
                                                              userInfo:@{MCMermaidKeysKey: [NSSet setWithObject:job[0]]}];
            MCMermaidRunNext();
        });
    });
}

static void MCMermaidAsk(NSString *key, NSString *source, NSString *theme) {
    MCMermaidSetUp();
    if (gPictures[key] || gFailures[key] || [gUnderWay containsObject:key]) return;
    [gUnderWay addObject:key];
    [gQueue addObject:@[key, source, theme]];
}

// --------------------------------------------------------------- a window
@implementation MCMarkdownMermaid {
    BOOL _on, _dark;
    NSMutableSet<NSString *> *_awaited;
    BOOL _renderQueued;
    id _observer;
}

- (instancetype)init {
    if ((self = [super init])) {
        __weak MCMarkdownMermaid *weakSelf = self;
        _observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:MCMermaidDrawnNotification object:nil queue:nil
                    usingBlock:^(NSNotification *n) { [weakSelf drawn:n.userInfo[MCMermaidKeysKey]]; }];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:_observer];
}

- (void)beginPageDark:(BOOL)dark {
    MCMermaidSetUp();
    _dark = dark;
    _on = [AppSettings shared].settings.mermaid() && MCMermaidPath() != nil;
    _awaited = [NSMutableSet set];
}

- (NSAttributedString *)pictureForRuns:(const std::vector<MdRun> &)runs
                                    at:(size_t)i
                                   end:(size_t *)end
                               lineKey:(NSAttributedStringKey)lineKey
                             complaint:(NSString **)complaint
                                   key:(NSString **)key {
    *complaint = nil;
    *key = nil;
    if (!_on || i >= runs.size() || !MermaidDiagram::isMermaidRun(runs[i])) return nil;
    if (i > 0 && MermaidDiagram::isMermaidRun(runs[i - 1])) return nil;   // not the block's first run
    size_t e = i;
    std::string source;
    while (e < runs.size() && MermaidDiagram::isMermaidRun(runs[e])) source += runs[e++].text;
    const std::string theme = MermaidDiagram::theme(_dark);
    NSString *k = NSFromStd(MermaidDiagram::key(source, theme));
    NSData *png = MCMermaidPicture(k);
    NSImage *image = png ? MCMermaidImage(png) : nil;
    if (!image) {
        if (gFailures[k]) {
            *complaint = gFailures[k];
            return nil;
        }
        *key = k;
        [_awaited addObject:k];
        MCMermaidAsk(k, NSFromStd(source), NSFromStd(theme));
        return nil;
    }
    *end = e;
    MCMarkdownImage *att = [[MCMarkdownImage alloc] initWithPicture:image];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.alignment = NSTextAlignmentCenter;
    ps.paragraphSpacingBefore = 4;
    ps.paragraphSpacing = 8;
    NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
        initWithAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
    [pic appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"]];
    const NSRange all = NSMakeRange(0, pic.length);
    [pic addAttribute:NSParagraphStyleAttributeName value:ps range:all];
    if (runs[i].line >= 0 && lineKey) [pic addAttribute:lineKey value:@(runs[i].line) range:all];
    return pic;
}

- (void)endPage {
    MCMermaidRunNext();
}

- (void)drawn:(NSSet<NSString *> *)keys {
    if (!_awaited.count || ![_awaited intersectsSet:keys]) return;
    [_awaited minusSet:keys];
    if (_renderQueued) return;
    _renderQueued = YES;
    __weak MCMarkdownMermaid *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        MCMarkdownMermaid *s = weakSelf;
        if (!s) return;
        s->_renderQueued = NO;
        if (s.onArrived) s.onArrived();
    });
}

- (void)forget {
    _awaited = nil;
}

@end
