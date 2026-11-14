// MultiCaretView.h — several carets in the editor's text view.
//
// Cmd+Shift+drag, Option+drag or a middle-button drag selects a column: a
// caret or selection on every line between the press and the pointer, from
// the press's x to the pointer's. Typing, Backspace, Delete, Return, Tab,
// Paste, Copy, Cut, the arrows (with Shift, Option and Command) then act at
// every caret, each edit one undo step. A Cmd+Shift click after another
// makes the column between them. Shift+Option click or drag is VS Code's
// box selection: the column from the caret to the pointer. Option+click
// adds a caret (or removes one already there). Esc, a plain click, or
// anything else that sets the selection goes back to one caret.
//
// NSTextView can hold several selections but only one empty one, so the
// carets live here (in MultiCaret's terms) and are drawn by the view; the
// text view's own selection holds the non-empty ones, which it highlights.
// The text logic is MultiCaret (pure C++, tested); this is the glue.
#import <Cocoa/Cocoa.h>

@class MCAutoClose;

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
// A middle-button press (no modifiers): a column from it, as in VS Code.
- (BOOL)otherMouseDown:(NSEvent *)event;
// Typing and key commands while active; YES when handled. A character
// typed at one caret or several, and Backspace, go through the
// auto-closing rules first (MCAutoClose), active or not.
- (BOOL)insertText:(id)text;
- (BOOL)insertText:(id)text replacementRange:(NSRange)replacement;
- (BOOL)doCommand:(SEL)selector;
// Auto-closing pairs in this view (its host says what the editor shows).
@property(nonatomic, readonly) MCAutoClose *autoClose;
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
// A line for the status bar: Select All Occurrences, Add Next Occurrence
// and Add Cursor make at most 10,000 carets, as in VS Code, and say so here
// when they stop there.
@property(nonatomic, copy) void (^onNote)(NSString *note);
// Draws the carets (call after the text view has drawn).
- (void)drawInRect:(NSRect)dirty;
// The carets, as ranges (location = the smaller end), for tests.
- (NSArray<NSValue *> *)caretRanges;
// Sets the carets directly, for tests and the demo: one range each.
- (void)setCaretRanges:(NSArray<NSValue *> *)ranges;
@end
