// EditorController+Layout.mm — EditorController: window setup, layout and look, the splits and what fills the sidebar, the title.
#import "EditorController+Private.h"

@implementation EditorController (Layout)

// -------------------------------------------------------------- window setup
- (void)showWindow {
    NSRect frame = NSMakeRect(0, 0, 1100, 720);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                       // Content runs under the title bar, so MiniCode can draw
                       // it (see applySettings); layoutContainer keeps the
                       // panels below it.
                       NSWindowStyleMaskFullSizeContentView;
    // MCKeyboardWindow (Browser.h): a web page cannot take the keyboard.
    self.window = [[MCKeyboardWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"MiniCode";
    // ARC owns the window through the strong `window` property. NSWindow defaults
    // to releasedWhenClosed=YES, which adds a legacy unbalanced release on close;
    // combined with the ARC release that's a double-free that crashes on close.
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    self.window.minSize = NSMakeSize(640, 400);
    [self.window center];

    // --- split view: sidebar | editor
    NSSplitView *split = [[NSSplitView alloc] initWithFrame:frame];
    self.splitView = split;
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    split.delegate = self;   // pins sidebar width, prevents collapse-to-zero

    // --- sidebar (file tree)
    PanelScrollView *treeScroll = [[PanelScrollView alloc] init];
    self.sidebarScroll = treeScroll;
    treeScroll.hasVerticalScroller = YES;
    treeScroll.automaticallyAdjustsContentInsets = NO;   // colored in applySettings

    self.outline = [[ClickOutline alloc] init];
    __weak EditorController *weakSelf = self;
    self.outline.onRowClick = ^(NSInteger row, NSPoint pt) {
        [weakSelf handleRowClick:row atPoint:pt];
    };
    self.outline.onActivate = ^(NSInteger row) {
        [weakSelf activateRow:row];
    };
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"file"];
    col.editable = NO;
    col.minWidth = 80;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [self.outline addTableColumn:col];
    self.outline.outlineTableColumn = col;
    self.outline.columnAutoresizingStyle =
        NSTableViewLastColumnOnlyAutoresizingStyle;
    self.outline.headerView = nil;
    self.outline.dataSource = self;
    self.outline.delegate = self;
    // Clear, so the scroll view's color is the only one (a translucent color
    // drawn twice would look more opaque than configured).
    self.outline.backgroundColor = [NSColor clearColor];
    self.outline.rowSizeStyle = NSTableViewRowSizeStyleMedium;
    self.outline.indentationPerLevel = 14;
    self.outline.floatsGroupRows = NO;
    self.outline.menu = [self buildTreeContextMenu];
    // Drag and drop: rows move into folders; files from Finder are copied in,
    // and rows dragged out to Finder or the terminal are copies.
    [self.outline registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    [self.outline setDraggingSourceOperationMask:NSDragOperationMove forLocal:YES];
    [self.outline setDraggingSourceOperationMask:NSDragOperationCopy forLocal:NO];
    treeScroll.documentView = self.outline;

    // --- editor pane
    PanelScrollView *textScroll = [[PanelScrollView alloc] init];
    self.editorScroll = textScroll;
    textScroll.hasVerticalScroller = YES;
    textScroll.hasHorizontalScroller = YES;
    textScroll.autohidesScrollers = YES;
    textScroll.automaticallyAdjustsContentInsets = NO;

    // TextKit 1, over a storage that fixes attributes as edits happen, not
    // in 64K blocks around every keystroke (SourceStorage.h). Built here
    // so the view and what it makes at init (carets, folds) watch that
    // storage; the controller keeps it, since a layout manager does not.
    _sourceStorage = [MCSourceStorage new];
    NSLayoutManager *layout = [NSLayoutManager new];
    [_sourceStorage addLayoutManager:layout];
    NSTextContainer *textBox =
        [[NSTextContainer alloc] initWithSize:NSMakeSize(NSWidth(frame), CGFLOAT_MAX)];
    [layout addTextContainer:textBox];
    self.textView = [[CodeTextView alloc] initWithFrame:frame textContainer:textBox];
    self.textView.editable = YES;
    self.textView.delegate = self;
    self.textView.textStorage.delegate = self;   // incremental highlighting
    self.textView.richText = YES;
    self.textView.allowsUndo = YES;
    self.textView.automaticSpellingCorrectionEnabled = NO;
    self.textView.automaticDashSubstitutionEnabled = NO;
    self.textView.usesFindBar = NO;                // Cmd+F is MCEditorFind's bar
    // Not the Font panel: MiniCode sets its own fonts, and a text view that
    // uses it also pushes the color under the caret into the shared color
    // panel whenever the selection moves. With the panel picking a color for
    // the settings file (colorPicked:), that push came back as a pick and
    // wrote the text color over the color just chosen.
    self.textView.usesFontPanel = NO;
    // Links are only used for the color swatches in the settings file; keep
    // their own colors and just show a pointing hand.
    self.textView.linkTextAttributes =
        @{NSCursorAttributeName: [NSCursor pointingHandCursor]};
    self.textView.incrementalSearchingEnabled = YES;
    // Layout stays contiguous (allowsNonContiguousLayout off, the default):
    // laying out a character lays out everything before it, so the rects
    // the editor reads (tab and Format Document scroll positions, the
    // agent bar, Markdown place keeping, several carets) are exact. The
    // price is in big files: an edit is laid out again from itself to where
    // NSTextView asks (1.9 s to lay out 100,000 lines, which is what a
    // Format Document touching both ends of such a file costs).
    // Non-contiguous layout would remove that but makes far positions
    // estimates; it needs trying on a real screen before it is turned on.
    // The backdrop paints the background over the whole scroll view; the text
    // view drawing it too would double a translucent color.
    self.textView.drawsBackground = NO;
    self.textView.textContainerInset = NSMakeSize(8, 8);
    self.textView.automaticQuoteSubstitutionEnabled = NO;
    self.textView.minSize = NSMakeSize(0, 0);
    self.textView.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    self.textView.verticallyResizable = YES;
    self.textView.horizontallyResizable = NO;   // lines wrap; see PanelScrollView
    self.textView.autoresizingMask = NSViewWidthSizable;
    self.textView.textContainer.widthTracksTextView = YES;
    textScroll.documentView = self.textView;
    self.gutter = [[MCLineGutter alloc] initWithTextView:self.textView];

    // Language servers: completion, go to definition, error underlines.
    self.lsp = [[LspSession alloc] initWithTextView:self.textView root:_root.path];
    // Jumps (definition, references, symbols, lines) are remembered for Ctrl+-.
    self.navigator = [[MCNavigator alloc] initWithTextView:self.textView host:self];
    self.lsp.aroundJump = ^(dispatch_block_t jump) {
        EditorController *s = weakSelf;
        if (s) [s.navigator jump:jump];
        else jump();
    };
    self.textView.onPreviewDoubleClick = ^BOOL(NSUInteger index, NSPoint point) {
        return [weakSelf editMarkdownAtCharacter:index point:point];
    };
    self.textView.onBlur = ^{ [weakSelf autoSaveOnFocusChange]; };
    self.textView.multiCarets.onNote = ^(NSString *note) { [weakSelf showStatusNote:note]; };
    self.lsp.onStatus =^(NSString *text, NSColor *dot) {
        [weakSelf.lspLabel setStatus:text dot:dot];
        [weakSelf updateInstallBar];
        [weakSelf updateProblemCounts];
    };
    self.lsp.onProblemsChanged = ^{ [weakSelf updateProblems]; };
    self.lsp.openFile = ^BOOL(NSString *p) {
        EditorController *s = weakSelf;
        if ([p hasPrefix:[s.rootPath stringByAppendingString:@"/"]])
            [s revealPath:p andOpen:YES];
        else
            [s openFileAtPath:p];
        // A tab opened under another spelling (/private/tmp for /tmp) is
        // the same file.
        return [s.currentPath isEqualToString:p] ||
               [s.currentPath.stringByResolvingSymlinksInPath
                   isEqualToString:p.stringByResolvingSymlinksInPath];
    };
    // Rename and quick fixes edit files in other tabs, and references show
    // their lines, from the tab's unsaved text.
    self.lsp.bufferText = ^NSString *(NSString *p) {
        EditorController *s = weakSelf;
        MCTab *t = [s tabForPath:p];
        if (!t || !t.loaded || t.showingMessage) return nil;
        if (t == s->_activeTab && s.textView.editable) return s.textView.string;
        return t.sourceText;
    };
    self.lsp.showNote = ^(NSString *message) { [weakSelf showStatusNote:message]; };
    self.lsp.shownTabPath = ^NSString *{ return weakSelf.currentPath; };
    // Every tab's file stays open on its server, with the tab's text.
    self.lsp.openPaths = ^NSArray<NSString *> *{
        EditorController *s = weakSelf;
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (MCTab *t in s ? s->_tabs : @[]) if (t.currentPath) [paths addObject:t.currentPath];
        return paths;
    };
    // A tab an edit opened, then could not change (it is unedited, so
    // nothing is asked).
    self.lsp.closeFile = ^(NSString *p) {
        EditorController *s = weakSelf;
        MCTab *t = [s tabForPath:p];
        if (t && !t.dirty) [s closeTab:t prompt:NO];
    };

    // Right side hosts the editor plus (lazily) a terminal dock and browser.
    self.rightArea = [[PanelHost alloc] initWithFrame:frame];
    self.rightArea.onLayout = ^{ [weakSelf relayoutRightArea]; };
    textScroll.frame = self.rightArea.bounds;
    [self.rightArea addSubview:textScroll];
    _editorFind = [[MCEditorFind alloc] initWithTextView:self.textView];
    _editorFind.onLayoutNeeded = ^{ [weakSelf relayoutRightArea]; };
    _editorFind.onNote = ^(NSString *note) { [weakSelf flashStatus:note]; };
    [self.rightArea addSubview:_editorFind.bar];
    [self buildTabStrip];

    [split addSubview:treeScroll];
    [split addSubview:self.rightArea];

    // Container holds, back to front: the blur, the title bar strip, the split
    // view, the status bar (bottom) and the hints overlay. layoutContainer
    // places them by hand.
    PanelHost *container = [[PanelHost alloc] initWithFrame:frame];
    self.blurView = [[NSVisualEffectView alloc] initWithFrame:frame];
    self.blurView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    self.blurView.state = NSVisualEffectStateActive;   // stays frosted when inactive
    self.blurView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [container addSubview:self.blurView];
    [self buildTitlebarInContainer:container];
    [container addSubview:split];
    [self buildStatusBarInContainer:container height:kStatusBarHeight];
    [self buildHintsPanelInContainer:container];

    self.window.contentView = container;
    container.onLayout = ^{ [weakSelf layoutContainer]; };
    [self applySettings];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(applySettings)
               name:MCSettingsDidChangeNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(keymapChanged:)
               name:MCKeymapDidChangeNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(editorZoomChanged:)
               name:MCEditorZoomDidChangeNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(markdownWebImageArrived:)
               name:MCMarkdownWebImageNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(markdownMathArrived:)
               name:MCMathTypesetNotification object:nil];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(treeDetailsChanged:)
               name:MCTreeDetailsDidChangeNotification object:nil];
    [self updateDetailsTimer];
    [self.window addObserver:self forKeyPath:@"title"
                     options:NSKeyValueObservingOptionInitial context:NULL];
    [self showEmptyState];
    [self.window makeKeyAndOrderFront:nil];
    [self.outline reloadData];

    // Set the divider AFTER the window is on screen and laid out — otherwise
    // autoresizing collapses the sidebar to zero width.
    [self.window layoutIfNeeded];
    [self layoutContainer];
    [split adjustSubviews];
    [split setPosition:260 ofDividerAtIndex:0];
    [self.outline sizeLastColumnToFit];

    [self startWatching:_root.path];   // live tree updates
    [self restoreSession];             // the files open here last time
    [self restoreBrowserFromSession];  // and the browser, where it was
    [[NSNotificationCenter defaultCenter] addObserver:self
        selector:@selector(backupsAppWillTerminate:)
            name:NSApplicationWillTerminateNotification object:nil];
}

