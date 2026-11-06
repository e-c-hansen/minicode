// EditorPanes.mm — see EditorPanes.h.
#import "EditorPanes.h"

@implementation PanelHost
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; if (self.onLayout) self.onLayout(); }
@end

@implementation DragBar {
    CGFloat _grabOffset;   // pointer Y minus boundary Y at mouseDown
    BOOL _hot;             // hovered or dragging
    BOOL _dragging;
}

- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingCursorUpdate |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:nil]];
}

// Cursor rects lose to overlapping text views; a tracking-area cursor update
// goes to whichever view is on top under the pointer.
- (NSCursor *)cursor {
    return self.vertical ? [NSCursor resizeLeftRightCursor]
                         : [NSCursor resizeUpDownCursor];
}
- (void)cursorUpdate:(NSEvent *)event { [[self cursor] set]; }

- (void)setHot:(BOOL)hot {
    if (hot == _hot) return;
    _hot = hot;
    self.needsDisplay = YES;
}
- (void)mouseEntered:(NSEvent *)event { [self setHot:YES]; }
// A bar hidden mid-drag (dragged to an edge) never sees the pointer leave,
// and would come back lit.
- (void)viewDidHide { [super viewDidHide]; _hot = NO; }
- (void)mouseExited:(NSEvent *)event { if (!_dragging) [self setHot:NO]; }

- (CGFloat)boundary {
    return self.vertical ? NSMidX(self.frame) : NSMidY(self.frame);
}
- (CGFloat)along:(NSPoint)p { return self.vertical ? p.x : p.y; }

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self.superview convertPoint:event.locationInWindow fromView:nil];
    _grabOffset = [self along:p] - [self boundary];
    _dragging = YES;
    [self setHot:YES];
    [[self cursor] set];
}
- (void)mouseDragged:(NSEvent *)event {
    NSPoint p = [self.superview convertPoint:event.locationInWindow fromView:nil];
    [[self cursor] set];
    [self setHot:YES];
    if (self.onDrag) self.onDrag([self along:p] - _grabOffset);
}
- (void)mouseUp:(NSEvent *)event {
    _dragging = NO;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    [self setHot:NSPointInRect(p, self.bounds)];
}

- (void)drawRect:(NSRect)dirty {
    NSRect b = self.bounds;
    CGFloat h = _hot ? 2 : 1;
    NSRect line = self.vertical
        ? NSMakeRect(floor(NSMidX(b) - h / 2), 0, h, b.size.height)
        : NSMakeRect(0, floor(NSMidY(b) - h / 2), b.size.width, h);
    [(_hot ? [NSColor colorWithSRGBRed:0x4E / 255.0 green:0xA1 / 255.0
                                  blue:0xF7 / 255.0 alpha:1]
           : [NSColor colorWithSRGBRed:0x33 / 255.0 green:0x33 / 255.0
                                  blue:0x33 / 255.0 alpha:1]) set];
    NSRectFill(line);
}
@end

@implementation PanelScrollView {
    NSView *_backdrop;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.drawsBackground = NO;
        _backdrop = [[NSView alloc] initWithFrame:self.bounds];
        _backdrop.wantsLayer = YES;
        [self addSubview:_backdrop positioned:NSWindowBelow relativeTo:self.contentView];
    }
    return self;
}
- (void)tile {
    [super tile];
    // Not behind a ruler (the editor's line numbers), which paints its own
    // background: a translucent color painted twice would come out darker.
    NSRect back = self.bounds;
    if (self.rulersVisible && self.hasVerticalRuler && self.verticalRulerView) {
        const CGFloat ruler = NSMaxX(self.verticalRulerView.frame);
        back.origin.x += ruler;
        back.size.width = MAX(0, back.size.width - ruler);
    }
    _backdrop.frame = back;
    // A text view that wraps is exactly as wide as what shows of it. Left to
    // itself it kept the width it was created with (the whole window), so
    // lines, tables and pictures ran past the pane's right edge.
    NSTextView *tv = [self.documentView isKindOfClass:NSTextView.class]
                         ? (NSTextView *)self.documentView : nil;
    // A ruler (the line numbers) is an inset of the clip view, which still
    // spans the whole width.
    const NSEdgeInsets in = self.contentView.contentInsets;
    const CGFloat width = self.contentSize.width - in.left - in.right;
    if (tv && !tv.horizontallyResizable && width > 0 && tv.frame.size.width != width)
        [tv setFrameSize:NSMakeSize(width, tv.frame.size.height)];
}
- (void)setPanelColor:(NSColor *)color {
    _panelColor = color;
    _backdrop.layer.backgroundColor = color.CGColor;
}
@end
