// EditorController.h — Cocoa window: file tree (left) + editor/preview (right).
#import <Cocoa/Cocoa.h>

@class BrowserView, MCAgentNotes, MCComparison, MCPaletteContext, TerminalView;

@interface EditorController : NSObject <NSOutlineViewDataSource,
                                        NSOutlineViewDelegate,
                                        NSSplitViewDelegate,
                                        NSTextViewDelegate,
                                        NSWindowDelegate,
                                        NSMenuItemValidation,
                                        NSMenuDelegate>
@property(nonatomic, readonly, strong) NSWindow *window;
@property(nonatomic, readonly, copy) NSString *rootPath;
- (instancetype)initWithRootPath:(NSString *)path;
- (void)undo:(id)sender;              // Cmd+Z; the text view handles its own
- (void)redo:(id)sender;              // Shift+Cmd+Z
// Commands still running in this window's terminals (empty when every shell
// is idle), for the question asked before quitting.
- (NSArray<NSString *> *)runningTerminalCommands;
@end

// EditorController+Layout.mm
@interface EditorController (Layout)
- (void)showWindow;
- (void)toggleSidebarVisibility:(id)sender;     // Cmd+B collapse/restore file tree
- (void)toggleSourceControl:(id)sender;   // Ctrl+Shift+G git panel in the sidebar
- (void)toggleAgents:(id)sender;          // Shift+Cmd+A agent sessions in the sidebar
- (void)toggleEditor:(id)sender;      // Shift+Cmd+E hide/restore the file editor
@end

// EditorController+Git.mm: pushing and pulling, staging everything,
// discarding and the branch picker, on the Source Control panel's object
// whether it shows or not (MCGitPanel).
@class MCGitPanel;
@interface EditorController (Git)
- (MCGitPanel *)ensureGitPanel;   // made with the window, for autofetch
- (void)gitFetch:(id)sender;
- (void)gitPull:(id)sender;
- (void)gitPush:(id)sender;
- (void)gitSync:(id)sender;       // pull, then push
- (void)gitStageAll:(id)sender;
- (void)gitUnstageAll:(id)sender;
- (void)gitClean:(id)sender;      // Discard Changes: the panel's row, else the file on show
- (void)gitUndoDiscard:(id)sender;
- (void)gitCheckout:(id)sender;   // the branch picker, in the palette
- (BOOL)validateGitItem:(NSMenuItem *)item enabled:(BOOL *)enabled;
@end

// EditorController+Processes.mm
@interface EditorController (Processes)
// The Processes panel in the sidebar's place, as Agents is (ProcessesPanel.h).
- (void)toggleProcesses:(id)sender;
@end

// EditorController+StatusBar.mm
@interface EditorController (StatusBar)
- (void)toggleHints:(id)sender;       // Shift+Cmd+H shortcut-hints overlay
@end

// EditorController+Panels.mm
@interface EditorController (Panels)
- (void)toggleTerminal:(id)sender;    // Ctrl+` bottom terminal dock
- (void)newTerminal:(id)sender;       // Ctrl+Shift+` a terminal in a new tab
- (void)splitTerminal:(id)sender;     // Cmd+\ a terminal beside the active one
- (void)nextTerminal:(id)sender;      // Option+Cmd+] across panes and tabs
- (void)previousTerminal:(id)sender;  // Option+Cmd+[
- (void)closeTerminal:(id)sender;     // Ctrl+Shift+W the active terminal
// Shows the dock if hidden, then `terminal`'s tab, with the keyboard in it
// (Next Waiting Terminal, the Agents panel's Show Terminal). NO when it is
// not one of this window's.
- (BOOL)revealTerminal:(TerminalView *)terminal;
// Closes the terminal the keyboard is in (asking first when a command runs
// in it) and returns YES; NO when the keyboard is not in a terminal (for a
// Cmd+W that closes what has focus).
- (BOOL)closeFocusedTerminal;
- (void)runCurrentFile:(id)sender;    // Shift+Cmd+Return run the file in a terminal
- (void)toggleBrowser:(id)sender;     // Shift+Cmd+B embedded browser
- (void)focusTree:(id)sender;         // Cmd+0 move keyboard focus to file tree
- (void)focusEditor:(id)sender;       // Cmd+1 move keyboard focus to editor
- (void)focusTerminal:(id)sender;     // Cmd+2 the terminal, opening it if hidden
- (void)focusBrowser:(id)sender;      // Cmd+3 the browser's address bar, likewise
@end

