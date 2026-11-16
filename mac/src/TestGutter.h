// TestGutter.h — Objective-C++. The ▶ beside each test in a test file, one
// object per window: it finds the tests in the file on show (Runnables.h:
// pytest, go test, cargo test, Jest and Vitest) off the main thread, 0.3
// seconds after typing stops and at once for another file, and gives the
// gutter (MCLineGutter's test lane) a mark for each. A click on one, or Run
// Test at Cursor, saves the file and types the command for that test into
// the terminal as Run File does (never into a busy one, the keyboard left
// where it was). When the terminal says the command finished
// (MCTerminalCommandDidFinishNotification, with 133;D's exit status), the
// mark becomes a green check or a red cross, until the file's text
// changes. No marks in large-file mode, a preview or a diff, or for a
// language whose test.<language> is off.
#import <Cocoa/Cocoa.h>

@class CodeTextView, MCLineGutter, TerminalView;

@protocol MCTestGutterHost <NSObject>
- (CodeTextView *)testGutterTextView;
- (MCLineGutter *)testGutterView;
// The file the editor shows as editable source text, or nil.
- (NSString *)testGutterPath;
- (NSString *)testGutterRoot;
// What {python} stands for, as Run File has it.
- (NSString *)testGutterPythonFor:(NSString *)path;
// Saves the file and types `line` into a terminal as Run File does, to run
// in `dir` (a cd first when the terminal is elsewhere); the terminal, or
// nil when nothing was typed (the save failed).
- (TerminalView *)testGutterRun:(NSString *)line inDirectory:(NSString *)dir;
@end

@interface MCTestGutter : NSObject
- (instancetype)initWithHost:(id<MCTestGutterHost>)host;
// Look again soon (the window became key, the settings changed).
- (void)refreshSoon;

// The ▶ on test `index` (in the file's order), and Run Test at Cursor: the
// test the caret is in, or the whole file when it is above every test. NO,
// with a beep and a status note, when nothing can run.
- (BOOL)runTestAtIndex:(NSInteger)index;
- (BOOL)runTestAtCaret;
// Run Test at Cursor can act: the file on show is a test file with tests.
@property(nonatomic, readonly) BOOL canRunTestAtCaret;

// For tests: the tests found ("TestParser::test_parse"), each one's state
// (MCTestState), the looks finished, and the command last typed.
- (NSArray<NSString *> *)testLabels;
- (NSArray<NSNumber *> *)testStates;
@property(nonatomic, readonly) NSUInteger looks;
@property(nonatomic, readonly, copy) NSString *lastCommand;
@property(nonatomic, copy) void (^onNote)(NSString *text);   // the status bar
@end
