// PageSidePane.h — Open Preview to the Side for a LaTeX or an HTML file: the
// file's page in a pane at the right of its source, which stays on show and
// keeps the keyboard, as Markdown's pane (MarkdownSidePreview.h) does.
//
// This object holds only the split: the bar between the source and the
// page, the page's share of the slot, and where the page view goes. The
// page view is the controller's (EditorController+PageSide.mm): a LaTeX
// tab's own preview (LatexView), or the window's HTML page for the side
// (MCHtmlPage showing the buffer). One per window, as Markdown's pane is.
#import <Cocoa/Cocoa.h>

@class DragBar;

@interface MCPageSidePane : NSObject
// The bar goes into `host`, hidden until laid out as shown.
- (instancetype)initInView:(NSView *)host;

@property(nonatomic, readonly) DragBar *divider;
// The page's share of the slot it splits with the source, 0.5 at first.
@property(nonatomic) CGFloat fraction;
// Laid out as shown, last time.
@property(nonatomic, readonly) BOOL shown;
// The rect the page was given last time, in the host's coordinates.
@property(nonatomic, readonly) NSRect pageRect;

// Splits `rect` (the source's slot): `page` at its right when it is given
// and there is room for both, else nothing shown. Returns what is left for
// the source. The page view is framed and shown here; it is not hidden
// here when the pane is not shown (the caller decides that).
- (NSRect)layoutInRect:(NSRect)rect page:(NSView *)page;
// The bar was dragged to x (in the host's coordinates): the new share. NO
// when x is within a snap distance of the right edge, which closes the
// pane, as Markdown's does.
- (BOOL)dragTo:(CGFloat)x;
@property(nonatomic, copy) void (^onDrag)(CGFloat x);
@end
