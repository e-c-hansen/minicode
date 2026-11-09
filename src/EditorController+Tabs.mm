// EditorController+Tabs.mm — EditorController: tabs, closed tabs, a tab's menu, saving.
#import "EditorController+Private.h"
#import "EditorController+TabsIvars.h"
#import "TextKit2.h"

@implementation EditorController (Tabs)

- (NSArray<NSString *> *)recentTabPaths {
    NSArray<MCTab *> *byUse = [_tabs sortedArrayUsingComparator:^NSComparisonResult(MCTab *a, MCTab *b) {
        return a.lastActive == b.lastActive ? NSOrderedSame
             : a.lastActive > b.lastActive  ? NSOrderedAscending : NSOrderedDescending;
    }];
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (MCTab *t in byUse)
        if (t.currentPath) [paths addObject:t.currentPath];
    return paths;
}

// ---------------------------------------------------------- a tab's menu
// A right click on a tab: closing it and its neighbours, and the file
// commands the tree offers for its file. Each item carries its tab.
- (NSMenu *)menuForTabAtIndex:(NSInteger)i {
    NSMutableArray<MCTab *> *group = [self groupTabs];
    if (i < 0 || i >= (NSInteger)group.count) return nil;
    MCTab *t = group[(NSUInteger)i];
    NSMenu *m = [[NSMenu alloc] init];
    m.autoenablesItems = NO;
    NSMenuItem *(^add)(NSString *, SEL, BOOL) = ^NSMenuItem *(NSString *title, SEL a, BOOL on) {
        NSMenuItem *it = [m addItemWithTitle:title action:a keyEquivalent:@""];
        it.target = self;
        it.representedObject = t;
        it.enabled = on;
        return it;
    };
    const NSInteger count = (NSInteger)group.count;
    if (t.preview || t.agentPreview) {   // VS Code's Keep Open, for a preview tab
        add(@"Keep Open", @selector(keepTabFromMenu:), YES);
        [m addItem:[NSMenuItem separatorItem]];
    }
    add(@"Close", @selector(closeTabFromMenu:), YES);
    add(@"Close Other Tabs", @selector(closeOtherTabsFromMenu:), count > 1);
    add(@"Close Tabs to the Right", @selector(closeTabsToTheRightFromMenu:), i < count - 1);
    add(@"Close All Tabs", @selector(closeAllTabsFromMenu:), YES);
    NSString *path = t.currentPath;
    if (path) {
        const BOOL inRoot = [self pathFromRoot:path] != nil;
        const BOOL onDisk = [[NSFileManager defaultManager] fileExistsAtPath:path];
        [m addItem:[NSMenuItem separatorItem]];
        add(@"Copy Path", @selector(copyTabPath:), YES);
        add(@"Copy Relative Path", @selector(copyTabRelativePath:), inRoot);
        add(@"Reveal in Finder", @selector(revealTabInFinder:), onDisk);
        add(@"Reveal in File Tree", @selector(revealTabInTree:), inRoot && onDisk);
        [m addItem:[NSMenuItem separatorItem]];
        add(@"Rename…", @selector(renameTabFile:), onDisk);
        add(@"Move to Trash", @selector(trashTabFile:), onDisk);
    }
    return m;
}

- (MCTab *)tabOfMenuItem:(id)sender {
    id t = [sender respondsToSelector:@selector(representedObject)]
               ? [sender representedObject] : nil;
    return [t isKindOfClass:MCTab.class] && [_tabs containsObject:t] ? t : nil;
}

- (void)closeTabFromMenu:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t) [self closeTab:t prompt:YES];
}

// Closes `tabs` in turn; Cancel at any unsaved one stops there.
- (void)closeTabsInList:(NSArray<MCTab *> *)tabs {
    for (MCTab *each in tabs)
        if (![self closeTab:each prompt:YES]) return;
}

- (void)closeOtherTabsFromMenu:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (!t) return;
    NSMutableArray *others = [[self groupTabs] mutableCopy];
    [others removeObject:t];
    [self closeTabsInList:others];
}

- (void)closeTabsToTheRightFromMenu:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (!t) return;
    NSMutableArray<MCTab *> *group = [self groupTabs];
    const NSUInteger i = [group indexOfObject:t];
    if (i == NSNotFound) return;
    [self closeTabsInList:[group subarrayWithRange:NSMakeRange(i + 1, group.count - i - 1)]];
}

- (void)closeAllTabsFromMenu:(id)sender {
    [self closeTabsInList:[[self groupTabs] copy]];
}

- (void)copyTabPath:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self copyText:t.currentPath];
}

- (void)copyTabRelativePath:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    NSString *rel = t.currentPath ? [self pathFromRoot:t.currentPath] : nil;
    if (rel) [self copyText:rel];
}

- (void)revealTabInFinder:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self revealPathInFinder:t.currentPath];
}

// Selects the tab's file in the tree, bringing the tree back if the sidebar
// is collapsed or shows Source Control.
- (void)revealTabInTree:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    NSString *rel = t.currentPath ? [self pathFromRoot:t.currentPath] : nil;
    if (!rel) return;
    if (self.gitPanelVisible) [self toggleSourceControl:nil];
    if (self.agentsVisible) [self toggleAgents:nil];
    if (self.sidebarCollapsed) [self toggleSidebarVisibility:nil];
    // Spelled from the tree's root, which revealPath: walks down from.
    [self revealPath:[rel isEqualToString:@"."] ? _root.path
                                                : [_root.path stringByAppendingPathComponent:rel]
             andOpen:NO];
    [self.window makeFirstResponder:self.outline];
}

// The same commands in the main menu (File), for the tab on show, so the
// menu bar and the command palette have them too.
- (NSMenuItem *)menuItemForActiveTab {
    NSMenuItem *it = [[NSMenuItem alloc] init];
    it.representedObject = _activeTab;
    return it;
}
- (void)closeOtherTabs:(id)sender {
    if (_activeTab) [self closeOtherTabsFromMenu:[self menuItemForActiveTab]];
}
- (void)closeTabsToTheRight:(id)sender {
    if (_activeTab) [self closeTabsToTheRightFromMenu:[self menuItemForActiveTab]];
}
- (void)closeAllTabs:(id)sender { [self closeAllTabsFromMenu:sender]; }
- (void)keepActiveTabOpen:(id)sender {
    if (_activeTab.preview || _activeTab.agentPreview) [self keepPreviewTab:_activeTab];
    else NSBeep();
}
- (void)copyActiveFilePath:(id)sender { [self copyTabPath:[self menuItemForActiveTab]]; }
- (void)copyActiveFileRelativePath:(id)sender {
    [self copyTabRelativePath:[self menuItemForActiveTab]];
}
- (void)revealActiveFileInFinder:(id)sender {
    [self revealTabInFinder:[self menuItemForActiveTab]];
}
- (void)revealActiveFileInTree:(id)sender {
    [self revealTabInTree:[self menuItemForActiveTab]];
}

