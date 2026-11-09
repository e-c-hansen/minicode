// KeystrokeSuite.mm — `make guitest SUITES=keystroke`: what a typed
// character costs in a 100,000-line Python file, with the highlighting
// after it checked against a full re-highlight, in a fresh window and
// again once the window has shown a Markdown page and another long file,
// after Select All, and after a Replace All and its undo.
// Set MC_GUI_SAMPLE=<file> to run /usr/bin/sample on the process while it
// types (<file>.used and <file>.replaced for the later typings).
#import "Harness.h"
#import "ControlServer.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "LayoutCatchUp.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import "SourceStorage.h"
#include <mach/mach_time.h>
#include <random>
#include <vector>

namespace {

double Ms() {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

// Every foreground color run of the storage, as [start, length, color].
// Each bracket is a run of its own whose color is left out: bracket-pair
// colors are painted on the lines on screen and as lines come into view
// (EditorController+Highlighting.mm), so a full pass and the edits before
// it differ there by design; bracket_pairs checks those colors.
NSArray<NSArray *> *ColorRuns(NSTextStorage *ts) {
    NSMutableArray *out = [NSMutableArray array];
    NSString *s = ts.string;
    NSCharacterSet *brackets = [NSCharacterSet characterSetWithCharactersInString:@"()[]{}"];
    __block NSUInteger runStart = 0;
    __block id runColor = nil;
    void (^piece)(NSUInteger, id) = ^(NSUInteger at, id color) {
        if (runColor && ![color isEqual:@"bracket"] && [runColor isEqual:color]) return;
        if (runColor) [out addObject:@[@(runStart), @(at - runStart), runColor]];
        runStart = at;
        runColor = color;
    };
    [ts enumerateAttribute:NSForegroundColorAttributeName inRange:NSMakeRange(0, ts.length)
                   options:0 usingBlock:^(NSColor *c, NSRange r, BOOL *stop) {
        (void)stop;
        id color = c ?: (id)NSNull.null;
        NSUInteger at = r.location;
        while (at < NSMaxRange(r)) {
            const NSRange b = [s rangeOfCharacterFromSet:brackets options:0
                                                   range:NSMakeRange(at, NSMaxRange(r) - at)];
            const NSUInteger to = b.location == NSNotFound ? NSMaxRange(r) : b.location;
            if (to > at) piece(at, color);
            if (b.location == NSNotFound) break;
            piece(b.location, @"bracket");
            at = b.location + 1;
        }
    }];
    if (runColor) [out addObject:@[@(runStart), @(s.length - runStart), runColor]];
    return out;
}

// Types `text` a character at a time, each its own undo group; returns
// each keystroke's main-thread CPU time in ms (its wall time into `wall`).
// A keystroke's work is all on the main thread, so its CPU time is what
// it costs; wall time on a loaded machine also counts the time the thread
// waited for a core.
std::vector<double> TypeChars(NSTextView *tv, NSString *text, std::vector<double> *wall = nullptr) {
    std::vector<double> times;
    for (NSUInteger i = 0; i < text.length; i++) {
        NSString *ch = [text substringWithRange:NSMakeRange(i, 1)];
        const double t0 = Ms(), c0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:ch replacementRange:tv.selectedRange];
        });
        times.push_back(MCGuiThreadCpuMs() - c0);
        if (wall) wall->push_back(Ms() - t0);
    }
    return times;
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double P95(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() * 95 / 100];
}

// MC_GUI_SAMPLE=<file>: /usr/bin/sample on the process for six seconds
// into <file><suffix>; YES when it runs (the typing then lasts longer).
BOOL SampleInto(NSString *suffix) {
    NSString *out = NSProcessInfo.processInfo.environment[@"MC_GUI_SAMPLE"];
    if (!out.length) return NO;
    [NSTask launchedTaskWithLaunchPath:@"/usr/bin/sample"
                             arguments:@[[NSString stringWithFormat:@"%d", getpid()], @"6", @"1",
                                         @"-file", [out stringByAppendingString:suffix]]];
    [NSThread sleepForTimeInterval:0.5];
    return YES;
}

