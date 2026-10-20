// Terminal.h — a shell panel backed by a real pty.
// zsh runs interactively on a pseudo-terminal, so programs see a tty: Ctrl+C
// interrupts, prompts like sudo's work, and output streams line by line. Shell
// integration marks (OSC 133 / OSC 7) delimit commands and report the cwd.
// Ordinary output is shown as a colored log with a line input; full-screen
// programs (vim, less, htop) get a cell grid that takes keys directly.
#import <Cocoa/Cocoa.h>

// Something Cmd+click can open in terminal output: a file that exists (with
// the line and column the reference named, 1-based, 0 when absent) or a URL.
@interface MCTermLink : NSObject
@property(nonatomic, copy) NSString *target;   // absolute path, or the URL
@property(nonatomic) BOOL isURL;
@property(nonatomic) BOOL isDirectory;
@property(nonatomic) int line, column;
@property(nonatomic) NSUInteger byteStart, byteLength;   // within the line's UTF-8
@end

// Posted (object: the TerminalView) when a command typed or run in a
// terminal finishes, per the shell integration's marks. Something it
// installed may now be found (LspSession looks again for a missing server).
extern NSNotificationName const MCTerminalCommandDidFinishNotification;

@interface TerminalView : NSView
// Cmd+click on a link in the output. The window opens files and URLs.
@property(nonatomic, copy) void (^onOpenLink)(MCTermLink *link);
// Where a relative path is looked for when the shell's own directory does
// not have it: the window's folder.
@property(nonatomic, copy) NSString *projectRoot;
- (instancetype)initWithDirectory:(NSString *)dir;
- (void)setDirectory:(NSString *)dir;   // sync cwd when the open folder changes
- (void)focusInput;                     // the input line, or the grid when shown
- (BOOL)hasFocus;                       // the keyboard is somewhere in this terminal

// zsh is waiting for a command (from the shell integration's marks).
@property(nonatomic, readonly) BOOL atPrompt;
// A full-screen program has the grid.
@property(nonatomic, readonly) BOOL gridMode;
// A command is running, or one is waiting to be typed: not a terminal to
// type another command into. A shell still starting is not busy.
@property(nonatomic, readonly) BOOL busy;
// What closing this terminal would end or drop, for the question asked
// first: the command in the foreground ("npm run dev", "vim": per the shell
// integration, or any program holding the terminal's foreground), a command
// waiting for the first prompt ("python3 a.py (waiting to start)"), and
// background jobs ("sleep 100 (in the background)", or "(stopped)"). Empty
// when the shell is idle with no jobs, or still starting with nothing queued.
@property(nonatomic, readonly, copy) NSArray<NSString *> *runningCommands;
// The first of them, or nil.
@property(nonatomic, readonly, copy) NSString *runningCommand;
// A runCommand: line still waiting for the shell's prompt, or nil.
@property(nonatomic, readonly, copy) NSString *pendingCommand;
// What a tab shows: the command running, else the folder's name.
@property(nonatomic, readonly, copy) NSString *title;
@property(nonatomic, readonly, copy) NSString *directory;   // the shell's cwd
@property(nonatomic, copy) void (^onTitleChange)(void);
// Types a command as if it were entered in the input line, so it shows and
// goes into the shell's history like any other. Waits for the prompt when the
// shell is still starting (or restarts a shell that exited).
- (void)runCommand:(NSString *)line;
// Puts a command in the input line and gives it the keyboard, for the user
// to read and press Return (Run in Terminal for an install command). Never
// runs it. NO when the terminal cannot take it now (a program has the grid,
// or the terminal is busy); then nothing changed.
- (BOOL)typeCommand:(NSString *)line;
// Ends the shell (SIGHUP, the pty closed) for good. The view is then dead.
- (void)close;
@end