// What those items can do for the tab on show, as its right click menu
// decides it; NO for an action that is not one of them.
- (BOOL)validateActiveTabAction:(SEL)a enabled:(BOOL *)enabled {
    MCTab *t = _activeTab;
    NSMutableArray<MCTab *> *group = [self groupTabs];
    const NSInteger count = (NSInteger)group.count;
    const NSInteger i = t ? (NSInteger)[group indexOfObject:t] : -1;
    NSString *path = t.currentPath;
    const BOOL inRoot = path && [self pathFromRoot:path] != nil;
    const BOOL onDisk = path && [[NSFileManager defaultManager] fileExistsAtPath:path];
    if (a == @selector(reopenClosedTab:)) *enabled = [self canReopenClosedTab];
    else if (a == @selector(closeOtherTabs:)) *enabled = t && count > 1;
    else if (a == @selector(closeTabsToTheRight:)) *enabled = t && i < count - 1;
    else if (a == @selector(closeAllTabs:)) *enabled = count > 0;
    else if (a == @selector(keepActiveTabOpen:)) *enabled = t.preview || t.agentPreview;
    else if (a == @selector(copyActiveFilePath:)) *enabled = path != nil;
    else if (a == @selector(copyActiveFileRelativePath:)) *enabled = inRoot;
    else if (a == @selector(revealActiveFileInFinder:)) *enabled = onDisk;
    else if (a == @selector(revealActiveFileInTree:)) *enabled = inRoot && onDisk;
    else return NO;
    return YES;
}

- (void)renameTabFile:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self renamePath:t.currentPath];
}

- (void)trashTabFile:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self trashPath:t.currentPath];
}

// Typing, and edits made from the Markdown preview, are undone per tab.
- (NSUndoManager *)undoManagerForTextView:(NSTextView *)view {
    return _activeTab ? _activeTab.undoManager : _noTabUndo;
}

- (BOOL)hasUnsavedChanges {
    for (MCTab *t in _tabs) if (t.dirty) return YES;
    return NO;
}

- (void)buildTabStrip {
    MCTabStrip *strip = [[MCTabStrip alloc] initWithFrame:NSZeroRect];
    strip.hidden = YES;
    strip.surface = @"tabs";
    [self wireTabStrip:strip];
    [self.rightArea addSubview:strip];
    self.tabStrip = strip;
    // With no file open the slot is empty but for one muted line.
    NSTextField *label = [NSTextField labelWithString:@"No file open. Choose one in the file tree."];
    label.font = [NSFont systemFontOfSize:13];
    label.alignment = NSTextAlignmentCenter;
    label.hidden = YES;
    [self.rightArea addSubview:label];
    self.emptyLabel = label;
}

// Either group's strip: a press on the group without the keyboard moves
// the editor there first (EditorController+Groups.mm), so the index is
// always the focused group's.
- (void)wireTabStrip:(MCTabStrip *)strip {
    __weak EditorController *weakSelf = self;
    __weak MCTabStrip *weakStrip = strip;
    strip.onSelect = ^(NSInteger i) {
        [weakSelf focusGroupOfStrip:weakStrip];
        [weakSelf selectTabAtIndex:i];
    };
    strip.onClose = ^(NSInteger i) {
        [weakSelf focusGroupOfStrip:weakStrip];
        [weakSelf closeTabAtIndex:i];
    };
    strip.menuForTab = ^NSMenu *(NSInteger i) {
        [weakSelf focusGroupOfStrip:weakStrip];
        return [weakSelf menuForTabAtIndex:i];
    };
    strip.onDoubleClick = ^(NSInteger i) {
        [weakSelf focusGroupOfStrip:weakStrip];
        [weakSelf keepTabAtIndex:i];
    };
}

- (void)applyTabSettings {
    AppSettings *cfg = [AppSettings shared];
    self.tabStrip.stripColor = [cfg background:Surface::Sidebar];
    self.tabStrip.activeColor = [cfg background:Surface::Editor];
    self.tabStrip.activeText = [cfg text:Surface::Editor];
    self.tabStrip.inactiveText = [[cfg text:Surface::Sidebar] colorWithAlphaComponent:0.7];
    self.tabStrip.needsDisplay = YES;
    self.emptyLabel.textColor = Hex(0x9CA3AF);
    [self colorOtherGroupStrip];
}

// Called by relayoutRightArea with the editor's slot: the strip takes its
// top while any tab is open, and the empty line sits in it while none is.
// Returns what is left for the editor.
- (NSRect)layoutTabStripIn:(NSRect)slot editorShown:(BOOL)shown {
    const NSUInteger count = [self groupTabs].count;
    const BOOL strip = shown && count > 0;
    const BOOL empty = shown && count == 0;
    self.tabStrip.hidden = !strip;
    self.emptyLabel.hidden = !empty;
    if (empty)
        self.emptyLabel.frame = NSMakeRect(NSMinX(slot) + 16, NSMidY(slot) + 10,
                                           MAX(0, NSWidth(slot) - 32), 20);
    if (!strip) return slot;
    const CGFloat h = MIN(kMCTabStripHeight, NSHeight(slot));
    self.tabStrip.frame = NSMakeRect(NSMinX(slot), NSMaxY(slot) - h, NSWidth(slot), h);
    return NSMakeRect(NSMinX(slot), NSMinY(slot), NSWidth(slot), NSHeight(slot) - h);
}

// The strip again from the tabs: titles, dots for unsaved changes, the
// active one. The editor's slot is laid out again when the strip comes or goes.
- (void)reloadTabStrip {
    NSMutableArray<MCTab *> *group = [self groupTabs];
    const BOOL wasShown = !self.tabStrip.hidden;
    self.tabStrip.items = [self stripItemsForTabs:group];
    self.tabStrip.activeIndex = _activeTab ? (NSInteger)[group indexOfObject:_activeTab] : -1;
    [self reloadParkedStrip];
    if (wasShown != (group.count > 0)) [self relayoutRightArea];
}

