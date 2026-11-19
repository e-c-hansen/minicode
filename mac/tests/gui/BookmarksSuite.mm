// BookmarksSuite.mm — `make guitest SUITES=bookmarks`: web bookmarks kept
// as files (WebBookmark.h, Bookmarks.h, EditorController+Bookmarks.mm). A
// .url or .webloc file (XML or binary) clicked in the tree loads in the
// window's browser, once per address; one with an address it may not open
// (javascript:) opens as text, and Open as Text shows any of them so. A
// folder's Open All in Browser Tabs opens its bookmarks in name order.
// Save Page as Bookmark writes a .url file named for the page into the
// workspace's .mutible/bookmarks, made on first use (and nothing in the
// shared folder, browser.bookmarks), and only once a page (saving again
// says where it is); Cmd+D does it while the page has the keyboard and
// stays Add Next Occurrence in the editor. The toolbar's star fills once
// the page on show is bookmarked, and a click on it then moves the
// bookmark to the Trash. Save All Tabs writes a folder of them. The
// toolbar's book and the menu bar's Bookmarks menu list the workspace's
// bookmarks, its folders as submenus, and not another workspace's; a
// bookmark loads in the tab on show, a folder's Open All in Tabs opens
// each. The shared folder's bookmarks show under Shared, a page there is
// already saved, and Move to This Workspace moves the file into the
// workspace's folder. Bookmarks and web addresses opened as Finder and the
// launcher open them (application:openURLs:, a "web" request file) go to
// the browser window, the window on the bookmark folder, made when
// missing, the page over the editor's side; it lists and saves in the
// shared folder itself, with no Shared part. The launcher sorts addresses
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
#import <objc/runtime.h>
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

// The Trash is a folder in the scratch world (the real one is the user's).
NSString *ScratchTrash() { return [MCGuiScratch() stringByAppendingPathComponent:@"BmTrash"]; }
IMP gRealTrash;
void UseScratchTrash(BOOL on) {
    Method m = class_getInstanceMethod(NSFileManager.class,
                                       @selector(trashItemAtURL:resultingItemURL:error:));
    if (!on) {
        if (gRealTrash) method_setImplementation(m, gRealTrash);
        gRealTrash = nil;
        return;
    }
    gRealTrash = method_setImplementation(m, imp_implementationWithBlock(
        ^BOOL(NSFileManager *fm, NSURL *url, NSURL *__autoreleasing *out,
              NSError *__autoreleasing *err) {
            [fm createDirectoryAtPath:ScratchTrash() withIntermediateDirectories:YES
                           attributes:nil error:nil];
            NSString *to = [ScratchTrash() stringByAppendingPathComponent:url.lastPathComponent];
            BOOL ok = [fm moveItemAtPath:url.path toPath:to error:err];
            if (ok && out) *out = [NSURL fileURLWithPath:to];
            return ok;
        }));
}

// The menu filled as it would be on opening, and its item titled `title`.
NSMenuItem *MenuItem(NSMenu *menu, NSString *title) {
    [menu.delegate menuNeedsUpdate:menu];
    for (NSMenuItem *it in menu.itemArray)
        if ([it.title isEqualToString:title]) return it;
    return nil;
}

NSMenu *MenuBarBookmarks() {
    for (NSMenuItem *it in NSApp.mainMenu.itemArray)
        if ([it.submenu.title isEqualToString:@"Bookmarks"]) return it.submenu;
    return nil;
}

