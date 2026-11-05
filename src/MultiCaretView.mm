// MultiCaretView.mm — see MultiCaretView.h.
#import "MultiCaretView.h"
#include "AutoIndent.h"
#include "MultiCaret.h"
#include <string>
#include <vector>

using MultiCaret::Caret;

namespace {

std::u16string U16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}

NSString *NSFrom(const std::u16string &u) {
    return [NSString stringWithCharacters:(const unichar *)u.data() length:u.size()];
}

const NSEventModifierFlags kModMask = NSEventModifierFlagCommand | NSEventModifierFlagShift |
                                      NSEventModifierFlagOption | NSEventModifierFlagControl;

bool IsColumnPress(NSEvent *e) {
    NSEventModifierFlags m = e.modifierFlags & kModMask;
    return m == (NSEventModifierFlagCommand | NSEventModifierFlagShift) ||
           m == NSEventModifierFlagOption ||
           m == (NSEventModifierFlagOption | NSEventModifierFlagShift);
}

// Cmd+Shift and Shift+Option presses extend a column from where the last one
// began; Option alone adds a caret or drags a fresh column.
bool IsExtendPress(NSEvent *e) {
    NSEventModifierFlags m = e.modifierFlags & kModMask;
    return m == (NSEventModifierFlagCommand | NSEventModifierFlagShift) ||
           m == (NSEventModifierFlagOption | NSEventModifierFlagShift);
}

}  // namespace

@implementation MCMultiCarets {
    __weak NSTextView *_tv;
    std::vector<Caret> _carets;
    std::vector<CGFloat> _goalX;   // for Up and Down in a row; empty otherwise
    BOOL _applying;
    // Where the last column press began (its character and x), and the
    // carets it left. The next Cmd+Shift press extends from it while the
    // carets and the text are still as it left them.
    BOOL _hasAnchor;
    NSUInteger _anchorIndex, _anchorLength;
    CGFloat _anchorX;
    std::vector<Caret> _anchorCarets;
    // What the last Cmd+D or Shift+Cmd+L left: the selection it added (the
    // next press searches on from it), whether it matches whole words, and
    // the carets and text length then, to tell whether it still holds.
    BOOL _hasFind;
    Caret _findLast;
    BOOL _findWholeWord;
    std::vector<Caret> _findCarets;
    NSUInteger _findLength;
}

- (instancetype)initWithTextView:(NSTextView *)tv {
    if ((self = [super init])) _tv = tv;
    return self;
}

- (BOOL)active { return _carets.size() > 1; }
- (BOOL)applying { return _applying; }

// ---- geometry

