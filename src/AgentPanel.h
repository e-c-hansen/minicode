// AgentPanel.h — Objective-C++. The inline agent (Cmd+I, VS Code's inline
// chat key): a bar over whatever has the keyboard, which asks Claude Code
// and puts the answer where it belongs. The prompts and the reading of the
// answers are InlineAgent.h's; EditorController builds an MCAgentContext
// for what has the keyboard and lays the bar out.
//
// - Edit (a file in the editor, or a Markdown file's preview): the bar opens
//   under the cursor's line. Return asks; Claude's edit goes into the buffer
//   as one undo step, its lines tinted, and Return keeps it or Esc puts the
//   text back. Another request in the same bar builds on the last.
// - Command (a terminal): the command Claude writes is typed at the prompt,
//   never run.
// - Task (anywhere else): the request, with where it was asked from, goes to
//   Claude Code in a new terminal tab of the open folder.
// Cmd+Return in any of them hands the request over as a task instead.
#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, MCAgentTarget) {
    MCAgentTargetEdit,
    MCAgentTargetCommand,
    MCAgentTargetTask,
};

// What Cmd+I was pressed on, built by EditorController.
@interface MCAgentContext : NSObject
@property(nonatomic) MCAgentTarget target;
@property(nonatomic, copy) NSString *root;       // the open folder, the agent's cwd
@property(nonatomic, copy) NSString *where;      // "the file tree, with src/ selected"
@property(nonatomic, copy) NSString *hint;       // the bar's line beside its heading
@property(nonatomic, copy) NSString *path;       // the open file, as the prompt names it
// Edit: the text now (nil once it is no longer on show), the selection when
// Cmd+I was pressed (UTF-16), putting a new text in as one undo step (NO
// when it could not), and tinting the lines that differ from `original`
// (nil clears the tint).
@property(nonatomic, copy) NSString *(^currentText)(void);
@property(nonatomic) NSRange selection;
@property(nonatomic, copy) NSRange (^currentSelection)(void);   // the editor's now
@property(nonatomic, copy) BOOL (^applyText)(NSString *text, NSString *actionName);
@property(nonatomic, copy) void (^tint)(NSString *original);
// Command: the shell's folder and screen, and typing a command at its prompt
// (NO when the terminal cannot take it now).
@property(nonatomic, copy) NSString *directory;
@property(nonatomic, copy) NSString *screen;
@property(nonatomic, copy) BOOL (^typeCommand)(NSString *command);
// For a task: the selected text and its lines, when there is a selection.
@property(nonatomic, copy) NSString *selectionText;
@property(nonatomic) int firstLine, lastLine;
// Where the bar goes, in the host view's coordinates: the area to stay in,
// and the line to sit under (or over, with anchorAbove, or when there is
// no room under it).
@property(nonatomic, copy) NSRect (^area)(void);
@property(nonatomic, copy) NSRect (^anchor)(void);
@property(nonatomic) BOOL anchorAbove;
// A clip view whose scrolling moves the anchor: the bar follows it.
@property(nonatomic, weak) NSClipView *followScroll;
// The keyboard goes back here when the bar closes.
@property(nonatomic, weak) NSResponder *returnFocus;
// An identity for "the same place": Cmd+I again there focuses the open bar.
@property(nonatomic, weak) id owner;
@end

@protocol MCAgentHost <NSObject>
@property(nonatomic, readonly, strong) NSWindow *window;
// The view the bar is a subview of (the right pane).
- (NSView *)agentBarHost;
// The bar was shown, hidden or resized: place it again.
- (void)agentBarDidChange;
// Claude Code, in a new terminal tab of `root`, running `command` (typed
// into the terminal, so it shows and stays in its history).
- (void)agentRunInTerminal:(NSString *)command;
- (void)showStatusNote:(NSString *)note;
@end

typedef NS_ENUM(NSInteger, MCAgentBarMode) {
    MCAgentBarPrompt,    // the request box
    MCAgentBarWorking,   // waiting for Claude
    MCAgentBarReview,    // an edit was made: Keep or Discard
    MCAgentBarAnswer,    // a text answer, or a problem
};

@interface MCAgentBar : NSView <NSTextViewDelegate>
@property(nonatomic) MCAgentBarMode mode;
@property(nonatomic, readonly) NSTextView *textView;    // the request box
@property(nonatomic, copy) NSString *heading;           // "Agent", beside it the hint
@property(nonatomic, copy) NSString *hint;
@property(nonatomic, copy) NSString *message;           // the status or answer below the box
@property(nonatomic) BOOL messageIsError;
@property(nonatomic, copy) void (^onSend)(NSString *request);
@property(nonatomic, copy) void (^onTask)(NSString *request);   // Cmd+Return
@property(nonatomic, copy) void (^onKeep)(void);
@property(nonatomic, copy) void (^onDiscard)(void);
@property(nonatomic, copy) void (^onStop)(void);
@property(nonatomic, copy) void (^onClose)(void);
@property(nonatomic, copy) void (^onResize)(void);
- (void)setBackground:(NSColor *)background text:(NSColor *)text field:(NSColor *)field;
- (CGFloat)heightForWidth:(CGFloat)width;
- (void)clearRequest;
@end

// Runs agent.command in print mode with a prompt on stdin, in a login shell
// (so the user's PATH finds claude) with minicode's search folders added.
// The completion runs on the main queue, never after cancel.
@interface MCAgentRun : NSObject
+ (instancetype)runPrompt:(NSString *)prompt inFolder:(NSString *)folder
               completion:(void (^)(NSString *output, NSString *error))completion;
- (void)cancel;
@end

// One per window.
@interface MCAgentSession : NSObject
- (instancetype)initWithHost:(id<MCAgentHost>)host;
@property(nonatomic, readonly) MCAgentBar *bar;
@property(nonatomic, readonly) BOOL shown;
@property(nonatomic, readonly) MCAgentContext *context;
// An edit the agent made waits for Keep or Discard (the bar is open on it).
// Auto-save leaves the tab alone meanwhile, so nothing reaches the disk
// before the user has chosen.
@property(nonatomic, readonly) BOOL editPending;
// Cmd+I: the bar for `context`, or the open one focused when it is for the
// same place. Anything else open is closed first (its edit kept).
- (void)beginWithContext:(MCAgentContext *)context;
// The request typed into the bar (also a test hook).
- (void)send:(NSString *)request;
- (void)handOver:(NSString *)request;   // Cmd+Return
- (void)keep;
- (void)discard;
- (void)close;
// The bar's place, in the host view: call from the host's layout.
- (void)layout;
// The tab on show changed: an edit bar for another tab closes, keeping its edit.
- (void)documentChanged;
- (void)applyColors;
@end
