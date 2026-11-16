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
// through the blocks across files.
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
@property(nonatomic, readonly, copy) NSString *message;   // the last action's words
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