// The caret's box at a character index, in the view's coordinates.
- (NSRect)caretRectAt:(NSUInteger)i {
    NSTextView *tv = _tv;
    NSRect screen = [tv firstRectForCharacterRange:NSMakeRange(i, 0) actualRange:NULL];
    if (!tv.window) return NSZeroRect;
    NSRect inWindow = [tv.window convertRectFromScreen:screen];
    NSRect r = [tv convertRect:inWindow fromView:nil];
    if (r.size.height < 1) {
        NSFont *f = tv.font ?: [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
        r.size.height = f.ascender - f.descender + f.leading;
    }
    return r;
}

- (NSUInteger)indexAtPoint:(NSPoint)p {
    return [_tv characterIndexForInsertionAtPoint:p];
}

// ---- state

- (void)clear {
    _carets.clear();
    _goalX.clear();
    _tv.needsDisplay = YES;
}

// Takes new carets: shows them, or hands a single one back to the text view.
- (void)setCarets:(std::vector<Caret>)carets {
    NSTextView *tv = _tv;
    carets = MultiCaret::normalize(std::move(carets), tv.string.length);
    _applying = YES;
    if (carets.size() < 2) {
        _carets.clear();
        const Caret c = carets[0];
        [tv setSelectedRange:NSMakeRange(c.lo(), c.hi() - c.lo())
                    affinity:NSSelectionAffinityDownstream
              stillSelecting:NO];
    } else {
        _carets = carets;
        NSMutableArray *sel = [NSMutableArray array];
        for (const Caret &c : _carets)
            if (!c.empty()) [sel addObject:[NSValue valueWithRange:NSMakeRange(c.lo(), c.hi() - c.lo())]];
        if (sel.count == 0) [sel addObject:[NSValue valueWithRange:NSMakeRange(_carets[0].head, 0)]];
        [tv setSelectedRanges:sel affinity:NSSelectionAffinityDownstream stillSelecting:NO];
    }
    _applying = NO;
    tv.needsDisplay = YES;
}

- (std::vector<Caret>)caretsFromSelection {
    std::vector<Caret> out;
    for (NSValue *v in _tv.selectedRanges) {
        NSRange r = v.rangeValue;
        out.push_back({r.location, NSMaxRange(r)});
    }
    return out;
}

- (NSArray<NSValue *> *)caretRanges {
    NSMutableArray *out = [NSMutableArray array];
    for (const Caret &c : _carets)
        [out addObject:[NSValue valueWithRange:NSMakeRange(c.lo(), c.hi() - c.lo())]];
    return out;
}

- (void)setCaretRanges:(NSArray<NSValue *> *)ranges {
    std::vector<Caret> v;
    for (NSValue *r in ranges) v.push_back({r.rangeValue.location, NSMaxRange(r.rangeValue)});
    if (v.empty()) return;
    [self setCarets:v];
}

// ---- the mouse

// The carets of a column from (x0, line of a0) to (x1, line of a1): one per
// line, each from the character at x0 to the character at x1, clamped to its
// line. A line too short for either x gets a caret at its end.
- (std::vector<Caret>)columnFrom:(NSUInteger)a0 x:(CGFloat)x0 to:(NSUInteger)a1 x:(CGFloat)x1 {
    NSTextView *tv = _tv;
    NSString *s = tv.string;
    const NSUInteger n = s.length;
    NSUInteger top = MIN(a0, a1), bottom = MAX(a0, a1);
    NSUInteger lineStart = [s lineRangeForRange:NSMakeRange(MIN(top, n), 0)].location;
    std::vector<Caret> out;
    for (;;) {
        NSUInteger contentsEnd = 0, lineEnd = 0;
        [s getLineStart:NULL end:&lineEnd contentsEnd:&contentsEnd
               forRange:NSMakeRange(lineStart, 0)];
        const NSRect r = [self caretRectAt:lineStart];
        const CGFloat y = NSMidY(r);
        NSUInteger a = [self indexAtPoint:NSMakePoint(x0, y)];
        NSUInteger b = [self indexAtPoint:NSMakePoint(x1, y)];
        a = MAX(lineStart, MIN(a, contentsEnd));
        b = MAX(lineStart, MIN(b, contentsEnd));
        out.push_back({a, b});
        if (lineEnd >= n && lineEnd > lineStart && bottom >= n && contentsEnd < n) {
            out.push_back({n, n});   // the empty line after a final newline
            break;
        }
        if (lineEnd >= n || lineEnd <= lineStart || lineEnd > bottom) break;
        lineStart = lineEnd;
        if (lineStart > bottom) break;
    }
    return out;
}

- (std::vector<Caret>)currentCarets {
    return self.active ? _carets : [self caretsFromSelection];
}

- (void)rememberAnchor:(NSUInteger)index x:(CGFloat)x {
    _hasAnchor = YES;
    _anchorIndex = index;
    _anchorX = x;
    _anchorLength = _tv.string.length;
    _anchorCarets = [self currentCarets];
}

- (BOOL)mouseDown:(NSEvent *)event {
    NSTextView *tv = _tv;
    if (!tv.editable || !IsColumnPress(event) || event.clickCount > 1) return NO;
    if (tv.window.firstResponder != tv) [tv.window makeFirstResponder:tv];
    const NSPoint p0 = [tv convertPoint:event.locationInWindow fromView:nil];
    const NSUInteger a0 = [self indexAtPoint:p0];
    _goalX.clear();

    // Cmd+Shift (or Shift+Option): the first press puts the caret there; the
    // next, clicked or dragged, makes the column from that place to this
    // one, and further ones move its far end.
    if (IsExtendPress(event)) {
        const BOOL extending = _hasAnchor && _anchorLength == tv.string.length &&
                               [self currentCarets] == _anchorCarets;
        const NSUInteger anchor = extending ? _anchorIndex : a0;
        const CGFloat anchorX = extending ? _anchorX : p0.x;
        [self setCarets:[self columnFrom:anchor x:anchorX to:a0 x:p0.x]];
        NSUInteger lastA1 = a0;
        CGFloat lastX1 = p0.x;
        for (;;) {
            NSEvent *e = [tv.window nextEventMatchingMask:NSEventMaskLeftMouseDragged |
                                                          NSEventMaskLeftMouseUp];
            if (!e || e.type == NSEventTypeLeftMouseUp) break;
            [tv autoscroll:e];
            const NSPoint q = [tv convertPoint:e.locationInWindow fromView:nil];
            const NSUInteger a1 = [self indexAtPoint:q];
            if (a1 == lastA1 && q.x == lastX1) continue;
            lastA1 = a1;
            lastX1 = q.x;
            [self setCarets:[self columnFrom:anchor x:anchorX to:a1 x:q.x]];
        }
        [self rememberAnchor:anchor x:anchorX];
        return YES;
    }

    BOOL dragged = NO;
    NSUInteger lastA1 = NSNotFound;
    CGFloat lastX1 = -1;
    for (;;) {
        NSEvent *e = [tv.window nextEventMatchingMask:NSEventMaskLeftMouseDragged |
                                                      NSEventMaskLeftMouseUp];
        if (!e || e.type == NSEventTypeLeftMouseUp) break;
        const NSPoint p1 = [tv convertPoint:e.locationInWindow fromView:nil];
        if (!dragged && fabs(p1.x - p0.x) < 3 && fabs(p1.y - p0.y) < 3) continue;
        dragged = YES;
        [tv autoscroll:e];
        const NSPoint q = [tv convertPoint:e.locationInWindow fromView:nil];
        const NSUInteger a1 = [self indexAtPoint:q];
        if (a1 == lastA1 && q.x == lastX1) continue;
        lastA1 = a1;
        lastX1 = q.x;
        [self setCarets:[self columnFrom:a0 x:p0.x to:a1 x:q.x]];
    }
    if (dragged) {
        [self rememberAnchor:a0 x:p0.x];   // Cmd+Shift+click can stretch it
        return YES;
    }

    // A click: add a caret there, or take one away.
    std::vector<Caret> carets = self.active ? _carets : [self caretsFromSelection];
    for (size_t i = 0; i < carets.size(); i++) {
        if (carets[i].empty() && carets[i].head == a0) {
            if (carets.size() > 1) {
                carets.erase(carets.begin() + (long)i);
                [self setCarets:carets];
            }
            return YES;
        }
    }
    carets.push_back({a0, a0});
    [self setCarets:carets];
    return YES;
}

// ---- edits

// Applies an edit as one undo step, through the text view so undo, the
// highlighter, the language server and the dirty mark all see it.
- (void)applyEdit:(const MultiCaret::Edit &)e action:(NSString *)action {
    NSTextView *tv = _tv;
    if (e.changes.empty()) {
        [self setCarets:e.carets];
        return;
    }
    NSMutableArray *ranges = [NSMutableArray array];
    NSMutableArray *strings = [NSMutableArray array];
    for (const MultiCaret::Change &c : e.changes) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(c.start, c.length)]];
        [strings addObject:NSFrom(c.text)];
    }
    _applying = YES;
    [tv breakUndoCoalescing];
    if ([tv shouldChangeTextInRanges:ranges replacementStrings:strings]) {
        NSTextStorage *ts = tv.textStorage;
        NSDictionary *attrs = tv.typingAttributes;
        [ts beginEditing];
        for (NSInteger i = (NSInteger)ranges.count - 1; i >= 0; i--) {
            NSAttributedString *a = [[NSAttributedString alloc] initWithString:strings[i]
                                                                    attributes:attrs];
            [ts replaceCharactersInRange:[ranges[i] rangeValue] withAttributedString:a];
        }
        [ts endEditing];
        [tv didChangeText];
        [tv breakUndoCoalescing];
        if (action) [tv.undoManager setActionName:action];
        _applying = NO;
        [self setCarets:e.carets];
    }
    _applying = NO;
}

