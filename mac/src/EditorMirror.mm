// EditorMirror.mm — see EditorMirror.h.
#import "SourceStorage.h"
#import "EditorMirror.h"
#include "LineIndex.h"
#include <vector>

// The editor gutter's fold lane (Gutter.mm), right of the numbers.
static const CGFloat kMirrorFoldLane = 14;
static const NSUInteger kMirrorMinDigits = 3;

// ---------------------------------------------------------------- the views
@interface MCMirrorTextView : NSTextView
@property(nonatomic, copy) void (^onPress)(NSEvent *event);
@end

@implementation MCMirrorTextView
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (void)mouseDown:(NSEvent *)event {
    if (self.onPress) self.onPress(event);
}
- (void)rightMouseDown:(NSEvent *)event {
    if (self.onPress) self.onPress(event);
}
- (void)otherMouseDown:(NSEvent *)event {
    if (self.onPress) self.onPress(event);
}
// Read-only: nothing to offer but what the focused editor will.
- (NSMenu *)menuForEvent:(NSEvent *)event { return nil; }
@end

@interface MCMirrorPictureView : NSImageView
@property(nonatomic, copy) void (^onPress)(NSEvent *event);
@end

@implementation MCMirrorPictureView
- (BOOL)acceptsFirstResponder { return NO; }
- (BOOL)isFlipped { return YES; }
- (void)mouseDown:(NSEvent *)event {
    if (self.onPress) self.onPress(event);
}
- (void)rightMouseDown:(NSEvent *)event {
    if (self.onPress) self.onPress(event);
}
// Drawn at the top left at its own size, as the editor showed it.
- (void)drawRect:(NSRect)dirty {
    NSImage *img = self.image;
    if (!img) return;
    const NSSize s = img.size;
    [img drawInRect:NSMakeRect(0, 0, s.width, s.height) fromRect:NSZeroRect
          operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
}
@end

// The line numbers, on the scroll view as its vertical ruler.
@interface MCMirrorGutter : NSRulerView
@property(nonatomic, strong) NSFont *font;
@property(nonatomic, strong) NSColor *numberColor;
@property(nonatomic, assign) CGFloat laneWidth;
- (void)storageChanged;
@end

@implementation MCMirrorGutter {
    __weak NSTextView *_tv;
    __weak NSTextStorage *_observed;
    LineIndex _index;
    NSUInteger _digits;
}

- (instancetype)initWithTextView:(NSTextView *)tv {
    NSScrollView *scroll = tv.enclosingScrollView;
    if ((self = [super initWithScrollView:scroll orientation:NSVerticalRuler])) {
        _tv = tv;
        _laneWidth = 26;
        self.clientView = tv;
        self.reservedThicknessForMarkers = 0;
        self.reservedThicknessForAccessoryView = 0;
        self.wantsLayer = YES;
        tv.usesRuler = NO;
        scroll.verticalRulerView = self;
        scroll.hasVerticalRuler = YES;
        scroll.rulersVisible = YES;
        scroll.contentView.postsBoundsChangedNotifications = YES;
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(redraw:)
                   name:NSViewBoundsDidChangeNotification object:scroll.contentView];
        [self storageChanged];
    }
    return self;
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (void)redraw:(NSNotification *)note { self.needsDisplay = YES; }

- (void)reindex {
    NSString *s = _tv.textStorage.string;
    std::vector<unichar> buf(s.length);
    if (s.length) [s getCharacters:buf.data() range:NSMakeRange(0, s.length)];
    _index.reset(reinterpret_cast<const char16_t *>(buf.data()), buf.size());
    [self updateThickness];
}

// The storage under the view changed (shared, copied, emptied).
- (void)storageChanged {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    NSTextStorage *ts = _tv.textStorage;
    if (_observed != ts) {
        if (_observed)
            [nc removeObserver:self name:NSTextStorageDidProcessEditingNotification
                        object:_observed];
        _observed = ts;
        if (ts)
            [nc addObserver:self selector:@selector(storageEdited:)
                       name:NSTextStorageDidProcessEditingNotification object:ts];
    }
    [self reindex];
    self.needsDisplay = YES;
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSRange er = ts.editedRange;
    const NSInteger delta = ts.changeInLength;
    const NSUInteger oldLen = (NSUInteger)((NSInteger)er.length - delta);
    if (er.location + oldLen > _index.length() ||
        _index.length() + er.length - oldLen != ts.length) {
        [self reindex];
    } else {
        std::vector<unichar> buf(er.length);
        NSString *s = ts.string;
        if (er.length) [s getCharacters:buf.data() range:er];
        _index.edit(er.location, oldLen, reinterpret_cast<const char16_t *>(buf.data()),
                    buf.size(), er.location ? [s characterAtIndex:er.location - 1] : 0,
                    NSMaxRange(er) < s.length ? [s characterAtIndex:NSMaxRange(er)] : 0);
        [self updateThickness];
    }
    self.needsDisplay = YES;
}

- (CGFloat)digitWidth {
    return _font ? [@"8" sizeWithAttributes:@{NSFontAttributeName: _font}].width : 8;
}

// As the editor gutter measures itself: the lanes and three digits at least.
- (void)updateThickness {
    NSUInteger digits = 1;
    for (size_t n = _index.lineCount(); n >= 10; n /= 10) digits++;
    digits = MAX(digits, kMirrorMinDigits);
    const CGFloat width = ceil(_laneWidth + digits * [self digitWidth]);
    _digits = digits;
    if (self.ruleThickness != width) {
        // Never inside a storage edit: the scroll view tiles at once.
        dispatch_async(dispatch_get_main_queue(), ^{ self.ruleThickness = width; });
    }
}

- (void)setFont:(NSFont *)font {
    _font = font;
    [self updateThickness];
}
- (void)setLaneWidth:(CGFloat)w {
    _laneWidth = w;
    [self updateThickness];
}

- (void)drawHashMarksAndLabelsInRect:(NSRect)rect {}

- (void)drawRect:(NSRect)dirty {
    NSTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    if (!tv || !lm || !_font) return;
    if (_index.length() != tv.textStorage.length) [self reindex];
    const NSPoint origin = tv.textContainerOrigin;
    const NSRect inView = [self convertRect:dirty toView:tv];
    const NSRect inContainer = NSMakeRect(0, NSMinY(inView) - origin.y, tc.size.width,
                                          NSHeight(inView));
    const NSRange glyphs = [lm glyphRangeForBoundingRect:inContainer inTextContainer:tc];
    NSDictionary *attrs = @{NSFontAttributeName: _font,
                            NSForegroundColorAttributeName: _numberColor ?: NSColor.grayColor};
    const CGFloat right = NSWidth(self.bounds) - kMirrorFoldLane;
    const NSUInteger n = tv.textStorage.length;
    NSUInteger g = glyphs.location;
    while (g < NSMaxRange(glyphs)) {
        NSRange lineGlyphs;
        const NSRect frag = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:&lineGlyphs];
        if (lineGlyphs.length == 0) break;
        const NSUInteger c = [lm characterIndexForGlyphAtIndex:lineGlyphs.location];
        if (c <= n && _index.isLineStart(c)) {
            NSString *label = [NSString stringWithFormat:@"%zu", _index.lineOf(c) + 1];
            const CGFloat baseline = NSMinY(frag) + origin.y +
                                     [lm locationForGlyphAtIndex:lineGlyphs.location].y;
            const CGFloat y = [self convertPoint:NSMakePoint(0, baseline) fromView:tv].y;
            const CGFloat w = [label sizeWithAttributes:attrs].width;
            [label drawAtPoint:NSMakePoint(right - w, y - _font.ascender) withAttributes:attrs];
        }
        g = NSMaxRange(lineGlyphs);
    }
    // The empty last line after a final break.
    const NSRect extra = lm.extraLineFragmentRect;
    if (!NSIsEmptyRect(extra) && lm.extraLineFragmentTextContainer == tc) {
        NSString *label = [NSString stringWithFormat:@"%zu", _index.lineCount()];
        const CGFloat baseline = NSMinY(extra) + origin.y + _font.ascender;
        const CGFloat y = [self convertPoint:NSMakePoint(0, baseline) fromView:tv].y;
        const CGFloat w = [label sizeWithAttributes:attrs].width;
        [label drawAtPoint:NSMakePoint(right - w, y - _font.ascender) withAttributes:attrs];
    }
}
@end

