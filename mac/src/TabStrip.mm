// TabStrip.mm — see TabStrip.h.
#import "TabStrip.h"
#import "Appearance.h"
#import "AppSettings.h"
#include "EditorTabs.h"
#include <cmath>
#include <vector>

NSFont *MCTabStripFont(void) {
    const Settings &s = [AppSettings shared].settings;
    return [NSFont systemFontOfSize:s.tabsFontSizeSet() ? (CGFloat)s.tabsFontSize() : 12];
}

CGFloat MCTabStripHeight(void) {
    NSFont *f = MCTabStripFont();
    // The line's height with 7 points above and below; 12 points fits the
    // strip's old 30.
    return MAX(30, ceil(f.ascender - f.descender + f.leading) + 14);
}

@implementation MCTabStripItem
@end

static NSColor *TabHex(unsigned int rgb, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}

// A tab is its title (and hint) between padding, then the close button.
static const CGFloat kPadLeft = 12, kPadRight = 6, kCloseSize = 16, kTitleGap = 6;
// Tabs keep their natural width within these (EditorTabs::layoutStrip):
// past the widest, a title is cut in the middle.
static const CGFloat kMinTab = 80, kMaxTab = 320;
// The hint's gap after the name, and the least of it worth showing cut.
static const CGFloat kHintGap = 5, kMinHint = 24;
// The scroll bar shown while the pointer is over a row that overflows, the
// fades at its cut-off ends, and a drag's scrolling near an end.
static const CGFloat kBarHeight = 3, kFadeWidth = 18, kAutoScrollEdge = 28, kAutoScrollStep = 14;
// An attention dot before the title, and the gap after it.
static const CGFloat kDotSize = 8, kDotGap = 6;

@implementation MCTabStrip {
    std::vector<double> _x, _w;    // each tab's left edge and width, before scrolling
    CGFloat _content;              // the whole row's width
    CGFloat _scroll;               // how far the row is scrolled left
    BOOL _reveal;                  // bring the tab on show into view at the next layout
    BOOL _inside;                  // the pointer is over the strip: the scroll bar shows
    NSTimer *_autoScroll;          // scrolls while a dragged tab is held near an end
    CGFloat _dragX;                // where the dragged tab's pointer is, in the strip
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
        // tabs.font-size: new widths at once; the owner sets the height.
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(relayout)
                                                     name:MCSettingsDidChangeNotification
                                                   object:nil];
    }
    return self;
}

- (void)dealloc {
    [_autoScroll invalidate];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSFont *)titleFont { return MCTabStripFont(); }

// A tab's title font: italic for a preview tab.
- (NSFont *)fontForItem:(MCTabStripItem *)item {
    NSFont *font = [self titleFont];
    return item.preview ? [[NSFontManager sharedFontManager] convertFont:font
                                                             toHaveTrait:NSFontItalicTrait]
                        : font;
}

// The hint's: a point smaller, as VS Code draws a tab's description.
- (NSFont *)hintFont {
    NSFont *f = [self titleFont];
    return [NSFont systemFontOfSize:MAX(9, f.pointSize - 1)];
}

- (void)setItems:(NSArray<MCTabStripItem *> *)items {
    // The tab on show is brought into view when it is another tab now: the
    // row changed length, or its title did (a preview replaced). A dot for
    // unsaved changes leaves the row where the user scrolled it.
    const NSInteger a = _activeIndex;
    if (items.count != _items.count) _reveal = YES;
    else if (a >= 0 && a < (NSInteger)items.count &&
             (![items[(NSUInteger)a].title ?: @"" isEqualToString:_items[(NSUInteger)a].title ?: @""] ||
              ![items[(NSUInteger)a].toolTip ?: @"" isEqualToString:_items[(NSUInteger)a].toolTip ?: @""]))
        _reveal = YES;
    _items = [items copy];
    if (_hover >= (NSInteger)_items.count) _hover = -1;
    [self relayout];
}

- (void)setActiveIndex:(NSInteger)i {
    if (i != _activeIndex) _reveal = YES;
    _activeIndex = i;
    [self relayout];
}

- (void)setFrameSize:(NSSize)size {
    if (size.width != self.frame.size.width) _reveal = YES;
    [super setFrameSize:size];
    [self relayout];
}

- (CGFloat)contentWidth { return _content; }
- (CGFloat)scrollOffset { return _scroll; }

// A string's drawn width in a font.
static double TabTextWidth(const std::string &s, NSFont *font) {
    NSString *t = [NSString stringWithUTF8String:s.c_str()] ?: @"";
    return [t sizeWithAttributes:@{NSFontAttributeName: font}].width;
}

static std::string TabUTF8(NSString *s) { return s.length ? std::string(s.UTF8String) : std::string(); }

