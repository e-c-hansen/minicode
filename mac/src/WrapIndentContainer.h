// WrapIndentContainer.h — Objective-C++. The editor's text container, which
// starts a wrapped line's continuation under the line's indentation
// (editor.wrapping-indent; the arithmetic is the core's, WrapIndent.h).
//
// TextKit 1 asks the container for each line fragment's rect, with the
// index of the line's first character
// (lineFragmentRectForProposedRect:atIndex:...). For a line that does not
// start a paragraph, a continuation, the rect answered starts that much
// further right and is that much narrower, so the typesetter breaks the
// line within it: the text, its attributes, the carets, copies and undo
// are as they were, and only layout sees the indent. A paragraph style's
// head indent would have done it too, but as a stored attribute, set
// paragraph by paragraph and kept up to date through every edit; this
// reads the paragraph's leading spaces and tabs as each line is laid out.
//
// The container still says it is a simple rectangle: TextKit keeps
// non-contiguous layout (large-file mode, a tab shown again far down) only
// for a container that says so, and still asks this one for every line
// fragment's rect.
//
// Cost: one look for the paragraph's bounds and its leading whitespace per
// paragraph laid out (the last paragraph is remembered until the text
// changes), which layout reads anyway; a 5,000,000-character line costs
// one scan of it per edit, against the 0.35 s TextKit takes to lay it out
// again.
#import <Cocoa/Cocoa.h>
#include "WrapIndent.h"

@interface MCWrapIndentContainer : NSTextContainer
// None turns it off; a change lays the text out again. The editor sets it
// from the settings while it shows source, and None while it shows a page
// or a viewer's text (a Markdown page has indents of its own).
@property(nonatomic, assign) WrapIndent::Mode mode;
// The text changed: what was read about the last paragraph is stale. The
// editor's layout manager calls it from processEditing.
- (void)textDidChange;
// How far in a continuation of the paragraph holding `index` starts, in
// points (0: at the edge), for checks.
- (CGFloat)continuationIndentAt:(NSUInteger)index;
@end
