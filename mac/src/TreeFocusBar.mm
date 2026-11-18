// TreeFocusBar.mm — see TreeFocusBar.h.
#import "TreeFocusBar.h"

#include <algorithm>
#include <string>
#include <vector>

#include "FileDetails.h"

namespace {
const CGFloat kPad = 8;          // before the first part
const CGFloat kCloseWidth = 22;  // the ×'s square, at the right
const CGFloat kSepPad = 4;       // either side of a ›
// A folder's label on the label row: padding, the label, a gap, its ×,
// padding; and the space between two labels.
const CGFloat kChipPadLeft = 6;
const CGFloat kChipGap = 2;
const CGFloat kChipCloseWidth = 12;
const CGFloat kChipPadRight = 3;
const CGFloat kChipSpace = 5;
const NSInteger kRemoveHover = 1000;   // _hover for a label's ×: this plus its index
}  // namespace

@implementation MCTreeFocusBar {
    NSArray<NSString *> *_labels;
    NSArray<NSString *> *_foci;
    std::vector<NSRect> _rects;   // by part; NSZeroRect when left out
    NSRect _ellipsis;
    // The label row (several folders): each label's background and its ×.
    BOOL _several;
    std::vector<NSRect> _chips;
    std::vector<NSRect> _removes;
    NSInteger _hover;             // the part under the mouse, -2 the ×, -1 none
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _labels = @[];
        _foci = @[];
        _hover = -1;
        _textColor = NSColor.labelColor;
        self.accessibilityRole = NSAccessibilityGroupRole;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }

- (NSArray<NSString *> *)partLabels { return _labels; }

- (BOOL)showsFolders { return _several; }

- (void)setTextColor:(NSColor *)color {
    _textColor = color ?: NSColor.labelColor;
    self.needsDisplay = YES;
}

- (void)setRootName:(NSString *)name focus:(NSString *)focus {
    _several = NO;
    NSMutableArray<NSString *> *labels = [NSMutableArray array], *foci = [NSMutableArray array];
    for (const TreeFocus::Part &p : TreeFocus::breadcrumb(name.UTF8String ?: "", focus.UTF8String ?: "")) {
        [labels addObject:@(p.label.c_str()) ?: @""];
        [foci addObject:@(p.focus.c_str()) ?: @""];
    }
    _labels = labels;
    _foci = foci;
    self.toolTip = [NSString stringWithFormat:@"The tree shows %@ only\n"
                    @"Click a folder above to show it instead; × shows the whole folder",
                    [labels componentsJoinedByString:@"/"]];
    self.accessibilityLabel = [@"Focused folder: " stringByAppendingString:
                               [labels componentsJoinedByString:@" › "]];
    [self layoutParts];
    self.needsDisplay = YES;
    [self.window invalidateCursorRectsForView:self];
}

- (void)setFolders:(NSArray<NSString *> *)folders {
    _several = YES;
    _foci = [folders copy] ?: @[];
    _labels = _foci;
    self.toolTip = [NSString stringWithFormat:@"The tree shows %@ only\n"
                    @"Click a folder above to show it; its × takes it out of the focus; "
                    @"the last × shows the whole folder",
                    [_foci componentsJoinedByString:@", "]];
    self.accessibilityLabel = [@"Focused folders: " stringByAppendingString:
                               [_foci componentsJoinedByString:@", "]];
    _hover = -1;
    [self layoutParts];
    self.needsDisplay = YES;
    [self.window invalidateCursorRectsForView:self];
}

- (NSFont *)fontForPart:(NSInteger)i {
    if (_several) return [NSFont systemFontOfSize:11.5];
    return i == (NSInteger)_labels.count - 1
               ? [NSFont systemFontOfSize:11.5 weight:NSFontWeightSemibold]
               : [NSFont systemFontOfSize:11.5];
}

- (CGFloat)widthOf:(NSString *)s font:(NSFont *)font {
    return ceil([s sizeWithAttributes:@{NSFontAttributeName: font}].width);
}

