// Terminal.h — a shell panel backed by a real pty.
// zsh runs interactively on a pseudo-terminal, so programs see a tty: Ctrl+C
// interrupts, prompts like sudo's work, and output streams line by line. Shell
// integration marks (OSC 133 / OSC 7) delimit commands and report the cwd.
// In grid mode (the default) the terminal is a cell grid with zsh's own line
// editor, as in Terminal.app. In line mode ordinary output is shown as a
// colored log with a line input, and full-screen programs (vim, less, htop)
// get the cell grid, which takes keys directly.
#import <Cocoa/Cocoa.h>

// One place a path in the output was found (R2.10).
@interface MCTermLinkMatch : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic) NSTimeInterval modified;   // Unix seconds
@property(nonatomic) BOOL isDirectory;
@end

// Something Cmd+click can open in terminal output: a file that exists (with
// the line and column the reference named, 1-based, 0 when absent) or a URL.
// A relative path is looked for under the shell's folder, the window's
// folder, then the roots extraLinkRoots gives (worktrees, scratchpads);
// `matches` has every place it exists, newest first, and `target` the first.
// A path-like reference found nowhere (one with a slash or a line number)
// is a link too, with no matches, `target` as written and `tried` the roots
// looked in, so a click can say where it looked.
@interface MCTermLink : NSObject
@property(nonatomic, copy) NSString *target;   // absolute path, or the URL
@property(nonatomic) BOOL isURL;
@property(nonatomic) BOOL isDirectory;
@property(nonatomic) int line, column;
@property(nonatomic) NSUInteger byteStart, byteLength;   // within the line's UTF-8
@property(nonatomic, copy) NSArray<MCTermLinkMatch *> *matches;
@property(nonatomic, copy) NSArray<NSString *> *tried;
@end

// Posted (object: the TerminalView) when a command typed or run in a
// terminal finishes, per the shell integration's marks. Something it
// installed may now be found (LspSession looks again for a missing server).
extern NSNotificationName const MCTerminalCommandDidFinishNotification;
// Posted (object: the TerminalView) when the user types or pastes into a
// terminal (keys, not the app's own runCommand:), and when one closes for
// good. A "waiting" or "done" mark on its tab goes (TerminalAttention.h).
extern NSNotificationName const MCTerminalUserDidTypeNotification;
extern NSNotificationName const MCTerminalDidCloseNotification;

@interface TerminalView : NSView
// Cmd+click on a link in the output. The window opens files and URLs.
@property(nonatomic, copy) void (^onOpenLink)(MCTermLink *link);
// Where a relative path is looked for when the shell's own directory does
// not have it: the window's folder.
@property(nonatomic, copy) NSString *projectRoot;
// More folders a relative path is looked for in, after the shell's and the
// window's, in order: the worktrees under the folder, then the indexed agent
// sessions' scratchpads (AgentModel's worktreeRoots).
@property(nonatomic, copy) NSArray<NSString *> *(^extraLinkRoots)(void);
// The roots of the agent session running in this terminal (its scratchpad
// and worktrees, R7.4): looked in before the other extra roots, and a
// match under them is taken over other worktrees' copies.
@property(nonatomic, copy) NSArray<NSString *> *(^sessionLinkRoots)(TerminalView *terminal);
- (instancetype)initWithDirectory:(NSString *)dir;
// The id `minicode state` reports for this terminal, fixed for its life and
// never reused in this process. Its shell, and everything run from it, has
// it as MINICODE_TERMINAL_ID, with MINICODE_PID (this process), so a Claude
// Code hook can say which terminal it ran in (`minicode notify --from-hook`).
@property(nonatomic, readonly) NSInteger terminalID;
- (void)setDirectory:(NSString *)dir;   // sync cwd when the open folder changes
- (void)focusInput;                     // the input line, or the grid when shown
- (BOOL)hasFocus;                       // the keyboard is somewhere in this terminal

// zsh is waiting for a command (from the shell integration's marks, which
// count only with this shell's nonce).
@property(nonatomic, readonly) BOOL atPrompt;
// A full-screen program has the screen (vim, less). In line mode, that is
// when the grid shows in place of the log.
@property(nonatomic, readonly) BOOL gridMode;
// This terminal opened in line mode (terminal.mode = line): the log and an
// input line. Otherwise it is a grid terminal, with zsh's own line editor.
@property(nonatomic, readonly) BOOL lineMode;
// A command is running (per the marks, or a program other than the shell
// holds the terminal's foreground), or one is waiting to be typed: not a
// terminal to type another command into. A shell still starting is not busy.
@property(nonatomic, readonly) BOOL busy;
// What closing this terminal would end or drop, for the question asked
// first: the command in the foreground ("npm run dev", "vim": per the shell
// integration, or any program holding the terminal's foreground), a command
// waiting for the first prompt ("python3 a.py (waiting to start)"), and
// background jobs ("sleep 100 (in the background)", or "(stopped)"). Empty
// when the shell is idle with no jobs, or still starting with nothing queued.
@property(nonatomic, readonly, copy) NSArray<NSString *> *runningCommands;
// The shell's process id, 0 while there is none (the Processes panel
// tracks what descends from it).
@property(nonatomic, readonly) pid_t shellPid;
// The first of them, or nil.
@property(nonatomic, readonly, copy) NSString *runningCommand;
// A runCommand: line still waiting for the shell's prompt, or nil.
@property(nonatomic, readonly, copy) NSString *pendingCommand;
// What a tab shows: the running program's own title (OSC 0 or 2), else
// the command running, else the folder's name.
@property(nonatomic, readonly, copy) NSString *title;
// A title given from outside (`minicode term --title`), shown in place of
// the one above; nil for none.
@property(nonatomic, copy) NSString *fixedTitle;
@property(nonatomic, readonly, copy) NSString *directory;   // the shell's cwd
@property(nonatomic, copy) void (^onTitleChange)(void);
// Types a command as if it were entered in the input line, so it shows and
// goes into the shell's history like any other. Waits for the prompt when the
// shell is still starting (or restarts a shell that exited), and whenever
// the shell does not hold the terminal's foreground.
- (void)runCommand:(NSString *)line;
// Puts a command in the input line (grid mode: at zsh's prompt, once it
// shows) and gives it the keyboard, for the user to read and press Return
// (Run in Terminal for an install command). Never runs it. NO when the
// terminal cannot take it now (a program has the screen, or the terminal is
// busy); then nothing changed.
- (BOOL)typeCommand:(NSString *)line;
// Ends the shell (SIGHUP, the pty closed) for good. The view is then dead.
- (void)close;
// The last `maxLines` lines the terminal shows, scrollback included, as
// plain text: what the inline agent is told the terminal holds.
- (NSString *)recentText:(NSUInteger)maxLines;
@end
