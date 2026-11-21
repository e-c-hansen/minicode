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

// A list of the caller's own in place of the modes above, as VS Code's quick
// picks (the branch picker): typing filters the rows by the same fuzzy
// rules, the whole text being the query; Return or a click runs `choose`
// with the row's index, after the palette has closed. `typedRow` may make a
// last row from what is typed ("Create branch “x”"), nil or "" for none,
// which runs `chooseTyped` with the text.
@interface MCPalettePick : NSObject
@property(nonatomic, copy) NSString *placeholder;
@property(nonatomic, copy) NSArray<NSString *> *labels;    // matched and shown
@property(nonatomic, copy) NSArray<NSString *> *details;   // muted, at the right
@property(nonatomic, copy) void (^choose)(NSInteger index);
@property(nonatomic, copy) NSString *(^typedRow)(NSString *text);
@property(nonatomic, copy) void (^chooseTyped)(NSString *text);
@property(nonatomic, copy) NSString *emptyText;   // when nothing matches
// A list that shows each row as the list comes to it, as the theme picker
// does (ThemePickerPanel.h). With `preview` set: the row of initialIndex is
// selected while the text is empty, and the row last shown is kept
// selected while it matches what is typed (else the best match); `preview`
// runs with a row's index whenever the selection comes to another row, by
// the arrow keys, by typing, or by the pointer moving over it; `cancel`
// runs when the palette closes without a choice (Esc, a click outside,
// the keyboard going elsewhere). The palette follows a settings change
// while it is open, so it shows each preview's colors too.
@property(nonatomic) NSInteger initialIndex;
@property(nonatomic, copy) void (^preview)(NSInteger index);
@property(nonatomic, copy) void (^cancel)(void);
@end

// mutible's own folder picker (Open Folder…, the Dock's Open Folder…), in
// place of the system's open panel; its rules are the core's FolderPicker.h.
// It lists the recent folders, then the base folder's subfolders; a typed
// path ("~/Code/", "/Users/", "../") lists that folder's subfolders as it is
// typed, read off the main thread, the reading of a folder no longer typed
// cancelled. Tab, or '/' typed after a name, completes the selected folder
// and goes into it; Return opens the selected row; Cmd+Return (or the "Open
// This Folder" row) opens the folder the text names; Esc closes. The last
// row is Choose in Finder…, which runs chooseInFinder.
@interface MCFolderPick : NSObject
@property(nonatomic, copy) NSArray<NSString *> *recentFolders;   // most recent first
@property(nonatomic, copy) NSString *baseFolder;   // listed under them; relative paths start here
@property(nonatomic, copy) void (^choose)(NSString *path);   // after the palette has closed
@property(nonatomic, copy) void (^chooseInFinder)(void);
// Add Folder to Focus…: only folders strictly inside scopeFolder are
// offered (labelled by their path from it), and there is no Choose in
// Finder…. The field's placeholder and the typed folder's row ("Open This
// Folder") may be given their own words.
@property(nonatomic, copy) NSString *scopeFolder;
@property(nonatomic, copy) NSString *placeholder;
@property(nonatomic, copy) NSString *typedRowTitle;
@end

// Shows the folder picker over window (it must have a content view).
void MCShowFolderPicker(NSWindow *window, MCFolderPick *pick);

// What the window offers the palette beyond the menu.
@interface MCPaletteContext : NSObject
// A pick list instead of the modes (see MCPalettePick); nil for the palette.
@property(nonatomic, strong) MCPalettePick *pick;
// The folder picker instead of the modes (see MCFolderPick).
@property(nonatomic, strong) MCFolderPick *folderPick;
// The folder Quick Open lists, nil for none. Paths are joined to it as
// spelled, so they match the tree's.
@property(nonatomic, copy) NSString *root;
// Lists root's files itself instead of the palette's walk of this Mac's
// disk (a window over SSH: its host's list), calling `done` once on the
// main thread with paths from root. nil: the palette walks root.
@property(nonatomic, copy) void (^listFiles)(void (^done)(NSArray<NSString *> *files, BOOL truncated));
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
// or, if it is open there already, give it that text and the keyboard. A
// pick list (context.pick) always opens afresh, closing an open palette, and
// any other call closes an open pick list first.
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
// For tests: the pointer moved over a row (index into the labels), as a
// mouseMoved: over the list delivers it.
void MCCommandPaletteHoverRow(NSView *palette, NSInteger row);
BOOL MCCommandPaletteFilesListed(NSView *palette);
