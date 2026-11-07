// GitPanel.h — Objective-C++. The Source Control panel: runs git for one
// window and shows its status in the sidebar's place. Parsing is in the pure
// C++ core (GitStatus.{h,cpp}, GitGraph.{h,cpp}); this file runs the command
// line and draws.
//
// Only commands that cannot lose work are ever run: status, diff, add,
// restore --staged (rm --cached before the first commit, which also touches
// only the index), and commit -m; for the graph, log, for-each-ref, rev-list
// and show, which only read; fetch, pull and push, never forced
// (GitUi::syncPlan), with no terminal and no password prompt; switch, which
// git refuses when it would overwrite a change; and for Discard Changes,
// restore --worktree, asked first and only after the file as it was has
// gone to the Trash (GitUi::discardPlan), so it can be put back. Always
// argument arrays, never a shell.
#import <Cocoa/Cocoa.h>

// --------------------------------------------------------------- running git
@interface MCGitResult : NSObject
@property(nonatomic, assign) int status;          // exit status; -1 if git never ran
@property(nonatomic, strong) NSData *output;      // stdout, bytes as git wrote them
@property(nonatomic, copy) NSString *errorText;   // stderr, trimmed
@property(nonatomic, assign) BOOL timedOut;       // stopped at its time limit
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
//
// Every command has a time limit, so a git hung on a network filesystem, a
// lock or a credential helper cannot hold the panel's queue for ever:
// MCGitTimeoutFor gives 30 seconds for the commands that only read (status,
// diff, log, show, rev-parse, for-each-ref, rev-list), 2 minutes for add,
// restore and rm (clean filters such as Git LFS run on add, and git waits on
// another process's index.lock), and 10 minutes for commit, whose hooks may
// run a test suite and whose signing may wait on a passphrase dialog. Past it
// git and everything it started (its own process group) get SIGTERM, then
// SIGKILL 2 seconds later, and errorText says "git did not answer within 30
// seconds: git status ...", with timedOut set and status -1.
MCGitResult *MCGitRunSync(NSString *dir, NSArray<NSString *> *args);
MCGitResult *MCGitRunSyncWithTimeout(NSString *dir, NSArray<NSString *> *args,
                                     NSTimeInterval timeout);
NSTimeInterval MCGitTimeoutFor(NSArray<NSString *> *args);

// A diff as colored text: added lines green, removed red, hunk and file
// headers muted. `font` is the monospaced font to use.
NSAttributedString *MCGitDiffText(NSData *diff, NSFont *font);

// A commit as `git show --format=<Git::showFormat()> --stat --patch` printed
// it: hash, author, date and the whole message, then the stat and the diff.
NSAttributedString *MCGitCommitText(NSData *show, NSFont *font);

// ------------------------------------------------------------------- panel
// The branch picker's rows (GitUi::branchItems), read by listBranches:.
@interface MCGitBranches : NSObject
@property(nonatomic, readonly, copy) NSArray<NSString *> *labels;    // "main", "origin/x"
@property(nonatomic, readonly, copy) NSArray<NSString *> *details;   // "current · a1b2c3d"
// The picker's last row for a typed name, or nil (GitUi::createBranchLabel).
- (NSString *)createLabelFor:(NSString *)typed;
@end

@interface MCGitPanel : NSView
- (instancetype)initWithRoot:(NSString *)root;
@property(nonatomic, copy) NSString *root;   // the window's folder; refreshes

// Show a file's diff: `name` for the title, `diff` the bytes git printed.
@property(nonatomic, copy) void (^onShowDiff)(NSString *name, NSString *path,
                                              NSData *diff);
// Show a commit from the graph: "<short hash> <subject>" and what git show
// printed (for MCGitCommitText).
@property(nonatomic, copy) void (^onShowCommit)(NSString *title, NSData *show);

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

// Where the panel's alerts go, and its messages, while it is not shown:
// the window it belongs to, and a note for the status bar.
@property(nonatomic, weak) NSWindow *hostWindow;
@property(nonatomic, copy) void (^onNote)(NSString *text);
// A file row's menu: Open File and Reveal in File Tree (paths absolute).
@property(nonatomic, copy) void (^onOpenFile)(NSString *path);
@property(nonatomic, copy) void (^onRevealFile)(NSString *path);
// The branch line was clicked: the controller opens the branch picker.
@property(nonatomic, copy) void (^onChooseBranch)(void);

// Stage All Changes and Unstage All Changes (git.stageAll,
// git.unstageAll), from a status read just now, so they work while the
// panel is hidden; Stage All asks first when a file has a conflict.
- (void)stageAll;
- (void)unstageAll;
// Discard Changes (git.clean) on a row of the Changes list, or on the file
// at an absolute path (the file on show), after a question naming it. The
// file as it was goes to the Trash first (GitUi::discardPlan), and Undo
// Discard puts the last one back; nil when there is nothing to undo.
- (void)discardAtRow:(NSInteger)row;
- (void)discardPath:(NSString *)path;
- (void)undoDiscard;
@property(nonatomic, readonly, copy) NSString *undoDiscardTitle;
- (NSInteger)selectedFileRow;   // the list's selected file row, or -1
- (NSMenu *)menuForRow:(NSInteger)row;   // a file row's right click

// The branch picker: the branches (nil, with the reason shown, when there
// are none to list), then switching to one or making one from a typed name.
- (void)listBranches:(void (^)(MCGitBranches *branches))done;
- (void)switchToBranchAtIndex:(NSInteger)index of:(MCGitBranches *)branches;
- (void)createBranchNamed:(NSString *)typed of:(MCGitBranches *)branches;
@property(nonatomic, copy) void (^onActionDone)(void);   // after each of these, for tests

// The commit graph under the change lists.
@property(nonatomic, readonly) NSTableView *graphList;
@property(nonatomic, assign) BOOL allBranches;       // every branch, not just HEAD and upstream
@property(nonatomic, readonly, copy) NSString *graphSummary;   // "2 to push, 1 to pull ..."
@property(nonatomic, copy) void (^onGraphLoaded)(void);        // after each graph load
- (NSArray<NSString *> *)graphDescriptions;   // "hash lane marks subject [labels]"
- (void)showCommitAtRow:(NSInteger)row;
- (void)showMore;                             // the next 200 commits
- (void)focusGraph;

// Fetch, Pull, Push and Sync (pull, then push), from the button under
// Commit ("Sync Changes ↓ 1 ↑ 2", "Publish Branch"), the panel's menu and
// the commands. Each reads the status first, so it works while the panel is
// hidden; what happened shows under the Commit button.
- (void)fetch;
- (void)pull;
- (void)push;
- (void)sync;
@property(nonatomic, readonly) BOOL syncing;
@property(nonatomic, readonly, copy) NSString *syncButtonTitle;   // "" when hidden
@property(nonatomic, readonly, copy) NSString *lastSync;   // "push ok", "fetch failed", "pull refused"
@property(nonatomic, copy) void (^onSynced)(void);         // after each

// git.autofetch (Settings.h, GitUi::autoFetchDue): a timer looks every 30
// seconds, from the panel's creation (the window's, not its first showing)
// until stop. isActive says whether the window has the keyboard (by
// default, whether the panel's window is key).
@property(nonatomic, copy) BOOL (^isActive)(void);
@property(nonatomic, readonly) NSInteger autoFetches;   // background fetches run
- (BOOL)autoFetchTickAt:(NSTimeInterval)now;            // YES when one started
- (void)stop;                                           // the window closes
@end
