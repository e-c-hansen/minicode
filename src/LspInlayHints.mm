// LspInlayHints.mm — Objective-C++. See LspInlayHints.h.
#import "LspInlayHints.h"
#import "AppSettings.h"
#import "EditorFont.h"
#import "SourceStorage.h"
#include <algorithm>

@interface MCInlayHintItem : NSObject
@property(nonatomic, copy) NSString *label;
@property(nonatomic, assign) BOOL paddingLeft, paddingRight;
@property(nonatomic, assign) CGFloat labelWidth;
@end
@implementation MCInlayHintItem
@end

@implementation MCInlayHints {
    __weak NSTextView *_tv;
    NSMutableArray<MCInlayHintItem *> *_items;   // by the gaps' tags
    NSFont *_font;
    NSColor *_color, *_background;
    CGFloat _padding;   // VS Code's: a third of the font size, in whole points
}

- (instancetype)initWithTextView:(NSTextView *)textView {
    if (!(self = [super init])) return nil;
    _tv = textView;
    _items = [NSMutableArray array];
    [self takeSettings];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(styleChanged:)
               name:MCSettingsDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(styleChanged:)
               name:MCEditorZoomDidChangeNotification object:nil];
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (MCSourceStorage *)storage {
    NSTextStorage *ts = _tv.textStorage;
    return [ts isKindOfClass:[MCSourceStorage class]] ? (MCSourceStorage *)ts : nil;
}

// The hints' font is the editor's (VS Code's editor.inlayHints.fontSize 0
// and fontFamily "" mean the editor's own).
- (void)takeSettings {
    const Settings &st = [AppSettings shared].settings;
    _font = MCEditorFont();
    _color = MCColor(st.inlayHintColor());
    _background = MCColor(st.inlayHintBackground());
    _padding = floor(_font.pointSize / 3);
}

- (CGFloat)widthOfLabel:(NSString *)label {
    return ceil([label sizeWithAttributes:@{NSFontAttributeName: _font}].width);
}

- (CGFloat)gapWidthOf:(MCInlayHintItem *)item {
    return (item.paddingLeft ? _padding : 0) + item.labelWidth + (item.paddingRight ? _padding : 0);
}

// The font or the colors changed: the gaps take the labels' new widths.
- (void)styleChanged:(NSNotification *)note {
    (void)note;
    NSFont *was = _font;
    [self takeSettings];
    MCSourceStorage *ts = [self storage];
    if (!ts || ts.gaps.empty()) return;
    if (![was isEqual:_font]) {
        for (MCInlayHintItem *item in _items) item.labelWidth = [self widthOfLabel:item.label];
        std::vector<MCTextGap> gaps = ts.gaps;
        for (MCTextGap &g : gaps)
            if (g.tag < _items.count) g.width = [self gapWidthOf:_items[g.tag]];
        ts.gaps = gaps;
    }
    _tv.needsDisplay = YES;
}

- (void)showHints:(const std::vector<Lsp::InlayHint> &)hints
           placed:(const std::vector<Lsp::PlacedInlayHint> &)placed {
    MCSourceStorage *ts = [self storage];
    if (!ts) return;
    [_items removeAllObjects];
    std::vector<MCTextGap> gaps;
    gaps.reserve(placed.size());
    const NSUInteger length = ts.length;
    for (const Lsp::PlacedInlayHint &p : placed) {
        if (p.hint >= hints.size() || p.offset > length) continue;
        // At the start of an empty last line there is no character to make
        // room on.
        if ([ts characterOfGapAt:p.offset lineStart:NULL] >= length) continue;
        const Lsp::InlayHint &h = hints[p.hint];
        MCInlayHintItem *item = [MCInlayHintItem new];
        item.label = [[NSString alloc] initWithBytes:h.label.data() length:h.label.size()
                                            encoding:NSUTF8StringEncoding] ?: @"";
        item.paddingLeft = h.paddingLeft;
        item.paddingRight = h.paddingRight;
        item.labelWidth = [self widthOfLabel:item.label];
        MCTextGap g;
        g.offset = p.offset;
        g.width = [self gapWidthOf:item];
        g.followsBefore = p.followsBefore;
        g.tag = (uint32_t)_items.count;
        [_items addObject:item];
        gaps.push_back(g);
    }
    if (gaps.empty() && ts.gaps.empty()) return;
    ts.gaps = gaps;
    _tv.needsDisplay = YES;
}

- (void)clear {
    [_items removeAllObjects];
    MCSourceStorage *ts = [self storage];
    if (!ts || ts.gaps.empty()) return;
    ts.gaps = {};
    _tv.needsDisplay = YES;
}

- (NSUInteger)count {
    MCSourceStorage *ts = [self storage];
    return ts ? ts.gaps.size() : 0;
}

- (NSArray<NSDictionary *> *)shownHints {
    NSMutableArray *out = [NSMutableArray array];
    MCSourceStorage *ts = [self storage];
    if (!ts) return out;
    for (const MCTextGap &g : ts.gaps) {
        NSString *label = g.tag < _items.count ? _items[g.tag].label : @"";
        [out addObject:@{@"offset": @(g.offset), @"label": label}];
    }
    return out;
}

