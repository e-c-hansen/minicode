// TabStrip.mm — see TabStrip.h.
#import "TabStrip.h"
#import "Appearance.h"
#include "EditorTabs.h"
#include <cmath>
#include <vector>

const CGFloat kMCTabStripHeight = 30;

@implementation MCTabStripItem
@end

static NSColor *TabHex(unsigned int rgb, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

// A tab is its title between padding, then the close button.
static const CGFloat kPadLeft = 12, kPadRight = 6, kCloseSize = 16, kTitleGap = 6;
static const CGFloat kMinTab = 80, kMaxTab = 240;
// An attention dot before the title, and the gap after it.
static const CGFloat kDotSize = 8, kDotGap = 6;

@implementation MCTabStrip {
    std::vector<CGFloat> _x, _w;   // each tab's left edge and width, before scrolling
    CGFloat _scroll;               // how far the row is scrolled left
    NSInteger _hover;              // tab under the pointer, or -1
    BOOL _hoverClose;              // the pointer is on that tab's close button
    NSInteger _pressedClose;       // close button the mouse went down on, or -1
    NSInteger _dragFrom;           // tab the mouse went down on, while it is held, or -1
    CGFloat _dragStartX;           // where it went down, in the strip
    BOOL _dragging;                // moved past 4 points since
}

// A press that moves this far along the strip is a drag, not a click.
static const CGFloat kDragThreshold = 4;

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _activeIndex = -1;
        _hover = -1;
        _pressedClose = -1;
        _dragFrom = -1;
        _dropTarget = -1;
        self.wantsLayer = YES;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSFont *)titleFont { return [NSFont systemFontOfSize:12]; }

- (void)setItems:(NSArray<MCTabStripItem *> *)items {
    _items = [items copy];
    if (_hover >= (NSInteger)_items.count) _hover = -1;
    [self relayout];
}

- (void)setActiveIndex:(NSInteger)i {
    _activeIndex = i;
    [self relayout];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self relayout];
}

// Widths from the titles; when they do not all fit they share the width
// evenly, down to a smallest tab, and past that the row scrolls so the
// active tab is always in view.
- (void)relayout {
    const NSUInteger n = _items.count;
    _x.assign(n, 0);
    _w.assign(n, 0);
    NSDictionary *attrs = @{NSFontAttributeName: [self titleFont]};
    CGFloat total = 0;
    for (NSUInteger i = 0; i < n; i++) {
        CGFloat text = ceil([_items[i].title ?: @"" sizeWithAttributes:attrs].width);
        if (_items[i].attention != MCTabAttentionNone) text += kDotSize + kDotGap;
        _w[i] = MIN(kMaxTab, MAX(kMinTab, kPadLeft + text + kTitleGap + kCloseSize + kPadRight));
        total += _w[i];
    }
    const CGFloat W = self.bounds.size.width;
    if (n > 0 && total > W) {
        const CGFloat even = MAX(kMinTab, floor(W / n));
        total = 0;
        for (NSUInteger i = 0; i < n; i++) { _w[i] = MIN(_w[i], even); total += _w[i]; }
    }
    CGFloat x = 0;
    for (NSUInteger i = 0; i < n; i++) { _x[i] = x; x += _w[i]; }
    // Keep the scroll in range, and the active tab in view.
    _scroll = MAX(0, MIN(_scroll, total - W));
    if (_activeIndex >= 0 && _activeIndex < (NSInteger)n) {
        const CGFloat left = _x[(size_t)_activeIndex], right = left + _w[(size_t)_activeIndex];
        if (left < _scroll) _scroll = left;
        if (right > _scroll + W) _scroll = right - W;
    }
    _scroll = MAX(0, _scroll);
    [self updateToolTips];
    self.needsDisplay = YES;
}

- (NSRect)rectForTab:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_x.size()) return NSZeroRect;
    NSRect r = NSMakeRect(_x[(size_t)i] - _scroll, 0, _w[(size_t)i], self.bounds.size.height);
    return NSIntersectsRect(r, self.bounds) ? r : NSZeroRect;
}

- (NSRect)closeRectForTab:(NSInteger)i {
    NSRect t = [self rectForTab:i];
    if (NSIsEmptyRect(t)) return NSZeroRect;
    return NSMakeRect(NSMaxX(t) - kPadRight - kCloseSize,
                      floor(NSMidY(t) - kCloseSize / 2), kCloseSize, kCloseSize);
}

- (NSRect)attentionRectForTab:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_items.count || _items[(NSUInteger)i].attention == MCTabAttentionNone)
        return NSZeroRect;
    NSRect t = [self rectForTab:i];
    if (NSIsEmptyRect(t)) return NSZeroRect;
    return NSMakeRect(NSMinX(t) + kPadLeft, floor(NSMidY(t) - kDotSize / 2), kDotSize, kDotSize);
}

// Orange like the developer bar's amber, green like the git panel's added
// lines: both read on the dark and the light themes.
+ (NSColor *)waitingColor { return TabHex(0xF0A030, 1); }
+ (NSColor *)doneColor { return TabHex(0x4CC38A, 1); }

