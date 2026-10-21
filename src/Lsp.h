// Lsp.h — Objective-C++. The GUI half of the language server client: the
// server processes, and what the editor shows from them (error underlines,
// the completion list with its documentation, the signature hint, go to
// definition, hover info). The protocol itself is in LspClient.{h,cpp}; the
// status bar item, the Language Servers popover and the Python interpreter
// picker are in LspPanel.{h,mm}.
//
// EditorController owns one LspSession per window and tells it when a file is
// opened, saved or closed. The session watches the text view for edits on its
// own, so typing needs no hook in the controller.
#import <Cocoa/Cocoa.h>

// The editor's text view: draws diagnostics as squiggles under the text,
// reports where the pointer rests (for hover info, which also shows their
// messages), and reports Cmd+click. Diagnostics are
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
// The keyboard left the view (the terminal, the tree, another window...).
@property(nonatomic, copy) void (^onFocusLost)(void);
// The pointer moved: the character under it, or NSNotFound when it is over
// no character (past a line's end, between lines) or left the view.
@property(nonatomic, copy) void (^onMouseHover)(NSUInteger index);
// The view scrolled under the pointer (a wheel or trackpad).
@property(nonatomic, copy) void (^onScrolled)(void);
// The character drawn at a point in the view, or NSNotFound.
- (NSUInteger)characterIndexUnderPoint:(NSPoint)point;
// The message of the diagnostic covering a character, or nil.
- (NSString *)diagnosticMessageAtIndex:(NSUInteger)index;
// The line fragments a character range covers, in view coordinates.
- (void)enumerateRectsForRange:(NSRange)range block:(void (^)(NSRect rect))block;
@end

// What one language's server is doing, as the status bar and the Language
// Servers popover tell it.
typedef NS_ENUM(NSInteger, MCLspState) {
    MCLspStateIdle,          // not started: starts when a file of its language opens
    MCLspStateStarting,      // launched, handshake not answered yet
    MCLspStateRunning,
    MCLspStateNotInstalled,  // no command found on any of the search folders
    MCLspStateOff,           // lsp.<server> = off, or lsp.enabled = false
    MCLspStateStopped,       // stopped from the popover; Start brings it back
    MCLspStateFailed,        // could not be launched, or refused `initialize`
    MCLspStateRestarting,    // exited on its own; a restart is scheduled
    MCLspStateCrashed,       // exited on its own too often; restarted by hand only
};

@interface MCLspServerInfo : NSObject
@property(nonatomic, copy) NSString *key;          // cpp, python, go, rust, typescript
@property(nonatomic, copy) NSString *language;     // "Python"
@property(nonatomic, assign) MCLspState state;
@property(nonatomic, copy) NSString *stateText;    // "running", "not installed", ...
@property(nonatomic, copy) NSString *serverName;   // "pyright", or nil when none found
@property(nonatomic, copy) NSString *command;      // the command line used or tried
@property(nonatomic, copy) NSString *program;      // its resolved path, or nil
@property(nonatomic, assign) pid_t pid;            // 0 when not running
@property(nonatomic, copy) NSString *message;      // why it failed or crashed, or nil
@property(nonatomic, copy) NSString *installHint;  // for a missing server
// Python only: the interpreter the server analyses with (nil: the server's
// own choice) and where that came from ("this folder", "settings",
// "automatic").
@property(nonatomic, copy) NSString *interpreter;
@property(nonatomic, copy) NSString *interpreterSource;
@property(nonatomic, assign) BOOL current;         // the open file's language
@end

// One interpreter offered by Select Python Interpreter.
@interface MCPythonCandidate : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, copy) NSString *kind;      // project, active, conda, pyenv, path, ...
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *version;   // "" when unknown
@end

// Posted (object: the session) whenever a server's state, the open file's
// language or the Python interpreter changes.
extern NSNotificationName const MCLspSessionDidChangeNotification;
// Posted (object: the folder's path) when a folder's Python interpreter is
// chosen, so every window on that folder follows.
extern NSNotificationName const MCPythonInterpreterDidChangeNotification;

@interface LspSession : NSObject
- (instancetype)initWithTextView:(CodeTextView *)textView root:(NSString *)root;

