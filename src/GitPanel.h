// GitPanel.h — Objective-C++. The Source Control panel: runs git for one
// window and shows its status in the sidebar's place. Parsing is in the pure
// C++ core (GitStatus.{h,cpp}); this file runs the command line and draws.
//
// Only commands that cannot lose work are ever run: status, diff, add,
// restore --staged (rm --cached before the first commit, which also touches
// only the index), and commit -m. Always argument arrays, never a shell.
#import <Cocoa/Cocoa.h>

// --------------------------------------------------------------- running git
@interface MCGitResult : NSObject
@property(nonatomic, assign) int status;          // exit status; -1 if git never ran
@property(nonatomic, strong) NSData *output;      // stdout, bytes as git wrote them
@property(nonatomic, copy) NSString *errorText;   // stderr, trimmed
@property(nonatomic, readonly) BOOL ok;           // status == 0
@end

// The git the app runs: found like the language servers (PATH, then the
// Homebrew directories, then the developer tools' own copy, never the
// /usr/bin shim). nil when there is none.
NSString *MCGitExecutable(void);

// Runs git with `args` in `dir` and waits. Call it off the main thread.
// The environment keeps git from prompting (no terminal, no editor) and from
// taking optional locks, so a status never writes to .git (the folder is
// watched, and a write there would trigger another refresh).
MCGitResult *MCGitRunSync(NSString *dir, NSArray<NSString *> *args);

// A diff as colored text: added lines green, removed red, hunk and file
// headers muted. `font` is the monospaced font to use.
NSAttributedString *MCGitDiffText(NSData *diff, NSFont *font);

// ------------------------------------------------------------------- panel
@interface MCGitPanel : NSView
- (instancetype)initWithRoot:(NSString *)root;
@property(nonatomic, copy) NSString *root;   // the window's folder; refreshes

// Show a file's diff: `name` for the title, `diff` the bytes git printed.
@property(nonatomic, copy) void (^onShowDiff)(NSString *name, NSString *path,
                                              NSData *diff);

// Reload the status (coalesced; runs off the main thread). Does nothing
// while the panel is not in a window.
- (void)refresh;
- (void)applySettings;
- (void)focusList;

// Test hooks, and what the controller reads.
@property(nonatomic, readonly) BOOL inRepository;
@property(nonatomic, readonly, copy) NSString *topLevel;
@property(nonatomic, readonly, copy) NSString *branchText;
@property(nonatomic, readonly, copy) NSString *errorLine;
@property(nonatomic, readonly) NSTableView *list;
@property(nonatomic, readonly) NSTextView *messageView;
@property(nonatomic, copy) void (^onRefreshed)(void);   // after each status
- (NSArray<NSString *> *)rowDescriptions;   // "S M path", "C U path", headers
- (void)toggleStageAtRow:(NSInteger)row;
- (void)showDiffAtRow:(NSInteger)row;
- (void)commit;
@end
