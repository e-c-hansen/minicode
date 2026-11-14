// LspInlayHints.mm — Objective-C++. See LspInlayHints.h.
#import "LspInlayHints.h"
#import "AppSettings.h"
#import "EditorFont.h"
#import "SourceStorage.h"
#include <algorithm>

@interface MCInlayHintItem : NSObject {
@public
    Lsp::InlayHint hint;   // as the server sent it (its edits, for accepting)
}
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
        item->hint = h;
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
    [self caretMayHaveMoved];
}

- (void)clear {
    [_items removeAllObjects];
    MCSourceStorage *ts = [self storage];
    if (!ts || ts.gaps.empty()) return;
    ts.gaps = {};
    _tv.needsDisplay = YES;
    [self caretMayHaveMoved];
}

- (void)removeHintAtIndex:(NSUInteger)index {
    MCSourceStorage *ts = [self storage];
    if (!ts || index >= ts.gaps.size()) return;
    std::vector<MCTextGap> gaps = ts.gaps;
    gaps.erase(gaps.begin() + (long)index);
    ts.gaps = gaps;   // the items stay, by tag, for the gaps left
    _tv.needsDisplay = YES;
    [self caretMayHaveMoved];
}

- (const Lsp::InlayHint *)hintAtIndex:(NSUInteger)index {
    MCSourceStorage *ts = [self storage];
    if (!ts || index >= ts.gaps.size()) return nullptr;
    const uint32_t tag = ts.gaps[index].tag;
    return tag < _items.count ? &_items[tag]->hint : nullptr;
}

// The gaps moved under the caret: the text view places it again (the
// caret beside a hint that follows the word before it is drawn before it).
- (void)caretMayHaveMoved {
    NSTextView *tv = _tv;
    if (tv.window && tv.selectedRange.length == 0) [tv updateInsertionPointStateAndRestartTimer:NO];
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
    [self enumerateRoomsIn:chars
                     block:^(size_t gapIndex, MCInlayHintItem *item, NSRect box, NSRect slot,
                             NSRect room, NSUInteger offset, CGFloat baseline) {
        (void)slot;
        (void)room;
        (void)offset;
        if (item) block(gapIndex, item, box, baseline);
    }];
}

// The same, with each gap's slot (its label and padding, the line's
// height) and the room all the gaps at its offset open, in view
// coordinates, and the offset. `item` is nil for a gap without one.
- (void)enumerateRoomsIn:(NSRange)chars
                   block:(void (^)(size_t gapIndex, MCInlayHintItem *item, NSRect box, NSRect slot,
                                   NSRect room, NSUInteger offset, CGFloat baseline))block {
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
            const NSRect room = NSMakeRect(x, origin.y + NSMinY(frag), total, NSHeight(frag));
            for (auto at = it; at != end; ++at) {
                MCInlayHintItem *item = at->tag < _items.count ? _items[at->tag] : nil;
                const NSRect slot = NSMakeRect(x, NSMinY(room), at->width, NSHeight(room));
                NSRect box = NSZeroRect;
                if (item)
                    box = NSMakeRect(x + (item.paddingLeft ? _padding : 0), baseline - ascender,
                                     item.labelWidth, ascender - descender);
                block((size_t)(at - gaps.begin()), item, box, slot, room, it->offset, baseline);
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

// ---- the pointer and the caret
// The characters near a point, for the gaps whose room may hold it.
- (NSRange)charactersNearPoint:(NSPoint)point {
    NSLayoutManager *lm = _tv.layoutManager;
    NSTextContainer *tc = _tv.textContainer;
    MCSourceStorage *ts = [self storage];
    if (!lm || !tc || !ts || ts.length == 0) return NSMakeRange(NSNotFound, 0);
    const NSPoint origin = _tv.textContainerOrigin;
    const NSUInteger g = [lm glyphIndexForPoint:NSMakePoint(point.x - origin.x, point.y - origin.y)
                                inTextContainer:tc];
    const NSUInteger k = [lm characterIndexForGlyphAtIndex:g];
    const NSUInteger from = k > 0 ? k - 1 : 0;
    return NSMakeRange(from, MIN(k + 1, ts.length - 1) - from + 1);
}

- (NSUInteger)hintIndexAtPoint:(NSPoint)point {
    MCSourceStorage *ts = [self storage];
    if (!ts || ts.gaps.empty() || !_tv.editable) return NSNotFound;
    const NSRange near = [self charactersNearPoint:point];
    if (near.location == NSNotFound) return NSNotFound;
    __block NSUInteger found = NSNotFound;
    [self enumerateRoomsIn:near
                     block:^(size_t gapIndex, MCInlayHintItem *item, NSRect box, NSRect slot,
                             NSRect room, NSUInteger offset, CGFloat baseline) {
        (void)box;
        (void)room;
        (void)offset;
        (void)baseline;
        if (item && found == NSNotFound && NSPointInRect(point, slot)) found = gapIndex;
    }];
    return found;
}

- (CGFloat)caretShiftAtOffset:(NSUInteger)offset {
    MCSourceStorage *ts = [self storage];
    if (!ts || !_tv.editable) return 0;
    const std::vector<MCTextGap> &gaps = ts.gaps;
    auto it = std::lower_bound(gaps.begin(), gaps.end(), offset,
                               [](const MCTextGap &g, NSUInteger o) { return g.offset < o; });
    CGFloat shift = 0;
    for (; it != gaps.end() && it->offset == offset; ++it)
        if (it->followsBefore) shift += it->width;
    if (shift <= 0) return 0;
    // Only where the room is laid out (not on a folded line).
    BOOL lineStart = NO;
    const NSUInteger k = [ts characterOfGapAt:offset lineStart:&lineStart];
    if (lineStart || k >= ts.length) return 0;
    NSLayoutManager *lm = _tv.layoutManager;
    const NSUInteger g = [lm glyphIndexForCharacterAtIndex:k];
    if ([lm propertyForGlyphAtIndex:g] & NSGlyphPropertyNull) return 0;
    return shift;
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