- (NSRect)closeRect {
    const NSRect b = self.bounds;
    return NSMakeRect(NSMaxX(b) - kCloseWidth - 2, NSMinY(b), kCloseWidth, b.size.height);
}

// The label row: each folder's full path when they all fit, else the
// shortest labels that tell them apart, cut to share the room when even
// those do not (the core's shortLabels and shared).
- (void)layoutFolders {
    const NSUInteger n = _foci.count;
    _chips.assign(n, NSZeroRect);
    _removes.assign(n, NSZeroRect);
    _rects.assign(n, NSZeroRect);
    _labels = _foci;
    if (!n) return;
    NSFont *font = [self fontForPart:0];
    const CGFloat fixed = kChipPadLeft + kChipGap + kChipCloseWidth + kChipPadRight;
    const CGFloat right = NSMinX([self closeRect]) - 2;
    const CGFloat room = MAX(0, right - kPad - n * fixed - (n - 1) * kChipSpace);
    auto widthsOf = [&](NSArray<NSString *> *labels) {
        std::vector<double> w;
        for (NSString *l in labels) w.push_back([self widthOf:l font:font]);
        return w;
    };
    std::vector<double> widths = widthsOf(_labels);
    double all = 0;
    for (double w : widths) all += w;
    if (all > room) {
        std::vector<std::string> paths;
        for (NSString *f in _foci) paths.push_back(f.UTF8String ?: "");
        NSMutableArray<NSString *> *shortLabels = [NSMutableArray array];
        for (const std::string &l : TreeFocus::shortLabels(paths)) [shortLabels addObject:@(l.c_str()) ?: @""];
        _labels = shortLabels;
        widths = TreeFocus::shared(widthsOf(_labels), room);
    }
    const NSRect b = self.bounds;
    CGFloat x = kPad;
    for (NSUInteger i = 0; i < n; i++) {
        const CGFloat w = (CGFloat)widths[i];
        _chips[i] = NSMakeRect(x, NSMinY(b) + 4, fixed + w, MAX(0, b.size.height - 8));
        _rects[i] = NSMakeRect(x + kChipPadLeft, NSMinY(b), w, b.size.height);
        _removes[i] = NSMakeRect(NSMaxX(_rects[i]) + kChipGap, NSMinY(b), kChipCloseWidth, b.size.height);
        x += fixed + w + kChipSpace;
    }
}

- (NSRect)removeRectOfPart:(NSInteger)index {
    return _several && index >= 0 && (size_t)index < _removes.size() ? _removes[(size_t)index] : NSZeroRect;
}

// Where each part goes: as many as fit, by the core's rule.
- (void)layoutParts {
    if (_several) {
        [self layoutFolders];
        return;
    }
    _chips.clear();
    _removes.clear();
    _rects.assign(_labels.count, NSZeroRect);
    _ellipsis = NSZeroRect;
    if (!_labels.count) return;
    NSFont *plain = [NSFont systemFontOfSize:11.5];
    const CGFloat sep = [self widthOf:@"›" font:plain] + 2 * kSepPad;
    const CGFloat ell = [self widthOf:@"…" font:plain];
    std::vector<double> widths;
    for (NSUInteger i = 0; i < _labels.count; i++)
        widths.push_back([self widthOf:_labels[i] font:[self fontForPart:(NSInteger)i]]);
    const CGFloat right = NSMinX([self closeRect]) - 2;
    const CGFloat available = MAX(0, right - kPad);
    const NSRect b = self.bounds;
    CGFloat x = kPad;
    bool first = true;
    for (int i : TreeFocus::fit(widths, sep, ell, available)) {
        if (!first) x += sep;
        first = false;
        const CGFloat w = i < 0 ? ell : (CGFloat)widths[(size_t)i];
        const NSRect r = NSMakeRect(x, NSMinY(b), MAX(0, MIN(w, right - x)), b.size.height);
        if (i < 0) _ellipsis = r;
        else _rects[(size_t)i] = r;
        x += w;
    }
}

