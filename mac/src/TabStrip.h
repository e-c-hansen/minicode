// TabStrip.h — the row of open files above the editor, and of terminals
// above the terminal dock. It only draws and reports clicks and drags; its
// owner keeps the tabs (see EditorTabs.h for the rules the editor follows).
#import <Cocoa/Cocoa.h>

// The titles' font: tabs.font-size points (12 when unset), the same for
// every strip (the editor's, the split group's, the browser's and the
// terminals'), so a change in the settings file reaches all of them.
NSFont *MCTabStripFont(void);
// How tall a strip is, in points: 30, or more when the titles' font needs
// it. Owners lay strips out at this height again on a settings change.
CGFloat MCTabStripHeight(void);

// A dot before a tab's title: a terminal waiting for the user (orange) or
// done (green), from `minicode notify` (TerminalAttention.h); an editor tab
// holding an agent's note not yet seen (blue, the notes' color: the note
// opened beside a user who was typing, AgentNotes.h).
typedef NS_ENUM(NSInteger, MCTabAttention) {
    MCTabAttentionNone = 0,
    MCTabAttentionWaiting,
    MCTabAttentionDone,
    MCTabAttentionNote,
};

// What one tab shows.
@interface MCTabStripItem : NSObject
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *toolTip;   // the whole path
@property(nonatomic, assign) BOOL dirty;        // a dot in place of the close button
@property(nonatomic, assign) BOOL preview;      // a preview tab (the user's or an agent's): an italic title
@property(nonatomic, assign) MCTabAttention attention;
@end

@interface MCTabStrip : NSView
@property(nonatomic, copy) NSArray<MCTabStripItem *> *items;
@property(nonatomic, assign) NSInteger activeIndex;   // -1 for none
// A click on a tab, and a click on its close button or a middle click on it.
@property(nonatomic, copy) void (^onSelect)(NSInteger index);
@property(nonatomic, copy) void (^onClose)(NSInteger index);
// A double-click on a tab's title (keeps a preview tab).
@property(nonatomic, copy) void (^onDoubleClick)(NSInteger index);
// A tab dragged along the strip (past 4 points) and let go over it: from
// its index to the one it takes (EditorTabs::dropIndex, by the tabs'
// middles). A line marks where it will go meanwhile. nil: tabs do not drag.
@property(nonatomic, copy) void (^onMove)(NSInteger from, NSInteger to);
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
// Where a dragged tab will go (its index after the move), or -1 when no
// drag is under way past its first 4 points.
@property(nonatomic, readonly) NSInteger dropTarget;
// Where a tab's attention dot is drawn; empty when it has none or is out of sight.
- (NSRect)attentionRectForTab:(NSInteger)index;
// The dots' colors.
+ (NSColor *)waitingColor;
+ (NSColor *)doneColor;
+ (NSColor *)noteColor;
@end
