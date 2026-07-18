// EditorController.h — Cocoa window: file tree (left) + editor/preview (right).
#import <Cocoa/Cocoa.h>

@interface EditorController : NSObject <NSOutlineViewDataSource,
                                        NSOutlineViewDelegate,
                                        NSSplitViewDelegate,
                                        NSTextViewDelegate,
                                        NSWindowDelegate>
@property(nonatomic, readonly) BOOL canTogglePreview;
@property(nonatomic, readonly, strong) NSWindow *window;
@property(nonatomic, readonly, copy) NSString *rootPath;
- (instancetype)initWithRootPath:(NSString *)path;
- (void)showWindow;
- (void)openFolder:(id)sender;    // menu action
- (void)saveCurrentFile:(id)sender;   // Cmd+S
- (void)togglePreview:(id)sender;     // Markdown: source <-> rendered
- (void)toggleHints:(id)sender;       // Ctrl+H shortcut-hints overlay
- (void)toggleTerminal:(id)sender;    // Ctrl+` bottom terminal dock
- (void)toggleBrowser:(id)sender;     // Shift+Cmd+B embedded browser
- (void)focusTree:(id)sender;         // Cmd+0 move keyboard focus to file tree
- (void)focusEditor:(id)sender;       // Cmd+1 move keyboard focus to editor
- (void)switchToPreviousFile:(id)sender;  // Ctrl+Tab jump to previous file
- (void)newFile:(id)sender;
- (void)newFolder:(id)sender;
- (void)renameSelected:(id)sender;
- (void)deleteSelected:(id)sender;
- (void)refreshTree:(id)sender;
@end
