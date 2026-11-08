// GitGutter.h — Objective-C++. The editor's change bars (VS Code's quick
// diff), one object per window: it reads the file on show's index version
// off the main thread, diffs the buffer against it (QuickDiff.h) 0.3
// seconds after typing stops, and gives the bars to the gutter
// (MCLineGutter), which draws them and moves them with each edit until the
// next diff. A click on a bar shows its old lines in a popover with Revert
// Change and Stage Change; Next Change and Previous Change (Option+F5,
// Shift+Option+F5) move the caret between them.
//
// The original is read again only when the file on show changes or the
// repository's index file does (its stat is taken each time, so a stage, a
// commit or a checkout in a terminal shows on the next look); looks come
// after edits, on the window becoming key, and after the Source Control
// panel refreshes. No bars for a file outside a repository, an untracked,
// ignored or conflicted one, a link, a submodule, in large-file mode, a
// preview or a diff, or with editor.git-gutter off.
//
// Revert Change is an edit of the buffer (one undo step, nothing saved),
// VS Code's way; Stage Change writes the index's text with that one change
// in it as an object and points the index at it (GitUi::hashObjectArgs,
// updateIndexArgs), after checking the index still holds what the bars were
// made from.
#import <Cocoa/Cocoa.h>

@class CodeTextView, MCLineGutter;

@protocol MCGitGutterHost <NSObject>
- (CodeTextView *)gitGutterTextView;
- (MCLineGutter *)gitGutterView;
// The file the editor shows as source text, or nil (a preview, a diff, a
// viewer, large-file mode, no file).
- (NSString *)gitGutterPath;
// Stage Change changed the index: the panel should look again.
- (void)gitGutterChangedIndex;
@end

@interface MCGitGutter : NSObject
- (instancetype)initWithHost:(id<MCGitGutterHost>)host;
// Look again now (the window became key, the panel refreshed).
- (void)refreshSoon;

// The bars the gutter has, and counters, for tests.
@property(nonatomic, readonly) NSUInteger barCount;
- (NSArray<NSString *> *)barDescriptions;   // "modified 3+1 (was 3+1)", for tests and logs
@property(nonatomic, readonly) NSUInteger diffs;   // diffs applied, for tests
@property(nonatomic, readonly) NSUInteger looks;   // looks begun (with a file or not)

// Option+F5, Shift+Option+F5: the caret to the next or previous change's
// first line, wrapping; NO (and nothing moves) without bars.
- (BOOL)goToNextChange:(BOOL)forward;
// The bar on the caret's line, or -1.
- (NSInteger)barAtCaret;
// Revert Change and Stage Change for bar `index` (barAtCaret from the
// menus). NO, saying why in a beep and the status note, when it cannot.
- (BOOL)revertChange:(NSInteger)index;
- (BOOL)stageChange:(NSInteger)index;
@property(nonatomic, copy) void (^onStaged)(NSString *failure);   // nil when it worked; tests
@property(nonatomic, copy) void (^onNote)(NSString *text);        // the status bar

// The popover a click on bar `index` shows (its title and old lines; the
// harness reads them, since a popover never shows offscreen).
- (void)showChange:(NSInteger)index rect:(NSRect)rect;
@property(nonatomic, readonly, copy) NSString *popoverTitle;
@property(nonatomic, readonly, copy) NSString *popoverText;
- (void)closePopover;
@end
