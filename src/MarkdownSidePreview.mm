// MarkdownSidePreview.mm — see MarkdownSidePreview.h.
#import "MarkdownSidePreview.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "EditorPanes.h"
#import "TextKit2.h"

NSRect MCTextViewRectForCharacter(NSTextView *tv, NSUInteger i) {
    const NSUInteger n = tv.string.length;
    if (MCUsesTextKit2(tv) || n == 0) return NSZeroRect;
    NSLayoutManager *lm = tv.layoutManager;
    if (i >= n) i = n - 1;
    [lm ensureLayoutForCharacterRange:NSMakeRange(0, i + 1)];
    NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(i, 1) actualCharacterRange:NULL];
    NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
    if (NSIsEmptyRect(r)) r.size = NSMakeSize(1, 1);   // a bare newline
    NSPoint o = tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

void MCScrollTextViewToY(NSScrollView *scroll, NSTextView *tv, CGFloat y) {
    NSClipView *clip = scroll.contentView;
    const CGFloat most = NSHeight(tv.frame) - NSHeight(clip.bounds);
    y = MAX(0, MIN(y, MAX(0, most)));
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [scroll reflectScrolledClipView:clip];
}

// The page's text view: its context menu ends with Close and Adjust
// Settings (the Markdown surface's keys, as the full preview's does).
@interface MCSidePreviewTextView : NSTextView
@property(nonatomic, weak) MCMarkdownSidePreview *owner;
@end

@implementation MCSidePreviewTextView
- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSMenu *menu = [super menuForEvent:event] ?: [[NSMenu alloc] initWithTitle:@""];
    if (self.owner.onClose) {
        if (menu.numberOfItems) [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *close = [menu addItemWithTitle:@"Close Preview to the Side"
                                            action:@selector(closeSidePreview:)
                                     keyEquivalent:@""];
        close.target = self;
    }
    MCAddAdjustSettingsItem(menu, @"markdown", self);
    return menu;
}
- (void)closeSidePreview:(id)sender {
    (void)sender;
    if (self.owner.onClose) self.owner.onClose();
}
@end

@implementation MCMarkdownSidePreview {
    NSRect _slot;   // the rect last split, in the host's coordinates
}

// Each side keeps at least this much while both show; a slot narrower than
// twice it shows the source alone.
static const CGFloat kMinPaneWidth = 150;
static const CGFloat kSnapDistance = 16;   // the bar this close to the right edge closes it

- (instancetype)initInView:(NSView *)host {
    if (!(self = [super init])) return nil;
    _fraction = 0.5;
    _scrollView = [[PanelScrollView alloc] initWithFrame:NSZeroRect];
    _scrollView.hasVerticalScroller = YES;
    _scrollView.autohidesScrollers = YES;
    _scrollView.automaticallyAdjustsContentInsets = NO;
    _scrollView.hidden = YES;

    // TextKit 1, as the editor's: the place keeping and the source lines
    // read positions from a layout manager.
    NSTextStorage *storage = [NSTextStorage new];
    NSLayoutManager *layout = [NSLayoutManager new];
    [storage addLayoutManager:layout];
    NSTextContainer *box = [[NSTextContainer alloc] initWithSize:NSMakeSize(400, CGFLOAT_MAX)];
    [layout addTextContainer:box];
    MCSidePreviewTextView *tv = [[MCSidePreviewTextView alloc]
        initWithFrame:NSMakeRect(0, 0, 400, 400) textContainer:box];
    tv.owner = self;
    tv.editable = NO;
    tv.selectable = YES;
    tv.richText = YES;
    tv.usesFontPanel = NO;   // see the editor's: the color panel follows the caret otherwise
    tv.usesFindBar = NO;
    tv.drawsBackground = NO;   // the scroll view's backdrop paints it
    tv.textContainerInset = NSMakeSize(14, 10);
    tv.minSize = NSMakeSize(0, 0);
    tv.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    tv.verticallyResizable = YES;
    tv.horizontallyResizable = NO;
    tv.autoresizingMask = NSViewWidthSizable;
    tv.textContainer.widthTracksTextView = YES;
    // Links keep the page's colors; only the pointer changes.
    tv.linkTextAttributes = @{NSCursorAttributeName: [NSCursor pointingHandCursor]};
    tv.delegate = self;
    _textView = tv;
    _scrollView.documentView = tv;

    _divider = [[DragBar alloc] initWithFrame:NSZeroRect];
    _divider.vertical = YES;
    _divider.hidden = YES;
    __weak MCMarkdownSidePreview *weakSelf = self;
    _divider.onDrag = ^(CGFloat x) {
        MCMarkdownSidePreview *me = weakSelf;
        if (me.onDrag) me.onDrag(x);
    };
    [host addSubview:_scrollView];
    [host addSubview:_divider];
    [self applySettings];
    return self;
}

