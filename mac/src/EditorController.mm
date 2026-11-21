// EditorController.mm — Objective-C++. Bridges the pure-C++ core (syntax
// highlighter, markdown parser) to AppKit views.
#import "EditorController+Private.h"
// Every category's own instance variables, laid out here.
#import "EditorController+AgentsIvars.h"
#import "EditorController+EditCommandsIvars.h"
#import "EditorController+EditorViewIvars.h"
#import "EditorController+FormatIvars.h"
#import "EditorController+GitIvars.h"
#import "EditorController+GroupsIvars.h"
#import "EditorController+HighlightingIvars.h"
#import "EditorController+LanguageServersIvars.h"
#import "EditorController+LargeFileIvars.h"
#import "EditorController+LayoutIvars.h"
#import "EditorController+MarkdownIvars.h"
#import "EditorController+PageSideIvars.h"
#import "EditorController+PersistenceIvars.h"
#import "EditorController+TabsIvars.h"
#import "EditorController+TestsIvars.h"
#import "EditorController+LineHistoryIvars.h"
#import "EditorController+TreeIvars.h"
#import "EditorController+TreeDetailsIvars.h"
#import "EditorController+TreeFocusIvars.h"
#import "EditorController+TreeRevealIvars.h"
#import "IdeLinkServer.h"

// Every window's tabs (Model::Window), one of each per editor window.
static Model::Session &MCTabWindows() {
    static Model::Session session;
    return session;
}

@implementation EditorController

- (instancetype)initWithRootPath:(NSString *)path {
    return [self initWithRootPath:path listTree:YES];
}

// `list` NO: the tree lists its folder later, from where it comes from (a
// window over SSH lists through its host once it has its link:
// useRemoteSessionLink:, never this Mac's disk at that path).
- (instancetype)initWithRootPath:(NSString *)path listTree:(BOOL)list {
    if ((self = [super init])) {
        _root = [FileItem new];
        _root.path = path; _root.isDir = YES;
        if (list) [_root loadChildren];
        _terminalHeight = 220;
        _browserFraction = 0.5;
        _winId = MCTabWindows().addWindow();
        _win = MCTabWindows().window(_winId);
        _docs = [NSMutableDictionary dictionary];
        _noTabUndo = [NSUndoManager new];
        [self setUpTreeDetails];
        _agents = [[MCAgentWindow alloc] initWithHost:self];
        MCIdeLinkWindowOpened(self);   // the Claude Code IDE link, while agent.ide-link is on
    }
    return self;
}

- (NSString *)rootPath { return _root.path; }

