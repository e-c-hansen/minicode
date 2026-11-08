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
        NSArray *want = @[@"Open File", @"Open Changes", @"Stage Changes",
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
        if (![v isKindOfClass:NSString.class] || ![v hasPrefix:@"minicode-hunk:"]) return;
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
