// BrowserTabsSuite.mm — `make guitest SUITES=browser_tabs`: the browser
// pane's tabs (Browser.h, BrowserTabs.h). Cmd+T opens one while the pane
// has the keyboard (and is Go to Symbol in Workspace elsewhere, a PDF
// included); Cmd+W closes the tab on show and, with the last, hides the
// pane; Ctrl+Tab and Ctrl+Shift+Tab step through them, and go on to the
// editor's tabs when the pane has one; tabs reorder and close from the
// strip; a Command-clicked link opens behind and a target=_blank one
// beside; only two pages stay live, the others suspended to their address
// and WebKit's record and brought back on show, their WebKit processes
// gone; the session keeps every tab, and an older session's single page
// still comes back. It measures the memory of eight tabs both ways.
//
// Only local pages (file://). Keys are CGEvent-made, through [NSApp
// sendEvent:] (the key monitor, the window's views, then the menus). It
// works in a window of its own.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "Browser.h"
#import "BrowserPages.h"
#import "ControlServer.h"
#import "PalettePanel.h"
#import "TabStrip.h"
#import "Keymap.h"
#import <Carbon/Carbon.h>
#import <Quartz/Quartz.h>
#import <WebKit/WebKit.h>
#include <libproc.h>
#include <objc/message.h>
#include <signal.h>
#include <sys/resource.h>

namespace {

NSString *gDir;
const NSEventModifierFlags kCmd = NSEventModifierFlagCommand;
const NSEventModifierFlags kCtrl = NSEventModifierFlagControl;

NSString *F(NSString *name) { return [gDir stringByAppendingPathComponent:name]; }
NSString *U(NSString *name) { return [NSURL fileURLWithPath:F(name)].absoluteString; }

CGEventFlags CGFlagsOf(NSEventModifierFlags flags) {
    CGEventFlags f = 0;
    if (flags & NSEventModifierFlagCommand) f |= kCGEventFlagMaskCommand;
    if (flags & NSEventModifierFlagShift) f |= kCGEventFlagMaskShift;
    if (flags & NSEventModifierFlagOption) f |= kCGEventFlagMaskAlternate;
    if (flags & NSEventModifierFlagControl) f |= kCGEventFlagMaskControl;
    return f;
}

// A key made as the hardware's are (CGEvent), for the window, through
// [NSApp sendEvent:]: the key monitor, the window's views, the menus.
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

BOOL Gone(pid_t pid) { return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH; }

WKWebView *ShownWeb(BrowserView *b) { return [b valueForKey:@"web"]; }
NSTextField *UrlBar(BrowserView *b) { return [b valueForKey:@"urlBar"]; }
MCTabStrip *Strip(BrowserView *b) { return [b valueForKey:@"strip"]; }

// Whether the keyboard is in the address bar (its field editor).
BOOL InUrlBar(EditorController *c) {
    NSResponder *r = c.window.firstResponder;
    return [r isKindOfClass:NSTextView.class] && [(NSTextView *)r isFieldEditor] &&
           [(NSTextView *)r delegate] == (id)UrlBar(c.browser);
}

// The keyboard into the user's page on show, as a Tab from the address bar
// puts it.
void FocusPage(EditorController *c) {
    [c.browser focusURLBar];
    [ShownWeb(c.browser) setValue:@YES forKey:@"toolbarNavigation"];
    [c.window makeFirstResponder:ShownWeb(c.browser)];
}

BOOL InPage(EditorController *c) {
    NSResponder *r = c.window.firstResponder;
    WKWebView *w = ShownWeb(c.browser);
    return w && [r isKindOfClass:NSView.class] && [(NSView *)r isDescendantOf:w];
}

NSString *Title(BrowserView *b) { return ShownWeb(b).title ?: @""; }
BOOL Loaded(BrowserView *b, NSString *title) {
    return [Title(b) isEqualToString:title] && !ShownWeb(b).isLoading;
}

// A PDF of one page, drawn with Core Graphics.
BOOL WritePDF(NSString *path) {
    CGRect box = CGRectMake(0, 0, 612, 792);
    CGContextRef ctx = CGPDFContextCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                 &box, NULL);
    if (!ctx) return NO;
    CGPDFContextBeginPage(ctx, NULL);
    CGContextSetRGBFillColor(ctx, 0.2, 0.3, 0.9, 1);
    CGContextFillRect(ctx, CGRectMake(36, 36, 540, 720));
    CGPDFContextEndPage(ctx);
    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    return YES;
}

