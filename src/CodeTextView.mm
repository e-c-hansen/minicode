// CodeTextView.mm — Objective-C++. The editor's text view (see Lsp.h):
// squiggles, hover, several carets, folds, the ruler and bracket boxes.
#import "LspShared.h"
#import "LspInlayHints.h"
#import "SourceStorage.h"
#include <os/log.h>

// -------------------------------------------------------- CodeTextView
@implementation MCDiagnosticMark
@end

// Marks after an edit of `ts`: those after it shift, those it touches
// stretch over what replaced them.
static NSArray<MCDiagnosticMark *> *MCMarksAfterEdit(NSArray<MCDiagnosticMark *> *marks,
                                                    NSTextStorage *ts) {
    NSRange er = ts.editedRange;
    NSInteger delta = ts.changeInLength;
    NSInteger s = (NSInteger)er.location;
    NSInteger oldEnd = (NSInteger)NSMaxRange(er) - delta;
    NSInteger newEnd = (NSInteger)NSMaxRange(er);
    NSInteger length = (NSInteger)ts.length;
    NSMutableArray *kept = [NSMutableArray array];
    for (MCDiagnosticMark *m in marks) {
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
        n.unnecessary = m.unnecessary;
        n.deprecated = m.deprecated;
        [kept addObject:n];
    }
    return kept;
}

@implementation CodeTextView {
    NSTrackingArea *_hoverArea;
    MCMultiCarets *_multi;
    MCFolds *_folds;
    MCInlayHints *_inlays;
    NSEvent *_keyEvent;   // the key being interpreted, during keyDown:
}

- (MCMultiCarets *)multiCarets { return _multi; }
- (MCFolds *)folds { return _folds; }
- (MCInlayHints *)inlayHints { return _inlays; }
- (BOOL)multiCaretActive { return _multi.active; }

