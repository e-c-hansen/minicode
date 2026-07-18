// Terminal.h — a lightweight command runner (not a full VT100 emulator).
// Runs each command via zsh, tracks the working directory, and shows output.
#import <Cocoa/Cocoa.h>

@interface TerminalView : NSView
- (instancetype)initWithDirectory:(NSString *)dir;
- (void)setDirectory:(NSString *)dir;   // sync cwd when the open folder changes
- (void)focusInput;
@end