- (NSArray<MCTabStripItem *> *)stripItemsForTabs:(NSArray<MCTab *> *)tabs {
    std::vector<std::string> paths;
    for (MCTab *t in tabs) paths.push_back(t.currentPath ? Utf8String(t.currentPath) : "");
    std::vector<std::string> names = EditorTabs::labels(paths);
    NSMutableArray<MCTabStripItem *> *items = [NSMutableArray array];
    for (NSUInteger i = 0; i < tabs.count; i++) {
        MCTab *t = tabs[i];
        MCTabStripItem *item = [MCTabStripItem new];
        if (t.page)
            item.title = t.page.pageTitle ?: @"";
        else if (t.comparison)
            item.title = t.comparison.title ?: @"Diff";
        else if (t.isDiff)
            item.title = t.diffIsCommit ? (t.diffName ?: @"Commit")
                                        : [NSString stringWithFormat:@"%@ (diff)", t.diffName ?: @""];
        else
            item.title = [NSString stringWithUTF8String:names[i].c_str()] ?: t.currentPath.lastPathComponent;
        if (t.gone) item.title = [item.title stringByAppendingString:@" (deleted)"];
        item.toolTip = t.currentPath ?: item.title;
        item.dirty = t.dirty;
        item.preview = t.agentPreview || t.preview;
        [items addObject:item];
    }
    return items;
}

- (MCTab *)tabForPath:(NSString *)path {
    for (MCTab *t in _tabs) if ([t.currentPath isEqualToString:path]) return t;
    // The same file spelled another way (/tmp and /private/tmp).
    NSString *real = path.stringByResolvingSymlinksInPath;
    for (MCTab *t in _tabs)
        if (t.currentPath && [t.currentPath.stringByResolvingSymlinksInPath isEqualToString:real])
            return t;
    return nil;
}

// ------------------------------------------------------ the preview tab
// VS Code's preview editor (workbench.editor.enablePreview): a file opened
// by a single click in the tree, a single click on a search result, or a
// code navigation goes in the window's one preview tab, its title in
// italics, which the next such open replaces; anything else opens in a
// kept tab. The rules are EditorTabs' (opensInPreview, staysPreview,
// placePreview). The agents' preview tabs (agentPreview) are kept apart:
// an agent's note waits in its own tab however the user browses, and the
// user's file is never taken away by an agent's open.

- (BOOL)opensInPreview:(EditorTabs::OpenFrom)from {
    const Settings &cfg = [AppSettings shared].settings;
    EditorTabs::PreviewSettings s;
    s.enabled = cfg.enablePreview();
    s.fromQuickOpen = cfg.enablePreviewFromQuickOpen();
    return EditorTabs::opensInPreview(from, s);
}

// The user's preview tab, if there is one.
- (MCTab *)previewTab {
    for (MCTab *t in [self groupTabs])
        if (t.preview) return t;
    return nil;
}

- (void)keepPreviewTab:(MCTab *)t {
    if (!t || !(t.preview || t.agentPreview)) return;
    t.preview = NO;
    t.agentPreview = NO;
    [self reloadTabStrip];
    [self saveSession];
}

// A double-click on a tab's title keeps it, as in VS Code.
- (void)keepTabAtIndex:(NSInteger)i {
    NSMutableArray<MCTab *> *group = [self groupTabs];
    if (i < 0 || i >= (NSInteger)group.count) return;
    [self keepPreviewTab:group[(NSUInteger)i]];
}

- (void)keepTabFromMenu:(id)sender {
    [self keepPreviewTab:[self tabOfMenuItem:sender]];
}

// Show a file in its tab, else in a new one: the preview tab when `from`
// opens in preview (replacing the old preview tab, which has no unsaved
// changes since an edit keeps it), else a kept tab just right of the one on
// show. The file in the tab that was on show stays as it was, unsaved
// changes and all.
- (void)openFileAtPath:(NSString *)path from:(EditorTabs::OpenFrom)from {
    [self revealEditor];
    if (!path) return;
    const BOOL inPreview = [self opensInPreview:from];
    MCTab *tab = [self tabForPath:path];
    if (tab) {
        // A kept open of the preview tab's file keeps it; an open in preview
        // never makes a kept tab a preview.
        const BOOL wasPreview = tab.preview || tab.agentPreview;
        if (wasPreview && !EditorTabs::staysPreview(true, inPreview)) [self keepPreviewTab:tab];
        if (tab != _activeTab) [self activateTab:tab];   // the one on show: nothing to render
        else [self reselectCurrentFileInTree];          // a Cmd+click may have deselected it
        return;
    }
    if (![self confirmOpeningLargeFile:path]) return;   // over 50 MB: asked first
    tab = [MCTab new];
    tab.currentPath = path;
    MCTab *old = inPreview ? [self previewTab] : nil;
    // An old preview with unsaved changes (from a backup) is never dropped.
    if (old.dirty) {
        [self keepPreviewTab:old];
        old = nil;
    }
    if (!inPreview) {
        [self insertTab:tab];
        [self activateTab:tab];
        return;
    }
    NSMutableArray<MCTab *> *group = [self groupTabs];
    const NSInteger active = _activeTab ? (NSInteger)[group indexOfObject:_activeTab] : -1;
    const NSInteger oldAt = old ? (NSInteger)[group indexOfObject:old] : -1;
    const EditorTabs::PreviewPlace place =
        EditorTabs::placePreview((int)active, (int)oldAt, (int)group.count);
    // `index` counts without the old tab, which is still in the list.
    NSUInteger at = (NSUInteger)place.index;
    if (place.replace >= 0 && place.index >= place.replace) at++;
    tab.preview = YES;
    [self groupInsertTab:tab atIndex:at];
    [self activateTab:tab];
    // Leaving it can have saved it (a Cmd+S waiting for its formatter),
    // which keeps it.
    if (old.preview && !old.dirty) [self dropPreviewTab:old];
}

// The preview tab a newer one replaced: no question (it has no unsaved
// changes) and no place in Reopen Closed Tab, as VS Code leaves a replaced
// preview editor out of its recently closed list.
- (void)dropPreviewTab:(MCTab *)t {
    if (t == _activeTab || ![[self groupTabs] containsObject:t]) return;
    if (![self otherGroupHoldsTab:t]) {   // else the document stays, there
        [self discardBackupOf:t];
        [t.latex removeFromSuperview];
        [self closePageOf:t];
    }
    [self groupRemoveTab:t];
    [self.lsp tabsChanged];
    [self reloadTabStrip];
    [self saveSession];
}

