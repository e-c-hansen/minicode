// CodeTextView.mm — Objective-C++. The editor's text view (see Lsp.h):
// squiggles, hover, several carets, folds, the ruler and bracket boxes.
#import "LspShared.h"

// -------------------------------------------------------- CodeTextView
@implementation MCDiagnosticMark
@end

@implementation CodeTextView {
    NSTrackingArea *_hoverArea;
    MCMultiCarets *_multi;
    MCFolds *_folds;
    NSEvent *_keyEvent;   // the key being interpreted, during keyDown:
}

- (MCMultiCarets *)multiCarets { return _multi; }
- (MCFolds *)folds { return _folds; }
- (BOOL)multiCaretActive { return _multi.active; }

// The designated initializer (initWithFrame: comes through it too), so a
// view made over a text storage of its own (the editor's MCSourceStorage)
// watches that storage.
- (instancetype)initWithFrame:(NSRect)frame textContainer:(NSTextContainer *)container {
    if ((self = [super initWithFrame:frame textContainer:container])) {
        _multi = [[MCMultiCarets alloc] initWithTextView:self];
        _folds = [[MCFolds alloc] initWithTextView:self];
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification
                 object:self.textStorage];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)setDiagnostics:(NSArray<MCDiagnosticMark *> *)diagnostics {
    _diagnostics = [diagnostics copy];
    self.needsDisplay = YES;
    self.enclosingScrollView.verticalRulerView.needsDisplay = YES;   // the gutter's dots
}

- (BOOL)resignFirstResponder {
    BOOL ok = [super resignFirstResponder];
    if (ok && _multi.active) self.needsDisplay = YES;   // carets dimmed
    if (ok && self.onFocusLost) self.onFocusLost();
    if (ok && self.onBlur) self.onBlur();
    return ok;
}

// Keep the marks on their text while it is edited, until the server sends
// fresh ones: shift those after the edit, stretch those it touches.
- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    // A change made by anyone but the carets themselves (undo, a reload, a
    // formatter, a tab switch) leaves them pointing at the wrong text.
    if ((ts.editedMask & NSTextStorageEditedCharacters) && _multi.active && !_multi.applying)
        [_multi clear];
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || _diagnostics.count == 0) return;
    NSRange er = ts.editedRange;
    NSInteger delta = ts.changeInLength;
    NSInteger s = (NSInteger)er.location;
    NSInteger oldEnd = (NSInteger)NSMaxRange(er) - delta;
    NSInteger newEnd = (NSInteger)NSMaxRange(er);
    NSInteger length = (NSInteger)ts.length;
    NSMutableArray *kept = [NSMutableArray array];
    for (MCDiagnosticMark *m in _diagnostics) {
        NSInteger a = (NSInteger)m.range.location, b = (NSInteger)NSMaxRange(m.range);
        if (b <= s) {
            // before the edit: unchanged
        } else if (a >= oldEnd) {
            a += delta; b += delta;
        } else {
            a = MIN(a, s);
            b = MAX(newEnd, b + delta);
        }
        a = MAX(0, MIN(a, length));
        b = MAX(a, MIN(b, length));
        MCDiagnosticMark *n = [MCDiagnosticMark new];
        n.range = NSMakeRange((NSUInteger)a, (NSUInteger)(b - a));
        n.severity = m.severity;
        n.message = m.message;
        [kept addObject:n];
    }
    _diagnostics = kept;
    self.needsDisplay = YES;
}

- (NSString *)diagnosticMessageAtIndex:(NSUInteger)index {
    NSMutableArray *msgs = [NSMutableArray array];
    for (MCDiagnosticMark *m in _diagnostics)
        if (NSLocationInRange(index, m.range) ||
            (m.range.length == 0 && index == m.range.location))
            [msgs addObject:m.message];
    return msgs.count ? [msgs componentsJoinedByString:@"\n"] : nil;
}

