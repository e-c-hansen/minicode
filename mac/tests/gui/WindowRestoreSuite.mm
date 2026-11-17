// WindowRestoreSuite.mm — `make guitest SUITES=window_restore`: quitting and
// launching again from the Dock brings the window back as it was (W1). The
// suite runs the app twice more as child processes in the same scratch
// world: the first (_restore_writer) opens a folder, arranges its window
// (frame, sidebar width, three tabs with a caret far down one, a Markdown
// file left in its source, the split editor, a terminal split in two with
// one pane named, Problems, a file selected deep in the tree) and quits as
// Cmd+Q does; the second (_restore_reader) is launched with no folder, as
// the Dock launches it, and finds every one of those back.
//
// The suite's own process is a third minicode on the same sessions.json,
// with windows of its own, and it writes them down as every minicode does
// (three seconds after a tab or window changes). Until 2026-11-17 each
// minicode wrote its windows over the whole record, so a write of the
// parent's landing between the children's (CI, 44044357638: utf16 closed
// its window three seconds before the reader ran) left the parent's window
// where the restored folder's should be: the same loss a user running two
// minicodes at once met. The parent now writes at every point between the
// children on purpose, and each time the children's windows must stay.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "SessionStore.h"

namespace {
NSString *Folder() { return [MCGuiScratch() stringByAppendingPathComponent:@"restore"]; }
NSString *In(NSString *name) { return [Folder() stringByAppendingPathComponent:name]; }
NSString *Expect() { return [MCGuiScratch() stringByAppendingPathComponent:@"restore-expect.plist"]; }
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
NSString *Real(NSString *p) { return p.stringByResolvingSymlinksInPath; }

// This process's windows written down now, as its three-second timer, a
// window closing or quitting writes them, and in the file.
void ParentWritesItsWindows() {
    [EditorController saveOpenWindows];
    MCSessionsFlush();
}

// The stored windows on `root`, and those this process owns.
size_t WindowsOn(const TabSessions::Store &store, NSString *root) {
    size_t n = 0;
    for (const TabSessions::Window &w : store.windows) n += w.root == Real(root).UTF8String;
    return n;
}
size_t ParentWindows(const TabSessions::Store &store) {
    size_t n = 0;
    for (const TabSessions::Window &w : store.windows) n += w.ownerPid == (long)getpid();
    return n;
}
// Whether this process has a window the record would list.
bool ParentHasWindows() { return [EditorController openWindowsFrontFirst].count > 0; }

// The line a character is on, from 0.
NSUInteger LineOf(NSString *s, NSUInteger at) {
    NSUInteger n = 0;
    for (NSUInteger i = 0; i < MIN(at, s.length); i++)
        if ([s characterAtIndex:i] == '\n') n++;
    return n;
}

NSString *LongText() {
    NSMutableString *s = [NSMutableString string];
    // Short lines, so the split editor's narrower pane wraps none of them.
    for (int i = 0; i < 400; i++) [s appendFormat:@"line %d\n", i];
    return s;
}

// Where the window goes: inside the first screen's visible frame (the one
// a window with no screen of its own goes to), so it comes back unmoved.
NSRect WantedFrame() {
    NSScreen *screen = NSScreen.screens.firstObject;
    NSRect v = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1024, 740);
    return NSMakeRect(NSMinX(v) + 30, NSMinY(v) + 20, MIN(900, NSWidth(v) - 60),
                      MIN(620, NSHeight(v) - 40));
}
}  // namespace