// A file a language server's answer names (a workspace edit's, or a code
// navigation's target): selected in the tree when it is in the folder, and
// opened. YES when it is now on show.
- (BOOL)openFileForLanguageServer:(NSString *)p from:(EditorTabs::OpenFrom)from {
    // VS Code's workbench.editor.enablePreviewFromCodeNavigation, off by
    // default: a preview tab a navigation starts from is kept, so the
    // target does not replace the file it was reached from.
    if (from == EditorTabs::OpenFrom::CodeNavigation && _activeTab.preview &&
        [self opensInPreview:from] && [self tabForPath:p] != _activeTab)
        [self keepPreviewTab:_activeTab];
    if ([p hasPrefix:[self.rootPath stringByAppendingString:@"/"]])
        [self revealPath:p open:YES from:from];
    else
        [self openFileAtPath:p from:from];
    // A tab opened under another spelling (/private/tmp for /tmp) is the
    // same file.
    return [self.currentPath isEqualToString:p] ||
           [self.currentPath.stringByResolvingSymlinksInPath
               isEqualToString:p.stringByResolvingSymlinksInPath];
}

- (void)openFileFromQuickOpen:(NSString *)path {
    [self revealPath:path open:YES from:EditorTabs::OpenFrom::QuickOpen];
}

- (void)insertTab:(MCTab *)tab {
    NSMutableArray<MCTab *> *group = [self groupTabs];
    NSInteger active = _activeTab ? (NSInteger)[group indexOfObject:_activeTab] : -1;
    [self groupInsertTab:tab
                 atIndex:(NSUInteger)EditorTabs::insertionIndex((int)active, (int)group.count)];
}

// Put a tab on show. The tab that was on show keeps its text, unsaved
// changes, undo stack and place; a tab shown for the first time reads its
// file, and one shown again is checked against the disk.
- (void)activateTab:(MCTab *)tab {
    if (!tab) return;
    [self revealEditor];
    [self ensureTabInFocusedGroup:tab];   // a tab of the other group's joins this one
    tab.lastActive = ++_tabStamp;
    if (tab == _activeTab) { [self reloadTabStrip]; return; }
    [self editorWillShowTab:tab];     // the other group lets go of the text that goes
    [self.navigator tabWillSwitch];   // Go Back returns to the tab left
    [_notes activeTabWillChange];
    [self stashActiveTab];
    if (_activeTab) [self autoSaveOnFocusChange];   // the tab that leaves (focus-change)
    _activeTab = tab;
    if (!tab.loaded) {
        [self loadActiveTab];
    } else {
        [self showActiveTab];
        [self checkExternalChange];
    }
    [self reselectCurrentFileInTree];
    [self reloadTabStrip];
    [self saveSession];
    [self.navigator tabDidSwitch];
    [_notes activeTabDidChange];
    [self syncEditorMirror];   // the other group, if it shows this tab too
}

// The active tab is about to leave the screen: remember where its view
// was, and let go of what the shared views hold for it (a playing video
// stops).
- (void)stashActiveTab {
    MCTab *t = _activeTab;
    if (!t) return;
    [_layoutCatchUp cancel];   // the text goes; its place is read from what is laid out
    [self saveBeforeLeavingFormatSave];   // Cmd+S is never lost to a tab switch
    NSTextView *tv = self.textView;
    [self.markdownEditPanel close];
    [self sidePreviewTabWillLeave:t];   // the page beside the source: where it was
    [_viewers commitEditing];   // a CSV cell typed into lands in this tab's file
    // A color pick saves the settings file a moment later (colorPicked:);
    // that save belongs to this tab, so it happens now, not in the next one.
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(saveCurrentFile:)
                                               object:nil];
    if (t.dirty && [t.currentPath isEqualToString:self.colorEditPath] && [self isSettingsFile])
        [self saveCurrentFile:nil];
    [tv breakUndoCoalescing];   // typing in the next tab is a new undo step
    [self flushHighlighting];
    if (tv.editable && !t.showingMessage) t.sourceText = tv.string;
    if (tv.editable && !t.showingMessage) t.folds = [self.textView.folds snapshot];
    t.selection = tv.selectedRange;
    t.topCharacter = 0;
    t.topOffset = 0;
    const NSUInteger n = tv.string.length;
    if (n > 0 && !t.showingMessage && !(t.isLatex && t.previewMode)) {
        const NSRect visible = tv.visibleRect;
        NSUInteger top = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)];
        if (top >= n) top = n - 1;
        const NSRect r = [self textRectForCharacter:top];
        t.topCharacter = top;
        t.topOffset = NSIsEmptyRect(r) ? 0 : NSMinY(r) - NSMinY(visible);
    }
    t.haveView = YES;
    if (t.isDiff && self.diffScroll) t.diffScroll = self.diffScroll.contentView.bounds.origin;
    if (t.isDiff && t.comparison && self.compareView) t.diffScroll = self.compareView.scrollOrigin;
    t.sourceSelection = _sourceSelection;
    t.sourceCaretOffset = _sourceCaretOffset;
    t.haveSourcePosition = _haveSourcePosition;
    t.previewEntryScroll = _previewEntryScroll;
    if (t.isPDF && self.pdfView.document) {
        PDFDestination *at = self.pdfView.currentDestination;
        t.pdfPage = [self.pdfView.document indexForPage:at.page];
        t.pdfPoint = at.point;
        t.pdfScale = self.pdfView.scaleFactor;
        t.pdfAutoScales = self.pdfView.autoScales;
    }
    if (t.isMedia && self.playerView.player)
        t.mediaTime = CMTimeGetSeconds(self.playerView.player.currentTime);
    // The shared views let go; the tab's own flags stay as they are.
    [_viewers clear];
    self.imageView.image = nil;
    self.pdfView.document = nil;
    [self.playerView.player pause];
    [self observeMediaItem:nil];
    self.playerView.player = nil;
    [self.diffView.textStorage setAttributedString:[NSAttributedString new]];
    [_awaitedWebImages removeAllObjects];
    [_awaitedMath removeAllObjects];
    t.latex.hidden = YES;
    _liveHighlight = NO;
}