// ------------------------------------------------------------ layout + look
// Height of the title bar the content runs under (0 in full screen). Before
// the window is on screen contentLayoutRect is empty, so ask the frame math.
- (CGFloat)titlebarHeight {
    NSWindow *w = self.window;
    NSRect layout = w.contentLayoutRect;
    if (NSIsEmptyRect(layout)) {
        NSRect frame = w.frame;
        return MAX(0, frame.size.height -
                      [NSWindow contentRectForFrameRect:frame
                                              styleMask:w.styleMask &
                                  ~NSWindowStyleMaskFullSizeContentView].size.height);
    }
    return MAX(0, NSMaxY(w.contentView.bounds) - NSMaxY(layout));
}

- (void)layoutContainer {
    NSView *c = self.window.contentView;
    if (!c || !self.splitView) return;
    CGFloat W = c.bounds.size.width, H = c.bounds.size.height;
    CGFloat titleH = [self titlebarHeight];
    self.blurView.frame = c.bounds;
    self.titlebarView.frame = NSMakeRect(0, H - titleH, W, titleH);
    // Traffic lights on the left; keep the title centered on the window.
    self.titleLabel.frame = NSMakeRect(80, floor((titleH - 16) / 2), MAX(0, W - 160), 16);
    self.splitView.frame = NSMakeRect(0, kStatusBarHeight, W,
                                      MAX(0, H - kStatusBarHeight - titleH));
    self.statusBar.frame = NSMakeRect(0, 0, W, kStatusBarHeight);
    if (self.hintsVisible) [self updateHints];
}