- (void)insertStrings:(const std::vector<std::u16string> &)texts action:(NSString *)action {
    [self applyEdit:MultiCaret::insert(_carets, texts) action:action];
}

- (BOOL)insertText:(id)text {
    NSString *s = [text isKindOfClass:[NSAttributedString class]] ? [text string] : text;
    if (![s isKindOfClass:[NSString class]]) return NO;
    // An opening bracket or quote typed over the selection (or over every
    // caret's selection) wraps it in the pair, as most code editors do.
    // Not while an input method composes: its text replaces the marked
    // range, which is a selection only to it.
    NSTextView *tv = _tv;
    if (s.length == 1 && MultiCaret::closerFor([s characterAtIndex:0]) && tv.editable &&
        !tv.hasMarkedText) {
        const std::vector<Caret> carets = self.active ? _carets : [self caretsFromSelection];
        bool all = !carets.empty();
        for (const Caret &c : carets)
            if (c.empty()) all = false;
        if (all) {
            _goalX.clear();
            [self applyEdit:MultiCaret::surround(carets, [s characterAtIndex:0])
                     action:@"Typing"];
            return YES;
        }
    }
    if (!self.active) return NO;
    _goalX.clear();
    [self insertStrings:{U16(s)} action:@"Typing"];
    return YES;
}

