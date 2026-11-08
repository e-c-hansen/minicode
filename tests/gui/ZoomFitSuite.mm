// ZoomFitSuite.mm — `make guitest SUITES=zoomfit`: seeing the whole of a
// page or a file in a pane. Cmd+B hides and shows the sidebar from every
// pane (it reached NSSplitView's own toggleSidebar: before, and did
// nothing); no command's action is one AppKit's views answer themselves;
// Cmd+=, Cmd+- and Cmd+0 zoom the browser's page (with the keyboard in it,
// or after a click in it) and a PDF, and the editor everywhere else; Zoom
// to Fit Width, Fit Page and the zoom remembered per site; Maximize Pane;
// the editor never scrolling sideways while it wraps (beside the browser,
// with the line numbers and the ruler, zoomed, in a Markdown preview and a
// diff), and Option+Z and editor.word-wrap turning wrapping off.
//
// Only local pages (file://). Keys are CGEvent-made, through [NSApp
// sendEvent:] (the key monitor, the window's views, then the menus), and a
// few posted to this process with CGEventPostToPid as the hardware's are.
// It works in a window of its own.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "Browser.h"
#import "BrowserPages.h"
#import "ControlServer.h"
#import "EditorFont.h"
#import "PageZoom.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import "TerminalPanel.h"
#import "TreeDetails.h"
#include "Commands.h"
#import <Carbon/Carbon.h>
#import <Quartz/Quartz.h>
#import <WebKit/WebKit.h>

namespace {

NSString *gDir;

NSString *F(NSString *name) { return [gDir stringByAppendingPathComponent:name]; }

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

// The same key posted to this process, down and up, as a keyboard's would
// come; it arrives on a later turn of the run loop.
void Post(CGKeyCode code, NSEventModifierFlags flags) {
    for (bool down : {true, false}) {
        CGEventRef cg = CGEventCreateKeyboardEvent(NULL, code, down);
        CGEventSetFlags(cg, CGFlagsOf(flags));
        CGEventPostToPid(getpid(), cg);
        CFRelease(cg);
    }
}

// A click in a view, through [NSApp sendEvent:] (the pointer's watcher
// sees it first).
void Click(NSView *v) {
    NSPoint p = [v convertPoint:NSMakePoint(NSMidX(v.bounds), NSMidY(v.bounds)) toView:nil];
    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:p
        modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:v.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
    NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:p
        modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:v.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:0];
    [NSApp postEvent:up atStart:NO];
    [NSApp sendEvent:down];
}

const NSEventModifierFlags kCmd = NSEventModifierFlagCommand;
const NSEventModifierFlags kCtrlCmd = NSEventModifierFlagCommand | NSEventModifierFlagControl;

CGFloat SidebarWidth(EditorController *c) {
    return c.splitView.subviews.firstObject.frame.size.width;
}
BOOL SidebarShown(EditorController *c) { return SidebarWidth(c) >= 1 && !c.sidebarCollapsed; }

WKWebView *UserWeb(EditorController *c) { return [c.browser valueForKey:@"web"]; }

// The keyboard into the user's page, as a Tab from the address bar puts it.
void FocusPage(EditorController *c) {
    [c.browser focusURLBar];
    [UserWeb(c) setValue:@YES forKey:@"toolbarNavigation"];
    [c.window makeFirstResponder:UserWeb(c)];
}

BOOL Near(double a, double b) { return fabs(a - b) < 0.005; }

// A PDF of `pages` US letter pages, drawn with Core Graphics.
BOOL WriteLetterPDF(NSString *path, int pages) {
    CGRect box = CGRectMake(0, 0, 612, 792);
    CGContextRef ctx = CGPDFContextCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                 &box, NULL);
    if (!ctx) return NO;
    for (int i = 0; i < pages; i++) {
        CGPDFContextBeginPage(ctx, NULL);
        CGContextSetRGBFillColor(ctx, 0.9, 0.3, 0.2, 1);
        CGContextFillRect(ctx, CGRectMake(36, 36, 540, 720));
        CGPDFContextEndPage(ctx);
    }
    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    return YES;
}

// Whether a scroll view shows its whole document's width: no scrolling
// sideways.
BOOL FitsAcross(NSScrollView *sv) {
    [sv tile];
    NSView *doc = sv.documentView;
    const CGFloat clip = sv.contentView.bounds.size.width;
    const BOOL fits = doc.frame.size.width <= clip + 0.5;
    if (!fits)
        MCGuiNote(@"%@: document %.1f wide in a clip %.1f wide", sv.className,
                  doc.frame.size.width, clip);
    return fits;
}

