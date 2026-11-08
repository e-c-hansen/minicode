// GitGutterSuite.mm — `make guitest SUITES=git_gutter`: the editor's change
// bars (GitGutter.h, QuickDiff.h) on a scratch repository with real git. A
// tracked file edited in three places gets a changed, an added and a
// deleted bar; a line typed above them moves them at once and the next
// diff agrees; Option+F5 and Shift+Option+F5 (through [NSApp sendEvent:],
// as a key press arrives) move the caret between them, wrapping; the
// popover's title and old lines; Revert Change puts a change back as one
// undo step; Stage Change puts one change in the index and nothing else;
// the bars draw in their colors; none for an untracked file, in large-file
// mode, or with editor.git-gutter off.
#import "Harness.h"
#import "EditorController.h"
#import "GitGutter.h"
#import "GitPanel.h"
#import "Gutter.h"
#import "Lsp.h"

namespace {
MCGitGutter *Bars(EditorController *c) { return [c valueForKey:@"gitGutter"]; }
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCLineGutter *GutterOf(EditorController *c) { return [c valueForKey:@"gutter"]; }
NSString *IndexText(NSString *repo, NSString *rel) {
    MCGitResult *r = MCGitRunSync(repo, @[@"show", [@":" stringByAppendingString:rel]]);
    return [[NSString alloc] initWithData:r.output encoding:NSUTF8StringEncoding] ?: @"";
}
NSUInteger CaretLine(EditorController *c) {
    return [GutterOf(c) lineOfCharacter:Text(c).selectedRange.location];
}
// An edit as typing makes it: one undo group, through the text view.
void Type(EditorController *c, NSRange r, NSString *s) {
    NSTextView *tv = Text(c);
    MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:s replacementRange:r]; });
}
NSRange LineRange(NSTextView *tv, NSString *line) {
    return [tv.string rangeOfString:[line stringByAppendingString:@"\n"]];
}
void Shell(NSString *what, NSString *line) {
    MCGuiAsync(what, 60, ^(MCGuiDone done) {
        MCGuiRunProcess(@[@"/bin/sh", @"-c", line], nil, ^(int s, NSString *out, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"%@: %@%@", what, out, err);
            done();
        });
    });
}
NSString *F5() { return [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey]; }
}  // namespace

