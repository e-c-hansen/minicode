// Gutter.mm — see Gutter.h.
#import "Gutter.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import "EditorFont.h"
#import "FoldView.h"
#include "LineIndex.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

// Lanes, left to right: a diagnostic dot, the numbers, and the fold lane:
// a chevron beside each line a region starts on, shown while the pointer
// is over the gutter (as VS Code does), and always for a closed fold.
static const CGFloat kDotLane = 12;
static const CGFloat kFoldLane = 14;
// The change bars' lane, between the dots and the numbers, while on.
static const CGFloat kChangeLane = 5;
static const CGFloat kChangeBarWidth = 3;
static const NSUInteger kMinDigits = 3;

@implementation MCLineGutter {
    __weak CodeTextView *_tv;
    LineIndex _index;
    NSUInteger _digits;
    CGFloat _digitWidth;
    NSUInteger _dragAnchorLine;
    BOOL _observing;
    BOOL _thicknessQueued;
    BOOL _hovering;                 // the pointer is over the gutter
    NSTrackingArea *_tracking;
    std::vector<QuickDiff::Bar> _bars;   // sorted by line
    NSColor *_barColors[3];              // added, modified, deleted
}

- (instancetype)initWithTextView:(CodeTextView *)tv {
    NSScrollView *scroll = tv.enclosingScrollView;
    if ((self = [super initWithScrollView:scroll orientation:NSVerticalRuler])) {
        _tv = tv;
        _mode = MCLineNumbersOn;
        _digits = kMinDigits;
        self.font = MCEditorFont();
        self.clientView = tv;
        self.reservedThicknessForMarkers = 0;
        self.reservedThicknessForAccessoryView = 0;
        self.wantsLayer = YES;
        // NSTextView puts its paragraph markers on a ruler it uses; this one
        // holds line numbers only.
        tv.usesRuler = NO;
        scroll.verticalRulerView = self;
        scroll.hasVerticalRuler = YES;
        [self reindex];
        [self updateThickness];
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification object:tv.textStorage];
        [nc addObserver:self selector:@selector(selectionChanged:)
                   name:NSTextViewDidChangeSelectionNotification object:tv];
        scroll.contentView.postsBoundsChangedNotifications = YES;
        [nc addObserver:self selector:@selector(redraw:)
                   name:NSViewBoundsDidChangeNotification object:scroll.contentView];
        tv.postsFrameChangedNotifications = YES;
        [nc addObserver:self selector:@selector(redraw:)
                   name:NSViewFrameDidChangeNotification object:tv];
        [tv addObserver:self forKeyPath:@"editable" options:0 context:NULL];
        __weak MCLineGutter *weakSelf = self;
        tv.folds.onChange = ^{ weakSelf.needsDisplay = YES; };
        _observing = YES;
        [self updateVisibility];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    if (_observing) [_tv removeObserver:self forKeyPath:@"editable"];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    (void)keyPath; (void)object; (void)change; (void)context;
    [self updateVisibility];
}

- (BOOL)shown {
    return self.scrollView.rulersVisible && self.scrollView.hasVerticalRuler;
}

- (void)updateVisibility {
    CodeTextView *tv = _tv;
    const BOOL want = _mode != MCLineNumbersOff && tv.editable;
    NSScrollView *scroll = self.scrollView;
    if (scroll.rulersVisible != want) scroll.rulersVisible = want;
    self.needsDisplay = YES;
}

- (void)setMode:(MCLineNumbers)mode {
    _mode = mode;
    [self updateVisibility];
}

- (void)setFont:(NSFont *)font {
    _font = font;
    _digitWidth = [@"8" sizeWithAttributes:@{NSFontAttributeName: font}].width;
    [self updateThickness];
}

- (void)setBackgroundColor:(NSColor *)color {
    _backgroundColor = color;
    self.layer.backgroundColor = color.CGColor;
}

- (void)setNumberColor:(NSColor *)color {
    _numberColor = color;
    self.needsDisplay = YES;
}

- (void)setActiveNumberColor:(NSColor *)color {
    _activeNumberColor = color;
    self.needsDisplay = YES;
}

// ---- the index, and the width it asks for
- (void)reindex {
    NSString *s = _tv.textStorage.string;
    std::vector<unichar> buf(s.length);
    if (s.length) [s getCharacters:buf.data() range:NSMakeRange(0, s.length)];
    _index.reset(reinterpret_cast<const char16_t *>(buf.data()), buf.size());
}

