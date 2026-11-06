// EditorController.mm — Objective-C++. Bridges the pure-C++ core (syntax
// highlighter, markdown parser) to AppKit views.
#import "EditorController+Private.h"

// A task list item's box, drawn rather than the ☐ and ☑ characters, whose
// look depends on the font. Checked, it is filled with the link color and
// carries a check mark.
@interface MCTaskBoxCell : NSTextAttachmentCell
@property(nonatomic) BOOL checked;
@property(nonatomic, strong) NSColor *ink, *fill;
@end

@implementation MCTaskBoxCell
- (NSSize)cellSize { return NSMakeSize(14, 14); }
- (NSPoint)cellBaselineOffset { return NSMakePoint(0, -2); }
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    NSRect box = NSInsetRect(frame, 0.75, 0.75);
    NSBezierPath *outline = [NSBezierPath bezierPathWithRoundedRect:box xRadius:3 yRadius:3];
    if (self.checked) {
        [self.fill setFill];
        [outline fill];
        // The text view is flipped: y grows downwards.
        NSBezierPath *tick = [NSBezierPath bezierPath];
        [tick moveToPoint:NSMakePoint(NSMinX(box) + 3.0, NSMidY(box) + 0.2)];
        [tick lineToPoint:NSMakePoint(NSMinX(box) + 5.6, NSMaxY(box) - 3.2)];
        [tick lineToPoint:NSMakePoint(NSMaxX(box) - 2.8, NSMinY(box) + 3.2)];
        tick.lineWidth = 1.8;
        tick.lineCapStyle = NSLineCapStyleRound;
        tick.lineJoinStyle = NSLineJoinStyleRound;
        [[NSColor whiteColor] setStroke];
        [tick stroke];
    } else {
        outline.lineWidth = 1.3;
        [[self.ink colorWithAlphaComponent:0.75] setStroke];
        [outline stroke];
    }
}
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view
       characterIndex:(NSUInteger)charIndex layoutManager:(NSLayoutManager *)lm {
    [self drawWithFrame:frame inView:view];
}
@end

// The cell is handed out by an override, since one set in init reads back
// nil (see MarkdownImage).
@interface MCTaskBox : NSTextAttachment
@property(nonatomic, strong) MCTaskBoxCell *box;
@end

@implementation MCTaskBox
- (id<NSTextAttachmentCell>)attachmentCell { return self.box; }
@end

@implementation EditorController

- (instancetype)initWithRootPath:(NSString *)path {
    if ((self = [super init])) {
        _root = [FileItem new];
        _root.path = path; _root.isDir = YES;
        [_root loadChildren];
        _terminalHeight = 220;
        _browserFraction = 0.5;
        _tabs = [NSMutableArray array];
        _noTabUndo = [NSUndoManager new];
        _pendingStart = _pendingEnd = NSNotFound;
        [self setUpTreeDetails];
        _agents = [[MCAgentWindow alloc] initWithHost:self];
    }
    return self;
}

- (NSString *)rootPath { return _root.path; }

- (void)dealloc {
    [self stopMedia];
    [self stopWatching];
    [self.window removeObserver:self forKeyPath:@"title"];
}

- (TerminalView *)terminal { return self.terminalPanel.activeTerminal; }

// ====================================================================== tabs
// The document properties are the active tab's (see MCTab): with no tab
// open they read as nil, NO and zero, and setting them does nothing.
#define MC_TAB_PROPERTY(type, name, setter)                     \
    - (type)name { return _activeTab.name; }                    \
    - (void)setter:(type)value { _activeTab.name = value; }
MC_TAB_PROPERTY(NSString *, currentPath, setCurrentPath)
MC_TAB_PROPERTY(NSString *, sourceText, setSourceText)
MC_TAB_PROPERTY(NSString *, savedText, setSavedText)
MC_TAB_PROPERTY(NSString *, currentExt, setCurrentExt)
MC_TAB_PROPERTY(BOOL, isMarkdown, setIsMarkdown)
MC_TAB_PROPERTY(BOOL, isLatex, setIsLatex)
MC_TAB_PROPERTY(BOOL, previewMode, setPreviewMode)
MC_TAB_PROPERTY(BOOL, dirty, setDirty)
MC_TAB_PROPERTY(NSDate *, fileModDate, setFileModDate)
MC_TAB_PROPERTY(LatexView *, latex, setLatex)
MC_TAB_PROPERTY(BOOL, isImage, setIsImage)
MC_TAB_PROPERTY(NSSize, imagePixels, setImagePixels)
MC_TAB_PROPERTY(BOOL, isPDF, setIsPDF)
MC_TAB_PROPERTY(BOOL, isMedia, setIsMedia)
MC_TAB_PROPERTY(MCViewerKind, viewerKind, setViewerKind)
MC_TAB_PROPERTY(NSSize, mediaPixels, setMediaPixels)
MC_TAB_PROPERTY(double, mediaSeconds, setMediaSeconds)
MC_TAB_PROPERTY(BOOL, showingMessage, setShowingMessage)
MC_TAB_PROPERTY(BOOL, isDiff, setIsDiff)
MC_TAB_PROPERTY(NSString *, diffName, setDiffName)
MC_TAB_PROPERTY(NSData *, diffData, setDiffData)
MC_TAB_PROPERTY(BOOL, diffIsCommit, setDiffIsCommit)
#undef MC_TAB_PROPERTY

// -------------------------------------------------------- NSWindowDelegate
// Commands still running in this window's terminals, for the question asked
// before the window closes or the app quits.
- (NSArray<NSString *> *)runningTerminalCommands {
    return self.terminalPanel ? MCRunningCommands(self.terminalPanel.terminals) : @[];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if (!MCConfirmClosingRunning([self runningTerminalCommands], @"Close the window?",
                                 @"Close"))
        return NO;
    return [self confirmCloseAllTabs];
}
- (void)windowWillClose:(NSNotification *)note {
    [_agent close];
    // Asked already (windowShouldClose:): what is still unsaved was let go.
    for (MCTab *t in _tabs) [self discardBackupOf:t];
    _closing = YES;
    // The shells go with the window (hung up and reaped), and the dock with
    // them, so nothing of the window outlives it.
    if (self.terminalPanel) {
        MCTerminalPanel *panel = self.terminalPanel;
        self.terminalPanel = nil;
        [panel closeAll];
        [panel removeFromSuperview];
    }
    [self saveSession];        // reopened with the folder next time
    [_sizer cancel];           // no counting for a closed window
    [_detailsTimer invalidate];
    _detailsTimer = nil;
    _awaitedWebImages = nil;   // pictures still on their way land in the cache only
    _awaitedMath = nil;        // and so do formulas
    [self stopMedia];      // no sound from a closed window
    _formatGeneration++;   // a formatter still running finds nothing to apply
    [self.lsp shutdown];   // never leave a server running for a closed window
    [_change stop];        // no build watched for a closed window
    [_agents stop];        // no transcripts watched for a closed window
}
- (void)windowDidBecomeKey:(NSNotification *)note {
    [self checkExternalChange];
    if (self.gitPanelVisible) [self.gitPanel refresh];
    [_agents windowBecameKey];
}
// Another window or another app (editor.auto-save = focus-change).
- (void)windowDidResignKey:(NSNotification *)note {
    [self autoSaveOnFocusChange];
}

- (BOOL)canTogglePreview {
    return self.isMarkdown || self.isLatex || MCViewerKindOverText(self.viewerKind) ||
           MCViewerKindBesideText(self.viewerKind) || MCViewerKindRendersMarkdown(self.viewerKind);
}

