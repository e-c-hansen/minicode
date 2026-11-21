// EditorController.h — Cocoa window: file tree (left) + editor/preview (right).
#import <Cocoa/Cocoa.h>
#import "GitGutter.h"   // MCGitGutterHost, which the Git category adopts
#import "TestGutter.h"  // MCTestGutterHost, which the Tests category adopts
#import "EditorLineHistory.h"   // MCLineHistoryHost, which the LineHistory category adopts

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
// `list` NO: the tree is not listed yet (a window over SSH lists through
// its host, from useRemoteSessionLink:).
- (instancetype)initWithRootPath:(NSString *)path listTree:(BOOL)list;
- (void)undo:(id)sender;             // Cmd+Z; the text view handles its own
- (void)redo:(id)sender;              // Shift+Cmd+Z
// Commands still running in this window's terminals (empty when every shell
// is idle), for the question asked before quitting.
- (NSArray<NSString *> *)runningTerminalCommands;
@end

// EditorController+Layout.mm
@interface EditorController (Layout)
- (void)showWindow;
- (void)toggleSidebarVisibility:(id)sender;     // Cmd+B collapse/restore file tree
- (void)toggleSourceControl:(id)sender;   // Shift+Cmd+G / Ctrl+Shift+G: git panel in the sidebar
- (void)toggleAgents:(id)sender;          // Shift+Cmd+A agent sessions in the sidebar
- (void)toggleEditor:(id)sender;      // Shift+Cmd+E hide/restore the file editor
@end

// EditorController+Tests.mm: the ▶ beside each test (TestGutter.h), whose
// host it is, and Run Test at Cursor.
@interface EditorController (Tests) <MCTestGutterHost>
- (MCTestGutter *)ensureTestGutter;   // made with the window
- (void)runTestAtCursor:(id)sender;   // Terminal > Run Test at Cursor
@end

// EditorController+LineHistory.mm: Show Line History (EditorLineHistory.h),
// whose host it is: who last changed each line, a commit or a Claude Code
// session, from the gutter's hover or beside every line.
@interface EditorController (LineHistory) <MCLineHistoryHost>
- (MCLineHistory *)ensureLineHistory;   // made with the window
- (void)toggleLineHistory:(id)sender;   // View > Show Line History
@end

// EditorController+Git.mm: pushing and pulling, staging everything,
// discarding and the branch picker, on the Source Control panel's object
// whether it shows or not (MCGitPanel); the last commit and the stash; and
// the editor's change bars (GitGutter.h), whose host it is.
@class MCGitPanel;
@interface EditorController (Git) <MCGitGutterHost>
- (MCGitPanel *)ensureGitPanel;   // made with the window, for autofetch
// A path as git names it (links resolved: /private/tmp) spelled from the
// window's folder (/tmp), as the tree and the tabs spell it.
- (NSString *)gitPathInRoot:(NSString *)path;
- (void)gitCommitAmend:(id)sender;   // Commit and Amend
- (void)gitUndoCommit:(id)sender;    // Undo Last Commit
- (void)gitStash:(id)sender;         // Stash, with untracked files
- (void)gitStashPop:(id)sender;      // Pop Stash
- (void)nextChange:(id)sender;       // Option+F5: the next change bar
- (void)previousChange:(id)sender;   // Shift+Option+F5
- (void)revertChange:(id)sender;     // the change bar on the caret's line
- (void)stageChange:(id)sender;
- (void)gitReviewChanges:(id)sender;   // every uncommitted change in one tab
// A commit on the commit page (the review page in commit mode): in the
// preview tab, which the next commit shown in preview replaces (the first
// form), or in a kept tab of its own.
- (void)showCommitReview:(NSString *)hash title:(NSString *)title;
- (void)showCommitReview:(NSString *)hash title:(NSString *)title preview:(BOOL)preview;
// The review of every uncommitted change at a file (nil: its top).
- (void)showChangeReviewAt:(NSString *)rel staged:(BOOL)staged preview:(BOOL)preview;
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

