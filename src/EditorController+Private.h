// EditorController+Private.h — what the pieces of EditorController share:
// the class extension (its instance variables and private properties),
// the declarations of the methods each file implements, and the small
// helpers and constants more than one of them uses. Only the
// EditorController files import it.
#pragma once
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#import "RunCommand.h"
#import "Browser.h"
#import "Latex.h"
#import "Search.h"
#import "AppSettings.h"
#import "Lsp.h"
#import "MultiCaretView.h"
#import "LspPanel.h"
#import "MarkdownImage.h"
#import "MarkdownMath.h"
#import "MarkdownEditPanel.h"
#import "GitPanel.h"
#import "AgentWindow.h"
#import "AgentsPanel.h"
#import "AgentModel.h"
#import "EditorPage.h"
#import "Appearance.h"
#import "TabStrip.h"
#import "FormatRunner.h"
#import "EditingCommands.h"
#import "TreeDetails.h"
#import "InstallPanel.h"
#import "ChangePanel.h"
#import "BackupStore.h"
#import "AgentPanel.h"
#import "EditorFind.h"
#import "ProblemsPanel.h"
#import "PalettePanel.h"
#import "Navigation.h"
#import "Gutter.h"
#import "FoldView.h"
#import "EditorFont.h"
#import "SourceStorage.h"
#import "AgentNotes.h"
#import "CompareView.h"
#import "FileViewers.h"
#import "QuickLook.h"
#import "MarkdownCode.h"
#import "Keymap.h"
#import <CoreServices/CoreServices.h>   // FSEvents, for live file-tree updates
#import <AVKit/AVKit.h>                 // video and audio in the editor's slot
#import <AVFoundation/AVFoundation.h>
#include <sys/stat.h>
#include <map>
#include "SyntaxHighlighter.h"
#include "MarkdownParser.h"
#include "MarkdownEdit.h"
#include "LineComments.h"
#include "MarkdownTasks.h"
#include "EditorTabs.h"
#include "LineDiff.h"
#include "Formatter.h"
#include "LspClient.h"   // Lsp::serverDisplayName
#include "BracketMatch.h"
#include "ControlProtocol.h"   // the selection's size limit
#include <memory>
#include <string>
#import "EditorPanes.h"
#import "FileTree.h"
#import "EditorTab.h"

// ---------------------------------------------------------------------------
// Dark theme colors (VS Code "Dark+" inspired).
// ---------------------------------------------------------------------------
static inline NSColor *Hex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8)  & 0xFF) / 255.0
                                blue:( rgb        & 0xFF) / 255.0
                               alpha:1.0];
}

static const CGFloat kStatusBarHeight = 24;

static inline std::string Utf8String(NSString *s) {
    const char *c = s.UTF8String;
    return c ? c : "";
}

static inline std::u16string U16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}
static inline NSString *FromU16(const std::u16string &u) {
    return [NSString stringWithCharacters:(const unichar *)u.data() length:u.size()];
}

// Link value marking a clickable color in the settings file.
static NSString *const kColorLink = @"minicode-color";
static NSString *const kOpacityLink = @"minicode-opacity";
// On rendered Markdown: the 0-based source line a stretch of text came from.
static NSAttributedStringKey const kMarkdownSourceLine = @"MCMarkdownSourceLine";
// On a rendered table cell: @[row, column], row 0 being the header.
static NSAttributedStringKey const kMarkdownTableCell = @"MCMarkdownTableCell";
// On the alt text standing in for a web picture still on its way:
// @[address, n], n numbering them so two in a row stay two runs.
static NSAttributedStringKey const kMarkdownWebImage = @"MCMarkdownWebImage";
// On the TeX standing in for a formula still being typeset: @[key, n], as
// for web pictures.
static NSAttributedStringKey const kMarkdownMath = @"MCMarkdownMath";
// On the line at the top of a page whose math cannot be typeset because
// tectonic is missing.
static NSAttributedStringKey const kMarkdownMathNote = @"MCMarkdownMathNote";
// The link in that line that downloads tectonic.
static NSString *const kMathDownloadLink = @"minicode-math:download-tectonic";
// Link value on a task's box in rendered Markdown, followed by its 0-based
// source line: a click ticks or clears it.
static NSString *const kTaskLinkPrefix = @"minicode-task:";

// Panel, text and syntax colors come from the settings file (AppSettings);
// Hex is for the fixed accents that aren't configurable.
static inline NSColor *ColorForStyle(TokenStyle s) {
    return [[AppSettings shared] syntax:s];
}

