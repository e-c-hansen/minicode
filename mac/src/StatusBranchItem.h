// StatusBranchItem.h — Objective-C++. The status bar's branch item, at its
// left end, as VS Code's bottom left: a branch glyph and the branch (the
// short hash when HEAD is detached, "*" after it with uncommitted
// changes), then, when the branch has an upstream, what there is to push
// and pull ("↑2 ↓1"). A click on the name switches branch (the palette's
// branch picker), one on the counts syncs (pull, then push). The words
// come from the core (Git::statusBranch, GitStatus.h); the window's tree
// colors (TreeGitColors.h) read the status they come from, so the item
// runs no git of its own. Hidden outside a repository. No C++ here: the
// header reaches every file that imports EditorController+Private.h.
#import <Cocoa/Cocoa.h>

@interface MCStatusBranchItem : NSView
@property(nonatomic, copy) void (^onSwitchBranch)(void);
@property(nonatomic, copy) void (^onSync)(void);
@property(nonatomic, strong) NSColor *textColor;   // the status bar's
// Shows Git::StatusBranch's words (hidden when `name` is empty); NO when
// nothing changed.
- (BOOL)showName:(NSString *)name counts:(NSString *)counts tip:(NSString *)tip;
// Its width with the words it shows (0 when hidden).
@property(nonatomic, readonly) CGFloat fittingWidth;
// Tests: the two parts (the counts hidden without an upstream).
@property(nonatomic, readonly) NSButton *nameButton;
@property(nonatomic, readonly) NSButton *countsButton;
@end