// EditorController+Session.mm (SessionLink.h, SessionWindow.h)
@interface EditorController (Session)
// The window is closing, or the app quitting: its documents and terminals
// stay on the session's host for the folder's next window, but those the
// close question's Don't Save dropped (stage 6).
- (void)detachFromSessionHost;
// A window over SSH (stage 8, `mutible ssh://host/path`): before
// showWindow, the window is put on the remote host's link (an
// MCSessionLinkRef whose link is MCSession::forRemote's), so its files are
// read and saved there and its terminals run there.
- (void)useRemoteSessionLink:(id)link;
// Whether the window is on another machine's host, and its "host:path".
@property(nonatomic, readonly) BOOL sessionIsRemote;
@property(nonatomic, readonly) NSString *sessionRemoteLabel;
// The remote link's address (ssh://...), nil for a window on this Mac.
@property(nonatomic, readonly) NSString *sessionRemoteAddress;
// Which host it is on: the machine's ssh:// and the folder there (the
// link's key()), nil for a window on this Mac.
@property(nonatomic, readonly) NSString *sessionRemoteKey;
// Whether the window's host went (a headless one killed, a connection over
// SSH dropped) and Reconnect has not found it again.
@property(nonatomic, readonly) BOOL sessionHostLost;
@end

// EditorController+Remote.mm: a window over SSH's folder through its host.
@class MCPaletteContext;
@interface EditorController (Remote)
// Quick Open lists the host's files; nothing changes for a window on this
// Mac.
- (void)addRemoteFilesToPaletteContext:(MCPaletteContext *)context;
// The file an ssh:// address named, shown (showFile:); `isNew` when it is
// not there yet: its tab opens empty, and its first save makes it there.
- (BOOL)showRemoteFile:(NSString *)path isNew:(BOOL)isNew;
@end

// EditorController+SessionBrowser.mm (SessionBrowser.h)
@class MCSessionBrowser;
@interface EditorController (SessionBrowser)
// View > Show Session in Browser…: the window's tabs on the session's host,
// served read-only on 127.0.0.1 and opened in the default browser; with
// editor.session-host off, an alert offers to turn it on.
- (void)showSessionInBrowser:(id)sender;
// View > Copy Session Browser Link: a fresh link to the same page, good for
// one browser.
- (void)copySessionBrowserLink:(id)sender;
// The window's server, nil until one of the two was chosen (checks).
@property(nonatomic, readonly) MCSessionBrowser *sessionBrowser;
@end

// EditorController+StatusBar.mm
@interface EditorController (StatusBar)
- (void)toggleHints:(id)sender;       // Shift+Cmd+H shortcut-hints overlay
// While the hints show, a key without Command or Control types into their
// search box wherever the keyboard is (the key monitor asks first); YES
// when the box took it.
- (BOOL)hintsPanelTakesKeyDown:(NSEvent *)event;
@end

// EditorController+Panels.mm
@interface EditorController (Panels)
- (void)toggleTerminal:(id)sender;    // Ctrl+` bottom terminal dock
- (void)newTerminal:(id)sender;       // Ctrl+Shift+` a terminal in a new tab
- (void)splitTerminal:(id)sender;     // Cmd+\ a terminal beside the active one
- (void)splitTerminalDown:(id)sender; // a terminal below the active one
- (void)renameTerminal:(id)sender;    // Rename Terminal… the active terminal
- (void)moveTerminalLeft:(id)sender;  // the shown terminal tab one place left
- (void)moveTerminalRight:(id)sender; // and right
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
// View > New Browser Tab, from anywhere: the browser shown with a new,
// empty tab and the keyboard in its address bar. (Cmd+T, a page key, is
// the pane's own newBrowserTab:, so it acts only while the pane has the
// keyboard; elsewhere the key is Go to Symbol in Workspace.)
- (void)openBrowserTab:(id)sender;
- (void)focusTree:(id)sender;         // Cmd+0 move keyboard focus to file tree
- (void)focusEditor:(id)sender;       // move keyboard focus to the editor
- (void)focusTerminal:(id)sender;     // the terminal, opening it if hidden
- (void)focusBrowser:(id)sender;      // Cmd+3 the browser's address bar, likewise
@end

