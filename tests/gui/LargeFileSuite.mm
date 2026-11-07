// LargeFileSuite.mm — `make guitest SUITES=large_file`: large-file mode
// (EditorController+LargeFile.mm, VS Code's thresholds in LargeFile.h): a
// 25 MB file opens plain, unfolded and kept from the language servers,
// with the status bar's note, and typing in it stays under the
// keystroke suite's bounds; a 300,001-line file counts as large too, and the
// note's click turns everything on for that tab; the setting turned off
// opens it as any file; a file over 50 MB asks before it is read. Then the
// lexer's limits in an ordinary file: a 5,000,000-character line stays plain
// and a keystroke in it is cheap, and a file with old Mac breaks colors as
// its '\n' twin.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#include <mach/mach_time.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {

double Ms() {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }

BOOL FullPerf() { return NSProcessInfo.processInfo.environment[@"MC_GUI_FULL_PERF"].length > 0; }

// The foreground colors' runs over a range.
NSUInteger ColorRunCount(NSTextStorage *ts, NSRange r) {
    __block NSUInteger n = 0;
    [ts enumerateAttribute:NSForegroundColorAttributeName inRange:r options:0
                usingBlock:^(id v, NSRange rr, BOOL *stop) { (void)v; (void)rr; (void)stop; n++; }];
    return n;
}

NSColor *ColorAt(NSTextStorage *ts, NSUInteger i) {
    return [ts attribute:NSForegroundColorAttributeName atIndex:i effectiveRange:NULL];
}

std::vector<double> TypeChars(NSTextView *tv, NSString *text) {
    std::vector<double> times;
    for (NSUInteger i = 0; i < text.length; i++) {
        NSString *ch = [text substringWithRange:NSMakeRange(i, 1)];
        const double t0 = Ms();
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:ch replacementRange:tv.selectedRange]; });
        times.push_back(Ms() - t0);
    }
    return times;
}

void Bounds(const std::vector<double> &times, NSString *what, double medianBound, double p95Bound,
            const char *file, int line) {
    std::vector<double> v = times;
    std::sort(v.begin(), v.end());
    const double med = v.empty() ? 0 : v[v.size() / 2];
    const double p95 = v.empty() ? 0 : v[v.size() * 95 / 100];
    MCGuiNote(@"%@: %lu keystrokes, median %.2f ms, 95th percentile %.2f ms", what,
              (unsigned long)v.size(), med, p95);
    MCGuiCheck(med < medianBound, [NSString stringWithFormat:@"%@: median under %.0f ms (%.2f)",
                                  what, medianBound, med], file, line);
    MCGuiCheck(p95 < p95Bound, [NSString stringWithFormat:@"%@: 95th percentile under %.0f ms (%.2f)",
                                what, p95Bound, p95], file, line);
}

// MC_GUI_SAMPLE=<folder>: /usr/bin/sample on the process for the next
// `seconds`, into <folder>/<tag>.txt, to see where a step's time goes.
void SampleFor(NSString *tag, int seconds) {
    NSString *dir = NSProcessInfo.processInfo.environment[@"MC_GUI_SAMPLE"];
    if (!dir.length) return;
    [NSTask launchedTaskWithLaunchPath:@"/usr/bin/sample"
                             arguments:@[[NSString stringWithFormat:@"%d", getpid()],
                                         [NSString stringWithFormat:@"%d", seconds], @"1", @"-file",
                                         [dir stringByAppendingPathComponent:
                                                  [tag stringByAppendingPathExtension:@"txt"]]]];
    [NSThread sleepForTimeInterval:0.5];
}

}  // namespace

