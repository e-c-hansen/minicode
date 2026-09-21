// Terminal.h — a shell panel backed by a real pty.
// zsh runs interactively on a pseudo-terminal, so programs see a tty: Ctrl+C
// interrupts, prompts like sudo's work, and output streams line by line. Shell
// integration marks (OSC 133 / OSC 7) delimit commands and report the cwd.
// Ordinary output is shown as a colored log with a line input; full-screen
// programs (vim, less, htop) get a cell grid that takes keys directly.
#import <Cocoa/Cocoa.h>

@interface TerminalView : NSView
- (instancetype)initWithDirectory:(NSString *)dir;
- (void)setDirectory:(NSString *)dir;   // sync cwd when the open folder changes
- (void)focusInput;                     // the input line, or the grid when shown
@end
