// EditorKeysSuite.mm — `make guitest SUITES=editor_keys`: the owner's two
// daily annoyances with the editor's keys (2026-11-08).
//
// Words: a double-click on arr in Python's arr.size selects arr (AppKit's
// words keep a period between letters), as do a drag by words and the
// Option arrows, by VS Code's default editor.wordSeparators
// (MultiCaret::wordSelection, MCSourceStorage); a read-only page keeps
// AppKit's words. The press is a CGEvent-made double-click handed to the
// view the window's hit test finds, as MultiCaretClickSuite does, since an
// offscreen window delivers no mouse-down itself.
//
// Cmd+Up and Cmd+Down go to the file's start and end, Shift selecting, as
// in VS Code on the Mac (with several carets, one is left); Move Line stays
// on Option+Up/Down and Copy Line on Shift+Option+Up/Down. The keys are
// CGEvent-made key presses to the text view's keyDown:, after checking no
// menu item or the key monitor takes them first; a Command arrow, which an
// offscreen text view never interprets, goes in as the selector AppKit's
// key bindings make of it (see Press).
//
// Home and End go to the line's start (its first non-blank character,
// then column 0) and end, Shift selecting, at every caret, and with word
// wrap to the row's end first, as VS Code's cursorHome and cursorEnd.
#import "Harness.h"
#import "EditorController.h"
#import "EditorFont.h"
#import "Keymap.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import <Carbon/Carbon.h>

namespace {

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }

CGEventFlags CGFlagsFor(NSEventModifierFlags f) {
    CGEventFlags g = 0;
    if (f & NSEventModifierFlagCommand) g |= kCGEventFlagMaskCommand;
    if (f & NSEventModifierFlagShift) g |= kCGEventFlagMaskShift;
    if (f & NSEventModifierFlagOption) g |= kCGEventFlagMaskAlternate;
    if (f & NSEventModifierFlagControl) g |= kCGEventFlagMaskControl;
    return g;
}

// A key press made as the hardware makes it.
NSEvent *KeyEvent(CGKeyCode code, NSEventModifierFlags flags) {
    CGEventRef ce = CGEventCreateKeyboardEvent(NULL, code, true);
    CGEventSetFlags(ce, CGFlagsFor(flags) | (CGEventGetFlags(ce) & (kCGEventFlagMaskSecondaryFn |
                                                                     kCGEventFlagMaskNumericPad)));
    NSEvent *e = [NSEvent eventWithCGEvent:ce];
    CFRelease(ce);
    return e;
}

// The press as it reaches the editor: neither the key monitor nor a menu
// item takes it (checked), then the view's keyDown:.
BOOL Press(NSWindow *w, CodeTextView *tv, CGKeyCode code, NSEventModifierFlags flags) {
    NSEvent *e = KeyEvent(code, flags);
    if (MCKeymapHandleKeyDown(e)) return NO;
    if (MCGuiPerformMenuKey(w, code, flags, nil, nil)) return NO;
    // The text input context is active only in a key window on screen, and
    // NSTextView interprets no key without it.
    [tv.inputContext activate];
    const BOOL cmdArrow = (flags & NSEventModifierFlagCommand) &&
                          (code == kVK_UpArrow || code == kVK_DownArrow);
    if (!cmdArrow) {
        MCGuiUndoGroup(tv.undoManager, ^{ [tv keyDown:e]; });
        return YES;
    }
    // Offscreen, even then a Command key is never interpreted (a bare
    // NSTextView does nothing with it either), so the selector AppKit's
    // standard key bindings give Cmd+Up and Cmd+Down (and with Shift) goes
    // in as interpretKeyEvents: would send it, the press recorded as
    // CodeTextView's keyDown: records it: what the old Move Line took.
    const BOOL shift = (flags & NSEventModifierFlagShift) != 0, up = code == kVK_UpArrow;
    SEL sel = up ? (shift ? @selector(moveToBeginningOfDocumentAndModifySelection:)
                          : @selector(moveToBeginningOfDocument:))
                 : (shift ? @selector(moveToEndOfDocumentAndModifySelection:)
                          : @selector(moveToEndOfDocument:));
    [tv setValue:e forKey:@"keyEvent"];
    MCGuiUndoGroup(tv.undoManager, ^{ [tv doCommandBySelector:sel]; });
    [tv setValue:nil forKey:@"keyEvent"];
    return YES;
}

