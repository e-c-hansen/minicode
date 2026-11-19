// GitPanelSuite.mm — `make guitest SUITES=git_panel`: Source Control
// (Ctrl+Shift+G) in a window of its own on a scratch repository. The panel
// lists a modification, a deletion and an untracked file, Space's action
// stages and unstages, a diff takes the editor's slot (and saves nothing),
// a commit with a message lands in the graph, an empty message is refused
// with git's own words, and nothing but the index and the commit changed.
#import "Harness.h"
#import "EditorController.h"
#import "EditorFont.h"
#import "AgentWindow.h"
#import "AppSettings.h"
#import "ChangeReviewPage.h"
#import "EditorConflicts.h"
#import "GitPanel.h"
#import "Lsp.h"
#import "PalettePanel.h"
#include <objc/runtime.h>
#include <vector>

namespace {
MCGitPanel *Git(EditorController *c) { return [c valueForKey:@"gitPanel"]; }
NSInteger RowOf(EditorController *c, NSString *description) {
    return (NSInteger)[[Git(c) rowDescriptions] indexOfObject:description];
}
BOOL Rows(EditorController *c, NSArray<NSString *> *rows) {
    return Git(c).inRepository && [[Git(c) rowDescriptions] isEqualToArray:rows];
}
id ActiveTab(EditorController *c) { return [c valueForKey:@"activeTab"]; }
id ActivePage(EditorController *c) { return [ActiveTab(c) valueForKey:@"page"]; }
BOOL ActiveIsPreview(EditorController *c) { return [[ActiveTab(c) valueForKey:@"preview"] boolValue]; }
NSUInteger PagesOfKind(EditorController *c, NSString *kind) {
    NSUInteger n = 0;
    for (id t in [c valueForKey:@"tabs"])
        if ([[[t valueForKey:@"page"] pageKind] isEqualToString:kind]) n++;
    return n;
}
// A press on a row as MCGitTable's mouseDown: hands it on once tracked.
void Click(EditorController *c, NSInteger row, BOOL graph, NSInteger count, BOOL sameRow,
           NSEventModifierFlags modifiers = 0) {
    [Git(c) clickedRow:row graph:graph clickCount:count sameRow:sameRow modifiers:modifiers];
}
// A PNG of w by h pixels, every pixel gray.
NSData *PngOf(NSInteger w, NSInteger h) {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4
                        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:0 bitsPerPixel:0];
    memset(rep.bitmapData, 0x80, (size_t)rep.bytesPerRow * (size_t)h);
    return [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
}
NSString *SizeLabel(NSString *side, NSInteger w, NSInteger h, NSData *png) {
    NSString *size = [NSString stringWithFormat:@"%ld \u00D7 %ld pixels, %lu bytes", (long)w, (long)h,
                                                (unsigned long)png.length];
    return side ? [NSString stringWithFormat:@"%@: %@", side, size] : size;
}
}  // namespace

MC_GUI_SUITE(git_panel) {
    __block EditorController *c = nil;
    __block NSString *git = nil;
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"gitrepo"];

    MCGuiStep(@"a repository with one commit", ^{
        git = MCGitExecutable();
        if (!git) MCGuiNote(@"no git here: the git panel checks are skipped");
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"a.txt"], @"one\ntwo\n");
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"b.txt"], @"bee\n");
    });
    MCGuiAsync(@"git init, add, commit", 30, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        NSString *sh = [NSString stringWithFormat:
            @"cd '%@' && '%@' init -q -b main && '%@' add -A && '%@' commit -qm first", repo, git, git, git];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int s, NSString *, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"git: %@", err);
            done();
        });
    });
    MCGuiStep(@"three changes; a window on it; Ctrl+Shift+G", ^{
        if (!git) return;
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"a.txt"], @"one\nTWO\n");
        [[NSFileManager defaultManager] removeItemAtPath:[repo stringByAppendingPathComponent:@"b.txt"]
                                                   error:nil];
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"c.txt"], @"new\n");
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        GCHECK(MCGuiPerformMenuKey(c.window, 5, NSEventModifierFlagControl | NSEventModifierFlagShift,
                                   nil, nil));
        GCHECK([[c valueForKey:@"gitPanelVisible"] boolValue]);
    });
    MCGuiWait(@"the changes listed", 15, ^BOOL {
        return !git || Rows(c, @[@"# Changes  3", @"W M a.txt", @"W D b.txt", @"W U c.txt"]);
    });
    // A single click opens the changes in the preview tab, as the tree
    // opens a file and VS Code's list does; arrows only move the selection.
    MCGuiStep(@"a single click on a.txt opens its changes in the preview tab", ^{
        if (!git) return;
        Click(c, RowOf(c, @"W M a.txt"), NO, 1, NO);
        MCChangeReviewPage *p = ActivePage(c);
        GCHECK([p isKindOfClass:MCChangeReviewPage.class] && [p.pageKind isEqualToString:@"review"]);
        GCHECK(ActiveIsPreview(c));
        // The keyboard stays in the list, as after a click in the tree.
        GCHECK(c.window.firstResponder != p.textView);
    });
    MCGuiWait(@"the review read", 15, ^BOOL {
        return !git || ([ActivePage(c) isKindOfClass:MCChangeReviewPage.class] && [ActivePage(c) loaded]);
    });
    MCGuiStep(@"a quick click on b.txt (counted as a second click) shows b.txt in the same tab", ^{
        if (!git) return;
        id tab = ActiveTab(c);
        const NSUInteger tabs = [[c valueForKey:@"tabs"] count];
        Click(c, RowOf(c, @"W D b.txt"), NO, 2, NO);
        GCHECK(ActiveTab(c) == tab && ActiveIsPreview(c));
        GCHECK([[c valueForKey:@"tabs"] count] == tabs);
        GCHECK(PagesOfKind(c, @"review") == 1);
        // Arrows move the selection and open nothing.
        NSTableView *list = [Git(c) valueForKey:@"table"];
        [c.window makeFirstResponder:list];
        [list selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)RowOf(c, @"W M a.txt")]
          byExtendingSelection:NO];
        NSEvent *down = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                    modifierFlags:0 timestamp:0 windowNumber:c.window.windowNumber
                                          context:nil characters:@"\uF701"
                      charactersIgnoringModifiers:@"\uF701" isARepeat:NO keyCode:125];
        [list keyDown:down];
        GCHECK(list.selectedRow == RowOf(c, @"W D b.txt"));
        GCHECK(ActiveTab(c) == tab && [[c valueForKey:@"tabs"] count] == tabs);
        // A Command click chooses rows and opens nothing either.
        Click(c, RowOf(c, @"W U c.txt"), NO, 1, NO, NSEventModifierFlagCommand);
        GCHECK(ActiveTab(c) == tab && [[c valueForKey:@"tabs"] count] == tabs);
    });
    MCGuiStep(@"a double-click keeps the tab", ^{
        if (!git) return;
        Click(c, RowOf(c, @"W D b.txt"), NO, 1, NO);
        Click(c, RowOf(c, @"W D b.txt"), NO, 2, YES);
        GCHECK([ActivePage(c) isKindOfClass:MCChangeReviewPage.class]);
        GCHECK(!ActiveIsPreview(c));
        GCHECK(PagesOfKind(c, @"review") == 1);
    });
    MCGuiStep(@"stage a.txt and c.txt", ^{
        if (!git) return;
        MCGuiNote(@"branch: %@", Git(c).branchText);
        GCHECK([Git(c).branchText containsString:@"main"]);
        [Git(c) toggleStageAtRow:RowOf(c, @"W M a.txt")];
    });
    MCGuiWait(@"a.txt staged", 10, ^BOOL {
        return !git || RowOf(c, @"S M a.txt") != NSNotFound;
    });
    MCGuiStep(@"and c.txt", ^{ if (git) [Git(c) toggleStageAtRow:RowOf(c, @"W U c.txt")]; });
    MCGuiWait(@"c.txt staged", 10, ^BOOL {
        return !git || Rows(c, @[@"# Staged changes  2", @"S M a.txt", @"S A c.txt",
                                 @"# Changes  1", @"W D b.txt"]);
    });
    MCGuiStep(@"unstage c.txt", ^{ if (git) [Git(c) toggleStageAtRow:RowOf(c, @"S A c.txt")]; });
    MCGuiWait(@"c.txt untracked again", 10, ^BOOL {
        return !git || Rows(c, @[@"# Staged changes  1", @"S M a.txt",
                                 @"# Changes  2", @"W D b.txt", @"W U c.txt"]);
    });
    MCGuiStep(@"and staged again", ^{ if (git) [Git(c) toggleStageAtRow:RowOf(c, @"W U c.txt")]; });
    MCGuiWait(@"c.txt staged again", 10, ^BOOL { return !git || RowOf(c, @"S A c.txt") != NSNotFound; });
    MCGuiStep(@"the rows draw when AppKit has no monospaced font to give", ^{
        if (!git) return;
        // +[NSFont monospacedSystemFontOfSize:weight:] answers nil now and
        // then while other threads make fonts; a status letter drawn with
        // it threw inside drawRect: and ended the app (CI, 2026-11-06).
        Method m = class_getClassMethod(NSFont.class, @selector(monospacedSystemFontOfSize:weight:));
        IMP real = method_getImplementation(m);
        method_setImplementation(m, imp_implementationWithBlock(^NSFont *(id, CGFloat, NSFontWeight) {
            return (NSFont *)nil;
        }));
        NSFont *fallback = MCMonoFont(12, NSFontWeightSemibold);
        NSView *panel = Git(c);
        NSBitmapImageRep *rep = [panel bitmapImageRepForCachingDisplayInRect:panel.bounds];
        BOOL drew = NO;
        @try {
            [panel cacheDisplayInRect:panel.bounds toBitmapImageRep:rep];
            drew = YES;
        } @catch (NSException *e) {
            MCGuiNote(@"drawing threw: %@", e.reason);
        }
        method_setImplementation(m, real);
        GCHECK(fallback != nil);
        GCHECK(drew);
    });
    MCGuiStep(@"a staged file's diff takes the editor's slot", ^{
        if (!git) return;
        [Git(c) showDiffAtRow:RowOf(c, @"S M a.txt")];
    });
    MCGuiWait(@"the diff", 10, ^BOOL { return !git || [[c valueForKey:@"isDiff"] boolValue]; });
    MCGuiStep(@"its lines; Cmd+S writes nothing", ^{
        if (!git) return;
        NSTextView *diff = [c valueForKey:@"diffView"];
        GCHECK([c.window.title hasPrefix:@"a.txt (diff)"]);
        GCHECK(diff != nil && [diff.string containsString:@"-two"] && [diff.string containsString:@"+TWO"]);
        [c saveCurrentFile:nil];
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:[repo stringByAppendingPathComponent:@"a.txt"]
                                                encoding:NSUTF8StringEncoding error:nil], @"one\nTWO\n");
    });
    MCGuiStep(@"an empty message: git refuses, and says why", ^{
        if (!git) return;
        Git(c).messageView.string = @"";
        [Git(c) commit];
    });
    MCGuiWait(@"git's words under the button", 10, ^BOOL { return !git || Git(c).errorLine.length > 0; });
    MCGuiStep(@"a message; commit", ^{
        if (!git) return;
        MCGuiNote(@"empty message: %@", Git(c).errorLine);
        Git(c).messageView.string = @"Second commit\n\nWith a body.";
        [Git(c) commit];
    });
    MCGuiWait(@"committed: b.txt's deletion left", 15, ^BOOL {
        return !git || Rows(c, @[@"# Changes  1", @"W D b.txt"]);
    });
    MCGuiWait(@"the graph shows both commits", 15, ^BOOL {
        return !git || [Git(c) graphDescriptions].count == 2;
    });
    MCGuiStep(@"the new commit on top, the message box empty", ^{
        if (!git) return;
        NSArray *g = [Git(c) graphDescriptions];
        MCGuiNote(@"graph: %@; after the commit: %@", g, Git(c).errorLine);
        GCHECK([g.firstObject containsString:@"Second commit"]);
        GCHECK([g.lastObject containsString:@"first"]);
        GCHECK([Git(c).errorLine containsString:@"Second commit"]);
        // In words, the short hash after them.
        GCHECK([Git(c).errorLine hasPrefix:@"Committed \u201CSecond commit\u201D to main.  "]);
        GCHECK(Git(c).messageView.string.length == 0);
    });
    MCGuiStep(@"the line over the graph wraps in a narrow panel and has a tooltip", ^{
        if (!git) return;
        MCGitPanel *panel = Git(c);
        NSTextField *summary = [panel valueForKey:@"summaryLabel"];
        GCHECK_EQ_STR(panel.graphSummary, @"main has no upstream: nothing is marked as pushed.");
        const NSSize was = panel.frame.size;
        [panel setFrameSize:NSMakeSize(200, was.height)];
        [panel resizeSubviewsWithOldSize:was];
        const NSSize need = [summary.cell cellSizeForBounds:NSMakeRect(0, 0, NSWidth(summary.frame), 400)];
        MCGuiNote(@"summary at 200 points: frame %@, needs %@", NSStringFromRect(summary.frame),
                  NSStringFromSize(need));
        GCHECK(NSHeight(summary.frame) + 0.5 >= need.height && NSHeight(summary.frame) > 16);
        GCHECK_EQ_STR(summary.toolTip, panel.graphSummary);
        // The graph starts under it.
        NSScrollView *graph = [panel valueForKey:@"graphScroll"];
        GCHECK(NSMinY(graph.frame) >= NSMaxY(summary.frame));
        [panel setFrameSize:was];
        [panel resizeSubviewsWithOldSize:NSMakeSize(200, was.height)];
        // The new commit from the graph, by a single click: the review
        // page, not git show text, in the preview tab.
        Click(c, 0, YES, 1, NO);
        GCHECK(ActiveIsPreview(c));
    });
    MCGuiWait(@"the commit's page", 15, ^BOOL {
        if (!git) return YES;
        id page = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
        return [page isKindOfClass:MCChangeReviewPage.class] && [page loaded];
    });
    MCGuiStep(@"its header in words, its files colored, no buttons but Side by Side", ^{
        if (!git) return;
        MCChangeReviewPage *p = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
        NSArray *rows = p.rowDescriptions;
        MCGuiNote(@"commit page: %@ / %@ / %@", p.headerTitle, p.headerDetail, rows);
        GCHECK_EQ_STR(p.pageKind, @"commit");
        GCHECK_EQ_STR(p.headerTitle, @"Second commit");
        GCHECK([p.headerDetail containsString:@"2 files, +2 -1"]);
        GCHECK([c.window.title hasPrefix:[Git(c).graphDescriptions.firstObject substringToIndex:7]] ||
               [c.window.title containsString:@"Second commit"]);
        GCHECK([rows containsObject:@"F a.txt  +1 -1"]);
        GCHECK([rows containsObject:@"F c.txt  (new)  +1"]);
        GCHECK([rows containsObject:@"- two"] && [rows containsObject:@"+ TWO"]);
        for (NSString *r in rows) GCHECK(![r containsString:@"diff --git"] && ![r hasPrefix:@"  commit "]);
        const NSInteger header = (NSInteger)[rows indexOfObject:@"F a.txt  +1 -1"];
        GCHECK(!NSIsEmptyRect([p buttonRect:@"Side by Side" row:header]));
        GCHECK(NSIsEmptyRect([p buttonRect:@"Open" row:header]));
        GCHECK(NSIsEmptyRect([p buttonRect:@"Stage" row:[p rowOfBlock:0]]));
        GCHECK(NSIsEmptyRect([p buttonRect:@"Discard" row:[p rowOfBlock:0]]));
        // A single click on the first commit replaces it in the same tab.
        Click(c, 1, YES, 1, NO);
    });
    MCGuiWait(@"the first commit in its place", 15, ^BOOL {
        if (!git) return YES;
        MCChangeReviewPage *p = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
        return [p isKindOfClass:MCChangeReviewPage.class] && [p.headerTitle isEqualToString:@"first"];
    });
    MCGuiStep(@"a root commit: every file new; one commit tab, the preview", ^{
        if (!git) return;
        MCChangeReviewPage *p = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
        GCHECK([p.rowDescriptions containsObject:@"F a.txt  (new)  +2"]);
        GCHECK(PagesOfKind(c, @"commit") == 1);
        GCHECK(ActiveIsPreview(c));
        // A double-click keeps it; the next single click opens another
        // commit in a new preview tab, and the kept one stays.
        Click(c, 1, YES, 2, YES);
        GCHECK(!ActiveIsPreview(c));
        Click(c, 0, YES, 1, NO);
        GCHECK(ActiveIsPreview(c));
        GCHECK(PagesOfKind(c, @"commit") == 2);
        // Return keeps it too.
        [Git(c) showCommitAtRow:0];
        GCHECK(!ActiveIsPreview(c));
        GCHECK(PagesOfKind(c, @"commit") == 2);
    });
    MCGuiAsync(@"git agrees: the body kept, b.txt still in HEAD", 15, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        NSString *sh = [NSString stringWithFormat:@"cd '%@' && '%@' log -1 --format=%%B && '%@' ls-tree --name-only HEAD",
                                                  repo, git, git];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int s, NSString *out, NSString *) {
            GCHECK(s == 0);
            GCHECK([out containsString:@"Second commit\n\nWith a body."]);
            GCHECK([out containsString:@"b.txt"] && [out containsString:@"c.txt"]);
            done();
        });
    });
    // Pictures: a PNG made 4 by 3, then changed to 8 by 6, committed; the
    // commit shows the two side by side, Before and After, with their sizes.
    __block NSData *small = nil, *large = nil, *added = nil;
    MCGuiStep(@"a PNG committed, then changed and committed again", ^{
        if (!git) return;
        small = PngOf(4, 3);
        large = PngOf(8, 6);
        NSString *png = [repo stringByAppendingPathComponent:@"pic.png"];
        GCHECK([small writeToFile:png atomically:YES]);
        GCHECK(MCGitRunSync(repo, @[@"add", @"pic.png"]).ok);
        GCHECK(MCGitRunSync(repo, @[@"commit", @"-qm", @"a picture"]).ok);
        GCHECK([large writeToFile:png atomically:YES]);
        GCHECK(MCGitRunSync(repo, @[@"commit", @"-qam", @"a larger picture"]).ok);
        [Git(c) refresh];
    });
    MCGuiWait(@"the graph shows it", 15, ^BOOL {
        return !git || [[Git(c) graphDescriptions].firstObject containsString:@"a larger picture"];
    });
    MCGuiStep(@"a click on it", ^{
        if (!git) return;
        Click(c, 0, YES, 1, NO);
    });
    MCGuiWait(@"the commit's page, its pictures on show", 15, ^BOOL {
        if (!git) return YES;
        MCChangeReviewPage *p = ActivePage(c);
        return [p isKindOfClass:MCChangeReviewPage.class] &&
               [p.headerTitle isEqualToString:@"a larger picture"] && p.imageViews.count > 0;
    });
    MCGuiStep(@"two image views, Before and After, with their sizes", ^{
        if (!git) return;
        MCChangeReviewPage *p = ActivePage(c);
        NSArray<NSImageView *> *views = p.imageViews;
        MCGuiNote(@"pictures: %@; rows %@", p.imageLabels, p.rowDescriptions);
        GCHECK(views.count == 2);
        GCHECK([p.imageLabels isEqualToArray:(@[SizeLabel(@"Before", 4, 3, small),
                                                SizeLabel(@"After", 8, 6, large)])]);
        GCHECK(![p.rowDescriptions containsObject:@"! A binary file: its bytes are not shown."]);
        if (views.count == 2) {
            NSBitmapImageRep *before = (NSBitmapImageRep *)views[0].image.representations.firstObject;
            NSBitmapImageRep *after = (NSBitmapImageRep *)views[1].image.representations.firstObject;
            GCHECK(before.pixelsWide == 4 && before.pixelsHigh == 3);
            GCHECK(after.pixelsWide == 8 && after.pixelsHigh == 6);
            // At their pixel sizes, side by side, over the file's rows.
            MCGuiNote(@"frames %@ %@", NSStringFromRect(views[0].frame), NSStringFromRect(views[1].frame));
            GCHECK(NSWidth(views[0].frame) == 4 && NSHeight(views[0].frame) == 3);
            GCHECK(NSWidth(views[1].frame) == 8 && NSHeight(views[1].frame) == 6);
            const NSRect a = [views[0] convertRect:views[0].bounds toView:p.textView];
            const NSRect b = [views[1] convertRect:views[1].bounds toView:p.textView];
            GCHECK(NSMinX(b) > NSMaxX(a) && NSMinY(a) == NSMinY(b));
            const NSInteger header = (NSInteger)[p.rowDescriptions indexOfObject:@"F pic.png"];
            GCHECK(header >= 0 && NSMinY(a) >= NSMaxY([p rectOfRow:header]));
        }
        MCGuiSnapshot(p.pageView, @"git-commit-picture");
        // A new picture among the uncommitted changes: shown alone.
        added = PngOf(5, 2);
        GCHECK([added writeToFile:[repo stringByAppendingPathComponent:@"new.png"] atomically:YES]);
        [c gitReviewChanges:nil];
    });
    MCGuiWait(@"the review shows new.png's picture", 15, ^BOOL {
        if (!git) return YES;
        MCChangeReviewPage *p = ActivePage(c);
        return [p isKindOfClass:MCChangeReviewPage.class] && [p.pageKind isEqualToString:@"review"] &&
               [p rowOfFile:@"new.png" staged:NO] >= 0 && p.imageViews.count == 1;
    });
    MCGuiStep(@"one picture, its size alone", ^{
        if (!git) return;
        MCChangeReviewPage *p = ActivePage(c);
        MCGuiNote(@"new picture: %@", p.imageLabels);
        GCHECK([p.imageLabels isEqualToArray:@[SizeLabel(nil, 5, 2, added)]]);
        // Staged whole, as any binary file.
        const NSInteger header = [p rowOfFile:@"new.png" staged:NO];
        GCHECK(!NSIsEmptyRect([p buttonRect:@"Stage File" row:header]));
    });
    MCGuiStep(@"the window closes", ^{
        if (!git) return;
        [c.window close];
    });
    MCGuiWait(@"gone", 5, ^BOOL { return !git || ![MCGuiControllers() containsObject:c]; });
}