// Show the active tab again from what it kept.
- (void)showActiveTab {
    [_agent documentChanged];   // an edit bar for the tab that left closes
    MCTab *t = _activeTab;
    NSString *path = t.currentPath;
    _sourceSelection = t.sourceSelection;
    _sourceCaretOffset = t.sourceCaretOffset;
    _haveSourcePosition = t.haveSourcePosition;
    _previewEntryScroll = t.previewEntryScroll;
    [self makeEditorReadOnly];   // a long text the tab that left kept goes first
    if (MCViewerKindWithoutText(t.viewerKind)) {
        if (![self showViewer:t.viewerKind]) [self loadActiveTab];
        return;
    }
    if (t.isImage) {
        if (![self showImageAtPath:path]) [self loadActiveTab];
        return;
    }
    if (t.isPDF) {
        if (![self showPDFAtPath:path]) { [self loadActiveTab]; return; }
        PDFDocument *doc = self.pdfView.document;
        if (t.haveView && t.pdfPage < doc.pageCount) {
            if (!t.pdfAutoScales) self.pdfView.scaleFactor = t.pdfScale;
            [self.pdfView goToDestination:[[PDFDestination alloc]
                initWithPage:[doc pageAtIndex:t.pdfPage] atPoint:t.pdfPoint]];
        }
        return;
    }
    if (t.isMedia) {
        const double at = t.mediaTime;
        if (![self showMediaAtPath:path]) { [self loadActiveTab]; return; }
        if (at > 0)
            [self.playerView.player seekToTime:CMTimeMakeWithSeconds(at, 600)
                               toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
        return;
    }
    if (t.isDiff) {
        [self setPlainMessage:@""];
        [self renderDiff];
        [self relayoutRightArea];
        if (t.comparison) self.compareView.scrollOrigin = t.diffScroll;
        else if (!t.page) [self.diffView scrollPoint:t.diffScroll];
        [self.lsp documentOpened:nil];
        [self updateTitle];
        return;
    }
    if (t.showingMessage) {
        [self setPlainMessage:t.message ?: @""];
        [self relayoutRightArea];
        [self.lsp documentOpened:nil];
        [self updateTitle];
        return;
    }
    [self refreshDisplay];
    [self restoreTextPlaceOf:t];
    // Sorted as it was saved in the background (saveTab:): the sort goes in
    // now, as one undo step, which leaves the buffer as the file is.
    if (t.pendingSort) {
        NSString *sorted = t.pendingSort;
        t.pendingSort = nil;
        if (self.textView.editable) {
            [self replaceSourceWithText:sorted actionName:@"Sort keep-sorted Blocks"];
        } else {
            t.sourceText = sorted;
            [self forgetHistoryOf:t];
            [self refreshDisplay];
        }
    }
    [self.lsp documentOpened:self.previewMode ? nil : [self languageServerPath:path]];
    [self updateTitle];
}

// How far down a tab's place may be before showing it again lays out only
// the place, the lines above caught up in the background: about 1,000
// lines, some tens of milliseconds laid out from the top.
static const NSUInteger kCatchUpFrom = 1 << 15;

// The selection and the scroll a tab kept (stashActiveTab), put back in the
// text view once it shows the tab's text.
//
// Far down an ordinary tab's text (editor N2), laying out from the top to
// the place took 1.5 to 3.9 s of the main thread at the middle of 100,000
// lines: the editor's layout is contiguous, so its rects are exact. The
// place is laid out alone instead, with non-contiguous layout, and
// MCLayoutCatchUp lays out the lines above it in idle turns, keeping the
// text on screen still, until everything from the top through the pane is
// laid out and every rect there is the contiguous one; it keeps it so after
// edits and new widths (LayoutCatchUp.h, also for why the flag stays on).
// Meanwhile every rect is read from the layout the text is drawn from (the
// stretch around the place), so carets, squiggles, boxes and the gutter
// agree with what is on screen; only a rect's distance from the top of the
// text is an estimate until the lines above it are laid out, as in
// large-file mode, which keeps non-contiguous layout and is left alone.
- (void)restoreTextPlaceOf:(MCTab *)t {
    if ((t.isLatex && t.previewMode) || !t.haveView) return;
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    [_layoutCatchUp cancel];
    NSRange sel = t.selection;
    if (sel.location > n) sel = NSMakeRange(n, 0);
    if (NSMaxRange(sel) > n) sel.length = n - sel.location;
    // The place decides, not the caret: a caret far below a pane at the top
    // lays out nothing until it is shown.
    const BOOL catchUp = tv.editable && !MCUsesTextKit2(tv) && !self.showingMessage &&
                         !tv.layoutManager.allowsNonContiguousLayout &&
                         MIN(t.topCharacter, n) > kCatchUpFrom;
    if (catchUp) {
        if (!_layoutCatchUp) {
            _layoutCatchUp = [[MCLayoutCatchUp alloc] initWithTextView:tv
                                                            scrollView:self.editorScroll];
            __weak EditorController *weakSelf = self;
            __weak NSTextView *weakView = tv;
            _layoutCatchUp.stillWanted = ^BOOL {
                EditorController *me = weakSelf;
                return me && weakView.editable && !me.showingMessage && ![me largeFileFeaturesOff];
            };
        }
        [_layoutCatchUp prepare];
    }
    if (t.folds.count && tv.editable) [self.textView.folds restore:t.folds];
    tv.selectedRange = sel;
    [self scrollEditorToCharacter:t.topCharacter offset:t.topOffset];
    if (catchUp) [_layoutCatchUp start];
    if (t.isMarkdown && t.previewMode)
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
}

// ------------------------------------------------- closed tabs, reopened
// Option+Shift+Cmd+T (VS Code's Shift+Cmd+T is minicode's Toggle Terminal):
// the last tab closed in this window comes back where it was in the strip,
// with its selection, scroll and preview as it left them. A file already
// open again is just shown; one no longer on disk is passed over.
static const NSUInteger kMaxClosedTabs = 50;

- (void)rememberClosedTab:(MCTab *)t index:(NSUInteger)index {
    if (!t.currentPath || t.isDiff) return;
    if (!_closedTabs) _closedTabs = [NSMutableArray array];
    NSMutableDictionary *c = [@{@"path": t.currentPath, @"index": @(index),
                                @"preview": @(t.previewMode),
                                @"sidePreview": @(t.sidePreview)} mutableCopy];
    if (t.haveView) {
        c[@"selection"] = [NSValue valueWithRange:t.selection];
        c[@"top"] = @(t.topCharacter);
        c[@"topOffset"] = @(t.topOffset);
    }
    [_closedTabs addObject:c];
    if (_closedTabs.count > kMaxClosedTabs) [_closedTabs removeObjectAtIndex:0];
}

- (BOOL)canReopenClosedTab { return _closedTabs.count > 0; }

- (void)reopenClosedTab:(id)sender {
    NSFileManager *fm = [NSFileManager defaultManager];
    while (_closedTabs.count) {
        NSDictionary *c = _closedTabs.lastObject;
        [_closedTabs removeLastObject];
        NSString *path = c[@"path"];
        if (MCTab *open = [self tabForPath:path]) {
            [self activateTab:open];
            [self focusActiveDocument];
            return;
        }
        BOOL dir = NO;
        if (![fm fileExistsAtPath:path isDirectory:&dir] || dir) continue;
        MCTab *t = [MCTab new];
        t.currentPath = path;
        t.sidePreview = [c[@"sidePreview"] boolValue];
        [self groupInsertTab:t atIndex:[c[@"index"] unsignedIntegerValue]];
        [self activateTab:t];   // reads the file
        if (_activeTab != t) return;
        const BOOL preview = [c[@"preview"] boolValue];
        if (self.canTogglePreview && self.previewMode != preview) {
            if (self.textView.editable) self.sourceText = self.textView.string;
            self.previewMode = preview;
            [self refreshDisplay];
            [self.lsp documentOpened:preview ? nil : [self languageServerPath:path]];
            [self updateTitle];
        }
        if (c[@"selection"]) {
            t.haveView = YES;
            t.selection = [c[@"selection"] rangeValue];
            t.topCharacter = [c[@"top"] unsignedIntegerValue];
            t.topOffset = [c[@"topOffset"] doubleValue];
            [self restoreTextPlaceOf:t];
        }
        [self focusActiveDocument];
        return;
    }
    NSBeep();
}

// The editor's slot with no tab open: nothing to edit, one muted line.
- (void)showEmptyState {
    [_notes activeTabWillChange];
    [self editorWillShowTab:nil];
    _activeTab = nil;
    [self resetViewMode];
    [self setPlainMessage:@""];
    self.textView.editable = NO;
    [self.lsp documentOpened:nil];
    [self reloadTabStrip];
    [self relayoutRightArea];
    [self updateTitle];
    [_notes activeTabDidChange];
}

- (void)focusActiveDocument {
    if ([self latexIsShowing]) [self.window makeFirstResponder:self.latex];
    else if (self.isMedia && self.playerView) [self.window makeFirstResponder:self.playerView];
    else if (self.isPDF && self.pdfView) [self.window makeFirstResponder:self.pdfView];
    else if (self.isDiff && _activeTab.page)
        [self.window makeFirstResponder:_activeTab.page.pageFocusView];
    else if (self.isDiff && _activeTab.comparison && self.compareView)
        [self.window makeFirstResponder:self.compareView.leftView];
    else if (self.isDiff && self.diffView) [self.window makeFirstResponder:self.diffView];
    else if (self.isImage) [self.window makeFirstResponder:nil];   // nothing in it takes keys
    else if (self.viewerKind != MCViewerNone && _viewers.focusView)
        [self.window makeFirstResponder:_viewers.focusView];
    else [self.window makeFirstResponder:self.textView];
}

- (void)selectTabAtIndex:(NSInteger)i {
    NSMutableArray<MCTab *> *group = [self groupTabs];
    if (i < 0 || i >= (NSInteger)group.count) return;
    [self activateTab:group[(NSUInteger)i]];
    [self focusActiveDocument];
}

- (void)closeTabAtIndex:(NSInteger)i {
    NSMutableArray<MCTab *> *group = [self groupTabs];
    if (i < 0 || i >= (NSInteger)group.count) return;
    [self closeTab:group[(NSUInteger)i] prompt:YES];
}

// Ctrl+Tab and Shift+Cmd+], and the other way.
- (void)selectNextTab:(id)sender { [self stepTab:1]; }
- (void)selectPreviousTab:(id)sender { [self stepTab:-1]; }
- (void)stepTab:(int)delta {
    NSMutableArray<MCTab *> *group = [self groupTabs];
    if (group.count == 0) { NSBeep(); return; }
    NSInteger active = _activeTab ? (NSInteger)[group indexOfObject:_activeTab] : -1;
    [self selectTabAtIndex:EditorTabs::step((int)active, (int)group.count, delta)];
}

// Shift+Cmd+1 to 9 and Ctrl+1 to 9; the menu item's tag is the number.
- (void)selectTabNumber:(id)sender {
    const int i = EditorTabs::indexForNumber((int)[sender tag], (int)[self groupTabs].count);
    if (i < 0) { NSBeep(); return; }
    [self selectTabAtIndex:i];
}

// YES when the tab may go: no unsaved changes, or the user saved them or
// let them go.
- (BOOL)confirmCloseTab:(MCTab *)t {
    // With auto-save on, what it would write once the tab goes is written
    // first, so it is not asked about (focus-change's save, scheduled for
    // the next turn of the run loop, would come after the question).
    if (t.dirty) [self autoSaveNow];
    if (!t.dirty) return YES;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Save changes to “%@”?",
                     t.currentPath.lastPathComponent ?: @"this file"];
    a.informativeText = @"Your changes will be lost if you don't save them.";
    [a addButtonWithTitle:@"Save"];         // 1000
    [a addButtonWithTitle:@"Don't Save"];   // 1001
    [a addButtonWithTitle:@"Cancel"];       // 1002
    NSModalResponse r = [a runModal];
    if (r == NSAlertFirstButtonReturn) return [self saveTab:t];
    return r == NSAlertSecondButtonReturn;
}

