// TerminalPanel.h — the terminal dock: several terminals, as tabs and as
// panes side by side within a tab.
//
// A thin tab strip runs across the top (each tab titled with its running
// command or its folder, with a × to close it, then + for a new tab and ◫ to
// split), and the shown tab's terminals share the space below in equal
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
- (void)closeTerminal:(TerminalView *)terminal;
- (void)closeTab:(NSUInteger)tab;
- (void)focusActive;
- (BOOL)hasFocus;                 // the keyboard is in one of the terminals
// The window's folder changed: every terminal at a prompt moves there.
- (void)setDirectory:(NSString *)dir;
@end
