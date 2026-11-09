// Gutter.h — Objective-C++. The line numbers beside the editor: a vertical
// ruler on the editor's scroll view, so it scrolls with the text and takes
// its width from the text's side of the pane.
//
// Each logical line is numbered on its first line fragment (a wrapped
// line's continuations stay blank), the carets' lines in the active color;
// lines with an error or a warning from the language server (or from the
// LaTeX preview's last typeset, CodeTextView's buildMarks) have a dot in
// the severity's color at the left. Only the fragments on screen are
// visited, and line numbers come from a LineIndex kept up to date from the
// text storage's edits, so neither drawing nor typing counts lines through
// the file. A click selects a line, a drag the lines it passes, Shift
// extends the selection.
//
// The gutter shows only while the text view is editable (source, never a
// preview or a message) and the setting is not off. Its background is a
// plain layer color, painted once (see PanelScrollView), so it is right at
// any opacity. Its right side is the fold lane (FoldView.h): a chevron by
// each line a region starts on while the pointer is over the gutter, and
// always by a closed fold; a click there folds or unfolds.
#import <Cocoa/Cocoa.h>
#include "QuickDiff.h"
#include <vector>

@class CodeTextView;

typedef NS_ENUM(NSInteger, MCLineNumbers) {
    MCLineNumbersOff,
    MCLineNumbersOn,
    MCLineNumbersRelative,
};

@interface MCLineGutter : NSRulerView
// Adds itself to the text view's scroll view as its vertical ruler.
- (instancetype)initWithTextView:(CodeTextView *)textView;
@property(nonatomic, assign) MCLineNumbers mode;
// The editor's font: the numbers are drawn in it, on the text's baselines.
@property(nonatomic, strong) NSFont *font;
@property(nonatomic, strong) NSColor *numberColor, *activeNumberColor, *backgroundColor;
// The line a character is on (0-based), from the index.
- (NSUInteger)lineOfCharacter:(NSUInteger)index;
// Where a line starts, and how many there are (a text ending in a break has
// an empty last line), from the same index.
- (NSUInteger)startOfLine:(NSUInteger)line;
@property(nonatomic, readonly) NSUInteger lineCount;
// Whether it is showing now.
@property(nonatomic, readonly) BOOL shown;
// The width of its lanes beside the digits (dots, change bars, folds), for
// the split editor's other group (EditorMirror.h), which matches it.
@property(nonatomic, readonly) CGFloat laneWidth;

// The change bars (QuickDiff.h, the editor's git gutter): a lane of their
// own between the dots and the numbers while changeLane is on, a 3-point
// bar beside each added or changed line and a small triangle where lines
// were deleted, in the three colors (added, modified, deleted). They move
// with each edit to the text (QuickDiff::shift) until the next diff is set.
// A click on one calls onChangeBar with its index and its rect in the
// gutter's coordinates, instead of selecting lines.
@property(nonatomic, assign) BOOL changeLane;
- (void)setChangeBars:(const std::vector<QuickDiff::Bar> &)bars;
- (const std::vector<QuickDiff::Bar> &)changeBars;
- (void)setChangeColorsAdded:(NSColor *)added modified:(NSColor *)modified
                     deleted:(NSColor *)deleted;
@property(nonatomic, copy) void (^onChangeBar)(NSInteger index, NSRect rect);
// The rect a bar is drawn in (or for a deletion, the boundary it marks),
// in the gutter's coordinates; NSZeroRect when its lines are not laid out.
- (NSRect)rectOfChangeBar:(NSInteger)index;
@end
