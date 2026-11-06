// Lsp.h — Objective-C++. The GUI half of the language server client: the
// server processes, and what the editor shows from them (error underlines,
// the completion list with its documentation, the signature hint, go to
// definition, hover info, rename, references and quick fixes). The protocol
// itself is in LspClient.{h,cpp}, the lists and the rename field in
// LspPicker.{h,mm}; the status bar item, the Language Servers popover and
// the Python interpreter picker are in LspPanel.{h,mm}.
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

@class MCMultiCarets;

// The color a diagnostic's severity is drawn in (1 error ... 4 hint): the
// squiggles, the hover box's messages and the gutter's dots.
NSColor *MCDiagnosticColor(NSInteger severity);

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
// The same, for the window's controller (editor.auto-save = focus-change);
// onFocusLost belongs to the language server session.
@property(nonatomic, copy) void (^onBlur)(void);
// The pointer moved: the character under it, or NSNotFound when it is over
// no character (past a line's end, between lines) or left the view.
@property(nonatomic, copy) void (^onMouseHover)(NSUInteger index);
// The view scrolled under the pointer (a wheel or trackpad).
@property(nonatomic, copy) void (^onScrolled)(void);
// Several carets (MultiCaretView.h): a column from Cmd+Shift+drag or
// Option+drag, or carets added with Option+click.
@property(nonatomic, readonly) MCMultiCarets *multiCarets;
// Two or more carets are in play.
@property(nonatomic, readonly) BOOL multiCaretActive;
// VS Code's multi-cursor commands (Edit menu, MultiCaretView.h): Cmd+D,
// Shift+Cmd+L, Option+Cmd+Up / Down. Enabled while the view is editable.
- (IBAction)addNextOccurrence:(id)sender;
- (IBAction)selectAllOccurrences:(id)sender;
- (IBAction)addCursorAbove:(id)sender;
- (IBAction)addCursorBelow:(id)sender;
// Select Line (Cmd+L) and Delete Line (Ctrl+Shift+K), VS Code's, at every
// caret. Enabled while the view is editable.
- (IBAction)expandLineSelection:(id)sender;
- (IBAction)deleteLine:(id)sender;
// editor.ruler: thin lines behind the text at these columns of the
// monospaced font (none while the view is read-only, as a preview is).
@property(nonatomic, copy) NSArray<NSNumber *> *rulerColumns;
@property(nonatomic, strong) NSColor *rulerColor;
// Drawn behind the text, after the background: the find bar's highlights.
@property(nonatomic, copy) void (^drawBehindText)(NSRect rect);
// The Edit menu's find items (performTextFinderAction:, by their
// NSTextFinderAction tags) go here when set, in place of NSTextFinder.
@property(nonatomic, copy) BOOL (^onFindAction)(NSInteger tag);
@property(nonatomic, copy) BOOL (^validateFindAction)(NSInteger tag);
// Bracket matching (EditorController decides which): a box around each of
// these characters, behind the text, in this color (faintly filled with it).
@property(nonatomic, copy) NSArray<NSValue *> *bracketRanges;
@property(nonatomic, strong) NSColor *bracketMatchColor;
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
// cpp, python, go, rust, typescript; a second server's is "python.also".
@property(nonatomic, copy) NSString *key;
@property(nonatomic, copy) NSString *language;     // "Python", "Python, second server"
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
@property(nonatomic, assign) BOOL current;         // one of the open file's servers
// A second server the built-in rule does not want for this project (no
// Ruff configuration): the popover says when it would run, with no Start.
@property(nonatomic, assign) BOOL notUsedHere;
@end

// One interpreter offered by Select Python Interpreter.
@interface MCPythonCandidate : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, copy) NSString *kind;      // project, active, conda, pyenv, path, ...
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *version;   // "" when unknown
@end