// The characters on screen, so drawing never forces layout of a whole file.
- (NSRange)visibleCharacterRange {
    NSTextLayoutManager *tlm = self.textLayoutManager;
    if (tlm) {
        NSTextRange *vr = tlm.textViewportLayoutController.viewportRange;
        if (!vr) return NSMakeRange(0, self.string.length);
        NSTextContentManager *cm = tlm.textContentManager;
        NSInteger a = [cm offsetFromLocation:cm.documentRange.location toLocation:vr.location];
        NSInteger b = [cm offsetFromLocation:cm.documentRange.location
                                  toLocation:vr.endLocation];
        if (a < 0 || b < a) return NSMakeRange(0, self.string.length);
        return NSMakeRange((NSUInteger)a, (NSUInteger)(b - a));
    }
    NSLayoutManager *lm = self.layoutManager;
    NSRange glyphs = [lm glyphRangeForBoundingRect:self.visibleRect
                                   inTextContainer:self.textContainer];
    return [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
}

// Line fragments covering a character range, in view coordinates. TextKit 2
// (the default) draws no underline from rendering attributes, so the marks
// are drawn by hand from these rects; TextKit 1 is handled in case something
// ever switches the view over.
- (void)enumerateRectsForRange:(NSRange)r block:(void (^)(NSRect rect))block {
    NSPoint origin = self.textContainerOrigin;
    NSTextLayoutManager *tlm = self.textLayoutManager;
    if (tlm) {
        NSTextContentManager *cm = tlm.textContentManager;
        id<NSTextLocation> start = [cm locationFromLocation:cm.documentRange.location
                                                 withOffset:(NSInteger)r.location];
        id<NSTextLocation> end = start ? [cm locationFromLocation:start
                                                       withOffset:(NSInteger)r.length]
                                       : nil;
        if (!start || !end) return;
        NSTextRange *tr = [[NSTextRange alloc] initWithLocation:start endLocation:end];
        if (!tr) return;
        [tlm enumerateTextSegmentsInRange:tr
                                     type:NSTextLayoutManagerSegmentTypeStandard
                                  options:NSTextLayoutManagerSegmentOptionsRangeNotRequired
                               usingBlock:^BOOL(NSTextRange *sr, CGRect frame,
                                                CGFloat baseline, NSTextContainer *c) {
            (void)sr; (void)baseline; (void)c;
            block(NSOffsetRect(frame, origin.x, origin.y));
            return YES;
        }];
        return;
    }
    NSLayoutManager *lm = self.layoutManager;
    NSRange gr = [lm glyphRangeForCharacterRange:r actualCharacterRange:NULL];
    [lm enumerateEnclosingRectsForGlyphRange:gr
                    withinSelectedGlyphRange:NSMakeRange(NSNotFound, 0)
                             inTextContainer:self.textContainer
                                  usingBlock:^(NSRect rect, BOOL *stop) {
        (void)stop;
        block(NSOffsetRect(rect, origin.x, origin.y));
    }];
}

NSColor *MCDiagnosticColor(NSInteger severity) { return SeverityColor(severity); }

- (void)drawRect:(NSRect)dirty {
    [super drawRect:dirty];
    if (_folds.hasFolds && self.editable)
        [_folds drawPlaceholdersInRect:dirty color:_bracketMatchColor ?: [NSColor grayColor]];
    [_multi drawInRect:dirty];
    if (_diagnostics.count == 0) return;
    NSRange visible = [self visibleCharacterRange];
    // Errors last, so they draw over a warning on the same text.
    NSArray *ordered = [_diagnostics sortedArrayUsingComparator:
        ^NSComparisonResult(MCDiagnosticMark *a, MCDiagnosticMark *b) {
            return a.severity > b.severity ? NSOrderedAscending
                 : a.severity < b.severity ? NSOrderedDescending : NSOrderedSame;
        }];
    for (MCDiagnosticMark *m in ordered) {
        NSRange r = m.range;
        if (NSMaxRange(r) < visible.location || r.location > NSMaxRange(visible)) continue;
        if ([_folds rangeIsHidden:r]) continue;   // folded away
        NSColor *color = SeverityColor(m.severity);
        [self enumerateRectsForRange:r block:^(NSRect rect) {
            if (!NSIntersectsRect(NSInsetRect(rect, 0, -3), dirty)) return;
            CGFloat width = MAX(rect.size.width, 6);
            // A squiggle along the bottom of the line fragment (the view is
            // flipped, so the bottom is maxY).
            CGFloat y = NSMaxY(rect) - 2, amp = 1.5, step = 2;
            NSBezierPath *path = [NSBezierPath bezierPath];
            path.lineWidth = 1;
            [path moveToPoint:NSMakePoint(rect.origin.x, y)];
            BOOL up = YES;
            for (CGFloat x = rect.origin.x + step; x <= rect.origin.x + width + 0.1; x += step) {
                [path lineToPoint:NSMakePoint(x, y + (up ? -amp : amp))];
                up = !up;
            }
            [color set];
            [path stroke];
        }];
    }
}

// ---- the pointer, for hover info
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_hoverArea) [self removeTrackingArea:_hoverArea];
    _hoverArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:nil];
    [self addTrackingArea:_hoverArea];
}