- (void)buildTitlebarInContainer:(NSView *)container {
    self.titlebarView = [[NSView alloc] initWithFrame:NSZeroRect];
    self.titlebarView.wantsLayer = YES;
    self.titlebarView.menu = MCAdjustSettingsMenu(@"titlebar", self.titlebarView);
    NSTextField *label = [NSTextField labelWithString:@""];
    label.font = [NSFont titleBarFontOfSize:0];
    label.alignment = NSTextAlignmentCenter;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.titleLabel = label;
    [self.titlebarView addSubview:label];
    [container addSubview:self.titlebarView];
}

// Apply the settings file: window transparency and blur, the title bar, and
// each panel's background and text colors. Runs at startup and whenever the
// file changes.
- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    const Settings &st = cfg.settings;
    NSWindow *w = self.window;

    BOOL opaque = st.windowIsOpaque();
    w.opaque = opaque;
    w.backgroundColor = opaque ? [NSColor windowBackgroundColor]
                               : [NSColor clearColor];
    self.blurView.hidden = !st.blur();
    self.blurView.material = cfg.material;
    // The window's own controls (scroll bars, text fields, the find bar,
    // menus opened in it) go light or dark with the theme as drawn.
    NSAppearance *look = [NSAppearance appearanceNamed:st.isDark() ? NSAppearanceNameDarkAqua
                                                                   : NSAppearanceNameAqua];
    if (![w.appearance.name isEqualToString:look.name]) w.appearance = look;
    self.blurView.appearance = look;

    [_editorFind applySettings];

    BOOL custom = st.customTitlebar();
    w.titlebarAppearsTransparent = custom;
    w.titleVisibility = custom ? NSWindowTitleHidden : NSWindowTitleVisible;
    w.titlebarSeparatorStyle = custom ? NSTitlebarSeparatorStyleNone
                                      : NSTitlebarSeparatorStyleAutomatic;
    self.titlebarView.hidden = !custom;
    self.titlebarView.layer.backgroundColor = [cfg background:Surface::Titlebar].CGColor;
    self.titleLabel.textColor = [cfg text:Surface::Titlebar];

    self.sidebarScroll.panelColor = [cfg background:Surface::Sidebar];
    [self refreshTreeCells];   // names, and their sizes and dates

    self.editorScroll.panelColor = [cfg background:Surface::Editor];
    // A selection keeps the syntax colors under it, in every theme (AppKit's
    // own selected-text color took over after a switch between light and dark).
    self.textView.selectedTextAttributes =
        @{NSBackgroundColorAttributeName: [NSColor selectedTextBackgroundColor]};
    for (MCTab *t in _tabs) [t.latex applySettings];
    [self applyTabSettings];
    self.imageView.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    self.pdfView.backgroundColor = [cfg background:Surface::Editor];
    self.playerView.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    self.diffScroll.panelColor = [cfg background:Surface::Editor];
    if (self.isDiff && self.diffData) [self renderDiff];
    [self.gitPanel applySettings];
    if (_agents.started) [_agents.panel applySettings];
    for (MCTab *t in _tabs)
        if ([t.page respondsToSelector:@selector(pageApplySettings)]) [t.page pageApplySettings];
    self.textView.insertionPointColor = [cfg text:Surface::Editor];
    NSMutableArray *rulers = [NSMutableArray array];
    for (int column : st.rulers()) [rulers addObject:@(column)];
    self.textView.rulerColumns = rulers;
    self.textView.rulerColor = MCColor(st.rulerColor());
    [self applyGutterSettings];   // and the font, which recolorEditor sets in the text
    [self recolorEditor];

    self.statusBar.layer.backgroundColor = [cfg background:Surface::Statusbar].CGColor;
    self.statusLabel.textColor = [cfg text:Surface::Statusbar];
    self.restartButton.attributedTitle = [[NSAttributedString alloc] initWithString:@"↻"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13],
                     NSForegroundColorAttributeName: [cfg text:Surface::Statusbar]}];
    self.settingsLabel.textColor = [cfg text:Surface::Statusbar];
    self.savedLabel.textColor = [cfg text:Surface::Statusbar];
    self.lspLabel.textColor = [cfg text:Surface::Statusbar];
    self.problemsLabel.textColor = [cfg text:Surface::Statusbar];
    [self layoutStatusRight];
    [self.problemsPanel applySettings];
    [self applyInstallBarColors];
    [self applyChangeBarColors];
    [_agent applyColors];
    [self showSettingsErrors];

    [w invalidateShadow];
    [self layoutContainer];
}

