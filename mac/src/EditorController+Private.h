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
#import "SidebarIcons.h"
#import "HintsSearchBox.h"
#import "StatusBranchItem.h"
#import "PalettePanel.h"
#import "Navigation.h"
#import "Gutter.h"
#import "FoldView.h"
#import "EditorFont.h"
#import "SourceStorage.h"
#import "AgentNotes.h"
#import "CompareView.h"
#import "FileViewers.h"
#include "TabSessions.h"
#include "Backups.h"
@class MCChangeReviewPage;
@class MCRemoteWorkspace;
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
#include "FolderSearch.h"   // the one writer's atomic write

// The stamp FolderSearch::writeFileAtomically gives for what it wrote.
MCDiskStamp *MCDiskStampFromFileStamp(const FolderSearch::FileStamp &s);

// A line diff of the source made ahead of time (off the main thread, for a
// clean tab following its file): the text it was made from, the new text,
// and the hunks between them (replaceSourceWithDiff:).
struct MCSourceDiff {
    std::u16string from, to;
    std::vector<LineDiff::Hunk> hunks;
};

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
static NSString *const kColorLink = @"mutible-color";
static NSString *const kOpacityLink = @"mutible-opacity";
// On rendered Markdown: the 0-based source line a stretch of text came from.
static NSAttributedStringKey const kMarkdownSourceLine = @"MCMarkdownSourceLine";
// On a fenced block's label line above it (a notebook's "In [3]"), for tests.
static NSAttributedStringKey const kMarkdownCodeLabel = @"MCMarkdownCodeLabel";
// On the runs of one of GitHub's alerts: its kind's name ("note", "tip",
// MarkdownParser::alertName), for tests.
static NSAttributedStringKey const kMarkdownAlert = @"MCMarkdownAlert";
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
static NSString *const kMathDownloadLink = @"mutible-math:download-tectonic";
// Link value on a task's box in rendered Markdown, followed by its 0-based
// source line: a click ticks or clears it.
static NSString *const kTaskLinkPrefix = @"mutible-task:";
// Link value on a <details> block's summary in rendered Markdown:
// "mutible-details:<tag line>:<summary run's line>:<1 open, 0 closed>". A
// click opens or closes it (toggleMarkdownDetails:atIndex:inPage:).
static NSString *const kDetailsLinkPrefix = @"mutible-details:";

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

// Shared by more than one of the files below.
// Screen points per PDF point of a formula set beside text of `fontSize`.
// TeX sets the formulas at 10 pt. Computer Modern's lowercase letters are
// smaller for their size than the system font's and its capitals about the
// same, so its em is made a little larger than the text's: halfway, near
// enough, between matching the capitals and matching the lowercase.
static inline CGFloat MCMathScaleFor(CGFloat fontSize) {
    static const CGFloat kEmToText = 1.12;             // math em / text size
    static const CGFloat kTenPointsInPdf = 10 * 72.0 / 72.27;
    return fontSize * kEmToText / kTenPointsInPdf;
}

typedef NS_ENUM(NSInteger, MCDiskDecision) { MCDiskWrite, MCDiskReload, MCDiskCancel };

// The tree menu's items for a worktree (shown only on one).
static const NSInteger kWorktreeMenuTag = 7701;
// Rename… and Move to Trash (and the line after them): hidden on a worktree
// row and the Worktrees node, where they would lose an agent's work.
static const NSInteger kNotOnWorktreeMenuTag = 7710;
// And for a folder holding web bookmarks, and for a bookmark file
// (EditorController+Bookmarks.mm).
static const NSInteger kBookmarkFolderMenuTag = 7702;
static const NSInteger kBookmarkFileMenuTag = 7703;
// Focus on Folder (and the line after it), shown on a folder's row, and
// Show Whole Folder in Tree, shown while the tree is focused.
static const NSInteger kFocusFolderMenuTag = 7704;
static const NSInteger kTreeFocusedMenuTag = 7705;
// Add to Focus, shown on a folder's row the tree does not show already as
// a focused folder or inside one, and Remove from Focus, on a root row
// while several folders are focused.
static const NSInteger kAddFocusMenuTag = 7706;
static const NSInteger kRemoveFocusMenuTag = 7707;

