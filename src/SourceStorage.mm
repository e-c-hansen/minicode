// SourceStorage.mm — see SourceStorage.h.
#import "SourceStorage.h"
#include "InlayHints.h"
#include <algorithm>

namespace {

// Whether every character in `range` is printable ASCII, a tab or a line
// break: characters every font the editor uses has, so font substitution
// finds nothing to do in them.
BOOL OnlyPlainAscii(NSString *string, NSRange range) {
    unichar buf[4096];
    for (NSUInteger at = range.location, end = NSMaxRange(range); at < end;) {
        const NSUInteger n = MIN((NSUInteger)4096, end - at);
        [string getCharacters:buf range:NSMakeRange(at, n)];
        for (NSUInteger i = 0; i < n; i++) {
            const unichar c = buf[i];
            if ((c < 0x20 || c > 0x7E) && c != '\t' && c != '\n' && c != '\r') return NO;
        }
        at += n;
    }
    return YES;
}

// Whether a font has a glyph for every printable ASCII character (asked once
// per font).
BOOL CoversAscii(NSFont *font) {
    static NSMapTable<NSFont *, NSNumber *> *known;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ known = [NSMapTable strongToStrongObjectsMapTable]; });
    @synchronized(known) {
        NSNumber *answer = [known objectForKey:font];
        if (!answer) {
            UniChar chars[0x7F - 0x20];
            CGGlyph glyphs[0x7F - 0x20];
            for (int i = 0; i < 0x7F - 0x20; i++) chars[i] = (UniChar)(0x20 + i);
            answer = @(CTFontGetGlyphsForCharacters((__bridge CTFontRef)font, chars, glyphs,
                                                    0x7F - 0x20));
            [known setObject:answer forKey:font];
        }
        return answer.boolValue;
    }
}

}  // namespace

@implementation MCSourceStorage {
    NSMutableAttributedString *_store;
    std::vector<MCTextGap> _gaps;
    uint64_t _gapsGeneration;
}

- (instancetype)init {
    if ((self = [super init])) _store = [[NSMutableAttributedString alloc] init];
    return self;
}

- (instancetype)initWithString:(NSString *)str attributes:(NSDictionary *)attrs {
    if ((self = [super init]))
        _store = [[NSMutableAttributedString alloc] initWithString:str ?: @"" attributes:attrs];
    return self;
}

- (instancetype)initWithAttributedString:(NSAttributedString *)attr {
    if ((self = [super init]))
        _store = attr ? [attr mutableCopy] : [[NSMutableAttributedString alloc] init];
    return self;
}

- (BOOL)fixesAttributesLazily { return NO; }

// ---- the primitives
- (NSString *)string { return _store.string; }

// The stored attributes at `location` (over `run`) with the gaps' kern: a
// gap's character alone gets its kern added; a run without one stops short
// of the gaps either side.
- (NSDictionary *)withGaps:(NSDictionary *)attrs at:(NSUInteger)location run:(NSRange *)run {
    // The gaps on characters at or after this one; a gap at offset p is on
    // the character p - 1.
    auto it = std::lower_bound(_gaps.begin(), _gaps.end(), location + 1,
                               [](const MCTextGap &g, NSUInteger o) { return g.offset < o; });
    if (it != _gaps.end() && it->offset == location + 1) {
        CGFloat width = 0;
        for (auto k = it; k != _gaps.end() && k->offset == location + 1; ++k) width += k->width;
        NSMutableDictionary *m = [attrs mutableCopy];
        m[NSKernAttributeName] = @([attrs[NSKernAttributeName] doubleValue] + width);
        *run = NSMakeRange(location, 1);
        return m;
    }
    NSUInteger lo = run->location, hi = NSMaxRange(*run);
    if (it != _gaps.end() && it->offset - 1 < hi) hi = it->offset - 1;
    if (it != _gaps.begin() && (it - 1)->offset > lo) lo = (it - 1)->offset;
    *run = NSMakeRange(lo, hi - lo);
    return attrs;
}

- (NSDictionary<NSAttributedStringKey, id> *)attributesAtIndex:(NSUInteger)location
                                                effectiveRange:(NSRangePointer)range {
    if (_gaps.empty() || _gapsHidden) return [_store attributesAtIndex:location effectiveRange:range];
    NSRange run;
    NSDictionary *attrs = [self withGaps:[_store attributesAtIndex:location effectiveRange:&run]
                                      at:location run:&run];
    if (range) *range = run;
    return attrs;
}