// Problems in settings.conf, then in keys.conf (Keymap.h), on the left of
// the status bar: the first, with a count of the rest; all in the tooltip.
- (void)showSettingsErrors {
    AppSettings *cfg = [AppSettings shared];
    MCKeymap *keys = [MCKeymap shared];
    NSMutableArray<NSString *> *errors = [NSMutableArray array];
    for (NSString *e in cfg.errors) [errors addObject:[@"Settings " stringByAppendingString:e]];
    for (NSString *e in keys.errors) [errors addObject:[@"Keys " stringByAppendingString:e]];
    self.settingsLabel.stringValue = errors.count == 0 ? @"" :
        [NSString stringWithFormat:@"%@%@", errors.firstObject,
            errors.count > 1
                ? [NSString stringWithFormat:@" (and %lu more)",
                   (unsigned long)errors.count - 1] : @""];
    NSMutableArray<NSString *> *tip = [NSMutableArray array];
    if (cfg.errors.count)
        [tip addObject:[NSString stringWithFormat:@"%@\n%@", cfg.path,
                        [cfg.errors componentsJoinedByString:@"\n"]]];
    if (keys.errors.count)
        [tip addObject:[NSString stringWithFormat:@"%@\n%@", keys.path,
                        [keys.errors componentsJoinedByString:@"\n"]]];
    self.settingsLabel.toolTip = tip.count ? [tip componentsJoinedByString:@"\n\n"] : nil;
}

// keys.conf changed: the menus have their new keys already; the status bar
// and the shortcut hints follow.
- (void)keymapChanged:(NSNotification *)note {
    (void)note;
    [self showSettingsErrors];
    if (!self.hintsPanel.hidden) [self updateHints];
}

