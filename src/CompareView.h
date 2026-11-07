// CompareView.h — a side-by-side diff in the editor's slot: the old text on
// the left, the new on the right, each read-only with its line numbers,
// lined up row by row (SideBySide.h) so equal lines sit side by side and a
// line only one side has faces an empty, hatched row. Removed lines are
// tinted red on the left, added ones green on the right (the git panel's
// diff colors), the words that changed inside a changed line more strongly,
// both panes take the file's syntax colors, and the two scroll together.
// F7 and Shift+F7 (VS Code's Go to Next and Previous Difference) step
// through the runs of changes, wrapping around.
//
// `minicode diff` shows one in the window's agent preview tab
// (ControlCommands.mm); anything else that compares two texts (a worktree
// against its main branch) can make an MCComparison and hand it to
// EditorController's showComparison:preview:.
#import <Cocoa/Cocoa.h>

// What is compared: two texts and what each one is.
@interface MCComparison : NSObject
@property(nonatomic, copy) NSString *title;        // the tab's title
@property(nonatomic, copy) NSString *leftLabel;    // "HEAD: src/a.py"
@property(nonatomic, copy) NSString *rightLabel;   // "src/a.py"
@property(nonatomic, copy) NSString *leftText;
@property(nonatomic, copy) NSString *rightText;
// The file on the right, when it is one (for `minicode state`); else nil.
@property(nonatomic, copy) NSString *path;
// The name whose extension picks the syntax colors; path's when not set.
@property(nonatomic, copy) NSString *fileName;
// How many rows differ: changed, removed and added lines.
@property(nonatomic, readonly) NSUInteger changedRows;
// How many runs of changes (hunks) there are.
@property(nonatomic, readonly) NSUInteger hunkCount;

// A change offered for the user's decision (the Claude Code IDE link's
// openDiff, IdeLinkServer.mm): the view shows `decisionPrompt` in a bar over
// the panes with Accept and Reject, and Accept and Reject on each hunk
// between the panes. `onDecision` is called once: NO when nothing was
// accepted, else YES with `acceptedText` the text made of the accepted
// hunks (rightText when all were). Closing the tab first is a rejection
// (tabClosed).
@property(nonatomic, copy) NSString *decisionPrompt;
@property(nonatomic, copy) void (^onDecision)(BOOL accepted);
// Pending (0), accepted (1, all or some hunks), rejected (-1), or no longer
// wanted (2: the agent stopped waiting, endDecision:).
@property(nonatomic, readonly) NSInteger decision;
// The text the user accepted, once decision is 1.
@property(nonatomic, readonly, copy) NSString *acceptedText;
// The whole change: YES accepts every hunk not rejected on its own, NO
// rejects all of it. Either ends the decision.
- (void)decide:(BOOL)accepted;
// One hunk: accepted (1), rejected (-1) or undecided again (0). When no
// hunk is left undecided, the decision ends: accepted if any hunk was.
- (void)decideHunk:(NSUInteger)hunk as:(NSInteger)choice;
// A hunk's own choice (1, -1, or 0 while undecided).
- (NSInteger)decisionOfHunk:(NSUInteger)hunk;
// The bar's buttons once some hunks are decided: every hunk still
// undecided takes `accepted`, and the decision ends.
- (void)decideRest:(BOOL)accepted;
// Whether any hunk has a choice of its own yet.
@property(nonatomic, readonly) BOOL hunksDecided;
// The agent no longer waits for an answer: the bar says `why` and offers
// nothing, and onDecision is not called.
- (void)endDecision:(NSString *)why;
@property(nonatomic, readonly, copy) NSString *decisionNote;
// The tab showing the comparison closed (EditorController's closePageOf:).
- (void)tabClosed;
// Lines the two texts up, once (changedRows and the view use the rows), and
// works out each pane's text, syntax colors and changed words. Costly for
// big texts, so a caller off the main thread does it there before handing
// the comparison over; setting a text again undoes it.
- (void)prepare;
@end

@interface MCCompareView : NSView
@property(nonatomic, strong) MCComparison *comparison;   // shows it, scrolled to the top
// The editor's colors, again (a settings change).
- (void)applySettings;
// Where the panes are scrolled, to keep and give back across tab switches.
@property(nonatomic) NSPoint scrollOrigin;
// The decision bar's buttons (hidden when there is nothing to decide), for
// tests.
@property(nonatomic, readonly) NSButton *acceptButton, *rejectButton;
// The panes, for focus and tests.
@property(nonatomic, readonly) NSTextView *leftView, *rightView;
@property(nonatomic, readonly) NSScrollView *leftScroll, *rightScroll;

// F7 and Shift+F7 (Navigate > Next Difference, Previous Difference): the
// next or previous run of changes after the one last gone to, or after the
// top of the panes when that one is scrolled away; wrapping around.
- (IBAction)goToNextDifference:(id)sender;
- (IBAction)goToPreviousDifference:(id)sender;
// The hunk last gone to (-1 before any), and the words over the right pane
// ("3 changes", "Change 2 of 3").
@property(nonatomic, readonly) NSInteger currentHunk;
@property(nonatomic, readonly) NSString *changesText;
// The first row on screen.
@property(nonatomic, readonly) NSUInteger topRow;

// The per-hunk buttons between the panes, while a decision is pending: a
// hunk's Accept (YES) or Reject button in this view's coordinates (zero
// when the hunk is scrolled out of sight), and a press at a point, as a
// click there does (YES when it hit one).
- (NSRect)rectForHunk:(NSUInteger)hunk accept:(BOOL)accept;
- (BOOL)pressHunkButtonAtPoint:(NSPoint)point;
@end
