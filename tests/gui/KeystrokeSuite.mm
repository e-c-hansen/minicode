// KeystrokeSuite.mm — `make guitest SUITES=keystroke`: what a typed
// character costs in a 100,000-line Python file, with the highlighting
// after it checked against a full re-highlight. Set MC_GUI_SAMPLE=<file>
// to run /usr/bin/sample on the process while it types.
#import "Harness.h"
#import "ControlServer.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#include <mach/mach_time.h>
#include <vector>

namespace {

double Ms() {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

// Every foreground color run of the storage, as [start, length, color].
NSArray<NSArray *> *ColorRuns(NSTextStorage *ts) {
    NSMutableArray *out = [NSMutableArray array];
    [ts enumerateAttribute:NSForegroundColorAttributeName inRange:NSMakeRange(0, ts.length)
                   options:0 usingBlock:^(NSColor *c, NSRange r, BOOL *stop) {
        (void)stop;
        [out addObject:@[@(r.location), @(r.length), c ?: (id)NSNull.null]];
    }];
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

} // namespace

MC_GUI_SUITE(keystroke) {
    __block EditorController *c = nil;
    __block NSTextView *tv = nil;
    __block NSUInteger at = 0;
    // A window of its own on a folder of its own, as large_file's: in a
    // whole run the shared window's state from earlier suites made each
    // keystroke here cost 5 to 10 ms of CPU against 0.5 ms alone (after the
    // folding suite every keystroke recalculated the layout manager's used
    // rect over the laid-out lines, _enableTextViewResizing in a sample).
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"keystroke"];
    NSString *path = [dir stringByAppendingPathComponent:@"big100k.py"];
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
        __block BOOL sampling = NO;
        NSString *sampleOut = NSProcessInfo.processInfo.environment[@"MC_GUI_SAMPLE"];
        NSString *typed = [@"" stringByPaddingToLength:200 withString:@"ab cd " startingAtIndex:0];
        if (sampleOut.length) {
            NSTask *t = [NSTask launchedTaskWithLaunchPath:@"/usr/bin/sample"
                arguments:@[[NSString stringWithFormat:@"%d", getpid()], @"6", @"1", @"-file",
                            sampleOut]];
            (void)t;
            sampling = YES;
            [NSThread sleepForTimeInterval:0.5];
            typed = [@"" stringByPaddingToLength:2000 withString:@"ab cd " startingAtIndex:0];
        }
        std::vector<double> wall;
        std::vector<double> times = TypeChars(tv, typed, &wall);
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
    MCGuiStep(@"the suite's window closes unasked, and its file goes", ^{
        for (MCTab *t in [(NSArray *)[c valueForKey:@"tabs"] copy]) [c closeTab:t prompt:NO];
        [c.window close];
        c = nil;
        tv = nil;
        MCGuiSetKeyWindow(nil);
        [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
    });
}

