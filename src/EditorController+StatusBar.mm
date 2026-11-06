// EditorController+StatusBar.mm — EditorController: the status bar, its notes and "Saved ...", and the shortcut hints.
#import "EditorController+Private.h"
#import "EditorFont.h"

@implementation EditorController (StatusBar)

// -------------------------------------------------------- status bar + hints
- (void)buildStatusBarInContainer:(NSView *)container height:(CGFloat)barH {
    self.statusBar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, container.bounds.size.width, barH)];
    self.statusBar.wantsLayer = YES;   // colors set in applySettings
    self.statusBar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    self.statusBar.menu = MCAdjustSettingsMenu(@"statusbar", self.statusBar);

    NSTextField *label = [NSTextField labelWithString:@"⇧⌘H  Shortcuts"];
    label.font = [NSFont systemFontOfSize:11];
    label.backgroundColor = [NSColor clearColor];
    label.frame = NSMakeRect(0, 3, container.bounds.size.width - 34, 16);
    label.alignment = NSTextAlignmentRight;
    label.autoresizingMask = NSViewWidthSizable;
    self.statusLabel = label;
    [self.statusBar addSubview:label];

    // Far right: restart minicode, keeping its windows, tabs, unsaved
    // changes, browser pages and a conversation with Claude (restartMinicode:).
    NSButton *restart = [NSButton buttonWithTitle:@"↻" target:nil
                                           action:NSSelectorFromString(@"restartMinicode:")];
    restart.bordered = NO;
    restart.font = [NSFont systemFontOfSize:13];
    restart.toolTip = @"Restart minicode, keeping its windows, tabs, unsaved changes "
                      @"and browser pages";
    restart.frame = NSMakeRect(container.bounds.size.width - 28, 1, 22, 20);
    restart.autoresizingMask = NSViewMinXMargin;
    self.restartButton = restart;
    [self.statusBar addSubview:restart];

    // Left side: the first problem in the settings file, if any.
    NSTextField *errors = [NSTextField labelWithString:@""];
    errors.font = [NSFont systemFontOfSize:11];
    errors.frame = NSMakeRect(12, 3, container.bounds.size.width * 0.6, 16);
    errors.autoresizingMask = NSViewWidthSizable;
    errors.lineBreakMode = NSLineBreakByTruncatingTail;
    self.settingsLabel = errors;
    [self.statusBar addSubview:errors];

    // Left of them, with the tree's last-saved column on
    // (Option+Shift+Cmd+I): when the file on show was last saved.
    NSTextField *saved = [NSTextField labelWithString:@""];
    saved.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    saved.lineBreakMode = NSLineBreakByTruncatingTail;
    saved.hidden = YES;
    self.savedLabel = saved;
    [self.statusBar addSubview:saved];

    // Middle: the language server for the open file and its problem count,
    // right-aligned just left of the shortcuts hint.
    MCLspStatusField *lsp = [[MCLspStatusField alloc] initWithFrame:NSZeroRect];
    lsp.font = [NSFont systemFontOfSize:11];
    lsp.alignment = NSTextAlignmentRight;
    lsp.lineBreakMode = NSLineBreakByTruncatingHead;
    lsp.frame = NSMakeRect(container.bounds.size.width - 150 - 340, 3, 340, 16);
    lsp.autoresizingMask = NSViewMinXMargin;
    __weak EditorController *weakSelf = self;
    // A missing server: how to install it. Otherwise every server.
    lsp.onClick = ^{
        EditorController *s = weakSelf;
        if (s.lsp.missingServer) [s showInstallHelpFrom:s.lspLabel];
        else [s showLanguageServers:nil];
    };
    self.lspLabel = lsp;
    [self.statusBar addSubview:lsp];
    // Left of it: the window's problems, every open file's, clickable to
    // show the Problems list.
    MCLspStatusField *problems = [[MCLspStatusField alloc] initWithFrame:NSZeroRect];
    problems.font = [NSFont systemFontOfSize:11];
    problems.alignment = NSTextAlignmentRight;
    problems.lineBreakMode = NSLineBreakByTruncatingTail;
    problems.autoresizingMask = NSViewMinXMargin;
    problems.toolTip = @"Problems in the open files: click to list them (⇧⌘M)";
    problems.hidden = YES;
    problems.onClick = ^{ [weakSelf toggleProblems:nil]; };
    self.problemsLabel = problems;
    [self.statusBar addSubview:problems];
    [container addSubview:self.statusBar];
    [self layoutStatusRight];
}

