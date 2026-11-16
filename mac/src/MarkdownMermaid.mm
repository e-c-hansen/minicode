// MarkdownMermaid.mm — see MarkdownMermaid.h; the decisions are in
// MermaidDiagram.h.
#import "MarkdownMermaid.h"
#import "AppSettings.h"
#import "Lsp.h"            // MCFindProgram, MCSearchDirs
#import "MarkdownImage.h"  // MCMarkdownImage
#import <CommonCrypto/CommonDigest.h>
#import <WebKit/WebKit.h>
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
NSString *const MCMermaidDownloadLink = @"minicode-mermaid:download";

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

// ------------------------------------------------------------ mermaid.js

static NSString *MCHex(const unsigned char *bytes, size_t n) {
    NSMutableString *s = [NSMutableString stringWithCapacity:n * 2];
    for (size_t i = 0; i < n; i++) [s appendFormat:@"%02x", bytes[i]];
    return s;
}

static NSString *MCSha256(NSData *data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    return MCHex(digest, sizeof digest);
}

// Application Support/MiniCode/mermaid/mermaid-<version>.min.js.
static NSString *MCManagedMermaidJs(void) {
    NSString *support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                            NSUserDomainMask, YES).firstObject;
    NSString *dir = [[support stringByAppendingPathComponent:@"MiniCode"]
        stringByAppendingPathComponent:@"mermaid"];
    return [dir stringByAppendingPathComponent:
        [NSString stringWithFormat:@"mermaid-%s.min.js", MermaidDiagram::kMermaidJsVersion]];
}

// The script's text, read (and for the downloaded copy, checked) once.
static NSString *gMermaidJsText, *gMermaidJsTextPath;

static NSString *MCMermaidJsText(void) {
    NSString *path = MCMermaidJsPath();
    if (!path) return nil;
    if (gMermaidJsText && [gMermaidJsTextPath isEqualToString:path]) return gMermaidJsText;
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data.length) return nil;
    gMermaidJsText = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    gMermaidJsTextPath = path;
    return gMermaidJsText;
}

NSString *MCMermaidJsPath(void) {
    const char *forced = getenv("MINICODE_MERMAID_JS");
    if (forced && *forced) {
        NSString *p = [NSString stringWithUTF8String:forced];
        return [[NSFileManager defaultManager] isReadableFileAtPath:p] ? p : nil;
    }
    NSString *managed = MCManagedMermaidJs();
    static NSString *checked;   // the copy found good this launch
    if ([checked isEqualToString:managed] &&
        [[NSFileManager defaultManager] fileExistsAtPath:managed])
        return managed;
    NSData *data = [NSData dataWithContentsOfFile:managed];
    if (!data.length) return nil;
    if (![MCSha256(data) isEqualToString:@(MermaidDiagram::kMermaidJsSha256)]) return nil;
    checked = managed;
    return managed;
}

static NSMutableArray<void (^)(NSString *)> *gMermaidWaiters;
static NSString *gMermaidProblem;

BOOL MCMermaidDownloading(void) {
    return gMermaidWaiters != nil;
}

NSString *MCMermaidDownloadProblem(void) {
    return gMermaidProblem;
}

