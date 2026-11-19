// PieceString.mm — see PieceString.h.
#import "PieceString.h"
#include "PieceTable.h"
#include <objc/runtime.h>
#include <algorithm>
#include <vector>

namespace {

PieceTable::Reader ReaderOf(NSString *original) {
    return [original](size_t start, size_t n, char16_t *out) {
        [original getCharacters:(unichar *)out range:NSMakeRange(start, n)];
    };
}

void CheckIndex(NSString *s, NSUInteger index, NSUInteger length) {
    if (index >= length)
        [NSException raise:NSRangeException format:@"-[%@ characterAtIndex:]: index %lu out of "
                                                   @"bounds; length %lu", s.class,
                                                   (unsigned long)index, (unsigned long)length];
}

void CheckRange(NSString *s, NSRange r, NSUInteger length) {
    if (r.location > length || r.length > length - r.location)
        [NSException raise:NSRangeException format:@"%@: range %@ out of bounds; length %lu",
                                                   s.class, NSStringFromRange(r),
                                                   (unsigned long)length];
}

}  // namespace

// ---------------------------------------------------------------- snapshot
@interface MCPieceSnapshot () {
@public
    NSString *_original;
    PieceTable _table;
    PieceTable::Reader _read;
}
- (instancetype)initWithOriginal:(NSString *)original table:(const PieceTable &)table;
@end

@implementation MCPieceSnapshot

- (instancetype)initWithOriginal:(NSString *)original table:(const PieceTable &)table {
    if ((self = [super init])) {
        _original = original;
        _table = table;
        _read = ReaderOf(original);
    }
    return self;
}

- (NSUInteger)length { return _table.length(); }

- (unichar)characterAtIndex:(NSUInteger)index {
    CheckIndex(self, index, _table.length());
    return _table.at(index, _read);
}

- (void)getCharacters:(unichar *)buffer range:(NSRange)range {
    CheckRange(self, range, _table.length());
    _table.read(range.location, range.length, (char16_t *)buffer, _read);
}

- (id)copyWithZone:(NSZone *)zone { return self; }
- (BOOL)isEqualToString:(NSString *)other { return MCSameText(self, other); }
- (BOOL)isEqual:(id)other {
    return [other isKindOfClass:NSString.class] && MCSameText(self, other);
}

@end

// ------------------------------------------------------------ the string
@implementation MCPieceString {
@public
    NSString *_original;
    PieceTable _table;
    PieceTable::Reader _read;
}

- (instancetype)init { return [self initWithText:@""]; }
- (instancetype)initWithCapacity:(NSUInteger)capacity { return [self initWithText:@""]; }

- (instancetype)initWithText:(NSString *)text {
    if ((self = [super init])) {
        if ([text isKindOfClass:MCPieceSnapshot.class]) {
            MCPieceSnapshot *s = (MCPieceSnapshot *)text;
            _original = s->_original;
            _table = s->_table;
        } else {
            _original = [text copy] ?: @"";
            _table = PieceTable(_original.length);
        }
        _read = ReaderOf(_original);
    }
    return self;
}

- (BOOL)isOriginal { return _table.isOriginal(); }
- (NSUInteger)pieceCount { return _table.pieceCount(); }

- (NSUInteger)length { return _table.length(); }

- (unichar)characterAtIndex:(NSUInteger)index {
    CheckIndex(self, index, _table.length());
    return _table.at(index, _read);
}

- (void)getCharacters:(unichar *)buffer range:(NSRange)range {
    CheckRange(self, range, _table.length());
    _table.read(range.location, range.length, (char16_t *)buffer, _read);
}

// A long string goes in a block at a time, never copied whole.
- (void)replaceCharactersInRange:(NSRange)range withString:(NSString *)str {
    CheckRange(self, range, _table.length());
    const NSUInteger n = str.length;
    const NSUInteger block = 1 << 16;
    std::vector<unichar> buf(MIN(n, block));
    NSUInteger done = MIN(n, block);
    if (done) [str getCharacters:buf.data() range:NSMakeRange(0, done)];
    _table.replace(range.location, range.length, (const char16_t *)buf.data(), done);
    while (done < n) {
        const NSUInteger k = MIN(block, n - done);
        [str getCharacters:buf.data() range:NSMakeRange(done, k)];
        _table.replace(range.location + done, 0, (const char16_t *)buf.data(), k);
        done += k;
    }
}

