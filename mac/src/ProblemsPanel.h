// ProblemsPanel.h — Objective-C++. The Problems list (Shift+Cmd+M): every
// diagnostic the window's language servers have published, a heading per
// file and a row per problem (severity mark, message, source and code,
// line and column), docked across the bottom of the right pane above the
// terminal. LspSession gives the rows (problemRows); EditorController owns
// the panel, lays it out and opens a row through the session.
//
// Keyboard first: Up and Down move through the problems (headings are
// skipped), Return or a click opens one, Esc gives the keyboard back to the
// editor, Cmd+W and Shift+Cmd+M close it (EditorController).
#import <Cocoa/Cocoa.h>

@class MCProblem;

@interface MCProblemsPanel : NSView
// Return or a click on a problem.
@property(nonatomic, copy) void (^onOpen)(MCProblem *problem);
// Esc: back to the editor, the panel staying open.
@property(nonatomic, copy) void (^onEscape)(void);
// The × in its heading.
@property(nonatomic, copy) void (^onClose)(void);

// New rows, keeping the selection on the same problem when it is still
// listed (else near where it was). `root` shortens paths to the folder's.
- (void)setRows:(NSArray<MCProblem *> *)rows counts:(NSString *)counts root:(NSString *)root;
// Give the list the keyboard, selecting the first problem if none is.
- (void)focusList;
// The list has the keyboard.
@property(nonatomic, readonly) BOOL hasFocus;
// Colors from the settings (the terminal's surface, as the dock it shares).
- (void)applySettings;

// Test hooks: each row as text ("# main.py — 2 errors" for a heading,
// "E 3:5 message [source(code)]" for a problem), the selected row, and the
// counts line.
@property(nonatomic, readonly) NSArray<NSString *> *rowTexts;
@property(nonatomic, readonly) NSInteger selectedRow;
@property(nonatomic, readonly) NSString *countsText;
@property(nonatomic, readonly) NSTableView *table;
@end
