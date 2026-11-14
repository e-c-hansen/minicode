// FileTree.mm — see FileTree.h.
#import "FileTree.h"
#import "QuickLook.h"
#include <atomic>
#include <mutex>
#include <sys/stat.h>

BOOL gShowHidden = NO;

@implementation MCTreeEntry
@end

MCTreeEntry *MCTreeStat(NSString *path) {
    MCTreeEntry *e = [MCTreeEntry new];
    e.path = path;
    struct stat lst, st;
    const char *p = path.fileSystemRepresentation;
    const BOOL haveL = p && lstat(p, &lst) == 0;
    const BOOL have = p && stat(p, &st) == 0;
    e.isLink = haveL && S_ISLNK(lst.st_mode);
    e.isDir = have && S_ISDIR(st.st_mode);
    const struct stat &use = have ? st : lst;
    e.size = have && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
    e.modified = (have || haveL)
        ? (double)use.st_mtimespec.tv_sec + use.st_mtimespec.tv_nsec / 1e9 : 0;
    e.created = (have || haveL)
        ? (double)use.st_birthtimespec.tv_sec + use.st_birthtimespec.tv_nsec / 1e9 : 0;
    return e;
}

namespace {
std::mutex gListedMutex;
std::atomic<NSUInteger> gListings{0};
NSMutableArray<NSString *> *gListed;   // under gListedMutex
}  // namespace

NSUInteger MCTreeListingCount(void) { return gListings.load(); }
NSArray<NSString *> *MCTreeListedFolders(void) {
    std::lock_guard<std::mutex> lock(gListedMutex);
    return gListed ? [gListed copy] : @[];
}

NSArray<MCTreeEntry *> *MCTreeList(NSString *path, BOOL showHidden) {
    gListings++;
    {
        std::lock_guard<std::mutex> lock(gListedMutex);
        if (!gListed) gListed = [NSMutableArray array];
        [gListed addObject:path ?: @""];
        if (gListed.count > 500) [gListed removeObjectsInRange:NSMakeRange(0, gListed.count - 500)];
    }
    NSArray<NSString *> *names = path ? [[NSFileManager defaultManager]
        contentsOfDirectoryAtPath:path error:nil] : nil;
    if (!names) return nil;
    names = [names sortedArrayUsingComparator:^(NSString *a, NSString *b) {
        return [a caseInsensitiveCompare:b];
    }];
    NSMutableArray<MCTreeEntry *> *dirs = [NSMutableArray array];
    NSMutableArray<MCTreeEntry *> *files = [NSMutableArray array];
    for (NSString *name in names) {
        if (!showHidden && [name hasPrefix:@"."]) continue;   // hide dotfiles
        MCTreeEntry *e = MCTreeStat([path stringByAppendingPathComponent:name]);
        [(e.isDir ? dirs : files) addObject:e];                // dirs first
    }
    [dirs addObjectsFromArray:files];
    return dirs;
}

@implementation FileItem
- (NSString *)name { return self.path.lastPathComponent; }

- (void)readStat {
    MCTreeEntry *e = MCTreeStat(self.path);
    self.isDir = e.isDir;
    [self takeStat:e];
}

- (void)takeStat:(MCTreeEntry *)e {
    self.isLink = e.isLink;
    self.size = e.size;
    self.modified = e.modified;
    self.created = e.created;
}

- (void)adoptListing:(NSArray<MCTreeEntry *> *)entries {
    NSMutableDictionary<NSString *, FileItem *> *existing = [NSMutableDictionary dictionary];
    for (FileItem *c in self.children) existing[c.path] = c;
    NSMutableArray<FileItem *> *children = [NSMutableArray arrayWithCapacity:entries.count];
    long long rank = 0;
    for (MCTreeEntry *e in entries) {
        FileItem *item = existing[e.path];
        if (!item || item.isDir != e.isDir) {   // else the same node, so expansion stays
            item = [FileItem new];
            item.path = e.path;
            item.isDir = e.isDir;
        }
        [item takeStat:e];
        item.parent = self;
        item.nameRank = rank++;
        [children addObject:item];
    }
    self.children = children;
    self.loaded = YES;
    self.shown = nil;
}