// The room a tab's title and hint have at width `w`.
- (CGFloat)titleRoomForItem:(MCTabStripItem *)item width:(CGFloat)w {
    CGFloat room = w - kPadLeft - kTitleGap - kCloseSize - kPadRight;
    if (item.attention != MCTabAttentionNone) room -= kDotSize + kDotGap;
    return MAX(0, room);
}

// What tab i draws of its title and hint (EditorTabs::fitName).
- (EditorTabs::TabName)fittedNameForTab:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_items.count || i >= (NSInteger)_w.size()) return {};
    MCTabStripItem *item = _items[(NSUInteger)i];
    NSFont *font = [self fontForItem:item], *hintFont = [self hintFont];
    return EditorTabs::fitName({TabUTF8(item.title), TabUTF8(item.hint)},
                               [self titleRoomForItem:item width:_w[(size_t)i]], kHintGap, kMinHint,
                               [font](const std::string &s) { return TabTextWidth(s, font); },
                               [hintFont](const std::string &s) { return TabTextWidth(s, hintFont); });
}

- (NSString *)displayedTitleForTab:(NSInteger)i {
    return [NSString stringWithUTF8String:[self fittedNameForTab:i].name.c_str()] ?: @"";
}

- (NSString *)displayedHintForTab:(NSInteger)i {
    return [NSString stringWithUTF8String:[self fittedNameForTab:i].hint.c_str()] ?: @"";
}

// Each tab at its natural width, its whole title and hint (between the
// smallest and the widest tab), never squeezed to fit: a longer row
// scrolls, and the tab on show comes into view when it changes
// (EditorTabs::layoutStrip, revealScroll).
- (void)relayout {
    const NSUInteger n = _items.count;
    std::vector<double> natural(n, 0);
    NSFont *hintFont = [self hintFont];
    for (NSUInteger i = 0; i < n; i++) {
        MCTabStripItem *item = _items[i];
        NSFont *font = [self fontForItem:item];
        CGFloat text = ceil(TabTextWidth(TabUTF8(item.title), font));
        if (item.hint.length) text += kHintGap + ceil(TabTextWidth(TabUTF8(item.hint), hintFont));
        if (item.attention != MCTabAttentionNone) text += kDotSize + kDotGap;
        natural[i] = kPadLeft + text + kTitleGap + kCloseSize + kPadRight;
    }
    const EditorTabs::StripLayout l = EditorTabs::layoutStrip(natural, kMinTab, kMaxTab);
    _x = l.x;
    _w = l.w;
    _content = l.content;
    const CGFloat W = self.bounds.size.width;
    if (_reveal && _activeIndex >= 0 && _activeIndex < (NSInteger)n) {
        const double left = _x[(size_t)_activeIndex];
        _scroll = EditorTabs::revealScroll(_scroll, left, left + _w[(size_t)_activeIndex], _content, W);
    } else {
        _scroll = EditorTabs::clampScroll(_scroll, _content, W);
    }
    // Only once there is a width to reveal in (a strip is made at zero).
    if (W > 0) _reveal = NO;
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
+ (NSColor *)noteColor { return TabHex(0x4EA1F7, 1); }   // the note box's accent

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
    ps.lineBreakMode = NSLineBreakByClipping;   // titles come cut already (fitName)
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
                               : (self.inactiveText ?: TabHex(0x9D9D9D, 1));
        NSFont *font = [self fontForItem:item];
        NSDictionary *attrs = @{NSFontAttributeName: font,
                                NSForegroundColorAttributeName: text,
                                NSParagraphStyleAttributeName: ps};
        NSRect close = [self closeRectForTab:i];
        const CGFloat lineH = 16;
        CGFloat left = NSMinX(r) + kPadLeft;
        NSRect dot = [self attentionRectForTab:i];
        if (!NSIsEmptyRect(dot)) {
            [(item.attention == MCTabAttentionWaiting ? MCTabStrip.waitingColor
              : item.attention == MCTabAttentionNote  ? MCTabStrip.noteColor
                                                       : MCTabStrip.doneColor) set];
            [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
            left = NSMaxX(dot) + kDotGap;
        }
        // The title whole, or cut in the middle past the widest tab, and the
        // hint after it, dimmed and a point smaller, on the same baseline.
        const EditorTabs::TabName shown = [self fittedNameForTab:i];
        NSString *name = [NSString stringWithUTF8String:shown.name.c_str()] ?: @"";
        NSRect title = NSMakeRect(left, floor(NSMidY(r) - lineH / 2),
                                  MAX(0, NSMinX(close) - kTitleGap - left), lineH);
        [name drawInRect:title withAttributes:attrs];
        if (!shown.hint.empty()) {
            NSFont *hf = [self hintFont];
            const CGFloat nameW = ceil([name sizeWithAttributes:attrs].width);
            NSRect hr = title;
            hr.origin.x += nameW + kHintGap;
            hr.size.width = MAX(0, NSMaxX(title) - NSMinX(hr));
            hr.origin.y += round(font.ascender - hf.ascender);
            [([NSString stringWithUTF8String:shown.hint.c_str()] ?: @"")
                drawInRect:hr
                withAttributes:@{NSFontAttributeName: hf,
                                 NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.55],
                                 NSParagraphStyleAttributeName: ps}];
        }

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
    // A row that overflows: its cut-off ends fade into the strip, and while
    // the pointer is over it a thin bar says where the view is.
    const EditorTabs::Clipped cut = EditorTabs::clipped(_scroll, _content, NSWidth(b));
    NSColor *strip = self.stripColor ?: TabHex(0x252526, 1);
    if (cut.left || cut.right) {
        NSGradient *fade = [[NSGradient alloc] initWithStartingColor:[strip colorWithAlphaComponent:0]
                                                         endingColor:strip];
        const CGFloat fh = NSHeight(b) - 1;   // above the strip's bottom line
        if (cut.left) [fade drawInRect:NSMakeRect(0, 0, kFadeWidth, fh) angle:180];
        if (cut.right) [fade drawInRect:NSMakeRect(NSMaxX(b) - kFadeWidth, 0, kFadeWidth, fh) angle:0];
    }
    if (_inside && _content > NSWidth(b) && _content > 0) {
        const CGFloat W = NSWidth(b);
        NSRect bar = NSMakeRect(floor(_scroll / _content * W), NSMaxY(b) - kBarHeight - 1,
                                MAX(24, floor(W * W / _content)), kBarHeight);
        [[(self.inactiveText ?: TabHex(0x9D9D9D, 1)) colorWithAlphaComponent:0.45] set];
        [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:1.5 yRadius:1.5] fill];
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
    const double x = _dropTarget < _dragFrom ? _x[t] : _x[t] + _w[t];
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

- (void)mouseMoved:(NSEvent *)event {
    if (!_inside) { _inside = YES; self.needsDisplay = YES; }
    [self setHoverFromEvent:event];
}
- (void)mouseEntered:(NSEvent *)event {
    _inside = YES;
    self.needsDisplay = YES;
    [self setHoverFromEvent:event];
}
- (void)mouseExited:(NSEvent *)event {
    _inside = NO;
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
    _dragX = p.x;
    [self updateDropTarget];
    // Held near an end of a row that scrolls: the row scrolls that way.
    if (!_autoScroll &&
        EditorTabs::dragAutoScroll(p.x, NSWidth(self.bounds), kAutoScrollEdge, kAutoScrollStep) != 0) {
        __weak MCTabStrip *weakSelf = self;
        _autoScroll = [NSTimer timerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *) {
            [weakSelf autoScrollTick];
        }];
        [[NSRunLoop currentRunLoop] addTimer:_autoScroll forMode:NSRunLoopCommonModes];
    }
}

