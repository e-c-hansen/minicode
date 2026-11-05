// Gutter.h — Objective-C++. The line numbers beside the editor: a vertical
// ruler on the editor's scroll view, so it scrolls with the text and takes
// its width from the text's side of the pane.
//
// Each logical line is numbered on its first line fragment (a wrapped
// line's continuations stay blank), the carets' lines in the active color;
// lines with an error or a warning from the language server have a dot in
// the severity's color at the left. Only the fragments on screen are
// visited, and line numbers come from a LineIndex kept up to date from the
// text storage's edits, so neither drawing nor typing counts lines through
// the file. A click selects a line, a drag the lines it passes, Shift
// extends the selection.
//
// The gutter shows only while the text view is editable (source, never a
// preview or a message) and the setting is not off. Its background is a
// plain layer color, painted once (see PanelScrollView), so it is right at
// any opacity. Its right side is kept free for fold markers.
#import <Cocoa/Cocoa.h>

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
// Whether it is showing now.
@property(nonatomic, readonly) BOOL shown;
@end