// The designated initializer (initWithFrame: comes through it too), so a
// view made over a text storage of its own (the editor's MCSourceStorage)
// watches that storage.
- (instancetype)initWithFrame:(NSRect)frame textContainer:(NSTextContainer *)container {
    if ((self = [super initWithFrame:frame textContainer:container])) {
        _multi = [[MCMultiCarets alloc] initWithTextView:self];
        _folds = [[MCFolds alloc] initWithTextView:self];
        _inlays = [[MCInlayHints alloc] initWithTextView:self];
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

// needsDisplay = YES asks to redraw the whole view, and NSTextView lays out
// every glyph in a rect it is asked to redraw: clearing a file's squiggles
// laid out all of a 25 MB file (2.5 s), after which every keystroke walked
// every line fragment. Only what shows, and what AppKit drew ahead of the
// scroll (the prepared rect), can need redrawing.
- (void)setNeedsDisplay:(BOOL)flag {
    if (!flag) { [super setNeedsDisplay:NO]; return; }
    [self setNeedsDisplayInRect:NSUnionRect(self.visibleRect, self.preparedContentRect)];
}

- (void)setDiagnostics:(NSArray<MCDiagnosticMark *> *)diagnostics {
    _diagnostics = [diagnostics copy];
    self.needsDisplay = YES;
    self.enclosingScrollView.verticalRulerView.needsDisplay = YES;   // the gutter's dots
}

- (void)setBuildMarks:(NSArray<MCDiagnosticMark *> *)marks {
    if (marks.count == 0 && _buildMarks.count == 0) return;
    _buildMarks = [marks copy];
    self.enclosingScrollView.verticalRulerView.needsDisplay = YES;
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
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    if (_diagnostics.count) {
        _diagnostics = MCMarksAfterEdit(_diagnostics, ts);
        self.needsDisplay = YES;
    }
    if (_buildMarks.count) _buildMarks = MCMarksAfterEdit(_buildMarks, ts);
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
    if (@available(macOS 12.0, *)) {
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
    if (@available(macOS 12.0, *)) {
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
    [_inlays drawInRect:dirty];
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
        // As VS Code draws them: code never used is faded (pyright's "x is
        // not accessed" hints), a deprecated name struck through, and a
        // hint with neither gets a few dots under its start, never a
        // squiggle like a warning's.
        if (m.unnecessary || m.deprecated || m.severity == 4) {
            NSColor *bg = [[AppSettings shared] background:Surface::Editor];
            NSColor *ink = self.textColor ?: [NSColor textColor];
            __block BOOL first = YES;
            [self enumerateRectsForRange:r block:^(NSRect rect) {
                if (!NSIntersectsRect(NSInsetRect(rect, 0, -3), dirty)) return;
                if (m.unnecessary && rect.size.width > 0) {
                    [[bg colorWithAlphaComponent:0.5 * bg.alphaComponent] set];
                    NSRectFillUsingOperation(rect, NSCompositingOperationSourceOver);
                }
                if (m.deprecated && rect.size.width > 0) {
                    NSRect line = NSMakeRect(rect.origin.x, round(NSMidY(rect)), rect.size.width, 1);
                    [[ink colorWithAlphaComponent:0.8] set];
                    NSRectFillUsingOperation(line, NSCompositingOperationSourceOver);
                }
                if (m.severity == 4 && !m.unnecessary && !m.deprecated && first) {
                    [color set];
                    for (int k = 0; k < 3; k++)
                        NSRectFill(NSMakeRect(rect.origin.x + 1 + k * 3, NSMaxY(rect) - 2, 1.5, 1.5));
                }
                first = NO;
            }];
            if (m.severity == 4) continue;
        }
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

// From this many ranges on, replaceRanges drops the glyphs over them first.
static const NSUInteger kDropGlyphsAtEdits = 64;

static os_log_t MCEditLog() {
    static os_log_t log = os_log_create("com.minicode.editor", "edit");
    return log;
}

// The ranges from index `from` on, moved by `delta`.
static NSArray<NSValue *> *MCShifted(NSArray<NSValue *> *ranges, NSUInteger from, NSInteger delta) {
    if (!delta) return ranges;
    NSMutableArray<NSValue *> *out = [NSMutableArray arrayWithCapacity:ranges.count];
    for (NSUInteger i = 0; i < ranges.count; i++) {
        NSRange r = ranges[i].rangeValue;
        if (i >= from) r.location = (NSUInteger)((NSInteger)r.location + delta);
        [out addObject:[NSValue valueWithRange:r]];
    }
    return out;
}

// Where the pieces an edit inserted (`inserted`, at `after` in a text of
// `length` characters) are in `text` now: at `after` while the text is
// that long; otherwise those from some index `from` on moved by the length
// difference, as one change made between piece from - 1 and piece from
// (or before the first, or after the last) leaves them. NO when no such
// placement holds every piece as it was inserted.
static BOOL MCPlaceInserted(NSString *text, NSArray<NSString *> *inserted, NSArray<NSValue *> *after,
                            NSUInteger length, NSUInteger *from, NSInteger *delta) {
    const NSUInteger count = after.count;
    const NSInteger d = (NSInteger)text.length - (NSInteger)length;
    BOOL (^holds)(NSUInteger, NSInteger) = ^BOOL(NSUInteger i, NSInteger by) {
        NSRange r = after[i].rangeValue;
        const NSInteger at = (NSInteger)r.location + by;
        if (at < 0 || (NSUInteger)at + r.length > text.length) return NO;
        r.location = (NSUInteger)at;
        return [text compare:inserted[i] options:NSLiteralSearch range:r] == NSOrderedSame;
    };
    NSUInteger j = 0;
    while (j < count && holds(j, 0)) j++;
    *from = count;
    *delta = 0;
    if (d == 0) return j == count;
    if (j == count) return YES;   // changed after the last piece
    // A change of d characters between piece j - 1 and piece j: room for a
    // deletion there, and every later piece moved by d.
    const NSUInteger gapStart = j ? NSMaxRange(after[j - 1].rangeValue) : 0;
    if (d < 0 && after[j].rangeValue.location - gapStart < (NSUInteger)-d) return NO;
    for (NSUInteger i = j; i < count; i++)
        if (!holds(i, d)) return NO;
    *from = j;
    *delta = d;
    return YES;
}

// ---- several replacements as one edit (Lsp.h)
//
// Each replacement is its own storage edit, last first, so every watcher of
// the storage (the highlighter, the folds, the squiggles, the gutter, the
// language server, Go Back's places) sees small edits, as for typing. One
// beginEditing around them reported a single edit from the first range to
// the last: folds between them opened, squiggles stretched over the span,
// and the highlighter relexed and restyled all of it (4.5 s for a quote
// typed at two carets 99,800 lines apart in a 100,000-line file).
//
// The undo step is this view's own, not NSTextView's: putting back many
// ranges NSTextView's way inserts each old text as an attributed string,
// which splits the storage's attribute runs one at a time (undoing a letter
// typed over 20,000 selections ran for minutes). Ours puts the old text back
// as plain strings, through this method, and selects it again.
- (BOOL)replaceRanges:(NSArray<NSValue *> *)ranges
          withStrings:(NSArray<NSString *> *)strings
           actionName:(NSString *)name {
    const NSUInteger count = ranges.count;
    if (count == 0 || count != strings.count) return NO;
    NSTextStorage *ts = self.textStorage;
    NSString *text = ts.string;
    NSMutableArray<NSString *> *olds = [NSMutableArray arrayWithCapacity:count];
    NSMutableArray<NSValue *> *after = [NSMutableArray arrayWithCapacity:count];
    NSUInteger end = 0;
    NSInteger shift = 0;
    for (NSUInteger i = 0; i < count; i++) {
        const NSRange r = ranges[i].rangeValue;
        if (r.location < end || NSMaxRange(r) > text.length) return NO;   // not ascending and apart
        end = NSMaxRange(r);
        [olds addObject:[text substringWithRange:r]];
        [after addObject:[NSValue valueWithRange:NSMakeRange((NSUInteger)((NSInteger)r.location + shift),
                                                             strings[i].length)]];
        shift += (NSInteger)strings[i].length - (NSInteger)r.length;
    }
    NSUndoManager *um = self.undoManager;
    const BOOL allows = self.allowsUndo, undo = allows && um && um.isUndoRegistrationEnabled;
    [self breakUndoCoalescing];
    if (undo) [um beginUndoGrouping];
    self.allowsUndo = NO;   // keeps what is on the stack; registers nothing for this change
    const BOOL ok = [self shouldChangeTextInRanges:ranges replacementStrings:strings];
    self.allowsUndo = allows;
    if (ok) {
        // NSTextView moves every selected range after each storage edit:
        // with the selections of 10,000 carets that was most of the time.
        // The callers set the selection afterwards.
        if (self.selectedRanges.count > 1)
            [super setSelectedRanges:@[ [NSValue valueWithRange:NSMakeRange(ranges[0].rangeValue.location, 0)] ]
                            affinity:NSSelectionAffinityDownstream
                      stillSelecting:NO];
        // Plain strings take the attributes before them; an empty text has
        // none to give, so the typing attributes then.
        NSDictionary *attrs = ts.length == 0 ? self.typingAttributes : nil;
        // Many edits: drop the glyphs from the first range to the last once,
        // first. Each storage edit otherwise replaces its glyphs in the
        // layout manager's glyph tree, which costs in proportion to the
        // glyphs already made, and the highlighter's restyles after it pay
        // again: Replace All of 20,000 matches in a 100,000-line file laid
        // out to its end took 3 to 8 s, nearly all of it there. Layout
        // between the first edit and the last is lost after several edits
        // either way (TextKit 1 keeps one soft-invalidated span), so only
        // the glyphs are given up, and they are made again on demand.
        if (count >= kDropGlyphsAtEdits) {
            const NSUInteger from = ranges[0].rangeValue.location;
            const NSUInteger to = NSMaxRange(ranges[count - 1].rangeValue);
            if (to > from)
                [self.layoutManager invalidateGlyphsForCharacterRange:NSMakeRange(from, to - from)
                                                       changeInLength:0
                                                 actualCharacterRange:NULL];
        }
        for (NSInteger k = (NSInteger)count - 1; k >= 0; k--) {
            const NSRange r = ranges[(NSUInteger)k].rangeValue;
            if (attrs)
                [ts replaceCharactersInRange:r
                        withAttributedString:[[NSAttributedString alloc]
                                                 initWithString:strings[(NSUInteger)k]
                                                     attributes:attrs]];
            else
                [ts replaceCharactersInRange:r withString:strings[(NSUInteger)k]];
        }
        [self didChangeText];
        if (undo) {
            const NSUInteger length = ts.length;
            NSString *undoName = [name copy];
            NSArray<NSValue *> *before = [ranges copy];
            NSArray<NSString *> *inserted = [strings copy];
            [um registerUndoWithTarget:self handler:^(CodeTextView *tv) {
                // Undo runs in order, so the text is as this edit left it,
                // unless something changed it outside the undo history.
                // Then the pieces are looked for where that change moved
                // them, and old text is never put back over anything but
                // what this edit inserted. Nothing found: said in the log
                // and with a beep, not silently.
                NSUInteger from = 0;
                NSInteger delta = 0;
                if (!MCPlaceInserted(tv.textStorage.string, inserted, after, length, &from, &delta)) {
                    os_log_error(MCEditLog(),
                                 "undo of \"%{public}@\" (%lu ranges) not done: the text has %lu "
                                 "characters, not the %lu it left, and no longer holds what it inserted",
                                 undoName, (unsigned long)after.count,
                                 (unsigned long)tv.textStorage.length, (unsigned long)length);
                    NSBeep();
                    return;
                }
                if (delta)
                    os_log(MCEditLog(),
                           "undo of \"%{public}@\": the text changed outside the undo history by "
                           "%ld characters before range %lu; undone where its pieces are now",
                           undoName, (long)delta, (unsigned long)from);
                if (![tv replaceRanges:MCShifted(after, from, delta) withStrings:olds
                            actionName:undoName])
                    return;
                // The old text selected again, as NSTextView's undo does.
                NSArray<NSValue *> *was = MCShifted(before, from, delta);
                NSMutableArray<NSValue *> *sel = [NSMutableArray array];
                for (NSValue *v in was)
                    if (v.rangeValue.length) [sel addObject:v];
                if (!sel.count) [sel addObject:was.firstObject];
                [tv setSelectedRanges:sel affinity:NSSelectionAffinityDownstream stillSelecting:NO];
                [tv scrollRangeToVisible:sel.firstObject.rangeValue];
            }];
        }
    }
    if (undo) {
        if (name) [um setActionName:name];
        [um endUndoGrouping];
    }
    [self breakUndoCoalescing];
    return ok;
}

// ---- several carets: everything that types, moves or sets the selection
// goes to MCMultiCarets first while it has two or more.
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    if ([_multi insertText:string replacementRange:replacementRange]) return;
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

// NSTextView takes the typing attributes from the character before the
// caret, as the storage answers it, so typing right after an inlay hint's
// gap stored its kern in every character typed (the highlighter then set
// their colors again on each keystroke, and plain text kept it). The
// editor never kerns anything of its own.
- (void)setTypingAttributes:(NSDictionary<NSAttributedStringKey, id> *)attrs {
    if (attrs[NSKernAttributeName]) {
        NSMutableDictionary *m = [attrs mutableCopy];
        [m removeObjectForKey:NSKernAttributeName];
        attrs = m;
    }
    [super setTypingAttributes:attrs];
}

// What goes to a pasteboard (a copy, a drag, a service) is the text with
// its own attributes: the inlay hints' gaps are left out of the rich text.
- (BOOL)writeSelectionToPasteboard:(NSPasteboard *)pboard types:(NSArray<NSPasteboardType> *)types {
    MCSourceStorage *ts = [self.textStorage isKindOfClass:[MCSourceStorage class]]
                              ? (MCSourceStorage *)self.textStorage : nil;
    ts.gapsHidden = YES;
    BOOL ok = [super writeSelectionToPasteboard:pboard types:types];
    ts.gapsHidden = NO;
    return ok;
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

- (void)otherMouseDown:(NSEvent *)event {
    if ([_multi otherMouseDown:event]) return;   // VS Code's middle-button column
    [super otherMouseDown:event];
}
@end