// The right of the status bar: the problem counts, then the language
// server, each as wide as its text, ending where the shortcuts hint begins.
- (void)layoutStatusRight {
    if (!self.statusBar) return;
    const CGFloat W = self.statusBar.bounds.size.width;
    const CGFloat right = W - 150;
    CGFloat lw = MIN(340, ceil(self.lspLabel.attributedStringValue.size.width) + 6);
    self.lspLabel.frame = NSMakeRect(right - lw, 3, lw, 16);
    if (!self.problemsLabel.hidden) {
        CGFloat pw = MIN(220, ceil(self.problemsLabel.attributedStringValue.size.width) + 6);
        self.problemsLabel.frame = NSMakeRect(right - lw - 16 - pw, 3, pw, 16);
    }
}

- (void)buildHintsPanelInContainer:(NSView *)container {
    const CGFloat pw = 360, ph = 240, margin = 18;
    NSRect r = NSMakeRect(container.bounds.size.width - pw - margin,
                          container.bounds.size.height - ph - margin, pw, ph);
    self.hintsPanel = [[NSView alloc] initWithFrame:r];
    self.hintsPanel.wantsLayer = YES;
    self.hintsPanel.layer.backgroundColor =
        [NSColor colorWithSRGBRed:0.11 green:0.11 blue:0.12 alpha:0.97].CGColor;
    self.hintsPanel.layer.cornerRadius = 8;
    self.hintsPanel.layer.borderWidth = 1;
    self.hintsPanel.layer.borderColor = Hex(0x3C3C3C).CGColor;
    self.hintsPanel.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    self.hintsPanel.hidden = YES;

    NSTextField *lbl = [NSTextField labelWithString:@""];
    lbl.frame = NSMakeRect(16, 14, pw - 32, ph - 28);
    lbl.font = MCMonoFont(12, NSFontWeightRegular);
    lbl.textColor = Hex(0xE0E0E0);
    lbl.backgroundColor = [NSColor clearColor];
    lbl.maximumNumberOfLines = 0;
    lbl.lineBreakMode = NSLineBreakByWordWrapping;
    [self.hintsPanel addSubview:lbl];
    self.hintsLabel = lbl;
    [container addSubview:self.hintsPanel];
}

// Set the hint text and size the panel to fit it, anchored to the top-right.
- (void)updateHints {
    if (!self.hintsLabel) return;
    NSAttributedString *text = [self hintsText];
    self.hintsLabel.attributedStringValue = text;

    NSView *container = self.hintsPanel.superview;
    const CGFloat pw = kHintsWidth, pad = 16, margin = 18;
    NSRect textRect = [text boundingRectWithSize:NSMakeSize(pw - 2 * pad, 10000)
        options:NSStringDrawingUsesLineFragmentOrigin];
    CGFloat ph = ceil(textRect.size.height) + 2 * pad;
    self.hintsPanel.frame = NSMakeRect(
        container.bounds.size.width - pw - margin,
        container.bounds.size.height - [self titlebarHeight] - ph - margin, pw, ph);
    self.hintsLabel.frame = NSMakeRect(pad, pad, pw - 2 * pad, ph - 2 * pad);
}

// The hints are laid out on tab stops, never with spaces: the key glyphs
// (⌘ ⇧ ⌃ ⌫ ⏎) have different widths even in a monospaced font, so padding with
// spaces leaves every column ragged. A row holds one or two shortcuts:
//   key | label | key | label       (two shortcuts)
//   key | label | state             (a pane, with whether it is open)
static const CGFloat kHintsWidth = 420;
static const CGFloat kHintsLabel1 = 52, kHintsKey2 = 208, kHintsLabel2 = 260;

- (void)appendHintsHeading:(NSString *)title to:(NSMutableAttributedString *)s {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.paragraphSpacingBefore = s.length ? 10 : 0;
    ps.paragraphSpacing = 4;
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:[title.uppercaseString stringByAppendingString:@"\n"]
            attributes:@{
                NSFontAttributeName: [NSFont systemFontOfSize:10
                                                       weight:NSFontWeightSemibold],
                NSForegroundColorAttributeName: Hex(0x9CA3AF),
                NSKernAttributeName: @0.6,
                NSParagraphStyleAttributeName: ps,
            }]];
}

