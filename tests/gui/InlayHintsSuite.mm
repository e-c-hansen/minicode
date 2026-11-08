// InlayHintsSuite.mm — `make guitest SUITES=inlay_hints`: inlay hints
// against the scripted server (build/fakelsp).
//
// The server offers inlay hints and answers for a small Python file with a
// return type, a variable's type and two parameter names. They show once
// the file opens, in gaps the layout opens (the text, its stored
// attributes, copies and the caret's steps are as without them); they
// follow typing at once and are asked again when typing stops; a delete
// and its undo leave no kern in the text; editor.inlay-hints = off takes
// them away and asks for none. Last, the cost: a keystroke in a
// 100,000-line file with hints on the lines on show, in CPU time of the
// main thread, against the same keystrokes with none.
//
// MC_GUI_REAL_BASEDPYRIGHT=1 adds the real basedpyright (when installed):
// its hints for the same file arrive and are placed.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "LspInlayHints.h"
#import "SourceStorage.h"
#include <time.h>
#include <vector>

namespace {
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
CodeTextView *Text(EditorController *c) { return (CodeTextView *)[c valueForKey:@"textView"]; }

NSArray<NSString *> *Labels(EditorController *c) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *h in Text(c).inlayHints.shownHints) [out addObject:h[@"label"]];
    return out;
}

NSUInteger OffsetOf(EditorController *c, NSString *label) {
    for (NSDictionary *h in Text(c).inlayHints.shownHints)
        if ([h[@"label"] isEqualToString:label]) return [h[@"offset"] unsignedIntegerValue];
    return NSNotFound;
}

// The x where a character's glyph starts, in view coordinates.
CGFloat GlyphX(NSTextView *tv, NSUInteger index) {
    NSLayoutManager *lm = tv.layoutManager;
    const NSUInteger g = [lm glyphIndexForCharacterAtIndex:index];
    const NSRect frag = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:NULL];
    return tv.textContainerOrigin.x + frag.origin.x + [lm locationForGlyphAtIndex:g].x;
}

void TypeAt(NSTextView *tv, NSUInteger at, NSUInteger length, NSString *text) {
    MCGuiUndoGroup(tv.undoManager, ^{
        [tv insertText:text replacementRange:NSMakeRange(at, length)];
    });
}

// Whether any kern is stored in the text (a gap's is only laid out).
BOOL Stored(NSTextView *tv) {
    NSAttributedString *all = [tv.textStorage attributedSubstringFromRange:
                                                  NSMakeRange(0, tv.textStorage.length)];
    __block BOOL kerned = NO;
    [all enumerateAttribute:NSKernAttributeName inRange:NSMakeRange(0, all.length) options:0
                 usingBlock:^(id v, NSRange r, BOOL *stop) {
        (void)r;
        if (v) { kerned = YES; *stop = YES; }
    }];
    return kerned;
}

double CpuMs() { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) / 1e6; }

NSDictionary *Hint(int line, int ch, id label, int kind, BOOL left, BOOL right) {
    return @{@"position": @{@"line": @(line), @"character": @(ch)}, @"label": label,
             @"kind": @(kind), @"paddingLeft": @(left), @"paddingRight": @(right)};
}

NSString *const kSource = @"def area(width, height):\n"
                          @"    return width * height\n"
                          @"\n"
                          @"total = area(3, 4)\n";
}  // namespace

