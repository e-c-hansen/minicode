// SnippetSession.h — Objective-C++. The tab stops of a snippet completion
// in the editor's text view (the text logic is Snippet::Session, pure C++,
// tested): Tab and Shift+Tab move between them, a stop's mirrors become
// carets (MCMultiCarets), a choice stop opens the completion list with its
// options, and the stops are drawn as faint boxes behind the text.
//
// The session follows every edit of the storage. It ends at the final stop
// ($0), on Esc, when the selection leaves the current stop (a click, an
// arrow past its edge), on an undo or redo, when a replacement swallows
// the stop being typed in (Select All and typing, a tab switch's new
// text), or when another snippet starts. Typing in a stop is ordinary
// typing, each its own undo step; the insertion was one (the accept's).
#import <Cocoa/Cocoa.h>
#include "Snippet.h"

@class CodeTextView;

@interface MCSnippetSession : NSObject
- (instancetype)initWithTextView:(CodeTextView *)tv;

// A session is running.
@property(nonatomic, readonly) BOOL active;
// Starts one for an expansion whose text was just put into the view at
// `base`, selecting its first stop. Does nothing (and ends any running
// session) when the expansion has no stop but the final one; then the
// caller puts the caret at the final stop itself.
- (void)startWithExpansion:(const Snippet::Expansion &)expansion base:(NSUInteger)base;
- (void)end;

// Tab, Shift+Tab and Esc while a session runs (YES when taken). The text
// view asks before anything else has the key, the several carets
// included; `commandTakenFirst` is asked first in turn, so the completion
// list (a choice stop's options) keeps Tab and Esc while it shows.
- (BOOL)doCommand:(SEL)selector;
@property(nonatomic, copy) BOOL (^commandTakenFirst)(SEL selector);
// A choice stop was reached: the options, for the stop's range (selected).
@property(nonatomic, copy) void (^onChoices)(NSArray<NSString *> *choices, NSRange range);

// Draws the stops (call behind the text).
- (void)drawInRect:(NSRect)dirty;

// For tests: the snippet index of the current stop (0 the final), and every
// living stop's ranges, the current one first.
@property(nonatomic, readonly) NSInteger currentStopIndex;
@property(nonatomic, readonly) NSArray<NSValue *> *currentRanges;
@property(nonatomic, readonly) NSArray<NSValue *> *allRanges;
@end
