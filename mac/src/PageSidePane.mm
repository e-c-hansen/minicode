// PageSidePane.mm — see PageSidePane.h.
#import "PageSidePane.h"
#import "EditorPanes.h"

// As Markdown's pane: each side keeps at least this much while both show,
// and the bar this close to the right edge closes the pane.
static const CGFloat kMinPaneWidth = 150;
static const CGFloat kSnapDistance = 16;

@implementation MCPageSidePane {
    NSRect _slot;   // the rect last split, in the host's coordinates
}

- (instancetype)initInView:(NSView *)host {
    if (!(self = [super init])) return nil;
    _fraction = 0.5;
    _divider = [[DragBar alloc] initWithFrame:NSZeroRect];
    _divider.vertical = YES;
    _divider.hidden = YES;
    __weak MCPageSidePane *weakSelf = self;
    _divider.onDrag = ^(CGFloat x) {
        MCPageSidePane *me = weakSelf;
        if (me.onDrag) me.onDrag(x);
    };
    [host addSubview:_divider];
    return self;
}

- (NSRect)layoutInRect:(NSRect)rect page:(NSView *)page {
    const CGFloat W = NSWidth(rect);
    const BOOL on = page != nil && W >= 2 * kMinPaneWidth;
    _shown = on;
    _divider.hidden = !on;
    if (!on) {
        _pageRect = NSZeroRect;
        return rect;
    }
    _slot = rect;
    CGFloat pw = round(W * _fraction);
    pw = MIN(MAX(pw, kMinPaneWidth), W - kMinPaneWidth);
    NSRect text = rect, pane = rect;
    text.size.width = W - pw;
    pane.origin.x = NSMaxX(text);
    pane.size.width = pw;
    _pageRect = pane;
    if (page.superview != _divider.superview)
        [_divider.superview addSubview:page positioned:NSWindowBelow relativeTo:_divider];
    if (!NSEqualRects(page.frame, pane)) page.frame = pane;
    page.hidden = NO;
    _divider.frame = NSMakeRect(NSMinX(pane) - kDragBarHeight / 2, NSMinY(pane), kDragBarHeight,
                                NSHeight(pane));
    return text;
}

- (BOOL)dragTo:(CGFloat)x {
    const CGFloat W = NSWidth(_slot);
    if (W <= 0) return YES;
    if (x >= NSMaxX(_slot) - kSnapDistance) return NO;
    const CGFloat pw =
        MIN(MAX(NSMaxX(_slot) - x, kMinPaneWidth), MAX(kMinPaneWidth, W - kMinPaneWidth));
    _fraction = pw / W;
    return YES;
}
@end