- (void)dealloc {
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
}

- (NSRect)layoutInRect:(NSRect)rect shown:(BOOL)on {
    const CGFloat W = NSWidth(rect);
    on = on && W >= 2 * kMinPaneWidth;
    _shown = on;
    _scrollView.hidden = !on;
    _divider.hidden = !on;
    if (!on) return rect;
    _slot = rect;
    CGFloat pw = round(W * _fraction);
    pw = MIN(MAX(pw, kMinPaneWidth), W - kMinPaneWidth);
    NSRect text = rect, pane = rect;
    text.size.width = W - pw;
    pane.origin.x = NSMaxX(text);
    pane.size.width = pw;
    if (!NSEqualRects(_scrollView.frame, pane)) {
        _scrollView.frame = pane;
        [_scrollView tile];
    }
    _divider.frame = NSMakeRect(NSMinX(pane) - kDragBarHeight / 2, NSMinY(pane),
                                kDragBarHeight, NSHeight(pane));
    return text;
}

- (BOOL)dragTo:(CGFloat)x {
    const CGFloat W = NSWidth(_slot);
    if (W <= 0) return YES;
    if (x >= NSMaxX(_slot) - kSnapDistance) return NO;
    const CGFloat pw = MIN(MAX(NSMaxX(_slot) - x, kMinPaneWidth), MAX(kMinPaneWidth, W - kMinPaneWidth));
    _fraction = pw / W;
    return YES;
}

- (CGFloat)pageWidth {
    NSTextContainer *box = _textView.textContainer;
    return box.size.width - 2 * box.lineFragmentPadding;
}

- (void)showPage:(NSAttributedString *)page {
    [_textView.textStorage setAttributedString:page ?: [NSAttributedString new]];
    [_textView scrollToBeginningOfDocument:nil];
}

- (void)clear {
    [self cancelRender];
    _anchors = nil;
    if (_textView.textStorage.length)
        [_textView.textStorage setAttributedString:[NSAttributedString new]];
}

- (BOOL)scrollToAnchor:(NSString *)anchor {
    NSString *key = (anchor.stringByRemovingPercentEncoding ?: anchor).lowercaseString;
    NSNumber *at = _anchors[key];
    if (!at) return NO;
    const NSRect r = MCTextViewRectForCharacter(_textView, at.unsignedIntegerValue);
    if (NSIsEmptyRect(r)) [_textView scrollRangeToVisible:NSMakeRange(at.unsignedIntegerValue, 0)];
    else MCScrollTextViewToY(_scrollView, _textView, NSMinY(r) - 8);
    return YES;
}

- (CGFloat)scrollY { return _scrollView.contentView.bounds.origin.y; }

- (void)setScrollY:(CGFloat)y {
    if (!MCUsesTextKit2(_textView))   // the full height, or the scroll stops where layout did
        [_textView.layoutManager ensureLayoutForTextContainer:_textView.textContainer];
    MCScrollTextViewToY(_scrollView, _textView, y);
}

- (void)scheduleRenderAfter:(NSTimeInterval)delay {
    [self cancelRender];
    _renderPending = YES;
    [self performSelector:@selector(renderNow) withObject:nil afterDelay:delay];
}

- (void)cancelRender {
    _renderPending = NO;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(renderNow) object:nil];
}

- (void)renderNow {
    _renderPending = NO;
    if (self.onRender) self.onRender();
}

- (BOOL)textView:(NSTextView *)tv clickedOnLink:(id)link atIndex:(NSUInteger)i {
    (void)tv;
    return self.onLink ? self.onLink(link, i) : NO;
}

- (void)applySettings {
    _scrollView.panelColor = [[AppSettings shared] background:Surface::Editor];
    _textView.selectedTextAttributes =
        @{NSBackgroundColorAttributeName: [NSColor selectedTextBackgroundColor]};
}
@end