// One row of the Problems list (ProblemsPanel.h): a file's heading, or one
// diagnostic a server published for it. Positions are 0-based, columns in
// UTF-16 units, in the text as it is now (a tab's unsaved text included).
@interface MCProblem : NSObject
@property(nonatomic, assign) BOOL heading;
@property(nonatomic, copy) NSString *path;       // the file, as the tree spells it
@property(nonatomic, assign) NSInteger line, column, endLine, endColumn;
@property(nonatomic, assign) NSInteger severity;   // 1 error ... 4 hint
@property(nonatomic, copy) NSString *message;    // on one line
@property(nonatomic, copy) NSString *fullMessage;
@property(nonatomic, copy) NSString *source, *code;
@property(nonatomic, assign) NSInteger errors, warnings, others;   // a heading's
@end

// One symbol for Go to Symbol (the palette's "@" and "#"): from a language
// server, or from the built-in outline (SymbolOutline.h).
@interface MCSymbolItem : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSString *detail;      // the server's, often a signature
@property(nonatomic, copy) NSString *container;   // the enclosing symbol's name, or nil
@property(nonatomic, assign) NSInteger kind;      // LSP's SymbolKind, 0 unknown
@property(nonatomic, assign) NSInteger depth;     // levels below the top
// The file, as the tree spells it; nil for the file the symbols were asked of.
@property(nonatomic, copy) NSString *path;
// Its name, 0-based line and UTF-16 column (the whole symbol for a
// workspace symbol's), and, for the file the symbols were asked of, the
// same as a range of its text (NSNotFound otherwise).
@property(nonatomic, assign) NSInteger line, character, endLine, endCharacter;
@property(nonatomic, assign) NSRange range;
@end

// The built-in outline of a file's text (SymbolOutline.h) as items, with
// the index of the innermost one holding character `caret` (-1 for none).
NSArray<MCSymbolItem *> *MCOutlineSymbols(NSString *path, NSString *text, NSUInteger caret,
                                          NSInteger *caretIndex);

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
// The unsaved text of a file open in a tab (another tab's included), or nil
// when no tab has it: what references show and what an edit checks a
// file's buffer against.
@property(nonatomic, copy) NSString *(^bufferText)(NSString *path);
// A message for the status bar ("Renamed 4 occurrences in 2 files").
@property(nonatomic, copy) void (^showNote)(NSString *message);
// The paths of every file open in a tab of the window, in any order. Each
// one whose text bufferText gives is kept open on its language's server
// (when that server is running), so the server sees unsaved tabs too.
@property(nonatomic, copy) NSArray<NSString *> *(^openPaths)(void);
// Close the tab of a file that an edit opened and then could not change.
@property(nonatomic, copy) void (^closeFile)(NSString *path);
// The diagnostics changed (published, a file closed, a server stopped):
// called once for a burst, a moment after it.
@property(nonatomic, copy) void (^onProblemsChanged)(void);

// The file now in the text view, or nil when the view shows no editable
// source (a message, a binary file, a preview). The previous one stays open
// on its server while its tab does.
- (void)documentOpened:(NSString *)path;
// A tab closed, or a tab's text changed in the background (reloaded from
// disk): the servers' open documents follow.
- (void)tabsChanged;
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
// F2: the name at the caret, through prepareRename when the server has it,
// in a field under it; Return renames it everywhere the server says. Edits
// to other files go into their tabs (opened when need be) as one undo step
// each, unsaved.
- (void)renameSymbol;
// Edits to files open in tabs (Replace in Folder), made as rename makes
// its: every file or none, each in its tab's buffer as one undo step,
// unsaved. ranges[i] (UTF-16, ascending, not overlapping) and strings[i]
// are paths[i]'s. Returns how many files changed, or -1 with *failure.
- (NSInteger)applyEditsToPaths:(NSArray<NSString *> *)paths
                        ranges:(NSArray<NSArray<NSValue *> *> *)ranges
                       strings:(NSArray<NSArray<NSString *> *> *)strings
                    actionName:(NSString *)name
                       failure:(NSString **)failure;