// One row. `key2`/`label2` may be nil; `state` (muted) takes the second
// column instead, for panes.
- (void)appendHintsRow:(NSMutableAttributedString *)s
                   key:(NSString *)key1 label:(NSString *)label1
                  key2:(NSString *)key2 label2:(NSString *)label2
                 state:(NSString *)state {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.tabStops = @[
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                        location:kHintsLabel1 options:@{}],
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                        location:kHintsKey2 options:@{}],
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                        location:kHintsLabel2 options:@{}],
    ];
    ps.lineSpacing = 3;
    NSDictionary *keyAttr = @{
        NSFontAttributeName: MCMonoFont(12, NSFontWeightMedium),
        NSForegroundColorAttributeName: Hex(0x4EA1F7),
        NSParagraphStyleAttributeName: ps,
    };
    NSDictionary *labelAttr = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12],
        NSForegroundColorAttributeName: Hex(0xE0E0E0),
        NSParagraphStyleAttributeName: ps,
    };
    NSDictionary *stateAttr = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12],
        NSForegroundColorAttributeName: Hex(0x9CA3AF),
        NSParagraphStyleAttributeName: ps,
    };
    void (^add)(NSString *, NSDictionary *) = ^(NSString *t, NSDictionary *a) {
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:t
                                                                  attributes:a]];
    };
    add(key1, keyAttr);
    add([@"\t" stringByAppendingString:label1], labelAttr);
    if (key2) {
        add([@"\t" stringByAppendingString:key2], keyAttr);
        add([@"\t" stringByAppendingString:label2 ?: @""], labelAttr);
    } else if (state) {
        add([@"\t" stringByAppendingString:state], stateAttr);
    }
    add(@"\n", labelAttr);
}

// A command's first key as the hints show it (from keys.conf and the
// registry, Keymap.h); "—" when keys.conf took every key away.
- (NSString *)hk:(NSString *)commandId {
    NSString *k = MCKeymapGlyphs(commandId);
    return k.length ? k : @"—";
}

// Every key of a command, "⌥↓ ⌘↓"; the one at index, "" when there is none;
// and a pane's title with its other keys, "Terminal  (also ⌘J, ⌃`)".
- (NSString *)hkAll:(NSString *)commandId {
    NSArray<NSString *> *all = MCKeymapAllGlyphs(commandId);
    return all.count ? [all componentsJoinedByString:@" "] : @"—";
}
- (NSString *)hk:(NSString *)commandId at:(NSUInteger)index {
    NSArray<NSString *> *all = MCKeymapAllGlyphs(commandId);
    return index < all.count ? all[index] : @"";
}
- (NSString *)hkAlso:(NSString *)commandId title:(NSString *)title {
    NSArray<NSString *> *all = MCKeymapAllGlyphs(commandId);
    if (all.count < 2) return title;
    NSArray *rest = [all subarrayWithRange:NSMakeRange(1, all.count - 1)];
    return [NSString stringWithFormat:@"%@  (also %@)", title, [rest componentsJoinedByString:@", "]];
}