// Before the window closes, the app quits or another folder takes over:
// one question for every tab with unsaved changes.
- (BOOL)confirmCloseAllTabs {
    [self saveBeforeLeavingFormatSave];   // a Cmd+S still waiting for its formatter
    [self autoSaveNow];   // auto-save's writes first, so only what it skips is asked about
    NSMutableArray<MCTab *> *dirty = [NSMutableArray array];
    for (MCTab *t in _tabs) if (t.dirty) [dirty addObject:t];
    if (dirty.count == 0) return YES;
    if (dirty.count == 1) {
        [self activateTab:dirty[0]];   // so the file asked about is the one on show
        return [self confirmCloseTab:dirty[0]];
    }
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (MCTab *t in dirty) [names addObject:t.currentPath.lastPathComponent ?: @"?"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Save changes to %lu files?",
                     (unsigned long)dirty.count];
    a.informativeText = [NSString stringWithFormat:
        @"%@\n\nYour changes will be lost if you don't save them.",
        [names componentsJoinedByString:@"\n"]];
    [a addButtonWithTitle:@"Save All"];     // 1000
    [a addButtonWithTitle:@"Don't Save"];   // 1001
    [a addButtonWithTitle:@"Cancel"];       // 1002
    NSModalResponse r = [a runModal];
    if (r == NSAlertSecondButtonReturn) return YES;
    if (r != NSAlertFirstButtonReturn) return NO;
    // Every question about the disk first (Keep Mine, Reload, Cancel), and
    // only then any writing, so Cancel at any of them leaves every file and
    // every tab as it was.
    NSMutableArray<MCTab *> *write = [NSMutableArray array], *reload = [NSMutableArray array];
    for (MCTab *t in dirty) {
        switch ([self diskDecisionForTab:t]) {
        case MCDiskWrite: [write addObject:t]; break;
        case MCDiskReload: [reload addObject:t]; break;
        case MCDiskCancel: return NO;
        }
    }
    for (MCTab *t in reload) [self reloadTabFromDisk:t];
    _approvedWrites = [NSSet setWithArray:write];
    BOOL ok = YES;
    for (MCTab *t in write)
        if (![self saveTab:t]) { ok = NO; break; }
    _approvedWrites = nil;
    return ok;
}