// EditorController+Search.mm
@interface EditorController (Search)
- (void)openSearch:(id)sender;        // Cmd+Shift+F project-wide search
- (void)openReplaceInFolder:(id)sender;   // Option+Shift+Cmd+F, the same with replace
// The editor window itself, or a window it made for itself (Find in Folder,
// TODOs, a sheet or child window of it).
- (BOOL)ownsWindow:(NSWindow *)window;
- (void)showTodos:(id)sender;          // Shift+Cmd+D TODOs in the folder
@end

// EditorController+Tree.mm
@interface EditorController (Tree)
- (void)revealPath:(NSString *)path andOpen:(BOOL)open;  // select in tree
// Open a file handed in from outside (Finder, the Dock, the command line),
// spelled from rootPath, in its tab (a new one unless it is open already).
- (BOOL)showFile:(NSString *)path;
- (void)openFolder:(id)sender;    // menu action
// This window moves to `dir` (minicode -r), asking about unsaved tabs first;
// NO when the user cancelled.
- (BOOL)switchToFolder:(NSString *)dir;
- (void)toggleHiddenFiles:(id)sender; // Cmd+Shift+. show/hide dotfiles
// The file tree's right click menu: the clicked or selected row, else the
// open folder. The Sort By items' tags are TreeSort keys (sortTreeBy:) and
// 1 or 0 for ascending (setTreeSortDirection:).
- (void)revealInFinder:(id)sender;
- (void)copyPath:(id)sender;
- (void)copyRelativePath:(id)sender;
+ (NSMenu *)treeSortMenu;
- (void)newFile:(id)sender;
- (void)newFolder:(id)sender;
- (void)renameSelected:(id)sender;
- (void)deleteSelected:(id)sender;
- (void)refreshTree:(id)sender;
@end

// EditorController+TreeDetails.mm
@interface EditorController (TreeDetails)
- (void)toggleTreeColumn:(id)sender;  // the tree's size, saved and created columns (tag)
- (void)sortTreeBy:(id)sender;
- (void)setTreeSortDirection:(id)sender;
- (void)toggleFoldersFirst:(id)sender;
@end

// EditorController+Tabs.mm
@interface EditorController (Tabs)
- (void)saveCurrentFile:(id)sender;   // Cmd+S
- (void)selectNextTab:(id)sender;     // Ctrl+Tab, Shift+Cmd+]
- (void)selectPreviousTab:(id)sender; // Ctrl+Shift+Tab, Shift+Cmd+[
- (void)selectTabNumber:(id)sender;   // Shift+Cmd+1..9, Ctrl+1..9 (tag = n; 9 = last)
- (void)closeFocused:(id)sender;      // Cmd+W the tab, or the focused terminal/browser
- (void)reopenClosedTab:(id)sender;   // Option+Shift+Cmd+T the last tab closed here
// A tab's right click menu, for the tab on show, in the File menu (so the
// command palette finds them too).
- (void)closeOtherTabs:(id)sender;
- (void)closeTabsToTheRight:(id)sender;
- (void)closeAllTabs:(id)sender;
- (void)copyActiveFilePath:(id)sender;
- (void)copyActiveFileRelativePath:(id)sender;
- (void)revealActiveFileInFinder:(id)sender;
- (void)revealActiveFileInTree:(id)sender;
// Asks about every tab with unsaved changes (window close, quit). NO when
// the user cancelled or a save failed.
- (BOOL)confirmCloseAllTabs;
@property(nonatomic, readonly) BOOL hasUnsavedChanges;
- (NSArray<NSString *> *)recentTabPaths;
@end

// EditorController+Persistence.mm
@interface EditorController (Persistence)
// Request a Change's restart: every unsaved tab's backup written now, for
// the new build to bring back, so they need no question. NO when that could
// not be done (backups off, or a write failed), and the question is asked.
- (BOOL)keepUnsavedTabsForRestart;
// Writes this window's session (tabs, browser) now; quitting calls it.
- (void)saveSession;
@end

// EditorController+Rendering.mm
@interface EditorController (Rendering)
- (void)exportPDF:(id)sender;         // Shift+Cmd+S LaTeX: save the typeset PDF
@property(nonatomic, readonly) BOOL canExportPDF;
- (void)showLineInPDF:(id)sender;     // Option+Cmd+J LaTeX: the caret's line on the page
@property(nonatomic, readonly) BOOL canShowLineInPDF;
- (void)togglePlayback:(id)sender;     // Shift+Cmd+Space play or pause video/audio
@property(nonatomic, readonly) BOOL canTogglePlayback;
@end

