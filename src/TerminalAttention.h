// TerminalAttention.h — Objective-C++. "Waiting for you" on the right
// terminal tab (spec feature 7), for the whole app: the board of marks the
// control channel's `notify` command sets (AttentionBoard.h does the
// deciding), and what shows it:
//   - an orange dot on a waiting terminal's tab, its message as the tab's
//     tooltip, and a green dot on one that is done (MCTerminalPanel draws
//     them, asking here);
//   - the Dock icon's badge, the number of waiting terminals in every window;
//   - Terminal > Next Waiting Terminal (Ctrl+Cmd+W), which brings the next
//     one forward, its window, its tab and the keyboard.
// Typing in a terminal clears its mark, and a closed terminal is forgotten
// (Terminal.h's notifications). Which Claude Code session runs in which
// terminal is kept too (R7.4), for the Agents panel's Show Terminal and for
// the session's own roots when a path in the output is clicked.
#import <Cocoa/Cocoa.h>
#include "Json.h"

@class TerminalView;

// Posted (object: the TerminalView, or nil) whenever a mark, its message or
// the waiting count changed.
extern NSNotificationName const MCTerminalAttentionDidChangeNotification;

typedef NS_ENUM(NSInteger, MCAttentionMark) {
    MCAttentionNone = 0,
    MCAttentionWaiting,
    MCAttentionDone,
};

@interface MCTerminalAttention : NSObject
+ (instancetype)shared;

// A `notify` request's arguments (ControlProtocol.h's table: terminal,
// state, message, event, session_id, transcript_path, cwd) for `terminal`.
// NO with *error for a state that is not waiting, done or clear.
- (BOOL)applyNotify:(const Json &)args toTerminal:(TerminalView *)terminal
              error:(NSString **)error;

- (MCAttentionMark)markForTerminal:(TerminalView *)terminal;
// "waiting", "done", or nil.
- (NSString *)markNameForTerminal:(TerminalView *)terminal;
- (NSString *)messageForTerminal:(TerminalView *)terminal;   // a waiting one's, else nil
@property(nonatomic, readonly) NSInteger waitingCount;

// The session last seen in a terminal (nil when none was).
- (NSString *)sessionIdForTerminal:(TerminalView *)terminal;
- (NSString *)transcriptForTerminal:(TerminalView *)terminal;
- (NSString *)sessionCwdForTerminal:(TerminalView *)terminal;
// The live terminal a session runs in, by its id or else its transcript.
- (TerminalView *)terminalForSession:(NSString *)sessionId transcript:(NSString *)transcript;
@end

// The next waiting terminal after the one the keyboard is in, across every
// window (window by window, tab by tab), and nil when none waits.
TerminalView *MCNextWaitingTerminal(void);
// Brings `terminal` forward: its window, its tab (the dock shown if hidden)
// and the keyboard. NO when no window has it.
BOOL MCRevealTerminal(TerminalView *terminal);
// Ctrl+Cmd+W: MCRevealTerminal(MCNextWaitingTerminal()), or a beep and a
// status note when nothing is waiting.
void MCJumpToNextWaitingTerminal(void);