// The rendered Markdown preview is up: a Markdown file's, or a notebook's,
// which is Markdown made from it (read-only: double-click editing, task
// boxes and the like stay with Markdown files).
- (BOOL)showsRenderedMarkdown {
    return (self.isMarkdown || MCViewerKindRendersMarkdown(self.viewerKind)) && self.previewMode;
}

// The Markdown that preview renders.
- (NSString *)markdownToRender {
    if (MCViewerKindRendersMarkdown(self.viewerKind)) return MCNotebookMarkdown(self.sourceText ?: @"");
    return self.sourceText;
}

- (void)undo:(id)sender {
    if ([self latexIsShowing]) [self.latex undoEdit];
    else NSBeep();
}
- (void)redo:(id)sender {
    if ([self latexIsShowing]) [self.latex redoEdit];
    else NSBeep();
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    BOOL enabled = NO;
    if (item.action == @selector(navigateBack:)) return [self.navigator canGoBack];
    if (item.action == @selector(navigateForward:)) return [self.navigator canGoForward];
    if ([self validateActiveTabAction:item.action enabled:&enabled]) return enabled;
    if (item.action == @selector(sortTreeBy:)) {
        item.state = item.tag == (NSInteger)_treeSort.key ? NSControlStateValueOn
                                                          : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(setTreeSortDirection:)) {
        item.state = _treeSort.ascending == (item.tag == 1) ? NSControlStateValueOn
                                                             : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFoldersFirst:)) {
        item.state = _treeSort.foldersFirst ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleTreeColumn:)) {
        item.state = MCTreeColumnShown((MCTreeColumns)item.tag) ? NSControlStateValueOn
                                                                 : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(closeFocused:)) {
        static NSString *const titles[] = {@"Close Window", @"Close Terminal",
                                           @"Close Browser", @"Close Tab",
                                           @"Close Problems"};
        item.title = titles[[self focusedCloseTarget]];
        return YES;
    }
    if (item.action == @selector(selectTabNumber:))
        return EditorTabs::indexForNumber((int)item.tag, (int)_tabs.count) >= 0;
    if (item.action == @selector(selectNextTab:) || item.action == @selector(selectPreviousTab:))
        return _tabs.count > 0;
    if (item.action == @selector(goToBracket:)) return [self bracketMatchingApplies];
    // Off without a tour, so Option+] and Option+[ type their characters.
    if (item.action == @selector(nextTourStop:) || item.action == @selector(previousTourStop:))
        return self.tourActive;
    if (item.action == @selector(exportPDF:)) return self.canExportPDF;
    if (item.action == @selector(togglePlayback:)) return self.canTogglePlayback;
    if (item.action == @selector(toggleProblems:)) {
        item.state = self.problemsVisible ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFindMatchCase:)) {
        item.state = _editorFind.matchCase ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFindWholeWord:)) {
        item.state = _editorFind.wholeWord ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFindRegex:)) {
        item.state = _editorFind.regex ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(performTextFinderAction:))
        return !self.editorScroll.hidden && [_editorFind validateFindAction:item.tag];
    if (item.action == @selector(undo:))
        return [self latexIsShowing] && [self.latex canUndoEdit];
    if (item.action == @selector(redo:))
        return [self latexIsShowing] && [self.latex canRedoEdit];
    return YES;
}

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
// in the tree. The tree takes F2 itself (ClickOutline keyDown:), and a menu
// key that reaches here from the tree goes the same way.
- (void)renameSymbol:(id)sender {
    if (self.window.firstResponder == self.outline) {
        [self renameSelected:sender];
        return;
    }
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
- (void)updateInstallBar {
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
    if (show && !self.installBar) {
        MCInstallBar *bar = [[MCInstallBar alloc] initWithFrame:NSZeroRect];
        __weak EditorController *weakSelf = self;
        bar.onInstall = ^(NSButton *sender) { [weakSelf showInstallHelpFrom:sender]; };
        bar.onClose = ^{ [weakSelf closeInstallBarMuting:NO]; };
        bar.onMute = ^{ [weakSelf closeInstallBarMuting:YES]; };
        self.installBar = bar;
        [self applyInstallBarColors];
        bar.hidden = YES;   // shown, and laid out, just below
        [self.rightArea addSubview:bar];
    }
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
    BOOL show = shown && !bar.hidden && _installBarKey &&
                [_installBarKey isEqualToString:self.lsp.missingServer];
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

- (void)togglePreview:(id)sender {
    if (!self.canTogglePreview) {
        NSBeep();
        return;
    }
    if (MCViewerKindBesideText(self.viewerKind)) {   // the JSON outline: shown or not
        self.viewers.besideTextShown = !self.viewers.besideTextShown;
        [self relayoutRightArea];
        return;
    }
    [self revealEditor];
    NSTextView *tv = self.textView;
    const BOOL toPreview = !self.previewMode;
    NSInteger sourceLine = -1;       // the caret's line, for the preview to show
    NSInteger previewLine = -1;      // the line at the top of a scrolled preview
    if (toPreview) {
        _sourceSelection = tv.selectedRange;
        NSRect caret = [self textRectForCharacter:_sourceSelection.location];
        _sourceCaretOffset = NSMinY(caret) - NSMinY(tv.visibleRect);
        _haveSourcePosition = !NSIsEmptyRect(caret);
        sourceLine = [self lineOfCharacter:_sourceSelection.location inString:tv.string];
    } else if (self.isMarkdown &&
               !NSEqualPoints(self.editorScroll.contentView.bounds.origin, _previewEntryScroll)) {
        NSUInteger top = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMidX(tv.visibleRect), NSMinY(tv.visibleRect) + 4)];
        previewLine = [self markdownSourceLineNear:top];
    }
    if (tv.editable) self.sourceText = tv.string;  // keep edits
    self.previewMode = toPreview;
    [self refreshDisplay];
    [self updateTitle];

    if (toPreview && self.isMarkdown) {
        // Show the part of the page the caret was in, a third of the way down.
        NSUInteger target = [self markdownCharacterForSourceLine:sourceLine];
        NSRect r = [self textRectForCharacter:target];
        if (!NSIsEmptyRect(r))
            [self scrollEditorToY:NSMinY(r) - NSHeight(tv.visibleRect) / 3];
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
    } else if (!toPreview && previewLine >= 0) {
        // The preview was scrolled: open the source where it was looking.
        NSUInteger at = [self characterAtStartOfLine:previewLine inString:tv.string];
        tv.selectedRange = NSMakeRange(at, 0);
        NSRect r = [self textRectForCharacter:at];
        if (!NSIsEmptyRect(r)) [self scrollEditorToY:NSMinY(r) - 8];
    } else if (!toPreview && _haveSourcePosition) {
        const NSUInteger n = tv.string.length;
        NSRange sel = _sourceSelection;
        if (sel.location > n) sel = NSMakeRange(n, 0);
        if (NSMaxRange(sel) > n) sel.length = n - sel.location;
        tv.selectedRange = sel;
        NSRect r = [self textRectForCharacter:sel.location];
        if (!NSIsEmptyRect(r)) [self scrollEditorToY:NSMinY(r) - _sourceCaretOffset];
    }
    if (!self.previewMode) [self.window makeFirstResponder:tv];
    else if (MCViewerKindOverText(self.viewerKind) && _viewers.focusView)
        [self.window makeFirstResponder:_viewers.focusView];
}

// Where character `i` of the editor is drawn, in the text view's coordinates;
// the end of the text counts as the last character. Empty when it cannot be
// known. The editor runs TextKit 1 (see CLAUDE.md), so this asks the layout
// manager, and it never touches one under TextKit 2, where asking would
// switch the view over.
- (NSRect)textRectForCharacter:(NSUInteger)i {
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    if (tv.textLayoutManager || n == 0) return NSZeroRect;
    NSLayoutManager *lm = tv.layoutManager;
    if (i >= n) i = n - 1;
    [lm ensureLayoutForCharacterRange:NSMakeRange(0, i + 1)];
    NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(i, 1)
                                actualCharacterRange:NULL];
    NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
    if (NSIsEmptyRect(r)) r.size = NSMakeSize(1, 1);   // a bare newline
    NSPoint o = tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