void MCDownloadMermaid(void (^done)(NSString *problem)) {
    if (gMermaidWaiters) {
        [gMermaidWaiters addObject:[done copy]];
        return;
    }
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    NSString *urlText = @(MermaidDiagram::kMermaidJsUrl);
    NSString *want = @(MermaidDiagram::kMermaidJsSha256);
    if ([env[@"MINICODE_MERMAID_DOWNLOAD"] length]) {   // the checks' stand-in
        urlText = env[@"MINICODE_MERMAID_DOWNLOAD"];
        if ([env[@"MINICODE_MERMAID_SHA256"] length]) want = env[@"MINICODE_MERMAID_SHA256"];
    }
    NSURL *url = [NSURL URLWithString:urlText];
    if (!url) {
        done(@"The download address is not valid.");
        return;
    }
    gMermaidProblem = nil;
    gMermaidWaiters = [NSMutableArray arrayWithObject:[done copy]];
    NSURLSession *session = [NSURLSession sessionWithConfiguration:
        NSURLSessionConfiguration.ephemeralSessionConfiguration];
    NSURLSessionDownloadTask *task = [session downloadTaskWithURL:url
        completionHandler:^(NSURL *tmp, NSURLResponse *response, NSError *error) {
        NSString *problem = error.localizedDescription;
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        if (!problem && [http isKindOfClass:NSHTTPURLResponse.class] && http.statusCode != 200)
            problem = [NSString stringWithFormat:@"The download failed (HTTP %ld).",
                                                 (long)http.statusCode];
        NSData *data = problem ? nil : [NSData dataWithContentsOfURL:tmp];
        if (!problem && data.length == 0) problem = @"The download did not arrive.";
        if (!problem && ![MCSha256(data) isEqualToString:want])
            problem = [NSString stringWithFormat:@"The download did not match mermaid.js %s's "
                                                 @"published checksum, so it was not kept.",
                                                 MermaidDiagram::kMermaidJsVersion];
        if (!problem) {
            NSString *dest = MCManagedMermaidJs();
            [[NSFileManager defaultManager] createDirectoryAtPath:dest.stringByDeletingLastPathComponent
                                      withIntermediateDirectories:YES attributes:nil error:nil];
            NSError *writeError = nil;
            if (![data writeToFile:dest options:NSDataWritingAtomic error:&writeError])
                problem = writeError.localizedDescription ?: @"mermaid.js could not be saved.";
        }
        [session finishTasksAndInvalidate];
        dispatch_async(dispatch_get_main_queue(), ^{
            NSArray<void (^)(NSString *)> *waiters = gMermaidWaiters;
            gMermaidWaiters = nil;
            gMermaidProblem = [problem copy];
            for (void (^w)(NSString *) in waiters) w(problem);
        });
    }];
    [task resume];
}

int MCMermaidDrawerState(void) {
    if (MCMermaidPath()) return 4;
    if (MCMermaidJsPath()) return 3;
    if (MCMermaidDownloading()) return 1;
    return gMermaidProblem ? 2 : 0;
}

// The view that runs mermaid.js: never in a window, a data store of its
// own that keeps nothing, every load refused but the empty page it starts
// with (by its navigation delegate, and by a content rule list for what a
// page fetches itself), the script put in as a user script rather than
// loaded from anywhere. It draws without a background, and a diagram comes
// out as a PDF WebKit makes of it (vectors, so it stays sharp at any zoom):
// NSImage's own SVG drawing ignores Mermaid's style sheet and its em
// offsets, which put every label above its box in dark grey and filled each
// curved edge as a black wedge (round 8, D3).
@interface MCMermaidWeb : NSObject <WKNavigationDelegate>
+ (instancetype)shared;
- (void)drawSource:(NSString *)source theme:(NSString *)theme
              done:(void (^)(NSData *pdf, NSString *error))done;
@property(nonatomic, readonly) NSUInteger refused;
@end

@implementation MCMermaidWeb {
    WKWebView *_web;
    NSString *_scriptPath;
    BOOL _ready, _starting;
    NSMutableArray *_waiting;   // blocks run once the page is ready
}

+ (instancetype)shared {
    static MCMermaidWeb *web;
    if (!web) web = [MCMermaidWeb new];
    return web;
}