// Copies (and NSTextView's undo, which keeps the attributed text it
// replaces) get the stored attributes, never a gap's kern.
- (NSAttributedString *)attributedSubstringFromRange:(NSRange)range {
    return [_store attributedSubstringFromRange:range];
}

- (std::vector<MCTextGap>)gaps { return _gaps; }
- (uint64_t)gapsGeneration { return _gapsGeneration; }

- (void)setGaps:(std::vector<MCTextGap>)gaps {
    std::stable_sort(gaps.begin(), gaps.end(),
                     [](const MCTextGap &a, const MCTextGap &b) { return a.offset < b.offset; });
    // The characters whose kern changes: a gap's width at each offset,
    // before and after.
    std::vector<std::pair<NSUInteger, CGFloat>> was, now;
    for (const MCTextGap &g : _gaps) was.push_back({g.offset, g.width});
    for (const MCTextGap &g : gaps) now.push_back({g.offset, g.width});
    auto sum = [](std::vector<std::pair<NSUInteger, CGFloat>> &v) {
        std::vector<std::pair<NSUInteger, CGFloat>> out;
        for (auto &p : v) {
            if (!out.empty() && out.back().first == p.first) out.back().second += p.second;
            else out.push_back(p);
        }
        v.swap(out);
    };
    sum(was);
    sum(now);
    std::vector<NSUInteger> changed;
    size_t i = 0, j = 0;
    while (i < was.size() || j < now.size()) {
        if (j == now.size() || (i < was.size() && was[i].first < now[j].first)) {
            changed.push_back(was[i++].first);
        } else if (i == was.size() || now[j].first < was[i].first) {
            changed.push_back(now[j++].first);
        } else {
            if (was[i].second != now[j].second) changed.push_back(was[i].first);
            i++;
            j++;
        }
    }
    _gaps = std::move(gaps);
    _gapsGeneration++;
    const NSUInteger length = _store.length;
    for (NSLayoutManager *lm in self.layoutManagers)
        for (NSUInteger p : changed)
            if (p > 0 && p <= length)
                [lm invalidateLayoutForCharacterRange:NSMakeRange(p - 1, 1) actualCharacterRange:NULL];
}

// The gaps follow an edit before anything lays the text out again.
- (void)moveGapsForEdit:(NSRange)range newLength:(NSUInteger)newLength {
    if (_gaps.empty()) return;
    bool moved = false;
    std::vector<MCTextGap> kept;
    kept.reserve(_gaps.size());
    for (MCTextGap g : _gaps) {
        const NSUInteger was = g.offset;
        size_t offset = g.offset;
        if (!Lsp::inlayFollowEdit(offset, g.followsBefore, range.location, range.length,
                                  newLength)) {
            moved = true;
            continue;
        }
        g.offset = offset;
        if (g.offset != was) moved = true;
        kept.push_back(g);
    }
    if (!moved) return;
    std::stable_sort(kept.begin(), kept.end(),
                     [](const MCTextGap &a, const MCTextGap &b) { return a.offset < b.offset; });
    _gaps.swap(kept);
    _gapsGeneration++;
}

- (void)replaceCharactersInRange:(NSRange)range withString:(NSString *)str {
    [_store replaceCharactersInRange:range withString:str];
    [self moveGapsForEdit:range newLength:str.length];
    if (_onReplaceCharacters) _onReplaceCharacters(range, str.length);
    [self edited:NSTextStorageEditedCharacters | NSTextStorageEditedAttributes range:range
        changeInLength:(NSInteger)str.length - (NSInteger)range.length];
}

