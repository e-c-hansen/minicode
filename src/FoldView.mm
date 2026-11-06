// FoldView.mm — see FoldView.h.
#import "FoldView.h"
#import "EditorFont.h"
#import "MultiCaretView.h"
#include "Folding.h"
#include "LineIndex.h"
#include <algorithm>
#include <string>
#include <vector>

static dispatch_queue_t FoldQueue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        q = dispatch_queue_create("minicode.folding",
                                  dispatch_queue_attr_make_with_qos_class(
                                      DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
    return q;
}

// The text view's carets, whichever object holds them (MultiCaretView.h).
@protocol MCFoldCaretHost
@property(nonatomic, readonly) MCMultiCarets *multiCarets;
@property(nonatomic, readonly) BOOL multiCaretActive;
@end

@implementation MCFolds {
    __weak NSTextView *_tv;
    LineIndex _index;
    Fold::Collapsed _folds;
    std::vector<Fold::Span> _outer;      // _folds.outermost(), kept for the delegate
    std::vector<Fold::Range> _core;      // computed, for the text at _coreGen
    uint64_t _coreGen;
    BOOL _haveCore;
    std::vector<Fold::Range> _server;    // the language server's, at _serverGen
    uint64_t _serverGen;
    BOOL _haveServer;
    uint64_t _generation;
    NSFont *_pillFont;
    CGFloat _pillWidth;
}

- (instancetype)initWithTextView:(NSTextView *)tv {
    if ((self = [super init])) {
        _tv = tv;
        tv.layoutManager.delegate = self;
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification
                 object:tv.textStorage];
        [self reindex];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
}

- (BOOL)hasFolds { return !_folds.empty(); }

- (void)changed {
    if (_onChange) _onChange();
}

// ---- lines
- (void)reindex {
    NSString *s = _tv.textStorage.string;
    std::vector<unichar> buf(s.length);
    if (s.length) [s getCharacters:buf.data() range:NSMakeRange(0, s.length)];
    _index.reset(reinterpret_cast<const char16_t *>(buf.data()), buf.size());
}

- (void)checkIndex {
    if (_index.length() != _tv.textStorage.length) [self reindex];
}

// Where a line's text ends: before its "\n" or "\r\n".
- (NSUInteger)contentEndOfLine:(size_t)line {
    NSString *s = _tv.textStorage.string;
    const size_t a = _index.lineStart(line);
    size_t e = _index.lineEnd(line);
    if (e > a && [s characterAtIndex:e - 1] == '\n') {
        e--;
        if (e > a && [s characterAtIndex:e - 1] == '\r') e--;
    }
    return e;
}

- (Fold::Span)spanOf:(const Fold::Range &)r {
    const size_t n = _index.lineCount();
    if (n == 0 || r.start >= n) return {0, 0};
    const size_t end = std::min(r.end, n - 1);
    return Fold::spanOf([self contentEndOfLine:r.start], [self contentEndOfLine:end]);
}

// ---- following the text
- (void)setFileName:(NSString *)fileName {
    _fileName = [fileName copy];
    _haveCore = _haveServer = NO;
    _core.clear();
    _server.clear();
    if (!_folds.empty()) {
        std::vector<Fold::Span> was = _outer;
        _folds.clear();
        _outer.clear();
        for (const Fold::Span &s : was) [self invalidate:s];
    }
    [self checkIndex];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(computeRegions)
                                               object:nil];
    if (_fileName.length) [self performSelector:@selector(computeRegions) withObject:nil afterDelay:0];
    [self changed];
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters)) return;
    const NSRange er = ts.editedRange;
    const NSInteger delta = ts.changeInLength;
    const NSUInteger oldLen = (NSUInteger)((NSInteger)er.length - delta);
    std::vector<unichar> buf;
    const BOOL small = er.length < 1 << 20 || !_folds.empty();
    if (small) {
        buf.resize(er.length);
        if (er.length) [ts.string getCharacters:buf.data() range:er];
    }
    if (!small || er.location + oldLen > _index.length() ||
        _index.length() + er.length - oldLen != ts.length) {
        [self reindex];
    } else {
        _index.edit(er.location, oldLen, reinterpret_cast<const char16_t *>(buf.data()),
                    buf.size());
    }
    _generation++;
    if (!_folds.empty()) {
        std::vector<Fold::Span> opened;
        const unichar following =
            NSMaxRange(er) < ts.length ? [ts.string characterAtIndex:NSMaxRange(er)] : 0;
        _folds.edit(er.location, oldLen, reinterpret_cast<const char16_t *>(buf.data()),
                    buf.size(), following, &opened);
        _outer = _folds.outermost();
        if (!opened.empty()) {
            // Not from inside the storage's edit: the layout manager has not
            // taken it in yet.
            dispatch_async(dispatch_get_main_queue(), ^{
                for (const Fold::Span &s : opened) [self invalidate:s];
                [self changed];
            });
        }
    }
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(computeRegions)
                                               object:nil];
    if (_fileName.length)
        [self performSelector:@selector(computeRegions) withObject:nil afterDelay:0.3];
}

