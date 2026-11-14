// QuickLook.h — Quick Look in minicode, two ways:
//
//   - MCQuickLookSlot: a read-only preview of a file in the editor's slot
//     (QLPreviewView), for anything minicode has no viewer of its own for:
//     Office and iWork documents, archives, fonts, binaries (which Quick
//     Look shows as an icon with the name and size, as Finder does).
//   - MCQuickLookPanel: the floating Quick Look panel (QLPreviewPanel) that
//     Space on a file in the tree toggles, as in Finder. The tree keeps the
//     keyboard, so Space again or Esc closes it and the arrows move through
//     the files while it is open.
//
// The panel is driven through the responder chain as QuickLook asks: the
// view with the keyboard answers acceptsPreviewPanelControl: while the panel
// is open and hands the panel to MCQuickLookPanel in begin/end.
#import <Cocoa/Cocoa.h>
#import <Quartz/Quartz.h>

@interface MCQuickLookSlot : NSView
@property(nonatomic, readonly) QLPreviewView *preview;
@property(nonatomic, readonly, copy) NSString *path;   // what it shows, or nil
- (void)showPath:(NSString *)path;
- (void)clear;   // lets go of the file (another tab, or nothing)
@end

@interface MCQuickLookPanel : NSObject <QLPreviewPanelDataSource, QLPreviewPanelDelegate>
+ (instancetype)shared;
// Open (as minicode asked for it, not whether the window server shows it).
@property(nonatomic, readonly) BOOL shown;
@property(nonatomic, readonly, copy) NSArray<NSURL *> *items;
// Space in `owner` (a tree): open on `urls` (when there are any), or close
// when open for that owner. Open for another window's tree, or left with
// no view driving it (the keyboard went to the editor), it shows `urls`
// for this owner instead of closing.
- (void)toggleWithURLs:(NSArray<NSURL *> *)urls from:(id)owner;
// The selection moved while the panel is open: it follows.
- (void)followURLs:(NSArray<NSURL *> *)urls;
- (void)close;
// For the view with the keyboard (QLPreviewPanelController).
- (BOOL)acceptsControl;
- (void)beginControl:(QLPreviewPanel *)panel;
- (void)endControl:(QLPreviewPanel *)panel;
@end