// Context-aware shortcut list.
- (NSAttributedString *)hintsText {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSString *(^openOr)(BOOL) = ^NSString *(BOOL open) {
        return open ? @"open" : @"hidden";
    };

    [self appendHintsHeading:@"Editing" to:s];
    [self appendHintsRow:s key:[self hk:@"explorer.newFile"] label:@"New file"
                    key2:[self hk:@"workbench.action.files.openFolder"] label2:@"Open folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.files.save"] label:@"Save"
                    key2:[self hk:@"actions.find"] label2:@"Find in file" state:nil];
    [self appendHintsRow:s key:[self hk:@"undo"] label:@"Undo"
                    key2:[self hk:@"redo"] label2:@"Redo" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"editor.action.clipboardCopyAction"], [self hk:@"editor.action.clipboardCutAction"]] label:@"Copy, cut (the line if none)"
                    key2:[self hk:@"editor.action.selectAll"] label2:@"Select all" state:nil];
    [self appendHintsRow:s key:[self hk:@"expandLineSelection"] label:@"Select line"
                    key2:[self hk:@"editor.action.deleteLines"] label2:@"Delete line" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.commentLine"] label:@"Toggle comment"
                    key2:[self hk:@"workbench.action.findInFiles"] label2:@"Find in folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.startFindReplaceAction"] label:@"Replace in file"
                    key2:[self hk:@"workbench.action.replaceInFiles"] label2:@"Replace in folder" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"editor.action.nextMatchFindAction"], [self hk:@"editor.action.previousMatchFindAction"]] label:@"Next, previous match"
                    key2:[self hk:@"actions.findWithSelection"] label2:@"Use selection for find" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"toggleFindCaseSensitive"], [self hk:@"toggleFindWholeWord"]] label:@"Match case, whole word"
                    key2:[self hk:@"toggleFindRegex"] label2:@"Regular expression" state:nil];
    [self appendHintsRow:s key:@"⇧⌘1" label:@"Replace (in the find bar)"
                    key2:[self hk:@"editor.action.replaceAll"] label2:@"Replace all" state:nil];
    [self appendHintsRow:s key:@"⌥⌘L" label:@"Find in selection (in the bar)"
                    key2:[self hk:@"togglePreserveCase"] label2:@"Preserve case" state:nil];
    [self appendHintsRow:s key:@"⇧⌘drag" label:@"Column, or two clicks"
                    key2:@"⌥click" label2:@"Add a cursor" state:nil];
    [self appendHintsRow:s key:[self hkAll:@"editor.action.moveLinesDownAction"] label:@"Move line down"
                    key2:[self hkAll:@"editor.action.moveLinesUpAction"] label2:@"Move line up" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.copyLinesDownAction"] label:@"Copy line down"
                    key2:[self hk:@"editor.action.copyLinesUpAction"] label2:@"Copy line up" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.insertCursorBelow"] label:@"Add cursor below"
                    key2:[self hk:@"editor.action.insertCursorAbove"] label2:@"Add cursor above" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.addSelectionToNextFindMatch"] label:@"Add next occurrence"
                    key2:[self hk:@"editor.action.selectHighlights"] label2:@"Select all occurrences" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.markdown.toggleTask"] label:@"Toggle task (Markdown)"
                    key2:[self hk:@"minicode.showTodos"] label2:@"TODOs in folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.openSettings"] label:@"Settings"
                    key2:[self hk:@"editor.action.triggerSuggest"] label2:@"Complete" state:nil];
    [self appendHintsRow:s key:[self hk:@"inlineChat.start"] label:@"Inline agent"
                    key2:[self hk:@"editor.action.showHover"] label2:@"Hover info" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.revealDefinition"] label:@"Go to definition"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.goToReferences"] label:@"Find references"
                    key2:[self hk:@"editor.action.rename"] label2:@"Rename symbol" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.marker.next"] label:@"Next problem"
                    key2:[self hk:@"editor.action.marker.prev"] label2:@"Previous problem" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.quickFix"] label:@"Quick fixes"
                    key2:[self hk:@"editor.action.organizeImports"] label2:@"Organize imports" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.formatDocument"] label:@"Format document"
                    key2:@"⇥" label2:@"Take completion" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.sortLines"] label:@"Sort lines"
                    key2:[self hk:@"minicode.sortKeepSortedBlocks"] label2:@"Sort keep-sorted blocks" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.indentLines"] label:@"Indent lines (or ⇥)"
                    key2:[self hk:@"editor.action.outdentLines"] label2:@"Outdent lines (or ⇧⇥)" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.insertLineAfter"] label:@"Insert line below"
                    key2:[self hk:@"python.execSelectionInTerminal"] label2:@"Line to Python REPL" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"editor.action.fontZoomIn"], [self hk:@"editor.action.fontZoomOut"]] label:@"Zoom in, out"
                    key2:[self hk:@"editor.action.fontZoomReset"] label2:@"Reset zoom" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.toggleWordWrap"] label:@"Word wrap"
                    key2:nil label2:nil state:MCEditorWordWrap() ? @"on" : @"off"];
    [self appendHintsRow:s key:[self hk:@"editor.fold"] label:@"Fold"
                    key2:[self hk:@"editor.unfold"] label2:@"Unfold" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.foldAll"] label:@"Fold all"
                    key2:[self hk:@"editor.unfoldAll"] label2:@"Unfold all" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.toggleFold"] label:@"Toggle fold"
                    key2:@"⌃⌘1…7" label2:@"Fold level 1 to 7" state:nil];

    [self appendHintsHeading:@"Files" to:s];
    [self appendHintsRow:s key:[self hk:@"workbench.action.newWindow"] label:@"New window"
                    key2:[self hk:@"explorer.newFolder"] label2:@"New folder" state:nil];
    [self appendHintsRow:s key:@"⌘⌫" label:@"Trash (in the tree)"
                    key2:[self hk:@"workbench.files.action.refreshFilesExplorer"] label2:@"Refresh tree" state:nil];
    [self appendHintsRow:s key:[self hk:@"editor.action.rename"] label:@"Rename (in the tree)"
                    key2:@"Space" label2:@"Quick Look (in the tree)" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleHiddenFiles"] label:@"Show hidden files"
                    key2:[self hk:@"minicode.tree.toggleSizes"] label2:@"Sizes" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.tree.toggleSaved"] label:@"When last saved"
                    key2:[self hk:@"minicode.tree.toggleCreated"] label2:@"When created" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.files.saveAll"] label:@"Save all"
                    key2:[self hk:@"workbench.action.openRecent"] label2:@"Open recent folder" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.closeOtherEditors"] label:@"Close other tabs"
                    key2:nil label2:nil state:nil];

    [self appendHintsHeading:@"Navigation" to:s];
    [self appendHintsRow:s key:[self hk:@"workbench.action.showCommands"] label:@"Command palette"
                    key2:[self hk:@"workbench.action.quickOpen"] label2:@"Go to file" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.gotoLine"] label:@"Go to line"
                    key2:[self hk:@"editor.action.jumpToBracket"] label2:@"Go to bracket" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.gotoSymbol"] label:@"Go to symbol"
                    key2:[self hk:@"workbench.action.showAllSymbols"] label2:@"Symbol in workspace" state:nil];
    [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"workbench.action.navigateBack"], [self hk:@"workbench.action.navigateForward"]] label:@"Back, forward"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.focusSideBar"] label:@"Focus tree"
                    key2:[self hk:@"workbench.action.focusFirstEditorGroup"] label2:@"Focus editor" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.focus"] label:@"Focus terminal"
                    key2:[self hk:@"minicode.focusBrowser"] label2:@"Focus browser" state:nil];
    [self appendHintsRow:s key:[self hk:@"minicode.runFile"] label:@"Run the file in the terminal"
                    key2:[self hk:@"minicode.requestChange"] label2:@"Request a change to minicode" state:nil];
    [self appendHintsRow:s key:@"↑ ↓" label:@"Browse tree"
                    key2:@"⏎" label2:@"Open file" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.toggleSidebarVisibility"] label:@"Toggle sidebar"
                    key2:[self hk:@"editor.action.showContextMenu"] label2:@"Context menu" state:nil];

    [self appendHintsHeading:@"Tabs" to:s];
    [self appendHintsRow:s key:[self hk:@"workbench.action.nextEditor"] label:@"Next tab"
                    key2:[self hk:@"workbench.action.previousEditor"] label2:@"Previous tab" state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.nextEditor" at:1] label:@"Next tab"
                    key2:[self hk:@"workbench.action.previousEditor" at:1] label2:@"Previous tab" state:nil];
    [self appendHintsRow:s key:@"⌃1…9" label:@"Tab 1 to 9 (also ⇧⌘1…9; 9 is the last)"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.closeActiveEditor"] label:@"Close tab, or the focused panel"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.reopenClosedEditor"] label:@"Reopen closed tab"
                    key2:nil label2:nil state:nil];
    [self appendHintsRow:s key:[self hk:@"workbench.action.closeWindow"] label:@"Close window"
                    key2:[self hk:@"workbench.action.quit"] label2:@"Quit" state:nil];

    [self appendHintsHeading:@"Panes" to:s];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleEditor"] label:@"Editor"
                    key2:nil label2:nil state:openOr(!self.editorHidden)];
    [self appendHintsRow:s key:@"⇧⌘T" label:[self hkAlso:@"workbench.action.terminal.toggleTerminal" title:@"Terminal"]
                    key2:nil label2:nil state:openOr(self.terminalVisible)];
    if (self.terminalVisible) {
        [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.new"] label:@"New terminal"
                        key2:[self hk:@"workbench.action.terminal.split"] label2:@"Split terminal" state:nil];
        [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.focusNext"] label:@"Next terminal"
                        key2:[self hk:@"workbench.action.terminal.focusPrevious"] label2:@"Previous" state:nil];
        [self appendHintsRow:s key:[self hk:@"workbench.action.terminal.kill"] label:@"Close terminal"
                        key2:nil label2:nil state:nil];
        [self appendHintsRow:s key:@"⌘ click" label:@"Open a file:line or URL in the terminal"
                        key2:nil label2:nil state:nil];
        [self appendHintsRow:s key:[self hk:@"actions.find"] label:@"Find in the terminal"
                        key2:[self hk:@"workbench.action.terminal.clear"] label2:@"Clear the terminal" state:nil];
        if (!self.terminal.lineMode) {
            [self appendHintsRow:s key:@"⇧⇞" label:@"Scroll back"
                            key2:@"⌘↖" label2:@"Top of the scrollback" state:nil];
            [self appendHintsRow:s key:@"⌘← ⌘→" label:@"Start, end of the line"
                            key2:@"⌘⌫" label2:@"Delete to its start" state:nil];
            [self appendHintsRow:s
                             key:[NSString stringWithFormat:@"%@ %@",
                                  [self hk:@"workbench.action.terminal.scrollToPreviousCommand"],
                                  [self hk:@"workbench.action.terminal.scrollToNextCommand"]]
                           label:@"Previous, next command"
                            key2:[self hk:@"workbench.action.terminal.selectToPreviousCommand"]
                          label2:@"Select it" state:nil];
        }
    }
    [self appendHintsRow:s key:[self hk:@"minicode.toggleBrowser"] label:@"Browser"
                    key2:nil label2:nil state:openOr(self.browserVisible)];
    if (self.browserVisible) {
        // A page's own keys, kept off the menus (Keymap.mm).
        [self appendHintsRow:s key:[NSString stringWithFormat:@"%@ %@", [self hk:@"minicode.page.zoomIn"],
                                    [self hk:@"minicode.page.zoomOut"]]
                       label:@"Zoom the page or a PDF"
                        key2:[self hk:@"minicode.page.actualSize"] label2:@"Actual size" state:nil];
    }
    [self appendHintsRow:s key:[self hk:@"minicode.toggleMaximizedPane"] label:@"Maximize pane"
                    key2:nil label2:nil
                   state:self.maximizedPane != MCPaneNone ? @"maximized" : nil];
    [self appendHintsRow:s key:[self hk:@"workbench.view.scm"] label:@"Source control"
                    key2:nil label2:nil state:openOr(self.gitPanelVisible)];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleAgents"] label:@"Agents"
                    key2:nil label2:nil state:openOr(self.agentsVisible)];
    [self appendHintsRow:s key:[self hk:@"workbench.actions.view.problems"] label:@"Problems"
                    key2:nil label2:nil state:openOr(self.problemsVisible)];
    if (self.problemsVisible) {
        [self appendHintsRow:s key:@"⏎" label:@"Go to the problem"
                        key2:@"⎋" label2:@"Back to the editor" state:nil];
    }
    if (self.agentsVisible) {
        [self appendHintsRow:s key:@"⏎" label:@"Evidence, or open the file"
                        key2:@"Space" label2:@"Expand or collapse" state:nil];
    }
    if (self.gitPanelVisible) {
        [self appendHintsRow:s key:@"⏎" label:@"Show the diff or commit"
                        key2:@"Space" label2:@"Stage or unstage" state:nil];
        [self appendHintsRow:s key:@"⇥" label:@"Changes, graph, message"
                        key2:@"⌘⏎" label2:@"Commit (in the message)" state:nil];
    }
    if (self.isMarkdown) {
        [self appendHintsRow:s key:[self hk:@"markdown.showPreview"] label:@"Markdown preview"
                        key2:nil label2:nil
                       state:self.previewMode ? @"rendered" : @"source"];
    } else if (self.isLatex) {
        [self appendHintsRow:s key:[self hk:@"markdown.showPreview"] label:@"LaTeX preview"
                        key2:nil label2:nil
                       state:self.previewMode ? @"typeset" : @"source"];
        [self appendHintsRow:s key:[self hk:@"minicode.latex.exportPDF"] label:@"Export PDF"
                        key2:nil label2:nil state:nil];
    }
    if (self.isMedia) {
        [self appendHintsRow:s key:[self hk:@"minicode.togglePlayback"] label:@"Play or pause"
                        key2:[self hk:@"workbench.action.focusFirstEditorGroup"] label2:@"Focus the player" state:nil];
    }
    if (_notes.tourActive) {
        [self appendHintsRow:s key:[self hk:@"minicode.tour.next"] label:@"Next stop of the tour"
                        key2:[self hk:@"minicode.tour.previous"] label2:@"Previous stop" state:nil];
    }

    // A short gap, not a whole empty heading, before the way out.
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:6]}]];
    [self appendHintsRow:s key:[self hk:@"minicode.toggleShortcutHints"] label:@"Hide these hints"
                    key2:nil label2:nil state:nil];
    // No trailing newline, or the panel sizes itself one empty line too tall.
    if ([s.string hasSuffix:@"\n"])
        [s deleteCharactersInRange:NSMakeRange(s.length - 1, 1)];
    return s;
}