- (NSUInteger)characterIndexUnderPoint:(NSPoint)point {
    NSString *text = self.string;
    NSUInteger n = text.length;
    if (n == 0) return NSNotFound;
    NSUInteger i = [self characterIndexForInsertionAtPoint:point];
    // The insertion point is before the character whose left half holds
    // the point, after it for its right half; either way it is one of these
    // two, and only if the point is inside its box (past a line's end the
    // nearest insertion point is the line's end).
    for (NSInteger k = (NSInteger)MIN(i, n - 1); k >= 0 && k >= (NSInteger)i - 1; k--) {
        unichar c = [text characterAtIndex:(NSUInteger)k];
        if (c == '\n' || c == '\r') continue;
        __block BOOL inside = NO;
        [self enumerateRectsForRange:NSMakeRange((NSUInteger)k, 1) block:^(NSRect rect) {
            if (NSPointInRect(point, rect)) inside = YES;
        }];
        if (inside) return (NSUInteger)k;
    }
    return NSNotFound;
}

- (void)mouseMoved:(NSEvent *)event {
    [super mouseMoved:event];
    if (!self.onMouseHover) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    self.onMouseHover([self characterIndexUnderPoint:p]);
}

- (void)mouseExited:(NSEvent *)event {
    [super mouseExited:event];
    if (self.onMouseHover) self.onMouseHover(NSNotFound);
}

- (void)scrollWheel:(NSEvent *)event {
    if (self.onScrolled) self.onScrolled();
    [super scrollWheel:event];
}

// ---- several carets: everything that types, moves or sets the selection
// goes to MCMultiCarets first while it has two or more.
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    if ([_multi insertText:string]) return;
    [super insertText:string replacementRange:replacementRange];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)rep {
    if (_multi.active) [_multi clear];   // an input method composes at one place
    [super setMarkedText:string selectedRange:sel replacementRange:rep];
}

- (void)keyDown:(NSEvent *)event {
    // Cmd+Home / End (Fn+Cmd+Left / Right on a laptop) go to the start or
    // end of the document, with Shift selecting: Cmd+Up / Down move lines.
    const NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
         NSEventModifierFlagControl);
    if ((event.keyCode == 115 || event.keyCode == 119) &&
        (mods == NSEventModifierFlagCommand ||
         mods == (NSEventModifierFlagCommand | NSEventModifierFlagShift))) {
        const BOOL home = event.keyCode == 115, extend = (mods & NSEventModifierFlagShift) != 0;
        [self doCommandBySelector:home ? (extend ? @selector(moveToBeginningOfDocumentAndModifySelection:)
                                                 : @selector(moveToBeginningOfDocument:))
                                       : (extend ? @selector(moveToEndOfDocumentAndModifySelection:)
                                                 : @selector(moveToEndOfDocument:))];
        return;
    }
    // Option+Up / Down move the lines too, as in VS Code on the Mac, with or
    // without a selection (AppKit would move the caret by paragraph). Taken
    // by key code here, since AppKit sends two selectors for each.
    if ((event.keyCode == 125 || event.keyCode == 126) && mods == NSEventModifierFlagOption &&
        self.editable) {
        [_multi moveLinesDown:event.keyCode == 125];
        return;
    }
    NSEvent *outer = _keyEvent;
    _keyEvent = event;
    [super keyDown:event];
    _keyEvent = outer;
}

