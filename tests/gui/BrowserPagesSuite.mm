// BrowserPagesSuite.mm — `make guitest SUITES=browserpages`: agents checking
// pages in the browser pane (spec feature 6). `minicode browse` loads into
// the pane's agent page, never the user's, without taking the keyboard;
// `--wait` prints the HTTP status; `page` reads the text, the console with
// source lines, screenshots and the address, only of local pages; the user's
// page stays as it was through all of it and when the agent page closes;
// and twenty checks of a page leave the memory where it was.
//
// The pages come from a small HTTP server inside this process, on
// 127.0.0.1 and ::1 at a port the system picks, so nothing touches the
// network and nothing is left running. It works in a window of its own.
#import "Harness.h"
#import "EditorController.h"
#import "Browser.h"
#import "BrowserPages.h"
#import "ControlCommands.h"
#import "ControlServer.h"
#import <WebKit/WebKit.h>
#include <arpa/inet.h>
#include <libproc.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <objc/message.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

NSString *gDir;    // the suite's folder (the window's)
NSString *gSite;   // what the server serves
NSString *gWid;
int gPort;
int gListen4 = -1, gListen6 = -1;

NSString *CLIPath() { return [MCGuiBuildDir() stringByAppendingPathComponent:@"minicode-cli"]; }
NSString *Pid() { return [NSString stringWithFormat:@"%d", getpid()]; }

void CLI(NSString *name, NSArray<NSString *> *args, void (^check)(int, NSString *, NSString *)) {
    MCGuiAsync(name, 60, ^(MCGuiDone done) {
        NSMutableArray *argv = [NSMutableArray arrayWithObject:CLIPath()];
        [argv addObjectsFromArray:args];
        [argv addObjectsFromArray:@[@"--window", gWid, @"--pid", Pid()]];
        MCGuiRunProcess(argv, nil, ^(int status, NSString *out, NSString *err) {
            if (NSProcessInfo.processInfo.environment[@"MC_GUI_VERBOSE"].length)
                MCGuiNote(@"%@: exit %d, out %@, err %@", name, status,
                          out.length > 600 ? [out substringToIndex:600] : out, err);
            check(status, out, err);
            done();
        });
    });
}

id ParseJSON(NSString *s) {
    NSData *d = [s dataUsingEncoding:NSUTF8StringEncoding];
    return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
}

NSArray<NSDictionary *> *JSONLines(NSString *s) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *line in [s componentsSeparatedByString:@"\n"])
        if (line.length) {
            id o = ParseJSON(line);
            if ([o isKindOfClass:NSDictionary.class]) [out addObject:o];
        }
    return out;
}

NSString *Local(NSString *path) {
    return [NSString stringWithFormat:@"localhost:%d%@", gPort, path];
}

// ------------------------------------------------------- the HTTP server
// HTTP/1.0, one request a connection: a file under gSite, or 404.
void Serve(int fd) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
    }
    std::string path = "/";
    size_t sp = req.find(' ');
    if (sp != std::string::npos) {
        size_t sp2 = req.find(' ', sp + 1);
        path = req.substr(sp + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp - 1);
    }
    path = path.substr(0, path.find('?'));
    if (path == "/") path = "/index.html";
    NSData *body = nil;
    if (path.find("..") == std::string::npos)
        body = [NSData dataWithContentsOfFile:[gSite stringByAppendingString:@(path.c_str())]];
    NSString *type = [@(path.c_str()) hasSuffix:@".js"] ? @"text/javascript" : @"text/html";
    int status = body ? 200 : 404;
    if (!body) body = [@"<html><head><title>Not found</title></head><body>No such page</body></html>"
                       dataUsingEncoding:NSUTF8StringEncoding];
    NSString *head = [NSString stringWithFormat:
        @"HTTP/1.0 %d %@\r\nContent-Type: %@; charset=utf-8\r\nContent-Length: %lu\r\n"
        @"Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, status == 200 ? @"OK" : @"Not Found", type, (unsigned long)body.length];
    NSMutableData *out = [[head dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
    [out appendData:body];
    size_t off = 0;
    while (off < out.length) {
        ssize_t n = write(fd, (const char *)out.bytes + off, out.length - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
}

void AcceptLoop(int listener) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        for (;;) {
            int fd = accept(listener, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                return;   // closed at the end
            }
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ Serve(fd); });
        }
    });
}