// ---- drawing
// Each gap's labels, left to right, for the gaps whose characters lie in
// `chars`: the block gets each label's box (view coordinates) and the
// baseline. Gaps at one offset share one kern, the labels side by side.
- (void)enumerateBoxesIn:(NSRange)chars
                   block:(void (^)(size_t gapIndex, MCInlayHintItem *item, NSRect box,
                                   CGFloat baseline))block {
    MCSourceStorage *ts = [self storage];
    if (!ts) return;
    const std::vector<MCTextGap> gaps = ts.gaps;
    if (gaps.empty()) return;
    NSLayoutManager *lm = _tv.layoutManager;
    NSTextContainer *tc = _tv.textContainer;
    if (!lm || !tc) return;
    NSString *text = ts.string;
    const NSUInteger length = text.length;
    const NSPoint origin = _tv.textContainerOrigin;
    const CGFloat ascender = _font.ascender, descender = _font.descender;
    // The first gap on a character at or after chars.location (a gap at a
    // line's start is on the character at its offset, others on the one
    // before).
    auto it = std::lower_bound(gaps.begin(), gaps.end(), chars.location,
                               [](const MCTextGap &g, NSUInteger o) { return g.offset < o; });
    while (it != gaps.end() && it->offset <= NSMaxRange(chars) + 1) {
        auto end = it;
        CGFloat total = 0;
        while (end != gaps.end() && end->offset == it->offset) total += (end++)->width;
        BOOL lineStart = NO;
        const NSUInteger k = [ts characterOfGapAt:it->offset lineStart:&lineStart];
        if (k < chars.location || k > NSMaxRange(chars) || k >= length) {
            it = end;
            continue;
        }
        const unichar c = [text characterAtIndex:k];
        const NSUInteger g = !lineStart && (c == '\n' || c == '\r')
                                 ? NSNotFound : [lm glyphIndexForCharacterAtIndex:k];
        // A glyph a fold hides (a null glyph) has no room drawn after it.
        if (g != NSNotFound && !([lm propertyForGlyphAtIndex:g] & NSGlyphPropertyNull)) {
            const NSRect frag = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:NULL];
            const NSPoint glyphAt = [lm locationForGlyphAtIndex:g];
            const CGFloat baseline = origin.y + frag.origin.y + glyphAt.y;
            // A kern is the glyph's advance past its own: the room ends
            // where the glyph's box does. A line-start gap is the first
            // line's indent: it ends where the line's first glyph starts.
            const CGFloat right =
                lineStart ? frag.origin.x + glyphAt.x
                          : NSMaxX([lm boundingRectForGlyphRange:NSMakeRange(g, 1)
                                                 inTextContainer:tc]);
            CGFloat x = origin.x + right - total;
            for (auto at = it; at != end; ++at) {
                MCInlayHintItem *item = at->tag < _items.count ? _items[at->tag] : nil;
                if (!item) { x += at->width; continue; }
                const CGFloat left = x + (item.paddingLeft ? _padding : 0);
                NSRect box = NSMakeRect(left, baseline - ascender, item.labelWidth,
                                        ascender - descender);
                block((size_t)(at - gaps.begin()), item, box, baseline);
                x += at->width;
            }
        }
        it = end;
    }
}

- (NSRect)rectOfHintAtIndex:(NSUInteger)index {
    MCSourceStorage *ts = [self storage];
    if (!ts || index >= ts.gaps.size()) return NSZeroRect;
    const NSUInteger k = [ts characterOfGapAt:ts.gaps[index].offset lineStart:NULL];
    __block NSRect found = NSZeroRect;
    [self enumerateBoxesIn:NSMakeRange(k, 0)
                     block:^(size_t gapIndex, MCInlayHintItem *item, NSRect box, CGFloat baseline) {
        (void)item;
        (void)baseline;
        if (gapIndex == index) found = box;
    }];
    return found;
}

- (void)drawInRect:(NSRect)dirty {
    MCSourceStorage *ts = [self storage];
    if (!ts || ts.gaps.empty() || !_tv.editable) return;
    NSLayoutManager *lm = _tv.layoutManager;
    const NSPoint origin = _tv.textContainerOrigin;
    // Only what is drawn: never a layout of text off screen.
    const NSRange glyphs = [lm glyphRangeForBoundingRect:NSOffsetRect(dirty, -origin.x, -origin.y)
                                         inTextContainer:_tv.textContainer];
    const NSRange chars = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    NSDictionary *attrs = @{NSFontAttributeName: _font, NSForegroundColorAttributeName: _color};
    NSColor *background = _background;
    [self enumerateBoxesIn:chars
                     block:^(size_t gapIndex, MCInlayHintItem *item, NSRect box, CGFloat baseline) {
        (void)gapIndex;
        (void)baseline;
        if (!NSIntersectsRect(box, dirty)) return;
        [background set];
        NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
        [item.label drawAtPoint:box.origin withAttributes:attrs];
    }];
}

@end