// The editor's text as the incremental highlighter reads it: UTF-16 straight
// out of the text storage, a line at a time, never copied whole.
class NSStringSource : public TextSource<char16_t> {
public:
    explicit NSStringSource(NSString *s) : s_(s) {}
    size_t length() const override { return s_.length; }
    void read(size_t pos, size_t len, char16_t *out) const override {
        [s_ getCharacters:(unichar *)out range:NSMakeRange(pos, len)];
    }
private:
    NSString *s_;
};

typedef NS_ENUM(NSInteger, MCDiskDecision) { MCDiskWrite, MCDiskReload, MCDiskCancel };

// The tree menu's items for a worktree (shown only on one).
static const NSInteger kWorktreeMenuTag = 7701;

// ---------------------------------------------------------------------------
@interface EditorController () <NSTextStorageDelegate> {
    FileItem *_root;
    // The open files, in the order the strip shows them, and the one shown.
    NSMutableArray<MCTab *> *_tabs;
    MCTab *_activeTab;
    // The viewers of FileViewers.h, made on first use (-viewers).
    MCFileViewers *_viewers;
    uint64_t _tabStamp;                    // counts activations, for MCTab.lastActive
    NSUndoManager *_noTabUndo;             // the text view's while no tab is open
    FSEventStreamRef _fsStream;            // watches the open folder for changes
    // Syntax highlighting of the editable source. While _liveHighlight is on,
    // every change to the text storage is re-highlighted as it happens, from
    // the storage delegate; _hl (null for a language without a grammar)
    // tracks line states so only the changed lines are re-lexed.
    std::unique_ptr<IncrementalHighlighter<char16_t>> _hl;
    BOOL _liveHighlight;
    BOOL _bracketMatchQueued;   // updateBracketMatch waits for the next turn
    // Rendered Markdown's headings, by GitHub's anchor spelling, for #links.
    NSDictionary<NSString *, NSNumber *> *_markdownAnchors;
    // Where the source view was when a preview took over, so coming back
    // lands there again: the selection, and how far below the top of the
    // pane the caret sat. _previewEntryScroll tells whether the preview was
    // scrolled since, in which case coming back follows it instead.
    NSRange _sourceSelection;
    CGFloat _sourceCaretOffset;
    BOOL _haveSourcePosition;
    NSPoint _previewEntryScroll;
    // Web pictures the rendered page is waiting for, by address; renderMarkdown
    // starts a new set, and nil (after the window closes) waits for none.
    NSMutableSet<NSString *> *_awaitedWebImages;
    NSUInteger _webImageMarks;             // numbers kMarkdownWebImage runs
    BOOL _webImageRenderQueued;
    // The same for formulas being typeset, by MCMathKey; renderMarkdown
    // collects the ones to ask for in _mathWanted.
    NSMutableSet<NSString *> *_awaitedMath;
    // The tree's rows being dragged, while a drag from the tree is on.
    NSArray<NSString *> *_draggedPaths;
    NSMutableArray<NSArray *> *_mathWanted;
    NSUInteger _mathMarks;
    BOOL _mathRenderQueued;
    BOOL _mathOn;                          // this render typesets math
    NSString *_mathRunProblem;             // why a whole run failed, this render
    NSString *_tectonicProblem;            // why the last download failed
    NSUInteger _pendingStart, _pendingEnd; // edited, not yet recolored (NSNotFound: none)
    BOOL _flushScheduled;
    BOOL _highlightingSettingsFile;        // give color values swatches
    NSColor *_styleColors[8];              // by TokenStyle, from the settings
    NSColor *_plainColor;
    NSArray<NSDictionary *> *_styleAttributes;   // attributesForStyle:, for _styleAttributesBase
    NSDictionary *_styleAttributesBase;
    NSDictionary *_sourceAttributes;       // font, paragraph style, plain color
    NSUInteger _formatGeneration;          // a format result for an older one is dropped
    NSPopover *_formatNote;                // what Format Document has to say
    // The tab whose Cmd+S (format.on-save) waits for the formatter. A save
    // is never lost: leaving the tab saves it at once, and Format Document
    // started meanwhile saves when it is done.
    __weak MCTab *_formatSaveTab;
    BOOL _closing;                         // windowWillClose: has run
    NSSet<MCTab *> *_approvedWrites;       // Save All: disk questions already answered
    NSMutableArray<NSDictionary *> *_closedTabs;   // Reopen Closed Tab's stack, newest last
    // The tree's sizes, dates and order (see TreeDetails.h). Each folder's
    // order on show is worked out again when _sortGeneration moves on.
    MCFolderSizer *_sizer;
    TreeSort::Options _treeSort;
    NSUInteger _sortGeneration;
    NSString *_realRoot;                   // the root as FSEvents spells it
    NSTimer *_detailsTimer;                // ages and "Saved ..." each half minute
    MCTreeColumns _shownColumns;           // the tree's columns, as last laid out
    // The bar over the editor when the open file's language server is
    // missing: the language it was offered for (nil once closed), shown
    // while a file of that language is on show and the server still missing.
    NSString *_installBarKey;
    NSPopover *_installPopover;
    NSString *_missingBefore;              // the open file's missing server, last time
    MCChangeSession *_change;              // Request a Change (Shift+Cmd+K), made on first use
    MCAgentSession *_agent;                // the inline agent (Cmd+I), made on first use
    MCSourceStorage *_sourceStorage;       // the editor's text storage (the view does not own it)
    MCEditorFind *_editorFind;             // the editor's find and replace bar (EditorFind.h)
    NSDate *_backupFirstPending;           // the oldest buffer change not yet backed up
    MCAgentNotes *_notes;                  // notes and tours over the editor, made on first use
    MCAgentWindow *_agents;                // the Agents panel, worktrees, evidence (AgentWindow.h)
    FileItem *_worktreesItem;              // the tree's Worktrees node, while there is one
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) ClickOutline *outline;
@property(nonatomic, strong) NSButton *restartButton;      // the status bar's ↻
@property(nonatomic, strong) CodeTextView *textView;
@property(nonatomic, strong) NSTextField *statusLabel;
@property(nonatomic, copy)   NSString *currentPath;
@property(nonatomic, copy)   NSString *sourceText;   // raw file text (edited)
@property(nonatomic, copy)   NSString *savedText;    // the text on disk (see MCTab)
@property(nonatomic, copy)   NSString *currentExt;
@property(nonatomic, assign) BOOL isMarkdown;
@property(nonatomic, assign) BOOL isLatex;
@property(nonatomic, assign) BOOL previewMode;       // markdown: rendered vs source
@property(nonatomic, assign) BOOL dirty;             // unsaved changes
@property(nonatomic, strong) NSDate *fileModDate;    // on-disk mtime we last saw
@property(nonatomic, strong) NSView *statusBar;
@property(nonatomic, strong) NSView *hintsPanel;
@property(nonatomic, strong) NSTextField *hintsLabel;
@property(nonatomic, assign) BOOL hintsVisible;
@property(nonatomic, strong) PanelHost *rightArea;
@property(nonatomic, strong) PanelScrollView *editorScroll;
@property(nonatomic, strong) MCLineGutter *gutter;   // the editor's line numbers
@property(nonatomic, strong) MCTerminalPanel *terminalPanel;   // tabs of terminals
@property(nonatomic, readonly) TerminalView *terminal;         // its active one
@property(nonatomic, strong) DragBar *termDivider;
@property(nonatomic, strong) DragBar *browserDivider;   // editor | browser
@property(nonatomic, strong) BrowserView *browser;
@property(nonatomic, strong) LatexView *latex;
@property(nonatomic, strong) MCMarkdownEditPanel *markdownEditPanel;
@property(nonatomic, strong) NSImageView *imageView;   // editor's slot, for images
@property(nonatomic, assign) BOOL isImage;
@property(nonatomic, assign) NSSize imagePixels;       // for the title bar
@property(nonatomic, strong) PDFView *pdfView;         // editor's slot, for PDFs
@property(nonatomic, assign) BOOL isPDF;
@property(nonatomic, strong) AVPlayerView *playerView; // editor's slot, video and audio
@property(nonatomic, assign) BOOL isMedia;
@property(nonatomic, assign) MCViewerKind viewerKind;   // FileViewers.h
@property(nonatomic, strong) AVPlayerItem *mediaItem;  // observed for size and duration
@property(nonatomic, assign) NSSize mediaPixels;       // zero for audio
@property(nonatomic, assign) double mediaSeconds;      // < 0 until known
@property(nonatomic, assign) BOOL terminalVisible;
@property(nonatomic, assign) BOOL browserVisible;
@property(nonatomic, assign) CGFloat terminalHeight;
@property(nonatomic, assign) CGFloat browserFraction;   // of the top's width
@property(nonatomic, strong) NSSplitView *splitView;
@property(nonatomic, strong) PanelScrollView *sidebarScroll;
@property(nonatomic, assign) BOOL sidebarCollapsed;
@property(nonatomic, assign) CGFloat sidebarWidthBeforeCollapse;
@property(nonatomic, assign) BOOL editorHidden;   // Shift+Cmd+E: file view hidden
@property(nonatomic, strong) SearchPanel *searchPanel;
@property(nonatomic, strong) SearchPanel *todoPanel;   // Shift+Cmd+D
@property(nonatomic, strong) NSVisualEffectView *blurView;   // window.blur
@property(nonatomic, strong) NSView *titlebarView;   // drawn when customTitlebar
@property(nonatomic, strong) NSTextField *titleLabel;
@property(nonatomic, strong) NSTextField *settingsLabel;     // settings errors
@property(nonatomic, assign) BOOL showingMessage;   // welcome/binary text shown
@property(nonatomic, copy) NSString *lastFormatNote;  // tests: the last format note
@property(nonatomic, assign) NSUInteger colorLineStart;   // line the color panel edits
@property(nonatomic, copy)   NSString *colorEditPath;
@property(nonatomic, assign) NSUInteger opacityLineStart;   // line the opacity slider edits
@property(nonatomic, assign) NSUInteger keyFlashGeneration;  // Adjust Settings' tint
@property(nonatomic, strong) LspSession *lsp;         // language servers
@property(nonatomic, strong) MCNavigator *navigator;  // Go Back and Go Forward
@property(nonatomic, strong) MCLspStatusField *lspLabel;   // their status; click for more
// Every problem the servers report, across the bottom above the terminal
// (Shift+Cmd+M), and their counts in the status bar (click to toggle it).
@property(nonatomic, strong) MCProblemsPanel *problemsPanel;
@property(nonatomic, assign) BOOL problemsVisible;
@property(nonatomic, strong) MCLspStatusField *problemsLabel;
@property(nonatomic, strong) MCInstallBar *installBar;     // "No Python language server..."
@property(nonatomic, strong) MCGitPanel *gitPanel;    // Source Control, in the sidebar's place
@property(nonatomic, assign) BOOL gitPanelVisible;
@property(nonatomic, assign) BOOL agentsVisible;      // the Agents panel in the sidebar's place
@property(nonatomic, strong) PanelScrollView *diffScroll;   // editor's slot, for a diff
@property(nonatomic, strong) NSTextView *diffView;
@property(nonatomic, strong) MCCompareView *compareView;   // editor's slot, side by side
@property(nonatomic, assign) BOOL isDiff;
@property(nonatomic, copy)   NSString *diffName;     // for the title
@property(nonatomic, strong) NSData *diffData;       // re-colored on a settings change
@property(nonatomic, assign) BOOL diffIsCommit;      // a whole commit (git show), not one file
@property(nonatomic, strong) MCTabStrip *tabStrip;   // the open files, over the editor
@property(nonatomic, strong) NSTextField *emptyLabel; // the editor's slot with no file open
@property(nonatomic, strong) NSTextField *savedLabel; // "Saved 14:32", with details on