- (void)doCommandBySelector:(SEL)selector {
    // Backspace just after a fold's placeholder, or Delete just before it,
    // opens the fold rather than deleting text nobody can see.
    if (_folds.hasFolds && self.editable && !_multi.active && self.selectedRange.length == 0) {
        const NSUInteger at = self.selectedRange.location;
        if (selector == @selector(deleteBackward:) && [_folds openFoldEndingAt:at]) return;
        if (selector == @selector(deleteForward:) && [_folds openFoldStartingAt:at]) return;
    }
    // Cmd+Shift+Up/Down (select to the top or end of the file in AppKit) and
    // Shift+Option+Up/Down (select by paragraph) copy the lines instead, as
    // in VS Code. Only from the arrow keys: Shift+Home and Shift+End send the
    // same selectors and keep their meaning.
    NSEvent *ev = _keyEvent;
    if (self.editable && ev &&
        (ev.keyCode == 125 || ev.keyCode == 126) &&
        (selector == @selector(moveToEndOfDocumentAndModifySelection:) ||
         selector == @selector(moveToBeginningOfDocumentAndModifySelection:) ||
         selector == @selector(moveParagraphForwardAndModifySelection:) ||
         selector == @selector(moveParagraphBackwardAndModifySelection:))) {
        [_multi duplicateLinesDown:ev.keyCode == 125];
        return;
    }
    // Cmd+Down / Up move the lines, as the user expects; the system's move
    // to the end or start of the document is Cmd+End / Home (keyDown:).
    if (self.editable && ev && (ev.keyCode == 125 || ev.keyCode == 126) &&
        (selector == @selector(moveToEndOfDocument:) ||
         selector == @selector(moveToBeginningOfDocument:))) {
        [_multi moveLinesDown:ev.keyCode == 125];
        return;
    }
    if ([_multi doCommand:selector]) return;
    [super doCommandBySelector:selector];
}

- (void)copy:(id)sender {
    if (![_multi copy]) [super copy:sender];
}

- (void)cut:(id)sender {
    if (![_multi cut]) [super cut:sender];
}

- (void)paste:(id)sender {
    if (![_multi paste]) [super paste:sender];
}

// VS Code's multi-cursor keys, from the Edit menu (MultiCaretView.h).
- (IBAction)addNextOccurrence:(id)sender {
    if (!self.editable || ![_multi addNextOccurrence]) NSBeep();
}

- (IBAction)selectAllOccurrences:(id)sender {
    if (!self.editable || ![_multi selectAllOccurrences]) NSBeep();
}

- (IBAction)addCursorAbove:(id)sender {
    if (!self.editable || ![_multi addCursorVertically:-1]) NSBeep();
}

- (IBAction)addCursorBelow:(id)sender {
    if (!self.editable || ![_multi addCursorVertically:1]) NSBeep();
}

// Cmd+L and Ctrl+Shift+K, as in VS Code.
- (IBAction)expandLineSelection:(id)sender {
    if (!self.editable) { NSBeep(); return; }
    [_multi expandLineSelection];
}

- (IBAction)deleteLine:(id)sender {
    if (!self.editable) { NSBeep(); return; }
    [_multi deleteLines];
}

