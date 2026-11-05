// MultiCaretView.h — several carets in the editor's text view.
//
// Cmd+Shift+drag (or Option+drag, or Shift+Option+drag) selects a column:
// a caret or selection on every line between the press and the pointer,
// from the press's x to the pointer's. Typing, Backspace, Delete, Return,
// Tab, Paste, Copy, Cut, the arrows (with Shift, Option and Command) then
// act at every caret, each edit one undo step. A Cmd+Shift (or
// Shift+Option) click after another makes the column between them, and
// Option+click adds a caret (or removes one already there). Esc, a plain
// click, or anything else that sets the selection goes back to one caret.
//
// NSTextView can hold several selections but only one empty one, so the
// carets live here (in MultiCaret's terms) and are drawn by the view; the
// text view's own selection holds the non-empty ones, which it highlights.
// The text logic is MultiCaret (pure C++, tested); this is the glue.
#import <Cocoa/Cocoa.h>

@interface MCMultiCarets : NSObject
- (instancetype)initWithTextView:(NSTextView *)tv;
// Two or more carets are in play.
@property(nonatomic, readonly) BOOL active;
// Set while this object edits the text or sets the selection, so the view
// can tell its own changes from anyone else's.
@property(nonatomic, readonly) BOOL applying;
// A press with the column modifiers: tracks the drag (or the click) and
// returns YES. NO for any other press.
- (BOOL)mouseDown:(NSEvent *)event;
// Typing and key commands while active; YES when handled.
- (BOOL)insertText:(id)text;
- (BOOL)doCommand:(SEL)selector;
// Copy Line Down / Up (Cmd+Shift+Down / Up, Shift+Option+Down / Up), for
// one caret or several.
- (void)duplicateLinesDown:(BOOL)down;
// Move Line Down / Up (Cmd+Down / Up): the lines swap with the one below
// or above, for one caret or several.
- (void)moveLinesDown:(BOOL)down;
// Add Next Occurrence (Cmd+D): an empty caret selects its word, and each
// press after it adds the next place its text occurs, as in VS Code. NO
// when there was nothing (more) to select.
- (BOOL)addNextOccurrence;
// Select All Occurrences (Shift+Cmd+L): every place the selection's text,
// or the word at the caret, occurs. NO when there is nothing to select.
- (BOOL)selectAllOccurrences;
// Add Cursor Below / Above (Option+Cmd+Down / Up): a caret on the next or
// previous line of each one, at the same x (the goal the arrow keys keep).
// NO when no caret has a line there.
- (BOOL)addCursorVertically:(int)direction;
// Select Line (Cmd+L): every caret's whole line, and the next line on each
// press after. Delete Line (Ctrl+Shift+K): the lines every caret touches.
// For one caret or several.
- (void)expandLineSelection;
- (void)deleteLines;
// Copy, Cut and Paste at several carets (YES when handled). With nothing
// selected, at one caret or several, Copy and Cut take the whole lines and
// mark the copy as lines, and Paste of such a copy puts it above the line
// (VS Code's behaviour); the caller does the rest.
- (BOOL)copy;
- (BOOL)cut;
- (BOOL)paste;
// Back to the text view's own single selection (left as it is).
- (void)clear;
// Draws the carets (call after the text view has drawn).
- (void)drawInRect:(NSRect)dirty;
// The carets, as ranges (location = the smaller end), for tests.
- (NSArray<NSValue *> *)caretRanges;
// Sets the carets directly, for tests and the demo: one range each.
- (void)setCaretRanges:(NSArray<NSValue *> *)ranges;
@end
