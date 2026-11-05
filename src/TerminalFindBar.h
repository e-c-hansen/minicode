// TerminalFindBar.h — the find bar over a terminal (Cmd+F while a terminal
// has the keyboard): a field, "3 of 12", Match Case, previous, next and
// close, in a small box at the top right of the terminal, as VS Code's
// terminal find widget sits. The terminal does the searching (TerminalView
// asks its TerminalGridView); this only takes the keys and reports them.
//
// Keys while the field has the keyboard: typing searches as you go, Return
// is the next match (further down, newer), Shift+Return the previous one,
// Cmd+G and Shift+Cmd+G the same, Cmd+F selects the field's text, Esc
// closes. Window key equivalents reach the bar before the menu, so Cmd+G
// and Cmd+F are taken in performKeyEquivalent: (the field editor, a text
// view, would otherwise answer the menu's Find items itself).
#import <Cocoa/Cocoa.h>

@interface MCTerminalFindBar : NSView <NSTextFieldDelegate>
@property(nonatomic, readonly) NSTextField *field;
@property(nonatomic, readonly) BOOL matchCase;
@property(nonatomic, copy) void (^onChange)(void);          // text or Match Case
@property(nonatomic, copy) void (^onNext)(BOOL forward);
@property(nonatomic, copy) void (^onClose)(void);
// "3 of 12", "No results", or "10000+" past the limit.
- (void)showCount:(NSInteger)count index:(NSInteger)index limit:(NSInteger)limit;
// The keyboard to the field, its text selected.
- (void)focusField;
// The field is being edited (its field editor has the keyboard).
- (BOOL)isEditing;
- (void)applySettings;
+ (NSSize)preferredSize;
@end
