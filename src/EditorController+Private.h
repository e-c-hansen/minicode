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

// ---------------------------------------------------------------------------
@interface EditorController () <NSTextStorageDelegate, MCChangeHost,
                                MCAgentHost, MCAgentNotesHost, MCNavigatorHost,
                                MCAgentWorkHost> {
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
@end

typedef NS_ENUM(NSInteger, MCDiskDecision) { MCDiskWrite, MCDiskReload, MCDiskCancel };

// The tree menu's items for a worktree (shown only on one).
static const NSInteger kWorktreeMenuTag = 7701;
