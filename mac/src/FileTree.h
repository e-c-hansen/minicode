// FileTree.h — the file tree's nodes (FileItem) and its outline view
// (ClickOutline), which EditorController drives.
#import <Cocoa/Cocoa.h>


// ---------------------------------------------------------------------------
// MCTreeEntry: one entry of a folder as read from the disk, a plain value
// made on any thread, so a folder can be listed off the main thread and the
// result handed to its FileItem there.
// ---------------------------------------------------------------------------
@interface MCTreeEntry : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, assign) BOOL isDir;
@property(nonatomic, assign) BOOL isLink;
@property(nonatomic, assign) long long size;
@property(nonatomic, assign) double modified;
@property(nonatomic, assign) double created;
@end

// The entry's kind, size and dates. A symbolic link shows what it points to
// (a link to a folder is a folder); one that points nowhere is a file of
// unknown size. Any thread.
MCTreeEntry *MCTreeStat(NSString *path);
// A folder's entries in name order, folders first: the order the tree has
// always had. Dotfiles only with `showHidden`. nil when the folder cannot be
// read (gone, not a folder, no permission). Any thread.
NSArray<MCTreeEntry *> *MCTreeList(NSString *path, BOOL showHidden);
// For tests: how many folders MCTreeList has read since launch, and the
// last 500 of them, oldest first.
NSUInteger MCTreeListingCount(void);
NSArray<NSString *> *MCTreeListedFolders(void);

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
@property(nonatomic, weak) FileItem *parent;     // the folder that listed it
@property(nonatomic, assign) BOOL loaded;
@property(nonatomic, strong) NSArray<FileItem *> *shown;   // children in the order on show
@property(nonatomic, assign) NSUInteger shownGeneration;
// The rest of the file uses these (see FileTree.mm).
- (NSString *)name;
- (void)readStat;
- (void)loadChildren;
- (void)refresh;
// Takes a listing of this folder (MCTreeList's), reusing the child of each
// path that is still there with the same kind, so the outline keeps its
// expanded and selected state. Marks the folder loaded.
- (void)adoptListing:(NSArray<MCTreeEntry *> *)entries;
// Takes a fresh stat of this entry (its kind is kept: a change of kind
// shows when its folder is listed again).
- (void)takeStat:(MCTreeEntry *)entry;
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
// The click onRowClick is called for: its count, whether the press before
// it was on the same row (the window server counts clicks by time alone, so
// a quick click on another row comes as a double-click), and whether
// Command was held.
@property(nonatomic, readonly) NSInteger clickCount;
@property(nonatomic, readonly) BOOL sameRowAsLastPress;
@property(nonatomic, readonly) BOOL commandClick;
@end
