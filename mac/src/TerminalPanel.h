// TerminalPanel.h — the terminal dock: several terminals, as tabs, and as
// panes within a tab: columns side by side, each a stack of panes one above
// the other (one level deep, as VS Code's terminal groups).
//
// A tab strip runs across the top, the editor's MCTabStrip (each tab titled
// with its running command or its folder, or the name the user gave it,
// with a × to close it; tabs drag to a new place, and a right click offers
// Rename, Move Left and Right, the splits and Close), then + for a new tab
// and ◫ to split (Option-click: split down). The shown tab's terminals share
// the space below in the shares TerminalLayout.h keeps, with a DragBar on
// every boundary; the active one is marked by an accent line along its
// bottom. Every terminal is a TerminalView with its own shell on its own
// pty, sized to its pane; tabs that are not shown keep running and keep
// their output. The active terminal is the one the keyboard was last in,
// in the shown tab.
#import <Cocoa/Cocoa.h>
#import "Terminal.h"

@class DragBar;

// Resize Terminal Pane's directions, the menu items' tags.
typedef NS_ENUM(NSInteger, MCPaneDirection) {
    MCPaneLeft = 1,
    MCPaneRight = 2,
    MCPaneUp = 3,
    MCPaneDown = 4,
};

@class MCSessionTermPlace;
@interface MCTerminalPanel : NSView
- (instancetype)initWithDirectory:(NSString *)dir;   // with one terminal

@property(nonatomic, copy) void (^onOpenLink)(MCTermLink *link);
// The last terminal was closed. The panel is empty and should go.
@property(nonatomic, copy) void (^onEmpty)(void);
// A passing note for the status bar (a queued command dropped by a close).
@property(nonatomic, copy) void (^onNote)(NSString *note);
// A bar across the panel under its strip, above the terminals (the offer
// of the waiting dots, ClaudeHooksOffer.h); nil for none. Its frame's
// height is kept; the panes share what is left.
@property(nonatomic, strong) NSView *noticeBar;
@property(nonatomic, copy) NSString *projectRoot;     // passed to every terminal
@property(nonatomic, copy) NSArray<NSString *> *(^extraLinkRoots)(void);   // likewise
@property(nonatomic, copy) NSArray<NSString *> *(^sessionLinkRoots)(TerminalView *);   // likewise

@property(nonatomic, readonly) TerminalView *activeTerminal;
@property(nonatomic, readonly) NSArray<TerminalView *> *terminals;   // tab by tab
@property(nonatomic, readonly) NSUInteger tabCount;
@property(nonatomic, readonly) NSInteger selectedTab;
// The terminals of one tab, column by column, each top to bottom.
- (NSArray<TerminalView *> *)terminalsInTab:(NSUInteger)tab;
// The same as columns, left to right, each a stack top to bottom.
- (NSArray<NSArray<TerminalView *> *> *)columnsInTab:(NSUInteger)tab;

- (TerminalView *)addTab;         // a new terminal in a new tab, made active
// The same, its shell starting in `dir` (nil: the panel's folder).
- (TerminalView *)addTabInDirectory:(NSString *)dir;
// A panel whose first terminal starts in `first` rather than `dir`.
- (instancetype)initWithDirectory:(NSString *)dir firstIn:(NSString *)first;
// The same, asking `environment` for each new terminal's added variables
// (terminal.activate-venv), the first one's included; it gets the folder
// the terminal starts in.
- (instancetype)initWithDirectory:(NSString *)dir firstIn:(NSString *)first
                      environment:(NSDictionary<NSString *, NSString *> *(^)(NSString *dir))environment;
- (instancetype)initWithDirectory:(NSString *)dir firstIn:(NSString *)first
                      environment:(NSDictionary<NSString *, NSString *> *(^)(NSString *dir))environment
                     sessionPlace:(MCSessionTermPlace *(^)(void))place;
