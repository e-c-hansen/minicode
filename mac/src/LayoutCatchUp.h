// LayoutCatchUp.h — a text view shown far down its text without laying out
// every line above the place first (editor N2). The editor's layout is
// contiguous, so its rects are exact, and showing a 100,000-line tab again
// at its middle laid out 50,000 lines on the main thread before it could
// draw (1.5 to 3.9 s). Instead the place is laid out alone, with
// non-contiguous layout, and this object lays out the lines above it about
// 8 ms at a time on the main thread's idle turns, upward
// from the pane, keeping what is on screen where it is while the estimate
// above it becomes the real height. Once nothing between the top of the
// text and the pane is unlaid, every rect from the top through the pane is
// the one contiguous layout gives (the keystroke suite compares them);
// below the pane, text is laid out as it is reached, as contiguous layout
// lays it out, and a jump far down leaves a gap above it that is closed
// the same way.
//
// Measured (TextKit 1, macOS 27): turning non-contiguous layout off again
// throws all of the layout away (the next rect at the middle of 100,000
// lines then cost 0.7 s, as from scratch), so the flag stays on while the
// tab shows; and with it on, an edit that adds a line above the pane leaves
// what follows estimated until laid out again. So this keeps watching: an
// edit made through the text view has what it left unlaid above the pane
// laid out as it ends, the pane's scroll kept, so the text moves as it
// does with contiguous layout; a new width or another change goes to the
// pieces 0.3 s later. Before an edit of many ranges it goes back to
// contiguous layout for good (MCTextViewWillReplaceManyRangesNotification).
#import <Cocoa/Cocoa.h>

// Posted by CodeTextView (object: the view) before an edit of many ranges
// (Replace All, thousands of carets), which non-contiguous layout makes
// several times slower in a highlighted text: after each storage edit
// TextKit looks up the attributes at the pane and the caret, walking the
// color runs between them and the edit (Replace All of 20,000 matches in
// 100,000 lines: 4.3 s against 0.6). The catch-up turns contiguous layout
// back on first, and the pane is laid out from the top once, after it.
extern NSNotificationName const MCTextViewWillReplaceManyRangesNotification;

@interface MCLayoutCatchUp : NSObject

- (instancetype)initWithTextView:(NSTextView *)textView scrollView:(NSScrollView *)scroll;

// Turns non-contiguous layout on for the text the view holds; call before
// laying out the place. Then `start` once the place is on screen.
- (void)prepare;
- (void)start;

// Stops, leaving the layout and the flag as they are (the text is going:
// whatever shows next sets the flag for itself).
- (void)cancel;

// Asked before each piece: NO stops it for good (another text, a preview,
// large-file mode, which keeps non-contiguous layout and its estimates).
@property (nonatomic, copy) BOOL (^stillWanted)(void);

// Laying out pieces now; NO once nothing above the pane is unlaid.
@property (nonatomic, readonly) BOOL running;
// Started and not cancelled: keeping the text above the pane laid out.
@property (nonatomic, readonly) BOOL watching;
// Main-thread CPU since `start`: the longest turn, all of them, how many,
// and the turn at a fraction of them in order (0.95: the 95th percentile).
@property (nonatomic, readonly) double longestPieceMilliseconds;
@property (nonatomic, readonly) double totalMilliseconds;
@property (nonatomic, readonly) NSUInteger pieces;
- (double)turnMillisecondsAtFraction:(double)fraction;

@end