// The palette closed as Esc in its field closes it.
void ClosePalette(EditorController *c) {
    NSView *p = MCCommandPaletteIn(c.window);
    NSTextView *ed = p ? (NSTextView *)MCCommandPaletteField(p).currentEditor : nil;
    [ed doCommandBySelector:@selector(cancelOperation:)];
}

// The menu bar's item for an action (submenus included).
NSMenuItem *MenuItem(SEL action) {
    NSMutableArray<NSMenu *> *stack = [NSMutableArray arrayWithObject:NSApp.mainMenu];
    while (stack.count) {
        NSMenu *m = stack.lastObject;
        [stack removeLastObject];
        for (NSMenuItem *i in m.itemArray) {
            if (i.submenu) [stack addObject:i.submenu];
            else if (i.action == action && !i.hidden) return i;
        }
    }
    return nil;
}

}  // namespace

// A link's navigation as WebKit describes a real click's: the modifier
// keys and the button, which a script's click never carries.
@interface MCFakeLinkAction : WKNavigationAction
@property(nonatomic, strong) NSURLRequest *fakeRequest;
@property(nonatomic) NSEventModifierFlags fakeFlags;
@property(nonatomic) NSInteger fakeButton;
@end
@implementation MCFakeLinkAction
- (NSURLRequest *)request { return self.fakeRequest; }
- (WKNavigationType)navigationType { return WKNavigationTypeLinkActivated; }
- (NSEventModifierFlags)modifierFlags { return self.fakeFlags; }
- (NSInteger)buttonNumber { return self.fakeButton; }
@end

namespace {

NSString *Page(NSString *title, NSString *body) {
    return [NSString stringWithFormat:@"<html><head><title>%@</title></head><body>%@</body></html>",
                                      title, body];
}

}  // namespace