- (void)scrollEditorToY:(CGFloat)y {
    NSClipView *clip = self.editorScroll.contentView;
    const CGFloat most = NSHeight(self.textView.frame) - NSHeight(clip.bounds);
    y = MAX(0, MIN(y, MAX(0, most)));
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [self.editorScroll reflectScrolledClipView:clip];
}

- (NSInteger)lineOfCharacter:(NSUInteger)i inString:(NSString *)s {
    NSInteger line = 0;
    const NSUInteger end = MIN(i, s.length);
    for (NSUInteger k = 0; k < end; k++)
        if ([s characterAtIndex:k] == '\n') line++;
    return line;
}

- (NSUInteger)characterAtStartOfLine:(NSInteger)line inString:(NSString *)s {
    NSUInteger k = 0;
    for (NSInteger l = 0; l < line && k < s.length; k++)
        if ([s characterAtIndex:k] == '\n') l++;
    return k;
}

// The first rendered character that came from source line `line` or later
// (the end of the page when none did).
- (NSUInteger)markdownCharacterForSourceLine:(NSInteger)line {
    NSTextStorage *st = self.textView.textStorage;
    __block NSUInteger found = st.length;
    if (line <= 0) return 0;
    [st enumerateAttribute:kMarkdownSourceLine inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(NSNumber *v, NSRange r, BOOL *stop) {
        if (v && v.integerValue >= line) { found = r.location; *stop = YES; }
    }];
    return found;
}

// The source line behind rendered character `i`, or the nearest one after it.
- (NSInteger)markdownSourceLineNear:(NSUInteger)i {
    NSTextStorage *st = self.textView.textStorage;
    if (i >= st.length) i = st.length ? st.length - 1 : 0;
    __block NSInteger line = -1;
    if (st.length == 0) return line;
    [st enumerateAttribute:kMarkdownSourceLine inRange:NSMakeRange(i, st.length - i) options:0
                usingBlock:^(NSNumber *v, NSRange r, BOOL *stop) {
        (void)r;
        if (v) { line = v.integerValue; *stop = YES; }
    }];
    return line;
}

// ---------------------------------------------- editing from the preview

// A double-click on rendered Markdown opens the block behind it (a
// paragraph, heading, list item, quote, code block or table cell) in a
// popover holding its Markdown; saving splices just those bytes back into
// the buffer, which is then dirty until Cmd+S, as typing leaves it.
- (BOOL)editMarkdownAtCharacter:(NSUInteger)index point:(NSPoint)point {
    if (!self.isMarkdown || !self.previewMode) return NO;
    NSTextStorage *st = self.textView.textStorage;
    if (st.length == 0) return NO;
    // The insertion point can fall just after the character clicked.
    if (index >= st.length) index = st.length - 1;
    if (index > 0 && !NSPointInRect(point, [self textRectForCharacter:index]) &&
        NSPointInRect(point, [self textRectForCharacter:index - 1]))
        index--;
    // A box was ticked by the first click; the second does nothing more.
    id link = [st attribute:NSLinkAttributeName atIndex:index effectiveRange:NULL];
    if ([link isKindOfClass:NSString.class] && [link hasPrefix:kTaskLinkPrefix]) return YES;
    NSNumber *line = [st attribute:kMarkdownSourceLine atIndex:index effectiveRange:NULL];
    if (!line) return NO;
    NSArray<NSNumber *> *cell = [st attribute:kMarkdownTableCell atIndex:index
                               effectiveRange:NULL];
    int sourceLine = line.intValue, column = -1;
    if (cell.count == 2) {
        const int row = cell[0].intValue;
        sourceLine += row + (row > 0 ? 1 : 0);   // the separator row follows the header
        column = cell[1].intValue;
    }
    const std::string src = Utf8String(self.sourceText ?: @"");
    MarkdownEdit::Block block = MarkdownEdit::blockAt(src, sourceLine, column);
    if (block.kind == MarkdownEdit::Block::None) return NO;

    static NSString *const titles[] = {@"", @"Paragraph", @"Heading", @"List item",
                                       @"Quote", @"Code block", @"Table cell", @"Table row",
                                       @"Formula"};
    NSString *title = [titles[block.kind] stringByAppendingString:
        @" (Markdown; Return saves, Shift+Return for a new line)"];
    NSString *text = [NSString stringWithUTF8String:
        src.substr(block.start, block.end - block.start).c_str()] ?: @"";

    NSRect anchor = [self textRectForCharacter:index];
    if (NSIsEmptyRect(anchor)) anchor = NSMakeRect(point.x, point.y, 1, 1);
    if (!self.markdownEditPanel) self.markdownEditPanel = [MCMarkdownEditPanel new];
    __weak EditorController *weakSelf = self;
    NSString *before = self.sourceText;
    void (^addItem)(NSString *) = nil;
    if (block.kind == MarkdownEdit::Block::ListItem) {
        addItem = ^(NSString *newText) {
            EditorController *me = weakSelf;
            if (!me || ![me.sourceText isEqualToString:before]) return;
            std::string edited = MarkdownEdit::addItem(src, block, Utf8String(newText));
            [me applyMarkdownSource:[NSString stringWithUTF8String:edited.c_str()]];
        };
    }
    [self.markdownEditPanel showText:text title:title inView:self.textView rect:anchor
        commit:^(NSString *newText) {
            EditorController *me = weakSelf;
            if (!me || ![me.sourceText isEqualToString:before]) return;   // file changed meanwhile
            std::string edited = MarkdownEdit::replace(src, block, Utf8String(newText));
            if (edited != src) [me applyMarkdownSource:[NSString stringWithUTF8String:edited.c_str()]];
        }
        addItem:addItem];
    return YES;
}

// A new buffer from a preview edit: dirty, undoable with Cmd+Z, and shown
// again without moving the page.
- (void)applyMarkdownSource:(NSString *)source {
    if (!source || [source isEqualToString:self.sourceText]) return;
    NSString *old = self.sourceText ?: @"";
    NSUndoManager *undo = self.textView.undoManager;
    __weak EditorController *weakSelf = self;
    [undo registerUndoWithTarget:self handler:^(EditorController *target) {
        (void)target;
        [weakSelf applyMarkdownSource:old];
    }];
    [undo setActionName:@"Preview Edit"];
    self.sourceText = source;
    [self noteEdited];
    [self updateTitle];
    if (self.isMarkdown && self.previewMode) {
        NSPoint origin = self.editorScroll.contentView.bounds.origin;
        [self renderMarkdown:source];
        [self scrollEditorToY:origin.y];
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
    } else if (self.isMarkdown) {
        [self refreshDisplay];
    }
}

