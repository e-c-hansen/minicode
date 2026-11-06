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

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }

std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

NSArray<NSArray *> *ColorRuns(NSTextStorage *ts) {
    NSMutableArray *out = [NSMutableArray array];
    [ts enumerateAttribute:NSForegroundColorAttributeName inRange:NSMakeRange(0, ts.length)
                   options:0 usingBlock:^(NSColor *c, NSRange r, BOOL *stop) {
        (void)stop;
        [out addObject:@[@(r.location), @(r.length), c ?: (id)NSNull.null]];
    }];
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

    MCGuiStep(@"Replace All of 20,000 matches", ^{
        MCEditorFind *find = [c valueForKey:@"editorFind"];
        std::vector<TextFind::Edit16> edits = ReplaceAllEdits(tv.string, @"None", @"Nada");
        GCHECK(edits.size() == 20000);
        const double t0 = Ms();
        MCGuiUndoGroup(tv.undoManager, ^{ GCHECK([find applyEdits:edits actionName:@"Replace All"]); });
        MCGuiNote(@"Replace All of %zu matches: %.1f ms", edits.size(), Ms() - t0);
        GCHECK([tv.string rangeOfString:@"None"].location == NSNotFound);
        GCHECK(ColorsAsFullPass(c, tv));
        const double t1 = Ms();
        [tv.undoManager undo];
        MCGuiNote(@"undoing it: %.1f ms", Ms() - t1);
        GCHECK([tv.string rangeOfString:@"Nada"].location == NSNotFound);
    });
}
