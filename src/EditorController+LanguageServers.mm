// EditorController+LanguageServers.mm — EditorController: the language servers' commands, problems, and installing a server.
#import "EditorController+Private.h"

@implementation EditorController (LanguageServers)

// Ctrl+Space, F12, Option+Cmd+I. The session beeps and says why when the file has
// no language server.
- (BOOL)lspActionAllowed {
    if (self.window.firstResponder == self.textView && self.textView.editable) return YES;
    NSBeep();
    return NO;
}
- (void)triggerCompletion:(id)sender {
    if ([self lspActionAllowed]) [self.lsp triggerCompletion];
}
- (void)goToDefinition:(id)sender {
    if ([self lspActionAllowed]) [self.lsp goToDefinition];
}
- (void)showHoverInfo:(id)sender {
    if ([self lspActionAllowed]) [self.lsp showHoverInfo];
}
// F2 is VS Code's key for both: the symbol in the editor, the selected file
// in the tree, where it is the registry's renameFile (Commands.cpp), which
// the key monitor runs; so a keys.conf that moves one leaves the other.
- (void)renameSymbol:(id)sender {
    if ([self lspActionAllowed]) [self.lsp renameSymbol];
}
- (void)findReferences:(id)sender {
    if ([self lspActionAllowed]) [self.lsp findReferences];
}

- (void)showQuickFixes:(id)sender {
    if ([self lspActionAllowed]) [self.lsp showQuickFixes];
}
- (void)organizeImports:(id)sender {
    if ([self lspActionAllowed]) [self.lsp organizeImports];
}
// F8 / Shift+F8: the next or previous problem in the file on show.
- (void)nextProblem:(id)sender {
    if ([self lspActionAllowed]) [self.lsp goToProblem:YES];
}
- (void)previousProblem:(id)sender {
    if ([self lspActionAllowed]) [self.lsp goToProblem:NO];
}

// Shift+Cmd+M, as in VS Code: shows the Problems list with the keyboard in
// it; pressed in it, hides it again; pressed elsewhere while it shows,
// gives it the keyboard.
- (void)toggleProblems:(id)sender {
    if (self.problemsVisible && !self.problemsPanel.hasFocus &&
        [sender isKindOfClass:NSMenuItem.class]) {
        [self.problemsPanel focusList];
        return;
    }
    if (self.problemsVisible) {
        BOOL hadFocus = self.problemsPanel.hasFocus;
        self.problemsVisible = NO;
        [self relayoutRightArea];
        [self collapseRightAreaIfEmpty];
        if (hadFocus) [self focusAfterClosingPanel];
    } else {
        if (!self.problemsPanel) {
            MCProblemsPanel *panel = [[MCProblemsPanel alloc] initWithFrame:NSZeroRect];
            __weak EditorController *weakSelf = self;
            panel.onOpen = ^(MCProblem *p) { [weakSelf.lsp openProblem:p]; };
            panel.onEscape = ^{ [weakSelf focusEditor:nil]; };
            panel.onClose = ^{
                EditorController *s = weakSelf;
                if (s.problemsVisible) [s toggleProblems:nil];
            };
            panel.hidden = YES;
            [self.rightArea addSubview:panel];
            self.problemsPanel = panel;
        }
        self.problemsVisible = YES;
        [self updateProblems];
        [self showRightArea];
        [self relayoutRightArea];
        [self.problemsPanel focusList];
    }
    if (self.hintsVisible) [self updateHints];
}

// The servers' diagnostics changed: the counts in the status bar, and the
// list when it shows.
- (void)updateProblems {
    [self updateProblemCounts];
    if (self.problemsVisible)
        [self.problemsPanel setRows:[self.lsp problemRows] counts:self.lsp.problemCountsText
                               root:_root.path];
}