// Write a tab's text to its file, YES when nothing is left unsaved. The
// active tab saves as a save MiniCode makes itself does (saveCurrentFile:
// with no sender). A tab in the background saves where it is by the same
// rules: the disk is checked first (Keep Mine, Reload, Cancel), and with
// editor.keep-sorted-on-save its blocks are sorted unless it shows a
// preview; the sort reaches its buffer as an undoable edit when the tab is
// next shown. format.on-save stays with Cmd+S on the tab on show, so a tab
// is never formatted unseen.
- (BOOL)saveTab:(MCTab *)t {
    if (t == _activeTab) {
        [self saveCurrentFile:nil];
        return !self.dirty;
    }
    if (!t.dirty || ![self tabHasTextToWrite:t]) return YES;
    if (![self mayWriteTab:t]) return !t.dirty;   // Reload leaves nothing to save
    NSString *text = t.sourceText ?: @"";
    NSString *sortProblem = nil;
    const BOOL preview = (t.isMarkdown || t.isLatex) && t.previewMode;
    if (!preview && [AppSettings shared].settings.keepSortedOnSave())
        text = MCKeepSortedText(text, &sortProblem) ?: text;
    if (![self writeText:text toTab:t interactive:YES]) return NO;
    if (![text isEqualToString:t.sourceText ?: @""]) t.pendingSort = text;
    if (sortProblem)
        [self flashStatus:[NSString stringWithFormat:@"%@: %@",
                           t.currentPath.lastPathComponent, sortProblem]];
    return YES;
}

// Close a tab, asking about unsaved changes first when `prompt` is set. NO
// when the user kept it. The tab shown next is the one used most recently.
- (BOOL)closeTab:(MCTab *)t prompt:(BOOL)prompt {
    // A tab only the other group holds is closed there, which takes the
    // keyboard for it (a file trashed, a server's or an agent's close).
    if (![[self groupTabs] containsObject:t] && [self otherGroupHoldsTab:t]) {
        if (!t.dirty || !prompt) {
            [self removeTabFromOtherGroup:t];
            if (![_tabs containsObject:t]) {
                [self discardBackupOf:t];
                [t.latex removeFromSuperview];
                [self closePageOf:t];
                [self.lsp tabsChanged];
                [self saveSession];
            }
            return YES;
        }
        [self focusEditorGroup:1 - [self focusedGroup]];
    }
    NSMutableArray<MCTab *> *group = [self groupTabs];
    const NSUInteger index = [group indexOfObject:t];
    if (index == NSNotFound) return YES;
    // Open in the other group too: only this group's tab goes, unasked, and
    // the document stays as it is there.
    const BOOL elsewhere = [self otherGroupHoldsTab:t];
    if (t == _activeTab) [self saveBeforeLeavingFormatSave];   // saved, so not asked about
    if (!elsewhere && prompt && ![self confirmCloseTab:t]) return NO;
    if (!elsewhere) [self discardBackupOf:t];   // saved, or let go
    std::vector<uint64_t> stamps;
    for (MCTab *each in group) stamps.push_back(each.lastActive);
    const NSInteger active = _activeTab ? (NSInteger)[group indexOfObject:_activeTab] : -1;
    const int next = EditorTabs::activeAfterClose(stamps, (int)active, (int)index);
    const BOOL wasActive = t == _activeTab;
    if (wasActive) {
        [self editorWillShowTab:nil];
        [self stashActiveTab];
        _activeTab = nil;
    }
    if (!elsewhere) {
        [self rememberClosedTab:t index:index];
        [t.latex removeFromSuperview];
        [self closePageOf:t];
    }
    [self groupRemoveTab:t];
    if (wasActive && next >= 0) {
        [self activateTab:group[(NSUInteger)next]];
    } else if (group.count == 0 && [self editorSplit]) {
        [self closeEmptyEditorGroup];   // its last tab: the other group is the only one
        [self.lsp tabsChanged];
    } else {
        if (wasActive) [self showEmptyState];
        else [self.lsp tabsChanged];   // its file is closed on its server
        [self reloadTabStrip];
        [self updateTitle];
        [self saveSession];
    }
    return YES;
}

- (BOOL)closeTabEverywhere:(MCTab *)t prompt:(BOOL)prompt {
    if ([self otherGroupHoldsTab:t] && [[self groupTabs] containsObject:t])
        [self removeTabFromOtherGroup:t];   // the copy here is then the only one
    return [self closeTab:t prompt:prompt];
}

// Every tab goes, unasked (the caller asked), and nothing is remembered of
// it: the folder's tabs stay remembered for when it is opened again.
- (void)dropAllTabs {
    [self joinEditorGroupsQuietly];
    [self stashActiveTab];
    _activeTab = nil;
    for (MCTab *t in _tabs) {
        [t.latex removeFromSuperview];
        [self closePageOf:t];
        [self discardBackupOf:t];
    }
    [_tabs removeAllObjects];
    [self showEmptyState];
}