// Still in EditorController.mm.
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context;
- (CGFloat)titlebarHeight;
- (void)layoutContainer;
- (void)buildTitlebarInContainer:(NSView *)container;
- (void)applySettings;
- (void)showSettingsErrors;
- (void)keymapChanged:(NSNotification *)note;
- (void)recolorEditor;
- (NSMenu *)buildTreeContextMenu;
- (void)buildStatusBarInContainer:(NSView *)container height:(CGFloat)barH;
- (void)layoutStatusRight;
- (void)buildHintsPanelInContainer:(NSView *)container;
- (void)updateHints;
- (void)appendHintsHeading:(NSString *)title to:(NSMutableAttributedString *)s;
- (void)appendHintsRow:(NSMutableAttributedString *)s
                   key:(NSString *)key1 label:(NSString *)label1
                  key2:(NSString *)key2 label2:(NSString *)label2
                 state:(NSString *)state;
- (NSString *)hk:(NSString *)commandId;
- (NSString *)hkAll:(NSString *)commandId;
- (NSString *)hk:(NSString *)commandId at:(NSUInteger)index;
- (NSString *)hkAlso:(NSString *)commandId title:(NSString *)title;
- (NSAttributedString *)hintsText;
- (void)relayoutRightArea;
- (void)terminalBarDraggedTo:(CGFloat)y;
- (void)browserBarDraggedTo:(CGFloat)x;
- (void)makeTerminalPanel;
- (void)makeTerminalPanelFirstIn:(NSString *)first;
- (void)terminalPanelEmptied;
- (void)showTerminalPanel;
- (void)showStatusNote:(NSString *)note;
- (void)openTerminalLink:(MCTermLink *)link;
- (void)openTerminalLinkNow:(MCTermLink *)link;
- (void)makeBrowserWithHome:(NSString *)home;
- (BOOL)rightAreaCollapsed;
- (BOOL)splitView:(NSSplitView *)sv shouldAdjustSizeOfSubview:(NSView *)view;
- (NSRect)splitView:(NSSplitView *)sv effectiveRect:(NSRect)proposed
          forDrawnRect:(NSRect)drawn ofDividerAtIndex:(NSInteger)i;