// EditorController+Groups.mm: two editor groups side by side
@interface EditorController (Groups)
- (void)splitEditor:(id)sender;              // Cmd+\ in the editor: the tab in a group beside
- (void)focusFirstEditorGroup:(id)sender;    // Cmd+1 the left group (or the one editor)
- (void)focusSecondEditorGroup:(id)sender;   // Cmd+2 the right group, made if there is none
- (void)moveEditorToNextGroup:(id)sender;    // Ctrl+Cmd+Right
- (void)moveEditorToPreviousGroup:(id)sender;   // Ctrl+Cmd+Left
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
// This window moves to `dir` (mutible -r), asking about unsaved tabs first;
// NO when the user cancelled.
- (BOOL)switchToFolder:(NSString *)dir;
- (void)toggleHiddenFiles:(id)sender; // Cmd+Shift+. show/hide dotfiles, every window
// The switch as last left (the app's defaults), at launch.
+ (void)loadShowHiddenFiles;
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

// EditorController+TreeFocus.mm: Focus on Folder, the tree rooted at one
// folder inside the window's (the selected folder, or the selected file's
// or the active file's folder), with a breadcrumb back up; Add to Focus,
// several folders each a root row of the tree under a row of their labels;
// and back to the whole folder. Only the tree narrows: the window's folder
// is unchanged.
@interface EditorController (TreeFocus)
- (void)focusOnFolder:(id)sender;
- (void)addFolderToFocus:(id)sender;
// Add Folder to Focus…: the folder picker scoped to the window's folder.
- (void)chooseFolderToAddToFocus:(id)sender;
- (void)removeFolderFromFocus:(id)sender;
- (void)showWholeFolderInTree:(id)sender;
// The focus as a path from the window's folder ("mac/src"), several joined
// by ", ", "" for none; the folders themselves, in the order added; and
// setting one (NO when that folder is not in the tree: gone, or hidden
// with hidden files off).
@property(nonatomic, readonly) NSString *treeFocus;
@property(nonatomic, readonly) NSArray<NSString *> *treeFocusFolders;
- (BOOL)focusTreeOn:(NSString *)focus;
// The set's rules (TreeFocus::added, removed): NO when a folder named is
// not in the tree.
- (BOOL)addTreeFocus:(NSString *)folder;
- (BOOL)removeTreeFocus:(NSString *)folder;
- (BOOL)setTreeFocusFolders:(NSArray<NSString *> *)folders;
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
// `closing`: the window is about to close (or the app to quit) and detach
// from the session's host. Don't Save then names the tabs to close there
// (the rest stay on the host), and a headless host adds a fourth answer,
// Keep Unsaved, which leaves the changes unsaved there for the folder's
// next window (stage 6).
- (BOOL)confirmCloseAllTabsClosing:(BOOL)closing;
@property(nonatomic, readonly) BOOL hasUnsavedChanges;
- (NSArray<NSString *> *)recentTabPaths;
// Quick Open's file: in a kept tab, or in the preview tab with
// workbench.enable-preview-from-quick-open; selected in the tree as a click
// there would.
- (void)openFileFromQuickOpen:(NSString *)path;
// Keep Open: the tab on show, if it is a preview tab, becomes an ordinary one.
- (void)keepActiveTabOpen:(id)sender;
// The keyboard to what the tab on show shows (its text, PDF, player...).
// A file opened from Finder, the Dock or the command line gets it.
- (void)focusActiveDocument;
@end

