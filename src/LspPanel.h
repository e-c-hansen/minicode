// LspPanel.h — Objective-C++. What the user sees of the language servers
// beyond the editor itself: the status bar item, the Language Servers
// popover (each server's state, command, binary, PID and Python
// interpreter, with Restart, Stop and Start, and how to install a missing
// one) and the Select Python Interpreter sheet. Everything it shows comes
// from LspSession (Lsp.h).
#import <Cocoa/Cocoa.h>

@class LspSession;

// The status bar's language server item: a dot in the state's color, then
// the text in the status bar's own text color. A click runs onClick.
@interface MCLspStatusField : NSTextField
@property(nonatomic, copy) void (^onClick)(void);
- (void)setStatus:(NSString *)text dot:(NSColor *)dot;
@end

// The Language Servers popover, shown under `view` (the status item). It
// follows the servers while open.
void MCShowLanguageServers(LspSession *session, NSView *view);
// Select Python Interpreter, as a sheet on `window`.
void MCChoosePythonInterpreter(LspSession *session, NSWindow *window);

// Test hooks. The popover's content, built but not shown.
NSView *MCLanguageServersView(LspSession *session);

// The picker behind Select Python Interpreter: row 0 is "Automatic" (no
// choice for this folder), the rest the session's candidates.
@interface MCInterpreterPicker : NSObject
- (instancetype)initWithSession:(LspSession *)session;
@property(nonatomic, readonly) NSPanel *panel;
@property(nonatomic, readonly) NSArray<NSString *> *rowTitles;
@property(nonatomic, readonly) NSInteger selectedRow;
- (void)selectRow:(NSInteger)row;
// Store the selected row's interpreter for the folder and close.
- (void)commit;
@end
