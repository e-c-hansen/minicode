// FileViewers.h — the viewers that take the editor's slot for files that
// are not plain text, beyond the image, PDF and media viewers in
// EditorController: Quick Look for anything minicode has no viewer for.
// EditorController keeps one MCFileViewers per window and calls it from a
// few hooks (loading and showing a tab, laying out the slot, letting go);
// which viewer a file gets comes from FileSniff, by content first.
#import <Cocoa/Cocoa.h>

@class MCQuickLookSlot;

typedef NS_ENUM(NSInteger, MCViewerKind) {
    MCViewerNone = 0,      // the text view, as before
    MCViewerQuickLook,     // read-only, no text: QLPreviewView
};

// The viewer for a file, from its first bytes and its name. Images, PDFs,
// video and audio are routed before this is asked.
MCViewerKind MCViewerKindForFile(NSString *path);
// A viewer of a file that has no text in the tab (nothing to save).
BOOL MCViewerKindWithoutText(MCViewerKind kind);

@interface MCFileViewers : NSObject
// Views are made on first use, as subviews of `container` below `sibling`.
- (instancetype)initWithContainer:(NSView *)container below:(NSView *)sibling;
@property(nonatomic, readonly) MCViewerKind shownKind;
@property(nonatomic, readonly) MCQuickLookSlot *quickLook;   // nil until used

// A viewer with no text behind it; NO when it cannot show the file.
- (BOOL)showWithoutText:(MCViewerKind)kind path:(NSString *)path;
// Nothing shown any more (another tab, another kind of file).
- (void)clear;
// Lays the viewers out in the editor's slot and returns what is left of it
// for the text view; *coversText is YES when a viewer takes all of it.
- (NSRect)layoutInRect:(NSRect)rect editorShown:(BOOL)shown coversText:(BOOL *)coversText;
// The view that should get the keyboard (Cmd+1), or nil.
- (NSView *)focusView;
@end
