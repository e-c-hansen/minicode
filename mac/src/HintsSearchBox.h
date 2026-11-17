// HintsSearchBox.h — the search box at the top of the shortcut hints
// (Shift+Cmd+H). While the panel is open every key without Command or
// Control, and not a function key, types into the box wherever the
// keyboard was (the window's key monitor hands it over through
// takeKeyDown:): the box takes the keyboard at the first such key and
// gives it back when the panel closes. Esc clears the box, then closes the
// panel; Return and Tab do nothing. A click puts the keyboard in it too.
// What a query finds is the core's (HintsSearch.h); the panel recolors.
#import <Cocoa/Cocoa.h>

// Marks on the hints' text where the box's query was found, and on the
// shortcuts it did not find (drawn dimmed); the checks read them.
extern NSAttributedStringKey const MCHintsMatchAttribute;
extern NSAttributedStringKey const MCHintsDimmedAttribute;

@interface MCHintsSearchBox : NSObject <NSSearchFieldDelegate>
@property(nonatomic, readonly) NSSearchField *field;
@property(nonatomic, copy) void (^onChange)(NSString *query);
@property(nonatomic, copy) void (^onClose)(void);   // Esc with the box empty
- (NSString *)query;
- (void)setFontSize:(CGFloat)size;
// The field's height at its font size.
- (CGFloat)height;
// Empties the box (onChange runs when it held something).
- (void)clear;
// A key press in the panel's window while the panel is open: YES when the
// box took it (typed, cleared, closed), NO when it goes on as before.
- (BOOL)takeKeyDown:(NSEvent *)event;
// The box has the keyboard.
- (BOOL)hasKeyboard;
// Puts the keyboard back where the box took it from, if the box has it.
// NO when it had it but cannot tell where from (a click put it there).
- (BOOL)giveBackKeyboard;
@end