// ---------------------------------------------------------------------------
// The instance variables more than one category uses. A variable only one
// category uses goes in that category's own class extension instead,
// EditorController+<Category>Ivars.h, imported by EditorController.mm (where
// the ivars are laid out: a class extension's ivars exist only if the
// class's @implementation sees it) and by that category's file, so the
// other files neither see it nor rebuild when it changes. New features in
// files of their own keep their state in their own objects (MCEditorFind,
// MCFolds, MCNavigator, MCAgentSession), as the controller's pieces should.
// The `structure:ivars` core test holds this block to that rule.
@interface EditorController () <NSTextStorageDelegate> {
    FileItem *_root;
    // The open files: the window's tabs are the core's Model::Window (its
    // groups, their order, the tab each shows, the preview tabs, the closed
    // tabs), one of the app's Model::Session, which names each document by
    // its MCTab's docId; _docs is those MCTabs by id, every document of
    // either group. _activeTab is the tab the editor shows: the focused
    // group's tab on show, except while a tab is closing or being shown.
    Model::Window *_win;
    NSMutableDictionary<NSNumber *, MCTab *> *_docs;
    MCTab *_activeTab;
    // The viewers of FileViewers.h, made on first use (-viewers).
    MCFileViewers *_viewers;
    // Syntax highlighting of the editable source. While _liveHighlight is on,
    // every change to the text storage is re-highlighted as it happens, from
    // the storage delegate; _hl (null for a language without a grammar)
    // tracks line states so only the changed lines are re-lexed.
    std::unique_ptr<IncrementalHighlighter<char16_t>> _hl;
    BOOL _liveHighlight;
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
    // The same for formulas being typeset, by MCMathKey.
    NSMutableSet<NSString *> *_awaitedMath;
    NSDictionary *_sourceAttributes;       // font, paragraph style, plain color
    // The tab whose Cmd+S (format.on-save) waits for the formatter. A save
    // is never lost: leaving the tab saves it at once, and Format Document
    // started meanwhile saves when it is done.
    __weak MCTab *_formatSaveTab;
    BOOL _closing;                         // windowWillClose: has run
    NSSet<MCTab *> *_approvedWrites;       // Save All: disk questions already answered
    MCFolderSizer *_sizer;                 // the tree's folder totals (TreeDetails.h)
    MCAgentSession *_agent;                // the inline agent (Cmd+I), made on first use
    MCEditorFind *_editorFind;             // the editor's find and replace bar (EditorFind.h)
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
@property(nonatomic, strong) NSScrollView *hintsScroll;
@property(nonatomic, strong) NSTextView *hintsTextView;
@property(nonatomic, assign) NSInteger hintsColumns;   // as many as fit, as last laid out
@property(nonatomic, assign) BOOL hintsVisible;
@property(nonatomic, strong) MCHintsSearchBox *hintsSearchBox;   // the panel's search box
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
// The split view's first subview: the row of panel icons and whichever
// panel is on show (SidebarIcons.h, EditorController+SidebarIcons.mm).
@property(nonatomic, strong) MCSidebarHost *sidebarHost;
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
// The file on show's encoding when it is not plain UTF-8 ("Windows-1252",
// "UTF-16 LE", "UTF-8 with BOM"); hidden otherwise (StatusBar).
@property(nonatomic, strong) NSTextField *encodingLabel;
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
// The branch at the status bar's left end (StatusBranchItem.h), from the
// tree's git status; hidden outside a repository.
@property(nonatomic, strong) MCStatusBranchItem *statusBranchItem;
@end

// Each category's own methods. EditorController.h declares the ones the
// rest of the app calls, in an @interface of the same name, which is
// what the warning below is about.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-duplicate-category-definition"

// EditorController+Layout.mm: window setup, layout and look, the splits and what fills the sidebar, the title.
@interface EditorController (Layout)
- (CGFloat)titlebarHeight;
// The sidebar's width when shown (the width it comes back at while Cmd+B
// hides it), and setting it, hidden or not (a window brought back).
- (CGFloat)sidebarWidthToShow;
- (void)setSidebarWidth:(CGFloat)width hidden:(BOOL)hidden;
- (void)layoutContainer;
- (void)buildTitlebarInContainer:(NSView *)container;
- (void)applySettings;
- (void)showSettingsErrors;
- (void)keymapChanged:(NSNotification *)note;
- (void)recolorEditor;
- (void)relayoutRightArea;
// Maximize Pane (Ctrl+Cmd+M): one of the right area's panes takes all of
// it until toggled again. Showing or focusing another pane ends it
// (leaveMaximizedPaneFor:, which answers whether it did).
typedef NS_ENUM(NSInteger, MCPane) { MCPaneNone, MCPaneEditor, MCPaneBrowser, MCPaneTerminal };
- (MCPane)maximizedPane;
- (void)toggleMaximizedPane:(id)sender;
- (BOOL)leaveMaximizedPaneFor:(MCPane)pane;
- (void)terminalBarDraggedTo:(CGFloat)y;
- (void)browserBarDraggedTo:(CGFloat)x;
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
- (void)updateTitle;
@end

// EditorController+Processes.mm: the Processes panel's hooks.
@class MCProcessesPanel;
@class MCMarkdownSidePreview;
@interface EditorController (Processes)
@property(nonatomic, assign) BOOL processesVisible;   // the panel in the sidebar's place
- (MCProcessesPanel *)processesPanel;                 // made on first use
- (MCProcessesPanel *)processesPanelOnShow;           // nil unless shown
// The close question's Show in Processes: the panel shown, `t`'s job selected.
- (void)showProcessesForTerminal:(TerminalView *)t job:(pid_t)job;
@end

// EditorController+SidebarIcons.mm: the sidebar's row of panel icons
// (SidebarIcons.h). Layout makes the host; the panels' toggles show what
// they put in the sidebar through sidebarShowPanelView: and call
// sidebarIconsUpdate, as do the tree's git colors when they change.
@interface EditorController (SidebarIcons)
- (void)sidebarIconsInstall;                     // once, with the window
- (void)sidebarShowPanelView:(NSView *)view;     // where replaceSubview: was
- (void)sidebarIconsUpdate;                      // the mark, tips, and badges
- (void)sidebarIconsApplySettings;               // colors, and the setting
- (void)showSidebarPanel:(MCSidebarPanel)panel;  // an icon's click
@end

// EditorController+LatexInputs.mm: the files a LaTeX document reads, as its
// preview edits and saves them.
@class LatexView;
@interface EditorController (LatexInputs)
// The preview's hooks for \input files: their text, an edit put into their
// tab, whether they have unsaved changes.
- (void)wireLatexInputsOf:(LatexView *)view;
// Cmd+S with the preview on show: the files the last typeset read, saved.
- (void)saveLatexInputsOfActiveTab;
// A tab was written: previews that read its file typeset again.
- (void)latexFileSaved:(MCTab *)saved;
@end

// EditorController+ZoomFit.mm: Page Zoom for the browser and PDFs, and word wrap.
@interface EditorController (ZoomFit)
// What Cmd+= zooms in a page: a BrowserView or a PDFView, or nil.
- (NSView *)pageZoomTarget;
// Word wrap in the editor (editor.word-wrap, View > Word Wrap): applied to
// the text view, always wrapping a preview, which is not editable.
- (void)applyWordWrap;
- (BOOL)validateZoomFitItem:(NSMenuItem *)item enabled:(BOOL *)enabled;
@end

// EditorController+StatusBar.mm: the status bar, its notes and "Saved ...", and the shortcut hints.
@interface EditorController (StatusBar)
- (void)buildStatusBarInContainer:(NSView *)container height:(CGFloat)barH;
- (void)layoutStatusRight;
- (void)buildHintsPanelInContainer:(NSView *)container;
- (void)updateHints;
- (void)appendHintsHeading:(NSString *)title to:(NSMutableArray *)s;
- (void)appendHintsRow:(NSMutableArray *)s
                   key:(NSString *)key1 label:(NSString *)label1
                  key2:(NSString *)key2 label2:(NSString *)label2
                 state:(NSString *)state;
- (NSString *)hk:(NSString *)commandId;
- (NSString *)hkAll:(NSString *)commandId;
- (NSString *)hk:(NSString *)commandId at:(NSUInteger)index;
- (NSString *)hkAlso:(NSString *)commandId title:(NSString *)title;
- (NSAttributedString *)hintsText;
- (NSArray<NSDictionary *> *)hintsRows;
- (void)showStatusNote:(NSString *)note;
- (void)updateSavedStatus;
- (void)layoutStatusLabels;
// The tree's git status read the branch (Git::statusBranch's words; an
// empty name hides it).
- (void)showStatusBranchName:(NSString *)name counts:(NSString *)counts tip:(NSString *)tip;
- (void)flashStatus:(NSString *)message;
- (void)endFlash;
@end

// EditorController+Panels.mm: the terminal dock and the browser, and moving the keyboard between panes.
@interface EditorController (Panels)
- (void)makeTerminalPanel;
- (void)makeTerminalPanelFirstIn:(NSString *)first;
- (void)terminalPanelEmptied;
- (void)showTerminalPanel;
- (void)openTerminalLink:(MCTermLink *)link;
- (void)openTerminalLinkNow:(MCTermLink *)link;
- (void)makeBrowserWithHome:(NSString *)home;
// Rename Terminal… and Move Terminal Left and Right can act.
- (BOOL)validateTerminalTabCommand:(NSMenuItem *)item;
- (BOOL)resizeTerminalDockBy:(CGFloat)delta;
// Run File's way of running a command (see +Panels): saved first, typed
// into a terminal that is not busy (after a cd to `dir`, when given and
// the terminal is elsewhere), the keyboard kept. The terminal, or nil when
// the save failed.
- (TerminalView *)runFileCommand:(NSString *)command inDirectory:(NSString *)dir
                        busyNote:(NSString *)busyNote;
@end

// EditorController+EditCommands.mm: Indent and Outdent Lines, Insert Line
// Below and Above, Toggle Bold and Italic, Shift+Return to a Python REPL,
// Save As, Save All, and what a new terminal's environment gets.
@interface EditorController (EditCommandsPrivate) <MCKeymapLanguage>
- (BOOL)isEditCommand:(SEL)action;
- (BOOL)validateEditCommand:(NSMenuItem *)item;
- (NSDictionary<NSString *, NSString *> *)terminalEnvironmentForDirectory:(NSString *)dir;
@end

// EditorController+Search.mm: find and replace, Find in Folder and TODOs.
@interface EditorController (Search)
- (void)performTextFinderAction:(id)sender;
- (void)toggleFindMatchCase:(id)sender;
- (void)toggleFindWholeWord:(id)sender;
- (void)toggleFindRegex:(id)sender;
- (void)toggleFindPreserveCase:(id)sender;
- (void)toggleFindInSelection:(id)sender;
- (void)makeSearchPanel;
- (int)tabHoldOfPath:(NSString *)path disk:(NSString *__strong *)disk;
- (MCOpenFileState)replaceStateOfPath:(NSString *)path;
- (void)jumpToLine:(NSInteger)line;
@end

// EditorController+Tree.mm: the file tree: its data source, drag and drop, FSEvents, worktree rows, file operations, opening a folder.
@interface EditorController (Tree)
- (NSMenu *)buildTreeContextMenu;
- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item;
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)i ofItem:(id)item;
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item;
- (NSView *)outlineView:(NSOutlineView *)ov
     viewForTableColumn:(NSTableColumn *)col
                   item:(id)item;
