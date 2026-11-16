// EditorConflicts.mm — see EditorConflicts.h.
#import "EditorConflicts.h"
#import "FoldView.h"
#import "Lsp.h"
#include "MergeConflicts.h"
#include <string>
#include <vector>

using MergeConflicts::Block;
using MergeConflicts::Keep;

namespace {

// Past this many UTF-16 units the text is not read (large-file mode's
// threshold, LargeFile.h).
const NSUInteger kMaxConflictText = 20u * 1024 * 1024;

std::u16string U16(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

NSString *NS16(const std::u16string &s) {
    return [NSString stringWithCharacters:(const unichar *)s.data() length:s.size()];
}

NSString *NS8(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

Keep KeepOf(NSInteger k) { return k == 1 ? Keep::Incoming : k == 2 ? Keep::Both : Keep::Current; }

// A side's markers: the same color, more strongly (VS Code's header
// backgrounds are its content colors at two and a half times).
NSColor *Header(NSColor *c) {
    return [c colorWithAlphaComponent:MIN(1.0, MAX(0.25, c.alphaComponent * 2.5))];
}

NSDictionary *ButtonAttrs(NSColor *ink) {
    return @{NSFontAttributeName: [NSFont systemFontOfSize:11 weight:NSFontWeightMedium],
             NSForegroundColorAttributeName: ink ?: NSColor.textColor};
}

struct Button {
    Keep keep;
    size_t blockStart;   // which block, by where it starts
    NSRect rect;
};

}  // namespace

@implementation MCEditorConflicts {
    __weak CodeTextView *_tv;
    std::vector<Block> _blocks;
}

- (instancetype)initWithTextView:(CodeTextView *)tv {
    if ((self = [super init])) {
        _tv = tv;
        _currentColor = [NSColor colorWithSRGBRed:0x40 / 255.0 green:0xC8 / 255.0 blue:0xAE / 255.0
                                            alpha:0.2];
        _incomingColor = [NSColor colorWithSRGBRed:0x40 / 255.0 green:0xA6 / 255.0 blue:1.0
                                             alpha:0.2];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(storageEdited:)
                                                     name:NSTextStorageDidProcessEditingNotification
                                                   object:tv.textStorage];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
}

- (NSUInteger)count { return _blocks.size(); }

- (NSArray<NSString *> *)blockDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    NSString *s = _tv.string;
    for (const Block &b : _blocks) {
        auto lines = [&](size_t from, size_t to) {
            NSUInteger n = 0;
            for (size_t i = from; i < to && i < s.length; i++)
                if ([s characterAtIndex:i] == '\n') n++;
            return n;
        };
        [out addObject:[NSString stringWithFormat:@"current %@ %lu, incoming %@ %lu%@",
                        NS8(b.currentLabel), (unsigned long)lines(b.currentStart, b.currentEnd),
                        NS8(b.incomingLabel), (unsigned long)lines(b.incomingStart, b.incomingEnd),
                        b.hasBase() ? @", base" : @""]];
    }
    return out;
}

// ------------------------------------------------------------ reading
- (BOOL)reads {
    CodeTextView *tv = _tv;
    if (!tv || !tv.editable || tv.string.length > kMaxConflictText) return NO;
    return !self.wanted || self.wanted();
}

- (void)rescanSoon {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(rescanNow) object:nil];
    [self performSelector:@selector(rescanNow) withObject:nil afterDelay:0.2];
}