- (void)whenReady:(void (^)(NSString *error))then {
    NSString *path = MCMermaidJsPath();
    if (_ready && [path isEqualToString:_scriptPath]) {
        then(nil);
        return;
    }
    if (!_waiting) _waiting = [NSMutableArray array];
    [_waiting addObject:[then copy]];
    if (_starting) return;
    _starting = YES;
    _ready = NO;
    NSString *script = MCMermaidJsText();
    if (!script) {
        [self started:@"mermaid.js could not be read."];
        return;
    }
    _scriptPath = path;
    // Every load a page could make of itself (the page's own navigation is
    // the delegate's to refuse).
    NSString *rules = @"[{\"trigger\":{\"url-filter\":\".*\",\"resource-type\":[\"image\","
                      @"\"style-sheet\",\"script\",\"font\",\"raw\",\"svg-document\",\"media\","
                      @"\"popup\"]},\"action\":{\"type\":\"block\"}}]";
    __weak MCMermaidWeb *weakSelf = self;
    [WKContentRuleListStore.defaultStore
        compileContentRuleListForIdentifier:@"minicode-mermaid-no-loads"
                     encodedContentRuleList:rules
                          completionHandler:^(WKContentRuleList *list, NSError *error) {
        MCMermaidWeb *me = weakSelf;
        if (!me) return;
        if (!list) {
            [me started:error.localizedDescription ?: @"WebKit refused the rule list."];
            return;
        }
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
        [config.userContentController addContentRuleList:list];
        [config.userContentController addUserScript:[[WKUserScript alloc]
            initWithSource:script injectionTime:WKUserScriptInjectionTimeAtDocumentStart
          forMainFrameOnly:YES]];
        me->_web = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 900) configuration:config];
        // Transparent, so the PDF has no white page under the drawing (no
        // public switch on the Mac; Safari's own key). Without it a diagram
        // still draws, on white.
        @try {
            [me->_web setValue:@NO forKey:@"drawsBackground"];
        } @catch (NSException *e) {
            (void)e;
        }
        me->_web.navigationDelegate = me;
        [me->_web loadHTMLString:@"<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head>"
                                 @"<body></body></html>"
                         baseURL:nil];
    }];
}

- (void)started:(NSString *)error {
    _starting = NO;
    _ready = error == nil;
    NSArray *waiting = _waiting;
    _waiting = nil;
    for (void (^w)(NSString *) in waiting) w(error);
}

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
    // Only the empty page, loaded from a string, ever goes in.
    NSString *scheme = action.request.URL.scheme.lowercaseString;
    const BOOL blank = !_ready && (scheme.length == 0 || [scheme isEqualToString:@"about"]);
    if (!blank) _refused++;
    decisionHandler(blank ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    if (_starting) [self started:nil];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation
      withError:(NSError *)error {
    if (_starting) [self started:error.localizedDescription ?: @"The drawing page did not load."];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation
      withError:(NSError *)error {
    if (_starting) [self started:error.localizedDescription ?: @"The drawing page did not load."];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    _ready = NO;   // made again for the next diagram
}

- (void)drawSource:(NSString *)source theme:(NSString *)theme
              done:(void (^)(NSData *pdf, NSString *error))done {
    __weak MCMermaidWeb *weakSelf = self;
    __block BOOL finished = NO;
    void (^finish)(NSData *, NSString *) = ^(NSData *pdf, NSString *error) {
        if (finished) return;
        finished = YES;
        done(pdf, error);
    };
    // A diagram that never comes back must not hold up the rest.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        finish(nil, @"mermaid.js did not finish within 30 seconds");
    });
    [self whenReady:^(NSString *error) {
        MCMermaidWeb *me = weakSelf;
        if (error || !me) {
            finish(nil, error ?: @"mermaid.js is not there");
            return;
        }
        NSString *body = [NSString stringWithUTF8String:MermaidDiagram::renderScript().c_str()];
        [me->_web callAsyncJavaScript:body
                            arguments:@{@"source": source, @"theme": theme}
                              inFrame:nil
                       inContentWorld:WKContentWorld.pageWorld
                    completionHandler:^(id result, NSError *jsError) {
            NSDictionary *place = [result isKindOfClass:NSDictionary.class] ? result : nil;
            const NSRect rect = NSMakeRect([place[@"x"] doubleValue], [place[@"y"] doubleValue],
                                           [place[@"width"] doubleValue], [place[@"height"] doubleValue]);
            if (jsError || !place || rect.size.width <= 0 || rect.size.height <= 0) {
                NSString *why = jsError.userInfo[@"WKJavaScriptExceptionMessage"] ?:
                                jsError.localizedDescription ?: @"mermaid.js drew nothing";
                if ([why hasPrefix:@"Error: "]) why = [why substringFromIndex:7];
                finish(nil, why);
                return;
            }
            MCMermaidWeb *again = weakSelf;
            if (!again) {
                finish(nil, @"mermaid.js is not there");
                return;
            }
            WKPDFConfiguration *pdf = [WKPDFConfiguration new];
            pdf.rect = rect;
            [again->_web createPDFWithConfiguration:pdf
                                  completionHandler:^(NSData *data, NSError *pdfError) {
                if (!data.length)
                    finish(nil, pdfError.localizedDescription ?: @"WebKit made no picture of the diagram");
                else
                    finish(data, nil);
            }];
        }];
    }];
}

