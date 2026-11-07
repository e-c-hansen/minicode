// CompareSuite.mm — `make guitest SUITES=compare`: the compare view as a
// review tool (CompareView.h).
//   - Both panes take the file's syntax colors, and inside a changed pair of
//     lines only the words that changed are marked.
//   - F7 and Shift+F7, through the key monitor as a key press goes, step
//     through the runs of changes, wrapping around both ways; after the user
//     scrolls away, F7 goes on from what the panes show. F7 in the editor
//     is left alone.
//   - A comparison with a decision gets Accept and Reject per hunk between
//     the panes; the decision ends once every hunk has a choice, or with the
//     bar's Accept the Rest or Reject the Rest, and its text is made of the
//     accepted hunks only.
#import <Carbon/Carbon.h>

#import "Harness.h"
#import "EditorController.h"
#import "AppSettings.h"
#import "CompareView.h"
#include "SyntaxHighlighter.h"
#include <mach/mach.h>

namespace {
// The main thread's CPU time, in ms.
double CpuMs() {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const mach_port_t self = mach_thread_self();
    const kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS) return 0;
    return (info.user_time.seconds + info.system_time.seconds) * 1e3 +
           (info.user_time.microseconds + info.system_time.microseconds) / 1e3;
}

MCCompareView *CompareViewOf(EditorController *c) { return [c valueForKey:@"compareView"]; }

NSString *F7() { return [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey]; }
void PressF7(EditorController *c, BOOL shift) {
    MCGuiSendKeyToApp(c.window, kVK_F7,
                      NSEventModifierFlagFunction | (shift ? NSEventModifierFlagShift : 0), F7(), F7());
}

// 150 lines of Python; the right side changes three of them far apart
// (one word each) and adds a line after the last.
NSString *OldText() {
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < 150; i++) [s appendFormat:@"def f%d(x):  # line %d\n", i, i + 1];
    return s;
}
NSString *NewText() {
    NSMutableArray *lines = [[OldText() componentsSeparatedByString:@"\n"] mutableCopy];
    lines[10] = @"def f10(y):  # line 11";
    lines[70] = @"def f70(x):  # row 71";
    lines[130] = @"def renamed(x):  # line 131";
    [lines insertObject:@"    pass" atIndex:131];
    return [lines componentsJoinedByString:@"\n"];
}

MCComparison *MakeComparison(NSString *title) {
    MCComparison *cmp = [MCComparison new];
    cmp.title = title;
    cmp.leftLabel = @"before: review.py";
    cmp.rightLabel = @"after: review.py";
    cmp.leftText = OldText();
    cmp.rightText = NewText();
    cmp.fileName = @"review.py";
    [cmp prepare];
    return cmp;
}

// The row of a pane's line holding `text` (the pane's rows are "\n" apart).
NSUInteger RowOf(NSTextView *tv, NSString *text) {
    NSArray<NSString *> *rows = [tv.string componentsSeparatedByString:@"\n"];
    for (NSUInteger i = 0; i < rows.count; i++)
        if ([rows[i] containsString:text]) return i;
    return NSNotFound;
}

// Where `text` is in a pane, and the background there.
NSColor *BackgroundAt(NSTextView *tv, NSString *text) {
    const NSRange r = [tv.string rangeOfString:text];
    if (r.location == NSNotFound) return nil;
    return [tv.textStorage attribute:NSBackgroundColorAttributeName atIndex:r.location effectiveRange:NULL];
}
NSColor *ForegroundAt(NSTextView *tv, NSString *text) {
    const NSRange r = [tv.string rangeOfString:text];
    if (r.location == NSNotFound) return nil;
    return [tv.textStorage attribute:NSForegroundColorAttributeName atIndex:r.location effectiveRange:NULL];
}

// The row the pane's caret is on.
NSUInteger CaretRow(NSTextView *tv) {
    NSString *before = [tv.string substringToIndex:MIN(tv.selectedRange.location, tv.string.length)];
    return [before componentsSeparatedByString:@"\n"].count - 1;
}
}  // namespace