MC_GUI_SUITE(inlay_hints) {
    __block EditorController *c = nil;
    __block NSUInteger line3 = 0;
    __block NSUInteger asked = 0;
    NSString *file = MCGuiProjectFile(@"inlay/area.py");
    // "-> int" after the signature's ")", ": int" after total, and the two
    // parameter names before the arguments; label parts for one of them.
    NSArray *first = @[Hint(0, 23, @"-> int", 1, YES, NO), Hint(3, 5, @": int", 1, NO, NO),
                       Hint(3, 13, @[@{@"value": @"width"}, @{@"value": @"="}], 2, NO, YES),
                       Hint(3, 16, @"height=", 2, NO, YES)];
    // After "grand_" is typed before total: the same, moved, and a float.
    NSArray *second = @[Hint(0, 23, @"-> int", 1, YES, NO), Hint(3, 11, @": float", 1, NO, NO),
                        Hint(3, 19, @"width=", 2, NO, YES), Hint(3, 22, @"height=", 2, NO, YES)];
    NSDictionary *script = @{
        @"name": @"fakelsp",
        @"capabilities": @{@"textDocumentSync": @{@"openClose": @YES, @"change": @1},
                           @"inlayHintProvider": @YES},
        @"rules": @[
            @{@"method": @"textDocument/inlayHint", @"when": @{@"textContains": @"grand_total"},
              @"result": second},
            @{@"method": @"textDocument/inlayHint", @"when": @{@"textContains": @"total = area"},
              @"result": first},
            @{@"method": @"textDocument/inlayHint", @"result": @[]}]};
    __block NSString *cmd = nil;

    MCGuiStep(@"a Python file whose server offers inlay hints", ^{
        c = MCGuiController();
        MCGuiWriteFile(file, kSource);
        cmd = MCGuiFakeLsp(@"inlay", script, nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        GCHECK([c showFile:file]);
        line3 = [Text(c).string rangeOfString:@"total"].location;
    });
    MCGuiWait(@"its hints show", 20, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"they are the server's, at their places, and asked for the lines on show", ^{
        NSArray *labels = Labels(c);
        GCHECK([labels isEqualToArray:(@[@"-> int", @": int", @"width=", @"height="])]);
        GCHECK(OffsetOf(c, @"-> int") == 23);
        GCHECK(OffsetOf(c, @": int") == line3 + 5);
        GCHECK(OffsetOf(c, @"width=") == line3 + 13);
        NSArray *asks = MCGuiFakeLspReceived(@"inlay", @"textDocument/inlayHint");
        GCHECK(asks.count >= 1);
        NSDictionary *range = [asks.lastObject[@"params"] objectForKey:@"range"];
        GCHECK([range[@"start"][@"line"] intValue] == 0 &&
               [range[@"end"][@"line"] intValue] == 4 && [range[@"end"][@"character"] intValue] == 0);
        asked = Session(c).inlayHintRequests;
        GCHECK(asked >= 1);
    });
    MCGuiStep(@"the text is as it was: characters, stored attributes and copies", ^{
        CodeTextView *tv = Text(c);
        GCHECK_EQ_STR(tv.string, kSource);
        GCHECK_EQ_STR(tv.textStorage.string, kSource);
        // No kern is stored: only layout sees the gaps.
        GCHECK(!Stored(tv));
        GCHECK([tv.textStorage attributedSubstringFromRange:NSMakeRange(line3, 6)]
                   .string.length == 6);
        NSAttributedString *sub = [tv.textStorage attributedSubstringFromRange:NSMakeRange(line3, 18)];
        GCHECK([sub attribute:NSKernAttributeName atIndex:4 effectiveRange:NULL] == nil);
        // A copy of the line: the plain text, and rich text with no kern.
        tv.selectedRange = NSMakeRange(line3, 18);
        [tv copy:nil];
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        GCHECK_EQ_STR([pb stringForType:NSPasteboardTypeString], @"total = area(3, 4)");
        NSData *rtf = [pb dataForType:NSPasteboardTypeRTF];
        if (rtf) {
            NSAttributedString *a = [[NSAttributedString alloc] initWithRTF:rtf documentAttributes:NULL];
            GCHECK_EQ_STR(a.string, @"total = area(3, 4)");
            __block BOOL rich = NO;
            [a enumerateAttribute:NSKernAttributeName inRange:NSMakeRange(0, a.length) options:0
                       usingBlock:^(id v, NSRange r, BOOL *stop) {
                (void)r;
                if (v && [v doubleValue] != 0) { rich = YES; *stop = YES; }
            }];
            GCHECK(!rich);
        }
        MCGuiNote(@"the copy had %@rich text", rtf ? @"" : @"no ");
    });
    MCGuiStep(@"a gap as wide as the label opens where the hint is, and it is drawn there", ^{
        CodeTextView *tv = Text(c);
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        // ": int" sits between "total" and " = ": the space after total
        // starts past the label's box, which starts past the "l".
        NSUInteger index = NSNotFound;
        NSArray *shown = tv.inlayHints.shownHints;
        for (NSUInteger i = 0; i < shown.count; i++)
            if ([shown[i][@"label"] isEqualToString:@": int"]) index = i;
        const NSRect box = [tv.inlayHints rectOfHintAtIndex:index];
        const CGFloat l = GlyphX(tv, line3 + 4), space = GlyphX(tv, line3 + 5);
        const CGFloat e = GlyphX(tv, line3 + 6);   // "=" after the space
        const CGFloat cell = e - space;              // a monospaced character
        MCGuiNote(@"l at %.1f, the box %.1f to %.1f, the space at %.1f (a cell %.1f)", l,
                  NSMinX(box), NSMaxX(box), space, cell);
        GCHECK(NSWidth(box) > cell * 4);
        GCHECK(NSMinX(box) >= l + cell - 0.5 && NSMaxX(box) <= space + 0.5);
        GCHECK(space - l > cell + NSWidth(box) - 0.5);
        // A parameter name pads on its right: "width=" ends before the 3.
        NSUInteger w = NSNotFound;
        for (NSUInteger i = 0; i < shown.count; i++)
            if ([shown[i][@"label"] isEqualToString:@"width="]) w = i;
        const NSRect wb = [tv.inlayHints rectOfHintAtIndex:w];
        GCHECK(NSMaxX(wb) < GlyphX(tv, line3 + 13) - 1);
        // A picture to look at with MC_GUI_SNAPSHOTS.
        MCGuiSnapshot(tv, @"inlay-hints");
    });
    MCGuiStep(@"the caret steps over a hint one character at a time", ^{
        CodeTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(line3, 0);
        for (int i = 0; i < 5; i++) [tv moveRight:nil];
        GCHECK(tv.selectedRange.location == line3 + 5);
        [tv moveRight:nil];
        GCHECK(tv.selectedRange.location == line3 + 6);
        [tv moveToEndOfLine:nil];
        GCHECK(tv.selectedRange.location == line3 + 18);
        [tv moveToBeginningOfLine:nil];
        GCHECK(tv.selectedRange.location == line3);
        // A click in the gap puts the caret on one side of it.
        const NSRect box = [tv.inlayHints rectOfHintAtIndex:1];
        const NSUInteger i = [tv characterIndexForInsertionAtPoint:
                                     NSMakePoint(NSMidX(box), NSMidY(box))];
        GCHECK(i == line3 + 4 || i == line3 + 5);
    });
    MCGuiStep(@"typing before them moves them at once", ^{
        MCGuiFakeLspClearLog(@"inlay");
        TypeAt(Text(c), line3, 0, @"grand_");
        GCHECK(OffsetOf(c, @": int") == line3 + 11);
        GCHECK(OffsetOf(c, @"width=") == line3 + 19);
        GCHECK(OffsetOf(c, @"-> int") == 23);
        GCHECK(Text(c).inlayHints.count == 4);
    });
    MCGuiWait(@"and the server is asked again once typing stops", 10, ^BOOL {
        return [Labels(c) containsObject:@": float"];
    });
    MCGuiStep(@"its new hints replace the old", ^{
        GCHECK(Session(c).inlayHintRequests > asked);
        GCHECK(OffsetOf(c, @": float") == line3 + 11);
        GCHECK(OffsetOf(c, @": int") == NSNotFound);
        GCHECK(Text(c).inlayHints.count == 4);
        // Typed, the hint that follows a name moves along with it, and the
        // typed character takes no kern from the gap it was typed at.
        CodeTextView *tv = Text(c);
        tv.selectedRange = NSMakeRange(line3 + 11, 0);
        GCHECK(tv.typingAttributes[NSKernAttributeName] == nil);
        TypeAt(tv, line3 + 11, 0, @"s");
        GCHECK(OffsetOf(c, @": float") == line3 + 12);
        GCHECK(tv.typingAttributes[NSKernAttributeName] == nil);
        // A parameter's name stays before what is typed at it.
        const NSUInteger w = OffsetOf(c, @"width=");
        TypeAt(tv, w, 0, @"1");
        GCHECK(OffsetOf(c, @"width=") == w);
        [tv.undoManager undo];
        [tv.undoManager undo];
        GCHECK([tv.string containsString:@"grand_total = area(3, 4)"]);
    });
    MCGuiStep(@"a delete before a hint and its undo leave no kern in the text", ^{
        CodeTextView *tv = Text(c);
        const NSUInteger p = OffsetOf(c, @": float");
        GCHECK(p != NSNotFound);
        TypeAt(tv, p - 1, 1, @"");
        GCHECK(OffsetOf(c, @": float") == p - 1);
        [tv.undoManager undo];
        GCHECK([tv.string containsString:@"grand_total = area(3, 4)"]);
        GCHECK(!Stored(tv));
    });
    MCGuiWait(@"the hints settle again", 10, ^BOOL {
        return [Labels(c) containsObject:@": float"] && Text(c).inlayHints.count == 4 &&
               OffsetOf(c, @": float") == line3 + 11;
    });
    MCGuiStep(@"editor.inlay-hints = off takes them away", ^{
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n"
            @"editor.inlay-hints = off\n", cmd]);
    });
    MCGuiWait(@"they go", 10, ^BOOL { return Text(c).inlayHints.count == 0; });
    MCGuiStep(@"and typing asks for none", ^{
        asked = Session(c).inlayHintRequests;
        TypeAt(Text(c), line3, 0, @"x");
        [Text(c).undoManager undo];
    });
    MCGuiPause(0.8);   // longer than the pause after typing: nothing may be asked
    MCGuiStep(@"none was asked; on again, they come back", ^{
        GCHECK(Session(c).inlayHintRequests == asked);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
    });
    MCGuiWait(@"they come back", 10, ^BOOL { return Text(c).inlayHints.count == 4; });

    // ------------------------------------------------ the cost of a keystroke
    // Hints on every line on show in a 100,000-line file, given to the view
    // directly (as an answer would), then 200 keystrokes on a hinted line;
    // the main thread's CPU time against the same keystrokes with none.
    __block CodeTextView *big = nil;
    __block NSUInteger at = 0;
    NSString *bigFile = MCGuiProjectFile(@"inlay/big100k.py");
    MCGuiStep(@"a 100,000-line Python file", ^{
        NSMutableString *s = [NSMutableString string];
        for (int i = 0; i < 100000 / 5; i++)
            [s appendFormat:@"def f%d(x, y=\"s%d\"):  # a comment %d\n"
                            @"    '''doc'''\n"
                            @"    return x + %d * y\n"
                            @"\n"
                            @"value_%d = f%d(1, 2.5)\n", i, i, i, i, i, i];
        MCGuiWriteFile(bigFile, s);
        GCHECK([c showFile:bigFile]);
        big = Text(c);
        NSRange mid = [big.string rangeOfString:@"value_10000 = f10000("];
        GCHECK(mid.location != NSNotFound);
        at = mid.location;
        big.selectedRange = NSMakeRange(at, 0);
        [big scrollRangeToVisible:big.selectedRange];
        [big.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 4000)];
    });
    auto typeTimed = ^double(NSString *typed) {
        const double t0 = CpuMs();
        for (NSUInteger i = 0; i < typed.length; i++) {
            NSString *ch = [typed substringWithRange:NSMakeRange(i, 1)];
            MCGuiUndoGroup(big.undoManager, ^{
                [big insertText:ch replacementRange:big.selectedRange];
            });
            [big.layoutManager ensureLayoutForCharacterRange:NSMakeRange(at, 200)];
        }
        return CpuMs() - t0;
    };
    NSString *typed = [@"" stringByPaddingToLength:200 withString:@"ab cd " startingAtIndex:0];
    MCGuiStep(@"200 keystrokes on a hinted line, hints on every line on show", ^{
        // Hints as basedpyright gives them: a type after each value_N and
        // two parameter names in its call, for the 400 lines around.
        NSString *text = big.string;
        NSUInteger a = at;
        for (int i = 0; i < 200 && a > 0; i++) a = [text lineRangeForRange:NSMakeRange(a - 1, 0)].location;
        NSUInteger b = at;
        for (int i = 0; i < 200 && b < text.length; i++) b = NSMaxRange([text lineRangeForRange:NSMakeRange(b, 0)]);
        __block std::vector<Lsp::InlayHint> hints;
        __block std::vector<Lsp::PlacedInlayHint> placed;
        [text enumerateSubstringsInRange:NSMakeRange(a, b - a)
                                 options:NSStringEnumerationByLines
                              usingBlock:^(NSString *line, NSRange r, NSRange er, BOOL *stop) {
            (void)er;
            (void)stop;
            if (![line hasPrefix:@"value_"]) return;
            NSRange eq = [line rangeOfString:@" = "];
            NSRange paren = [line rangeOfString:@"("];
            NSRange comma = [line rangeOfString:@", "];
            const std::pair<NSUInteger, const char *> spots[] = {
                {eq.location, ": float"}, {paren.location + 1, "x="}, {NSMaxRange(comma), "y="}};
            for (const auto &spot : spots) {
                Lsp::InlayHint h;
                h.label = spot.second;
                h.paddingRight = spot.second[0] != ':';
                Lsp::PlacedInlayHint p;
                p.offset = r.location + spot.first;
                p.followsBefore = spot.second[0] == ':';
                p.hint = hints.size();
                hints.push_back(h);
                placed.push_back(p);
            }
        }];
        const double s0 = CpuMs();
        [big.inlayHints showHints:hints placed:placed];
        [big.layoutManager ensureLayoutForCharacterRange:NSMakeRange(a, b - a)];
        const double showMs = CpuMs() - s0;
        MCGuiNote(@"%lu hints shown and laid out in %.1f ms of CPU", (unsigned long)placed.size(),
                  showMs);
        GCHECK(big.inlayHints.count == placed.size());
        MCGuiCheck(showMs < 150, [NSString stringWithFormat:
                       @"showing the hints of 400 lines takes under 150 ms of CPU (%.1f)", showMs],
                   __FILE__, __LINE__);
        // The keystrokes go right after "value_10000", at its hint (which
        // moves along with them), then are undone; rounds with the gaps and
        // without them take turns after one to warm up, and the fastest of
        // each is compared, so what the window went through before (other
        // suites) weighs on both alike.
        MCSourceStorage *ts = (MCSourceStorage *)big.textStorage;
        const std::vector<MCTextGap> saved = ts.gaps;
        auto round = ^double(BOOL withGaps, NSString *keys) {
            ts.gaps = withGaps ? saved : std::vector<MCTextGap>();
            big.selectedRange = NSMakeRange(at + 11, 0);
            const double ms = typeTimed(keys);
            for (NSUInteger i = 0; i < keys.length; i++) [big.undoManager undo];
            return ms;
        };
        round(YES, [typed substringToIndex:50]);
        double hinted = 1e9, plain = 1e9;
        for (int i = 0; i < 3; i++) {
            hinted = MIN(hinted, round(YES, typed));
            plain = MIN(plain, round(NO, typed));
        }
        ts.gaps = saved;
        GCHECK([[big.string substringWithRange:NSMakeRange(at, 13)] isEqualToString:@"value_10000 ="]);
        const double extra = (hinted - plain) / typed.length;
        MCGuiNote(@"200 keystrokes: %.1f ms of CPU with no hints, %.1f ms with them "
                  @"(%.3f ms a keystroke more)", plain, hinted, extra);
        // The plan's bound: a hint costs a keystroke under a millisecond.
        MCGuiCheck(extra < 1.0, [NSString stringWithFormat:
                       @"hints add under 1 ms of CPU a keystroke (%.3f)", extra],
                   __FILE__, __LINE__);
        BOOL kept = NO;
        for (NSDictionary *h in big.inlayHints.shownHints)
            if ([h[@"offset"] unsignedIntegerValue] == at + 11 && [h[@"label"] isEqualToString:@": float"])
                kept = YES;
        GCHECK(kept);
        [big.inlayHints clear];
    });

    // ------------------------------------------------ a real basedpyright
    if (!NSProcessInfo.processInfo.environment[@"MC_GUI_REAL_BASEDPYRIGHT"].length) return;
    NSString *real = MCGuiProjectFile(@"inlay-real/area.py");
    MCGuiStep(@"the same file under basedpyright", ^{
        NSString *exe = MCFindProgram(@"basedpyright-langserver");
        GCHECK(exe.length > 0);
        MCGuiWriteFile(real, kSource);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@ --stdio\nlsp.python.also = off\n", exe]);
        GCHECK([c showFile:real]);
    });
    MCGuiWait(@"basedpyright's hints arrive", 60, ^BOOL { return Text(c).inlayHints.count > 0; });
    MCGuiStep(@"and are placed", ^{
        MCGuiNote(@"basedpyright: %@", Text(c).inlayHints.shownHints);
        GCHECK_EQ_STR(Text(c).string, kSource);
        GCHECK([Labels(c) count] >= 2);
    });
}