- (BOOL)splitView:(NSSplitView *)sv canCollapseSubview:(NSView *)view;
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMinCoordinate:(CGFloat)min
               ofSubviewAt:(NSInteger)i;
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMaxCoordinate:(CGFloat)max
               ofSubviewAt:(NSInteger)i;
- (void)revealEditor;
- (void)showRightArea;
- (void)collapseRightAreaIfEmpty;
- (NSView *)sidebarViewOnShow;
- (void)showDiffNamed:(NSString *)name path:(NSString *)path data:(NSData *)diff;
- (void)showCommitTitled:(NSString *)title data:(NSData *)show;
- (void)showDiffTitled:(NSString *)name data:(NSData *)diff commit:(BOOL)commit;
- (void)renderDiff;
- (void)performTextFinderAction:(id)sender;
- (void)toggleFindMatchCase:(id)sender;
- (void)toggleFindWholeWord:(id)sender;
- (void)toggleFindRegex:(id)sender;
- (void)makeSearchPanel;
- (int)tabHoldOfPath:(NSString *)path disk:(NSString *__strong *)disk;
- (MCOpenFileState)replaceStateOfPath:(NSString *)path;
- (void)jumpToLine:(NSInteger)line;
- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item;
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)i ofItem:(id)item;
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item;
- (NSView *)outlineView:(NSOutlineView *)ov
     viewForTableColumn:(NSTableColumn *)col
                   item:(id)item;