- (void)rescanNow {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(rescanNow) object:nil];
    std::vector<Block> found;
    NSString *s = _tv.string;
    if ([self reads] && [s rangeOfString:@"<<<<<<<" options:NSLiteralSearch].location != NSNotFound)
        found = MergeConflicts::parse(U16(s));
    bool same = found.size() == _blocks.size();
    for (size_t i = 0; same && i < found.size(); i++)
        same = found[i].start == _blocks[i].start && found[i].end == _blocks[i].end &&
               found[i].separator == _blocks[i].separator;
    _blocks.swap(found);
    if (!same) [_tv setNeedsDisplayInRect:_tv.visibleRect];
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSRange r = ts.editedRange;
    if (r.location == NSNotFound) return;
    const NSInteger delta = ts.changeInLength;
    const size_t removed = (size_t)MAX((NSInteger)0, (NSInteger)r.length - delta);
    const size_t before = _blocks.size();
    MergeConflicts::followEdit(_blocks, r.location, removed, r.length);
    if (_blocks.size() != before) [_tv setNeedsDisplayInRect:_tv.visibleRect];
    if (r.location == 0 && r.length == ts.length) {
        // The whole text replaced (another tab, a reload): read it as soon
        // as the edit is over, so the tints come with the text.
        __weak MCEditorConflicts *weakSelf = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf rescanNow]; });
    } else if (before > 0 || removed > 0 || [self mayMakeMarker:ts range:r]) {
        [self rescanSoon];
    }
}

// Whether typed or pasted text could finish a block in a file with none:
// only one holding a marker's character can (anything removed is read
// again too, to be safe).
- (BOOL)mayMakeMarker:(NSTextStorage *)ts range:(NSRange)r {
    if (r.length > 4096) return YES;
    static NSCharacterSet *marks;
    if (!marks) marks = [NSCharacterSet characterSetWithCharactersInString:@"<|=>"];
    return [ts.string rangeOfCharacterFromSet:marks options:NSLiteralSearch range:r].location !=
           NSNotFound;
}

// ------------------------------------------------------------ drawing
// The rect of each line fragment of characters [from, to), across the
// view, in view coordinates; nothing for lines hidden by a fold.
- (void)eachLineFrom:(size_t)from to:(size_t)to block:(void (^)(NSRect rect))block {
    CodeTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    if (!lm || to <= from) return;
    const NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(from, to - from)
                                      actualCharacterRange:NULL];
    if (glyphs.length == 0) return;
    const NSPoint origin = tv.textContainerOrigin;
    const CGFloat width = NSWidth(tv.bounds);
    [lm enumerateLineFragmentsForGlyphRange:glyphs
                                 usingBlock:^(NSRect rect, NSRect used, NSTextContainer *c,
                                              NSRange g, BOOL *stop) {
        (void)used; (void)c; (void)stop;
        const NSRange chars = [lm characterRangeForGlyphRange:g actualGlyphRange:NULL];
        if ([tv.folds rangeIsHidden:NSMakeRange(chars.location, 1)]) return;
        block(NSMakeRect(0, rect.origin.y + origin.y, width, NSHeight(rect)));
    }];
}

// The characters drawn in a rect, without laying out more.
- (NSRange)charactersInRect:(NSRect)rect {
    CodeTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    NSTextContainer *tc = tv.textContainer;
    if (!lm || !tc) return NSMakeRange(0, 0);
    NSRect r = NSOffsetRect(rect, -tv.textContainerOrigin.x, -tv.textContainerOrigin.y);
    const NSRange g = [lm glyphRangeForBoundingRectWithoutAdditionalLayout:r inTextContainer:tc];
    return [lm characterRangeForGlyphRange:g actualGlyphRange:NULL];
}

- (void)drawBehindInRect:(NSRect)dirty {
    if (_blocks.empty() || !_tv.editable) return;
    const NSRange shown = [self charactersInRect:dirty];
    NSColor *cur = self.currentColor, *inc = self.incomingColor;
    NSColor *base = [NSColor colorWithWhite:0x60 / 255.0 alpha:0x29 / 255.0];
    auto fill = [&](size_t from, size_t to, NSColor *color) {
        if (to <= from || to <= shown.location || from >= NSMaxRange(shown) + 1) return;
        [color set];
        [self eachLineFrom:from to:to block:^(NSRect line) {
            if (NSIntersectsRect(line, dirty)) NSRectFillUsingOperation(line, NSCompositingOperationSourceOver);
        }];
    };
    for (const Block &b : _blocks) {
        if (b.end <= shown.location || b.start > NSMaxRange(shown)) continue;
        fill(b.start, b.currentStart, Header(cur));
        fill(b.currentStart, b.currentEnd, cur);
        if (b.hasBase()) {
            fill(b.baseMarker, b.baseStart, Header(base));
            fill(b.baseStart, b.baseEnd, base);
        }
        fill(b.incomingStart, b.incomingEnd, inc);
        fill(b.endMarker, b.end, Header(inc));
    }
}