@property(nonatomic, copy) NSDictionary<NSString *, NSString *> *(^environment)(NSString *dir);
// Where each terminal it makes goes on the session's host (the window's
// link, and a held terminal to take over while the window has one); nil, or
// a nil answer: the link for its folder, and a new shell.
@property(nonatomic, copy) MCSessionTermPlace *(^sessionPlace)(void);
// A new terminal beside the active one (a new column to its right), taking
// half its width, or an equal share of the tab when half would be narrower
// than 20 columns; nil (a beep and a status note) when even that would.
- (TerminalView *)splitActive;
- (TerminalView *)splitActiveInDirectory:(NSString *)dir;
// The same below the active one, in its column's stack (5 rows at least).
- (TerminalView *)splitActiveDown;
- (TerminalView *)splitActiveDownInDirectory:(NSString *)dir;
// Resize Terminal Pane (Ctrl+Cmd+arrows, with the keyboard in a terminal):
// the active pane's boundary in that direction moves 4 cells, as VS Code's;
// Up and Down for a pane alone in its column resize the dock (onResizeDock).
// NO when nothing could move. The menu's resizeTerminalPane: (tag: the
// direction) beeps then.
- (BOOL)resizeActivePane:(MCPaneDirection)direction;
- (void)resizeTerminalPane:(id)sender;
// Grows (positive) or shrinks the dock by so many points; NO when it could not.
@property(nonatomic, copy) BOOL (^onResizeDock)(CGFloat delta);
// The bars between the shown tab's panes, for the checks.
- (NSArray<DragBar *> *)splitBars;
+ (CGFloat)minPaneWidth;    // 20 columns
+ (CGFloat)minPaneHeight;   // 5 rows
// Tab `from` moved to `to` (a drag in the strip, Move Terminal Left and
// Right), its terminals and shells as they were; the shown tab stays shown.
- (BOOL)moveTab:(NSUInteger)from to:(NSUInteger)to;
- (BOOL)moveSelectedTab:(NSInteger)delta;
- (BOOL)canMoveSelectedTab:(NSInteger)delta;
// Rename Terminal…: asks for a name (namePrompt, else an alert with the
// current one filled in) and gives it to `terminal`, where it wins over the
// program's title and `mutible term --title`; an empty name clears it. NO
// when cancelled.
- (BOOL)renameTerminal:(TerminalView *)terminal;
// Returns the name, or nil for Cancel. nil: an alert asks.
@property(nonatomic, copy) NSString *(^namePrompt)(TerminalView *terminal);
- (void)selectTab:(NSUInteger)tab;
// Shows the tab `terminal` is in and makes it that tab's active pane.
- (void)selectTerminal:(TerminalView *)terminal;
// Next (+1) or previous (-1) terminal: through the shown tab's panes, then on
// to the next tab, wrapping around.
- (void)moveActive:(NSInteger)delta;
// Close a terminal, or a tab's terminals. A command still running in one,
// waiting to start, or a background job (see TerminalView runningCommands)
// is asked about first, in a dialog that answers at once; NO when the user
// kept it (Cancel, or Show in Processes).
- (BOOL)closeTerminal:(TerminalView *)terminal;
- (BOOL)closeTab:(NSUInteger)tab;
// The same as the user meets it (the tab's ×, its menu's Close, Cmd+W,
// Ctrl+Shift+W): the question drops down as a sheet from the panel's
// window, so it is plain why nothing closed yet, and the close happens on
// its answer: Close, Cancel, or Show in Processes (onShowInProcesses, the
// terminal kept). While one sheet is up another close only beeps, so a
// second press of × never tears a terminal down twice. With nothing
// running, or no window, it closes at once.
- (void)closeTerminalAsking:(TerminalView *)terminal;
- (void)closeTabAsking:(NSUInteger)tab;
// The close question's Show in Processes: the Processes panel with
// `terminal`'s job selected (`job`: its foreground process group, 0 for a
// background job only).
@property(nonatomic, copy) void (^onShowInProcesses)(TerminalView *terminal, pid_t job);
// Every terminal, unasked (the window is closing): shells hung up and reaped.
- (void)closeAll;
- (void)focusActive;
- (BOOL)hasFocus;                 // the keyboard is in one of the terminals
// The window's folder changed: every terminal at a prompt moves there.
- (void)setDirectory:(NSString *)dir;
@end

#ifdef __cplusplus
#include "TabSessions.h"
// The dock between runs (EditorController+Persistence.mm): each tab's panes
// with their folders, names and shares, and bringing them back as fresh
// shells in those folders. A running program is not brought back.
@interface MCTerminalPanel (Session)
- (std::vector<TabSessions::TerminalTab>)sessionTabs;
// On a panel just made (its one terminal is the first tab's first pane):
// the rest of the panes and tabs, then `selected` shown.
- (void)restoreSessionTabs:(const std::vector<TabSessions::TerminalTab> &)tabs
                  selected:(NSInteger)selected;
@end
#endif

// The commands running in these terminals, for a question before they close.
NSArray<NSString *> *MCRunningCommands(NSArray<TerminalView *> *terminals);
// The question ("Close the terminal?"), then "“vim” is still running." (or a
// list), with `button` (Return) and Cancel (Esc). YES to go ahead, and YES at
// once when nothing is running.
BOOL MCConfirmClosingRunning(NSArray<NSString *> *commands, NSString *question,
                             NSString *button);
// That alert, with Show in Processes as a third button when `processes`:
// answers NSAlertFirstButtonReturn (go ahead), Second (Cancel), Third.
NSAlert *MCClosingRunningAlert(NSArray<NSString *> *commands, NSString *question,
                               NSString *button, BOOL processes);