- (id<NSPasteboardWriting>)outlineView:(NSOutlineView *)ov
               pasteboardWriterForItem:(id)item;
- (void)outlineView:(NSOutlineView *)ov draggingSession:(NSDraggingSession *)session
   willBeginAtPoint:(NSPoint)pt forItems:(NSArray *)items;
- (void)outlineView:(NSOutlineView *)ov draggingSession:(NSDraggingSession *)session
       endedAtPoint:(NSPoint)pt operation:(NSDragOperation)op;
- (NSArray<NSString *> *)droppedPaths:(id<NSDraggingInfo>)info local:(BOOL)local;
- (NSDragOperation)outlineView:(NSOutlineView *)ov validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item proposedChildIndex:(NSInteger)index;
- (BOOL)outlineView:(NSOutlineView *)ov acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item childIndex:(NSInteger)index;
- (void)pathMovedFrom:(NSString *)src to:(NSString *)dst;
- (void)handleRowClick:(NSInteger)row atPoint:(NSPoint)pt;
- (void)activateRow:(NSInteger)row;
- (void)startWatching:(NSString *)path;
- (void)stopWatching;
- (void)reloadTreeKeepingState:(BOOL)rescan;
- (void)reExpand:(FileItem *)node usingSet:(NSSet<NSString *> *)expanded;
- (void)setUpTreeDetails;
- (void)rootChanged;
- (NSArray<FileItem *> *)childrenOf:(FileItem *)node;
- (NSString *)worktreesFolder;
- (NSArray<FileItem *> *)withWorktreesNode:(NSArray<FileItem *> *)shown of:(FileItem *)node;
- (BOOL)isWorktreeRow:(FileItem *)node;
- (NSAttributedString *)worktreeTitleFor:(FileItem *)node color:(NSColor *)text
                                    icon:(NSString **)icon tint:(NSColor **)tint
                                     tip:(NSString **)tip;