- (void)updateThickness {
    NSUInteger digits = 1;
    for (size_t n = _index.lineCount(); n >= 10; n /= 10) digits++;
    digits = MAX(digits, kMinDigits);
    const CGFloat width =
        ceil(kDotLane + (_changeLane ? kChangeLane : 0) + digits * _digitWidth + kFoldLane);
    if (digits == _digits && self.ruleThickness == width) return;
    _digits = digits;
    self.ruleThickness = width;   // NSRulerView asks its scroll view to tile
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSRange er = ts.editedRange;
    const NSInteger delta = ts.changeInLength;
    const NSUInteger oldLen = (NSUInteger)((NSInteger)er.length - delta);
    if (er.location + oldLen > _index.length() ||
        _index.length() + er.length - oldLen != ts.length) {
        [self reindex];   // out of step (never expected): start again
        _bars.clear();
    } else {
        // The change bars move with the lines (only when there are some);
        // a whole new text (another tab) has none until its own diff.
        if (er.location == 0 && oldLen == _index.length() && er.length == ts.length)
            _bars.clear();
        size_t firstLine = 0, removed = 0;
        if (!_bars.empty()) {
            firstLine = _index.lineOf(er.location);
            removed = _index.lineOf(er.location + oldLen) - firstLine;
        }
        std::vector<unichar> buf(er.length);
        NSString *s = ts.string;
        if (er.length) [s getCharacters:buf.data() range:er];
        _index.edit(er.location, oldLen, reinterpret_cast<const char16_t *>(buf.data()),
                    buf.size(), er.location ? [s characterAtIndex:er.location - 1] : 0,
                    NSMaxRange(er) < s.length ? [s characterAtIndex:NSMaxRange(er)] : 0);
        if (!_bars.empty()) {
            const size_t added = _index.lineOf(NSMaxRange(er)) - firstLine;
            QuickDiff::shift(_bars, firstLine, removed, added);
        }
    }
    [self updateThicknessLater];
    self.needsDisplay = YES;
}

// Not from inside the storage's edit: the layout manager has not heard of
// it yet, and the scroll view's tile asks it to lay out text that is gone.
- (void)updateThicknessLater {
    if (_thicknessQueued) return;
    _thicknessQueued = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_thicknessQueued = NO;
        [self updateThickness];
    });
}

- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    self.needsDisplay = YES;
}

- (void)redraw:(NSNotification *)note {
    (void)note;
    if (self.shown) self.needsDisplay = YES;
}

- (NSUInteger)lineOfCharacter:(NSUInteger)index {
    return _index.lineOf(index);
}

- (NSUInteger)startOfLine:(NSUInteger)line {
    return line < _index.lineCount() ? _index.lineStart(line) : _index.length();
}

- (NSUInteger)lineCount { return _index.lineCount(); }

// ---- the change bars
- (void)setChangeLane:(BOOL)on {
    if (_changeLane == on) return;
    _changeLane = on;
    [self updateThicknessLater];
    self.needsDisplay = YES;
}

- (void)setChangeBars:(const std::vector<QuickDiff::Bar> &)bars {
    if (bars == _bars) return;
    _bars = bars;
    self.needsDisplay = YES;
}

- (const std::vector<QuickDiff::Bar> &)changeBars { return _bars; }

- (void)setChangeColorsAdded:(NSColor *)added modified:(NSColor *)modified
                     deleted:(NSColor *)deleted {
    _barColors[0] = added;
    _barColors[1] = modified;
    _barColors[2] = deleted;
    if (!_bars.empty()) self.needsDisplay = YES;
}

- (NSColor *)colorOfBar:(const QuickDiff::Bar &)b {
    const int k = b.kind == QuickDiff::Kind::Added ? 0 : b.kind == QuickDiff::Kind::Modified ? 1 : 2;
    return _barColors[k] ?: (k == 0 ? NSColor.systemGreenColor
                             : k == 1 ? NSColor.systemBlueColor : NSColor.systemRedColor);
}

// The bars' x, in the lane left of the numbers.
- (CGFloat)changeBarX { return kDotLane; }

