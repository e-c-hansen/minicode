// EditorController+Agents.mm — EditorController: the inline agent, Request a Change, the control channel and agent work.
#import "EditorController+Private.h"

@implementation EditorController (Agents)

// ------------------------------------------------ the inline agent (Cmd+I)
// See AgentPanel.h. What has the keyboard decides what the agent does: the
// editor's file is edited, a terminal gets a command, anything else becomes
// a task for Claude Code in a terminal tab. The context built here is all
// the agent knows of the window.
- (MCAgentSession *)agentSession {
    if (!_agent) _agent = [[MCAgentSession alloc] initWithHost:self];
    return _agent;
}
- (NSView *)agentBarHost { return self.rightArea; }
- (void)agentBarDidChange {
    [self relayoutRightArea];
    // Keep or Discard (or closing the bar) ends a pending edit: auto-save
    // goes on: after the delay, or for focus-change now (the keyboard was
    // in the bar, away from the editor).
    if (!_agent.editPending) {
        [self scheduleAutoSave];
        [self autoSaveOnFocusChange];
    }
}
- (void)agentRunInTerminal:(NSString *)command {
    [self changeTerminalRunning:command focus:YES];
}

// A path as the agent is told it: from the open folder, when it is inside.
- (NSString *)agentPathFor:(NSString *)path {
    if (!path.length) return @"";
    NSString *rel = [self pathFromRoot:path];
    return rel.length ? rel : path;
}

// 1-based line of a UTF-16 offset. Read a block at a time: characterAtIndex
// on the editor's string costs a message per character.
static int MCLineAt(NSString *text, NSUInteger index) {
    index = MIN(index, text.length);
    int line = 1;
    unichar buf[4096];
    for (NSUInteger at = 0; at < index; at += 4096) {
        const NSUInteger n = MIN((NSUInteger)4096, index - at);
        [text getCharacters:buf range:NSMakeRange(at, n)];
        for (NSUInteger i = 0; i < n; i++)
            if (buf[i] == '\n') line++;
    }
    return line;
}

// The start of a 0-based source line, in UTF-16.
static NSUInteger MCLineStart(NSString *text, int line) {
    if (line <= 0) return 0;
    unichar buf[4096];
    int l = 0;
    for (NSUInteger at = 0; at < text.length; at += 4096) {
        const NSUInteger n = MIN((NSUInteger)4096, text.length - at);
        [text getCharacters:buf range:NSMakeRange(at, n)];
        for (NSUInteger i = 0; i < n; i++)
            if (buf[i] == '\n' && ++l == line) return at + i + 1;
    }
    return text.length;
}