MC_GUI_SUITE(compare) {
    __block EditorController *c = nil;
    __block MCCompareView *v = nil;

    // A window of its own, closed at the end, so the tabs left here never
    // reach another suite's counts.
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"compare"];
    MCGuiStep(@"a window on a folder of its own", ^{
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"README.md"], @"compare\n");
        const NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:dir]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
    });

    MCGuiStep(@"a comparison of a Python file, shown", ^{
        [c showComparison:MakeComparison(@"review") preview:YES];
        v = CompareViewOf(c);
        GCHECK(v != nil && !v.hidden);
        GCHECK(v.comparison.hunkCount == 3);
        GCHECK_EQ_STR(v.changesText, @"3 changes");
        GCHECK(v.topRow == 0);
        MCGuiSnapshot(c.window.contentView, @"compare-top");
    });

    MCGuiStep(@"both panes are syntax colored", ^{
        AppSettings *cfg = [AppSettings shared];
        NSColor *keyword = [cfg syntax:TokenStyle::Keyword], *comment = [cfg syntax:TokenStyle::Comment];
        GCHECK([ForegroundAt(v.leftView, @"def f0") isEqual:keyword]);
        GCHECK([ForegroundAt(v.rightView, @"def f0") isEqual:keyword]);
        GCHECK([ForegroundAt(v.rightView, @"# row 71") isEqual:comment]);
        GCHECK([ForegroundAt(v.leftView, @"# line 1\n") isEqual:comment]);
    });

    MCGuiStep(@"inside a changed line only the changed words are marked", ^{
        // f10(x) -> f10(y): only the x and the y.
        const NSUInteger row = RowOf(v.rightView, @"def f10(y)");
        GCHECK(row == RowOf(v.leftView, @"def f10(x)") && row == 10);
        GCHECK(BackgroundAt(v.rightView, @"y):  # line 11") != nil);
        GCHECK(BackgroundAt(v.leftView, @"x):  # line 11") != nil);
        GCHECK(BackgroundAt(v.rightView, @"def f10(y)") == nil);
        GCHECK(BackgroundAt(v.rightView, @"# line 11") == nil);
        // line -> row in a comment.
        GCHECK(BackgroundAt(v.rightView, @"row 71") != nil);
        GCHECK(BackgroundAt(v.rightView, @"def f70") == nil);
        // An unchanged line has none.
        GCHECK(BackgroundAt(v.rightView, @"def f5(x)") == nil);
    });

    MCGuiStep(@"F7 goes to each change in turn and wraps to the first", ^{
        [c.window makeFirstResponder:v.leftView];
        PressF7(c, NO);
        GCHECK(v.currentHunk == 0);
        GCHECK_EQ_STR(v.changesText, @"Change 1 of 3");
        GCHECK(CaretRow(v.leftView) == 10);
        PressF7(c, NO);
        GCHECK(v.currentHunk == 1);
        GCHECK(CaretRow(v.leftView) == 70);
        // Shown on screen, with the panes still level.
        GCHECK(v.topRow <= 70 && v.topRow + 5 < 70 + 40);
        GCHECK(fabs(v.leftScroll.contentView.bounds.origin.y - v.rightScroll.contentView.bounds.origin.y) < 0.5);
        PressF7(c, NO);
        GCHECK(v.currentHunk == 2);
        GCHECK_EQ_STR(v.changesText, @"Change 3 of 3");
        PressF7(c, NO);
        GCHECK(v.currentHunk == 0);
        MCGuiNote(@"top row after wrapping: %lu", (unsigned long)v.topRow);
        GCHECK(v.topRow <= 10);
    });

    MCGuiStep(@"Shift+F7 goes back, wrapping to the last", ^{
        PressF7(c, YES);
        GCHECK(v.currentHunk == 2);
        PressF7(c, YES);
        GCHECK(v.currentHunk == 1);
        // The keyboard stayed in the pane.
        GCHECK(c.window.firstResponder == v.leftView);
    });

    MCGuiStep(@"after scrolling away, F7 goes on from what the panes show", ^{
        // Scroll to just past the second change: F7 finds the third.
        NSRect row = [v.leftView.layoutManager lineFragmentRectForGlyphAtIndex:
                          [v.leftView.layoutManager glyphIndexForCharacterAtIndex:
                              [v.leftView.string rangeOfString:@"def f90(x)"].location] effectiveRange:NULL];
        [v.leftScroll.contentView scrollToPoint:NSMakePoint(0, NSMinY(row) + v.leftView.textContainerOrigin.y)];
        [v.leftScroll reflectScrolledClipView:v.leftScroll.contentView];
        MCGuiNote(@"top row: %lu", (unsigned long)v.topRow);
        GCHECK(v.topRow >= 85 && v.topRow <= 95);
        PressF7(c, NO);
        GCHECK(v.currentHunk == 2);
    });

    MCGuiStep(@"the menu items are on in the compare view, off in the editor", ^{
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Next Difference"
                                                      action:@selector(goToNextDifference:)
                                               keyEquivalent:@""];
        id target = [NSApp targetForAction:@selector(goToNextDifference:) to:nil from:item];
        GCHECK(target == v && [target validateMenuItem:item]);
        // In the editor nothing answers F7, and the key does not type.
        NSString *py = [dir stringByAppendingPathComponent:@"compare-plain.py"];
        MCGuiWriteFile(py, @"x = 1\n");
        GCHECK([c showFile:py]);
        NSTextView *tv = [c valueForKey:@"textView"];
        [c.window makeFirstResponder:tv];
        GCHECK([NSApp targetForAction:@selector(goToNextDifference:) to:nil from:item] == nil);
        PressF7(c, NO);
        GCHECK_EQ_STR(tv.string, @"x = 1\n");
    });

    // ------------------------------------------------- deciding per hunk
    __block int calls = 0;
    __block BOOL answeredYes = NO;
    __block MCComparison *decided = nil;
    MCGuiStep(@"a comparison waiting for a decision gets Accept and Reject per hunk", ^{
        decided = MakeComparison(@"decide");
        decided.decisionPrompt = @"Claude Code wants to change review.py.";
        decided.onDecision = ^(BOOL accepted) {
            calls++;
            answeredYes = accepted;
        };
        [c showComparison:decided preview:YES];
        GCHECK(v.comparison == decided);
        GCHECK(!v.acceptButton.hidden && [v.acceptButton.title isEqualToString:@"Accept"]);
        // The first hunk is on screen at the top; the third is not.
        GCHECK(!NSIsEmptyRect([v rectForHunk:0 accept:YES]));
        GCHECK(NSIsEmptyRect([v rectForHunk:2 accept:YES]));
        // The panes left room for the buttons between them.
        GCHECK(NSMaxX(v.leftScroll.frame) + 30 < NSMinX(v.rightScroll.frame));
        MCGuiSnapshot(c.window.contentView, @"compare-decide");
    });

    MCGuiStep(@"reject the first change from its button", ^{
        const NSRect r = [v rectForHunk:0 accept:NO];
        GCHECK([v pressHunkButtonAtPoint:NSMakePoint(NSMidX(r), NSMidY(r))]);
        GCHECK([decided decisionOfHunk:0] == -1 && decided.decision == 0 && calls == 0);
        GCHECK_EQ_STR(v.rejectButton.title, @"Reject the Rest");
        // A press beside the buttons does nothing.
        GCHECK(![v pressHunkButtonAtPoint:NSMakePoint(NSMinX(r) - 30, NSMidY(r))]);
    });

    MCGuiStep(@"F7 to the second change, accept it; Accept the Rest takes the third", ^{
        [c.window makeFirstResponder:v.rightView];
        PressF7(c, NO);
        PressF7(c, NO);
        GCHECK(v.currentHunk == 1);
        const NSRect r = [v rectForHunk:1 accept:YES];
        GCHECK(!NSIsEmptyRect(r));
        GCHECK([v pressHunkButtonAtPoint:NSMakePoint(NSMidX(r), NSMidY(r))]);
        GCHECK([decided decisionOfHunk:1] == 1 && calls == 0);
        [v.acceptButton performClick:nil];
        GCHECK(calls == 1 && answeredYes && decided.decision == 1);
        GCHECK([decided decisionOfHunk:2] == 1);
        // The text: the old first change, the new second and third.
        NSMutableArray *want = [[NewText() componentsSeparatedByString:@"\n"] mutableCopy];
        want[10] = @"def f10(x):  # line 11";
        GCHECK_EQ_STR(decided.acceptedText, [want componentsJoinedByString:@"\n"]);
        GCHECK(v.acceptButton.hidden && NSIsEmptyRect([v rectForHunk:1 accept:YES]));
        // Pressing again does nothing more.
        [decided decide:NO];
        GCHECK(calls == 1 && decided.decision == 1);
    });

    MCGuiStep(@"Reject with nothing accepted rejects the whole change", ^{
        MCComparison *cmp = MakeComparison(@"reject");
        cmp.decisionPrompt = @"Claude Code wants to change review.py.";
        __block int n = 0;
        __block BOOL yes = YES;
        cmp.onDecision = ^(BOOL accepted) { n++; yes = accepted; };
        [c showComparison:cmp preview:YES];
        [v.rejectButton performClick:nil];
        GCHECK(n == 1 && !yes && cmp.decision == -1 && cmp.acceptedText == nil);
    });

    MCGuiStep(@"Accept with nothing decided per hunk answers the whole new text", ^{
        MCComparison *cmp = MakeComparison(@"accept");
        cmp.decisionPrompt = @"Claude Code wants to change review.py.";
        __block BOOL yes = NO;
        cmp.onDecision = ^(BOOL accepted) { yes = accepted; };
        [c showComparison:cmp preview:YES];
        [v.acceptButton performClick:nil];
        GCHECK(yes && [cmp.acceptedText isEqualToString:NewText()]);
    });

    // A big review: 50,000 lines of Python, a word changed in every tenth.
    // Lining up, coloring and word diffs are prepare's (off the main thread
    // for `minicode diff` and openDiff); showing is the main thread's. CPU
    // time of this thread, not wall time, so a loaded machine still passes.
    MCGuiStep(@"a 50,000-line comparison with 5,000 changes is prepared and shown in bounded time", ^{
        NSMutableString *a = [NSMutableString string], *b = [NSMutableString string];
        for (int i = 0; i < 50000; i++) {
            NSString *line = [NSString stringWithFormat:@"    total_%d = compute(value_%d, %d)  # step %d\n", i, i, i, i];
            [a appendString:line];
            [b appendString:i % 10 == 3 ? [line stringByReplacingOccurrencesOfString:@"compute" withString:@"recompute"]
                                       : line];
        }
        MCComparison *cmp = [MCComparison new];
        cmp.title = @"big review";
        cmp.leftText = a;
        cmp.rightText = b;
        cmp.fileName = @"big.py";
        const double c0 = CpuMs();
        [cmp prepare];
        const double prepared = CpuMs() - c0;
        [c showComparison:cmp preview:YES];
        PressF7(c, NO);
        const double shown = CpuMs() - c0 - prepared;
        MCGuiNote(@"prepare %.0f ms CPU, show and F7 %.0f ms CPU", prepared, shown);
        GCHECK(cmp.hunkCount == 5000);
        GCHECK(v.currentHunk == 0);
        GCHECK(prepared < 4000);
        GCHECK(shown < 4000);
    });

    MCGuiStep(@"rejecting every hunk on its own is a rejection", ^{
        MCComparison *cmp = MakeComparison(@"each");
        cmp.decisionPrompt = @"Claude Code wants to change review.py.";
        __block int n = 0;
        __block BOOL yes = YES;
        cmp.onDecision = ^(BOOL accepted) { n++; yes = accepted; };
        [c showComparison:cmp preview:YES];
        for (NSUInteger i = 0; i < 3; i++) [cmp decideHunk:i as:-1];
        GCHECK(n == 1 && !yes && cmp.decision == -1);
    });

    MCGuiStep(@"close the window", ^{ [c.window close]; });
    MCGuiWait(@"the window gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
    MCGuiStep(@"done with the window", ^{
        c = nil;
        v = nil;
        MCGuiSetKeyWindow(nil);
    });
}
