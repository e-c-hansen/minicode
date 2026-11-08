// LspInlayHints.h — Objective-C++. Inlay hints in the editor's text view:
// what a language server says between the words (`x: int`, `-> str`,
// `count=`), drawn in the text without being part of it.
//
// The text never changes. Each hint opens a gap after the character before
// it (MCTextGap, a kern the storage answers in attributesAtIndex: and never
// stores), as wide as its label, and the label is drawn in the gap after
// the text, in a muted color on a faint box, as VS Code draws them. So the
// characters, undo, the highlighter, copying, the carets' columns, Find
// and the folds all see the text as it is; only the layout has room for
// the labels. The storage keeps the gaps on their text through every edit
// (Lsp::inlayFollowEdit), and LspSession (InlayHints) asks the server for
// the hints of the lines on show again a moment after typing stops.
#import <Cocoa/Cocoa.h>
#include "InlayHints.h"
#include <vector>

@interface MCInlayHints : NSObject
- (instancetype)initWithTextView:(NSTextView *)textView;

// Shows `hints` at the places Lsp::placeInlayHints gave for the text as it
// is now, in place of those shown. Nothing happens in a view whose storage
// is not the editor's (MCSourceStorage).
- (void)showHints:(const std::vector<Lsp::InlayHint> &)hints
           placed:(const std::vector<Lsp::PlacedInlayHint> &)placed;
- (void)clear;

// What is shown now: each hint's offset (as edits have moved it) and label,
// in order. For checks and the session.
@property(nonatomic, readonly) NSUInteger count;
- (NSArray<NSDictionary *> *)shownHints;   // {offset, label}
// The label's box in view coordinates for a shown hint (by its place in
// shownHints), NSZeroRect when it is not drawn (folded, at a line break).
- (NSRect)rectOfHintAtIndex:(NSUInteger)index;

// Called by the view after it draws its text.
- (void)drawInRect:(NSRect)dirty;
@end
