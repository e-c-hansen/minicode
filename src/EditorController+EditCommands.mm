// EditorController+EditCommands.mm — EditorController: the daily-work
// commands VS Code users reach for: Indent and Outdent Lines (Cmd+] and
// Cmd+[), Insert Line Below and Above (Cmd+Return), Toggle Bold and Italic in
// Markdown, Shift+Return sending Python to a REPL, Save As, Save All
// (Option+Cmd+S), Open Recent (Ctrl+R), the language the keymap's
// language-scoped commands go by, and the environment a new terminal starts
// with (terminal.activate-venv). The keys are rows of Commands.cpp.
#import "EditorController+Private.h"
#import "EditorController+EditCommandsIvars.h"
#import "RecentFolders.h"
#include "Commands.h"
#include "RunFile.h"

namespace {
std::string StdOf(NSString *s) {
    const char *c = s.fileSystemRepresentation;
    return s.length && c ? c : "";
}
NSString *NSOf(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
}  // namespace

@implementation EditorController (EditCommands)

// ------------------------------------------------- the keymap's languages
// The language of the file the editor shows, for the commands scoped to one
// (MCKeymapLanguage): Markdown's Option+C, Python's Shift+Return.
- (NSString *)keymapLanguage {
    if (!_activeTab || self.showingMessage || !self.currentPath) return nil;
    if (self.isMarkdown) return @"markdown";
    const std::string lang = Commands::languageOfPath(StdOf(self.currentPath));
    return lang.empty() ? nil : NSOf(lang);
}

// The editor has the keyboard over source it can change.
- (BOOL)editorTakesEdits {
    return _activeTab && self.window.firstResponder == self.textView && self.textView.editable &&
           !self.showingMessage;
}

- (BOOL)isEditCommand:(SEL)a {
    return a == @selector(indentLines:) || a == @selector(outdentLines:) ||
           a == @selector(insertLineBelow:) || a == @selector(insertLineAbove:) ||
           a == @selector(toggleBold:) || a == @selector(toggleItalic:) ||
           a == @selector(runSelectionInTerminal:) || a == @selector(saveFileAs:) ||
           a == @selector(saveAllFiles:);
}

- (BOOL)validateEditCommand:(NSMenuItem *)item {
    SEL a = item.action;
    if (a == @selector(toggleBold:) || a == @selector(toggleItalic:))
        return [self editorTakesEdits] && self.isMarkdown;
    if (a == @selector(runSelectionInTerminal:))
        return [self editorTakesEdits] && [[self keymapLanguage] isEqualToString:@"python"];
    if (a == @selector(saveFileAs:)) return [self tabHasTextToWrite:_activeTab];
    if (a == @selector(saveAllFiles:)) {
        for (MCTab *t in _tabs)
            if (t.dirty) return YES;
        return NO;
    }
    return [self editorTakesEdits];
}

// ------------------------------------------------------ lines and Markdown
- (void)indentLines:(id)sender {
    if ([self editorTakesEdits]) MCIndentLines(self.textView, self.currentPath, NO);
    else NSBeep();
}

- (void)outdentLines:(id)sender {
    if ([self editorTakesEdits]) MCIndentLines(self.textView, self.currentPath, YES);
    else NSBeep();
}

- (void)insertLineBelow:(id)sender {
    if ([self editorTakesEdits]) MCInsertLine(self.textView, self.currentPath, NO, self.isMarkdown);
    else NSBeep();
}

- (void)insertLineAbove:(id)sender {
    if ([self editorTakesEdits]) MCInsertLine(self.textView, self.currentPath, YES, self.isMarkdown);
    else NSBeep();
}

- (void)toggleBold:(id)sender {
    if ([self editorTakesEdits] && self.isMarkdown)
        MCToggleMarkdownWrap(self.textView, @"**", @"Toggle Bold");
    else NSBeep();
}

- (void)toggleItalic:(id)sender {
    if ([self editorTakesEdits] && self.isMarkdown)
        MCToggleMarkdownWrap(self.textView, @"*", @"Toggle Italic");
    else NSBeep();
}

// ---------------------------------------- Shift+Return: Python to a REPL
// The Python extension's python.execSelectionInTerminal: the selection, or
// the cursor's line (then the cursor moves to the next line with code), runs
// in a Python REPL in a terminal tab of its own, started with the
// interpreter Run File uses. The keyboard stays in the editor.
- (void)runSelectionInTerminal:(id)sender {
    NSTextView *tv = self.textView;
    if (![self editorTakesEdits] || ![[self keymapLanguage] isEqualToString:@"python"]) {
        NSBeep();
        return;
    }
    NSString *s = tv.string;
    NSRange sel = tv.selectedRange;
    NSString *code;
    BOOL advance = NO;
    if (sel.length) {
        code = [s substringWithRange:sel];
    } else {
        code = [s substringWithRange:[s lineRangeForRange:NSMakeRange(sel.location, 0)]];
        advance = YES;
    }
    if (advance) [self moveCaretToNextCodeLine];
    if (![code stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
             .length)
        return;
    if (!_replPending) _replPending = [NSMutableArray array];
    [_replPending addObject:code];
    [self startPythonRepl];
    [self sendPendingToRepl];
}

- (void)moveCaretToNextCodeLine {
    NSTextView *tv = self.textView;
    NSString *s = tv.string;
    NSRange line = [s lineRangeForRange:NSMakeRange(tv.selectedRange.location, 0)];
    NSUInteger at = NSMaxRange(line);
    while (at < s.length) {
        NSRange next = [s lineRangeForRange:NSMakeRange(at, 0)];
        NSString *text = [s substringWithRange:next];
        NSRange code = [text rangeOfCharacterFromSet:
            NSCharacterSet.whitespaceAndNewlineCharacterSet.invertedSet];
        if (code.location != NSNotFound) {
            at = next.location + code.location;
            break;
        }
        at = NSMaxRange(next);
    }
    if (at > s.length) at = s.length;
    [tv setSelectedRange:NSMakeRange(at, 0)];
    [tv scrollRangeToVisible:NSMakeRange(at, 0)];
}

// The REPL's terminal: the one started before while Python still runs in
// it; else that terminal (Python having ended there) or a new tab, with the
// interpreter started in it.
- (void)startPythonRepl {
    TerminalView *t = _pythonRepl;
    if (t && ![self.terminalPanel.terminals containsObject:t]) t = nil;
    if (t && (t.programHasTerminal || _replWaiting)) return;
    NSResponder *keep = self.window.firstResponder;
    const BOOL fresh = self.terminalPanel == nil;
    [self showTerminalPanel];
    if (!t) t = fresh ? self.terminalPanel.activeTerminal : [self.terminalPanel addTab];
    NSString *python = [self.lsp pythonInterpreterWithSource:nil] ?: @"python3";
    [t runCommand:NSOf(RunFile::shellQuote(StdOf(python)))];
    _pythonRepl = t;
    _replWaiting = YES;
    if (keep) [self.window makeFirstResponder:keep];
}

// Code goes in once Python holds the terminal, a moment after it starts so
// its line editor is up (Python 3.13's turns bracketed paste on), and at
// most 15 seconds after it was asked for.
- (void)sendPendingToRepl {
    TerminalView *t = _pythonRepl;
    if (!t || !_replPending.count) return;
    if (_replWaiting) {
        if (!_replPolling) {
            _replPolling = YES;
            [self pollRepl:0 held:0];
        }
        return;
    }
    NSArray<NSString *> *codes = [_replPending copy];
    [_replPending removeAllObjects];
    for (NSString *code in codes) {
        const BOOL bracketed = t.programTakesBracketedPaste;
        bool extra = false;
        std::vector<std::string> lines = RunFile::pythonReplLines(StdOf(code), bracketed, &extra);
        if (lines.empty()) continue;
        std::string joined;
        for (size_t i = 0; i < lines.size(); i++) joined += (i ? "\n" : "") + lines[i];
        if (![t sendToProgram:NSOf(joined) bracketed:bracketed]) {
            [self showStatusNote:@"Python is not running in the terminal"];
            return;
        }
        [t sendToProgram:extra ? @"\n\n" : @"\n" bracketed:NO];
    }
}

// Every tenth of a second: Python holding the terminal four looks in a
// row (its line editor up by then) sends the code; 150 looks without it
// drops it, saying so.
- (void)pollRepl:(int)tries held:(int)held {
    TerminalView *t = _pythonRepl;
    if (!t) {
        _replWaiting = _replPolling = NO;
        [_replPending removeAllObjects];
        return;
    }
    held = t.programHasTerminal ? held + 1 : 0;
    if (held >= 4) {
        _replWaiting = _replPolling = NO;
        [self sendPendingToRepl];
        return;
    }
    if (tries >= 150) {
        _replWaiting = _replPolling = NO;
        [_replPending removeAllObjects];
        [self showStatusNote:@"Python did not start in the terminal"];
        return;
    }
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 10), dispatch_get_main_queue(), ^{
        [weakSelf pollRepl:tries + 1 held:held];
    });
}

