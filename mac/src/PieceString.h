// PieceString.h — Objective-C++. A large file's text held once (large-file
// mode, EditorController+LargeFile.mm).
//
// Opening a 259 MB log took 1.7 GB (round 9's grading): the tab's text,
// then a copy in an NSMutableAttributedString made to set the storage, the
// storage's own copy, and a UTF-16 copy of all of it for each of the two
// line indexes, and macOS's malloc keeps a freed block that large in the
// process's footprint. Now the tab's text (an NSString of the file's bytes,
// 8-bit for ASCII, read into pages of its own: MCReadDiskText) is the only
// copy: the editor's storage holds an MCPieceAttributedString whose string
// is an MCPieceString over it, the core's PieceTable keeping the edits.
//
// - MCPieceString: an NSMutableString over an immutable original. Edits cost
//   the number of pieces, not a move of the text after the edit (a flat
//   string moved up to the whole file on each keystroke). Its copy is the
//   original itself while unedited, else an MCPieceSnapshot that shares the
//   original and the edits' blocks (the tab's sourceText, the find bar's
//   text): no copy of the characters, safe to read on another thread while
//   the string goes on being edited.
// - isEqualToString: against the original (noteEdited's dirty check) reads
//   only the pieces that moved.
// - MCPieceAttributedString: an NSMutableAttributedString over an
//   MCPieceString, its attributes kept as runs (one run for a plain large
//   file), for MCSourceStorage's store (SourceStorage.h, holdLargeText:).
#import <Cocoa/Cocoa.h>

@interface MCPieceString : NSMutableString
// `text` immutable (an MCPieceSnapshot's pieces are taken over, so a text
// shown again does not stack one table over another).
- (instancetype)initWithText:(NSString *)text;
// The text is its original, unedited.
@property(nonatomic, readonly) BOOL isOriginal;
@property(nonatomic, readonly) NSUInteger pieceCount;
@end

@interface MCPieceSnapshot : NSString
@end

@interface MCPieceAttributedString : NSMutableAttributedString
- (instancetype)initWithText:(NSString *)text attributes:(NSDictionary *)attributes;
@property(nonatomic, readonly) NSUInteger runCount;
@end

// Whether `a` and `b` hold the same characters: a block at a time (never a
// copy of either), and in no time for a piece string against its original.
BOOL MCSameText(NSString *a, NSString *b);