// Redraw the open document in the current colors, keeping the scroll
// position and selection.
- (void)recolorEditor {
    if (self.showingMessage || !self.currentPath) return;
    if ([self showsRenderedMarkdown]) {
        NSPoint origin = self.editorScroll.contentView.bounds.origin;
        [self renderMarkdown:[self markdownToRender]];
        [self.editorScroll.contentView scrollToPoint:origin];
        [self.editorScroll reflectScrolledClipView:self.editorScroll.contentView];
    } else {
        NSMutableDictionary *typing = [self.textView.typingAttributes mutableCopy];
        typing[NSForegroundColorAttributeName] =
            [[AppSettings shared] text:Surface::Editor];
        self.textView.typingAttributes = typing;
        [self applyHighlighting];
    }
}

// -------------------------------------------------------- terminal + browser
// The top of the pane holds the file editor (or whatever has its slot: the
// LaTeX preview, an image, a PDF, a player, a diff) on the left and the
// browser on the right, with a vertical bar between them; either can be
// hidden and the other takes the whole width. The terminal docks below both,
// across the full width, or fills the pane when the top is empty.
// The smallest top while dragging: the tab strip and 40 points of editor.
static const CGFloat kMinTopSlotHeight = 40 + kMCTabStripHeight;
static const CGFloat kTopSnapDistance  = 16;   // bar this close to the top hides it
static const CGFloat kMinSideWidth     = 150;  // editor or browser beside the other
static const CGFloat kSideSnapDistance = 16;   // bar this close to a side hides that pane

- (void)relayoutRightArea {
    NSRect b = self.rightArea.bounds;
    // Request a Change's bar runs across the top, over editor and browser.
    b = [self layoutChangeBarIn:b];
    CGFloat W = b.size.width, H = b.size.height;
    BOOL showTerm = self.terminalVisible && self.terminalPanel != nil;
    BOOL showBrowser = self.browserVisible && self.browser != nil;
    BOOL showEditor = !self.editorHidden;
    BOOL topShown = showBrowser || showEditor;

    // The Problems list sits between the top and the terminal, at a fixed
    // share of the height; alone at the bottom it takes it all.
    BOOL showProblems = self.problemsVisible && self.problemsPanel != nil;
    CGFloat probH = 0;
    if (showProblems)
        probH = (topShown || showTerm)
            ? MIN(MAX(round(H * 0.3), 100), MAX(60, H - kMinTopSlotHeight - (showTerm ? 80 : 0)))
            : H;
    const CGFloat Hrest = MAX(0, H - probH);
    CGFloat termH = 0;
    if (showTerm)
        termH = topShown ? MIN(MAX(self.terminalHeight, 80),
                               MAX(80, Hrest - kMinTopSlotHeight))
                         : Hrest;
    NSRect topRect = NSMakeRect(0, termH + probH, W, MAX(0, Hrest - termH));
    if (self.problemsPanel) {
        self.problemsPanel.frame = NSMakeRect(0, termH, W, probH);
        self.problemsPanel.hidden = !showProblems;
    }

    // Side by side, the browser gets its remembered share of the width, but
    // neither pane goes below kMinSideWidth while there is room for both.
    NSRect editorRect = topRect, browserRect = topRect;
    BOOL sideBySide = showEditor && showBrowser;
    if (sideBySide) {
        CGFloat bw = round(W * self.browserFraction);
        if (W >= 2 * kMinSideWidth)
            bw = MIN(MAX(bw, kMinSideWidth), W - kMinSideWidth);
        else
            bw = round(W / 2);
        editorRect.size.width = W - bw;
        browserRect.origin.x = W - bw;
        browserRect.size.width = bw;
    }
    // The tab strip takes the top of the editor's side; the browser keeps
    // its whole height.
    editorRect = [self layoutTabStripIn:editorRect editorShown:showEditor];
    editorRect = [self layoutInstallBarIn:editorRect editorShown:showEditor];

    // The LaTeX preview sits in the editor's slot, in place of the text view.
    BOOL showLatex = showEditor && self.latex != nil && self.isLatex &&
                     self.previewMode;
    // So does an image, in place of the text view.
    BOOL showImage = showEditor && self.imageView != nil && self.isImage;
    BOOL showPDF = showEditor && self.pdfView != nil && self.isPDF;
    BOOL showMedia = showEditor && self.playerView != nil && self.isMedia;
    BOOL showCompare = showEditor && self.compareView != nil && self.isDiff &&
                       _activeTab.comparison != nil;
    BOOL showPage = showEditor && self.isDiff && _activeTab.page != nil;
    for (MCTab *t in _tabs) {
        if (!t.page) continue;
        NSView *v = t.page.pageView;
        const BOOL on = showPage && t == _activeTab;
        if (on && v.superview != self.rightArea)
            [self.rightArea addSubview:v positioned:NSWindowBelow relativeTo:self.termDivider];
        if (on) v.frame = editorRect;
        v.hidden = !on;
    }
    BOOL showDiff = showEditor && self.diffScroll != nil && self.isDiff && !showCompare && !showPage;
    // Quick Look and the other viewers (FileViewers.h): the whole slot, or
    // a part of it beside the text.
    BOOL showViewer = NO;
    NSRect textRect = _viewers ? [_viewers layoutInRect:editorRect editorShown:showEditor
                                             coversText:&showViewer]
                               : editorRect;
    self.editorScroll.frame = textRect;
    self.editorScroll.hidden = !showEditor || showLatex || showImage || showPDF ||
                               showMedia || showDiff || showCompare || showPage || showViewer;
    [_editorFind layoutInEditorRect:textRect visible:!self.editorScroll.hidden];
    if (self.diffScroll) {
        self.diffScroll.frame = editorRect;
        self.diffScroll.hidden = !showDiff;
    }
    if (self.compareView) {
        self.compareView.frame = editorRect;
        self.compareView.hidden = !showCompare;
    }
    if (self.pdfView) {
        self.pdfView.frame = editorRect;
        self.pdfView.hidden = !showPDF;
    }
    if (self.imageView) {
        self.imageView.frame = editorRect;
        self.imageView.hidden = !showImage;
    }
    if (self.playerView) {
        self.playerView.frame = editorRect;
        self.playerView.hidden = !showMedia;
    }
    if (self.latex) {
        self.latex.frame = editorRect;
        self.latex.hidden = !showLatex;
    }
    if (self.browser) {
        self.browser.frame = browserRect;
        self.browser.hidden = !showBrowser;
    }
    if (self.terminalPanel) {
        self.terminalPanel.frame = NSMakeRect(0, 0, W, termH);
        self.terminalPanel.hidden = !showTerm;
    }
    if (self.browserDivider) {
        self.browserDivider.frame =
            NSMakeRect(NSMinX(browserRect) - kDragBarHeight / 2, NSMinY(topRect),
                       kDragBarHeight, NSHeight(topRect));
        self.browserDivider.hidden = !sideBySide;
    }
    if (self.termDivider) {
        // With the top slot empty the bar sits at the top edge, still inside
        // the pane, so it can be dragged back down.
        CGFloat barY = topShown ? termH - kDragBarHeight / 2
                                : Hrest - kDragBarHeight;
        self.termDivider.frame = NSMakeRect(0, barY, W, kDragBarHeight);
        self.termDivider.hidden = !showTerm;
    }
    // Views added later (the browser, a PDF view) would otherwise sit on top
    // of the bars and swallow the clicks meant for them. The terminal's bar
    // goes last, so it wins where the two meet. Only moved when out of
    // order: moving a bar in the middle of its own drag is asking for trouble.
    NSMutableArray<NSView *> *bars = [NSMutableArray array];
    if (self.browserDivider) [bars addObject:self.browserDivider];
    if (self.termDivider) [bars addObject:self.termDivider];
    // The find bar floats over the editor, and the inline agent's bar over
    // everything, the drag bars too.
    if (!_editorFind.bar.hidden) [bars addObject:_editorFind.bar];
    if (_agent.shown) {
        [_agent layout];
        [bars addObject:_agent.bar];
    }
    NSArray<NSView *> *subs = self.rightArea.subviews;
    BOOL inOrder = subs.count >= bars.count &&
        [[subs subarrayWithRange:NSMakeRange(subs.count - bars.count, bars.count)]
            isEqualToArray:bars];
    if (!inOrder)
        for (NSView *bar in bars)
            [self.rightArea addSubview:bar positioned:NSWindowAbove relativeTo:nil];
}

