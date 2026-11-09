// AgentPlacesSuite.mm — `make guitest SUITES=agent_places`: where an agent's
// pointers land, in the files where they went wrong.
//   - A note, and a line revealed (`minicode open big.log:250000 --note`),
//     in a large-file tab, whose layout is non-contiguous and whose rects
//     far down are estimates: the line ends up on screen a third of the
//     way down and the note box over it (agents 1, round 6).
//   - `selection` and `state` count lines as the gutter does: a lone '\r'
//     and U+2029 end a line; and in a large file without walking the text
//     from the top (AG-D7).
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AgentNotes.h"
#import "ControlServer.h"

namespace {

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }

// Every line the same length, so line n starts at (n - 1) * kLineLength.
const NSUInteger kLineLength = 40;
const NSUInteger kLines = 350000;

NSString *BigText() {
    NSMutableString *s = [NSMutableString stringWithCapacity:kLines * kLineLength];
    for (NSUInteger i = 1; i <= kLines; i++)
        [s appendFormat:@"log line %07lu: value = %-13lu\n", (unsigned long)i, (unsigned long)(i * 7)];
    return s;
}

// Where line `line` (1-based) is now laid out, in the text view, laid out
// at the line itself first, so the answer is TextKit's real place and not
// its estimate.
NSRect LaidOutLine(NSTextView *tv, NSUInteger line) {
    NSLayoutManager *lm = tv.layoutManager;
    const NSUInteger at = (line - 1) * kLineLength;
    [lm ensureLayoutForCharacterRange:NSMakeRange(at, 1)];
    NSRect r = [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:at]
                                    effectiveRange:nil];
    return NSOffsetRect(r, tv.textContainerOrigin.x, tv.textContainerOrigin.y);
}

// The line's top as a fraction of the pane's height from its top (on
// screen when between 0 and 1).
CGFloat PlaceInPane(NSTextView *tv, NSUInteger line) {
    NSClipView *clip = tv.enclosingScrollView.contentView;
    return (NSMinY(LaidOutLine(tv, line)) - NSMinY(clip.bounds)) / NSHeight(clip.bounds);
}

}  // namespace

MC_GUI_SUITE(agent_places) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"agent-places"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    NSString *big = [dir stringByAppendingPathComponent:@"big.log"];
    NSString *mac = [dir stringByAppendingPathComponent:@"oldmac.txt"];

    MCGuiStep(@"a 350,000-line file opens in large-file mode", ^{
        MCGuiWriteFile(big, BigText());
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        for (EditorController *w in MCGuiControllers())
            if ([w.rootPath isEqualToString:dir]) c = w;
        GCHECK(c != nil);
        MCGuiSetKeyWindow(c.window);
        tv = Editor(c);
        [c focusEditor:nil];
        GCHECK([c showFile:big]);
        GCHECK(Active(c).largeFile && tv.layoutManager.allowsNonContiguousLayout);
        GCHECK(tv.string.length == kLines * kLineLength);
    });

    MCGuiStep(@"a note at line 250,000: the line a third of the way down, the box over it", ^{
        MCAgentNotes *notes = c.agentNotes;
        GCHECK([notes showNote:@"here" from:nil line:250000 column:0 toLine:250000 toColumn:0]);
        GCHECK(notes.noteShown && notes.noteRange.location == 249999 * kLineLength);
        const CGFloat place = PlaceInPane(tv, 250000);
        MCGuiNote(@"line 250,000 is %.2f of the way down the pane", place);
        GCHECK(place > 0.2 && place < 0.5);
        NSView *box = notes.noteView;
        const NSRect line = LaidOutLine(tv, 250000);
        GCHECK(box && NSMaxY(box.frame) <= NSMinY(line) + 0.5 && NSMaxY(box.frame) > NSMinY(line) - 12);
        NSClipView *clip = tv.enclosingScrollView.contentView;
        GCHECK(NSMinY(box.frame) >= NSMinY(clip.bounds) - 0.5);
        [notes closeNote];
    });

    MCGuiStep(@"a line revealed far up, then far down, lands a third of the way down each time", ^{
        MCAgentNotes *notes = c.agentNotes;
        for (NSNumber *n in @[@40000, @330000, @120000]) {
            const NSUInteger line = n.unsignedIntegerValue;
            GCHECK([notes revealLine:(int)line column:5]);
            GCHECK(tv.selectedRange.location == (line - 1) * kLineLength + 4);
            const CGFloat place = PlaceInPane(tv, line);
            MCGuiNote(@"line %lu is %.2f of the way down the pane", (unsigned long)line, place);
            GCHECK(place > 0.2 && place < 0.5);
        }
    });

    // AG-D7: `selection` reads lines from the gutter's index, which has
    // the answer for line 300,000 without a walk through 12 MB.
    MCGuiStep(@"selection in a large file costs no walk from the top", ^{
        tv.selectedRange = NSMakeRange(299999 * kLineLength + 9, 3);
        const double c0 = MCGuiThreadCpuMs();
        NSDictionary *sel = nil;
        for (int i = 0; i < 20; i++) sel = [c controlSelection];
        const double each = (MCGuiThreadCpuMs() - c0) / 20;
        MCGuiNote(@"selection at line 300,000: %.3f ms of CPU a call", each);
        GCHECK([sel[@"start"][@"line"] integerValue] == 300000 &&
               [sel[@"start"][@"column"] integerValue] == 10 &&
               [sel[@"end"][@"column"] integerValue] == 13);
        GCHECK_TIME(each, 2, @"selection at line 300,000");
    });

    MCGuiStep(@"old Mac line breaks and U+2029 count as the gutter counts them", ^{
        MCGuiWriteFile(mac, @"first\rsecond\rthird fourth\r\nfifth\n");
        GCHECK([c showFile:mac]);
        NSString *s = tv.string;
        tv.selectedRange = NSMakeRange([s rangeOfString:@"third"].location + 2, 0);
        NSDictionary *sel = [c controlSelection];
        GCHECK([sel[@"start"][@"line"] integerValue] == 3 && [sel[@"start"][@"column"] integerValue] == 3);
        tv.selectedRange = NSMakeRange([s rangeOfString:@"fifth"].location, 5);
        sel = [c controlSelection];
        GCHECK([sel[@"start"][@"line"] integerValue] == 5 && [sel[@"end"][@"column"] integerValue] == 6);
        // Between the '\r' and '\n' of a CRLF: still the fourth line.
        tv.selectedRange = NSMakeRange([s rangeOfString:@"\r\n"].location + 1, 0);
        sel = [c controlSelection];
        GCHECK([sel[@"start"][@"line"] integerValue] == 4 && [sel[@"start"][@"column"] integerValue] == 8);
        // Just after a lone '\r': the next line's first column.
        tv.selectedRange = NSMakeRange([s rangeOfString:@"second"].location, 0);
        sel = [c controlSelection];
        GCHECK([sel[@"start"][@"line"] integerValue] == 2 && [sel[@"start"][@"column"] integerValue] == 1);
        // The gutter says the same.
        GCHECK([c.gutter lineOfCharacter:[s rangeOfString:@"fifth"].location] == 4);
    });

    MCGuiStep(@"close the window", ^{ [c.window close]; });
    MCGuiWait(@"the window gone", 5, ^BOOL { return ![MCGuiControllers() containsObject:c]; });
    MCGuiStep(@"done with the window", ^{
        c = nil;
        tv = nil;
        MCGuiSetKeyWindow(nil);
    });
}