- (NSRect)rectOfPart:(NSInteger)index {
    return index >= 0 && (size_t)index < _rects.size() ? _rects[(size_t)index] : NSZeroRect;
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self layoutParts];
    [self.window invalidateCursorRectsForView:self];
}

// The label row: each folder's label on a faint ground, its × after it.
- (void)drawFolders {
    NSColor *text = _textColor;
    NSColor *dim = [text colorWithAlphaComponent:0.6];
    NSFont *font = [self fontForPart:0];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingHead;   // the folder's own name stays
    auto drawText = [&](NSString *str, NSRect r, NSFont *f, NSColor *color, BOOL underline) {
        NSMutableDictionary *a = [@{NSFontAttributeName: f, NSForegroundColorAttributeName: color,
                                    NSParagraphStyleAttributeName: ps} mutableCopy];
        if (underline) a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
        const CGFloat h = [str sizeWithAttributes:a].height;
        [str drawInRect:NSMakeRect(NSMinX(r), NSMidY(r) - h / 2, r.size.width + 1, h) withAttributes:a];
    };
    for (size_t i = 0; i < _chips.size() && i < _labels.count; i++) {
        const BOOL hot = _hover == (NSInteger)i || _hover == kRemoveHover + (NSInteger)i;
        [[text colorWithAlphaComponent:hot ? 0.14 : 0.08] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:_chips[i] xRadius:4 yRadius:4] fill];
        drawText(_labels[i], _rects[i], font, text, _hover == (NSInteger)i);
        drawText(@"×", _removes[i], [NSFont systemFontOfSize:12],
                 _hover == kRemoveHover + (NSInteger)i ? text : dim, NO);
    }
    drawText(@"×", NSInsetRect([self closeRect], 6, 0), [NSFont systemFontOfSize:14],
             _hover == -2 ? text : dim, NO);
    [[text colorWithAlphaComponent:0.12] setFill];
    NSRectFillUsingOperation(NSMakeRect(0, NSMaxY(self.bounds) - 1, self.bounds.size.width, 1),
                             NSCompositingOperationSourceOver);
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (_several) {
        [self drawFolders];
        return;
    }
    NSColor *text = _textColor;
    NSColor *dim = [text colorWithAlphaComponent:0.6];
    NSFont *plain = [NSFont systemFontOfSize:11.5];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    auto drawText = [&](NSString *str, NSRect r, NSFont *font, NSColor *color, BOOL underline) {
        NSMutableDictionary *a = [@{NSFontAttributeName: font, NSForegroundColorAttributeName: color,
                                    NSParagraphStyleAttributeName: ps} mutableCopy];
        if (underline) a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
        const CGFloat h = [str sizeWithAttributes:a].height;
        [str drawInRect:NSMakeRect(NSMinX(r), NSMidY(r) - h / 2, r.size.width + 1, h) withAttributes:a];
    };
    const NSInteger last = (NSInteger)_labels.count - 1;
    const CGFloat sepWidth = [self widthOf:@"›" font:plain];
    NSRect prev = NSZeroRect;
    // Parts and the ellipsis in order of x.
    std::vector<std::pair<NSRect, NSInteger>> order;
    for (size_t i = 0; i < _rects.size(); i++)
        if (!NSIsEmptyRect(_rects[i])) order.push_back({_rects[i], (NSInteger)i});
    if (!NSIsEmptyRect(_ellipsis)) order.push_back({_ellipsis, -1});
    std::sort(order.begin(), order.end(),
              [](const auto &a, const auto &b) { return NSMinX(a.first) < NSMinX(b.first); });
    for (const auto &[r, i] : order) {
        if (!NSIsEmptyRect(prev))
            drawText(@"›", NSMakeRect((NSMaxX(prev) + NSMinX(r) - sepWidth) / 2, NSMinY(r), sepWidth,
                                      r.size.height), plain, dim, NO);
        if (i < 0) drawText(@"…", r, plain, dim, NO);
        else drawText(_labels[(NSUInteger)i], r, [self fontForPart:i],
                      i == last || i == _hover ? text : dim, i == _hover && i != last);
        prev = r;
    }
    // The ×, and a hairline under the bar.
    drawText(@"×", NSInsetRect([self closeRect], 6, 0), [NSFont systemFontOfSize:14],
             _hover == -2 ? text : dim, NO);
    [[text colorWithAlphaComponent:0.12] setFill];
    NSRectFillUsingOperation(NSMakeRect(0, NSMaxY(self.bounds) - 1, self.bounds.size.width, 1),
                             NSCompositingOperationSourceOver);
}

