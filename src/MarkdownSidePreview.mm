// MarkdownSidePreview.mm — see MarkdownSidePreview.h.
#import "MarkdownSidePreview.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "EditorPanes.h"
#import "TextKit2.h"
#include "PreviewScroll.h"
#include <cmath>

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
    // Laid out down to the screenful there, or the view is not yet tall
    // enough to scroll that far and the place is cut short.
    if (!MCUsesTextKit2(tv) && tv.textContainer)
        [tv.layoutManager ensureLayoutForBoundingRect:NSMakeRect(0, MAX(0, y - tv.textContainerOrigin.y),
                                                                 NSWidth(tv.frame), NSHeight(clip.bounds))
                                      inTextContainer:tv.textContainer];
    const CGFloat most = NSHeight(tv.frame) - NSHeight(clip.bounds);
    y = MAX(0, MIN(y, MAX(0, most)));
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [scroll reflectScrolledClipView:clip];
}

// The page's text view: its context menu ends with Close and Adjust
// Settings (the Markdown surface's keys, as the full preview's does); a
// double-click goes to the block editor; and it draws the bar beside the
// block of the source's caret line.
@interface MCSidePreviewTextView : NSTextView
@property(nonatomic, weak) MCMarkdownSidePreview *owner;
@end

// VS Code's markdown.css: a 3 px bar, white at 40% on a dark theme and
// black at 15% on a light one.
static NSColor *MCMarkColor(void) {
    return [AppSettings shared].settings.isDark() ? [NSColor colorWithWhite:1 alpha:0.4]
                                                  : [NSColor colorWithWhite:0 alpha:0.15];
}

@implementation MCSidePreviewTextView
- (void)drawViewBackgroundInRect:(NSRect)rect {
    [super drawViewBackgroundInRect:rect];
    const NSRect mark = self.owner.markRect;
    if (NSIsEmptyRect(mark) || !NSIntersectsRect(mark, rect)) return;
    [MCMarkColor() setFill];
    NSRectFillUsingOperation(mark, NSCompositingOperationSourceOver);
}

- (void)mouseDown:(NSEvent *)event {
    MCMarkdownSidePreview *owner = self.owner;
    if (event.clickCount == 2 && owner.onDoubleClick) {
        const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        const NSUInteger i = [self characterIndexForInsertionAtPoint:p];
        if (owner.onDoubleClick(i, p)) return;
    }
    [super mouseDown:event];
}

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
    // Following the source.
    __weak NSTextView *_source;
    __weak NSScrollView *_sourceScroll;
    NSUInteger (^_lineOf)(NSUInteger);
    NSUInteger (^_startOf)(NSUInteger);
    NSUInteger (^_lineCount)(void);
    PreviewScroll::Page _map;   // the page's marks, made on first use after a change
    BOOL _mapValid;
    CGFloat _mapWidth;          // the text width it was made for
    NSInteger _holding;         // the page's moves lead nothing while above 0
    BOOL _moving;               // one side is being moved to the other
    BOOL _observingPage;
    CGFloat _pageY, _sourceY;   // where each side was last seen
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
    [[NSNotificationCenter defaultCenter] removeObserver:self];
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
        [self holdFollowing:^{
            self->_scrollView.frame = pane;
            [self->_scrollView tile];
        }];
        [self pageMoved];
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
    [self holdFollowing:^{
        [self->_textView.textStorage setAttributedString:page ?: [NSAttributedString new]];
        [self->_textView scrollToBeginningOfDocument:nil];
    }];
    _mapValid = NO;
    [self setMarkedRange:NSMakeRange(NSNotFound, 0)];
}

- (void)clear {
    [self cancelRender];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(followAfterMove)
                                               object:nil];
    _anchors = nil;
    _mapValid = NO;
    _pageLeads = NO;
    [self setMarkedRange:NSMakeRange(NSNotFound, 0)];
    if (_textView.textStorage.length)
        [self holdFollowing:^{
            [self->_textView.textStorage setAttributedString:[NSAttributedString new]];
        }];
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
    [self holdFollowing:^{
        if (!MCUsesTextKit2(self->_textView))   // the full height, or the scroll stops where layout did
            [self->_textView.layoutManager ensureLayoutForTextContainer:self->_textView.textContainer];
        MCScrollTextViewToY(self->_scrollView, self->_textView, y);
    }];
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
    if (!NSIsEmptyRect(_markRect)) [_textView setNeedsDisplayInRect:_markRect];   // its color
}