// The line fragment holding character `index`, in the text view's
// coordinates (the editor is TextKit 1; see LSP in CLAUDE.md). Only the
// layout up to that character is asked for: the whole text's took 1.9 s
// in a 100,000-line file the first time the bar opened, before background
// layout had got there.
- (NSRect)agentLineRectAt:(NSUInteger)index {
    CodeTextView *tv = self.textView;
    NSLayoutManager *lm = tv.layoutManager;
    const NSUInteger length = tv.string.length;
    if (length) [lm ensureLayoutForCharacterRange:NSMakeRange(MIN(index, length - 1), 1)];
    NSRect r;
    if (index >= length && !NSIsEmptyRect(lm.extraLineFragmentRect)) {
        r = lm.extraLineFragmentRect;
    } else if (length == 0) {
        r = NSMakeRect(0, 0, 10, [lm defaultLineHeightForFont:tv.font ?: [NSFont systemFontOfSize:13]]);
    } else {
        // numberOfGlyphs would generate the glyphs of the whole text.
        NSUInteger glyph = [lm glyphIndexForCharacterAtIndex:MIN(index, length - 1)];
        r = [lm lineFragmentRectForGlyphAtIndex:glyph effectiveRange:NULL];
    }
    const NSPoint o = tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

- (void)agentTintAgainst:(NSString *)original {
    CodeTextView *tv = self.textView;
    NSLayoutManager *lm = tv.layoutManager;
    const NSRange all = NSMakeRange(0, tv.string.length);
    [lm removeTemporaryAttribute:NSBackgroundColorAttributeName forCharacterRange:all];
    if (!original || !tv.editable) return;
    NSColor *tint = [[NSColor colorWithSRGBRed:0x73 / 255.0 green:0xC9 / 255.0 blue:0x91 / 255.0
                                         alpha:1] colorWithAlphaComponent:0.2];
    for (const LineDiff::Hunk &h : LineDiff::diff(U16(original), U16(tv.string)))
        if (h.newLength > 0 && h.newStart + h.newLength <= all.length)
            [lm addTemporaryAttribute:NSBackgroundColorAttributeName value:tint
                    forCharacterRange:NSMakeRange(h.newStart, h.newLength)];
}

- (void)inlineAgent:(id)sender {
    (void)sender;
    NSWindow *w = self.window;
    NSString *otherWindow = nil;
    NSWindow *key = NSApp.keyWindow;
    if (key && key != w && !key.sheet && key.parentWindow != w) {
        otherWindow = key.title.length ? key.title : @"another";
        [w makeKeyAndOrderFront:nil];
    }
    NSResponder *fr = w.firstResponder;
    NSView *view = [fr isKindOfClass:NSView.class] ? (NSView *)fr : nil;
    // A text field's field editor stands for the field.
    if ([fr isKindOfClass:NSTextView.class] && ((NSTextView *)fr).isFieldEditor &&
        [((NSTextView *)fr).delegate isKindOfClass:NSView.class])
        view = (NSView *)((NSTextView *)fr).delegate;
    if (_agent.shown && view && [view isDescendantOf:_agent.bar]) return;   // already there

    [self showRightArea];
    MCAgentContext *ctx = [MCAgentContext new];
    ctx.root = self.rootPath;
    ctx.returnFocus = fr;
    ctx.path = [self agentPathFor:self.currentPath];
    __weak EditorController *weakSelf = self;
    // Anywhere: under the top of the right pane, in its middle.
    ctx.area = ^NSRect { return weakSelf.rightArea.bounds; };
    ctx.anchor = ^NSRect {
        const NSRect b = weakSelf.rightArea.bounds;
        return NSMakeRect(NSMidX(b) - 360, NSMaxY(b) - 6, 0, 0);
    };

    CodeTextView *tv = self.textView;
    const BOOL inEditor = view == tv && !tv.isHiddenOrHasHiddenAncestor;
    const BOOL preview = self.isMarkdown && self.previewMode && !tv.editable;
    TerminalView *term = self.terminalPanel.activeTerminal;
    const BOOL inTerminal = !otherWindow && term && view && [view isDescendantOf:self.terminalPanel];

    // The editor's selection, for a task as much as an edit.
    if (inEditor && self.currentPath && tv.editable && !self.showingMessage) {
        const NSRange sel = tv.selectedRange;
        if (sel.length) {
            NSString *text = tv.string;
            ctx.selectionText = [text substringWithRange:sel];
            ctx.firstLine = MCLineAt(text, sel.location);
            ctx.lastLine = MCLineAt(text, NSMaxRange(sel) -
                                    ([text characterAtIndex:NSMaxRange(sel) - 1] == '\n' &&
                                     sel.length > 1 ? 1 : 0));
        }
    }

    if (!otherWindow && inEditor && self.currentPath && !self.showingMessage &&
        (tv.editable || preview)) {
        MCTab *tab = _activeTab;
        __weak MCTab *weakTab = tab;
        ctx.target = MCAgentTargetEdit;
        ctx.owner = tab;
        ctx.where = [NSString stringWithFormat:@"%@ in the editor", ctx.path];
        ctx.currentText = ^NSString * {
            EditorController *s = weakSelf;
            if (!s || !weakTab || weakTab != s->_activeTab) return nil;
            // Copies: the text view's string is its live storage.
            if (s.textView.editable) return [s.textView.string copy];
            if (s.isMarkdown && s.previewMode) return [s.sourceText copy] ?: @"";
            return nil;
        };
        ctx.applyText = ^BOOL(NSString *text, NSString *name) {
            EditorController *s = weakSelf;
            if (!s || !weakTab || weakTab != s->_activeTab) return NO;
            if (s.textView.editable) {
                [s replaceSourceWithText:text actionName:name];
                return YES;
            }
            if (s.isMarkdown && s.previewMode) {
                [s applyMarkdownSource:text];
                return YES;
            }
            return NO;
        };
        if (preview) {
            // The source line the preview's caret sits in.
            NSRange sel = NSMakeRange(0, 0);
            NSTextStorage *st = tv.textStorage;
            if (st.length) {
                NSNumber *line = [st attribute:kMarkdownSourceLine
                                       atIndex:MIN(tv.selectedRange.location, st.length - 1)
                                effectiveRange:NULL];
                if (line) sel.location = MCLineStart(self.sourceText ?: @"", line.intValue);
            }
            ctx.selection = sel;
            ctx.hint = [NSString stringWithFormat:@"Editing %@ from its preview",
                                                  self.currentPath.lastPathComponent];
            ctx.anchor = ^NSRect {
                const NSRect f = weakSelf.editorScroll.frame;
                return NSMakeRect(NSMinX(f) + 24, NSMaxY(f) - 6, 0, 0);
            };
        } else {
            ctx.selection = tv.selectedRange;
            ctx.currentSelection = ^NSRange { return weakSelf.textView.selectedRange; };
            ctx.tint = ^(NSString *original) { [weakSelf agentTintAgainst:original]; };
            NSString *lines = ctx.selectionText
                ? (ctx.firstLine == ctx.lastLine
                       ? [NSString stringWithFormat:@"line %d", ctx.firstLine]
                       : [NSString stringWithFormat:@"lines %d to %d", ctx.firstLine, ctx.lastLine])
                : [NSString stringWithFormat:@"line %d", MCLineAt(tv.string, tv.selectedRange.location)];
            ctx.hint = [NSString stringWithFormat:@"Editing %@, %@",
                                                  self.currentPath.lastPathComponent, lines];
            ctx.followScroll = self.editorScroll.contentView;
            // Under the selection's last line, or over its first.
            ctx.anchor = ^NSRect {
                EditorController *s = weakSelf;
                CodeTextView *t = s.textView;
                const NSRange r = t.selectedRange;
                const NSUInteger len = t.string.length;
                NSRect u = NSUnionRect([s agentLineRectAt:MIN(r.location, len)],
                                       [s agentLineRectAt:MIN(r.length ? NSMaxRange(r) - 1
                                                                       : r.location, len)]);
                u = [s.rightArea convertRect:u fromView:t];
                u.origin.x = NSMinX(s.editorScroll.frame) + 24;
                u.size.width = 0;
                return u;
            };
            ctx.area = ^NSRect { return weakSelf.editorScroll.frame; };
        }
    } else if (inTerminal && !term.busy) {
        __weak TerminalView *weakTerm = term;
        ctx.target = MCAgentTargetCommand;
        ctx.owner = term;
        NSString *dir = term.directory ?: self.rootPath;
        ctx.directory = dir;
        ctx.screen = [term recentText:60];
        ctx.where = [NSString stringWithFormat:@"a terminal in %@", dir];
        ctx.hint = [NSString stringWithFormat:@"A command for %@",
                                              [dir stringByAbbreviatingWithTildeInPath]];
        ctx.typeCommand = ^BOOL(NSString *command) {
            EditorController *s = weakSelf;
            TerminalView *t = weakTerm;
            if (!s || ![s terminalIsOpen:t] || t.busy || ![t typeCommand:command]) return NO;
            [s focusChangeTerminal:t];
            return YES;
        };
        ctx.area = ^NSRect { return weakSelf.terminalPanel.frame; };
        ctx.anchor = ^NSRect {
            const NSRect f = weakSelf.terminalPanel.frame;
            return NSMakeRect(NSMinX(f) + 24, NSMinY(f) + 4, 0, 0);
        };
        ctx.anchorAbove = YES;
    } else {
        ctx.target = MCAgentTargetTask;
        NSString *where = nil;
        if (otherWindow) {
            where = [NSString stringWithFormat:@"the %@ window", otherWindow];
        } else if (inTerminal) {
            ctx.screen = [term recentText:60];
            where = [NSString stringWithFormat:@"a terminal running %@",
                                               term.runningCommand ?: @"a program"];
            ctx.owner = term;
        } else if (view && (view == self.outline || [view isDescendantOf:self.outline])) {
            FileItem *item = [self clickedOrSelectedItem];
            where = item ? [NSString stringWithFormat:@"the file tree, with %@ selected",
                                                      [self agentPathFor:item.path]]
                         : @"the file tree";
            ctx.owner = self.outline;
        } else if (view && self.gitPanel && [view isDescendantOf:self.gitPanel]) {
            where = @"the Source Control panel";
            ctx.owner = self.gitPanel;
        } else if (view && self.browser && [view isDescendantOf:self.browser]) {
            NSString *url = self.browser.pageURL;
            NSString *title = self.browser.pageTitle;
            where = url ? [NSString stringWithFormat:@"the browser panel, showing %@%@", url,
                                                     title ? [NSString stringWithFormat:@" (%@)", title] : @""]
                        : @"the browser panel";
            ctx.owner = self.browser;
        } else if (self.isLatex && self.previewMode && self.currentPath) {
            where = [NSString stringWithFormat:@"the LaTeX preview of %@", ctx.path];
            ctx.owner = _activeTab;
        } else if (self.isDiff) {
            where = self.diffIsCommit ? @"a commit shown in the editor" : @"a diff shown in the editor";
        } else if (self.currentPath) {
            where = [NSString stringWithFormat:@"the editor, showing %@", ctx.path];
            ctx.owner = _activeTab;
        } else {
            where = @"the minicode window";
        }
        ctx.where = where;
        ctx.hint = @"For Claude Code, in a new terminal tab";
    }
    [[self agentSession] beginWithContext:ctx];
}

// ----------------------------------------- Request a Change (Shift+Cmd+K)
// See ChangePanel.h: a bar for the request, Claude Code in a terminal tab of
// minicode's source folder, and a restart into the new build.
- (MCChangeSession *)changeSession {
    if (!_change) _change = [[MCChangeSession alloc] initWithHost:self];
    return _change;
}

- (void)requestChange:(id)sender {
    (void)sender;
    MCChangeSession *s = [self changeSession];
    if (!s.bar.superview) {
        [self applyChangeBarColors];
        [self.rightArea addSubview:s.bar];
    }
    [self showRightArea];
    [s begin];
}

- (void)resumeChangeWithSource:(NSString *)source command:(NSString *)command {
    MCChangeSession *s = [self changeSession];
    if (!s.bar.superview) {
        [self applyChangeBarColors];
        [self.rightArea addSubview:s.bar];
    }
    [s resumeWithSource:source command:command];
}

- (NSRect)layoutChangeBarIn:(NSRect)area {
    MCChangeBar *bar = _change ? _change.bar : nil;
    if (!bar) return area;
    if (!_change.barShown) {
        bar.hidden = YES;
        return area;
    }
    bar.hidden = NO;
    const CGFloat h = MIN([bar heightForWidth:NSWidth(area)], NSHeight(area));
    bar.frame = NSMakeRect(NSMinX(area), NSMaxY(area) - h, NSWidth(area), h);
    // Above the panes, which are added later and would cover it.
    if (self.rightArea.subviews.lastObject != bar)
        [self.rightArea addSubview:bar positioned:NSWindowAbove relativeTo:nil];
    return NSMakeRect(NSMinX(area), NSMinY(area), NSWidth(area), NSHeight(area) - h);
}

- (void)applyChangeBarColors {
    if (!_change) return;
    AppSettings *cfg = [AppSettings shared];
    [_change.bar setBackground:[cfg background:Surface::Sidebar]
                          text:[cfg text:Surface::Sidebar]
                         field:[cfg background:Surface::Editor]];
}

// Restart minicode: a conversation with Claude from Request a Change, still
// running here, carries over as it does after a rebuild.
- (NSString *)changeSourceToResume {
    return [self changeTerminal] ? _change.source : nil;
}

// MCChangeHost
- (TerminalView *)changeTerminalRunning:(NSString *)command focus:(BOOL)focus {
    NSResponder *keep = self.window.firstResponder;
    BOOL fresh = self.terminalPanel == nil;
    [self showTerminalPanel];
    TerminalView *term = self.terminalPanel.activeTerminal;
    // A dock made just now has one idle terminal; otherwise a new tab, so
    // nothing the user was doing is typed over.
    if (!fresh || term.busy) term = [self.terminalPanel addTab];
    // Claude's screen wants room.
    if (self.terminalHeight < 320 && !self.editorHidden) {
        self.terminalHeight = MAX(self.terminalHeight,
                                  MIN(420, round(NSHeight(self.rightArea.bounds) * 0.55)));
        [self relayoutRightArea];
    }
    [term runCommand:command];
    if (focus) [self focusChangeTerminal:term];
    else if (keep && keep != self.window && self.window.firstResponder != keep)
        [self.window makeFirstResponder:keep];
    return term;
}

- (void)runCommand:(NSString *)command inTerminal:(TerminalView *)terminal focus:(BOOL)focus {
    [self showTerminalPanel];
    [terminal runCommand:command];
    if (focus) [self focusChangeTerminal:terminal];
}

- (BOOL)terminalIsOpen:(TerminalView *)terminal {
    return terminal && [self.terminalPanel.terminals containsObject:terminal];
}

- (void)focusChangeTerminal:(TerminalView *)terminal {
    if (![self terminalIsOpen:terminal]) return;
    [self showTerminalPanel];
    for (NSUInteger tab = 0; tab < self.terminalPanel.tabCount; tab++)
        if ([[self.terminalPanel terminalsInTab:tab] containsObject:terminal]) {
            [self.terminalPanel selectTab:tab];
            break;
        }
    [terminal focusInput];
}

- (void)changeBarDidChange {
    [self relayoutRightArea];
}

- (void)restartIntoApp:(NSString *)appPath source:(NSString *)source
               command:(NSString *)command {
    id<MCRestarting> app = (id<MCRestarting>)NSApp.delegate;
    if (![app conformsToProtocol:@protocol(MCRestarting)]) { NSBeep(); return; }
    [app restartIntoApp:appPath resumingIn:self source:source command:command];
}

// Claude's terminal, which a restart resumes rather than asks about.
- (TerminalView *)changeTerminal {
    TerminalView *t = _change.terminal;
    return [self terminalIsOpen:t] ? t : nil;
}

- (NSArray<NSString *> *)runningTerminalCommandsExceptChange {
    if (!self.terminalPanel) return @[];
    NSMutableArray<TerminalView *> *others = [self.terminalPanel.terminals mutableCopy];
    TerminalView *t = [self changeTerminal];
    if (t) [others removeObject:t];
    return MCRunningCommands(others);
}

- (MCAgentNotes *)agentNotes {
    if (!_notes) _notes = [[MCAgentNotes alloc] initWithHost:self];
    return _notes;
}

// The first responder, and a text field's selection, to put back after
// opening and showing things for an agent (a field takes the keyboard back
// with all its text selected).
- (void)keepKeyboardDuring:(void (^)(void))block {
    NSResponder *keep = self.window.firstResponder;
    NSRange fieldSelection = NSMakeRange(NSNotFound, 0);
    if ([keep isKindOfClass:NSTextView.class] && ((NSTextView *)keep).isFieldEditor) {
        fieldSelection = ((NSTextView *)keep).selectedRange;
        id field = ((NSTextView *)keep).delegate;
        if ([field isKindOfClass:NSResponder.class]) keep = field;
    }
    block();
    if (!keep || keep == self.window || self.window.firstResponder == keep) return;
    if ([keep isKindOfClass:NSTextField.class] &&
        [(NSTextField *)keep currentEditor]) return;   // it has it already
    [self.window makeFirstResponder:keep];
    if (fieldSelection.location != NSNotFound && [keep isKindOfClass:NSTextField.class]) {
        NSText *editor = [(NSTextField *)keep currentEditor];
        if (NSMaxRange(fieldSelection) <= editor.string.length) editor.selectedRange = fieldSelection;
    }
}

- (NSTextView *)notesTextView {
    if (!_activeTab || !self.currentPath || self.showingMessage || self.isImage || self.isPDF ||
        self.isMedia || self.isDiff)
        return nil;
    // Lines are in the source, so a Markdown or LaTeX preview shows its source.
    if (self.previewMode && self.canTogglePreview)
        [self keepKeyboardDuring:^{ [self togglePreview:nil]; }];
    return self.previewMode ? nil : self.textView;
}

- (id)notesActiveTab { return _activeTab; }

- (BOOL)openFileForAgent:(NSString *)path preview:(BOOL)preview {
    if (!path.length) return NO;
    [self keepKeyboardDuring:^{
        MCTab *existing = [self tabForPath:path];
        if (existing) {
            [self activateTab:existing];   // open already: shown where it is
            return;
        }
        if (!preview) {
            [self openFileAtPath:path];
            return;
        }
        // The window's one preview tab takes the file, in its place.
        MCTab *old = nil;
        for (MCTab *t in self->_tabs)
            if (t.agentPreview) { old = t; break; }
        MCTab *tab = [MCTab new];
        tab.currentPath = path;
        tab.agentPreview = YES;
        if (old) [self->_tabs insertObject:tab atIndex:[self->_tabs indexOfObject:old]];
        else [self insertTab:tab];
        [self activateTab:tab];
        if (old) [self dropAgentPreviewTab:old];
    }];
    return [self tabForPath:path] == _activeTab && _activeTab != nil;
}

// The preview tab a newer one replaced: no question (it never has unsaved
// changes, since an edit makes it an ordinary tab) and no place in Reopen
// Closed Tab.
- (void)dropAgentPreviewTab:(MCTab *)t {
    if (t == _activeTab || ![_tabs containsObject:t]) return;
    [self discardBackupOf:t];
    [t.latex removeFromSuperview];
    [_tabs removeObject:t];
    [self.lsp tabsChanged];
    [self reloadTabStrip];
    [self saveSession];
}

// A double-click on the preview tab's title keeps it, as in VS Code.
- (void)keepAgentPreviewTabAtIndex:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_tabs.count) return;
    MCTab *t = _tabs[(NSUInteger)i];
    if (!t.agentPreview) return;
    t.agentPreview = NO;
    [self reloadTabStrip];
    [self saveSession];
}

