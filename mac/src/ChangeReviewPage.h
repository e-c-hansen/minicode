// ChangeReviewPage.h — Objective-C++. The review of every uncommitted
// change in one tab (an EditorPage), as Zed's Uncommitted Changes and VS
// Code's per-change buttons are: the staged files, then the changed ones,
// each a header line (its path, "+12 -3", and Side by Side, Open, Discard
// File and Stage File, or Unstage File) followed by its changes, every run
// of changed lines a block of its own with removed lines tinted red, added
// ones green, the changed words marked more strongly and the file's syntax
// colors on both. Unchanged stretches fold to a row saying how many lines
// they hide, which a click opens. Each block has Stage and Discard beside
// its first line (Unstage in the staged list); a selection's right click
// offers the same for the changed lines it covers. F7 and Shift+F7 step
// through the blocks across files. A file with merge conflicts shows each
// conflict block whole instead, markers and all, with Keep Current, Keep
// Incoming and Keep Both (which write the file), Side by Side of every
// current side against every incoming one, and once none are left, Mark
// Resolved, which stages it (ChangeReview::conflictFile).
//
// What the core works out (ChangeReview.h): the rows, and the index text
// or file text a block's action makes. Staging and unstaging write that
// text to the index as the change bars' Stage Change does (hash-object,
// update-index) after checking the entry still names the object the view
// was drawn from; Discard asks, puts a copy of the file in the Trash
// (Undo Discard brings it back), and writes the file through the atomic
// writer after checking it is still as read, so a tab open on it follows
// the disk as one undo step. A file with unsaved text in a tab is left
// alone, and said so.
//
// Reading is off the main thread: one status, one ls-files, one ls-tree
// and one cat-file --batch for every file at once, then the files from the
// disk. The page reads everything again when the panel refreshes (after
// any action, a window coming to the front, files changing), keeping the
// place the eye was on. Rows are all one height, so sizes and places are
// arithmetic and layout is non-contiguous, as in the compare view.
#import <Cocoa/Cocoa.h>
#import "EditorPage.h"

@class MCGitPanel, MCComparison;

@interface MCChangeReviewPage : NSObject <MCEditorPage>
- (instancetype)initWithRoot:(NSString *)root panel:(MCGitPanel *)panel;
@property(nonatomic, readonly, copy) NSString *root;

// A commit from the graph, the same way and without buttons: a header with
// its subject, author and date in words, and the rest of its message, then
// each file it changed against its first parent (what a merge brought in),
// colored, changed words marked, in one scroll. One such page per window,
// which each commit shown replaces (its key is the window's, not the
// commit's), as the diff tab was.
- (instancetype)initForCommitsWithRoot:(NSString *)root panel:(MCGitPanel *)panel;
- (void)showCommit:(NSString *)hash title:(NSString *)title;
// Compare with Main for a worktree (R2.9), the same way: every file the
// agent changed (committed or not, untracked ones too) against where its
// branch left main (the merge base, so main's later work is left out), in
// one scroll, with Side by Side and Open on each file and, at the top,
// Apply to Main…, which asks, naming the files, then brings the changes
// into the main checkout as uncommitted changes (GitUi::applyToMainPlan):
// refused, naming them, when main has changes of its own in those files;
// a file that does not apply cleanly comes in with conflict marks.
// Once applied, the result has a line of its own under the bar with Show
// in Source Control (onShowSourceControl, the main checkout's path), and
// the button reads "Applied to Main", unavailable, until the worktree's
// changes differ from those brought in.
- (instancetype)initForWorktree:(NSString *)path label:(NSString *)label
                         branch:(NSString *)branch mainRef:(NSString *)mainRef;
- (void)applyToMain;
@property(nonatomic, readonly) BOOL applied;
@property(nonatomic, copy) void (^onShowSourceControl)(NSString *mainPath);
@property(nonatomic, readonly, copy) NSString *worktree;     // nil unless a worktree's page
@property(nonatomic, readonly, copy) NSString *commit;       // nil for uncommitted changes
@property(nonatomic, readonly, copy) NSString *headerTitle;  // the bar's words, for checks
@property(nonatomic, readonly, copy) NSString *headerDetail;

// The window's hooks: open a file at a 1-based line (0: wherever it was),
// show two versions side by side, and whether a tab holds unsaved text
// for a path (a discard then leaves the file alone).
@property(nonatomic, copy) void (^onOpenFile)(NSString *path, NSInteger line);
@property(nonatomic, copy) void (^onShowComparison)(MCComparison *comparison);
@property(nonatomic, copy) BOOL (^hasUnsavedText)(NSString *path);

// Read everything again, keeping the place.
- (void)reload;
// Scroll to a file's header once it is read (a click on its row in the
// panel); nil scrolls to the top.
- (void)revealFile:(NSString *)rel staged:(BOOL)staged;

// For the GUI checks.
@property(nonatomic, readonly) BOOL loaded;            // a read is on show
@property(nonatomic, readonly) NSUInteger loads;       // reads put on show so far
@property(nonatomic, copy) void (^onLoaded)(void);     // after each
@property(nonatomic, copy) void (^onActionDone)(void); // after each action, done or refused
// The last action's words, at the top right, or on a line of their own
// under the bar when they do not fit there; gone once a later read shows
// the changes moved on (a commit, an edit, git in a terminal).
@property(nonatomic, readonly, copy) NSString *message;
@property(nonatomic, readonly) NSTextView *textView;
@property(nonatomic, readonly) NSScrollView *scrollView;
@property(nonatomic, readonly) NSUInteger blockCount;
// "§ Changes  2", "F a.txt  +1 -1", "  same", "- old", "+ new", "~ 6 unchanged lines", "! note"
- (NSArray<NSString *> *)rowDescriptions;
- (NSInteger)topRow;                               // the first row on screen
- (NSInteger)rowOfBlock:(NSUInteger)block;         // its first row, -1 past the end
- (NSInteger)rowOfFile:(NSString *)rel staged:(BOOL)staged;
- (NSRect)rectOfRow:(NSInteger)row;                // in the text view
// A button on a row, by its title ("Stage", "Discard File", ...), in the
// text view's coordinates; zero when the row has none such.
- (NSRect)buttonRect:(NSString *)title row:(NSInteger)row;
// A click at a point in the text view: a button, or a folded stretch.
- (BOOL)pressAtPoint:(NSPoint)point;
// Main-thread CPU time of the last read put on show, in milliseconds.
@property(nonatomic, readonly) double lastShowCpuMilliseconds;
@end