bool StartServer() {
    gListen4 = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(gListen4, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a4 = {};
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a4.sin_port = 0;
    if (bind(gListen4, (struct sockaddr *)&a4, sizeof a4) != 0 || listen(gListen4, 64) != 0)
        return false;
    socklen_t len = sizeof a4;
    getsockname(gListen4, (struct sockaddr *)&a4, &len);
    gPort = ntohs(a4.sin_port);
    AcceptLoop(gListen4);
    // The same port on ::1, for "localhost" resolving there first.
    gListen6 = socket(AF_INET6, SOCK_STREAM, 0);
    setsockopt(gListen6, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    setsockopt(gListen6, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on);
    struct sockaddr_in6 a6 = {};
    a6.sin6_family = AF_INET6;
    a6.sin6_addr = in6addr_loopback;
    a6.sin6_port = htons((uint16_t)gPort);
    if (bind(gListen6, (struct sockaddr *)&a6, sizeof a6) == 0 && listen(gListen6, 64) == 0)
        AcceptLoop(gListen6);
    else {
        close(gListen6);
        gListen6 = -1;
    }
    return true;
}

// A port nothing listens on: one the system gave out, closed again.
int ClosedPort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    int port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

void StopServer() {
    if (gListen4 >= 0) { shutdown(gListen4, SHUT_RDWR); close(gListen4); gListen4 = -1; }
    if (gListen6 >= 0) { shutdown(gListen6, SHUT_RDWR); close(gListen6); gListen6 = -1; }
}

// ---------------------------------------------------------------- memory
uint64_t Footprint(pid_t pid) {
    struct rusage_info_v4 ru;
    if (pid <= 0 || proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ru) != 0) return 0;
    return ru.ri_phys_footprint;
}

// WebKit's page process for a web view (a private property, read only
// here, in a test).
pid_t WebProcess(WKWebView *web) {
    SEL sel = NSSelectorFromString(@"_webProcessIdentifier");
    if (!web || ![web respondsToSelector:sel]) return 0;
    return ((pid_t (*)(id, SEL))objc_msgSend)(web, sel);
}

BrowserView *Pane(EditorController *c) { return [c browserPane]; }

// Pixel color at (x, y from the top) of a PNG.
NSColor *PixelAt(NSString *png, NSInteger x, NSInteger yFromTop) {
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:png]];
    if (!rep) return nil;
    return [[rep colorAtX:x y:yFromTop] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
}

NSSize PixelSize(NSString *png) {
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:png]];
    return rep ? NSMakeSize(rep.pixelsWide, rep.pixelsHigh) : NSZeroSize;
}

// WebKit's pictures of #FF0000 read back as about (1, 0.15, 0) here, in
// a snapshot of the visible area and of the whole page alike.
BOOL IsRed(NSColor *c) { return c && c.redComponent > 0.9 && c.greenComponent < 0.3 && c.blueComponent < 0.2; }

}  // namespace

