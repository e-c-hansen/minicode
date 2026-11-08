// SourceStorage.h — Objective-C++. The editor's text storage.
//
// What a keystroke cost in a highlighted 100,000-line Python file (440,000
// attribute runs) was almost all AppKit fixing attributes:
//   - AppKit's own storage fixes them lazily (fixesAttributesLazily), in
//     blocks of 65,536 characters around wherever layout next reads, so
//     every keystroke re-fixed the whole block around the caret, twice
//     (the typed edit, then the highlighter's recolor);
//   - and fixing includes checking NSGlyphInfo runs, which asks for the
//     glyph info's longest effective range, a walk over every run of the
//     whole text for an attribute nothing has, however little is fixed.
// Together about 11 ms a keystroke. This storage fixes eagerly, so only
// the edited range (to its paragraph's ends) is fixed, in processEditing,
// and skips the glyph info step unless some is in the range (the editor
// never sets any; pasted rich text might): 0.6 ms a keystroke. The price
// is fixing the whole text when all of it is restyled (opening a file, a
// settings change): 195 ms instead of 96 ms to open that file.
//
// It keeps the characters and attributes in a plain NSMutableAttributedString
// and reports every change with edited:range:changeInLength:, as an
// NSTextStorage subclass must. Nothing else about the storage changes:
// delegates, undo, the layout manager and fonts for characters the editor's
// font lacks are as before.
//
// Gaps: room opened between two characters for something drawn there that
// is not text (an inlay hint's label, LspInlayHints.h). A gap at offset p
// is a kern on the character before it, answered by attributesAtIndex:
// and never stored, so the characters, their stored attributes, undo, the
// highlighter's runs and copies (attributedSubstringFromRange:) are as
// without it; only layout sees it. The storage keeps the gaps on their
// text through every edit itself (Lsp::inlayFollowEdit), at the moment of
// the edit, so layout never meets a gap on the wrong character.
#import <Cocoa/Cocoa.h>
#include <cstdint>
#include <vector>

struct MCTextGap {
    NSUInteger offset = 0;    // between the characters offset - 1 and offset
    CGFloat width = 0;        // points
    bool followsBefore = false;   // see Lsp::PlacedInlayHint
    uint32_t tag = 0;         // the owner's, to find what it draws there
};

@interface MCSourceStorage : NSTextStorage
// Ascending by offset, as they are now. Setting them lays out again only
// the characters whose gaps changed.
@property(nonatomic, assign) std::vector<MCTextGap> gaps;
// Counts gap changes: setting gaps, and edits that moved or dropped one.
@property(nonatomic, readonly) uint64_t gapsGeneration;
// While YES, attributesAtIndex: leaves the gaps out (writing the selection
// to a pasteboard, where a kern would come along in the rich text).
@property(nonatomic, assign) BOOL gapsHidden;
// The attributes as stored, with no gap's kern: what the highlighter
// compares its colors with, so a gap never makes it set them again.
- (NSDictionary<NSAttributedStringKey, id> *)storedAttributesAtIndex:(NSUInteger)location
                                               longestEffectiveRange:(NSRangePointer)range
                                                             inRange:(NSRange)limit;
// Told of every replacement of characters as it is made: `range` of the
// text as it was became `length` characters. Exact, where the editedRange
// of NSTextStorageDidProcessEditing is the union of an editing session's
// changes, attribute fixes included (a typed letter came as the rest of
// its paragraph once fonts were fixed there), which is too wide for
// marks that must tell a replaced range from one beside it (the snippet
// tab stops, SnippetSession.mm). Called inside the edit: arithmetic only,
// never layout.
@property(nonatomic, copy) void (^onReplaceCharacters)(NSRange range, NSUInteger length);
@end