MC_GUI_SUITE(browser_tabs) {
    __block EditorController *c = nil;
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"browser-tabs"];

    MCGuiStep(@"pages, two files, and a window of the suite's own", ^{
        for (NSString *n in @[@"A", @"B", @"C", @"D"])
            MCGuiWriteFile(F([n.lowercaseString stringByAppendingString:@".html"]),
                           Page(n, [NSString stringWithFormat:@"<p>Page %@</p>"
                                                              @"<div style='height:3000px'></div>", n]));
        MCGuiWriteFile(F(@"links.html"),
                       Page(@"Links", @"<a id=behind href='b.html'>behind</a> "
                                      @"<a id=beside href='c.html' target=_blank>beside</a>"));
        // About 40 MB held by the page, so its WebKit process is easy to see.
        MCGuiWriteFile(F(@"heavy.html"),
                       Page(@"Heavy", @"<script>window.big = new Uint8Array(40 * 1024 * 1024);"
                                      @"for (let i = 0; i < big.length; i += 4096) big[i] = i & 255;"
                                      @"document.title = 'Heavy ' + location.search.slice(1);"
                                      @"</script>"));
        MCGuiWriteFile(F(@"one.txt"), @"one\n");
        MCGuiWriteFile(F(@"two.txt"), @"two\n");
        GCHECK(WritePDF(F(@"doc.pdf")));
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        GCHECK([c showFile:F(@"one.txt")]);
        GCHECK([c showFile:F(@"two.txt")]);
    });

    // ------------------------------------------------------- opening tabs
    MCGuiStep(@"View > New Browser Tab opens the pane, its address bar ready", ^{
        GCHECK(c.browser == nil);
        GCHECK(MenuItem(@selector(openBrowserTab:)) != nil);
        [NSApp sendAction:@selector(openBrowserTab:) to:nil from:nil];
        GCHECK(c.browser != nil && c.browserVisible);
        GCHECK(c.browser.tabCount == 1 && c.browser.selectedTab == 0);
        GCHECK(InUrlBar(c));
        GCHECK_EQ_STR(c.browser.tabTitles.firstObject, @"New Tab");
        [c.browser navigateToString:U(@"a.html")];
    });
    MCGuiWait(@"A loads", 20, ^BOOL { return Loaded(c.browser, @"A"); });
    MCGuiStep(@"Cmd+T in the address bar: a second tab, the keyboard in its address bar", ^{
        [c.browser focusURLBar];
        Key(c.window, kVK_ANSI_T, kCmd);
        GCHECK(c.browser.tabCount == 2 && c.browser.selectedTab == 1);
        GCHECK(InUrlBar(c));
        GCHECK_EQ_STR(UrlBar(c.browser).stringValue, @"");
        GCHECK(MCCommandPaletteIn(c.window) == nil);   // not Go to Symbol in Workspace
        [c.browser navigateToString:U(@"b.html")];
    });
    MCGuiWait(@"B loads", 20, ^BOOL { return Loaded(c.browser, @"B"); });
    MCGuiStep(@"Cmd+T with the keyboard in the page", ^{
        FocusPage(c);
        GCHECK(InPage(c));
        Key(c.window, kVK_ANSI_T, kCmd);
        GCHECK(c.browser.tabCount == 3 && c.browser.selectedTab == 2);
        GCHECK(InUrlBar(c));
        [c.browser navigateToString:U(@"c.html")];
    });
    MCGuiWait(@"C loads", 20, ^BOOL { return Loaded(c.browser, @"C"); });
    MCGuiStep(@"the + at the strip's end opens a fourth", ^{
        NSView *plus = [c.browser valueForKey:@"plus"];
        GCHECK(plus.frame.size.width > 0 && !plus.hidden);
        void (^press)(void) = [plus valueForKey:@"onPress"];
        press();
        GCHECK(c.browser.tabCount == 4 && c.browser.selectedTab == 3 && InUrlBar(c));
        [c.browser navigateToString:U(@"d.html")];
    });
    MCGuiWait(@"D loads", 20, ^BOOL { return Loaded(c.browser, @"D"); });

    // ------------------------------------------------- live and suspended
    __block __weak WKWebView *webD = nil;
    __block pid_t pidA = 0;
    MCGuiStep(@"two live pages: the one on show and the one before; the strip names all four", ^{
        BrowserView *b = c.browser;
        NSArray *titles = @[@"A", @"B", @"C", @"D"];
        GCHECK([b.tabTitles isEqualToArray:titles]);
        GCHECK(Strip(b).items.count == 4 && Strip(b).activeIndex == 3);
        GCHECK_EQ_STR(Strip(b).items[0].title, @"A");
        GCHECK(b.livePageCount == 2);
        GCHECK([b webViewOfTab:0] == nil && [b webViewOfTab:1] == nil);
        GCHECK([b webViewOfTab:2] != nil && [b webViewOfTab:3] != nil);
        GCHECK([b.tabAddresses[0] hasSuffix:@"a.html"]);
        webD = [b webViewOfTab:3];
        MCGuiSnapshot(b, @"browser-tabs-four");
        // Nothing else of the pane's web views is in the view tree.
        NSUInteger webs = 0;
        for (NSView *v in b.subviews) webs += [v isKindOfClass:WKWebView.class];
        GCHECK(webs == 2);
    });
    MCGuiStep(@"Ctrl+Tab from the page wraps to A, which loads again, the keyboard with it", ^{
        FocusPage(c);
        Key(c.window, kVK_Tab, kCtrl);
        GCHECK(c.browser.selectedTab == 0);
        GCHECK(c.browser.livePageCount == 2);
        GCHECK([c.browser webViewOfTab:3] == webD);   // D stays live: used just before
        GCHECK([c.browser webViewOfTab:2] == nil);    // C is let go
        GCHECK(InPage(c));
        // The editor's tab stayed where it was.
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"two.txt");
    });
    MCGuiWait(@"A is back, at its address", 20, ^BOOL { return Loaded(c.browser, @"A"); });
    MCGuiStep(@"Ctrl+Shift+Tab in the address bar goes back to D, the same page, not loaded again", ^{
        pidA = WebProcess(ShownWeb(c.browser));
        [c.browser focusURLBar];
        // As the hardware makes it (a back-tab rebuilt from its characters
        // matches no menu item), from the address bar: in the page WebKit
        // sends a key on to the menus again itself, as a new event the
        // harness cannot hand to its offscreen window.
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_Tab, kCtrl | NSEventModifierFlagShift, nil, nil));
        GCHECK(c.browser.selectedTab == 3);
        GCHECK(ShownWeb(c.browser) == webD);
        GCHECK(Loaded(c.browser, @"D"));
        GCHECK(InUrlBar(c));
        GCHECK([UrlBar(c.browser).stringValue hasSuffix:@"d.html"]);
    });
    MCGuiStep(@"Shift+Cmd+[ and Ctrl+2 in the address bar step through them too", ^{
        [c.browser focusURLBar];
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_LeftBracket,
                                   kCmd | NSEventModifierFlagShift, nil, nil));
        GCHECK(c.browser.selectedTab == 2);
        GCHECK([UrlBar(c.browser).stringValue hasSuffix:@"c.html"]);
        Key(c.window, kVK_ANSI_2, kCtrl);
        GCHECK(c.browser.selectedTab == 1);
        // A was used least recently now: its page went, and its process.
        GCHECK([c.browser webViewOfTab:0] == nil);
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"two.txt");
    });
    MCGuiWait(@"A's WebKit process ends with its page", 15, ^BOOL { return Gone(pidA); });

    // ------------------------------------------------------- closing
    MCGuiWait(@"B is back", 20, ^BOOL { return Loaded(c.browser, @"B"); });
    MCGuiStep(@"Cmd+W in the page closes the tab on show", ^{
        FocusPage(c);
        NSMenuItem *close = MenuItem(@selector(closeFocused:));
        GCHECK(close != nil);
        id target = [NSApp targetForAction:@selector(closeFocused:) to:nil from:close];
        GCHECK(target == c.browser);
        GCHECK([target validateMenuItem:close]);
        GCHECK_EQ_STR(close.title, @"Close Browser Tab");
        // A Command key in a page goes to WebKit's page first, and back to
        // the menus a moment later when the page does not take it.
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_W, kCmd, nil, nil));
    });
    MCGuiWait(@"the tab closes", 5, ^BOOL { return c.browser.tabCount == 3; });
    MCGuiStep(@"the one used before shows, the keyboard in its page", ^{
        GCHECK([c.browser.tabTitles isEqualToArray:(@[@"A", @"C", @"D"])]);
        GCHECK(c.browser.selectedTab == 1);   // C, used just before B
        GCHECK(InPage(c));
        GCHECK(c.browserVisible);
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"two.txt");
    });
    MCGuiStep(@"a tab drags along the strip; a middle click or its x closes it", ^{
        BrowserView *b = c.browser;
        WKWebView *shown = ShownWeb(b);
        void (^move)(NSInteger, NSInteger) = Strip(b).onMove;
        move(0, 2);
        GCHECK([b.tabTitles isEqualToArray:(@[@"C", @"D", @"A"])]);
        GCHECK(b.selectedTab == 0 && ShownWeb(b) == shown);   // C still on show
        void (^close)(NSInteger) = Strip(b).onClose;
        close(2);
        GCHECK([b.tabTitles isEqualToArray:(@[@"C", @"D"])]);
        GCHECK(b.selectedTab == 0);
        NSMenu *m = Strip(b).menuForTab(1);
        GCHECK([m itemWithTitle:@"Close Other Tabs"] != nil);
        GCHECK([m itemWithTitle:@"Copy Address"] != nil);
    });

    // ------------------------------------------------------ links
    MCGuiStep(@"a page of links", ^{ [c.browser navigateToString:U(@"links.html")]; });
    MCGuiWait(@"it loads", 20, ^BOOL { return Loaded(c.browser, @"Links"); });
    MCGuiStep(@"a Command-click on a link opens it in a new tab behind", ^{
        // WebKit gives a script's click no modifiers, so the pane is asked
        // about a navigation as a real Command-click makes it.
        MCFakeLinkAction *a = [MCFakeLinkAction new];
        a.fakeRequest = [NSURLRequest requestWithURL:[NSURL URLWithString:U(@"b.html")]];
        a.fakeFlags = NSEventModifierFlagCommand;
        __block WKNavigationActionPolicy policy = WKNavigationActionPolicyAllow;
        [(id<WKNavigationDelegate>)c.browser webView:ShownWeb(c.browser)
                     decidePolicyForNavigationAction:a
                                     decisionHandler:^(WKNavigationActionPolicy p) { policy = p; }];
        GCHECK(policy == WKNavigationActionPolicyCancel);
        // A plain click stays in the page; a middle click opens behind too.
        a.fakeFlags = 0;
        [(id<WKNavigationDelegate>)c.browser webView:ShownWeb(c.browser)
                     decidePolicyForNavigationAction:a
                                     decisionHandler:^(WKNavigationActionPolicy p) { policy = p; }];
        GCHECK(policy == WKNavigationActionPolicyAllow);
        GCHECK(c.browser.tabCount == 3);
    });
    MCGuiStep(@"the new tab is right of the page, not loaded, and the page stayed", ^{
        BrowserView *b = c.browser;
        GCHECK(b.selectedTab == 0 && Loaded(b, @"Links"));
        GCHECK([b.tabAddresses[1] hasSuffix:@"b.html"]);
        GCHECK([b webViewOfTab:1] == nil);
        GCHECK_EQ_STR(b.tabTitles[1], @"b.html");
    });
    MCGuiAsync(@"a target=_blank link opens beside, shown", 20, ^(MCGuiDone done) {
        [ShownWeb(c.browser) evaluateJavaScript:@"document.getElementById('beside').click(); true"
                              completionHandler:^(id, NSError *) { done(); }];
    });
    MCGuiWait(@"C in a new tab", 10, ^BOOL {
        return c.browser.tabCount == 4 && Loaded(c.browser, @"C");
    });
    MCGuiStep(@"it went right of its opener, and the opener is still live", ^{
        BrowserView *b = c.browser;
        GCHECK(b.selectedTab == 1);
        GCHECK([b.tabTitles[0] isEqualToString:@"Links"] && [b webViewOfTab:0] != nil);
        GCHECK(b.livePageCount == 2);
    });

    // ------------------------------------------------------ the session
    __block NSDictionary *saved = nil;
    MCGuiStep(@"the session keeps every tab, and which one showed", ^{
        saved = [c.browser savedState];
        NSArray *tabs = saved[@"tabs"];
        GCHECK(tabs.count == 4);
        GCHECK([saved[@"active"] integerValue] == 1);
        GCHECK([tabs[2][@"url"] hasSuffix:@"b.html"]);
        GCHECK([tabs[0][@"title"] isEqualToString:@"Links"]);
        // The tab on show is also the one page an older minicode reads.
        GCHECK([saved[@"url"] hasSuffix:@"c.html"]);
        if (@available(macOS 12.0, *)) {
            GCHECK([tabs[1][@"state"] isKindOfClass:NSData.class]);   // live: its record
            GCHECK([tabs[0][@"state"] isKindOfClass:NSData.class]);
        }
    });
    __block BrowserView *restored = nil;
    MCGuiStep(@"a new pane takes them back, only the tab on show loading", ^{
        restored = [[BrowserView alloc] initWithHomeURL:@""];
        restored.frame = NSMakeRect(0, 0, 500, 400);
        [restored restoreState:saved];
        GCHECK(restored.tabCount == 4 && restored.selectedTab == 1);
        GCHECK(restored.livePageCount == 1);
        GCHECK([restored.tabTitles[0] isEqualToString:@"Links"]);   // before any load
        GCHECK([restored.tabTitles[2] isEqualToString:@"b.html"]);
    });
    MCGuiWait(@"its tab on show loads", 20, ^BOOL { return Loaded(restored, @"C"); });
    MCGuiStep(@"an older session's single page comes back as one tab", ^{
        BrowserView *old = [[BrowserView alloc] initWithHomeURL:@""];
        [old restoreState:@{@"url": U(@"d.html")}];
        GCHECK(old.tabCount == 1 && old.selectedTab == 0);
        GCHECK([old.tabAddresses[0] hasSuffix:@"d.html"]);
        restored = nil;
    });

    // ----------------------------------------------- the editor's tabs
    MCGuiStep(@"with one browser tab, Ctrl+Tab in the pane goes on to the editor's tabs", ^{
        BrowserView *b = c.browser;
        while (b.tabCount > 1) [b closeTab:b.tabCount - 1];
        GCHECK(b.tabCount == 1);
        [b focusURLBar];
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"two.txt");
        Key(c.window, kVK_Tab, kCtrl);
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"one.txt");
    });

    // ------------------------------------------ Cmd+T elsewhere
    MCGuiStep(@"in the editor Cmd+T is still Go to Symbol in Workspace", ^{
        [c.window makeFirstResponder:c.textView];
        Key(c.window, kVK_ANSI_T, kCmd);
        GCHECK(c.browser.tabCount == 1);
        NSView *p = MCCommandPaletteIn(c.window);
        GCHECK(p != nil);
        if (p) GCHECK_EQ_STR(MCCommandPaletteField(p).stringValue, @"#");
        ClosePalette(c);
        GCHECK(MCCommandPaletteIn(c.window) == nil);
    });
    MCGuiStep(@"and in a PDF, a page too, it is not a browser tab", ^{
        GCHECK([c showFile:F(@"doc.pdf")]);
        GCHECK(c.isPDF);
        [c.window makeFirstResponder:c.pdfView];
        // The key monitor leaves the key alone (only the browser's pane
        // answers New Browser Tab), so it goes on to the menus, where it is
        // Go to Symbol in Workspace's.
        CGEventRef cg = CGEventCreateKeyboardEvent(NULL, kVK_ANSI_T, true);
        CGEventSetFlags(cg, kCGEventFlagMaskCommand);
        NSEvent *e0 = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                 modifierFlags:e0.modifierFlags timestamp:0
                                  windowNumber:c.window.windowNumber context:nil
                                    characters:e0.characters
                   charactersIgnoringModifiers:e0.charactersIgnoringModifiers
                                     isARepeat:NO keyCode:kVK_ANSI_T];
        GCHECK(!MCKeymapHandleKeyDown(e));
        GCHECK(c.browser.tabCount == 1);
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_T, kCmd, nil, nil));
        GCHECK(c.browser.tabCount == 1);
        NSView *p = MCCommandPaletteIn(c.window);
        GCHECK(p != nil);
        if (p) GCHECK_EQ_STR(MCCommandPaletteField(p).stringValue, @"#");
        ClosePalette(c);
    });

    // --------------------------------------------- the last tab
    MCGuiStep(@"Cmd+W on the last tab hides the pane; the keyboard goes to the editor", ^{
        GCHECK([c showFile:F(@"one.txt")]);
        [c.browser focusURLBar];
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_W, kCmd, nil, nil));
        GCHECK(!c.browserVisible);
        GCHECK(c.browser.tabCount == 1);
        GCHECK_EQ_STR(c.browser.tabTitles.firstObject, @"New Tab");
        GCHECK(c.window.firstResponder == c.textView);
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"one.txt");   // no editor tab closed
        GCHECK([c.browser savedState] == nil);
        // New Browser Tab then uses that empty tab rather than adding one.
        [NSApp sendAction:@selector(openBrowserTab:) to:nil from:nil];
        GCHECK(c.browserVisible && c.browser.tabCount == 1 && InUrlBar(c));
        [c toggleBrowser:nil];
        GCHECK(!c.browserVisible);
    });

    // ------------------------------------------- the agent page
    MCGuiStep(@"the agent page beside the tabs: none of them active, a click shows yours", ^{
        [c toggleBrowser:nil];
        BrowserView *b = c.browser;
        [b navigateToString:U(@"a.html")];
        [c.window makeFirstResponder:c.textView];
        [b openAgentPage];
        [b loadInAgentPage:U(@"d.html")];
        GCHECK([b showAgentPageQuietly]);
        GCHECK(b.agentPageShown && Strip(b).activeIndex == -1);
        void (^select)(NSInteger) = Strip(b).onSelect;
        select(0);
        GCHECK(!b.agentPageShown && Strip(b).activeIndex == 0 && b.hasAgentPage);
        [b closeAgentPage];
    });

    // ------------------------------------------------- memory
    __block NSMutableArray<NSNumber *> *pids = [NSMutableArray array];
    __block uint64_t appAllLive = 0;
    __block double allLive = 0;
    MCGuiStep(@"eight heavy tabs, all of them live for the measurement", ^{
        BrowserView *b = c.browser;
        b.liveLimit = 100;
        [b navigateToString:[U(@"heavy.html") stringByAppendingString:@"?1"]];
    });
    for (int i = 2; i <= 8; i++) {
        MCGuiWait([NSString stringWithFormat:@"heavy %d loads", i - 1], 20, ^BOOL {
            return Loaded(c.browser, [NSString stringWithFormat:@"Heavy %d", i - 1]);
        });
        MCGuiStep([NSString stringWithFormat:@"tab %d", i], ^{
            [c.browser newTab];
            [c.browser navigateToString:[U(@"heavy.html") stringByAppendingFormat:@"?%d", i]];
        });
    }
    MCGuiWait(@"heavy 8 loads", 20, ^BOOL { return Loaded(c.browser, @"Heavy 8"); });
    // A moment for the pages' memory to settle before it is read (a
    // measurement, with no condition to wait on).
    MCGuiPause(1.0);
    MCGuiStep(@"memory with eight live pages", ^{
        BrowserView *b = c.browser;
        GCHECK(b.tabCount == 8 && b.livePageCount == 8);
        NSMutableSet *seen = [NSMutableSet set];
        uint64_t web = 0;
        for (NSUInteger i = 0; i < b.tabCount; i++) {
            pid_t p = WebProcess([b webViewOfTab:i]);
            [pids addObject:@(p)];
            if (p > 0 && ![seen containsObject:@(p)]) {
                [seen addObject:@(p)];
                web += Footprint(p);
            }
        }
        appAllLive = Footprint(getpid());
        allLive = web / 1048576.0;
        MCGuiNote(@"eight live pages: %lu WebKit processes, %.1f MB; minicode %.1f MB",
                  (unsigned long)seen.count, allLive, appAllLive / 1048576.0);
        GCHECK(seen.count >= 2);
        b.liveLimit = 2;   // the default
        GCHECK(b.livePageCount == 2);
        GCHECK([b webViewOfTab:7] != nil && [b webViewOfTab:6] != nil);
    });
    MCGuiWait(@"the six suspended pages' processes end", 20, ^BOOL {
        for (NSUInteger i = 0; i < 6; i++)
            if (!Gone(pids[i].intValue)) return NO;
        return YES;
    });
    MCGuiPause(1.0);   // as above: memory settling before a measurement
    MCGuiStep(@"memory with two live pages", ^{
        BrowserView *b = c.browser;
        uint64_t web = Footprint(WebProcess([b webViewOfTab:7]));
        if (WebProcess([b webViewOfTab:6]) != WebProcess([b webViewOfTab:7]))
            web += Footprint(WebProcess([b webViewOfTab:6]));
        const double twoLive = web / 1048576.0;
        const uint64_t app = Footprint(getpid());
        MCGuiNote(@"two live pages of eight: %.1f MB in WebKit (%.1f with all live); "
                  @"minicode %.1f MB (%.1f with all live)",
                  twoLive, allLive, app / 1048576.0, appAllLive / 1048576.0);
        GCHECK(twoLive < allLive / 2);
    });
    MCGuiStep(@"a suspended heavy tab shown loads again, and the oldest goes", ^{
        [c.browser selectTab:0];
        GCHECK(c.browser.livePageCount == 2);
        GCHECK([c.browser webViewOfTab:7] != nil && [c.browser webViewOfTab:6] == nil);
    });
    MCGuiWait(@"heavy 1 is back", 20, ^BOOL { return Loaded(c.browser, @"Heavy 1"); });

    MCGuiStep(@"the suite's window closes", ^{
        [c.window close];
        c = nil;
        MCGuiSetKeyWindow(nil);
    });
}
