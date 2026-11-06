// Keymap.h — Objective-C++. The command registry (Commands.h) on the Mac:
// reads ~/.config/minicode/keys.conf (or $MINICODE_KEYS), watches it as
// AppSettings watches settings.conf, and puts the keys in effect on the
// main menu's items, so a saved change rebinds them at once.
//
// The menus are built in main.mm as before, keys and all; MCKeymapInstall
// then finds each item's command by its action and tag and gives it the
// command's keys: the first on the item itself, any others on hidden items
// beside it (allowsKeyEquivalentWhenHidden), as Cmd+J was for the terminal.
//
// Some keys never reach a menu by themselves, so the app's key monitor asks
// MCKeymapHandleKeyDown first, which runs a command (validated as its menu
// item would be) when:
//   - the command is Global (Ctrl+` and Ctrl+Shift+` for the terminal, the
//     Menu key, Cmd+keypad 0, Option+] and Option+[ during a tour);
//   - the key has neither Command nor Control (Shift+Option+F, F2, F9, ...)
//     and the editor has the keyboard: AppKit would hand it to the text view
//     as typing first. In another text field or the terminal's grid such a
//     key types instead, and anywhere else it goes on as before;
//   - two commands share the key in different places (keys.conf can give
//     one key to an editor command and a terminal command): what has the
//     keyboard decides;
//   - it is a key a menu cannot show or match (keypad digits, the Menu key,
//     Ctrl+Shift with punctuation);
//   - the command is the file tree's (F2 renames, Cmd+Delete trashes) and
//     the tree has the keyboard. A tree command's keys are never put on its
//     menu item, since a menu key works in every pane.
// A plain Control key with a letter or symbol goes to the terminal's grid
// while it has the keyboard (Ctrl+G for nano), unless its command is Global.
#import <Cocoa/Cocoa.h>

extern NSNotificationName const MCKeymapDidChangeNotification;

@interface MCKeymap : NSObject
+ (instancetype)shared;
@property(nonatomic, readonly, copy) NSString *path;
// Problems in the file, as "line N: message"; empty when it read cleanly.
@property(nonatomic, readonly, copy) NSArray<NSString *> *errors;
// Re-read the file now; applies it and notifies if it changed.
- (void)reload;
// Write Keyboard Shortcuts' commented list of every command if there is no
// file yet.
- (BOOL)ensureFileExists;
@end

// After the main menu is built: read the file, put its keys on the menus,
// and watch it.
void MCKeymapInstall(void);

// From the key monitor, for a key down in an editor window: YES when it
// ran a command (or sent the key where it belongs) and the event is done.
BOOL MCKeymapHandleKeyDown(NSEvent *e);
// Is the event's key bound to any command (Cmd+., which is also cancel)?
BOOL MCKeymapEventIsBound(NSEvent *e);

// A command's keys as the menu draws them ("⇧⌘P"): the first, or every one;
// "" or empty when it has none.
NSString *MCKeymapGlyphs(NSString *commandId);
NSArray<NSString *> *MCKeymapAllGlyphs(NSString *commandId);
// The first key of the command a menu item runs, as glyphs; nil when the
// item runs no command of the registry.
NSString *MCKeymapGlyphsForMenuItem(NSMenuItem *item);
// Registry commands with a selector but no item in the main menu, as
// unattached items (title, action, tag) with their menu path as the
// representedObject, for the palette. Empty today.
NSArray<NSMenuItem *> *MCKeymapCommandsWithoutMenuItems(void);

// For tests: what the menus held as built that the registry disagrees with
// (an item with a key and no command, a key that is not the command's
// default), found the first time the keys were applied. Empty when they
// agree.
NSArray<NSString *> *MCKeymapMenuProblems(void);
// The command id of an item (nil for none).
NSString *MCKeymapCommandForMenuItem(NSMenuItem *item);