// A line's rect in the gutter (its first fragment to its last), from the
// layout manager; NSZeroRect when the line is past the text.
- (NSRect)rectOfLines:(size_t)line count:(size_t)count {
    CodeTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    if (!lm || line >= _index.lineCount()) return NSZeroRect;
    const NSPoint origin = tv.textContainerOrigin;
    const size_t last = MIN(line + MAX(count, (size_t)1), _index.lineCount()) - 1;
    const NSUInteger start = _index.lineStart(line);
    const NSUInteger end = MAX(_index.lineEnd(last), start);
    NSRect r = NSZeroRect;
    if (start >= tv.textStorage.length) {
        r = lm.extraLineFragmentRect;   // the empty line after a final break
    } else {
        const NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(start, MAX(end - start, (NSUInteger)1))
                                          actualCharacterRange:NULL];
        NSRect first = [lm lineFragmentRectForGlyphAtIndex:glyphs.location effectiveRange:NULL];
        NSRect lastR = [lm lineFragmentRectForGlyphAtIndex:NSMaxRange(glyphs) - 1 effectiveRange:NULL];
        r = NSUnionRect(first, lastR);
    }
    if (NSHeight(r) <= 0) return NSZeroRect;
    r.origin.y += origin.y;
    r = [self convertRect:r fromView:tv];
    return NSMakeRect(self.changeBarX, NSMinY(r), kChangeBarWidth, NSHeight(r));
}

- (NSRect)rectOfChangeBar:(NSInteger)index {
    if (index < 0 || (size_t)index >= _bars.size()) return NSZeroRect;
    const QuickDiff::Bar &b = _bars[(size_t)index];
    if (b.kind != QuickDiff::Kind::Deleted) return [self rectOfLines:b.line count:b.lines];
    // A deletion: the boundary below the line above it (or above line 0).
    NSRect at = [self rectOfLines:QuickDiff::anchorLine(b) count:1];
    if (NSIsEmptyRect(at)) return NSZeroRect;
    const CGFloat y = b.line == 0 ? NSMinY(at) : NSMaxY(at);
    return NSMakeRect(self.changeBarX, y - 4, 6, 8);
}

// Only the bars whose lines are among `lines` (the first and last line on
// show) are drawn.
- (void)drawChangeBarsFrom:(size_t)firstLine to:(size_t)lastLine {
    if (_bars.empty() || !_changeLane) return;
    auto from = std::lower_bound(_bars.begin(), _bars.end(), firstLine,
        [](const QuickDiff::Bar &b, size_t line) {
            return b.line + std::max<size_t>(b.lines, 1) <= line;
        });
    for (auto it = from; it != _bars.end() && QuickDiff::anchorLine(*it) <= lastLine; ++it) {
        const NSRect r = [self rectOfChangeBar:(NSInteger)(it - _bars.begin())];
        if (NSIsEmptyRect(r)) continue;
        [[self colorOfBar:*it] set];
        if (it->kind != QuickDiff::Kind::Deleted) {
            NSRectFill(NSIntegralRectWithOptions(r, NSAlignAllEdgesNearest));
            continue;
        }
        // VS Code's: a small triangle pointing into the text.
        NSBezierPath *p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(NSMinX(r), NSMinY(r))];
        [p lineToPoint:NSMakePoint(NSMaxX(r), NSMidY(r))];
        [p lineToPoint:NSMakePoint(NSMinX(r), NSMaxY(r))];
        [p closePath];
        [p fill];
    }
}

// ---- drawing
// The lines the carets are on: the end of each selection, as VS Code
// highlights the cursor's line rather than the anchor's.
- (std::set<size_t>)caretLines:(size_t *)primary {
    CodeTextView *tv = _tv;
    std::set<size_t> lines;
    NSArray<NSValue *> *ranges = tv.multiCaretActive ? tv.multiCarets.caretRanges
                                                     : tv.selectedRanges;
    for (NSValue *v : ranges) lines.insert(_index.lineOf(NSMaxRange(v.rangeValue)));
    *primary = _index.lineOf(NSMaxRange(tv.selectedRange));
    return lines;
}