// ---- regions
- (void)computeRegions {
    NSTextView *tv = _tv;
    if (!tv || !_fileName.length) return;
    NSString *text = [tv.textStorage.string copy];
    const uint64_t gen = _generation;
    const std::string name = _fileName.UTF8String ?: "";
    NSString *file = _fileName;
    __weak MCFolds *weakSelf = self;
    dispatch_async(FoldQueue(), ^{
        std::u16string u(text.length, u'\0');
        if (text.length) [text getCharacters:(unichar *)u.data() range:NSMakeRange(0, text.length)];
        std::vector<Fold::Range> r = Fold::compute(u, name);
        dispatch_async(dispatch_get_main_queue(), ^{
            MCFolds *me = weakSelf;
            if (!me || gen != me->_generation || ![file isEqualToString:me->_fileName]) return;
            me->_core = std::move(r);
            me->_coreGen = gen;
            me->_haveCore = YES;
            [me changed];
        });
    });
}

- (void)setServerRanges:(NSArray<NSValue *> *)ranges kinds:(NSArray<NSString *> *)kinds
             generation:(uint64_t)generation {
    if (generation != _generation) return;
    std::vector<Fold::Range> rs;
    for (NSUInteger i = 0; i < ranges.count; i++) {
        const NSRange r = ranges[i].rangeValue;
        NSString *k = i < kinds.count ? kinds[i] : @"";
        rs.push_back({r.location, NSMaxRange(r), Fold::kindFromLsp(k.UTF8String ?: "")});
    }
    [self checkIndex];
    _server = Fold::sanitize(std::move(rs), _index.lineCount());
    _serverGen = generation;
    _haveServer = !_server.empty();
    [self changed];
}

// What the gutter shows: the latest regions, even from a moment ago.
- (const std::vector<Fold::Range> &)regions {
    if (_haveServer && _serverGen == _generation) return _server;
    return _core;
}

