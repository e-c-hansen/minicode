// ProcessesPanel.h — Objective-C++. The Processes panel (View > Toggle
// Processes): what is still running for the window's folder, what it costs
// and what started it (spec feature 5), in the sidebar's place as Agents and
// Source Control are. MCProcessMonitor (ProcessMonitor.h) does the looking;
// this file only draws and asks.
//
// Shift+Cmd+J shows it, and so does the terminal close question's Show in
// Processes (revealTerminal:job:). The heading says the folder's total ("1.8 GB in 14 processes"); then a
// group per terminal of the window, orphans (their terminal closed), and
// "not started here" (other programs whose working folder is in the
// folder), each with its memory, the processes in it as a tree, children
// folded under their parent. A row: the name, memory, CPU and listening
// ports, and a Stop button at its right while the pointer is over it or it
// is selected. Actions (right click, and keys): Stop (asks first unless an
// orphan), Open localhost:PORT in the browser pane, Reveal Origin (the
// terminal), Copy Command Line, Copy Process ID. Return opens the row's
// first port, else reveals its terminal; Space or Right unfolds; Cmd+Delete
// stops; Tab goes to the editor. Nothing is ever stopped otherwise.
#import <Cocoa/Cocoa.h>

@interface MCProcessesPanel : NSView
- (instancetype)init;
// The window's id (`minicode state`'s) and folder, asked at each reload.
@property(nonatomic, copy) NSInteger (^windowID)(void);
@property(nonatomic, copy) NSString *(^folder)(void);
@property(nonatomic, copy) void (^onOpenURL)(NSString *url);
// Show and focus the terminal with that id; NO when it is not open.
@property(nonatomic, copy) BOOL (^onRevealTerminal)(NSInteger terminal);
@property(nonatomic, copy) void (^onNote)(NSString *note);
@property(nonatomic, copy) void (^onTab)(void);
- (void)applySettings;
- (void)focusList;
- (void)reload;
// Selects terminal `terminal`'s process `job` (its process group's leader),
// or with none the first process still running under that terminal's group
// (else its first), once a look has it on show (within 15 s), and gives
// the list the keyboard.
- (void)revealTerminal:(NSInteger)terminal job:(int)job;

// For the GUI checks.
@property(nonatomic, readonly) NSOutlineView *outline;
@property(nonatomic, readonly, copy) NSString *summaryText;   // "1.8 GB in 14 processes"
@property(nonatomic, readonly, copy) NSString *helpText;
// One line per row on show: "G <kind> <title> | <memory>" for a group,
// "P <depth> <pid> <name> | <memory> <cpu> :<ports> [exited]" for a process.
- (NSArray<NSString *> *)rowDescriptions;
- (NSInteger)rowForPid:(int)pid;      // -1 when not on show
- (void)expandAll;
- (void)activateRow:(NSInteger)row;   // Return
- (void)stopRow:(NSInteger)row;       // Stop, with its question
- (void)revealOriginOfRow:(NSInteger)row;
- (void)openPortOfRow:(NSInteger)row;
- (NSMenu *)menuForRow:(NSInteger)row;
// The row's Stop button (nil for a group or an exited process); shown while
// the pointer is over the row or it is selected.
- (NSButton *)stopButtonForRow:(NSInteger)row;
@end