// Dragging the terminal bar. Near the top it hides whatever is above the
// terminal (editor, and the browser if open); dragging down again brings the
// editor back.
- (void)terminalBarDraggedTo:(CGFloat)y {
    CGFloat H = self.rightArea.bounds.size.height;
    BOOL topShown = (self.browserVisible && self.browser) || !self.editorHidden;
    if (y >= H - kTopSnapDistance) {
        if (topShown) {
            self.editorHidden = YES;
            self.browserVisible = NO;
            [self.window makeFirstResponder:self.terminal];
            [self.terminal focusInput];
        }
    } else {
        if (!topShown) self.editorHidden = NO;
        self.terminalHeight = MIN(MAX(y, 80), MAX(80, H - kMinTopSlotHeight));
    }
    [self relayoutRightArea];
    if (self.hintsVisible) [self updateHints];
}

// Dragging the bar between the editor and the browser. Near the left edge it
// hides the editor, near the right edge the browser; dragging back out in
// the same drag brings it back, as the terminal bar does.
- (void)browserBarDraggedTo:(CGFloat)x {
    CGFloat W = self.rightArea.bounds.size.width;
    if (W <= 0 || !self.browser) return;
    if (x <= kSideSnapDistance) {
        if (!self.editorHidden && self.browserVisible) {
            self.editorHidden = YES;
            [self.browser focusURLBar];
        }
    } else if (x >= W - kSideSnapDistance) {
        if (self.browserVisible && !self.editorHidden) {
            self.browserVisible = NO;
            [self.window makeFirstResponder:self.textView];
        }
    } else {
        if (self.editorHidden && self.browserVisible) self.editorHidden = NO;
        if (!self.browserVisible && !self.editorHidden) self.browserVisible = YES;
        CGFloat bw = MIN(MAX(W - x, kMinSideWidth), MAX(kMinSideWidth, W - kMinSideWidth));
        self.browserFraction = bw / W;
    }
    [self relayoutRightArea];
    if (self.hintsVisible) [self updateHints];
}