- (id)copyWithZone:(NSZone *)zone {
    if (_table.isOriginal()) return _original;
    return [[MCPieceSnapshot alloc] initWithOriginal:_original table:_table];
}

- (id)mutableCopyWithZone:(NSZone *)zone {
    return [[MCPieceString alloc] initWithText:[self copy]];
}

- (BOOL)isEqualToString:(NSString *)other { return MCSameText(self, other); }
- (BOOL)isEqual:(id)other {
    return [other isKindOfClass:NSString.class] && MCSameText(self, other);
}

@end

BOOL MCSameText(NSString *a, NSString *b) {
    if (a == b) return YES;
    if (!a || !b) return NO;
    const NSUInteger n = a.length;
    if (b.length != n) return NO;
    // A piece string against its own original: only what moved is read.
    for (int pass = 0; pass < 2; pass++) {
        NSString *x = pass ? b : a, *y = pass ? a : b;
        if ([x isKindOfClass:MCPieceString.class] && ((MCPieceString *)x)->_original == y)
            return ((MCPieceString *)x)->_table.sameAsOriginal(((MCPieceString *)x)->_read);
        if ([x isKindOfClass:MCPieceSnapshot.class] && ((MCPieceSnapshot *)x)->_original == y)
            return ((MCPieceSnapshot *)x)->_table.sameAsOriginal(((MCPieceSnapshot *)x)->_read);
    }
    // Two piece texts over the same original (a tab's text and the text it
    // last saved): only what they do not share is read.
    const PieceTable *ta = nullptr, *tb = nullptr;
    NSString *oa = nil, *ob = nil;
    const PieceTable::Reader *read = nullptr;
    for (int pass = 0; pass < 2; pass++) {
        NSString *x = pass ? b : a;
        const PieceTable *&t = pass ? tb : ta;
        __strong NSString *&o = pass ? ob : oa;
        if ([x isKindOfClass:MCPieceString.class]) {
            t = &((MCPieceString *)x)->_table;
            o = ((MCPieceString *)x)->_original;
            read = &((MCPieceString *)x)->_read;
        } else if ([x isKindOfClass:MCPieceSnapshot.class]) {
            t = &((MCPieceSnapshot *)x)->_table;
            o = ((MCPieceSnapshot *)x)->_original;
            read = &((MCPieceSnapshot *)x)->_read;
        }
    }
    if (ta && tb && oa == ob) return ta->sameAs(*tb, *read);
    const BOOL pieces = [a isKindOfClass:MCPieceString.class] ||
                        [a isKindOfClass:MCPieceSnapshot.class] ||
                        [b isKindOfClass:MCPieceString.class] ||
                        [b isKindOfClass:MCPieceSnapshot.class];
    if (!pieces) return CFStringCompare((CFStringRef)a, (CFStringRef)b, 0) == kCFCompareEqualTo;
    const NSUInteger block = 4096;
    unichar x[block], y[block];
    for (NSUInteger at = 0; at < n; at += block) {
        const NSUInteger k = MIN(block, n - at);
        [a getCharacters:x range:NSMakeRange(at, k)];
        [b getCharacters:y range:NSMakeRange(at, k)];
        if (memcmp(x, y, k * sizeof(unichar)) != 0) return NO;
    }
    return YES;
}

// ------------------------------------------------------ the attributed one
namespace {
struct Run {
    NSUInteger length;
    NSDictionary *attrs;
};
BOOL SameAttributes(NSDictionary *a, NSDictionary *b) {
    return a == b || [a isEqualToDictionary:b];
}
}  // namespace

@implementation MCPieceAttributedString {
    MCPieceString *_string;
    std::vector<Run> _runs;
    std::vector<NSUInteger> _at;   // where each run begins
}

- (instancetype)init { return [self initWithText:@"" attributes:@{}]; }

- (instancetype)initWithText:(NSString *)text attributes:(NSDictionary *)attributes {
    if ((self = [super init])) {
        _string = [[MCPieceString alloc] initWithText:text];
        if (_string.length) {
            _runs.push_back({_string.length, [attributes copy] ?: @{}});
            _at.push_back(0);
        }
    }
    return self;
}

