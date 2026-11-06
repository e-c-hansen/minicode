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
#import <Cocoa/Cocoa.h>

@interface MCSourceStorage : NSTextStorage
@end