// Files dropped on the editor's text open in tabs; folders show in the tree.
- (BOOL)openDroppedPaths:(NSArray<NSString *> *)paths;
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
- (void)revealPath:(NSString *)path open:(BOOL)open from:(EditorTabs::OpenFrom)from;
- (void)startWatching:(NSString *)path;
- (void)stopWatching;
- (void)reloadTreeKeepingState:(BOOL)rescan;
- (void)treeFoldersChangedOnDisk:(NSArray<NSString *> *)paths subtree:(NSArray<NSNumber *> *)subtree;
- (void)listChangedTreeFolders;
- (NSMutableDictionary<NSString *, FileItem *> *)loadedTreeFolders;
// A file (not a folder) at `path`, by this Mac's disk or over SSH the host.
- (BOOL)treeHasFileAt:(NSString *)path;
- (void)applyTreeListings:(NSDictionary<NSString *, NSArray<MCTreeEntry *> *> *)listings
                    stats:(NSDictionary<NSString *, MCTreeEntry *> *)stats
               reshowRoot:(BOOL)reshowRoot;
- (void)collectExpanded:(FileItem *)node into:(NSMutableSet<NSString *> *)expanded;
- (void)reExpand:(FileItem *)node usingSet:(NSSet<NSString *> *)expanded;
- (NSColor *)gitColorOf:(FileItem *)node tip:(NSString **)tip;
- (NSUInteger)treeGitRuns;
- (NSUInteger)treeGitChangeCount;   // paths git reports changed, for the Source Control badge
- (NSString *)worktreesFolder;
- (NSArray<FileItem *> *)withWorktreesNode:(NSArray<FileItem *> *)shown of:(FileItem *)node;
- (BOOL)isWorktreeRow:(FileItem *)node;
// A worktree's row or the Worktrees node: never renamed or trashed here.
- (BOOL)isWorktreeRowOrNode:(FileItem *)node;
- (NSAttributedString *)worktreeTitleFor:(FileItem *)node color:(NSColor *)text
                                    icon:(NSString **)icon tint:(NSColor **)tint
                                     tip:(NSString **)tip;
- (FileItem *)worktreeMenuItem;
- (void)compareWorktreeWithMain:(id)sender;
- (void)openWorktreeInNewWindow:(id)sender;
- (void)menuNeedsUpdate:(NSMenu *)menu;
- (void)outlineViewItemDidExpand:(NSNotification *)note;
- (NSString *)targetDirectory;
- (FileItem *)clickedOrSelectedItem;
- (NSString *)promptForName:(NSString *)title default:(NSString *)initial;
- (void)renamePath:(NSString *)path;
- (void)trashPath:(NSString *)path;
- (void)revealPathInFinder:(NSString *)path;
- (void)copyText:(NSString *)text;
- (NSString *)pathFromRoot:(NSString *)path;
- (void)warn:(NSString *)msg;
- (void)changeRootTo:(NSString *)dir;
// Selects `path` in the tree and opens it when `open` is set. While the
// tree is focused on a folder, a path outside it widens the focus first
// when `widen` is set (a reveal), and is otherwise left out of the tree
// (an open: Quick Open, a definition, a file from Finder). revealPath:
// andOpen: and revealPath:open:from: widen exactly when they do not open.
- (void)revealPath:(NSString *)path open:(BOOL)open from:(EditorTabs::OpenFrom)from
             widen:(BOOL)widen;
// Opens the tree's folders down to `path`, listing only those on the way,
// and gives the deepest row reached (`path`'s, or the folder where the way
// ends); nil when the path is left out of the tree (outside the focus,
// `widen` unset).
- (FileItem *)treeOpenFoldersTo:(NSString *)path widen:(BOOL)widen;
- (void)reloadTreeKeepingState:(BOOL)rescan alsoExpanding:(NSSet<NSString *> *)more;
@end

// EditorController+WindowTitle.mm: the window's title (the core's
// WindowTitle.h), set by updateTitle and when who has the keyboard changes.
@interface EditorController (WindowTitle)
// The title and representedURL for the tab on show and the focused pane.
- (void)refreshWindowTitle;
// The terminal's or the browser's titles changed: the window's again, only
// while one of them has the keyboard (a program's titles never cost the
// document's detail, such as a Markdown file's tasks counted).
- (void)windowTitlePaneChanged;
// The window's firstResponder observed (showWindow) and no longer (dealloc).
- (void)windowTitleStartObservingFocus;
- (void)windowTitleStopObservingFocus;
// observeValueForKeyPath: hands every change here first: YES when it was
// this category's.
- (BOOL)windowTitleObserved:(NSString *)keyPath of:(id)object context:(void *)context;
@end

// EditorController+TreeReveal.mm: the tree follows the active tab
// (explorer.auto-reveal). Its state is in EditorController+TreeRevealIvars.h.
@interface EditorController (TreeReveal)
// Another tab is on show (activateTab:): its file revealed in the tree,
// centered unless in view, or kept for when the tree shows again.
- (void)treeFollowActiveTab;
// The tree may have come on show (the sidebar opened, the tree back in
// it): a file that came on show while it was hidden is revealed, once.
- (void)treeRevealIfShown;
// Scrolls the tree to `row`: not at all when it is wholly in view, else
// with it in the middle.
- (void)scrollTreeRowIntoView:(NSInteger)row;
// A click or Return in the tree opens `path` (nil when done): its row is
// selected as clicked, and the tree does not scroll.
- (void)treeOpensPath:(NSString *)path;
// Held while a window's tabs come back: its tree is put back as it was.
- (void)holdTreeFollowing:(BOOL)hold;
// applySettings: explorer.auto-reveal turned on reveals the file on show.
- (void)treeAutoRevealSettingsApplied;
@end