// ------------------------------------------------------- the parent's part
MC_GUI_SUITE(window_restore) {
    MCGuiStep(@"a folder of files, one deep in a subfolder", ^{
        MCGuiWriteFile(In(@"a.txt"), LongText());
        MCGuiWriteFile(In(@"b.md"), @"# Notes\n\nSome *text*.\n");
        MCGuiWriteFile(In(@"c.py"), @"print('hi')\n");
        MCGuiWriteFile(In(@"src/deep/x.c"), @"int x;\n");
        [[NSFileManager defaultManager] removeItemAtPath:Expect() error:nil];
    });
    MCGuiStep(@"this process writes its own windows down before the first app starts", ^{
        ParentWritesItsWindows();
        const TabSessions::Store store = MCSessionsRead();
        MCGuiNote(@"parent's windows: %zu of %zu", ParentWindows(store), store.windows.size());
        GCHECK(ParentWindows(store) == [EditorController openWindowsFrontFirst].count);
        GCHECK(WindowsOn(store, Folder()) == 0);
    });
    MCGuiAsync(@"a first app arranges its window and quits", 90, ^(MCGuiDone done) {
        MCGuiRunChild(@"_restore_writer", Folder(), ^(int status, NSString *out) {
            GCHECK(status == 0);
            GCHECK(![out containsString:@"FAIL"]);
            if (status != 0 || [out containsString:@"FAIL"]) MCGuiNote(@"writer:\n%@", out);
            done();
        });
    });
    MCGuiStep(@"it wrote down what to check", ^{
        GCHECK([[NSFileManager defaultManager] fileExistsAtPath:Expect()]);
    });
    MCGuiStep(@"its quit kept this process's windows beside its own", ^{
        const TabSessions::Store store = MCSessionsRead();
        GCHECK(WindowsOn(store, Folder()) == 1);
        if (ParentHasWindows()) GCHECK(ParentWindows(store) > 0);
    });
    MCGuiStep(@"this process writes again before the second app starts: the first's stay", ^{
        // The race CI lost, made certain: before the fix this left only the
        // parent's window, and the second app brought that one back.
        ParentWritesItsWindows();
        const TabSessions::Store store = MCSessionsRead();
        GCHECK(WindowsOn(store, Folder()) == 1);
        // This process's own first, then the first app's.
        if (ParentHasWindows())
            GCHECK(!store.windows.empty() && store.windows[0].ownerPid == (long)getpid());
    });
    MCGuiAsync(@"a second app, launched with nothing to open, brings it back", 90,
               ^(MCGuiDone done) {
        MCGuiRunChild(@"_restore_reader", nil, ^(int status, NSString *out) {
            GCHECK(status == 0);
            GCHECK(![out containsString:@"FAIL"]);
            if (status != 0 || [out containsString:@"FAIL"]) MCGuiNote(@"reader:\n%@", out);
            done();
        });
    });
    MCGuiStep(@"the windows and the folder's tabs are in sessions.json", ^{
        GCHECK([[NSFileManager defaultManager] fileExistsAtPath:MCSessionsFilePath()]);
        const TabSessions::Store store = MCSessionsRead();
        GCHECK(WindowsOn(store, Folder()) == 1);
        GCHECK(store.folders.count(Real(Folder()).UTF8String) == 1);
    });
    MCGuiStep(@"this process writes once more after the second app quit: its window stays", ^{
        ParentWritesItsWindows();
        const TabSessions::Store store = MCSessionsRead();
        GCHECK(WindowsOn(store, Folder()) == 1);
        if (ParentHasWindows()) GCHECK(ParentWindows(store) > 0);
        // The folder's window is the second app's, written as it went
        // (closing the last window keeps it), not the parent's.
        for (const TabSessions::Window &w : store.windows)
            if (w.root == Real(Folder()).UTF8String) GCHECK(w.ownerPid != (long)getpid());
    });
    MCGuiStep(@"an older minicode's user defaults are carried over as they were", ^{
        NSString *root = @"/old/proj";
        NSData *record = [@"history" dataUsingEncoding:NSUTF8StringEncoding];
        NSDictionary *folders = @{
            root: @{@"files": @[@"/old/proj/a.txt", @"/old/proj/b.md", @"/old/proj/c.txt"],
                    @"active": @1, @"preview": @"/old/proj/c.txt",
                    @"sidePreview": @[@"/old/proj/b.md"], @"used": @1762700000.0,
                    @"groups": @{@"groups": @[@[@"/old/proj/a.txt"],
                                              @[@"/old/proj/b.md", @"/old/proj/c.txt"]],
                                 @"active": @[@"/old/proj/a.txt", @"/old/proj/b.md"],
                                 @"focused": @1, @"fraction": @0.4},
                    @"browser": @{@"page": @{@"tabs": @[@{@"url": @"https://example.com/",
                                                          @"title": @"Example",
                                                          @"state": record,
                                                          @"archived": @YES},
                                                        @{@"url": @"http://localhost:3000/"}],
                                             @"active": @1},
                                  @"visible": @YES, @"fraction": @0.3}},
            @"/old/empty": @{@"files": @[], @"active": @0},
        };
        TabSessions::Window w;
        w.root = "/old/proj";
        const TabSessions::Store store = MCSessionsFromDefaults(
            folders, @(TabSessions::encodeWindows({w}).c_str()));
        GCHECK(store.windows.size() == 1 && store.windows[0].root == "/old/proj");
        GCHECK(store.folders.size() == 1 && store.folders.count("/old/proj") == 1);
        if (!store.folders.count("/old/proj")) return;
        // Written to the file and read back, as the next launch reads it.
        const TabSessions::Session s =
            TabSessions::decodeStore(TabSessions::encodeStore(store)).folders.at("/old/proj");
        GCHECK(s.tabs.size() == 3 && s.active == 1 && s.preview == 2);
        GCHECK(s.tabs.size() == 3 && s.tabs[1].side && !s.tabs[0].side && !s.tabs[2].side);
        GCHECK(s.used == 1762700000.0);
        GCHECK(s.split && s.groups.focused == 1 && s.groups.fraction == 0.4);
        GCHECK(s.groups.groups[1].size() == 2 && s.groups.active[1] == "/old/proj/b.md");
        GCHECK(s.browser.size() == 2 && s.browserActive == 1);
        GCHECK(s.browserShown && s.browserFraction == 0.3);
        // The pane gets its tabs back as it kept them, the record's bytes
        // and form included.
        NSDictionary *page = MCSessionBrowserPage(s);
        NSArray *tabs = page[@"tabs"];
        GCHECK(tabs.count == 2 && [page[@"active"] intValue] == 1);
        GCHECK(tabs.count == 2 && [tabs[0][@"state"] isEqual:record] &&
               [tabs[0][@"archived"] boolValue] && [tabs[0][@"title"] isEqual:@"Example"]);
        GCHECK(tabs.count == 2 && tabs[1][@"state"] == nil &&
               [tabs[1][@"url"] isEqual:@"http://localhost:3000/"]);
    });
}

