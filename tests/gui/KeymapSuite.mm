// KeymapSuite.mm — `make guitest SUITES=keymap`: the command registry and
// keys.conf (Commands.h, Keymap.h). The menus as built agree with the
// registry; Keyboard Shortcuts writes keys.conf; a saved keys.conf rebinds a
// menu item and a key the app's key monitor handles, takes keys away, has
// its conflict shown in the status bar, and shows in the palette and the
// hints; emptying it puts the defaults back.
#import "Harness.h"
#import "EditorController.h"
#import "Keymap.h"
#import "PalettePanel.h"
#import "TerminalPanel.h"
#import "EditorFont.h"
#include "Commands.h"

namespace {

NSMenuItem *FindItem(NSMenu *menu, SEL action, NSInteger tag, BOOL hidden) {
    for (NSMenuItem *it in menu.itemArray) {
        if (it.submenu) {
            NSMenuItem *found = FindItem(it.submenu, action, tag, hidden);
            if (found) return found;
        }
        if (it.action == action && it.tag == tag && it.isHidden == hidden) return it;
    }
    return nil;
}
NSMenuItem *Item(SEL action) { return FindItem(NSApp.mainMenu, action, 0, NO); }
NSMenuItem *HiddenItem(SEL action) { return FindItem(NSApp.mainMenu, action, 0, YES); }

// "mask:key" of every item with a key equivalent, for finding two on one key.
void CollectKeys(NSMenu *menu, NSMutableDictionary<NSString *, NSMutableArray *> *out) {
    for (NSMenuItem *item in menu.itemArray) {
        if (item.submenu) CollectKeys(item.submenu, out);
        if (!item.keyEquivalent.length || item.isAlternate) continue;
        // AppKit's own Edit menu additions (Emoji & Symbols, Start Dictation),
        // which a menu update can add more than once.
        if (item.action == NSSelectorFromString(@"orderFrontCharacterPalette:") ||
            item.action == NSSelectorFromString(@"startDictation:"))
            continue;
        NSEventModifierFlags m = item.keyEquivalentModifierMask &
            (NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption |
             NSEventModifierFlagControl);
        NSString *k = [NSString stringWithFormat:@"%lu:%@", (unsigned long)m, item.keyEquivalent];
        if (!out[k]) out[k] = [NSMutableArray array];
        [out[k] addObject:item.title];
    }
}

NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
NSUInteger TerminalCount(EditorController *c) {
    MCTerminalPanel *panel = [c valueForKey:@"terminalPanel"];
    return panel ? panel.terminals.count : 0;
}
NSString *StatusLeft(EditorController *c) {
    return ((NSTextField *)[c valueForKey:@"settingsLabel"]).stringValue;
}
NSString *Hints(EditorController *c) {
    NSAttributedString *a = [c performSelector:@selector(hintsText)];
    return a.string;
}
void WriteKeys(NSString *text) {
    MCGuiWriteFile([MCKeymap shared].path, text);
    [[MCKeymap shared] reload];
}
unichar F(int n) { return (unichar)(NSF1FunctionKey + n - 1); }
NSString *FKey(int n) { return [NSString stringWithFormat:@"%C", F(n)]; }
const NSEventModifierFlags kCtrlAltCmd =
    NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagCommand;

}  // namespace

