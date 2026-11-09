// PreviewTabsSuite.mm — `make guitest SUITES=preview_tabs`: VS Code's preview
// tab (workbench.enable-preview, EditorTabs.h "The preview tab"). A single
// click on a file in the tree opens it in the preview tab, its title in
// italics, which the next single click replaces, so tabs do not pile up; a
// double-click on the file, Cmd+click, an edit, a save, a double-click on
// the tab's title or Keep Open keeps it. Quick Open opens kept unless
// workbench.enable-preview-from-quick-open; Go to Definition's file opens in
// preview and keeps the preview tab it started from; a single click on a
// Find in Folder result previews it, a double-click keeps it. The setting
// off opens every file kept. A session keeps the preview tab as one, and
// leaves an agent's preview tab out.
//
// Clicks go to the tree's mouseDown: with the click count and modifiers a
// real mouse gives (a window never on screen gets no mouse-down through
// sendEvent:); the release is queued for the outline's tracking loop.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "Lsp.h"
#import "Search.h"
#import "TabStrip.h"

namespace {

NSString *Dir(void) { return [MCGuiScratch() stringByAppendingPathComponent:@"preview-tabs"]; }
NSString *F(NSString *rel) { return [Dir() stringByAppendingPathComponent:rel]; }

NSArray<MCTab *> *TabsOf(EditorController *c) { return [c valueForKey:@"tabs"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }
MCTabStrip *Strip(EditorController *c) { return [c valueForKey:@"tabStrip"]; }

// The tabs as names, the preview tab's in brackets: "a.txt [b.txt]".
NSString *Row(EditorController *c) {
    NSMutableArray *names = [NSMutableArray array];
    for (MCTab *t in TabsOf(c)) {
        NSString *n = t.currentPath.lastPathComponent ?: @"?";
        [names addObject:t.preview || t.agentPreview ? [NSString stringWithFormat:@"[%@]", n] : n];
    }
    return [names componentsJoinedByString:@" "];
}

// The strip draws the same tabs in italics where they are previews.
BOOL StripAgrees(EditorController *c) {
    NSArray<MCTab *> *tabs = TabsOf(c);
    NSArray<MCTabStripItem *> *items = Strip(c).items;
    if (items.count != tabs.count) return NO;
    for (NSUInteger i = 0; i < tabs.count; i++)
        if (items[i].preview != (tabs[i].preview || tabs[i].agentPreview)) return NO;
    return YES;
}

NSInteger RowOf(EditorController *c, NSString *path) {
    for (NSInteger r = 0; r < c.outline.numberOfRows; r++)
        if ([((FileItem *)[c.outline itemAtRow:r]).path isEqualToString:path]) return r;
    return -1;
}

// A press on the file's row with `clicks` and `flags`, released at once.
BOOL ClickFile(EditorController *c, NSString *path, NSInteger clicks, NSEventModifierFlags flags) {
    ClickOutline *tree = c.outline;
    const NSInteger row = RowOf(c, path);
    if (row < 0) return NO;
    const NSRect r = [tree rectOfRow:row];
    const NSPoint at = [tree convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil];
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at
                                  modifierFlags:flags timestamp:now
                                   windowNumber:c.window.windowNumber context:nil
                                    eventNumber:0 clickCount:clicks pressure:1];
    NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:at
                                modifierFlags:flags timestamp:now
                                 windowNumber:c.window.windowNumber context:nil
                                  eventNumber:0 clickCount:clicks pressure:0];
    [NSApp postEvent:up atStart:NO];
    [tree mouseDown:down];
    return YES;
}

NSDictionary *SavedSession(EditorController *c) {
    NSUserDefaults *d = [[NSUserDefaults alloc]
        initWithSuiteName:NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"]];
    return [d dictionaryForKey:@"MCTabSessions"][c.rootPath.stringByResolvingSymlinksInPath];
}

}  // namespace