// EditorController+Settings.mm
@interface EditorController (Settings)
- (void)openSettings:(id)sender;       // Cmd+, open the settings file
- (void)openKeyboardShortcuts:(id)sender;   // keys.conf, written the first time
// View > Adjust Settings > (part): the settings file at that part's keys. The
// sender's representedObject is a surface id (Settings::appearanceSurfaces).
- (void)adjustAppearance:(id)sender;
- (void)showSettingsForSurface:(NSString *)surface;
@end

// EditorController+Format.mm
@interface EditorController (Format)
- (void)formatDocument:(id)sender;     // Shift+Option+F run the file's formatter
@end

// EditorController+EditCommands.mm
@interface EditorController (EditCommands)
- (void)indentLines:(id)sender;         // Cmd+] the lines in one level
- (void)outdentLines:(id)sender;        // Cmd+[ and out
- (void)insertLineBelow:(id)sender;     // Cmd+Return
- (void)insertLineAbove:(id)sender;
- (void)toggleBold:(id)sender;          // Markdown ** around the selection or word
- (void)toggleItalic:(id)sender;        // Markdown *
- (void)runSelectionInTerminal:(id)sender;   // Shift+Return in Python: to a REPL
- (void)saveFileAs:(id)sender;
- (BOOL)saveActiveTabAs:(NSString *)path;    // Save As without the panel
- (void)saveAllFiles:(id)sender;        // Option+Cmd+S
- (NSString *)keymapLanguage;           // "markdown", "python" or nil (Keymap.h)
@end

// EditorController+EditorView.mm
@interface EditorController (EditorView)
- (void)toggleComment:(id)sender;      // Cmd+/ comment or uncomment lines
- (void)toggleTask:(id)sender;         // Option+Cmd+L Markdown task on the lines
- (void)sortLines:(id)sender;          // F9 sort lines, or the keep-sorted block
- (void)fixKeepSorted:(id)sender;      // Shift+F9 sort every keep-sorted block
- (void)goToBracket:(id)sender;        // Shift+Cmd+\ to the matching bracket
@end

// EditorController+Navigation.mm
@interface EditorController (Navigation)
// For the palette (PalettePanel.h): a 1-based line and column (0: the line's
// start) of the file on show, whether it has lines, and the tabs' files,
// most recently shown first.
- (void)goToLine:(int)line column:(int)column;
@property(nonatomic, readonly) BOOL canGoToLine;
// Go to Symbol in File ("@", Shift+Cmd+O) and in Workspace ("#", Cmd+T):
// what the file on show and the window's language servers offer, and how
// the editor shows a symbol while the list moves.
- (void)addSymbolsToPaletteContext:(MCPaletteContext *)context;
- (void)navigateBack:(id)sender;       // Ctrl+-: the place before the last jump
- (void)navigateForward:(id)sender;    // Ctrl+Shift+-
@end

// EditorController+LanguageServers.mm
@interface EditorController (LanguageServers)
- (void)triggerCompletion:(id)sender;  // Ctrl+Space language server completion
- (void)goToDefinition:(id)sender;     // F12 (and Cmd+click)
- (void)showHoverInfo:(id)sender;      // Option+Cmd+I type and docs at the cursor
- (void)renameSymbol:(id)sender;       // F2: the name at the cursor, or the tree's file
- (void)findReferences:(id)sender;     // Shift+F12
- (void)showQuickFixes:(id)sender;     // Cmd+.
- (void)organizeImports:(id)sender;    // Shift+Option+O
- (void)nextProblem:(id)sender;        // F8: the next problem in the file
- (void)previousProblem:(id)sender;    // Shift+F8
- (void)toggleProblems:(id)sender;     // Shift+Cmd+M every problem, above the terminal
- (void)showLanguageServers:(id)sender;     // the servers popover (status bar click)
- (void)restartLanguageServer:(id)sender;   // the open file's server
- (void)selectPythonInterpreter:(id)sender; // the Python server's interpreter
@end

