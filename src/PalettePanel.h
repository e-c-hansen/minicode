// PalettePanel.h — Objective-C++. The command palette (Shift+Cmd+P, View >
// Command Palette…): a search field over a list of every enabled command in
// the main menu, drawn over the top of a window. Typing filters (fuzzy, by
// the rules in CommandPalette.h), Up and Down choose, Return runs the
// command through the responder chain as its menu item would, Esc or a click
// outside closes it, and the keyboard goes back where it was.
#import <Cocoa/Cocoa.h>

// Open the palette over window, or put the keyboard back in its field if it
// is already open there. The commands are read from NSApp.mainMenu now,
// validated for the window's current first responder.
void MCShowCommandPalette(NSWindow *window);

// The palette open over window, or nil. For tests: its field, the labels
// listed in order, and the index of the selected one.
NSView *MCCommandPaletteIn(NSWindow *window);
NSTextField *MCCommandPaletteField(NSView *palette);
NSArray<NSString *> *MCCommandPaletteLabels(NSView *palette);
NSInteger MCCommandPaletteSelection(NSView *palette);
