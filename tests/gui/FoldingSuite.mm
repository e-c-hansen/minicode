// FoldingSuite.mm — `make guitest SUITES=folding`: code folding in the
// editor (FoldView.h): the menu keys, what a fold hides and what it leaves
// alone (the text, undo, copying, the gutter's numbers), folds following
// edits and opening when touched or gone to, the gutter's chevrons and the
// placeholder, tab switches, and timings on a 100,000-line file.
#import "Harness.h"
#import "EditorController.h"
#import "FoldView.h"
#import "Gutter.h"
#import "Lsp.h"
#include <chrono>

namespace {

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }

// The character where a 1-based line starts.
NSUInteger LineStart(NSString *s, NSUInteger line) {
    NSUInteger at = 0;
    for (NSUInteger l = 1; l < line && at < s.length; l++)
        at = NSMaxRange([s lineRangeForRange:NSMakeRange(at, 0)]);
    return at;
}

// The 1-based lines that start a line fragment, in order: what is on show.
NSArray<NSNumber *> *ShownLines(NSTextView *tv) {
    NSLayoutManager *lm = tv.layoutManager;
    [lm ensureLayoutForTextContainer:tv.textContainer];
    NSString *s = tv.string;
    NSMutableArray *out = [NSMutableArray array];
    __block NSUInteger line = 1, scanned = 0;
    [lm enumerateLineFragmentsForGlyphRange:NSMakeRange(0, lm.numberOfGlyphs)
                                 usingBlock:^(NSRect r, NSRect u, NSTextContainer *c, NSRange g, BOOL *stop) {
        (void)r; (void)u; (void)c; (void)stop;
        NSUInteger ch = [lm characterIndexForGlyphAtIndex:g.location];
        for (; scanned < ch; scanned++)
            if ([s characterAtIndex:scanned] == '\n') line++;
        [out addObject:@(line)];
    }];
    return out;
}

NSString *Join(NSArray<NSNumber *> *a) { return [a componentsJoinedByString:@" "]; }

double Ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

// A point at the middle of a 1-based line's first fragment, in the window.
NSPoint WindowPointOfLine(NSTextView *tv, NSUInteger line, NSView *in, CGFloat x) {
    NSLayoutManager *lm = tv.layoutManager;
    NSUInteger ch = LineStart(tv.string, line);
    NSRect r = [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:ch]
                                    effectiveRange:NULL];
    NSPoint p = NSMakePoint(0, NSMidY(r) + tv.textContainerOrigin.y);
    p = [tv convertPoint:p toView:in];
    p.x = x;
    return [in convertPoint:p toView:nil];
}

NSEvent *Click(NSWindow *w, NSPoint at, NSInteger count) {
    return [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at modifierFlags:0
                             timestamp:0 windowNumber:w.windowNumber context:nil
                           eventNumber:0 clickCount:count pressure:1];
}

}  // namespace