// ---- NSSplitViewDelegate: sidebar | right pane ----
// The right pane (rightArea: editor, terminal, browser) shrinks down to this
// width as the divider is dragged right; NSSplitView collapses it once the
// pointer is past the midpoint of what's left, like the GTK port's paned.
static const CGFloat kMinRightAreaWidth = 150;
// The thin divider is drawn 1px wide; this much on each side also grabs it.
// NSSplitView's default is 2px, which is easy to miss.
static const CGFloat kDividerGrabSlop = 5;

- (BOOL)rightAreaCollapsed {
    return [self.splitView isSubviewCollapsed:self.rightArea];
}
- (BOOL)splitView:(NSSplitView *)sv shouldAdjustSizeOfSubview:(NSView *)view {
    // On window resize, grow/shrink the right pane, not the sidebar, unless
    // the right pane is collapsed; then the sidebar takes the whole window.
    if (self.rightAreaCollapsed) return view != self.rightArea;
    return view != sv.subviews.firstObject;
}
- (NSRect)splitView:(NSSplitView *)sv effectiveRect:(NSRect)proposed
          forDrawnRect:(NSRect)drawn ofDividerAtIndex:(NSInteger)i {
    return NSInsetRect(drawn, -kDividerGrabSlop, 0);
}
- (BOOL)splitView:(NSSplitView *)sv canCollapseSubview:(NSView *)view {
    return view == self.rightArea;
}
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMinCoordinate:(CGFloat)min
               ofSubviewAt:(NSInteger)i {
    return self.sidebarCollapsed ? 0 : 160;   // allow full collapse via Cmd+B
}
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMaxCoordinate:(CGFloat)max
               ofSubviewAt:(NSInteger)i {
    return MAX(160, sv.bounds.size.width - kMinRightAreaWidth);
}

// Shift+Cmd+E: hide or restore the file editor/preview only. The terminal and
// browser stay put and take over its space.
- (void)toggleEditor:(id)sender {
    if (self.editorHidden || self.rightAreaCollapsed) {
        [self revealEditor];
        [self.window makeFirstResponder:self.textView];
        return;
    }
    self.editorHidden = YES;
    [self relayoutRightArea];
    if (self.terminalVisible && self.terminal) [self.terminal focusInput];
    else if (self.browserVisible && self.browser) [self.browser focusURLBar];
    else [self.window makeFirstResponder:self.outline];
    [self collapseRightAreaIfEmpty];
    if (self.hintsVisible) [self updateHints];
}

// Show the file editor again: unhide it and uncollapse the right pane. Called
// by anything that puts a file in front of the user, so opening a file never
// appears to do nothing.
- (void)revealEditor {
    BOOL wasHidden = self.editorHidden;
    self.editorHidden = NO;
    [self showRightArea];
    if (wasHidden) [self relayoutRightArea];
    if (self.hintsVisible) [self updateHints];
}

// Uncollapse the right pane if it was dragged or toggled shut.
- (void)showRightArea {
    if (!self.rightAreaCollapsed) return;
    CGFloat W = self.splitView.bounds.size.width;
    CGFloat width = self.sidebarWidthBeforeCollapse;
    if (width < 160) width = 260;
    width = MIN(width, MAX(160, W - kMinRightAreaWidth));
    [self.splitView setPosition:width ofDividerAtIndex:0];
}

// With the editor hidden and no terminal or browser, the right pane would be
// an empty panel; collapse it so the file tree fills the window.
- (void)collapseRightAreaIfEmpty {
    BOOL empty = self.editorHidden && !(self.terminalVisible && self.terminal) &&
                 !(self.problemsVisible && self.problemsPanel) &&
                 !(self.browserVisible && self.browser);
    if (!empty || self.rightAreaCollapsed) return;
    NSView *sidebar = self.splitView.subviews.firstObject;
    self.sidebarWidthBeforeCollapse = sidebar.frame.size.width;
    self.sidebarCollapsed = NO;   // never leave the window with nothing in it
    // With canCollapseSubview:, moving the divider to the far edge collapses.
    [self.splitView setPosition:self.splitView.bounds.size.width
               ofDividerAtIndex:0];
    [self.window makeFirstResponder:self.outline];
}

// Cmd+B: collapse or restore the file-tree sidebar.
- (void)toggleSidebar:(id)sender {
    self.sidebarCollapsed = !self.sidebarCollapsed;
    [self.splitView setPosition:(self.sidebarCollapsed ? 0 : 260)
               ofDividerAtIndex:0];
}