- (NSInteger)tabAtPoint:(NSPoint)p {
    for (NSInteger i = 0; i < (NSInteger)_x.size(); i++)
        if (NSPointInRect(p, [self rectForTab:i])) return i;
    return -1;
}

// ------------------------------------------------------------------ drawing
- (void)drawRect:(NSRect)dirty {
    const NSRect b = self.bounds;
    // Each area is filled once, so translucent colors are not doubled.
    [(self.stripColor ?: TabHex(0x252526, 1)) set];
    NSRectFill(b);
    NSColor *divider = TabHex(0x333333, 1);
    NSColor *accent = TabHex(0x4EA1F7, 1);
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingMiddle;
    for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
        NSRect r = [self rectForTab:i];
        if (NSIsEmptyRect(r)) continue;
        MCTabStripItem *item = _items[(NSUInteger)i];
        const BOOL active = i == _activeIndex;
        if (active) {
            [(self.activeColor ?: TabHex(0x1E1E1E, 1)) set];
            NSRectFill(r);
            [accent set];
            NSRectFill(NSMakeRect(NSMinX(r), 0, NSWidth(r), 2));
        }
        // A line between tabs, and under the inactive ones.
        [divider set];
        NSRectFill(NSMakeRect(NSMaxX(r) - 1, 0, 1, NSHeight(r)));
        if (!active) NSRectFill(NSMakeRect(NSMinX(r), NSMaxY(r) - 1, NSWidth(r), 1));

        NSColor *text = active ? (self.activeText ?: TabHex(0xE0E0E0, 1))
                               : (self.inactiveText ?: TabHex(0x9CA3AF, 1));
        NSFont *font = [self titleFont];
        if (item.preview)
            font = [[NSFontManager sharedFontManager] convertFont:font
                                                      toHaveTrait:NSFontItalicTrait];
        NSDictionary *attrs = @{NSFontAttributeName: font,
                                NSForegroundColorAttributeName: text,
                                NSParagraphStyleAttributeName: ps};
        NSRect close = [self closeRectForTab:i];
        const CGFloat lineH = 16;
        CGFloat left = NSMinX(r) + kPadLeft;
        NSRect dot = [self attentionRectForTab:i];
        if (!NSIsEmptyRect(dot)) {
            [(item.attention == MCTabAttentionWaiting ? MCTabStrip.waitingColor
                                                       : MCTabStrip.doneColor) set];
            [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
            left = NSMaxX(dot) + kDotGap;
        }
        NSRect title = NSMakeRect(left, floor(NSMidY(r) - lineH / 2),
                                  MAX(0, NSMinX(close) - kTitleGap - left), lineH);
        [item.title ?: @"" drawInRect:title withAttributes:attrs];

        // The close button: a dot while the file has unsaved changes, which
        // turns into the cross while the pointer is on it.
        const BOOL onClose = i == _hover && _hoverClose;
        if (onClose) {
            [TabHex(0xFFFFFF, 0.12) set];
            [[NSBezierPath bezierPathWithRoundedRect:close xRadius:3 yRadius:3] fill];
        }
        if (item.dirty && !onClose) {
            [text set];
            [[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(close, 4.5, 4.5)] fill];
        } else {
            NSBezierPath *x = [NSBezierPath bezierPath];
            NSRect c = NSInsetRect(close, 4.5, 4.5);
            [x moveToPoint:NSMakePoint(NSMinX(c), NSMinY(c))];
            [x lineToPoint:NSMakePoint(NSMaxX(c), NSMaxY(c))];
            [x moveToPoint:NSMakePoint(NSMaxX(c), NSMinY(c))];
            [x lineToPoint:NSMakePoint(NSMinX(c), NSMaxY(c))];
            x.lineWidth = 1.2;
            x.lineCapStyle = NSLineCapStyleRound;
            [(onClose || active || i == _hover ? text : [text colorWithAlphaComponent:0.6]) set];
            [x stroke];
        }
    }
    // Under the empty part of the row too, so the strip ends in a line.
    CGFloat end = _x.empty() ? 0 : _x.back() + _w.back() - _scroll;
    if (end < NSMaxX(b)) {
        [divider set];
        NSRectFill(NSMakeRect(MAX(0, end), NSMaxY(b) - 1, NSMaxX(b) - MAX(0, end), 1));
    }
    // A tab being dragged: dimmed where it is, and a line where it will go.
    if (_dragging && _dragFrom >= 0 && _dragFrom < (NSInteger)_x.size()) {
        NSRect from = [self rectForTab:_dragFrom];
        if (!NSIsEmptyRect(from)) {
            [TabHex(0x000000, 0.25) set];
            NSRectFillUsingOperation(from, NSCompositingOperationSourceOver);
        }
        if (_dropTarget >= 0 && _dropTarget != _dragFrom) {
            [accent set];
            NSRectFill(NSMakeRect(floor([self dropLineX] - 1), 0, 2, NSHeight(b)));
        }
    }
}