// `make guitest SUITES=git_actions`: Stage All and Unstage All from the menu
// bar and the panel; Discard Changes asks, leaves the file alone on Cancel,
// restores a changed file, brings a deleted one back and moves an untracked
// one to the Trash, and Undo Discard puts each back; Discard from the menu
// bar acts on the file on show; a file row's right click has its commands;
// the branch picker switches branches, makes one from a typed name, refuses
// a bad name, and shows git's words when a switch would overwrite a change.
// The Trash is a folder in the scratch world (the real one is the user's).
namespace {
NSString *ScratchTrash() { return [MCGuiScratch() stringByAppendingPathComponent:@"Trash"]; }
IMP gRealTrash;
void UseScratchTrash(BOOL on) {
    Method m = class_getInstanceMethod(NSFileManager.class,
                                       @selector(trashItemAtURL:resultingItemURL:error:));
    if (!on) {
        if (gRealTrash) method_setImplementation(m, gRealTrash);
        gRealTrash = nil;
        return;
    }
    static int n = 0;
    gRealTrash = method_setImplementation(m, imp_implementationWithBlock(
        ^BOOL(NSFileManager *fm, NSURL *url, NSURL *__autoreleasing *out,
              NSError *__autoreleasing *err) {
            [fm createDirectoryAtPath:ScratchTrash() withIntermediateDirectories:YES
                           attributes:nil error:nil];
            NSString *to = [ScratchTrash() stringByAppendingPathComponent:
                [NSString stringWithFormat:@"%d-%@", ++n, url.lastPathComponent]];
            BOOL ok = [fm moveItemAtPath:url.path toPath:to error:err];
            if (ok && out) *out = [NSURL fileURLWithPath:to];
            return ok;
        }));
}
NSString *Read(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
NSArray<NSString *> *TrashedTexts() {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:ScratchTrash()
                                                                            error:nil])
        [out addObject:Read([ScratchTrash() stringByAppendingPathComponent:f]) ?: @""];
    return out;
}
void GitShell(NSString *what, NSString *line) {
    MCGuiAsync(what, 30, ^(MCGuiDone done) {
        MCGuiRunProcess(@[@"/bin/sh", @"-c", line], nil, ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"%@: %@%@", what, out, err);
            done();
        });
    });
}
NSView *Palette(EditorController *c) { return MCCommandPaletteIn(c.window); }
NSTextView *PaletteEditor(EditorController *c) {
    return (NSTextView *)MCCommandPaletteField(Palette(c)).currentEditor;
}
void PaletteType(EditorController *c, NSString *s) {
    [PaletteEditor(c) insertText:s replacementRange:PaletteEditor(c).selectedRange];
}
NSArray<NSString *> *MenuTitles(NSMenu *m) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSMenuItem *it in m.itemArray)
        if (!it.isSeparatorItem) [out addObject:it.title];
    return out;
}
}  // namespace