// Each caret's line's leading blanks, for Return.
- (std::vector<std::u16string>)newlinesKeepingIndent {
    const std::u16string t = U16(_tv.string);
    std::vector<std::u16string> out;
    for (const Caret &c : _carets) {
        size_t s = c.lo();
        while (s > 0 && t[s - 1] != u'\n' && t[s - 1] != u'\r') s--;
        size_t e = s;
        while (e < c.lo() && (t[e] == u' ' || t[e] == u'\t')) e++;
        out.push_back(u"\n" + t.substr(s, e - s));
    }
    return out;
}

// The carets moved one line up or down, keeping their x.
- (void)moveVertically:(int)dir extend:(BOOL)extend {
    NSTextView *tv = _tv;
    const NSUInteger n = tv.string.length;
    if (_goalX.size() != _carets.size()) {
        _goalX.clear();
        for (const Caret &c : _carets) _goalX.push_back(NSMinX([self caretRectAt:c.head]));
    }
    std::vector<Caret> out;
    std::vector<CGFloat> goals;
    for (size_t i = 0; i < _carets.size(); i++) {
        const Caret &c = _carets[i];
        const NSRect r = [self caretRectAt:c.head];
        const CGFloat y = dir < 0 ? NSMinY(r) - r.size.height / 2 : NSMaxY(r) + r.size.height / 2;
        NSUInteger p;
        if (y < 0) p = 0;
        else if (y > NSHeight(tv.bounds)) p = n;
        else p = [self indexAtPoint:NSMakePoint(_goalX[i], y)];
        out.push_back(extend ? Caret{c.anchor, p} : Caret{p, p});
        goals.push_back(_goalX[i]);
    }
    const std::vector<Caret> norm = MultiCaret::normalize(out, n);
    [self setCarets:out];
    // Carets that merged lose their goals; keep them only when none did.
    _goalX = norm.size() == goals.size() ? goals : std::vector<CGFloat>();
}

- (void)move:(MultiCaret::Move)m extend:(BOOL)extend {
    _goalX.clear();
    [self setCarets:MultiCaret::move(U16(_tv.string), _carets, m, extend)];
}