// ------------------------------------------ the first child: arrange, quit
MC_GUI_SUITE(_restore_writer) {
    __block EditorController *c = nil;
    MCGuiStep(@"the window: its frame and sidebar", ^{
        c = MCGuiController();
        GCHECK_EQ_STR(Real(c.rootPath), Real(Folder()));
        [c.window setFrame:WantedFrame() display:NO];
        [c setSidebarWidth:300 hidden:NO];
    });
    MCGuiStep(@"three tabs: b.md in its source, a caret far down a.txt", ^{
        GCHECK([c showFile:In(@"c.py")]);
        GCHECK([c showFile:In(@"b.md")]);
        if ([[c valueForKey:@"previewMode"] boolValue]) [c togglePreview:nil];
        GCHECK(![[c valueForKey:@"previewMode"] boolValue]);
        GCHECK([c showFile:In(@"a.txt")]);
        NSTextView *tv = Text(c);
        const NSRange line = [tv.string lineRangeForRange:NSMakeRange(
            [tv.string rangeOfString:@"\nline 250\n"].location + 1, 0)];
        tv.selectedRange = NSMakeRange(line.location + 5, 3);
        [tv scrollRangeToVisible:tv.selectedRange];
        GCHECK(tv.visibleRect.origin.y > 100);
    });
    MCGuiStep(@"the split editor, a terminal split in two, Problems", ^{
        [c splitEditor:nil];
        GCHECK([[c valueForKey:@"split"] boolValue]);
        [c toggleTerminal:nil];
        c.terminalHeight = 250;
        [c relayoutRightArea];
        TerminalView *second = [c.terminalPanel splitActiveInDirectory:In(@"src")];
        GCHECK(second != nil);
        second.userTitle = @"server";
        if (![[c valueForKey:@"problemsVisible"] boolValue]) [c toggleProblems:nil];
        GCHECK([[c valueForKey:@"problemsVisible"] boolValue]);
    });
    MCGuiStep(@"a file deep in the tree selected", ^{
        [c revealPath:In(@"src/deep/x.c") andOpen:NO];
        NSOutlineView *o = c.outline;
        GCHECK(o.selectedRow >= 0);
        if (o.selectedRow >= 0)
            GCHECK_EQ_STR(Real([(FileItem *)[o itemAtRow:o.selectedRow] path]),
                          Real(In(@"src/deep/x.c")));
    });
    MCGuiStep(@"what the second app should find, then Quit", ^{
        NSRange sel;
        NSUInteger top = 0;
        CGFloat offset = 0;
        GCHECK([c liveTextPlace:&sel top:&top offset:&offset]);
        NSRect f = c.window.frame;
        NSDictionary *want = @{
            @"frame": NSStringFromRect(f),
            @"selection": NSStringFromRange(Text(c).selectedRange),
            @"topLine": @(LineOf(Text(c).string, top)),
            @"sidebar": @([c sidebarWidthToShow]),
        };
        GCHECK([want writeToFile:Expect() atomically:YES]);
        MCGuiNote(@"writer: %@", want);
        fflush(stdout);
        [NSApp terminate:nil];   // as Cmd+Q: the windows are written down, then exit
    });
}

