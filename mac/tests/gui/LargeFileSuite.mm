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
#import "ControlServer.h"
#import "EditorFind.h"
#import "FindWidget.h"
#import "PieceString.h"
#import "SourceStorage.h"
#include <mach/mach.h>
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

// The process's physical footprint (what Activity Monitor's Memory column
// and `footprint` report), in MB.
double FootprintMB() {
    task_vm_info_data_t info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return 0;
    return (double)info.phys_footprint / 1048576.0;
}

// A server log of about `bytes` (timestamps, levels, messages; ASCII, as
// logs are), its last line holding NEEDLE-4242.
void WriteLog(NSString *path, size_t bytes) {
    FILE *f = fopen(path.fileSystemRepresentation, "w");
    if (!f) return;
    const char *levels[] = {"INFO ", "INFO ", "DEBUG", "WARN ", "ERROR"};
    size_t written = 0;
    for (unsigned i = 0; written < bytes; i++) {
        written += (size_t)fprintf(f, "2026-11-18T%02u:%02u:%02u.%03uZ %s worker-%02u request id=%08u "
                                      "path=/api/items/%u took=%ums\n",
                                   (i / 3600) % 24, (i / 60) % 60, i % 60, i % 1000, levels[i % 5],
                                   i % 32, i, i % 9973, (i * 7919) % 1000);
    }
    fprintf(f, "2026-11-18T23:59:59.999Z ERROR worker-07 NEEDLE-4242 payment failed\n");
    fclose(f);
}

BOOL FullPerf() { return NSProcessInfo.processInfo.environment[@"MC_GUI_FULL_PERF"].length > 0; }

// The character at the top left of the editor's visible rect.
NSUInteger TopCharacter(NSTextView *tv) {
    const NSRect v = tv.visibleRect;
    return [tv characterIndexForInsertionAtPoint:NSMakePoint(NSMinX(v) + 20, NSMinY(v) + 1)];
}

NSUInteger LineOf(NSString *s, NSUInteger i) {
    __block NSUInteger n = 0;
    [s enumerateSubstringsInRange:NSMakeRange(0, MIN(i, s.length))
                          options:NSStringEnumerationByLines | NSStringEnumerationSubstringNotRequired
                       usingBlock:^(NSString *l, NSRange r, NSRange e, BOOL *stop) {
        (void)l; (void)r; (void)e; (void)stop; n++;
    }];
    return n;
}

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

// Each keystroke's main-thread CPU time in ms (as KeystrokeSuite's).
std::vector<double> TypeChars(NSTextView *tv, NSString *text) {
    std::vector<double> times;
    for (NSUInteger i = 0; i < text.length; i++) {
        NSString *ch = [text substringWithRange:NSMakeRange(i, 1)];
        const double c0 = MCGuiThreadCpuMs();
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:ch replacementRange:tv.selectedRange]; });
        times.push_back(MCGuiThreadCpuMs() - c0);
    }
    return times;
}