// ---------------------------------------------------- Save As, Save All
- (void)saveFileAs:(id)sender {
    MCTab *t = _activeTab;
    if (![self tabHasTextToWrite:t]) {
        NSBeep();
        return;
    }
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.directoryURL = [NSURL fileURLWithPath:t.currentPath.stringByDeletingLastPathComponent];
    panel.nameFieldStringValue = t.currentPath.lastPathComponent;
    panel.canCreateDirectories = YES;
    if ([panel runModal] != NSModalResponseOK || !panel.URL.path.length) return;
    [self saveActiveTabAs:panel.URL.path];
}

// The buffer written to `path` (the save panel has asked about replacing a
// file there), then the tab shows that file in place of the old one, whose
// own file is left as it was on disk, as in VS Code.
- (BOOL)saveActiveTabAs:(NSString *)path {
    MCTab *old = _activeTab;
    if (![self tabHasTextToWrite:old] || !path.length) return NO;
    if ([path.stringByResolvingSymlinksInPath
            isEqualToString:old.currentPath.stringByResolvingSymlinksInPath]) {
        [self saveCurrentFile:nil];
        return !self.dirty;
    }
    // A tab already open on the new file goes first (asked about, if it has
    // changes of its own).
    MCTab *there = [self tabForPath:path];
    if (there && there != old && ![self closeTab:there prompt:YES]) return NO;
    if (_activeTab != old) [self activateTab:old];
    NSString *text = self.textView.editable ? [self.textView.string copy] : (self.sourceText ?: @"");
    // In the old file's encoding, as VS Code's Save As keeps it.
    std::string bytes, error;
    if (!MCEncodeFileText(text, old.encoding, old.bom, &bytes, &error) ||
        !FolderSearch::writeFileAtomically(path.fileSystemRepresentation, bytes, &error)) {
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"“%@” could not be saved.", path.lastPathComponent];
        a.informativeText = [NSString stringWithFormat:@"%@: %@.", path, NSOf(error)];
        [a runModal];
        return NO;
    }
    if (![self showFile:path]) return NO;
    if (_activeTab != old && [_tabs containsObject:old]) {
        old.dirty = NO;   // what it held is in the new file
        [self closeTab:old prompt:NO];
    }
    [self showStatusNote:[NSString stringWithFormat:@"Saved as %@", path.lastPathComponent]];
    return YES;
}