// ---------------------------------------------------------------- the mirror
@implementation MCEditorMirror {
    NSTextStorage *_own;          // its own text, when not sharing
    MCGapLayoutManager *_layout;  // held here: a storage holds it only while on it
    NSTextContainer *_box;
    MCMirrorTextView *_tv;
    MCMirrorPictureView *_picture;
    MCMirrorGutter *_gutter;
    BOOL _lineNumbers;
}

- (instancetype)initInView:(NSView *)host {
    if ((self = [super init])) {
        const NSRect frame = NSMakeRect(0, 0, 400, 400);
        _scrollView = [[PanelScrollView alloc] initWithFrame:frame];
        _scrollView.hasVerticalScroller = YES;
        _scrollView.hasHorizontalScroller = YES;
        _scrollView.autohidesScrollers = YES;
        _scrollView.automaticallyAdjustsContentInsets = NO;
        _own = [NSTextStorage new];
        _layout = [MCGapLayoutManager new];
        // It reads no far positions: only what it shows is laid out.
        _layout.allowsNonContiguousLayout = YES;
        [_own addLayoutManager:_layout];
        _box = [[NSTextContainer alloc] initWithSize:NSMakeSize(NSWidth(frame), CGFLOAT_MAX)];
        [_layout addTextContainer:_box];
        _tv = [[MCMirrorTextView alloc] initWithFrame:frame textContainer:_box];
        _tv.editable = NO;
        _tv.selectable = NO;
        _tv.richText = YES;
        _tv.drawsBackground = NO;
        _tv.textContainerInset = NSMakeSize(8, 8);   // the editor's
        _tv.minSize = NSMakeSize(0, 0);
        _tv.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
        _tv.verticallyResizable = YES;
        _tv.autoresizingMask = NSViewWidthSizable;
        _tv.linkTextAttributes = @{};
        _scrollView.documentView = _tv;
        _gutter = [[MCMirrorGutter alloc] initWithTextView:_tv];
        _lineNumbers = YES;
        [self setWraps:YES];
        _picture = [[MCMirrorPictureView alloc] initWithFrame:frame];
        _picture.hidden = YES;
        __weak MCEditorMirror *weakSelf = self;
        _tv.onPress = ^(NSEvent *e) { [weakSelf pressed:e inPicture:NO]; };
        _picture.onPress = ^(NSEvent *e) { [weakSelf pressed:e inPicture:YES]; };
        _scrollView.hidden = YES;
        [host addSubview:_scrollView];
        [host addSubview:_picture];
    }
    return self;
}