// EditorController+TreeFocus.mm: Focus on Folder and Add to Focus
// (EditorController.h). Its state is in EditorController+TreeFocusIvars.h;
// the other categories ask these.
@class MCTreeFocusBar;
@interface EditorController (TreeFocus)
// The node the tree shows as its root: the focused folder, else _root;
// nil while several folders are focused, each a root row of its own.
- (FileItem *)treeTop;
// The rows at the top of the tree: the top's children, or the focused
// folders themselves while there are several.
- (NSArray<FileItem *> *)treeTopRows;
// The folder a command with no row to go by works in (New File with
// nothing selected, Copy Path): the top, else the first focused folder.
- (FileItem *)treeDefaultFolder;
// The focused folder (one of several) that is `path` or holds it; nil
// with fewer than two, or outside them all.
- (FileItem *)treeRootFor:(NSString *)path;
// A root row's label while several folders are focused (its path from the
// window's folder, "mac/src"); nil for any other row.
- (NSString *)treeRootLabelFor:(FileItem *)item;
// Whether a folder's rows are on show in the tree: a top or below one.
- (BOOL)treeShowsFolder:(FileItem *)folder;
// Opens the folders of `expanded` from the top down (the Worktrees node
// too on the whole folder); with several roots, each root open exactly
// when `expanded` holds it.
- (void)reExpandTreeTop:(NSSet<NSString *> *)expanded;
// Whether `path` (the tree's spelling) can show in the tree now, widening
// the focus to take it in when `widen` is set (one folder: to the nearest
// folder above both; several: the path's folder added).
- (BOOL)treeFocusShowsPath:(NSString *)path widen:(BOOL)widen;
// After the tree read the disk again: a focused folder gone from it is
// dropped (with a note), the whole folder shown when none is left, and the
// folders open outside the focus are returned to open again; nil when the
// focus stands.
- (NSSet<NSString *> *)dropLostTreeFocus;
// A new folder in the window: no focus, and nothing remembered.
- (void)forgetTreeFocus;
// The folders kept by a session or a window record: set again while they
// still name folders in the tree (those gone are dropped; with none left,
// the whole folder). Saves nothing.
- (void)restoreTreeFocus:(NSArray<NSString *> *)saved;
// Folders open outside the focus, kept so widening opens them again, and
// added to (a window brought back).
- (NSSet<NSString *> *)treeFocusRememberedExpanded;
- (void)rememberTreeExpanded:(NSSet<NSString *> *)paths;
// The breadcrumb or label row shown, hidden and colored as things stand.
- (void)updateTreeFocusBar;
- (BOOL)validateTreeFocusItem:(NSMenuItem *)item enabled:(BOOL *)enabled;
// What Add to Focus would add (the clicked folder; else the selected row's
// folder, else the open file's, the first not shown yet); nil for none.
- (NSString *)folderToAddToFocus;
// The same for one row (a folder, or a file's folder) alone, as its right
// click offers it.
- (NSString *)folderToAddToFocusFrom:(FileItem *)node;
// The bar, for the checks (nil until the tree is first focused).
- (MCTreeFocusBar *)treeFocusBar;
@end

// EditorController+TreeDetails.mm: the tree's sizes, dates and order.
@interface EditorController (TreeDetails)
- (void)setUpTreeDetails;
- (void)rootChanged;
- (NSArray<FileItem *> *)childrenOf:(FileItem *)node;
- (NSArray<FileItem *> *)orderOf:(NSArray<FileItem *> *)items;
- (void)configureTreeCell:(MCTreeCell *)cell item:(FileItem *)node;
- (void)refreshTreeCells;
- (void)fitTreeColumn;
- (void)sidebarFrameChanged:(NSNotification *)note;
- (void)folderTotalsArrived:(NSSet<NSString *> *)paths;
- (void)reorderNode:(FileItem *)node changed:(BOOL *)changed;
- (void)folderChangedOnDisk:(NSString *)path subtree:(BOOL)subtree;
- (NSString *)treeSpellingOf:(NSString *)diskPath;
- (void)setTreeSort:(TreeSort::Options)options;
- (TreeSort::Options)treeSortOptions;
- (void)treeDetailsChanged:(NSNotification *)note;
- (void)updateDetailsTimer;
@end

// EditorController+Tabs.mm: tabs, closed tabs, a tab's menu, saving.
@interface EditorController (Tabs)
// The text view's selection, first character in view and its distance
// below the pane's top, as stashActiveTab keeps them; NO when the slot
// shows no text of the tab's.
- (BOOL)liveTextPlace:(NSRange *)selection top:(NSUInteger *)top offset:(CGFloat *)offset;
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
- (NSUndoManager *)undoManagerForTextView:(NSTextView *)view;
- (void)buildTabStrip;
// A strip's clicks, closes and menus, for either group's strip.
- (void)wireTabStrip:(MCTabStrip *)strip;
- (NSArray<MCTabStripItem *> *)stripItemsForGroup:(int)group;
- (void)applyTabSettings;
- (NSRect)layoutTabStripIn:(NSRect)slot editorShown:(BOOL)shown;
- (void)reloadTabStrip;
- (MCTab *)tabForPath:(NSString *)path;
- (void)insertTab:(MCTab *)tab;
- (void)activateTab:(MCTab *)tab;
- (void)stashActiveTab;
- (void)showActiveTab;
- (void)restoreTextPlaceOf:(MCTab *)t;
- (EditorTabs::ClosedTab)closedPlaceOf:(MCTab *)t;
- (BOOL)canReopenClosedTab;
- (void)showEmptyState;
- (void)selectTabAtIndex:(NSInteger)i;
- (void)closeTabAtIndex:(NSInteger)i;
- (void)stepTab:(int)delta;
- (BOOL)confirmCloseTab:(MCTab *)t;
- (BOOL)saveTab:(MCTab *)t;
- (BOOL)tabHasTextToWrite:(MCTab *)t;
- (BOOL)writeText:(NSString *)text toTab:(MCTab *)t interactive:(BOOL)interactive;
// The same, why it failed in *failure (when given) rather than an alert or
// the status bar: a browser's page's Save As says it on the page.
- (BOOL)writeText:(NSString *)text toTab:(MCTab *)t interactive:(BOOL)interactive failure:(NSString **)failure;
// What every save leaves in the tab (writeText:, and the host's writes):
// `saved` the file's text, `stamp` the file as written.
- (void)tab:(MCTab *)t tookSaveOf:(NSString *)saved stamp:(MCDiskStamp *)stamp dirty:(BOOL)dirty;
// Closes the tab in the editor group with the keyboard (in the other group
// when only that one holds it). A file the other group also holds stays
// open there, so nothing is asked.
- (BOOL)closeTab:(MCTab *)t prompt:(BOOL)prompt;
// Closes the document in both groups (its file trashed, a server's or an
// agent's close).
- (BOOL)closeTabEverywhere:(MCTab *)t prompt:(BOOL)prompt;
- (void)dropAllTabs;
- (int)focusedCloseTarget;
- (void)focusAfterClosingPanel;
// Preview tabs (EditorTabs.h, "The preview tab"): a file opened from
// `from` goes in the preview tab when the settings say so, else in a kept
// tab; a kept open of a file in a preview tab keeps it.
- (void)openFileAtPath:(NSString *)path from:(EditorTabs::OpenFrom)from;
- (BOOL)opensInPreview:(EditorTabs::OpenFrom)from;
- (EditorTabs::PreviewSettings)previewSettings;
// A preview tab, the user's or an agent's, becomes an ordinary tab.
- (void)keepPreviewTab:(MCTab *)t;
- (void)keepTabAtIndex:(NSInteger)i;
- (void)keepTabFromMenu:(id)sender;
- (BOOL)openFileForLanguageServer:(NSString *)p from:(EditorTabs::OpenFrom)from;
@end

