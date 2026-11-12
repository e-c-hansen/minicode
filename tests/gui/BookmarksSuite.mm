// BookmarksSuite.mm — `make guitest SUITES=bookmarks`: web bookmarks kept
// as files (WebBookmark.h, Bookmarks.h, EditorController+Bookmarks.mm). A
// .url or .webloc file (XML or binary) clicked in the tree loads in the
// window's browser, once per address; one with an address it may not open
// (javascript:) opens as text, and Open as Text shows any of them so. A
// folder's Open All in Browser Tabs opens its bookmarks in name order.
// Save Page as Bookmark writes a .url file named for the page into the
// bookmark folder (browser.bookmarks), made on first use, a number added
// when the name is taken; Cmd+D does it while the page has the keyboard
// and stays Add Next Occurrence in the editor. Save All Tabs writes a
// folder of them. Bookmarks and web addresses opened as Finder and the
// launcher open them (application:openURLs:, a "web" request file) go to
// the browser window, the window on the bookmark folder, made when
// missing, the page over the editor's side. The launcher sorts addresses
// from paths and refuses what is no web address.
//
// Pages come from an HTTP server inside the test process on 127.0.0.1;
// nothing leaves the Mac. It works in windows of its own.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "Bookmarks.h"
#import "Browser.h"
#import "ControlServer.h"
#import <Carbon/Carbon.h>
#import <WebKit/WebKit.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "WebBookmark.h"

namespace {

int gListen = -1;
int gPort = 0;

// HTTP/1.0, one request a connection: a page titled for its name
// ("/one.html" is "One"), whatever the name.
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
    std::string name = path.substr(path.rfind('/') + 1);
    name = name.substr(0, name.find('.'));
    if (!name.empty()) name[0] = (char)toupper((unsigned char)name[0]);
    const std::string body = "<html><head><title>" + name + "</title></head><body><p>" + name +
                             " page</p></body></html>";
    const std::string out = "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                            "Content-Length: " + std::to_string(body.size()) +
                            "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = write(fd, out.data() + off, out.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
}

bool StartServer() {
    gListen = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(gListen, (struct sockaddr *)&a, sizeof a) != 0 || listen(gListen, 64) != 0)
        return false;
    socklen_t len = sizeof a;
    getsockname(gListen, (struct sockaddr *)&a, &len);
    gPort = ntohs(a.sin_port);
    const int listener = gListen;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        for (;;) {
            int fd = accept(listener, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                return;   // closed at the end
            }
            int nosig = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof nosig);
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ Serve(fd); });
        }
    });
    return true;
}

void StopServer() {
    if (gListen >= 0) {
        shutdown(gListen, SHUT_RDWR);
        close(gListen);
        gListen = -1;
    }
}

NSString *Page(NSString *name) {
    return [NSString stringWithFormat:@"http://127.0.0.1:%d/%@.html", gPort, name];
}

WKWebView *ShownWeb(BrowserView *b) { return [b valueForKey:@"web"]; }
BOOL Loaded(BrowserView *b, NSString *title) {
    return [ShownWeb(b).title ?: @"" isEqualToString:title] && !ShownWeb(b).isLoading;
}
BOOL InPage(EditorController *c) {
    NSResponder *r = c.window.firstResponder;
    WKWebView *w = ShownWeb(c.browser);
    return w && [r isKindOfClass:NSView.class] && [(NSView *)r isDescendantOf:w];
}

CGEventFlags CGFlagsOf(NSEventModifierFlags flags) {
    CGEventFlags f = 0;
    if (flags & NSEventModifierFlagCommand) f |= kCGEventFlagMaskCommand;
    if (flags & NSEventModifierFlagShift) f |= kCGEventFlagMaskShift;
    return f;
}

// A key made as the hardware's are, through [NSApp sendEvent:]: the key
// monitor, the window's views, the menus.
void Key(NSWindow *w, CGKeyCode code, NSEventModifierFlags flags) {
    CGEventRef cg = CGEventCreateKeyboardEvent(NULL, code, true);
    CGEventSetFlags(cg, CGFlagsOf(flags));
    NSEvent *e0 = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                             modifierFlags:e0.modifierFlags
                                 timestamp:NSProcessInfo.processInfo.systemUptime
                              windowNumber:w.windowNumber context:nil
                                characters:e0.characters
               charactersIgnoringModifiers:e0.charactersIgnoringModifiers
                                 isARepeat:NO keyCode:code];
    [NSApp sendEvent:e];
}

