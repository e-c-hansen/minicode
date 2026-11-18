// TreeFocusBar.h — the slim breadcrumb row across the top of the file tree
// while it is focused on a folder (Focus on Folder, EditorController+
// TreeFocus.mm): "minicode › mac › src", then a ×. A click on a part
// roots the tree there, the first part or the × shows the whole folder
// again. The parts, and which of them fit, are the core's (FileDetails.h,
// TreeFocus::breadcrumb and fit); this only draws them and reports clicks.
#import <Cocoa/Cocoa.h>

static const CGFloat kMCTreeFocusBarHeight = 24;

@interface MCTreeFocusBar : NSView
// A part was clicked: the focus it stands for ("" for the whole folder, as
// the first part and the × give).
@property(nonatomic, copy) void (^onPick)(NSString *focus);
// The sidebar's text color; the separators, the × and the line below take
// it dimmed.
@property(nonatomic, strong) NSColor *textColor;
// The window's folder's name and the focus ("mac/src").
- (void)setRootName:(NSString *)name focus:(NSString *)focus;
// For the checks: every part's label, and where a part (by its index in
// those) and the × are drawn, in the bar's coordinates (NSZeroRect for a
// part left out for want of room).
@property(nonatomic, readonly) NSArray<NSString *> *partLabels;
- (NSRect)rectOfPart:(NSInteger)index;
- (NSRect)closeRect;
@end