MC_GUI_SUITE(keymap) {
    __block EditorController *c = nil;
    NSString *py = MCGuiProjectFile(@"keys/sample.py");

    MCGuiStep(@"the menus as built agree with the registry", ^{
        c = MCGuiController();
        NSArray<NSString *> *problems = MCKeymapMenuProblems();
        for (NSString *p in problems) MCGuiNote(@"menu problem: %@", p);
        GCHECK(problems.count == 0);
        // Every command with a selector has a menu item, or says why not.
        for (NSMenuItem *it in MCKeymapCommandsWithoutMenuItems()) {
            const Commands::Command *cmd = Commands::findBySelector(
                NSStringFromSelector(it.action).UTF8String, (int)it.tag);
            BOOL explained = cmd && cmd->note;
            if (!explained) MCGuiNote(@"no menu item for %@ > %@", it.representedObject, it.title);
            GCHECK(explained);
        }
        // No two items on one key, the hidden second keys included.
        NSMutableDictionary<NSString *, NSMutableArray *> *keys = [NSMutableDictionary dictionary];
        CollectKeys(NSApp.mainMenu, keys);
        for (NSString *k in keys)
            if (keys[k].count > 1) {
                MCGuiNote(@"two items on %@: %@", k, keys[k]);
                GCHECK(keys[k].count == 1);
            }
        GCHECK([[MCKeymap shared].errors count] == 0);
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:[MCKeymap shared].path]);
        // The defaults on the items, second keys on hidden ones.
        GCHECK_EQ_STR(Item(@selector(toggleComment:)).keyEquivalent, @"/");
        GCHECK_EQ_STR(HiddenItem(@selector(toggleTerminal:)).keyEquivalent, @"j");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"workbench.action.showCommands"), @"⇧⌘P");
        GCHECK([Hints(c) containsString:@"⇧⌘P"]);
    });

    MCGuiStep(@"the monitor's default keys: Cmd+keypad 0 and Ctrl+`", ^{
        [c.window makeFirstResponder:Text(c)];
        MCEditorZoomBy(2);
        GCHECK(MCEditorZoom() != 0);
        MCGuiSendKeyToApp(c.window, 82 /* keypad 0 */,
                          NSEventModifierFlagCommand | NSEventModifierFlagNumericPad, @"0", @"0");
        GCHECK(MCEditorZoom() == 0);
        BOOL shown = [[c valueForKey:@"terminalVisible"] boolValue];
        MCGuiSendKeyToApp(c.window, 50 /* ` */, NSEventModifierFlagControl, @"`", @"`");
        GCHECK([[c valueForKey:@"terminalVisible"] boolValue] == !shown);
        MCGuiSendKeyToApp(c.window, 50, NSEventModifierFlagControl, @"`", @"`");
        GCHECK([[c valueForKey:@"terminalVisible"] boolValue] == shown);
    });

    MCGuiStep(@"Keyboard Shortcuts writes keys.conf and opens it", ^{
        [NSApp sendAction:@selector(openKeyboardShortcuts:) to:nil from:nil];
        NSString *path = [MCKeymap shared].path;
        NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding
                                                      error:nil];
        GCHECK([text containsString:@"# shift+cmd+p = workbench.action.showCommands"]);
        GCHECK([text containsString:@"(no key) = workbench.action.openGlobalKeybindings"]);
        NSString *shown = [c valueForKey:@"currentPath"];
        GCHECK([shown.stringByResolvingSymlinksInPath
            isEqualToString:path.stringByResolvingSymlinksInPath]);
        GCHECK([[MCKeymap shared].errors count] == 0);
        // The palette finds it by name.
        NSMenuItem *item = Item(@selector(openKeyboardShortcuts:));
        GCHECK(item != nil && item.keyEquivalent.length == 0);
    });

    MCGuiStep(@"a saved keys.conf rebinds menu items and monitor keys", ^{
        MCGuiWriteFile(py, @"b = 2\na = 1\n");
        GCHECK([c showFile:py]);
        WriteKeys(@"# the suite's keys\n"
                  @"ctrl+alt+cmd+k = editor.action.commentLine\n"   // a menu key, added
                  @"f7 = minicode.sortLines\n"                      // a monitor key, added
                  @"f9 = -\n"                                       // and one taken away
                  @"ctrl+shift+` = -\n"                             // the monitor's Ctrl+Shift+`
                  @"ctrl+alt+cmd+t = workbench.action.terminal.new\n"
                  @"cmd+k = -\n"                                    // Clear Terminal's
                  @"ctrl+alt+cmd+j = minicode.toggleBrowser\n"
                  @"ctrl+alt+cmd+j = minicode.toggleAgents\n");     // a conflict
        NSArray<NSString *> *errors = [MCKeymap shared].errors;
        GCHECK(errors.count == 1);
        GCHECK([errors.firstObject hasPrefix:@"line 9:"]);
        GCHECK([errors.firstObject containsString:@"from line 8"]);
        GCHECK([StatusLeft(c) hasPrefix:@"Keys line 9:"]);

        NSMenuItem *comment = Item(@selector(toggleComment:));
        GCHECK_EQ_STR(comment.keyEquivalent, @"k");
        GCHECK(comment.keyEquivalentModifierMask == kCtrlAltCmd);
        GCHECK_EQ_STR(HiddenItem(@selector(toggleComment:)).keyEquivalent, @"/");
        GCHECK_EQ_STR(Item(@selector(sortLines:)).keyEquivalent, FKey(7));
        GCHECK(HiddenItem(@selector(sortLines:)) == nil);
        GCHECK_EQ_STR(Item(@selector(clearTerminal:)).keyEquivalent, @"");
        GCHECK_EQ_STR(Item(@selector(newTerminal:)).keyEquivalent, @"t");
        GCHECK_EQ_STR(Item(@selector(toggleBrowser:)).keyEquivalent, @"j");
        GCHECK_EQ_STR(Item(@selector(toggleAgents:)).keyEquivalent, @"a");
        GCHECK([Hints(c) containsString:@"⌃⌥⌘K"]);
    });

    MCGuiStep(@"the new menu key runs its command", ^{
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(0, 0);
        MCGuiUndoGroup(tv.undoManager, ^{
            GCHECK(MCGuiPerformMenuKey(c.window, 40 /* k */, kCtrlAltCmd, nil, nil));
        });
        GCHECK_EQ_STR(tv.string, @"# b = 2\na = 1\n");
        [tv.undoManager undo];
        GCHECK_EQ_STR(tv.string, @"b = 2\na = 1\n");
    });

    MCGuiStep(@"F7 sorts through the key monitor, and F9 no longer does", ^{
        NSTextView *tv = Text(c);
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(0, tv.string.length);
        MCGuiUndoGroup(tv.undoManager, ^{
            MCGuiSendKeyToApp(c.window, 101 /* F9 */, NSEventModifierFlagFunction, FKey(9), FKey(9));
        });
        GCHECK_EQ_STR(tv.string, @"b = 2\na = 1\n");
        tv.selectedRange = NSMakeRange(0, tv.string.length);
        MCGuiUndoGroup(tv.undoManager, ^{
            MCGuiSendKeyToApp(c.window, 98 /* F7 */, NSEventModifierFlagFunction, FKey(7), FKey(7));
        });
        GCHECK_EQ_STR(tv.string, @"a = 1\nb = 2\n");
    });

    __block NSUInteger terminals = 0;
    MCGuiStep(@"Ctrl+Shift+` is free, and the new key opens a terminal", ^{
        terminals = TerminalCount(c);
        MCGuiSendKeyToApp(c.window, 50 /* ` */,
                          NSEventModifierFlagControl | NSEventModifierFlagShift, @"~", @"~");
        GCHECK(TerminalCount(c) == terminals);
        MCGuiSendKeyToApp(c.window, 17 /* t */, kCtrlAltCmd, @"t", @"t");
        GCHECK(TerminalCount(c) == terminals + 1);
    });

    MCGuiStep(@"the palette shows the new key", ^{
        [c.window makeFirstResponder:Text(c)];
        [NSApp sendAction:@selector(showCommandPalette:) to:nil from:nil];
        NSView *palette = MCCommandPaletteIn(c.window);
        GCHECK(palette != nil);
        NSTextView *ed = (NSTextView *)MCCommandPaletteField(palette).currentEditor;
        [ed insertText:@"toggle comment" replacementRange:ed.selectedRange];
        NSArray<NSString *> *labels = MCCommandPaletteLabels(palette);
        NSArray<NSString *> *keys = MCCommandPaletteKeys(palette);
        NSUInteger i = [labels indexOfObject:@"Edit > Toggle Comment"];
        GCHECK(i != NSNotFound);
        if (i != NSNotFound) GCHECK_EQ_STR(keys[i], @"⌃⌥⌘K");
        [ed doCommandBySelector:@selector(cancelOperation:)];
        GCHECK(MCCommandPaletteIn(c.window) == nil);
    });

    MCGuiStep(@"an empty keys.conf puts the defaults back", ^{
        WriteKeys(@"");
        GCHECK([[MCKeymap shared].errors count] == 0);
        GCHECK_EQ_STR(StatusLeft(c), @"");
        NSMenuItem *comment = Item(@selector(toggleComment:));
        GCHECK_EQ_STR(comment.keyEquivalent, @"/");
        GCHECK(comment.keyEquivalentModifierMask == NSEventModifierFlagCommand);
        GCHECK(HiddenItem(@selector(toggleComment:)) == nil);
        GCHECK_EQ_STR(Item(@selector(sortLines:)).keyEquivalent, FKey(9));
        GCHECK_EQ_STR(Item(@selector(clearTerminal:)).keyEquivalent, @"k");
        GCHECK_EQ_STR(Item(@selector(newTerminal:)).keyEquivalent, @"`");
        GCHECK_EQ_STR(HiddenItem(@selector(toggleTerminal:)).keyEquivalent, @"j");
        GCHECK(MCKeymapMenuProblems().count == 0);
        NSMutableDictionary<NSString *, NSMutableArray *> *keys = [NSMutableDictionary dictionary];
        CollectKeys(NSApp.mainMenu, keys);
        for (NSString *k in keys)
            if (keys[k].count > 1) {
                MCGuiNote(@"two items on %@: %@", k, keys[k]);
                GCHECK(keys[k].count == 1);
            }
        // The old monitor key works again.
        NSUInteger before = TerminalCount(c);
        MCGuiSendKeyToApp(c.window, 50, NSEventModifierFlagControl | NSEventModifierFlagShift,
                          @"~", @"~");
        GCHECK(TerminalCount(c) == before + 1);
        [[NSFileManager defaultManager] removeItemAtPath:[MCKeymap shared].path error:nil];
    });
}
