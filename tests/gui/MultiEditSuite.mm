// MultiEditSuite.mm — `make guitest SUITES=multiedit`: edits at several
// places at once (several carets, Replace All) as separate storage edits,
// so a fold, a squiggle or the colors between two of them are left alone;
// and what those edits, Up and Down at several carets, Select All
// Occurrences and Add Cursor Below cost in a 100,000-line Python file.
#import "Harness.h"
#import "EditorController.h"
#import "EditorFind.h"
#import "FoldView.h"
#import "Gutter.h"
#import "Navigation.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#include "TextFind.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <string>
#include <vector>

// Replace All's last step, called directly so its time is the edit's alone.
@interface MCEditorFind (MultiEditSuite)
- (BOOL)applyEdits:(const std::vector<TextFind::Edit16> &)edits actionName:(NSString *)name;
@end

namespace {

double Ms() {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

// The main thread's CPU time, in ms: what the bounds check, since wall time
// on a loaded machine (other builds running) varied threefold run to run.
double CpuMs() {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const mach_port_t self = mach_thread_self();
    const kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS) return Ms();
    return (info.user_time.seconds + info.system_time.seconds) * 1e3 +
           (info.user_time.microseconds + info.system_time.microseconds) / 1e3;
}

// Times `work`: wall and main-thread CPU, printed; returns the CPU time.
double Timed(NSString *what, void (^work)(void)) {
    const double w0 = Ms(), c0 = CpuMs();
    work();
    const double cpu = CpuMs() - c0, wall = Ms() - w0;
    MCGuiNote(@"%@: %.1f ms (%.1f ms CPU)", what, wall, cpu);
    return cpu;
}

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }

std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

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

// The incremental colors equal a full re-highlight.
BOOL ColorsAsFullPass(EditorController *c, NSTextView *tv) {
    NSArray *incremental = ColorRuns(tv.textStorage);
    [c performSelector:@selector(applyHighlighting)];
    return [incremental isEqualToArray:ColorRuns(tv.textStorage)];
}

NSValue *At(NSUInteger i) { return [NSValue valueWithRange:NSMakeRange(i, 0)]; }

std::vector<TextFind::Edit16> ReplaceAllEdits(NSString *text, NSString *find, NSString *with) {
    TextFind::Options o;
    o.caseSensitive = true;
    return TextFind::replaceAll(U16(text), TextFind::Query::make(U16(find), o), U16(with));
}

}  // namespace