// EditorController+Session.mm: the session's host (SessionLink.h,
// editor.session-host): the storage's edits of the tab on show go to its
// document there, and the text view asks which document it shows.
@class MCSessionWindow, MCSessionLinkRef, MCSessionTermPlace;
@interface EditorController (Session)
- (void)installSessionHooks;
- (void)sessionForgetHistoryOf:(MCTab *)t;
// A backup restored into `t` (loadActiveTab): the host counts its text
// unsaved against the file's, `saved` ("" for a file not on disk).
- (void)sessionRestoredBackupOf:(MCTab *)t saved:(NSString *)saved;
// The tab's file holds `saved` (nil: the tab's text as it is now), which
// the window counts its text against: the host's count follows (DocSaved).
- (void)sessionTab:(MCTab *)t savedAs:(NSString *)saved;
- (CodeTextView *)textViewShowingSourceOf:(MCTab *)t;
- (void)tabTextChangedByHost:(MCTab *)t;
// Stage 7: saving through the host. Whether the host writes `text` for the
// tab (its document on a host that saves for apps), and the write itself,
// after the window's questions; the host's write for a page, and its count
// of the tab as unsaved (MCTabOwner).
- (BOOL)hostWritesText:(NSString *)text ofTab:(MCTab *)t;
- (BOOL)writeTextThroughHost:(NSString *)text
                       toTab:(MCTab *)t
                 interactive:(BOOL)interactive
                     failure:(NSString **)failure;
// A browser's page's New File: an untitled tab (Untitled-1, -2, ... in the
// folder, a file not on disk yet) with a document on the host and no path
// there, shown; nil when the window has no session.
- (MCTab *)openUntitledSessionTab;
- (void)tab:(MCTab *)t writtenByHost:(id)write;
- (void)tab:(MCTab *)t dirtyFromHost:(BOOL)dirty;
// How many times a tab was marked unsaved by the host's count (checks: a
// clean tab's reload must never be, docs/notes/session-host.md's traps).
extern NSUInteger MCDirtyFromHostMarks;
// Stage 6: the window on its host (SessionWindow.h), nil with the setting
// off; what a window opening takes (before the session restores the rest,
// then after it); Open Folder moving it; a window closing detaching; the
// close question's Keep Unsaved and Don't Save; Reconnect.
- (MCSessionWindow *)sessionWindow;
- (MCSessionLinkRef *)sessionLinkForTab:(MCTab *)t;
- (MCSessionTermPlace *)sessionTerminalPlace;
- (void)sessionDockChanged;
- (BOOL)sessionHoldsTerminals;
- (void)attachSessionHeld;
- (void)finishSessionAttach;
- (void)sessionFolderChanged;
- (BOOL)sessionKeepsUnsaved;
- (void)sessionDropOnClose:(NSArray<MCTab *> *)tabs;
- (void)reconnectSessionHost:(id)sender;
// Stage 8, a window over SSH: a tab's file read through the remote host
// (nil when the window is on this Mac: read the disk), and whether a tab
// needs a document on a host whatever editor.session-host says
// (MCTabOwner: every tab of a remote window does).
- (MCDiskText *)sessionRemoteTextOf:(MCTab *)t;
- (BOOL)tabNeedsSession:(MCTab *)t;
// The remote host's link of a window over SSH, nil for a window on this Mac.
- (MCSessionLinkRef *)sessionRemoteLink;
@end

// EditorController+Remote.mm: a window over SSH's folder through its host
// (docs/notes/session-host.md, "The window's tree, Quick Open and Find in
// Folder over SSH"): the tree's listings and the host's notices, Quick
// Open's list, Find in Folder, New File, and what is refused there. Every
// method does nothing (or NO) in a window on this Mac.
@interface EditorController (RemotePrivate)
// From useRemoteSessionLink: and moveToSessionLink: (Reconnect).
- (void)startRemoteWorkspace;
- (void)remoteWorkspaceMovedToLink;
// The window's MCRemoteWorkspace, nil on this Mac.
- (MCRemoteWorkspace *)remoteWorkspace;
// The tree listed, opened or closed folders: the host is told which to
// watch (on the main queue's next turn, once).
- (void)remoteTreeFoldersChanged;
// YES, with a status note saying why, when `what` cannot be done in a
// window over SSH (it would change or read this Mac's disk).
- (BOOL)refuseOverSsh:(NSString *)what;
// Find in Folder's window searches on the host.
- (void)searchOnHost:(SearchPanel *)panel;
// A file the window opened that is not on the host yet, and opening one.
- (BOOL)remotePathIsNew:(NSString *)path;
- (void)openNewRemoteFile:(NSString *)path;
@end

// EditorController+SessionBrowser.mm: what the window's record carries
// besides its tabs (SessionWindow.h).
@interface EditorController (SessionBrowserRecord)
- (NSDictionary *)sessionBrowserPanes;
- (NSDictionary *)sessionBrowserTabs;
// The window's open commit pages and worktree pages, for the page to show
// read-only: each @{@"kind": @"commit", @"hash", @"title", @"onShow"} or
// @{@"kind": @"worktree", @"path", @"title", @"onShow"}.
- (NSArray<NSDictionary *> *)sessionBrowserReviews;
// A tab's LaTeX preview, made (makeLatexPreview): its typesets go to the
// window's page in a browser (R3's third slice).
- (void)wireSessionTypesetOf:(LatexView *)view;
// The window's folder changed on disk (its FSEvents): the page's Source
// Control reads the changes again (R3's fourth slice).
- (void)sessionFolderChangedOnDisk;
@end

