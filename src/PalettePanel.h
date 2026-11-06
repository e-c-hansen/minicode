// PalettePanel.h — Objective-C++. The palette, VS Code's quick input: a
// search field over a list, drawn over the top of a window. What it lists
// follows the field's first character (CommandPalette::parse):
//
//   ">"   every enabled command of the main menu (Shift+Cmd+P opens it so)
//   ":"   a line of the file on show, "42" or "42:7" (Ctrl+G opens it so)
//   "@"   a symbol of the file on show, "@:" grouped by kind (Shift+Cmd+O)
//   "#"   a symbol of the workspace, from the language servers (Cmd+T)
//   else  the window's folder's files, Quick Open (Cmd+P opens it empty)
//
// Typing filters (fuzzy, by the rules in CommandPalette.h), Up and Down
// choose, Return runs the command (through the responder chain, as its menu
// item would), opens the file or goes to the line or symbol; Esc or a click
// outside closes it, and the keyboard goes back where it was. Moving through
// a file's symbols shows each in the editor, and Esc puts the editor back.
#import <Cocoa/Cocoa.h>
#import "Lsp.h"   // MCSymbolItem

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
// "@": the file on show's symbols, asked for once when "@" is first typed
// (done may come at once or later); nil when nothing on show has symbols.
@property(nonatomic, copy) void (^documentSymbols)
    (void (^done)(NSArray<MCSymbolItem *> *items, NSInteger caretIndex, NSString *source));
// Show a symbol in the editor while the list is on it, the keyboard staying
// in the palette; goToSymbol goes there for good (the palette has closed);
// endPreview puts the editor back as it was when the palette closes
// without going anywhere.
@property(nonatomic, copy) void (^previewSymbol)(MCSymbolItem *item);
@property(nonatomic, copy) void (^goToSymbol)(MCSymbolItem *item);
@property(nonatomic, copy) void (^endPreview)(void);
// "#": the workspace's symbols for a query, from the window's servers;
// done comes as each answers (see LspSession workspaceSymbols:). nil when
// the window has no language servers at all.
@property(nonatomic, copy) void (^workspaceSymbols)
    (NSString *query, void (^done)(NSArray<MCSymbolItem *> *items, BOOL complete, NSString *note));
@property(nonatomic, copy) void (^cancelWorkspaceSymbols)(void);
@property(nonatomic, copy) void (^openWorkspaceSymbol)(MCSymbolItem *item);
@end

// Open the palette over window with text in its field (@">", @":", @"@",
// @"#" or @""),
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
NSArray<NSString *> *MCCommandPaletteKeys(NSView *palette);   // each row's keys, as drawn
NSInteger MCCommandPaletteSelection(NSView *palette);
BOOL MCCommandPaletteFilesListed(NSView *palette);