// ------------------------------------------------ following the source

- (void)followSource:(NSTextView *)source
              scroll:(NSScrollView *)scroll
     lineOfCharacter:(NSUInteger (^)(NSUInteger))lineOf
         startOfLine:(NSUInteger (^)(NSUInteger))startOf
           lineCount:(NSUInteger (^)(void))lineCount {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (_sourceScroll)
        [nc removeObserver:self name:NSViewBoundsDidChangeNotification
                    object:_sourceScroll.contentView];
    if (_source)
        [nc removeObserver:self name:NSTextViewDidChangeSelectionNotification object:_source];
    _source = source;
    _sourceScroll = scroll;
    _lineOf = [lineOf copy];
    _startOf = [startOf copy];
    _lineCount = [lineCount copy];
    scroll.contentView.postsBoundsChangedNotifications = YES;
    [nc addObserver:self selector:@selector(sourceScrolled:)
               name:NSViewBoundsDidChangeNotification object:scroll.contentView];
    [nc addObserver:self selector:@selector(sourceSelectionChanged:)
               name:NSTextViewDidChangeSelectionNotification object:source];
    NSClipView *clip = _scrollView.contentView;
    if (!_observingPage) {
        _observingPage = YES;
        clip.postsBoundsChangedNotifications = YES;
        [nc addObserver:self selector:@selector(pageScrolled:)
                   name:NSViewBoundsDidChangeNotification object:clip];
    }
    _sourceY = scroll.contentView.bounds.origin.y;
    _pageY = clip.bounds.origin.y;
}

- (BOOL)isFollowing {
    return _shown && _source && _sourceScroll && _lineOf && _startOf && _lineCount &&
           self.following && self.following();
}

- (void)holdFollowing:(void (^)(void))block {
    _holding++;
    block();
    _holding--;
    _pageY = _scrollView.contentView.bounds.origin.y;
}

// The page's marks: where each source line's text starts, for the text
// width the page is laid out at now.
- (const PreviewScroll::Page &)map {
    const CGFloat width = self.pageWidth;
    if (!_mapValid || width != _mapWidth) {
        _mapValid = YES;
        _mapWidth = width;
        _map = PreviewScroll::Page();
        NSTextView *tv = _textView;
        NSTextStorage *st = tv.textStorage;
        if (_sourceLineKey && st.length && !MCUsesTextKit2(tv)) {
            NSLayoutManager *lm = tv.layoutManager;
            [self holdFollowing:^{   // the whole page, so every top is where it will stay
                [lm ensureLayoutForTextContainer:tv.textContainer];
            }];
            std::vector<PreviewScroll::Mark> runs;
            std::vector<PreviewScroll::Mark> *out = &runs;
            [st enumerateAttribute:_sourceLineKey inRange:NSMakeRange(0, st.length) options:0
                        usingBlock:^(NSNumber *v, NSRange r, BOOL *stop) {
                (void)stop;
                if (![v isKindOfClass:NSNumber.class]) return;
                const int line = v.intValue;
                if (!out->empty() && line <= out->back().line) return;   // no rect needed
                const NSRect rect = MCTextViewRectForCharacter(tv, r.location);
                out->push_back({line, NSMinY(rect), (long)r.location});
            }];
            _map.marks = PreviewScroll::marks(runs);
            _map.bottom = NSMaxY([lm usedRectForTextContainer:tv.textContainer]) +
                          tv.textContainerOrigin.y;
        }
    }
    _map.lines = _lineCount ? (int)_lineCount() : 0;
    return _map;
}