NSString *Read(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

NSArray<NSString *> *Names(NSString *dir) {
    return [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]
        sortedArrayUsingSelector:@selector(compare:)];
}

EditorController *BrowserWindow() {
    for (EditorController *c in MCGuiControllers())
        if (c.isBrowserWindow) return c;
    return nil;
}

void OpenURLs(NSArray<NSURL *> *urls) {
    [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp openURLs:urls];
}

}  // namespace

MC_GUI_SUITE(bookmarks) {
    __block EditorController *c = nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *proj = [MCGuiScratch() stringByAppendingPathComponent:@"bm-project"];
    NSString *links = [proj stringByAppendingPathComponent:@"links"];
    NSString *bm = [MCGuiScratch() stringByAppendingPathComponent:@"My Bookmarks"];
    NSString *one = [links stringByAppendingPathComponent:@"one.url"];
    NSString *two = [links stringByAppendingPathComponent:@"two.webloc"];
    NSString *three = [links stringByAppendingPathComponent:@"three.webloc"];
    NSString *bad = [links stringByAppendingPathComponent:@"bad.url"];
    NSString *notes = [proj stringByAppendingPathComponent:@"notes.txt"];
    const bool serving = StartServer();

    MCGuiStep(@"bookmarks of each kind, the setting, and a window of the suite's own", ^{
        GCHECK(serving);
        MCGuiWriteFile(one, [NSString stringWithFormat:@"[InternetShortcut]\r\nURL=%@\r\n",
                                                       Page(@"one")]);
        MCGuiWriteFile(two, [NSString stringWithFormat:
            @"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
            @"\t<key>URL</key>\n\t<string>%@</string>\n</dict>\n</plist>\n", Page(@"two")]);
        NSData *binary = [NSPropertyListSerialization
            dataWithPropertyList:@{@"URL": Page(@"three")}
                          format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
        GCHECK([binary writeToFile:three atomically:YES]);
        MCGuiWriteFile(bad, @"[InternetShortcut]\nURL=javascript:alert(1)\n");
        MCGuiWriteFile(notes, @"alpha beta alpha\n");
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\n"
                                                    @"browser.bookmarks = %@\n", bm]);
        GCHECK([MCBookmarksFolder() isEqualToString:bm]);
        GCHECK(![fm fileExistsAtPath:bm]);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:proj];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        GCHECK(!c.isBrowserWindow);
        GCHECK(MCBookmarkAddress(three) && [MCBookmarkAddress(three) isEqualToString:Page(@"three")]);
        GCHECK(MCBookmarkAddress(bad) == nil);
    });

    MCGuiStep(@"a click on a bookmark in the tree loads it in the browser, not a tab", ^{
        [c openFileAtPath:one from:EditorTabs::treeClick(1, false, false)];
        GCHECK(c.browser != nil && c.browserVisible);
        GCHECK([c.browser.tabAddresses containsObject:Page(@"one")]);
        GCHECK(![c.currentPath isEqualToString:one]);
        GCHECK(!c.editorHidden);   // a code window keeps its editor beside the page
    });
    MCGuiWait(@"the page loads", 20, ^BOOL { return Loaded(c.browser, @"One"); });

    MCGuiStep(@"a second click shows the same tab; a bad address opens as text", ^{
        const NSUInteger tabs = c.browser.tabCount;
        [c openFileAtPath:one from:EditorTabs::treeClick(1, false, false)];
        GCHECK(c.browser.tabCount == tabs);
        [c openFileAtPath:bad from:EditorTabs::treeClick(1, false, false)];
        GCHECK([c.currentPath isEqualToString:bad]);
        GCHECK(c.browser.tabCount == tabs);
    });

    MCGuiStep(@"Open as Text shows a bookmark's own text", ^{
        [c revealPath:one andOpen:NO];
        GCHECK(c.canOpenBookmarkAsText);
        GCHECK(!c.canOpenBookmarksInTabs);   // a file, not a folder
        [c openBookmarkAsText:nil];
        GCHECK([c.currentPath isEqualToString:one]);
        GCHECK([c.sourceText containsString:@"[InternetShortcut]"]);
    });

    MCGuiStep(@"Open All in Browser Tabs opens a folder's bookmarks in name order", ^{
        [c revealPath:links andOpen:NO];
        GCHECK(c.canOpenBookmarksInTabs);
        GCHECK(!c.canOpenBookmarkAsText);
        [c openBookmarksInTabs:nil];
        NSArray<NSString *> *a = c.browser.tabAddresses;
        const NSUInteger i1 = [a indexOfObject:Page(@"one")];
        const NSUInteger i3 = [a indexOfObject:Page(@"three")];
        const NSUInteger i2 = [a indexOfObject:Page(@"two")];
        GCHECK(i1 != NSNotFound && i2 != NSNotFound && i3 != NSNotFound);
        GCHECK(i1 < i3 && i3 < i2);   // one.url, three.webloc, two.webloc
        GCHECK(a.count == 3);
        GCHECK(c.browser.selectedTab == (NSInteger)i1);   // the first, already open, shows
        GCHECK(InPage(c));
        // A folder without bookmarks offers nothing.
        [c revealPath:notes andOpen:NO];
        GCHECK(!c.canOpenBookmarksInTabs);
    });

    MCGuiStep(@"Save Page as Bookmark makes the folder and writes the page there", ^{
        GCHECK(c.canSaveBrowserBookmark);
        [NSApp sendAction:@selector(saveBrowserBookmark:) to:nil from:nil];
        NSString *saved = [bm stringByAppendingPathComponent:@"One.url"];
        GCHECK([fm fileExistsAtPath:saved]);
        GCHECK([Read(saved) isEqualToString:
                    @(WebBookmark::shortcutText(Page(@"one").UTF8String).c_str())]);
        GCHECK([MCBookmarkAddress(saved) isEqualToString:Page(@"one")]);
        // Again: a second file, never over the first.
        [NSApp sendAction:@selector(saveBrowserBookmark:) to:nil from:nil];
        GCHECK([fm fileExistsAtPath:[bm stringByAppendingPathComponent:@"One 2.url"]]);
    });

    MCGuiStep(@"Cmd+D in the page saves it; in the editor it is Add Next Occurrence", ^{
        [c.browser focusPage];
        GCHECK(InPage(c));
        Key(c.window, kVK_ANSI_D, NSEventModifierFlagCommand);
        GCHECK([fm fileExistsAtPath:[bm stringByAppendingPathComponent:@"One 3.url"]]);
        GCHECK([c showFile:notes]);
        [c.window makeFirstResponder:c.textView];
        c.textView.selectedRange = NSMakeRange(0, 5);   // "alpha"
        const NSUInteger before = Names(bm).count;
        Key(c.window, kVK_ANSI_D, NSEventModifierFlagCommand);
        GCHECK(Names(bm).count == before);
        GCHECK(c.textView.selectedRanges.count == 2);   // both "alpha"s
    });

    MCGuiStep(@"Save All Tabs writes a folder with a bookmark a tab", ^{
        GCHECK(c.canSaveBrowserTabs);
        NSArray *before = Names(bm);
        [NSApp sendAction:@selector(saveBrowserTabsAsBookmarks:) to:nil from:nil];
        NSString *folder = nil;
        for (NSString *n in Names(bm))
            if (![before containsObject:n]) folder = [bm stringByAppendingPathComponent:n];
        GCHECK([folder.lastPathComponent hasPrefix:@"Tabs "]);
        NSMutableSet *addresses = [NSMutableSet set];
        for (NSString *n in Names(folder)) {
            NSString *u = MCBookmarkAddress([folder stringByAppendingPathComponent:n]);
            if (u) [addresses addObject:u];
        }
        GCHECK(Names(folder).count == 3);
        GCHECK([addresses isEqual:([NSSet setWithArray:@[Page(@"one"), Page(@"two"),
                                                         Page(@"three")]])]);
    });

    MCGuiStep(@"bookmarks opened from Finder go to the browser window, as tabs", ^{
        GCHECK(BrowserWindow() == nil);
        const NSUInteger windows = MCGuiControllers().count;
        OpenURLs(@[[NSURL fileURLWithPath:two], [NSURL fileURLWithPath:three],
                   [NSURL fileURLWithPath:notes]]);
        EditorController *b = BrowserWindow();
        GCHECK(b != nil && b != c);
        GCHECK(MCGuiControllers().count == windows + 1);
        NSArray<NSString *> *a = b.browser.tabAddresses;
        GCHECK(a.count == 2 && [a[0] isEqualToString:Page(@"two")] &&
               [a[1] isEqualToString:Page(@"three")]);
        GCHECK(b.browserVisible && b.editorHidden);
        // The plain file went to the window whose folder holds it.
        GCHECK([c.currentPath isEqualToString:notes]);
    });
    MCGuiWait(@"the browser window's first page loads", 20, ^BOOL {
        return Loaded(BrowserWindow().browser, @"Two");
    });

    MCGuiStep(@"a web request (minicode https://...) joins the browser window", ^{
        EditorController *b = BrowserWindow();
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"MiniCode-open"];
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *req = [dir stringByAppendingPathComponent:@"suite.minicode-open"];
        NSString *body = [NSString stringWithFormat:@"minicode-open-1%Cweb%C%@%Cjavascript:x%C%@%C",
                                                    (unichar)0, (unichar)0, Page(@"four"),
                                                    (unichar)0, (unichar)0, Page(@"two"), (unichar)0];
        GCHECK([[body dataUsingEncoding:NSUTF8StringEncoding] writeToFile:req atomically:YES]);
        const NSUInteger windows = MCGuiControllers().count;
        OpenURLs(@[[NSURL fileURLWithPath:req]]);
        GCHECK(![fm fileExistsAtPath:req]);   // read once and removed
        GCHECK(MCGuiControllers().count == windows);
        NSArray<NSString *> *a = b.browser.tabAddresses;
        GCHECK(a.count == 3);   // four added; two was open already; javascript: never
        GCHECK([a containsObject:Page(@"four")]);
        GCHECK([b.browser.tabAddresses[(NSUInteger)b.browser.selectedTab]
                   isEqualToString:Page(@"four")]);
    });

    MCGuiStep(@"in the browser window a bookmark saves to the tree's folder", ^{
        EditorController *b = BrowserWindow();
        NSString *sub = [bm stringByAppendingPathComponent:@"Reading"];
        GCHECK([fm createDirectoryAtPath:sub withIntermediateDirectories:NO attributes:nil
                                   error:nil]);
        [b refreshTree:nil];
        [b revealPath:sub andOpen:NO];
        MCGuiSetKeyWindow(b.window);
        [b saveBrowserBookmark:nil];
        NSArray *names = Names(sub);
        GCHECK(names.count == 1 && [names.firstObject hasSuffix:@".url"]);
        // A file opened beside it brings the editor back.
        [b openFileAtPath:[sub stringByAppendingPathComponent:names.firstObject]];
        GCHECK(b.browserVisible);
        MCGuiSetKeyWindow(c.window);
    });

    __block NSString *launcherTmp = nil;
    MCGuiAsync(@"the launcher sends web addresses as a request of their own", 30,
               ^(MCGuiDone done) {
        launcherTmp = [MCGuiScratch() stringByAppendingPathComponent:@"launcher-tmp"];
        [fm createDirectoryAtPath:launcherTmp withIntermediateDirectories:YES attributes:nil
                            error:nil];
        // The checkout's copy, which is not executable (make copies it into
        // the bundle).
        NSString *launcher = [[MCGuiBuildDir() stringByDeletingLastPathComponent]
            stringByAppendingPathComponent:@"scripts/minicode-launcher"];
        MCGuiRunProcess(@[@"/bin/bash", launcher, @"https://a.example/x", notes, @"HTTP://b.example"],
                        @{@"TMPDIR": launcherTmp, @"MINICODE_LAUNCHER_DRYRUN": @"1"},
                        ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            GCHECK(err.length == 0);
            GCHECK([out containsString:notes]);
            NSString *dir = [launcherTmp stringByAppendingPathComponent:@"MiniCode-open"];
            NSArray *reqs = Names(dir);
            GCHECK(reqs.count == 1);
            NSData *d = [NSData dataWithContentsOfFile:
                                    [dir stringByAppendingPathComponent:reqs.firstObject]];
            NSString *t = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
            NSString *want = [NSString stringWithFormat:@"minicode-open-1%Cweb%Chttps://a.example/x%C"
                                                        @"HTTP://b.example%C",
                                                        (unichar)0, (unichar)0, (unichar)0, (unichar)0];
            GCHECK([t isEqualToString:want]);
            GCHECK([out containsString:reqs.firstObject]);
            done();
        });
    });
    MCGuiAsync(@"and refuses what is no web address", 30, ^(MCGuiDone done) {
        // The checkout's copy, which is not executable (make copies it into
        // the bundle).
        NSString *launcher = [[MCGuiBuildDir() stringByDeletingLastPathComponent]
            stringByAppendingPathComponent:@"scripts/minicode-launcher"];
        MCGuiRunProcess(@[@"/bin/bash", launcher, @"https://exa mple.com", @"https:///x"],
                        @{@"TMPDIR": launcherTmp, @"MINICODE_LAUNCHER_DRYRUN": @"1"},
                        ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 1);
            GCHECK(out.length == 0);
            GCHECK([err containsString:@"not a web address"]);
            done();
        });
    });

    MCGuiStep(@"the suite's windows close", ^{
        StopServer();
        [BrowserWindow().window close];
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });
}
