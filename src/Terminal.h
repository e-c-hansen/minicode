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
@end