MC_GUI_SUITE(folding) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    __block MCFolds *folds = nil;
    __block NSString *original = nil;
    NSString *py = MCGuiProjectFile(@"fold.py");
    NSString *cfile = MCGuiProjectFile(@"fold.c");
    NSString *md = MCGuiProjectFile(@"fold.md");
    NSString *pyText =
        @"import os\n"              // 1
        @"\n"                       // 2
        @"def f(x):\n"              // 3
        @"    if x:\n"              // 4
        @"        return 1\n"       // 5
        @"    return 2\n"           // 6
        @"\n"                       // 7
        @"class A:\n"               // 8
        @"    def g(self):\n"       // 9
        @"        target = 1\n"     // 10
        @"        return target\n"  // 11
        @"\n"                       // 12
        @"x = 3\n";                 // 13

    MCGuiStep(@"the files", ^{
        MCGuiWriteFile(py, pyText);
        MCGuiWriteFile(cfile, @"int main() {\n    if (a) {\n        b();\n    } else {\n        c();\n    }\n    return 0;\n}\n");
        MCGuiWriteFile(md, @"# One\ntext\n\n## Sub\nmore\n\n# Two\nend\n");
        c = MCGuiController();
        tv = Editor(c);
        folds = tv.folds;
        GCHECK(c && tv && folds);
        GCHECK([c showFile:py]);
        original = [tv.string copy];
    });
    MCGuiWait(@"regions for fold.py", 5, ^BOOL { return [folds lineStartsRegion:2]; });

    MCGuiStep(@"menu keys: every one once, folding on Control+Command", ^{
        NSMutableDictionary<NSString *, NSMutableArray *> *keys = [NSMutableDictionary dictionary];
        __block void (^walk)(NSMenu *);
        __block __weak void (^weakWalk)(NSMenu *);
        weakWalk = walk = ^(NSMenu *menu) {
            for (NSMenuItem *it in menu.itemArray) {
                if (it.submenu) weakWalk(it.submenu);
                if (!it.keyEquivalent.length) continue;
                NSEventModifierFlags m = it.keyEquivalentModifierMask &
                    (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                     NSEventModifierFlagOption | NSEventModifierFlagControl);
                NSString *k = [NSString stringWithFormat:@"%lu:%@", (unsigned long)m,
                                                         it.keyEquivalent.lowercaseString];
                if (!keys[k]) keys[k] = [NSMutableArray array];
                [keys[k] addObject:it.title];
            }
        };
        walk(NSApp.mainMenu);
        NSUInteger clashes = 0;
        for (NSString *k in keys)
            if (keys[k].count > 1) {
                // Hidden second keys share an action and a title, not a clash.
                NSSet *titles = [NSSet setWithArray:keys[k]];
                if (titles.count > 1) { clashes++; MCGuiNote(@"clash %@: %@", k, keys[k]); }
            }
        GCHECK(clashes == 0);
        NSString *(^title)(NSEventModifierFlags, NSString *) = ^NSString *(NSEventModifierFlags m, NSString *k) {
            return keys[[NSString stringWithFormat:@"%lu:%@", (unsigned long)m, k]].firstObject;
        };
        const NSEventModifierFlags cc = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        const NSEventModifierFlags oc = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        GCHECK_EQ_STR(title(cc, @"["), @"Fold");
        GCHECK_EQ_STR(title(cc, @"0"), @"Fold All");
        GCHECK_EQ_STR(title(cc, @"7"), @"Fold Level 7");
        GCHECK_EQ_STR(title(oc, @"["), @"Previous Terminal");
    });

    MCGuiStep(@"Control+Command+[ folds the def the caret is in", ^{
        [c.window makeFirstResponder:tv];
        // A key as the keyboard makes it: Control+[ types ESC, and the menu
        // goes by the key without its modifiers.
        tv.selectedRange = NSMakeRange(LineStart(tv.string, 5) + 10, 0);   // in "return 1"
        GCHECK(MCGuiPerformMenuKey(c.window, 33, NSEventModifierFlagControl | NSEventModifierFlagCommand,
                                   nil, nil));
        // The innermost: the if on line 4, hiding line 5.
        GCHECK([folds lineStartsFold:3]);
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 4 6 7 8 9 10 11 12 13");
        // The caret went to the end of the fold's first line.
        GCHECK(tv.selectedRange.location == LineStart(tv.string, 5) - 1);
        GCHECK_EQ_STR(tv.string, original);   // the text is untouched
        // Again: the def.
        GCHECK(MCGuiPerformMenuKey(c.window, 33, NSEventModifierFlagControl | NSEventModifierFlagCommand,
                                   nil, nil));
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 7 8 9 10 11 12 13");
        MCGuiSnapshot(c.window.contentView, @"fold-def");
    });

    MCGuiStep(@"the gutter numbers only the lines on show", ^{
        NSScrollView *sv = tv.enclosingScrollView;
        MCLineGutter *g = (MCLineGutter *)sv.verticalRulerView;
        GCHECK([g isKindOfClass:MCLineGutter.class] && g.shown);
        MCGuiSnapshot(sv, @"fold-gutter");
    });

    MCGuiStep(@"typing elsewhere moves the fold; at the end of its first line too", ^{
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"# top\n" replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK([folds lineStartsFold:3]);   // now line 4 (0-based 3)
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 4 8 9 10 11 12 13 14");
        NSUInteger end = LineStart(tv.string, 5) - 1;   // "def f(x):|"
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"  # c" replacementRange:NSMakeRange(end, 0)];
        });
        GCHECK([folds lineStartsFold:3]);
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 4 8 9 10 11 12 13 14");
        [tv.undoManager undo];
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
        GCHECK([folds lineStartsFold:2]);
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 7 8 9 10 11 12 13");
    });

    MCGuiStep(@"copying across a fold copies what it hides", ^{
        NSUInteger a = LineStart(tv.string, 3), b = LineStart(tv.string, 8);
        tv.selectedRange = NSMakeRange(a, b - a);
        GCHECK([folds lineStartsFold:2]);   // a selection over it leaves it closed
        [tv copy:nil];
        NSString *got = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        GCHECK([got containsString:@"return 1"] && [got containsString:@"return 2"]);
    });

    MCGuiStep(@"Go to Line inside the fold opens it", ^{
        [c goToLine:5 column:5];
        GCHECK(![folds lineStartsFold:2] && ![folds lineStartsFold:3]);
        GCHECK(ShownLines(tv).count == 13);
        GCHECK(tv.selectedRange.location == LineStart(tv.string, 5) + 4);
    });

    MCGuiStep(@"Fold All, a selection inside (as Find makes), Unfold All", ^{
        GCHECK(MCGuiPerformMenuKey(c.window, 29, NSEventModifierFlagControl | NSEventModifierFlagCommand,
                                   nil, nil));
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 7 8 12 13");
        NSRange t = [tv.string rangeOfString:@"target = 1"];
        tv.selectedRange = t;   // what Find does with a match
        GCHECK([[tv.string substringWithRange:tv.selectedRange] isEqualToString:@"target = 1"]);
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 7 8 9 10 11 12 13");
        GCHECK(MCGuiPerformMenuKey(c.window, 38, NSEventModifierFlagControl | NSEventModifierFlagCommand,
                                   nil, nil));
        GCHECK(ShownLines(tv).count == 13 && !folds.hasFolds);
    });

    MCGuiStep(@"Fold Level 2 leaves the caret's region open", ^{
        tv.selectedRange = NSMakeRange(LineStart(tv.string, 10), 0);
        GCHECK(MCGuiPerformMenuKey(c.window, 19, NSEventModifierFlagControl | NSEventModifierFlagCommand,
                                   nil, nil));
        // Level 2: the if (line 4) and def g (line 9, holding the caret).
        GCHECK([folds lineStartsFold:3] && ![folds lineStartsFold:8]);
        [folds unfoldAll];
    });

    MCGuiStep(@"the chevron in the gutter folds and unfolds", ^{
        MCLineGutter *g = (MCLineGutter *)tv.enclosingScrollView.verticalRulerView;
        [g mouseEntered:[NSEvent enterExitEventWithType:NSEventTypeMouseEntered location:NSZeroPoint
                                          modifierFlags:0 timestamp:0 windowNumber:c.window.windowNumber
                                                context:nil eventNumber:0 trackingNumber:0 userData:nil]];
        [g display];
        MCGuiSnapshot(tv.enclosingScrollView, @"fold-chevrons");
        NSPoint at = WindowPointOfLine(tv, 8, g, NSWidth(g.bounds) - 6);   // class A
        NSRange before = tv.selectedRange;
        [g mouseDown:Click(c.window, at, 1)];
        GCHECK([folds lineStartsFold:7]);
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 4 5 6 7 8 12 13");
        GCHECK(NSEqualRanges(before, tv.selectedRange) || tv.selectedRange.location < LineStart(tv.string, 9));
        // A click on the numbers still selects the line.
        NSPoint num = WindowPointOfLine(tv, 3, g, 20);
        [g mouseDown:Click(c.window, num, 1)];
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(LineStart(tv.string, 3),
                                                          LineStart(tv.string, 4) - LineStart(tv.string, 3))));
        GCHECK([folds lineStartsFold:7]);
        [g mouseDown:Click(c.window, at, 1)];
        GCHECK(![folds lineStartsFold:7]);
    });

    MCGuiStep(@"a click on the placeholder opens the fold", ^{
        [folds toggleLine:7];
        GCHECK([folds lineStartsFold:7]);
        [tv display];
        MCGuiSnapshot(tv.enclosingScrollView, @"fold-placeholder");
        // The placeholder sits just after "class A:".
        NSLayoutManager *lm = tv.layoutManager;
        NSUInteger hs = LineStart(tv.string, 9) - 1;
        NSRect r = [lm boundingRectForGlyphRange:NSMakeRange([lm glyphIndexForCharacterAtIndex:hs], 1)
                                 inTextContainer:tv.textContainer];
        GCHECK(NSWidth(r) > 10);
        NSPoint p = NSMakePoint(NSMidX(r) + tv.textContainerOrigin.x, NSMidY(r) + tv.textContainerOrigin.y);
        [tv mouseDown:Click(c.window, [tv convertPoint:p toView:nil], 1)];
        GCHECK(![folds lineStartsFold:7]);
        GCHECK(ShownLines(tv).count == 13);
    });

    MCGuiStep(@"Backspace after the placeholder opens the fold, deleting nothing", ^{
        [folds toggleLine:7];
        NSUInteger he = LineStart(tv.string, 12) - 1;   // the line break ending "return target"
        tv.selectedRange = NSMakeRange(he, 0);
        GCHECK([folds lineStartsFold:7]);
        [tv doCommandBySelector:@selector(deleteBackward:)];
        GCHECK(![folds lineStartsFold:7]);
        GCHECK_EQ_STR(tv.string, original);
    });

    MCGuiStep(@"Return after the placeholder makes a line below the fold", ^{
        [folds toggleLine:7];
        NSUInteger he = LineStart(tv.string, 12) - 1;
        tv.selectedRange = NSMakeRange(he, 0);
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"\n" replacementRange:tv.selectedRange]; });
        GCHECK([folds lineStartsFold:7]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
    });

    MCGuiStep(@"typing inside a fold's lines (an edit from elsewhere) opens it", ^{
        GCHECK([folds lineStartsFold:7]);
        NSRange t = [tv.string rangeOfString:@"target = 1"];
        [tv.textStorage replaceCharactersInRange:NSMakeRange(t.location, 6) withString:@"aim"];
    });
    MCGuiWait(@"the fold opens", 2, ^BOOL { return ![folds lineStartsFold:7] && ShownLines(tv).count == 13; });

    MCGuiStep(@"select all occurrences reaching into a fold opens it", ^{
        [tv.textStorage replaceCharactersInRange:NSMakeRange(0, tv.string.length) withString:original];
        [folds toggleLine:7];
        GCHECK([folds lineStartsFold:7]);
        tv.selectedRange = [tv.string rangeOfString:@"x = 3"];
        tv.selectedRange = NSMakeRange([tv.string rangeOfString:@"return 2"].location, 6);   // "return"
        [tv selectAllOccurrences:nil];
        GCHECK(![folds lineStartsFold:7]);
    });

    MCGuiStep(@"a language server's ranges replace the computed ones while the text stays", ^{
        [folds unfoldAll];
        const uint64_t gen = folds.generation;
        [folds setServerRanges:@[[NSValue valueWithRange:NSMakeRange(1, 3)]] kinds:@[@"region"]
                    generation:gen - 1];   // for an older text: ignored
        GCHECK([folds lineStartsRegion:2] && ![folds lineStartsRegion:1]);
        [folds setServerRanges:@[[NSValue valueWithRange:NSMakeRange(1, 3)],
                                 [NSValue valueWithRange:NSMakeRange(9, 0)]]
                         kinds:@[@"region", @""] generation:gen];
        GCHECK([folds lineStartsRegion:1] && ![folds lineStartsRegion:2] && ![folds lineStartsRegion:9]);
        GCHECK([folds toggleLine:1]);
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 6 7 8 9 10 11 12 13");
        [folds unfoldAll];
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"#" replacementRange:NSMakeRange(0, 0)]; });
    });
    MCGuiWait(@"an edit brings the computed ones back", 3, ^BOOL {
        return [folds lineStartsRegion:2] && ![folds lineStartsRegion:1];
    });
    MCGuiStep(@"undo the edit", ^{ [tv.undoManager undo]; GCHECK_EQ_STR(tv.string, original); });

    MCGuiStep(@"folds stay with their tab", ^{
        [folds unfoldAll];
        [folds toggleLine:2];
        [folds toggleLine:7];
        GCHECK(folds.hasFolds);
        GCHECK([c showFile:cfile]);
        GCHECK(!folds.hasFolds);
    });
    MCGuiWait(@"regions for fold.c", 5, ^BOOL { return [folds lineStartsRegion:0]; });
    MCGuiStep(@"brackets: } else { closes one region and opens the next", ^{
        GCHECK([folds lineStartsRegion:1] && [folds lineStartsRegion:3]);
        [folds toggleLine:1];
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 4 5 6 7 8");
        GCHECK([c showFile:py]);
        GCHECK([folds lineStartsFold:2] && [folds lineStartsFold:7]);
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 2 3 7 8 12 13");
        GCHECK([c showFile:cfile]);
        GCHECK([folds lineStartsFold:1]);
        [folds unfoldAll];
        GCHECK([c showFile:md]);
        // Markdown opens in its preview: to the source.
        if (!tv.editable) [c performSelector:@selector(togglePreview:) withObject:nil];
        GCHECK(tv.editable);
    });
    MCGuiWait(@"regions for fold.md", 5, ^BOOL { return [folds lineStartsRegion:0]; });
    MCGuiStep(@"Markdown: headings", ^{
        GCHECK([folds lineStartsRegion:3] && [folds lineStartsRegion:6]);
        [folds toggleLine:0];
        GCHECK_EQ_STR(Join(ShownLines(tv)), @"1 6 7 8");
        // The preview takes the text away; the source comes back unfolded.
        [c performSelector:@selector(togglePreview:) withObject:nil];
        GCHECK(!tv.editable);
        GCHECK(!folds.hasFolds);
        [c performSelector:@selector(togglePreview:) withObject:nil];
        GCHECK(tv.editable);
    });

}

