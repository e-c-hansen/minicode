// AgentsPanel.h — Objective-C++. The Agents panel (Shift+Cmd+A): the agent
// sessions that worked in the window's folder, in the sidebar's place, the
// way Source Control is (R2.5-R2.7).
//
// A row per session, newest activity first, with its subagents nested under
// it: its description, whether it is running, where it worked (its
// worktree's branch), its age, and how many files it wrote or edited and
// commands it ran. Expanding a row shows those files in three groups (in
// the folder, in its worktree, outside the folder); a file no longer on
// disk is struck through. The model is MCAgentModel; this file only draws.
//
// Keys, as in the git panel: Up and Down move, Return shows a session's
// Evidence or opens a file, Space or Right expands (Left collapses), Tab
// goes to the editor. A click on a session shows its Evidence, on a file
// opens it.
#import <Cocoa/Cocoa.h>

@class MCAgentModel;

@interface MCAgentsPanel : NSView
- (instancetype)initWithModel:(MCAgentModel *)model;
@property(nonatomic, readonly, strong) MCAgentModel *model;
@property(nonatomic, copy) void (^onShowEvidence)(NSString *transcript, NSString *name);
@property(nonatomic, copy) void (^onOpenFile)(NSString *path);
@property(nonatomic, copy) void (^onOpenFolderInNewWindow)(NSString *path);
@property(nonatomic, copy) void (^onTab)(void);
- (void)applySettings;
- (void)focusList;
// The rows again (the model calls this through its notification; the
// running state and ages also move on a 30 s timer).
- (void)reload;

// For the GUI checks.
@property(nonatomic, readonly) NSOutlineView *outline;
@property(nonatomic, readonly, copy) NSString *noticeText;    // the empty state, "" with rows
@property(nonatomic, readonly, copy) NSString *summaryText;   // "1 running · 2 sessions"
// One line per row on show: "S <depth> <state> <name> | <detail>",
// "G <title>", "F <path>" (with " (missing)" when it is gone).
- (NSArray<NSString *> *)rowDescriptions;
- (void)activateRow:(NSInteger)row;   // Return
- (void)toggleRow:(NSInteger)row;     // Space
- (NSMenu *)menuForRow:(NSInteger)row;
@end