// The storage's paragraph style answer at `at` (SourceStorage.h: a run kept
// through edits) against what is stored there and over the range it gives.
BOOL StyleAnswerHolds(MCSourceStorage *st, NSUInteger at) {
    NSRange r = NSMakeRange(NSNotFound, 0);
    id v = [st attribute:NSParagraphStyleAttributeName atIndex:at effectiveRange:&r];
    if (!NSLocationInRange(at, r) || NSMaxRange(r) > st.length) return NO;
    for (NSUInteger i = r.location; i < NSMaxRange(r); i++) {
        id stored = [st storedAttributesAtIndex:i longestEffectiveRange:NULL
                                        inRange:NSMakeRange(0, st.length)][NSParagraphStyleAttributeName];
        if (stored != v && ![stored isEqual:v]) return NO;
    }
    return YES;
}

// Random edits of every kind to a storage, each at the ends of the run
// just asked for as often as anywhere; the answer checked around the edit
// and the run's ends, before fixing (inside the editing session) and
// after. Returns the wrong answers; `asked` gets how many were checked.
int StyleRunFuzz(int ops, int *asked) {
    NSMutableParagraphStyle *p1 = [NSMutableParagraphStyle new], *p2 = [NSMutableParagraphStyle new];
    p1.lineSpacing = 2;
    p2.lineSpacing = 2;   // equal to p1, another object
    NSMutableParagraphStyle *p3 = [NSMutableParagraphStyle new];
    p3.headIndent = 24;
    NSArray *styles = @[p1, p2, p3, NSNull.null];   // NSNull: none
    NSDictionary *(^attrsWith)(id) = ^NSDictionary *(id ps) {
        return ps == NSNull.null ? @{NSForegroundColorAttributeName: NSColor.redColor}
                                 : @{NSParagraphStyleAttributeName: ps,
                                     NSForegroundColorAttributeName: NSColor.blueColor};
    };
    NSMutableString *start = [NSMutableString string];
    for (int i = 0; i < 12; i++) [start appendString:@"line of text\n"];
    MCSourceStorage *st = [[MCSourceStorage alloc] initWithString:start attributes:attrsWith(p1)];
    std::mt19937 rng(20261108);
    auto pick = [&](NSUInteger n) { return n ? (NSUInteger)(rng() % n) : 0; };
    NSArray *pieces = @[@"", @"x", @"ab", @"\n", @"word\nnext", @"  "];
    auto text = [&]() -> NSString * { return pieces[rng() % pieces.count]; };
    int wrong = 0;
    *asked = 0;
    for (int op = 0; op < ops; op++) {
        NSRange known = NSMakeRange(0, 0);
        if (st.length)
            [st attribute:NSParagraphStyleAttributeName atIndex:pick(st.length) effectiveRange:&known];
        const NSUInteger len = st.length;
        NSUInteger loc = pick(len + 1);
        const unsigned where = rng() % 6;
        if (where == 0) loc = known.location;
        else if (where == 1) loc = NSMaxRange(known);
        else if (where == 2 && known.location) loc = known.location - 1;
        const NSUInteger span = pick(12);
        NSRange r = NSMakeRange(loc, MIN(span, len - loc));
        if (rng() % 3 == 0) r.length = 0;
        id ps = styles[rng() % styles.count];
        NSUInteger made = r.length;   // characters where r was, after the edit
        [st beginEditing];
        switch (rng() % 9) {
        case 0: case 1: case 2: {
            NSString *t = text();
            made = t.length;
            [st replaceCharactersInRange:r withString:t];
            break;
        }
        case 3: {   // one style, or two
            NSMutableAttributedString *a =
                [[NSMutableAttributedString alloc] initWithString:text() attributes:attrsWith(ps)];
            if (rng() % 2)
                [a appendAttributedString:[[NSAttributedString alloc]
                    initWithString:text() attributes:attrsWith(styles[rng() % styles.count])]];
            made = a.length;
            [st replaceCharactersInRange:r withAttributedString:a];
            break;
        }
        case 4: [st setAttributes:attrsWith(ps) range:r]; break;
        case 5:
            if (ps != NSNull.null) [st addAttribute:NSParagraphStyleAttributeName value:ps range:r];
            break;
        case 6: [st addAttributes:attrsWith(ps) range:r]; break;
        case 7: [st removeAttribute:NSParagraphStyleAttributeName range:r]; break;
        case 8: [st addAttribute:NSForegroundColorAttributeName value:NSColor.greenColor range:r]; break;
        }
        const NSInteger shift = (NSInteger)made - (NSInteger)r.length;
        const NSInteger a = (NSInteger)known.location, b = (NSInteger)NSMaxRange(known);
        const NSInteger probes[] = {(NSInteger)loc - 1, (NSInteger)loc, (NSInteger)(loc + made) - 1,
                                    (NSInteger)(loc + made), a, a + shift, b - 1, b + shift - 1,
                                    b + shift, (NSInteger)pick(st.length)};
        const NSUInteger first = pick(10);   // the first probe meets the run the edit left
        for (int pass = 0; pass < 2; pass++) {
            for (NSUInteger k = 0; k < 10; k++) {
                const NSInteger p = probes[(first + k) % 10];
                if (p < 0 || (NSUInteger)p >= st.length) continue;
                (*asked)++;
                if (!StyleAnswerHolds(st, (NSUInteger)p)) wrong++;
            }
            if (pass == 0) [st endEditing];   // fixing makes each paragraph one style
        }
        if (op % 500 == 499)
            [st setAttributedString:[[NSAttributedString alloc]
                initWithString:start attributes:attrsWith(styles[rng() % styles.count])]];
    }
    return wrong;
}

} // namespace

