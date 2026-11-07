// MultiCaretView.mm — see MultiCaretView.h.
#import "MultiCaretView.h"
#import "EditingCommands.h"
#import "EditorFont.h"
#import "Lsp.h"
#import "TextKit2.h"
#include "AutoIndent.h"
#include "MultiCaret.h"
#include <algorithm>
#include <string>
#include <vector>

using MultiCaret::Caret;

namespace {

std::u16string U16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}

// The view's text for the line and caret commands, read in blocks near the
// carets rather than copied whole (a 100,000-line file is 5 MB a key
// press). `s` is the storage's live string: use the result before the text
// changes.
MultiCaret::Text TextOf(NSString *s) {
    return MultiCaret::Text(s.length, [s](size_t start, size_t length, char16_t *out) {
        [s getCharacters:(unichar *)out range:NSMakeRange(start, length)];
    });
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

// Cmd+Shift presses come in pairs: the first puts the caret there, the next
// makes the column from it. Shift+Option presses are VS Code's box
// selection: the column from the caret (or from where the last column
// began) to the press. Option alone adds a caret or drags a fresh column.
bool IsPairPress(NSEvent *e) {
    return (e.modifierFlags & kModMask) == (NSEventModifierFlagCommand | NSEventModifierFlagShift);
}
bool IsBoxPress(NSEvent *e) {
    return (e.modifierFlags & kModMask) == (NSEventModifierFlagOption | NSEventModifierFlagShift);
}

// Select All Occurrences, Add Next Occurrence and Add Cursor stop at this
// many carets, VS Code's editor.multiCursorLimit: past it a keystroke at
// every caret costs more than it is worth (each is its own edit). A column
// drag over more lines still gets a caret on each.
constexpr size_t kMaxCarets = 10000;

NSString *LimitNote() {
    return [NSString stringWithFormat:@"The number of cursors has been limited to %zu", kMaxCarets];
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
    // The character the last press landed on, to tell a double-click from
    // a quick click somewhere else (see mouseDown:).
    BOOL _hasLastPress;
    NSUInteger _lastPressIndex;
    // What the last Cmd+D or Shift+Cmd+L left: the selection it added (the
    // next press searches on from it), whether it matches whole words, and
    // the carets and text length then, to tell whether it still holds.
    BOOL _hasFind;
    Caret _findLast;
    BOOL _findWholeWord;
    std::vector<Caret> _findCarets;
    NSUInteger _findLength;
    MCAutoClose *_autoClose;
}

- (instancetype)initWithTextView:(NSTextView *)tv {
    if ((self = [super init])) {
        _tv = tv;
        _autoClose = [[MCAutoClose alloc] initWithTextView:tv];
        // A caret the view moves itself (a click, an arrow, typing at one
        // caret) lets go of the auto-closed pairs it left.
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(selectionChanged:)
                                                     name:NSTextViewDidChangeSelectionNotification
                                                   object:tv];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (MCAutoClose *)autoClose { return _autoClose; }

- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    if (!_applying) [_autoClose caretsAre:[self currentCarets]];
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
        NSFont *f = tv.font ?: MCEditorFont();
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
    [_autoClose caretsAre:carets];
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
    _goalX.clear();
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

// Follows the pointer until the button comes up, putting the column from
// (anchor, anchorX) to the pointer. With `slop` the pointer must first move
// 3 points from the press; YES when it was dragged.
- (BOOL)trackColumnFrom:(NSUInteger)anchor x:(CGFloat)anchorX
                  press:(NSPoint)p0
                   slop:(BOOL)slop
                   mask:(NSEventMask)mask
                     up:(NSEventType)upType {
    NSTextView *tv = _tv;
    BOOL dragged = NO;
    NSUInteger lastA1 = NSNotFound;
    CGFloat lastX1 = -1;
    for (;;) {
        NSEvent *e = [tv.window nextEventMatchingMask:mask];
        if (!e || e.type == upType) break;
        const NSPoint p1 = [tv convertPoint:e.locationInWindow fromView:nil];
        if (slop && !dragged && fabs(p1.x - p0.x) < 3 && fabs(p1.y - p0.y) < 3) continue;
        dragged = YES;
        [tv autoscroll:e];
        const NSPoint q = [tv convertPoint:e.locationInWindow fromView:nil];
        const NSUInteger a1 = [self indexAtPoint:q];
        if (a1 == lastA1 && q.x == lastX1) continue;
        lastA1 = a1;
        lastX1 = q.x;
        [self setCarets:[self columnFrom:anchor x:anchorX to:a1 x:q.x]];
    }
    return dragged;
}

- (BOOL)mouseDown:(NSEvent *)event {
    NSTextView *tv = _tv;
    if (!tv.editable) return NO;
    const NSPoint p0 = [tv convertPoint:event.locationInWindow fromView:nil];
    const NSUInteger a0 = [self indexAtPoint:p0];
    // The window server counts clicks by time alone: a press within the
    // double-click interval of the last one has a clickCount of 2 wherever
    // it lands, so a quick Cmd+Shift click a few lines down came as a
    // double-click and went to NSTextView, which extended the selection.
    // Only a press on the character the last one landed on is a
    // double-click here, as in VS Code (MouseDownState.trySetCount).
    const BOOL repeat = event.clickCount > 1 && _hasLastPress && a0 == _lastPressIndex;
    _hasLastPress = YES;
    _lastPressIndex = a0;
    if (!IsColumnPress(event) || repeat) return NO;
    if (tv.window.firstResponder != tv) [tv.window makeFirstResponder:tv];
    _goalX.clear();

    // Cmd+Shift: the first press puts the caret there; the next, clicked or
    // dragged, makes the column from that place to this one, and further
    // ones move its far end. Shift+Option, as in VS Code: the column from
    // the caret (its selection's start) to the press, or from where the
    // last column began while that column still holds.
    if (IsPairPress(event) || IsBoxPress(event)) {
        const BOOL extending = _hasAnchor && _anchorLength == tv.string.length &&
                               [self currentCarets] == _anchorCarets;
        NSUInteger anchor = a0;
        CGFloat anchorX = p0.x;
        if (extending) {
            anchor = _anchorIndex;
            anchorX = _anchorX;
        } else if (IsBoxPress(event)) {
            anchor = MIN((NSUInteger)[self currentCarets].front().anchor, tv.string.length);
            anchorX = NSMinX([self caretRectAt:anchor]);
        }
        [self setCarets:[self columnFrom:anchor x:anchorX to:a0 x:p0.x]];
        [self trackColumnFrom:anchor x:anchorX press:p0 slop:NO
                         mask:NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp
                           up:NSEventTypeLeftMouseUp];
        [self rememberAnchor:anchor x:anchorX];
        return YES;
    }

    // Option: a drag is a fresh column from the press (this replaces
    // NSTextView's own Option-drag rectangle).
    if ([self trackColumnFrom:a0 x:p0.x press:p0 slop:YES
                         mask:NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp
                           up:NSEventTypeLeftMouseUp]) {
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

- (BOOL)otherMouseDown:(NSEvent *)event {
    NSTextView *tv = _tv;
    if (!tv.editable || event.buttonNumber != 2 || (event.modifierFlags & kModMask)) return NO;
    if (tv.window.firstResponder != tv) [tv.window makeFirstResponder:tv];
    const NSPoint p0 = [tv convertPoint:event.locationInWindow fromView:nil];
    const NSUInteger a0 = [self indexAtPoint:p0];
    _goalX.clear();
    _hasLastPress = YES;
    _lastPressIndex = a0;
    [self setCarets:{{a0, a0}}];
    [self trackColumnFrom:a0 x:p0.x press:p0 slop:NO
                     mask:NSEventMaskOtherMouseDragged | NSEventMaskOtherMouseUp
                       up:NSEventTypeOtherMouseUp];
    [self rememberAnchor:a0 x:p0.x];
    return YES;
}

// ---- edits

// Applies an edit as one undo step, through the text view so undo, the
// highlighter, the language server and the dirty mark all see it, each
// change its own storage edit (CodeTextView replaceRanges:).
- (void)applyEdit:(const MultiCaret::Edit &)e action:(NSString *)action {
    CodeTextView *tv = (CodeTextView *)_tv;
    if (e.changes.empty()) {
        [self setCarets:e.carets];
        return;
    }
    NSMutableArray *ranges = [NSMutableArray arrayWithCapacity:e.changes.size()];
    NSMutableArray *strings = [NSMutableArray arrayWithCapacity:e.changes.size()];
    for (const MultiCaret::Change &c : e.changes) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(c.start, c.length)]];
        [strings addObject:NSFrom(c.text)];
    }
    _applying = YES;
    const BOOL ok = [tv replaceRanges:ranges withStrings:strings actionName:action];
    _applying = NO;
    if (ok) [self setCarets:e.carets];
}

- (void)insertStrings:(const std::vector<std::u16string> &)texts action:(NSString *)action {
    [self applyEdit:MultiCaret::insert(_carets, texts) action:action];
}

- (BOOL)insertText:(id)text {
    return [self insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
}

- (BOOL)insertText:(id)text replacementRange:(NSRange)replacement {
    NSString *s = [text isKindOfClass:[NSAttributedString class]] ? [text string] : text;
    if (![s isKindOfClass:[NSString class]]) return NO;
    // One character typed at one caret or several: the auto-closing rules
    // first (AutoClose: type over a closer they put there, close a bracket
    // or quote, or wrap every selection in the pair). Not while an input
    // method composes (its text replaces the marked range, which is a
    // selection only to it), nor for text replacing a range of its own (the
    // accent menu's).
    NSTextView *tv = _tv;
    if (s.length == 1 && tv.editable && !tv.hasMarkedText && replacement.location == NSNotFound) {
        const std::vector<Caret> carets =
            MultiCaret::normalize([self currentCarets], tv.string.length);
        const AutoClose::Typed t = [_autoClose type:[s characterAtIndex:0] carets:carets];
        if (t.kind != AutoClose::Typed::Kind::None) {
            _goalX.clear();
            [self applyEdit:t.edit action:@"Typing"];
            if (t.kind == AutoClose::Typed::Kind::Close) [_autoClose remember:t.closed];
            if (!self.active) [tv scrollRangeToVisible:tv.selectedRange];
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
    const MultiCaret::Text t = TextOf(_tv.string);
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

// The x of a caret at character i, in the view's coordinates, and the
// glyphs of its line fragment, from TextKit 1's layout manager (which lays
// out what it needs, on screen or not). The end of a text ending in a line
// break is the extra line fragment, given an empty glyph range at the end.
// The glyph count, which generates every glyph, is asked for only at the
// end of the text, which is then laid out anyway.
static CGFloat MCCaretX(NSLayoutManager *lm, NSTextContainer *tc, NSPoint origin,
                        NSUInteger i, NSUInteger n, NSRange *lineGlyphs, NSRect *lineRect) {
    if (i >= n) {
        const NSUInteger glyphs = lm.numberOfGlyphs;
        if (glyphs == 0 || !NSIsEmptyRect(lm.extraLineFragmentRect)) {
            const NSRect r = lm.extraLineFragmentRect;
            *lineGlyphs = NSMakeRange(glyphs, 0);
            *lineRect = r;
            return origin.x + NSMinX(r) + tc.lineFragmentPadding;
        }
        // After the last character, on its line.
        const NSUInteger last = glyphs - 1;
        *lineRect = [lm lineFragmentRectForGlyphAtIndex:last effectiveRange:lineGlyphs];
        const NSRect box = [lm boundingRectForGlyphRange:NSMakeRange(last, 1) inTextContainer:tc];
        return origin.x + NSMaxX(box);
    }
    const NSUInteger g = [lm glyphIndexForCharacterAtIndex:i];
    *lineRect = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:lineGlyphs];
    return origin.x + NSMinX(*lineRect) + [lm locationForGlyphAtIndex:g].x;
}

// Lays out the text up to the line after the one holding `furthest`, which
// is all Up, Down and Add Cursor need: a whole layout of 100,000 lines took
// about 2 s, and every edit made the next press pay again from there.
static void MCLayOutPast(NSLayoutManager *lm, NSTextContainer *tc, NSString *text,
                         NSUInteger furthest) {
    const NSUInteger n = text.length;
    NSUInteger end = n;
    if (furthest < n) {
        end = NSMaxRange([text lineRangeForRange:NSMakeRange(furthest, 0)]);
        if (end < n) end = NSMaxRange([text lineRangeForRange:NSMakeRange(end, 0)]);
    }
    if (end >= n) [lm ensureLayoutForTextContainer:tc];
    else [lm ensureLayoutForCharacterRange:NSMakeRange(0, end)];
}

// The caret index on the line fragment holding glyphs `line` (rect `rect`)
// nearest x (view coordinates), never past the line's own end.
static NSUInteger MCIndexOnLine(NSLayoutManager *lm, NSTextContainer *tc, NSPoint origin,
                                NSString *text, NSRange line, NSRect rect, CGFloat x) {
    const NSUInteger n = text.length;
    if (line.length == 0) return n;   // the extra line fragment
    // The glyph starts on the line are in order (left to right), so a
    // binary search finds the nearest to x; glyphIndexForPoint: took nine
    // tenths of the time.
    const CGFloat tx = x - origin.x - NSMinX(rect);
    NSUInteger lo = line.location, hi = NSMaxRange(line);   // first start past tx in [lo, hi]
    while (lo < hi) {
        const NSUInteger mid = lo + (hi - lo) / 2;
        if ([lm locationForGlyphAtIndex:mid].x <= tx) lo = mid + 1;
        else hi = mid;
    }
    NSUInteger g = lo;   // the first glyph starting right of tx, or the line's end
    const NSRange chars = [lm characterRangeForGlyphRange:line actualGlyphRange:NULL];
    NSUInteger ch;
    if (g > line.location) {
        const CGFloat left = [lm locationForGlyphAtIndex:g - 1].x;
        // Past every glyph's start: the right candidate is the end of the
        // last glyph (what follows it on the last line of the text).
        const CGFloat right = g < NSMaxRange(line)
            ? [lm locationForGlyphAtIndex:g].x
            : NSMaxX([lm boundingRectForGlyphRange:NSMakeRange(g - 1, 1) inTextContainer:tc]) -
                  NSMinX(rect);
        if (tx - left <= right - tx) ch = [lm characterIndexForGlyphAtIndex:g - 1];
        else if (g < NSMaxRange(line)) ch = [lm characterIndexForGlyphAtIndex:g];
        else ch = NSMaxRange(chars);
    } else {
        ch = [lm characterIndexForGlyphAtIndex:g];
    }
    NSUInteger end = NSMaxRange(chars);
    if (end > chars.location && end <= n) {
        const unichar last = [text characterAtIndex:end - 1];
        // A caret goes before the line's break, or before the last character
        // of a fragment that wraps onto the next.
        if (last == '\n' || last == '\r' || last == 0x2028 || end < n) end--;
    }
    return MAX(chars.location, MIN(ch, end));
}

// Where a caret on the line fragment `line` goes one fragment up (dir < 0)
// or down, at x `goal` (view coordinates), or NSNotFound when there is no
// line there: up from the first line, down from the last (or from the last
// text line when no line break ends the text; after one, the empty line
// below it is the end). The layout must reach the line below.
static NSUInteger MCIndexAcross(NSLayoutManager *lm, NSTextContainer *tc, NSPoint origin,
                                NSString *text, NSRange line, int dir, CGFloat goal) {
    const NSUInteger n = text.length;
    if (dir > 0) {
        if (line.length == 0) return NSNotFound;   // the extra line is the last
        const NSUInteger chars = NSMaxRange([lm characterRangeForGlyphRange:line
                                                           actualGlyphRange:NULL]);
        if (chars >= n) {
            const unichar last = n ? [text characterAtIndex:n - 1] : 0;
            return last == '\n' || last == '\r' || last == 0x2029 ? n : NSNotFound;
        }
        NSRange below;
        const NSRect r = [lm lineFragmentRectForGlyphAtIndex:NSMaxRange(line) effectiveRange:&below];
        return MCIndexOnLine(lm, tc, origin, text, below, r, goal);
    }
    NSUInteger glyph;
    if (line.length == 0) {   // from the extra line to the last line
        const NSUInteger glyphs = lm.numberOfGlyphs;
        if (glyphs == 0) return NSNotFound;
        glyph = glyphs - 1;
    } else if (line.location == 0) {
        return NSNotFound;
    } else {
        glyph = line.location - 1;
    }
    NSRange above;
    const NSRect r = [lm lineFragmentRectForGlyphAtIndex:glyph effectiveRange:&above];
    return MCIndexOnLine(lm, tc, origin, text, above, r, goal);
}

// The carets moved one line up or down, keeping their x. Through the
// layout manager, line by line: firstRectForCharacterRange:, used before,
// cost about 65 µs a caret (3.3 s for 50,000) and gave nothing for lines
// not laid out yet, which sent those carets to the top and merged them.
// Now about 2 µs a caret once the text is laid out (105 ms for 50,000).
- (BOOL)moveVerticallyByLayout:(int)dir extend:(BOOL)extend {
    NSTextView *tv = _tv;
    if (MCUsesTextKit2(tv) || !tv.layoutManager || !tv.textContainer) return NO;
    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    NSString *text = tv.string;
    const NSUInteger n = text.length;
    const NSPoint origin = tv.textContainerOrigin;
    NSUInteger furthest = 0;
    for (const Caret &c : _carets) furthest = MAX(furthest, MAX(c.head, c.anchor));
    MCLayOutPast(lm, tc, text, furthest);
    const BOOL keepGoals = _goalX.size() == _carets.size();
    std::vector<Caret> out;
    std::vector<CGFloat> goals;
    out.reserve(_carets.size());
    goals.reserve(_carets.size());
    for (size_t i = 0; i < _carets.size(); i++) {
        const Caret &c = _carets[i];
        NSRange line;
        NSRect rect;
        const CGFloat x = MCCaretX(lm, tc, origin, c.head, n, &line, &rect);
        const CGFloat goal = keepGoals ? _goalX[i] : x;
        NSUInteger p = MCIndexAcross(lm, tc, origin, text, line, dir, goal);
        if (p == NSNotFound) p = dir > 0 ? n : 0;   // the end, or the start, of the text
        out.push_back(extend ? Caret{c.anchor, p} : Caret{p, p});
        goals.push_back(goal);
    }
    const std::vector<Caret> norm = MultiCaret::normalize(out, n);
    [self setCarets:out];
    _goalX = norm.size() == goals.size() ? goals : std::vector<CGFloat>();
    return YES;
}

// The carets moved one line up or down, keeping their x.
- (void)moveVertically:(int)dir extend:(BOOL)extend {
    if ([self moveVerticallyByLayout:dir extend:extend]) return;
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
    [self setCarets:MultiCaret::move(TextOf(_tv.string), _carets, m, extend)];
}

// Empty carets stretched by a move, then everything selected deleted.
- (void)deleteBy:(MultiCaret::Move)m {
    _goalX.clear();
    const MultiCaret::Text t = TextOf(_tv.string);
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
    // Backspace between an empty pair auto-closing put in deletes both, at
    // one caret or several.
    if (sel == @selector(deleteBackward:) && _tv.editable && !_tv.hasMarkedText) {
        const MultiCaret::Edit e = [_autoClose
            deletePairAt:MultiCaret::normalize([self currentCarets], _tv.string.length)];
        if (!e.changes.empty()) {
            _goalX.clear();
            [self applyEdit:e action:@"Typing"];
            if (!self.active) [_tv scrollRangeToVisible:_tv.selectedRange];
            return YES;
        }
    }
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
        [self applyEdit:MultiCaret::backspace(TextOf(_tv.string), _carets) action:@"Typing"];
        return YES;
    }
    if (sel == @selector(deleteForward:)) {
        _goalX.clear();
        [self applyEdit:MultiCaret::deleteForward(TextOf(_tv.string), _carets) action:@"Typing"];
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
    [self applyEdit:MultiCaret::duplicateLines(TextOf(tv.string), [self currentCarets], down)
             action:down ? @"Copy Line Down" : @"Copy Line Up"];
    if (!self.active) [tv scrollRangeToVisible:tv.selectedRange];
}

- (void)moveLinesDown:(BOOL)down {
    NSTextView *tv = _tv;
    _goalX.clear();
    [self applyEdit:MultiCaret::moveLines(TextOf(tv.string), [self currentCarets], down)
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
    if (current.size() >= kMaxCarets) {
        if (_onNote) _onNote(LimitNote());
        return NO;
    }
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
    MultiCaret::Found f = MultiCaret::selectAllOccurrences(
        U16(tv.string), holds ? _findLast : current.back(), holds && _findWholeWord);
    if (!f.changed) return NO;
    if (f.carets.size() > kMaxCarets) {
        // The ones around the selection it started from.
        const auto at = std::lower_bound(f.carets.begin(), f.carets.end(), f.added,
                                         [](const Caret &a, const Caret &b) { return a.lo() < b.lo(); });
        const size_t i = (size_t)(at - f.carets.begin());
        const size_t from = std::min(i > kMaxCarets / 2 ? i - kMaxCarets / 2 : 0,
                                     f.carets.size() - kMaxCarets);
        f.carets = std::vector<Caret>(f.carets.begin() + (long)from,
                                      f.carets.begin() + (long)(from + kMaxCarets));
        if (_onNote) _onNote(LimitNote());
    }
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
    if (current.size() * 2 > kMaxCarets) {
        if (_onNote) _onNote(LimitNote());
        return NO;
    }
    // Each caret keeps the x it had when Up or Down began, so a column
    // grown past a short line comes back to its place below it.
    const BOOL keepGoals = self.active && _goalX.size() == _carets.size();
    // Through the layout manager, as Up and Down are: a caret costs a few
    // layout queries (firstRectForCharacterRange: cost 65 µs), laid out
    // only as far as the furthest caret.
    NSLayoutManager *lm = MCUsesTextKit2(tv) ? nil : tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    NSString *text = tv.string;
    const NSUInteger n = text.length;
    const NSPoint origin = tv.textContainerOrigin;
    if (lm && tc) {
        NSUInteger furthest = 0;
        for (const Caret &c : current) furthest = MAX(furthest, MAX(c.head, c.anchor));
        MCLayOutPast(lm, tc, text, furthest);
    }
    // Where i goes, and the x it is at.
    auto across = [&](NSUInteger i, CGFloat goal, BOOL useGoal, CGFloat *x) -> NSUInteger {
        if (lm && tc) {
            NSRange line;
            NSRect rect;
            *x = MCCaretX(lm, tc, origin, i, n, &line, &rect);
            return MCIndexAcross(lm, tc, origin, text, line, dir, useGoal ? goal : *x);
        }
        *x = NSMinX([self caretRectAt:i]);
        return [self indexFrom:i x:useGoal ? goal : *x direction:dir];
    };
    std::vector<Caret> all = current;
    std::vector<std::pair<NSUInteger, CGFloat>> goalAt;   // head, goal
    goalAt.reserve(current.size() * 2);
    NSUInteger reveal = NSNotFound;
    for (size_t i = 0; i < current.size(); i++) {
        const Caret &c = current[i];
        CGFloat x = 0;
        const NSUInteger head = across(c.head, keepGoals ? _goalX[i] : 0, keepGoals, &x);
        const CGFloat goal = keepGoals ? _goalX[i] : x;
        goalAt.push_back({c.head, goal});
        if (head == NSNotFound) continue;
        NSUInteger anchor = head;
        if (!c.empty()) {
            CGFloat ax = 0;
            anchor = across(c.anchor, 0, NO, &ax);
            if (anchor == NSNotFound) anchor = head;
        }
        all.push_back({anchor, head});
        goalAt.push_back({head, goal});
        if (reveal == NSNotFound || (dir > 0 ? head > reveal : head < reveal)) reveal = head;
    }
    if (reveal == NSNotFound) return NO;
    [self setCarets:all];
    // The goals follow their carets into normalize's order.
    std::stable_sort(goalAt.begin(), goalAt.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<CGFloat> kept;
    kept.reserve(_carets.size());
    for (const Caret &c : _carets) {
        auto it = std::lower_bound(goalAt.begin(), goalAt.end(), c.head,
                                   [](const auto &a, NSUInteger h) { return a.first < h; });
        kept.push_back(it != goalAt.end() && it->first == c.head ? it->second
                                                                 : NSMinX([self caretRectAt:c.head]));
    }
    _goalX = kept;
    [tv scrollRangeToVisible:NSMakeRange(reveal, 0)];
    return YES;
}

// ---- whole lines

- (void)expandLineSelection {
    NSTextView *tv = _tv;
    _goalX.clear();
    [self setCarets:MultiCaret::expandLineSelection(TextOf(tv.string), [self currentCarets])];
    // setCarets: puts a single selection back in the text view, forwards.
    if (!self.active) [tv scrollRangeToVisible:tv.selectedRange];
}

- (void)deleteLines {
    NSTextView *tv = _tv;
    _goalX.clear();
    [self applyEdit:MultiCaret::deleteLines(TextOf(tv.string), [self currentCarets])
             action:@"Delete Line"];
    if (!self.active) [tv scrollRangeToVisible:tv.selectedRange];
}

// Copy, Cut and Paste with nothing selected work on whole lines (VS Code):
// the copy is marked with a type of minicode's own beside the text, so a
// paste can tell it from text copied anywhere else and put it above the
// caret's line instead of at the caret. Another app's copy clears the mark
// with the rest of the pasteboard.
static NSPasteboardType const kLineCopyType = @"org.minicode.editor.line-copy";

- (BOOL)nothingSelected {
    for (const Caret &c : [self currentCarets])
        if (!c.empty()) return NO;
    return YES;
}

- (void)copyLines {
    const std::u16string s = MultiCaret::lineCopy(TextOf(_tv.string), [self currentCarets]);
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:NSFrom(s) forType:NSPasteboardTypeString];
    [pb setString:NSFrom(s) forType:kLineCopyType];
}

- (BOOL)copy {
    if (_tv.editable && [self nothingSelected]) {
        [self copyLines];
        return YES;
    }
    if (!self.active) return NO;
    const std::u16string s = MultiCaret::copy(TextOf(_tv.string), _carets);
    if (s.empty()) return YES;   // nothing selected: leave the clipboard alone
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:NSFrom(s) forType:NSPasteboardTypeString];
    return YES;
}

- (BOOL)cut {
    if (_tv.editable && [self nothingSelected]) {
        [self copyLines];
        _goalX.clear();
        [self applyEdit:MultiCaret::deleteLines(TextOf(_tv.string), [self currentCarets])
                 action:@"Cut"];
        return YES;
    }
    if (!self.active) return NO;
    [self copy];
    _goalX.clear();
    [self applyEdit:MultiCaret::insert(_carets, {u""}) action:@"Cut"];
    return YES;
}

- (BOOL)paste {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *clip = [pb stringForType:NSPasteboardTypeString];
    // A whole-line copy, still the text on the pasteboard, with no selection.
    NSString *lines = [pb stringForType:kLineCopyType];
    if (_tv.editable && clip.length && [lines isEqualToString:clip] && [self nothingSelected]) {
        _goalX.clear();
        [self applyEdit:MultiCaret::pasteLinesAbove(TextOf(_tv.string), [self currentCarets],
                                                    U16(clip))
                 action:@"Paste"];
        if (!self.active) [_tv scrollRangeToVisible:_tv.selectedRange];
        return YES;
    }
    if (!self.active) return NO;
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
    // Only carets in the characters on screen are placed: finding where a
    // caret is drawn costs a layout query, and 50,000 of them took 6 s a
    // frame. The editor is TextKit 1 (see Lsp.mm); never ask a TextKit 2
    // view for its layout manager, which would switch it over.
    NSUInteger lo = 0, hi = NSUIntegerMax;
    if (!MCUsesTextKit2(tv) && tv.layoutManager && tv.textContainer) {
        const NSPoint o = tv.textContainerOrigin;
        NSRect area = NSOffsetRect(NSInsetRect(visible, 0, -4), -o.x, -o.y);
        NSLayoutManager *lm = tv.layoutManager;
        NSRange glyphs = [lm glyphRangeForBoundingRect:area inTextContainer:tv.textContainer];
        NSRange chars = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
        lo = chars.location;
        hi = NSMaxRange(chars);
    }
    for (const Caret &c : _carets) {
        if (c.head < lo || c.head > hi) continue;
        NSRect r = [self caretRectAt:c.head];
        if (!NSIntersectsRect(NSInsetRect(r, -2, 0), visible)) continue;
        NSRect bar = NSMakeRect(floor(r.origin.x), r.origin.y, 1.5, r.size.height);
        if (NSIntersectsRect(bar, dirty)) NSRectFill(bar);
    }
}

@end