// EditorController+Groups.mm: two editor groups side by side (Split Editor),
// and the window's tabs (Model::Window), which each MCTab asks about itself.
@interface EditorController (Groups) <MCTabOwner>
- (BOOL)editorSplit;
- (BOOL)isSplit;   // the same, for key-value coding ("split")
- (NSInteger)focusedGroup;
// Every document of either group once (Model::Window::documents): the
// focused group's in tab order, then the other's. Backups, auto-save,
// sessions, the disk checks and the servers' open files go by it.
- (NSArray<MCTab *> *)tabs;
- (BOOL)holdsTab:(MCTab *)t;
// The focused group's tabs in strip order, and the other group's (empty
// unless split) with the tab it shows.
- (NSArray<MCTab *> *)groupTabs;
- (NSArray<MCTab *> *)parkedTabs;
- (MCTab *)parkedActive;
- (BOOL)otherGroupHoldsTab:(MCTab *)t;
// A new tab (one the window does not have yet) into the focused group, at
// `i`, or at the end (a session coming back); `kind` its preview state.
- (void)groupInsertTab:(MCTab *)t atIndex:(NSUInteger)i;
- (void)groupAppendTab:(MCTab *)t preview:(Model::Preview)kind;
// A new tab where the window's rules put it (Model::Window::add), replacing
// the preview tab of its kind; the replaced tab, for letGoOfReplacedTab:
// once the new one is on show.
- (MCTab *)groupAddTab:(MCTab *)t preview:(Model::Preview)kind passOver:(MCTab *)passOver;
- (void)letGoOfReplacedTab:(MCTab *)t;
- (void)removeTabFromOtherGroup:(MCTab *)t;
- (MCTab *)tabWithId:(Model::DocId)i;
// The MCTabs neither group holds any more are let go (after a close).
- (void)forgetClosedTabs;
- (NSRect)layoutEditorGroupsIn:(NSRect)slot editorShown:(BOOL)shown;
- (NSView *)groupDivider;
- (void)applyGroupSettings;
- (void)colorOtherGroupStrip;
- (void)reloadParkedStrip;
- (void)focusGroupOfStrip:(MCTabStrip *)strip;
- (void)focusEditorGroup:(NSInteger)g;
- (void)syncEditorMirror;
- (void)editorWillShowTab:(MCTab *)tab;
- (void)editorGroupsJoined;
- (void)joinEditorGroupsQuietly;
- (void)editorGroupClosed;
- (BOOL)validateGroupItem:(NSMenuItem *)item enabled:(BOOL *)enabled;
- (NSDictionary *)editorGroupsSessionState;
- (void)restoreEditorGroups:(NSDictionary *)state;
@end

// EditorController+Tests.mm: Run Test at Cursor's menu item.
@interface EditorController (TestsPrivate)
- (BOOL)validateTestItem:(NSMenuItem *)item enabled:(BOOL *)enabled;
@end

// EditorController+LineHistory.mm: Show Line History's menu item, and
// letting it go with the window.
@interface EditorController (LineHistoryPrivate)
- (BOOL)validateLineHistoryItem:(NSMenuItem *)item enabled:(BOOL *)enabled;
- (void)stopLineHistory;
@end

// EditorController+Persistence.mm: the tabs between runs, backups and hot exit, auto-save, files changed, moved or deleted outside.
@interface EditorController (PageTabs)
// A tab that shows no file as the sessions keep it (the review, a commit's
// page, a comparison that can be made again); NO for any other tab.
- (BOOL)pageTabOf:(MCTab *)t into:(TabSessions::PageTab *)out;
// Such a tab made again, in no window yet; nil when it cannot be (a page
// of its kind is open already). A comparison's sides are read after.
- (MCTab *)tabForRestoredPage:(const TabSessions::PageTab &)p;
@end

@interface EditorController (Git)
// The window's review page and commit page, new, with their hooks.
- (MCChangeReviewPage *)newChangeReviewPage;
- (MCChangeReviewPage *)newCommitReviewPage;
@end

@interface EditorController (Persistence)
// The folder's session as sessions.json holds it, in the shape the user
// defaults held it (files, active, preview, sidePreview, groups, browser),
// for the checks; nil for none.
- (NSDictionary *)storedSession;
- (BOOL)sessionsEnabled;
- (NSString *)sessionKey;
- (NSArray<NSString *> *)sessionFilePaths;
- (NSDictionary *)browserSessionState;
- (void)restoreBrowserFromSession;
- (void)restoreSession;
// A window brought back (restoreWindowRecord:), as showWindow makes it: its
// frame before it is shown, its sidebar, terminals and Problems before its
// tabs come back, and its tree, hidden editor and keyboard after.
- (void)placeRestoredWindow;
- (void)restoreWindowLayout;
- (void)finishRestoringWindow;
// A tab brought back with its window, read for the first time
// (loadActiveTab): its page or source, its selection and scroll.
- (void)applySessionPlaceOf:(MCTab *)t;
// The window is closing: the windows to bring back without it, or with it
// when it is the last (windowWillClose:, before the terminals go).
- (void)noteWindowClosing;
- (BOOL)tabTakesBackup:(MCTab *)t;
- (BOOL)tabNeedsBackup:(MCTab *)t;
- (void)scheduleBackups;
- (void)writeBackups;
- (void)writeBackupsOf:(NSArray<MCTab *> *)tabs;
// The window is closing: no backup or auto-save is planned for it any more.
- (void)stopSavePlan;
- (void)discardBackupOf:(MCTab *)t;
// windowWillClose:'s: every tab's backup, unless quitting kept them.
- (void)discardBackupsAsWindowCloses;
- (void)backupsAppWillTerminate:(NSNotification *)note;
- (BOOL)takeBackup:(NSString *)bid intoTab:(MCTab *)t;
// This window's name in the backups (Backups::Entry::window).
- (NSString *)backupKey;
- (void)restoreConflictingBackups:(const std::vector<Backups::Restore::Item> &)conflicts;
- (void)showRestoredText:(NSString *)text base:(NSDate *)base inode:(uint64_t)inode;
- (Settings::AutoSave)autoSaveMode;
- (BOOL)tabTakesAutoSave:(MCTab *)t;
- (void)scheduleAutoSave;
- (void)autoSaveOnFocusChange;
- (void)autoSaveNow;
- (BOOL)writeTabForAutoSave:(MCTab *)t;
- (void)reloadCleanTab:(MCTab *)t;
- (void)followCleanTabInBackground:(MCTab *)t;
- (void)takeDiskRead:(MCDiskText *)d intoCleanTab:(MCTab *)t
                diff:(std::shared_ptr<MCSourceDiff>)diff;
- (void)registerReloadUndoForTab:(MCTab *)t text:(NSString *)old;
- (void)registerTextUndoForTab:(MCTab *)t text:(NSString *)old actionName:(NSString *)name;
- (void)putBackText:(NSString *)text inTab:(MCTab *)t;
- (void)putBackText:(NSString *)text inTab:(MCTab *)t actionName:(NSString *)name;
- (void)checkExternalChange;
- (void)takeDiskText:(MCDiskText *)d forTab:(MCTab *)t;
- (void)tabWentMissing:(MCTab *)t;
- (void)tabCameBack:(MCTab *)t;
- (NSString *)renamedFileFor:(MCTab *)t;
- (void)checkTabsOnDisk;
- (BOOL)mayWriteTab:(MCTab *)t;
- (void)reloadTabFromDisk:(MCTab *)t;
- (MCDiskDecision)diskDecisionForTab:(MCTab *)t;
@end