- (void)showComparison:(MCComparison *)comparison preview:(BOOL)preview {
    if (!comparison) return;
    [self keepKeyboardDuring:^{
        [self revealEditor];
        MCTab *old = nil;
        if (preview)
            for (MCTab *t in self->_tabs)
                if (t.agentPreview) { old = t; break; }
        MCTab *tab = [MCTab new];
        tab.loaded = YES;
        tab.isDiff = YES;
        tab.comparison = comparison;
        tab.diffName = comparison.title;
        tab.agentPreview = preview;
        if (old) [self->_tabs insertObject:tab atIndex:[self->_tabs indexOfObject:old]];
        else [self insertTab:tab];
        [self compareViewMade];
        [self activateTab:tab];
        if (old) [self dropAgentPreviewTab:old];
    }];
}

// A page in the editor's slot (EditorPage.h), in a tab of its own; a page
// with the same key as one already open selects that tab instead.
- (void)showPage:(id<MCEditorPage>)page {
    if (!page) return;
    for (MCTab *t in _tabs)
        if (t.page && [t.page.pageKey isEqualToString:page.pageKey]) {
            [self revealEditor];
            [self activateTab:t];
            [self focusActiveDocument];
            return;
        }
    [self revealEditor];
    MCTab *tab = [MCTab new];
    tab.loaded = YES;
    tab.isDiff = YES;
    tab.page = page;
    tab.diffName = page.pageTitle;
    [self insertTab:tab];
    [self activateTab:tab];
    [self focusActiveDocument];
}