- (NSTextView *)textView { return _tv; }
- (NSImageView *)pictureView { return _picture; }
- (BOOL)showsPicture { return _picture.image != nil; }

- (NSTextStorage *)sharedStorage {
    NSTextStorage *s = _layout.textStorage;
    return s == _own ? nil : s;
}

- (void)pressed:(NSEvent *)e inPicture:(BOOL)picture {
    NSUInteger c = NSNotFound;
    if (!picture) {
        const NSPoint p = [_tv convertPoint:e.locationInWindow fromView:nil];
        c = [_tv characterIndexForInsertionAtPoint:p];
        if (c > _tv.textStorage.length) c = _tv.textStorage.length;
    }
    if (self.onPress) self.onPress(e, c);
}

- (void)layoutInRect:(NSRect)rect hidden:(BOOL)hidden {
    const BOOL picture = _picture.image != nil;
    _scrollView.frame = rect;
    _picture.frame = rect;
    _scrollView.hidden = hidden || picture;
    _picture.hidden = hidden || !picture;
    _scrollView.rulersVisible = _lineNumbers && !picture;
}

- (void)setWraps:(BOOL)wraps {
    if (wraps) {
        _tv.horizontallyResizable = NO;
        _box.widthTracksTextView = YES;
        _box.size = NSMakeSize(NSWidth(_tv.frame), CGFLOAT_MAX);
    } else {
        _box.widthTracksTextView = NO;
        _box.size = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
        _tv.horizontallyResizable = YES;
    }
}

// The layout manager onto `storage`. Never replaceTextStorage: here, which
// would carry the editor's own layout manager along with it.
- (void)moveLayoutTo:(NSTextStorage *)storage {
    NSTextStorage *now = _layout.textStorage;
    if (now == storage) return;
    _tv.selectedRange = NSMakeRange(0, 0);
    [now removeLayoutManager:_layout];
    [storage addLayoutManager:_layout];
    [_gutter storageChanged];
    _tv.needsDisplay = YES;
}

- (void)forgetPicture {
    if (!_picture.image) return;
    _picture.image = nil;
    _picture.hidden = YES;
    _scrollView.hidden = NO;
    _scrollView.rulersVisible = _lineNumbers;
}