- (void)loadChildren {
    if (self.loaded || !self.isDir) return;
    [self adoptListing:MCTreeList(self.path, gShowHidden) ?: @[]];
}

// Re-scan this directory, reusing existing child objects for paths that still
// exist so the outline keeps its expanded and selected state. Recurses into
// any subdirectory that was already loaded.
- (void)refresh {
    if (!self.isDir || !self.loaded) return;
    [self adoptListing:MCTreeList(self.path, gShowHidden) ?: @[]];
    for (FileItem *c in self.children)           // recurse into open subdirs
        if (c.isDir && c.loaded) [c refresh];
}
@end

@implementation ClickOutline {
    NSTimeInterval _typedAt;   // the last letter typed to the tree, for type-select
    NSInteger _lastPressRow;   // the row the press before this one was on
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) _lastPressRow = -1;
    return self;
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    _clickCount = event.clickCount;
    _sameRowAsLastPress = row >= 0 && row == _lastPressRow;
    _commandClick = (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    _lastPressRow = row;
    // The native tracking runs first (selection, expansion, and a drag once
    // the mouse moves), and returns at the release or when a drag starts.
    // A press that became a drag opens nothing; any other is a click.
    self.dragBegan = NO;
    [super mouseDown:event];
    // An open Quick Look panel follows a click as it follows the arrows.
    if (!self.dragBegan) [[MCQuickLookPanel shared] followURLs:[self selectedFileURLs]];
    if (row >= 0 && !self.dragBegan && self.onRowClick) self.onRowClick(row, p);
}
- (void)keyDown:(NSEvent *)event {
    // F2 (rename) and Cmd+Delete (Move to Trash) are the registry's tree
    // commands (renameFile, moveFileToTrash in Commands.cpp), which the key
    // monitor runs before the tree sees the key, so keys.conf can move them.
    NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift |
         NSEventModifierFlagOption | NSEventModifierFlagControl);
    // Space toggles Quick Look on the selection, as in Finder; Esc closes it.
    // A space in the middle of a name typed to find a row ("my file") is
    // type-select's, as it is in Finder.
    if (mods == 0 && event.keyCode == 49 /* kVK_Space */) {
        const NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
        if (_typedAt > 0 && now - _typedAt < 1.0) {
            _typedAt = now;
            [super keyDown:event];
            return;
        }
        [[MCQuickLookPanel shared] toggleWithURLs:[self selectedFileURLs] from:self];
        return;
    }
    if (mods == 0 && event.keyCode == 53 /* kVK_Escape */ && [MCQuickLookPanel shared].shown) {
        [[MCQuickLookPanel shared] close];
        return;
    }
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length == 1) {
        unichar c = [chars characterAtIndex:0];
        if ((c == NSCarriageReturnCharacter || c == NSEnterCharacter) &&
            self.onActivate) {
            self.onActivate(self.selectedRow);
            return;
        }
    }
    if (mods == 0 && chars.length == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c > ' ' && c != 0x7F && !(c >= 0xF700 && c <= 0xF8FF))
            _typedAt = NSDate.timeIntervalSinceReferenceDate;
        else _typedAt = 0;
    }
    [super keyDown:event];   // arrows, left/right expand-collapse stay native
    [[MCQuickLookPanel shared] followURLs:[self selectedFileURLs]];   // when open
}

- (NSArray<NSURL *> *)selectedFileURLs {
    NSMutableArray<NSURL *> *urls = [NSMutableArray array];
    [self.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger row, BOOL *) {
        FileItem *item = [self itemAtRow:(NSInteger)row];
        if ([item isKindOfClass:FileItem.class] && item.path)
            [urls addObject:[NSURL fileURLWithPath:item.path]];
    }];
    return urls;
}

// The Quick Look panel asks the responder chain who drives it (QuickLook.h).
- (BOOL)acceptsPreviewPanelControl:(QLPreviewPanel *)panel {
    return [[MCQuickLookPanel shared] acceptsControl];
}
- (void)beginPreviewPanelControl:(QLPreviewPanel *)panel {
    [[MCQuickLookPanel shared] beginControl:panel];
}
- (void)endPreviewPanelControl:(QLPreviewPanel *)panel {
    [[MCQuickLookPanel shared] endControl:panel];
}
@end
