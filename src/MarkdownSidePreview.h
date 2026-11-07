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

// ---- Following the source (VS Code's markdown.preview.scrollPreviewWithEditor,
// scrollEditorWithPreview and markEditorSelection, all on there by default)
//
// Scrolling the source puts the page's text from the source's top line at
// the page's top, and scrolling the page puts the line its top shows at
// the source's top, fractions of lines included, by the map of
// PreviewScroll.h over the page's runs (their `sourceLineKey`). Only the
// user's scrolling leads: the pane's own moves (a render, a resize, the
// other side being followed) are held, and a move that leaves a side where
// it was is no move. The block holding the source's caret line gets a bar
// at the page's left, as VS Code's preview marks it.
//
// The source's text view and scroll view (the editor's, which every tab
// shares) and its lines: the line of a character, the start of a line and
// the line count, from the editor's line index.
- (void)followSource:(NSTextView *)source
              scroll:(NSScrollView *)scroll
     lineOfCharacter:(NSUInteger (^)(NSUInteger index))lineOf
         startOfLine:(NSUInteger (^)(NSUInteger line))startOf
           lineCount:(NSUInteger (^)(void))lineCount;
// The attribute every rendered run carries its source line in.
@property(nonatomic, copy) NSAttributedStringKey sourceLineKey;
// Whether the page on show is the source's, so the two follow each other.
@property(nonatomic, copy) BOOL (^following)(void);
// The user last scrolled the page, not the source: a render keeps the
// page's place rather than following the source.
@property(nonatomic) BOOL pageLeads;
// Runs `block` (a render, or putting the place back after one) without
// the page's moves leading the source.
- (void)holdFollowing:(void (^)(void))block;
// A new page is on show: its map is made again, its block marked, and,
// unless the page leads, it goes to the source's top line.
- (void)pageRendered;
// The source's top line (its fraction is how far down that line the
// source's top is), and the line the page's top shows.
@property(nonatomic, readonly) double sourceTopLine;
@property(nonatomic, readonly) double pageTopLine;
// Each to the other.
- (void)scrollPageToSource;
- (void)scrollSourceToPage;
// The page's block marked for the source's caret line, and its bar (in
// the page's text view's coordinates); empty for none.
@property(nonatomic, readonly) NSRange markedRange;
@property(nonatomic, readonly) NSRect markRect;
// Marks the block of the source's caret line again (the caret moved).
- (void)markCaretLine;

// A double-click in the page (its character and point, in the text view's
// coordinates); YES when handled, as the full preview's block editor does.
@property(nonatomic, copy) BOOL (^onDoubleClick)(NSUInteger index, NSPoint point);
@end