- (void)shareStorage:(NSTextStorage *)storage wraps:(BOOL)wraps {
    [self forgetPicture];
    [self setWraps:wraps];
    if (_own.length) [_own setAttributedString:[NSAttributedString new]];
    _layout.holdsPage = NO;
    _layout.allowsNonContiguousLayout = YES;
    [self moveLayoutTo:storage];
}

- (void)showText:(NSAttributedString *)text wraps:(BOOL)wraps {
    [self forgetPicture];
    [self setWraps:wraps];
    [self moveLayoutTo:_own];
    // A copy of a rendered page: its tables need layout from the top
    // (MCGapLayoutManager finds their rows by what is laid out).
    __block BOOL table = NO;
    [text enumerateAttribute:NSParagraphStyleAttributeName
                     inRange:NSMakeRange(0, text.length)
                     options:0
                  usingBlock:^(NSParagraphStyle *ps, NSRange, BOOL *stop) {
        if ([ps.textBlocks.lastObject isKindOfClass:NSTextTableBlock.class]) table = *stop = YES;
    }];
    _layout.holdsPage = YES;
    _layout.allowsNonContiguousLayout = !table;
    [_own setAttributedString:text ?: [NSAttributedString new]];
    [_gutter storageChanged];
}

- (void)keepCopyOfSharedText {
    NSTextStorage *shared = self.sharedStorage;
    if (!shared) return;
    CGFloat offset = 0;
    const NSUInteger top = [self topCharacter:&offset];
    NSAttributedString *copy = [[NSAttributedString alloc] initWithAttributedString:shared];
    [self moveLayoutTo:_own];
    [_own setAttributedString:copy];
    [_gutter storageChanged];
    [self scrollToCharacter:top offset:offset];
}

- (void)showPicture:(NSImage *)picture {
    [self moveLayoutTo:_own];
    if (_own.length) [_own setAttributedString:[NSAttributedString new]];
    _picture.image = picture;
    _picture.needsDisplay = YES;
}

- (void)clear {
    [self forgetPicture];
    [self moveLayoutTo:_own];
    if (_own.length) [_own setAttributedString:[NSAttributedString new]];
    [_gutter storageChanged];
}

- (NSUInteger)topCharacter:(CGFloat *)offset {
    if (offset) *offset = 0;
    const NSUInteger n = _tv.textStorage.length;
    if (n == 0 || _picture.image) return 0;
    const NSRect visible = _tv.visibleRect;
    NSUInteger top = [_tv characterIndexForInsertionAtPoint:
                          NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)];
    if (top >= n) top = n - 1;
    const NSRect r = [self rectOfCharacter:top];
    if (offset && !NSIsEmptyRect(r)) *offset = NSMinY(r) - NSMinY(visible);
    return top;
}

// A character's box in the text view's coordinates.
- (NSRect)rectOfCharacter:(NSUInteger)c {
    const NSUInteger n = _tv.textStorage.length;
    if (n == 0) return NSZeroRect;
    if (c >= n) c = n - 1;
    const NSRange g = [_layout glyphRangeForCharacterRange:NSMakeRange(c, 1)
                                      actualCharacterRange:NULL];
    [_layout ensureLayoutForGlyphRange:g];
    NSRect r = [_layout boundingRectForGlyphRange:g inTextContainer:_box];
    const NSPoint o = _tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

- (void)scrollToCharacter:(NSUInteger)c offset:(CGFloat)offset {
    if (_tv.textStorage.length == 0) {
        [_tv scrollPoint:NSZeroPoint];
        return;
    }
    const NSRect r = [self rectOfCharacter:c];
    [_tv scrollPoint:NSMakePoint(0, MAX(0, NSMinY(r) - offset))];
    _gutter.needsDisplay = YES;
}

- (NSRect)windowRectOfCharacter:(NSUInteger)c {
    if (c > _tv.textStorage.length) return NSZeroRect;
    return [_tv convertRect:[self rectOfCharacter:c] toView:nil];
}

- (void)applyBackground:(NSColor *)background gutterBackground:(NSColor *)gutterBackground
            numberColor:(NSColor *)numberColor font:(NSFont *)font
            lineNumbers:(BOOL)lineNumbers laneWidth:(CGFloat)laneWidth {
    _scrollView.panelColor = background;
    _gutter.layer.backgroundColor = gutterBackground.CGColor;
    _gutter.numberColor = numberColor;
    _gutter.laneWidth = laneWidth;
    _gutter.font = font;
    _lineNumbers = lineNumbers;
    _scrollView.rulersVisible = lineNumbers && !_picture.image;
    _gutter.needsDisplay = YES;
}
@end