MC_GUI_SUITE(preview_tabs) {
    __block EditorController *c = nil;

    MCGuiStep(@"a folder of files, a window of its own, no tabs", ^{
        for (NSString *n in @[@"a.txt", @"b.txt", @"c.txt", @"d.txt", @"e.txt"])
            MCGuiWriteFile(F(n), [NSString stringWithFormat:@"%@\nline 2\n", n]);
        MCGuiWriteFile(F(@"main.go"), @"package main\n\nfunc main() { helper() }\n");
        MCGuiWriteFile(F(@"helper.go"), @"package main\n\nfunc helper() {}\n");
        const NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:Dir()]]];
        c = MCGuiController();
        GCHECK(MCGuiControllers().count == before + 1);
        MCGuiSetKeyWindow(c.window);
        GCHECK(TabsOf(c).count == 0);
        GCHECK(RowOf(c, F(@"a.txt")) >= 0);
    });

    MCGuiStep(@"a single click opens the file in the preview tab, in italics", ^{
        GCHECK(ClickFile(c, F(@"a.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"[a.txt]");
        GCHECK(Active(c).preview && StripAgrees(c));
        GCHECK(Strip(c).items.firstObject.preview);
    });
    MCGuiStep(@"another single click replaces it: still one tab", ^{
        GCHECK(ClickFile(c, F(@"b.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"[b.txt]");
        GCHECK([Active(c).currentPath isEqualToString:F(@"b.txt")] && StripAgrees(c));
        // A replaced preview tab is not a closed tab to reopen.
        GCHECK(![c canReopenClosedTab]);
    });
    MCGuiStep(@"a double-click on the file keeps it", ^{
        GCHECK(ClickFile(c, F(@"b.txt"), 1, 0));
        GCHECK(ClickFile(c, F(@"b.txt"), 2, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt");
        GCHECK(!Active(c).preview && StripAgrees(c));
    });
    MCGuiStep(@"the next click opens a new preview tab beside the kept one", ^{
        GCHECK(ClickFile(c, F(@"c.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt [c.txt]");
    });
    MCGuiStep(@"a quick click on another row counts once: it replaces the preview", ^{
        // The window server counts clicks by time alone: a click within half a
        // second of the last is clickCount 2 wherever it lands.
        GCHECK(ClickFile(c, F(@"d.txt"), 2, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt [d.txt]");
    });
    MCGuiStep(@"Cmd+click opens a kept tab and leaves the preview tab alone", ^{
        GCHECK(ClickFile(c, F(@"c.txt"), 1, NSEventModifierFlagCommand));
        GCHECK_EQ_STR(Row(c), @"b.txt [d.txt] c.txt");
        GCHECK([Active(c).currentPath isEqualToString:F(@"c.txt")] && !Active(c).preview);
        GCHECK(c.outline.selectedRow == RowOf(c, F(@"c.txt")));
        // Cmd+click on the preview tab's file keeps it.
        GCHECK(ClickFile(c, F(@"d.txt"), 1, NSEventModifierFlagCommand));
        GCHECK_EQ_STR(Row(c), @"b.txt d.txt c.txt");
        GCHECK(StripAgrees(c));
    });
    MCGuiStep(@"a click on a file open in a kept tab only shows it", ^{
        GCHECK(ClickFile(c, F(@"b.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt d.txt c.txt");
        GCHECK([Active(c).currentPath isEqualToString:F(@"b.txt")]);
    });

    MCGuiStep(@"typing in the preview tab keeps it", ^{
        GCHECK(ClickFile(c, F(@"a.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt [a.txt] d.txt c.txt");
        NSTextView *tv = [c valueForKey:@"textView"];
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"typed " replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK_EQ_STR(Row(c), @"b.txt a.txt d.txt c.txt");
        GCHECK(StripAgrees(c));
        // Saved and closed, for the steps below.
        [c saveCurrentFile:nil];
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });
    MCGuiStep(@"saving the preview tab keeps it", ^{
        GCHECK(ClickFile(c, F(@"e.txt"), 1, 0));
        GCHECK(Active(c).preview);
        [c saveCurrentFile:c];   // Cmd+S, which also writes a clean file
        GCHECK(!Active(c).preview && StripAgrees(c));
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });
    MCGuiStep(@"a double-click on the preview tab's title keeps it", ^{
        GCHECK(ClickFile(c, F(@"e.txt"), 1, 0));
        MCTabStrip *strip = Strip(c);
        const NSInteger i = strip.activeIndex;
        GCHECK(i >= 0 && strip.items[(NSUInteger)i].preview);
        const NSRect r = [strip rectForTab:i];
        const NSPoint p = [strip convertPoint:NSMakePoint(NSMidX(r) - 10, NSMidY(r)) toView:nil];
        NSEvent *e = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:p
                                   modifierFlags:0 timestamp:0 windowNumber:c.window.windowNumber
                                         context:nil eventNumber:0 clickCount:2 pressure:1];
        [strip mouseDown:e];
        GCHECK(!Active(c).preview && !strip.items[(NSUInteger)strip.activeIndex].preview);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        [c.window makeFirstResponder:c.outline];
    });
    MCGuiStep(@"Keep Open, in the tab's menu and the File menu, keeps it", ^{
        GCHECK(ClickFile(c, F(@"e.txt"), 1, 0));
        NSMenuItem *validated = [[NSMenuItem alloc] initWithTitle:@"Keep Open"
                                                           action:@selector(keepActiveTabOpen:)
                                                    keyEquivalent:@""];
        GCHECK([c validateMenuItem:validated]);
        const NSInteger i = (NSInteger)[TabsOf(c) indexOfObject:Active(c)];
        NSMenu *m = [c menuForTabAtIndex:i];
        NSMenuItem *keep = [m itemWithTitle:@"Keep Open"];
        GCHECK(keep != nil && keep.enabled);
        [NSApp sendAction:keep.action to:keep.target from:keep];
        GCHECK(!Active(c).preview && StripAgrees(c));
        // A kept tab has no Keep Open, and the File menu's is disabled.
        GCHECK([[c menuForTabAtIndex:i] itemWithTitle:@"Keep Open"] == nil);
        GCHECK(![c validateMenuItem:validated]);
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });

    MCGuiStep(@"Quick Open opens kept, as VS Code's default", ^{
        GCHECK(ClickFile(c, F(@"a.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt [a.txt] d.txt c.txt");
        [c openFileFromQuickOpen:F(@"e.txt")];
        GCHECK_EQ_STR(Row(c), @"b.txt [a.txt] e.txt d.txt c.txt");
        GCHECK(c.outline.selectedRow == RowOf(c, F(@"e.txt")));
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });
    MCGuiStep(@"with workbench.enable-preview-from-quick-open it replaces the preview", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nworkbench.enable-preview-from-quick-open = on\n");
        [c openFileFromQuickOpen:F(@"e.txt")];
        GCHECK_EQ_STR(Row(c), @"b.txt [e.txt] d.txt c.txt");
    });
    MCGuiStep(@"with workbench.enable-preview off, a click opens a kept tab", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nworkbench.enable-preview = off\n");
        GCHECK(ClickFile(c, F(@"a.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt [e.txt] a.txt d.txt c.txt");
        [c openFileFromQuickOpen:F(@"a.txt")];
        GCHECK(!Active(c).preview);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    MCGuiStep(@"the session keeps the preview tab as one, and no agent's preview tab", ^{
        GCHECK([c openFileForAgent:F(@"a.txt") preview:YES]);
        GCHECK(Active(c).agentPreview);
        [c saveSession];
        NSDictionary *s = SavedSession(c);
        MCGuiNote(@"session: %@", s);
        GCHECK_EQ_STR(s[@"preview"], F(@"e.txt"));
        GCHECK(![s[@"files"] containsObject:F(@"a.txt")]);
        GCHECK([s[@"files"] containsObject:F(@"e.txt")]);
        [c dropAllTabs];
        GCHECK(TabsOf(c).count == 0);
        [c restoreSession];
        GCHECK_EQ_STR(Row(c), @"b.txt [e.txt] d.txt c.txt");
        GCHECK(StripAgrees(c));
        // The preview tab restored is replaced by the next click as before.
        GCHECK(ClickFile(c, F(@"a.txt"), 1, 0));
        GCHECK_EQ_STR(Row(c), @"b.txt [a.txt] d.txt c.txt");
    });

    MCGuiStep(@"Find in Folder for a word in two files", ^{
        MCGuiWriteFile(F(@"found/one.txt"), @"first\nneedle one\n");
        MCGuiWriteFile(F(@"found/two.txt"), @"needle two\n");
        [c openSearch:nil];
        SearchPanel *p = [c valueForKey:@"searchPanel"];
        [p setScope:F(@"found")];
        p.field.stringValue = @"needle";
        [p runSearch:nil];
    });
    MCGuiWait(@"the results", 10, ^BOOL {
        SearchPanel *p = [c valueForKey:@"searchPanel"];
        return !p.busy && [[p valueForKey:@"hits"] count] == 2;
    });
    MCGuiStep(@"a single click on a result line previews it, the keyboard left in the results", ^{
        SearchPanel *p = [c valueForKey:@"searchPanel"];
        NSOutlineView *results = (NSOutlineView *)p.table;
        [results expandItem:nil expandChildren:YES];
        NSInteger lineOne = -1, lineTwo = -1;
        for (NSInteger r = 0; r < results.numberOfRows; r++) {
            id item = [results itemAtRow:r];
            if (![item respondsToSelector:@selector(line)]) continue;
            NSString *path = [item valueForKey:@"path"];
            if ([path hasSuffix:@"one.txt"]) lineOne = r;
            if ([path hasSuffix:@"two.txt"]) lineTwo = r;
        }
        GCHECK(lineOne >= 0 && lineTwo >= 0);
        BOOL (^click)(NSInteger, NSInteger) = ^BOOL(NSInteger row, NSInteger clicks) {
            if (row < 0) return NO;
            const NSRect r = [results rectOfRow:row];
            const NSPoint at = [results convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil];
            NSWindow *w = results.window;
            NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at
                                          modifierFlags:0 timestamp:0 windowNumber:w.windowNumber
                                                context:nil eventNumber:0 clickCount:clicks
                                               pressure:1];
            NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:at
                                        modifierFlags:0 timestamp:0 windowNumber:w.windowNumber
                                              context:nil eventNumber:0 clickCount:clicks
                                             pressure:0];
            [NSApp postEvent:up atStart:NO];
            [results mouseDown:down];
            return YES;
        };
        // The results' window has the keyboard, as it would after a real click.
        MCGuiSetKeyWindow(p.window);
        GCHECK(click(lineOne, 1));
        GCHECK_EQ_STR(Row(c), @"b.txt [one.txt] d.txt c.txt");
        GCHECK([Active(c).currentPath isEqualToString:F(@"found/one.txt")] && Active(c).preview);
        GCHECK(p.window.firstResponder == results);   // the keyboard stays in the results
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK_EQ_STR([tv.string substringWithRange:tv.selectedRange], @"needle one\n");
        // The next result's click replaces it in the preview tab.
        GCHECK(click(lineTwo, 1));
        GCHECK([Active(c).currentPath isEqualToString:F(@"found/two.txt")] && Active(c).preview);
        GCHECK([c tabForPath:F(@"found/one.txt")] == nil);
        // A double-click keeps it.
        GCHECK(click(lineTwo, 2));
        GCHECK([Active(c).currentPath isEqualToString:F(@"found/two.txt")] && !Active(c).preview);
        GCHECK(StripAgrees(c));
        [p.window orderOut:nil];
        MCGuiSetKeyWindow(c.window);
        [c closeTab:Active(c) prompt:NO];
    });

    // ------------------------------------------------ code navigation
    MCGuiStep(@"Go files and the stand-in server for Go, definition answering helper.go", ^{
        // Both preview tabs go first, for a clean row.
        for (MCTab *t in [TabsOf(c) copy])
            if (t.preview || t.agentPreview) [c closeTab:t prompt:NO];
        NSString *helperURI = [NSURL fileURLWithPath:F(@"helper.go")].absoluteString;
        NSDictionary *script = @{@"rules": @[@{
            @"method": @"textDocument/definition",
            @"result": @{@"uri": helperURI,
                         @"range": @{@"start": @{@"line": @2, @"character": @5},
                                     @"end": @{@"line": @2, @"character": @11}}}}]};
        NSString *cmd = MCGuiFakeLsp(@"preview-nav", script, nil, nil);
        MCGuiFakeLspClearLog(@"preview-nav");
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = true\nlsp.go = %@\n", cmd]);
        GCHECK(ClickFile(c, F(@"main.go"), 1, 0));
        GCHECK([Row(c) containsString:@"[main.go]"] && TabsOf(c).count == 4);
    });
    MCGuiWait(@"main.go open on the server", 30, ^BOOL {
        return MCGuiFakeLspReceived(@"preview-nav", @"textDocument/didOpen").count >= 1;
    });
    MCGuiStep(@"Go to Definition", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        const NSRange r = [tv.string rangeOfString:@"helper()"];
        tv.selectedRange = NSMakeRange(r.location + 1, 0);
        [[c valueForKey:@"lsp"] goToDefinition];
    });
    MCGuiWait(@"helper.go on show", 20, ^BOOL {
        return [Active(c).currentPath isEqualToString:F(@"helper.go")];
    });
    MCGuiStep(@"the target is the preview tab; main.go, where it started, is kept", ^{
        MCGuiNote(@"tabs: %@", Row(c));
        GCHECK(TabsOf(c).count == 5 && [Row(c) containsString:@"[helper.go]"]);
        GCHECK(![c tabForPath:F(@"main.go")].preview);
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK_EQ_STR([tv.string substringWithRange:tv.selectedRange], @"helper");
        GCHECK(StripAgrees(c));
    });

    MCGuiStep(@"tidy up: the window closes", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        for (MCTab *t in [TabsOf(c) copy]) [c closeTab:t prompt:NO];
        [c.window performClose:nil];
        MCGuiSetKeyWindow(nil);
    });
    MCGuiWait(@"gone", 10, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}
