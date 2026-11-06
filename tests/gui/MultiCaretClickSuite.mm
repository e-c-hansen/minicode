// MultiCaretClickSuite.mm — `make guitest SUITES=multicaretclick`: column
// carets made with the mouse (MultiCaretView.h): Cmd+Shift clicks in pairs,
// VS Code's Shift+Option box selection, Option clicks and drags, and the
// middle-button drag, with events made the way the window server makes
// them.
//
// The events are CGEvents (CGEventCreateMouseEvent) turned into NSEvents,
// with the click count a real mouse gives: the window server counts clicks
// by time alone (HIDClickSpace is (0,0) on this Mac), so a second click
// within half a second of the first is clickCount 2 wherever it lands. That
// is what broke the owner's Cmd+Shift clicks: the second came as a
// double-click and went to NSTextView. Each press is handed to the view the
// window's hit test finds, as NSWindow does, because a mouse-down never
// gets that far for a window that was never on screen: neither
// [window sendEvent:] nor CGEventPostToPid (whose events arrive with no
// window) reached the view here, even with isVisible, isActive and
// acceptsFirstMouse: answering YES.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import "MultiCaretView.h"

namespace {

CodeTextView *Editor(EditorController *c) { return [c valueForKey:@"textView"]; }

// Lines of 12 characters and a newline: line n, column k is 13 (n - 1) + k.
NSUInteger At(NSUInteger line, NSUInteger col) { return 13 * (line - 1) + col; }

// The point just inside the caret position before character i, in the
// window, at the middle of its line.
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

CGEventFlags CGFlagsFor(NSEventModifierFlags f) {
    CGEventFlags g = 0;
    if (f & NSEventModifierFlagCommand) g |= kCGEventFlagMaskCommand;
    if (f & NSEventModifierFlagShift) g |= kCGEventFlagMaskShift;
    if (f & NSEventModifierFlagOption) g |= kCGEventFlagMaskAlternate;
    if (f & NSEventModifierFlagControl) g |= kCGEventFlagMaskControl;
    return g;
}

// A mouse event as the hardware's are made. An event with no window has its
// location in screen coordinates, so the CGEvent is placed where the screen
// point equals the window point: the view then reads the window point.
NSEvent *Mouse(CGEventType type, NSPoint inWindow, NSEventModifierFlags flags, int64_t clicks,
               CGMouseButton button) {
    const CGFloat h = NSMaxY(NSScreen.screens.firstObject.frame);
    CGEventRef cg = CGEventCreateMouseEvent(NULL, type, CGPointMake(inWindow.x, h - inWindow.y),
                                            button);
    CGEventSetFlags(cg, CGFlagsFor(flags));
    CGEventSetIntegerValueField(cg, kCGMouseEventClickState, clicks);
    NSEvent *e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    return e;
}

// A press at `at`, dragged through `drags`, released at the last point:
// the release and drags are queued for the view's tracking loop, and the
// press goes to the view the window's hit test finds there. Returns that
// view.
NSView *Press(NSWindow *w, NSPoint at, NSEventModifierFlags flags, int64_t clicks,
              NSArray<NSValue *> *drags, BOOL middle) {
    const CGMouseButton b = middle ? kCGMouseButtonCenter : kCGMouseButtonLeft;
    NSPoint last = at;
    for (NSValue *v in drags) {
        last = v.pointValue;
        [NSApp postEvent:Mouse(middle ? kCGEventOtherMouseDragged : kCGEventLeftMouseDragged,
                               last, flags, clicks, b)
                 atStart:NO];
    }
    [NSApp postEvent:Mouse(middle ? kCGEventOtherMouseUp : kCGEventLeftMouseUp, last, flags,
                           clicks, b)
             atStart:NO];
    NSEvent *down = Mouse(middle ? kCGEventOtherMouseDown : kCGEventLeftMouseDown, at, flags,
                          clicks, b);
    NSView *hit = [w.contentView.superview hitTest:at];
    if (middle)
        [hit otherMouseDown:down];
    else
        [hit mouseDown:down];
    return hit;
}

NSString *Carets(CodeTextView *tv) {
    NSMutableArray *out = [NSMutableArray array];
    NSArray *ranges = tv.multiCarets.active ? tv.multiCarets.caretRanges : tv.selectedRanges;
    for (NSValue *v in ranges) {
        NSRange r = v.rangeValue;
        [out addObject:r.length ? [NSString stringWithFormat:@"%lu+%lu", r.location, r.length]
                                : [NSString stringWithFormat:@"%lu", r.location]];
    }
    return [out componentsJoinedByString:@" "];
}

NSValue *Pt(NSPoint p) { return [NSValue valueWithPoint:p]; }

}  // namespace