// Empty carets stretched by a move, then everything selected deleted.
- (void)deleteBy:(MultiCaret::Move)m {
    _goalX.clear();
    const std::u16string t = U16(_tv.string);
    std::vector<Caret> stretched;
    for (const Caret &c : _carets) {
        if (!c.empty()) {
            stretched.push_back(c);
            continue;
        }
        Caret moved = MultiCaret::move(t, {c}, m, true)[0];
        // Never across a line break: a column stays a column.
        const size_t lo = moved.lo(), hi = moved.hi();
        bool crosses = false;
        for (size_t k = lo; k < hi; k++)
            if (t[k] == u'\n' || t[k] == u'\r') crosses = true;
        stretched.push_back(crosses ? c : moved);
    }
    // Stretched carets may now overlap; delete each piece of text once.
    stretched = MultiCaret::normalize(stretched, t.size());
    [self applyEdit:MultiCaret::insert(stretched, {u""}) action:@"Delete"];
}

- (BOOL)doCommand:(SEL)sel {
    if (!self.active) return NO;
    using MultiCaret::Move;
    NSString *name = NSStringFromSelector(sel);
    const BOOL extend = [name hasSuffix:@"AndModifySelection:"];
    if (sel == @selector(cancelOperation:) || sel == @selector(complete:)) {
        // Back to one caret, the first.
        const Caret c = _carets[0];
        [self clear];
        _applying = YES;
        [_tv setSelectedRange:NSMakeRange(c.head, 0)];
        _applying = NO;
        return YES;
    }
    if (sel == @selector(insertNewline:) || sel == @selector(insertNewlineIgnoringFieldEditor:)) {
        _goalX.clear();
        [self insertStrings:[self newlinesKeepingIndent] action:@"Typing"];
        return YES;
    }
    if (sel == @selector(insertTab:) || sel == @selector(insertTabIgnoringFieldEditor:)) {
        _goalX.clear();
        const AutoIndent::Unit unit = AutoIndent::detect(U16(_tv.string), "");
        [self insertStrings:{unit.text()} action:@"Typing"];
        return YES;
    }
    if (sel == @selector(insertBacktab:)) return YES;
    if (sel == @selector(deleteBackward:) ||
        sel == @selector(deleteBackwardByDecomposingPreviousCharacter:)) {
        _goalX.clear();
        [self applyEdit:MultiCaret::backspace(U16(_tv.string), _carets) action:@"Typing"];
        return YES;
    }
    if (sel == @selector(deleteForward:)) {
        _goalX.clear();
        [self applyEdit:MultiCaret::deleteForward(U16(_tv.string), _carets) action:@"Typing"];
        return YES;
    }
    if (sel == @selector(deleteWordBackward:)) { [self deleteBy:Move::WordLeft]; return YES; }
    if (sel == @selector(deleteWordForward:)) { [self deleteBy:Move::WordRight]; return YES; }
    if (sel == @selector(deleteToBeginningOfLine:) ||
        sel == @selector(deleteToBeginningOfParagraph:)) {
        [self deleteBy:Move::LineStart];
        return YES;
    }
    if (sel == @selector(deleteToEndOfLine:) || sel == @selector(deleteToEndOfParagraph:)) {
        [self deleteBy:Move::LineEnd];
        return YES;
    }
    if (sel == @selector(moveUp:) || sel == @selector(moveUpAndModifySelection:)) {
        [self moveVertically:-1 extend:extend];
        return YES;
    }
    if (sel == @selector(moveDown:) || sel == @selector(moveDownAndModifySelection:)) {
        [self moveVertically:1 extend:extend];
        return YES;
    }
    struct { SEL a, b; Move m; } moves[] = {
        {@selector(moveLeft:), @selector(moveLeftAndModifySelection:), Move::Left},
        {@selector(moveBackward:), @selector(moveBackwardAndModifySelection:), Move::Left},
        {@selector(moveRight:), @selector(moveRightAndModifySelection:), Move::Right},
        {@selector(moveForward:), @selector(moveForwardAndModifySelection:), Move::Right},
        {@selector(moveWordLeft:), @selector(moveWordLeftAndModifySelection:), Move::WordLeft},
        {@selector(moveWordBackward:), @selector(moveWordBackwardAndModifySelection:), Move::WordLeft},
        {@selector(moveWordRight:), @selector(moveWordRightAndModifySelection:), Move::WordRight},
        {@selector(moveWordForward:), @selector(moveWordForwardAndModifySelection:), Move::WordRight},
        {@selector(moveToBeginningOfLine:), @selector(moveToBeginningOfLineAndModifySelection:),
         Move::LineStart},
        {@selector(moveToLeftEndOfLine:), @selector(moveToLeftEndOfLineAndModifySelection:),
         Move::LineStart},
        {@selector(moveToEndOfLine:), @selector(moveToEndOfLineAndModifySelection:), Move::LineEnd},
        {@selector(moveToRightEndOfLine:), @selector(moveToRightEndOfLineAndModifySelection:),
         Move::LineEnd},
    };
    for (const auto &mv : moves) {
        if (sel == mv.a || sel == mv.b) {
            [self move:mv.m extend:sel == mv.b];
            return YES;
        }
    }
    // Scrolling (Page Up and Down, Home and End on a Mac keyboard) moves no
    // caret.
    if ([name hasPrefix:@"scroll"]) return NO;
    // Anything else (page moves, select all, transpose...) works on one
    // caret: the first.
    const Caret c = _carets[0];
    [self clear];
    _applying = YES;
    [_tv setSelectedRange:NSMakeRange(c.lo(), c.hi() - c.lo())];
    _applying = NO;
    return NO;
}

