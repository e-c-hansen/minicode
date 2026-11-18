// AgentTaskBar.h — Objective-C++. New Agent Task (Edit > New Agent Task…,
// Option+Shift+Cmd+A): a Claude Code session on a fresh git worktree in one
// step. A bar across the top of the right pane, as Request a Change's is,
// takes a prompt (a box that wraps and grows; Return starts, Shift or
// Option+Return adds a line, Esc closes) and a short name made from it,
// which the user may change; the name becomes the branch and the folder.
//
// Return adds a worktree from HEAD at <folder>/.claude/worktrees/<name> on a
// new branch <name> (git worktree add, never a shell), where Claude Code's
// own worktrees go and where the Agents panel, the tree's Worktrees node,
// Compare with Main and Apply to Main already look; then a terminal tab
// runs `cd <worktree> && <agent.command> <prompt>` (AgentTask::startCommand:
// the prompt quoted on the line, or read from a file when it is long or has
// several lines), and the task is listed in the Agents panel
// (MCAgentModel addTask:) until Claude Code's session for it shows up there.
//
// What it says no to, in the bar's line: a folder outside a git repository,
// a repository with no commit yet, the agent's command not installed (with
// where to get Claude Code), a worktree that would land in ~/.claude, and
// git's own refusal. Uncommitted changes are allowed and said to stay
// behind. A name already taken, as a branch or a folder, gets -2, -3, ...
// The pure parts are the core's AgentTask.h.
#import <Cocoa/Cocoa.h>

@class MCAgentModel, MCAgentTask, TerminalView;

// What the bar needs from its window (EditorController, through
// MCAgentWindow).
@protocol MCAgentTaskHost <NSObject>
@property(nonatomic, readonly, strong) NSWindow *window;
@property(nonatomic, readonly, copy) NSString *rootPath;
// A terminal in a new tab of the dock (opening it) running `command`, with
// the keyboard there (ChangePanel.h's).
- (TerminalView *)changeTerminalRunning:(NSString *)command focus:(BOOL)focus;
- (void)showStatusNote:(NSString *)note;
// The bar was shown, hidden or grew: lay the right pane out again.
- (void)agentTaskBarDidChange;
@end

@interface MCAgentTaskBar : NSView <NSTextViewDelegate, NSTextFieldDelegate>
@property(nonatomic, readonly) NSTextView *textView;     // the prompt
@property(nonatomic, readonly) NSTextField *nameField;   // the name, made from the prompt
@property(nonatomic, readonly) NSButton *startButton;
@property(nonatomic, copy) NSString *hint;               // the line beside the heading
@property(nonatomic) BOOL hintIsProblem;                 // drawn in the error color
@property(nonatomic, readonly) BOOL nameEdited;          // the user typed a name of their own
@property(nonatomic, copy) void (^onStart)(void);
@property(nonatomic, copy) void (^onClose)(void);
@property(nonatomic, copy) void (^onResize)(void);
- (void)setBackground:(NSColor *)background text:(NSColor *)text field:(NSColor *)field;
- (CGFloat)heightForWidth:(CGFloat)width;
- (void)clear;
@end

@interface MCAgentTaskSession : NSObject
- (instancetype)initWithHost:(id<MCAgentTaskHost>)host model:(MCAgentModel *)model;
@property(nonatomic, readonly) MCAgentTaskBar *bar;
@property(nonatomic, readonly) BOOL barShown;
// The bar, its prompt focused, and a look at the repository (its branch,
// its uncommitted changes) for the line beside the heading.
- (void)begin;
// Return: checks, the worktree, the terminal, the panel's row.
- (void)start;
- (void)close;
// The pane's top for the bar; returns what is left below it.
- (NSRect)layoutIn:(NSRect)area;

// For the GUI checks.
@property(nonatomic, readonly) BOOL busy;                     // git is making the worktree
@property(nonatomic, readonly) BOOL repositoryChecked;        // begin's look has answered
@property(nonatomic, readonly, strong) MCAgentTask *lastTask; // the last one started
@property(nonatomic, readonly, copy) NSString *lastProblem;   // the last refusal, or nil
@property(nonatomic, copy) void (^onFinished)(void);          // after each start, either way
@end
