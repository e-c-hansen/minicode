// WindowRestoreSuite.mm — `make guitest SUITES=window_restore`: quitting and
// launching again from the Dock brings the window back as it was (W1). The
// suite runs the app twice more as child processes in the same scratch
// world: the first (_restore_writer) opens a folder (a git repository with
// one commit and a change since), arranges its window (frame, sidebar
// width, three tabs with a caret far down one, a Markdown file left in its
// source, unsaved changes in one, an untitled buffer as the session's host
// leaves one, the review page, a commit's page and a side-by-side
// comparison, the split editor, a terminal split in two with one pane
// named, Problems, a file selected deep in the tree), checks that with
// files.hot-exit = off quitting asks (and Cancel keeps everything), and
// quits as Cmd+Q does, which with hot exit (the default) asks nothing at
// all; the second (_restore_reader) is launched with no folder, as the Dock
// launches it, and finds every one of those back: the unsaved tab and the
// untitled one still unsaved with their text, the pages made again from git.
//
// The suite's own process is a third mutible on the same sessions.json,
// with windows of its own, and it writes them down as every mutible does
// (three seconds after a tab or window changes). Until 2026-11-17 each
// mutible wrote its windows over the whole record, so a write of the
// parent's landing between the children's (CI, 44044357638: utf16 closed
// its window three seconds before the reader ran) left the parent's window
// where the restored folder's should be: the same loss a user running two
// mutibles at once met. The parent now writes at every point between the
// children on purpose, and each time the children's windows must stay.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "SessionStore.h"
#import "GitPanel.h"
#import "ChangeReviewPage.h"

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
BOOL Git(NSString *dir, NSArray<NSString *> *args) {
    MCGitResult *r = MCGitRunSync(dir, args);
    if (!r.ok) MCGuiNote(@"git %@ failed: %@", [args componentsJoinedByString:@" "], r.errorText);
    return r.ok;
}
// The backups in the store that name a file in the folder.
NSUInteger BackupsOfFolder() {
    NSString *dir = NSProcessInfo.processInfo.environment[@"MUTIBLE_BACKUP_DIR"];
    NSUInteger n = 0;
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]) {
        if (![f.pathExtension isEqualToString:@"mcbackup"]) continue;
        NSString *t = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:f]
                                                encoding:NSUTF8StringEncoding error:nil];
        if ([t containsString:@"/restore/"]) n++;
    }
    return n;
}
NSString *SettingsText() {
    return [NSString stringWithContentsOfFile:NSProcessInfo.processInfo.environment[@"MUTIBLE_SETTINGS"]
                                     encoding:NSUTF8StringEncoding error:nil] ?: @"";
}
// The window's tab whose page or comparison is of a kind, else nil.
MCTab *PageTab(EditorController *c, NSString *kind) {
    for (MCTab *t in [c valueForKey:@"tabs"]) {
        if ([kind isEqualToString:@"compare"]) {
            if (t.comparison && !t.page) return t;
            continue;
        }
        if (![t.page isKindOfClass:MCChangeReviewPage.class]) continue;
        MCChangeReviewPage *p = (MCChangeReviewPage *)t.page;
        if ([kind isEqualToString:@"commit"] == (p.commit.length > 0)) return t;
    }
    return nil;
}
MCTab *FileTab(EditorController *c, NSString *name) {
    for (MCTab *t in [c valueForKey:@"tabs"])
        if ([t.currentPath.lastPathComponent isEqualToString:name]) return t;
    return nil;
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
        // A repository with one commit, and a change since for the review.
        GCHECK(Git(Folder(), @[@"init", @"-q"]));
        GCHECK(Git(Folder(), @[@"add", @"-A"]));
        GCHECK(Git(Folder(), @[@"commit", @"-q", @"-m", @"First"]));
        MCGuiWriteFile(In(@"src/deep/x.c"), @"int x = 1;\n");
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
            for (NSString *line in [out componentsSeparatedByString:@"\n"])   // the backups as it quit
                if ([line containsString:@"backups of the folder"]) MCGuiNote(@"writer: %@", line);
            done();
        });
    });
    MCGuiStep(@"it wrote down what to check", ^{
        GCHECK([[NSFileManager defaultManager] fileExistsAtPath:Expect()]);
    });
    MCGuiStep(@"its quit kept the unsaved tab and the untitled one in the backups, and wrote nothing", ^{
        MCGuiNote(@"backups of the folder: %lu", (unsigned long)BackupsOfFolder());
        GCHECK(BackupsOfFolder() == 2);
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:In(@"c.py") encoding:NSUTF8StringEncoding error:nil],
                      @"print('hi')\n");
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:In(@"Untitled-1")]);
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
    MCGuiStep(@"an older mutible's user defaults are carried over as they were", ^{
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
    MCGuiStep(@"three tabs: c.py unsaved, b.md in its source, a caret far down a.txt", ^{
        GCHECK([c showFile:In(@"c.py")]);
        NSTextView *py = Text(c);
        MCGuiUndoGroup(py.undoManager, ^{
            py.selectedRange = NSMakeRange(0, 0);
            [py insertText:@"unsaved " replacementRange:py.selectedRange];
        });
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
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
    MCGuiStep(@"an untitled buffer, as the session's host leaves one: unsaved, never on disk", ^{
        // The Mac makes untitled tabs only of what the session's host holds
        // without a path (EditorController+Session.mm's attachSessionHeld),
        // so one is made here the same way, without a host.
        MCTab *t = [MCTab new];
        t.untitled = YES;
        t.currentPath = In(@"Untitled-1");
        t.loaded = YES;
        t.savedText = @"";
        t.sourceText = @"notes not saved anywhere\n";
        t.dirty = YES;
        [c groupAppendTab:t preview:Model::Preview::None];
        [c reloadTabStrip];
    });
    MCGuiStep(@"the review page, a commit's page and a comparison, then a.txt again", ^{
        MCGitResult *head = MCGitRunSync(Folder(), @[@"rev-parse", @"HEAD"]);
        NSString *hash = [[[NSString alloc] initWithData:head.output encoding:NSUTF8StringEncoding]
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        GCHECK(hash.length == 40);
        [c gitReviewChanges:nil];
        GCHECK(PageTab(c, @"review") != nil);
        [c showCommitReview:hash title:[[hash substringToIndex:7] stringByAppendingString:@" First"]
                     preview:NO];   // kept, as Return or a double-click keeps it
        GCHECK(PageTab(c, @"commit") != nil);
        // As `mutible diff --keep src/deep/x.c` makes it: HEAD's against the disk's.
        MCComparison *cmp = [MCComparison new];
        cmp.title = @"x.c (HEAD ↔ working copy)";
        cmp.leftLabel = @"HEAD: src/deep/x.c";
        cmp.rightLabel = @"src/deep/x.c (working copy)";
        cmp.leftText = @"int x;\n";
        cmp.rightText = @"int x = 1;\n";
        cmp.path = In(@"src/deep/x.c");
        cmp.leftPath = In(@"src/deep/x.c");
        cmp.leftRev = @"HEAD";
        cmp.rightPath = In(@"src/deep/x.c");
        cmp.rightRev = @"";
        [c showComparison:cmp preview:NO];
        GCHECK(PageTab(c, @"compare") != nil);
        GCHECK([c showFile:In(@"a.txt")]);
        Text(c).selectedRange = NSMakeRange(
            [Text(c).string lineRangeForRange:NSMakeRange(
                [Text(c).string rangeOfString:@"\nline 250\n"].location + 1, 0)].location + 5, 3);
        [Text(c) scrollRangeToVisible:Text(c).selectedRange];
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
    MCGuiStep(@"files.hot-exit = off: Quit asks about both unsaved tabs; Cancel keeps all", ^{
        NSString *was = SettingsText();
        MCGuiSetSettings([was stringByAppendingString:@"\nfiles.hot-exit = off\n"]);
        const NSUInteger before = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);   // Save All, Don't Save, Cancel
        const NSApplicationTerminateReply r = [(id<NSApplicationDelegate>)NSApp.delegate
            applicationShouldTerminate:NSApp];
        GCHECK(r == NSTerminateCancel);
        NSArray *shown = MCGuiAlertsShown();
        MCGuiNote(@"alerts: %@", shown);
        GCHECK(shown.count == before + 1);
        if (shown.count == before + 1) GCHECK_EQ_STR(shown.lastObject, @"Save changes to 2 files?");
        GCHECK([FileTab(c, @"c.py") dirty] && [FileTab(c, @"Untitled-1") dirty]);
        MCGuiSetSettings(was);   // hot exit again, the default
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
        MCGuiNote(@"before quitting: %lu backups of the folder, unsaved %d",
                  (unsigned long)BackupsOfFolder(), (int)c.hasUnsavedChanges);
        // Hot exit: no question at all on the way out, unsaved tabs or not.
        const NSUInteger alerts = MCGuiAlertsShown().count, sheets = MCGuiSheetsShown().count;
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationWillTerminateNotification object:nil queue:nil
                    usingBlock:^(NSNotification *) {
            MCGuiNote(@"at quit: %lu backups of the folder", (unsigned long)BackupsOfFolder());
            const BOOL quiet = MCGuiAlertsShown().count == alerts && MCGuiSheetsShown().count == sheets;
            if (!quiet) MCGuiNote(@"asked at quit: %@ %@", MCGuiAlertsShown(), MCGuiSheetsShown());
            MCGuiCheck(quiet, @"Quit asked nothing", __FILE__, __LINE__);
            fflush(stdout);
        }];
        fflush(stdout);
        [NSApp terminate:nil];   // as Cmd+Q: the windows are written down, then exit
        MCGuiCheck(NO, @"Quit went on (it was cancelled)", __FILE__, __LINE__);
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
    MCGuiStep(@"its file tabs and pages, a.txt on show with its caret and scroll", ^{
        NSMutableArray *paths = [NSMutableArray array];
        for (MCTab *t in [c valueForKey:@"tabs"])
            if (t.currentPath) [paths addObject:t.currentPath];
        MCGuiNote(@"tabs: %@", [paths valueForKey:@"lastPathComponent"]);
        GCHECK(paths.count == 4);
        for (NSString *name in @[@"a.txt", @"b.md", @"c.py", @"Untitled-1"])
            GCHECK([[paths valueForKey:@"lastPathComponent"] containsObject:name]);
        GCHECK(PageTab(c, @"review") != nil);
        GCHECK(PageTab(c, @"commit") != nil);
        GCHECK(PageTab(c, @"compare") != nil);
        GCHECK([(NSArray *)[c valueForKey:@"tabs"] count] == 7);
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
    MCGuiStep(@"c.py: back unsaved with the text typed, the disk's as saved, a note", ^{
        MCTab *py = FileTab(c, @"c.py");
        GCHECK(py != nil);
        if (py) [c activateTab:py];
        GCHECK_EQ_STR(Text(c).string, @"unsaved print('hi')\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK_EQ_STR([[c valueForKey:@"settingsLabel"] stringValue], @"Restored unsaved changes to c.py");
    });
    MCGuiStep(@"Untitled-1: back untitled and unsaved, still not on disk", ^{
        MCTab *u = FileTab(c, @"Untitled-1");
        GCHECK(u != nil);
        if (u) [c activateTab:u];
        GCHECK(u.untitled && !u.gone);
        GCHECK_EQ_STR(Text(c).string, @"notes not saved anywhere\n");
        GCHECK([[c valueForKey:@"dirty"] boolValue]);
        GCHECK(![c.window.title containsString:@"(deleted)"]);
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:In(@"Untitled-1")]);
    });
    MCGuiStep(@"the review page, read again from git", ^{
        MCTab *t = PageTab(c, @"review");
        if (t) [c activateTab:t];
    });
    MCGuiWait(@"it shows x.c's change", 20, ^BOOL {
        MCChangeReviewPage *p = (MCChangeReviewPage *)PageTab(c, @"review").page;
        return p.loaded && [[p.rowDescriptions componentsJoinedByString:@"\n"] containsString:@"x.c"];
    });
    MCGuiStep(@"the commit's page, the same commit", ^{
        MCTab *t = PageTab(c, @"commit");
        if (t) [c activateTab:t];
        MCChangeReviewPage *p = (MCChangeReviewPage *)t.page;
        GCHECK(p.commit.length == 40);
        GCHECK([p.pageTitle hasSuffix:@" First"]);
        MCGuiNote(@"commit tab: %@", p.pageTitle);
    });
    MCGuiWait(@"it reads", 20, ^BOOL {
        MCChangeReviewPage *p = (MCChangeReviewPage *)PageTab(c, @"commit").page;
        return p.loaded && p.rowDescriptions.count > 0;
    });
    MCGuiStep(@"the comparison, its title and labels kept", ^{
        MCTab *t = PageTab(c, @"compare");
        if (t) [c activateTab:t];
        GCHECK_EQ_STR(t.comparison.title, @"x.c (HEAD ↔ working copy)");
        GCHECK_EQ_STR(t.comparison.leftLabel, @"HEAD: src/deep/x.c");
    });
    MCGuiWait(@"its sides read again: HEAD's and the disk's", 20, ^BOOL {
        MCComparison *cmp = PageTab(c, @"compare").comparison;
        return [cmp.leftText isEqualToString:@"int x;\n"] && [cmp.rightText isEqualToString:@"int x = 1;\n"];
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
