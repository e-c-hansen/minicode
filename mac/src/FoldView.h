// FoldView.h — Objective-C++. Code folding in the editor's text view: the
// regions a file can fold (Folding.h, or the language server's), which are
// folded, and hiding them without touching the text.
//
// The text is never changed. MCFolds is the layout manager's delegate (the
// editor is TextKit 1): the characters a fold hides get null glyphs, except
// the line break that ends its first line, which becomes a blank as wide as
// the placeholder the view draws there (a rounded "⋯"). So the hidden lines
// take no line fragments, the gutter skips their numbers by itself, and
// editing, undo, the highlighter, copying and the carets all see the whole
// text as before. A fold that an edit touches opens (Fold::Collapsed); one
// that a selection or a caret lands inside opens too, which is how Find, Go
// to Line, go to definition and F8 reveal what they go to.
//
// Regions are worked out off the main thread a moment after typing stops,
// counted by `generation` (one per edit to the characters), and a language
// server's ranges are used instead while they are for the text as it is.
#import <Cocoa/Cocoa.h>

@interface MCFolds : NSObject <NSLayoutManagerDelegate>
- (instancetype)initWithTextView:(NSTextView *)textView;

// The file shown (its name picks the way regions are found), or nil when the
// view holds no source; setting it forgets every fold and region.
@property(nonatomic, copy) NSString *fileName;
// Counts edits to the characters.
@property(nonatomic, readonly) uint64_t generation;
// The language server's ranges for the text at `generation` (0-based
// lines, as [start, end] pairs in NSValue ranges: location = start line,
// length = end - start); they replace the computed ones while the text
// stays as it was.
- (void)setServerRanges:(NSArray<NSValue *> *)ranges kinds:(NSArray<NSString *> *)kinds
             generation:(uint64_t)generation;
// Called when regions or folds change (the gutter redraws).
@property(nonatomic, copy) void (^onChange)(void);

// ---- what the gutter needs, by 0-based line
- (BOOL)lineStartsRegion:(NSUInteger)line;
- (BOOL)lineStartsFold:(NSUInteger)line;          // a fold that is closed
// The chevron's click: folds or unfolds the region starting on the line.
- (BOOL)toggleLine:(NSUInteger)line;

// ---- commands, at every caret (NO and a beep from the caller when nothing
// was there to do)
- (BOOL)foldAtCarets;           // the innermost open region holding the caret
- (BOOL)unfoldAtCarets;         // the fold starting on the caret's line
- (BOOL)toggleAtCarets;
- (BOOL)foldRecursivelyAtCarets;
- (BOOL)unfoldRecursivelyAtCarets;
- (BOOL)foldAll;
- (BOOL)unfoldAll;
- (BOOL)foldLevel:(NSInteger)level;   // every region that deep, but those holding a caret
- (BOOL)foldAllComments;

// ---- the view's side
@property(nonatomic, readonly) BOOL hasFolds;
// Draws the placeholders in view (after the text).
- (void)drawPlaceholdersInRect:(NSRect)dirty color:(NSColor *)color;
// A press on a placeholder opens its fold: YES when it took the press.
- (BOOL)mouseDown:(NSEvent *)event;
// Opens the folds that hide any end of these ranges (a selection, carets).
- (void)revealRanges:(NSArray<NSValue *> *)ranges;
// Whether every character of the range is hidden.
- (BOOL)rangeIsHidden:(NSRange)range;
// For the Backspace and Delete keys at a placeholder: the fold that ends
// (or starts) exactly at this caret is opened instead of deleting into it.
- (BOOL)openFoldEndingAt:(NSUInteger)caret;
- (BOOL)openFoldStartingAt:(NSUInteger)caret;

// ---- a tab's folds, kept while another tab is shown
- (NSArray<NSValue *> *)snapshot;            // the closed folds' spans
- (void)restore:(NSArray<NSValue *> *)spans;  // after the same text is back
@end