// EditorController+Rendering.mm: what the editor's slot shows: a file's mode, pictures, PDFs, media, viewers, diffs, LaTeX.
@interface EditorController (Rendering)
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context;
- (void)showDiffNamed:(NSString *)name path:(NSString *)path data:(NSData *)diff;
- (void)showCommitTitled:(NSString *)title data:(NSData *)show;
- (void)showDiffTitled:(NSString *)name data:(NSData *)diff commit:(BOOL)commit;
- (void)renderDiff;
- (void)resetViewMode;
+ (BOOL)isImagePath:(NSString *)path;
- (BOOL)showImageAtPath:(NSString *)path;
- (BOOL)showPDFAtPath:(NSString *)path;
- (void)pdfKeepsPlaceGiven;   // a tab shown again puts back its own place
+ (BOOL)isMediaPath:(NSString *)path;
- (void)observeMediaItem:(AVPlayerItem *)item;
- (void)stopMedia;
- (void)mediaItemChanged:(AVPlayerItem *)item;
- (BOOL)showMediaAtPath:(NSString *)path;
- (MCFileViewers *)viewers;
- (void)viewerEditedText:(NSString *)text actionName:(NSString *)name;
- (void)setViewerText:(NSString *)text actionName:(NSString *)name reshow:(BOOL)reshow;
- (BOOL)registerViewerUndoFrom:(NSString *)old to:(NSString *)now actionName:(NSString *)name;
- (BOOL)showViewer:(MCViewerKind)kind;
- (void)openFileAtPath:(NSString *)path;
- (void)loadActiveTab;
- (NSDate *)diskModDateFor:(NSString *)path;
- (void)recordDiskStateOf:(MCTab *)t;
- (void)recordStamp:(MCDiskStamp *)s ofTab:(MCTab *)t;
- (BOOL)tab:(MCTab *)t changedOnDisk:(MCDiskStamp *)s;
- (void)recordModDate;
- (void)forgetHistoryOf:(MCTab *)t;
- (void)reselectCurrentFileInTree;
- (void)refreshDisplay;
- (void)showLatexPreview;
- (void)makeLatexPreview;
- (void)openLatexProblemAt:(NSString *)path line:(NSInteger)line;
- (void)showLatexProblemMarks;
- (BOOL)latexIsShowing;
- (void)latexDidEditSource:(NSString *)newSource;
- (void)latexShowSource:(NSString *)path line:(NSInteger)line column:(NSInteger)column;
- (void)setPlainMessage:(NSString *)msg;
@end

// EditorController+PageSide.mm: Open Preview to the Side for LaTeX and HTML files.
@interface EditorController (PageSide)
- (BOOL)pageSideWanted;
- (BOOL)pageSideShowing;
- (BOOL)pageSideShowsLatex;
- (NSView *)pageSideDivider;
- (NSRect)layoutPageSideIn:(NSRect)rect textShown:(BOOL)textShown;
- (void)refreshPageSide;
- (void)pageSideSourceChanged;
- (void)pageSideSelectionChanged;
- (void)pageSideSaved:(MCTab *)t;
- (NSString *)pageSideBufferFor:(MCTab *)t;
- (void)pageSideTabWillLeave:(MCTab *)t;
- (void)closePageSide;
- (void)pageSideBarDraggedTo:(CGFloat)x;
@end

// EditorController+Highlighting.mm: the source's syntax colors, kept up as it is edited.
@interface EditorController (Highlighting)
- (void)displaySourceEditable;
// The editor's storage is about to hold text blocks (a Markdown page's
// quotes, code boxes, headings and tables): the next source shown starts
// the layout manager's block table afresh.
- (void)layoutHeldTextBlocks;
// Turn the editor read-only, letting go of a long text first, so the
// line numbers hiding does not lay all of it out.
- (void)makeEditorReadOnly;
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
// The language server's semantic tokens for the text as it is
// (LspSession.onSemanticTokens): an array of Lsp::SemanticSpan.
- (void)semanticTokensArrived:(NSData *)spans;
// editor.spell-check for the file shown (SpellMarks.h), and the whole
// lines of a range with their tokens, which its checks read.
- (void)updateSpellCheck;
- (NSRange)spellLinesFor:(NSRange)range tokens:(std::vector<Token> *)tokens;
@end

// EditorController+LargeFile.mm: large-file mode (LargeFile.h), its status
// bar note, and the question before opening a file over 1 GB.
@interface EditorController (LargeFile)
- (void)noteLargeFileText:(NSString *)text;     // the tab on show, as read
- (BOOL)largeFileFeaturesOff;                   // its colors, folds, wrap and server are off
- (NSString *)languageServerPath:(NSString *)path;   // nil to keep it from the servers
- (BOOL)languageServerMayRead:(MCTab *)tab;
- (BOOL)confirmOpeningLargeFile:(NSString *)path;
- (void)updateLargeFileNote;
- (CGFloat)layoutLargeFileNoteEndingAt:(CGFloat)right;
- (void)turnOnLargeFileFeatures:(id)sender;
@end

// EditorController+Settings.mm: the settings file: color swatches, opacity, Adjust Settings.
@interface EditorController (Settings)
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
@end

// EditorController+Format.mm: Format Document.
@interface EditorController (Format)
- (BOOL)canFormatSource;
- (void)formatSourceQuietly:(BOOL)quiet then:(void (^)(void))then;
- (BOOL)formatBeforeSaving;
- (BOOL)organizeOrFormatBeforeSaving:(BOOL)organize;
- (void)finishFormatSave;
- (void)saveBeforeLeavingFormatSave;
- (BOOL)replaceSourceWithText:(NSString *)text actionName:(NSString *)name;
- (BOOL)replaceSourceWithDiff:(std::shared_ptr<MCSourceDiff>)diff actionName:(NSString *)name;
- (void)showFormatNote:(NSString *)message error:(BOOL)error;
- (void)showFormatNote:(NSString *)message error:(BOOL)error
               install:(NSArray<NSString *> *)install;
- (void)offerFormatOnSaveAfterSaving;
- (NSRect)layoutFormatOnSaveOfferIn:(NSRect)slot editorShown:(BOOL)shown;
@end

// EditorController+EditorView.mm: the editor: line numbers, brackets, zoom, comments, tasks and the editing hooks.
@interface EditorController (EditorView) <MCAutoCloseHost>
- (void)noteEdited;
- (void)toggleTaskOnSourceLine:(int)line;
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
- (void)textDidChange:(NSNotification *)note;
@end

// EditorController+Navigation.mm: Go to Line, Go Back and Forward, Go to Symbol.
@interface EditorController (Navigation) <MCNavigatorHost>
- (void)moveToLine:(int)line column:(int)column;
- (NSString *)navigationPath:(BOOL *)hasText;
- (BOOL)showNavigationPlace:(MCPlace *)place;
- (void)revealSymbolRange:(NSRange)r;
@end

// EditorController+LanguageServers.mm: the language servers' commands, problems, and installing a server.
@interface EditorController (LanguageServers)
- (BOOL)lspActionAllowed;
- (void)updateProblems;
- (void)updateProblemCounts;
- (void)updateInstallBar;
- (NSRect)layoutInstallBarIn:(NSRect)slot editorShown:(BOOL)shown;
- (void)applyInstallBarColors;
- (void)closeInstallBarMuting:(BOOL)mute;
- (void)showInstallHelpFrom:(NSView *)anchor;
- (void)typeCommandInTerminal:(NSString *)command;
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
- (void)agentTintAgainst:(NSString *)proposed;
- (MCChangeSession *)changeSession;
- (NSRect)layoutChangeBarIn:(NSRect)area;
- (void)applyChangeBarColors;
- (NSRect)layoutAgentTaskBarIn:(NSRect)area;
- (void)applyAgentTaskBarColors;
- (void)agentTaskBarDidChange;
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
- (void)showPage:(id<MCEditorPage>)page;
- (MCTab *)showPage:(id<MCEditorPage>)page preview:(BOOL)preview;
- (void)closePageOf:(MCTab *)t;
- (void)agentWorktreesChanged;
- (BOOL)agentWorktreesOnShow;
- (NSAttributedString *)agentMarkdown:(NSString *)markdown width:(CGFloat)width;
- (void)agentFolderChangedOnDisk:(NSString *)path;
- (MCCompareView *)compareViewMade;
- (void)openFileAtPath:(NSString *)path line:(NSInteger)line;
@end