// EditorController+Persistence.mm
@interface EditorController (Persistence)
// Quitting (hot exit, files.hot-exit = on-exit) and Request a Change's
// restart: every unsaved tab's backup written now, on this thread, for the
// next launch to bring back, so they need no question and are kept as the
// app terminates. NO when that could not be done (backups off, a tab that
// cannot be backed up, a write that failed), and the question is asked.
- (BOOL)keepUnsavedTabsForNextLaunch;
// The quit was cancelled (a question's Cancel): what the window kept for
// the next launch is unsaved work again, whose backups go as usual.
- (void)quitWasCancelled;
// The window the last run left on a folder opened by name, encoded for
// restoreWindowRecord:; nil for none (TabSessions::toReopen).
+ (NSString *)windowRecordToReopenFolder:(NSString *)root;
// Writes this window's session (tabs, browser) now; quitting calls it.
- (void)saveSession;
// Every window as it is, for the next launch that is asked to open nothing
// (VS Code's window.restoreWindows "all"): frames, folders, tabs with their
// carets and scroll, both editor groups, the panels and their sizes, the
// terminals' folders and splits, the tree's open folders and selected row
// (TabSessions::Window). Written as mutible quits, as a window closes (the
// last one is kept, since closing it quits), and a moment after a tab
// switch, so a crash loses little. Another mutible running at once keeps
// its own windows in the record (SessionStore's MCSessionsPutWindows).
+ (void)saveOpenWindows;
// The windows saveOpenWindows writes down: the app's open windows on a
// folder, the one in front first.
+ (NSArray<EditorController *> *)openWindowsFrontFirst;
// The windows to bring back, front first: @{@"root": folder, @"record":
// what restoreWindowRecord: takes}, for folders still there, but those
// another running mutible holds open. None for the demos and the
// benchmark, which start the same way every time.
+ (NSArray<NSDictionary *> *)windowsToRestore;
// Before showWindow: the window comes back as `record` says, its tabs from
// it rather than from the folder's session.
- (void)restoreWindowRecord:(NSString *)record;
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
- (void)navigateBack:(id)sender;     // Ctrl+-: the place before the last jump
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
// Ask Again About This Folder's Tools: forget the folder's answer about
// running its own .venv and node_modules programs; enabled while it has one.
- (void)forgetFolderToolsAnswer:(id)sender;
@property(nonatomic, readonly) BOOL canForgetFolderToolsAnswer;
@end

// EditorController+Bookmarks.mm: web bookmarks as .url files (Bookmarks.h).
@interface EditorController (Bookmarks)
// The window on the bookmark folder (browser.bookmarks, ~/Bookmarks), where
// `mutible https://...` and bookmarks opened from Finder go.
@property(nonatomic, readonly) BOOL isBrowserWindow;
// Each http or https address in a browser tab of this window's pane (shown,
// in the browser window over the editor's side while no file is open);
// `focus` gives the first page the keyboard.
- (void)showWebAddresses:(NSArray<NSString *> *)urls focusPage:(BOOL)focus;
// File > Save Page as Bookmark (Cmd+D in the page): the page on show as a
// .url file in this workspace's .mutible/bookmarks, made on the first save
// (in the browser window, the tree's selected folder), selected in the
// tree when it shows it. A page bookmarked here or in the shared folder
// is not saved again.
- (void)saveBrowserBookmark:(id)sender;
@property(nonatomic, readonly) BOOL canSaveBrowserBookmark;
// The browser toolbar's star: saves the page on show, or moves its
// bookmark (this workspace's, else the shared one) to the Trash.
- (void)toggleBrowserBookmark:(id)sender;
// The menu bar's Bookmarks menu (Bookmarks.h): a bookmark loads in this
// window's browser, a folder's Open All in Tabs opens each in a tab.
- (void)openBookmarkFromMenu:(id)sender;
- (void)openBookmarkFolderFromMenu:(id)sender;
// Shared > Move to This Workspace: the shared bookmark (the sender's
// representedObject) moved into this workspace's .mutible/bookmarks.
- (void)moveBookmarkToWorkspace:(id)sender;
// File > Save All Tabs as Bookmarks: a .url file per tab in a new folder,
// "Tabs 2026-11-12 14.03".
- (void)saveBrowserTabsAsBookmarks:(id)sender;
@property(nonatomic, readonly) BOOL canSaveBrowserTabs;
// The tree's Open All in Browser Tabs (a folder holding bookmarks) and
// Open as Text (a bookmark file, to mend its address).
- (void)openBookmarksInTabs:(id)sender;
@property(nonatomic, readonly) BOOL canOpenBookmarksInTabs;
- (void)openBookmarkAsText:(id)sender;
@property(nonatomic, readonly) BOOL canOpenBookmarkAsText;
@end