// The buttons of the blocks whose "<<<<<<<" line is in `rect`: after the
// line's text, or against the right edge of what shows when that is too
// far right.
- (std::vector<Button>)buttonsInRect:(NSRect)rect label:(NSMutableArray *)labels {
    std::vector<Button> out;
    CodeTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    if (_blocks.empty() || !tv.editable || !lm) return out;
    const NSRange shown = [self charactersInRect:rect];
    NSDictionary *attrs = ButtonAttrs(nil);
    for (size_t i = 0; i < _blocks.size(); i++) {
        const Block &b = _blocks[i];
        if (b.start < shown.location || b.start > NSMaxRange(shown)) continue;
        if ([tv.folds rangeIsHidden:NSMakeRange(b.start, 1)]) continue;
        const NSUInteger g = [lm glyphIndexForCharacterAtIndex:b.start];
        NSRange lineGlyphs;
        const NSRect frag = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:&lineGlyphs];
        const NSRect used = [lm lineFragmentUsedRectForGlyphAtIndex:g effectiveRange:NULL];
        const NSPoint origin = tv.textContainerOrigin;
        const CGFloat h = MAX(14, NSHeight(frag) - 2), gap = 6, padX = 8;
        const Keep keeps[3] = {Keep::Current, Keep::Incoming, Keep::Both};
        CGFloat widths[3], total = 0;
        for (int k = 0; k < 3; k++) {
            widths[k] = ceil([NS8(MergeConflicts::buttonTitle(keeps[k])) sizeWithAttributes:attrs].width) +
                        2 * padX;
            total += widths[k] + gap;
        }
        NSString *label = NS8(MergeConflicts::positionText(i, _blocks.size()));
        const CGFloat labelWidth = ceil([label sizeWithAttributes:attrs].width);
        CGFloat x = origin.x + NSMaxX(used) + 16;
        const CGFloat right = NSMaxX(tv.visibleRect) - 10;
        if (x + total + labelWidth > right) x = MAX(origin.x, right - total - labelWidth);
        const CGFloat y = origin.y + NSMinY(frag) + (NSHeight(frag) - h) / 2;
        for (int k = 0; k < 3; k++) {
            out.push_back({keeps[k], b.start, NSMakeRect(x, y, widths[k], h)});
            x += widths[k] + gap;
        }
        [labels addObject:@[label, [NSValue valueWithRect:NSMakeRect(x, y, labelWidth, h)]]];
    }
    return out;
}

- (void)drawOverInRect:(NSRect)dirty {
    if (_blocks.empty() || !_tv.editable) return;
    NSMutableArray *labels = [NSMutableArray array];
    const std::vector<Button> buttons = [self buttonsInRect:NSInsetRect(dirty, 0, -40) label:labels];
    if (buttons.empty()) return;
    NSColor *ink = _tv.textColor ?: NSColor.textColor;
    NSColor *bg = _tv.backgroundColor ?: NSColor.textBackgroundColor;
    for (const Button &b : buttons) {
        if (!NSIntersectsRect(b.rect, dirty)) continue;
        NSColor *fill = b.keep == Keep::Current ? Header(self.currentColor)
                      : b.keep == Keep::Incoming ? Header(self.incomingColor)
                      : [bg blendedColorWithFraction:0.3 ofColor:NSColor.grayColor];
        // A backing first, so the text under a pinned button does not show
        // through a translucent fill.
        [[bg colorWithAlphaComponent:1] set];
        [[NSBezierPath bezierPathWithRoundedRect:b.rect xRadius:4 yRadius:4] fill];
        [fill set];
        [[NSBezierPath bezierPathWithRoundedRect:b.rect xRadius:4 yRadius:4] fill];
        NSString *title = NS8(MergeConflicts::buttonTitle(b.keep));
        NSDictionary *attrs = ButtonAttrs(ink);
        const NSSize sz = [title sizeWithAttributes:attrs];
        [title drawAtPoint:NSMakePoint(NSMidX(b.rect) - sz.width / 2, NSMidY(b.rect) - sz.height / 2)
            withAttributes:attrs];
    }
    NSDictionary *muted = ButtonAttrs([ink colorWithAlphaComponent:0.6]);
    for (NSArray *l in labels) {
        const NSRect r = [l[1] rectValue];
        if (!NSIntersectsRect(r, dirty)) continue;
        const NSSize sz = [l[0] sizeWithAttributes:muted];
        [l[0] drawAtPoint:NSMakePoint(NSMinX(r), NSMidY(r) - sz.height / 2) withAttributes:muted];
    }
}