// Status bar text for the open file's language server (its name and
// problem counts, or its state), and the color of the dot drawn before it:
// green running, yellow starting, red failed or crashed, gray when there is
// none; nil for no dot. Called on every change.
@property(nonatomic, copy) void (^onStatus)(NSString *text, NSColor *dot);
// Open a file (for go to definition). Return YES if it is now the current
// file; the session then selects the target range.
@property(nonatomic, copy) BOOL (^openFile)(NSString *path);

// The file now in the text view, or nil when the view shows no editable
// source (a message, a binary file, a preview). Closes the previous one.
- (void)documentOpened:(NSString *)path;
- (void)documentSaved;
// The folder changed: stop every server, they belong to the old root.
- (void)setRoot:(NSString *)root;
@property(nonatomic, readonly, copy) NSString *root;
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

// ---- the servers, for the Language Servers popover
// Every language MiniCode knows, the open file's first.
- (NSArray<MCLspServerInfo *> *)serverInfos;
// The open file's language key, or nil.
@property(nonatomic, readonly, copy) NSString *currentLanguage;
// The open file's language key when its server is wanted (lsp.enabled on,
// the language not off) but was not found; nil otherwise.
@property(nonatomic, readonly, copy) NSString *missingServer;
// Look for the open file's missing server again and start it when it is
// there now. Also done on its own whenever the window becomes key and when
// a command finishes in any terminal, so a server installed meanwhile is
// picked up without a restart.
- (void)lookAgainForMissingServer;
// The open file is on a server (starting or running).
@property(nonatomic, readonly) BOOL hasServer;
- (void)restartServer:(NSString *)key;   // stop if running, then start
- (void)stopServer:(NSString *)key;      // stays stopped until started
- (void)startServer:(NSString *)key;
// Restart Language Server: the open file's. Beeps when it has none.
- (void)restartCurrentServer;

// ---- the Python interpreter
// What the Python server analyses with for the open file (nil: left to the
// server), and where the choice came from.
- (NSString *)pythonInterpreterWithSource:(NSString **)source;
// The same, leaving this folder's own choice out: what "Automatic" means.
- (NSString *)defaultPythonInterpreterWithSource:(NSString **)source;
// This folder's own choice, or nil for none (the setting or automatic).
- (NSString *)chosenPythonInterpreter;
// Store a choice for this folder (nil: none) and apply it.
- (void)choosePythonInterpreter:(NSString *)path;
- (NSArray<MCPythonCandidate *> *)pythonCandidates;

// Test hooks.
@property(nonatomic, readonly) BOOL completionVisible;
@property(nonatomic, readonly) NSArray<NSString *> *completionLabels;
@property(nonatomic, readonly) NSString *completionSelectedLabel;
// The documentation panel beside the list: its text, or nil when hidden.
@property(nonatomic, readonly) NSString *completionDocText;
// The signature hint's text, or nil when hidden.
@property(nonatomic, readonly) NSString *signatureText;
@property(nonatomic, readonly) NSString *statusText;
@property(nonatomic, readonly) NSColor *statusDot;
@property(nonatomic, readonly) NSString *lastHoverText;
@property(nonatomic, readonly) BOOL hoverVisible;
@end

// Where tools are looked for: PATH, then the package managers' directories
// (a Finder-launched app gets a short PATH): Homebrew, MacPorts, npm (a
// prefix in ~/.npmrc, ~/.npm-global, nvm, fnm, volta), pnpm, bun, Cargo, Go,
// pipx and uv (~/.local/bin), pip --user (~/Library/Python/*/bin), pyenv's
// shims; Homebrew's llvm last, for its clangd. Also the PATH given to
// children.
NSArray<NSString *> *MCSearchDirs(void);
// A program by name (or path) on those directories, or nil. /usr/bin's
// developer-tool shims are never run: the real binary is found instead, so
// a Mac without the command line tools never gets the install dialog.
NSString *MCFindProgram(NSString *name);
// The developer tools' own copy of a tool in /usr/bin (python3, clangd), or
// nil when the tools are not installed.
NSString *MCDeveloperToolPath(NSString *name);

// Stop every server the app started, now: shutdown and exit, then a signal
// for any that are still running a moment later. For applicationWillTerminate.
void MCLspTerminateAllServers(void);
// How many server processes are running (tests).
NSUInteger MCLspRunningServerCount(void);
