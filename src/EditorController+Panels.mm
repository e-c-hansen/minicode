// EditorController+Panels.mm — EditorController: the terminal dock and the browser, and moving the keyboard between panes.
#import "EditorController+Private.h"
#import "ProcessesPanel.h"

@implementation EditorController (Panels)

// The terminal dock, with its first terminal, made when first needed and
// again after its last terminal was closed.
- (void)makeTerminalPanel { [self makeTerminalPanelFirstIn:nil]; }
- (void)makeTerminalPanelFirstIn:(NSString *)first {
    if (self.terminalPanel) return;
    self.terminalPanel = [[MCTerminalPanel alloc] initWithDirectory:_root.path firstIn:first];
    self.terminalPanel.projectRoot = _root.path;
    __weak EditorController *weakSelf = self;
    self.terminalPanel.extraLinkRoots = ^NSArray<NSString *> *{
        EditorController *s = weakSelf;
        return s ? [s->_agents linkRoots] : @[];
    };
    self.terminalPanel.sessionLinkRoots = ^NSArray<NSString *> *(TerminalView *t) {
        EditorController *s = weakSelf;
        return s ? [s->_agents linkRootsForTerminal:t] : @[];
    };
    self.terminalPanel.onOpenLink = ^(MCTermLink *link) {
        [weakSelf openTerminalLink:link];
    };
    self.terminalPanel.onEmpty = ^{ [weakSelf terminalPanelEmptied]; };
    self.terminalPanel.onNote = ^(NSString *note) { [weakSelf showStatusNote:note]; };
    self.terminalPanel.hidden = YES;
    [self.rightArea addSubview:self.terminalPanel];

    if (!self.termDivider) {
        self.termDivider = [[DragBar alloc] initWithFrame:NSZeroRect];
        self.termDivider.onDrag = ^(CGFloat y) {
            [weakSelf terminalBarDraggedTo:y];
        };
        [self.rightArea addSubview:self.termDivider];
    }
}

// The last terminal was closed: the dock goes, and the keyboard goes back to
// the editor (or the tree, or the browser).
- (void)terminalPanelEmptied {
    MCTerminalPanel *panel = self.terminalPanel;
    BOOL hadFocus = [self.window.firstResponder isKindOfClass:NSView.class] &&
        [(NSView *)self.window.firstResponder isDescendantOf:panel];
    self.terminalPanel = nil;
    self.terminalVisible = NO;
    [panel removeFromSuperview];
    [self relayoutRightArea];
    if (hadFocus || !self.window.firstResponder ||
        self.window.firstResponder == self.window) {
        if (!self.editorHidden) [self focusActiveDocument];
        else if (self.browserVisible && self.browser) [self.browser focusURLBar];
        else [self.window makeFirstResponder:self.outline];
    }
    [self collapseRightAreaIfEmpty];
    if (self.hintsVisible) [self updateHints];
}

// Shows the dock (making it if needed) without changing the focus.
- (void)showTerminalPanel {
    [self makeTerminalPanel];
    if (!self.terminalVisible) {
        self.terminalVisible = YES;
        [self showRightArea];
        [self relayoutRightArea];
        if (self.hintsVisible) [self updateHints];
    }
}

// Ctrl+Shift+`: a new terminal in a new tab, opening the dock if needed.
- (void)newTerminal:(id)sender {
    BOOL fresh = self.terminalPanel == nil;
    [self showTerminalPanel];
    if (!fresh) [self.terminalPanel addTab];
    [self.terminalPanel focusActive];
}

// Cmd+\: a new terminal beside the active one.
- (void)splitTerminal:(id)sender {
    BOOL fresh = self.terminalPanel == nil;
    [self showTerminalPanel];
    if (!fresh) [self.terminalPanel splitActive];
    [self.terminalPanel focusActive];
}

// Option+Cmd+] and [: the next or previous terminal, across tabs.
- (void)nextTerminal:(id)sender {
    if (!self.terminalPanel) { [self newTerminal:nil]; return; }
    [self showTerminalPanel];
    [self.terminalPanel moveActive:1];
}
- (void)previousTerminal:(id)sender {
    if (!self.terminalPanel) { [self newTerminal:nil]; return; }
    [self showTerminalPanel];
    [self.terminalPanel moveActive:-1];
}

