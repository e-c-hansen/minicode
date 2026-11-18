// TreeFocusBar.h — the slim row across the top of the file tree while it
// is focused (EditorController+TreeFocus.mm). On one folder (Focus on
// Folder) it is a breadcrumb, "minicode › mac › src", then a ×: a click on
// a part roots the tree there, the first part or the × shows the whole
// folder again. On several (Add to Focus) it is one small label per folder
// ("mac/src ×  linux/src ×"), then a ×: a click on a label shows that
// folder's root row, its × takes the folder out of the focus, and the last
// × shows the whole folder. The parts, which of them fit, and the labels'
// short forms and widths are the core's (FileDetails.h, TreeFocus::
// breadcrumb, fit, shortLabels and shared); this only draws them and
// reports clicks.
#import <Cocoa/Cocoa.h>

static const CGFloat kMCTreeFocusBarHeight = 24;

@interface MCTreeFocusBar : NSView
// A part was clicked: the focus it stands for ("" for the whole folder, as
// the first part and the × give).
@property(nonatomic, copy) void (^onPick)(NSString *focus);
// The sidebar's text color; the separators, the × and the line below take
// it dimmed.
@property(nonatomic, strong) NSColor *textColor;
// Several folders: a label's × was clicked (the folder), or the label
// itself (to show the folder's root row).
@property(nonatomic, copy) void (^onRemove)(NSString *folder);
@property(nonatomic, copy) void (^onShowFolder)(NSString *folder);
// The window's folder's name and the focus ("mac/src"): the breadcrumb.
- (void)setRootName:(NSString *)name focus:(NSString *)focus;
// The focused folders, in their order: the label row.
- (void)setFolders:(NSArray<NSString *> *)folders;
// Whether it shows the label row (several folders), not the breadcrumb.
@property(nonatomic, readonly) BOOL showsFolders;
// For the checks: every part's label (the breadcrumb's parts, or the
// folders' labels as drawn), and where a part (by its index in those),
// a label's × and the last × are drawn, in the bar's coordinates
// (NSZeroRect for a part left out for want of room).
@property(nonatomic, readonly) NSArray<NSString *> *partLabels;
- (NSRect)rectOfPart:(NSInteger)index;
- (NSRect)removeRectOfPart:(NSInteger)index;
- (NSRect)closeRect;
@end
