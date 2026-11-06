// DailyWorkSuite.mm — `make guitest SUITES=daily_work`: the daily-work
// commands of review 3's P9 part A. Cmd+] and Cmd+[ indent and outdent the
// caret's line, Cmd+Return inserts a line below; Toggle Bold and Italic wrap
// the word in Markdown, Option+C ticks a task there and does nothing in a
// Python file, Cmd+B stays the sidebar in Markdown; Shift+Return sends the
// line to a Python REPL in a terminal tab of its own and reuses it; a new
// terminal activates the folder's .venv only once it is allowed, with
// VIRTUAL_ENV, PATH, the prompt and deactivate; Save As, Save All, Close
// Other Tabs and Option+Cmd+Right; the recent folders and Ctrl+R; F1.
#import "Harness.h"
#import "EditorController.h"
#import "Keymap.h"
#import "Lsp.h"
#import "PalettePanel.h"
#import "RecentFolders.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#import "TreeDetails.h"   // MCAppDefaults
#include <sys/stat.h>

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTerminalPanel *Panel(EditorController *c) { return [c valueForKey:@"terminalPanel"]; }
NSString *Path(EditorController *c) { return [c valueForKey:@"currentPath"]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
void Caret(EditorController *c, NSUInteger at) {
    [c.window makeFirstResponder:Text(c)];
    Text(c).selectedRange = NSMakeRange(at, 0);
}
BOOL MenuKey(EditorController *c, unsigned short code, NSEventModifierFlags flags) {
    return MCGuiPerformMenuKey(c.window, code, flags, nil, nil);
}
const NSEventModifierFlags kCmd = NSEventModifierFlagCommand;
const NSEventModifierFlags kOptCmd = NSEventModifierFlagCommand | NSEventModifierFlagOption;
}  // namespace