- (BOOL)revealTerminal:(TerminalView *)terminal {
    if (!terminal || ![self.terminalPanel.terminals containsObject:terminal]) return NO;
    [self showTerminalPanel];
    [self.terminalPanel selectTerminal:terminal];
    [terminal focusInput];
    return YES;
}

// Closes the terminal the keyboard is in. NO (and nothing closed) when the
// keyboard is elsewhere, so Cmd+W can fall through to closing something else.
- (BOOL)closeFocusedTerminal {
    if (!(self.terminalVisible && [self.terminalPanel hasFocus])) return NO;
    [self.terminalPanel closeTerminal:self.terminalPanel.activeTerminal];
    return YES;
}

// Ctrl+Shift+W: the active terminal of the dock on show, wherever the
// keyboard is.
- (void)closeTerminal:(id)sender {
    if (!(self.terminalVisible && self.terminalPanel)) { NSBeep(); return; }
    [self.terminalPanel closeTerminal:self.terminalPanel.activeTerminal];
}

// Shift+Cmd+Return: run the open file in a terminal, typed as if by hand so
// it shows and lands in the shell's history. The file is saved first, as VS
// Code does. A terminal busy with a command is never typed into (the keys
// would go to that program): with run.terminal = current the file then runs
// in a new terminal, and the status bar says so, which costs nothing and
// interrupts nothing, where asking would. The keyboard stays where it was,
// usually the editor, as in VS Code; Cmd+2 goes to the terminal.
- (void)runCurrentFile:(id)sender {
    if (!self.currentPath || self.showingMessage || self.isImage || self.isPDF ||
        self.isMedia || self.isDiff) {
        NSBeep();
        [self showStatusNote:@"Open a file to run it"];
        return;
    }
    NSString *problem = nil;
    // The interpreter the Python server uses (this folder's choice, then
    // python.interpreter, then the project's virtualenv), so a run and the
    // completions agree on which packages exist.
    NSString *python = [self.lsp.currentLanguage isEqualToString:@"python"]
                           ? [self.lsp pythonInterpreterWithSource:nil] : nil;
    NSString *command = MCRunCommandForFile(self.currentPath, _root.path, python, &problem);
    if (!command) {
        NSBeep();
        [self showStatusNote:problem];
        return;
    }
    if (self.dirty) {
        [self saveCurrentFile:nil];
        if (self.dirty) return;   // the save failed and said why
    }

    NSResponder *keep = self.window.firstResponder;
    BOOL fresh = self.terminalPanel == nil;
    [self showTerminalPanel];
    TerminalView *term = self.terminalPanel.activeTerminal;
    NSString *note = nil;
    if (!fresh && [AppSettings shared].settings.runInNewTerminal()) {
        term = [self.terminalPanel addTab];
    } else if (!fresh && term.busy) {
        term = [self.terminalPanel addTab];
        note = @"The terminal was busy, so the file runs in a new one";
    }
    [term runCommand:command];
    if (keep && keep != self.window && [self.window firstResponder] != keep)
        [self.window makeFirstResponder:keep];
    if (note) [self showStatusNote:note];
}

- (void)toggleTerminal:(id)sender {
    [self makeTerminalPanel];
    self.terminalVisible = !self.terminalVisible;
    if (self.terminalVisible) [self showRightArea];
    [self relayoutRightArea];
    if (self.terminalVisible) [self.terminal focusInput];
    else [self collapseRightAreaIfEmpty];
    if (self.hintsVisible) [self updateHints];
}

// Cmd+click on a link in the terminal. A URL opens in the browser panel; a
// file opens in the editor at the line and column the reference named; a
// folder is shown in the tree.
- (void)openTerminalLink:(MCTermLink *)link {
    // Found in several places (a worktree's copy and the folder's), or in
    // none: the picker, or a note saying where it was looked for.
    __weak EditorController *weakSelf = self;
    if ([_agents handleLink:link open:^(MCTermLink *chosen) { [weakSelf openTerminalLink:chosen]; }])
        return;
    [self.navigator jump:^{ [self openTerminalLinkNow:link]; }];
}