// ------------------------------- the second child: launched from the Dock
MC_GUI_SUITE(_restore_reader) {
    __block EditorController *c = nil;
    __block NSDictionary *want = nil;
    MCGuiStep(@"one window, on the folder, where it was", ^{
        want = [NSDictionary dictionaryWithContentsOfFile:Expect()];
        GCHECK(want != nil);
        GCHECK(MCGuiControllers().count == 1);
        c = MCGuiController();
        GCHECK_EQ_STR(Real(c.rootPath), Real(Folder()));
        const NSRect f = c.window.frame, w = NSRectFromString(want[@"frame"]);
        MCGuiNote(@"frame %@, was %@", NSStringFromRect(f), want[@"frame"]);
        GCHECK(fabs(NSMinX(f) - NSMinX(w)) < 1 && fabs(NSMinY(f) - NSMinY(w)) < 1);
        GCHECK(fabs(NSWidth(f) - NSWidth(w)) < 1 && fabs(NSHeight(f) - NSHeight(w)) < 1);
        GCHECK(fabs([c sidebarWidthToShow] - [want[@"sidebar"] doubleValue]) < 2);
    });
    MCGuiStep(@"its three tabs, a.txt on show with its caret and scroll", ^{
        NSArray *paths = [c valueForKeyPath:@"tabs.currentPath"];
        MCGuiNote(@"tabs: %@", [paths valueForKey:@"lastPathComponent"]);
        GCHECK(paths.count == 3);
        for (NSString *name in @[@"a.txt", @"b.md", @"c.py"])
            GCHECK([[paths valueForKey:@"lastPathComponent"] containsObject:name]);
        GCHECK([[c.currentPath lastPathComponent] isEqualToString:@"a.txt"]);
        NSTextView *tv = Text(c);
        GCHECK_EQ_STR(NSStringFromRange(tv.selectedRange), want[@"selection"]);
        NSRange sel;
        NSUInteger top = 0;
        CGFloat offset = 0;
        [c liveTextPlace:&sel top:&top offset:&offset];
        const long line = (long)LineOf(tv.string, top), was = [want[@"topLine"] longValue];
        MCGuiNote(@"top line %ld, was %ld", line, was);
        GCHECK(labs(line - was) <= 2);
        GCHECK([[c valueForKey:@"split"] boolValue]);
        GCHECK(c.window.firstResponder == tv);
    });
    MCGuiStep(@"the terminal, split as it was, the pane's name kept; Problems", ^{
        GCHECK([[c valueForKey:@"terminalVisible"] boolValue]);
        GCHECK(c.terminalPanel.tabCount == 1);
        NSArray<NSArray<TerminalView *> *> *cols = [c.terminalPanel columnsInTab:0];
        GCHECK(cols.count == 2);
        if (cols.count == 2) {
            GCHECK_EQ_STR(cols[1][0].userTitle, @"server");
            GCHECK_EQ_STR(Real(cols[1][0].directory), Real(In(@"src")));
            GCHECK_EQ_STR(Real(cols[0][0].directory), Real(Folder()));
        }
        GCHECK(fabs(c.terminalHeight - 250) < 1);
        GCHECK([[c valueForKey:@"problemsVisible"] boolValue]);
    });
    MCGuiStep(@"the tree: the deep file selected, its folders open", ^{
        NSOutlineView *o = c.outline;
        GCHECK(o.selectedRow >= 0);
        if (o.selectedRow >= 0)
            GCHECK_EQ_STR(Real([(FileItem *)[o itemAtRow:o.selectedRow] path]),
                          Real(In(@"src/deep/x.c")));
    });
    MCGuiStep(@"b.md comes back in its source, not its page", ^{
        MCTab *md = [c tabForPath:In(@"b.md")];
        if (!md)
            for (MCTab *t in [c valueForKey:@"tabs"])
                if ([[(id)t valueForKey:@"currentPath"] hasSuffix:@"/b.md"]) md = t;
        GCHECK(md != nil);
        if (md) [c activateTab:md];
        GCHECK([[c.currentPath lastPathComponent] isEqualToString:@"b.md"]);
        GCHECK(![[c valueForKey:@"previewMode"] boolValue]);
        GCHECK([Text(c).string hasPrefix:@"# Notes"]);
    });
}