// The status bar's counts alone (cheap: no text is read), on every change
// of the language server's status too.
- (void)updateProblemCounts {
    LspSession *lsp = self.lsp;
    BOOL known = lsp.problemsKnown;
    self.problemsLabel.hidden = !known;
    if (known) {
        NSInteger errors = lsp.problemErrors, warnings = lsp.problemWarnings;
        NSColor *dot = errors ? [NSColor colorWithSRGBRed:0xF1 / 255.0 green:0x4C / 255.0
                                                     blue:0x4C / 255.0 alpha:1]
                     : warnings ? [NSColor colorWithSRGBRed:0xCC / 255.0 green:0xA7 / 255.0
                                                       blue:0 alpha:1]
                     : nil;
        [self.problemsLabel setStatus:lsp.problemCountsText dot:dot];
    }
    [self layoutStatusRight];
}
- (void)showLanguageServers:(id)sender {
    __weak EditorController *weakSelf = self;
    MCShowLanguageServers(self.lsp, self.lspLabel, ^(NSString *command) {
        [weakSelf typeCommandInTerminal:command];
    });
}

// ------------------------------------------------- installing a server
// The bar over the editor, when the open file's language server is wanted
// but missing. It is offered once per language per launch (across windows)
// and never for a language whose bar said Don't Show Again; it stays over
// that language's files in this window until closed or the server turns up.
// Folders whose question about their own Python environment was put off
// with × in this launch: it comes back when minicode starts again.
static NSMutableSet<NSString *> *EnvironmentQuestionPutOff(void) {
    static NSMutableSet<NSString *> *roots;
    if (!roots) roots = [NSMutableSet set];
    return roots;
}

// The folder's own Python environment to ask about now, or nil.
- (NSString *)environmentToAsk {
    NSString *env = self.lsp.folderEnvironmentAwaitingConsent;
    if (!env || [EnvironmentQuestionPutOff() containsObject:self.lsp.root ?: @""]) return nil;
    return env;
}

- (void)ensureInstallBar {
    if (self.installBar) return;
    MCInstallBar *bar = [[MCInstallBar alloc] initWithFrame:NSZeroRect];
    __weak EditorController *weakSelf = self;
    __weak MCInstallBar *weakBar = bar;
    // The same bar asks about a folder's environment; its buttons then
    // answer that.
    bar.onInstall = ^(NSButton *sender) {
        if (weakBar.consentEnvironment) [weakSelf.lsp setFolderEnvironmentAllowed:YES];
        else [weakSelf showInstallHelpFrom:sender];
    };
    bar.onClose = ^{
        EditorController *s = weakSelf;
        if (weakBar.consentEnvironment) {
            [EnvironmentQuestionPutOff() addObject:s.lsp.root ?: @""];
            [s updateInstallBar];
        } else {
            [s closeInstallBarMuting:NO];
        }
    };
    bar.onMute = ^{
        if (weakBar.consentEnvironment) [weakSelf.lsp setFolderEnvironmentAllowed:NO];
        else [weakSelf closeInstallBarMuting:YES];
    };
    self.installBar = bar;
    [self applyInstallBarColors];
    bar.hidden = YES;   // shown, and laid out, by the caller
    [self.rightArea addSubview:bar];
}

- (void)updateInstallBar {
    // The question about the folder's own Python environment comes first:
    // until it is answered the servers run without it, and a server
    // missing from PATH may be in it.
    NSString *ask = [self environmentToAsk];
    if (ask) {
        [self ensureInstallBar];
        if (![self.installBar.consentEnvironment isEqualToString:ask])
            self.installBar.consentEnvironment = ask;
        if (self.installBar.hidden) {
            self.installBar.hidden = NO;
            [self relayoutRightArea];
        }
        return;
    }
    if (self.installBar.consentEnvironment) {
        self.installBar.consentEnvironment = nil;
        self.installBar.hidden = YES;
        [self relayoutRightArea];
    }
    NSString *missing = self.lsp.missingServer;
    NSString *language = self.lsp.currentLanguage;
    // The open file's server was missing a moment ago and runs now.
    if (_missingBefore && !missing && [language isEqualToString:_missingBefore] &&
        self.lsp.hasServer) {
        [_installPopover close];
        [self showStatusNote:[NSString stringWithFormat:@"Found the %@ language server",
                             @(Lsp::serverDisplayName(Utf8String(language)).c_str())]];
    }
    _missingBefore = [missing copy];
    if (_installBarKey && !missing && [language isEqualToString:_installBarKey]) {
        _installBarKey = nil;   // found, or switched off: the bar has done its job
    } else if (missing && ![missing isEqualToString:_installBarKey] &&
               !MCInstallBarMuted(missing) && MCInstallBarTakeOffer(missing)) {
        _installBarKey = [missing copy];   // a language not offered yet takes the bar
    }
    BOOL show = _installBarKey && [_installBarKey isEqualToString:missing];
    if (show) [self ensureInstallBar];
    if (show) self.installBar.serverKey = _installBarKey;
    if (self.installBar && self.installBar.hidden == show) {
        self.installBar.hidden = !show;
        [self relayoutRightArea];
    }
}