- (void)openTerminalLinkNow:(MCTermLink *)link {
    if (link.isURL) {
        [self openWebAddressFromTerminal:link.target];
        return;
    }
    // The terminal spells paths the way the shell does; put one inside the
    // folder back into the tree's own spelling (the root may be /tmp/x while
    // the file resolved to /private/tmp/x/y), so the tree can select it.
    NSString *path = link.target;
    NSString *rootReal = _root.path.stringByResolvingSymlinksInPath;
    NSString *real = path.stringByResolvingSymlinksInPath;
    BOOL inside = [real isEqualToString:rootReal] ||
                  [real hasPrefix:[rootReal stringByAppendingString:@"/"]];
    if (inside && real.length > rootReal.length)
        path = [_root.path stringByAppendingPathComponent:
                [real substringFromIndex:rootReal.length + 1]];
    else if (inside)
        path = _root.path;

    if (link.isDirectory) {
        if (!inside) { NSBeep(); return; }
        if (self.sidebarCollapsed) [self toggleSidebar:nil];
        [self revealPath:path andOpen:NO];
        return;
    }
    // Selected in the tree when the tree lists it, and opened either way (a
    // file in a worktree under the hidden .claude folder, a dotfile).
    if (inside) [self revealPath:path andOpen:NO];
    [self openFileAtPath:path];
    // Opening can be refused ("Save changes?" answered Cancel), and an image
    // or PDF has no lines to go to.
    if (![self.currentPath isEqualToString:path] || self.isImage || self.isPDF ||
        self.isMedia) return;
    if (link.line > 0) [self goToLine:link.line column:link.column];
    else [self.window makeFirstResponder:self.textView];
}

- (void)makeBrowserWithHome:(NSString *)home {
    if (self.browser) return;
    self.browser = [[BrowserView alloc] initWithHomeURL:home];
    [self.rightArea addSubview:self.browser];
    self.browserDivider = [[DragBar alloc] initWithFrame:NSZeroRect];
    self.browserDivider.vertical = YES;
    __weak EditorController *weakSelf = self;
    self.browserDivider.onDrag = ^(CGFloat x) {
        [weakSelf browserBarDraggedTo:x];
    };
    [self.rightArea addSubview:self.browserDivider];
}

- (void)toggleBrowser:(id)sender {
    [self makeBrowserWithHome:@"https://duckduckgo.com"];
    self.browserVisible = !self.browserVisible;
    if (self.browserVisible) [self showRightArea];
    [self relayoutRightArea];
    if (self.browserVisible) [self.browser focusURLBar];
    else [self collapseRightAreaIfEmpty];
    if (self.hintsVisible) [self updateHints];
}

// --------------------------------------------------- keyboard focus + switch
- (void)focusTree:(id)sender {
    // The sidebar holds the Source Control panel instead: focus its list.
    if (self.gitPanelVisible) { [self.gitPanel focusList]; return; }
    if (self.agentsVisible) { [_agents.panel focusList]; return; }
    if (MCProcessesPanel *procs = [self processesPanelOnShow]) { [procs focusList]; return; }
    if (self.outline.selectedRow < 0 && self.outline.numberOfRows > 0)
        [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
                  byExtendingSelection:NO];
    [self.window makeFirstResponder:self.outline];
}
- (void)focusEditor:(id)sender {
    [self revealEditor];
    // What the tab shows: the text, or the LaTeX preview, a PDF, the player
    // (whose own Space, arrows and J K L work) or a diff, never the text
    // view hidden behind them.
    [self focusActiveDocument];
}
// Cmd+2: the terminal's input (or its grid), opening the terminal if needed.
- (void)focusTerminal:(id)sender {
    if (!(self.terminalVisible && self.terminal)) { [self toggleTerminal:nil]; return; }
    [self showRightArea];
    [self.terminal focusInput];
}
// Cmd+3: the browser's address bar, opening the browser if needed.
- (void)focusBrowser:(id)sender {
    if (!(self.browserVisible && self.browser)) { [self toggleBrowser:nil]; return; }
    [self showRightArea];
    [self.browser focusURLBar];
}

@end