// A click on a link in rendered Markdown. "#section" scrolls to that heading;
// a web address opens in the browser panel (another scheme, such as mailto,
// goes to the system); anything else is a path relative to the Markdown
// file, opened as the terminal opens paths, with "#section" or GitHub's
// "#L12" honoured when the target is a Markdown or source file.
- (void)openMarkdownLink:(NSString *)href {
    if ([href hasPrefix:@"#"]) {
        if (![self scrollToMarkdownAnchor:[href substringFromIndex:1]]) NSBeep();
        return;
    }
    NSURL *url = [NSURL URLWithString:href];
    NSString *scheme = url.scheme.lowercaseString;
    if (scheme.length && ![scheme isEqualToString:@"file"]) {
        if ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) {
            MCTermLink *link = [MCTermLink new];
            link.isURL = YES;
            link.target = href;
            [self openTerminalLink:link];
        } else if (url) {
            [[NSWorkspace sharedWorkspace] openURL:url];
        }
        return;
    }
    NSString *path = href, *fragment = nil;
    if ([scheme isEqualToString:@"file"]) path = url.path;
    NSRange hash = [path rangeOfString:@"#"];
    if (hash.location != NSNotFound) {
        fragment = [path substringFromIndex:hash.location + 1];
        path = [path substringToIndex:hash.location];
    }
    path = (path.stringByRemovingPercentEncoding ?: path).stringByExpandingTildeInPath;
    if (!path.absolutePath)
        path = [self.currentPath.stringByDeletingLastPathComponent
                   stringByAppendingPathComponent:path];
    path = path.stringByStandardizingPath;
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir]) {
        NSBeep();
        return;
    }
    MCTermLink *link = [MCTermLink new];
    link.target = path;
    link.isDirectory = dir;
    if (fragment.length > 1 && [fragment characterAtIndex:0] == 'L')
        link.line = [fragment substringFromIndex:1].intValue;   // GitHub's #L12
    [self openTerminalLink:link];
    if (fragment.length && link.line <= 0 && self.isMarkdown && self.previewMode &&
        [self.currentPath isEqualToString:path])
        [self scrollToMarkdownAnchor:fragment];
}

- (BOOL)scrollToMarkdownAnchor:(NSString *)anchor {
    NSString *key = (anchor.stringByRemovingPercentEncoding ?: anchor).lowercaseString;
    NSNumber *at = _markdownAnchors[key];
    if (!at) return NO;
    NSRect r = [self textRectForCharacter:at.unsignedIntegerValue];
    if (NSIsEmptyRect(r)) {
        [self.textView scrollRangeToVisible:NSMakeRange(at.unsignedIntegerValue, 0)];
        return YES;
    }
    [self scrollEditorToY:NSMinY(r) - 8];
    return YES;
}

- (void)renderMarkdown:(NSString *)content {
    std::string md = content.UTF8String ? content.UTF8String : "";
    std::vector<MdRun> runs = MarkdownParser::parse(md);
    _awaitedWebImages = [NSMutableSet set];   // markdownRun: fills it
    _webImageMarks = 0;
    _awaitedMath = [NSMutableSet set];        // and these
    _mathWanted = [NSMutableArray array];
    _mathMarks = 0;
    _mathOn = [self typesetsMath];            // asked once, not per formula
    _mathRunProblem = nil;                    // markdownRun: notes a failed run

    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    NSMutableDictionary<NSString *, NSNumber *> *anchors = [NSMutableDictionary dictionary];
    // A paragraph's look comes from its first run, and every run in it gets
    // that paragraph style; a quote's or a code block's paragraphs share one
    // text block, which draws the bar or the box around all of them.
    NSParagraphStyle *para = nil;
    NSTextBlock *quoteBlock = nil, *codeBox = nil;
    BOOL lineStart = YES;
    MCMarkdownCodeBlocks *codeColors = [MCMarkdownCodeBlocks new];   // ```python and the like
    for (size_t i = 0; i < runs.size();) {
        const MdRun &r = runs[i];
        if (r.tableId > 0) {
            size_t end = i;
            while (end < runs.size() && runs[end].tableId == r.tableId) end++;
            [self appendMarkdownTable:runs from:i to:end into:out];
            i = end;
            lineStart = YES;
            quoteBlock = codeBox = nil;
            continue;
        }
        if (lineStart) {
            if (r.heading > 0) {
                // A heading's text may be several runs (bold, code, links).
                std::string title;
                for (size_t k = i; k < runs.size() && runs[k].heading > 0 &&
                                   runs[k].line == r.line; k++)
                    title += runs[k].text;
                NSString *slug = MCMarkdownAnchor([NSString stringWithUTF8String:title.c_str()]);
                // GitHub numbers repeats: intro, intro-1, intro-2.
                NSString *key = slug;
                for (int n = 1; anchors[key]; n++)
                    key = [NSString stringWithFormat:@"%@-%d", slug, n];
                if (key.length) anchors[key] = @(out.length);
            }
            para = [self markdownParagraphFor:r quote:&quoteBlock code:&codeBox];
        }
        NSAttributedString *piece = [self markdownRun:r style:para inCell:NO];
        if (piece.length) {
            if (r.codeBlock) [codeColors noteRun:NSMakeRange(out.length, piece.length) language:r.lang];
            [out appendAttributedString:piece];
            lineStart = [piece.string hasSuffix:@"\n"];
        }
        i++;
    }
    [codeColors colorPage:out];
    // A page whose math cannot be typeset says why in a line at its top.
    BOOL hasMath = NO;
    for (const MdRun &r : runs) hasMath = hasMath || r.math;
    NSAttributedString *note = !hasMath ? nil
                             : self.mathNoteWanted ? [self mathNote]
                             : _mathRunProblem ? [self mathNoteForProblem:_mathRunProblem]
                             // Formulas still out with tectonic: say so, or the TeX
                             // standing in looks like math that will never be typeset.
                             : _awaitedMath.count ? [self mathNoteTypesetting:_awaitedMath.count]
                             : nil;
    if (note) {
        [out insertAttributedString:note atIndex:0];
        for (NSString *key in anchors.allKeys)
            anchors[key] = @(anchors[key].unsignedIntegerValue + note.length);
    }
    _markdownAnchors = anchors;
    _liveHighlight = NO;   // rendered Markdown is not source
    [self.textView.textStorage setAttributedString:out];
    [self.textView scrollToBeginningOfDocument:nil];
    // Formulas not typeset yet go to tectonic in one run; the page renders
    // again when they are back (markdownMathArrived:).
    if (_mathWanted.count) MCMathTypeset(_mathWanted);
    _mathWanted = nil;
}

// ------------------------------------------------------ math in the preview

// Whether formulas are typeset: the setting, and tectonic to do it.
- (BOOL)typesetsMath {
    return [AppSettings shared].settings.math() && MCTectonicPath() != nil;
}

// The line at the top of a page with math is wanted when typesetting is
// on but tectonic is not there.
- (BOOL)mathNoteWanted {
    return [AppSettings shared].settings.math() && MCTectonicPath() == nil;
}

// The line at the top of a page, muted, with a link at its end if any.
- (NSAttributedString *)mathNoteText:(NSString *)text link:(NSString *)link {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.paragraphSpacing = 10;
    NSDictionary *muted = @{
        NSFontAttributeName: [NSFont systemFontOfSize:MCZoomed(12)],
        NSForegroundColorAttributeName: Hex(0x9CA3AF),
        NSParagraphStyleAttributeName: ps,
        kMarkdownMathNote: @YES,
    };
    NSMutableAttributedString *note = [[NSMutableAttributedString alloc] init];
    [note appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:muted]];
    if (link) {
        NSMutableDictionary *a = [muted mutableCopy];
        a[NSLinkAttributeName] = kMathDownloadLink;
        a[NSForegroundColorAttributeName] = [[AppSettings shared] markdown:MarkdownColor::Link];
        [note appendAttributedString:[[NSAttributedString alloc] initWithString:link attributes:a]];
    }
    [note appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:muted]];
    return note;
}

// A whole run failed (tectonic could not fetch its files, say): why, and
// that it is tried again.
- (NSAttributedString *)mathNoteForProblem:(NSString *)problem {
    NSString *why = [problem stringByTrimmingCharactersInSet:
                                 NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if ([why hasSuffix:@"."]) why = [why substringToIndex:why.length - 1];
    return [self mathNoteText:[NSString stringWithFormat:
        @"The math on this page shows as TeX: tectonic could not typeset it (%@). "
        @"It is tried again when the page is next shown, a minute from now or later.", why]
                         link:nil];
}

// tectonic keeps the LaTeX files it downloads in its own cache. Before its
// first run there is none, and that run fetches about 42 MB first.
static BOOL MCTectonicCacheIsEmpty(void) {
    NSString *dir = NSProcessInfo.processInfo.environment[@"TECTONIC_CACHE_DIR"];
    if (!dir.length)
        dir = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)
                   .firstObject stringByAppendingPathComponent:@"TectonicProject.Tectonic"];
    NSArray *inside = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil];
    return inside.count == 0;
}