// Every tab with unsaved changes, by Save's rules (the disk is checked for
// each first); format.on-save stays with Cmd+S, as for Save All on closing.
- (void)saveAllFiles:(id)sender {
    for (MCTab *t in [_tabs copy])
        if (t.dirty && ![self saveTab:t]) break;
}

// ----------------------------------------- terminals: the venv activated
// What a new terminal in `dir` adds to its shell's environment: the Python
// environment to activate (terminal.activate-venv), when the interpreter
// Run File and the servers use is a virtual environment's. The folder's own
// .venv only once allowed (the bar over a Python file asks), since its bin
// goes first on the terminal's PATH; until then, one status note.
- (NSDictionary<NSString *, NSString *> *)terminalEnvironmentForDirectory:(NSString *)dir {
    if (![AppSettings shared].settings.terminalActivateVenv() || !self.lsp) return nil;
    BOOL waiting = NO;
    NSString *python = [self.lsp terminalPythonInterpreterInDirectory:dir waiting:&waiting];
    if (waiting && !_venvNoteShown) {
        _venvNoteShown = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self showStatusNote:@"The folder's .venv is activated in new terminals once you "
                                  "allow it (the bar over a Python file asks)"];
        });
    }
    RunFile::Activation act;
    if (!python || !RunFile::activationFor(StdOf(python), &act)) return nil;
    return @{@"MINICODE_VENV": NSOf(act.prefix), @"MINICODE_VENV_PROMPT": NSOf(act.prompt)};
}

@end