// Where line `l` of the source starts, down its text view; past the last
// line, the end of its text.
- (CGFloat)sourceTopOfLine:(NSUInteger)l {
    NSTextView *tv = _source;
    NSLayoutManager *lm = tv.layoutManager;
    const NSUInteger n = tv.string.length;
    const NSUInteger at = l < _lineCount() ? _startOf(l) : n;
    if (at < n) return NSMinY(MCTextViewRectForCharacter(tv, at));
    // The end: the empty line after a final break, or the bottom of the text.
    if (n) [lm ensureLayoutForCharacterRange:NSMakeRange(0, n)];
    const NSRect extra = lm.extraLineFragmentRect;
    const CGFloat o = tv.textContainerOrigin.y;
    if (!NSIsEmptyRect(extra) && l < _lineCount()) return NSMinY(extra) + o;
    return NSMaxY([lm usedRectForTextContainer:tv.textContainer]) + o;
}

- (double)sourceTopLine {
    NSTextView *tv = _source;
    if (!tv || !_lineOf || MCUsesTextKit2(tv) || tv.string.length == 0) return 0;
    const CGFloat y = NSMinY(tv.visibleRect);
    if (y <= 0) return 0;
    NSLayoutManager *lm = tv.layoutManager;
    const NSPoint o = tv.textContainerOrigin;
    const NSUInteger g = [lm glyphIndexForPoint:NSMakePoint(0, MAX(0, y - o.y))
                                inTextContainer:tv.textContainer
                 fractionOfDistanceThroughGlyph:NULL];
    NSUInteger l = _lineOf([lm characterIndexForGlyphAtIndex:g]);
    CGFloat top = [self sourceTopOfLine:l];
    if (top > y && l > 0) top = [self sourceTopOfLine:--l];   // in the space above a line
    const CGFloat next = [self sourceTopOfLine:l + 1];
    if (next <= top || y <= top) return l;
    return l + MIN(1.0, (y - top) / (next - top));
}

- (double)pageTopLine {
    return PreviewScroll::lineForY([self map], NSMinY(_textView.visibleRect));
}

- (void)scrollPageToSource {
    if (![self isFollowing] || MCUsesTextKit2(_textView)) return;
    const double y = PreviewScroll::yForLine([self map], self.sourceTopLine);
    _moving = YES;
    [self holdFollowing:^{ MCScrollTextViewToY(self->_scrollView, self->_textView, y); }];
    _moving = NO;
}

- (void)scrollSourceToPage {
    if (![self isFollowing]) return;
    NSTextView *tv = _source;
    NSScrollView *scroll = _sourceScroll;
    const NSUInteger count = _lineCount();
    if (MCUsesTextKit2(tv) || count == 0) return;
    const double line = MAX(0.0, self.pageTopLine);
    NSUInteger l = (NSUInteger)floor(line);
    double fraction = line - l;
    if (l >= count) {
        l = count - 1;
        fraction = 0;
    }
    const CGFloat top = [self sourceTopOfLine:l];
    const CGFloat y = top + fraction * MAX(0.0, [self sourceTopOfLine:l + 1] - top);
    _moving = YES;
    MCScrollTextViewToY(scroll, tv, y);
    _moving = NO;
    _sourceY = scroll.contentView.bounds.origin.y;
}

// The user scrolled the source (or typed it along): the page follows.
- (void)sourceScrolled:(NSNotification *)note {
    (void)note;
    const CGFloat y = _sourceScroll.contentView.bounds.origin.y;
    if (fabs(y - _sourceY) < 0.5) return;   // resized, not moved
    _sourceY = y;
    if (_moving || ![self isFollowing]) return;
    _pageLeads = NO;
    [self scrollPageToSource];
}

// The user scrolled the page: the source follows.
- (void)pageScrolled:(NSNotification *)note {
    (void)note;
    const CGFloat y = _scrollView.contentView.bounds.origin.y;
    if (fabs(y - _pageY) < 0.5) return;
    _pageY = y;
    if (_moving || _holding || ![self isFollowing]) return;
    _pageLeads = YES;
    [self scrollSourceToPage];
}

- (void)pageRendered {
    _mapValid = NO;
    _pageY = _scrollView.contentView.bounds.origin.y;
    [self markCaretLine];
    if (!_pageLeads) [self scrollPageToSource];
}