// "Typesetting 12 formulas…" while they are out with tectonic, with a word
// on the wait when this is tectonic's first run.
- (NSAttributedString *)mathNoteTypesetting:(NSUInteger)count {
    NSString *text = count == 1 ? @"Typesetting 1 formula…"
                                : [NSString stringWithFormat:@"Typesetting %lu formulas…",
                                                             (unsigned long)count];
    if (MCTectonicCacheIsEmpty())
        text = [text stringByAppendingString:
            @" tectonic's first run downloads its LaTeX files first, which takes about a minute."];
    return [self mathNoteText:text link:nil];
}

// "Math shows as TeX..." with a link that downloads tectonic, or how that
// download is going.
- (NSAttributedString *)mathNote {
    NSString *text, *link = nil;
    if (MCTectonicDownloading()) {
        text = @"Downloading tectonic, which typesets the math on this page…";
    } else if (_tectonicProblem) {
        text = [NSString stringWithFormat:@"tectonic could not be downloaded (%@), so the math "
                                          @"shows as TeX. ", _tectonicProblem];
        link = @"Try again";
    } else {
        text = @"The math on this page shows as TeX: MiniCode typesets it with tectonic, "
               @"which is not installed. ";
        link = @"Download tectonic";
    }
    return [self mathNoteText:text link:link];
}

- (void)downloadTectonicForMath {
    if (MCTectonicDownloading()) return;
    _tectonicProblem = nil;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    MCDownloadTectonic(^(NSString *problem) {
        EditorController *me = weakSelf;
        if (!me) return;
        me->_tectonicProblem = [problem copy];
        if (me.isMarkdown && me.previewMode && [path isEqualToString:me.currentPath])
            [me renderMarkdownKeepingPlace];
    });
    [self renderMarkdownKeepingPlace];   // "Downloading..."
}

// How long the line about tectonic at the top of the page is, or 0.
- (NSUInteger)mathNoteLength {
    NSTextStorage *st = self.textView.textStorage;
    NSRange r = NSMakeRange(0, 0);
    if (st.length && [st attribute:kMarkdownMathNote atIndex:0 longestEffectiveRange:&r
                                inRange:NSMakeRange(0, st.length)])
        return r.length;
    return 0;
}

// The typeset formula for a math run, or nil when it is to show as TeX:
// typesetting is off, it is still being typeset, or it failed.
- (MCMathFormula *)typesetFormulaFor:(const MdRun &)r {
    if (!r.math || !_mathOn) return nil;
    NSString *tex = [NSString stringWithUTF8String:r.text.c_str()];
    if (!tex) return nil;
    MCMathFormula *f = MCMathLookup(tex, r.math == 2);
    return f && !f.failed ? f : nil;
}

// Screen points per PDF point of a formula set beside text of `fontSize`.
// TeX sets the formulas at 10 pt. Computer Modern's lowercase letters are
// smaller for their size than the system font's and its capitals about the
// same, so its em is made a little larger than the text's: halfway, near
// enough, between matching the capitals and matching the lowercase.
static CGFloat MCMathScaleFor(CGFloat fontSize) {
    static const CGFloat kEmToText = 1.12;             // math em / text size
    static const CGFloat kTenPointsInPdf = 10 * 72.0 / 72.27;
    return fontSize * kEmToText / kTenPointsInPdf;
}

// Some formulas of the page on show are back from tectonic: render it again
// with them in, once for all that arrive together, keeping the place.
- (void)markdownMathArrived:(NSNotification *)note {
    NSSet<NSString *> *keys = note.userInfo[MCMathKeysKey];
    if (!_awaitedMath || ![_awaitedMath intersectsSet:keys]) return;
    [_awaitedMath minusSet:keys];
    if (_mathRenderQueued) return;
    _mathRenderQueued = YES;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [weakSelf showArrivedMathForPath:path];
    });
}

- (void)showArrivedMathForPath:(NSString *)path {
    _mathRenderQueued = NO;
    if (!_awaitedMath || ![self showsRenderedMarkdown] || !path ||
        ![path isEqualToString:self.currentPath])
        return;
    [self renderMarkdownKeepingPlace];
}

// A web picture finished. When the page on show waits for it, the page is
// rendered again with the picture in, and pictures finishing close together
// share one render. A failure keeps the alt text and needs nothing.
- (void)markdownWebImageArrived:(NSNotification *)note {
    NSString *src = note.userInfo[MCMarkdownWebImageURLKey];
    if (!src || ![_awaitedWebImages containsObject:src]) return;
    [_awaitedWebImages removeObject:src];
    if (![note.userInfo[MCMarkdownWebImageOKKey] boolValue] || _webImageRenderQueued) return;
    _webImageRenderQueued = YES;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [weakSelf showArrivedWebImagesForPath:path];
    });
}

- (void)showArrivedWebImagesForPath:(NSString *)path {
    _webImageRenderQueued = NO;
    // Not after the window closed, another file was opened, or the source
    // took the preview's place (the preview renders anew when it is back).
    if (!_awaitedWebImages || ![self showsRenderedMarkdown] || !path ||
        ![path isEqualToString:self.currentPath])
        return;
    [self renderMarkdownKeepingPlace];
}

// Render the preview again, keeping the text at the top of the pane where it
// was. Each web picture that has arrived takes the place of its alt text, and
// each typeset formula the place of its TeX, one character for many, and
// moves what follows by the height it adds, so the text to keep is found
// again by its character, counted past those replacements (and past the
// line about tectonic, when it is going). A page at the very top stays at
// the top, as a browser's does while its pictures load.
- (void)renderMarkdownKeepingPlace {
    NSTextView *tv = self.textView;
    NSTextStorage *st = tv.textStorage;
    NSClipView *clip = self.editorScroll.contentView;
    const NSPoint origin = clip.bounds.origin;
    const BOOL unscrolled = NSEqualPoints(origin, _previewEntryScroll);
    NSUInteger anchor = NSNotFound;
    CGFloat offset = 0;
    if (origin.y > 0 && st.length > 0) {
        const NSRect visible = tv.visibleRect;
        NSUInteger i = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)];
        if (i >= st.length) i = st.length - 1;
        const NSRect r = [self textRectForCharacter:i];
        if (!NSIsEmptyRect(r)) {
            offset = NSMinY(r) - NSMinY(visible);
            // Stretches before the anchor that change length: where they
            // are, and how many characters they will be.
            NSMutableArray<NSArray<NSNumber *> *> *changes = [NSMutableArray array];
            const NSRange before = NSMakeRange(0, i + 1);
            [st enumerateAttribute:kMarkdownWebImage inRange:before options:0
                        usingBlock:^(NSArray *mark, NSRange range, BOOL *stop) {
                (void)stop;
                if (mark && MCMarkdownWebImageIsReady(mark.firstObject))
                    [changes addObject:@[@(range.location), @(range.length), @1]];
            }];
            if ([self typesetsMath])
                [st enumerateAttribute:kMarkdownMath inRange:before options:0
                            usingBlock:^(NSArray *mark, NSRange range, BOOL *stop) {
                    (void)stop;
                    if (mark && MCMathIsReady(mark.firstObject))
                        [changes addObject:@[@(range.location), @(range.length), @1]];
                }];
            [changes sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) {
                return [a[0] compare:b[0]];
            }];
            NSUInteger shift = 0, inside = NSNotFound;
            for (NSArray<NSNumber *> *c in changes) {
                const NSRange range = NSMakeRange(c[0].unsignedIntegerValue,
                                                  c[1].unsignedIntegerValue);
                if (NSLocationInRange(i, range)) {
                    inside = range.location - shift;
                    break;
                }
                shift += range.length - c[2].unsignedIntegerValue;
            }
            anchor = inside != NSNotFound ? inside : i - shift;
        }
    }
    const NSUInteger noteBefore = [self mathNoteLength];
    [self renderMarkdown:[self markdownToRender]];
    // The line about tectonic at the top may have come or gone.
    const NSUInteger noteAfter = [self mathNoteLength];
    if (anchor != NSNotFound) {
        if (anchor >= noteBefore) anchor = anchor - noteBefore + noteAfter;
        else anchor = 0;
    }
    if (anchor != NSNotFound && st.length > 0) {
        if (!tv.textLayoutManager)   // TextKit 1, as the editor is: the full height
            [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
        const NSRect r = [self textRectForCharacter:MIN(anchor, st.length - 1)];
        [self scrollEditorToY:NSIsEmptyRect(r) ? origin.y : NSMinY(r) - offset];
    }
    if (unscrolled) _previewEntryScroll = clip.bounds.origin;
}

