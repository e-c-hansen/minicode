// SidebarIcons.h — Objective-C++. The sidebar's row of icons
// (workbench.sidebar-icons): one per panel that takes the sidebar's place,
// the file tree, Source Control, Agents, and Processes, so they can be seen
// and clicked as VS Code's and Zed's can, not only reached by their keys.
//
// MCSidebarHost is the split view's first subview for the window's life:
// the row at its top and, under it, whichever panel is on show (`content`,
// swapped where replaceSubview: swapped the split view's subview before).
// The row is quiet: template symbols in the sidebar's text color, dimmed
// but for the panel on show, which is also underlined in the accent; a
// count of changes on Source Control, an orange dot on Agents while a
// session waits, and a count of running servers on Processes. Each icon's
// tooltip names its panel and key. The row paints the sidebar's color once,
// as a plain layer color (translucency), and hides with the sidebar (a
// host narrower than a point) or with the setting off. It draws only:
// EditorController+SidebarIcons.mm says what each icon shows and does.
#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, MCSidebarPanel) {
    MCSidebarPanelTree = 0,
    MCSidebarPanelSourceControl,
    MCSidebarPanelAgents,
    MCSidebarPanelProcesses,
};

extern const CGFloat kMCSidebarIconBarHeight;

@interface MCSidebarIconBar : NSView
// A click on an icon (its tag is the MCSidebarPanel).
@property(nonatomic, copy) void (^onPick)(MCSidebarPanel panel);
// The panel on show, marked.
@property(nonatomic) MCSidebarPanel current;
- (void)setBackground:(NSColor *)background text:(NSColor *)text;
// A count on the icon ("3", "99+"); nil or "" for none.
- (void)setBadge:(NSString *)badge forPanel:(MCSidebarPanel)panel;
- (void)setDot:(BOOL)dot forPanel:(MCSidebarPanel)panel;
- (void)setTip:(NSString *)tip forPanel:(MCSidebarPanel)panel;

// For the GUI checks: the four buttons, in panel order, and what each shows.
@property(nonatomic, readonly) NSArray<NSButton *> *buttons;
- (NSString *)badgeForPanel:(MCSidebarPanel)panel;   // "" for none
- (BOOL)dotForPanel:(MCSidebarPanel)panel;
@end

@interface MCSidebarHost : NSView
@property(nonatomic, readonly) MCSidebarIconBar *iconBar;
// The panel on show under the row; setting it removes the one before.
@property(nonatomic, strong) NSView *content;
// workbench.sidebar-icons; with it off the panel takes the whole height.
@property(nonatomic) BOOL showsIcons;
// Whether the row is on screen now (on, and the sidebar shown).
@property(nonatomic, readonly) BOOL iconsShown;
@end
