// MarkdownSidePreview.h — Open Preview to the Side (Control+Command+V, VS
// Code's Cmd+K V): a Markdown tab's rendered page in a pane of its own at the
// right of its source, which stays on show and keeps the keyboard.
//
// One per window, as the editor's text view is one for every tab: the tab
// says whether it wants the pane (MCTab sidePreview) and where it was
// scrolled, and the window's controller renders the page into this pane
// with the same code as the full preview (EditorController+MarkdownRender),
// so math, Mermaid diagrams, web pictures, tables, footnotes and task boxes
// come out as they do there. This object holds only the views: a read-only
// TextKit 1 text view in a scroll view, the bar between it and the source,
// the pane's share of the slot, the page's heading anchors, and the timer
// that re-renders a moment after typing stops.
//
// Every rendered run carries its source line (kMarkdownSourceLine), which is
// what scrolling the two together and editing from the pane build on.
#import <Cocoa/Cocoa.h>

@class PanelScrollView, DragBar;

// Where character `i` of a TextKit 1 text view is drawn, in the view's
// coordinates (the end of the text counts as its last character); empty for
// an empty text or a view on TextKit 2, which asking would switch over.
NSRect MCTextViewRectForCharacter(NSTextView *tv, NSUInteger i);
// Scrolls `scroll` so that y (in the text view's coordinates) is at its top,
// within the text.
void MCScrollTextViewToY(NSScrollView *scroll, NSTextView *tv, CGFloat y);

@interface MCMarkdownSidePreview : NSObject <NSTextViewDelegate>
// Its views go into `host` (hidden until laid out as shown); the divider
// last, so it takes the clicks along the boundary.
- (instancetype)initInView:(NSView *)host;

@property(nonatomic, readonly) PanelScrollView *scrollView;
@property(nonatomic, readonly) NSTextView *textView;
@property(nonatomic, readonly) DragBar *divider;
// The pane's share of the slot it splits with the source, 0.5 at first.
@property(nonatomic) CGFloat fraction;
// Laid out as shown, last time.
@property(nonatomic, readonly) BOOL shown;
// The width the page's text is laid out in (tables with formulas are sized
// to it).
@property(nonatomic, readonly) CGFloat pageWidth;
// The page's headings and footnotes by anchor, for #links.
@property(nonatomic, copy) NSDictionary<NSString *, NSNumber *> *anchors;

// Splits `rect` (the source's slot): the pane at its right when `on` and
// there is room for both, else hidden. Returns what is left for the source.
- (NSRect)layoutInRect:(NSRect)rect shown:(BOOL)on;
// The divider was dragged to x (in the host's coordinates): the new share.
// NO when x is within a snap distance of the right edge, which closes the
// pane, as the browser's bar does.
- (BOOL)dragTo:(CGFloat)x;

// A new page: the text replaced, the view at its top.
- (void)showPage:(NSAttributedString *)page;
// Empties the page (the tab left, or the pane closed), and cancels a
// pending render.
- (void)clear;
- (BOOL)scrollToAnchor:(NSString *)anchor;
@property(nonatomic) CGFloat scrollY;

// Calls onRender `delay` seconds after the last call, once.
- (void)scheduleRenderAfter:(NSTimeInterval)delay;
- (void)cancelRender;
@property(nonatomic, readonly) BOOL renderPending;
@property(nonatomic, copy) void (^onRender)(void);

// A click on a link in the page (NSLinkAttributeName's value); YES when
// handled.
@property(nonatomic, copy) BOOL (^onLink)(id link, NSUInteger index);
@property(nonatomic, copy) void (^onDrag)(CGFloat x);
// The context menu's Close item.
@property(nonatomic, copy) void (^onClose)(void);

// The editor's background behind the page (settings, themes).
- (void)applySettings;
@end