// GitHub's anchor for a heading: lowercase, punctuation dropped (letters of
// any script, digits, hyphens and underscores stay), spaces as hyphens.
static NSString *MCMarkdownAnchor(NSString *title) {
    NSString *t = [title stringByTrimmingCharactersInSet:
                            NSCharacterSet.whitespaceAndNewlineCharacterSet].lowercaseString;
    NSMutableString *slug = [NSMutableString string];
    [t enumerateSubstringsInRange:NSMakeRange(0, t.length)
                          options:NSStringEnumerationByComposedCharacterSequences
                       usingBlock:^(NSString *ch, NSRange r, NSRange er, BOOL *stop) {
        (void)r; (void)er; (void)stop;
        unichar c = [ch characterAtIndex:0];
        if (c == ' ') [slug appendString:@"-"];
        else if (c == '-' || c == '_' ||
                 [NSCharacterSet.alphanumericCharacterSet characterIsMember:c])
            [slug appendString:ch];
    }];
    return slug;
}

// The preview's vertical rhythm, in points at no zoom: the empty line
// between two blocks (the parser's `gap` runs), and the indent of each list
// level.
static const CGFloat kMarkdownGap = 12;
static const CGFloat kMarkdownListStep = 24;

// A text block as wide as the text container.
static NSTextBlock *MCFullWidthBlock(void) {
    NSTextBlock *b = [NSTextBlock new];
    [b setValue:100 type:NSTextBlockPercentageValueType forDimension:NSTextBlockWidth];
    return b;
}

// The paragraph style for a paragraph of rendered Markdown, from its first
// run. A block quote draws a bar down its left side and a code block a box;
// `quote` and `code` carry that text block from one paragraph to the next,
// so the paragraphs of one quote or code block (and a quote's inner gaps)
// share it and it is drawn once around them all.
- (NSParagraphStyle *)markdownParagraphFor:(const MdRun &)r
                                     quote:(NSTextBlock * __strong *)quote
                                      code:(NSTextBlock * __strong *)code {
    // Display math on a line of its own: centred when it is typeset, in a
    // code block's box while it shows as TeX.
    if (r.math == 2 && r.heading == 0 && !r.table && ![self typesetFormulaFor:r]) {
        MdRun box = r;
        box.math = 0;
        box.codeBlock = true;
        return [self markdownParagraphFor:box quote:quote code:code];
    }
    AppSettings *cfg = [AppSettings shared];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = 3.0;
    if (r.math == 2 && r.heading == 0 && !r.table) {
        ps.alignment = NSTextAlignmentCenter;
        ps.lineSpacing = 0;
        ps.paragraphSpacingBefore = 4;
        ps.paragraphSpacing = 4;
    }
    NSMutableArray<NSTextBlock *> *blocks = [NSMutableArray array];
    if (r.quote) {
        if (!*quote) {
            NSTextBlock *b = MCFullWidthBlock();
            [b setWidth:3 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
                   edge:NSRectEdgeMinX];
            [b setBorderColor:Hex(0x4B5563) forEdge:NSRectEdgeMinX];
            [b setWidth:13 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinX];
            *quote = b;
        }
        [blocks addObject:*quote];
    } else {
        *quote = nil;
    }
    if (r.codeBlock) {
        if (!*code) {
            NSTextBlock *b = MCFullWidthBlock();
            b.backgroundColor = MCColor(cfg.settings.markdownCodeBackground());
            [b setWidth:12 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinX];
            [b setWidth:12 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxX];
            [b setWidth:9 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinY];
            [b setWidth:9 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxY];
            *code = b;
        }
        [blocks addObject:*code];
        ps.lineSpacing = 2.0;
    } else {
        *code = nil;
    }
    if (r.gap) {
        ps.minimumLineHeight = ps.maximumLineHeight = MCZoomed(kMarkdownGap);
        ps.lineSpacing = 0;
    } else if (r.heading > 0) {
        ps.paragraphSpacingBefore = r.heading <= 2 ? 10.0 : 4.0;
        ps.paragraphSpacing = 2.0;
        if (r.heading <= 2) {   // a line under the two biggest, as GitHub has
            NSTextBlock *b = MCFullWidthBlock();
            [b setWidth:1 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
                   edge:NSRectEdgeMaxY];
            [b setBorderColor:Hex(0x3F3F46) forEdge:NSRectEdgeMaxY];
            [b setWidth:6 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxY];
            [blocks addObject:b];
        }
    } else if (r.rule) {
        NSTextBlock *b = MCFullWidthBlock();
        [b setWidth:2 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
               edge:NSRectEdgeMaxY];
        [b setBorderColor:Hex(0x4B5563) forEdge:NSRectEdgeMaxY];
        [blocks addObject:b];
        ps.minimumLineHeight = ps.maximumLineHeight = 4;
        ps.lineSpacing = 0;
    }
    if (r.listDepth > 0) {
        // The text of every line sits at the level's indent; a marker is
        // right-aligned just before it, so numbers of any width line up.
        const CGFloat step = MCZoomed(kMarkdownListStep);
        const CGFloat text = r.listDepth * step;
        ps.headIndent = text;
        ps.firstLineHeadIndent = r.marker ? text - step : text;
        ps.tabStops = @[
            [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentRight
                                            location:text - 7 options:@{}],
            [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                            location:text options:@{}],
        ];
        ps.paragraphSpacing = 3.0;
    }
    ps.textBlocks = blocks;
    return ps;
}