MC_GUI_SUITE(git_actions) {
    __block EditorController *c = nil;
    NSString *git = MCGitExecutable();
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"gitactions"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    NSString *a = [repo stringByAppendingPathComponent:@"a.txt"];
    NSString *b = [repo stringByAppendingPathComponent:@"b.txt"];
    NSString *cFile = [repo stringByAppendingPathComponent:@"c.txt"];
    __block NSUInteger alerts = 0;
    __block NSInteger actions = 0;
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the git action checks are skipped"); });
        return;
    }
    NSArray *changes3 = @[@"# Changes  3", @"W M a.txt", @"W D b.txt", @"W U c.txt"];
    NSArray *(^newAlerts)(void) = ^NSArray * {
        NSArray *all = MCGuiAlertsShown();
        return [all subarrayWithRange:NSMakeRange(alerts, all.count - alerts)];
    };

    GitShell(@"a repository on main, with a branch called feature", [NSString stringWithFormat:
        @"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; printf 'one\\ntwo\\n' > a.txt; "
         "echo bee > b.txt; %@ add -A; %@ commit -qm first; %@ checkout -q -b feature; "
         "echo f > f.txt; %@ add -A; %@ commit -qm feat; %@ checkout -q main; "
         "printf 'one\\nTWO\\n' > a.txt; rm b.txt; echo new > c.txt",
        repo, repo, G, G, G, G, G, G, G]);
    MCGuiStep(@"a window on it; the panel; a Trash of our own", ^{
        UseScratchTrash(YES);
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c toggleSourceControl:nil];
        Git(c).onActionDone = ^{ actions++; };
    });
    MCGuiWait(@"three changes", 15, ^BOOL { return Rows(c, changes3); });

    MCGuiStep(@"Stage All Changes from the menu bar", ^{
        GCHECK([NSApp sendAction:@selector(gitStageAll:) to:nil from:nil]);
    });
    MCGuiWait(@"all three staged", 15, ^BOOL {
        return Rows(c, @[@"# Staged changes  3", @"S M a.txt", @"S D b.txt", @"S A c.txt"]);
    });
    MCGuiStep(@"it says so; Unstage All from the panel", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Staged 3 files.");
        [Git(c) unstageAll];
    });
    MCGuiWait(@"all three back in Changes", 15, ^BOOL { return Rows(c, changes3); });
    MCGuiStep(@"Unstage All with nothing staged says so", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Unstaged 3 files.");
        actions = 0;
        [Git(c) unstageAll];
    });
    MCGuiWait(@"refused", 10, ^BOOL { return actions == 1; });

    MCGuiStep(@"Discard a.txt, then Cancel: nothing changes", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Nothing is staged.");
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        [Git(c) discardAtRow:RowOf(c, @"W M a.txt")];
        GCHECK([newAlerts() isEqualToArray:@[@"Discard the changes to a.txt?"]]);
        GCHECK_EQ_STR(Read(a), @"one\nTWO\n");
        GCHECK(Git(c).undoDiscardTitle == nil);
    });
    MCGuiStep(@"Discard a.txt, yes", ^{
        actions = 0;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Git(c) discardAtRow:RowOf(c, @"W M a.txt")];
    });
    MCGuiWait(@"a.txt as committed", 15, ^BOOL {
        return actions == 1 && Rows(c, @[@"# Changes  2", @"W D b.txt", @"W U c.txt"]);
    });
    MCGuiStep(@"its changed copy is in the Trash; Undo Discard from the menu bar", ^{
        GCHECK_EQ_STR(Read(a), @"one\ntwo\n");
        GCHECK([TrashedTexts() containsObject:@"one\nTWO\n"]);
        GCHECK([Git(c).errorLine containsString:@"in the Trash"]);
        GCHECK_EQ_STR(Git(c).undoDiscardTitle, @"Undo Discard of a.txt");
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Undo Discard"
                                                      action:@selector(gitUndoDiscard:)
                                               keyEquivalent:@""];
        GCHECK([c validateMenuItem:item]);
        GCHECK_EQ_STR(item.title, @"Undo Discard of a.txt");
        actions = 0;
        GCHECK([NSApp sendAction:@selector(gitUndoDiscard:) to:nil from:nil]);
    });
    MCGuiWait(@"a.txt changed again", 15, ^BOOL { return actions == 1 && Rows(c, changes3); });
    MCGuiStep(@"its text is back; discard the untracked c.txt", ^{
        GCHECK_EQ_STR(Read(a), @"one\nTWO\n");
        GCHECK(Git(c).undoDiscardTitle == nil);
        actions = 0;
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Git(c) discardAtRow:RowOf(c, @"W U c.txt")];
        GCHECK([newAlerts() isEqualToArray:@[@"Move c.txt to the Trash?"]]);
    });
    MCGuiWait(@"c.txt gone", 15, ^BOOL {
        return actions == 1 && Rows(c, @[@"# Changes  2", @"W M a.txt", @"W D b.txt"]);
    });
    MCGuiStep(@"in the Trash; undone from the panel", ^{
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:cFile]);
        GCHECK([TrashedTexts() containsObject:@"new\n"]);
        actions = 0;
        [Git(c) undoDiscard];
    });
    MCGuiWait(@"c.txt back", 15, ^BOOL { return actions == 1 && Rows(c, changes3); });
    MCGuiStep(@"discard b.txt's deletion", ^{
        GCHECK_EQ_STR(Read(cFile), @"new\n");
        actions = 0;
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Git(c) discardAtRow:RowOf(c, @"W D b.txt")];
        GCHECK([newAlerts() isEqualToArray:@[@"Restore b.txt?"]]);
    });
    MCGuiWait(@"b.txt restored", 15, ^BOOL {
        return actions == 1 && Rows(c, @[@"# Changes  2", @"W M a.txt", @"W U c.txt"]);
    });
    MCGuiStep(@"Discard Changes from the menu bar acts on the file on show", ^{
        GCHECK_EQ_STR(Read(b), @"bee\n");
        GCHECK([c showFile:a]);
        [c.window makeFirstResponder:[c valueForKey:@"textView"]];
        actions = 0;
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        GCHECK([NSApp sendAction:@selector(gitClean:) to:nil from:nil]);
    });
    MCGuiWait(@"a.txt discarded", 15, ^BOOL {
        return actions == 1 && Rows(c, @[@"# Changes  1", @"W U c.txt"]);
    });
    MCGuiStep(@"it asked about a.txt", ^{
        GCHECK([newAlerts() isEqualToArray:@[@"Discard the changes to a.txt?"]]);
        GCHECK_EQ_STR(Read(a), @"one\ntwo\n");
    });
    MCGuiWait(@"the clean tab follows the disk", 10, ^BOOL {
        return [[[c valueForKey:@"textView"] string] isEqualToString:@"one\ntwo\n"];
    });
    MCGuiStep(@"a file row's right click", ^{
        NSArray *titles = MenuTitles([Git(c) menuForRow:RowOf(c, @"W U c.txt")]);
        MCGuiNote(@"row menu: %@", titles);
        NSArray *want = @[@"Open File", @"Open Changes", @"Open as git Diff", @"Stage Changes",
                          @"Discard (Move to Trash)…", @"Reveal in File Tree"];
        GCHECK([titles isEqualToArray:want]);
        NSEvent *none = nil;   // the selected row, as the keyboard's Menu key asks
        NSMenu *full = [Git(c).list menuForEvent:none];
        GCHECK([MenuTitles(full).lastObject hasPrefix:@"Adjust Settings"]);
        GCHECK([Git(c) menuForRow:0] == nil);   // the heading
        // The panel's own menu has the whole-list commands.
        NSArray *panelTitles = MenuTitles([Git(c) menuForEvent:none]);

        for (NSString *t in @[@"Stage All Changes", @"Unstage All Changes", @"Switch Branch…"])
            GCHECK([panelTitles containsObject:t]);
    });

    MCGuiStep(@"Switch Branch…: the picker lists main first, then feature", ^{
        GCHECK([NSApp sendAction:@selector(gitCheckout:) to:nil from:nil]);
    });
    MCGuiWait(@"the picker", 10, ^BOOL {
        return [MCCommandPaletteLabels(Palette(c)) isEqualToArray:@[@"main", @"feature"]];
    });
    MCGuiStep(@"\"feat\" and Return", ^{
        GCHECK([MCCommandPaletteKeys(Palette(c)).firstObject hasPrefix:@"current"]);
        PaletteType(c, @"feat");
        GCHECK([MCCommandPaletteLabels(Palette(c)).firstObject isEqualToString:@"feature"]);
        actions = 0;
        [PaletteEditor(c) doCommandBySelector:@selector(insertNewline:)];
    });
    MCGuiWait(@"on feature", 15, ^BOOL {
        return actions == 1 && [Git(c).branchText hasPrefix:@"feature"];
    });
    MCGuiStep(@"its file is there; a new branch from a typed name", ^{
        GCHECK(Palette(c) == nil);
        GCHECK_EQ_STR(Git(c).errorLine, @"Switched to feature.");
        GCHECK([[NSFileManager defaultManager]
            fileExistsAtPath:[repo stringByAppendingPathComponent:@"f.txt"]]);
        [c gitCheckout:nil];
    });
    MCGuiWait(@"the picker again", 10, ^BOOL {
        return MCCommandPaletteLabels(Palette(c)).count == 2;
    });
    MCGuiStep(@"\"new topic\" offers to make new-topic", ^{
        PaletteType(c, @"new topic");
        GCHECK([MCCommandPaletteLabels(Palette(c))
            isEqualToArray:@[@"Create branch “new-topic” from feature"]]);
        actions = 0;
        [PaletteEditor(c) doCommandBySelector:@selector(insertNewline:)];
    });
    MCGuiWait(@"on new-topic", 15, ^BOOL {
        return actions == 1 && [Git(c).branchText hasPrefix:@"new-topic"];
    });
    MCGuiStep(@"a name git would refuse is refused before it runs", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Created new-topic and switched to it.");
        [c gitCheckout:nil];
    });
    MCGuiWait(@"the picker, three branches", 10, ^BOOL {
        return MCCommandPaletteLabels(Palette(c)).count == 3;
    });
    MCGuiStep(@"\"bad..name\"", ^{
        PaletteType(c, @"bad..name");
        actions = 0;
        [PaletteEditor(c) doCommandBySelector:@selector(insertNewline:)];
    });
    MCGuiWait(@"refused", 10, ^BOOL { return actions == 1; });
    MCGuiStep(@"said why", ^{
        MCGuiNote(@"bad name: %@", Git(c).errorLine);
        GCHECK([Git(c).errorLine containsString:@"cannot hold"]);
        GCHECK([Git(c).branchText hasPrefix:@"new-topic"]);
    });
    GitShell(@"a commit on new-topic, and a change to the same file left uncommitted",
             [NSString stringWithFormat:@"cd '%@' && echo X > a.txt && %@ commit -qam x && "
                                         "echo Y > a.txt", repo, G]);
    MCGuiStep(@"switching to main would overwrite it", ^{ [c gitCheckout:nil]; });
    MCGuiWait(@"the picker", 10, ^BOOL { return MCCommandPaletteLabels(Palette(c)).count == 3; });
    MCGuiStep(@"main and Return", ^{
        PaletteType(c, @"main");
        GCHECK([MCCommandPaletteLabels(Palette(c)).firstObject isEqualToString:@"main"]);
        actions = 0;
        [PaletteEditor(c) doCommandBySelector:@selector(insertNewline:)];
    });
    MCGuiWait(@"git refused", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"git's own words; still on new-topic, the change kept", ^{
        MCGuiNote(@"refused switch: %@", Git(c).errorLine);
        GCHECK([Git(c).errorLine containsString:@"would be overwritten"]);
        GCHECK_EQ_STR(Read(a), @"Y\n");
    });
    MCGuiWait(@"still on new-topic", 10, ^BOOL {
        return [Git(c).branchText hasPrefix:@"new-topic"];
    });

    // A submodule with a change inside it: Discard used to trash its whole
    // checkout and restore the gitlink, leaving an empty folder.
    NSString *sub = [MCGuiScratch() stringByAppendingPathComponent:@"gitactions-sub"];
    NSString *libX = [repo stringByAppendingPathComponent:@"lib/x.txt"];
    GitShell(@"a submodule, lib, and a change inside it", [NSString stringWithFormat:
        @"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; echo inner > x.txt; %@ add -A; "
         "%@ commit -qm sub; cd '%@'; %@ -c protocol.file.allow=always submodule add -q '%@' lib; "
         "%@ commit -qm 'add lib'; echo changed > lib/x.txt",
        sub, sub, G, G, G, repo, G, sub, G]);
    MCGuiStep(@"look again", ^{ [Git(c) refresh]; });
    MCGuiWait(@"lib listed as changed", 15, ^BOOL { return RowOf(c, @"W M lib") != NSNotFound; });
    MCGuiStep(@"Discard on lib is refused, with what to do instead", ^{
        const NSInteger row = RowOf(c, @"W M lib");
        NSMenu *menu = [Git(c) menuForRow:row];
        NSMenuItem *discard = nil;
        for (NSMenuItem *it in menu.itemArray)
            if ([it.title hasPrefix:@"Discard"]) discard = it;
        GCHECK(discard != nil && ![(id<NSMenuItemValidation>)Git(c) validateMenuItem:discard]);
        MCGuiNote(@"its tooltip: %@", discard.toolTip);
        GCHECK([discard.toolTip containsString:@"submodule"]);
        actions = 0;
        alerts = MCGuiAlertsShown().count;
        [Git(c) discardAtRow:row];
    });
    MCGuiWait(@"refused", 10, ^BOOL { return actions == 1; });
    MCGuiStep(@"nothing asked, the checkout untouched", ^{
        MCGuiNote(@"refusal: %@", Git(c).errorLine);
        GCHECK([Git(c).errorLine hasPrefix:@"lib is a submodule"]);
        GCHECK(newAlerts().count == 0);
        GCHECK_EQ_STR(Read(libX), @"changed\n");
        GCHECK([[NSFileManager defaultManager]
            fileExistsAtPath:[repo stringByAppendingPathComponent:@"lib/.git"]]);
        GCHECK(![TrashedTexts() containsObject:@"changed\n"]);
        // Its diff is one commit for another: no hunk links.
        [Git(c) showDiffAtRow:RowOf(c, @"W M lib")];
    });
    MCGuiWait(@"its diff", 10, ^BOOL { return [[c valueForKey:@"isDiff"] boolValue]; });
    MCGuiStep(@"no hunk links on a gitlink's diff; staging it still works", ^{
        NSTextView *diff = [c valueForKey:@"diffView"];
        MCGuiNote(@"lib's diff: %@", diff.string);
        GCHECK([diff.string containsString:@"Subproject commit"]);
        [Git(c) runHunkOp:2 fromLine:5 toLine:5 lines:NO];   // 2: Discard (GitUi::HunkOp)
        GCHECK([Git(c).errorLine hasPrefix:@"lib is a submodule"]);
        GCHECK_EQ_STR(Read(libX), @"changed\n");
        [Git(c) toggleStageAtRow:RowOf(c, @"W M lib")];
    });
    MCGuiWait(@"add ran (a change inside a submodule stays one: nothing to stage)", 15, ^BOOL {
        return Git(c).errorLine.length == 0 && RowOf(c, @"W M lib") != NSNotFound;
    });
    MCGuiStep(@"the window closes; the real Trash back", ^{
        UseScratchTrash(NO);
        [c.window close];
    });
    MCGuiWait(@"gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}

// `make guitest SUITES=git_hunks`: the hunks of a diff on show (W26). A
// file changed in two places shows Stage Hunk and Discard Hunk on each
// hunk's header line; Stage Hunk on the second stages only it (git diff
// --cached holds it alone) and the diff shows what is left; the staged
// diff's menu unstages one selected line of the two it added; Discard Hunk
// asks, puts the hunk back as git has it with a copy of the file in the
// Trash, and Undo Discard brings it back; a diff gone stale since it was
// shown is refused with a reason, and the diff is shown again.
namespace {
struct HunkLink { NSString *link; NSRange range; int op; NSUInteger line; };
std::vector<HunkLink> HunkLinks(NSTextView *tv) {
    __block std::vector<HunkLink> out;
    NSTextStorage *ts = tv.textStorage;
    [ts enumerateAttribute:NSLinkAttributeName inRange:NSMakeRange(0, ts.length) options:0
                usingBlock:^(id v, NSRange r, BOOL *stop) {
        (void)stop;
        if (![v isKindOfClass:NSString.class] || ![v hasPrefix:@"mutible-hunk:"]) return;
        NSArray<NSString *> *p = [v componentsSeparatedByString:@":"];
        out.push_back({v, r, p[1].intValue, (NSUInteger)p[2].integerValue});
    }];
    return out;
}
NSString *GitText(NSString *repo, NSArray<NSString *> *args) {
    MCGitResult *r = MCGitRunSync(repo, args);
    return [[NSString alloc] initWithData:r.output encoding:NSUTF8StringEncoding] ?: @"";
}
}  // namespace

MC_GUI_SUITE(git_hunks) {
    __block EditorController *c = nil;
    NSString *git = MCGitExecutable();
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"githunks"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    NSString *f = [repo stringByAppendingPathComponent:@"f.txt"];
    __block NSInteger actions = 0;
    __block NSUInteger alerts = 0;
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the hunk checks are skipped"); });
        return;
    }
    NSString *both = @"a\nB\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk2\nk3\nl\nm\n";
    NSTextView *(^diff)(void) = ^NSTextView * { return [c valueForKey:@"diffView"]; };
    NSString *(^index)(void) = ^NSString * { return GitText(repo, @[@"show", @":f.txt"]); };

    GitShell(@"a repository; f.txt changed near its top and near its end", [NSString
        stringWithFormat:@"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; "
                          "printf 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl\\nm\\n' > f.txt; "
                          "%@ add -A; %@ commit -qm first; "
                          "printf 'a\\nB\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nk2\\nk3\\nl\\nm\\n' > f.txt",
                          repo, repo, G, G, G]);
    MCGuiStep(@"a window on it; the panel; a Trash of our own", ^{
        UseScratchTrash(YES);
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c toggleSourceControl:nil];
        Git(c).onActionDone = ^{ actions++; };
    });
    MCGuiWait(@"f.txt changed", 15, ^BOOL { return Rows(c, @[@"# Changes  1", @"W M f.txt"]); });
    MCGuiStep(@"its diff", ^{ [Git(c) showDiffAtRow:RowOf(c, @"W M f.txt")]; });
    MCGuiWait(@"two hunks, each with Stage Hunk and Discard Hunk", 10, ^BOOL {
        return [[c valueForKey:@"isDiff"] boolValue] && HunkLinks(diff()).size() == 4;
    });
    MCGuiStep(@"the links read as they say; Stage Hunk on the second", ^{
        NSTextView *tv = diff();
        std::vector<HunkLink> links = HunkLinks(tv);
        GCHECK(tv.delegate == Git(c).diffDelegate);
        GCHECK_EQ_STR([tv.string substringWithRange:links[0].range], @"Stage Hunk");
        GCHECK_EQ_STR([tv.string substringWithRange:links[3].range], @"Discard Hunk");
        GCHECK(links[0].op == 0 && links[1].op == 2 && links[2].line > links[0].line);
        // Still one line of text per line of the diff.
        GCHECK([tv.string componentsSeparatedByString:@"\n"].count ==
               [GitText(repo, @[@"diff", @"--", @"f.txt"]) componentsSeparatedByString:@"\n"].count);
        actions = 0;
        GCHECK([(id<NSTextViewDelegate>)tv.delegate textView:tv clickedOnLink:links[2].link
                                                     atIndex:links[2].range.location]);
    });
    MCGuiWait(@"staged, and the diff shows the hunk left", 15, ^BOOL {
        return actions == 1 && HunkLinks(diff()).size() == 2 &&
               Rows(c, @[@"# Staged changes  1", @"S M f.txt", @"# Changes  1", @"W M f.txt"]);
    });
    MCGuiStep(@"only that hunk is in the index", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Staged a hunk of f.txt.");
        NSString *cached = GitText(repo, @[@"diff", @"--cached"]);
        GCHECK([cached containsString:@"+k2\n+k3\n"] && ![cached containsString:@"+B"]);
        GCHECK_EQ_STR(index(), @"a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk2\nk3\nl\nm\n");
        GCHECK_EQ_STR(Read(f), both);
        GCHECK(![diff().string containsString:@"+k2"] && [diff().string containsString:@"+B"]);
        [Git(c) showDiffAtRow:RowOf(c, @"S M f.txt")];
    });
    MCGuiWait(@"the staged diff, with Unstage Hunk", 10, ^BOOL {
        std::vector<HunkLink> l = HunkLinks(diff());
        return l.size() == 1 && l[0].op == 1;
    });
    MCGuiStep(@"select +k2; the menu unstages the selected lines", ^{
        NSTextView *tv = diff();
        const NSRange k2 = [tv.string rangeOfString:@"+k2\n"];
        GCHECK(k2.location != NSNotFound);
        tv.selectedRange = k2;
        NSEvent *right = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                            location:NSZeroPoint modifierFlags:0 timestamp:0
                                        windowNumber:c.window.windowNumber context:nil
                                         eventNumber:0 clickCount:1 pressure:1];
        NSMenu *menu = [(id<NSTextViewDelegate>)tv.delegate
            textView:tv menu:[[NSMenu alloc] initWithTitle:@""] forEvent:right
             atIndex:k2.location + 1];
        NSArray *titles = MenuTitles(menu);
        MCGuiNote(@"diff menu: %@", titles);
        NSArray *want = @[@"Unstage Hunk", @"Unstage Selected Lines"];
        GCHECK([titles isEqualToArray:want]);
        actions = 0;
        [menu performActionForItemAtIndex:1];
    });
    MCGuiWait(@"k2 unstaged, k3 still staged", 15, ^BOOL {
        return actions == 1 && [index() isEqualToString:@"a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk3\nl\nm\n"];
    });
    MCGuiStep(@"it says so; the work tree untouched; the changes' diff", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Unstaged the selected lines of f.txt.");
        GCHECK_EQ_STR(Read(f), both);
        [Git(c) showDiffAtRow:RowOf(c, @"W M f.txt")];
    });
    MCGuiWait(@"two hunks again (B, and k2)", 10, ^BOOL {
        return HunkLinks(diff()).size() == 4 && [diff().string containsString:@"+k2"];
    });
    MCGuiStep(@"Discard Hunk on the first asks; yes", ^{
        NSTextView *tv = diff();
        std::vector<HunkLink> links = HunkLinks(tv);
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        actions = 0;
        [(id<NSTextViewDelegate>)tv.delegate textView:tv clickedOnLink:links[1].link
                                              atIndex:links[1].range.location];
        NSArray *shown = MCGuiAlertsShown();
        GCHECK([[shown subarrayWithRange:NSMakeRange(alerts, shown.count - alerts)]
            isEqualToArray:@[@"Discard this hunk of f.txt?"]]);
    });
    MCGuiWait(@"discarded", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"b is back, k2 kept; the file as it was is in the Trash", ^{
        MCGuiNote(@"after discard: %@", Git(c).errorLine);
        GCHECK_EQ_STR(Read(f), @"a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nk2\nk3\nl\nm\n");
        GCHECK([TrashedTexts() containsObject:both]);
        GCHECK_EQ_STR(Git(c).undoDiscardTitle, @"Undo Discard of f.txt");
        actions = 0;
        [Git(c) undoDiscard];
    });
    MCGuiWait(@"undone", 15, ^BOOL { return actions == 1 && [Read(f) isEqualToString:both]; });
    MCGuiStep(@"the diff on show is the one from before the undo", ^{
        GCHECK(HunkLinks(diff()).size() == 2 && ![diff().string containsString:@"+B"]);
    });
    GitShell(@"f.txt staged from outside, so the diff on show is stale",
             [NSString stringWithFormat:@"cd '%@' && %@ add f.txt", repo, G]);
    MCGuiStep(@"Stage Hunk on the stale diff", ^{
        NSTextView *tv = diff();
        std::vector<HunkLink> links = HunkLinks(tv);
        actions = 0;
        [(id<NSTextViewDelegate>)tv.delegate textView:tv clickedOnLink:links[0].link
                                              atIndex:links[0].range.location];
    });
    MCGuiWait(@"refused", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"said why; nothing changed", ^{
        MCGuiNote(@"stale: %@", Git(c).errorLine);
        GCHECK([Git(c).errorLine hasPrefix:@"f.txt changed since this diff was shown"]);
        GCHECK_EQ_STR(index(), both);
        GCHECK_EQ_STR(Read(f), both);
    });
    MCGuiWait(@"and the diff shown again: nothing left to stage", 10, ^BOOL {
        return HunkLinks(diff()).empty() &&
               [diff().string containsString:@"No differences to show."];
    });
    MCGuiStep(@"the window closes; the real Trash back", ^{
        UseScratchTrash(NO);
        [c.window close];
    });
    MCGuiWait(@"gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}

// `make guitest SUITES=git_verbs`: several rows at once, and the routine
// verbs after them. Two rows chosen stage and unstage together, from Space
// and from the row menu, which names the count; three chosen discard after
// one question; Commit and Amend rewrites the last commit's message; Undo
// Last Commit leaves its changes staged and its message in the box; Stash
// takes every change (the untracked file too) and Pop Stash brings them
// back; a second Pop is refused.
MC_GUI_SUITE(git_verbs) {
    __block EditorController *c = nil;
    NSString *git = MCGitExecutable();
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"gitverbs"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    __block NSInteger actions = 0;
    __block NSUInteger alerts = 0;
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the verb checks are skipped"); });
        return;
    }
    NSArray *changes3 = @[@"# Changes  3", @"W M a.txt", @"W M b.txt", @"W U n.txt"];
    NSArray *(^newAlerts)(void) = ^NSArray * {
        NSArray *all = MCGuiAlertsShown();
        return [all subarrayWithRange:NSMakeRange(alerts, all.count - alerts)];
    };
    void (^choose)(NSArray<NSString *> *) = ^(NSArray<NSString *> *rows) {
        NSMutableIndexSet *set = [NSMutableIndexSet indexSet];
        for (NSString *r in rows) {
            const NSInteger at = RowOf(c, r);
            GCHECK(at != NSNotFound);
            if (at != NSNotFound) [set addIndex:(NSUInteger)at];
        }
        [Git(c).list selectRowIndexes:set byExtendingSelection:NO];
    };

    GitShell(@"a repository: two files changed, one untracked", [NSString stringWithFormat:
        @"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; echo one > a.txt; echo one > b.txt; "
         "%@ add -A; %@ commit -qm first; echo two > a.txt; echo two > b.txt; echo n > n.txt",
        repo, repo, G, G, G]);
    MCGuiStep(@"a window on it; the panel; a Trash of our own", ^{
        UseScratchTrash(YES);
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c toggleSourceControl:nil];
        Git(c).onActionDone = ^{ actions++; };
    });
    MCGuiWait(@"three changes", 15, ^BOOL { return Rows(c, changes3); });
    MCGuiStep(@"a.txt and b.txt chosen; Space stages both", ^{
        GCHECK(Git(c).list.allowsMultipleSelection);
        choose(@[@"W M a.txt", @"W M b.txt"]);
        GCHECK(Git(c).list.selectedRowIndexes.count == 2);
        [Git(c) toggleStageAtRow:RowOf(c, @"W M a.txt")];
    });
    MCGuiWait(@"both staged", 15, ^BOOL {
        return Rows(c, @[@"# Staged changes  2", @"S M a.txt", @"S M b.txt", @"# Changes  1",
                         @"W U n.txt"]);
    });
    MCGuiStep(@"both still chosen; the row menu names them, and unstages both", ^{
        GCHECK(Git(c).list.selectedRowIndexes.count == 2);
        NSMenu *m = [Git(c) menuForRow:RowOf(c, @"S M b.txt")];
        NSArray *titles = MenuTitles(m);
        MCGuiNote(@"menu for two: %@", titles);
        GCHECK([titles containsObject:@"Unstage Changes (2 Files)"]);
        GCHECK([titles containsObject:@"Discard Changes (2 Files)…"]);
        NSMenuItem *discard = [m itemWithTitle:@"Discard Changes (2 Files)…"];
        GCHECK(![(id<NSMenuItemValidation>)Git(c) validateMenuItem:discard]);   // staged: unstage first
        NSMenuItem *unstage = [m itemWithTitle:@"Unstage Changes (2 Files)"];
        [NSApp sendAction:unstage.action to:unstage.target from:unstage];
    });
    MCGuiWait(@"both back in Changes", 15, ^BOOL { return Rows(c, changes3); });
    MCGuiStep(@"all three chosen: Discard asks once, and Cancel does nothing", ^{
        choose(@[@"W M a.txt", @"W M b.txt", @"W U n.txt"]);
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        [Git(c) discardAtRow:RowOf(c, @"W U n.txt")];
        GCHECK([newAlerts() isEqualToArray:@[@"Discard the changes to 3 files?"]]);
        GCHECK([[NSFileManager defaultManager]
            fileExistsAtPath:[repo stringByAppendingPathComponent:@"n.txt"]]);
    });
    MCGuiStep(@"yes", ^{
        choose(@[@"W M a.txt", @"W M b.txt", @"W U n.txt"]);
        actions = 0;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [Git(c) discardAtRow:RowOf(c, @"W M a.txt")];
    });
    MCGuiWait(@"no changes left", 15, ^BOOL {
        return actions == 1 && Git(c).inRepository && [Git(c) rowDescriptions].count == 0;
    });
    MCGuiStep(@"each is its own Undo Discard: the last, n.txt, first", ^{
        GCHECK_EQ_STR(Read([repo stringByAppendingPathComponent:@"a.txt"]), @"one\n");
        GCHECK([Git(c).errorLine containsString:@"3 files"]);
        GCHECK_EQ_STR(Git(c).undoDiscardTitle, @"Undo Discard of n.txt");
        actions = 0;
        [Git(c) undoDiscard];
    });
    MCGuiWait(@"n.txt back", 15, ^BOOL { return actions == 1 && RowOf(c, @"W U n.txt") != NSNotFound; });

    MCGuiStep(@"the panel's menu has the verbs", ^{
        NSEvent *none = nil;
        NSArray *titles = MenuTitles([Git(c) menuForEvent:none]);
        for (NSString *t in @[@"Commit and Amend", @"Undo Last Commit", @"Stash", @"Pop Stash"])
            GCHECK([titles containsObject:t]);
        // A second commit to work on.
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"a.txt"], @"three\n");
    });
    GitShell(@"a.txt changed and committed as \"second\"",
             [NSString stringWithFormat:@"cd '%@' && %@ commit -qam second", repo, G]);
    MCGuiStep(@"Commit and Amend from the menu bar, with a new message", ^{
        [Git(c) refresh];
        Git(c).messageView.string = @"second, amended";
        actions = 0;
        GCHECK([NSApp sendAction:@selector(gitCommitAmend:) to:nil from:nil]);
    });
    MCGuiWait(@"amended", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"the last commit has the new message; still two commits", ^{
        MCGuiNote(@"after amend: %@", Git(c).errorLine);
        GCHECK_EQ_STR(GitText(repo, @[@"log", @"-1", @"--format=%s"]), @"second, amended\n");
        GCHECK_EQ_STR(GitText(repo, @[@"rev-list", @"--count", @"HEAD"]), @"2\n");
        GCHECK(Git(c).messageView.string.length == 0);
        actions = 0;
        [Git(c) undoLastCommit];
    });
    MCGuiWait(@"undone", 15, ^BOOL { return actions == 1 && RowOf(c, @"S M a.txt") != NSNotFound; });
    MCGuiStep(@"one commit; its change staged; its message back in the box", ^{
        MCGuiNote(@"after undo: %@", Git(c).errorLine);
        GCHECK_EQ_STR(GitText(repo, @[@"rev-list", @"--count", @"HEAD"]), @"1\n");
        GCHECK_EQ_STR(Git(c).messageView.string, @"second, amended");
        GCHECK_EQ_STR(Read([repo stringByAppendingPathComponent:@"a.txt"]), @"three\n");
        Git(c).messageView.string = @"half done";
        actions = 0;
        GCHECK([NSApp sendAction:@selector(gitStash:) to:nil from:nil]);
    });
    MCGuiWait(@"stashed", 15, ^BOOL {
        return actions == 1 && Git(c).inRepository && [Git(c) rowDescriptions].count == 0;
    });
    MCGuiStep(@"every change in the stash, the untracked file too", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Stashed the changes as “half done”.");
        GCHECK([GitText(repo, @[@"stash", @"list"]) containsString:@"half done"]);
        GCHECK(![[NSFileManager defaultManager]
            fileExistsAtPath:[repo stringByAppendingPathComponent:@"n.txt"]]);
        GCHECK_EQ_STR(Read([repo stringByAppendingPathComponent:@"a.txt"]), @"one\n");
        actions = 0;
        GCHECK([NSApp sendAction:@selector(gitStashPop:) to:nil from:nil]);
    });
    MCGuiWait(@"popped", 15, ^BOOL {
        return actions == 1 && RowOf(c, @"W U n.txt") != NSNotFound &&
               RowOf(c, @"W M a.txt") != NSNotFound;
    });
    MCGuiStep(@"back; a second pop is refused", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"Popped the stash.");
        GCHECK_EQ_STR(Read([repo stringByAppendingPathComponent:@"a.txt"]), @"three\n");
        actions = 0;
        [Git(c) popStash];
    });
    MCGuiWait(@"refused", 10, ^BOOL { return actions == 1; });
    MCGuiStep(@"said why; the window closes", ^{
        GCHECK_EQ_STR(Git(c).errorLine, @"There is no stash to pop.");
        UseScratchTrash(NO);
        [c.window close];
    });
    MCGuiWait(@"gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}

// `make guitest SUITES=git_review`: the review of every uncommitted change
// (ChangeReviewPage.h). A click on a file row opens it at that file, with
// no git headers; three edits a line apart are three blocks, each with its
// own buttons; Stage on the middle one leaves only it in the index; Discard
// asks, writes the file (a clean tab on it follows the disk as one undo
// step) and Undo Discard puts it back; Stage Selected Lines from the right
// click; Unstage in the staged list; an index changed from outside since
// the view was drawn refuses with words; the place is kept after an action;
// F7 steps through the blocks; a folded stretch opens on a click; syntax
// colors show; and 50,000 changed lines go on show within a CPU bound.
namespace {
MCChangeReviewPage *Review(EditorController *c) {
    id page = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
    return [page isKindOfClass:MCChangeReviewPage.class] ? page : nil;
}
NSInteger RowNamed(MCChangeReviewPage *p, NSString *description) {
    const NSUInteger at = [p.rowDescriptions indexOfObject:description];
    return at == NSNotFound ? -1 : (NSInteger)at;
}
// Whether a row is on screen.
BOOL RowShows(MCChangeReviewPage *p, NSInteger row) {
    return row >= 0 && NSIntersectsRect([p rectOfRow:row], p.scrollView.contentView.bounds);
}
// The first block's row after a file's header row.
NSInteger FirstBlockAfter(MCChangeReviewPage *p, NSInteger header) {
    for (NSUInteger b = 0; b < p.blockCount; b++)
        if ([p rowOfBlock:b] > header) return [p rowOfBlock:b];
    return -1;
}
// A file's header row in one list as the page shows it ("F f.txt  +2 -2"),
// or nil: what the page's own data says, which a load count alone does not
// (a read already on the queue when an action wrote lands first and counts).
NSString *FileHeader(MCChangeReviewPage *p, NSString *rel, BOOL staged) {
    const NSInteger at = [p rowOfFile:rel staged:staged];
    NSArray<NSString *> *rows = p.rowDescriptions;
    return at >= 0 && (NSUInteger)at < rows.count ? rows[(NSUInteger)at] : nil;
}
BOOL PressButton(MCChangeReviewPage *p, NSString *title, NSInteger row) {
    const NSRect r = [p buttonRect:title row:row];
    if (NSIsEmptyRect(r)) return NO;
    return [p pressAtPoint:NSMakePoint(NSMidX(r), NSMidY(r))];
}
}  // namespace

MC_GUI_SUITE(git_review) {
    __block EditorController *c = nil;
    NSString *git = MCGitExecutable();
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"gitreview"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    NSString *f = [repo stringByAppendingPathComponent:@"f.txt"];
    __block NSInteger actions = 0;
    __block NSUInteger loads = 0, alerts = 0;
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the review checks are skipped"); });
        return;
    }
    NSString *edited = @"a\nB\nc\nD\ne\nF\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\n";
    NSString *(^index)(void) = ^NSString * { return GitText(repo, @[@"show", @":f.txt"]); };

    GitShell(@"a repository; f.txt changed on three lines a line apart, g.py on one", [NSString
        stringWithFormat:@"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; "
                          "printf 'a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl\\nm\\nn\\no\\np\\nq\\nr\\ns\\nt\\n' > f.txt; "
                          "printf 'def f():\\n    return 1\\n' > g.py; "
                          "%@ add -A; %@ commit -qm first; "
                          "printf 'a\\nB\\nc\\nD\\ne\\nF\\ng\\nh\\ni\\nj\\nk\\nl\\nm\\nn\\no\\np\\nq\\nr\\ns\\nt\\n' > f.txt; "
                          "printf 'def f():\\n    return 2\\n' > g.py",
                          repo, repo, G, G, G]);
    MCGuiStep(@"a window on it; the panel; a Trash of our own", ^{
        UseScratchTrash(YES);
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c toggleSourceControl:nil];
    });
    MCGuiWait(@"both files listed", 15, ^BOOL {
        return Rows(c, @[@"# Changes  2", @"W M f.txt", @"W M g.py"]);
    });
    MCGuiStep(@"Review Changes shows; a click on f.txt's row opens the review", ^{
        GCHECK([[Git(c) valueForKey:@"reviewButtonShown"] boolValue]);
        [Git(c) reviewRow:RowOf(c, @"W M f.txt")];
    });
    MCGuiWait(@"the review, read", 15, ^BOOL { return Review(c).loaded && Review(c).blockCount == 4; });
    MCGuiStep(@"every file in one scroll, three blocks in f.txt, no git headers", ^{
        MCChangeReviewPage *p = Review(c);
        p.onActionDone = ^{ actions++; };
        NSArray<NSString *> *rows = p.rowDescriptions;
        MCGuiNote(@"review rows: %@", rows);
        GCHECK_EQ_STR(rows.firstObject, @"§ Changes  2");
        GCHECK([rows containsObject:@"F f.txt  +3 -3"]);
        GCHECK([rows containsObject:@"F g.py  +1 -1"]);
        GCHECK([rows containsObject:@"~ 11 unchanged lines"]);
        NSArray *want = @[@"F f.txt  +3 -3", @"  a", @"- b", @"+ B", @"  c", @"- d", @"+ D",
                          @"  e", @"- f", @"+ F", @"  g", @"  h", @"  i", @"~ 11 unchanged lines"];
        const NSInteger at = RowNamed(p, @"F f.txt  +3 -3");
        GCHECK(at >= 0 && [[rows subarrayWithRange:NSMakeRange((NSUInteger)at, want.count)]
                              isEqualToArray:want]);
        for (NSString *r in rows)
            GCHECK(![r containsString:@"diff --git"] && ![r containsString:@"@@"] &&
                   ![r containsString:@"index "]);
        // Each block has its own Stage and Discard; the header its own.
        for (NSUInteger b = 0; b < 3; b++) {
            GCHECK(!NSIsEmptyRect([p buttonRect:@"Stage" row:[p rowOfBlock:b]]));
            GCHECK(!NSIsEmptyRect([p buttonRect:@"Discard" row:[p rowOfBlock:b]]));
        }
        for (NSString *t in @[@"Side by Side", @"Open", @"Discard File", @"Stage File"])
            GCHECK(!NSIsEmptyRect([p buttonRect:t row:at]));
        // Opened at f.txt: its header on screen.
        GCHECK(RowShows(p, at));
        // g.py's "def" in the keyword color.
        NSTextStorage *ts = p.textView.textStorage;
        const NSRange def = [ts.string rangeOfString:@"def f():"];
        GCHECK(def.location != NSNotFound);
        if (def.location != NSNotFound) {
            NSColor *color = [ts attribute:NSForegroundColorAttributeName atIndex:def.location
                            effectiveRange:nil];
            GCHECK([color isEqual:[[AppSettings shared] syntax:TokenStyle::Keyword]]);
        }
        // The changed word of g.py's line marked more strongly than the line.
        const NSRange two = [ts.string rangeOfString:@"    return 2"];
        GCHECK(two.location != NSNotFound);
        if (two.location != NSNotFound)
            GCHECK([ts attribute:NSBackgroundColorAttributeName atIndex:NSMaxRange(two) - 1
                  effectiveRange:nil] != nil);
        MCGuiSnapshot(p.pageView, @"git-review");
        loads = p.loads;
        actions = 0;
        GCHECK(PressButton(p, @"Stage", [p rowOfBlock:1]));
    });
    MCGuiWait(@"the middle block staged, and read again", 15, ^BOOL {
        return actions == 1 && Review(c).loads > loads &&
               [Review(c).rowDescriptions containsObject:@"§ Staged Changes  1"];
    });
    MCGuiStep(@"only it is in the index; the file untouched; the eye on f.txt still", ^{
        MCChangeReviewPage *p = Review(c);
        GCHECK_EQ_STR(p.message, @"Staged a change to f.txt.");
        // f.txt is in both lists, and one file: two files in all.
        MCGuiNote(@"header after staging: %@", p.headerDetail);
        GCHECK_EQ_STR(p.headerDetail, @"2 files, +4 -4");
        GCHECK_EQ_STR(index(), @"a\nb\nc\nD\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\n");
        GCHECK_EQ_STR(Read(f), edited);
        const NSInteger at = [p rowOfFile:@"f.txt" staged:NO];
        MCGuiNote(@"after staging: top %ld, f.txt's header %ld", (long)p.topRow, (long)at);
        // Still in sight, though the staged list came in above it (on a
        // page taller than the pane it is the top row again).
        GCHECK(RowShows(p, at));
        GCHECK([p.rowDescriptions containsObject:@"F f.txt  +2 -2"]);
        // The staged block has Unstage, not Stage or Discard.
        const NSInteger staged = FirstBlockAfter(p, [p rowOfFile:@"f.txt" staged:YES]);
        GCHECK(staged >= 0 && !NSIsEmptyRect([p buttonRect:@"Unstage" row:staged]));
        GCHECK(NSIsEmptyRect([p buttonRect:@"Discard" row:staged]));
        // f.txt open in a clean tab, then the review again.
        GCHECK([c showFile:f]);
        [c gitReviewChanges:nil];
        GCHECK(Review(c) == p);
    });
    MCGuiStep(@"Discard on f.txt's first block asks; yes", ^{
        MCChangeReviewPage *p = Review(c);
        const NSInteger block = FirstBlockAfter(p, [p rowOfFile:@"f.txt" staged:NO]);
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Discard", block));
        NSArray *shown = MCGuiAlertsShown();
        GCHECK([[shown subarrayWithRange:NSMakeRange(alerts, shown.count - alerts)]
            isEqualToArray:@[@"Discard this change to f.txt?"]]);
    });
    MCGuiWait(@"discarded", 15, ^BOOL { return actions == 1 && Review(c).loads > loads; });
    MCGuiStep(@"b is back, D and F kept; the file as it was is in the Trash", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"after discard: %@", p.message);
        GCHECK_EQ_STR(Read(f), @"a\nb\nc\nD\ne\nF\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\n");
        GCHECK([TrashedTexts() containsObject:edited]);
        GCHECK_EQ_STR(Git(c).undoDiscardTitle, @"Undo Discard of f.txt");
        GCHECK([c showFile:f]);
    });
    MCGuiWait(@"the clean tab follows the disk, as one undo step", 10, ^BOOL {
        return [[[c valueForKey:@"textView"] string] hasPrefix:@"a\nb\nc\nD\n"];
    });
    MCGuiStep(@"its undo step; Undo Discard", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        MCGuiNote(@"tab's undo: %@", tv.undoManager.undoActionName);
        GCHECK_EQ_STR(tv.undoManager.undoActionName, @"Reload from Disk");
        [c gitReviewChanges:nil];
        actions = 0;
        [Git(c) undoDiscard];
    });
    MCGuiWait(@"undone", 15, ^BOOL { return [Read(f) isEqualToString:edited]; });
    MCGuiWait(@"and the review shows B again", 15, ^BOOL {
        return [Review(c).rowDescriptions containsObject:@"+ B"];
    });
    MCGuiStep(@"select the + F line; the right click stages the selected lines", ^{
        MCChangeReviewPage *p = Review(c);
        NSTextView *tv = p.textView;
        const NSInteger row = RowNamed(p, @"+ F");
        GCHECK(row >= 0);
        // The row's characters: the text is one line per row.
        NSArray *lines = [tv.string componentsSeparatedByString:@"\n"];
        NSUInteger start = 0;
        for (NSInteger i = 0; i < row; i++) start += [lines[(NSUInteger)i] length] + 1;
        tv.selectedRange = NSMakeRange(start, 1);
        NSEvent *right = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                            location:NSZeroPoint modifierFlags:0 timestamp:0
                                        windowNumber:c.window.windowNumber context:nil
                                         eventNumber:0 clickCount:1 pressure:1];
        NSMenu *menu = [tv menuForEvent:right];
        NSArray *titles = MenuTitles(menu);
        MCGuiNote(@"review menu: %@", titles);
        GCHECK([titles containsObject:@"Stage Selected Lines"] &&
               [titles containsObject:@"Discard Selected Lines…"]);
        GCHECK([titles.lastObject hasPrefix:@"Adjust Settings"]);
        actions = 0;
        loads = p.loads;
        [menu performActionForItemAtIndex:[menu indexOfItemWithTitle:@"Stage Selected Lines"]];
    });
    // The page must hold a read made after the write before Unstage is
    // pressed: Unstage checks the index entry against the one the page
    // read, and a read already queued when Stage Selected Lines wrote lands
    // first, counts as a load, and still has D alone staged (CI twice).
    MCGuiWait(@"F staged as well, and the page read it", 15, ^BOOL {
        return actions == 1 && Review(c).loads > loads &&
               [index() isEqualToString:@"a\nb\nc\nD\ne\nF\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\n"] &&
               [FileHeader(Review(c), @"f.txt", YES) isEqualToString:@"F f.txt  +2 -2"] &&
               [FileHeader(Review(c), @"f.txt", NO) isEqualToString:@"F f.txt  +1 -1"];
    });
    MCGuiStep(@"Unstage on the staged list's first block", ^{
        MCChangeReviewPage *p = Review(c);
        GCHECK_EQ_STR(p.message, @"Staged the selected lines of f.txt.");
        const NSInteger block = FirstBlockAfter(p, [p rowOfFile:@"f.txt" staged:YES]);
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Unstage", block));
    });
    MCGuiWait(@"D unstaged, F still staged", 15, ^BOOL {
        return actions == 1 &&
               [index() isEqualToString:@"a\nb\nc\nd\ne\nF\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq\nr\ns\nt\n"];
    });
    // The same: the page's data, not only its count, so the view the next
    // step changes the index under is the one Unstage left.
    MCGuiWait(@"read again", 15, ^BOOL {
        return Review(c).loads > loads &&
               [FileHeader(Review(c), @"f.txt", YES) isEqualToString:@"F f.txt  +1 -1"] &&
               [FileHeader(Review(c), @"f.txt", NO) isEqualToString:@"F f.txt  +2 -2"];
    });
    MCGuiStep(@"the index changed from outside since the view was drawn: Stage refuses", ^{
        MCChangeReviewPage *p = Review(c);
        // In the same step, so the page has not read it yet.
        MCGitResult *r = MCGitRunSync(repo, @[@"add", @"f.txt"]);
        GCHECK(r.ok);
        const NSInteger header = [p rowOfFile:@"f.txt" staged:NO];
        GCHECK(header >= 0);
        actions = 0;
        GCHECK(PressButton(p, @"Stage", FirstBlockAfter(p, header)));
    });
    MCGuiWait(@"refused", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"said why; the index is what git add made", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"stale: %@", p.message);
        GCHECK([p.message hasPrefix:@"The staged version of f.txt changed since this view was drawn"]);
        GCHECK_EQ_STR(index(), edited);
    });
    MCGuiWait(@"read again: f.txt only staged now", 15, ^BOOL {
        return [Review(c) rowOfFile:@"f.txt" staged:NO] < 0 &&
               [Review(c) rowOfFile:@"f.txt" staged:YES] >= 0;
    });
    MCGuiStep(@"F7 steps through the blocks across files, wrapping", ^{
        MCChangeReviewPage *p = Review(c);
        NSTextView *tv = p.textView;
        const NSUInteger n = p.blockCount;
        GCHECK(n == 4);
        [tv scrollPoint:NSZeroPoint];
        NSMutableArray *seen = [NSMutableArray array];
        for (NSUInteger i = 0; i <= n; i++) {
            [NSApp sendAction:@selector(goToNextDifference:) to:tv from:nil];
            [seen addObject:@(tv.selectedRange.location)];
        }
        MCGuiNote(@"F7 went to %@", seen);
        GCHECK([seen.firstObject isEqual:seen.lastObject]);
        GCHECK([[NSSet setWithArray:seen] count] == n);
    });
    MCGuiStep(@"a click on a folded stretch opens it", ^{
        MCChangeReviewPage *p = Review(c);
        const NSInteger gap = RowNamed(p, @"~ 11 unchanged lines");
        GCHECK(gap >= 0);
        const NSUInteger before = p.rowDescriptions.count;
        const NSRect r = [p rectOfRow:gap];
        GCHECK([p pressAtPoint:NSMakePoint(NSMinX(r) + 120, NSMidY(r))]);
        GCHECK(p.rowDescriptions.count == before + 10);
        GCHECK(RowNamed(p, @"~ 11 unchanged lines") < 0 && RowNamed(p, @"  t") >= 0);
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Stage File", [p rowOfFile:@"g.py" staged:NO]));
    });
    MCGuiWait(@"g.py staged and read again", 15, ^BOOL {
        return actions == 1 && Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:YES] >= 0;
    });
    MCGuiStep(@"its note stays while nothing else moves; then a commit from the panel", ^{
        MCChangeReviewPage *p = Review(c);
        GCHECK_EQ_STR(p.message, @"Staged g.py.");
        loads = p.loads;
        Git(c).messageView.string = @"both staged";
        [Git(c) commit];
    });
    MCGuiWait(@"committed, and the review read again", 15, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:YES] < 0 &&
               [Review(c) rowOfFile:@"f.txt" staged:YES] < 0;
    });
    MCGuiStep(@"the note about staging went with the staged changes", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"after the commit: \"%@\", rows %@", p.message, p.rowDescriptions);
        GCHECK(p.message.length == 0);
        GCHECK_EQ_STR([[p valueForKey:@"messageLabel"] stringValue], @"");
    });
    // The other order: the commit lands after the action wrote and before
    // the page's first read after it, so that read already shows the
    // change committed and the note had nothing to compare it with (CI on
    // main, 6ea9d0f: "Staged g.py." stayed). The page's queue is held
    // between the action and its read, so the order is certain here.
    __block dispatch_semaphore_t hold = nil;
    GitShell(@"g.py changed again", [NSString stringWithFormat:@"cd '%@' && printf 'def f():\\n    return 3\\n' > g.py", repo]);
    MCGuiStep(@"read again for it", ^{
        loads = Review(c).loads;
        [Review(c) reload];
    });
    MCGuiWait(@"g.py among the changes", 15, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:NO] >= 0;
    });
    MCGuiStep(@"Stage File on g.py, the page's next read held back", ^{
        MCChangeReviewPage *p = Review(c);
        actions = 0;
        GCHECK(PressButton(p, @"Stage File", [p rowOfFile:@"g.py" staged:NO]));
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        hold = s;
        // Behind the action's write, so ahead of the read its end starts.
        dispatch_async((dispatch_queue_t)[p valueForKey:@"_queue"], ^{
            dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 30 * (int64_t)NSEC_PER_SEC));
        });
    });
    MCGuiWait(@"staged and said", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"a commit before the page reads; then the read", ^{
        GCHECK_EQ_STR(Review(c).message, @"Staged g.py.");
        GCHECK(MCGitRunSync(repo, @[@"commit", @"-qm", @"meanwhile"]).ok);
        loads = Review(c).loads;
        dispatch_semaphore_signal(hold);
    });
    MCGuiWait(@"read with g.py committed", 15, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:YES] < 0 &&
               [Review(c) rowOfFile:@"g.py" staged:NO] < 0;
    });
    MCGuiStep(@"the note went with the change it spoke of", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"committed before the first read: \"%@\"", p.message);
        GCHECK(p.message.length == 0);
        GCHECK_EQ_STR([[p valueForKey:@"messageLabel"] stringValue], @"");
    });
    // The order CI met next (main, 69ff43b): a read started after the
    // action was queued and before its note was said (a refresh after an
    // edit, say), run after the commit. It is the first read on show after
    // the note and already shows g.py committed; the note was kept for the
    // read after it, so the page showed no staged g.py under "Staged g.py.".
    GitShell(@"g.py changed once more", [NSString stringWithFormat:@"cd '%@' && printf 'def f():\\n    return 4\\n' > g.py", repo]);
    MCGuiStep(@"read again for it", ^{
        loads = Review(c).loads;
        [Review(c) reload];
    });
    MCGuiWait(@"g.py among the changes again", 15, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:NO] >= 0;
    });
    MCGuiStep(@"Stage File on g.py held, and a read asked for behind it", ^{
        MCChangeReviewPage *p = Review(c);
        actions = 0;
        GCHECK(PressButton(p, @"Stage File", [p rowOfFile:@"g.py" staged:NO]));
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        hold = s;
        dispatch_async((dispatch_queue_t)[p valueForKey:@"_queue"], ^{
            dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 30 * (int64_t)NSEC_PER_SEC));
        });
        [p reload];
        GCHECK([[p valueForKey:@"_loading"] boolValue]);
    });
    MCGuiWait(@"staged and said, the read still waiting", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"a commit; then the read that was asked for first", ^{
        GCHECK_EQ_STR(Review(c).message, @"Staged g.py.");
        GCHECK(MCGitRunSync(repo, @[@"commit", @"-qm", @"meanwhile again"]).ok);
        loads = Review(c).loads;
        dispatch_semaphore_signal(hold);
    });
    MCGuiWait(@"the first read after the note on show", 15, ^BOOL { return Review(c).loads > loads; });
    MCGuiStep(@"it shows g.py committed, and the note is gone with it", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"first read after the note: \"%@\", g.py rows %ld %ld", p.message,
                  (long)[p rowOfFile:@"g.py" staged:YES], (long)[p rowOfFile:@"g.py" staged:NO]);
        GCHECK([p rowOfFile:@"g.py" staged:YES] < 0 && [p rowOfFile:@"g.py" staged:NO] < 0);
        GCHECK(p.message.length == 0);
        GCHECK_EQ_STR([[p valueForKey:@"messageLabel"] stringValue], @"");
    });
    // The note speaks of g.py: another file moving leaves it; g.py
    // unstaged from a terminal takes it away.
    GitShell(@"g.py changed for the last time", [NSString stringWithFormat:@"cd '%@' && printf 'def f():\\n    return 5\\n' > g.py", repo]);
    MCGuiStep(@"read again for it", ^{
        loads = Review(c).loads;
        [Review(c) reload];
    });
    MCGuiWait(@"g.py among the changes once more", 15, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:NO] >= 0;
    });
    MCGuiStep(@"Stage File on g.py", ^{
        MCChangeReviewPage *p = Review(c);
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Stage File", [p rowOfFile:@"g.py" staged:NO]));
    });
    MCGuiWait(@"g.py staged and on show", 15, ^BOOL {
        return actions == 1 && Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:YES] >= 0;
    });
    GitShell(@"another file appears", [NSString stringWithFormat:@"cd '%@' && printf 'x\\n' > other.txt", repo]);
    MCGuiStep(@"read again for it", ^{
        GCHECK_EQ_STR(Review(c).message, @"Staged g.py.");
        loads = Review(c).loads;
        [Review(c) reload];
    });
    MCGuiWait(@"other.txt on show", 15, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"other.txt" staged:NO] >= 0;
    });
    MCGuiStep(@"the note about g.py stays; then g.py unstaged in a terminal", ^{
        MCChangeReviewPage *p = Review(c);
        GCHECK_EQ_STR(p.message, @"Staged g.py.");
        GCHECK(MCGitRunSync(repo, @[@"restore", @"--staged", @"g.py"]).ok);
        loads = p.loads;
        [p reload];
    });
    MCGuiWait(@"g.py back among the changes", 15, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"g.py" staged:YES] < 0;
    });
    MCGuiStep(@"the note went with the staged g.py", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"after the unstage: \"%@\"", p.message);
        GCHECK(p.message.length == 0);
    });
    GitShell(@"g.py and other.txt as committed", [NSString stringWithFormat:@"cd '%@' && %@ checkout -q -- g.py && rm other.txt", repo, G]);
    GitShell(@"50,000 lines, every one changed", [NSString
        stringWithFormat:@"set -e; cd '%@'; seq 1 50000 | sed 's/^/line /' > big.txt; %@ add big.txt; "
                          "%@ commit -qm big; seq 1 50000 | sed 's/^/LINE /' > big.txt",
                          repo, G, G]);
    MCGuiStep(@"read again", ^{
        loads = Review(c).loads;
        [Review(c) reload];
    });
    MCGuiWait(@"big.txt on show", 60, ^BOOL {
        return Review(c).loads > loads && [Review(c) rowOfFile:@"big.txt" staged:NO] >= 0;
    });
    MCGuiStep(@"within a CPU bound on the main thread", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"50,000 changed lines on show in %.0f ms of main-thread CPU",
                  p.lastShowCpuMilliseconds);
        GCHECK_TIME(p.lastShowCpuMilliseconds, 1500, @"the review with 50,000 changed lines shown");
        GCHECK(p.rowDescriptions.count > 100000);
    });
    // The place after Stage on a page taller than the pane: m1.txt's one
    // long block, z2.txt's two blocks, then zz3.txt's long block below.
    // Stage on z2.txt's first block puts its second block where the first
    // was on screen, though a staged list now stands above everything (the
    // walker's page jumped from summary.py up to models.py).
    __block CGFloat pressedY = 0;
    GitShell(@"big.txt as committed; m1.txt, z2.txt and zz3.txt changed", [NSString
        stringWithFormat:@"set -e; cd '%@'; %@ checkout -q -- big.txt; "
                          "seq 1 150 | sed 's/^/m /' > m1.txt; seq 1 80 | sed 's/^/z /' > z2.txt; "
                          "seq 1 100 | sed 's/^/zz /' > zz3.txt; %@ add m1.txt z2.txt zz3.txt; "
                          "%@ commit -qm three; seq 1 150 | sed 's/^/M /' > m1.txt; "
                          "seq 1 80 | sed 's/^/z /' | sed -e 's/^z 2$/z TWO/' -e 's/^z 30$/z THIRTY/' > z2.txt; "
                          "seq 1 100 | sed 's/^/ZZ /' > zz3.txt",
                          repo, G, G, G]);
    MCGuiStep(@"read again", ^{
        loads = Review(c).loads;
        [Review(c) reload];
    });
    MCGuiWait(@"three files, four blocks", 30, ^BOOL {
        MCChangeReviewPage *p = Review(c);
        return p.loads > loads && [p rowOfFile:@"z2.txt" staged:NO] >= 0 &&
               [p rowOfFile:@"big.txt" staged:NO] < 0 && p.blockCount == 4;
    });
    MCGuiStep(@"z2.txt's first block in the middle of the pane; Stage on it", ^{
        MCChangeReviewPage *p = Review(c);
        const NSInteger block = FirstBlockAfter(p, [p rowOfFile:@"z2.txt" staged:NO]);
        GCHECK(block >= 0);
        NSClipView *clip = p.scrollView.contentView;
        MCGuiNote(@"page %.0f tall, pane %.0f", NSHeight(p.textView.frame), NSHeight(clip.bounds));
        GCHECK(NSHeight(p.textView.frame) > 2 * NSHeight(clip.bounds));
        [clip scrollToPoint:NSMakePoint(0, NSMinY([p rectOfRow:block]) - floor(NSHeight(clip.bounds) / 2))];
        [p.scrollView reflectScrolledClipView:clip];
        pressedY = NSMinY([p rectOfRow:block]) - NSMinY(clip.bounds);
        GCHECK(RowShows(p, block));
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Stage", block));
    });
    MCGuiWait(@"staged, and read again", 15, ^BOOL {
        MCChangeReviewPage *p = Review(c);
        return actions == 1 && p.loads > loads && [p rowOfFile:@"z2.txt" staged:YES] >= 0;
    });
    MCGuiStep(@"z2.txt's other block where the staged one was, z2.txt still in sight", ^{
        MCChangeReviewPage *p = Review(c);
        const NSInteger header = [p rowOfFile:@"z2.txt" staged:NO];
        const NSInteger next = FirstBlockAfter(p, header);
        const CGFloat y = NSMinY([p rectOfRow:next]) - NSMinY(p.scrollView.contentView.bounds);
        MCGuiNote(@"after Stage: z2.txt's next block at %.0f (pressed at %.0f), top row %ld: %@",
                  y, pressedY, (long)p.topRow, p.rowDescriptions[(NSUInteger)MAX(0L, (long)p.topRow)]);
        GCHECK(header >= 0 && next > header);
        GCHECK([p.rowDescriptions[(NSUInteger)next] isEqualToString:@"- z 30"]);
        GCHECK(fabs(y - pressedY) <= 1);
        GCHECK(RowShows(p, next));
        // Not m1.txt's rows, nor the staged list at the top.
        GCHECK(p.topRow > [p rowOfFile:@"m1.txt" staged:NO]);
    });
    MCGuiStep(@"the window closes; the real Trash back", ^{
        UseScratchTrash(NO);
        [c.window close];
    });
    MCGuiWait(@"gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}

// `make guitest SUITES=worktree_review`: Compare with Main on a worktree is
// the review page: every file the agent changed against where its branch
// left main (main's later commit left out), in one scroll. Apply to Main is
// refused, naming the file, while main has a change of its own in it; then
// asks and brings every change in as unstaged changes, the untracked file
// too, with nothing committed; and a change that no longer applies comes in
// with conflict marks, listed as a conflict, in words.
MC_GUI_SUITE(worktree_review) {
    __block EditorController *c = nil;
    __block NSInteger actions = 0;
    __block NSUInteger alerts = 0, loads = 0;
    NSString *git = MCGitExecutable();
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"wtreview"];
    NSString *wt = [repo stringByAppendingPathComponent:@".claude/worktrees/agent-x"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the worktree checks are skipped"); });
        return;
    }
    MCChangeReviewPage *(^page)(void) = ^MCChangeReviewPage * {
        id p = [[c valueForKey:@"activeTab"] valueForKey:@"page"];
        return [p isKindOfClass:MCChangeReviewPage.class] ? p : nil;
    };
    NSString *(^inMain)(NSString *) = ^NSString *(NSString *name) {
        return Read([repo stringByAppendingPathComponent:name]);
    };
    GitShell(@"main and a worktree that changed a.txt (committed), b.txt and an untracked n.txt",
             [NSString stringWithFormat:
                  @"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; "
                   "printf '1\\n2\\n3\\n4\\n5\\n' > a.txt; echo b > b.txt; echo .claude/ > .gitignore; "
                   "%@ add -A; %@ commit -qm first; %@ worktree add -q -b wt .claude/worktrees/agent-x; "
                   "cd .claude/worktrees/agent-x; printf '1\\nTWO\\n3\\n4\\n5\\n' > a.txt; "
                   "%@ commit -qam agent; echo bb > b.txt; echo new > n.txt; "
                   "cd '%@'; echo c > c.txt; %@ add c.txt; %@ commit -qm 'main moved on'",
                  repo, repo, G, G, G, G, G, repo, G, G]);
    MCGuiStep(@"a window on main; the agents' model asked", ^{
        UseScratchTrash(YES);
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c toggleAgents:nil];
    });
    MCGuiWait(@"git lists the worktree", 20, ^BOOL {
        return [(MCAgentWindow *)[c valueForKey:@"agents"] worktreeAtPath:wt] != nil;
    });
    MCGuiStep(@"Compare with Main", ^{
        [c toggleAgents:nil];
        [(MCAgentWindow *)[c valueForKey:@"agents"] compareWorktreeAt:wt];
    });
    MCGuiWait(@"the review of the worktree", 15, ^BOOL { return page().loaded; });
    MCGuiStep(@"every file in one scroll; main's later commit left out", ^{
        MCChangeReviewPage *p = page();
        p.onActionDone = ^{ actions++; };
        NSArray *rows = p.rowDescriptions;
        MCGuiNote(@"worktree rows: %@", rows);
        GCHECK_EQ_STR(p.pageKind, @"worktree-changes");
        GCHECK([rows containsObject:@"F a.txt  +1 -1"]);
        GCHECK([rows containsObject:@"F b.txt  +1 -1"]);
        GCHECK([rows containsObject:@"F n.txt  (new)  +1"]);
        for (NSString *r in rows) GCHECK(![r containsString:@"c.txt"]);
        GCHECK(![[p valueForKey:@"applyButton"] isHidden]);
        // Main busy in b.txt.
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"b.txt"], @"main's own edit\n");
        actions = 0;
        alerts = MCGuiAlertsShown().count;
        [p applyToMain];
    });
    MCGuiWait(@"refused", 15, ^BOOL { return actions == 1; });
    MCGuiStep(@"it names b.txt; nothing asked, nothing changed in main", ^{
        MCGuiNote(@"refusal: %@", page().message);
        GCHECK([page().message hasPrefix:@"The main checkout has uncommitted changes of its own in b.txt"]);
        GCHECK(MCGuiAlertsShown().count == alerts);
        GCHECK_EQ_STR(inMain(@"a.txt"), @"1\n2\n3\n4\n5\n");
    });
    GitShell(@"main's own edit put back", [NSString stringWithFormat:@"cd '%@' && %@ checkout -- b.txt", repo, G]);
    MCGuiStep(@"Apply to Main asks; yes", ^{
        actions = 0;
        alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [page() applyToMain];
    });
    MCGuiWait(@"applied", 20, ^BOOL { return actions == 1; });
    MCGuiStep(@"the changes are in main, unstaged and uncommitted; the worktree as it was", ^{
        NSArray *shown = MCGuiAlertsShown();
        GCHECK([[shown subarrayWithRange:NSMakeRange(alerts, shown.count - alerts)]
            isEqualToArray:@[@"Bring these 3 files' changes into main?"]]);
        MCGuiNote(@"applied: %@", page().message);
        GCHECK_EQ_STR(page().message, @"Brought 3 files into main as uncommitted changes.");
        GCHECK_EQ_STR(inMain(@"a.txt"), @"1\nTWO\n3\n4\n5\n");
        GCHECK_EQ_STR(inMain(@"b.txt"), @"bb\n");
        GCHECK_EQ_STR(inMain(@"n.txt"), @"new\n");
        NSString *st = GitText(repo, @[@"status", @"--porcelain"]);
        MCGuiNote(@"main's status: %@", st);
        GCHECK([st containsString:@" M a.txt"] && [st containsString:@" M b.txt"] &&
               [st containsString:@"?? n.txt"]);
        GCHECK_EQ_STR(GitText(repo, @[@"rev-list", @"--count", @"HEAD"]), @"2\n");
        GCHECK_EQ_STR(Read([wt stringByAppendingPathComponent:@"b.txt"]), @"bb\n");
    });
    MCGuiStep(@"the page says it worked: the result on a line of its own, the button done", ^{
        MCChangeReviewPage *p = page();
        [p.pageView layoutSubtreeIfNeeded];
        NSButton *apply = [p valueForKey:@"applyButton"];
        NSButton *source = [p valueForKey:@"sourceButton"];
        NSTextField *note = [p valueForKey:@"messageLabel"];
        MCGuiNote(@"after applying: button \"%@\" (%@), note %@ wide of %.0f",
                  apply.title, apply.enabled ? @"enabled" : @"disabled",
                  NSStringFromRect(note.frame), note.fittingSize.width);
        GCHECK(p.applied);
        GCHECK_EQ_STR(apply.title, @"Applied to Main");
        GCHECK(!apply.enabled && !apply.hidden);
        GCHECK(!source.hidden && NSWidth(source.frame) > 0);
        // The whole sentence shows, whatever the title beside it takes.
        GCHECK(NSWidth(note.frame) >= ceil(note.fittingSize.width));
        GCHECK(NSMinX(source.frame) >= NSMaxX(note.frame));
        MCGuiSnapshot(p.pageView, @"worktree-applied");
        [source performClick:nil];
    });
    MCGuiWait(@"Source Control and main's Uncommitted Changes", 15, ^BOOL {
        return [[c valueForKey:@"gitPanelVisible"] boolValue] && [page().pageKind isEqualToString:@"review"] &&
               page().loaded;
    });
    MCGuiStep(@"the files brought in are listed there; back to the worktree's page", ^{
        NSArray *rows = page().rowDescriptions;
        MCGuiNote(@"main's review: %@", rows);
        GCHECK([rows containsObject:@"F a.txt  +1 -1"] && [rows containsObject:@"F b.txt  +1 -1"]);
        [(MCAgentWindow *)[c valueForKey:@"agents"] compareWorktreeAt:wt];
        GCHECK_EQ_STR(page().pageKind, @"worktree-changes");
        GCHECK(page().applied);
    });
    GitShell(@"main put back, then a.txt's second line changed there and committed",
             [NSString stringWithFormat:@"set -e; cd '%@'; %@ checkout -- .; rm n.txt; "
                                         "printf '1\\nDOS\\n3\\n4\\n5\\n' > a.txt; %@ commit -qam clash",
                                        repo, G, G]);
    MCGuiStep(@"Apply to Main again", ^{
        actions = 0;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [page() applyToMain];
    });
    MCGuiWait(@"applied, with a conflict", 20, ^BOOL { return actions == 1; });
    MCGuiStep(@"a.txt came in with conflict marks, said in words; b.txt and n.txt cleanly", ^{
        MCGuiNote(@"conflict: %@", page().message);
        GCHECK([page().message containsString:@"a.txt did not apply cleanly"]);
        GCHECK([inMain(@"a.txt") containsString:@"<<<<<<<"]);
        NSString *st = GitText(repo, @[@"status", @"--porcelain"]);
        MCGuiNote(@"main's status: %@", st);
        GCHECK([st containsString:@"UU a.txt"] && [st containsString:@" M b.txt"] &&
               [st containsString:@"?? n.txt"]);
        GCHECK_EQ_STR(GitText(repo, @[@"rev-list", @"--count", @"HEAD"]), @"3\n");
    });
    // The note stays while the changes are what the first read after the
    // action showed; change the worktree only once that read is on show,
    // or it shows the change and the note stays (CI, PR #54's first run).
    MCGuiWait(@"the read after Apply on show", 15, ^BOOL {
        return [[page() valueForKey:@"_messageSettled"] boolValue];
    });
    MCGuiStep(@"the agent changes n.txt in the worktree", ^{
        loads = page().loads;
        MCGuiWriteFile([wt stringByAppendingPathComponent:@"n.txt"], @"newer\n");
        [page() reload];
    });
    MCGuiWait(@"the worktree read again", 15, ^BOOL { return page().loads > loads; });
    MCGuiStep(@"the agent worked on: Apply to Main… is back, the old result gone", ^{
        MCChangeReviewPage *p = page();
        NSButton *apply = [p valueForKey:@"applyButton"];
        MCGuiNote(@"after the worktree changed: \"%@\", note \"%@\"", apply.title, p.message);
        GCHECK(!p.applied);
        GCHECK_EQ_STR(apply.title, @"Apply to Main…");
        GCHECK(apply.enabled);
        GCHECK([[p valueForKey:@"sourceButton"] isHidden]);
        GCHECK(p.message.length == 0);
    });
    MCGuiStep(@"the window closes; the real Trash back", ^{
        UseScratchTrash(NO);
        [c.window close];
    });
    MCGuiWait(@"gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}

// `make guitest SUITES=git_conflicts`: a real merge conflict in a scratch
// repository (diff3 style, so each block has its common ancestor). In the
// editor, c.txt's three blocks are found with their labels; a press on
// Keep Current resolves the first as one undo step ("Keep Current
// Change", undone and done again), Keep Incoming the next, Next Change
// goes to the last, and Keep Both leaves none, saying so in the status
// bar. On the review page, r.txt's two blocks show whole with their
// markers, the ancestor and both sides, with Keep Current, Keep Incoming
// and Keep Both on each and Side by Side on the file; Keep Incoming and
// Keep Both write the file, and with none left Mark Resolved stages it,
// after which git lists no conflict.
MC_GUI_SUITE(git_conflicts) {
    __block EditorController *c = nil;
    NSString *git = MCGitExecutable();
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"gitconflicts"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    NSString *cf = [repo stringByAppendingPathComponent:@"c.txt"];
    NSString *rf = [repo stringByAppendingPathComponent:@"r.txt"];
    __block NSInteger actions = 0;
    __block NSUInteger loads = 0;
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the conflict checks are skipped"); });
        return;
    }
    // Eighteen lines; main and topic each change lines 2, 8 and 14 (r.txt:
    // 2 and 8), so the merge stops with three blocks in c.txt, two in r.txt.
    NSString *lines = @"1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\\n10\\n11\\n12\\n13\\n14\\n15\\n16\\n17\\n18\\n";
    GitShell(@"a repository whose merge of topic stops on conflicts in c.txt and r.txt", [NSString
        stringWithFormat:@"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; "
                          "%@ config merge.conflictStyle diff3; "
                          "printf '%@' > c.txt; printf '%@' > r.txt; %@ add -A; %@ commit -qm base; "
                          "%@ checkout -qb topic; "
                          "sed -i.bak -e 's/^2$/two topic/' -e 's/^8$/eight topic/' -e 's/^14$/fourteen topic/' c.txt; "
                          "sed -i.bak -e 's/^2$/two topic/' -e 's/^8$/eight topic/' r.txt; rm -f *.bak; "
                          "%@ commit -qam topic; %@ checkout -q main; "
                          "sed -i.bak -e 's/^2$/two main/' -e 's/^8$/eight main/' -e 's/^14$/fourteen main/' c.txt; "
                          "sed -i.bak -e 's/^2$/two main/' -e 's/^8$/eight main/' r.txt; rm -f *.bak; "
                          "%@ commit -qam main; %@ merge -q topic >/dev/null 2>&1 || true; "
                          "grep -q '^<<<<<<< HEAD' c.txt; grep -q '^||||||| ' r.txt",
                          repo, repo, G, G, lines, lines, G, G, G, G, G, G, G]);
    MCGuiStep(@"a window on it; c.txt open", ^{
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        GCHECK([c showFile:cf]);
    });
    MCGuiWait(@"three blocks found in the editor", 10, ^BOOL {
        CodeTextView *tv = [c valueForKey:@"textView"];
        return tv.conflicts.count == 3;
    });
    MCGuiStep(@"labelled; Keep Current pressed on the first", ^{
        CodeTextView *tv = [c valueForKey:@"textView"];
        MCEditorConflicts *x = tv.conflicts;
        MCGuiNote(@"blocks: %@", x.blockDescriptions);
        GCHECK_EQ_STR(x.blockDescriptions.firstObject, @"current HEAD 1, incoming topic 1, base");
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        [tv scrollRangeToVisible:NSMakeRange(0, 0)];
        const NSRect current = [x buttonRectForKeep:0 block:0];
        const NSRect incoming = [x buttonRectForKeep:1 block:0];
        const NSRect both = [x buttonRectForKeep:2 block:0];
        GCHECK(!NSIsEmptyRect(current) && !NSIsEmptyRect(incoming) && !NSIsEmptyRect(both));
        // In a row on the "<<<<<<<" line, after its text.
        GCHECK(NSMinX(incoming) > NSMaxX(current) && NSMinX(both) > NSMaxX(incoming));
        GCHECK(fabs(NSMidY(current) - NSMidY(MCGuiLineTextRect(tv, [tv.string rangeOfString:@"<<<<<<< HEAD"].location))) < 4);
        MCGuiSnapshot(c.window.contentView, @"git-conflicts-editor");
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK([x pressAtPoint:NSMakePoint(NSMidX(current), NSMidY(current))]);
        });
        GCHECK(x.count == 2);
        GCHECK([tv.string hasPrefix:@"1\ntwo main\n3\n"]);
        GCHECK_EQ_STR(tv.undoManager.undoActionName, @"Keep Current Change");
        GCHECK_EQ_STR([[c valueForKey:@"settingsLabel"] stringValue], @"2 conflicts left in c.txt.");
    });
    MCGuiStep(@"undone: the block is back", ^{
        CodeTextView *tv = [c valueForKey:@"textView"];
        [tv.undoManager undo];
        [tv.conflicts rescanNow];
        GCHECK(tv.conflicts.count == 3);
        GCHECK([tv.string hasPrefix:@"1\n<<<<<<< HEAD\ntwo main\n||||||| "]);
    });
    MCGuiStep(@"Keep Current again, then Keep Incoming on the next", ^{
        CodeTextView *tv = [c valueForKey:@"textView"];
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([tv.conflicts keep:0 block:0]); });
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([tv.conflicts keep:1 block:0]); });
        GCHECK(tv.conflicts.count == 1);
        GCHECK([tv.string hasPrefix:@"1\ntwo main\n3\n4\n5\n6\n7\neight topic\n9\n"]);
        GCHECK_EQ_STR(tv.undoManager.undoActionName, @"Keep Incoming Change");
    });
    MCGuiStep(@"Next Change goes to the last block; Keep Both there", ^{
        CodeTextView *tv = [c valueForKey:@"textView"];
        tv.selectedRange = NSMakeRange(0, 0);
        [c nextChange:nil];
        GCHECK([[tv.string substringFromIndex:tv.selectedRange.location] hasPrefix:@"<<<<<<< HEAD\nfourteen main"]);
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([tv.conflicts keep:2 block:0]); });
        GCHECK(tv.conflicts.count == 0);
        GCHECK_EQ_STR(tv.string, @"1\ntwo main\n3\n4\n5\n6\n7\neight topic\n9\n10\n11\n12\n13\n"
                                 "fourteen main\nfourteen topic\n15\n16\n17\n18\n");
        GCHECK([[[c valueForKey:@"settingsLabel"] stringValue] hasPrefix:@"No conflicts left in c.txt."]);
        GCHECK_EQ_STR(tv.undoManager.undoActionName, @"Keep Both Changes");
        [c saveCurrentFile:nil];
    });
    MCGuiWait(@"saved", 10, ^BOOL { return [Read(cf) hasPrefix:@"1\ntwo main\n"]; });
    MCGuiStep(@"the review page", ^{ [c gitReviewChanges:nil]; });
    MCGuiWait(@"read, both files listed", 15, ^BOOL {
        return Review(c).loaded && [Review(c) rowOfFile:@"r.txt" staged:NO] >= 0 &&
               [Review(c) rowOfFile:@"c.txt" staged:NO] >= 0;
    });
    MCGuiStep(@"r.txt's blocks whole, with the three choices; c.txt has Mark Resolved", ^{
        MCChangeReviewPage *p = Review(c);
        p.onActionDone = ^{ actions++; };
        NSArray<NSString *> *rows = p.rowDescriptions;
        MCGuiNote(@"conflict rows: %@", rows);
        GCHECK([rows containsObject:@"F r.txt  2 conflicts left"]);
        GCHECK([rows containsObject:@"F c.txt  no conflicts left"]);
        const NSInteger header = [p rowOfFile:@"r.txt" staged:NO];
        const NSInteger block = FirstBlockAfter(p, header);
        GCHECK(block > header);
        NSArray *want = @[@"< <<<<<<< HEAD", @"< two main", @"| ||||||| ", @"| 2", @"= =======",
                          @"> two topic", @"> >>>>>>> topic"];
        if (block > 0 && (NSUInteger)block + want.count <= rows.count) {
            NSArray *got = [rows subarrayWithRange:NSMakeRange((NSUInteger)block, want.count)];
            for (NSUInteger i = 0; i < want.count; i++)
                GCHECK([got[i] hasPrefix:want[i]]);
        }
        for (NSString *t in @[@"Keep Current", @"Keep Incoming", @"Keep Both"])
            GCHECK(!NSIsEmptyRect([p buttonRect:t row:block]));
        GCHECK(NSIsEmptyRect([p buttonRect:@"Stage" row:block]));
        GCHECK(!NSIsEmptyRect([p buttonRect:@"Side by Side" row:header]));
        GCHECK(NSIsEmptyRect([p buttonRect:@"Mark Resolved" row:header]));
        GCHECK(!NSIsEmptyRect([p buttonRect:@"Mark Resolved" row:[p rowOfFile:@"c.txt" staged:NO]]));
        MCGuiSnapshot(p.pageView, @"git-conflicts-review");
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Keep Incoming", block));
    });
    MCGuiWait(@"the first block resolved and read again", 15, ^BOOL {
        return actions == 1 && Review(c).loads > loads &&
               [Review(c).rowDescriptions containsObject:@"F r.txt  1 conflict left"];
    });
    MCGuiStep(@"written; Keep Both on the other", ^{
        MCChangeReviewPage *p = Review(c);
        MCGuiNote(@"after Keep Incoming: %@", p.message);
        GCHECK_EQ_STR(p.message, @"1 conflict left in r.txt.");
        GCHECK([Read(rf) hasPrefix:@"1\ntwo topic\n3\n"]);
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Keep Both", FirstBlockAfter(p, [p rowOfFile:@"r.txt" staged:NO])));
    });
    MCGuiWait(@"none left in r.txt", 15, ^BOOL {
        return actions == 1 && Review(c).loads > loads &&
               [Review(c).rowDescriptions containsObject:@"F r.txt  no conflicts left"];
    });
    MCGuiStep(@"Mark Resolved stages r.txt", ^{
        MCChangeReviewPage *p = Review(c);
        GCHECK_EQ_STR(Read(rf), @"1\ntwo topic\n3\n4\n5\n6\n7\neight main\neight topic\n9\n10\n11\n12\n"
                                @"13\n14\n15\n16\n17\n18\n");
        GCHECK([p.message hasPrefix:@"No conflicts left in r.txt."]);
        actions = 0;
        loads = p.loads;
        GCHECK(PressButton(p, @"Mark Resolved", [p rowOfFile:@"r.txt" staged:NO]));
    });
    MCGuiWait(@"staged", 15, ^BOOL {
        return actions == 1 && [GitText(repo, @[@"ls-files", @"-u", @"--", @"r.txt"]) length] == 0;
    });
    MCGuiStep(@"said so; then c.txt marked resolved too", ^{
        MCChangeReviewPage *p = Review(c);
        GCHECK_EQ_STR(p.message, @"Marked r.txt resolved.");
        actions = 0;
        GCHECK(PressButton(p, @"Mark Resolved", [p rowOfFile:@"c.txt" staged:NO]));
    });
    MCGuiWait(@"no conflicts left in git", 15, ^BOOL {
        return actions == 1 && [GitText(repo, @[@"ls-files", @"-u"]) length] == 0 &&
               [GitText(repo, @[@"diff", @"--cached", @"--name-only"]) containsString:@"c.txt"];
    });
    MCGuiStep(@"the window closes", ^{ [c.window close]; });
    MCGuiWait(@"gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}
