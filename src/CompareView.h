// CompareView.h — a side-by-side diff in the editor's slot: the old text on
// the left, the new on the right, each read-only with its line numbers,
// lined up row by row (SideBySide.h) so equal lines sit side by side and a
// line only one side has faces an empty, hatched row. Removed lines are
// tinted red on the left, added ones green on the right (the git panel's
// diff colors), and the two panes scroll together.
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
// How many rows differ: changed, removed and added lines.
@property(nonatomic, readonly) NSUInteger changedRows;
// A change offered for the user's decision (the Claude Code IDE link's
// openDiff, IdeLinkServer.mm): the view shows `decisionPrompt` in a bar over
// the panes with Accept and Reject, and `onDecision` is called once, with
// YES or NO. Closing the tab first is a rejection (tabClosed).
@property(nonatomic, copy) NSString *decisionPrompt;
@property(nonatomic, copy) void (^onDecision)(BOOL accepted);
// Pending (0), accepted (1), rejected (-1), or no longer wanted (2: the
// agent stopped waiting, endDecision:).
@property(nonatomic, readonly) NSInteger decision;
- (void)decide:(BOOL)accepted;
// The agent no longer waits for an answer: the bar says `why` and offers
// nothing, and onDecision is not called.
- (void)endDecision:(NSString *)why;
@property(nonatomic, readonly, copy) NSString *decisionNote;
// The tab showing the comparison closed (EditorController's closePageOf:).
- (void)tabClosed;
// Lines the two texts up, once (changedRows and the view use the rows).
// Costly for big texts, so a caller off the main thread does it there
// before handing the comparison over; setting a text again undoes it.
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
@end