// What a command acts on: regions for the text as it is now.
- (const std::vector<Fold::Range> &)freshRegions {
    if (_haveServer && _serverGen == _generation) return _server;
    if (!_haveCore || _coreGen != _generation) {
        NSString *s = _tv.textStorage.string;
        std::u16string u(s.length, u'\0');
        if (s.length) [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
        _core = Fold::compute(u, _fileName.UTF8String ?: "");
        _coreGen = _generation;
        _haveCore = YES;
    }
    return _core;
}

- (BOOL)lineStartsRegion:(NSUInteger)line {
    return Fold::startingAt([self regions], line) >= 0;
}

- (const Fold::Span *)foldStartingOnLine:(NSUInteger)line {
    if (_folds.empty() || line >= _index.lineCount()) return nullptr;
    const size_t at = [self contentEndOfLine:line];
    const std::vector<Fold::Span> &all = _folds.all();   // sorted by start
    auto it = std::lower_bound(all.begin(), all.end(), at,
                               [](const Fold::Span &f, size_t p) { return f.start < p; });
    return it != all.end() && it->start == at ? &*it : nullptr;
}

- (BOOL)lineStartsFold:(NSUInteger)line {
    [self checkIndex];
    return [self foldStartingOnLine:line] != nullptr;
}

// ---- closing and opening
- (void)invalidate:(Fold::Span)s {
    NSTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    const NSUInteger n = tv.textStorage.length;
    const NSUInteger a = MIN(s.start, n), b = MIN(s.end + 1, n);
    if (b > a) {
        const NSRange r = NSMakeRange(a, b - a);
        [lm invalidateGlyphsForCharacterRange:r changeInLength:0 actualCharacterRange:NULL];
        [lm invalidateLayoutForCharacterRange:r actualCharacterRange:NULL];
    }
    tv.needsDisplay = YES;
}

- (BOOL)collapse:(const Fold::Range &)r {
    const Fold::Span s = [self spanOf:r];
    if (s.end <= s.start || !_folds.add(s)) return NO;
    _outer = _folds.outermost();
    [self invalidate:s];
    return YES;
}

// Many at once: one pass over the folds and one invalidation.
- (BOOL)collapseAll:(const std::vector<Fold::Range> &)rs {
    std::vector<Fold::Span> spans;
    spans.reserve(rs.size());
    for (const Fold::Range &r : rs) {
        const Fold::Span s = [self spanOf:r];
        if (s.end > s.start) spans.push_back(s);
    }
    if (spans.empty() || _folds.addAll(spans) == 0) return NO;
    _outer = _folds.outermost();
    size_t a = spans.front().start, b = 0;
    for (const Fold::Span &s : spans) { a = std::min(a, (size_t)s.start); b = std::max(b, (size_t)s.end); }
    [self invalidate:{a, b}];
    return YES;
}

- (BOOL)open:(Fold::Span)s {
    if (!_folds.remove(s.start)) return NO;
    _outer = _folds.outermost();
    [self invalidate:s];
    return YES;
}

// The carets (multi or single) as ranges, and putting them back.
- (NSArray<NSValue *> *)caretRanges {
    NSTextView *tv = _tv;
    if ([tv respondsToSelector:@selector(multiCaretActive)] &&
        [(id<MCFoldCaretHost>)tv multiCaretActive])
        return [(id<MCFoldCaretHost>)tv multiCarets].caretRanges;
    return tv.selectedRanges;
}

// Carets left inside what was just hidden go to the start of their fold.
- (void)moveCaretsOutOfFolds {
    NSTextView *tv = _tv;
    NSArray<NSValue *> *ranges = [self caretRanges];
    BOOL moved = NO;
    NSMutableArray *out = [NSMutableArray array];
    for (NSValue *v : ranges) {
        NSRange r = v.rangeValue;
        size_t a = r.location, b = NSMaxRange(r);
        auto out1 = [&](size_t p) {
            const Fold::Span *f = _folds.hiding(p);
            if (f && p > f->start) { moved = YES; return (size_t)f->start; }
            return p;
        };
        a = out1(a);
        b = out1(b);
        [out addObject:[NSValue valueWithRange:NSMakeRange(a, b >= a ? b - a : 0)]];
    }
    if (!moved) return;
    if ([tv respondsToSelector:@selector(multiCaretActive)] &&
        [(id<MCFoldCaretHost>)tv multiCaretActive])
        [[(id<MCFoldCaretHost>)tv multiCarets] setCaretRanges:out];
    else
        [tv setSelectedRanges:@[out.firstObject]];
}

- (NSArray<NSNumber *> *)caretLines {
    [self checkIndex];
    NSMutableArray *lines = [NSMutableArray array];
    for (NSValue *v in [self caretRanges])
        [lines addObject:@(_index.lineOf(v.rangeValue.location))];
    return lines;
}

- (BOOL)finish:(BOOL)did {
    if (did) {
        [self moveCaretsOutOfFolds];
        [self changed];
    }
    return did;
}

- (BOOL)foldInnermostAt:(NSUInteger)line {
    const std::vector<Fold::Range> &rs = [self freshRegions];
    for (size_t i : Fold::containing(rs, line))
        if (![self foldStartingOnLine:rs[i].start] && [self collapse:rs[i]]) return YES;
    return NO;
}

- (BOOL)foldAtCarets {
    if (!_fileName.length) return NO;
    BOOL did = NO;
    for (NSNumber *l in [self caretLines]) did |= [self foldInnermostAt:l.unsignedIntegerValue];
    return [self finish:did];
}

- (BOOL)unfoldAtCarets {
    BOOL did = NO;
    [self checkIndex];
    for (NSValue *v in [self caretRanges]) {
        const NSRange r = v.rangeValue;
        if (r.length > 0) {
            // Every fold that starts in the selection.
            std::vector<Fold::Span> in;
            for (const Fold::Span &f : _folds.all())
                if (f.start >= r.location && f.start <= NSMaxRange(r)) in.push_back(f);
            for (const Fold::Span &f : in) did |= [self open:f];
            continue;
        }
        if (const Fold::Span *f = [self foldStartingOnLine:_index.lineOf(r.location)])
            did |= [self open:*f];
    }
    if (did) [self changed];
    return did;
}

- (BOOL)toggleAtCarets {
    [self checkIndex];
    const NSUInteger line = _index.lineOf(_tv.selectedRange.location);
    return [self foldStartingOnLine:line] ? [self unfoldAtCarets] : [self foldAtCarets];
}

- (BOOL)toggleLine:(NSUInteger)line {
    [self checkIndex];
    if (const Fold::Span *f = [self foldStartingOnLine:line]) {
        BOOL did = [self open:*f];
        if (did) [self changed];
        return did;
    }
    if (!_fileName.length) return NO;
    const std::vector<Fold::Range> &rs = [self freshRegions];
    const long i = Fold::startingAt(rs, line);
    return i >= 0 && [self finish:[self collapse:rs[(size_t)i]]];
}

- (BOOL)foldRecursivelyAtCarets {
    if (!_fileName.length) return NO;
    BOOL did = NO;
    const std::vector<Fold::Range> rs = [self freshRegions];
    for (NSNumber *l in [self caretLines]) {
        std::vector<size_t> c = Fold::containing(rs, l.unsignedIntegerValue);
        if (c.empty()) continue;
        const Fold::Range top = rs[c.front()];   // the innermost, and all inside it
        std::vector<Fold::Range> pick;
        for (const Fold::Range &r : rs)
            if (r.start >= top.start && r.end <= top.end) pick.push_back(r);
        did |= [self collapseAll:pick];
    }
    return [self finish:did];
}

- (BOOL)unfoldRecursivelyAtCarets {
    BOOL did = NO;
    const std::vector<Fold::Range> rs = [self freshRegions];
    for (NSNumber *l in [self caretLines]) {
        std::vector<size_t> c = Fold::containing(rs, l.unsignedIntegerValue);
        size_t a, b;
        if (!c.empty()) {
            const Fold::Range top = rs[c.back()];   // the outermost
            a = _index.lineStart(top.start);
            b = _index.lineEnd(top.end);
        } else {
            a = _index.lineStart(l.unsignedIntegerValue);
            b = _index.lineEnd(l.unsignedIntegerValue);
        }
        std::vector<Fold::Span> in;
        for (const Fold::Span &f : _folds.all())
            if (f.start >= a && f.start <= b) in.push_back(f);
        for (const Fold::Span &f : in) did |= [self open:f];
    }
    if (did) [self changed];
    return did;
}

- (BOOL)foldAll {
    if (!_fileName.length) return NO;
    return [self finish:[self collapseAll:[self freshRegions]]];
}

- (BOOL)unfoldAll {
    if (_folds.empty()) return NO;
    const Fold::Span all = {_outer.front().start, _outer.back().end};
    _folds.clear();
    _outer.clear();
    [self invalidate:all];
    [self changed];
    return YES;
}

- (BOOL)foldLevel:(NSInteger)level {
    if (!_fileName.length || level < 1) return NO;
    const std::vector<Fold::Range> rs = [self freshRegions];
    const std::vector<int> d = Fold::depths(rs);
    NSArray<NSNumber *> *carets = [self caretLines];
    std::vector<Fold::Range> pick;
    for (size_t i = 0; i < rs.size(); i++) {
        if (d[i] != level) continue;
        BOOL holds = NO;
        for (NSNumber *l in carets)
            if (l.unsignedIntegerValue >= rs[i].start && l.unsignedIntegerValue <= rs[i].end)
                holds = YES;
        if (!holds) pick.push_back(rs[i]);
    }
    return [self finish:[self collapseAll:pick]];
}

- (BOOL)foldAllComments {
    if (!_fileName.length) return NO;
    std::vector<Fold::Range> pick;
    for (const Fold::Range &r : [self freshRegions])
        if (r.kind == Fold::Kind::Comment) pick.push_back(r);
    return [self finish:[self collapseAll:pick]];
}

// ---- revealing
- (void)revealRanges:(NSArray<NSValue *> *)ranges {
    if (_folds.empty()) return;
    std::vector<Fold::Span> open;
    for (NSValue *v in ranges) {
        const NSRange r = v.rangeValue;
        for (size_t p : {(size_t)r.location, (size_t)NSMaxRange(r)})
            for (const Fold::Span &f : _folds.around(p)) open.push_back(f);
    }
    if (open.empty()) return;
    if (_tv.textStorage.editedMask != 0) {
        // In the middle of an edit: the folds it touches open by themselves,
        // and anything else waits until the layout manager has the edit.
        NSArray *copy = [ranges copy];
        dispatch_async(dispatch_get_main_queue(), ^{ [self revealRanges:copy]; });
        return;
    }
    BOOL did = NO;
    for (const Fold::Span &f : open) did |= [self open:f];
    if (did) [self changed];
}

- (BOOL)rangeIsHidden:(NSRange)r {
    if (_outer.empty() || r.length == 0) return NO;
    const Fold::Span *f = _folds.hiding(r.location);
    return f && NSMaxRange(r) <= f->end;
}

- (BOOL)openFoldEndingAt:(NSUInteger)caret {
    for (const Fold::Span &f : _outer)
        if (f.end == caret) {
            const Fold::Span s = f;
            [self open:s];
            [self changed];
            return YES;
        }
    return NO;
}

- (BOOL)openFoldStartingAt:(NSUInteger)caret {
    for (const Fold::Span &f : _outer)
        if (f.start == caret) {
            const Fold::Span s = f;
            [self open:s];
            [self changed];
            return YES;
        }
    return NO;
}

// ---- tabs
- (NSArray<NSValue *> *)snapshot {
    NSMutableArray *out = [NSMutableArray array];
    for (const Fold::Span &f : _folds.all())
        [out addObject:[NSValue valueWithRange:NSMakeRange(f.start, f.end - f.start)]];
    return out;
}

- (void)restore:(NSArray<NSValue *> *)spans {
    NSString *s = _tv.textStorage.string;
    const NSUInteger n = s.length;
    auto lineBreak = [&](NSUInteger i) {
        const unichar c = [s characterAtIndex:i];
        return c == '\n' || c == '\r';
    };
    std::vector<Fold::Span> in;
    for (NSValue *v in spans) {
        const NSRange r = v.rangeValue;
        if (r.length == 0 || NSMaxRange(r) > n || !lineBreak(r.location)) continue;
        if (NSMaxRange(r) < n && !lineBreak(NSMaxRange(r))) continue;
        in.push_back({r.location, NSMaxRange(r)});
    }
    if (in.empty() || _folds.addAll(in) == 0) return;
    _outer = _folds.outermost();
    [self invalidate:{_outer.front().start, _outer.back().end}];
    [self changed];
}

// ---- drawing the placeholders
- (CGFloat)pillWidth {
    NSFont *font = MCEditorFont();
    if (font != _pillFont) {
        _pillFont = font;
        _pillWidth = ceil([@"000" sizeWithAttributes:@{NSFontAttributeName: font}].width) + 4;
    }
    return _pillWidth;
}

// The placeholder's box for a fold, in the view's coordinates.
- (NSRect)pillRectFor:(const Fold::Span &)f {
    NSTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    if (f.start >= tv.textStorage.length) return NSZeroRect;
    const NSUInteger g = [lm glyphIndexForCharacterAtIndex:f.start];
    NSRect line = [lm lineFragmentRectForGlyphAtIndex:g effectiveRange:NULL];
    const NSPoint at = [lm locationForGlyphAtIndex:g];
    NSFont *font = MCEditorFont();
    const CGFloat h = ceil(font.ascender - font.descender) + 2;
    const NSPoint o = tv.textContainerOrigin;
    const CGFloat base = NSMinY(line) + at.y;   // the baseline, in the container
    NSRect r = NSMakeRect(NSMinX(line) + at.x + 2, base - font.ascender - 1, [self pillWidth] - 4, h);
    return NSOffsetRect(r, o.x, o.y);
}

- (void)drawPlaceholdersInRect:(NSRect)dirty color:(NSColor *)color {
    if (_outer.empty()) return;
    NSTextView *tv = _tv;
    NSLayoutManager *lm = tv.layoutManager;
    const NSRange glyphs = [lm glyphRangeForBoundingRect:tv.visibleRect inTextContainer:tv.textContainer];
    const NSRange chars = [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
    NSColor *c = color ?: NSColor.grayColor;
    const CGFloat dot = MAX(1.5, round(MCEditorFont().pointSize * 0.19 * 2) / 2);
    auto it = std::lower_bound(_outer.begin(), _outer.end(), chars.location,
                               [](const Fold::Span &f, size_t at) { return f.end < at; });
    for (; it != _outer.end(); ++it) {
        const Fold::Span &f = *it;
        if (f.start > NSMaxRange(chars) + 1) break;
        const NSRect r = [self pillRectFor:f];
        if (NSIsEmptyRect(r) || !NSIntersectsRect(NSInsetRect(r, -2, -2), dirty)) continue;
        NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 0.5, 0.5)
                                                          xRadius:4 yRadius:4];
        [[c colorWithAlphaComponent:c.alphaComponent * 0.18] set];
        [p fill];
        [[c colorWithAlphaComponent:c.alphaComponent * 0.7] set];
        p.lineWidth = 1;
        [p stroke];
        // Three dots, drawn rather than typed: the font's "⋯" is too small.
        [c set];
        for (int k = -1; k <= 1; k++) {
            const NSPoint m = NSMakePoint(NSMidX(r) + k * dot * 2.6, NSMidY(r));
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(m.x - dot / 2, m.y - dot / 2, dot, dot)] fill];
        }
    }
}

