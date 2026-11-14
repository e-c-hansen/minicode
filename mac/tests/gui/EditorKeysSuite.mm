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
#import "Harness.h"
#import "EditorController.h"
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
    MCGuiStep(@"the registry and the hints say so", ^{
        GCHECK_EQ_STR(MCKeymapGlyphs(@"cursorTop"), @"⌘↑");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"cursorBottomSelect"), @"⇧⌘↓");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"editor.action.moveLinesDownAction"), @"⌥↓");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"editor.action.copyLinesUpAction"), @"⌥⇧↑");
        NSAttributedString *hints = [c valueForKey:@"hintsText"];
        GCHECK([hints.string containsString:@"Start of the file"]);
    });
}
