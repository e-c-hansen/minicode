// Terminal.h — a shell panel backed by a real pty (not a full VT100 emulator).
// zsh runs interactively on a pseudo-terminal, so programs see a tty: Ctrl+C
// interrupts, prompts like sudo's work, and output streams line by line. Shell
// integration marks (OSC 133 / OSC 7) delimit commands and report the cwd.
// Output is shown as a log; cursor movement and colors are not rendered.
#import <Cocoa/Cocoa.h>

@interface TerminalView : NSView
- (instancetype)initWithDirectory:(NSString *)dir;
- (void)setDirectory:(NSString *)dir;   // sync cwd when the open folder changes
- (void)focusInput;
@end