// The boundary the dragged tab will land on: before the tab it takes the
// place of when moving left, after it when moving right.
- (CGFloat)dropLineX {
    const size_t t = (size_t)_dropTarget;
    if (t >= _x.size()) return 0;
    CGFloat x = _dropTarget < _dragFrom ? _x[t] : _x[t] + _w[t];
    return x - _scroll;
}

// ------------------------------------------------------------------- mouse
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:nil]];
}

- (void)setHoverFromEvent:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger i = [self tabAtPoint:p];
    BOOL onClose = i >= 0 && NSPointInRect(p, [self closeRectForTab:i]);
    if (i == _hover && onClose == _hoverClose) return;
    _hover = i;
    _hoverClose = onClose;
    self.needsDisplay = YES;
}

- (void)mouseMoved:(NSEvent *)event { [self setHoverFromEvent:event]; }
- (void)mouseEntered:(NSEvent *)event { [self setHoverFromEvent:event]; }
- (void)mouseExited:(NSEvent *)event {
    _hover = -1;
    _hoverClose = NO;
    self.needsDisplay = YES;
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSInteger i = [self tabAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    NSMenu *m = i >= 0 && self.menuForTab ? self.menuForTab(i) : nil;
    if (m) {
        if (self.surface) MCAddAdjustSettingsItem(m, self.surface, self);
        return m;
    }
    return self.surface ? MCAdjustSettingsMenu(self.surface, self) : nil;
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger i = [self tabAtPoint:p];
    _pressedClose = -1;
    [self endDrag];
    if (i < 0) return;
    if (NSPointInRect(p, [self closeRectForTab:i])) {
        _pressedClose = i;   // closes on release over the same button
        return;
    }
    if (self.onMove) {
        _dragFrom = i;
        _dragStartX = p.x;
    }
    if (self.onSelect) self.onSelect(i);
    if (event.clickCount == 2 && self.onDoubleClick) self.onDoubleClick(i);
}

// Tabs drag along the strip: past 4 points the press is a drag, and the
// tab's new place follows the pointer by the other tabs' middles.
- (void)mouseDragged:(NSEvent *)event {
    if (_dragFrom < 0 || _dragFrom >= (NSInteger)_w.size()) return;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (!_dragging && fabs(p.x - _dragStartX) <= kDragThreshold) return;
    _dragging = YES;
    std::vector<double> widths(_w.begin(), _w.end());
    _dropTarget = EditorTabs::dropIndex(widths, (int)_dragFrom, p.x + _scroll);
    self.needsDisplay = YES;
}

- (void)endDrag {
    if (_dragging) self.needsDisplay = YES;
    _dragFrom = -1;
    _dragging = NO;
    _dropTarget = -1;
}

- (void)mouseUp:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (_dragging) {
        const NSInteger from = _dragFrom, to = _dropTarget;
        [self endDrag];
        // Let go over the strip (a little above or below counts): moved;
        // anywhere else: left where it was.
        if (NSPointInRect(p, NSInsetRect(self.bounds, 0, -kMCTabStripHeight)) && to >= 0 &&
            to != from && self.onMove)
            self.onMove(from, to);
        return;
    }
    [self endDrag];
    NSInteger pressed = _pressedClose;
    _pressedClose = -1;
    if (pressed < 0) return;
    if (NSPointInRect(p, [self closeRectForTab:pressed]) && self.onClose)
        self.onClose(pressed);
}

// A middle click closes the tab under it, as in browsers.
- (void)otherMouseDown:(NSEvent *)event {
    if (event.buttonNumber != 2) { [super otherMouseDown:event]; return; }
    NSInteger i = [self tabAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (i >= 0 && self.onClose) self.onClose(i);
}

- (void)scrollWheel:(NSEvent *)event {
    CGFloat d = fabs(event.scrollingDeltaX) > fabs(event.scrollingDeltaY)
                    ? event.scrollingDeltaX : event.scrollingDeltaY;
    if (!event.hasPreciseScrollingDeltas) d *= 10;
    const CGFloat total = _x.empty() ? 0 : _x.back() + _w.back();
    _scroll = MAX(0, MIN(_scroll - d, total - self.bounds.size.width));
    [self updateToolTips];
    self.needsDisplay = YES;
}

// ---------------------------------------------------------------- tooltips
- (void)updateToolTips {
    [self removeAllToolTips];
    for (NSInteger i = 0; i < (NSInteger)_items.count; i++) {
        NSRect r = [self rectForTab:i];
        if (!NSIsEmptyRect(r))
            [self addToolTipRect:r owner:self userData:(void *)(intptr_t)i];
    }
}

- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag
             point:(NSPoint)point userData:(void *)data {
    NSInteger i = (NSInteger)(intptr_t)data;
    return i >= 0 && i < (NSInteger)_items.count ? _items[(NSUInteger)i].toolTip : nil;
}
@end