MC_GUI_SUITE(multicaretclick) {
    __block EditorController *c = nil;
    __block CodeTextView *tv = nil;
    __block NSString *original = nil;
    NSString *file = MCGuiProjectFile(@"columns.txt");
    NSMutableString *text = [NSMutableString string];
    for (int i = 1; i <= 12; i++) [text appendFormat:@"line %02d abcd\n", i];
    MCGuiWriteFile(file, text);
    // The window point of line n, column k.
    NSPoint (^P)(NSUInteger, NSUInteger) = ^NSPoint(NSUInteger line, NSUInteger col) {
        return WindowPointAt(tv, At(line, col));
    };
    const NSEventModifierFlags cmdShift = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    const NSEventModifierFlags optShift = NSEventModifierFlagOption | NSEventModifierFlagShift;
    const NSEventModifierFlags opt = NSEventModifierFlagOption;

    MCGuiStep(@"open a file", ^{
        c = MCGuiController();
        GCHECK([c showFile:file]);
        tv = Editor(c);
        [c.window makeFirstResponder:tv];
        GCHECK(tv.editable);
        original = [tv.string copy];
        NSEvent *e = Mouse(kCGEventLeftMouseDown, P(2, 5), cmdShift, 2, kCGMouseButtonLeft);
        GCHECK(NSEqualPoints(e.locationInWindow, P(2, 5)));
        GCHECK(e.clickCount == 2);
        GCHECK((e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask) == cmdShift);
    });

    // The owner's report: Cmd+Shift click one line, then Cmd+Shift click
    // another, quickly (within the double-click interval, so clickCount 2).
    MCGuiStep(@"Cmd+Shift click, then a quick Cmd+Shift click lines below", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        GCHECK(Press(c.window, P(2, 5), cmdShift, 1, nil, NO) == tv);
        GCHECK_EQ_STR(Carets(tv), @"18");
        GCHECK(Press(c.window, P(6, 5), cmdShift, 2, nil, NO) == tv);
        GCHECK_EQ_STR(Carets(tv), @"18 31 44 57 70");
    });
    MCGuiStep(@"typing goes in on every line of the column", ^{
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"|" replacementRange:NSMakeRange(NSNotFound, 0)];
        });
        NSArray *lines = [tv.string componentsSeparatedByString:@"\n"];
        GCHECK_EQ_STR(lines[0], @"line 01 abcd");
        GCHECK_EQ_STR(lines[1], @"line |02 abcd");
        GCHECK_EQ_STR(lines[5], @"line |06 abcd");
        GCHECK_EQ_STR(lines[6], @"line 07 abcd");
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, original);
    });
    MCGuiStep(@"a third quick click moves the far end, a slow one too", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        Press(c.window, P(2, 5), cmdShift, 1, nil, NO);
        Press(c.window, P(6, 5), cmdShift, 2, nil, NO);
        Press(c.window, P(9, 5), cmdShift, 3, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"18 31 44 57 70 83 96 109");
        Press(c.window, P(3, 5), cmdShift, 1, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"18 31");
    });
    MCGuiStep(@"upwards, and at another column", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        Press(c.window, P(6, 7), cmdShift, 1, nil, NO);
        Press(c.window, P(3, 7), cmdShift, 2, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"33 46 59 72");
    });
    MCGuiStep(@"the pointer moving and the keys changing between the clicks", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        Press(c.window, P(2, 5), cmdShift, 1, nil, NO);
        for (NSUInteger y = 3; y <= 5; y++)
            [tv mouseMoved:Mouse(kCGEventMouseMoved, P(y, 5), cmdShift, 0, kCGMouseButtonLeft)];
        const NSEventModifierFlags steps[] = {NSEventModifierFlagCommand, cmdShift};
        for (NSEventModifierFlags f : steps) {
            CGEventRef cg = CGEventCreate(NULL);
            CGEventSetType(cg, kCGEventFlagsChanged);
            CGEventSetFlags(cg, CGFlagsFor(f));
            NSEvent *e = [NSEvent eventWithCGEvent:cg];
            CFRelease(cg);
            [c.window sendEvent:[NSEvent keyEventWithType:NSEventTypeFlagsChanged
                                                 location:NSZeroPoint
                                            modifierFlags:e.modifierFlags
                                                timestamp:e.timestamp
                                             windowNumber:c.window.windowNumber
                                                  context:nil
                                               characters:@""
                              charactersIgnoringModifiers:@""
                                                isARepeat:NO
                                                  keyCode:56]];
        }
    });
    MCGuiPause(0.6);   // past the caret's blink, the bracket match, a hover
    MCGuiStep(@"... still give the column", ^{
        Press(c.window, P(5, 5), cmdShift, 1, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"18 31 44 57");
    });
    MCGuiStep(@"a real double-click (the same character) is left to the text view", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        Press(c.window, P(4, 5), cmdShift, 1, nil, NO);
        GCHECK(![tv.multiCarets mouseDown:Mouse(kCGEventLeftMouseDown, P(4, 5), cmdShift, 2,
                                                kCGMouseButtonLeft)]);
    });
    MCGuiStep(@"quick Option clicks at different places add carets", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        Press(c.window, P(3, 2), opt, 1, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"0 28");
        Press(c.window, P(5, 2), opt, 2, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"0 28 54");
        Press(c.window, P(3, 2), opt, 3, nil, NO);   // one already there goes
        GCHECK_EQ_STR(Carets(tv), @"0 54");
    });

    // VS Code's own binding (editor.multiCursorModifier "alt", the
    // default): Shift+Option click is a box from the caret to the click,
    // and more Shift+Option clicks keep its start. Its Cmd+Shift click is
    // a Shift click (extend the selection); the owner's pairs are kept on
    // Cmd+Shift here.
    MCGuiStep(@"Shift+Option click: the column from the caret", ^{
        [tv setSelectedRange:NSMakeRange(At(2, 5), 0)];
        GCHECK(Press(c.window, P(6, 5), optShift, 1, nil, NO) == tv);
        GCHECK_EQ_STR(Carets(tv), @"18 31 44 57 70");
        Press(c.window, P(4, 5), optShift, 2, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"18 31 44");
    });
    MCGuiStep(@"Shift+Option click from a selection: from its start", ^{
        [tv setSelectedRange:NSMakeRange(At(2, 5), 3)];
        Press(c.window, P(4, 8), optShift, 1, nil, NO);
        GCHECK_EQ_STR(Carets(tv), @"18+3 31+3 44+3");
    });
    MCGuiStep(@"Shift+Option drag: from the caret to the pointer", ^{
        [tv setSelectedRange:NSMakeRange(At(2, 5), 0)];
        Press(c.window, P(6, 5), optShift, 1, @[Pt(P(7, 5)), Pt(P(8, 5))], NO);
        GCHECK_EQ_STR(Carets(tv), @"18 31 44 57 70 83 96");
    });
    MCGuiStep(@"Option drag: a fresh column from the press", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        Press(c.window, P(3, 2), opt, 1, @[Pt(P(4, 4)), Pt(P(5, 6))], NO);
        GCHECK_EQ_STR(Carets(tv), @"28+4 41+4 54+4");
    });
    MCGuiStep(@"middle-button drag: a column from the press", ^{
        [tv setSelectedRange:NSMakeRange(0, 0)];
        GCHECK(Press(c.window, P(2, 1), 0, 1, @[Pt(P(3, 1)), Pt(P(4, 1))], YES) == tv);
        GCHECK_EQ_STR(Carets(tv), @"14 27 40");
        Press(c.window, P(5, 3), 0, 1, nil, YES);   // a middle click: the caret goes there
        GCHECK_EQ_STR(Carets(tv), @"55");
        GCHECK(!tv.multiCarets.active);
    });
    MCGuiStep(@"the text is as it was", ^{
        GCHECK_EQ_STR(tv.string, original);
    });
}