// Timings on 20,000 lines of Python: `make guitest SUITES=foldperf`, and
// on 100,000 with MC_GUI_FULL_PERF=1 (half of a whole run's time went to
// laying that file out, folded and not, for notes no check reads). Fold
// All, Unfold All and a keystroke among the folds are bounded in the main
// thread's CPU time: one region at a time, Fold All and Unfold All of
// 50,000 took 6 and 18 s, which MC_GUI_FULL_PERF=1 (50,000 folds) catches;
// at 10,000 the bounds catch only worse (Unfold All alone is 0.2 to 1 s,
// more in a whole run than alone).
MC_GUI_SUITE(foldperf) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    __block MCFolds *folds = nil;
    NSString *big = MCGuiProjectFile(@"big.py");
    __block std::chrono::steady_clock::time_point opened;
    const int defs = NSProcessInfo.processInfo.environment[@"MC_GUI_FULL_PERF"].length ? 25000 : 5000;
    const int mid = defs * 2 + 1;   // the line of the middle def, from 1
    MCGuiStep([NSString stringWithFormat:@"a %d-line file", defs * 4], ^{
        NSMutableString *b = [NSMutableString string];
        for (int i = 0; i < defs; i++)
            [b appendFormat:@"def f%d(x):\n    if x:\n        return %d\n\n", i, i];
        MCGuiWriteFile(big, b);
        c = MCGuiController();
        tv = Editor(c);
        folds = tv.folds;
        opened = std::chrono::steady_clock::now();
        GCHECK([c showFile:big]);
        MCGuiNote(@"opened big.py in %.0f ms", Ms(opened));
        opened = std::chrono::steady_clock::now();
    });
    MCGuiWait(@"its regions", 10, ^BOOL { return [folds lineStartsRegion:4 * (defs - 1)]; });
    MCGuiStep(@"fold all, type, scroll, unfold all on it", ^{
        MCGuiNote(@"regions arrived %.0f ms after the file showed", Ms(opened));
        NSLayoutManager *lm = tv.layoutManager;
        auto t = std::chrono::steady_clock::now();
        double cpu = MCGuiThreadCpuMs();
        GCHECK([folds foldAll]);
        cpu = MCGuiThreadCpuMs() - cpu;
        MCGuiNote(@"Fold All (%d folds): %.0f ms (%.0f ms of CPU)", defs * 2, Ms(t), cpu);
        GCHECK_TIME(cpu, defs > 5000 ? 3000 : 1000, @"Fold All");
        t = std::chrono::steady_clock::now();
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        MCGuiNote(@"laying the whole file out folded: %.0f ms", Ms(t));
        const CGFloat folded = NSHeight([lm usedRectForTextContainer:tv.textContainer]);
        GCHECK(folded < defs * 2 * 20);
        // A keystroke on a line between folds, with the view there.
        NSUInteger at = LineStart(tv.string, mid) + 3;   // into the middle def's name
        GCHECK(![folds rangeIsHidden:NSMakeRange(at, 1)]);
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv displayIfNeeded];
        for (int k = 0; k < 3; k++) {
            t = std::chrono::steady_clock::now();
            const double c0 = MCGuiThreadCpuMs();
            MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"z" replacementRange:tv.selectedRange]; });
            const double typed = Ms(t), typedCpu = MCGuiThreadCpuMs() - c0;
            t = std::chrono::steady_clock::now();
            [tv displayIfNeeded];
            MCGuiNote(@"a keystroke among %d folds: %.2f ms (%.2f ms of CPU), drawing %.2f ms",
                      defs * 2, typed, typedCpu, Ms(t));
            GCHECK_TIME(typedCpu, 50, @"a keystroke among the folds");
        }
        t = std::chrono::steady_clock::now();
        [tv scrollRangeToVisible:NSMakeRange(tv.string.length - 1, 0)];
        [tv displayIfNeeded];
        MCGuiNote(@"scrolling to the end and drawing: %.2f ms", Ms(t));
        t = std::chrono::steady_clock::now();
        cpu = MCGuiThreadCpuMs();
        GCHECK([folds unfoldAll]);
        cpu = MCGuiThreadCpuMs() - cpu;
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        MCGuiNote(@"Unfold All (%.0f ms of CPU) and laying the whole file out: %.0f ms", cpu, Ms(t));
        GCHECK_TIME(cpu, defs > 5000 ? 6000 : 3000, @"Unfold All");
        GCHECK(NSHeight([lm usedRectForTextContainer:tv.textContainer]) > folded * 1.5);
        for (int k = 0; k < 3; k++) [tv.undoManager undo];
    });
    MCGuiStep(@"typing with one fold above, and with none", ^{
        GCHECK([folds toggleLine:4]);
        NSUInteger at = LineStart(tv.string, mid) + 3;
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv displayIfNeeded];
        for (int k = 0; k < 2; k++) {
            auto t = std::chrono::steady_clock::now();
            MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"z" replacementRange:tv.selectedRange]; });
            [tv displayIfNeeded];
            MCGuiNote(@"a keystroke with one fold above: %.2f ms", Ms(t));
        }
        GCHECK([folds lineStartsFold:4]);
        [folds unfoldAll];
        for (int k = 0; k < 2; k++) {
            auto t = std::chrono::steady_clock::now();
            MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"z" replacementRange:tv.selectedRange]; });
            [tv displayIfNeeded];
            MCGuiNote(@"a keystroke with no folds: %.2f ms", Ms(t));
        }
        for (int k = 0; k < 4; k++) [tv.undoManager undo];
    });
}
