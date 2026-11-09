// SourceStorage.mm — see SourceStorage.h.
#import "SourceStorage.h"
#include "InlayHints.h"
#include "MultiCaret.h"
#include <algorithm>
#include <objc/runtime.h>

static char kStoredStyleKey;

NSParagraphStyle *MCGapParagraphStyle(NSParagraphStyle *stored, CGFloat width) {
    NSParagraphStyle *base = stored ?: [NSParagraphStyle defaultParagraphStyle];
    NSMutableParagraphStyle *m = [base mutableCopy];
    m.firstLineHeadIndent = base.firstLineHeadIndent + width;
    // What the character really has (nil for none), for typing attributes.
    objc_setAssociatedObject(m, &kStoredStyleKey, stored ?: (id)[NSNull null],
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return m;
}

BOOL MCIsGapParagraphStyle(NSParagraphStyle *style, NSParagraphStyle **stored) {
    id was = style ? objc_getAssociatedObject(style, &kStoredStyleKey) : nil;
    if (!was) return NO;
    if (stored) *stored = was == [NSNull null] ? nil : (NSParagraphStyle *)was;
    return YES;
}

namespace {

// Where TextKit starts a paragraph (U+2028 only breaks a line within one).
bool IsParagraphBreak(unichar c) { return c == '\n' || c == '\r' || c == 0x2029; }

// The attributes a gap changes, which layout may ask for by name.
bool GapAttribute(NSAttributedStringKey name) {
    return [name isEqualToString:NSKernAttributeName] ||
           [name isEqualToString:NSParagraphStyleAttributeName];
}

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
    // The gaps' offsets that start a paragraph (line-start gaps), worked
    // out again only after the gaps or the characters changed.
    std::vector<NSUInteger> _lineStartGaps;
    uint64_t _charEdits, _lineStartsFor, _lineStartsForEdits;
    BOOL _lineStartsKnown;
    // A run of one stored paragraph style (SourceStorage.h): every
    // character in _styleRun has _styleValue. Once edits have moved it, not
    // always the longest such run, which an effective range need not be.
    NSRange _styleRun;
    id _styleValue;
    BOOL _styleRunKnown;
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

- (BOOL)paragraphStartsAt:(NSUInteger)offset {
    return offset == 0 || IsParagraphBreak([_store.string characterAtIndex:offset - 1]);
}

- (NSUInteger)characterOfGapAt:(NSUInteger)offset lineStart:(BOOL *)lineStart {
    const BOOL start = offset <= _store.length && [self paragraphStartsAt:offset];
    if (lineStart) *lineStart = start;
    return start ? offset : offset - 1;
}

// The stored attributes at `location` (over `run`) with the gaps: a gap's
// character alone gets its room added, as a kern (a gap after it) or as
// the paragraph's first-line indent (gaps at the start of the paragraph it
// starts); a run without one stops short of the gaps' characters either
// side.
- (NSDictionary *)withGaps:(NSDictionary *)attrs at:(NSUInteger)location run:(NSRange *)run {
    NSString *s = _store.string;
    const BOOL breakHere = location < s.length && IsParagraphBreak([s characterAtIndex:location]);
    const BOOL startsParagraph = [self paragraphStartsAt:location];
    // The gaps after this character, and those before it.
    auto it = std::lower_bound(_gaps.begin(), _gaps.end(), location + 1,
                               [](const MCTextGap &g, NSUInteger o) { return g.offset < o; });
    // A gap at p is on the character p - 1, unless p starts a paragraph:
    // a kern on the line break before it would open no room on its line.
    const BOOL kern = !breakHere && it != _gaps.end() && it->offset == location + 1;
    const BOOL indent = startsParagraph && it != _gaps.begin() && (it - 1)->offset == location;
    if (kern || indent) {
        NSMutableDictionary *m = [attrs mutableCopy];
        if (kern) {
            CGFloat width = 0;
            for (auto k = it; k != _gaps.end() && k->offset == location + 1; ++k) width += k->width;
            m[NSKernAttributeName] = @([attrs[NSKernAttributeName] doubleValue] + width);
        }
        if (indent) {
            CGFloat width = 0;
            for (auto k = it; k != _gaps.begin() && (k - 1)->offset == location; --k)
                width += (k - 1)->width;
            NSParagraphStyle *ps = attrs[NSParagraphStyleAttributeName];
            m[NSParagraphStyleAttributeName] = MCGapParagraphStyle(ps, width);
        }
        *run = NSMakeRange(location, 1);
        return m;
    }
    NSUInteger lo = run->location, hi = NSMaxRange(*run);
    if (it != _gaps.end()) {
        const NSUInteger c = [self characterOfGapAt:it->offset lineStart:NULL];
        if (c < hi) hi = c;
    }
    if (it != _gaps.begin()) {
        const NSUInteger c = [self characterOfGapAt:(it - 1)->offset lineStart:NULL];
        if (c + 1 > lo) lo = c + 1;
    }
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
        for (NSUInteger p : changed) {
            if (p > length) continue;
            const NSUInteger c = [self characterOfGapAt:p lineStart:NULL];
            if (c < length)
                [lm invalidateLayoutForCharacterRange:NSMakeRange(c, 1) actualCharacterRange:NULL];
        }
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

// ---- the paragraph style's run
static BOOL SameStyle(id a, id b) { return a == b || [a isEqual:b]; }

// The stored paragraph style at `location`, and characters around it that
// all have it.
- (id)storedParagraphStyleAt:(NSUInteger)location range:(NSRangePointer)range {
    if (location >= _store.length)   // the store's own answer, an exception
        return [_store attribute:NSParagraphStyleAttributeName atIndex:location effectiveRange:range];
    if (!_styleRunKnown || !NSLocationInRange(location, _styleRun)) {
        NSRange r;
        _styleValue = [_store attribute:NSParagraphStyleAttributeName atIndex:location
                  longestEffectiveRange:&r inRange:NSMakeRange(0, _store.length)];
        _styleRun = r;
        _styleRunKnown = YES;
    }
    if (range) *range = _styleRun;
    return _styleValue;
}

// Characters `range` are being replaced by `length` new ones, which have
// the run's paragraph style when `joins`.
- (void)styleRunReplaced:(NSRange)range length:(NSUInteger)length joins:(BOOL)joins {
    if (!_styleRunKnown) return;
    const NSUInteger a = _styleRun.location, b = NSMaxRange(_styleRun);
    const NSInteger delta = (NSInteger)length - (NSInteger)range.length;
    if (range.location >= a && NSMaxRange(range) <= b) {
        if (length == 0 || joins) {   // within the run: it grows or shrinks
            _styleRun.length = (NSUInteger)((NSInteger)_styleRun.length + delta);
            if (!_styleRun.length) _styleRunKnown = NO;
        } else if (!range.length && range.location == b) {
            // put in after it
        } else if (!range.length && range.location == a) {
            _styleRun.location += length;   // put in before it
        } else {
            _styleRunKnown = NO;
        }
    } else if (NSMaxRange(range) <= a) {
        _styleRun.location = (NSUInteger)((NSInteger)a + delta);
    } else if (range.location < b) {
        _styleRunKnown = NO;   // across an end
    }
}

// Plain characters put in for `range` take the first replaced character's
// attributes, with none replaced the one's before, at the start the one's
// after.
- (BOOL)plainTextJoinsStyleRun:(NSRange)range {
    if (!_styleRunKnown) return NO;
    const NSUInteger from = range.length || range.location == 0 ? range.location : range.location - 1;
    return NSLocationInRange(from, _styleRun);
}

// The paragraph style of `range` is being set to `style`.
- (void)styleRunRestyled:(NSRange)range style:(id)style {
    if (!_styleRunKnown || !range.length) return;
    if (NSMaxRange(range) <= _styleRun.location || range.location >= NSMaxRange(_styleRun)) return;
    if (!SameStyle(style, _styleValue)) _styleRunKnown = NO;
}

- (void)replaceCharactersInRange:(NSRange)range withString:(NSString *)str {
    [self styleRunReplaced:range length:str.length joins:[self plainTextJoinsStyleRun:range]];
    [_store replaceCharactersInRange:range withString:str];
    _charEdits++;
    [self moveGapsForEdit:range newLength:str.length];
    if (_onReplaceCharacters) _onReplaceCharacters(range, str.length);
    [self edited:NSTextStorageEditedCharacters | NSTextStorageEditedAttributes range:range
        changeInLength:(NSInteger)str.length - (NSInteger)range.length];
}

- (void)setAttributes:(NSDictionary<NSAttributedStringKey, id> *)attrs range:(NSRange)range {
    [self styleRunRestyled:range style:attrs[NSParagraphStyleAttributeName]];
    [_store setAttributes:attrs range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

// ---- the rest, straight to the store (the generic versions go through
// the primitives a run at a time)
- (id)attribute:(NSAttributedStringKey)name atIndex:(NSUInteger)location
    effectiveRange:(NSRangePointer)range {
    // The typesetter asks for the kern (and the paragraph style) by name:
    // the gaps answer them.
    if (!_gaps.empty() && !_gapsHidden && [name isEqualToString:NSParagraphStyleAttributeName])
        return [self paragraphStyleAt:location range:range limit:NSMakeRange(0, _store.length)
                              longest:NO];
    if (!_gaps.empty() && !_gapsHidden && GapAttribute(name))
        return [self attributesAtIndex:location effectiveRange:range][name];
    if ([name isEqualToString:NSParagraphStyleAttributeName])
        return [self storedParagraphStyleAt:location range:range];
    return [_store attribute:name atIndex:location effectiveRange:range];
}

- (const std::vector<NSUInteger> &)lineStartGaps {
    if (!_lineStartsKnown || _lineStartsFor != _gapsGeneration ||
        _lineStartsForEdits != _charEdits) {
        _lineStartGaps.clear();
        for (const MCTextGap &g : _gaps)
            if (g.offset < _store.length && [self paragraphStartsAt:g.offset] &&
                (_lineStartGaps.empty() || _lineStartGaps.back() != g.offset))
                _lineStartGaps.push_back(g.offset);
        _lineStartsFor = _gapsGeneration;
        _lineStartsForEdits = _charEdits;
        _lineStartsKnown = YES;
    }
    return _lineStartGaps;
}

// The paragraph style by name, which the typesetter asks for often: the
// stored one, its range stopping short of the line-start gaps, except on
// a line-start gap's own character.
- (id)paragraphStyleAt:(NSUInteger)location range:(NSRangePointer)range limit:(NSRange)limit
               longest:(BOOL)longest {
    const std::vector<NSUInteger> &starts = [self lineStartGaps];
    auto it = std::lower_bound(starts.begin(), starts.end(), location);
    if (it != starts.end() && *it == location) {
        NSDictionary *attrs = [self attributesAtIndex:location effectiveRange:NULL];
        if (range) *range = NSMakeRange(location, 1);
        return attrs[NSParagraphStyleAttributeName];
    }
    NSRange r;
    id value = longest ? [_store attribute:NSParagraphStyleAttributeName atIndex:location
                         longestEffectiveRange:&r inRange:limit]
                       : [self storedParagraphStyleAt:location range:&r];
    if (range) {
        NSUInteger lo = r.location, hi = NSMaxRange(r);
        if (it != starts.end() && *it < hi) hi = *it;
        if (it != starts.begin() && *(it - 1) + 1 > lo) lo = *(it - 1) + 1;
        *range = NSMakeRange(lo, hi - lo);
    }
    return value;
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
    if (!_gaps.empty() && !_gapsHidden && [name isEqualToString:NSParagraphStyleAttributeName])
        return [self paragraphStyleAt:location range:range limit:limit longest:YES];
    if (!_gaps.empty() && !_gapsHidden && GapAttribute(name))
        return [self attributesAtIndex:location longestEffectiveRange:range inRange:limit][name];
    return [_store attribute:name atIndex:location longestEffectiveRange:range inRange:limit];
}

// AppKit's fixing also checks NSGlyphInfo runs against their characters,
// and does it by asking for the glyph info's longest effective range over
// the whole text, which for an attribute nothing has walks every run of
// the text (440,000 in a highlighted 100,000-line file), however small the
// range being fixed. The editor never sets glyph info, so that step runs
// only when some (pasted rich text) is in the range.
//
// Fixing reads the attributes it fixes through this storage and writes what
// it read back over the paragraph (a paragraph's style is its first
// character's), so it sees them as stored: a line-start gap's indent read
// here would be stored over the whole paragraph.
- (void)fixAttributesInRange:(NSRange)range {
    const BOOL wasHidden = _gapsHidden;
    _gapsHidden = YES;
    __block BOOL glyphInfo = NO;
    [_store enumerateAttribute:NSGlyphInfoAttributeName inRange:range options:0
                    usingBlock:^(id value, NSRange r, BOOL *stop) {
        (void)r;
        if (value) { glyphInfo = YES; *stop = YES; }
    }];
    if (glyphInfo) {
        [super fixAttributesInRange:range];
    } else {
        // Font substitution reads every character of the range for one its
        // font lacks: 1.2 s of putting a 25 MB file in, every time its tab
        // is shown. Plain ASCII under fonts that have all of it needs none.
        if (![self rangeNeedsNoFontFix:range]) [self fixFontAttributeInRange:range];
        [self fixParagraphStyleAttributeInRange:range];
        [self fixAttachmentAttributeInRange:range];
    }
    _gapsHidden = wasHidden;
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
    if (_styleRunKnown) {
        // Whether every new character has the run's paragraph style.
        __block BOOL joins = YES;
        id style = _styleValue;
        [attr enumerateAttribute:NSParagraphStyleAttributeName inRange:NSMakeRange(0, attr.length)
                         options:0 usingBlock:^(id value, NSRange r, BOOL *stop) {
            (void)r;
            if (!SameStyle(value, style)) { joins = NO; *stop = YES; }
        }];
        [self styleRunReplaced:range length:attr.length joins:joins];
    }
    [_store replaceCharactersInRange:range withAttributedString:attr];
    _charEdits++;
    [self moveGapsForEdit:range newLength:attr.length];
    if (_onReplaceCharacters) _onReplaceCharacters(range, attr.length);
    [self edited:NSTextStorageEditedCharacters | NSTextStorageEditedAttributes range:range
        changeInLength:(NSInteger)attr.length - (NSInteger)range.length];
}

- (void)addAttribute:(NSAttributedStringKey)name value:(id)value range:(NSRange)range {
    if ([name isEqualToString:NSParagraphStyleAttributeName]) [self styleRunRestyled:range style:value];
    [_store addAttribute:name value:value range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

- (void)addAttributes:(NSDictionary<NSAttributedStringKey, id> *)attrs range:(NSRange)range {
    if (id style = attrs[NSParagraphStyleAttributeName]) [self styleRunRestyled:range style:style];
    [_store addAttributes:attrs range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

- (void)removeAttribute:(NSAttributedStringKey)name range:(NSRange)range {
    if ([name isEqualToString:NSParagraphStyleAttributeName]) [self styleRunRestyled:range style:nil];
    [_store removeAttribute:name range:range];
    [self edited:NSTextStorageEditedAttributes range:range changeInLength:0];
}

// ---- words
// Source text (the text view editable) goes by the core's word rule, VS
// Code's editor.wordSeparators; a read-only page (a Markdown preview) by
// AppKit's, which is made for prose.
- (BOOL)usesCodeWords {
    return self.layoutManagers.firstObject.firstTextView.isEditable;
}

- (MultiCaret::Text)wordText {
    NSString *s = _store.string;
    return MultiCaret::Text(s.length, [s](size_t start, size_t length, char16_t *out) {
        [s getCharacters:(unichar *)out range:NSMakeRange(start, length)];
    });
}

- (NSRange)doubleClickAtIndex:(NSUInteger)location {
    if (![self usesCodeWords]) return [super doubleClickAtIndex:location];
    const MultiCaret::Caret w = MultiCaret::wordSelection([self wordText], location);
    return NSMakeRange(w.lo(), w.hi() - w.lo());
}

- (NSUInteger)nextWordFromIndex:(NSUInteger)location forward:(BOOL)forward {
    if (![self usesCodeWords]) return [super nextWordFromIndex:location forward:forward];
    const MultiCaret::Caret c{location, location};
    return MultiCaret::move([self wordText], {c},
                            forward ? MultiCaret::Move::WordRight : MultiCaret::Move::WordLeft,
                            false)[0].head;
}

@end