- (void)duplicateLinesDown:(BOOL)down {
    NSTextView *tv = _tv;
    _goalX.clear();
    [self applyEdit:MultiCaret::duplicateLines(U16(tv.string), [self currentCarets], down)
             action:down ? @"Copy Line Down" : @"Copy Line Up"];
    if (!self.active) [tv scrollRangeToVisible:tv.selectedRange];
}

- (void)moveLinesDown:(BOOL)down {
    NSTextView *tv = _tv;
    _goalX.clear();
    [self applyEdit:MultiCaret::moveLines(U16(tv.string), [self currentCarets], down)
             action:down ? @"Move Line Down" : @"Move Line Up"];
    if (!self.active) [tv scrollRangeToVisible:tv.selectedRange];
}

// ---- VS Code's selection commands

- (BOOL)findStateHolds {
    return _hasFind && _findLength == _tv.string.length && [self currentCarets] == _findCarets;
}

- (void)rememberFind:(const MultiCaret::Found &)f {
    _hasFind = YES;
    _findLast = f.added;
    _findWholeWord = f.wholeWord;
    _findCarets = [self currentCarets];
    _findLength = _tv.string.length;
}

- (BOOL)addNextOccurrence {
    NSTextView *tv = _tv;
    const std::vector<Caret> current = [self currentCarets];
    if (current.empty()) return NO;
    // A selection made by hand starts a plain search; one that Cmd+D began
    // from an empty caret goes on matching whole words (VS Code's rule).
    const BOOL holds = [self findStateHolds];
    const MultiCaret::Found f = MultiCaret::addNextOccurrence(
        U16(tv.string), current, holds ? _findLast : current.back(), holds && _findWholeWord);
    if (!f.changed) return NO;
    _goalX.clear();
    [self setCarets:f.carets];
    [tv scrollRangeToVisible:NSMakeRange(f.added.lo(), f.added.hi() - f.added.lo())];
    [self rememberFind:f];
    return YES;
}

- (BOOL)selectAllOccurrences {
    NSTextView *tv = _tv;
    const std::vector<Caret> current = [self currentCarets];
    if (current.empty()) return NO;
    const BOOL holds = [self findStateHolds];
    const MultiCaret::Found f = MultiCaret::selectAllOccurrences(
        U16(tv.string), holds ? _findLast : current.back(), holds && _findWholeWord);
    if (!f.changed) return NO;
    _goalX.clear();
    [self setCarets:f.carets];
    [self rememberFind:f];
    return YES;
}