// ---- Go to Symbol (Shift+Cmd+O, Cmd+T)
// The file on show's symbols, from its server when one answers
// textDocument/documentSymbol, else from the built-in outline; `source`
// names where they came from ("clangd", "outline"), and caretIndex is the
// innermost symbol at the caret, or -1. Called once, on the main thread;
// with nil items when there is no file to list.
- (void)documentSymbolsWithCompletion:(void (^)(NSArray<MCSymbolItem *> *items,
                                                NSInteger caretIndex,
                                                NSString *source))done;
// workspace/symbol for `query` from every running server of the window
// that answers it, merged; done is called as each answers (complete NO)
// and once more when the last has (complete YES), with a note when there
// is nothing to ask. A newer query, or cancelWorkspaceSymbols, drops the
// answers still out.
- (void)workspaceSymbols:(NSString *)query
              completion:(void (^)(NSArray<MCSymbolItem *> *items, BOOL complete,
                                   NSString *note))done;
- (void)cancelWorkspaceSymbols;
// Open a workspace symbol's file and select it, as a reference opens.
- (void)revealSymbol:(MCSymbolItem *)item;
// Every jump the session makes (definition, a reference, a problem, a
// symbol) runs inside this, so the window's Go Back can record it.
@property(nonatomic, copy) void (^aroundJump)(dispatch_block_t jump);

// Shift+F12: every reference to the name at the caret, grouped by file, in
// a list by the caret; Return or a click opens one.
- (void)findReferences;
// Cmd+.: the servers' code actions for the caret or the selection, each
// asked with its own diagnostics there, merged in one list by the caret
// (fixes first, then refactorings, then whole-file actions such as Ruff's
// Fix all); Return applies one.
- (void)showQuickFixes;
// Shift+Option+O: the servers' source.organizeImports action for the file,
// applied at once as one undo step (Ruff's own when it runs).
- (void)organizeImports;
// ---- the Problems list (Shift+Cmd+M) and F8 / Shift+F8
// Every diagnostic the window's servers have published: for the files open
// in its tabs, and for any other file a server reports on (as the server
// said, against the file on disk). Grouped by file (Lsp::problemRows).
- (NSArray<MCProblem *> *)problemRows;
// "2 errors, 3 warnings" over all of them, or "No problems".
@property(nonatomic, readonly, copy) NSString *problemCountsText;
@property(nonatomic, readonly) NSInteger problemErrors, problemWarnings;
// Some server is running, or something was published: worth a status item.
@property(nonatomic, readonly) BOOL problemsKnown;
// Open a problem's file (a tab) and select what it is about.
- (void)openProblem:(MCProblem *)problem;
// F8 / Shift+F8: the caret to the next or previous problem in the file on
// show, wrapping, with its message in the hover box. Beeps when it has none.
- (void)goToProblem:(BOOL)forward;

// Keys routed from the text view's doCommandBySelector: while the completion
// list is open (arrows, Return, Tab, Esc), and complete: (Option+Esc).
// Returns YES when handled.
- (BOOL)handleCommand:(SEL)selector;

// ---- the servers, for the Language Servers popover
// Every language MiniCode knows, the open file's first, each followed by its
// second server when it has one (or the built-in rule would give it one).
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
// The names of the servers the open file is on, its main one first (tests).
@property(nonatomic, readonly) NSArray<NSString *> *fileServerNames;
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
// The references or quick fix list: its rows as text, which kind it is
// ("references", "fixes", or nil when hidden), the selected row, its view.
@property(nonatomic, readonly) NSArray<NSString *> *pickerRows;
@property(nonatomic, readonly) NSString *pickerKind;
@property(nonatomic, readonly) NSInteger pickerSelectedRow;
@property(nonatomic, readonly) NSView *pickerView;
- (void)acceptPickerRow:(NSInteger)row;
// The rename field while it shows, else nil; and its popover's view.
@property(nonatomic, readonly) NSTextField *renameField;
@property(nonatomic, readonly) NSView *renameView;
// The last note sent to showNote.
@property(nonatomic, readonly) NSString *lastNote;
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