MC_GUI_SUITE(multiedit) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    __block MCFolds *folds = nil;
    NSString *py = MCGuiProjectFile(@"multiedit.py");
    NSString *pyText =
        @"name = 1\n"               // 1
        @"\n"                       // 2
        @"def f(x):\n"              // 3
        @"    if x:\n"              // 4
        @"        return name\n"    // 5
        @"    return 2\n"           // 6
        @"\n"                       // 7
        @"print(name)\n";           // 8

    MCGuiStep(@"a small file", ^{
        MCGuiWriteFile(py, pyText);
        c = MCGuiController();
        tv = Editor(c);
        folds = tv.folds;
        [c focusEditor:nil];
        GCHECK([c showFile:py]);
    });
    MCGuiWait(@"regions for multiedit.py", 5, ^BOOL { return [folds lineStartsRegion:2]; });

    // Typing at a caret above a fold and one below it leaves the fold
    // closed: neither edit touches it. One beginEditing around both made the
    // storage report one edit from the first caret to the last, which opened
    // every fold between them.
    MCGuiStep(@"typing at carets around a fold leaves it closed", ^{
        GCHECK([folds toggleLine:2]);
        GCHECK([folds lineStartsFold:2]);
        NSString *s = tv.string;
        const NSUInteger first = [s rangeOfString:@"name"].location;
        const NSUInteger last = [s rangeOfString:@"print(name)"].location + 6;
        [tv.multiCarets setCaretRanges:@[At(first), At(last)]];
        MCGuiUndoGroup(tv.undoManager, ^{ [tv.multiCarets insertText:@"z"]; });
        GCHECK([tv.string hasPrefix:@"zname = 1\n"]);
        GCHECK([tv.string hasSuffix:@"print(zname)\n"]);
        GCHECK([folds lineStartsFold:2]);
        GCHECK(ColorsAsFullPass(c, tv));
        [tv.multiCarets clear];
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, pyText);
    });

    // A squiggle between two carets keeps its length; it used to stretch
    // over the whole span the one merged edit reported.
    MCGuiStep(@"a squiggle between two carets keeps its length", ^{
        NSString *s = tv.string;
        MCDiagnosticMark *m = [MCDiagnosticMark new];
        m.range = NSMakeRange([s rangeOfString:@"return 2"].location, 6);
        m.severity = 1;
        m.message = @"a test problem";
        tv.diagnostics = @[m];
        const NSUInteger first = [s rangeOfString:@"name"].location;
        const NSUInteger last = [s rangeOfString:@"print(name)"].location + 6;
        const NSUInteger was = m.range.location;
        [tv.multiCarets setCaretRanges:@[At(first), At(last)]];
        MCGuiUndoGroup(tv.undoManager, ^{ [tv.multiCarets insertText:@"zz"]; });
        MCDiagnosticMark *moved = tv.diagnostics.firstObject;
        GCHECK(moved && moved.range.location == was + 2 && moved.range.length == 6);
        [tv.multiCarets clear];
        [tv.undoManager undo];
        tv.diagnostics = @[];
        GCHECK_EQ_STR(tv.string, pyText);
    });

    // Replace All goes through the same helper: a fold between two matches
    // stays closed, the text is replaced, and one undo takes it all back.
    MCGuiStep(@"Replace All around a fold leaves it closed, one undo step", ^{
        GCHECK([folds lineStartsFold:2] || [folds toggleLine:2]);
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        GCHECK(find != nil);
        tv.selectedRange = NSMakeRange(0, 0);
        std::vector<TextFind::Edit16> edits;
        TextFind::Edit16 a, b;   // "name" on line 1 and line 8, not the one in the fold
        a.start = 0; a.length = 4; a.text = u"title";
        b.start = [tv.string rangeOfString:@"print(name)"].location + 6; b.length = 4;
        b.text = u"title";
        edits = {a, b};
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([find applyEdits:edits actionName:@"Replace All"]); });
        GCHECK([tv.string hasPrefix:@"title = 1\n"]);
        GCHECK([tv.string hasSuffix:@"print(title)\n"]);
        GCHECK([folds lineStartsFold:2]);
        GCHECK(ColorsAsFullPass(c, tv));
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, pyText);
        [folds unfoldAll];
    });

    // An undo step whose text was changed outside the undo history (a
    // storage edit nobody registered) still undoes, at its pieces' new
    // places, when one change moved them; when the pieces are gone it
    // leaves the text alone (with a beep and a line in the log). It used to
    // give up silently whenever the length differed.
    // What changes: `length` characters from `offset` past where `at` is in
    // the edited text ("" for its start, "$" its end, "*" all of it).
    struct Outside { NSString *name, *at; NSUInteger offset, length; NSString *with, *expect; };
    const std::vector<Outside> outsides = {
        {@"before the first piece", @"", 0, 0, @"# top\n", [@"# top\n" stringByAppendingString:pyText]},
        {@"between the pieces", @"def", 2, 0, @"X",
         [pyText stringByReplacingOccurrencesOfString:@"def" withString:@"deXf"]},
        {@"deleted between the pieces", @"    return 2\n", 0, 13, @"",
         [pyText stringByReplacingOccurrencesOfString:@"    return 2\n" withString:@""]},
        {@"after the last piece", @"$", 0, 0, @"tail\n", [pyText stringByAppendingString:@"tail\n"]},
        {@"the pieces themselves gone", @"*", 0, 0, @"something else", @"something else"},
    };
    for (const Outside &each : outsides) {
        const Outside o = each;   // the block keeps a copy; the vector is gone when steps run
        MCGuiStep([@"undo after the text changed outside the undo history: " stringByAppendingString:o.name], ^{
            GCHECK([c showFile:py]);
            GCHECK_EQ_STR(tv.string, pyText);
            NSString *s = tv.string;
            const NSUInteger first = [s rangeOfString:@"name"].location;
            const NSUInteger last = [s rangeOfString:@"print(name)"].location + 6;
            [tv.multiCarets setCaretRanges:@[At(first), At(last)]];
            MCGuiUndoGroup(tv.undoManager, ^{ [tv.multiCarets insertText:@"zz"]; });
            [tv.multiCarets clear];
            NSString *now = tv.string;
            NSRange r = NSMakeRange(0, now.length);
            if ([o.at isEqualToString:@"$"]) r = NSMakeRange(now.length, 0);
            else if (![o.at isEqualToString:@"*"])
                r = NSMakeRange((o.at.length ? [now rangeOfString:o.at].location : 0) + o.offset, o.length);
            GCHECK([[now substringWithRange:NSMakeRange(0, 6)] isEqualToString:@"zzname"]);
            [tv.textStorage replaceCharactersInRange:r withString:o.with];   // registers no undo
            [tv.undoManager undo];
            GCHECK_EQ_STR(tv.string, o.expect);
            // Back to the file's text, outside the history again.
            [tv.textStorage replaceCharactersInRange:NSMakeRange(0, tv.string.length) withString:pyText];
            [tv.undoManager removeAllActions];
        });
    }

    // Many replacements (64 and more drop the glyphs over them first) around
    // a closed fold: it stays closed, and its text is laid out hidden again.
    NSString *manyFile = MCGuiProjectFile(@"multiedit-many.py");
    MCGuiStep(@"a file with a fold among many matches", ^{
        NSMutableString *t = [NSMutableString string];
        for (int i = 0; i < 40; i++) [t appendFormat:@"name_%d = %d\n", i, i];
        [t appendString:@"def f(x):\n    return name\n"];
        for (int i = 40; i < 80; i++) [t appendFormat:@"name_%d = %d\n", i, i];
        MCGuiWriteFile(manyFile, t);
        GCHECK([c showFile:manyFile]);
    });
    MCGuiWait(@"regions for multiedit-many.py", 5, ^BOOL { return [folds lineStartsRegion:40]; });
    MCGuiStep(@"Replace All of 80 matches around a fold keeps it closed and hidden", ^{
        GCHECK([folds toggleLine:40]);
        NSString *s = tv.string;
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, s.length)];
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        std::vector<TextFind::Edit16> edits = ReplaceAllEdits(s, @"name_", @"title_");
        GCHECK(edits.size() == 80);
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([find applyEdits:edits actionName:@"Replace All"]); });
        GCHECK([tv.string rangeOfString:@"name_"].location == NSNotFound);
        GCHECK([folds lineStartsFold:40]);
        const NSRange body = [tv.string rangeOfString:@"return name"];
        GCHECK([folds rangeIsHidden:body]);
        NSLayoutManager *lm = tv.layoutManager;
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        const NSUInteger g = [lm glyphIndexForCharacterAtIndex:body.location + 2];
        GCHECK([lm propertyForGlyphAtIndex:g] == NSGlyphPropertyNull);
        GCHECK(ColorsAsFullPass(c, tv));
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, s);
        [folds unfoldAll];
    });

    // Go Back's places follow edits: two lines typed above a place, and Back
    // lands on the same text, two lines further down.
    NSString *navFile = MCGuiProjectFile(@"multiedit-nav.txt");
    MCGuiStep(@"Go Back lands on the text it left after typing above it", ^{
        NSMutableString *t = [NSMutableString string];
        for (int i = 0; i < 40; i++) [t appendFormat:@"line %d\n", i];
        MCGuiWriteFile(navFile, t);
        GCHECK([c showFile:navFile]);
        MCNavigator *nav = [c valueForKey:@"navigator"];
        GCHECK(nav != nil);
        const NSUInteger far = [tv.string rangeOfString:@"line 30\n"].location;
        tv.selectedRange = NSMakeRange(0, 0);
        [nav jump:^{ tv.selectedRange = NSMakeRange(far + 2, 0); }];
        tv.selectedRange = NSMakeRange(0, 0);   // back to the top: a far move, recorded
        MCGuiUndoGroup(tv.undoManager, ^{ [tv insertText:@"new\nnew\n" replacementRange:NSMakeRange(0, 0)]; });
        GCHECK([nav.entries containsObject:[navFile stringByAppendingString:@":32"]]);
        [nav goBack];
        GCHECK(tv.selectedRange.location == far + 2 + 8);
        GCHECK([[tv.string substringWithRange:NSMakeRange(tv.selectedRange.location - 2, 7)]
            isEqualToString:@"line 30"]);
        [tv.undoManager undo];
    });

    // An old Mac file (lines ending in '\r' alone): the gutter numbers every
    // line, and the folds find the regions.
    NSString *crFile = MCGuiProjectFile(@"multiedit-cr.py");
    MCGuiStep(@"a file with CR line breaks", ^{
        MCGuiWriteFile(crFile, @"def f(x):\r    if x:\r        return 1\r    return 2\r\rx = 3\r");
        GCHECK([c showFile:crFile]);
        MCLineGutter *gutter = (MCLineGutter *)tv.enclosingScrollView.verticalRulerView;
        GCHECK([gutter isKindOfClass:[MCLineGutter class]]);
        GCHECK([gutter lineOfCharacter:[tv.string rangeOfString:@"x = 3"].location] == 5);
        GCHECK([gutter lineOfCharacter:[tv.string rangeOfString:@"return 2"].location] == 3);
    });
    MCGuiWait(@"regions for a CR file", 5, ^BOOL { return [folds lineStartsRegion:0]; });
    MCGuiStep(@"folding a CR file hides its body", ^{
        GCHECK([folds toggleLine:0]);
        GCHECK([folds rangeIsHidden:[tv.string rangeOfString:@"return 2"]]);
        GCHECK(![folds rangeIsHidden:[tv.string rangeOfString:@"x = 3"]]);
        [folds unfoldAll];
    });

    // ---- 100,000 lines
    NSString *big = MCGuiProjectFile(@"multiedit100k.py");
    MCGuiStep(@"a 100,000-line Python file", ^{
        NSMutableString *s = [NSMutableString string];
        for (int i = 0; i < 100000 / 5; i++)
            [s appendFormat:@"def f%d(x, y=\"s%d\"):  # a comment %d\n"
                            @"    '''doc'''\n"
                            @"    return x + %d * y\n"
                            @"\n"
                            @"value_%d = [1, 2.5, 'three', None]\n", i, i, i, i, i];
        MCGuiWriteFile(big, s);
        const double t0 = Ms();
        GCHECK([c showFile:big]);
        MCGuiNote(@"opening it: %.0f ms", Ms() - t0);
        GCHECK([tv.string hasPrefix:@"def f0"]);
    });

    // Opened and pressed in one step, before background layout gets far:
    // Down lays out only to the line below the furthest caret (it laid out
    // the whole file, about 2 s for 100,000 lines).
    MCGuiStep(@"Down at two carets right after opening lays out only to them", ^{
        NSString *copy = MCGuiProjectFile(@"multiedit100k-2.py");
        GCHECK([[NSFileManager defaultManager] copyItemAtPath:big toPath:copy error:nil]);
        GCHECK([c showFile:copy]);
        NSString *s = tv.string;
        const NSUInteger a = [s rangeOfString:@"return x + 40 *"].location;
        const NSUInteger b = [s rangeOfString:@"return x + 60 *"].location;
        [tv.multiCarets setCaretRanges:@[At(a), At(b)]];
        const double t0 = Ms();
        GCHECK([tv.multiCarets doCommand:@selector(moveDown:)]);
        MCGuiNote(@"Down at two carets in a file just opened: %.1f ms", Ms() - t0);
        NSArray<NSValue *> *r = tv.multiCarets.caretRanges;
        GCHECK(r.count == 2);
        if (r.count == 2) {
            // One line down from "    return x + 40 * y" is the blank line after it.
            GCHECK([[s substringWithRange:[s lineRangeForRange:r[0].rangeValue]] isEqualToString:@"\n"]);
            const double t1 = Ms();
            GCHECK([tv.multiCarets doCommand:@selector(moveUp:)]);
            MCGuiNote(@"Up at two carets: %.1f ms", Ms() - t1);
            GCHECK(tv.multiCarets.caretRanges[0].rangeValue.location == a);
            GCHECK(tv.multiCarets.caretRanges[1].rangeValue.location == b);
        }
        // Down from the last line goes to the end; Up from the first to the start.
        [tv.multiCarets setCaretRanges:@[At(2), At(s.length - 5)]];
        GCHECK([tv.multiCarets doCommand:@selector(moveDown:)]);
        GCHECK(tv.multiCarets.caretRanges.count == 0 ||
               tv.multiCarets.caretRanges.lastObject.rangeValue.location == s.length);
        GCHECK(tv.selectedRange.location == s.length || tv.multiCarets.active);
        [tv.multiCarets clear];
        GCHECK([c showFile:big]);
    });

    MCGuiStep(@"typing at two carets 99,800 lines apart", ^{
        NSString *s = tv.string;
        const NSUInteger a = [s rangeOfString:@"return x + 20 *"].location;
        const NSUInteger b = [s rangeOfString:@"return x + 19980 *"].location;
        [tv.multiCarets setCaretRanges:@[At(a), At(b)]];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(a, 100)];
        const double t0 = Ms();
        MCGuiUndoGroup(tv.undoManager, ^{ [tv.multiCarets insertText:@"\""]; });
        MCGuiNote(@"a quote at two carets 99,800 lines apart: %.1f ms", Ms() - t0);
        GCHECK([[tv.string substringWithRange:NSMakeRange(a, 7)] isEqualToString:@"\"return"]);
        GCHECK([[tv.string substringWithRange:NSMakeRange(b + 1, 7)] isEqualToString:@"\"return"]);
        [tv.multiCarets clear];
        GCHECK(ColorsAsFullPass(c, tv));
        [tv.undoManager undo];
        GCHECK([tv.string rangeOfString:@"\"return"].location == NSNotFound);
    });

    // 20,000 occurrences: Select All Occurrences stops at 10,000 carets,
    // as VS Code does, and says so.
    MCGuiStep(@"Select All Occurrences of a common word, then typing and undo", ^{
        NSString *s = tv.string;
        const NSUInteger first = [s rangeOfString:@"None"].location;
        __block NSString *note = nil;
        void (^was)(NSString *) = tv.multiCarets.onNote;
        tv.multiCarets.onNote = ^(NSString *n) { note = n; };
        tv.selectedRange = NSMakeRange(first, 4);
        double t0 = Ms();
        [tv selectAllOccurrences:nil];
        const NSUInteger n = tv.multiCarets.caretRanges.count;
        MCGuiNote(@"Select All Occurrences: %lu carets in %.1f ms", (unsigned long)n, Ms() - t0);
        tv.multiCarets.onNote = was;
        GCHECK(n == 10000);
        GCHECK([note containsString:@"10000"]);
        GCHECK(tv.multiCarets.caretRanges.firstObject.rangeValue.location == first);
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(first, 100)];
        // MC_GUI_SAMPLE=<file>: /usr/bin/sample on the process while it types.
        NSString *sampleOut = NSProcessInfo.processInfo.environment[@"MC_GUI_SAMPLE"];
        if (sampleOut.length) {
            [NSTask launchedTaskWithLaunchPath:@"/usr/bin/sample"
                                     arguments:@[[NSString stringWithFormat:@"%d", getpid()], @"2",
                                                 @"1", @"-file", sampleOut]];
            [NSThread sleepForTimeInterval:0.3];
        }
        t0 = Ms();
        MCGuiUndoGroup(tv.undoManager, ^{ [tv.multiCarets insertText:@"X"]; });
        MCGuiNote(@"typing a letter over %lu selections: %.1f ms", (unsigned long)n, Ms() - t0);
        GCHECK([[tv.string substringWithRange:NSMakeRange(first - 2, 4)] isEqualToString:@", X]"]);
        [tv.multiCarets clear];
        GCHECK(ColorsAsFullPass(c, tv));
        t0 = Ms();
        [tv.undoManager undo];
        MCGuiNote(@"undoing it: %.1f ms", Ms() - t0);
        GCHECK_EQ_STR(tv.string, s);
        // The old text is selected again, every piece of it.
        GCHECK(tv.selectedRanges.count == 10000);
        GCHECK(NSEqualRanges(tv.selectedRanges.firstObject.rangeValue, NSMakeRange(first, 4)));
        t0 = Ms();
        [tv.undoManager redo];
        MCGuiNote(@"redoing it: %.1f ms", Ms() - t0);
        GCHECK([[tv.string substringWithRange:NSMakeRange(first - 2, 4)] isEqualToString:@", X]"]);
        GCHECK(ColorsAsFullPass(c, tv));
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, s);
    });

    MCGuiStep(@"Add Cursor Below at 5,000 carets", ^{
        NSString *s = tv.string;
        NSMutableArray<NSValue *> *carets = [NSMutableArray array];
        NSRange r = NSMakeRange(0, s.length);
        while (carets.count < 5000) {
            const NSRange f = [s rangeOfString:@"value_" options:0 range:r];
            if (f.location == NSNotFound) break;
            [carets addObject:At(f.location)];
            r = NSMakeRange(NSMaxRange(f), s.length - NSMaxRange(f));
        }
        [tv.multiCarets setCaretRanges:carets];
        const double t0 = Ms();
        GCHECK([tv.multiCarets addCursorVertically:1]);
        MCGuiNote(@"Add Cursor Below at %lu carets: %.1f ms -> %lu carets",
                  (unsigned long)carets.count, Ms() - t0,
                  (unsigned long)tv.multiCarets.caretRanges.count);
        // "value_N = ..." is followed by "def fN+1(...", column 0 both.
        GCHECK(tv.multiCarets.caretRanges.count == 10000);
        const NSUInteger below = tv.multiCarets.caretRanges[1].rangeValue.location;
        GCHECK([[s substringWithRange:NSMakeRange(below, 4)] isEqualToString:@"def "]);
        // Past the limit: no more, and a note.
        __block NSString *note = nil;
        void (^was)(NSString *) = tv.multiCarets.onNote;
        tv.multiCarets.onNote = ^(NSString *n) { note = n; };
        GCHECK(![tv.multiCarets addCursorVertically:1]);
        tv.multiCarets.onNote = was;
        GCHECK(note != nil);
        [tv.multiCarets clear];
    });

    __block double replaceCpu = 0;
    MCGuiStep(@"Replace All of 20,000 matches", ^{
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        std::vector<TextFind::Edit16> edits = ReplaceAllEdits(tv.string, @"None", @"Nada");
        GCHECK(edits.size() == 20000);
        replaceCpu = Timed(@"Replace All of 20000 matches", ^{
            MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([find applyEdits:edits actionName:@"Replace All"]); });
        });
        GCHECK([tv.string rangeOfString:@"None"].location == NSNotFound);
        GCHECK(ColorsAsFullPass(c, tv));
        Timed(@"undoing it", ^{ [tv.undoManager undo]; });
        GCHECK([tv.string rangeOfString:@"Nada"].location == NSNotFound);
    });

    // The case the assessment met (8.1 s): the file laid out to its end, as
    // after scrolling there or once background layout finishes. Each
    // storage edit replaced its glyphs in the layout manager's glyph tree,
    // at a cost growing with the glyphs made, and the highlighter's restyles
    // paid again (3 to 8 s); replaceRanges now drops the glyphs over the
    // ranges first, so laid out costs about what not laid out does.
    MCGuiStep(@"Replace All of 20,000 matches in a file laid out to its end", ^{
        NSString *s = [tv.string copy];
        Timed(@"laying it out to its end", ^{
            [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, s.length)];
        });
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        std::vector<TextFind::Edit16> edits = ReplaceAllEdits(s, @"None", @"Nada");
        GCHECK(edits.size() == 20000);
        const double cpu = Timed(@"Replace All of 20000 matches, laid out", ^{
            MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([find applyEdits:edits actionName:@"Replace All"]); });
        });
        GCHECK(cpu < 4000);
        GCHECK(cpu < 2.5 * replaceCpu + 300);
        GCHECK([tv.string rangeOfString:@"None"].location == NSNotFound);
        GCHECK(ColorsAsFullPass(c, tv));
        Timed(@"undoing it", ^{ [tv.undoManager undo]; });
        GCHECK_EQ_STR(tv.string, s);
    });

    MCGuiStep(@"typing over 10,000 selections in a file laid out to its end", ^{
        NSString *s = [tv.string copy];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, s.length)];
        const NSUInteger first = [s rangeOfString:@"None"].location;
        tv.selectedRange = NSMakeRange(first, 4);
        [tv selectAllOccurrences:nil];
        GCHECK(tv.multiCarets.caretRanges.count == 10000);
        const double cpu = Timed(@"typing a letter over 10000 selections, laid out", ^{
            MCGuiUndoGroup(tv.undoManager, ^{ [tv.multiCarets insertText:@"X"]; });
        });
        GCHECK(cpu < 5000);
        GCHECK([[tv.string substringWithRange:NSMakeRange(first - 2, 4)] isEqualToString:@", X]"]);
        [tv.multiCarets clear];
        Timed(@"undoing it", ^{ [tv.undoManager undo]; });
        GCHECK_EQ_STR(tv.string, s);
    });

    // Backspace, the arrows and Move Line read the text near the carets
    // (they copied the whole buffer, 5 MB here, for every key).
    MCGuiStep(@"Backspace, Right and Move Line Down at two carets 99,800 lines apart", ^{
        NSString *s = [tv.string copy];
        const NSUInteger a = [s rangeOfString:@"return x + 20 *"].location;
        const NSUInteger b = [s rangeOfString:@"return x + 19980 *"].location;
        [tv.multiCarets setCaretRanges:@[At(a), At(b)]];
        double t0 = Ms();
        GCHECK([tv.multiCarets doCommand:@selector(moveRight:)]);
        MCGuiNote(@"Right at two carets: %.2f ms", Ms() - t0);
        GCHECK(tv.multiCarets.caretRanges.count == 2 &&
               tv.multiCarets.caretRanges[1].rangeValue.location == b + 1);
        t0 = Ms();
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([tv.multiCarets doCommand:@selector(deleteBackward:)]); });
        MCGuiNote(@"Backspace at two carets: %.2f ms", Ms() - t0);
        GCHECK([[tv.string substringWithRange:NSMakeRange(a, 6)] isEqualToString:@"eturn "]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, s);
        [tv.multiCarets setCaretRanges:@[At(a), At(b)]];
        t0 = Ms();
        MCGuiUndoGroup(tv.undoManager, ^{ [tv.multiCarets moveLinesDown:YES]; });
        MCGuiNote(@"Move Line Down at two carets: %.2f ms", Ms() - t0);
        // "    return x + 20 * y" swapped with the blank line below it.
        GCHECK([[tv.string substringWithRange:NSMakeRange(a - 4, 18)] isEqualToString:@"\n    return x + 20"]);
        [tv.multiCarets clear];
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, s);
    });
}
