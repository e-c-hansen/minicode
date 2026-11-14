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
// Then the caret and the pointer beside them (the owner's report: the
// caret moved oddly around them): a click anywhere in a hint's room puts
// the caret at the hint's offset, and the caret is drawn where the next
// character goes, before a hint that follows the word before it and after
// one that goes with what follows, with clicks made the way the window
// server makes them. A double-click on a hint writes its textEdits (an
// import too) as one undo step; one without edits selects a word; one
// whose text was typed before since goes where the hint is now. VS Code's
// on-unless-pressed and off-unless-pressed, and View > Inlay Hints.
//
// Where basedpyright is installed, its hints for a file arrive and are
// placed, a click beside one lands at it and a double-click writes it.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "LspInlayHints.h"
#import "SourceStorage.h"
#include <pwd.h>
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

NSDictionary *Edit(int line, int ch, NSString *text) {
    return @{@"range": @{@"start": @{@"line": @(line), @"character": @(ch)},
                         @"end": @{@"line": @(line), @"character": @(ch)}},
             @"newText": text};
}

NSDictionary *WithEdits(NSDictionary *hint, NSArray *edits) {
    NSMutableDictionary *m = [hint mutableCopy];
    m[@"textEdits"] = edits;
    return m;
}

// A mouse event as the window server makes them, at a point in the window
// (MultiCaretClickSuite.mm says why the view is handed it directly).
NSEvent *Mouse(CGEventType type, NSPoint inWindow, int64_t clicks) {
    const CGFloat h = NSMaxY(NSScreen.screens.firstObject.frame);
    CGEventRef cg = CGEventCreateMouseEvent(NULL, type, CGPointMake(inWindow.x, h - inWindow.y),
                                            kCGMouseButtonLeft);
    CGEventSetIntegerValueField(cg, kCGMouseEventClickState, clicks);
    NSEvent *e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    return e;
}

// A click (clickCount 1 or 2) at a point in the text view: the release is
// queued for its tracking loop and the press goes to the view the window's
// hit test finds there.
void Click(NSTextView *tv, NSPoint inView, int64_t clicks) {
    const NSPoint w = [tv convertPoint:inView toView:nil];
    [NSApp postEvent:Mouse(kCGEventLeftMouseUp, w, clicks) atStart:NO];
    NSView *hit = [tv.window.contentView.superview hitTest:w];
    [hit mouseDown:Mouse(kCGEventLeftMouseDown, w, clicks)];
}

void DoubleClick(NSTextView *tv, NSPoint inView) {
    Click(tv, inView, 1);
    Click(tv, inView, 2);
}

// Where the caret is drawn: the x of the text view's insertion indicator
// (macOS 14 and later), NAN without one.
CGFloat CaretX(NSTextView *tv) {
    [tv updateInsertionPointStateAndRestartTimer:NO];
    Class cls = NSClassFromString(@"NSTextInsertionIndicator");
    for (NSView *v in tv.subviews)
        if (cls && [v isKindOfClass:cls]) return NSMinX(v.frame);
    return NAN;
}

NSUInteger IndexOf(EditorController *c, NSString *label) {
    NSArray *shown = Text(c).inlayHints.shownHints;
    for (NSUInteger i = 0; i < shown.count; i++)
        if ([shown[i][@"label"] isEqualToString:label]) return i;
    return NSNotFound;
}

NSString *BasedPyright() {
    NSMutableArray *dirs = [[NSProcessInfo.processInfo.environment[@"PATH"]
        componentsSeparatedByString:@":"] mutableCopy];
    struct passwd *pw = getpwuid(getuid());   // HOME is the scratch one here
    if (pw) [dirs addObject:[@(pw->pw_dir) stringByAppendingPathComponent:@".local/bin"]];
    for (NSString *d in dirs) {
        NSString *p = [d stringByAppendingPathComponent:@"basedpyright-langserver"];
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:p]) return p;
    }
    return nil;
}