// Ctrl+Shift+G: the Source Control panel takes the file tree's place in the
// split view, and gives it back when toggled again. The sidebar's width is
// kept, and a collapsed sidebar opens for it.
- (void)toggleSourceControl:(id)sender {
    if (!self.gitPanel) {
        self.gitPanel = [[MCGitPanel alloc] initWithRoot:_root.path];
        __weak EditorController *weakSelf = self;
        self.gitPanel.onShowDiff = ^(NSString *name, NSString *path, NSData *diff) {
            [weakSelf showDiffNamed:name path:path data:diff];
        };
        self.gitPanel.onShowCommit = ^(NSString *title, NSData *show) {
            [weakSelf showCommitTitled:title data:show];
        };
    }
    NSView *from = [self sidebarViewOnShow];
    NSView *to = self.gitPanelVisible ? self.sidebarScroll : self.gitPanel;
    to.frame = from.frame;
    if (from != to) [self.splitView replaceSubview:from with:to];
    self.gitPanelVisible = !self.gitPanelVisible;
    self.agentsVisible = NO;
    if (self.sidebarCollapsed || from.frame.size.width < 1) {
        self.sidebarCollapsed = NO;
        [self.splitView setPosition:260 ofDividerAtIndex:0];
    }
    if (self.gitPanelVisible) {
        [self.gitPanel refresh];
        [self.gitPanel focusList];
    } else {
        [self.window makeFirstResponder:self.outline];
    }
    if (self.hintsVisible) [self updateHints];
}

// What the sidebar's place holds now: the tree, Source Control or Agents.
- (NSView *)sidebarViewOnShow {
    return self.agentsVisible ? _agents.panel : self.gitPanelVisible ? self.gitPanel : self.sidebarScroll;
}

// Shift+Cmd+A: the Agents panel takes the sidebar's place as Source Control
// does (AgentsPanel.h), and gives the tree back when toggled again.
- (void)toggleAgents:(id)sender {
    (void)sender;
    NSView *from = [self sidebarViewOnShow];
    NSView *to = self.agentsVisible ? self.sidebarScroll : _agents.panel;
    to.frame = from.frame;
    if (from != to) [self.splitView replaceSubview:from with:to];
    self.agentsVisible = !self.agentsVisible;
    self.gitPanelVisible = NO;
    if (self.sidebarCollapsed || from.frame.size.width < 1) {
        self.sidebarCollapsed = NO;
        [self.splitView setPosition:260 ofDividerAtIndex:0];
    }
    if (self.agentsVisible) [_agents.panel focusList];
    else [self.window makeFirstResponder:self.outline];
    if (self.hintsVisible) [self updateHints];
}

// m:ss, or h:mm:ss from an hour up.
static NSString *MCFormatDuration(double seconds) {
    long t = (long)llround(seconds);
    long h = t / 3600, m = (t / 60) % 60, s = t % 60;
    return h > 0 ? [NSString stringWithFormat:@"%ld:%02ld:%02ld", h, m, s]
                 : [NSString stringWithFormat:@"%ld:%02ld", m, s];
}

- (void)updateTitle {
    // With no file open, the folder's name.
    NSString *name = self.currentPath.lastPathComponent ?: _root.path.lastPathComponent;
    if (!name.length) name = @"MiniCode";
    if (_activeTab.gone) name = [name stringByAppendingString:@" (deleted)"];
    if (self.isDiff)
        name = _activeTab.page ? (_activeTab.page.pageTitle ?: @"")
             : _activeTab.comparison ? (_activeTab.comparison.title ?: @"Diff")
             : self.diffIsCommit ? self.diffName
                                 : [NSString stringWithFormat:@"%@ (diff)", self.diffName];
    NSString *flag = self.dirty ? @"● " : @"";
    NSString *mode = (self.canTogglePreview && self.previewMode) ? @"  [Preview]" : @"";
    if (self.isImage)
        mode = [NSString stringWithFormat:@"  %.0f × %.0f",
                self.imagePixels.width, self.imagePixels.height];
    if (self.isPDF) {
        NSUInteger n = self.pdfView.document.pageCount;
        mode = n == 1 ? @"  1 page"
                      : [NSString stringWithFormat:@"  %lu pages", (unsigned long)n];
    }
    if (self.isMedia) {
        // Video: pixel size as shown (rotation applied), then the duration.
        // Audio has no picture, so only the duration.
        NSMutableString *m = [NSMutableString string];
        NSSize px = self.mediaPixels;
        if (px.width > 0 && px.height > 0)
            [m appendFormat:@"  %.0f × %.0f", px.width, px.height];
        if (self.mediaSeconds >= 0)
            [m appendFormat:@"  %@", MCFormatDuration(self.mediaSeconds)];
        mode = m;
    }
    // A Markdown file with tasks: how many are done.
    if (self.isMarkdown && self.currentPath && !self.isDiff) {
        NSString *src = self.textView.editable ? self.textView.string : self.sourceText;
        MarkdownTasks::Counts c = MarkdownTasks::count(Utf8String(src ?: @""));
        if (c.total > 0)
            mode = [mode stringByAppendingFormat:@"  %d of %d done", c.done, c.total];
    }
    self.window.title = [NSString stringWithFormat:@"%@%@ — MiniCode%@",
                         flag, name, mode];
    // The close button's dot: unsaved changes in any tab.
    self.window.documentEdited = [self hasUnsavedChanges];
    [self reloadTabStrip];   // the dots on the tabs
    [self updateSavedStatus];
    if (self.hintsVisible) [self updateHints];
    [self scheduleBackups];   // dirty may have changed either way
    [self scheduleAutoSave];
}

@end