// One run of rendered Markdown, set in the paragraph style `ps`. Inside a
// table cell (`inCell`) the text is set in the body font, since a real
// table lines its columns up without monospace.
- (NSAttributedString *)markdownRun:(const MdRun &)r
                              style:(NSParagraphStyle *)ps
                             inCell:(BOOL)inCell {
    NSString *s = [NSString stringWithUTF8String:r.text.c_str()];
    if (!s) return nil;

    // Sizes at no zoom; Zoom In and Out scale the preview with the editor.
    NSFont *body = [NSFont systemFontOfSize:MCZoomed(15)];
    NSFont *mono = [NSFont monospacedSystemFontOfSize:MCZoomed(13)
                                               weight:NSFontWeightRegular];
    AppSettings *cfg = [AppSettings shared];
    NSFont *font = body;
    NSColor *color = [cfg text:Surface::Editor];
    NSMutableDictionary *a = [NSMutableDictionary dictionary];

    if (r.heading > 0) {
        CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
        font = [NSFont boldSystemFontOfSize:MCZoomed(sizes[r.heading])];
        color = [cfg markdown:MarkdownColor::Heading];
    }
    // Display math on a line of its own, shown as TeX, sits in a code box.
    const BOOL boxedMath = r.math == 2 && r.heading == 0 && !r.table;
    if (r.codeBlock || r.code) {
        font = mono;
        color = [cfg markdown:MarkdownColor::Code];
        if (!r.codeBlock && !boxedMath)   // a code block's box is its paragraphs' text block
            a[NSBackgroundColorAttributeName] =
                MCColor(cfg.settings.markdownCodeBackground());
    }
    if (r.table && !inCell) {
        font = mono;   // monospace keeps the padded columns aligned
        if (!r.code) color = r.bold ? [cfg markdown:MarkdownColor::Heading]
                                    : [cfg text:Surface::Editor];
    }
    if (r.table && inCell && r.bold && !r.code)
        color = [cfg markdown:MarkdownColor::Heading];
    if (r.quote && !r.code) color = [cfg markdown:MarkdownColor::Quote];
    if (r.marker) {
        // "  • " from the parser: the tabs of the paragraph style place it.
        NSString *m = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        s = [NSString stringWithFormat:@"\t%@\t", m];
    }
    // A line break inside a paragraph, not the start of a new one.
    if (r.hardBreak) s = @" ";
    if (r.rule) font = [NSFont systemFontOfSize:2];
    if (r.strike)
        a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    if (r.link) { color = [cfg markdown:MarkdownColor::Link]; a[NSUnderlineStyleAttributeName] =
        @(NSUnderlineStyleSingle); }
    // A link's target as written; textView:clickedOnLink: resolves it.
    NSString *href = r.link && !r.url.empty()
                         ? [NSString stringWithUTF8String:r.url.c_str()] : nil;
    if (href.length) a[NSLinkAttributeName] = href;
    if (r.line >= 0) a[kMarkdownSourceLine] = @(r.line);
    // A checked task's text: muted and struck through.
    if (r.task == 2 && !r.marker) {
        color = [cfg markdown:MarkdownColor::Done];
        a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    }
    // A task's marker is a box to click, placed by the tabs as a bullet is.
    if (r.marker && r.task && r.line >= 0) {
        MCTaskBox *att = [MCTaskBox new];
        att.box = [MCTaskBoxCell new];
        att.box.checked = r.task == 2;
        att.box.ink = [cfg text:Surface::Editor];
        att.box.fill = [cfg markdown:MarkdownColor::Link];
        NSMutableDictionary *ta = [a mutableCopy];
        ta[NSFontAttributeName] = font;
        ta[NSForegroundColorAttributeName] = color;
        ta[NSParagraphStyleAttributeName] = ps;
        NSMutableAttributedString *m =
            [[NSMutableAttributedString alloc] initWithString:@"\t" attributes:ta];
        NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
            initWithAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
        [pic addAttributes:ta range:NSMakeRange(0, pic.length)];
        [pic addAttribute:NSLinkAttributeName
                    value:[kTaskLinkPrefix stringByAppendingFormat:@"%d", r.line]
                    range:NSMakeRange(0, pic.length)];
        [pic addAttribute:NSToolTipAttributeName
                    value:r.task == 2 ? @"Done: click to clear" : @"Click to tick"
                    range:NSMakeRange(0, pic.length)];
        [m appendAttributedString:pic];
        [m appendAttributedString:[[NSAttributedString alloc] initWithString:@"\t"
                                                                  attributes:ta]];
        return m;
    }
    if (!ps) ps = [NSParagraphStyle defaultParagraphStyle];
    if (r.math && cfg.settings.math()) {
        // Typeset, a picture on the baseline in the color the text around
        // it has. Otherwise its TeX in the code style: while it is being
        // typeset (it is asked for once the page is built), or with TeX's
        // complaint on hover when it failed.
        const BOOL display = r.math == 2;
        MCMathFormula *f = _mathOn ? MCMathLookup(s, display) : nil;
        if (f && !f.failed) {
            NSColor *ink = [cfg text:Surface::Editor];
            NSFont *around = body;
            if (r.heading > 0) {
                const CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
                around = [NSFont boldSystemFontOfSize:MCZoomed(sizes[r.heading])];
                ink = [cfg markdown:MarkdownColor::Heading];
            }
            if (r.table && r.bold) ink = [cfg markdown:MarkdownColor::Heading];
            if (r.quote) ink = [cfg markdown:MarkdownColor::Quote];
            if (r.link) ink = [cfg markdown:MarkdownColor::Link];
            MCMathAttachment *att = [[MCMathAttachment alloc]
                initWithFormula:f scale:MCMathScaleFor(around.pointSize) color:ink font:around];
            NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
                initWithAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
            NSMutableDictionary *pa = [NSMutableDictionary dictionary];
            pa[NSParagraphStyleAttributeName] = ps;
            pa[NSFontAttributeName] = around;
            if (href.length) pa[NSLinkAttributeName] = href;
            if (r.line >= 0) pa[kMarkdownSourceLine] = @(r.line);
            [pic addAttributes:pa range:NSMakeRange(0, pic.length)];
            return pic;
        }
        if (f.failed) {
            if (f.transient) _mathRunProblem = f.error;
            a[NSToolTipAttributeName] = f.error.length ? f.error : @"TeX could not typeset this.";
            // A dotted red line under it says there is something to hover.
            a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle | NSUnderlinePatternDot);
            a[NSUnderlineColorAttributeName] = Hex(0xF14C4C);
        } else if (_mathOn) {
            NSString *key = MCMathKey(s, display);
            [_awaitedMath addObject:key];
            [_mathWanted addObject:@[s, @(display)]];
            a[kMarkdownMath] = @[key, @(++_mathMarks)];
        }
    }
    if (r.image) {
        NSString *src = [NSString stringWithUTF8String:r.src.c_str()];
        NSImage *picture = MCMarkdownLoadImage(src,
            self.currentPath.stringByDeletingLastPathComponent);
        // An https picture comes from the cache, or is fetched in the
        // background while its alt text stands in; the page is rendered
        // again when it arrives (markdownWebImageArrived:).
        BOOL waiting = NO;
        if (!picture && MCMarkdownIsWebImage(src) && cfg.settings.webImages())
            picture = MCMarkdownWebImage(src, &waiting);
        if (waiting) {
            [_awaitedWebImages addObject:src];
            a[kMarkdownWebImage] = @[src, @(++_webImageMarks)];
        }
        if (picture) {
            MCMarkdownImage *att = [[MCMarkdownImage alloc] initWithPicture:picture];
            NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
                initWithAttributedString:
                    [NSAttributedString attributedStringWithAttachment:att]];
            [pic addAttribute:NSParagraphStyleAttributeName value:ps
                        range:NSMakeRange(0, pic.length)];
            if (href.length)
                [pic addAttribute:NSLinkAttributeName value:href range:NSMakeRange(0, pic.length)];
            if (r.line >= 0)
                [pic addAttribute:kMarkdownSourceLine value:@(r.line)
                            range:NSMakeRange(0, pic.length)];
            return pic;
        }
        // Missing, still on its way, or a web picture that may not or could
        // not be fetched: its alt text, muted.
        if (!r.link) color = Hex(0x9CA3AF);
        if (s.length == 0) s = src;
    }

    NSFontManager *fm = [NSFontManager sharedFontManager];
    if (r.bold) font = [fm convertFont:font toHaveTrait:NSBoldFontMask];
    if (r.italic) font = [fm convertFont:font toHaveTrait:NSItalicFontMask];

    a[NSFontAttributeName] = font;
    a[NSForegroundColorAttributeName] = color;
    a[NSParagraphStyleAttributeName] = ps;
    return [[NSAttributedString alloc] initWithString:s attributes:a];
}