- (void)closePageOf:(MCTab *)t {
    if (!t.page) return;
    [t.page.pageView removeFromSuperview];
    if ([t.page respondsToSelector:@selector(pageClosed)]) [t.page pageClosed];
}

// ------------------------------------------------------------ agent work
// The model's worktrees changed: the tree's Worktrees rows say so.
- (void)agentWorktreesChanged {
    if (_worktreesItem) [self refreshTreeCells];
}

- (BOOL)agentWorktreesOnShow {
    return _worktreesItem && [self sidebarViewOnShow] == self.sidebarScroll &&
           [self.outline isItemExpanded:_worktreesItem];
}

- (void)agentFolderChangedOnDisk:(NSString *)path {
    [_agents folderChangedOnDisk:path];
}

// The side-by-side view, made on first use in the editor's slot.
- (MCCompareView *)compareViewMade {
    if (!self.compareView) {
        self.compareView = [[MCCompareView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
        self.compareView.hidden = YES;
        [self.rightArea addSubview:self.compareView positioned:NSWindowBelow
                        relativeTo:self.termDivider];
    }
    return self.compareView;
}

- (TerminalView *)newTerminalForAgentIn:(NSString *)dir beside:(BOOL)beside {
    __block TerminalView *t = nil;
    [self keepKeyboardDuring:^{
        if (!self.terminalPanel) {
            [self makeTerminalPanelFirstIn:dir];
            [self showTerminalPanel];
            t = self.terminalPanel.activeTerminal;
            return;
        }
        [self showTerminalPanel];
        t = beside ? [self.terminalPanel splitActiveInDirectory:dir]
                   : [self.terminalPanel addTabInDirectory:dir];
    }];
    return t;
}

- (void)browseForAgent:(NSString *)url {
    [self keepKeyboardDuring:^{
        if (!self.browser) [self makeBrowserWithHome:url];
        else [self.browser navigateToString:url];
        if (!self.browserVisible) {
            self.browserVisible = YES;
            [self showRightArea];
            [self relayoutRightArea];
            if (self.hintsVisible) [self updateHints];
        }
    }];
}

- (NSArray<NSDictionary *> *)controlTabs {
    NSMutableArray<NSDictionary *> *out = [NSMutableArray array];
    for (MCTab *t in _tabs) {
        NSString *kind = t.page ? t.page.pageKind : t.comparison ? @"diff" : t.isDiff ? (t.diffIsCommit ? @"commit" : @"git-diff")
                       : t.isImage ? @"image" : t.isPDF ? @"pdf" : t.isMedia ? @"media"
                       : t.viewerKind == MCViewerQuickLook ? @"quicklook"
                       : t.viewerKind == MCViewerCsv || t.viewerKind == MCViewerTsv ||
                         t.viewerKind == MCViewerCsvLarge ? @"table"
                       : t.viewerKind == MCViewerNotebook ? @"notebook"
                       : t.viewerKind == MCViewerSqlite ? @"sqlite" : @"text";
        NSString *path = t.currentPath ?: t.comparison.path;
        NSString *title = t.page ? t.page.pageTitle : t.comparison ? t.comparison.title
                        : t.isDiff ? t.diffName : t.currentPath.lastPathComponent;
        [out addObject:@{@"path": path ?: (id)NSNull.null,
                         @"title": title ?: @"",
                         @"kind": kind,
                         @"dirty": @(t.dirty),
                         @"active": @(t == _activeTab),
                         @"preview": @(t.agentPreview)}];
    }
    return out;
}

static NSDictionary *MCLineColumn(NSString *s, NSUInteger index) {
    NSUInteger line = 1, start = 0;
    for (NSUInteger i = 0; i < index && i < s.length; i++)
        if ([s characterAtIndex:i] == '\n') { line++; start = i + 1; }
    return @{@"line": @(line), @"column": @(index - start + 1)};
}

- (NSDictionary *)controlSelection {
    if (!_activeTab || !self.currentPath || self.showingMessage || self.isImage ||
        self.isPDF || self.isMedia || self.isDiff)
        return nil;
    NSTextView *tv = self.textView;
    NSString *s = tv.string;
    NSRange r = tv.selectedRange;
    if (NSMaxRange(r) > s.length) r = NSMakeRange(MIN(r.location, s.length), 0);
    NSString *text = [s substringWithRange:r];
    BOOL cut = NO;
    std::string utf8 = Utf8String(text);
    if (utf8.size() > ControlChannel::kMaxSelectionBytes) {
        utf8 = ControlChannel::clipUtf8(utf8, ControlChannel::kMaxSelectionBytes);
        text = [NSString stringWithUTF8String:utf8.c_str()] ?: @"";
        cut = YES;
    }
    return @{@"path": self.currentPath,
             @"text": text,
             @"start": MCLineColumn(s, r.location),
             @"end": MCLineColumn(s, NSMaxRange(r)),
             @"preview": @(self.previewMode),
             @"truncated": @(cut)};
}

- (NSArray<TerminalView *> *)controlTerminals {
    return self.terminalPanel ? self.terminalPanel.terminals : @[];
}

- (NSDictionary *)controlBrowser {
    if (!self.browser) return nil;
    return @{@"url": self.browser.pageURL ?: @"", @"title": self.browser.pageTitle ?: @"",
             @"visible": @(self.browserVisible)};
}

// Option+] and Option+[ (the key monitor, while a tour is on).
- (void)nextTourStop:(id)sender {
    (void)sender;
    if (![_notes stepTour:1]) NSBeep();
    if (self.hintsVisible) [self updateHints];
}
- (void)previousTourStop:(id)sender {
    (void)sender;
    if (![_notes stepTour:-1]) NSBeep();
}
- (BOOL)tourActive { return _notes.tourActive; }

@end
