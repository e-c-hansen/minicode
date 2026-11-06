// FileTree.h — the file tree's nodes (FileItem) and its outline view
// (ClickOutline), which EditorController drives.
#import <Cocoa/Cocoa.h>


// ---------------------------------------------------------------------------
// FileItem: one node in the file tree. Children load lazily on expansion.
// `children` is in name order, folders first, exactly as the tree always
// listed them; the order on show (`shown`) comes from the window's sort
// options (see childrenOf: in EditorController).
// ---------------------------------------------------------------------------
@interface FileItem : NSObject
@property(nonatomic, copy)   NSString *path;
@property(nonatomic, assign) BOOL isDir;
@property(nonatomic, assign) BOOL isLink;        // a symbolic link (to a folder or a file)
@property(nonatomic, assign) long long size;     // a file's bytes; -1 when unknown
@property(nonatomic, assign) double modified;    // seconds since 1970; 0 when unknown
@property(nonatomic, assign) double created;     // its birth time, the same way
@property(nonatomic, assign) long long nameRank; // its place in name order among its siblings
@property(nonatomic, strong) NSMutableArray<FileItem *> *children;
@property(nonatomic, assign) BOOL loaded;
@property(nonatomic, strong) NSArray<FileItem *> *shown;   // children in the order on show
@property(nonatomic, assign) NSUInteger shownGeneration;
// The rest of the file uses these (see FileTree.mm).
- (NSString *)name;
- (void)readStat;
- (void)loadChildren;
- (void)refresh;
@end

// When NO, entries beginning with "." are hidden from the tree (default).
extern BOOL gShowHidden;

// ---------------------------------------------------------------------------
// ClickOutline: handles a click in mouseDown directly, so opening a file never
// depends on NSOutlineView's internal selection/tracking loop (which was
// swallowing single clicks in this layout).
// ---------------------------------------------------------------------------
@interface ClickOutline : NSOutlineView
@property(nonatomic, copy) void (^onRowClick)(NSInteger row, NSPoint pointInView);
@property(nonatomic, copy) void (^onActivate)(NSInteger row);   // Return / Enter
@property(nonatomic, assign) BOOL dragBegan;   // set when a row is dragged
@end
