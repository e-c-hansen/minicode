// PalettePanel.h — Objective-C++. The palette, VS Code's quick input: a
// search field over a list, drawn over the top of a window. What it lists
// follows the field's first character (CommandPalette::parse):
//
//   ">"   every enabled command of the main menu (Shift+Cmd+P opens it so)
//   ":"   a line of the file on show, "42" or "42:7" (Ctrl+G opens it so)
//   else  the window's folder's files, Quick Open (Cmd+P opens it empty)
//
// Typing filters (fuzzy, by the rules in CommandPalette.h), Up and Down
// choose, Return runs the command (through the responder chain, as its menu
// item would), opens the file or goes to the line; Esc or a click outside
// closes it, and the keyboard goes back where it was.
#import <Cocoa/Cocoa.h>

// What the window offers the palette beyond the menu.
@interface MCPaletteContext : NSObject
// The folder Quick Open lists, nil for none. Paths are joined to it as
// spelled, so they match the tree's.
@property(nonatomic, copy) NSString *root;
// Files the window opened lately, most recent first (its tabs), listed
// first in Quick Open, ahead of those opened from Quick Open before.
@property(nonatomic, copy) NSArray<NSString *> *recentFiles;
// Opens a file of the folder (an absolute path below root).
@property(nonatomic, copy) void (^openFile)(NSString *path);
// Goes to a 1-based line and column (0: the line's start) of the file on
// show; nil when nothing on show has lines.
@property(nonatomic, copy) void (^goToLine)(NSInteger line, NSInteger column);
@end

// Open the palette over window with text in its field (@">", @":" or @""),
// or, if it is open there already, give it that text and the keyboard.
// Commands are read from NSApp.mainMenu now, validated for the window's
// current first responder; the folder's files are listed in the background.
void MCShowPalette(NSWindow *window, NSString *text, MCPaletteContext *context);

// The palette open over window, or nil. For tests: its field, the labels
// listed in order, the index of the selected one, and whether the folder's
// files have been listed.
NSView *MCCommandPaletteIn(NSWindow *window);
NSTextField *MCCommandPaletteField(NSView *palette);
NSArray<NSString *> *MCCommandPaletteLabels(NSView *palette);
NSInteger MCCommandPaletteSelection(NSView *palette);
BOOL MCCommandPaletteFilesListed(NSView *palette);
