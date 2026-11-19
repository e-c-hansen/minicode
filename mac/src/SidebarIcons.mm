// SidebarIcons.mm — see SidebarIcons.h.
#import "SidebarIcons.h"

const CGFloat kMCSidebarIconBarHeight = 28;

namespace {
const CGFloat kButtonWidth = 30;
const CGFloat kLeftInset = 6;

NSColor *SHex(unsigned rgb, CGFloat alpha = 1) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0 green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0 alpha:alpha];
}
// VS Code's activityBarBadge.background and activityBar.activeBorder (Dark
// and Light Modern), and the tabs' waiting orange (MCTabStrip.waitingColor).
NSColor *Accent() { return SHex(0x0078D4); }
NSColor *Waiting() { return SHex(0xF0A030); }

struct PanelInfo {
    const char *name, *symbol, *fallback;
};
const PanelInfo kPanels[] = {
    {"File Tree", "folder", "F"},
    {"Source Control", "arrow.triangle.branch", "S"},
    {"Agents", "sparkles", "A"},
    {"Processes", "cpu", "P"},
};
const NSInteger kPanelCount = sizeof kPanels / sizeof kPanels[0];

// `image`, a template, filled with `color` where it has ink.
void DrawTinted(NSImage *image, NSRect r, NSColor *color) {
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSaveGState(ctx);
    CGContextBeginTransparencyLayerWithRect(ctx, NSRectToCGRect(r), NULL);
    [image drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
             fraction:1 respectFlipped:YES hints:nil];
    [color set];
    NSRectFillUsingOperation(r, NSCompositingOperationSourceAtop);
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}
}  // namespace

// One icon: drawn by hand (an NSButton for its click, target and
// accessibility), never the keyboard's.
@interface MCSidebarIconButton : NSButton
@property(nonatomic, strong) NSImage *symbol;   // nil: the fallback letter
@property(nonatomic, copy) NSString *letter;
@property(nonatomic, copy) NSString *badge;
@property(nonatomic) BOOL dot, marked, hover;
@property(nonatomic, strong) NSColor *ink;
@end

@implementation MCSidebarIconButton

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *a in self.trackingAreas)
        if (a.owner == self) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect
               owner:self userInfo:nil]];
}
- (void)mouseEntered:(NSEvent *)e { (void)e; self.hover = YES; [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent *)e { (void)e; self.hover = NO; [self setNeedsDisplay:YES]; }
- (void)viewDidHide { [super viewDidHide]; self.hover = NO; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = self.bounds;
    const CGFloat cx = floor(NSMidX(b)), cy = floor(NSMidY(b));
    NSColor *ink = self.ink ?: NSColor.labelColor;
    // The panel on show and the one under the pointer in full; the rest
    // dimmed, as VS Code's activity bar.
    const BOOL lit = self.marked || self.hover;
    NSColor *color = [ink colorWithAlphaComponent:(lit ? 1.0 : 0.55) *
                                                  (self.cell.isHighlighted ? 0.7 : 1.0)];
    if (self.symbol) {
        NSSize s = self.symbol.size;
        NSRect r = NSMakeRect(round(cx - s.width / 2), round(cy - s.height / 2), s.width, s.height);
        DrawTinted(self.symbol, r, color);
    } else {
        NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold],
                            NSForegroundColorAttributeName: color};
        NSSize s = [self.letter sizeWithAttributes:a];
        [self.letter drawAtPoint:NSMakePoint(round(cx - s.width / 2), round(cy - s.height / 2))
                  withAttributes:a];
    }
    if (self.marked) {
        [Accent() set];
        NSRectFill(NSMakeRect(cx - 8, NSMaxY(b) - 2, 16, 2));
    }
    if (self.dot) {
        NSRect d = NSMakeRect(cx + 4, 4, 7, 7);
        [Waiting() set];
        [[NSBezierPath bezierPathWithOvalInRect:d] fill];
    } else if (self.badge.length) {
        NSDictionary *a = @{NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:8.5
                                                                                 weight:NSFontWeightSemibold],
                            NSForegroundColorAttributeName: NSColor.whiteColor};
        NSSize s = [self.badge sizeWithAttributes:a];
        const CGFloat h = 12, w = MAX(h, ceil(s.width) + 6);
        NSRect pill = NSMakeRect(MIN(cx + 2, NSMaxX(b) - w), 2, w, h);
        [Accent() set];
        [[NSBezierPath bezierPathWithRoundedRect:pill xRadius:h / 2 yRadius:h / 2] fill];
        [self.badge drawAtPoint:NSMakePoint(round(NSMidX(pill) - s.width / 2),
                                            round(NSMidY(pill) - s.height / 2))
                 withAttributes:a];
    }
}
@end