// EditorController+Markdown.mm: the Markdown preview: switching to it, editing from it, links, math and web pictures arriving.
@interface EditorController (Markdown)
- (void)endTextSessionOfTab:(MCTab *)t;
- (BOOL)showsRenderedMarkdown;
- (NSString *)markdownToRender;
- (NSRect)textRectForCharacter:(NSUInteger)i;
- (void)scrollEditorToY:(CGFloat)y;
- (void)scrollEditorToCharacter:(NSUInteger)i offset:(CGFloat)offset;
- (NSInteger)lineOfCharacter:(NSUInteger)i inString:(NSString *)s;
- (NSUInteger)characterAtStartOfLine:(NSInteger)line inString:(NSString *)s;
- (NSUInteger)markdownCharacterForSourceLine:(NSInteger)line;
- (NSInteger)markdownSourceLineNear:(NSUInteger)i;
- (BOOL)editMarkdownAtCharacter:(NSUInteger)index point:(NSPoint)point;
- (BOOL)editSidePreviewAtCharacter:(NSUInteger)index point:(NSPoint)point;
// A double-click at `at` (window coordinates) on whichever page is under it,
// the full page or the one beside the source: the block editor opens, as the
// page's own double-click opens it. For a double-click that began in the
// other editor group (otherGroupPressed:).
- (BOOL)editPageAtWindowPoint:(NSPoint)at;
- (BOOL)editMarkdownIn:(NSTextView *)page atCharacter:(NSUInteger)index point:(NSPoint)point;
- (void)applyMarkdownEdit:(NSString *)source side:(BOOL)side;
- (void)applySidePreviewEdit:(NSString *)source;
- (void)applyMarkdownSource:(NSString *)source;
- (void)openMarkdownLink:(NSString *)href;
- (void)htmlPageRefused:(NSURL *)url;
- (BOOL)scrollToMarkdownAnchor:(NSString *)anchor;
- (void)renderMarkdown:(NSString *)content;
- (BOOL)typesetsMath;
- (BOOL)mathNoteWanted;
- (NSAttributedString *)mathNoteText:(NSString *)text link:(NSString *)link;
- (NSAttributedString *)mathNoteForProblem:(NSString *)problem;
- (NSAttributedString *)mathNoteTypesetting:(NSUInteger)count;
- (NSAttributedString *)mathNote;
- (void)downloadTectonicForMath;
- (void)downloadMermaidForPreview;
- (NSUInteger)mathNoteLength;
- (MCMathFormula *)typesetFormulaFor:(const MdRun &)r;
- (void)markdownMathArrived:(NSNotification *)note;
- (void)markdownMathProgress:(NSNotification *)note;
- (void)showArrivedMathForPath:(NSString *)path;
- (void)markdownWebImageArrived:(NSNotification *)note;
- (void)showArrivedWebImagesForPath:(NSString *)path;
- (void)renderMarkdownKeepingPlace;
// refreshDisplay for a tab whose text changed under it (read again from
// disk, a reload undone): a rendered page stays where it was scrolled.
- (void)refreshDisplayKeepingPage;
- (void)finishMarkdownPage;
- (NSMutableAttributedString *)markdownPageFor:(NSString *)content
                                         width:(CGFloat)width
                                       anchors:(NSDictionary **)anchorsOut;
- (void)beginMarkdownPageWidth:(CGFloat)width;
- (NSMutableAttributedString *)markdownRuns:(const std::vector<MdRun> &)runs
                                       from:(size_t)from
                                         to:(size_t)to
                                    anchors:(NSMutableArray<NSArray *> *)anchors;
- (NSAttributedString *)markdownNoteForRuns:(const std::vector<MdRun> &)runs;
- (NSAttributedString *)markdownFragmentFor:(NSString *)content width:(CGFloat)width;
- (void)keepPlaceIn:(NSTextView *)tv scroll:(NSScrollView *)scroll
     whileRendering:(void (^)(void))render;
- (BOOL)showsMarkdownPage;
- (void)renderShownMarkdownKeepingPlace;
// The preview to the side (the pane is MarkdownSidePreview.h's).
- (BOOL)sidePreviewWanted;
- (MCMarkdownSidePreview *)sidePreview;
- (NSView *)sidePreviewDivider;
- (NSRect)layoutSidePreviewIn:(NSRect)rect textShown:(BOOL)textShown;
- (void)dropSidePreviewPage;
- (void)refreshSidePreview;
- (void)sidePreviewTabWillLeave:(MCTab *)t;
- (void)renderSidePreview;
- (void)renderSidePreviewKeepingPlace;
- (void)sidePreviewSourceChanged;
- (void)sidePreviewApplySettings;
- (void)closeSidePreview;
- (void)sidePreviewBarDraggedTo:(CGFloat)x;
- (BOOL)sidePreviewClickedLink:(id)link atIndex:(NSUInteger)index;
- (BOOL)toggleMarkdownDetails:(NSString *)link atIndex:(NSUInteger)index inPage:(NSTextView *)page;
- (std::map<int, bool>)markdownDetailsOpened;
- (BOOL)sidePageIsCurrent;
- (BOOL)catchUpSidePage;
- (void)renderSidePreviewAfterTyping;
@end

// EditorController+MarkdownRender.mm: turning Markdown runs into attributed text: paragraphs, runs, tables.
@interface EditorController (MarkdownRender)
- (void)applyMarkdownMeasure;   // markdown.max-width for the page on show
- (NSParagraphStyle *)markdownParagraphFor:(const MdRun &)r
                                    quotes:(NSMutableArray<NSTextBlock *> *)quotes
                                      code:(NSTextBlock * __strong *)code;
- (NSAttributedString *)markdownLabelFor:(const MdRun &)r;
- (NSAttributedString *)markdownRun:(const MdRun &)r
                              style:(NSParagraphStyle *)ps
                             inCell:(BOOL)inCell;
- (void)appendMarkdownTable:(const std::vector<MdRun> &)runs
                       from:(size_t)first
                         to:(size_t)end
                       into:(NSMutableAttributedString *)out;
@end

// EditorController+Bookmarks.mm: web bookmarks open in the browser pane.
@interface EditorController (Bookmarks)
// Loads the bookmark file's address in the browser and answers YES; NO for
// a file that is no bookmark (or holds no address it may open), which then
// opens as any file does. `focus` gives the page the keyboard.
- (BOOL)openBookmarkAtPath:(NSString *)path focusPage:(BOOL)focus;
// The browser pane made and shown, `load` run in it with the keyboard kept;
// `focus` gives the page the keyboard after.
- (void)showBrowserFor:(void (^)(void))load focusPage:(BOOL)focus;
@end

#pragma clang diagnostic pop