NSString *ShownAddress(BrowserView *b) {
    return b.selectedTab >= 0 ? b.tabAddresses[(NSUInteger)b.selectedTab] : nil;
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
    // The workspace's own bookmark folder, and another workspace's.
    NSString *ws = [proj stringByAppendingPathComponent:@".mutible/bookmarks"];
    NSString *proj2 = [MCGuiScratch() stringByAppendingPathComponent:@"bm-other"];
    NSString *ws2 = [proj2 stringByAppendingPathComponent:@".mutible/bookmarks"];
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
        MCGuiWriteFile([ws2 stringByAppendingPathComponent:@"Other.url"],
                       [NSString stringWithFormat:@"[InternetShortcut]\nURL=%@\n", Page(@"other")]);
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\n"
                                                    @"browser.bookmarks = %@\n", bm]);
        GCHECK([MCBookmarksFolder() isEqualToString:bm]);
        GCHECK(![fm fileExistsAtPath:bm]);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:proj];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        GCHECK(!c.isBrowserWindow);
        // A window on a folder saves in its .mutible/bookmarks and lists
        // the bookmark folder as Shared.
        GCHECK([MCSaveBookmarksFolder(c.rootPath).stringByResolvingSymlinksInPath
                   isEqualToString:ws.stringByResolvingSymlinksInPath]);
        GCHECK([MCSharedBookmarksFolder(c.rootPath) isEqualToString:bm]);
        GCHECK(MCSharedBookmarksFolder(bm) == nil);
        GCHECK([MCSaveBookmarksFolder(bm) isEqualToString:bm]);
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

    MCGuiStep(@"Save Page as Bookmark makes the workspace's folder and writes the page there", ^{
        GCHECK(c.canSaveBrowserBookmark);
        GCHECK(![fm fileExistsAtPath:ws]);
        [NSApp sendAction:@selector(saveBrowserBookmark:) to:nil from:nil];
        NSString *saved = [ws stringByAppendingPathComponent:@"One.url"];
        GCHECK([fm fileExistsAtPath:saved]);
        GCHECK([Read(saved) isEqualToString:
                    @(WebBookmark::shortcutText(Page(@"one").UTF8String).c_str())]);
        GCHECK([MCBookmarkAddress(saved) isEqualToString:Page(@"one")]);
        GCHECK([MCBookmarkFileFor(ws, Page(@"one")) isEqualToString:saved]);
        NSString *note = [[c valueForKey:@"statusLabel"] stringValue];
        MCGuiNote(@"status: %@", note);
        GCHECK([note containsString:@".mutible/bookmarks"]);
        // Nothing in the shared folder, and no .gitignore: committing them
        // is the point.
        GCHECK(![fm fileExistsAtPath:bm]);
        GCHECK([Names([proj stringByAppendingPathComponent:@".mutible"])
                   isEqualToArray:@[@"bookmarks"]]);
        // Again: the page is bookmarked already, so nothing is written.
        [NSApp sendAction:@selector(saveBrowserBookmark:) to:nil from:nil];
        GCHECK([Names(ws) isEqualToArray:@[@"One.url"]]);
        GCHECK([[[c valueForKey:@"statusLabel"] stringValue] containsString:@"Already bookmarked"]);
    });
    MCGuiWait(@"the star fills for the bookmarked page", 10, ^BOOL {
        return [c.browser.bookmarkPath.lastPathComponent isEqualToString:@"One.url"];
    });

    MCGuiStep(@"a click on the filled star moves the bookmark to the Trash", ^{
        GCHECK(c.browser.bookmarkButton.enabled);
        GCHECK([c.browser.bookmarkButton.toolTip containsString:@"remove"]);
        UseScratchTrash(YES);
        [c.browser.bookmarkButton performClick:nil];
        UseScratchTrash(NO);
        GCHECK(Names(ws).count == 0);
        GCHECK([fm fileExistsAtPath:[ScratchTrash() stringByAppendingPathComponent:@"One.url"]]);
    });
    MCGuiWait(@"the star empties", 10, ^BOOL { return c.browser.bookmarkPath == nil; });

    MCGuiStep(@"Cmd+D in the page saves it; in the editor it is Add Next Occurrence", ^{
        GCHECK([c.browser.bookmarkButton.toolTip containsString:@"Bookmark this page"]);
        [c.browser focusPage];
        GCHECK(InPage(c));
        Key(c.window, kVK_ANSI_D, NSEventModifierFlagCommand);
        GCHECK([fm fileExistsAtPath:[ws stringByAppendingPathComponent:@"One.url"]]);
        GCHECK([c showFile:notes]);
        [c.window makeFirstResponder:c.textView];
        c.textView.selectedRange = NSMakeRange(0, 5);   // "alpha"
        const NSUInteger before = Names(ws).count;
        Key(c.window, kVK_ANSI_D, NSEventModifierFlagCommand);
        GCHECK(Names(ws).count == before);
        GCHECK(c.textView.selectedRanges.count == 2);   // both "alpha"s
    });

    MCGuiStep(@"Save All Tabs writes a folder with a bookmark a tab", ^{
        GCHECK(c.canSaveBrowserTabs);
        NSArray *before = Names(ws);
        [NSApp sendAction:@selector(saveBrowserTabsAsBookmarks:) to:nil from:nil];
        NSString *folder = nil;
        for (NSString *n in Names(ws))
            if (![before containsObject:n]) folder = [ws stringByAppendingPathComponent:n];
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

    MCGuiStep(@"the toolbar's book lists the folder; a bookmark loads in the tab on show", ^{
        MCGuiWriteFile([ws stringByAppendingPathComponent:@"Five.url"],
                       [NSString stringWithFormat:@"[InternetShortcut]\nURL=%@\n", Page(@"five")]);
        NSMenu *menu = [c.browser bookmarksMenu];
        NSMenuItem *folder = nil;
        [menu.delegate menuNeedsUpdate:menu];
        for (NSMenuItem *it in menu.itemArray)
            if ([it.title hasPrefix:@"Tabs "]) folder = it;
        GCHECK(folder.submenu != nil);
        // Folders first, then the bookmarks, named without the extension.
        GCHECK([menu.itemArray indexOfObject:folder] == 0);
        NSMenuItem *five = MenuItem(menu, @"Five");
        GCHECK(five && five.action == @selector(openBookmarkFromMenu:) && five.target == c.browser);
        GCHECK(MenuItem(menu, @"One") != nil);
        // The workspace's folder in Finder, and the shared one; no Shared
        // submenu while the shared folder has nothing.
        GCHECK(MenuItem(menu, @"Show Bookmarks Folder").action ==
               @selector(showWorkspaceBookmarksFolder:));
        GCHECK([[MenuItem(menu, @"Show Bookmarks Folder").representedObject
                   stringByResolvingSymlinksInPath] isEqualToString:ws.stringByResolvingSymlinksInPath]);
        GCHECK(MenuItem(menu, @"Show Shared Bookmarks Folder").action ==
               @selector(showBookmarksFolder:));
        GCHECK(MenuItem(menu, @"Shared") == nil);
        GCHECK(MenuItem(menu, @"Other") == nil);   // another workspace's
        GCHECK([MenuItem(menu, @"Remove Bookmark").target isEqual:c.browser]);  // One is on show
        NSMenuItem *all = MenuItem(folder.submenu, @"Open All in Tabs");
        GCHECK(all && all.action == @selector(openBookmarkFolderFromMenu:));
        // A tab not loaded since the session was saved has no title, so
        // its bookmark is named for its address.
        BOOL hasThree = NO;
        for (NSMenuItem *it in folder.submenu.itemArray)
            if (it.action == @selector(openBookmarkFromMenu:) &&
                [MCBookmarkAddress(it.representedObject) isEqualToString:Page(@"three")])
                hasThree = YES;
        GCHECK(hasThree);
        // The tab showing two loads five in its place.
        [c.browser selectTab:[c.browser.tabAddresses indexOfObject:Page(@"two")]];
        const NSUInteger tabs = c.browser.tabCount;
        [NSApp sendAction:five.action to:five.target from:five];
        GCHECK(c.browser.tabCount == tabs);
        GCHECK([ShownAddress(c.browser) isEqualToString:Page(@"five")]);
        GCHECK(![c.browser.tabAddresses containsObject:Page(@"two")]);
        // A bookmark already in a tab shows that tab.
        [NSApp sendAction:@selector(openBookmarkFromMenu:) to:c.browser
                     from:MenuItem(menu, @"One")];
        GCHECK(c.browser.tabCount == tabs);
        GCHECK([ShownAddress(c.browser) isEqualToString:Page(@"one")]);
        // Open All in Tabs adds the folder's pages not open yet.
        [NSApp sendAction:all.action to:all.target from:all];
        GCHECK([c.browser.tabAddresses containsObject:Page(@"two")]);
        GCHECK(c.browser.tabCount == tabs + 1);
    });

    MCGuiStep(@"the menu bar's Bookmarks menu loads a bookmark with the editor in front", ^{
        MCGuiWriteFile([ws stringByAppendingPathComponent:@"Six.url"],
                       [NSString stringWithFormat:@"[InternetShortcut]\nURL=%@\n", Page(@"six")]);
        NSMenu *menu = MenuBarBookmarks();
        GCHECK(menu != nil);
        NSMenuItem *six = MenuItem(menu, @"Six");
        GCHECK(six && six.target == nil);
        GCHECK(MenuItem(menu, @"Bookmark This Page") != nil);
        [c.window makeFirstResponder:c.textView];
        GCHECK(!InPage(c));
        const NSUInteger tabs = c.browser.tabCount;
        GCHECK([NSApp sendAction:six.action to:nil from:six]);
        GCHECK(c.browser.tabCount == tabs);
        GCHECK([ShownAddress(c.browser) isEqualToString:Page(@"six")]);
        GCHECK(InPage(c));
    });
    MCGuiWait(@"the page loads and its star fills", 20, ^BOOL {
        return Loaded(c.browser, @"Six") &&
               [c.browser.bookmarkPath.lastPathComponent isEqualToString:@"Six.url"];
    });

    MCGuiStep(@"the shared folder's bookmarks show under Shared", ^{
        MCGuiWriteFile([bm stringByAppendingPathComponent:@"Seven.url"],
                       [NSString stringWithFormat:@"[InternetShortcut]\nURL=%@\n", Page(@"seven")]);
        MCGuiWriteFile([bm stringByAppendingPathComponent:@"Work/Eight.url"],
                       [NSString stringWithFormat:@"[InternetShortcut]\nURL=%@\n", Page(@"eight")]);
        NSMenu *menu = MenuBarBookmarks();
        GCHECK(MenuItem(menu, @"Six") != nil);
        GCHECK(MenuItem(menu, @"Seven") == nil);   // not among the workspace's own
        NSMenu *shared = MenuItem(menu, @"Shared").submenu;
        GCHECK(shared != nil);
        NSMenuItem *seven = MenuItem(shared, @"Seven");
        GCHECK(seven && seven.action == @selector(openBookmarkFromMenu:));
        GCHECK([seven.representedObject isEqualToString:
                                            [bm stringByAppendingPathComponent:@"Seven.url"]]);
        GCHECK(MenuItem(shared, @"Work").submenu != nil);
        GCHECK(MenuItem(MenuItem(shared, @"Work").submenu, @"Eight") != nil);
        GCHECK(MenuItem(shared, @"Six") == nil);
        NSMenu *move = MenuItem(shared, @"Move to This Workspace").submenu;
        GCHECK(move != nil);
        GCHECK(MenuItem(move, @"Seven").action == @selector(moveBookmarkToWorkspace:));
        // The toolbar's book has the same Shared part.
        GCHECK(MenuItem(MenuItem([c.browser bookmarksMenu], @"Shared").submenu, @"Seven") != nil);
        // A shared bookmark opens here as any.
        GCHECK([NSApp sendAction:seven.action to:nil from:seven]);
        GCHECK([ShownAddress(c.browser) isEqualToString:Page(@"seven")]);
    });
    MCGuiWait(@"the shared bookmark's page loads and its star fills", 20, ^BOOL {
        return Loaded(c.browser, @"Seven") &&
               [c.browser.bookmarkPath.stringByResolvingSymlinksInPath
                   isEqualToString:[bm stringByAppendingPathComponent:@"Seven.url"]
                                       .stringByResolvingSymlinksInPath];
    });

    MCGuiStep(@"a page in Shared is saved already; Move to This Workspace moves the file", ^{
        [NSApp sendAction:@selector(saveBrowserBookmark:) to:nil from:nil];
        NSString *note = [[c valueForKey:@"statusLabel"] stringValue];
        MCGuiNote(@"status: %@", note);
        GCHECK([note containsString:@"Already bookmarked in Shared"]);
        GCHECK(![fm fileExistsAtPath:[ws stringByAppendingPathComponent:@"Seven.url"]]);
        NSMenu *shared = MenuItem(MenuBarBookmarks(), @"Shared").submenu;
        NSMenuItem *move = MenuItem(MenuItem(shared, @"Move to This Workspace").submenu, @"Seven");
        GCHECK(move != nil);
        GCHECK([NSApp sendAction:move.action to:move.target from:move]);
        GCHECK(![fm fileExistsAtPath:[bm stringByAppendingPathComponent:@"Seven.url"]]);
        NSString *moved = [ws stringByAppendingPathComponent:@"Seven.url"];
        GCHECK([MCBookmarkAddress(moved) isEqualToString:Page(@"seven")]);
        NSMenu *menu = MenuBarBookmarks();
        GCHECK(MenuItem(menu, @"Seven") != nil);
        GCHECK(MenuItem(MenuItem(menu, @"Shared").submenu, @"Seven") == nil);
    });
    MCGuiWait(@"the star follows the moved bookmark", 10, ^BOOL {
        return [c.browser.bookmarkPath.stringByResolvingSymlinksInPath
            isEqualToString:[ws stringByAppendingPathComponent:@"Seven.url"]
                                .stringByResolvingSymlinksInPath];
    });

    __block EditorController *c2 = nil;
    MCGuiStep(@"another workspace's menu lists its own bookmarks, not this one's", ^{
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:proj2];
        for (EditorController *o in MCGuiControllers())
            if ([o.rootPath.stringByResolvingSymlinksInPath
                    isEqualToString:proj2.stringByResolvingSymlinksInPath])
                c2 = o;
        GCHECK(c2 != nil && c2 != c);
        MCGuiSetKeyWindow(c2.window);
        NSMenu *menu = MenuBarBookmarks();
        GCHECK(MenuItem(menu, @"Other") != nil);
        GCHECK(MenuItem(menu, @"One") == nil);
        GCHECK(MenuItem(menu, @"Seven") == nil);
        // The shared folder shows in every workspace.
        GCHECK(MenuItem(MenuItem(menu, @"Shared").submenu, @"Work") != nil);
        MCGuiSetKeyWindow(c.window);
        GCHECK(MenuItem(MenuBarBookmarks(), @"Other") == nil);
        GCHECK(MenuItem(MenuBarBookmarks(), @"One") != nil);
        [c2.window close];
        c2 = nil;
    });
    MCGuiWait(@"the other workspace's window is gone", 10, ^BOOL {
        for (EditorController *o in MCGuiControllers())
            if ([o.rootPath.stringByResolvingSymlinksInPath
                    isEqualToString:proj2.stringByResolvingSymlinksInPath])
                return NO;
        return YES;
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

    MCGuiStep(@"a web request (mutible https://...) joins the browser window", ^{
        EditorController *b = BrowserWindow();
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"Mutible-open"];
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *req = [dir stringByAppendingPathComponent:@"suite.mutible-open"];
        NSString *body = [NSString stringWithFormat:@"mutible-open-1%Cweb%C%@%Cjavascript:x%C%@%C",
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
        // The browser window keeps to the shared folder: nothing made in
        // it, its menu the folder itself with no Shared part.
        GCHECK(![fm fileExistsAtPath:[bm stringByAppendingPathComponent:@".mutible"]]);
        NSMenu *menu = MenuBarBookmarks();
        GCHECK(MenuItem(menu, @"Work").submenu != nil);
        GCHECK(MenuItem(menu, @"Reading").submenu != nil);
        GCHECK(MenuItem(menu, @"Shared") == nil);
        GCHECK(MenuItem(menu, @"One") == nil);   // the code window's
        GCHECK(MenuItem(menu, @"Show Bookmarks Folder").action == @selector(showBookmarksFolder:));
        GCHECK(MenuItem(menu, @"Show Shared Bookmarks Folder") == nil);
        GCHECK(MenuItem([b.browser bookmarksMenu], @"Work") != nil);
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
            stringByAppendingPathComponent:@"scripts/mutible-launcher"];
        MCGuiRunProcess(@[@"/bin/bash", launcher, @"https://a.example/x", notes, @"HTTP://b.example"],
                        @{@"TMPDIR": launcherTmp, @"MUTIBLE_LAUNCHER_DRYRUN": @"1"},
                        ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            GCHECK(err.length == 0);
            GCHECK([out containsString:notes]);
            NSString *dir = [launcherTmp stringByAppendingPathComponent:@"Mutible-open"];
            NSArray *reqs = Names(dir);
            GCHECK(reqs.count == 1);
            NSData *d = [NSData dataWithContentsOfFile:
                                    [dir stringByAppendingPathComponent:reqs.firstObject]];
            NSString *t = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
            NSString *want = [NSString stringWithFormat:@"mutible-open-1%Cweb%Chttps://a.example/x%C"
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
            stringByAppendingPathComponent:@"scripts/mutible-launcher"];
        MCGuiRunProcess(@[@"/bin/bash", launcher, @"https://exa mple.com", @"https:///x"],
                        @{@"TMPDIR": launcherTmp, @"MUTIBLE_LAUNCHER_DRYRUN": @"1"},
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