// Where index i lands one line up or down at x, or NSNotFound when there is
// no line there (the first or last line).
- (NSUInteger)indexFrom:(NSUInteger)i x:(CGFloat)x direction:(int)dir {
    NSTextView *tv = _tv;
    const NSRect r = [self caretRectAt:i];
    const CGFloat y = dir < 0 ? NSMinY(r) - r.size.height / 2 : NSMaxY(r) + r.size.height / 2;
    if (y < 0 || y > NSHeight(tv.bounds)) return NSNotFound;
    const NSUInteger p = [self indexAtPoint:NSMakePoint(x, y)];
    const NSRect q = [self caretRectAt:p];
    if (fabs(NSMidY(q) - NSMidY(r)) < r.size.height / 4) return NSNotFound;   // same line
    if (dir > 0 ? NSMidY(q) < NSMidY(r) : NSMidY(q) > NSMidY(r)) return NSNotFound;
    return p;
}

- (BOOL)addCursorVertically:(int)dir {
    NSTextView *tv = _tv;
    const std::vector<Caret> current = [self currentCarets];
    if (current.empty()) return NO;
    // Each caret keeps the x it had when Up or Down began, so a column
    // grown past a short line comes back to its place below it.
    std::vector<CGFloat> goals;
    if (self.active && _goalX.size() == _carets.size()) goals = _goalX;
    else
        for (const Caret &c : current) goals.push_back(NSMinX([self caretRectAt:c.head]));
    std::vector<Caret> all = current;
    std::vector<CGFloat> allGoals = goals;
    NSUInteger reveal = NSNotFound;
    for (size_t i = 0; i < current.size(); i++) {
        const Caret &c = current[i];
        const NSUInteger head = [self indexFrom:c.head x:goals[i] direction:dir];
        if (head == NSNotFound) continue;
        NSUInteger anchor = head;
        if (!c.empty()) {
            anchor = [self indexFrom:c.anchor x:NSMinX([self caretRectAt:c.anchor]) direction:dir];
            if (anchor == NSNotFound) anchor = head;
        }
        all.push_back({anchor, head});
        allGoals.push_back(goals[i]);
        if (reveal == NSNotFound || (dir > 0 ? head > reveal : head < reveal)) reveal = head;
    }
    if (reveal == NSNotFound) return NO;
    [self setCarets:all];
    // The goals follow their carets into normalize's order.
    std::vector<CGFloat> kept;
    for (const Caret &c : _carets) {
        CGFloat g = NSMinX([self caretRectAt:c.head]);
        for (size_t k = 0; k < all.size(); k++)
            if (all[k].head == c.head) { g = allGoals[k]; break; }
        kept.push_back(g);
    }
    _goalX = kept;
    [tv scrollRangeToVisible:NSMakeRange(reveal, 0)];
    return YES;
}

- (BOOL)copy {
    if (!self.active) return NO;
    const std::u16string s = MultiCaret::copy(U16(_tv.string), _carets);
    if (s.empty()) return YES;   // nothing selected: leave the clipboard alone
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:NSFrom(s) forType:NSPasteboardTypeString];
    return YES;
}

- (BOOL)cut {
    if (!self.active) return NO;
    [self copy];
    _goalX.clear();
    [self applyEdit:MultiCaret::insert(_carets, {u""}) action:@"Cut"];
    return YES;
}

- (BOOL)paste {
    if (!self.active) return NO;
    NSString *clip = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!clip.length) return YES;
    _goalX.clear();
    [self insertStrings:MultiCaret::pieces(U16(clip), _carets.size()) action:@"Paste"];
    return YES;
}

// ---- drawing

- (void)drawInRect:(NSRect)dirty {
    if (!self.active) return;
    NSTextView *tv = _tv;
    BOOL key = tv.window.isKeyWindow && tv.window.firstResponder == tv;
    NSColor *color = tv.insertionPointColor ?: NSColor.textColor;
    if (!key) color = [color colorWithAlphaComponent:0.45];
    [color set];
    const NSRect visible = tv.visibleRect;
    for (const Caret &c : _carets) {
        NSRect r = [self caretRectAt:c.head];
        if (!NSIntersectsRect(NSInsetRect(r, -2, 0), visible)) continue;
        NSRect bar = NSMakeRect(floor(r.origin.x), r.origin.y, 1.5, r.size.height);
        if (NSIntersectsRect(bar, dirty)) NSRectFill(bar);
    }
}

@end