// ---- folding (View > Folding)
- (void)foldCommand:(BOOL (^)(MCFolds *f))command {
    if (!self.editable || !command(_folds)) NSBeep();
}
- (IBAction)foldRegion:(id)sender { [self foldCommand:^BOOL(MCFolds *f) { return [f foldAtCarets]; }]; }
- (IBAction)unfoldRegion:(id)sender { [self foldCommand:^BOOL(MCFolds *f) { return [f unfoldAtCarets]; }]; }
- (IBAction)toggleFold:(id)sender { [self foldCommand:^BOOL(MCFolds *f) { return [f toggleAtCarets]; }]; }
- (IBAction)foldRecursively:(id)sender {
    [self foldCommand:^BOOL(MCFolds *f) { return [f foldRecursivelyAtCarets]; }];
}
- (IBAction)unfoldRecursively:(id)sender {
    [self foldCommand:^BOOL(MCFolds *f) { return [f unfoldRecursivelyAtCarets]; }];
}
- (IBAction)foldAll:(id)sender { [self foldCommand:^BOOL(MCFolds *f) { return [f foldAll]; }]; }
- (IBAction)unfoldAll:(id)sender { [self foldCommand:^BOOL(MCFolds *f) { return [f unfoldAll]; }]; }
- (IBAction)foldLevel:(id)sender {
    const NSInteger level = [sender respondsToSelector:@selector(tag)] ? [sender tag] : 1;
    [self foldCommand:^BOOL(MCFolds *f) { return [f foldLevel:level]; }];
}
- (IBAction)foldAllComments:(id)sender {
    [self foldCommand:^BOOL(MCFolds *f) { return [f foldAllComments]; }];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    SEL a = item.action;
    if (a == @selector(foldRegion:) || a == @selector(unfoldRegion:) ||
        a == @selector(toggleFold:) || a == @selector(foldRecursively:) ||
        a == @selector(unfoldRecursively:) || a == @selector(foldAll:) ||
        a == @selector(unfoldAll:) || a == @selector(foldLevel:) ||
        a == @selector(foldAllComments:))
        return self.editable;
    if (a == @selector(addNextOccurrence:) || a == @selector(selectAllOccurrences:) ||
        a == @selector(addCursorAbove:) || a == @selector(addCursorBelow:) ||
        a == @selector(expandLineSelection:) || a == @selector(deleteLine:))
        return self.editable;
    // With nothing selected, Copy and Cut take the line (MultiCaretView.h).
    if ((a == @selector(copy:) || a == @selector(cut:)) && self.editable) return YES;
    if (a == @selector(performTextFinderAction:) && _validateFindAction)
        return _validateFindAction(item.tag);
    return [super validateMenuItem:item];
}

- (void)performTextFinderAction:(id)sender {
    NSInteger tag = [sender respondsToSelector:@selector(tag)] ? [sender tag] : 0;
    if (_onFindAction && _onFindAction(tag)) return;
    [super performTextFinderAction:sender];
}

// ---- editor.ruler: drawn with the background, so the text covers it.
- (void)setRulerColumns:(NSArray<NSNumber *> *)columns {
    _rulerColumns = [columns copy];
    self.needsDisplay = YES;
}

- (void)setRulerColor:(NSColor *)color {
    _rulerColor = color;
    self.needsDisplay = YES;
}

// ---- bracket matching: the boxes are drawn with the background too.
- (void)setBracketRanges:(NSArray<NSValue *> *)ranges {
    if ([ranges isEqualToArray:_bracketRanges ?: @[]] ||
        (ranges.count == 0 && _bracketRanges.count == 0))
        return;
    for (NSValue *v in _bracketRanges) [self invalidateBracketBox:v.rangeValue];
    _bracketRanges = [ranges copy];
    for (NSValue *v in _bracketRanges) [self invalidateBracketBox:v.rangeValue];
}

- (void)setBracketMatchColor:(NSColor *)color {
    _bracketMatchColor = color;
    for (NSValue *v in _bracketRanges) [self invalidateBracketBox:v.rangeValue];
}

- (void)invalidateBracketBox:(NSRange)r {
    if (NSMaxRange(r) > self.textStorage.length) return;
    [self enumerateRectsForRange:r block:^(NSRect rect) {
        [self setNeedsDisplayInRect:NSInsetRect(rect, -2, -2)];
    }];
}

