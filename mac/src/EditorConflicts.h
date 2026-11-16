// EditorConflicts.h — Objective-C++. The merge conflicts in the editor's
// text (MergeConflicts.h), as VS Code and Zed offer them: each block's
// current side (ours) and incoming side (theirs) tinted behind the text in
// editor.merge-current and editor.merge-incoming, the markers' lines more
// strongly, the common ancestor (diff3, zdiff3) in gray, and on the
// "<<<<<<<" line, after its label, Keep Current, Keep Incoming and Keep
// Both, then "Conflict 2 of 3". A press on one replaces the block with the
// side kept (Both: current, then incoming) as one undo step, through
// CodeTextView replaceRanges:withStrings:actionName:, and says in the
// status bar how many are left (none: stage it to mark it resolved). Next
// Change and Previous Change (Option+F5, Shift+Option+F5) step between
// the blocks of a file that has them.
//
// The text view owns one (CodeTextView conflicts) and asks it to draw and
// to take a press first. The blocks move with each edit at once
// (MergeConflicts::followEdit: an edit inside a block drops it) and are
// read again from the text 0.2 seconds after typing pauses, or at once
// when the whole text is replaced (another tab). Nothing is read while
// the view is read-only, past 20 MB (large-file mode's threshold), or when
// `wanted` says no (a preview, a diff, large-file mode).
#import <Cocoa/Cocoa.h>

@class CodeTextView;

@interface MCEditorConflicts : NSObject
- (instancetype)initWithTextView:(CodeTextView *)tv;

// Whether the text shown is a file whose blocks count (the window says).
@property(nonatomic, copy) BOOL (^wanted)(void);
// After a choice: how many blocks are left.
@property(nonatomic, copy) void (^onResolved)(NSUInteger left);
@property(nonatomic, strong) NSColor *currentColor, *incomingColor;

// Read the text again now (the view became editable, settings changed).
- (void)rescanNow;
- (void)rescanSoon;

@property(nonatomic, readonly) NSUInteger count;
// "current HEAD 1, incoming feature 2" per block, for checks.
- (NSArray<NSString *> *)blockDescriptions;

// Drawing, from the text view: the tints behind the text, the buttons
// over it.
- (void)drawBehindInRect:(NSRect)dirty;
- (void)drawOverInRect:(NSRect)dirty;
// A press on a button: YES when taken.
- (BOOL)mouseDown:(NSEvent *)event;
- (BOOL)pressAtPoint:(NSPoint)point;

// keep: 0 current, 1 incoming, 2 both. The button's rect in the view, zero
// when the block is not laid out on screen; the choice made directly.
- (NSRect)buttonRectForKeep:(NSInteger)keep block:(NSUInteger)block;
- (BOOL)keep:(NSInteger)keep block:(NSUInteger)block;

// Next and Previous Change: the caret to the next or previous block's
// first line, wrapping. NO without blocks.
- (BOOL)goToNext:(BOOL)forward;
@end