// The window point just inside the start of character i, at its line's
// middle.
NSPoint WindowPointAt(NSTextView *tv, NSUInteger i) {
    NSLayoutManager *lm = tv.layoutManager;
    [lm ensureLayoutForTextContainer:tv.textContainer];
    NSUInteger g = [lm glyphIndexForCharacterAtIndex:i];
    NSRect line = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:NULL];
    NSPoint loc = [lm locationForGlyphAtIndex:g];
    NSPoint p = NSMakePoint(line.origin.x + loc.x + tv.textContainerOrigin.x + 1,
                            NSMidY(line) + tv.textContainerOrigin.y);
    return [tv convertPoint:p toView:nil];
}

NSEvent *Mouse(CGEventType type, NSPoint inWindow, int64_t clicks) {
    const CGFloat h = NSMaxY(NSScreen.screens.firstObject.frame);
    CGEventRef cg = CGEventCreateMouseEvent(NULL, type, CGPointMake(inWindow.x, h - inWindow.y),
                                            kCGMouseButtonLeft);
    CGEventSetIntegerValueField(cg, kCGMouseEventClickState, clicks);
    NSEvent *e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    return e;
}

// A double-click at `at`, dragged to `to` (or not), released there: the
// drag and release are queued for the view's tracking loop.
void DoubleClick(NSWindow *w, NSPoint at, const NSPoint *to) {
    if (to) [NSApp postEvent:Mouse(kCGEventLeftMouseDragged, *to, 2) atStart:NO];
    [NSApp postEvent:Mouse(kCGEventLeftMouseUp, to ? *to : at, 2) atStart:NO];
    NSView *hit = [w.contentView.superview hitTest:at];
    [hit mouseDown:Mouse(kCGEventLeftMouseDown, at, 2)];
}

NSString *Sel(NSTextView *tv) {
    NSArray *ranges = [tv isKindOfClass:CodeTextView.class] && ((CodeTextView *)tv).multiCarets.active
                          ? ((CodeTextView *)tv).multiCarets.caretRanges
                          : tv.selectedRanges;
    NSMutableArray *out = [NSMutableArray array];
    for (NSValue *v in ranges) [out addObject:NSStringFromRange(v.rangeValue)];
    return [out componentsJoinedByString:@" "];
}

}  // namespace

