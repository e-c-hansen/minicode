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
// Above the sessions, a row for each task New Agent Task started whose
// session has not shown up yet (AgentTaskBar.h): its name, its branch and
// how long ago; a click shows its terminal. A session (or task) that works
// in a worktree offers Compare with Main… in its menu, the review with
// Apply to Main.
//
// Keys, as in the git panel: Up and Down move, Return shows a session's
// Evidence or what it changed in a file, Space or Right expands (Left
// collapses), Tab goes to the editor. A click on a session shows its
// Evidence, on a file what the session changed in it.
#import <Cocoa/Cocoa.h>

@class MCAgentModel;

@interface MCAgentsPanel : NSView
- (instancetype)initWithModel:(MCAgentModel *)model;
@property(nonatomic, readonly, strong) MCAgentModel *model;
@property(nonatomic, copy) void (^onShowEvidence)(NSString *transcript, NSString *name);
@property(nonatomic, copy) void (^onOpenFile)(NSString *path);
// A click (or Return) on a file under a session: what that session changed
// in it (AgentPages.h's MCSessionFilePage). The row's menu still has Open.
@property(nonatomic, copy) void (^onShowFileChanges)(NSString *path, NSString *transcript,
                                                     NSString *sessionName);
@property(nonatomic, copy) void (^onOpenFolderInNewWindow)(NSString *path);
// Compare with Main on a worktree (MCAgentWindow compareWorktreeAt:).
@property(nonatomic, copy) void (^onCompareWorktree)(NSString *path);
@property(nonatomic, copy) void (^onTab)(void);
- (void)applySettings;
- (void)focusList;
// The rows again (the model calls this through its notification; the
// running state and ages also move on a 30 s timer).
- (void)reload;

// While Claude Code's hooks for the waiting dots are missing, a line under
// the heading offers them (ClaudeHooksOffer.h); hidden once they are in.
@property(nonatomic, readonly) NSButton *hooksButton;

// For the GUI checks.
@property(nonatomic, readonly) NSOutlineView *outline;
@property(nonatomic, readonly, copy) NSString *noticeText;    // the empty state, "" with rows
@property(nonatomic, readonly, copy) NSString *summaryText;   // "1 running · 2 sessions"
// One line per row on show: "T <name> | <detail>" for a task,
// "S <depth> <state> <name> | <detail>",
// "G <title>", "F <path>" (with " (missing)" when it is gone).
- (NSArray<NSString *> *)rowDescriptions;
- (void)activateRow:(NSInteger)row;   // Return
- (void)toggleRow:(NSInteger)row;     // Space
- (NSMenu *)menuForRow:(NSInteger)row;
@end
