// GitPanelSuite.mm — `make guitest SUITES=git_panel`: Source Control
// (Ctrl+Shift+G) in a window of its own on a scratch repository. The panel
// lists a modification, a deletion and an untracked file, Space's action
// stages and unstages, a diff takes the editor's slot (and saves nothing),
// a commit with a message lands in the graph, an empty message is refused
// with git's own words, and nothing but the index and the commit changed.
#import "Harness.h"
#import "EditorController.h"
#import "EditorFont.h"
#import "GitPanel.h"
#include <objc/runtime.h>

namespace {
MCGitPanel *Git(EditorController *c) { return [c valueForKey:@"gitPanel"]; }
NSInteger RowOf(EditorController *c, NSString *description) {
    return (NSInteger)[[Git(c) rowDescriptions] indexOfObject:description];
}
BOOL Rows(EditorController *c, NSArray<NSString *> *rows) {
    return Git(c).inRepository && [[Git(c) rowDescriptions] isEqualToArray:rows];
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
        GCHECK(Git(c).messageView.string.length == 0);
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
    MCGuiStep(@"the window closes", ^{
        if (!git) return;
        [c.window close];
    });
    MCGuiWait(@"gone", 5, ^BOOL { return !git || ![MCGuiControllers() containsObject:c]; });
}