// Cmd+W: what has the keyboard closes. In a terminal that terminal closes
// (its tab, or its pane of a split; the panel hides with the last one; a
// command still running in it is asked about), the browser panel hides, and
// anywhere else the tab on show closes. With no tab left a new press closes
// the window, so the key is never dead. A held key's repeats do nothing: one
// press closes one thing, where a held Cmd+W once took three terminals (one
// running a command) and then both editor tabs.
- (void)closeFocused:(id)sender {
    NSEvent *e = NSApp.currentEvent;
    if (e.type == NSEventTypeKeyDown && e.isARepeat) return;
    switch ([self focusedCloseTarget]) {
    case 1:
        [self closeFocusedTerminal];
        return;
    case 2:
        [self toggleBrowser:nil];
        [self focusAfterClosingPanel];
        return;
    case 3:
        [self closeTab:_activeTab prompt:YES];
        return;
    case 4:
        [self toggleProblems:nil];
        return;
    default:
        [self.window performClose:sender];
    }
}

// What Cmd+W would close: 1 the terminal, 2 the browser, 3 the active tab,
// 4 the Problems list, 0 the window.
- (int)focusedCloseTarget {
    NSResponder *fr = self.window.firstResponder;
    NSView *v = [fr isKindOfClass:NSView.class] ? (NSView *)fr : nil;
    if (v && self.problemsPanel && self.problemsVisible &&
        [v isDescendantOf:self.problemsPanel]) return 4;
    if (v && self.terminalPanel && self.terminalVisible &&
        [v isDescendantOf:self.terminalPanel]) return 1;
    if (v && self.browser && self.browserVisible && [v isDescendantOf:self.browser]) return 2;
    return _activeTab ? 3 : 0;
}

- (void)focusAfterClosingPanel {
    const BOOL browser = self.browserVisible && self.browser;
    if (self.rightAreaCollapsed) [self.window makeFirstResponder:self.outline];
    else if (!browser && !self.editorHidden) [self focusActiveDocument];
    else if (browser) [self.browser focusURLBar];
    else if (self.terminalVisible && self.terminal) [self.terminal focusInput];
    else [self.window makeFirstResponder:self.outline];
}

// Whether the tab holds text that is its file's: not a message (welcome,
// a binary file), a picture, a PDF, a player, a diff, or a viewer that
// shows the file without its text (SQLite, Quick Look, a large CSV), whose
// empty text view must never be written over the file.
- (BOOL)tabHasTextToWrite:(MCTab *)t {
    return t.currentPath && !t.showingMessage && !t.isImage && !t.isPDF && !t.isMedia &&
           !t.isDiff && !MCViewerKindWithoutText(t.viewerKind);
}

// The one writer: Cmd+S, a background tab's save (Save All, closing), and
// auto-save all write a tab's text here. The write goes through
// FolderSearch::writeFileAtomically, so a symlink stays a link (its file
// is written), a hard-linked file is written in place, and the mode,
// owner, extended attributes and creation date stay; the text is written
// in the tab's encoding (UTF-8, or UTF-16 as read) with its byte order
// mark back in front. On success the tab is clean and in step with
// the file as written (its stamp taken from the write itself), and its
// language servers hear of the save. `interactive` says a failure in an
// alert, else in the status bar (auto-save never asks anything).
- (BOOL)writeText:(NSString *)text toTab:(MCTab *)t interactive:(BOOL)interactive {
    if (!t || !text || ![self tabHasTextToWrite:t]) return NO;
    NSString *name = t.currentPath.lastPathComponent;
    std::string bytes, error;
    const BOOL encoded = MCEncodeFileText(text, t.encoding, t.bom, &bytes, &error);
    FolderSearch::FileStamp written;
    if (!encoded || !FolderSearch::writeFileAtomically(t.currentPath.fileSystemRepresentation,
                                                       bytes, &error, &written)) {
        NSString *why = [NSString stringWithUTF8String:error.c_str()] ?: @"";
        if (interactive) {
            NSAlert *a = [NSAlert new];
            a.messageText = [NSString stringWithFormat:@"“%@” could not be saved.", name];
            a.informativeText = [NSString stringWithFormat:@"%@: %@.", t.currentPath, why];
            [a runModal];
        } else {
            [self showStatusNote:[NSString stringWithFormat:@"Could not auto-save %@: %@",
                                  name, why]];
        }
        return NO;
    }
    t.savedText = text;
    t.gone = NO;
    t.dirty = NO;
    if (t.preview) [self keepPreviewTab:t];   // saved: kept, as in VS Code
    // So our own save never looks like a change made outside.
    [self recordStamp:MCDiskStampFromFileStamp(written) ofTab:t];
    if (t == _activeTab) {
        [self.lsp documentSaved];
    } else {
        // A tab saved in the background (Save All, auto-save, closing):
        // didSave to every server that has the file open.
        [self.lsp documentSavedAtPath:t.currentPath text:text];
    }
    [self updateTitle];
    return YES;
}

- (void)saveCurrentFile:(id)sender {
    // A message (welcome, binary file) is not the file's contents, nor is a
    // viewer's empty text view.
    if (![self tabHasTextToWrite:_activeTab]) return;
    // editor.organize-imports-on-save, then format.on-save: Cmd+S (a sender)
    // has the imports organized and the file formatted first, and saves when
    // they are done; saves asked for by MiniCode itself (nil, auto-save
    // among them) never wait.
    if (sender && [self organizeOrFormatBeforeSaving:YES]) return;
    // Never over a change made outside, or a file deleted outside, unasked.
    if (![self mayWriteTab:_activeTab]) return;
    // In markdown preview mode the text view holds rendered text, not source;
    // save the tracked source instead.
    // editor.keep-sorted-on-save: the blocks are sorted first, as one undo step.
    NSString *sortProblem = nil;
    if (self.textView.editable && [AppSettings shared].settings.keepSortedOnSave())
        sortProblem = MCKeepSortedBeforeSave(self.textView);
    NSString *text = self.textView.editable ? [self.textView.string copy]
                                            : (self.sourceText ?: @"");
    if ([self writeText:text toTab:_activeTab interactive:YES]) {
        self.sourceText = text;
        [self flashStatus:sortProblem];
    }
}

@end
