// TreeDetails.h — Objective-C++. Sizes and dates in the file tree (Mac):
// the formatting, the switch that shows them (Shift+Cmd+I, app-wide), the
// background counting of folder totals, and the row view that draws a name
// with its size and age at the right. The rules that can be tested (sort
// orders, ages, the totals cache) are plain C++ in FileDetails.h.
#import <Cocoa/Cocoa.h>
#include "FileDetails.h"

// The user defaults MiniCode keeps its own state in: a scratch suite when
// MINICODE_DEFAULTS_SUITE is set (tests), else the standard ones.
NSUserDefaults *MCAppDefaults(void);

// Shift+Cmd+I. Remembered across launches, the same for every window;
// changing it posts MCTreeDetailsDidChangeNotification.
BOOL MCTreeDetailsShown(void);
void MCSetTreeDetailsShown(BOOL shown);
extern NSNotificationName const MCTreeDetailsDidChangeNotification;

// The tree's sort order for a folder (by its resolved path), Name A to Z
// with folders first unless chosen otherwise there.
TreeSort::Options MCTreeSortForRoot(NSString *root);
void MCSetTreeSortForRoot(NSString *root, const TreeSort::Options &options);

// "4 KB", "1.2 MB": NSByteCountFormatter's file style, as Finder shows sizes.
NSString *MCFormatByteCount(long long bytes);
// A row's age: "now", "5 min", "3 h", "yesterday", else a short date
// ("12 Oct", or "Oct 2024" in another year).
NSString *MCCompactAge(NSDate *date, NSDate *now);
// The full date and time, for tooltips.
NSString *MCFullDate(NSDate *date);
// The status bar's "Saved 14:32" (see Age::saved).
NSString *MCSavedPhrase(NSDate *date, NSDate *now);

// Folder totals for one window, counted on a background queue (one at a
// time, the most recently asked first), cached per folder by
// FolderSizes::SizeCache. Paths are as the tree spells them.
@interface MCFolderSizer : NSObject
// Called on the main thread with the folders whose totals just arrived,
// gathered over a tenth of a second.
@property(nonatomic, copy) void (^onTotals)(NSSet<NSString *> *paths);
// The total if it is known and current. Otherwise the last one known for
// the folder, if any, with *stale set, so a row does not flicker back to
// "…" while it is counted again.
- (BOOL)totalFor:(NSString *)path total:(FolderSizes::Total *)total stale:(BOOL *)stale;
- (void)request:(NSString *)path;
// FSEvents said something changed in `path`: it and every folder above it
// are counted again when asked (and everything below it too with subtree).
- (void)invalidate:(NSString *)path subtree:(BOOL)subtree;
// Forget everything, and stop the count under way (Refresh, a new folder).
- (void)reset;
// Stop for good: the window is closing.
- (void)cancel;
@property(nonatomic, readonly) BOOL busy;   // for tests
@end

// A row of the tree: icon, name, and with details on, the size and age
// right-aligned in two fixed columns, muted. The name is truncated before
// them, and the age goes first, then the size, when the row is narrow.
@interface MCTreeCell : NSTableCellView
@property(nonatomic, strong, readonly) NSTextField *sizeField, *ageField;
- (void)setSize:(NSString *)size age:(NSString *)age color:(NSColor *)color;
@end

// Shift+F10, Ctrl+Return, or the PC keyboard's Menu key: the context menu of
// what has the keyboard in `window`, at the selected row of a table or
// outline, at the caret of a text view, else near the view's top left.
// Opened after the key event is done, through +[NSMenu
// popUpContextMenu:withEvent:forView:], so it is navigable by keyboard.
// Returns NO, and beeps, when there is no menu to show.
BOOL MCShowContextMenuInWindow(NSWindow *window);