- (void)updateDropTarget {
    _dropTarget = EditorTabs::dropIndex(_w, (int)_dragFrom, _dragX + _scroll);
    self.needsDisplay = YES;
}

- (void)autoScrollTick {
    const CGFloat W = NSWidth(self.bounds);
    const double step = _dragging ? EditorTabs::dragAutoScroll(_dragX, W, kAutoScrollEdge, kAutoScrollStep) : 0;
    const double next = EditorTabs::clampScroll(_scroll + step, _content, W);
    if (step == 0 || next == _scroll) {
        [_autoScroll invalidate];
        _autoScroll = nil;
        return;
    }
    _scroll = next;
    [self updateDropTarget];
    [self updateToolTips];
}

- (void)endDrag {
    if (_dragging) self.needsDisplay = YES;
    [_autoScroll invalidate];
    _autoScroll = nil;
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
        if (NSPointInRect(p, NSInsetRect(self.bounds, 0, -MCTabStripHeight())) && to >= 0 &&
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

// A trackpad or a wheel scrolls a row that overflows, the vertical wheel
// too (EditorTabs::wheelScroll).
- (void)scrollWheel:(NSEvent *)event {
    const CGFloat W = NSWidth(self.bounds);
    if (_content <= W) { [super scrollWheel:event]; return; }
    const double next = EditorTabs::wheelScroll(_scroll, event.scrollingDeltaX, event.scrollingDeltaY,
                                                event.hasPreciseScrollingDeltas, 12, _content, W);
    if (next == _scroll) return;
    _scroll = next;
    _inside = YES;
    [self updateToolTips];
    [self setHoverFromEvent:event];
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