@implementation MCSidebarIconBar {
    NSMutableArray<MCSidebarIconButton *> *_buttons;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    _buttons = [NSMutableArray array];
    NSImageSymbolConfiguration *size =
        [NSImageSymbolConfiguration configurationWithPointSize:14 weight:NSFontWeightRegular];
    for (NSInteger i = 0; i < kPanelCount; i++) {
        MCSidebarIconButton *b = [[MCSidebarIconButton alloc]
            initWithFrame:NSMakeRect(kLeftInset + i * kButtonWidth, 0, kButtonWidth,
                                     kMCSidebarIconBarHeight)];
        NSString *name = @(kPanels[i].name);
        b.symbol = [[NSImage imageWithSystemSymbolName:@(kPanels[i].symbol)
                              accessibilityDescription:name] imageWithSymbolConfiguration:size];
        b.letter = @(kPanels[i].fallback);
        b.bordered = NO;
        b.title = @"";
        b.tag = i;
        b.target = self;
        b.action = @selector(picked:);
        b.refusesFirstResponder = YES;
        b.toolTip = name;
        b.accessibilityLabel = name;
        [_buttons addObject:b];
        [self addSubview:b];
    }
    self.current = MCSidebarPanelTree;
    return self;
}

- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)picked:(NSButton *)sender {
    if (self.onPick) self.onPick((MCSidebarPanel)sender.tag);
}

- (NSArray<NSButton *> *)buttons { return [_buttons copy]; }

- (MCSidebarIconButton *)buttonFor:(MCSidebarPanel)panel {
    return panel >= 0 && panel < (NSInteger)_buttons.count ? _buttons[(NSUInteger)panel] : nil;
}

- (void)setCurrent:(MCSidebarPanel)current {
    _current = current;
    for (MCSidebarIconButton *b in _buttons) {
        const BOOL marked = b.tag == current;
        if (b.marked != marked) {
            b.marked = marked;
            [b setNeedsDisplay:YES];
        }
    }
}

- (void)setBackground:(NSColor *)background text:(NSColor *)text {
    self.layer.backgroundColor = background.CGColor;
    for (MCSidebarIconButton *b in _buttons) {
        b.ink = text;
        [b setNeedsDisplay:YES];
    }
}

- (void)setBadge:(NSString *)badge forPanel:(MCSidebarPanel)panel {
    MCSidebarIconButton *b = [self buttonFor:panel];
    NSString *text = badge ?: @"";
    if (!b || [b.badge ?: @"" isEqualToString:text]) return;
    b.badge = text;
    [b setNeedsDisplay:YES];
}

- (void)setDot:(BOOL)dot forPanel:(MCSidebarPanel)panel {
    MCSidebarIconButton *b = [self buttonFor:panel];
    if (!b || b.dot == dot) return;
    b.dot = dot;
    [b setNeedsDisplay:YES];
}

- (void)setTip:(NSString *)tip forPanel:(MCSidebarPanel)panel {
    MCSidebarIconButton *b = [self buttonFor:panel];
    if (b && ![b.toolTip isEqualToString:tip]) b.toolTip = tip;
}

- (NSString *)badgeForPanel:(MCSidebarPanel)panel { return [self buttonFor:panel].badge ?: @""; }
- (BOOL)dotForPanel:(MCSidebarPanel)panel { return [self buttonFor:panel].dot; }
@end

@implementation MCSidebarHost

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    _showsIcons = YES;
    _iconBar = [[MCSidebarIconBar alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width,
                                                                  kMCSidebarIconBarHeight)];
    [self addSubview:_iconBar];
    [self layoutParts];
    return self;
}

- (void)setContent:(NSView *)content {
    if (content == _content) return;
    [_content removeFromSuperview];
    _content = content;
    if (content) [self addSubview:content positioned:NSWindowBelow relativeTo:_iconBar];
    [self layoutParts];
}

- (void)setShowsIcons:(BOOL)showsIcons {
    if (showsIcons == _showsIcons) return;
    _showsIcons = showsIcons;
    [self layoutParts];
}

- (BOOL)iconsShown { return !_iconBar.hidden; }

- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
    (void)oldSize;
    [self layoutParts];
}

// By hand: the row along the top, the panel under it. A collapsed sidebar
// (0 wide) hides the row, whose buttons would otherwise draw past it.
- (void)layoutParts {
    const NSSize s = self.bounds.size;
    const BOOL row = _showsIcons && s.width >= 1 && s.height > kMCSidebarIconBarHeight;
    _iconBar.hidden = !row;
    _iconBar.frame = NSMakeRect(0, s.height - kMCSidebarIconBarHeight, s.width, kMCSidebarIconBarHeight);
    const CGFloat top = row ? kMCSidebarIconBarHeight : 0;
    NSRect r = NSMakeRect(0, 0, s.width, MAX(0, s.height - top));
    if (_content && !NSEqualRects(_content.frame, r)) _content.frame = r;
}
@end
