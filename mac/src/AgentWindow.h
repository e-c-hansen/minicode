// AgentWindow.h — Objective-C++. What one editor window does with agent work,
// so EditorController only calls in from a few hooks: the Agents panel and
// its model, the Evidence pages, Compare with Main and Open in New Window on
// a worktree, the labels of the Worktrees node in the file tree, and the
// picker or the note for a terminal path found in several places or none
// (R2.10, R2.11).
#import <Cocoa/Cocoa.h>
#import "EditorPage.h"

@class MCAgentModel, MCAgentsPanel, MCAgentWorktree, MCComparison, MCPickerPopup, MCTermLink,
    TerminalView;

// What the window does for it (EditorController).
@protocol MCAgentWorkHost <NSObject>
@property(nonatomic, readonly, strong) NSWindow *window;
@property(nonatomic, readonly, copy) NSString *rootPath;
- (void)openTerminalLink:(MCTermLink *)link;          // a file, at its line
- (void)showPage:(id<MCEditorPage>)page;              // a page in a tab of its own
- (void)showComparison:(MCComparison *)comparison preview:(BOOL)preview;
- (TerminalView *)newTerminalForAgentIn:(NSString *)dir beside:(BOOL)beside;
- (void)showStatusNote:(NSString *)note;
- (void)focusEditor:(id)sender;
- (void)agentWorktreesChanged;                        // the tree's labels again
// The tree's Worktrees node is open, so its ahead and modified marks are on
// show and worth asking git for as files change.
@property(nonatomic, readonly) BOOL agentWorktreesOnShow;
@end

@interface MCAgentWindow : NSObject
- (instancetype)initWithHost:(id<MCAgentWorkHost>)host;
// Made on first use (the panel shown, a worktree in the tree, a link).
@property(nonatomic, readonly, strong) MCAgentModel *model;
@property(nonatomic, readonly, strong) MCAgentsPanel *panel;
@property(nonatomic, readonly) BOOL started;   // the model exists
- (void)setRoot:(NSString *)root;
- (void)windowBecameKey;
// FSEvents in the window's folder: a change under .claude/worktrees or .git
// asks git about the worktrees again.
- (void)folderChangedOnDisk:(NSString *)path;
- (void)stop;

// ---- the Agents panel's and the tree's actions
- (void)showEvidenceForTranscript:(NSString *)transcript name:(NSString *)name;
- (void)compareWorktreeAt:(NSString *)path;          // R2.9 Compare with Main
- (void)openFolderInNewWindow:(NSString *)path;      // R2.9 Open in New Window
- (MCAgentWorktree *)worktreeAtPath:(NSString *)path;   // nil until git answers

// ---- terminal links
// Roots after the shell's and the window's folder (TerminalView extraLinkRoots).
- (NSArray<NSString *> *)linkRoots;
// Where the session running in a terminal worked (R7.4): its scratchpad, its
// worktree and its subagents' worktrees, and its own folder when that is not
// the window's. Looked in first for a path that terminal printed. Empty for
// a terminal no session was seen in.
- (NSArray<NSString *> *)linkRootsForTerminal:(TerminalView *)terminal;
// A file link with no match or several: the note, or the picker (whose
// choice is handed to `open`). NO for a link to open as it is.
- (BOOL)handleLink:(MCTermLink *)link open:(void (^)(MCTermLink *chosen))open;
@property(nonatomic, readonly, strong) MCPickerPopup *picker;   // for the GUI checks
@property(nonatomic, readonly, copy) NSArray<NSString *> *pickerPaths;   // its rows' paths
@property(nonatomic, readonly, copy) NSString *lastNote;        // the last status note
@end