- (void)drawBracketBoxesInRect:(NSRect)dirty {
    if (_bracketRanges.count == 0 || !_bracketMatchColor) return;
    const NSUInteger n = self.textStorage.length;
    NSColor *fill = [_bracketMatchColor colorWithAlphaComponent:_bracketMatchColor.alphaComponent * 0.25];
    for (NSValue *v in _bracketRanges) {
        const NSRange r = v.rangeValue;
        if (NSMaxRange(r) > n) continue;
        [self enumerateRectsForRange:r block:^(NSRect rect) {
            if (!NSIntersectsRect(rect, dirty)) return;
            // The glyph's box, a little shorter than its line fragment,
            // which holds the line spacing below the text.
            NSRect box = NSInsetRect([self backingAlignedRect:rect
                                                      options:NSAlignAllEdgesNearest], 0.5, 0.5);
            box.size.height = MAX(4, NSHeight(box) - 2);
            [fill set];
            NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
            [self->_bracketMatchColor set];
            NSBezierPath *p = [NSBezierPath bezierPathWithRect:box];
            p.lineWidth = 1;
            [p stroke];
        }];
    }
}

- (void)drawViewBackgroundInRect:(NSRect)rect {
    [super drawViewBackgroundInRect:rect];
    if (_drawBehindText) _drawBehindText(rect);
    if (self.editable) [self drawBracketBoxesInRect:rect];
    if (!self.editable || _rulerColumns.count == 0 || !_rulerColor) return;
    NSFont *font = self.typingAttributes[NSFontAttributeName];
    if (!font.fixedPitch) font = MCEditorFont();
    const CGFloat cw = [@"0" sizeWithAttributes:@{NSFontAttributeName: font}].width;
    const CGFloat x0 = self.textContainerOrigin.x + self.textContainer.lineFragmentPadding;
    [_rulerColor set];
    for (NSNumber *col in _rulerColumns) {
        NSRect line = [self backingAlignedRect:NSMakeRect(x0 + col.doubleValue * cw,
                                                          NSMinY(rect), 1, NSHeight(rect))
                                       options:NSAlignAllEdgesNearest];
        if (NSMaxX(line) > NSWidth(self.bounds) || !NSIntersectsRect(line, rect)) continue;
        NSRectFillUsingOperation(line, NSCompositingOperationSourceOver);
    }
}

- (void)setSelectedRanges:(NSArray<NSValue *> *)ranges
                 affinity:(NSSelectionAffinity)affinity
           stillSelecting:(BOOL)still {
    if (_multi.active && !_multi.applying) [_multi clear];
    // A selection that lands inside a fold (Find, Go to Line, a definition,
    // F8) opens it, so what was gone to is seen.
    if (_folds.hasFolds && !still) [_folds revealRanges:ranges];
    [super setSelectedRanges:ranges affinity:affinity stillSelecting:still];
}

- (BOOL)shouldDrawInsertionPoint {
    return _multi.active ? NO : [super shouldDrawInsertionPoint];
}

- (BOOL)becomeFirstResponder {
    BOOL ok = [super becomeFirstResponder];
    if (_multi.active) self.needsDisplay = YES;   // carets back to full color
    return ok;
}

- (void)mouseDown:(NSEvent *)event {
    if (self.editable && [_folds mouseDown:event]) return;   // a fold's placeholder
    if ([_multi mouseDown:event]) return;
    NSEventModifierFlags mods = event.modifierFlags &
        NSEventModifierFlagDeviceIndependentFlagsMask;
    if (mods == NSEventModifierFlagCommand && self.onCommandClick) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        NSUInteger i = [self characterIndexForInsertionAtPoint:p];
        if (self.onCommandClick(i)) return;
    }
    if (event.clickCount == 2 && !self.editable && self.onPreviewDoubleClick) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        NSUInteger i = [self characterIndexForInsertionAtPoint:p];
        if (self.onPreviewDoubleClick(i, p)) return;
    }
    [super mouseDown:event];
}
@end