void Bounds(const std::vector<double> &times, NSString *what, double medianBound, double p95Bound,
            const char *file, int line) {
    std::vector<double> v = times;
    std::sort(v.begin(), v.end());
    const double med = v.empty() ? 0 : v[v.size() / 2];
    const double p95 = v.empty() ? 0 : v[v.size() * 95 / 100];
    MCGuiNote(@"%@: %lu keystrokes, median %.2f ms of CPU, 95th percentile %.2f ms", what,
              (unsigned long)v.size(), med, p95);
    MCGuiCheckTime(med, medianBound, [NSString stringWithFormat:@"%@: the median", what], file, line);
    MCGuiCheckTime(p95, p95Bound, [NSString stringWithFormat:@"%@: the 95th percentile", what],
                   file, line);
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
    __block std::vector<double> freshKeys;   // the first typing's, to compare with later
    __block NSUInteger typedAt = 0;
    __block double openBase = 0;             // the first step's baseline, ms of CPU
    // A window of its own on a folder of its own: what earlier suites left
    // open (tabs, servers, trees watching the project) does not time this
    // suite's keystrokes, and 85 MB of files do not land in their folder.
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"large-files"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    NSString *big = [dir stringByAppendingPathComponent:@"big25.py"];
    NSString *lines = [dir stringByAppendingPathComponent:@"lines300k.py"];
    NSString *huge = [dir stringByAppendingPathComponent:@"huge60.txt"];
    NSString *minified = [dir stringByAppendingPathComponent:@"minified.js"];
    NSString *mac = [dir stringByAppendingPathComponent:@"oldmac.py"];

    MCGuiStep(@"a 25 MB Python file opens in large-file mode", ^{
        NSMutableString *s = [NSMutableString stringWithCapacity:26 << 20];
        for (int i = 0; s.length < (25u << 20); i++)
            [s appendFormat:@"value_%d = [1, 2.5, 'three', None]  # note (%d)\n", i, i];
        MCGuiWriteFile(big, s);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        for (EditorController *w in MCGuiControllers())
            if ([w.rootPath isEqualToString:dir]) c = w;
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
        tv = Editor(c);
        [c focusEditor:nil];
        // A baseline from this run: reading the file and making a plain
        // text storage of it, the least an open does, on the same thread
        // and so on the same kind of core.
        double base = MCGuiThreadCpuMs();
        @autoreleasepool {
            NSString *read = [NSString stringWithContentsOfFile:big encoding:NSUTF8StringEncoding
                                                          error:nil];
            NSTextStorage *plain = [[NSTextStorage alloc] initWithString:read ?: @""
                attributes:@{NSForegroundColorAttributeName: NSColor.textColor}];
            __block NSUInteger n = 0;
            [read enumerateLinesUsingBlock:^(NSString *l, BOOL *stop) { (void)l; (void)stop; n++; }];
            GCHECK(plain.length == read.length && n > 0);
        }
        base = MCGuiThreadCpuMs() - base;
        openBase = base;
        SampleFor(@"open25", 7);
        const double t0 = Ms(), c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:big]);
        const double opened = Ms() - t0;
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, 4000)];
        const double shown = Ms() - t0, shownCpu = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"opening 25 MB (%lu lines): %.0f ms, its first screen laid out by %.0f ms "
                  @"(%.0f ms of main-thread CPU; the baseline %.0f ms, %.1f times it)",
                  (unsigned long)[s componentsSeparatedByString:@"\n"].count - 1, opened, shown,
                  shownCpu, base, shownCpu / MAX(base, 1));
        // Made a text storage and highlighted plain: about 0.25 s of the
        // main thread's CPU, under the baseline (0.3 s), against 2.2 s with
        // the mode off (a full lex and attribute pass of 470,000 lines;
        // measured by turning the mode off in noteLargeFileText:). Font
        // substitution reading all 25 MB for missing glyphs made it 1.8 to
        // 2.6 s, six or seven times the baseline (MCSourceStorage now skips
        // it for plain ASCII). The open is on the main thread, so its CPU
        // time is the bound; the old 3 s of wall time let the mode off pass.
        // On a loaded Mac the thread may run on an efficiency core, several
        // times slower in CPU time too, and the baseline with it: so 1 s or
        // three times the baseline, whichever is more.
        GCHECK_TIME(shownCpu, MAX(1000, 3 * base), @"a 25 MB file shows");
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
        GCHECK(tv.layoutManager.allowsNonContiguousLayout);   // laid out only where shown
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
        const double t0 = Ms(), c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:big]);
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, 4000)];
        MCGuiNote(@"opening 25 MB with large-file mode off: %.0f ms (%.0f ms of main-thread CPU)",
                  Ms() - t0, MCGuiThreadCpuMs() - c0);
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
        freshKeys = times;
        GCHECK(ColorRunCount(tv.textStorage, NSMakeRange(at - 100, 400)) == 1);   // still plain
        GCHECK(Active(c).dirty);
        [tv.undoManager undo];   // one group per character: undo a few, then close unsaved
        typedAt = at;
    });

    // Leaving the tab hides the line numbers, which widens the text view;
    // with the 25 MB still in the storage that laid all of it out (about
    // 1.3 s). makeEditorReadOnly lets go of a long text first.
    NSString *small = [dir stringByAppendingPathComponent:@"beside.py"];
    NSString *page = [dir stringByAppendingPathComponent:@"page.md"];
    MCGuiStep(@"switching away from it lays none of it out", ^{
        MCTab *bigTab = Active(c);
        // Where the typing left it: line 300,000 at the top of the pane.
        const NSRange selection = tv.selectedRange;
        const NSUInteger topLine = LineOf(tv.string, TopCharacter(tv));
        GCHECK(topLine > 290000 && tv.undoManager.canUndo);
        MCGuiWriteFile(small, @"x = 1\n");
        double c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:small]);   // a new tab, read for the first time
        const double toNew = MCGuiThreadCpuMs() - c0;
        GCHECK(Active(c) != bigTab && [tv.string isEqualToString:@"x = 1\n"]);
        GCHECK(!tv.layoutManager.allowsNonContiguousLayout);   // a small file's rects are exact
        SampleFor(@"back25", 5);
        c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:big]);
        const double back = MCGuiThreadCpuMs() - c0;
        GCHECK(Active(c) == bigTab && bigTab.dirty);
        GCHECK([[tv.string substringWithRange:NSMakeRange(typedAt, 5)] isEqualToString:@"ab cd"]);
        // The caret, the top line and the undo steps as they were.
        GCHECK(NSEqualRanges(tv.selectedRange, selection));
        const NSUInteger topAgain = LineOf(tv.string, TopCharacter(tv));
        MCGuiNote(@"the top line %lu before, %lu shown again", (unsigned long)topLine,
                  (unsigned long)topAgain);
        GCHECK(topAgain + 1 >= topLine && topAgain <= topLine + 1);
        GCHECK(tv.undoManager.canUndo && tv.layoutManager.allowsNonContiguousLayout);
        c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:small]);   // a tab shown again
        const double toShown = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"from the 25 MB tab to a new tab: %.0f ms of CPU, back to it %.0f ms, "
                  @"away again %.0f ms", toNew, back, toShown);
        GCHECK_TIME(toNew, 250, @"switching from the 25 MB tab to a new tab");
        GCHECK_TIME(toShown, 250, @"switching from the 25 MB tab to a tab shown before");
        // Showing it again at line 300,000 laid out every line above it
        // (contiguous layout) and read all 25 MB for font substitution: 13
        // to 18 s of CPU, against about 0.25 s now, as much as the first
        // open. The bound is the open's own, from this run's baseline.
        GCHECK_TIME(back, MAX(1000, 3 * openBase), @"showing the 25 MB tab again at line 300,000");
        GCHECK([c closeTab:Active(c) prompt:NO]);
        GCHECK(Active(c) == bigTab);
    });

    // The same let-go with a language server: a 2 MB file is no large file,
    // so it is open on its server, and the emptied storage must never reach
    // the server as an edit of it (whatever shows next lets the session go
    // of the document in the same turn, before the 0.3 s didChange).
    NSString *served = [dir stringByAppendingPathComponent:@"served2.py"];
    MCGuiStep(@"a 2 MB file open on a server, then a tab without one", ^{
        NSMutableString *s = [NSMutableString stringWithCapacity:2200000];
        for (int i = 0; s.length < 2100000; i++) [s appendFormat:@"value_%d = %d  # served\n", i, i];
        MCGuiWriteFile(served, s);
        NSString *cmd = MCGuiFakeLsp(@"letgo", @{@"name": @"fakelsp", @"capabilities": @{
            @"textDocumentSync": @{@"openClose": @YES, @"change": @2}}, @"rules": @[]}, nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        GCHECK([c showFile:served]);
        GCHECK(!Active(c).largeFile && tv.editable);
    });
    MCGuiWait(@"the server has the file", 20, ^BOOL {
        return MCGuiFakeLspReceived(@"letgo", @"textDocument/didOpen").count > 0;
    });
    MCGuiStep(@"switching away and back sends it nothing", ^{
        MCGuiFakeLspClearLog(@"letgo");
        NSString *notes = [dir stringByAppendingPathComponent:@"notes.txt"];
        MCGuiWriteFile(notes, @"plain text, no server\n");
        GCHECK([c showFile:small]);    // a new tab (beside.py was closed): read afresh
        GCHECK([c showFile:served]);
        GCHECK([c showFile:notes]);    // a new tab with no server
        GCHECK([c showFile:served]);
        GCHECK([c showFile:small]);    // a tab shown before
        GCHECK([c showFile:served]);
        // A picture's tab shows no text: its own documentOpened:nil is what
        // keeps the emptying from going out as an edit.
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
            pixelsWide:4 pixelsHigh:4 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        NSString *picture = [dir stringByAppendingPathComponent:@"dot.png"];
        GCHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
                   writeToFile:picture atomically:YES]);
        GCHECK([c showFile:picture]);
        GCHECK(Active(c).isImage);
    });
    MCGuiPause(1.0);   // a negative check: a didChange would have gone 0.3 s after an edit
    MCGuiStep(@"no didChange or didClose reached the server", ^{
        NSArray *changes = MCGuiFakeLspReceived(@"letgo", @"textDocument/didChange");
        NSArray *closes = MCGuiFakeLspReceived(@"letgo", @"textDocument/didClose");
        MCGuiNote(@"%lu didChange, %lu didClose", (unsigned long)changes.count,
                  (unsigned long)closes.count);
        GCHECK(changes.count == 0 && closes.count == 0);
        GCHECK([c showFile:served]);
        GCHECK(tv.editable && [tv.string hasPrefix:@"value_0 = 0"]);
        for (MCTab *t in [(NSArray *)[c valueForKey:@"tabs"] copy])
            if (![t.currentPath isEqualToString:big]) GCHECK([c closeTab:t prompt:NO]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    // A Markdown page in the editor's view (its quotes, code boxes, h1 and
    // h2 rules, and tables are text blocks) left TextKit recalculating the
    // used rect over every laid-out line on each later keystroke: 16 to
    // 48 ms a key here in the window earlier suites had used, 9 ms with the
    // caret where this suite types (forgetTextBlockLayout).
    MCGuiStep(@"after a Markdown page in the window, typing in it costs as much", ^{
        MCGuiWriteFile(page, @"# Title\n\n> a quote\n\n```\ncode\n```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n");
        GCHECK([c showFile:page]);
        GCHECK(!tv.editable);   // the preview
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        GCHECK([c showFile:big]);
        GCHECK(Active(c).largeFile);
        tv.selectedRange = NSMakeRange(typedAt, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(typedAt, 2000)];
        SampleFor(@"type25used", 6);
        std::vector<double> times =
            TypeChars(tv, [@"" stringByPaddingToLength:200 withString:@"ab cd " startingAtIndex:0]);
        // Against this run's fresh keystrokes, so the bound holds on any
        // core: about the same cost, where the block table made the median
        // four times as much.
        std::vector<double> fresh = freshKeys;
        std::sort(fresh.begin(), fresh.end());
        const double freshMed = fresh.empty() ? 0 : fresh[fresh.size() / 2];
        const double freshP95 = fresh.empty() ? 0 : fresh[fresh.size() * 95 / 100];
        Bounds(times, @"a 25 MB file after a Markdown page", MAX(5, 2.5 * freshMed),
               MAX(10, 3 * freshP95), __FILE__, __LINE__);
        for (NSUInteger i = 0; i < times.size(); i++) [tv.undoManager undo];
        GCHECK([c closeTab:Active(c) prompt:NO]);
        GCHECK([c closeTab:Active(c) prompt:NO]);   // the page
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

    // C3 and C4 (round 8): what Finder sends (Open With, a double-click)
    // goes through the app delegate's open handler, as here.
    MCGuiStep(@"a lone script from Finder: caret at 1:1, the keyboard in its text", ^{
        NSString *lonely = [dir stringByAppendingPathComponent:@"lonely.py"];
        NSMutableString *py = [NSMutableString string];
        for (int i = 0; i < 33; i++) [py appendFormat:@"value_%d = %d  # line %d\n", i, i, i + 1];
        MCGuiWriteFile(lonely, py);
        [c.window makeFirstResponder:c.outline];   // where a new window has it
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:lonely]]];
        GCHECK([c.currentPath.lastPathComponent isEqualToString:@"lonely.py"]);
        MCGuiNote(@"caret %@ of %lu, first responder %@", NSStringFromRange(tv.selectedRange),
                  (unsigned long)tv.string.length, c.window.firstResponder.className);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(0, 0)));
        GCHECK(c.window.firstResponder == tv);
        GCHECK(NSMinY(tv.visibleRect) <= 1);
        GCHECK([c closeTab:Active(c) prompt:NO]);
    });

    MCGuiStep(@"a 60 MB log from Finder opens with no question, at its start", ^{
        {
            // 60 MB of text (a sparse file of NULs would go to Quick Look).
            NSMutableData *d = [NSMutableData dataWithCapacity:(60u << 20) + 64];
            const char line[] = "a line of a log that keeps going on and on and on\n";
            while (d.length < (60u << 20)) [d appendBytes:line length:sizeof line - 1];
            GCHECK([d writeToFile:huge atomically:NO]);
        }
        const NSUInteger asked = MCGuiAlertsShown().count;
        [c.window makeFirstResponder:c.outline];
        const double t0 = Ms();
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:huge]]];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, 4000)];
        MCGuiNote(@"opening 60 MB from Finder: %.0f ms", Ms() - t0);
        GCHECK(MCGuiAlertsShown().count == asked);
        GCHECK([c.currentPath.lastPathComponent isEqualToString:@"huge60.txt"]);
        GCHECK(Active(c).largeFile && [c largeFileFeaturesOff]);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(0, 0)));
        GCHECK(c.window.firstResponder == tv);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        unlink(huge.fileSystemRepresentation);
    });

    // Round 9's grading: a 259 MB log took 1.7 GB, the text held four
    // times over (read bytes, the tab's string, a copy made to set the
    // storage, the storage's own) and copied to UTF-16 for each line
    // index, and macOS keeps a freed block that size in the footprint. Now
    // the file's bytes are the tab's string and the storage edits over it
    // (PieceString.h): about the file's size.
    NSString *log100 = [dir stringByAppendingPathComponent:@"server100.log"];
    __block double logFootprint = 0, logMB = 0;
    __block NSData *logBytes = nil;
    MCGuiStep(@"a 100 MB log opens in about its size, its first screen quickly", ^{
        WriteLog(log100, 100u << 20);
        logMB = (double)MCStampOfFile(log100).size / 1048576.0;
        // A baseline from this run: reading the file into a string and
        // counting its lines, the least an open does, on this thread.
        double base = MCGuiThreadCpuMs();
        @autoreleasepool {
            NSData *d = [NSData dataWithContentsOfFile:log100];
            NSString *read = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
            std::vector<unichar> buf(1 << 16);
            size_t lines = 0;
            for (NSUInteger at = 0; at < read.length; at += buf.size()) {
                const NSUInteger k = MIN(buf.size(), read.length - at);
                [read getCharacters:buf.data() range:NSMakeRange(at, k)];
                for (NSUInteger i = 0; i < k; i++) lines += buf[i] == '\n';
            }
            GCHECK(lines > 1000000);
        }
        base = MCGuiThreadCpuMs() - base;
        logFootprint = FootprintMB();
        const double t0 = Ms(), c0 = MCGuiThreadCpuMs();
        GCHECK([c showFile:log100]);
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, 4000)];
        const double shown = Ms() - t0, shownCpu = MCGuiThreadCpuMs() - c0;
        const double grown = FootprintMB() - logFootprint;
        MCGuiNote(@"opening a %.0f MB log: its first screen laid out by %.0f ms (%.0f ms of "
                  @"main-thread CPU; the baseline %.0f ms); the footprint %.0f MB more",
                  logMB, shown, shownCpu, base, grown);
        GCHECK(Active(c).largeFile && [c largeFileFeaturesOff]);
        GCHECK([tv.string hasPrefix:@"2026-11-18T00:00:00.000Z INFO  worker-00"]);
        GCHECK([tv.string hasSuffix:@"NEEDLE-4242 payment failed\n"]);
        // The storage holds the tab's string itself, unedited.
        GCHECK([(MCSourceStorage *)tv.textStorage holdsLargeText]);
        GCHECK([tv.string isKindOfClass:MCPieceString.class] && ((MCPieceString *)tv.string).isOriginal);
        GCHECK([tv.string copy] == Active(c).sourceText);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(0, 0)) && NSMinY(tv.visibleRect) <= 1);
        // Under a second of the main thread, or three times the baseline on a
        // slow core (the 25 MB file's rule).
        GCHECK_TIME(shownCpu, MAX(1000, 3 * base), @"a 100 MB log shows its first screen");
    });
    MCGuiStep(@"its memory stays near its size", ^{
        // A turn later, so what the open left for the run loop has run.
        const double grown = FootprintMB() - logFootprint;
        MCGuiNote(@"the footprint grew %.0f MB for a %.0f MB log (%.2f times its size; 1.7 GB "
                  @"for 259 MB, 6.5 times, before)", grown, logMB, grown / logMB);
        GCHECK(grown < 1.5 * logMB);
    });
    MCGuiStep(@"typed in, it is dirty; typed back, it is not", ^{
        logBytes = [NSData dataWithContentsOfFile:log100];
        const NSUInteger at = tv.string.length / 2;
        tv.selectedRange = NSMakeRange(at, 0);
        [tv scrollRangeToVisible:tv.selectedRange];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 2000)];
        std::vector<double> times = TypeChars(tv, @"ab cd ab cd ab cd ab cd ");
        Bounds(times, @"a 100 MB log", 20, 60, __FILE__, __LINE__);
        GCHECK(Active(c).dirty);
        // Deleted again, the text is the file's: not dirty, found without
        // reading the 100 MB (the pieces join back into the original).
        for (int i = 0; i < 24; i++)
            MCGuiUndoGroup(tv.undoManager, ^{ [tv deleteBackward:nil]; });
        const double c0 = MCGuiThreadCpuMs();
        GCHECK(!Active(c).dirty);
        GCHECK(((MCPieceString *)tv.string).isOriginal);
        MCGuiNote(@"back to the file's text: dirty %d, %.2f ms", Active(c).dirty,
                  MCGuiThreadCpuMs() - c0);
    });
    MCGuiStep(@"an edit saves exactly", ^{
        const NSUInteger at = tv.string.length / 3;
        tv.selectedRange = NSMakeRange(at, 5);
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"EDITED" replacementRange:tv.selectedRange]; });
        GCHECK(Active(c).dirty);
        [c saveCurrentFile:nil];
        GCHECK(!Active(c).dirty);
        NSMutableData *want = [logBytes mutableCopy];
        [want replaceBytesInRange:NSMakeRange(at, 5) withBytes:"EDITED" length:6];
        NSData *saved = [NSData dataWithContentsOfFile:log100];
        MCGuiNote(@"saved %lu bytes, %lu expected", (unsigned long)saved.length,
                  (unsigned long)want.length);
        GCHECK([saved isEqualToData:want]);
        logBytes = nil;
    });
    MCGuiStep(@"Cmd+F finds its last line without holding the main thread", ^{
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        tv.selectedRange = NSMakeRange(0, 0);
        [c.window makeFirstResponder:tv];
        GCHECK(MCGuiPerformMenuKey(c.window, 3, NSEventModifierFlagCommand, nil, nil));
        GCHECK(find.shown);
        NSTextView *ed = (NSTextView *)find.bar.findField.currentEditor;
        const double c0 = MCGuiThreadCpuMs();
        [ed insertText:@"NEEDLE-4242" replacementRange:ed.selectedRange];
        const double typed = MCGuiThreadCpuMs() - c0;
        MCGuiNote(@"the query typed: %.1f ms of the main thread", typed);
        GCHECK_TIME(typed, 100, @"typing a query in a 100 MB log");
    });
    MCGuiWait(@"the search reaches the end of the log", 60, ^BOOL {
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        return find.resultsCurrent && find.matchRanges.count == 1;
    });
    MCGuiStep(@"its one match, at the end", ^{
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        const NSRange m = find.matchRanges.firstObject.rangeValue;
        GCHECK(m.location == [tv.string rangeOfString:@"NEEDLE-4242" options:NSBackwardsSearch].location);
        GCHECK(NSEqualRanges(tv.selectedRange, m));
        [find close];
        GCHECK([c closeTab:Active(c) prompt:NO]);
        unlink(log100.fileSystemRepresentation);
    });
    MCGuiStep(@"large files not in ASCII: UTF-8 and Windows-1252", ^{
        // Past the size read into pages of their own: UTF-8 with an accent
        // becomes UTF-16 (the pages let go), and bytes that are not UTF-8
        // go to the ordinary read, which takes them as Windows-1252.
        NSString *utf8 = [dir stringByAppendingPathComponent:@"accents.log"];
        NSString *latin = [dir stringByAppendingPathComponent:@"latin1.log"];
        NSMutableData *u = [NSMutableData data], *l = [NSMutableData data];
        const char lineU[] = "caf\xC3\xA9 line of a log\n", lineL[] = "caf\xE9 line of a log\n";
        while (u.length < (6u << 20)) [u appendBytes:lineU length:sizeof lineU - 1];
        while (l.length < (6u << 20)) [l appendBytes:lineL length:sizeof lineL - 1];
        GCHECK([u writeToFile:utf8 atomically:NO] && [l writeToFile:latin atomically:NO]);
        GCHECK([c showFile:utf8]);
        GCHECK([tv.string hasPrefix:@"café line"] && Active(c).encoding == MCTextEncodingUtf8);
        GCHECK(tv.string.length == u.length - u.length / (sizeof lineU - 1));
        GCHECK([c showFile:latin]);
        GCHECK([tv.string hasPrefix:@"café line"] &&
               Active(c).encoding == MCTextEncodingWindows1252);
        GCHECK(tv.string.length == l.length);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        GCHECK([c closeTab:Active(c) prompt:NO]);
        unlink(utf8.fileSystemRepresentation);
        unlink(latin.fileSystemRepresentation);
    });

    MCGuiStep(@"past 1 GB it still asks before anything is read", ^{
        // Sparse past its first lines: 1 GB and a byte on paper, a few KB
        // on disk, and text where the sniffer looks.
        NSString *giant = [dir stringByAppendingPathComponent:@"giant.log"];
        NSMutableString *head = [NSMutableString string];
        while (head.length < 16384) [head appendString:@"a line of a giant log\n"];
        MCGuiWriteFile(giant, head);
        const int fd = open(giant.fileSystemRepresentation, O_WRONLY);
        GCHECK(fd >= 0 && ftruncate(fd, (off_t)(1ull << 30) + 1) == 0);
        if (fd >= 0) close(fd);
        const NSUInteger asked = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Cancel
        GCHECK(![c showFile:giant]);
        GCHECK(MCGuiAlertsShown().count == asked + 1);
        GCHECK([MCGuiAlertsShown().lastObject containsString:@"giant.log"]);
        GCHECK([MCGuiAlertsShown().lastObject containsString:@"GB"]);
        GCHECK(![c.currentPath isEqualToString:giant]);
        unlink(giant.fileSystemRepresentation);
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

    MCGuiStep(@"the suite's window closes, and its files go", ^{
        [c.window close];
        c = nil;
        tv = nil;
        MCGuiSetKeyWindow(nil);
        [[NSFileManager defaultManager] removeItemAtPath:dir error:nil];
    });
}