- (FileItem *)worktreeMenuItem;
- (void)compareWorktreeWithMain:(id)sender;
- (void)openWorktreeInNewWindow:(id)sender;
- (void)menuNeedsUpdate:(NSMenu *)menu;
- (void)outlineViewItemDidExpand:(NSNotification *)note;
- (NSArray<FileItem *> *)orderOf:(NSArray<FileItem *> *)items;
- (void)configureTreeCell:(MCTreeCell *)cell item:(FileItem *)node;
- (void)refreshTreeCells;
- (void)folderTotalsArrived:(NSSet<NSString *> *)paths;
- (void)reorderNode:(FileItem *)node changed:(BOOL *)changed;
- (void)folderChangedOnDisk:(NSString *)path subtree:(BOOL)subtree;
- (void)setTreeSort:(TreeSort::Options)options;
- (TreeSort::Options)treeSortOptions;
- (void)treeDetailsChanged:(NSNotification *)note;
- (void)updateDetailsTimer;
- (void)updateSavedStatus;
- (void)layoutStatusLabels;
- (NSString *)targetDirectory;
- (FileItem *)clickedOrSelectedItem;
- (NSString *)promptForName:(NSString *)title default:(NSString *)initial;
- (void)renamePath:(NSString *)path;
- (void)trashPath:(NSString *)path;
- (void)revealPathInFinder:(NSString *)path;
- (void)copyText:(NSString *)text;
- (NSString *)pathFromRoot:(NSString *)path;
- (NSMenu *)menuForTabAtIndex:(NSInteger)i;
- (MCTab *)tabOfMenuItem:(id)sender;
- (void)closeTabFromMenu:(id)sender;
- (void)closeTabsInList:(NSArray<MCTab *> *)tabs;
- (void)closeOtherTabsFromMenu:(id)sender;
- (void)closeTabsToTheRightFromMenu:(id)sender;
- (void)closeAllTabsFromMenu:(id)sender;
- (void)copyTabPath:(id)sender;
- (void)copyTabRelativePath:(id)sender;
- (void)revealTabInFinder:(id)sender;
- (void)revealTabInTree:(id)sender;
- (NSMenuItem *)menuItemForActiveTab;
- (BOOL)validateActiveTabAction:(SEL)a enabled:(BOOL *)enabled;
- (void)renameTabFile:(id)sender;
- (void)trashTabFile:(id)sender;
- (void)warn:(NSString *)msg;
- (void)resetViewMode;
+ (BOOL)isImagePath:(NSString *)path;
- (BOOL)showImageAtPath:(NSString *)path;
- (BOOL)showPDFAtPath:(NSString *)path;
+ (BOOL)isMediaPath:(NSString *)path;
- (void)observeMediaItem:(AVPlayerItem *)item;
- (void)stopMedia;
- (void)mediaItemChanged:(AVPlayerItem *)item;
- (BOOL)showMediaAtPath:(NSString *)path;
- (MCFileViewers *)viewers;
- (void)viewerEditedText:(NSString *)text actionName:(NSString *)name;
- (void)setViewerText:(NSString *)text actionName:(NSString *)name reshow:(BOOL)reshow;
- (BOOL)showViewer:(MCViewerKind)kind;
- (void)openFileAtPath:(NSString *)path;
- (void)loadActiveTab;
- (NSDate *)diskModDateFor:(NSString *)path;
- (void)recordDiskStateOf:(MCTab *)t;
- (void)recordModDate;
- (void)forgetHistoryOf:(MCTab *)t;
- (void)reselectCurrentFileInTree;
- (NSUndoManager *)undoManagerForTextView:(NSTextView *)view;
- (void)buildTabStrip;
- (void)applyTabSettings;
- (NSRect)layoutTabStripIn:(NSRect)slot editorShown:(BOOL)shown;
- (void)reloadTabStrip;
- (MCTab *)tabForPath:(NSString *)path;
- (void)insertTab:(MCTab *)tab;
- (void)activateTab:(MCTab *)tab;
- (void)stashActiveTab;
- (void)showActiveTab;
- (void)restoreTextPlaceOf:(MCTab *)t;
- (void)rememberClosedTab:(MCTab *)t index:(NSUInteger)index;
- (BOOL)canReopenClosedTab;
- (void)showEmptyState;
- (void)focusActiveDocument;
- (void)selectTabAtIndex:(NSInteger)i;
- (void)closeTabAtIndex:(NSInteger)i;
- (void)stepTab:(int)delta;
- (BOOL)confirmCloseTab:(MCTab *)t;
- (BOOL)saveTab:(MCTab *)t;
- (BOOL)closeTab:(MCTab *)t prompt:(BOOL)prompt;
- (void)dropAllTabs;
- (int)focusedCloseTarget;
- (void)focusAfterClosingPanel;
- (NSUserDefaults *)sessionDefaults;
- (BOOL)sessionsEnabled;
- (NSString *)sessionKey;
- (NSArray<NSString *> *)sessionFilePaths;
- (NSDictionary *)browserSessionState;
- (void)restoreBrowserFromSession;
- (void)restoreSession;
- (BOOL)tabTakesBackup:(MCTab *)t;
- (BOOL)tabNeedsBackup:(MCTab *)t;
- (void)scheduleBackups;
- (void)writeBackups;
- (void)discardBackupOf:(MCTab *)t;
- (void)backupsAppWillTerminate:(NSNotification *)note;
- (BOOL)takeBackup:(NSString *)bid intoTab:(MCTab *)t;
- (void)showRestoredText:(NSString *)text base:(NSDate *)base inode:(uint64_t)inode;
- (Settings::AutoSave)autoSaveMode;
- (BOOL)tabTakesAutoSave:(MCTab *)t;
- (void)scheduleAutoSave;
- (void)autoSaveAfterDelay;
- (void)autoSaveOnFocusChange;
- (void)autoSaveNow;
- (BOOL)writeTabForAutoSave:(MCTab *)t;
- (void)reloadCleanTab:(MCTab *)t;
- (void)checkExternalChange;
- (void)takeDiskText:(NSString *)fresh forTab:(MCTab *)t;
- (void)tabWentMissing:(MCTab *)t;
- (void)tabCameBack:(MCTab *)t;
- (NSString *)renamedFileFor:(MCTab *)t;
- (void)checkTabsOnDisk;
- (BOOL)mayWriteTab:(MCTab *)t;
- (void)reloadTabFromDisk:(MCTab *)t;
- (MCDiskDecision)diskDecisionForTab:(MCTab *)t;
- (BOOL)showsRenderedMarkdown;
- (NSString *)markdownToRender;
- (void)refreshDisplay;
- (void)showLatexPreview;
- (BOOL)latexIsShowing;
- (void)latexDidEditSource:(NSString *)newSource;
- (void)noteEdited;
- (void)displaySourceEditable;
- (void)applyHighlighting;
- (void)restyleRange:(NSRange)range tokens:(const std::vector<Token> &)tokens
           inStorage:(NSTextStorage *)storage;