MC_GUI_SUITE(editor_keys) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    NSString *py = MCGuiProjectFile(@"words.py");
    // n = arr.size   (arr is 4..7, size 8..12)
    NSString *text = @"n = arr.size\nx = $el.value_2 + 1\ndone\n";
    MCGuiStep(@"a Python file", ^{
        MCGuiWriteFile(py, text);
        c = MCGuiController();
        GCHECK([c showFile:py]);
        tv = Editor(c);
        [c.window makeFirstResponder:tv];
        GCHECK(tv.editable);
    });

    // ------------------------------------------------------------ words
    MCGuiStep(@"a double-click on arr selects arr, not arr.size", ^{
        NSPoint p = WindowPointAt(tv, 5);
        NSView *hit = [c.window.contentView.superview hitTest:p];
        GCHECK(hit == tv);
        DoubleClick(c.window, p, nullptr);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(4, 3)));
        DoubleClick(c.window, WindowPointAt(tv, 9), nullptr);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(8, 4)));
        // $ is a separator and _ is not: $el.value_2 gives el and value_2.
        NSRange el = [tv.string rangeOfString:@"el."];
        DoubleClick(c.window, WindowPointAt(tv, el.location + 1), nullptr);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(el.location, 2)));
        NSRange value = [tv.string rangeOfString:@"value_2"];
        DoubleClick(c.window, WindowPointAt(tv, value.location + 5), nullptr);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(value));
    });
    MCGuiStep(@"a double-click dragged selects by these words", ^{
        const NSPoint to = WindowPointAt(tv, 9);
        DoubleClick(c.window, WindowPointAt(tv, 5), &to);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(4, 8)));
        // Dragged back from size to arr: still whole words.
        const NSPoint back = WindowPointAt(tv, 5);
        DoubleClick(c.window, WindowPointAt(tv, 10), &back);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(4, 8)));
    });
    MCGuiStep(@"Option+Right and Option+Left stop at the dot, Option+Delete too", ^{
        tv.selectedRange = NSMakeRange(4, 0);
        GCHECK(Press(c.window, tv, kVK_RightArrow, NSEventModifierFlagOption));
        GCHECK(tv.selectedRange.location == 7 && tv.selectedRange.length == 0);
        tv.selectedRange = NSMakeRange(12, 0);
        GCHECK(Press(c.window, tv, kVK_LeftArrow, NSEventModifierFlagOption));
        GCHECK(tv.selectedRange.location == 8);
        // Shift extends from where the selection began, AppKit keeping it.
        tv.selectedRange = NSMakeRange(12, 0);
        GCHECK(Press(c.window, tv, kVK_LeftArrow, NSEventModifierFlagOption | NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(8, 4)));
        tv.selectedRange = NSMakeRange(12, 0);
        GCHECK(Press(c.window, tv, kVK_Delete, NSEventModifierFlagOption));
        GCHECK([tv.string hasPrefix:@"n = arr.\n"]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, text);
    });
    MCGuiStep(@"with two carets the words are the same", ^{
        [tv.multiCarets setCaretRanges:@[[NSValue valueWithRange:NSMakeRange(4, 0)],
                                         [NSValue valueWithRange:NSMakeRange(18, 0)]]];
        GCHECK(tv.multiCarets.active);
        GCHECK(Press(c.window, tv, kVK_RightArrow, NSEventModifierFlagOption));
        GCHECK_EQ_STR(Sel(tv), @"{7, 0} {20, 0}");
        [tv.multiCarets clear];
        tv.selectedRange = NSMakeRange(0, 0);
    });
    MCGuiStep(@"a read-only page keeps AppKit's words", ^{
        tv.editable = NO;
        const NSRange r = [tv.textStorage doubleClickAtIndex:5];
        tv.editable = YES;
        GCHECK(r.location == 4 && r.length == 8);
        GCHECK(NSEqualRanges([tv.textStorage doubleClickAtIndex:5], NSMakeRange(4, 3)));
    });

    // ------------------------------------------------ Cmd+Up and Cmd+Down
    MCGuiStep(@"Cmd+Down and Cmd+Up go to the end and the start, the text unchanged", ^{
        tv.selectedRange = NSMakeRange(14, 0);
        GCHECK(Press(c.window, tv, kVK_DownArrow, NSEventModifierFlagCommand));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(text.length, 0)));
        GCHECK(Press(c.window, tv, kVK_UpArrow, NSEventModifierFlagCommand));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(0, 0)));
        GCHECK_EQ_STR(tv.string, text);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiStep(@"Shift+Cmd+Down and Shift+Cmd+Up select to there", ^{
        tv.selectedRange = NSMakeRange(14, 0);
        GCHECK(Press(c.window, tv, kVK_DownArrow, NSEventModifierFlagCommand | NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(14, text.length - 14)));
        tv.selectedRange = NSMakeRange(14, 0);
        GCHECK(Press(c.window, tv, kVK_UpArrow, NSEventModifierFlagCommand | NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(0, 14)));
        GCHECK_EQ_STR(tv.string, text);
    });
    MCGuiStep(@"with several carets: one caret, or one selection from the farthest", ^{
        NSArray *two = @[[NSValue valueWithRange:NSMakeRange(2, 0)],
                         [NSValue valueWithRange:NSMakeRange(15, 0)]];
        [tv.multiCarets setCaretRanges:two];
        GCHECK(Press(c.window, tv, kVK_DownArrow, NSEventModifierFlagCommand));
        GCHECK(!tv.multiCarets.active);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(text.length, 0)));
        [tv.multiCarets setCaretRanges:two];
        GCHECK(Press(c.window, tv, kVK_UpArrow, NSEventModifierFlagCommand));
        GCHECK(!tv.multiCarets.active);
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(0, 0)));
        [tv.multiCarets setCaretRanges:two];
        GCHECK(Press(c.window, tv, kVK_UpArrow, NSEventModifierFlagCommand | NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(0, 15)));
        [tv.multiCarets setCaretRanges:two];
        GCHECK(Press(c.window, tv, kVK_DownArrow, NSEventModifierFlagCommand | NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(2, text.length - 2)));
        GCHECK_EQ_STR(tv.string, text);
        tv.selectedRange = NSMakeRange(0, 0);
    });
    MCGuiStep(@"Option+Down moves the line and Shift+Option+Down copies it", ^{
        tv.selectedRange = NSMakeRange(1, 0);
        GCHECK(Press(c.window, tv, kVK_DownArrow, NSEventModifierFlagOption));
        GCHECK_EQ_STR(tv.string, @"x = $el.value_2 + 1\nn = arr.size\ndone\n");
        GCHECK(Press(c.window, tv, kVK_UpArrow, NSEventModifierFlagOption));
        GCHECK_EQ_STR(tv.string, text);
        tv.selectedRange = NSMakeRange(1, 0);
        GCHECK(Press(c.window, tv, kVK_DownArrow, NSEventModifierFlagOption | NSEventModifierFlagShift));
        GCHECK_EQ_STR(tv.string, [@"n = arr.size\n" stringByAppendingString:text]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, text);
        tv.selectedRange = NSMakeRange(14, 0);
        GCHECK(Press(c.window, tv, kVK_UpArrow, NSEventModifierFlagOption | NSEventModifierFlagShift));
        GCHECK_EQ_STR(tv.string, @"n = arr.size\nx = $el.value_2 + 1\nx = $el.value_2 + 1\ndone\n");
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, text);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });
    // ------------------------------------------------------- Home and End
    // VS Code's Mac keys (a grader's walk, 2026-11-18: Ctrl+G 18, End, Left
    // Left typed on line 17, AppKit's End having scrolled instead): Home to
    // the first non-blank character, then column 0; End to the line's end;
    // Shift selecting, from the same anchor as the Shift+arrows after.
    NSString *home = MCGuiProjectFile(@"home.py");
    // "def f():\n" is 0..8; "    return a + b" 9..24 (return at 13, its
    // break at 25); "    pass" 26..33 (pass at 30, break at 34).
    NSString *homeText = @"def f():\n    return a + b\n    pass\n";
    MCGuiStep(@"End goes to the line's end, Home to its first non-blank, then column 0", ^{
        MCGuiWriteFile(home, homeText);
        GCHECK([c showFile:home]);
        tv = Editor(c);
        [c.window makeFirstResponder:tv];
        GCHECK_EQ_STR(tv.string, homeText);
        tv.selectedRange = NSMakeRange(15, 0);
        GCHECK(Press(c.window, tv, kVK_End, 0));
        GCHECK_EQ_STR(Sel(tv), @"{25, 0}");
        GCHECK(Press(c.window, tv, kVK_Home, 0));
        GCHECK_EQ_STR(Sel(tv), @"{13, 0}");
        GCHECK(Press(c.window, tv, kVK_Home, 0));
        GCHECK_EQ_STR(Sel(tv), @"{9, 0}");
        GCHECK(Press(c.window, tv, kVK_Home, 0));
        GCHECK_EQ_STR(Sel(tv), @"{13, 0}");
        // The walk: from the line's start, End then Left twice stays on
        // the line.
        tv.selectedRange = NSMakeRange(9, 0);
        GCHECK(Press(c.window, tv, kVK_End, 0));
        GCHECK(Press(c.window, tv, kVK_LeftArrow, 0));
        GCHECK(Press(c.window, tv, kVK_LeftArrow, 0));
        GCHECK_EQ_STR(Sel(tv), @"{23, 0}");
        GCHECK_EQ_STR(tv.string, homeText);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiStep(@"Shift+End and Shift+Home select, keeping the anchor", ^{
        tv.selectedRange = NSMakeRange(15, 0);
        GCHECK(Press(c.window, tv, kVK_End, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{15, 10}");
        tv.selectedRange = NSMakeRange(15, 0);
        GCHECK(Press(c.window, tv, kVK_Home, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{13, 2}");
        GCHECK(Press(c.window, tv, kVK_Home, NSEventModifierFlagShift));   // column 0
        GCHECK_EQ_STR(Sel(tv), @"{9, 6}");
        // AppKit still extends from 15: Shift+Right takes a character back.
        GCHECK(Press(c.window, tv, kVK_RightArrow, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{10, 5}");
        // And Shift+End from there goes past the anchor to the line's end.
        GCHECK(Press(c.window, tv, kVK_End, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{15, 10}");
        GCHECK(Press(c.window, tv, kVK_LeftArrow, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{15, 9}");
        GCHECK_EQ_STR(tv.string, homeText);
    });
    MCGuiStep(@"Cmd+Home and Cmd+End go to the file's start and end, Shift selecting", ^{
        tv.selectedRange = NSMakeRange(15, 0);
        GCHECK(Press(c.window, tv, kVK_End, NSEventModifierFlagCommand));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(homeText.length, 0)));
        GCHECK(Press(c.window, tv, kVK_Home, NSEventModifierFlagCommand));
        GCHECK_EQ_STR(Sel(tv), @"{0, 0}");
        tv.selectedRange = NSMakeRange(15, 0);
        GCHECK(Press(c.window, tv, kVK_Home, NSEventModifierFlagCommand | NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{0, 15}");
        tv.selectedRange = NSMakeRange(15, 0);
        GCHECK(Press(c.window, tv, kVK_End, NSEventModifierFlagCommand | NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(15, homeText.length - 15)));
    });
    MCGuiStep(@"with several carets each moves, and Shift selects at each", ^{
        NSArray *two = @[[NSValue valueWithRange:NSMakeRange(15, 0)],
                         [NSValue valueWithRange:NSMakeRange(32, 0)]];
        [tv.multiCarets setCaretRanges:two];
        GCHECK(Press(c.window, tv, kVK_End, 0));
        GCHECK(tv.multiCarets.active);
        GCHECK_EQ_STR(Sel(tv), @"{25, 0} {34, 0}");
        GCHECK(Press(c.window, tv, kVK_Home, 0));
        GCHECK_EQ_STR(Sel(tv), @"{13, 0} {30, 0}");
        GCHECK(Press(c.window, tv, kVK_Home, 0));
        GCHECK_EQ_STR(Sel(tv), @"{9, 0} {26, 0}");
        [tv.multiCarets setCaretRanges:two];
        GCHECK(Press(c.window, tv, kVK_End, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{15, 10} {32, 2}");
        [tv.multiCarets setCaretRanges:two];
        GCHECK(Press(c.window, tv, kVK_Home, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), @"{13, 2} {30, 2}");
        GCHECK_EQ_STR(tv.string, homeText);
        [tv.multiCarets clear];
        tv.selectedRange = NSMakeRange(0, 0);
    });
    MCGuiStep(@"a wrapped line: End to the row's end first, Home to the row's start", ^{
        GCHECK(MCEditorWordWrap());
        NSMutableString *line = [NSMutableString string];
        while (line.length < 600) [line appendString:@"word "];
        [line appendString:@"end"];
        NSString *wrapped = [NSString stringWithFormat:@"x\n%@\ny\n", line];
        NSString *wrapFile = MCGuiProjectFile(@"wrapped.txt");
        MCGuiWriteFile(wrapFile, wrapped);
        GCHECK([c showFile:wrapFile]);
        tv = Editor(c);
        [c.window makeFirstResponder:tv];
        GCHECK_EQ_STR(tv.string, wrapped);
        const NSUInteger s = 2, e = 2 + line.length;
        NSLayoutManager *lm = tv.layoutManager;
        [lm ensureLayoutForTextContainer:tv.textContainer];
        NSRange g0, g1;
        [lm lineFragmentRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:s] effectiveRange:&g0];
        const NSRange row0 = [lm characterRangeForGlyphRange:g0 actualGlyphRange:NULL];
        [lm lineFragmentRectForGlyphAtIndex:NSMaxRange(g0) effectiveRange:&g1];
        const NSRange row1 = [lm characterRangeForGlyphRange:g1 actualGlyphRange:NULL];
        GCHECK_EQ_STR(NSStringFromRange(NSMakeRange(row0.location, 0)), @"{2, 0}");
        if (NSMaxRange(row0) >= e) MCGuiNote(@"the line did not wrap: rows %@ %@, line end %lu",
                                             NSStringFromRange(row0), NSStringFromRange(row1),
                                             (unsigned long)e);
        GCHECK(NSMaxRange(row0) < e && row1.location == NSMaxRange(row0));
        // End: the first row's end, before the space it broke at, then the
        // line's end.
        tv.selectedRange = NSMakeRange(s + 3, 0);
        GCHECK(Press(c.window, tv, kVK_End, 0));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(NSMaxRange(row0) - 1, 0)));
        GCHECK(Press(c.window, tv, kVK_End, 0));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(e, 0)));
        // Home on the second row: its start, then the line's start.
        tv.selectedRange = NSMakeRange(row1.location + 3, 0);
        GCHECK(Press(c.window, tv, kVK_Home, 0));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(row1.location, 0)));
        GCHECK(Press(c.window, tv, kVK_Home, 0));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(s, 0)));
        // Shift+End on the second row selects to that row's end.
        tv.selectedRange = NSMakeRange(row1.location + 3, 0);
        GCHECK(Press(c.window, tv, kVK_End, NSEventModifierFlagShift));
        GCHECK_EQ_STR(Sel(tv), NSStringFromRange(NSMakeRange(row1.location + 3,
                                                             NSMaxRange(row1) - 1 - row1.location - 3)));
        GCHECK_EQ_STR(tv.string, wrapped);
        // Back to the Python file for the steps after.
        GCHECK([c showFile:py]);
        tv = Editor(c);
        [c.window makeFirstResponder:tv];
        GCHECK_EQ_STR(tv.string, text);
        tv.selectedRange = NSMakeRange(0, 0);
    });
    // ------------------------------------------------------------ paste
    // No smart copy and paste: a word pasted over another after a dot is
    // just that word (AppKit's default made "n = arr. arr").
    MCGuiStep(@"copy arr, paste it over size: arr.arr, no space added", ^{
        tv.selectedRange = NSMakeRange(4, 3);   // arr
        [tv copy:nil];
        tv.selectedRange = NSMakeRange(8, 4);   // size
        MCGuiUndoGroup(tv.undoManager, ^{ [tv paste:nil]; });
        GCHECK_EQ_STR([tv.string substringToIndex:12], @"n = arr.arr\n");
        tv.selectedRange = NSMakeRange(4, 3);
        MCGuiUndoGroup(tv.undoManager, ^{ [tv cut:nil]; });
        GCHECK_EQ_STR([tv.string substringToIndex:9], @"n = .arr\n");   // the space stays too
        [tv.undoManager undo];
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, text);
    });
    MCGuiStep(@"the registry and the hints say so", ^{
        GCHECK_EQ_STR(MCKeymapGlyphs(@"cursorTop"), @"⌘↑");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"cursorBottomSelect"), @"⇧⌘↓");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"editor.action.moveLinesDownAction"), @"⌥↓");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"editor.action.copyLinesUpAction"), @"⌥⇧↑");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"cursorHome"), @"↖");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"cursorEndSelect"), @"⇧↘");
        NSAttributedString *hints = [c valueForKey:@"hintsText"];
        GCHECK([hints.string containsString:@"Start of the file"]);
        GCHECK([hints.string containsString:@"Start of the line"]);
    });
}