MC_GUI_SUITE(daily_work) {
    __block EditorController *c = nil;
    __block TerminalView *repl = nil, *venvTerm = nil;
    __block NSUInteger tabsBefore = 0;
    // The folders' .venv answers, put back at the end for the suites after.
    __block id consentBefore = nil;
    NSString *dir = MCGuiProjectFile(@"daily");
    NSString *py = [dir stringByAppendingPathComponent:@"code.py"];
    NSString *md = [dir stringByAppendingPathComponent:@"notes.md"];
    NSString *fakePython = [dir stringByAppendingPathComponent:@"fake-python"];
    NSString *venvDir = [dir stringByAppendingPathComponent:@"venvproj"];

    MCGuiStep(@"a Python file, a Markdown file, and a stand-in REPL", ^{
        MCGuiWriteFile(py, @"def f():\n    x = 1\n\ny = 2\n\n\ndef g():\n    if y:\n        return 3\n");
        MCGuiWriteFile(md, @"buy milk\nsome word here\n");
        // Reads lines and says what it got, as a REPL would run them.
        MCGuiWriteFile(fakePython, @"#!/bin/sh\nwhile IFS= read -r l; do echo \"got:$l:\"; done\n");
        chmod(fakePython.fileSystemRepresentation, 0755);
        MCGuiSetSettings([NSString stringWithFormat:@"lsp.enabled = false\npython.interpreter = %@\n",
                                                    fakePython]);
        c = MCGuiController();
        GCHECK([c showFile:py]);
    });

    // ---- Cmd+], Cmd+[ and Cmd+Return
    MCGuiStep(@"Cmd+] indents the caret's line, Cmd+[ takes it back", ^{
        Caret(c, 12);   // in "    x = 1"
        GCHECK(MenuKey(c, 30, kCmd));   // ]
        GCHECK_EQ_STR(Text(c).string, @"def f():\n        x = 1\n\ny = 2\n\n\ndef g():\n    if y:\n        return 3\n");
        GCHECK(MenuKey(c, 33, kCmd));   // [
        GCHECK_EQ_STR(Text(c).string, @"def f():\n    x = 1\n\ny = 2\n\n\ndef g():\n    if y:\n        return 3\n");
    });
    MCGuiStep(@"Cmd+Return inserts an indented line below, wherever the caret is", ^{
        Caret(c, 2);    // in "def f():"
        GCHECK(MenuKey(c, 36, kCmd));
        GCHECK_EQ_STR(Text(c).string, @"def f():\n    \n    x = 1\n\ny = 2\n\n\ndef g():\n    if y:\n        return 3\n");
        GCHECK(Text(c).selectedRange.location == 13);
        [Text(c).undoManager undo];
        GCHECK_EQ_STR(Text(c).string, @"def f():\n    x = 1\n\ny = 2\n\n\ndef g():\n    if y:\n        return 3\n");
    });

    // ---- Markdown
    MCGuiStep(@"Toggle Bold and Italic wrap the word at the caret, and unwrap it", ^{
        GCHECK([c showFile:md]);
        if ([[c valueForKey:@"previewMode"] boolValue]) [c togglePreview:nil];   // the source
        GCHECK(![[c valueForKey:@"previewMode"] boolValue]);
        Caret(c, 15);   // in "word"
        GCHECK_EQ_STR([c keymapLanguage], @"markdown");
        [c toggleBold:nil];
        GCHECK_EQ_STR(Text(c).string, @"buy milk\nsome **word** here\n");
        [c toggleBold:nil];
        GCHECK_EQ_STR(Text(c).string, @"buy milk\nsome word here\n");
        [c toggleItalic:nil];
        GCHECK_EQ_STR(Text(c).string, @"buy milk\nsome *word* here\n");
        [c toggleItalic:nil];
        GCHECK_EQ_STR(Text(c).string, @"buy milk\nsome word here\n");
        // No keys: Cmd+B and Cmd+I are the sidebar and the agent.
        GCHECK(MCKeymapGlyphs(@"markdown.extension.editing.toggleBold").length == 0);
    });
    MCGuiStep(@"Option+C makes the line a task in Markdown", ^{
        Caret(c, 2);
        MCGuiSendKeyToApp(c.window, 8, NSEventModifierFlagOption, @"ç", @"c");
        GCHECK_EQ_STR(Text(c).string, @"- [ ] buy milk\nsome word here\n");
    });
    MCGuiStep(@"Cmd+B stays the sidebar's in Markdown: the key monitor leaves it to the menu", ^{
        Caret(c, 2);
        NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                 modifierFlags:kCmd timestamp:0
                                  windowNumber:c.window.windowNumber context:nil characters:@"b"
                   charactersIgnoringModifiers:@"b" isARepeat:NO keyCode:11];
        GCHECK(!MCKeymapHandleKeyDown(e));
        GCHECK_EQ_STR(Text(c).string, @"- [ ] buy milk\nsome word here\n");
        GCHECK_EQ_STR(MCKeymapGlyphs(@"workbench.action.toggleSidebarVisibility"), @"⌘B");
    });
    MCGuiStep(@"Option+C is no task key in a Python file", ^{
        GCHECK([c showFile:py]);
        Caret(c, 0);
        MCGuiSendKeyToApp(c.window, 8, NSEventModifierFlagOption, @"ç", @"c");
        GCHECK(![Text(c).string containsString:@"[ ]"]);
    });

    // ---- Shift+Return
    MCGuiStep(@"Shift+Return sends the line to a Python REPL in a new tab", ^{
        GCHECK([c showFile:py]);
        GCHECK_EQ_STR([c keymapLanguage], @"python");
        [c newTerminal:nil];   // a terminal already open: the REPL gets a tab of its own
        tabsBefore = Panel(c).tabCount;
        Caret(c, 22);   // in "y = 2"
        MCGuiSendKeyToApp(c.window, 36, NSEventModifierFlagShift, @"\r", @"\r");
        GCHECK(Panel(c).tabCount == tabsBefore + 1);
        repl = Panel(c).activeTerminal;
        GCHECK(c.window.firstResponder == Text(c));   // the keyboard stays
        // On to the next line with code.
        GCHECK(Text(c).selectedRange.location == [Text(c).string rangeOfString:@"def g"].location);
    });
    MCGuiWait(@"the REPL ran the line", 20, ^BOOL {
        return [[repl recentText:20] containsString:@"got:y = 2:"];
    });
    MCGuiStep(@"a selection goes to the same REPL, its indentation taken off", ^{
        [c.window makeFirstResponder:Text(c)];
        Text(c).selectedRange = NSMakeRange(9, 9);   // "    x = 1"
        [c runSelectionInTerminal:nil];
        GCHECK(Panel(c).tabCount == tabsBefore + 1);
    });
    MCGuiWait(@"the REPL ran the selection", 10, ^BOOL {
        return [[repl recentText:20] containsString:@"got:x = 1:"];
    });
    MCGuiStep(@"Shift+Return in Markdown sends nothing", ^{
        GCHECK([c showFile:md]);
        Caret(c, 2);
        MCGuiSendKeyToApp(c.window, 36, NSEventModifierFlagShift, @"\r", @"\r");
        GCHECK(Panel(c).tabCount == tabsBefore + 1);
        GCHECK(![[repl recentText:20] containsString:@"buy milk"]);
    });
    MCGuiStep(@"the REPL's tab closes", ^{ GCHECK([Panel(c) closeTerminal:repl]); });

    // ---- terminal.activate-venv
    MCGuiStep(@"a folder with a .venv, not allowed: a new terminal leaves it alone", ^{
        MCGuiSetSettings(@"lsp.enabled = false\n");
        NSString *bin = [venvDir stringByAppendingPathComponent:@".venv/bin"];
        MCGuiWriteFile([bin stringByAppendingPathComponent:@"python"], @"#!/bin/sh\n");
        chmod([bin stringByAppendingPathComponent:@"python"].fileSystemRepresentation, 0755);
        MCGuiWriteFile([venvDir stringByAppendingPathComponent:@".venv/pyvenv.cfg"],
                       @"home = /usr/bin\nprompt = demo-env\n");
        consentBefore = [MCAppDefaults() objectForKey:@"MCPythonEnvironmentConsent"];
        [(LspSession *)[c valueForKey:@"lsp"] setFolderEnvironmentAllowed:NO];
        venvTerm = [Panel(c) addTabInDirectory:venvDir];
    });
    MCGuiWait(@"the shell's prompt", 20, ^BOOL { return venvTerm.atPrompt; });
    MCGuiStep(@"VIRTUAL_ENV is empty", ^{ [venvTerm runCommand:@"echo \"VE:$VIRTUAL_ENV:\""]; });
    MCGuiWait(@"the answer", 10, ^BOOL {
        return venvTerm.atPrompt && [[venvTerm recentText:10] containsString:@"\nVE::"];
    });
    MCGuiStep(@"allowed: the next terminal starts with it active", ^{
        [Panel(c) closeTerminal:venvTerm];
        [(LspSession *)[c valueForKey:@"lsp"] setFolderEnvironmentAllowed:YES];
        venvTerm = [Panel(c) addTabInDirectory:venvDir];
    });
    MCGuiWait(@"the shell's prompt", 20, ^BOOL { return venvTerm.atPrompt; });
    MCGuiStep(@"VIRTUAL_ENV, PATH and the prompt", ^{
        [venvTerm runCommand:@"echo \"VE:$VIRTUAL_ENV: P:$path[1]: N:$VIRTUAL_ENV_PROMPT: S:$PS1:\""];
    });
    MCGuiWait(@"the answer", 10, ^BOOL {
        return venvTerm.atPrompt && [[venvTerm recentText:10] containsString:@"/venvproj/.venv: P:"];
    });
    MCGuiStep(@"its bin is first on PATH, its name before the prompt", ^{
        NSString *out = [venvTerm recentText:10];
        MCGuiNote(@"the terminal: %@", out);
        GCHECK([out containsString:@"/venvproj/.venv/bin: N:demo-env:"]);
        GCHECK([out containsString:@"S:(demo-env) "]);
        [venvTerm runCommand:@"deactivate; echo \"after:${VIRTUAL_ENV-none}:$PS1\""];
    });
    MCGuiWait(@"deactivate puts it back", 10, ^BOOL {
        NSString *out = [venvTerm recentText:10];
        return venvTerm.atPrompt && [out containsString:@"\nafter:none:"] &&
               ![out containsString:@"\nafter:none:(demo-env)"];
    });
    MCGuiStep(@"terminal.activate-venv = false: none", ^{
        [Panel(c) closeTerminal:venvTerm];
        MCGuiSetSettings(@"lsp.enabled = false\nterminal.activate-venv = false\n");
        venvTerm = [Panel(c) addTabInDirectory:venvDir];
    });
    MCGuiWait(@"the shell's prompt", 20, ^BOOL { return venvTerm.atPrompt; });
    MCGuiStep(@"VIRTUAL_ENV is empty again", ^{ [venvTerm runCommand:@"echo \"VE:$VIRTUAL_ENV:\""]; });
    MCGuiWait(@"the answer", 10, ^BOOL {
        return venvTerm.atPrompt && [[venvTerm recentText:10] containsString:@"\nVE::"];
    });
    MCGuiStep(@"the venv folder goes", ^{
        [Panel(c) closeTerminal:venvTerm];
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [[NSFileManager defaultManager] removeItemAtPath:venvDir error:nil];
        if (consentBefore) [MCAppDefaults() setObject:consentBefore forKey:@"MCPythonEnvironmentConsent"];
        else [MCAppDefaults() removeObjectForKey:@"MCPythonEnvironmentConsent"];
    });

    // ---- Save As, Save All, Close Other Tabs, tab keys
    MCGuiStep(@"Save As writes the buffer to the new file and the tab follows it", ^{
        NSString *a = [dir stringByAppendingPathComponent:@"a.txt"];
        NSString *b = [dir stringByAppendingPathComponent:@"b.txt"];
        MCGuiWriteFile(a, @"alpha\n");
        GCHECK([c showFile:a]);
        Caret(c, 0);
        MCGuiUndoGroup(Text(c).undoManager, ^{
            [Text(c) insertText:@"edited " replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK([c saveActiveTabAs:b]);
        GCHECK_EQ_STR(Disk(b), @"edited alpha\n");
        GCHECK_EQ_STR(Disk(a), @"alpha\n");
        GCHECK_EQ_STR(Path(c), b);
        GCHECK(![[c recentTabPaths] containsObject:a]);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
    });
    MCGuiStep(@"Option+Cmd+S saves every tab with changes", ^{
        NSString *one = [dir stringByAppendingPathComponent:@"one.txt"];
        NSString *two = [dir stringByAppendingPathComponent:@"two.txt"];
        MCGuiWriteFile(one, @"1\n");
        MCGuiWriteFile(two, @"2\n");
        for (NSString *p in @[one, two]) {
            GCHECK([c showFile:p]);
            Caret(c, 0);
            MCGuiUndoGroup(Text(c).undoManager, ^{
                [Text(c) insertText:@"x" replacementRange:NSMakeRange(0, 0)];
            });
        }
        GCHECK(MenuKey(c, 1, kOptCmd));
        GCHECK_EQ_STR(Disk(one), @"x1\n");
        GCHECK_EQ_STR(Disk(two), @"x2\n");
    });
    MCGuiStep(@"Option+Cmd+Left and Right move between tabs", ^{
        NSString *now = Path(c);
        GCHECK(MenuKey(c, 123, kOptCmd));
        GCHECK(![Path(c) isEqualToString:now]);
        GCHECK(MenuKey(c, 124, kOptCmd));
        GCHECK_EQ_STR(Path(c), now);
    });
    MCGuiStep(@"Option+Cmd+T closes the other tabs", ^{
        GCHECK([c recentTabPaths].count > 1);
        NSString *now = Path(c);
        GCHECK(MenuKey(c, 17, kOptCmd));
        GCHECK([c recentTabPaths].count == 1);
        GCHECK_EQ_STR(Path(c), now);
    });

    // ---- Open Recent and F1
    MCGuiStep(@"the window's folder is a recent one, in File > Open Recent", ^{
        NSString *mine = nil;
        for (NSString *r in MCRecentFolders())
            if ([r.stringByResolvingSymlinksInPath
                    isEqualToString:c.rootPath.stringByResolvingSymlinksInPath])
                mine = r;
        GCHECK(mine != nil);
        GCHECK(MenuKey(c, 15, NSEventModifierFlagControl));   // Ctrl+R
        NSView *palette = MCCommandPaletteIn(c.window);
        GCHECK(palette != nil);
        GCHECK_EQ_STR(MCCommandPaletteField(palette).stringValue, @">Open Recent > ");
        NSArray<NSString *> *labels = MCCommandPaletteLabels(palette);
        MCGuiNote(@"Open Recent lists: %@", labels);
        GCHECK(labels.count >= 2);
        for (NSString *l in labels) GCHECK([l hasPrefix:@"File > Open Recent > "]);
        GCHECK(mine && [labels containsObject:[@"File > Open Recent > "
                                     stringByAppendingString:mine.stringByAbbreviatingWithTildeInPath]]);
        [(NSTextView *)MCCommandPaletteField(palette).currentEditor
            doCommandBySelector:@selector(cancelOperation:)];   // Esc
        GCHECK(MCCommandPaletteIn(c.window) == nil);
    });
    MCGuiStep(@"F1 opens the command palette", ^{
        GCHECK(MenuKey(c, 122, 0));
        NSView *palette = MCCommandPaletteIn(c.window);
        GCHECK(palette != nil);
        GCHECK_EQ_STR(MCCommandPaletteField(palette).stringValue, @">");
        [(NSTextView *)MCCommandPaletteField(palette).currentEditor
            doCommandBySelector:@selector(cancelOperation:)];
        GCHECK(MCCommandPaletteIn(c.window) == nil);
    });
    MCGuiStep(@"the menus as built still agree with the registry", ^{
        for (NSString *p in MCKeymapMenuProblems()) MCGuiNote(@"menu problem: %@", p);
        GCHECK(MCKeymapMenuProblems().count == 0);
    });
}