// ------------------------------------------------------------ acting
- (BOOL)mouseDown:(NSEvent *)event {
    if (_blocks.empty()) return NO;
    const NSEventModifierFlags mods =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
         NSEventModifierFlagControl);
    if (mods) return NO;
    return [self pressAtPoint:[_tv convertPoint:event.locationInWindow fromView:nil]];
}

- (BOOL)pressAtPoint:(NSPoint)p {
    for (const Button &b : [self buttonsInRect:NSMakeRect(0, p.y - 40, NSWidth(_tv.bounds), 80)
                                         label:nil])
        if (NSPointInRect(p, b.rect)) {
            [self keep:b.keep startingAt:b.blockStart];
            return YES;
        }
    return NO;
}

- (NSRect)buttonRectForKeep:(NSInteger)keep block:(NSUInteger)block {
    if (block >= _blocks.size()) return NSZeroRect;
    const size_t start = _blocks[block].start;
    for (const Button &b : [self buttonsInRect:_tv.visibleRect label:nil])
        if (b.blockStart == start && b.keep == KeepOf(keep)) return b.rect;
    return NSZeroRect;
}

- (BOOL)keep:(NSInteger)keep block:(NSUInteger)block {
    [self rescanNow];
    if (block >= _blocks.size()) {
        NSBeep();
        return NO;
    }
    return [self keep:KeepOf(keep) startingAt:_blocks[block].start];
}

// The block that starts at `start` (read again first, so a stale block is
// never replaced) resolved as one undo step.
- (BOOL)keep:(Keep)keep startingAt:(size_t)start {
    CodeTextView *tv = _tv;
    [self rescanNow];
    long index = -1;
    for (size_t i = 0; i < _blocks.size(); i++)
        if (_blocks[i].start == start) index = (long)i;
    if (!tv || !tv.editable || index < 0) {
        NSBeep();
        return NO;
    }
    const MergeConflicts::Splice s =
        MergeConflicts::resolve(U16(tv.string), _blocks[(size_t)index], keep);
    [tv breakUndoCoalescing];
    if (![tv replaceRanges:@[[NSValue valueWithRange:NSMakeRange(s.start, s.length)]]
               withStrings:@[NS16(s.text)]
                actionName:NS8(MergeConflicts::actionName(keep))]) {
        NSBeep();
        return NO;
    }
    [tv breakUndoCoalescing];
    [tv setSelectedRange:NSMakeRange(s.start, 0)];
    [tv scrollRangeToVisible:NSMakeRange(s.start, 0)];
    [self rescanNow];
    [tv setNeedsDisplayInRect:tv.visibleRect];
    if (self.onResolved) self.onResolved(_blocks.size());
    return YES;
}

- (BOOL)goToNext:(BOOL)forward {
    [self rescanNow];
    CodeTextView *tv = _tv;
    const long i = MergeConflicts::step(_blocks, tv.selectedRange.location, forward);
    if (i < 0 || !tv) return NO;
    const Block &b = _blocks[(size_t)i];
    [tv setSelectedRange:NSMakeRange(b.start, 0)];
    [tv scrollRangeToVisible:NSMakeRange(b.start, MIN(b.end, (size_t)tv.string.length) - b.start)];
    [tv scrollRangeToVisible:NSMakeRange(b.start, 0)];
    return YES;
}

@end
