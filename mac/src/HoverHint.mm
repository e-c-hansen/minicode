// HoverHint.mm — see HoverHint.h.
#import "HoverHint.h"

namespace {
const CGFloat kPadX = 6, kPadY = 3;
const CGFloat kMaxTextWidth = 420;
// Below the pointer's arrow, as AppKit places its tooltips.
const CGFloat kBelowPointer = 20, kAbovePointer = 6;
}  // namespace

@implementation MCHoverHint {
    NSTextField *_label;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.material = NSVisualEffectMaterialToolTip;
    self.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    self.state = NSVisualEffectStateActive;
    self.wantsLayer = YES;
    self.layer.cornerRadius = 4;
    self.layer.masksToBounds = YES;
    self.layer.borderWidth = 0.5;
    self.layer.borderColor = [NSColor.separatorColor CGColor];
    _label = [NSTextField wrappingLabelWithString:@""];
    _label.font = [NSFont toolTipsFontOfSize:0];
    _label.textColor = NSColor.labelColor;
    _label.selectable = NO;
    [self addSubview:_label];
    return self;
}

// Never in the way of a click, a selection or a scroll under it.
- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

+ (MCHoverHint *)hintIn:(NSView *)host {
    for (NSView *v in host.subviews)
        if ([v isKindOfClass:MCHoverHint.class]) return (MCHoverHint *)v;
    return nil;
}

+ (void)showText:(NSString *)text inView:(NSView *)host atPointer:(NSPoint)pointer {
    if (!host || !text.length) return;
    MCHoverHint *hint = [self hintIn:host];
    if (!hint) {
        hint = [[MCHoverHint alloc] initWithFrame:NSZeroRect];
        [host addSubview:hint];
    }
    hint->_label.stringValue = text;
    hint.appearance = host.effectiveAppearance;   // the theme's light or dark
    hint.layer.borderColor = [NSColor.separatorColor CGColor];
    const NSRect vis = host.visibleRect;
    const CGFloat room = MAX((CGFloat)40, MIN(kMaxTextWidth, NSWidth(vis) - 2 * kPadX - 4));
    NSSize s = [hint->_label.cell cellSizeForBounds:NSMakeRect(0, 0, room, CGFLOAT_MAX)];
    s.width = MIN(ceil(s.width), room);
    s.height = ceil(s.height);
    const NSSize box = NSMakeSize(s.width + 2 * kPadX, s.height + 2 * kPadY);
    // Under the pointer, else above it; inside the visible rect.
    const BOOL flipped = host.isFlipped;
    CGFloat x = MIN(pointer.x, NSMaxX(vis) - box.width - 2);
    x = MAX(x, NSMinX(vis) + 2);
    CGFloat y;
    if (flipped) {
        y = pointer.y + kBelowPointer;
        if (y + box.height > NSMaxY(vis)) y = pointer.y - kAbovePointer - box.height;
        y = MAX(NSMinY(vis), MIN(y, NSMaxY(vis) - box.height));
    } else {
        y = pointer.y - kBelowPointer - box.height;
        if (y < NSMinY(vis)) y = pointer.y + kAbovePointer;
        y = MAX(NSMinY(vis), MIN(y, NSMaxY(vis) - box.height));
    }
    hint.frame = NSMakeRect(round(x), round(y), box.width, box.height);
    hint->_label.frame = NSMakeRect(kPadX, kPadY, s.width, s.height);
    hint.hidden = NO;
}

+ (void)hideInView:(NSView *)host {
    [[self hintIn:host] removeFromSuperview];
}

+ (NSString *)textInView:(NSView *)host {
    MCHoverHint *hint = [self hintIn:host];
    return hint ? hint->_label.stringValue : nil;
}
@end
