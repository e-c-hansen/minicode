// EditorController.h — Cocoa window: file tree (left) + editor/preview (right).
#import <Cocoa/Cocoa.h>

@interface EditorController : NSObject <NSOutlineViewDataSource,
                                        NSOutlineViewDelegate,
                                        NSSplitViewDelegate,
                                        NSTextViewDelegate,
                                        NSWindowDelegate,
                                        NSMenuItemValidation>
@property(nonatomic, readonly) BOOL canTogglePreview;
@property(nonatomic, readonly, strong) NSWindow *window;
@property(nonatomic, readonly, copy) NSString *rootPath;
- (instancetype)initWithRootPath:(NSString *)path;
- (void)showWindow;
- (void)revealPath:(NSString *)path andOpen:(BOOL)open;  // select in tree
// Open a file handed in from outside (Finder, the Dock, the command line),
// spelled from rootPath, in its tab (a new one unless it is open already).
- (BOOL)showFile:(NSString *)path;
- (void)openFolder:(id)sender;    // menu action
- (void)saveCurrentFile:(id)sender;   // Cmd+S
- (void)togglePreview:(id)sender;     // Markdown: source <-> rendered
- (void)exportPDF:(id)sender;         // Shift+Cmd+S LaTeX: save the typeset PDF
@property(nonatomic, readonly) BOOL canExportPDF;
- (void)undo:(id)sender;              // Cmd+Z; the text view handles its own
- (void)redo:(id)sender;              // Shift+Cmd+Z
- (void)toggleHints:(id)sender;       // Shift+Cmd+H shortcut-hints overlay
- (void)toggleTerminal:(id)sender;    // Ctrl+` bottom terminal dock
- (void)newTerminal:(id)sender;       // Ctrl+Shift+` a terminal in a new tab
- (void)splitTerminal:(id)sender;     // Cmd+\ a terminal beside the active one
- (void)nextTerminal:(id)sender;      // Option+Cmd+] across panes and tabs
- (void)previousTerminal:(id)sender;  // Option+Cmd+[
- (void)closeTerminal:(id)sender;     // Ctrl+Shift+W the active terminal
// Closes the terminal the keyboard is in and returns YES; NO when the
// keyboard is not in a terminal (for a Cmd+W that closes what has focus).
- (BOOL)closeFocusedTerminal;
- (void)runCurrentFile:(id)sender;    // Shift+Cmd+Return run the file in a terminal
- (void)toggleBrowser:(id)sender;     // Shift+Cmd+B embedded browser
- (void)toggleSidebar:(id)sender;     // Cmd+B collapse/restore file tree
- (void)toggleSourceControl:(id)sender;   // Ctrl+Shift+G git panel in the sidebar
- (void)toggleEditor:(id)sender;      // Shift+Cmd+E hide/restore the file editor
- (void)toggleHiddenFiles:(id)sender; // Cmd+Shift+. show/hide dotfiles
- (void)openSearch:(id)sender;        // Cmd+Shift+F project-wide search
- (void)focusTree:(id)sender;         // Cmd+0 move keyboard focus to file tree
- (void)focusEditor:(id)sender;       // Cmd+1 move keyboard focus to editor
- (void)focusTerminal:(id)sender;     // Cmd+2 the terminal, opening it if hidden
- (void)focusBrowser:(id)sender;      // Cmd+3 the browser's address bar, likewise
- (void)selectNextTab:(id)sender;     // Ctrl+Tab, Shift+Cmd+]
- (void)selectPreviousTab:(id)sender; // Ctrl+Shift+Tab, Shift+Cmd+[
- (void)selectTabNumber:(id)sender;   // Shift+Cmd+1..9, Ctrl+1..9 (tag = n; 9 = last)
- (void)closeFocused:(id)sender;      // Cmd+W the tab, or the focused terminal/browser
// Asks about every tab with unsaved changes (window close, quit). NO when
// the user cancelled or a save failed.
- (BOOL)confirmCloseAllTabs;
@property(nonatomic, readonly) BOOL hasUnsavedChanges;
- (void)newFile:(id)sender;
- (void)newFolder:(id)sender;
- (void)renameSelected:(id)sender;
- (void)deleteSelected:(id)sender;
- (void)refreshTree:(id)sender;
- (void)openSettings:(id)sender;       // Cmd+, open the settings file
- (void)toggleComment:(id)sender;      // Cmd+/ comment or uncomment lines
- (void)toggleTask:(id)sender;         // Cmd+L Markdown task on the lines
- (void)showTodos:(id)sender;          // Shift+Cmd+L TODOs in the folder
- (void)triggerCompletion:(id)sender;  // Ctrl+Space language server completion
- (void)goToDefinition:(id)sender;     // F12 (and Cmd+click)
- (void)showHoverInfo:(id)sender;      // Cmd+I type and docs at the cursor
- (void)togglePlayback:(id)sender;     // Shift+Cmd+Space play or pause video/audio
@property(nonatomic, readonly) BOOL canTogglePlayback;
@end