- (NSDictionary *)attributesForStyle:(int)style;
- (void)restyleChangedIn:(NSRange)range tokens:(const std::vector<Token> &)tokens
               inStorage:(NSTextStorage *)storage;
- (void)textStorage:(NSTextStorage *)storage
    willProcessEditing:(NSTextStorageEditActions)actions
                 range:(NSRange)edited
        changeInLength:(NSInteger)delta;
- (void)flushHighlightingLater;
- (void)flushHighlighting;
- (BOOL)isSettingsFile;
- (void)decorateColorsIn:(NSTextStorage *)storage range:(NSRange)range;
- (BOOL)textView:(NSTextView *)tv clickedOnLink:(id)link atIndex:(NSUInteger)i;
- (void)pickColorAtIndex:(NSUInteger)i;
- (void)colorPicked:(NSColorPanel *)panel;
- (NSRect)editorRectForRange:(NSRange)range;
- (void)pickOpacityAtIndex:(NSUInteger)i;
- (void)opacityPicked:(double)value;
- (NSMenu *)textView:(NSTextView *)tv menu:(NSMenu *)menu forEvent:(NSEvent *)event
             atIndex:(NSUInteger)index;
- (void)appendSettingsLines:(const std::string &)lines title:(const char *)title;
- (void)centerEditorOnRange:(NSRange)range;
- (void)tintKeyLines:(NSArray<NSValue *> *)lines;
- (void)toggleTaskOnSourceLine:(int)line;
- (BOOL)canFormatSource;
- (void)formatSourceQuietly:(BOOL)quiet then:(void (^)(void))then;
- (BOOL)formatBeforeSaving;
- (void)finishFormatSave;
- (void)saveBeforeLeavingFormatSave;
- (BOOL)replaceSourceWithText:(NSString *)text actionName:(NSString *)name;
- (void)showFormatNote:(NSString *)message error:(BOOL)error;
- (void)showFormatNote:(NSString *)message error:(BOOL)error
               install:(NSArray<NSString *> *)install;
- (void)applyGutterSettings;
- (void)setEditorFont:(NSFont *)font;
- (void)editorZoomChanged:(NSNotification *)note;
- (std::vector<Brackets::Span>)nonCodeSpansFrom:(NSUInteger)lo to:(NSUInteger)hi;
- (NSRange)bracketWindowAround:(NSUInteger)caret;
- (BOOL)bracketMatchingApplies;
- (void)textViewDidChangeSelection:(NSNotification *)note;
- (void)updateBracketMatch;
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)sel;
- (BOOL)textView:(NSTextView *)tv shouldChangeTextInRange:(NSRange)range
    replacementString:(NSString *)replacement;
