// Lsp.h — Objective-C++. The GUI half of the language server client: the
// server processes, and what the editor shows from them (error underlines,
// the completion list, go to definition, hover info). The protocol itself is
// in LspClient.{h,cpp}.
//
// EditorController owns one LspSession per window and tells it when a file is
// opened, saved or closed. The session watches the text view for edits on its
// own, so typing needs no hook in the controller.
#import <Cocoa/Cocoa.h>

// The editor's text view: draws diagnostics as squiggles under the text,
// shows their messages as tooltips, and reports Cmd+click. Diagnostics are
// kept as character ranges that follow edits until the server sends new ones.
@interface MCDiagnosticMark : NSObject
@property(nonatomic, assign) NSRange range;
@property(nonatomic, assign) NSInteger severity;   // 1 error ... 4 hint
@property(nonatomic, copy) NSString *message;
@end

@interface CodeTextView : NSTextView
@property(nonatomic, copy) NSArray<MCDiagnosticMark *> *diagnostics;
// Cmd+click at a character index. Return NO to let the click through.
@property(nonatomic, copy) BOOL (^onCommandClick)(NSUInteger index);
// A double-click while the view is read-only (a rendered preview), with the
// character under the pointer and the click's point in the view. Returning
// YES takes the click; NO lets it select a word as usual.
@property(nonatomic, copy) BOOL (^onPreviewDoubleClick)(NSUInteger index, NSPoint point);
// The message of the diagnostic covering a character, or nil.
- (NSString *)diagnosticMessageAtIndex:(NSUInteger)index;
// The line fragments a character range covers, in view coordinates.
- (void)enumerateRectsForRange:(NSRange)range block:(void (^)(NSRect rect))block;
@end

@interface LspSession : NSObject
- (instancetype)initWithTextView:(CodeTextView *)textView root:(NSString *)root;

// Status bar text for the language server (server name and problem counts,
// or a short note when there is no server). Called on every change.
@property(nonatomic, copy) void (^onStatus)(NSString *text);
// Open a file (for go to definition). Return YES if it is now the current
// file; the session then selects the target range.
@property(nonatomic, copy) BOOL (^openFile)(NSString *path);

// The file now in the text view, or nil when the view shows no editable
// source (a message, a binary file, a preview). Closes the previous one.
- (void)documentOpened:(NSString *)path;
- (void)documentSaved;
// The folder changed: stop every server, they belong to the old root.
- (void)setRoot:(NSString *)root;
// The window is closing: close documents and shut the servers down.
- (void)shutdown;

// Menu actions. Each beeps (and says why in the status bar) when the current
// file has no server.
- (void)triggerCompletion;
- (void)goToDefinition;
- (void)showHoverInfo;
// Keys routed from the text view's doCommandBySelector: while the completion
// list is open (arrows, Return, Tab, Esc), and complete: (Option+Esc).
// Returns YES when handled.
- (BOOL)handleCommand:(SEL)selector;

// Test hooks.
@property(nonatomic, readonly) BOOL completionVisible;
@property(nonatomic, readonly) NSArray<NSString *> *completionLabels;
@property(nonatomic, readonly) NSString *statusText;
@property(nonatomic, readonly) NSString *lastHoverText;
@end

// Stop every server the app started, now: shutdown and exit, then a signal
// for any that are still running a moment later. For applicationWillTerminate.
void MCLspTerminateAllServers(void);
// How many server processes are running (tests).
NSUInteger MCLspRunningServerCount(void);
