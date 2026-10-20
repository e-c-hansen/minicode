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
// The command closing this terminal would end ("npm run dev", "vim"), or nil
// when the shell is idle at its prompt: a command per the shell integration,
// or any program holding the terminal's foreground.
@property(nonatomic, readonly, copy) NSString *runningCommand;
// What a tab shows: the command running, else the folder's name.
@property(nonatomic, readonly, copy) NSString *title;
@property(nonatomic, readonly, copy) NSString *directory;   // the shell's cwd
@property(nonatomic, copy) void (^onTitleChange)(void);
// Types a command as if it were entered in the input line, so it shows and
// goes into the shell's history like any other. Waits for the prompt when the
// shell is still starting (or restarts a shell that exited).
- (void)runCommand:(NSString *)line;
// Ends the shell (SIGHUP, the pty closed) for good. The view is then dead.
- (void)close;
@end