- (void)dealloc {
    [self stopMedia];
    [self stopWatching];
    [self.window removeObserver:self forKeyPath:@"title"];
    [self windowTitleStopObservingFocus];
    MCTabWindows().removeWindow(_winId);
    _win = nullptr;
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
    return [self confirmCloseAllTabsClosing:YES];
}
- (void)windowWillClose:(NSNotification *)note {
    [self noteWindowClosing];   // before the terminals go: the windows to bring back
    // The session's host keeps the window's documents and terminals for the
    // folder's next window (EditorController+Session.mm).
    [self detachFromSessionHost];
    [_agent close];
    // Asked already (windowShouldClose:): what is still unsaved was let go,
    // unless quitting kept it for the next launch (hot exit).
    [self discardBackupsAsWindowCloses];
    for (MCTab *t in [self tabs]) [t.latex stopPreview];   // no typeset or output folder outlives it
    _closing = YES;
    [self stopSavePlan];   // nothing more written for it on a timer
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
    [self stopLineHistory];   // no git blame or box for a closed window
    [self.gitPanel stop];  // no autofetch for a closed window
    [self stopWatching];   // no file events for a closed window (the stream holds an unretained self)
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

// With the LaTeX page beside the source holding the keyboard, Cmd+Z is the
// source's (an edit made on the page is one step there).
- (void)undo:(id)sender {
    if ([self latexIsShowing]) [self.latex undoEdit];
    else if ([self pageSideShowsLatex] && _activeTab.undoManager.canUndo)
        [_activeTab.undoManager undo];
    else NSBeep();
}
- (void)redo:(id)sender {
    if ([self latexIsShowing]) [self.latex redoEdit];
    else if ([self pageSideShowsLatex] && _activeTab.undoManager.canRedo)
        [_activeTab.undoManager redo];
    else NSBeep();
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    BOOL enabled = NO;
    if (item.action == @selector(navigateBack:)) return [self.navigator canGoBack];
    if (item.action == @selector(navigateForward:)) return [self.navigator canGoForward];
    if ([self validateActiveTabAction:item.action enabled:&enabled]) return enabled;
    if ([self validateZoomFitItem:item enabled:&enabled]) return enabled;
    if ([self validateGitItem:item enabled:&enabled]) return enabled;
    if ([self validateTestItem:item enabled:&enabled]) return enabled;
    if ([self validateLineHistoryItem:item enabled:&enabled]) return enabled;
    if ([self validateGroupItem:item enabled:&enabled]) return enabled;
    if ([self validateTreeFocusItem:item enabled:&enabled]) return enabled;
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
    if (item.action == @selector(toggleHiddenFiles:)) {
        item.state = gShowHidden ? NSControlStateValueOn : NSControlStateValueOff;
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
        return EditorTabs::indexForNumber((int)item.tag, (int)[self groupTabs].count) >= 0;
    if (item.action == @selector(selectNextTab:) || item.action == @selector(selectPreviousTab:))
        return [self groupTabs].count > 0;
    if (item.action == @selector(goToBracket:)) return [self bracketMatchingApplies];
    if (item.action == @selector(renameTerminal:) || item.action == @selector(moveTerminalLeft:) ||
        item.action == @selector(moveTerminalRight:))
        return [self validateTerminalTabCommand:item];
    if ([self isEditCommand:item.action]) return [self validateEditCommand:item];
    // Off without a tour, so Option+] and Option+[ type their characters.
    if (item.action == @selector(nextTourStop:) || item.action == @selector(previousTourStop:))
        return self.tourActive;
    if (item.action == @selector(exportPDF:)) return self.canExportPDF;
    if (item.action == @selector(exportMarkdownHtml:)) return self.canExportMarkdownHtml;
    if (item.action == @selector(saveBrowserBookmark:)) return self.canSaveBrowserBookmark;
    if (item.action == @selector(saveBrowserTabsAsBookmarks:)) return self.canSaveBrowserTabs;
    if (item.action == @selector(openBookmarksInTabs:)) return self.canOpenBookmarksInTabs;
    if (item.action == @selector(openBookmarkAsText:)) return self.canOpenBookmarkAsText;
    if (item.action == @selector(showLineInPDF:)) return self.canShowLineInPDF;
    if (item.action == @selector(toggleSidePreview:)) {
        item.state = [self sidePreviewShowing] || [self pageSideShowing] ? NSControlStateValueOn
                                                                          : NSControlStateValueOff;
        return self.canToggleSidePreview;
    }
    if (item.action == @selector(togglePlayback:)) return self.canTogglePlayback;
    if (item.action == @selector(forgetFolderToolsAnswer:)) return self.canForgetFolderToolsAnswer;
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
    if (item.action == @selector(toggleFindPreserveCase:)) {
        item.state = _editorFind.preserveCase ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFindInSelection:)) {
        item.state = _editorFind.findInSelection ? NSControlStateValueOn : NSControlStateValueOff;
        return !self.editorScroll.hidden &&
               (_editorFind.findInSelection || self.textView.selectedRange.length > 0);
    }
    if (item.action == @selector(performTextFinderAction:))
        return !self.editorScroll.hidden && [_editorFind validateFindAction:item.tag];
    if (item.action == @selector(undo:))
        return [self latexIsShowing] ? [self.latex canUndoEdit]
                                     : [self pageSideShowsLatex] && _activeTab.undoManager.canUndo;
    if (item.action == @selector(redo:))
        return [self latexIsShowing] ? [self.latex canRedoEdit]
                                     : [self pageSideShowsLatex] && _activeTab.undoManager.canRedo;
    return YES;
}

@end