MC_GUI_SUITE(keystroke) {
    __block EditorController *c = nil;
    __block NSTextView *tv = nil;
    __block NSUInteger at = 0;
    __block std::vector<double> fresh;   // the first typing's CPU times
    // A window of its own on a folder of its own, as large_file's, so the
    // first typing is a fresh window's; the used window is made on purpose
    // below. (In a whole run the shared window had shown Markdown pages,
    // whose text blocks made each keystroke here cost 5 to 10 ms of CPU
    // against 0.5 ms: forgetTextBlockLayout in EditorController+Highlighting.mm.)
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"keystroke"];
    NSString *path = [dir stringByAppendingPathComponent:@"big100k.py"];
    // The paragraph style by name comes from a run the storage keeps
    // through edits (SourceStorage.h). Each wrong turn of keeping it (a
    // run not moved for text put in before it, plain text taking the
    // wrong neighbour's style, a restyle or attributed text of another
    // style not forgetting it) made 100 to 4,000 wrong answers here.
    MCGuiStep(@"the storage's paragraph style answer follows every kind of edit", ^{
        int asked = 0;
        const int wrong = StyleRunFuzz(3000, &asked);
        MCGuiNote(@"%d answers checked through 3,000 edits, %d wrong", asked, wrong);
        GCHECK(wrong == 0);
        GCHECK(asked > 30000);
    });
    MCGuiStep(@"a 100,000-line Python file", ^{
        NSMutableString *s = [NSMutableString string];
        for (int i = 0; i < 100000 / 5; i++)
            [s appendFormat:@"def f%d(x, y=\"s%d\"):  # a comment %d\n"
                            @"    '''doc'''\n"
                            @"    return x + %d * y\n"
                            @"\n"
                            @"value_%d = [1, 2.5, 'three', None]\n", i, i, i, i, i];
        MCGuiWriteFile(path, s);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        for (EditorController *w in MCGuiControllers())
            if ([w.rootPath isEqualToString:dir]) c = w;
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
        [c focusEditor:nil];
        double t0 = Ms();
        GCHECK([c showFile:path]);
        MCGuiNote(@"opening it: %.0f ms", Ms() - t0);
        tv = [c valueForKey:@"textView"];
        GCHECK([tv.string hasPrefix:@"def f0"]);
        NSRange mid = [tv.string rangeOfString:@"return x + 10000 *"];
        GCHECK(mid.location != NSNotFound);
        at = mid.location + 10;
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 2000)];
    });
    MCGuiStep(@"typing 200 characters mid-file", ^{
        const BOOL sampling = SampleInto(@"");
        NSString *typed = [@"" stringByPaddingToLength:sampling ? 2000 : 200 withString:@"ab cd "
                                       startingAtIndex:0];
        std::vector<double> wall;
        std::vector<double> times = TypeChars(tv, typed, &wall);
        fresh = times;
        const double med = Median(times);
        double total = 0;
        for (double t : times) total += t;
        MCGuiNote(@"%lu keystrokes in a 100,000-line file: median %.2f ms of CPU, mean %.2f ms "
                  @"(wall: median %.2f ms)",
                  (unsigned long)times.size(), med, total / times.size(), Median(wall));
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
        // Bounds on the main thread's CPU time (about 2.5 ms a key on an
        // M3), tight enough to catch a keystroke that relays the file out
        // or re-highlights it whole (a full pass here is hundreds of ms).
        std::vector<double> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        const double p95 = sorted.empty() ? 0 : sorted[sorted.size() * 95 / 100];
        if (!sampling) {
            GCHECK_TIME(med, 20, @"the median keystroke");
            GCHECK_TIME(p95, 60, @"the 95th percentile keystroke");
        }
    });
    MCGuiStep(@"the colors equal a full re-highlight", ^{
        NSArray *incremental = ColorRuns(tv.textStorage);
        double t0 = Ms();
        [c performSelector:@selector(applyHighlighting)];
        MCGuiNote(@"a full re-highlight: %.0f ms", Ms() - t0);
        NSArray *full = ColorRuns(tv.textStorage);
        GCHECK(incremental.count == full.count);
        GCHECK([incremental isEqualToArray:full]);
        MCGuiNote(@"%lu color runs", (unsigned long)full.count);
        const double c0 = Ms();
        NSString *copy = [tv.string copy];
        MCGuiNote(@"the storage's string is a %@; a copy of it took %.3f ms",
                  NSStringFromClass([tv.string class]), Ms() - c0);
        (void)copy;
    });
    MCGuiStep(@"a comment typed, then undone, colors as a full pass", ^{
        tv.selectedRange = NSMakeRange(at, 0);
        TypeChars(tv, @"# x");
        NSArray *incremental = ColorRuns(tv.textStorage);
        [c performSelector:@selector(applyHighlighting)];
        GCHECK([incremental isEqualToArray:ColorRuns(tv.textStorage)]);
        for (int i = 0; i < 3; i++) [tv.undoManager undo];
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
        incremental = ColorRuns(tv.textStorage);
        [c performSelector:@selector(applyHighlighting)];
        GCHECK([incremental isEqualToArray:ColorRuns(tv.textStorage)]);
    });
    MCGuiStep(@"typing at three carets", ^{
        CodeTextView *ctv = (CodeTextView *)tv;
        NSRange l1 = [tv.string rangeOfString:@"return x + 15000 *"];
        NSRange l2 = [tv.string rangeOfString:@"return x + 15001 *"];
        NSRange l3 = [tv.string rangeOfString:@"return x + 15002 *"];
        [ctv.multiCarets setCaretRanges:@[[NSValue valueWithRange:NSMakeRange(l1.location, 0)],
                                          [NSValue valueWithRange:NSMakeRange(l2.location, 0)],
                                          [NSValue valueWithRange:NSMakeRange(l3.location, 0)]]];
        // Laid out first: the time is the keystroke's, not the layout of
        // the 25,000 lines between here and the last place laid out.
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(l3.location, 100)];
        const double t0 = Ms(), c0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{ [ctv.multiCarets insertText:@"\""]; });
        const double quote = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"a quote at three carets: %.2f ms of CPU (%.2f ms wall)", quote, Ms() - t0);
        GCHECK_TIME(quote, 100, @"a quote at three carets");
        GCHECK([[tv.string substringWithRange:NSMakeRange(l1.location, 7)] isEqualToString:@"\"return"]);
        NSArray *incremental = ColorRuns(tv.textStorage);
        [ctv.multiCarets clear];
        [c performSelector:@selector(applyHighlighting)];
        GCHECK([incremental isEqualToArray:ColorRuns(tv.textStorage)]);
        [tv.undoManager undo];
        GCHECK([tv.string rangeOfString:@"\"return x + 15000"].location == NSNotFound);
    });
    // A squiggle's mark follows typing before it (the view watches the
    // editor's own storage), and a caret set elsewhere ends several carets.
    MCGuiStep(@"a diagnostic mark moves with typing before it", ^{
        CodeTextView *ctv = (CodeTextView *)tv;
        MCDiagnosticMark *m = [MCDiagnosticMark new];
        m.range = NSMakeRange(at + 20, 4);
        m.severity = 1;
        m.message = @"a test problem";
        ctv.diagnostics = @[m];
        tv.selectedRange = NSMakeRange(at, 0);
        TypeChars(tv, @"xyz");
        MCDiagnosticMark *moved = ctv.diagnostics.firstObject;
        GCHECK(moved && moved.range.location == at + 23 && moved.range.length == 4);
        for (int i = 0; i < 3; i++) [tv.undoManager undo];
        ctv.diagnostics = @[];
    });
    // A selection reaching text not laid out leaves TextKit a display
    // invalidation it asks about again on every keystroke, looking up the
    // paragraph style at its far end, the end of the text. Through the
    // store's runs that walked every run from the caret to the end and
    // back, and each key cost three times what it cost before (1.1 to 1.6
    // ms against 0.35 to 0.5 here; SourceStorage.h). The lookups are
    // checked (8.7 to 30 ms of CPU for these 200 before); the typing is
    // only noted, since TextKit's own work on the pending range still
    // costs 1.4 to 2.2 times a fresh window's keystroke, too close to a
    // bound the old cost would break.
    MCGuiStep(@"after Select All the paragraph style is found without walking the runs", ^{
        NSTextStorage *ts = tv.textStorage;
        const double c0 = MCGuiThreadCpuMs();
        for (int i = 0; i < 100; i++) {
            [ts attribute:NSParagraphStyleAttributeName atIndex:at effectiveRange:NULL];
            [ts attribute:NSParagraphStyleAttributeName atIndex:ts.length - 1 effectiveRange:NULL];
        }
        const double lookups = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"200 paragraph style lookups between the caret and the end: %.2f ms of CPU",
                  lookups);
        GCHECK_TIME(lookups, 1, @"200 paragraph style lookups far apart");
        [tv selectAll:nil];
        [tv displayIfNeeded];
        tv.selectedRange = NSMakeRange(at, 0);   // a click
        [tv displayIfNeeded];
        std::vector<double> times = TypeChars(tv, [@"" stringByPaddingToLength:200
                                                                    withString:@"xy zw "
                                                               startingAtIndex:0]);
        const double med = Median(times), freshMed = Median(fresh);
        MCGuiNote(@"200 keystrokes after Select All: median %.2f ms of CPU (fresh: %.2f)", med,
                  freshMed);
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"xy zw "]);
        for (NSUInteger i = 0; i < times.size(); i++) [tv.undoManager undo];
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 2000)];
    });
    // Fixing attributes still happens: a character the editor's font lacks
    // gets a font that has it, typed or highlighted again.
    MCGuiStep(@"an emoji and a CJK character get fonts that have them", ^{
        tv.selectedRange = NSMakeRange(at, 0);
        TypeChars(tv, @"中");
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"😀" replacementRange:tv.selectedRange]; });
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 10)];
        NSFont *cjk = [tv.textStorage attribute:NSFontAttributeName atIndex:at effectiveRange:NULL];
        NSFont *emoji = [tv.textStorage attribute:NSFontAttributeName atIndex:at + 1 effectiveRange:NULL];
        NSFont *ascii = [tv.textStorage attribute:NSFontAttributeName atIndex:at + 3 effectiveRange:NULL];
        MCGuiNote(@"fonts: %@, %@, %@", cjk.fontName, emoji.fontName, ascii.fontName);
        GCHECK(![cjk.fontName isEqualToString:ascii.fontName]);
        GCHECK([emoji.fontName containsString:@"Emoji"]);
        [c performSelector:@selector(applyHighlighting)];
        emoji = [tv.textStorage attribute:NSFontAttributeName atIndex:at + 1 effectiveRange:NULL];
        GCHECK([emoji.fontName containsString:@"Emoji"]);
        for (int i = 0; i < 2; i++) [tv.undoManager undo];
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
    });
    // The window used as a day's work uses it: a Markdown page shown in the
    // editor's view (quotes, code boxes, h1 and h2 rules, and a table are
    // text blocks), and another 100,000-line file laid out to its end.
    // Then the first file again, typed in where the fresh window typed.
    // The block table a page left in the layout manager made TextKit
    // recalculate the used rect over every laid-out line on each keystroke
    // (1.5 ms against 0.3 here, _recalculateUsageForTextContainerAtIndex:
    // in a sample), until forgetTextBlockLayout reset it.
    NSString *page = [dir stringByAppendingPathComponent:@"page.md"];
    NSString *other = [dir stringByAppendingPathComponent:@"other100k.py"];
    // Where the first file's text was, laid out from the top (exact), for
    // when it shows again.
    __block NSRect exactAt = NSZeroRect;
    __block NSUInteger topBefore = 0;
    __block CGFloat topOffsetBefore = 0;
    NSUInteger (^topOfPane)(CGFloat *) = ^NSUInteger(CGFloat *offset) {
        const NSRect visible = tv.visibleRect;
        NSUInteger top = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)];
        top = [tv.string lineRangeForRange:NSMakeRange(MIN(top, tv.string.length - 1), 0)].location;
        if (offset) *offset = NSMinY([c textRectForCharacter:top]) - NSMinY(visible);
        return top;
    };
    // What contiguous layout does with a new line put in above the pane
    // (where the text in the pane goes), for the same edit made later in
    // the tab shown again.
    __block CGFloat contiguousTopShift = 0, contiguousCaretShift = 0;
    MCGuiStep(@"contiguous layout: a new line above the pane", ^{
        GCHECK(!tv.layoutManager.allowsNonContiguousLayout && !tv.layoutManager.hasNonContiguousLayout);
        exactAt = [c textRectForCharacter:at];
        topBefore = topOfPane(&topOffsetBefore);
        GCHECK(topBefore > 100000);   // far down: laying out from the top would cost seconds
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK([(CodeTextView *)tv replaceRanges:@[[NSValue valueWithRange:NSMakeRange(0, 0)]]
                                         withStrings:@[@"\n"]
                                          actionName:@"New Line"]);
        });
        [tv displayIfNeeded];
        contiguousTopShift =
            NSMinY([c textRectForCharacter:topBefore + 1]) - NSMinY(tv.visibleRect) - topOffsetBefore;
        contiguousCaretShift = NSMinY([c textRectForCharacter:at + 1]) - NSMinY(exactAt);
        MCGuiNote(@"contiguous layout: the pane's text moved %.1f, the caret's line %.1f",
                  contiguousTopShift, contiguousCaretShift);
        [tv.undoManager undo];   // which shows the line it took away, at the top
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
        GCHECK(NSEqualRects([c textRectForCharacter:at], exactAt));
        tv.selectedRange = NSMakeRange(at, 0);
        [c scrollEditorToCharacter:topBefore offset:topOffsetBefore];
        CGFloat offset = 0;
        GCHECK(topOfPane(&offset) == topBefore && fabs(offset - topOffsetBefore) < 0.01);
    });
    MCGuiStep(@"the window used: a Markdown page, then another long file laid out", ^{
        MCGuiWriteFile(page, @"# Title\n\n## Part\n\n> a quote\n\n```\ncode\n```\n\n---\n\n"
                             @"| a | b |\n|---|---|\n| 1 | 2 |\n");
        GCHECK([c showFile:page]);
        GCHECK(!tv.editable);   // its preview
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        NSMutableString *s = [NSMutableString string];
        for (int i = 0; i < 100000 / 4; i++)
            [s appendFormat:@"def g%d(x):\n    if x:\n        return %d\n\n", i, i];
        MCGuiWriteFile(other, s);
        GCHECK([c showFile:other]);
        GCHECK(tv.editable && [tv.string hasPrefix:@"def g0"]);
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        // Showing a 100,000-line tab again costs about what showing the
        // other one cost with nothing far down to lay out: the text, its
        // colors and a screen at the place. Laying out the 50,000 lines
        // above the place took 1.5 to 3.9 s here (editor N2).
        GCHECK([c showFile:page]);
        double c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:other]);
        const double baseline = MCGuiThreadCpuMs() - c0;
        c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:path]);
        const double back = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"back to the first file: %.0f ms of CPU (the other file from the top: %.0f ms)",
                  back, baseline);
        GCHECK_TIME(back, MAX(500.0, 2.5 * baseline), @"showing a long tab again far down");
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
        CGFloat offset = 0;
        GCHECK(topOfPane(&offset) == topBefore);
        GCHECK(fabs(offset - topOffsetBefore) < 1);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(at, 0)));
    });
    // The lines above the place are laid out in the background, a few
    // milliseconds a turn, with the text on screen kept where it is; then
    // every rect is the one laid out from the top gave before the switch.
    __block MCLayoutCatchUp *catchUp = nil;
    MCGuiStep(@"typing while the lines above catch up", ^{
        catchUp = [c valueForKey:@"layoutCatchUp"];
        GCHECK(catchUp.watching && catchUp.running && tv.layoutManager.allowsNonContiguousLayout);
        TypeChars(tv, @"q");
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 7)] isEqualToString:@"qab cd "]);
        [tv.undoManager undo];
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
    });
    // Laid out from the top of the text through the pane.
    BOOL (^allLaidOut)(void) = ^BOOL {
        if (catchUp.running) return NO;
        NSUInteger first = 0, glyph = 0;
        [tv.layoutManager getFirstUnlaidCharacterIndex:&first glyphIndex:&glyph];
        return first > NSMaxRange([tv.layoutManager characterRangeForGlyphRange:
            [tv.layoutManager glyphRangeForBoundingRectWithoutAdditionalLayout:tv.visibleRect
                                                               inTextContainer:tv.textContainer]
                                                             actualGlyphRange:NULL]) - 1;
    };
    MCGuiWait(@"the lines above the pane are laid out", 120, allLaidOut);
    MCGuiStep(@"laid out: the rects are exact and the place kept", ^{
        MCGuiNote(@"caught up in %lu pieces, %.0f ms of CPU, the longest %.1f ms",
                  (unsigned long)catchUp.pieces, catchUp.totalMilliseconds,
                  catchUp.longestPieceMilliseconds);
        GCHECK(catchUp.watching && catchUp.pieces > 10);
        // Each piece is sized to about 5 ms from what the last one cost.
        GCHECK_TIME(catchUp.longestPieceMilliseconds, 50, @"the longest piece of the catch-up");
        const NSRect now = [c textRectForCharacter:at];
        MCGuiNote(@"the caret's rect: %@ (laid out from the top before: %@)",
                  NSStringFromRect(now), NSStringFromRect(exactAt));
        GCHECK(NSEqualRects(now, exactAt));
        CGFloat offset = 0;
        GCHECK(topOfPane(&offset) == topBefore);
        GCHECK(fabs(offset - topOffsetBefore) < 1);
    });
    // An edit above the pane leaves what follows it unlaid, which
    // non-contiguous layout alone would estimate (the pane's text moved 4
    // points up, not a line down, and stayed there): it is laid out again
    // as the edit ends, so the text below moves exactly as contiguous
    // layout moved it in the fresh window.
    __block double spentBefore = 0;
    // Where the pane's top line (as it was before the edit) is, from the
    // pane's top, and the caret's line, both exact.
    void (^checkPushedDown)(NSString *) = ^(NSString *when) {
        const CGFloat top = NSMinY([c textRectForCharacter:topBefore + 1]) - NSMinY(tv.visibleRect);
        const NSRect caret = [c textRectForCharacter:at + 1];
        MCGuiNote(@"%@: the pane's text moved %.1f, the caret's line %.1f", when,
                  top - topOffsetBefore, NSMinY(caret) - NSMinY(exactAt));
        GCHECK(fabs(top - topOffsetBefore - contiguousTopShift) < 0.01);
        GCHECK(fabs(NSMinY(caret) - NSMinY(exactAt) - contiguousCaretShift) < 0.01);
    };
    MCGuiStep(@"a new line above the pane moves the text as contiguous layout does", ^{
        GCHECK(contiguousCaretShift > 5);   // a line
        spentBefore = catchUp.totalMilliseconds;
        // A programmatic edit, which leaves the scroll alone (typing would
        // scroll to the caret).
        GCHECK([(CodeTextView *)tv replaceRanges:@[[NSValue valueWithRange:NSMakeRange(0, 0)]]
                                     withStrings:@[@"\n"]
                                      actionName:@"New Line"]);
        GCHECK(tv.selectedRange.location == at + 1);
        MCGuiNote(@"laid out again in %.1f ms of CPU", catchUp.totalMilliseconds - spentBefore);
        GCHECK(allLaidOut());
        checkPushedDown(@"after the edit");
        [tv displayIfNeeded];
        checkPushedDown(@"drawn");
    });
    // Negative: nothing moves it later (the pieces have nothing to do).
    MCGuiPause(0.6);
    MCGuiStep(@"and stays there", ^{
        GCHECK(allLaidOut());
        checkPushedDown(@"a moment later");
        [tv.undoManager undo];
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
        GCHECK(allLaidOut());
        GCHECK(NSEqualRects([c textRectForCharacter:at], exactAt));
        // Back to the place, as the tab's own place, Go Back or a search
        // hit would: laid out there first, then above it.
        [c scrollEditorToCharacter:topBefore offset:topOffsetBefore];
    });
    MCGuiWait(@"laid out again after going back", 30, allLaidOut);
    MCGuiStep(@"and exactly where it was", ^{
        GCHECK(NSEqualRects([c textRectForCharacter:at], exactAt));
        CGFloat offset = 0;
        GCHECK(topOfPane(&offset) == topBefore);
        GCHECK(fabs(offset - topOffsetBefore) < 1);
    });
    // 200 keystrokes where the fresh window typed, against its times (the
    // bound about twice them, measured in this run so it holds on any
    // core; with the block table the median was five times them), then
    // undone. MC_GUI_SAMPLE samples it into <file><suffix>.
    void (^typeAsFresh)(NSString *, NSString *) = ^(NSString *suffix, NSString *where) {
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 2000)];
        const BOOL sampling = SampleInto(suffix);
        NSString *typed = [@"" stringByPaddingToLength:sampling ? 2000 : 200 withString:@"xy zw "
                                       startingAtIndex:0];
        std::vector<double> times = TypeChars(tv, typed);
        const double med = Median(times), p95 = P95(times);
        const double freshMed = Median(fresh), freshP95 = P95(fresh);
        MCGuiNote(@"%lu keystrokes %@: median %.2f ms of CPU, 95th percentile %.2f ms (fresh: "
                  @"%.2f and %.2f)", (unsigned long)times.size(), where, med, p95, freshMed, freshP95);
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"xy zw "]);
        if (!sampling) {
            GCHECK_TIME(med, MAX(1.0, 2.5 * freshMed),
                        ([NSString stringWithFormat:@"the median keystroke %@", where]));
            GCHECK_TIME(p95, MAX(3.0, 3 * freshP95),
                        ([NSString stringWithFormat:@"the 95th percentile %@", where]));
        }
        for (NSUInteger i = 0; i < times.size(); i++) [tv.undoManager undo];
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
    };
    MCGuiStep(@"typing in the used window costs what it cost in a fresh one", ^{
        GCHECK(catchUp.watching && tv.layoutManager.allowsNonContiguousLayout);
        typeAsFresh(@".used", @"in the used window");
    });
    // An edit of many ranges goes back to contiguous layout first: with
    // non-contiguous layout Replace All of 20,000 matches took 4.3 s of CPU
    // against 0.6 (multiedit's case), TextKit looking up the attributes at
    // the pane after each storage edit across the color runs between.
    MCGuiStep(@"Replace All of 20,000 matches goes back to contiguous layout", ^{
        GCHECK(catchUp.watching && tv.layoutManager.allowsNonContiguousLayout);
        NSString *s = tv.string;
        NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
        NSMutableArray<NSString *> *strings = [NSMutableArray array];
        for (NSRange r = [s rangeOfString:@"None"]; r.location != NSNotFound;
             r = [s rangeOfString:@"None" options:0
                            range:NSMakeRange(NSMaxRange(r), s.length - NSMaxRange(r))]) {
            [ranges addObject:[NSValue valueWithRange:r]];
            [strings addObject:@"Nada"];
        }
        GCHECK(ranges.count == 20000);
        const double c0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK([(CodeTextView *)tv replaceRanges:ranges withStrings:strings actionName:@"Replace All"]);
        });
        const double cpu = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"Replace All of 20,000 matches in the tab shown again: %.0f ms of CPU", cpu);
        GCHECK(!catchUp.watching && !tv.layoutManager.allowsNonContiguousLayout);
        GCHECK([tv.string rangeOfString:@"None"].location == NSNotFound);
        [tv.undoManager undo];
        GCHECK([tv.string rangeOfString:@"Nada"].location == NSNotFound);
        GCHECK(NSEqualRects([c textRectForCharacter:at], exactAt));   // contiguous: exact
        // The same Replace All again, now that the tab is contiguous, is the
        // baseline, measured on this thread in this step: a slow runner
        // (4.6 s on CI's macOS runner, 1.7 s on a quiet M3) slows both. With
        // the switch to contiguous layout it costs five to seven times the
        // baseline (going contiguous lays the text out from the top); kept
        // non-contiguous it took 15.5 s, about twenty-five times it.
        const double b0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK([(CodeTextView *)tv replaceRanges:ranges withStrings:strings actionName:@"Replace All"]);
        });
        const double base = MCGuiThreadCpuMs() - b0;
        MCGuiNote(@"the same Replace All in the contiguous tab: %.0f ms of CPU", base);
        GCHECK_TIME(cpu, MAX(4000.0, 10 * base),
                    @"Replace All of 20,000 matches in a tab shown again far down");
        [tv.undoManager undo];
        GCHECK([tv.string rangeOfString:@"Nada"].location == NSNotFound);
    });
    // The undo selected every piece it put back, text not laid out
    // included, which left each later keystroke three times the fresh
    // window's (SourceStorage.h), 2.3 ms against 0.6 on CI.
    MCGuiStep(@"typing after Replace All and its undo costs what it cost in a fresh window", ^{
        GCHECK(!tv.layoutManager.allowsNonContiguousLayout);
        typeAsFresh(@".replaced", @"after Replace All and its undo");
    });
    MCGuiStep(@"the suite's window closes unasked, and its file goes", ^{
        for (MCTab *t in [(NSArray *)[c valueForKey:@"tabs"] copy]) [c closeTab:t prompt:NO];
        [c.window close];
        c = nil;
        tv = nil;
        MCGuiSetKeyWindow(nil);
        [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
    });
}