// The pane was resized: once both sides are laid out, the follower is put
// back by the leader, and the mark redrawn where its block now is.
- (void)pageMoved {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(followAfterMove)
                                               object:nil];
    [self performSelector:@selector(followAfterMove) withObject:nil afterDelay:0];
}

- (void)followAfterMove {
    if (![self isFollowing]) return;
    [self markCaretLine];
    if (_pageLeads) [self scrollSourceToPage];
    else [self scrollPageToSource];
}

- (void)sourceSelectionChanged:(NSNotification *)note {
    (void)note;
    [self markCaretLine];
}

// What a paragraph of the page belongs to beyond itself: a table's cells
// one table, a code block's lines one box. nil for a paragraph alone.
static id MCBlockOfParagraph(NSTextStorage *st, NSUInteger at) {
    NSParagraphStyle *ps = [st attribute:NSParagraphStyleAttributeName atIndex:at effectiveRange:NULL];
    NSTextBlock *b = ps.textBlocks.lastObject;
    if ([b isKindOfClass:NSTextTableBlock.class]) return ((NSTextTableBlock *)b).table;
    return b.backgroundColor ? b : nil;   // a code block's box
}

- (void)markCaretLine {
    if (![self isFollowing]) {
        [self setMarkedRange:NSMakeRange(NSNotFound, 0)];
        return;
    }
    NSTextView *src = _source;
    const PreviewScroll::Page &map = [self map];
    const double line = (double)_lineOf(NSMaxRange(src.selectedRange));
    NSTextStorage *st = _textView.textStorage;
    NSString *page = st.string;
    NSCharacterSet *ink = NSCharacterSet.whitespaceAndNewlineCharacterSet.invertedSet;
    NSRange mark = NSMakeRange(NSNotFound, 0);
    // The block holding the line, or before a blank line's gap the block
    // above it, as VS Code marks the element before the line.
    for (int i = PreviewScroll::markAt(map, line); i >= 0; i--) {
        const NSUInteger at = (NSUInteger)map.marks[(size_t)i].at;
        if (at >= page.length) continue;
        const NSRange p = [page paragraphRangeForRange:NSMakeRange(at, 0)];
        if ([page rangeOfCharacterFromSet:ink options:0 range:p].location != NSNotFound) {
            mark = p;
            break;
        }
    }
    if (mark.location != NSNotFound) {
        if (id block = MCBlockOfParagraph(st, mark.location)) {
            while (mark.location > 0 && MCBlockOfParagraph(st, mark.location - 1) == block)
                mark = NSUnionRange(mark, [page paragraphRangeForRange:NSMakeRange(mark.location - 1, 0)]);
            while (NSMaxRange(mark) < page.length && MCBlockOfParagraph(st, NSMaxRange(mark)) == block)
                mark = NSUnionRange(mark, [page paragraphRangeForRange:NSMakeRange(NSMaxRange(mark), 0)]);
        }
    }
    [self setMarkedRange:mark];
}

- (void)setMarkedRange:(NSRange)range {
    const NSRect old = _markRect;
    _markedRange = range;
    _markRect = NSZeroRect;
    NSTextView *tv = _textView;
    if (range.location != NSNotFound && range.length &&
        NSMaxRange(range) <= tv.textStorage.length && !MCUsesTextKit2(tv)) {
        NSLayoutManager *lm = tv.layoutManager;
        [lm ensureLayoutForCharacterRange:NSMakeRange(0, NSMaxRange(range))];
        const NSRange glyphs = [lm glyphRangeForCharacterRange:range actualCharacterRange:NULL];
        const NSRect box = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
        const NSPoint o = tv.textContainerOrigin;
        // VS Code's sits 12 px left of the block; here left of the text's edge.
        const CGFloat x = MAX(1.0, o.x + tv.textContainer.lineFragmentPadding - 12);
        if (NSHeight(box) > 0) _markRect = NSMakeRect(x, NSMinY(box) + o.y, 3, NSHeight(box));
    }
    if (NSEqualRects(old, _markRect)) return;
    if (!NSIsEmptyRect(old)) [tv setNeedsDisplayInRect:old];
    if (!NSIsEmptyRect(_markRect)) [tv setNeedsDisplayInRect:_markRect];
}
@end