// EditorController+Agents.mm
@interface EditorController (Agents)
// runningTerminalCommands, leaving out Claude's terminal from Request a
// Change, which a restart into the new build resumes instead of ending.
- (NSArray<NSString *> *)runningTerminalCommandsExceptChange;
// The source folder of a Request a Change conversation still running in
// this window, which a restart continues; nil when there is none.
- (NSString *)changeSourceToResume;
// Shift+Cmd+K: Request a Change to minicode (see ChangePanel.h).
- (void)requestChange:(id)sender;
// After a restart: the conversation in `source` continued in a terminal here.
- (void)resumeChangeWithSource:(NSString *)source command:(NSString *)command;
- (void)inlineAgent:(id)sender;        // Cmd+I the agent, for whatever has the keyboard

// ---- the control channel (ControlCommands.mm) and agent notes (AgentNotes.h)
// None of these moves the keyboard; they only add tabs and terminals.
@property(nonatomic, readonly) MCAgentNotes *agentNotes;   // made on first use
// Opens a file as `minicode open` does: in its tab when open already, else
// in the window's agent preview tab (replacing the file it showed) or a tab
// of its own. YES when it is on show.
- (BOOL)openFileForAgent:(NSString *)path preview:(BOOL)preview;
// The same for an agent's command, returning the file's tab: with `quiet`,
// while the user is typing in the editor (userIsInDocument), the tab opens
// beside theirs without being shown, so their next keystroke still goes
// into their own file. nil when it could not be opened.
- (id)openTabForAgent:(NSString *)path preview:(BOOL)preview quiet:(BOOL)quiet;
- (BOOL)agentTabShown:(id)tab;
// The window is key and the keyboard is in the document on show.
- (BOOL)userIsInDocument;
// A side-by-side diff in the window's diff preview tab (its own, beside
// the note preview tab), or a tab of its own.
- (void)showComparison:(MCComparison *)comparison preview:(BOOL)preview;
// The same, beside the user's tab while they type (`quiet`); YES when shown.
- (BOOL)showComparison:(MCComparison *)comparison preview:(BOOL)preview quiet:(BOOL)quiet;
// A new terminal starting in `dir`: a new tab, or beside the active terminal
// (nil when there is no room to split).
- (TerminalView *)newTerminalForAgentIn:(NSString *)dir beside:(BOOL)beside;
// The browser pane for `minicode browse` and `page`, made and shown beside
// the editor if need be, the keyboard left where it was; or nil-able as it
// is. Agents load into its agent page (Browser.h), never the user's.
- (BrowserView *)agentBrowserPane;
- (BrowserView *)browserPane;
// A web address from a terminal (a Cmd+click on a link, or a program
// through BROWSER and `minicode open-url`): the user's page loads it, the
// pane shows beside the editor if it was hidden, and the keyboard stays
// where it was. 0.0.0.0 and [::] are read as localhost
// (TermLinks::browserAddress).
- (void)openWebAddressFromTerminal:(NSString *)url;
// For `minicode state`: each tab (path, title, kind, dirty, active, preview),
// the editor's selection (path, text up to 64 KB, start and end line and
// column; nil when no text file shows), the terminals, the browser's page.
- (NSArray<NSDictionary *> *)controlTabs;
- (NSDictionary *)controlSelection;
- (NSArray<TerminalView *> *)controlTerminals;
- (NSDictionary *)controlBrowser;
// ---- the Claude Code IDE link (IdeLinkServer.h)
// The text of `path` when it is the document on show as source (its
// buffer, unsaved changes included), else nil.
- (NSString *)agentBufferOfShownFile:(NSString *)path;
// Closes the tabs showing a comparison `match` accepts, unasked (a
// comparison has nothing to save); returns how many.
- (NSUInteger)closeComparisonTabsMatching:(BOOL (^)(MCComparison *comparison))match;
// Every problem the window's language servers report (Lsp.h, MCProblem).
- (NSArray *)agentProblems;
- (void)showAgentStatusNote:(NSString *)note;   // the status bar's left, for six seconds
// Whether `view` is the editor's text view.
- (BOOL)isEditorTextView:(id)view;
- (void)nextTourStop:(id)sender;       // Option+]
- (void)previousTourStop:(id)sender;   // Option+[
@property(nonatomic, readonly) BOOL tourActive;
@end

// EditorController+Markdown.mm
@interface EditorController (Markdown)
@property(nonatomic, readonly) BOOL canTogglePreview;
- (void)togglePreview:(id)sender;     // Markdown: source <-> rendered
@end
