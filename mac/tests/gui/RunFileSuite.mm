// RunFileSuite.mm — `make guitest SUITES=run_file`: Run File
// (Shift+Cmd+Return). The file is saved first, typed into the terminal so
// it shows and lands in the shell's history, run with the project's
// virtualenv's python, and the keyboard stays in the editor; a terminal
// busy with a command is never typed into (a new tab gets the run, and the
// status bar says why); a file with no way to run says so.
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#include <sys/stat.h>

namespace {
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTerminalPanel *Panel(EditorController *c) { return [c valueForKey:@"terminalPanel"]; }
NSString *Note(EditorController *c) { return [[c valueForKey:@"settingsLabel"] stringValue]; }
NSString *Disk(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}
BOOL RunFileKey(EditorController *c) {
    // Shift+Cmd+Return (kVK_Return).
    return MCGuiPerformMenuKey(c.window, 36, NSEventModifierFlagCommand | NSEventModifierFlagShift,
                               nil, nil);
}
}  // namespace

MC_GUI_SUITE(run_file) {
    __block EditorController *c = nil;
    __block TerminalView *first = nil;
    __block NSUInteger tabs = 0;
    NSString *dir = MCGuiProjectFile(@"runner");
    NSString *py = [dir stringByAppendingPathComponent:@"hello world.py"];
    NSString *sh = [dir stringByAppendingPathComponent:@"greet.sh"];
    NSString *txt = [dir stringByAppendingPathComponent:@"notes.txt"];
    NSString *venvPython = [dir stringByAppendingPathComponent:@".venv/bin/python"];

    MCGuiStep(@"a project with a virtualenv, a script and a text file", ^{
        MCGuiWriteFile(py, @"print('from python')\n");
        MCGuiWriteFile(sh, @"echo \"greet-ran-$((40 + 2))\"\n");
        MCGuiWriteFile(txt, @"just text\n");
        // A stand-in for the venv's python: says who it is and what it ran.
        MCGuiWriteFile(venvPython, @"#!/bin/sh\necho \"venv-python ran: $1\"\n");
        chmod(venvPython.fileSystemRepresentation, 0755);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        c = MCGuiController();
        [c focusEditor:nil];
        GCHECK([c showFile:sh]);
        // An unsaved change: Run File saves first.
        MCGuiUndoGroup(Text(c).undoManager, ^{
            Text(c).selectedRange = NSMakeRange(0, 0);
            [Text(c) insertText:@"# run me\n" replacementRange:Text(c).selectedRange];
        });
        [c.window makeFirstResponder:Text(c)];
        GCHECK(RunFileKey(c));
    });
    MCGuiWait(@"the script runs in the terminal", 20, ^BOOL {
        TerminalView *t = Panel(c).activeTerminal;
        return t && [[t recentText:6] containsString:@"\ngreet-ran-42"];
    });
    MCGuiStep(@"saved first; the keyboard stayed in the editor", ^{
        GCHECK([Disk(sh) hasPrefix:@"# run me\n"]);
        GCHECK(![[c valueForKey:@"dirty"] boolValue]);
        GCHECK(c.window.firstResponder == Text(c));
        first = Panel(c).activeTerminal;
        tabs = Panel(c).tabCount;
        MCGuiNote(@"the terminal shows: %@", [first recentText:3]);
    });
    MCGuiWait(@"back at the prompt", 10, ^BOOL { return first.atPrompt && !first.busy; });
    MCGuiStep(@"a Python file with a space in its name runs with the venv's python", ^{
        GCHECK([c showFile:py]);
        [c.window makeFirstResponder:Text(c)];
        GCHECK(RunFileKey(c));
    });
    MCGuiWait(@"the venv's python ran it", 20, ^BOOL {
        return [[first recentText:6] containsString:@"venv-python ran: "] &&
               [[first recentText:6] containsString:@"hello world.py"];
    });
    MCGuiStep(@"in the same terminal; in the shell's history", ^{
        GCHECK(Panel(c).tabCount == tabs);
        GCHECK(Panel(c).activeTerminal == first);
    });
    MCGuiWait(@"the prompt", 10, ^BOOL { return first.atPrompt && !first.busy; });
    MCGuiStep(@"fc -l lists the run", ^{ [first runCommand:@"fc -ln -1"]; });
    MCGuiWait(@"history", 10, ^BOOL {
        return first.atPrompt && [[first recentText:3] containsString:@".venv/bin/python"];
    });
    // A busy terminal is never typed into.
    MCGuiStep(@"sleep 5 holds the terminal; Run File again", ^{ [first runCommand:@"sleep 5"]; });
    MCGuiWait(@"busy", 10, ^BOOL { return first.busy; });
    MCGuiStep(@"Run File while it sleeps", ^{
        [c.window makeFirstResponder:Text(c)];
        GCHECK(RunFileKey(c));
        GCHECK(Panel(c).tabCount == tabs + 1);
        GCHECK(Panel(c).activeTerminal != first);
        GCHECK_EQ_STR(Note(c), @"The terminal was busy, so the file runs in a new one");
        GCHECK(c.window.firstResponder == Text(c));
    });
    MCGuiWait(@"it ran in the new tab", 20, ^BOOL {
        return [[Panel(c).activeTerminal recentText:6] containsString:@"venv-python ran: "];
    });
    MCGuiStep(@"the sleeping terminal got nothing typed into it", ^{
        // Typed in, the command would have been echoed below "sleep 5".
        NSString *after = [first recentText:200];
        NSRange sleep = [after rangeOfString:@"sleep 5" options:NSBackwardsSearch];
        GCHECK(sleep.location != NSNotFound &&
               [[after substringFromIndex:NSMaxRange(sleep)] rangeOfString:@"hello"].location ==
                   NSNotFound);
        GCHECK(first.busy);
        GCHECK_EQ_STR(first.runningCommand, @"sleep 5");
    });
    MCGuiStep(@"a text file has no way to run", ^{
        GCHECK([c showFile:txt]);
        NSUInteger before = Panel(c).tabCount;
        [c runCurrentFile:nil];
        GCHECK(Panel(c).tabCount == before);
        MCGuiNote(@"for notes.txt: %@", Note(c));
        GCHECK(Note(c).length > 0);
    });
    MCGuiStep(@"the extra tab closes", ^{
        GCHECK([Panel(c) closeTerminal:Panel(c).activeTerminal]);
    });
}
