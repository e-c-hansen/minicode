// BrowserFindSuite.mm — `make guitest SUITES=browser_find`: Cmd+F in the
// browser pane (the owner's report, 2026-11-09) opens a find bar with the
// keyboard in it, from the page and from the address bar; typing counts and
// finds, Return and Cmd+G move on, and Esc closes it and gives the page the
// keyboard back. Elsewhere Cmd+F stays the editor's. Only a local page.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "Browser.h"
#import "ControlServer.h"
#import "TerminalFindBar.h"
#import <Carbon/Carbon.h>
#import <WebKit/WebKit.h>

namespace {

NSString *gDir;
const NSEventModifierFlags kCmd = NSEventModifierFlagCommand;

WKWebView *Web(EditorController *c) { return [c.browser webViewOfTab:(NSUInteger)c.browser.selectedTab]; }

void FocusPage(EditorController *c) {
    [c.browser focusURLBar];
    [Web(c) setValue:@YES forKey:@"toolbarNavigation"];
    [c.window makeFirstResponder:Web(c)];
}

BOOL InPage(EditorController *c) {
    NSResponder *r = c.window.firstResponder;
    return [r isKindOfClass:NSView.class] && [(NSView *)r isDescendantOf:Web(c)];
}

NSString *CountText(EditorController *c) {
    return [[c.browser.findBar valueForKey:@"_count"] stringValue];
}

void TypeQuery(EditorController *c, NSString *q) {
    MCTerminalFindBar *bar = c.browser.findBar;
    bar.field.stringValue = q;
    [bar controlTextDidChange:[NSNotification notificationWithName:NSControlTextDidChangeNotification
                                                            object:bar.field]];
}

NSString *gSelected;
void AskSelection(EditorController *c) {
    gSelected = nil;
    [Web(c) evaluateJavaScript:@"String(getSelection())" completionHandler:^(id r, NSError *) {
        gSelected = [r isKindOfClass:NSString.class] ? r : @"";
    }];
}

}  // namespace

MC_GUI_SUITE(browser_find) {
    __block EditorController *c = nil;
    gDir = [MCGuiScratch() stringByAppendingPathComponent:@"browser-find"];
    NSString *page = [gDir stringByAppendingPathComponent:@"page.html"];

    MCGuiStep(@"a page in the pane of a window of the suite's own", ^{
        MCGuiWriteFile(page,
            @"<html><head><title>Find</title></head><body>"
            @"<p>one Needle here</p><p>two needle there</p><p>three needle last</p>"
            @"<p>and nothing else</p></body></html>");
        MCGuiWriteFile([gDir stringByAppendingPathComponent:@"one.txt"], @"needle in a file\n");
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:gDir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        GCHECK([c showFile:[gDir stringByAppendingPathComponent:@"one.txt"]]);
        [NSApp sendAction:@selector(openBrowserTab:) to:nil from:nil];
        GCHECK(c.browser != nil && c.browserVisible);
        [c.browser navigateToString:[NSURL fileURLWithPath:page].absoluteString];
    });
    MCGuiWait(@"the page loads", 20, ^BOOL {
        return [c.browser.pageTitle isEqualToString:@"Find"] && !Web(c).isLoading;
    });
    MCGuiStep(@"Cmd+F in the page opens the bar", ^{
        FocusPage(c);
        GCHECK(InPage(c));
        GCHECK(c.browser.findBar == nil);
        // A Command key in a page goes to WebKit's page first, and back to
        // the menus a moment later when the page does not take it.
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_F, kCmd, nil, nil));
    });
    MCGuiWait(@"the bar has the keyboard", 5, ^BOOL {
        return c.browser.findBar && !c.browser.findBar.hidden && c.browser.findBar.isEditing;
    });
    MCGuiStep(@"the bar sits inside the pane, over the page", ^{
        const NSRect f = c.browser.findBar.frame;
        GCHECK(NSContainsRect(c.browser.bounds, f));
        GCHECK(NSWidth(f) > 100);
        TypeQuery(c, @"needle");
    });
    MCGuiWait(@"every match, either case, is counted", 5, ^BOOL {
        return [CountText(c) isEqualToString:@"3"];
    });
    MCGuiStep(@"the first is selected", ^{ AskSelection(c); });
    MCGuiWait(@"the selection", 5, ^BOOL { return gSelected != nil; });
    MCGuiStep(@"Return moves on, Match Case narrows it", ^{
        GCHECK_EQ_STR(gSelected.lowercaseString, @"needle");
        c.browser.findBar.onNext(YES);
        c.browser.findBar.onNext(NO);
        NSButton *matchCase = [c.browser.findBar valueForKey:@"_caseButton"];
        [matchCase performClick:nil];
        GCHECK(c.browser.findBar.matchCase);
    });
    MCGuiWait(@"only the lower-case ones", 5, ^BOOL {
        return [CountText(c) isEqualToString:@"2"];
    });
    MCGuiStep(@"something the page does not hold", ^{
        NSButton *matchCase = [c.browser.findBar valueForKey:@"_caseButton"];
        [matchCase performClick:nil];
        TypeQuery(c, @"zzzq");
    });
    MCGuiWait(@"no results", 5, ^BOOL { return [CountText(c) isEqualToString:@"No results"]; });
    MCGuiStep(@"Esc closes the bar, the page has the keyboard again", ^{
        c.browser.findBar.onClose();
        GCHECK(c.browser.findBar.hidden);
        GCHECK(InPage(c));
    });
    MCGuiStep(@"Cmd+F in the address bar opens it too", ^{
        [c.browser focusURLBar];
        GCHECK(c.browser.findBar.hidden);
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_F, kCmd, nil, nil));
    });
    MCGuiWait(@"the bar is back with the keyboard", 5, ^BOOL {
        return !c.browser.findBar.hidden && c.browser.findBar.isEditing;
    });
    MCGuiStep(@"with the keyboard in the editor, Cmd+F is the editor's", ^{
        c.browser.findBar.onClose();
        NSTextView *tv = [c valueForKey:@"textView"];
        [c.window makeFirstResponder:tv];
        GCHECK(MCGuiPerformMenuKey(c.window, kVK_ANSI_F, kCmd, nil, nil));
        GCHECK(c.browser.findBar.hidden);
        id find = [c valueForKey:@"editorFind"];
        GCHECK(find != nil);
    });
    MCGuiStep(@"the window goes", ^{
        MCGuiSetKeyWindow(nil);
        [c.window close];
    });
}