- (void)drawRect:(NSRect)dirty {
    CodeTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    if (!tv || !lm || _mode == MCLineNumbersOff || !_font) return;
    if (_index.length() != tv.textStorage.length) {
        [self reindex];
        [self updateThicknessLater];
    }

    const NSPoint origin = tv.textContainerOrigin;
    // The dirty rect in the text container's coordinates (y only matters).
    const NSRect inView = [self convertRect:dirty toView:tv];
    const NSRect inContainer = NSMakeRect(0, NSMinY(inView) - origin.y,
                                          tc.size.width, NSHeight(inView));
    const NSRange glyphs = [lm glyphRangeForBoundingRect:inContainer inTextContainer:tc];

    size_t primary = 0;
    const std::set<size_t> carets = [self caretLines:&primary];
    const CGFloat right = NSWidth(self.bounds) - kFoldLane;
    NSDictionary *plain = @{NSFontAttributeName: _font,
                            NSForegroundColorAttributeName: _numberColor ?: NSColor.grayColor};
    NSDictionary *active = @{NSFontAttributeName: _font,
                             NSForegroundColorAttributeName: _activeNumberColor ?: NSColor.whiteColor};

    // Errors and warnings on the lines in view, the worst first.
    const NSRange chars = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    std::map<size_t, NSInteger> marks;
    NSArray<MCDiagnosticMark *> *all = tv.diagnostics;
    if (tv.buildMarks.count) all = [all ?: @[] arrayByAddingObjectsFromArray:tv.buildMarks];
    for (MCDiagnosticMark *m in all) {
        if (m.severity > 2) continue;
        const NSUInteger at = m.range.location;
        if (at + 1 < chars.location || at > NSMaxRange(chars) + 1) continue;
        const size_t line = _index.lineOf(at);
        auto it = marks.find(line);
        if (it == marks.end() || m.severity < it->second) marks[line] = m.severity;
    }

    const CGFloat ascender = _font.ascender;
    MCFolds *folds = tv.folds;
    const BOOL hovering = _hovering;
    void (^drawLine)(size_t, CGFloat) = ^(size_t line, CGFloat baseline) {
        // baseline: in the text view's coordinates.
        const CGFloat y = [self convertPoint:NSMakePoint(0, baseline) fromView:tv].y;
        size_t shown = line + 1;
        if (self->_mode == MCLineNumbersRelative && line != primary)
            shown = line > primary ? line - primary : primary - line;
        NSString *label = [NSString stringWithFormat:@"%zu", shown];
        NSDictionary *attrs = carets.count(line) ? active : plain;
        const CGFloat w = [label sizeWithAttributes:attrs].width;
        [label drawAtPoint:NSMakePoint(right - w, y - ascender) withAttributes:attrs];
        if (folds && (folds.hasFolds || hovering)) {
            const BOOL closed = [folds lineStartsFold:line];
            if (closed || (hovering && [folds lineStartsRegion:line]))
                [self drawChevronClosed:closed midY:y - self->_font.xHeight / 2 color:plain[NSForegroundColorAttributeName]];
        }
        auto mark = marks.find(line);
        if (mark != marks.end()) {
            const CGFloat d = 6;
            const CGFloat mid = y - self->_font.xHeight / 2;
            [MCDiagnosticColor(mark->second) set];
            [[NSBezierPath bezierPathWithOvalInRect:
                NSMakeRect((kDotLane - d) / 2 + 1, mid - d / 2, d, d)] fill];
        }
    };

    LineIndex &index = _index;
    // An empty line's only glyph is its newline, which has no baseline of
    // its own: it gets the one a line of text in the editor's font has.
    __block CGFloat textBaseline = [lm defaultBaselineOffsetForFont:_font];
    NSString *text = tv.textStorage.string;
    if (!_bars.empty() && _changeLane) {
        const size_t firstLine = _index.lineOf(chars.location);
        const size_t lastLine = _index.lineOf(NSMaxRange(chars));
        [self drawChangeBarsFrom:firstLine ? firstLine - 1 : 0 to:lastLine + 1];
    }
    [lm enumerateLineFragmentsForGlyphRange:glyphs
                                 usingBlock:^(NSRect rect, NSRect used, NSTextContainer *c,
                                              NSRange range, BOOL *stop) {
        (void)used; (void)c; (void)stop;
        const NSUInteger ch = [lm characterIndexForGlyphAtIndex:range.location];
        if (!index.isLineStart(ch)) return;   // a wrapped line's continuation
        const unichar first = ch < text.length ? [text characterAtIndex:ch] : '\n';
        if (first != '\n' && first != '\r')
            textBaseline = [lm locationForGlyphAtIndex:range.location].y;
        drawLine(index.lineOf(ch), origin.y + NSMinY(rect) + textBaseline);
    }];
    // The empty line after a final newline (or of an empty text).
    const NSRect extra = lm.extraLineFragmentRect;
    // (Only its height counts: the gutter is beside the text, not over it.)
    if (NSHeight(extra) > 0 && NSMinY(extra) + origin.y < NSMaxY(inView) &&
        NSMaxY(extra) + origin.y > NSMinY(inView))
        drawLine(index.lineCount() - 1, origin.y + NSMinY(extra) + textBaseline);
}

