// SourceStorage.mm — see SourceStorage.h.
#import "SourceStorage.h"

@implementation MCSourceStorage {
    NSMutableAttributedString *_store;
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

- (NSDictionary<NSAttributedStringKey, id> *)attributesAtIndex:(NSUInteger)location
                                                effectiveRange:(NSRangePointer)range {
    return [_store attributesAtIndex:location effectiveRange:range];
}

- (void)replaceCharactersInRange:(NSRange)range withString:(NSString *)str {
    [_store replaceCharactersInRange:range withString:str];
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
    return [_store attribute:name atIndex:location effectiveRange:range];
}

- (NSDictionary<NSAttributedStringKey, id> *)attributesAtIndex:(NSUInteger)location
                                         longestEffectiveRange:(NSRangePointer)range
                                                       inRange:(NSRange)limit {
    return [_store attributesAtIndex:location longestEffectiveRange:range inRange:limit];
}

- (id)attribute:(NSAttributedStringKey)name atIndex:(NSUInteger)location
    longestEffectiveRange:(NSRangePointer)range inRange:(NSRange)limit {
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
    [self fixFontAttributeInRange:range];
    [self fixParagraphStyleAttributeInRange:range];
    [self fixAttachmentAttributeInRange:range];
}

- (NSUInteger)length { return _store.length; }

- (void)replaceCharactersInRange:(NSRange)range withAttributedString:(NSAttributedString *)attr {
    [_store replaceCharactersInRange:range withAttributedString:attr];
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
