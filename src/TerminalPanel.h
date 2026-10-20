// TerminalPanel.h — the terminal dock: several terminals, as tabs and as
// panes side by side within a tab.
//
// A tab strip runs across the top, the editor's MCTabStrip (each tab titled
// with its running command or its folder, with a × to close it), then + for
// a new tab and ◫ to split; the shown tab's terminals share the space below in equal
// widths, the active one marked by an accent line along its bottom. Every
// terminal is a TerminalView with its own shell on its own pty;
// tabs that are not shown keep running and keep their output. The active
// terminal is the one the keyboard was last in, in the shown tab.
#import <Cocoa/Cocoa.h>
#import "Terminal.h"

@interface MCTerminalPanel : NSView
- (instancetype)initWithDirectory:(NSString *)dir;   // with one terminal

@property(nonatomic, copy) void (^onOpenLink)(MCTermLink *link);
// The last terminal was closed. The panel is empty and should go.
@property(nonatomic, copy) void (^onEmpty)(void);
// A passing note for the status bar (a queued command dropped by a close).
@property(nonatomic, copy) void (^onNote)(NSString *note);
@property(nonatomic, copy) NSString *projectRoot;     // passed to every terminal

@property(nonatomic, readonly) TerminalView *activeTerminal;
@property(nonatomic, readonly) NSArray<TerminalView *> *terminals;   // tab by tab
@property(nonatomic, readonly) NSUInteger tabCount;
@property(nonatomic, readonly) NSInteger selectedTab;
// The terminals of one tab, left to right.
- (NSArray<TerminalView *> *)terminalsInTab:(NSUInteger)tab;

- (TerminalView *)addTab;         // a new terminal in a new tab, made active
- (TerminalView *)splitActive;    // a new terminal beside the active one
- (void)selectTab:(NSUInteger)tab;
// Next (+1) or previous (-1) terminal: through the shown tab's panes, then on
// to the next tab, wrapping around.
- (void)moveActive:(NSInteger)delta;
// Close a terminal, or a tab's terminals. A command still running in one,
// waiting to start, or a background job (see TerminalView runningCommands)
// is asked about first; NO when the user kept it.
- (BOOL)closeTerminal:(TerminalView *)terminal;
- (BOOL)closeTab:(NSUInteger)tab;
// Every terminal, unasked (the window is closing): shells hung up and reaped.
- (void)closeAll;
- (void)focusActive;
- (BOOL)hasFocus;                 // the keyboard is in one of the terminals
// The window's folder changed: every terminal at a prompt moves there.
- (void)setDirectory:(NSString *)dir;
@end

// The commands running in these terminals, for a question before they close.
NSArray<NSString *> *MCRunningCommands(NSArray<TerminalView *> *terminals);
// The question ("Close the terminal?"), then "“vim” is still running." (or a
// list), with `button` (Return) and Cancel (Esc). YES to go ahead, and YES at
// once when nothing is running.
BOOL MCConfirmClosingRunning(NSArray<NSString *> *commands, NSString *question,
                             NSString *button);