// A chevron in the fold lane: pointing down for an open region, right for a
// closed fold.
- (void)drawChevronClosed:(BOOL)closed midY:(CGFloat)mid color:(NSColor *)color {
    const CGFloat cx = NSWidth(self.bounds) - kFoldLane / 2 - 1;
    const CGFloat r = MAX(2.5, round(_font.pointSize * 0.24 * 2) / 2);
    NSBezierPath *p = [NSBezierPath bezierPath];
    if (closed) {
        [p moveToPoint:NSMakePoint(cx - r / 2, mid - r)];
        [p lineToPoint:NSMakePoint(cx + r / 2, mid)];
        [p lineToPoint:NSMakePoint(cx - r / 2, mid + r)];
    } else {
        [p moveToPoint:NSMakePoint(cx - r, mid - r / 2)];
        [p lineToPoint:NSMakePoint(cx, mid + r / 2)];
        [p lineToPoint:NSMakePoint(cx + r, mid - r / 2)];
    }
    p.lineWidth = 1.25;
    p.lineCapStyle = NSLineCapStyleRound;
    p.lineJoinStyle = NSLineJoinStyleRound;
    [(closed ? (_activeNumberColor ?: color) : color) set];
    [p stroke];
}

// ---- the pointer, for the chevrons
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect
               owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)setHovering:(BOOL)hovering {
    if (_hovering == hovering) return;
    _hovering = hovering;
    self.needsDisplay = YES;
}

- (void)mouseEntered:(NSEvent *)event { (void)event; [self setHovering:YES]; }
- (void)mouseExited:(NSEvent *)event { (void)event; [self setHovering:NO]; }

// ---- selecting lines
- (NSUInteger)lineAtEvent:(NSEvent *)event {
    CodeTextView *tv = _tv;
    NSPoint p = [tv convertPoint:event.locationInWindow fromView:nil];
    p.x = tv.textContainerOrigin.x;
    const NSUInteger ch = [tv characterIndexForInsertionAtPoint:p];
    return _index.lineOf(ch);
}

- (void)selectLinesFrom:(NSUInteger)a to:(NSUInteger)b {
    CodeTextView *tv = _tv;
    const size_t first = MIN(a, b), last = MAX(a, b);
    const size_t start = _index.lineStart(first), end = _index.lineEnd(last);
    [tv setSelectedRange:NSMakeRange(start, end - start)];
}

- (void)mouseDown:(NSEvent *)event {
    CodeTextView *tv = _tv;
    if (!tv) return;
    [tv.window makeFirstResponder:tv];
    const NSUInteger line = [self lineAtEvent:event];
    // The change lane: a click on a bar shows its change.
    const NSPoint inGutter = [self convertPoint:event.locationInWindow fromView:nil];
    if (_changeLane && !_bars.empty() && self.onChangeBar && inGutter.x >= kDotLane - 1 &&
        inGutter.x < kDotLane + kChangeLane + 1) {
        const int at = QuickDiff::barAt(_bars, line);
        if (at >= 0) {
            _dragAnchorLine = NSNotFound;
            self.onChangeBar(at, [self rectOfChangeBar:at]);
            return;
        }
    }
    // The fold lane: a click on a region's line folds or unfolds it.
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (p.x >= NSWidth(self.bounds) - kFoldLane - 2 && tv.editable &&
        ([tv.folds lineStartsFold:line] || [tv.folds lineStartsRegion:line])) {
        _dragAnchorLine = NSNotFound;
        [tv.folds toggleLine:line];
        return;
    }
    if (event.modifierFlags & NSEventModifierFlagShift) {
        // From the line the selection starts on to this one.
        const NSRange sel = tv.selectedRange;
        _dragAnchorLine = _index.lineOf(sel.location);
        if (line < _dragAnchorLine)
            _dragAnchorLine = _index.lineOf(NSMaxRange(sel) > sel.location
                                                ? NSMaxRange(sel) - 1 : sel.location);
    } else {
        _dragAnchorLine = line;
    }
    [self selectLinesFrom:_dragAnchorLine to:line];
}

- (void)mouseDragged:(NSEvent *)event {
    CodeTextView *tv = _tv;
    if (!tv) return;
    if (_dragAnchorLine == NSNotFound) return;   // pressed on a chevron
    [tv autoscroll:event];
    [self selectLinesFrom:_dragAnchorLine to:[self lineAtEvent:event]];
}

- (void)mouseUp:(NSEvent *)event {
    (void)event;
}

- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:[NSCursor arrowCursor]];
}
@end
