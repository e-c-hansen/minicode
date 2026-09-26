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
- (void)openFolder:(id)sender;    // menu action
- (void)saveCurrentFile:(id)sender;   // Cmd+S
- (void)togglePreview:(id)sender;     // Markdown: source <-> rendered
- (void)exportPDF:(id)sender;         // Shift+Cmd+S LaTeX: save the typeset PDF
@property(nonatomic, readonly) BOOL canExportPDF;
- (void)undo:(id)sender;              // Cmd+Z; the text view handles its own
- (void)redo:(id)sender;              // Shift+Cmd+Z
- (void)toggleHints:(id)sender;       // Shift+Cmd+H shortcut-hints overlay
- (void)toggleTerminal:(id)sender;    // Ctrl+` bottom terminal dock
- (void)toggleBrowser:(id)sender;     // Shift+Cmd+B embedded browser
- (void)toggleSidebar:(id)sender;     // Cmd+B collapse/restore file tree
- (void)toggleSourceControl:(id)sender;   // Ctrl+Shift+G git panel in the sidebar
- (void)toggleEditor:(id)sender;      // Shift+Cmd+E hide/restore the file editor
- (void)toggleHiddenFiles:(id)sender; // Cmd+Shift+. show/hide dotfiles
- (void)openSearch:(id)sender;        // Cmd+Shift+F project-wide search
- (void)focusTree:(id)sender;         // Cmd+0 move keyboard focus to file tree
- (void)focusEditor:(id)sender;       // Cmd+1 move keyboard focus to editor
- (void)switchToPreviousFile:(id)sender;  // Ctrl+Tab jump to previous file
- (void)newFile:(id)sender;
- (void)newFolder:(id)sender;
- (void)renameSelected:(id)sender;
- (void)deleteSelected:(id)sender;
- (void)refreshTree:(id)sender;
- (void)openSettings:(id)sender;       // Cmd+, open the settings file
- (void)toggleComment:(id)sender;      // Cmd+/ comment or uncomment lines
- (void)triggerCompletion:(id)sender;  // Ctrl+Space language server completion
- (void)goToDefinition:(id)sender;     // F12 (and Cmd+click)
- (void)showHoverInfo:(id)sender;      // Cmd+I type and docs at the cursor
@end