// A PDF's page fits across its pane: its edges are within what the clip
// shows. PDFKit's automatic scale fits the page to the whole PDFView, so a
// scroll bar that takes room (no trackpad, as on CI's runners; try it here
// with MC_GUI_SCROLLBARS=always) used to hide its right edge.
BOOL PageFitsAcross(PDFView *v, NSScrollView *sv) {
    [sv tile];
    PDFPage *page = v.currentPage ?: [v.document pageAtIndex:0];
    if (!page) return NO;
    const NSRect inView = [v convertRect:[page boundsForBox:v.displayBox] fromPage:page];
    const NSRect inClip = [sv.contentView convertRect:inView fromView:v];
    const NSRect clip = sv.contentView.bounds;
    const BOOL fits = NSMinX(inClip) >= NSMinX(clip) - 0.5 && NSMaxX(inClip) <= NSMaxX(clip) + 0.5;
    if (!fits)
        MCGuiNote(@"the page %@ in a clip %@", NSStringFromRect(inClip), NSStringFromRect(clip));
    return fits;
}

NSMenuItem *ItemFor(SEL action) {
    return [[NSMenuItem alloc] initWithTitle:@"" action:action keyEquivalent:@""];
}

}  // namespace

MC_GUI_SUITE(zoomfit) {
    __block EditorController *c = nil;
    __block NSString *settingsBefore = nil;
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"zoomfit"];
    NSString *shots = NSProcessInfo.processInfo.environment[@"MC_GUI_SNAPSHOTS"];
    NSString *longLine = [@"" stringByPaddingToLength:600 withString:@"wrap me " startingAtIndex:0];

    MCGuiStep(@"a window of the suite's own, wide, with files and local pages", ^{
        MCGuiWriteFile(F(@"a.txt"), [NSString stringWithFormat:@"short\n%@\nend\n", longLine]);
        MCGuiWriteFile(F(@"notes.md"),
                       [NSString stringWithFormat:@"# Notes\n\n%@\n\n- [ ] a task\n", longLine]);
        // Wider than the pane: 1600 pixels of stripes.
        MCGuiWriteFile(F(@"wide.html"),
            @"<html><head><title>Wide</title><style>body{margin:0}"
            @"#w{width:1600px;height:400px;background:repeating-linear-gradient(90deg,"
            @"#2a6 0,#2a6 100px,#fd3 100px,#fd3 200px)}</style></head>"
            @"<body><div id=w></div><p>The end of the page</p></body></html>");
        MCGuiWriteFile(F(@"other.html"),
            @"<html><head><title>Other</title></head><body>Another local page</body></html>");
        GCHECK(WriteLetterPDF(F(@"letter.pdf"), 2));
        settingsBefore = [NSString stringWithContentsOfFile:
            NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"]
                                                   encoding:NSUTF8StringEncoding error:nil] ?: @"";
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        NSRect fr = c.window.frame;
        fr.size = NSMakeSize(1400, 900);
        [c.window setFrame:fr display:NO];
        GCHECK([c showFile:F(@"a.txt")]);
        MCEditorZoomBy(0);
    });

    // ---------------------------------------------- actions AppKit answers
    MCGuiStep(@"no command's action is one AppKit's views or windows answer themselves", ^{
        // NSSplitView and NSWindow answer toggleSidebar: on current macOS,
        // and the split view is in every pane's responder chain: Cmd+B went
        // there and did nothing. These answer on purpose (or, for the tab
        // keys, reach the window's tabs anyway; checked below).
        NSSet *meant = [NSSet setWithArray:@[@"performClose:", @"undo:", @"redo:", @"cut:",
            @"copy:", @"paste:", @"selectAll:", @"performTextFinderAction:",
            @"selectNextTab:", @"selectPreviousTab:", @"terminate:",
            @"orderFrontStandardAboutPanel:", @"editorZoomIn:", @"editorZoomOut:",
            @"editorZoomReset:"]];
        NSArray *answerers = @[[[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 10, 10)
                                    styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered
                                        defer:YES],
                               [NSSplitView new], [NSView new], [NSScrollView new],
                               [NSClipView new], [NSOutlineView new], [NSTextField new],
                               [[NSTextView alloc] initWithFrame:NSZeroRect],
                               [[WKWebView alloc] initWithFrame:NSZeroRect],
                               [[PDFView alloc] initWithFrame:NSZeroRect], NSApp];
        for (const Commands::Command &cmd : Commands::all()) {
            if (!cmd.selector) continue;
            NSString *name = @(cmd.selector);
            if ([meant containsObject:name]) continue;
            SEL sel = NSSelectorFromString(name);
            for (id v in answerers)
                if ([v respondsToSelector:sel]) {
                    MCGuiNote(@"%s's %@ is answered by %@", cmd.id, name, [v className]);
                    GCHECK(![v respondsToSelector:sel]);
                }
        }
        [c.window makeFirstResponder:c.textView];
        GCHECK([NSApp targetForAction:@selector(toggleSidebarVisibility:)] == c);
    });
    MCGuiStep(@"the tab keys reach the window's tabs all the same", ^{
        GCHECK([c showFile:F(@"notes.md")]);
        GCHECK([c showFile:F(@"a.txt")]);
        [c.window makeFirstResponder:c.textView];
        Key(c.window, kVK_Tab, NSEventModifierFlagControl);
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"notes.md");
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_RightBracket, kCmd | NSEventModifierFlagShift,
                                   nil, nil));
        GCHECK_EQ_STR(c.currentPath.lastPathComponent, @"a.txt");
    });

    // ------------------------------------------------------------ Cmd+B
    __block CGFloat wide = 0;
    MCGuiStep(@"the sidebar a little wider than it starts", ^{
        [c.splitView setPosition:300 ofDividerAtIndex:0];
        wide = SidebarWidth(c);
        GCHECK(fabs(wide - 300) < 1.5);
    });
    // Cmd+B twice with the keyboard in a pane: hidden, then back at its width.
    void (^cmdB)(NSString *, void (^)(void), Class) =
        ^(NSString *pane, void (^focus)(void), Class keyboardIn) {
        MCGuiStep([NSString stringWithFormat:@"Cmd+B in %@ hides the sidebar and shows it again", pane], ^{
            focus();
            NSResponder *fr = c.window.firstResponder;
            if (![fr isKindOfClass:keyboardIn])
                MCGuiNote(@"%@: the keyboard is in %@", pane, fr.className);
            GCHECK([fr isKindOfClass:keyboardIn]);
            GCHECK(SidebarShown(c));
            Key(c.window, kVK_ANSI_B, kCmd);
            GCHECK(!SidebarShown(c) && SidebarWidth(c) < 1);
            GCHECK(c.window.firstResponder == fr);   // a pane outside the sidebar keeps it
            Key(c.window, kVK_ANSI_B, kCmd);
            GCHECK(SidebarShown(c) && fabs(SidebarWidth(c) - wide) < 1.5);
        });
    };
    cmdB(@"the editor", ^{ [c.window makeFirstResponder:c.textView]; },
         NSClassFromString(@"CodeTextView"));
    MCGuiStep(@"Cmd+B from the tree hides it and gives the keyboard to the editor", ^{
        [c.window makeFirstResponder:c.outline];
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(!SidebarShown(c));
        GCHECK(c.window.firstResponder == c.textView);
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(SidebarShown(c));
    });
    MCGuiStep(@"the find bar", ^{
        [c.window makeFirstResponder:c.textView];
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_F, kCmd, nil, nil));
    });
    cmdB(@"the find bar's field", ^{}, NSTextView.class);
    MCGuiStep(@"Escape closes the find bar", ^{ Key(c.window, kVK_Escape, 0); });
    MCGuiStep(@"the browser beside the editor, on a local page", ^{
        [c toggleBrowser:nil];
        [c.browser navigateToString:[NSURL fileURLWithPath:F(@"wide.html")].absoluteString];
    });
    MCGuiWait(@"it loads", 20, ^BOOL {
        return [c.browser.userProbe.title isEqualToString:@"Wide"] && !c.browser.userProbe.loading;
    });
    cmdB(@"the browser's address bar", ^{ [c.browser focusURLBar]; }, NSTextView.class);
    cmdB(@"the browser's page", ^{ FocusPage(c); }, WKWebView.class);
    MCGuiStep(@"a terminal", ^{ [c focusTerminal:nil]; });
    MCGuiWait(@"its prompt", 20, ^BOOL { return c.terminal.atPrompt; });
    cmdB(@"the terminal's grid", ^{ [c focusTerminal:nil]; }, TerminalGridView.class);
    MCGuiStep(@"a Markdown preview", ^{
        GCHECK([c showFile:F(@"notes.md")]);
        GCHECK(c.previewMode && !c.textView.editable);
    });
    cmdB(@"the Markdown preview", ^{ [c.window makeFirstResponder:c.textView]; },
         NSClassFromString(@"CodeTextView"));
    MCGuiStep(@"Cmd+B hides Source Control in the sidebar's place and brings it back", ^{
        [c toggleSourceControl:nil];
        GCHECK(c.gitPanelVisible && SidebarShown(c));
        NSResponder *fr = c.window.firstResponder;
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(!SidebarShown(c));
        // The keyboard was in the panel's list: it goes to the document.
        GCHECK(fr != c.window.firstResponder);
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(SidebarShown(c) && c.gitPanelVisible && c.gitPanel.superview == c.splitView);
        [c toggleSourceControl:nil];
        GCHECK(!c.gitPanelVisible);
    });
    MCGuiStep(@"the Agents panel, then Processes, hide and come back too", ^{
        [c toggleAgents:nil];
        GCHECK(c.agentsVisible && SidebarShown(c));
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(!SidebarShown(c));
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(SidebarShown(c) && c.agentsVisible);
        [c toggleAgents:nil];
        [c toggleProcesses:nil];
        GCHECK(c.processesVisible && SidebarShown(c));
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(!SidebarShown(c));
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(SidebarShown(c) && c.processesVisible);
        [c toggleProcesses:nil];
        GCHECK(!c.processesVisible);
    });
    MCGuiStep(@"with the right pane dragged shut, Cmd+B gives it the window back", ^{
        [c.window makeFirstResponder:c.outline];
        [c.splitView setPosition:c.splitView.bounds.size.width ofDividerAtIndex:0];
        GCHECK(c.rightAreaCollapsed);
        Key(c.window, kVK_ANSI_B, kCmd);
        MCGuiNote(@"after Cmd+B: sidebar %.0f, right pane %.0f, collapsed %d", SidebarWidth(c),
                  NSWidth(c.rightArea.frame), c.rightAreaCollapsed);
        GCHECK(!SidebarShown(c) && !c.rightAreaCollapsed && NSWidth(c.rightArea.frame) > 1000);
        Key(c.window, kVK_ANSI_B, kCmd);
        GCHECK(SidebarShown(c) && !c.rightAreaCollapsed);
        [c.splitView setPosition:300 ofDividerAtIndex:0];
        [c.window makeFirstResponder:c.textView];
    });
    __block BOOL postedBefore = NO;
    MCGuiStep(@"Cmd+B posted to the app as a keyboard's, from the editor", ^{
        [c.window makeFirstResponder:c.textView];
        postedBefore = SidebarShown(c);
        Post(kVK_ANSI_B, kCmd);
    });
    MCGuiWait(@"the posted Cmd+B hides it", 5, ^BOOL { return SidebarShown(c) != postedBefore; });
    MCGuiStep(@"and posted again from the terminal brings it back", ^{
        [c focusTerminal:nil];
        Post(kVK_ANSI_B, kCmd);
    });
    MCGuiWait(@"the second posted Cmd+B shows it", 5, ^BOOL { return SidebarShown(c) == postedBefore; });

    // --------------------------------------------------- the browser's zoom
    MCGuiStep(@"the browser's page has the keyboard: Cmd+=, Cmd+- and Cmd+0 zoom it", ^{
        GCHECK([c showFile:F(@"a.txt")]);
        WKWebView *web = UserWeb(c);
        if (@available(macOS 12.3, *)) GCHECK(web.configuration.preferences.elementFullscreenEnabled);
        FocusPage(c);
        GCHECK([c.window.firstResponder isKindOfClass:WKWebView.class]);
        GCHECK(Near(web.pageZoom, 1));
        Key(c.window, kVK_ANSI_Equal, kCmd);
        GCHECK(Near(web.pageZoom, 1.15));
        GCHECK(!c.browser.zoomLabel.hidden);
        GCHECK_EQ_STR(c.browser.zoomLabel.stringValue, @"115%");
        Key(c.window, kVK_ANSI_Equal, kCmd | NSEventModifierFlagShift);   // Cmd+plus
        GCHECK(Near(web.pageZoom, 1.25));
        Key(c.window, kVK_ANSI_Minus, kCmd);
        Key(c.window, kVK_ANSI_Minus, kCmd);
        Key(c.window, kVK_ANSI_Minus, kCmd);
        GCHECK(Near(web.pageZoom, 0.85));
        GCHECK_EQ_STR(c.browser.zoomLabel.stringValue, @"85%");
        MCGuiSnapshot(c.browser, @"zoomfit-browser-toolbar-85");
        // The editor's zoom never moved, and the keyboard stayed.
        GCHECK(MCEditorZoom() == 0);
        GCHECK([c.window.firstResponder isKindOfClass:WKWebView.class]);
        Key(c.window, kVK_ANSI_0, kCmd);   // Actual Size, not Focus File Tree
        GCHECK(Near(web.pageZoom, 1));
        GCHECK([c.window.firstResponder isKindOfClass:WKWebView.class]);
        for (int i = 0; i < 12; i++) Key(c.window, kVK_ANSI_Minus, kCmd);
        GCHECK(Near(web.pageZoom, 0.5));   // Safari's smallest
        Key(c.window, kVK_ANSI_0, kCmd);
    });
    MCGuiStep(@"the address bar counts as the page", ^{
        [c.browser focusURLBar];
        Key(c.window, kVK_ANSI_Equal, kCmd);
        GCHECK(Near(UserWeb(c).pageZoom, 1.15));
        GCHECK(MCEditorZoom() == 0);
        Key(c.window, kVK_ANSI_0, kCmd);
        GCHECK(Near(UserWeb(c).pageZoom, 1));
    });
    MCGuiWait(@"the percentage leaves the toolbar after a moment", 10, ^BOOL {
        return c.browser.zoomLabel.hidden;
    });
    MCGuiStep(@"with the keyboard in the editor Cmd+= and Cmd+0 are the editor's", ^{
        [c.window makeFirstResponder:c.textView];
        MCPageForgetPointer();
        Key(c.window, kVK_ANSI_Equal, kCmd);
        GCHECK(MCEditorZoom() == 1);
        GCHECK(Near(UserWeb(c).pageZoom, 1));
        Key(c.window, kVK_ANSI_Minus, kCmd);
        GCHECK(MCEditorZoom() == 0);
        Key(c.window, kVK_ANSI_0, kCmd);
        GCHECK(c.window.firstResponder == c.outline);   // Focus File Tree
        GCHECK(Near(UserWeb(c).pageZoom, 1));
    });
    MCGuiStep(@"after a click in the page, Cmd+- zooms the page, with the keyboard elsewhere", ^{
        [c.window makeFirstResponder:c.textView];
        Click(UserWeb(c));
        // The keyboard back in the editor, whatever the click did with it:
        // the click is what counts.
        [c.window makeFirstResponder:c.textView];
        GCHECK(MCPagePointedAt(c.window) == c.browser);
        Key(c.window, kVK_ANSI_Minus, kCmd);
        GCHECK(Near(UserWeb(c).pageZoom, 0.85));
        GCHECK(MCEditorZoom() == 0);
        Key(c.window, kVK_ANSI_Minus, kCmd);
        GCHECK(Near(UserWeb(c).pageZoom, 0.75));
        // A key that does something else (an arrow), and the pointer no
        // longer counts.
        Key(c.window, kVK_RightArrow, 0);
        GCHECK(MCPagePointedAt(c.window) == nil);
        Key(c.window, kVK_ANSI_Minus, kCmd);
        GCHECK(MCEditorZoom() == -1 && Near(UserWeb(c).pageZoom, 0.75));
        MCEditorZoomBy(0);
    });
    MCGuiStep(@"the zoom is kept for the site, and a page of it opens at that zoom", ^{
        NSDictionary *kept = [MCAppDefaults() dictionaryForKey:@"MCBrowserZoom"];
        GCHECK(Near([kept[@"file:"] doubleValue], 0.75));
        [c.browser navigateToString:[NSURL fileURLWithPath:F(@"other.html")].absoluteString];
    });
    MCGuiWait(@"the other page", 20, ^BOOL {
        return [c.browser.userProbe.title isEqualToString:@"Other"] && !c.browser.userProbe.loading;
    });
    MCGuiStep(@"it shows at 75%", ^{
        GCHECK(Near(UserWeb(c).pageZoom, 0.75));
        FocusPage(c);
        Key(c.window, kVK_ANSI_0, kCmd);
        GCHECK(Near(UserWeb(c).pageZoom, 1));
        GCHECK([MCAppDefaults() dictionaryForKey:@"MCBrowserZoom"][@"file:"] == nil);
        [c.browser navigateToString:[NSURL fileURLWithPath:F(@"wide.html")].absoluteString];
    });
    MCGuiWait(@"the wide page again", 20, ^BOOL {
        return [c.browser.userProbe.title isEqualToString:@"Wide"] && !c.browser.userProbe.loading;
    });
    __block double shownBefore = 0, contentBefore = 0;
    MCGuiAsync(@"the page is wider than the pane", 10, ^(MCGuiDone done) {
        [c.browser.userProbe readWidths:^(double shown, double content, NSString *error) {
            GCHECK(!error);
            shownBefore = shown;
            contentBefore = content;
            MCGuiNote(@"the pane shows %.0f of the page's %.0f pixels", shown, content);
            GCHECK(shown > 300 && content >= 1600 && content > shown);
            done();
        }];
    });
    MCGuiStep(@"Zoom to Fit Width, from the menu", ^{
        FocusPage(c);
        GCHECK([NSApp sendAction:@selector(zoomPageToFitWidth:) to:nil from:nil]);
    });
    MCGuiWait(@"the zoom comes down", 10, ^BOOL { return UserWeb(c).pageZoom < 0.99; });
    MCGuiAsync(@"now the whole width shows", 10, ^(MCGuiDone done) {
        const double z = UserWeb(c).pageZoom;
        MCGuiNote(@"fit width: %.3f", z);
        GCHECK(fabs(z - shownBefore / contentBefore) < 0.01);
        [c.browser.userProbe readWidths:^(double shown, double content, NSString *error) {
            GCHECK(!error && content <= shown + 0.5);
            if (content > shown + 0.5) MCGuiNote(@"shown %.1f, content %.1f", shown, content);
            done();
        }];
    });
    MCGuiAsync(@"a picture of the fitted page", 15, ^(MCGuiDone done) {
        if (!shots.length) {
            done();
            return;
        }
        [c.browser.userProbe snapshotTo:[shots stringByAppendingPathComponent:@"zoomfit-browser-fit.png"]
                                   full:NO overwrite:YES done:^(NSDictionary *, NSString *error) {
            GCHECK(!error);
            done();
        }];
    });
    MCGuiStep(@"Zoom to Fit Page is a PDF's only; Actual Size back from the menu", ^{
        GCHECK(![c validateMenuItem:ItemFor(@selector(zoomPageToFit:))]);
        GCHECK([c validateMenuItem:ItemFor(@selector(zoomPageToFitWidth:))]);
        GCHECK([NSApp sendAction:@selector(zoomPageToActualSize:) to:nil from:nil]);
        GCHECK(Near(UserWeb(c).pageZoom, 1));
    });

    // ----------------------------------------------------- Maximize Pane
    __block NSRect editorWas, browserWas, termWas;
    MCGuiStep(@"Maximize Pane from the browser: it takes the whole right area", ^{
        editorWas = c.editorScroll.frame;
        browserWas = c.browser.frame;
        termWas = c.terminalPanel.frame;
        GCHECK(!c.editorScroll.hidden && !c.browser.hidden && !c.terminalPanel.hidden);
        [c.browser focusURLBar];
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_M, kCtrlCmd, nil, nil));
        GCHECK(c.maximizedPane == MCPaneBrowser);
        GCHECK(NSEqualRects(c.browser.frame, c.rightArea.bounds));
        GCHECK(c.editorScroll.hidden && c.terminalPanel.hidden && c.tabStrip.hidden);
        GCHECK(c.termDivider.hidden && c.browserDivider.hidden);
        MCGuiSnapshot(c.window.contentView, @"zoomfit-maximized-browser");
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_M, kCtrlCmd, nil, nil));
        GCHECK(c.maximizedPane == MCPaneNone);
        GCHECK(NSEqualRects(c.editorScroll.frame, editorWas) &&
               NSEqualRects(c.browser.frame, browserWas) &&
               NSEqualRects(c.terminalPanel.frame, termWas));
        GCHECK(!c.editorScroll.hidden && !c.browser.hidden && !c.terminalPanel.hidden);
    });
    MCGuiStep(@"from the terminal, the terminal", ^{
        [c focusTerminal:nil];
        Key(c.window, kVK_ANSI_M, kCtrlCmd);
        GCHECK(c.maximizedPane == MCPaneTerminal);
        GCHECK(NSEqualRects(c.terminalPanel.frame, c.rightArea.bounds));
        GCHECK(c.editorScroll.hidden && c.browser.hidden);
        Key(c.window, kVK_ANSI_M, kCtrlCmd);
        GCHECK(c.maximizedPane == MCPaneNone && NSEqualRects(c.terminalPanel.frame, termWas));
    });
    MCGuiStep(@"from the editor, the editor; showing the terminal ends it", ^{
        [c.window makeFirstResponder:c.textView];
        Key(c.window, kVK_ANSI_M, kCtrlCmd);
        GCHECK(c.maximizedPane == MCPaneEditor);
        GCHECK(c.browser.hidden && c.terminalPanel.hidden && !c.editorScroll.hidden);
        GCHECK(fabs(NSWidth(c.editorScroll.frame) - NSWidth(c.rightArea.bounds)) < 0.5);
        GCHECK(FitsAcross(c.editorScroll));
        NSMenuItem *it = ItemFor(@selector(toggleMaximizedPane:));
        GCHECK([c validateMenuItem:it] && it.state == NSControlStateValueOn);
        [c toggleTerminal:nil];   // shown, not closed
        GCHECK(c.maximizedPane == MCPaneNone && c.terminalVisible && !c.terminalPanel.hidden);
        GCHECK(NSEqualRects(c.editorScroll.frame, editorWas));
    });

    // ------------------------------------------------------- the editor wraps
    MCGuiStep(@"beside the browser, with line numbers and the ruler, a long line wraps", ^{
        [c.window makeFirstResponder:c.textView];
        GCHECK(MCEditorWordWrap());
        GCHECK(!c.browser.hidden && NSMaxX(c.editorScroll.frame) <= NSMinX(c.browser.frame) + 0.5);
        GCHECK(c.editorScroll.rulersVisible);   // the line numbers
        GCHECK(c.textView.rulerColumns.count > 0);
        GCHECK(FitsAcross(c.editorScroll));
        MCGuiSnapshot(c.window.contentView, @"zoomfit-wrapped-beside-browser");
    });
    MCGuiStep(@"zoomed in four steps, it still wraps", ^{
        for (int i = 0; i < 4; i++) Key(c.window, kVK_ANSI_Equal, kCmd);
        GCHECK(MCEditorZoom() == 4);
        GCHECK(FitsAcross(c.editorScroll));
    });
    MCGuiStep(@"the browser's share made larger, it still wraps", ^{
        c.browserFraction = 0.7;
        [c relayoutRightArea];
        GCHECK(FitsAcross(c.editorScroll));
        c.browserFraction = 0.5;
        [c relayoutRightArea];
        MCEditorZoomBy(0);
    });
    MCGuiStep(@"a Markdown preview wraps", ^{
        GCHECK([c showFile:F(@"notes.md")]);
        GCHECK(c.previewMode);
        GCHECK(FitsAcross(c.editorScroll));
    });
    MCGuiStep(@"a diff wraps", ^{
        NSString *diff = [NSString stringWithFormat:
            @"diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1,2 +1,2 @@\n short\n-%@\n+%@!\n",
            longLine, longLine];
        [c showDiffNamed:@"a.txt" path:F(@"a.txt") data:[diff dataUsingEncoding:NSUTF8StringEncoding]];
        GCHECK(c.isDiff && !c.diffScroll.hidden);
        GCHECK(FitsAcross(c.diffScroll));
    });
    MCGuiStep(@"a PDF fits the width of its pane", ^{
        GCHECK([c showFile:F(@"letter.pdf")]);
        GCHECK(c.isPDF && !c.pdfView.hidden && c.pdfView.autoScales);
        [c.pdfView layoutDocumentView];
        NSScrollView *sv = c.pdfView.documentView.enclosingScrollView;
        GCHECK(sv && PageFitsAcross(c.pdfView, sv) && FitsAcross(sv));
    });

    // ---------------------------------------------------------- PDF zoom
    MCGuiStep(@"a PDF with the keyboard: Cmd+=, Cmd+0, Cmd+-, Fit Page, Fit Width", ^{
        PDFView *pdf = c.pdfView;
        [c.window makeFirstResponder:pdf];
        GCHECK(MCPageViewForResponder(c.window.firstResponder) == pdf);
        const CGFloat fit = pdf.scaleFactor;
        Key(c.window, kVK_ANSI_Equal, kCmd);
        GCHECK(!pdf.autoScales && pdf.scaleFactor > fit);
        GCHECK(MCEditorZoom() == 0);
        Key(c.window, kVK_ANSI_0, kCmd);
        GCHECK(Near(pdf.scaleFactor, 1));
        GCHECK(MCPageViewForResponder(c.window.firstResponder) == pdf);
        Key(c.window, kVK_ANSI_Minus, kCmd);
        GCHECK(Near(pdf.scaleFactor, 0.85));
        GCHECK([c validateMenuItem:ItemFor(@selector(zoomPageToFit:))]);
        GCHECK([NSApp sendAction:@selector(zoomPageToFit:) to:nil from:nil]);
        // The whole of the page: its height within the view's.
        const NSSize page = [pdf.currentPage boundsForBox:pdf.displayBox].size;
        MCGuiNote(@"fit page: %.3f for a page %.0f x %.0f in a view %.0f x %.0f", pdf.scaleFactor,
                  page.width, page.height, NSWidth(pdf.bounds), NSHeight(pdf.bounds));
        GCHECK(page.height * pdf.scaleFactor <= NSHeight(pdf.bounds) + 0.5);
        GCHECK(page.width * pdf.scaleFactor <= NSWidth(pdf.bounds) + 0.5);
        GCHECK(page.height * pdf.scaleFactor > NSHeight(pdf.bounds) * 0.8);
        MCGuiSnapshot(pdf, @"zoomfit-pdf-fit-page");
        GCHECK([NSApp sendAction:@selector(zoomPageToFitWidth:) to:nil from:nil]);
        GCHECK(pdf.autoScales && fabs(pdf.scaleFactor - fit) < 0.01);
    });

    // ------------------------------------------------------ word wrap off
    MCGuiStep(@"Option+Z in the editor turns wrapping off: the long line runs on", ^{
        GCHECK([c showFile:F(@"a.txt")]);
        [c.window makeFirstResponder:c.textView];
        NSString *before = [c.textView.string copy];
        Key(c.window, kVK_ANSI_Z, NSEventModifierFlagOption);
        GCHECK([c.textView.string isEqualToString:before]);   // no Ω typed
        GCHECK(!MCEditorWordWrap());
        GCHECK(c.textView.horizontallyResizable);
        [c.editorScroll tile];
        GCHECK(NSWidth(c.textView.frame) > c.editorScroll.contentView.bounds.size.width + 100);
        NSMenuItem *it = ItemFor(@selector(toggleWordWrap:));
        GCHECK([c validateMenuItem:it] && it.state == NSControlStateValueOff);
        MCGuiSnapshot(c.window.contentView, @"zoomfit-wrap-off");
    });
    MCGuiStep(@"a Markdown preview still wraps; back in the source, it runs on", ^{
        GCHECK([c showFile:F(@"notes.md")]);
        GCHECK(c.previewMode && FitsAcross(c.editorScroll));
        GCHECK([c showFile:F(@"a.txt")]);
        GCHECK(c.textView.horizontallyResizable);
    });
    MCGuiStep(@"Option+Z again wraps it", ^{
        [c.window makeFirstResponder:c.textView];
        Key(c.window, kVK_ANSI_Z, NSEventModifierFlagOption);
        GCHECK(MCEditorWordWrap() && !c.textView.horizontallyResizable);
        GCHECK(FitsAcross(c.editorScroll));
    });
    MCGuiStep(@"editor.word-wrap = off in the settings, and back", ^{
        MCGuiSetSettings([settingsBefore stringByAppendingString:@"\neditor.word-wrap = off\n"]);
        GCHECK(!MCEditorWordWrap() && c.textView.horizontallyResizable);
        MCGuiSetSettings(settingsBefore);
        GCHECK(MCEditorWordWrap() && !c.textView.horizontallyResizable);
        GCHECK(FitsAcross(c.editorScroll));
    });
    MCGuiStep(@"Option+Z in the terminal is the terminal's", ^{
        [c focusTerminal:nil];
        Key(c.window, kVK_ANSI_Z, NSEventModifierFlagOption);
        GCHECK(MCEditorWordWrap());
    });

    MCGuiStep(@"the window goes", ^{
        MCEditorZoomBy(0);
        [MCAppDefaults() removeObjectForKey:@"MCBrowserZoom"];
        MCGuiSetKeyWindow(nil);
        [c.window performClose:nil];
    });
}
