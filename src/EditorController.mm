// EditorController.mm — Objective-C++. Bridges the pure-C++ core (syntax
// highlighter, markdown parser) to AppKit views.
#import "EditorController+Private.h"
// Every category's own instance variables, laid out here.
#import "EditorController+AgentsIvars.h"
#import "EditorController+EditorViewIvars.h"
#import "EditorController+FormatIvars.h"
#import "EditorController+HighlightingIvars.h"
#import "EditorController+LanguageServersIvars.h"
#import "EditorController+LayoutIvars.h"
#import "EditorController+MarkdownIvars.h"
#import "EditorController+PersistenceIvars.h"
#import "EditorController+TabsIvars.h"
#import "EditorController+TreeIvars.h"
#import "EditorController+TreeDetailsIvars.h"

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

@end