- (BOOL)mouseDown:(NSEvent *)event {
    if (_outer.empty() || event.clickCount > 1) return NO;
    NSTextView *tv = _tv;
    const NSPoint p = [tv convertPoint:event.locationInWindow fromView:nil];
    for (const Fold::Span &f : _outer) {
        if (!NSPointInRect(p, NSInsetRect([self pillRectFor:f], -1, -1))) continue;
        const Fold::Span s = f;
        [self open:s];
        [tv setSelectedRange:NSMakeRange(s.start, 0)];
        [self changed];
        return YES;
    }
    return NO;
}

// ---- NSLayoutManagerDelegate: hiding
// The outer fold holding character i, or null; folds are sorted and apart,
// so their ends are in order too.
- (const Fold::Span *)outerHolding:(size_t)i {
    auto it = std::upper_bound(_outer.begin(), _outer.end(), i,
                               [](size_t at, const Fold::Span &f) { return at < f.end; });
    return it != _outer.end() && it->start <= i ? &*it : nullptr;
}

- (NSUInteger)layoutManager:(NSLayoutManager *)lm
       shouldGenerateGlyphs:(const CGGlyph *)glyphs
                 properties:(const NSGlyphProperty *)props
           characterIndexes:(const NSUInteger *)chars
                       font:(NSFont *)font
              forGlyphRange:(NSRange)range {
    if (_outer.empty() || range.length == 0) return 0;
    const size_t first = chars[0], last = chars[range.length - 1];
    auto it = std::upper_bound(_outer.begin(), _outer.end(), first,
                               [](size_t at, const Fold::Span &f) { return at < f.end; });
    if (it == _outer.end() || it->start > last) return 0;
    std::vector<NSGlyphProperty> p(props, props + range.length);
    for (NSUInteger i = 0; i < range.length; i++) {
        const size_t c = chars[i];
        while (it != _outer.end() && it->end <= c) ++it;
        if (it == _outer.end()) break;
        if (c < it->start) continue;
        p[i] = c == it->start ? NSGlyphPropertyControlCharacter : NSGlyphPropertyNull;
    }
    [lm setGlyphs:glyphs properties:p.data() characterIndexes:chars font:font forGlyphRange:range];
    return range.length;
}

- (NSControlCharacterAction)layoutManager:(NSLayoutManager *)lm
                          shouldUseAction:(NSControlCharacterAction)action
               forControlCharacterAtIndex:(NSUInteger)index {
    const Fold::Span *f = [self outerHolding:index];
    if (!f) return action;
    return f->start == index ? NSControlCharacterActionWhitespace
                             : NSControlCharacterActionZeroAdvancement;
}

- (NSRect)layoutManager:(NSLayoutManager *)lm
    boundingBoxForControlGlyphAtIndex:(NSUInteger)glyphIndex
                     forTextContainer:(NSTextContainer *)container
                 proposedLineFragment:(NSRect)fragment
                        glyphPosition:(NSPoint)position
                       characterIndex:(NSUInteger)index {
    return NSMakeRect(position.x, 0, [self pillWidth] + 2, NSHeight(fragment));
}
@end