MC_GUI_SUITE(git_gutter) {
    __block EditorController *c = nil;
    __block NSUInteger diffs = 0, looks = 0;
    __block NSString *savedSettings = nil;
    NSString *git = MCGitExecutable();
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"gitgutter"];
    NSString *g = [repo stringByAppendingPathComponent:@"g.txt"];
    NSString *G = git ? [NSString stringWithFormat:@"'%@'", git] : @"git";
    if (!git) {
        MCGuiStep(@"no git", ^{ MCGuiNote(@"no git here: the change bar checks are skipped"); });
        return;
    }
    NSArray *three = @[@"modified 2+1 (was 2+1)", @"added 6+1 (was 6+0)", @"deleted 9+0 (was 8+1)"];
    NSArray *moved = @[@"modified 3+1 (was 2+1)", @"added 7+1 (was 6+0)", @"deleted 10+0 (was 8+1)"];

    Shell(@"a repository: g.txt of ten lines, and a file of 300,001 lines", [NSString
        stringWithFormat:@"set -e; mkdir -p '%@'; cd '%@'; %@ init -q -b main; "
                          "for i in 0 1 2 3 4 5 6 7 8 9; do echo l$i; done > g.txt; "
                          "awk 'BEGIN { for (i = 0; i <= 300000; i++) print \"x\" }' > big.txt; "
                          "%@ add -A; %@ commit -qm first",
                          repo, repo, G, G, G]);
    MCGuiStep(@"a window on it, showing g.txt", ^{
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        GCHECK(Bars(c) != nil);
        GCHECK(GutterOf(c).changeLane);
        diffs = Bars(c).diffs;
        GCHECK([c showFile:g]);
    });
    MCGuiWait(@"a first diff: no bars", 15, ^BOOL { return Bars(c).diffs > diffs; });
    MCGuiStep(@"three edits: a line changed, one added, one deleted", ^{
        GCHECK(Bars(c).barCount == 0);
        NSTextView *tv = Text(c);
        Type(c, [tv.string rangeOfString:@"l2"], @"L2");
        Type(c, NSMakeRange(NSMaxRange(LineRange(tv, @"l5")), 0), @"new\n");
        Type(c, LineRange(tv, @"l8"), @"");
        diffs = Bars(c).diffs;
    });
    MCGuiWait(@"three bars", 15, ^BOOL {
        return Bars(c).diffs > diffs && [Bars(c).barDescriptions isEqualToArray:three];
    });
    MCGuiStep(@"a line typed above them moves them at once", ^{
        Type(c, NSMakeRange(0, 0), @"top\n");
        MCGuiNote(@"moved: %@", Bars(c).barDescriptions);
        GCHECK([Bars(c).barDescriptions isEqualToArray:moved]);
        diffs = Bars(c).diffs;
    });
    MCGuiWait(@"the next diff agrees, and marks the new line", 15, ^BOOL {
        return Bars(c).diffs > diffs;
    });
    MCGuiStep(@"Option+F5 from the top goes to the changed line", ^{
        NSArray *want = [@[@"added 0+1 (was 0+0)"] arrayByAddingObjectsFromArray:moved];
        MCGuiNote(@"after the diff: %@", Bars(c).barDescriptions);
        GCHECK([Bars(c).barDescriptions isEqualToArray:want]);
        [c.window makeFirstResponder:Text(c)];
        [Text(c) setSelectedRange:NSMakeRange(0, 0)];
        MCGuiSendKeyToApp(c.window, 96, NSEventModifierFlagOption | NSEventModifierFlagFunction,
                          F5(), F5());
        GCHECK(CaretLine(c) == 3);
        MCGuiSendKeyToApp(c.window, 96, NSEventModifierFlagOption | NSEventModifierFlagFunction,
                          F5(), F5());
        GCHECK(CaretLine(c) == 7);
        MCGuiSendKeyToApp(c.window, 96, NSEventModifierFlagOption | NSEventModifierFlagFunction,
                          F5(), F5());
        GCHECK(CaretLine(c) == 10);   // a deletion: the line below it
        MCGuiSendKeyToApp(c.window, 96, NSEventModifierFlagOption | NSEventModifierFlagFunction,
                          F5(), F5());
        GCHECK(CaretLine(c) == 0);    // wrapped
        MCGuiSendKeyToApp(c.window, 96,
                          NSEventModifierFlagOption | NSEventModifierFlagShift |
                              NSEventModifierFlagFunction,
                          F5(), F5());
        MCGuiNote(@"Shift+Option+F5 from line 0: line %lu", (unsigned long)CaretLine(c));
        GCHECK(CaretLine(c) == 10);   // wrapped back
        // The text is as it was: the keys typed nothing.
        GCHECK([Text(c).string hasPrefix:@"top\nl0\nl1\nL2\n"]);
    });
    MCGuiStep(@"a click on the changed bar: its old line, with the buttons", ^{
        MCLineGutter *gutter = GutterOf(c);
        const NSRect r = [gutter rectOfChangeBar:1];
        MCGuiNote(@"the changed bar: %@ in a gutter %.0f wide", NSStringFromRect(r),
                  NSWidth(gutter.bounds));
        GCHECK(!NSIsEmptyRect(r) && NSMaxX(r) < NSWidth(gutter.bounds));
        GCHECK(gutter.onChangeBar != nil);
        gutter.onChangeBar(1, r);   // what mouseDown: calls on the bar
        GCHECK_EQ_STR(Bars(c).popoverTitle, @"1 line changed");
        GCHECK_EQ_STR(Bars(c).popoverText, @"l2");
        [Bars(c) showChange:3 rect:[gutter rectOfChangeBar:3]];
        GCHECK_EQ_STR(Bars(c).popoverTitle, @"1 line deleted");
        GCHECK_EQ_STR(Bars(c).popoverText, @"l8");
        [Bars(c) closePopover];
    });
    MCGuiStep(@"the bars draw in VS Code's colors", ^{
        MCLineGutter *gutter = GutterOf(c);
        NSRect r = [gutter rectOfChangeBar:1];
        NSBitmapImageRep *rep = [gutter bitmapImageRepForCachingDisplayInRect:gutter.bounds];
        [gutter cacheDisplayInRect:gutter.bounds toBitmapImageRep:rep];
        const CGFloat scale = rep.pixelsWide / NSWidth(gutter.bounds);
        NSColor *at = [[rep colorAtX:(NSInteger)(NSMidX(r) * scale)
                                   y:(NSInteger)(NSMidY(r) * scale)]
            colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        MCGuiNote(@"the changed bar's pixel: %.2f %.2f %.2f", at.redComponent, at.greenComponent,
                  at.blueComponent);
        // #1B81A8 (dark themes), within what color matching moves (the
        // cached picture is in the display's space).
        GCHECK(fabs(at.redComponent - 0x1B / 255.0) < 0.15 &&
               fabs(at.greenComponent - 0x81 / 255.0) < 0.15 &&
               fabs(at.blueComponent - 0xA8 / 255.0) < 0.15);
        GCHECK(at.blueComponent > at.redComponent + 0.4);   // not the gutter's gray
        // Drawing a screen of the gutter with bars costs little.
        const double t0 = MCGuiThreadCpuMs();
        for (int i = 0; i < 20; i++) [gutter cacheDisplayInRect:gutter.bounds toBitmapImageRep:rep];
        const double ms = (MCGuiThreadCpuMs() - t0) / 20;
        MCGuiNote(@"the gutter with bars draws in %.2f ms of CPU", ms);
        GCHECK_TIME(ms, 20, @"a gutter drawn with change bars");
    });
    MCGuiStep(@"Revert Change on the changed line", ^{
        NSTextView *tv = Text(c);
        [tv setSelectedRange:NSMakeRange([tv.string rangeOfString:@"L2"].location + 1, 0)];
        GCHECK([Bars(c) barAtCaret] == 1);
        [c revertChange:nil];
        GCHECK([tv.string hasPrefix:@"top\nl0\nl1\nl2\nl3\n"]);
        GCHECK_EQ_STR(tv.undoManager.undoActionName, @"Revert Change");
        // One undo step: undone, the change is back; redone, it is gone.
        [tv.undoManager undo];
        GCHECK([tv.string hasPrefix:@"top\nl0\nl1\nL2\nl3\n"]);
        [tv.undoManager redo];
        GCHECK([tv.string hasPrefix:@"top\nl0\nl1\nl2\nl3\n"]);
        diffs = Bars(c).diffs;
    });
    MCGuiWait(@"its bar gone", 15, ^BOOL {
        return Bars(c).diffs > diffs && Bars(c).barCount == 3;
    });
    __block NSString *stageFailure = @"(not yet)";
    MCGuiStep(@"Stage Change on the added line", ^{
        NSTextView *tv = Text(c);
        GCHECK(![[NSString stringWithContentsOfFile:g encoding:NSUTF8StringEncoding error:nil]
            containsString:@"new"]);   // never saved: the buffer's text is staged
        [tv setSelectedRange:NSMakeRange([tv.string rangeOfString:@"new\n"].location, 0)];
        GCHECK([Bars(c) barAtCaret] == 1);   // the top line's bar is first
        Bars(c).onStaged = ^(NSString *failure) { stageFailure = failure; };
        diffs = Bars(c).diffs;
        [c stageChange:nil];
    });
    MCGuiWait(@"staged", 15, ^BOOL { return stageFailure == nil; });
    MCGuiStep(@"the index has that one line added, nothing else", ^{
        NSString *index = IndexText(repo, @"g.txt");
        MCGuiNote(@"the index: %@", [index stringByReplacingOccurrencesOfString:@"\n" withString:@"|"]);
        GCHECK_EQ_STR(index, @"l0\nl1\nl2\nl3\nl4\nl5\nnew\nl6\nl7\nl8\nl9\n");
    });
    MCGuiWait(@"its bar gone too", 15, ^BOOL {
        return Bars(c).diffs > diffs && Bars(c).barCount == 2;
    });
    MCGuiStep(@"what is left: the top line, the deletion", ^{
        MCGuiNote(@"left: %@", Bars(c).barDescriptions);
        GCHECK([Bars(c).barDescriptions
            isEqualToArray:(@[@"added 0+1 (was 0+0)", @"deleted 10+0 (was 9+1)"])]);
    });

    MCGuiStep(@"an untracked file", ^{
        MCGuiWriteFile([repo stringByAppendingPathComponent:@"u.txt"], @"a\nb\n");
        looks = Bars(c).looks;
        GCHECK([c showFile:[repo stringByAppendingPathComponent:@"u.txt"]]);
    });
    MCGuiWait(@"looked at", 15, ^BOOL { return Bars(c).looks > looks; });
    MCGuiStep(@"edited", ^{
        Type(c, NSMakeRange(0, 0), @"zz\n");
        diffs = Bars(c).diffs;
    });
    MCGuiWait(@"diffed against nothing", 15, ^BOOL { return Bars(c).diffs > diffs; });
    MCGuiStep(@"no bars", ^{ GCHECK(Bars(c).barCount == 0); });

    MCGuiStep(@"a file in large-file mode", ^{
        looks = Bars(c).looks;
        GCHECK([c showFile:[repo stringByAppendingPathComponent:@"big.txt"]]);
    });
    MCGuiWait(@"looked at", 15, ^BOOL { return Bars(c).looks > looks; });
    MCGuiStep(@"edited", ^{
        GCHECK([[c valueForKey:@"largeFileFeaturesOff"] boolValue]);
        Type(c, NSMakeRange(0, 0), @"zz\n");
        looks = Bars(c).looks;
    });
    MCGuiWait(@"looked at again", 15, ^BOOL { return Bars(c).looks > looks; });
    MCGuiStep(@"no bars there either", ^{ GCHECK(Bars(c).barCount == 0); });

    MCGuiStep(@"editor.git-gutter off: no lane, no bars", ^{
        savedSettings = [NSString stringWithContentsOfFile:
                             NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"]
                                                  encoding:NSUTF8StringEncoding error:nil] ?: @"";
        GCHECK([c showFile:g]);
        MCGuiSetSettings([savedSettings stringByAppendingString:@"\neditor.git-gutter = off\n"]);
        looks = Bars(c).looks;
    });
    MCGuiWait(@"looked at", 15, ^BOOL { return Bars(c).looks > looks; });
    MCGuiStep(@"none", ^{
        GCHECK(!GutterOf(c).changeLane);
        GCHECK(Bars(c).barCount == 0);
        MCGuiSetSettings(savedSettings);
        GCHECK(GutterOf(c).changeLane);
        diffs = Bars(c).diffs;
    });
    MCGuiWait(@"back on: the bars again", 15, ^BOOL {
        return Bars(c).diffs > diffs && Bars(c).barCount == 2;
    });
    MCGuiStep(@"the window closes, unsaved tabs left as they are", ^{
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Don't Save
        [c.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
}