- (void)toggleHints:(id)sender {
    self.hintsVisible = !self.hintsVisible;
    if (self.hintsVisible) [self updateHints];
    self.hintsPanel.hidden = !self.hintsVisible;
    self.statusLabel.stringValue =
        self.hintsVisible ? @"⇧⌘H  Hide shortcuts" : @"⇧⌘H  Shortcuts";
}

// A passing note on the left of the status bar, where settings problems
// show; they come back when it goes.
- (void)showStatusNote:(NSString *)note {
    if (!note.length) return;
    self.settingsLabel.stringValue = note;
    self.settingsLabel.toolTip = note;
    NSString *shown = [note copy];
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(6 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        EditorController *s = weakSelf;
        if ([s.settingsLabel.stringValue isEqualToString:shown]) [s showSettingsErrors];
    });
}

// The status bar's "Saved 14:32" for the tab on show, with details on.
- (void)updateSavedStatus {
    NSString *text = @"", *tip = nil;
    MCTab *t = _activeTab;
    if (MCTreeColumnShown(MCTreeColumnSaved) && t.currentPath && !t.isDiff) {
        NSDate *disk = t.fileModDate;
        if (t.gone || !disk) text = @"Not saved";
        else if (t.dirty) text = @"Unsaved changes";
        else {
            text = MCSavedPhrase(disk, [NSDate date]);
            // Pictures, PDFs and media are never saved from here.
            if ((t.isImage || t.isPDF || t.isMedia || MCViewerKindWithoutText(t.viewerKind)) &&
                [text hasPrefix:@"Saved"])
                text = [@"Modified" stringByAppendingString:[text substringFromIndex:5]];
        }
        if (disk) tip = [@"On disk since " stringByAppendingString:MCFullDate(disk)];
    }
    self.savedLabel.stringValue = text;
    self.savedLabel.toolTip = tip;
    self.savedLabel.hidden = text.length == 0;
    [self layoutStatusLabels];
}

// The left of the status bar: "Saved ..." then any settings problem.
- (void)layoutStatusLabels {
    if (!self.statusBar) return;
    const CGFloat W = self.statusBar.bounds.size.width;
    CGFloat x = 12;
    if (!self.savedLabel.hidden) {
        const CGFloat w = MIN(ceil(self.savedLabel.fittingSize.width) + 2, 220);
        self.savedLabel.frame = NSMakeRect(x, 3, w, 16);
        x += w + 14;
    }
    self.settingsLabel.frame = NSMakeRect(x, 3, MAX(0, W * 0.6 - (x - 12)), 16);
}

// A message in the status bar for a few seconds, in place of the hints key.
- (void)flashStatus:(NSString *)message {
    if (!message.length) return;
    self.statusLabel.stringValue = message;
    self.statusLabel.toolTip = message;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(endFlash)
                                               object:nil];
    [self performSelector:@selector(endFlash) withObject:nil afterDelay:6];
}
- (void)endFlash {
    self.statusLabel.stringValue =
        self.hintsVisible ? @"⇧⌘H  Hide shortcuts" : @"⇧⌘H  Shortcuts";
    self.statusLabel.toolTip = nil;
}

@end