- (void)setAttributes:(NSDictionary<NSAttributedStringKey, id> *)attrs range:(NSRange)range {
    [_store setAttributes:attrs range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

// ---- the rest, straight to the store (the generic versions go through
// the primitives a run at a time)
- (id)attribute:(NSAttributedStringKey)name atIndex:(NSUInteger)location
    effectiveRange:(NSRangePointer)range {
    // The typesetter asks for the kern by name: the gaps answer it.
    if (!_gaps.empty() && !_gapsHidden && [name isEqualToString:NSKernAttributeName])
        return [self attributesAtIndex:location effectiveRange:range][NSKernAttributeName];
    return [_store attribute:name atIndex:location effectiveRange:range];
}

// The typesetter reads attributes this way, so the gaps are here too.
- (NSDictionary<NSAttributedStringKey, id> *)attributesAtIndex:(NSUInteger)location
                                         longestEffectiveRange:(NSRangePointer)range
                                                       inRange:(NSRange)limit {
    if (_gaps.empty() || _gapsHidden)
        return [_store attributesAtIndex:location longestEffectiveRange:range inRange:limit];
    NSRange run;
    NSDictionary *attrs = [self withGaps:[_store attributesAtIndex:location
                                                 longestEffectiveRange:&run inRange:limit]
                                      at:location run:&run];
    if (range) *range = NSIntersectionRange(run, limit);
    return attrs;
}

- (NSDictionary<NSAttributedStringKey, id> *)storedAttributesAtIndex:(NSUInteger)location
                                               longestEffectiveRange:(NSRangePointer)range
                                                             inRange:(NSRange)limit {
    return [_store attributesAtIndex:location longestEffectiveRange:range inRange:limit];
}

- (id)attribute:(NSAttributedStringKey)name atIndex:(NSUInteger)location
    longestEffectiveRange:(NSRangePointer)range inRange:(NSRange)limit {
    if (!_gaps.empty() && !_gapsHidden && [name isEqualToString:NSKernAttributeName])
        return [self attributesAtIndex:location longestEffectiveRange:range inRange:limit][name];
    return [_store attribute:name atIndex:location longestEffectiveRange:range inRange:limit];
}

// AppKit's fixing also checks NSGlyphInfo runs against their characters,
// and does it by asking for the glyph info's longest effective range over
// the whole text, which for an attribute nothing has walks every run of
// the text (440,000 in a highlighted 100,000-line file), however small the
// range being fixed. The editor never sets glyph info, so that step runs
// only when some (pasted rich text) is in the range.
- (void)fixAttributesInRange:(NSRange)range {
    __block BOOL glyphInfo = NO;
    [_store enumerateAttribute:NSGlyphInfoAttributeName inRange:range options:0
                    usingBlock:^(id value, NSRange r, BOOL *stop) {
        (void)r;
        if (value) { glyphInfo = YES; *stop = YES; }
    }];
    if (glyphInfo) {
        [super fixAttributesInRange:range];
        return;
    }
    // Font substitution reads every character of the range for one its
    // font lacks: 1.2 s of putting a 25 MB file in, every time its tab is
    // shown. Plain ASCII under fonts that have all of it needs none.
    if (![self rangeNeedsNoFontFix:range]) [self fixFontAttributeInRange:range];
    [self fixParagraphStyleAttributeInRange:range];
    [self fixAttachmentAttributeInRange:range];
}

- (BOOL)rangeNeedsNoFontFix:(NSRange)range {
    if (range.length == 0) return YES;
    __block BOOL covered = YES;
    __block NSFont *last = nil;
    [_store enumerateAttribute:NSFontAttributeName inRange:range options:0
                    usingBlock:^(NSFont *font, NSRange r, BOOL *stop) {
        (void)r;
        if (font == last) return;
        if (!font || !CoversAscii(font)) { covered = NO; *stop = YES; }
        last = font;
    }];
    return covered && OnlyPlainAscii(_store.string, range);
}

- (NSUInteger)length { return _store.length; }

- (void)replaceCharactersInRange:(NSRange)range withAttributedString:(NSAttributedString *)attr {
    [_store replaceCharactersInRange:range withAttributedString:attr];
    [self moveGapsForEdit:range newLength:attr.length];
    if (_onReplaceCharacters) _onReplaceCharacters(range, attr.length);
    [self edited:NSTextStorageEditedCharacters | NSTextStorageEditedAttributes range:range
        changeInLength:(NSInteger)attr.length - (NSInteger)range.length];
}

- (void)addAttribute:(NSAttributedStringKey)name value:(id)value range:(NSRange)range {
    [_store addAttribute:name value:value range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

- (void)addAttributes:(NSDictionary<NSAttributedStringKey, id> *)attrs range:(NSRange)range {
    [_store addAttributes:attrs range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

- (void)removeAttribute:(NSAttributedStringKey)name range:(NSRange)range {
    [_store removeAttribute:name range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

@end