// How wide a table cell's content must be (its widest word or formula, which
// cannot wrap) and would like to be (all of it on one line).
static void MCMeasureCell(NSAttributedString *s, CGFloat *least, CGFloat *most) {
    *least = *most = 0;
    if (s.length == 0) return;
    *most = ceil([s boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX) options:0].size.width);
    NSString *str = s.string;
    NSCharacterSet *space = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    const NSUInteger n = str.length;
    for (NSUInteger i = 0; i < n;) {
        while (i < n && [space characterIsMember:[str characterAtIndex:i]]) i++;
        NSUInteger j = i;
        while (j < n && ![space characterIsMember:[str characterAtIndex:j]]) j++;
        if (j > i) {
            NSAttributedString *word = [s attributedSubstringFromRange:NSMakeRange(i, j - i)];
            *least = MAX(*least, ceil([word boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)
                                                        options:0].size.width));
        }
        i = j;
    }
}

// Each column's share of a table `available` points wide, as percentages,
// laid out the way a browser lays out a table: every column gets its widest
// piece that cannot wrap, and the room left over goes to the columns by how
// much wider their text would like to be. `chrome` is a cell's padding and
// border. Empty when the pane is too narrow to bother.
static std::vector<CGFloat> MCTableColumnWidths(NSArray<NSAttributedString *> *cells, int rows,
                                                int cols, CGFloat available, CGFloat chrome) {
    std::vector<CGFloat> lo((size_t)cols, 0), hi((size_t)cols, 0);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++) {
            CGFloat least = 0, most = 0;
            MCMeasureCell(cells[(NSUInteger)(row * cols + col)], &least, &most);
            // A point to spare, for the rounding of percentages.
            lo[(size_t)col] = MAX(lo[(size_t)col], least + 1);
            hi[(size_t)col] = MAX(hi[(size_t)col], most + 1);
        }
    const CGFloat room = available - cols * chrome;
    if (room <= 40) return {};
    CGFloat sumLo = 0, sumHi = 0;
    for (int c = 0; c < cols; c++) {
        sumLo += lo[(size_t)c];
        sumHi += hi[(size_t)c];
    }
    std::vector<CGFloat> pct((size_t)cols);
    for (int c = 0; c < cols; c++) {
        const size_t k = (size_t)c;
        CGFloat w;
        if (sumHi <= room)         // everything fits on one line: share out the rest
            w = sumHi > 0 ? hi[k] + (room - sumHi) * hi[k] / sumHi : room / cols;
        else if (sumLo >= room)    // not even the widest pieces fit: scale them all
            w = lo[k] * room / sumLo;
        else
            w = lo[k] + (room - sumLo) * (hi[k] - lo[k]) / (sumHi - sumLo);
        pct[k] = w / available * 100;
    }
    return pct;
}

// A Markdown table as an NSTextTable: every cell is a paragraph of its own
// that wraps inside its column, so a table wider than the pane stays a
// table. Columns are sized to their content by AppKit, the header row is
// shaded, and each column keeps the alignment its separator row gave it.
// The parser's padding and rules (tableCol -1) are for monospace ports and
// are skipped here.
//
// AppKit's sizing reads text and knows nothing of an attachment's width, so
// a table holding typeset formulas gets its column widths from
// MCTableColumnWidths instead: formulas cannot wrap, and AppKit would make
// the columns equal and leave them to be scaled down.
- (void)appendMarkdownTable:(const std::vector<MdRun> &)runs
                       from:(size_t)first
                         to:(size_t)end
                       into:(NSMutableAttributedString *)out {
    const int cols = runs[first].tableCols;
    int rows = 0;
    for (size_t k = first; k < end; k++) rows = std::max(rows, runs[k].tableRow + 1);
    if (cols <= 0 || rows <= 0) return;

    AppSettings *cfg = [AppSettings shared];
    NSDictionary *plain = @{NSFontAttributeName: [NSFont systemFontOfSize:MCZoomed(15)],
                            NSForegroundColorAttributeName: [cfg text:Surface::Editor]};
    if (out.length && ![out.string hasSuffix:@"\n"])
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                    attributes:plain]];

    NSTextTable *table = [[NSTextTable alloc] init];
    table.numberOfColumns = (NSUInteger)cols;
    table.layoutAlgorithm = NSTextTableAutomaticLayoutAlgorithm;
    table.collapsesBorders = YES;
    table.hidesEmptyCells = NO;
    NSColor *border = Hex(0x444444);
    NSColor *headerBackground = MCColor(cfg.settings.markdownCodeBackground());
    const NSTextAlignment alignments[3] = {NSTextAlignmentLeft, NSTextAlignmentCenter,
                                           NSTextAlignmentRight};

    // The cells' contents first, so the columns can be measured.
    NSMutableArray<NSMutableAttributedString *> *contents = [NSMutableArray array];
    std::vector<int> aligns((size_t)(rows * cols), 0);
    BOOL typesetMath = NO;
    NSParagraphStyle *interim = [NSParagraphStyle defaultParagraphStyle];
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
            for (size_t k = first; k < end; k++) {
                const MdRun &r = runs[k];
                if (r.tableRow != row || r.tableCol != col) continue;
                aligns[(size_t)(row * cols + col)] = r.tableAlign;
                NSAttributedString *piece = [self markdownRun:r style:interim inCell:YES];
                if (!piece) continue;
                [text appendAttributedString:piece];
                if (r.math && piece.length == 1 &&
                    [[piece attribute:NSAttachmentAttributeName atIndex:0 effectiveRange:NULL]
                        isKindOfClass:MCMathAttachment.class])
                    typesetMath = YES;
            }
            [contents addObject:text];
        }
    }
    // Each cell pads its text by 8 points a side and has a 1 point border.
    static const CGFloat kCellChrome = 17;
    std::vector<CGFloat> widths;
    if (typesetMath)
        widths = MCTableColumnWidths(contents, rows, cols,
                                     self.textView.textContainer.size.width -
                                         2 * self.textView.textContainer.lineFragmentPadding,
                                     kCellChrome);

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            NSTextTableBlock *cell =
                [[NSTextTableBlock alloc] initWithTable:table startingRow:row rowSpan:1
                                         startingColumn:col columnSpan:1];
            [cell setWidth:1 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder];
            [cell setBorderColor:border];
            [cell setWidth:8 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMinX];
            [cell setWidth:8 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMaxX];
            [cell setWidth:4 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMinY];
            [cell setWidth:4 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMaxY];
            if (row == 0) cell.backgroundColor = headerBackground;
            if (!widths.empty())
                [cell setValue:widths[(size_t)col] type:NSTextBlockPercentageValueType
                  forDimension:NSTextBlockWidth];

            NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
            ps.textBlocks = @[cell];
            ps.lineSpacing = 2.0;
            const int align = aligns[(size_t)(row * cols + col)];
            ps.alignment = alignments[std::min(std::max(align, 0), 2)];
            NSMutableAttributedString *text = contents[(NSUInteger)(row * cols + col)];
            NSMutableDictionary *end = [plain mutableCopy];
            end[NSParagraphStyleAttributeName] = ps;
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                         attributes:end]];
            [text addAttribute:NSParagraphStyleAttributeName value:ps
                         range:NSMakeRange(0, text.length)];
            [text addAttribute:kMarkdownSourceLine value:@(runs[first].line)
                         range:NSMakeRange(0, text.length)];
            [text addAttribute:kMarkdownTableCell value:@[@(row), @(col)]
                         range:NSMakeRange(0, text.length)];
            [out appendAttributedString:text];
        }
    }
}

@end
