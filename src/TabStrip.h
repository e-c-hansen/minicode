// TabStrip.h — the row of open files above the editor. It only draws and
// reports clicks; EditorController owns the tabs (see EditorTabs.h for the
// rules it follows).
#import <Cocoa/Cocoa.h>

// How tall the strip is, in points.
extern const CGFloat kMCTabStripHeight;

// What one tab shows.
@interface MCTabStripItem : NSObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *toolTip;   // the whole path
@property(nonatomic, assign) BOOL dirty;        // a dot in place of the close button
@end

@interface MCTabStrip : NSView
@property(nonatomic, copy) NSArray<MCTabStripItem *> *items;
@property(nonatomic, assign) NSInteger activeIndex;   // -1 for none
// A click on a tab, and a click on its close button or a middle click on it.
@property(nonatomic, copy) void (^onSelect)(NSInteger index);
@property(nonatomic, copy) void (^onClose)(NSInteger index);
// Colors: the strip behind the tabs, the active tab (the editor's own
// background, so it runs into the editor), and the two text colors.
@property(nonatomic, strong) NSColor *stripColor, *activeColor, *activeText, *inactiveText;
// Where a tab and its close button are drawn, in the strip's coordinates
// (flipped). Empty when the tab is scrolled out of sight.
- (NSRect)rectForTab:(NSInteger)index;
- (NSRect)closeRectForTab:(NSInteger)index;
@end