// The part at a point: its index, -2 for the ×, -1 for none (the last part,
// already the tree's root, does nothing).
- (NSInteger)partAt:(NSPoint)p {
    if (NSPointInRect(p, [self closeRect])) return -2;
    if (_several) {
        // A label's × (the label row): kRemoveHover plus its index.
        for (size_t i = 0; i < _removes.size(); i++)
            if (NSPointInRect(p, NSInsetRect(_removes[i], -1, 0))) return kRemoveHover + (NSInteger)i;
        for (size_t i = 0; i < _rects.size(); i++)
            if (NSPointInRect(p, NSInsetRect(_rects[i], -kChipPadLeft, 0))) return (NSInteger)i;
        return -1;
    }
    for (size_t i = 0; i + 1 < _rects.size(); i++)
        if (NSPointInRect(p, NSInsetRect(_rects[i], -kSepPad, 0))) return (NSInteger)i;
    return -1;
}

- (void)mouseDown:(NSEvent *)event {
    const NSInteger part = [self partAt:[self convertPoint:event.locationInWindow fromView:nil]];
    if (part == -1) return;
    if (_several && part >= kRemoveHover) {
        if (self.onRemove) self.onRemove(_foci[(NSUInteger)(part - kRemoveHover)]);
        return;
    }
    if (_several && part >= 0) {
        if (self.onShowFolder) self.onShowFolder(_foci[(NSUInteger)part]);
        return;
    }
    if (!self.onPick) return;
    self.onPick(part == -2 ? @"" : _foci[(NSUInteger)part]);
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    NSMenu *m = [[NSMenu alloc] init];
    NSMenuItem *it = [m addItemWithTitle:@"Show Whole Folder in Tree"
                                  action:@selector(showWholeFolder:) keyEquivalent:@""];
    it.target = self;
    return m;
}

- (void)showWholeFolder:(id)sender {
    (void)sender;
    if (self.onPick) self.onPick(@"");
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)mouseMoved:(NSEvent *)event {
    const NSInteger part = [self partAt:[self convertPoint:event.locationInWindow fromView:nil]];
    if (part == _hover) return;
    _hover = part;
    self.needsDisplay = YES;
}

- (void)mouseExited:(NSEvent *)event {
    (void)event;
    _hover = -1;
    self.needsDisplay = YES;
}

- (void)resetCursorRects {
    if (_several) {
        for (size_t i = 0; i < _rects.size(); i++) {
            if (!NSIsEmptyRect(_rects[i])) [self addCursorRect:_rects[i] cursor:NSCursor.pointingHandCursor];
            if (i < _removes.size()) [self addCursorRect:_removes[i] cursor:NSCursor.pointingHandCursor];
        }
        [self addCursorRect:[self closeRect] cursor:NSCursor.pointingHandCursor];
        return;
    }
    for (size_t i = 0; i + 1 < _rects.size(); i++)
        if (!NSIsEmptyRect(_rects[i])) [self addCursorRect:_rects[i] cursor:NSCursor.pointingHandCursor];
    [self addCursorRect:[self closeRect] cursor:NSCursor.pointingHandCursor];
}
@end