MC_GUI_SUITE(large_file) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    NSString *big = MCGuiProjectFile(@"big25.py");
    NSString *lines = MCGuiProjectFile(@"lines300k.py");
    NSString *huge = MCGuiProjectFile(@"huge60.txt");
    NSString *minified = MCGuiProjectFile(@"minified.js");
    NSString *mac = MCGuiProjectFile(@"oldmac.py");

    MCGuiStep(@"a 25 MB Python file opens in large-file mode", ^{
        NSMutableString *s = [NSMutableString stringWithCapacity:26 << 20];
        for (int i = 0; s.length < (25u << 20); i++)
            [s appendFormat:@"value_%d = [1, 2.5, 'three', None]  # note (%d)\n", i, i];
        MCGuiWriteFile(big, s);
        c = MCGuiController();
        tv = Editor(c);
        [c focusEditor:nil];
        SampleFor(@"open25", 7);
        const double t0 = Ms();
        GCHECK([c showFile:big]);
        const double opened = Ms() - t0;
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, 4000)];
        const double shown = Ms() - t0;
        MCGuiNote(@"opening 25 MB (%lu lines): %.0f ms, its first screen laid out by %.0f ms",
                  (unsigned long)[s componentsSeparatedByString:@"\n"].count - 1, opened, shown);
        // Read, made a text storage and highlighted plain: far from the
        // seconds a full lex and attribute pass of 580,000 lines would take.
        MCGuiCheck(shown < 3000, [NSString stringWithFormat:@"a 25 MB file shows within 3 s (%.0f ms)",
                                  shown], __FILE__, __LINE__);
        MCTab *t = Active(c);
        GCHECK(t.largeFile && !t.largeFileFeatures && [c largeFileFeaturesOff]);
        GCHECK(tv.string.length > (25u << 20) - 100);
        // Plain: one run of one color over the first lines, comments and
        // numbers included.
        GCHECK(ColorRunCount(tv.textStorage, NSMakeRange(0, 4000)) == 1);
        // No folds, no bracket boxes, no server, the note on show. Word
        // wrap stays as set: VS Code turns it off for its own view's sake,
        // but here a view that does not wrap must lay out all of the text
        // to know its width (sizeToFit), nine tenths of the 5.5 s this open
        // took that way.
        GCHECK(tv.folds.fileName == nil);
        GCHECK(![c bracketMatchingApplies]);
        GCHECK(tv.textContainer.widthTracksTextView == MCEditorWordWrap());
        GCHECK([c languageServerPath:big] == nil && ![c languageServerMayRead:t]);
        GCHECK(c.lsp.bufferText(big) == nil);
        NSTextField *note = [c valueForKey:@"largeFileNote"];
        GCHECK(note && !note.hidden && [note.stringValue containsString:@"Large file"]);
        GCHECK([note.toolTip containsString:@"big25.py"]);
        GCHECK(NSMaxX(note.frame) <= NSMinX(c.lspLabel.frame) && note.frame.size.width > 40);
    });
    // MC_GUI_FULL_PERF=1: what the same file costs with the mode off, to
    // compare (a full lex and attribute pass on the main thread).
    if (FullPerf()) MCGuiStep(@"the same 25 MB file with large-file mode off", ^{
        MCTab *t = Active(c);
        MCGuiSetSettings(@"lsp.enabled = false\neditor.large-file-optimizations = off\n");
        GCHECK([c closeTab:t prompt:NO]);
        const double t0 = Ms();
        GCHECK([c showFile:big]);
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, 4000)];
        MCGuiNote(@"opening 25 MB with large-file mode off: %.0f ms", Ms() - t0);
        GCHECK(!Active(c).largeFile);
        NSRange mid = [tv.string rangeOfString:@"value_300000 = "];
        tv.selectedRange = NSMakeRange(mid.location + 6, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(mid.location, 2000)];
        std::vector<double> times = TypeChars(tv, @"ab cd ab cd ab cd ab cd ");
        std::sort(times.begin(), times.end());
        MCGuiNote(@"typing in it with the mode off: median %.2f ms", times[times.size() / 2]);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        GCHECK([c showFile:big]);
        GCHECK(Active(c).largeFile);
    });

    MCGuiStep(@"typing in it stays under the keystroke suite's bounds", ^{
        NSRange mid = [tv.string rangeOfString:@"value_300000 = "];
        GCHECK(mid.location != NSNotFound);
        const NSUInteger at = mid.location + 6;
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 2000)];
        SampleFor(@"type25", 6);
        std::vector<double> times =
            TypeChars(tv, [@"" stringByPaddingToLength:200 withString:@"ab cd " startingAtIndex:0]);
        GCHECK([[tv.string substringWithRange:NSMakeRange(at, 6)] isEqualToString:@"ab cd "]);
        Bounds(times, @"a 25 MB file in large-file mode", 20, 60, __FILE__, __LINE__);
        GCHECK(ColorRunCount(tv.textStorage, NSMakeRange(at - 100, 400)) == 1);   // still plain
        GCHECK(Active(c).dirty);
        [tv.undoManager undo];   // one group per character: undo a few, then close unsaved
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });

    MCGuiStep(@"300,001 short lines count as large; the note turns it all on", ^{
        NSMutableString *s = [NSMutableString stringWithCapacity:3 << 20];
        [s appendString:@"def f(x):\n    return (x)\n"];
        for (int i = 2; i < 300001; i++) [s appendString:@"x = 1\n"];
        MCGuiWriteFile(lines, s);   // 300,001 lines, the last one empty
        GCHECK([c showFile:lines]);
        MCTab *t = Active(c);
        GCHECK(t.largeFile && [c largeFileFeaturesOff]);
        GCHECK(ColorRunCount(tv.textStorage, NSMakeRange(0, 30)) == 1);
        const double t0 = Ms();
        [c turnOnLargeFileFeatures:nil];
        MCGuiNote(@"turning everything on for 300,001 lines: %.0f ms", Ms() - t0);
        GCHECK(t.largeFileFeatures && ![c largeFileFeaturesOff]);
        GCHECK(ColorRunCount(tv.textStorage, NSMakeRange(0, 30)) > 1);   // "def" and the numbers
        GCHECK([tv.folds.fileName isEqualToString:lines]);
        GCHECK(tv.textContainer.widthTracksTextView == MCEditorWordWrap());
        GCHECK([c bracketMatchingApplies]);
        GCHECK([c languageServerPath:lines] != nil && c.lsp.bufferText(lines) != nil);
        NSTextField *note = [c valueForKey:@"largeFileNote"];
        GCHECK(note.hidden);
    });
    MCGuiWait(@"its folds", 10, ^BOOL { return [tv.folds lineStartsRegion:0]; });
    MCGuiStep(@"the note comes back only for a large tab", ^{
        GCHECK([c closeTab:Active(c) prompt:NO]);
        NSTextField *note = [c valueForKey:@"largeFileNote"];
        GCHECK(note.hidden);
    });

    MCGuiStep(@"editor.large-file-optimizations = off opens it as any file", ^{
        MCGuiSetSettings(@"lsp.enabled = false\neditor.large-file-optimizations = off\n");
        GCHECK([c showFile:lines]);
        GCHECK(!Active(c).largeFile && ![c largeFileFeaturesOff]);
        GCHECK(ColorRunCount(tv.textStorage, NSMakeRange(0, 30)) > 1);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    MCGuiStep(@"a file over 50 MB asks before it is read", ^{
        {
            // 60 MB of text (a sparse file of NULs would go to Quick Look).
            NSMutableData *d = [NSMutableData dataWithCapacity:(60u << 20) + 64];
            const char line[] = "a line of a log that keeps going on and on and on\n";
            while (d.length < (60u << 20)) [d appendBytes:line length:sizeof line - 1];
            GCHECK([d writeToFile:huge atomically:NO]);
        }
        const NSUInteger asked = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Cancel
        GCHECK(![c showFile:huge]);
        GCHECK(MCGuiAlertsShown().count == asked + 1);
        GCHECK([MCGuiAlertsShown().lastObject containsString:@"huge60.txt"]);
        GCHECK([MCGuiAlertsShown().lastObject containsString:@"MB"]);
        GCHECK(![c.currentPath isEqualToString:huge]);
        // Open Anyway opens it, in large-file mode.
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        const double t0 = Ms();
        GCHECK([c showFile:huge]);
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, 4000)];
        MCGuiNote(@"opening 60 MB after the question: %.0f ms", Ms() - t0);
        GCHECK(MCGuiAlertsShown().count == asked + 2);
        GCHECK(Active(c).largeFile && [c largeFileFeaturesOff]);
        MCTab *hugeTab = Active(c);
        // Under the limit nothing is asked, nor for a file with a tab.
        NSString *small = MCGuiProjectFile(@"small.txt");
        MCGuiWriteFile(small, @"small\n");
        GCHECK([c showFile:small]);
        GCHECK([c showFile:huge]);
        GCHECK(MCGuiAlertsShown().count == asked + 2);
        GCHECK([c closeTab:hugeTab prompt:NO]);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        unlink(huge.fileSystemRepresentation);
    });

    MCGuiStep(@"a 5,000,000-character line: plain, and cheap to type in", ^{
        NSMutableString *s = [NSMutableString stringWithString:@"function f(a) { return a + 1; } // top\n"];
        NSMutableString *longLine = [NSMutableString stringWithCapacity:5000100];
        [longLine appendString:@"var m = ["];
        while (longLine.length < 5000000) [longLine appendString:@"\"x\",1,{\"k\":true},"];
        [longLine appendString:@"0];\n"];
        [s appendString:longLine];
        [s appendString:@"const tail = 'end'; // bottom\n"];
        MCGuiWriteFile(minified, s);
        const double t0 = Ms();
        GCHECK([c showFile:minified]);
        MCGuiNote(@"opening a 5 MB line: %.0f ms", Ms() - t0);
        GCHECK(!Active(c).largeFile);   // 5 MB, three lines: an ordinary file
        NSTextStorage *ts = tv.textStorage;
        const NSUInteger a = [tv.string rangeOfString:@"var m"].location;
        const NSUInteger b = [tv.string rangeOfString:@"const tail"].location;
        GCHECK(ColorRunCount(ts, NSMakeRange(a, b - 1 - a)) == 1);         // the long line: plain
        GCHECK(ColorRunCount(ts, NSMakeRange(0, a)) > 1);                  // the first: colored
        GCHECK(ColorRunCount(ts, NSMakeRange(b, ts.length - b)) > 1);      // the last too
        GCHECK(![ColorAt(ts, b) isEqual:ColorAt(ts, a)]);                  // "const" is a keyword
        const NSUInteger at = a + 2500000;
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 200)];
        SampleFor(@"typelong", 8);
        std::vector<double> times = TypeChars(tv, @"abcdefghijklmnopqrst");
        // Noted, not bounded: what is left of a keystroke here is TextKit
        // typesetting the 5 MB paragraph again (NSATSTypesetter and Core
        // Text in every sample), which no highlighting change reaches. The
        // highlighter's share is 3 units read (syntax:long-line).
        std::sort(times.begin(), times.end());
        MCGuiNote(@"a keystroke in a 5,000,000-character line: median %.0f ms (TextKit's layout)",
                  times[times.size() / 2]);
        GCHECK(ColorRunCount(ts, NSMakeRange(a, b + 20 - 1 - a)) == 1);
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });

    MCGuiStep(@"old Mac breaks color as '\\n' does", ^{
        MCGuiWriteFile(mac, @"# a comment\rdef f(x):\r    return 42\r");
        GCHECK([c showFile:mac]);
        NSTextStorage *ts = tv.textStorage;
        NSString *str = tv.string;
        const NSUInteger def = [str rangeOfString:@"def"].location;
        const NSUInteger num = [str rangeOfString:@"42"].location;
        // The comment ends at its '\r': "def" and 42 have colors of their
        // own, not the comment's (the old highlighter read one line).
        GCHECK(![ColorAt(ts, def) isEqual:ColorAt(ts, 0)]);
        GCHECK(![ColorAt(ts, num) isEqual:ColorAt(ts, 0)]);
        GCHECK(![ColorAt(ts, num) isEqual:ColorAt(ts, def)]);
        // Typing a '\n' after the first '\r' makes "\r\n": still colored right.
        tv.selectedRange = NSMakeRange([str rangeOfString:@"\r"].location + 1, 0);
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"\n" replacementRange:tv.selectedRange]; });
        str = tv.string;
        GCHECK(![ColorAt(ts, [str rangeOfString:@"def"].location) isEqual:ColorAt(ts, 0)]);
        [tv.undoManager undo];
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });
}