// EditorController+Agents.mm
@interface EditorController (Agents)
// runningTerminalCommands, leaving out Claude's terminal from Request a
// Change, which a restart into the new build resumes instead of ending.
- (NSArray<NSString *> *)runningTerminalCommandsExceptChange;
// The source folder of a Request a Change conversation still running in
// this window, which a restart continues; nil when there is none.
- (NSString *)changeSourceToResume;
// Shift+Cmd+K: Request a Change to mutible (see ChangePanel.h).
- (void)requestChange:(id)sender;
// Edit > New Agent Task… (Option+Shift+Cmd+A): Claude Code on a fresh
// worktree, from a prompt (AgentTaskBar.h).
- (void)newAgentTask:(id)sender;
// After a restart: the conversation in `source` continued in a terminal here.
- (void)resumeChangeWithSource:(NSString *)source command:(NSString *)command;
- (void)inlineAgent:(id)sender;        // Cmd+I the agent, for whatever has the keyboard

// ---- the control channel (ControlCommands.mm) and agent notes (AgentNotes.h)
// None of these moves the keyboard; they only add tabs and terminals.
@property(nonatomic, readonly) MCAgentNotes *agentNotes;   // made on first use
// Opens a file as `mutible open` does: in its tab when open already, else
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
// The browser pane for `mutible browse` and `page`, made and shown beside
// the editor if need be, the keyboard left where it was; or nil-able as it
// is. Agents load into its agent page (Browser.h), never the user's.
- (BrowserView *)agentBrowserPane;
- (BrowserView *)browserPane;
// A web address from a terminal (a Cmd+click on a link, or a program
// through BROWSER and `mutible open-url`): the user's page loads it, the
// pane shows beside the editor if it was hidden, and the keyboard stays
// where it was. 0.0.0.0 and [::] are read as localhost
// (TermLinks::browserAddress).
- (void)openWebAddressFromTerminal:(NSString *)url;
// For `mutible state`: each tab (path, title, kind, dirty, active, preview),
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
// Control+Command+V: the Markdown tab's page beside its source, or not.
@property(nonatomic, readonly) BOOL canToggleSidePreview;
- (void)toggleSidePreview:(id)sender;
// The page beside the source is on show for the tab on show.
- (BOOL)sidePreviewShowing;
// File > Export as HTML…: the Markdown tab's text as a standalone page.
@property(nonatomic, readonly) BOOL canExportMarkdownHtml;
- (void)exportMarkdownHtml:(id)sender;
- (BOOL)writeMarkdownHtmlTo:(NSString *)path;
// A page edit still open with a change in it goes into its document (one
// undo step), as Apply would: before a save, a close or a quit asks about
// unsaved changes, so what was typed there is never dropped unasked.
- (void)commitPageEdit;
@end
// Where Export as HTML… writes, given the suggested path (the file's own,
// as .html): nil asks with a save panel. The GUI checks answer here.
extern NSString * (^MCHtmlExportPathChooser)(NSString *suggested);
