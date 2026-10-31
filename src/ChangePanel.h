// ChangePanel.h — Objective-C++. Request a Change to minicode (Shift+Cmd+K),
// inside the window: a bar across the top of the right pane takes a request,
// a terminal tab opens in minicode's own source folder running Claude Code
// with it (so the user watches, answers its questions, and approves its
// edits right there; a copy installed without its source clones the running
// release first, in the same terminal), and once Claude has rebuilt MiniCode.app with `make`
// the bar offers to restart into the new build. The restart reopens every
// window's folder (their tabs come back with the folder's session) and the
// conversation, with `claude --continue`, in a terminal of the window it
// was in. The prompt and the commands are ChangeRequest.h's.
#import <Cocoa/Cocoa.h>

@class TerminalView;

// What the session needs from its window (EditorController).
@protocol MCChangeHost <NSObject>
@property(nonatomic, readonly, strong) NSWindow *window;
// A terminal in a new tab of the dock (opening it), running `command`.
// With `focus`, the keyboard goes there, so Claude's questions can be
// answered at once.
- (TerminalView *)changeTerminalRunning:(NSString *)command focus:(BOOL)focus;
// Types `command` into `terminal`, which is idle at its prompt.
- (void)runCommand:(NSString *)command inTerminal:(TerminalView *)terminal focus:(BOOL)focus;
- (BOOL)terminalIsOpen:(TerminalView *)terminal;
- (void)focusChangeTerminal:(TerminalView *)terminal;
// The bar was shown, hidden, or grew: lay the right pane out again.
- (void)changeBarDidChange;
- (void)showStatusNote:(NSString *)note;
// Quit (asking about unsaved files first, as quitting does) and open the
// app at `appPath`, resuming the conversation in `source` in this window.
- (void)restartIntoApp:(NSString *)appPath source:(NSString *)source
               command:(NSString *)command;
@end

// The app delegate: quits, asking first as quitting does, then opens
// `appPath`, which reopens every window's folder and continues the
// conversation in `source` in the window of `controller`.
@protocol MCRestarting <NSObject>
- (void)restartIntoApp:(NSString *)appPath resumingIn:(id)controller
                source:(NSString *)source command:(NSString *)command;
@end

typedef NS_ENUM(NSInteger, MCChangeBarMode) {
    MCChangeBarPrompt,    // a field for the request, Send, ×
    MCChangeBarWorking,   // what Claude is doing, Show Terminal, ×
    MCChangeBarRebuilt,   // the new build is ready, Restart, ×
    MCChangeBarNote,      // a message (a problem, or "restarted"), ×
};

@interface MCChangeBar : NSView <NSTextFieldDelegate>
@property(nonatomic) MCChangeBarMode mode;
@property(nonatomic, copy) NSString *message;   // the label in every mode but Prompt
@property(nonatomic, readonly) NSTextField *field;
@property(nonatomic, readonly) NSButton *actionButton;   // Send, Show Terminal, Restart
@property(nonatomic, copy) void (^onSend)(NSString *request);
@property(nonatomic, copy) void (^onAction)(void);   // Show Terminal or Restart
@property(nonatomic, copy) void (^onClose)(void);
- (void)setBackground:(NSColor *)background text:(NSColor *)text;
@end
extern const CGFloat kMCChangeBarHeight;

// One per window, made on first use.
@interface MCChangeSession : NSObject
- (instancetype)initWithHost:(id<MCChangeHost>)host;
@property(nonatomic, readonly) MCChangeBar *bar;
@property(nonatomic, readonly) BOOL barShown;
@property(nonatomic, readonly, weak) TerminalView *terminal;   // Claude's
@property(nonatomic, readonly, copy) NSString *source;
// Shift+Cmd+K: the prompt, or, while Claude still runs from the last
// request, its terminal.
- (void)begin;
// The request typed into the bar (also a test hook).
- (void)send:(NSString *)request;
// After a restart: Claude's conversation in `source`, continued in a new
// terminal, and the new build watched again.
- (void)resumeWithSource:(NSString *)source command:(NSString *)command;
// Looks at the build now (the timer does it every two seconds).
- (void)checkBuild;
- (void)restart;   // the Restart button
- (void)stop;      // the window is closing
@end

// minicode's source folder: develop.source when set (nil and *problem when
// it is not one), else the checkout this app was built in, else the clone
// made by an earlier request; nil when there is none yet, and the request
// clones the running release into MCChangeCloneFolder() first.
NSString *MCChangeSourceFolder(NSString **problem);
// ~/Library/Application Support/MiniCode/source
NSString *MCChangeCloneFolder(void);

// The restart: what to reopen, written as the app quits, read once by the
// next launch (if it comes within two minutes).
void MCSaveRestartRecord(NSDictionary *record);
NSDictionary *MCTakeRestartRecord(void);
// Opens `appPath` once this process has exited (a detached shell waits for
// it), with `open -n`, so the new build gets a launch like any other.
BOOL MCLaunchAfterExit(NSString *appPath);