MC_GUI_SUITE(browserpages) {
    __block EditorController *c = nil;
    __block NSTextView *tv = nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"pages"];
    gSite = [MCGuiScratch() stringByAppendingPathComponent:@"site"];
    NSString *shots = [MCGuiScratch() stringByAppendingPathComponent:@"shots"];
    NSString *mine = [gDir stringByAppendingPathComponent:@"mine.html"];
    // Started now: the command lines below are made, port and all, as the
    // steps are queued. It serves files written in the first step.
    const bool serving = StartServer();
    const int closedPort = ClosedPort();

    MCGuiStep(@"a local dev server and a window of the suite's own", ^{
        MCGuiWriteFile([gSite stringByAppendingPathComponent:@"index.html"],
            @"<html><head><title>Dev app</title>\n"
            @"<script src=\"/app.js\"></script></head>\n"
            @"<body><h1>Hello from the dev server</h1>\n"
            @"<p>Second paragraph</p>\n"
            @"<script>\n"
            @"console.log('ready', 42);\n"
            @"console.error(\"x\");\n"
            @"</script>\n"
            @"</body></html>\n");
        MCGuiWriteFile([gSite stringByAppendingPathComponent:@"app.js"],
            @"// the app\n"
            @"console.warn('from app.js');\n"
            @"setTimeout(function () {\n"
            @"  throw new Error('boom');\n"
            @"}, 0);\n");
        MCGuiWriteFile([gSite stringByAppendingPathComponent:@"tall.html"],
            @"<html><head><title>Tall</title><style>body{margin:0}"
            @"#top{height:3000px;background:#00ff00}#end{height:400px;background:#ff0000}</style>"
            @"</head><body><div id=top></div><div id=end></div></body></html>");
        MCGuiWriteFile([gSite stringByAppendingPathComponent:@"huge.html"],
            @"<html><head><title>Huge</title></head><body style=\"margin:0\">"
            @"<div style=\"height:20000px;background:#0000ff\"></div></body></html>");
        MCGuiWriteFile([gSite stringByAppendingPathComponent:@"quiet.html"],
            @"<html><head><title>Quiet</title></head><body>Nothing logged</body></html>");
        MCGuiWriteFile(mine, @"<html><head><title>Mine</title></head><body>The user's own page</body></html>");
        MCGuiWriteFile([gDir stringByAppendingPathComponent:@"notes.txt"], @"notes\n");
        [fm createDirectoryAtPath:shots withIntermediateDirectories:YES attributes:nil error:nil];
        GCHECK(serving);
        MCGuiNote(@"pages served on port %d", gPort);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        gWid = [NSString stringWithFormat:@"%ld", (long)MCControlWindowID(c)];
        tv = [c valueForKey:@"textView"];
        GCHECK([c showFile:[gDir stringByAppendingPathComponent:@"notes.txt"]]);
    });

    // --------------------------------------------------- the user's page
    MCGuiStep(@"the user has a page of their own open", ^{
        [c agentBrowserPane];   // the pane, with nothing loaded (no home page off the Mac)
        [Pane(c) navigateToString:[NSURL fileURLWithPath:mine].absoluteString];
    });
    MCGuiWait(@"it loads", 20, ^BOOL { return [Pane(c).userProbe.title isEqualToString:@"Mine"]; });
    __block NSResponder *keyboard = nil;
    __block uint64_t memBefore = 0;
    MCGuiStep(@"the keyboard is in the editor", ^{
        [c.window makeFirstResponder:tv];
        keyboard = c.window.firstResponder;
        GCHECK(!Pane(c).hasAgentPage);
        memBefore = Footprint(getpid());
    });
    CLI(@"page before any agent page", @[@"page", @"--text"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"no agent page"]);
    });

    // ----------------------------------------------------- browse --wait
    CLI(@"browse localhost --wait prints the status", @[@"browse", Local(@"/"), @"--wait"],
        ^(int s, NSString *out, NSString *) {
        NSDictionary *r = ParseJSON(out);
        GCHECK(s == 0);
        GCHECK([r[@"status"] intValue] == 200);
        GCHECK_EQ_STR(r[@"title"], @"Dev app");
        GCHECK([r[@"url"] hasPrefix:@"http://localhost:"]);
        GCHECK([r[@"loading"] isEqual:@NO]);
    });
    MCGuiStep(@"the agent page shows, the keyboard stayed, the user's page is as it was", ^{
        BrowserView *b = Pane(c);
        GCHECK(b.hasAgentPage && b.agentPageShown);
        GCHECK(c.window.firstResponder == keyboard);
        GCHECK_EQ_STR(b.userProbe.title, @"Mine");
        GCHECK([b.userProbe.url hasPrefix:@"file://"]);
        NSSegmentedControl *sw = [b valueForKey:@"switcher"];
        GCHECK(!sw.hidden && sw.selectedSegment == 1 && [[sw labelForSegment:0] isEqualToString:@"Yours"] &&
               [[sw labelForSegment:1] isEqualToString:@"Agent"]);
        MCGuiSnapshot(b, @"browser-agent-page");
    });
    CLI(@"page --text", @[@"page", @"--text"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [out containsString:@"Hello from the dev server"] &&
               [out containsString:@"Second paragraph"]);
        GCHECK(![out containsString:@"console.error"]);   // visible text, not the source
    });
    MCGuiPause(0.3);   // the setTimeout's error
    CLI(@"page --console has console.error and the uncaught error, with source lines",
        @[@"page", @"--console"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0);
        NSArray<NSDictionary *> *lines = JSONLines(out);
        NSDictionary *err = nil, *uncaught = nil, *warn = nil, *log = nil;
        for (NSDictionary *e in lines) {
            if ([e[@"text"] isEqual:@"x"] && [e[@"level"] isEqual:@"error"]) err = e;
            if ([e[@"uncaught"] boolValue]) uncaught = e;
            if ([e[@"level"] isEqual:@"warn"]) warn = e;
            if ([e[@"level"] isEqual:@"log"]) log = e;
        }
        if (!err || !uncaught || [err[@"line"] intValue] != 7) MCGuiNote(@"console said: %@", out);
        NSString *pageAddress = [NSString stringWithFormat:@"http://localhost:%d/", gPort];
        GCHECK(err && [err[@"source"] isEqual:pageAddress] && [err[@"line"] intValue] == 7);
        GCHECK(uncaught && [uncaught[@"text"] containsString:@"boom"] &&
               [uncaught[@"source"] hasSuffix:@"/app.js"] && [uncaught[@"line"] intValue] == 4);
        GCHECK(warn && [warn[@"source"] hasSuffix:@"/app.js"] && [warn[@"line"] intValue] == 2);
        GCHECK(log && [log[@"text"] isEqual:@"ready 42"]);
        GCHECK([lines.firstObject[@"seq"] intValue] >= 1);
    });
    __block NSString *lastSeq = nil;
    CLI(@"page --url", @[@"page", @"--url"], ^(int s, NSString *out, NSString *) {
        NSDictionary *r = ParseJSON(out);
        GCHECK(s == 0 && [r[@"status"] intValue] == 200 && [r[@"title"] isEqual:@"Dev app"] &&
               [r[@"page"] isEqual:@"agent"]);
        lastSeq = [r[@"console_seq"] stringValue];
        GCHECK(lastSeq.intValue >= 4);
    });
    // The test's own script, in the page's world, as the page would look.
    MCGuiAsync(@"the page cannot reach the record: no message handler in its world", 10, ^(MCGuiDone done) {
        WKWebView *web = [Pane(c) valueForKey:@"agentWeb"];
        [web evaluateJavaScript:@"!(window.webkit && window.webkit.messageHandlers && "
                                @"window.webkit.messageHandlers.minicodeConsole)"
                        inFrame:nil inContentWorld:WKContentWorld.pageWorld
              completionHandler:^(id v, NSError *e) {
            GCHECK(!e && [v boolValue]);
            done();
        }];
    });
    CLI(@"page --console --since gives only what came after", @[@"page", @"--console", @"--since", @"3"],
        ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0);
        for (NSDictionary *e in JSONLines(out)) GCHECK([e[@"seq"] intValue] > 3);
    });

    // ------------------------------------------------------ screenshots
    NSString *shot = [shots stringByAppendingPathComponent:@"visible.png"];
    CLI(@"page --screenshot writes the visible area", @[@"page", @"--screenshot", shot],
        ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [[out stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet]
                             isEqualToString:shot]);
        NSSize px = PixelSize(shot);
        WKWebView *web = [Pane(c) valueForKey:@"agentWeb"];
        GCHECK(px.width > 0 && px.height > 0);
        // As tall as the page on show, at the window's scale.
        GCHECK(fabs(px.height / px.width - web.bounds.size.height / web.bounds.size.width) < 0.02);
    });
    CLI(@"a screenshot is a PNG", @[@"page", @"--screenshot", [shots stringByAppendingPathComponent:@"a.jpg"]],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@".png"]);
    });
    CLI(@"browse the tall page", @[@"browse", Local(@"/tall.html"), @"--wait"],
        ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [ParseJSON(out)[@"title"] isEqual:@"Tall"]);
    });
    NSString *full = [shots stringByAppendingPathComponent:@"full.png"];
    CLI(@"page --screenshot --full takes the whole page", @[@"page", @"--screenshot", full, @"--full"],
        ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        NSSize px = PixelSize(full);
        WKWebView *web = [Pane(c) valueForKey:@"agentWeb"];
        // The app draws at the window's scale (1 on CI's runners, 2 here).
        const double scale = web.window.backingScaleFactor > 0 ? web.window.backingScaleFactor : 2;
        GCHECK(fabs(px.height - 3400 * scale) <= scale);
        // As wide as the page, which is the view less a scroll bar where
        // the system shows them always (no trackpad, as on CI: 15 points).
        GCHECK(px.width <= web.bounds.size.width * scale + 1 &&
               px.width >= (web.bounds.size.width - 20) * scale);
        GCHECK(IsRed(PixelAt(full, 10, (NSInteger)px.height - 10)));      // the bottom block
        NSColor *middle = PixelAt(full, 10, (NSInteger)(1500 * scale));   // below the visible area
        GCHECK(middle && middle.greenComponent > 0.9 && middle.redComponent < 0.1);
        NSColor *top = PixelAt(full, 10, 10);
        GCHECK(top && top.greenComponent > 0.9 && top.redComponent < 0.1);
        MCGuiNote(@"full page: %.0f x %.0f px", px.width, px.height);
    });

    CLI(@"a very tall page", @[@"browse", Local(@"/huge.html"), @"--wait"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
    });
    NSString *huge = [shots stringByAppendingPathComponent:@"huge.png"];
    CLI(@"is cut at 16,384 pixels", @[@"page", @"--screenshot", huge, @"--full"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
        NSSize px = PixelSize(huge);
        WKWebView *web = [Pane(c) valueForKey:@"agentWeb"];
        const double scale = web.window.backingScaleFactor > 0 ? web.window.backingScaleFactor : 2;
        GCHECK(px.height <= 16384 && px.height >= 16384 - scale);
        // WebKit ends a PDF page at 14,400 points and goes on in a second:
        // at 1x the picture reaches into it, and must be drawn there too.
        NSColor *bottom = PixelAt(huge, 10, (NSInteger)px.height - 2);
        GCHECK(bottom && bottom.blueComponent > 0.9 && bottom.redComponent < 0.1);
        MCGuiNote(@"huge page: %.0f x %.0f px at %.0fx", px.width, px.height, scale);
        [fm removeItemAtPath:huge error:nil];
    });

    // ------------------------------------------------- status, failures
    CLI(@"a missing page's status", @[@"browse", Local(@"/nowhere.html"), @"--wait"],
        ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [ParseJSON(out)[@"status"] intValue] == 404);
    });
    CLI(@"a server that is not there", @[@"browse", [NSString stringWithFormat:@"127.0.0.1:%d", closedPort], @"--wait"],
        ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"did not load"]);
    });
    CLI(@"never a search", @[@"browse", @"how to center a div"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"never searches"]);
    });
    CLI(@"never a script", @[@"browse", @"javascript:alert(1)"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"javascript:"]);
    });

    // ------------------------------------------------- the origin limit
    CLI(@"a page that is not local", @[@"browse", @"about:blank", @"--wait"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
    });
    for (NSArray *args in @[@[@"page", @"--text"], @[@"page", @"--console"],
                            @[@"page", @"--screenshot", [shots stringByAppendingPathComponent:@"no.png"]]]) {
        CLI([NSString stringWithFormat:@"%@ is refused there", args[1]], args,
            ^(int s, NSString *, NSString *err) {
            GCHECK(s == 1 && [err containsString:@"control.page-any-origin = true"]);
        });
    }
    MCGuiStep(@"no picture was written", ^{
        GCHECK(![fm fileExistsAtPath:[shots stringByAppendingPathComponent:@"no.png"]]);
    });
    CLI(@"its address may be read", @[@"page", @"--url"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [ParseJSON(out)[@"url"] isEqual:@"about:blank"]);
    });
    MCGuiStep(@"the setting lifts it", ^{ MCGuiSetSettings(@"lsp.enabled = false\ncontrol.page-any-origin = true\n"); });
    CLI(@"with the setting on", @[@"page", @"--text"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
    });
    MCGuiStep(@"the setting off again", ^{ MCGuiSetSettings(@"lsp.enabled = false\n"); });

    // ------------------------------------------------------- --yours
    CLI(@"--yours reads the user's page", @[@"page", @"--text", @"--yours"],
        ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [out containsString:@"The user's own page"]);
    });
    CLI(@"--yours --url", @[@"page", @"--url", @"--yours"], ^(int s, NSString *out, NSString *) {
        NSDictionary *r = ParseJSON(out);
        GCHECK(s == 0 && [r[@"title"] isEqual:@"Mine"] && [r[@"page"] isEqual:@"yours"]);
    });
    CLI(@"--yours has no console", @[@"page", @"--console", @"--yours"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"agent page only"]);
    });

    // ----------------------------------------------- reload and state
    CLI(@"back to the dev server", @[@"browse", Local(@"/"), @"--wait"], ^(int s, NSString *, NSString *) {
        GCHECK(s == 0);
    });
    CLI(@"page --reload --wait", @[@"page", @"--reload", @"--wait"], ^(int s, NSString *out, NSString *) {
        NSDictionary *r = ParseJSON(out);
        GCHECK(s == 0 && [r[@"status"] intValue] == 200 && [r[@"title"] isEqual:@"Dev app"]);
    });
    MCGuiAsync(@"state lists both pages", 30, ^(MCGuiDone done) {
      MCGuiRunProcess(@[CLIPath(), @"state", @"--pid", Pid()], nil, ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0);
        NSDictionary *w = nil;
        for (NSDictionary *x in ParseJSON(out)[@"windows"])
            if ([x[@"id"] intValue] == gWid.intValue) w = x;
        NSArray *pages = w[@"browser"];
        GCHECK(pages.count == 2);
        GCHECK([pages[0][@"page"] isEqual:@"yours"] && [pages[0][@"title"] isEqual:@"Mine"] &&
               ![pages[0][@"shown"] boolValue]);
        GCHECK([pages[1][@"page"] isEqual:@"agent"] && [pages[1][@"status"] intValue] == 200 &&
               [pages[1][@"shown"] boolValue]);
        done();
      });
    });

    // --------------------------------------- the user using their page
    MCGuiStep(@"the user picks Yours and types in the address bar", ^{
        BrowserView *b = Pane(c);
        NSSegmentedControl *sw = [b valueForKey:@"switcher"];
        sw.selectedSegment = 0;
        [sw sendAction:sw.action to:sw.target];
        GCHECK(!b.agentPageShown && [b.pageTitle isEqualToString:@"Mine"]);
        [b focusURLBar];
        keyboard = c.window.firstResponder;
    });
    CLI(@"an agent's page then loads behind the switcher", @[@"browse", Local(@"/quiet.html"), @"--wait"],
        ^(int s, NSString *out, NSString *err) {
        GCHECK(s == 0 && [ParseJSON(out)[@"title"] isEqual:@"Quiet"]);
        GCHECK([err containsString:@"using the browser pane"]);
        GCHECK(!Pane(c).agentPageShown && [Pane(c).pageTitle isEqualToString:@"Mine"]);
        GCHECK(c.window.firstResponder == keyboard);
    });
    CLI(@"and can still be read", @[@"page", @"--text"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && [out containsString:@"Nothing logged"]);
    });
    CLI(@"a new load starts a new record", @[@"page", @"--console"], ^(int s, NSString *out, NSString *) {
        GCHECK(s == 0 && JSONLines(out).count == 0);
    });
    NSString *behind = [shots stringByAppendingPathComponent:@"behind.png"];
    CLI(@"and a picture of it taken behind the switcher", @[@"page", @"--screenshot", behind],
        ^(int s, NSString *, NSString *err) {
        if (s != 0) MCGuiNote(@"behind the switcher: %@", err);
        GCHECK(s == 0 && PixelSize(behind).width > 0);
    });

    // ---------------------------------------------------- memory
    __block uint64_t memStart = 0, webStart = 0;
    __block pid_t webPid = 0;
    MCGuiStep(@"the keyboard back in the editor; memory before twenty checks", ^{
        [c.window makeFirstResponder:tv];
        webPid = WebProcess([Pane(c) valueForKey:@"agentWeb"]);
        memStart = Footprint(getpid());
        webStart = Footprint(webPid);
    });
    for (int i = 0; i < 20; i++) {
        NSString *png = [shots stringByAppendingPathComponent:[NSString stringWithFormat:@"check%d.png", i]];
        MCGuiAsync([NSString stringWithFormat:@"check %d: browse, text, console, screenshot", i + 1], 60,
                   ^(MCGuiDone done) {
            NSArray *base = @[@"--window", gWid, @"--pid", Pid()];
            MCGuiRunProcess([@[CLIPath(), @"browse", Local(@"/"), @"--wait"] arrayByAddingObjectsFromArray:base], nil,
                            ^(int s1, NSString *, NSString *) {
                MCGuiRunProcess([@[CLIPath(), @"page", @"--text"] arrayByAddingObjectsFromArray:base], nil,
                                ^(int s2, NSString *text, NSString *) {
                    MCGuiRunProcess([@[CLIPath(), @"page", @"--console"] arrayByAddingObjectsFromArray:base], nil,
                                    ^(int s3, NSString *, NSString *) {
                        MCGuiRunProcess([@[CLIPath(), @"page", @"--screenshot", png]
                                            arrayByAddingObjectsFromArray:base], nil,
                                        ^(int s4, NSString *, NSString *) {
                            GCHECK(s1 == 0 && s2 == 0 && s3 == 0 && s4 == 0 &&
                                   [text containsString:@"Hello from the dev server"]);
                            [fm removeItemAtPath:png error:nil];
                            done();
                        });
                    });
                });
            });
        });
    }
    MCGuiPause(1.0);
    MCGuiStep(@"memory after twenty checks grew by less than 50 MB", ^{
        const uint64_t memEnd = Footprint(getpid());
        const pid_t pidNow = WebProcess([Pane(c) valueForKey:@"agentWeb"]);
        const uint64_t webEnd = Footprint(pidNow);
        const double app = ((double)memEnd - (double)memStart) / 1048576.0;
        const double web = pidNow == webPid ? ((double)webEnd - (double)webStart) / 1048576.0 : 0;
        MCGuiNote(@"memory: minicode %.1f MB -> %.1f MB (%+.1f), the agent page's WebKit process "
                  @"(pid %d) %.1f MB -> %.1f MB (%+.1f); %.1f MB before the agent page existed",
                  memStart / 1048576.0, memEnd / 1048576.0, app, (int)pidNow, webStart / 1048576.0,
                  webEnd / 1048576.0, web, memBefore / 1048576.0);
        GCHECK(memStart > 0 && memEnd > 0);
        GCHECK(app + web < 50);
        GCHECK(pidNow == webPid);   // one page process, kept, not one per check
    });

    // ------------------------------------------------ closing the agent page
    MCGuiStep(@"closing the agent page shows the user's page as it was", ^{
        BrowserView *b = Pane(c);
        NSButton *x = [b valueForKey:@"closeAgent"];
        GCHECK(!x.hidden);
        [x performClick:nil];
        GCHECK(!b.hasAgentPage && !b.agentPageShown);
        GCHECK([b.pageTitle isEqualToString:@"Mine"] && b.showsUserPage);
        GCHECK(((NSSegmentedControl *)[b valueForKey:@"switcher"]).hidden);
        GCHECK(c.window.firstResponder == tv);
    });
    CLI(@"no agent page now", @[@"page", @"--url"], ^(int s, NSString *, NSString *err) {
        GCHECK(s == 1 && [err containsString:@"no agent page"]);
    });
    MCGuiStep(@"the session keeps the user's page only", ^{
        NSDictionary *st = [Pane(c) savedState];
        GCHECK([st[@"url"] hasPrefix:@"file://"] && [st[@"url"] hasSuffix:@"mine.html"]);
    });

    MCGuiStep(@"the server stops and the suite's window closes", ^{
        StopServer();
        [c.window close];
        c = nil;
        tv = nil;
        MCGuiSetKeyWindow(nil);
    });
}