NSMenuItem *ViewMenuItem(NSString *title) {
    for (NSMenuItem *top in NSApp.mainMenu.itemArray)
        if ([top.submenu.title isEqualToString:@"View"] || [top.title isEqualToString:@"View"])
            for (NSMenuItem *item in top.submenu.itemArray)
                if ([item.title isEqualToString:title]) return item;
    return nil;
}

NSString *const kAccept = @"def area(width, height):\n"
                          @"    return width * height\n"
                          @"\n"
                          @"items = [n for n in range(3)]\n"
                          @"total = area(3, 4)\n";

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
    // As basedpyright sends them: a return type with an import, a
    // variable's type and a parameter name with their text, and a
    // parameter name with none.
    NSArray *acceptHints = @[
        WithEdits(Hint(0, 23, @"-> Any", 1, YES, NO),
                  @[Edit(0, 23, @" -> Any"), Edit(0, 0, @"from typing import Any\n\n\n")]),
        WithEdits(Hint(3, 5, @": list[int]", 1, NO, NO), @[Edit(3, 5, @": list[int]")]),
        WithEdits(Hint(4, 13, @"width=", 2, NO, YES), @[Edit(4, 13, @"width=")]),
        Hint(4, 16, @"height=", 2, NO, YES)];
    NSDictionary *script = @{
        @"name": @"fakelsp",
        @"capabilities": @{@"textDocumentSync": @{@"openClose": @YES, @"change": @1},
                           @"inlayHintProvider": @YES},
        @"rules": @[
            @{@"method": @"textDocument/inlayHint",
              @"when": @{@"uriEndsWith": @"accept.py", @"textContains": @"items = [n"},
              @"result": acceptHints},
            @{@"method": @"textDocument/inlayHint", @"when": @{@"uriEndsWith": @"accept.py"},
              @"result": @[]},
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
        // A point anywhere in the gap is the hint's offset.
        const NSRect box = [tv.inlayHints rectOfHintAtIndex:1];
        for (CGFloat x : {NSMinX(box) + 1, NSMidX(box), NSMaxX(box) - 1})
            GCHECK([tv characterIndexForInsertionAtPoint:NSMakePoint(x, NSMidY(box))] == line3 + 5);
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

    // ------------------------------------------------ at a line's start
    // A hint at offset 0 and one at a line's start (an argument on a
    // continuation line) were dropped: a kern on the line break before
    // them opens no room. They are the first-line indent of their
    // paragraph now, laid out only, never stored, never typed with.
    MCGuiStep(@"hints at offset 0, at a line's start and on an empty line open room there", ^{
        CodeTextView *tv = Text(c);
        NSString *text = tv.string;
        const NSUInteger empty = [text rangeOfString:@"\n\n"].location + 1;   // line 2
        NSLayoutManager *lm = tv.layoutManager;
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, text.length)];
        const CGFloat d0 = GlyphX(tv, 0), t0 = GlyphX(tv, line3), e0 = GlyphX(tv, empty);
        const CGFloat s0 = GlyphX(tv, line3 + 6);   // "=" moves with its line
        const NSUInteger line1 = [text rangeOfString:@"    return"].location;
        const CGFloat r0 = GlyphX(tv, line1 + 4);
        std::vector<Lsp::InlayHint> hints(3);
        hints[0].label = "first:";
        hints[1].label = "args:";
        hints[1].paddingRight = true;
        hints[2].label = "empty";
        std::vector<Lsp::PlacedInlayHint> placed(3);
        placed[0].offset = 0;
        placed[0].hint = 0;
        placed[1].offset = line3;
        placed[1].hint = 1;
        placed[2].offset = empty;
        placed[2].hint = 2;
        [tv.inlayHints showHints:hints placed:placed];
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, text.length)];
        GCHECK(tv.inlayHints.count == 3);
        const CGFloat d1 = GlyphX(tv, 0), t1 = GlyphX(tv, line3), e1 = GlyphX(tv, empty);
        // Gaps sorted by offset: 0, the empty line, line 3.
        const NSRect b0 = [tv.inlayHints rectOfHintAtIndex:0];
        const NSRect be = [tv.inlayHints rectOfHintAtIndex:1];
        const NSRect b3 = [tv.inlayHints rectOfHintAtIndex:2];
        MCGuiNote(@"line 0's first glyph %.1f -> %.1f (box %.1f to %.1f); line 3's %.1f -> %.1f "
                  @"(box %.1f to %.1f); the empty line's %.1f -> %.1f", d0, d1, NSMinX(b0),
                  NSMaxX(b0), t0, t1, NSMinX(b3), NSMaxX(b3), e0, e1);
        GCHECK(NSWidth(b0) > 0 && NSWidth(b3) > 0 && NSWidth(be) > 0);
        GCHECK(d1 - d0 >= NSWidth(b0) - 0.5);
        GCHECK(NSMinX(b0) >= d0 - 0.5 && NSMaxX(b0) <= d1 + 0.5);
        GCHECK(t1 - t0 >= NSWidth(b3) - 0.5);
        GCHECK(NSMinX(b3) >= t0 - 0.5 && NSMaxX(b3) < t1 - 1);   // padded on its right
        GCHECK(GlyphX(tv, line3 + 6) - s0 > NSWidth(b3) - 0.5);
        GCHECK(NSMinX(be) >= e0 - 0.5 && e1 - e0 >= NSWidth(be) - 0.5);
        // A line with no hint is where it was.
        GCHECK(fabs(GlyphX(tv, line1 + 4) - r0) < 0.5);
        // Nothing is stored: no indent in the stored paragraph styles.
        MCSourceStorage *ts = (MCSourceStorage *)tv.textStorage;
        for (NSUInteger at : {(NSUInteger)0, line3, empty, line3 + 3}) {
            NSParagraphStyle *ps = [ts storedAttributesAtIndex:at longestEffectiveRange:NULL
                                                       inRange:NSMakeRange(at, 1)]
                                       [NSParagraphStyleAttributeName];
            GCHECK(ps.firstLineHeadIndent == 0);
        }
        // Typed at the line's start, the character takes no indent; the
        // hint stays before it (it goes with what follows), and fixing the
        // edited paragraph's attributes stores none either.
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(line3, 0);
        NSParagraphStyle *typing = tv.typingAttributes[NSParagraphStyleAttributeName];
        GCHECK(typing.firstLineHeadIndent == 0);
        TypeAt(tv, line3, 0, @"x");
        GCHECK(OffsetOf(c, @"args:") == line3);
        for (NSUInteger at = line3; at < line3 + 8; at++) {
            NSParagraphStyle *ps = [ts storedAttributesAtIndex:at longestEffectiveRange:NULL
                                                       inRange:NSMakeRange(at, 1)]
                                       [NSParagraphStyleAttributeName];
            GCHECK(ps.firstLineHeadIndent == 0);
        }
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        GCHECK(fabs(GlyphX(tv, line3) - t1) < 0.5);   // the typed x, after the room
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, text);
        // A copy carries no indent.
        tv.selectedRange = NSMakeRange(line3, 18);
        [tv copy:nil];
        NSData *rtf = [[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypeRTF];
        if (rtf) {
            NSAttributedString *a = [[NSAttributedString alloc] initWithRTF:rtf documentAttributes:NULL];
            NSParagraphStyle *ps = [a attribute:NSParagraphStyleAttributeName atIndex:0
                                 effectiveRange:NULL];
            GCHECK(ps.firstLineHeadIndent == 0);
        }
        MCGuiSnapshot(tv, @"inlay-hints-line-start");
        [tv.inlayHints clear];
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        GCHECK(fabs(GlyphX(tv, 0) - d0) < 0.5 && fabs(GlyphX(tv, line3) - t0) < 0.5);
    });

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
        // About 10 ms alone, over 100 after the other suites; laying out
        // the whole file would take seconds.
        MCGuiCheck(showMs < 400, [NSString stringWithFormat:
                       @"showing the hints of 400 lines takes under 400 ms of CPU (%.1f)", showMs],
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

    // --------------------------------------- the caret and the pointer beside them
    NSString *acceptFile = MCGuiProjectFile(@"inlay/accept.py");
    __block NSUInteger items = 0, total = 0;
    __block CGFloat cell = 0;
    MCGuiStep(@"a file whose hints carry their text", ^{
        MCGuiWriteFile(acceptFile, kAccept);
        GCHECK([c showFile:acceptFile]);
        items = [kAccept rangeOfString:@"items"].location;
        total = [kAccept rangeOfString:@"total"].location;
    });
    MCGuiWait(@"its four hints", 20, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"a click in the left half of a hint's room lands at the hint, not before the word's end", ^{
        CodeTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        cell = GlyphX(tv, items + 2) - GlyphX(tv, items + 1);
        const NSRect list = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @": list[int]")];
        tv.selectedRange = NSMakeRange(0, 0);
        Click(tv, NSMakePoint(NSMinX(list) + 1, NSMidY(list)), 1);
        MCGuiNote(@"a click at the left of \": list[int]\" put the caret at %lu (items ends at %lu)",
                  (unsigned long)tv.selectedRange.location, (unsigned long)(items + 5));
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(items + 5, 0)));
        // ...drawn where the next character goes: right after "items",
        // before the hint, which typing pushes along.
        const CGFloat x = CaretX(tv);
        if (!isnan(x)) {
            MCGuiNote(@"the caret at x %.1f; s at %.1f, the label %.1f to %.1f", x,
                      GlyphX(tv, items + 4), NSMinX(list), NSMaxX(list));
            GCHECK(x > GlyphX(tv, items + 4) + cell / 2 && x < NSMinX(list) + 1);
        }
        tv.selectedRange = NSMakeRange(0, 0);
        Click(tv, NSMakePoint(NSMaxX(list) - 1, NSMidY(list)), 1);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(items + 5, 0)));
        // A parameter's name: a click on its left lands at the argument,
        // not before the "(", and the caret is drawn after the name, where
        // typing goes.
        const NSRect width = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @"width=")];
        Click(tv, NSMakePoint(NSMinX(width) + 1, NSMidY(width)), 1);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(total + 13, 0)));
        const CGFloat wx = CaretX(tv);
        if (!isnan(wx)) GCHECK(wx > NSMaxX(width) - 1 && wx < GlyphX(tv, total + 13) + 1);
        // A click on a character's own left half is as ever.
        Click(tv, NSMakePoint(GlyphX(tv, items + 2) + 1, NSMidY(list)), 1);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(items + 2, 0)));
        // A drag from the hint's room back to the line's start selects the
        // whole word, not all but its last letter.
        const NSPoint from = [tv convertPoint:NSMakePoint(NSMinX(list) + 2, NSMidY(list)) toView:nil];
        const NSPoint to = [tv convertPoint:NSMakePoint(GlyphX(tv, items) + 1, NSMidY(list)) toView:nil];
        [NSApp postEvent:Mouse(kCGEventLeftMouseDragged, to, 1) atStart:NO];
        [NSApp postEvent:Mouse(kCGEventLeftMouseUp, to, 1) atStart:NO];
        [[tv.window.contentView.superview hitTest:from] mouseDown:Mouse(kCGEventLeftMouseDown, from, 1)];
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(items, 5)));
    });
    MCGuiStep(@"the arrows put the caret before the hint at the word's end, and after it past", ^{
        CodeTextView *tv = Text(c);
        const NSRect list = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @": list[int]")];
        tv.selectedRange = NSMakeRange(items, 0);
        for (int i = 0; i < 5; i++) [tv moveRight:nil];
        GCHECK(tv.selectedRange.location == items + 5);
        const CGFloat before = CaretX(tv);
        [tv moveRight:nil];
        GCHECK(tv.selectedRange.location == items + 6);
        const CGFloat past = CaretX(tv);
        [tv moveLeft:nil];
        const CGFloat back = CaretX(tv);
        if (!isnan(before)) {
            MCGuiNote(@"the caret at the word's end %.1f, one right %.1f, back %.1f", before, past, back);
            GCHECK(before < NSMinX(list) + 1);
            GCHECK(past > NSMaxX(list) && fabs(past - GlyphX(tv, items + 6)) < 1.5);
            GCHECK(fabs(back - before) < 0.5);
        }
        // Typed there, the letter goes before the hint and the caret with it.
        TypeAt(tv, items + 5, 0, @"x");
        GCHECK(OffsetOf(c, @": list[int]") == items + 6);
        GCHECK(tv.selectedRange.location == items + 6);
        const NSRect moved = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @": list[int]")];
        const CGFloat typed = CaretX(tv);
        if (!isnan(typed)) GCHECK(typed < NSMinX(moved) + 1 && typed > GlyphX(tv, items + 5));
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kAccept);
        // Hints gone, the caret is where the layout puts it again.
        tv.selectedRange = NSMakeRange(items + 5, 0);
        [tv.inlayHints clear];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        const CGFloat plain = CaretX(tv);
        if (!isnan(plain)) GCHECK(fabs(plain - GlyphX(tv, items + 5)) < 1.5);
        // Home and End are as ever.
        [tv moveToEndOfLine:nil];
        GCHECK(tv.selectedRange.location == items + 29);
        [tv moveToBeginningOfLine:nil];
        GCHECK(tv.selectedRange.location == items);
        // They come back with a nudge (an edit and its undo ask again).
        TypeAt(tv, 0, 0, @"#");
        [tv.undoManager undo];
    });
    MCGuiWait(@"the hints back", 10, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"a double-click on a type writes it, the caret after it, as one undo step", ^{
        CodeTextView *tv = Text(c);
        const NSRect list = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @": list[int]")];
        DoubleClick(tv, NSMakePoint(NSMidX(list), NSMidY(list)));
        GCHECK([tv.string containsString:@"\nitems: list[int] = [n for n in range(3)]\n"]);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(items + 5 + 11, 0)));
        GCHECK(IndexOf(c, @": list[int]") == NSNotFound);   // gone at once
        GCHECK([tv.undoManager.undoActionName isEqualToString:@"Insert Inlay Hint"]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kAccept);
    });
    MCGuiWait(@"the hints back", 10, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"one with an import writes both in one step", ^{
        CodeTextView *tv = Text(c);
        const NSRect any = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @"-> Any")];
        DoubleClick(tv, NSMakePoint(NSMidX(any), NSMidY(any)));
        GCHECK([tv.string hasPrefix:@"from typing import Any\n\n\ndef area(width, height) -> Any:\n"]);
        NSString *after = tv.string;
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kAccept);
        [tv.undoManager redo];
        GCHECK_EQ_STR(tv.string, after);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kAccept);
    });
    MCGuiWait(@"the hints back", 10, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"one with nothing to write leaves the text; one typed before since goes where it is", ^{
        CodeTextView *tv = Text(c);
        const NSRect height = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @"height=")];
        DoubleClick(tv, NSMakePoint(NSMidX(height), NSMidY(height)));
        GCHECK_EQ_STR(tv.string, kAccept);
        GCHECK(IndexOf(c, @"height=") != NSNotFound);
        // Typed before it, at once (before the server is asked again).
        TypeAt(tv, items, 0, @"z");
        GCHECK(OffsetOf(c, @": list[int]") == items + 6);
        const NSRect list = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @": list[int]")];
        DoubleClick(tv, NSMakePoint(NSMidX(list), NSMidY(list)));
        GCHECK([tv.string containsString:@"\nzitems: list[int] = [n"]);
        [tv.undoManager undo];
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kAccept);
    });
    MCGuiWait(@"the hints back", 10, ^BOOL { return Text(c).inlayHints.count == 4; });

    // ---------------------------------------- unless pressed, and the menu
    MCGuiStep(@"on-unless-pressed: shown, and gone while Control+Option are held", ^{
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n"
            @"editor.inlay-hints = on-unless-pressed\n", cmd]);
        GCHECK(Session(c).inlayHintsShown);
        GCHECK(Text(c).inlayHints.count == 4);
        MCInlayHintsSetPressed(YES);
        GCHECK(!Session(c).inlayHintsShown);
        GCHECK(Text(c).inlayHints.count == 0);
        MCInlayHintsSetPressed(NO);
    });
    MCGuiWait(@"back once let go", 10, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"off-unless-pressed (VS Code's spelling): none", ^{
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n"
            @"editor.inlay-hints = offUnlessPressed\n", cmd]);
        GCHECK(Text(c).inlayHints.count == 0);
        MCInlayHintsSetPressed(YES);
    });
    MCGuiWait(@"shown while held", 10, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"and gone when let go", ^{
        MCInlayHintsSetPressed(NO);
        GCHECK(Text(c).inlayHints.count == 0);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
    });
    MCGuiWait(@"on again", 10, ^BOOL { return Text(c).inlayHints.count == 4; });
    MCGuiStep(@"View > Inlay Hints, checked, turns them off in the settings file", ^{
        NSMenuItem *item = ViewMenuItem(@"Inlay Hints");
        GCHECK(item != nil);
        if (!item) return;
        GCHECK([(id)item.target validateMenuItem:item] && item.state == NSControlStateValueOn);
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
        NSString *file = [NSString stringWithContentsOfFile:
            NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"]
                                                  encoding:NSUTF8StringEncoding error:NULL];
        GCHECK([file containsString:@"editor.inlay-hints = off"]);
    });
    MCGuiWait(@"they go", 10, ^BOOL { return Text(c).inlayHints.count == 0; });
    MCGuiStep(@"unchecked; again turns them on", ^{
        NSMenuItem *item = ViewMenuItem(@"Inlay Hints");
        GCHECK([(id)item.target validateMenuItem:item] && item.state == NSControlStateValueOff);
        GCHECK([NSApp sendAction:item.action to:item.target from:item]);
    });
    MCGuiWait(@"they come back", 10, ^BOOL { return Text(c).inlayHints.count == 4; });

    // ------------------------------------------------ a real basedpyright
    NSString *bp = BasedPyright();
    if (!bp) {
        MCGuiStep(@"basedpyright is not installed: only the scripted server", ^{
            MCGuiNote(@"basedpyright-langserver not found; the checks ran against fakelsp only");
        });
        return;
    }
    NSString *real = MCGuiProjectFile(@"inlay-real/area.py");
    NSString *const kReal = [kSource stringByAppendingString:@"values = [n for n in range(3)]\n"];
    MCGuiStep(@"the same file under basedpyright", ^{
        MCGuiWriteFile(real, kReal);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@ --stdio\nlsp.python.also = off\n", bp]);
        GCHECK([c showFile:real]);
    });
    MCGuiWait(@"basedpyright's hints arrive", 60, ^BOOL {
        return IndexOf(c, @": list[int]") != NSNotFound;
    });
    MCGuiStep(@"placed; a click beside one lands at it; a double-click writes it", ^{
        CodeTextView *tv = Text(c);
        MCGuiNote(@"basedpyright: %@", tv.inlayHints.shownHints);
        GCHECK_EQ_STR(tv.string, kReal);
        GCHECK([Labels(c) count] >= 2);
        const NSUInteger v = [kReal rangeOfString:@"values"].location;
        GCHECK(OffsetOf(c, @": list[int]") == v + 6);
        [c.window makeFirstResponder:tv];
        [tv.layoutManager ensureLayoutForCharacterRange:NSMakeRange(0, tv.string.length)];
        const NSRect list = [tv.inlayHints rectOfHintAtIndex:IndexOf(c, @": list[int]")];
        Click(tv, NSMakePoint(NSMinX(list) + 1, NSMidY(list)), 1);
        GCHECK(NSEqualRanges(tv.selectedRange, NSMakeRange(v + 6, 0)));
        DoubleClick(tv, NSMakePoint(NSMidX(list), NSMidY(list)));
        GCHECK([tv.string containsString:@"\nvalues: list[int] = [n for n in range(3)]\n"]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, kReal);
    });
    MCGuiStep(@"language servers off again", ^{ MCGuiSetSettings(@"lsp.enabled = false\n"); });
}