- (void)flashStatus:(NSString *)message;
- (void)endFlash;
- (BOOL)lspActionAllowed;
- (void)updateProblems;
- (void)updateProblemCounts;
- (void)updateInstallBar;
- (NSRect)layoutInstallBarIn:(NSRect)slot editorShown:(BOOL)shown;
- (void)applyInstallBarColors;
- (void)closeInstallBarMuting:(BOOL)mute;
- (void)showInstallHelpFrom:(NSView *)anchor;
- (void)typeCommandInTerminal:(NSString *)command;
- (void)textDidChange:(NSNotification *)note;
- (NSRect)textRectForCharacter:(NSUInteger)i;
- (void)scrollEditorToY:(CGFloat)y;
- (NSInteger)lineOfCharacter:(NSUInteger)i inString:(NSString *)s;
- (NSUInteger)characterAtStartOfLine:(NSInteger)line inString:(NSString *)s;
- (NSUInteger)markdownCharacterForSourceLine:(NSInteger)line;
- (NSInteger)markdownSourceLineNear:(NSUInteger)i;
- (BOOL)editMarkdownAtCharacter:(NSUInteger)index point:(NSPoint)point;
- (void)applyMarkdownSource:(NSString *)source;
- (void)openMarkdownLink:(NSString *)href;
- (BOOL)scrollToMarkdownAnchor:(NSString *)anchor;
- (void)updateTitle;
- (void)renderMarkdown:(NSString *)content;
- (BOOL)typesetsMath;
- (BOOL)mathNoteWanted;
- (NSAttributedString *)mathNoteText:(NSString *)text link:(NSString *)link;
- (NSAttributedString *)mathNoteForProblem:(NSString *)problem;
- (NSAttributedString *)mathNoteTypesetting:(NSUInteger)count;
- (NSAttributedString *)mathNote;
- (void)downloadTectonicForMath;
- (NSUInteger)mathNoteLength;
- (MCMathFormula *)typesetFormulaFor:(const MdRun &)r;
- (void)markdownMathArrived:(NSNotification *)note;
- (void)showArrivedMathForPath:(NSString *)path;
- (void)markdownWebImageArrived:(NSNotification *)note;
- (void)showArrivedWebImagesForPath:(NSString *)path;
- (void)renderMarkdownKeepingPlace;
- (NSParagraphStyle *)markdownParagraphFor:(const MdRun &)r
                                     quote:(NSTextBlock * __strong *)quote
                                      code:(NSTextBlock * __strong *)code;
- (NSAttributedString *)markdownRun:(const MdRun &)r
                              style:(NSParagraphStyle *)ps
                             inCell:(BOOL)inCell;
- (void)appendMarkdownTable:(const std::vector<MdRun> &)runs
                       from:(size_t)first
                         to:(size_t)end
                       into:(NSMutableAttributedString *)out;
- (void)setPlainMessage:(NSString *)msg;
- (void)setStatus:(NSString *)path;
- (void)changeRootTo:(NSString *)dir;
@end

// Each category's own methods. EditorController.h declares the ones the
// rest of the app calls, in an @interface of the same name, which is
// what the warning below is about.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-duplicate-category-definition"

// EditorController+Navigation.mm: Go to Line, Go Back and Forward, Go to Symbol.
@interface EditorController (Navigation) <MCNavigatorHost>
- (void)moveToLine:(int)line column:(int)column;
- (NSString *)navigationPath:(BOOL *)hasText;
- (BOOL)showNavigationPlace:(MCPlace *)place;
- (void)revealSymbolRange:(NSRange)r;
@end

// EditorController+Agents.mm: the inline agent, Request a Change, the control channel and agent work.
@interface EditorController (Agents) <MCChangeHost, MCAgentHost,
                                      MCAgentNotesHost, MCAgentWorkHost>
- (MCAgentSession *)agentSession;
- (NSView *)agentBarHost;
- (void)agentBarDidChange;
- (void)agentRunInTerminal:(NSString *)command;
- (NSString *)agentPathFor:(NSString *)path;
- (NSRect)agentLineRectAt:(NSUInteger)index;
- (void)agentTintAgainst:(NSString *)original;
- (MCChangeSession *)changeSession;
- (NSRect)layoutChangeBarIn:(NSRect)area;
- (void)applyChangeBarColors;
- (TerminalView *)changeTerminalRunning:(NSString *)command focus:(BOOL)focus;
- (void)runCommand:(NSString *)command inTerminal:(TerminalView *)terminal focus:(BOOL)focus;
- (BOOL)terminalIsOpen:(TerminalView *)terminal;
- (void)focusChangeTerminal:(TerminalView *)terminal;
- (void)changeBarDidChange;
- (void)restartIntoApp:(NSString *)appPath source:(NSString *)source
               command:(NSString *)command;
- (TerminalView *)changeTerminal;
- (void)keepKeyboardDuring:(void (^)(void))block;
- (NSTextView *)notesTextView;
- (id)notesActiveTab;
- (void)dropAgentPreviewTab:(MCTab *)t;
- (void)keepAgentPreviewTabAtIndex:(NSInteger)i;
- (void)showPage:(id<MCEditorPage>)page;
- (void)closePageOf:(MCTab *)t;
- (void)agentWorktreesChanged;
- (BOOL)agentWorktreesOnShow;
- (void)agentFolderChangedOnDisk:(NSString *)path;
- (MCCompareView *)compareViewMade;
@end

#pragma clang diagnostic pop