@end

NSUInteger MCMermaidWebLoadsRefused(void) {
    return [MCMermaidWeb shared].refused;
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

// A diagram's file: mmdc's PNG, or mermaid.js's PDF.
static NSString *MCMermaidCacheFile(NSString *key, NSString *extension) {
    return [MCMermaidCacheDirectory() stringByAppendingPathComponent:[key stringByAppendingPathExtension:extension]];
}

// The picture for `key` (PNG or PDF bytes) from memory, else from disk
// (into memory); nil if none.
static NSData *MCMermaidPicture(NSString *key) {
    MCMermaidSetUp();
    NSData *d = gPictures[key];
    if (d) return d;
    for (NSString *ext in @[@"png", @"pdf"]) {
        d = [NSData dataWithContentsOfFile:MCMermaidCacheFile(key, ext)];
        if (d.length) break;
    }
    if (d.length) gPictures[key] = d;
    return d.length ? d : nil;
}

static BOOL MCIsPdf(NSData *data) {
    return data.length > 5 && memcmp(data.bytes, "%PDF-", 5) == 0;
}

BOOL MCMermaidIsReady(NSString *key) {
    return key && MCMermaidPicture(key) != nil;
}

// A picture of the PNG at its size in points (it is drawn at kScale), or
// of mermaid.js's drawing (a PDF) at its own size.
static NSImage *MCMermaidImage(NSData *png) {
    if (MCIsPdf(png)) {
        NSImage *pdf = [[NSImage alloc] initWithData:png];
        return pdf.size.width > 0 && pdf.size.height > 0 ? pdf : nil;
    }
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
        // svg: what mermaid.js's drawings were kept as before kVersion 3.
        if (![@[@"png", @"pdf", @"svg"] containsObject:name.pathExtension]) continue;
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

static void MCMermaidSettle(NSArray<NSString *> *job, NSData *picture, NSString *error) {
    gRunning = NO;
    [gUnderWay removeObject:job[0]];
    if (picture) gPictures[job[0]] = picture;
    else gFailures[job[0]] = error ?: @"the diagram could not be drawn";
    MCMermaidTrimCache();
    [[NSNotificationCenter defaultCenter] postNotificationName:MCMermaidDrawnNotification
                                                        object:nil
                                                      userInfo:@{MCMermaidKeysKey: [NSSet setWithObject:job[0]]}];
    MCMermaidRunNext();
}

static void MCMermaidKeep(NSString *key, NSData *picture, NSString *extension) {
    NSString *dir = MCMermaidCacheDirectory();
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    [picture writeToFile:MCMermaidCacheFile(key, extension) atomically:YES];
}

// The next diagram, by what its key was made for: mmdc off the main
// thread, mermaid.js in the WebKit view on it. One at a time either way.
static void MCMermaidRunNext(void) {
    if (gRunning || gQueue.count == 0) return;
    NSArray<NSString *> *job = gQueue.firstObject;
    [gQueue removeObjectAtIndex:0];
    const BOOL web = [job[3] isEqualToString:@(MermaidDiagram::kDrawerMermaidJs)];
    NSString *mmdc = web ? nil : MCMermaidPath();
    if (web ? !MCMermaidJsPath() : !mmdc) {   // gone meanwhile: asked again on a later page
        [gUnderWay removeObject:job[0]];
        MCMermaidRunNext();
        return;
    }
    gRunning = YES;
    if (web) {
        [[MCMermaidWeb shared] drawSource:job[1] theme:job[2] done:^(NSData *pdf, NSString *error) {
            NSImage *image = pdf ? MCMermaidImage(pdf) : nil;
            if (pdf && !image) error = @"mermaid.js drew a picture this Mac cannot show";
            if (image) MCMermaidKeep(job[0], pdf, @"pdf");
            MCMermaidSettle(job, image ? pdf : nil, error);
        }];
        return;
    }
    dispatch_async(gRunQueue, ^{
        NSString *error = nil;
        NSData *png = MCMermaidRunOne(mmdc, job[1], job[2], &error);
        if (png) MCMermaidKeep(job[0], png, @"png");
        dispatch_async(dispatch_get_main_queue(), ^{ MCMermaidSettle(job, png, error); });
    });
}

static void MCMermaidAsk(NSString *key, NSString *source, NSString *theme, NSString *drawer) {
    MCMermaidSetUp();
    if (gPictures[key] || gFailures[key] || [gUnderWay containsObject:key]) return;
    [gUnderWay addObject:key];
    [gQueue addObject:@[key, source, theme, drawer]];
}

// --------------------------------------------------------------- a window
@implementation MCMarkdownMermaid {
    BOOL _on, _dark, _wanted, _noted;
    NSString *_drawer;
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
    _wanted = [AppSettings shared].settings.mermaid();
    _drawer = MCMermaidPath() ? @(MermaidDiagram::kDrawerMmdc)
            : MCMermaidJsPath() ? @(MermaidDiagram::kDrawerMermaidJs) : nil;
    _on = _wanted && _drawer != nil;
    _noted = NO;
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
    NSString *k = NSFromStd(MermaidDiagram::key(source, theme, _drawer.UTF8String));
    NSData *png = MCMermaidPicture(k);
    NSImage *image = png ? MCMermaidImage(png) : nil;
    if (!image) {
        if (gFailures[k]) {
            *complaint = gFailures[k];
            return nil;
        }
        *key = k;
        [_awaited addObject:k];
        MCMermaidAsk(k, NSFromStd(source), NSFromStd(theme), _drawer);
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

- (NSAttributedString *)downloadNoteForRuns:(const std::vector<MdRun> &)runs
                                         at:(size_t)i
                                       font:(NSFont *)font
                                      color:(NSColor *)color
                                  linkColor:(NSColor *)linkColor {
    if (_on || !_wanted || _noted || i >= runs.size() || !MermaidDiagram::isMermaidRun(runs[i]))
        return nil;
    for (size_t k = 0; k < i; k++)   // the page's first diagram only
        if (MermaidDiagram::isMermaidRun(runs[k])) return nil;
    _noted = YES;
    NSString *text, *link = nil;
    if (MCMermaidDownloading()) {
        text = @"Downloading mermaid.js, which draws the diagrams on this page…";
    } else if (NSString *problem = MCMermaidDownloadProblem()) {
        text = [NSString stringWithFormat:@"mermaid.js could not be downloaded (%@), so the "
                                          @"diagrams show as their source. ", problem];
        link = @"Try again";
    } else {
        text = [NSString stringWithFormat:@"The diagrams on this page show as their source: "
                                          @"minicode draws them with mermaid.js, which is not "
                                          @"downloaded yet (%.1f MB, once; it runs with no "
                                          @"network). ", MermaidDiagram::kMermaidJsBytes / 1e6];
        link = @"Download Mermaid";
    }
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.paragraphSpacing = 6;
    NSDictionary *muted = @{NSFontAttributeName: font, NSForegroundColorAttributeName: color,
                            NSParagraphStyleAttributeName: ps};
    NSMutableAttributedString *note = [[NSMutableAttributedString alloc] initWithString:text
                                                                             attributes:muted];
    if (link) {
        NSMutableDictionary *a = [muted mutableCopy];
        a[NSLinkAttributeName] = MCMermaidDownloadLink;
        a[NSForegroundColorAttributeName] = linkColor;
        [note appendAttributedString:[[NSAttributedString alloc] initWithString:link attributes:a]];
    }
    [note appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:muted]];
    return note;
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
