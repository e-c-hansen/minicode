// InstallPanel.h — Objective-C++. How MiniCode tells the user to install a
// language server or formatter it could not find, at the moment it matters:
// the bar over the editor when a file's server is missing, the popover its
// Install… button opens (also from the status bar), the rows in the Language
// Servers popover, and Format Document's note. The commands come from
// InstallHints (the core's one table); MiniCode never runs them itself. Copy
// puts one on the pasteboard and Run in Terminal types it into the terminal
// for the user to press Return.
#import <Cocoa/Cocoa.h>

// Where Copy puts a command: the general pasteboard, or the private one
// named by $MINICODE_PASTEBOARD (tests, so the user's clipboard is left alone).
NSPasteboard *MCInstallPasteboard(void);

// Is an installer (uv, pipx, npm, brew, go, rustup) on this Mac? MCFindProgram,
// or, for tests, membership in $MINICODE_INSTALL_HAVE (comma-separated).
BOOL MCInstallerAvailable(NSString *program);

// How to install one missing tool: a sentence on what it gives, the command
// to run in a selectable box with Copy and Run in Terminal, how to get the
// installer first when it is missing too, the other ways (each with Copy),
// and a note that MiniCode finds the tool once it is installed.
@interface MCInstallView : NSView
// For a language server key ("python"). `message`, when given, comes first
// ("lsp.python in the settings runs foo, which was not found."). `run` nil
// leaves out Run in Terminal.
- (instancetype)initWithServer:(NSString *)key message:(NSString *)message
                         width:(CGFloat)width
                 runInTerminal:(void (^)(NSString *command))run;
// For formatters, best first ("pyink", "yapf"): the first one's ways, then
// the others' suggested commands as alternatives.
- (instancetype)initWithFormatters:(NSArray<NSString *> *)programs message:(NSString *)message
                             width:(CGFloat)width
                     runInTerminal:(void (^)(NSString *command))run;
// nil when InstallHints knows no such tool (nothing to show).
@property(nonatomic, readonly, copy) NSString *command;   // the one to run
@property(nonatomic, readonly, copy) NSArray<NSString *> *alternatives;
@property(nonatomic, copy) void (^onDone)(void);   // after Run in Terminal (closes the popover)
// What the Copy and Run in Terminal buttons do (also test hooks).
- (void)copyCommand:(NSString *)command;
- (void)runCommand:(NSString *)command;
// Every text shown, for tests.
- (NSString *)shownText;
@end

// Only the command rows for a server (its command with Copy and Run in
// Terminal, then the other ways with Copy), for the Language Servers popover.
NSView *MCInstallCommandRows(NSString *serverKey, CGFloat width,
                             void (^run)(NSString *command));

// `view` in a transient dark popover by `rect` of `anchor`.
NSPopover *MCShowInstallPopover(MCInstallView *view, NSView *anchor, NSRect rect,
                                NSRectEdge edge);

// The one-line bar over the editor when the open file's language server is
// missing: what is missing, Install…, Don't Show Again, and a close button.
@interface MCInstallBar : NSView
@property(nonatomic, copy) NSString *serverKey;   // sets the text
@property(nonatomic, readonly, copy) NSString *text;
@property(nonatomic, readonly) NSButton *installButton;
@property(nonatomic, copy) void (^onInstall)(NSButton *sender);
@property(nonatomic, copy) void (^onMute)(void);
@property(nonatomic, copy) void (^onClose)(void);
- (void)setBackground:(NSColor *)background text:(NSColor *)text;
@end
extern const CGFloat kMCInstallBarHeight;

// "Don't show again", per language server key, in the app's defaults.
BOOL MCInstallBarMuted(NSString *key);
void MCInstallBarSetMuted(NSString *key, BOOL muted);
// The bar is offered once per language per launch, across windows: YES the
// first time it is asked for `key`, NO after.
BOOL MCInstallBarTakeOffer(NSString *key);
