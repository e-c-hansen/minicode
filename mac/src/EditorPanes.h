// EditorPanes.h — the plain views the editor window is built from: a
// container that lays itself out by hand, the bars that resize the panes,
// and a scroll view with a see-through panel color.
#import <Cocoa/Cocoa.h>

// A plain container that relays every resize to a layout block, so we can
// dock the editor/browser/terminal manually (predictable, no split-view math).
@interface PanelHost : NSView
@property(nonatomic, copy) void (^onLayout)(void);
@end

// Drag handle for resizing the terminal dock (a horizontal bar) or the
// editor beside the browser (a vertical one). The view is a generous grab
// area centered on the boundary but draws only a 1px line, which lights up
// while hovered or dragged. Reports the new boundary Y, or X for a vertical
// bar (in its superview's coordinates), to a block, keeping the offset where
// the bar was grabbed so it doesn't jump. The host must keep it above
// neighboring views, or they take the clicks.
@interface DragBar : NSView
@property(nonatomic, copy) void (^onDrag)(CGFloat inSuperview);
@property(nonatomic) BOOL vertical;   // a boundary between left and right
@end
static const CGFloat kDragBarHeight = 12;

// A scroll view whose panel background is a plain layer color behind the clip
// view, which itself draws nothing. A clip view's own translucent
// backgroundColor comes out opaque; a layer color composites correctly over
// the blur or the desktop. NSScrollView lays out only its own parts, so the
// backdrop is sized in tile.
@interface PanelScrollView : NSScrollView
@property(nonatomic, strong) NSColor *panelColor;
@end