- (NSUInteger)runCount { return _runs.size(); }
- (NSString *)string { return _string; }
- (NSUInteger)length { return _string.length; }

- (size_t)runAt:(NSUInteger)location {
    return (size_t)(std::upper_bound(_at.begin(), _at.end(), location) - _at.begin()) - 1;
}

- (NSDictionary *)attributesAtIndex:(NSUInteger)location effectiveRange:(NSRangePointer)range {
    CheckIndex(_string, location, _string.length);
    const size_t i = [self runAt:location];
    if (range) *range = NSMakeRange(_at[i], _runs[i].length);
    return _runs[i].attrs;
}

// Runs over [pos, pos + removed) of a text `total` long become one run of
// `inserted` characters with `attrs`, joined with equal neighbours.
- (void)spliceAt:(NSUInteger)pos removed:(NSUInteger)removed inserted:(NSUInteger)inserted
           attrs:(NSDictionary *)attrs total:(NSUInteger)total {
    const NSUInteger end = pos + removed;
    const size_t first = pos < total ? [self runAt:pos] : _runs.size();
    size_t last;
    Run mid[3];
    size_t count = 0;
    if (first < _runs.size() && pos > _at[first]) mid[count++] = {pos - _at[first], _runs[first].attrs};
    if (inserted) mid[count++] = {inserted, attrs};
    if (end < total) {
        const size_t j = [self runAt:end];
        mid[count++] = {_at[j] + _runs[j].length - end, _runs[j].attrs};
        last = j + 1;
    } else {
        last = _runs.size();
    }
    _runs.erase(_runs.begin() + (long)first, _runs.begin() + (long)last);
    _runs.insert(_runs.begin() + (long)first, mid, mid + count);
    const size_t from = first ? first - 1 : 0;
    size_t to = std::min(_runs.size(), first + count + 1);
    for (size_t k = from; k + 1 < to;) {
        if (SameAttributes(_runs[k].attrs, _runs[k + 1].attrs)) {
            _runs[k].length += _runs[k + 1].length;
            _runs.erase(_runs.begin() + (long)k + 1);
            to--;
        } else {
            k++;
        }
    }
    _at.resize(_runs.size());
    for (size_t k = from; k < _runs.size(); k++) _at[k] = k ? _at[k - 1] + _runs[k - 1].length : 0;
}

// New characters take the first replaced character's attributes; with none
// replaced the one's before, at the start the one's after (NSMutableAttributedString's rule).
- (void)replaceCharactersInRange:(NSRange)range withString:(NSString *)str {
    const NSUInteger total = _string.length;
    CheckRange(_string, range, total);
    NSDictionary *attrs = @{};
    if (range.length) attrs = _runs[[self runAt:range.location]].attrs;
    else if (range.location > 0) attrs = _runs[[self runAt:range.location - 1]].attrs;
    else if (total) attrs = _runs[0].attrs;
    [_string replaceCharactersInRange:range withString:str ?: @""];
    [self spliceAt:range.location removed:range.length inserted:str.length attrs:attrs total:total];
}

- (void)setAttributes:(NSDictionary<NSAttributedStringKey, id> *)attrs range:(NSRange)range {
    const NSUInteger total = _string.length;
    CheckRange(_string, range, total);
    if (!range.length) return;
    [self spliceAt:range.location removed:range.length inserted:range.length
             attrs:[attrs copy] ?: @{} total:total];
}

@end

// ---------------------------------------------------------------- held bytes
static char kHeldAsciiKey;

void MCSetHeldAsciiBytes(NSString *text, const char *bytes, NSUInteger length) {
    if (!text || !bytes) return;
    // Not freed here: the string's own deallocator unmaps them.
    NSData *d = [NSData dataWithBytesNoCopy:(void *)bytes length:length freeWhenDone:NO];
    objc_setAssociatedObject(text, &kHeldAsciiKey, d, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

const char *MCHeldAsciiBytes(NSString *text, NSUInteger *length) {
    *length = 0;
    if (!text) return nullptr;
    NSData *d = objc_getAssociatedObject(text, &kHeldAsciiKey);
    if (!d || d.length != text.length) return nullptr;
    *length = d.length;
    return (const char *)d.bytes;
}