// The bar takes the top of the editor's slot, under the tab strip.
- (NSRect)layoutInstallBarIn:(NSRect)slot editorShown:(BOOL)shown {
    MCInstallBar *bar = self.installBar;
    if (!bar) return slot;
    BOOL show = shown && !bar.hidden &&
                (bar.consentEnvironment
                     ? [bar.consentEnvironment isEqualToString:[self environmentToAsk]]
                     : _installBarKey && [_installBarKey isEqualToString:self.lsp.missingServer]);
    if (!show) {
        bar.frame = NSZeroRect;
        return slot;
    }
    const CGFloat h = MIN(kMCInstallBarHeight, NSHeight(slot));
    bar.frame = NSMakeRect(NSMinX(slot), NSMaxY(slot) - h, NSWidth(slot), h);
    return NSMakeRect(NSMinX(slot), NSMinY(slot), NSWidth(slot), NSHeight(slot) - h);
}

- (void)applyInstallBarColors {
    AppSettings *cfg = [AppSettings shared];
    [self.installBar setBackground:[cfg background:Surface::Sidebar]
                              text:[cfg text:Surface::Sidebar]];
}

- (void)closeInstallBarMuting:(BOOL)mute {
    if (mute && _installBarKey) MCInstallBarSetMuted(_installBarKey, YES);
    _installBarKey = nil;
    self.installBar.hidden = YES;
    [self relayoutRightArea];
}

// The install popover for the open file's missing server, by `anchor` (the
// bar's Install… button or the status bar item).
- (void)showInstallHelpFrom:(NSView *)anchor {
    NSString *key = self.lsp.missingServer;
    if (!key) return;
    [_installPopover close];
    std::string setting = [AppSettings shared].settings.lspCommand(Utf8String(key));
    NSString *message = setting.empty() ? nil
        : [NSString stringWithFormat:@"lsp.%@ in the settings runs %@, which was not found. "
                                     @"The server minicode suggests is below.",
                                     key, @(setting.c_str())];
    __weak EditorController *weakSelf = self;
    MCInstallView *view = [[MCInstallView alloc]
        initWithServer:key message:message width:500
         runInTerminal:^(NSString *command) { [weakSelf typeCommandInTerminal:command]; }];
    if (!view.command) return;
    NSRectEdge edge = anchor == self.lspLabel ? NSRectEdgeMaxY : NSRectEdgeMinY;
    _installPopover = MCShowInstallPopover(view, anchor, anchor.bounds, edge);
}

// Run in Terminal: the command goes into a terminal's input line, and the
// keyboard with it, for the user to read and press Return. Never run here.
// A busy terminal is left alone and a new one opened, as Run File does.
- (void)typeCommandInTerminal:(NSString *)command {
    BOOL fresh = self.terminalPanel == nil;
    [self showTerminalPanel];
    TerminalView *term = self.terminalPanel.activeTerminal;
    if (!fresh && term.busy) term = [self.terminalPanel addTab];
    if (![term typeCommand:command]) {
        term = [self.terminalPanel addTab];
        [term typeCommand:command];
    }
    [self showStatusNote:@"Press Return in the terminal to run the command"];
}
- (void)restartLanguageServer:(id)sender {
    [self.lsp restartCurrentServer];
}
- (void)selectPythonInterpreter:(id)sender {
    MCChoosePythonInterpreter(self.lsp, self.window);
}

@end
