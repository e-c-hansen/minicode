// TabStrip.h — the row of open files above the editor. It only draws and
// reports clicks; EditorController owns the tabs (see EditorTabs.h for the
// rules it follows).
#import <Cocoa/Cocoa.h>

// How tall the strip is, in points.
extern const CGFloat kMCTabStripHeight;

// A dot before a tab's title: a terminal waiting for the user (orange) or
// done (green), from `minicode notify` (TerminalAttention.h).
typedef NS_ENUM(NSInteger, MCTabAttention) {
    MCTabAttentionNone = 0,
    MCTabAttentionWaiting,
    MCTabAttentionDone,
};

// What one tab shows.
@interface MCTabStripItem : NSObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *toolTip;   // the whole path
@property(nonatomic, assign) BOOL dirty;        // a dot in place of the close button
@property(nonatomic, assign) BOOL preview;      // the agent preview tab: an italic title
@property(nonatomic, assign) MCTabAttention attention;
@end

@interface MCTabStrip : NSView
@property(nonatomic, copy) NSArray<MCTabStripItem *> *items;
@property(nonatomic, assign) NSInteger activeIndex;   // -1 for none
// A click on a tab, and a click on its close button or a middle click on it.
@property(nonatomic, copy) void (^onSelect)(NSInteger index);
@property(nonatomic, copy) void (^onClose)(NSInteger index);
// A double-click on a tab's title (keeps the agent preview tab).
@property(nonatomic, copy) void (^onDoubleClick)(NSInteger index);
// A right click on a tab: its own menu (Adjust Settings… is added after it).
// Without one, or off the tabs, the strip's menu is only Adjust Settings….
@property(nonatomic, copy) NSMenu *(^menuForTab)(NSInteger index);
// Colors: the strip behind the tabs, the active tab (the editor's own
// background, so it runs into the editor), and the two text colors.
@property(nonatomic, strong) NSColor *stripColor, *activeColor, *activeText, *inactiveText;
// The appearance surface its right-click menu offers to adjust ("tabs" or
// "terminal-tabs", see Settings::appearanceSurfaces); nil for no menu.
@property(nonatomic, copy) NSString *surface;
// Where a tab and its close button are drawn, in the strip's coordinates
// (flipped). Empty when the tab is scrolled out of sight.
- (NSRect)rectForTab:(NSInteger)index;
- (NSRect)closeRectForTab:(NSInteger)index;
// Where a tab's attention dot is drawn; empty when it has none or is out of sight.
- (NSRect)attentionRectForTab:(NSInteger)index;
// The dots' colors.
+ (NSColor *)waitingColor;
+ (NSColor *)doneColor;
@end
