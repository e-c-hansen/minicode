// FileTree.mm — see FileTree.h.
#import "FileTree.h"
#import "QuickLook.h"
#include <sys/stat.h>

BOOL gShowHidden = NO;

@implementation FileItem
- (NSString *)name { return self.path.lastPathComponent; }

// The entry's kind, size and date. A symbolic link shows what it points to
// (a link to a folder is a folder, as before); one that points nowhere is a
// file of unknown size.
- (void)readStat {
    struct stat lst, st;
    const char *p = self.path.fileSystemRepresentation;
    const BOOL haveL = lstat(p, &lst) == 0;
    const BOOL have = stat(p, &st) == 0;
    self.isLink = haveL && S_ISLNK(lst.st_mode);
    self.isDir = have && S_ISDIR(st.st_mode);
    const struct stat &use = have ? st : lst;
    self.size = have && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
    self.modified = (have || haveL)
        ? (double)use.st_mtimespec.tv_sec + use.st_mtimespec.tv_nsec / 1e9 : 0;
    self.created = (have || haveL)
        ? (double)use.st_birthtimespec.tv_sec + use.st_birthtimespec.tv_nsec / 1e9 : 0;
}

// The folder's entries in name order, folders first: the order the tree
// has always had, and the base every other order starts from.
- (NSMutableArray<FileItem *> *)listReusing:(NSDictionary<NSString *, FileItem *> *)existing {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *entries = [[fm contentsOfDirectoryAtPath:self.path error:nil]
        sortedArrayUsingComparator:^(NSString *a, NSString *b) {
            return [a caseInsensitiveCompare:b];
        }];
    NSMutableArray *dirs = [NSMutableArray array];
    NSMutableArray *files = [NSMutableArray array];
    long long rank = 0;
    for (NSString *entry in entries) {
        if (!gShowHidden && [entry hasPrefix:@"."]) continue;   // hide dotfiles
        NSString *full = [self.path stringByAppendingPathComponent:entry];
        FileItem *item = [FileItem new];
        item.path = full;
        [item readStat];
        FileItem *old = existing[full];
        if (old && old.isDir == item.isDir) {   // the same node, so expansion stays
            old.isLink = item.isLink;
            old.size = item.size;
            old.modified = item.modified;
            old.created = item.created;
            item = old;
        }
        item.nameRank = rank++;
        [(item.isDir ? dirs : files) addObject:item];          // dirs first
    }
    NSMutableArray<FileItem *> *children = [NSMutableArray arrayWithArray:dirs];
    [children addObjectsFromArray:files];
    return children;
}

- (void)loadChildren {
    if (self.loaded || !self.isDir) return;
    self.loaded = YES;
    self.children = [self listReusing:@{}];
    self.shown = nil;
}

// Re-scan this directory, reusing existing child objects for paths that still
// exist so the outline keeps its expanded and selected state. Recurses into
// any subdirectory that was already loaded.
- (void)refresh {
    if (!self.isDir || !self.loaded) return;
    NSMutableDictionary<NSString *, FileItem *> *existing =
        [NSMutableDictionary dictionary];
    for (FileItem *c in self.children) existing[c.path] = c;
    self.children = [self listReusing:existing];
    self.shown = nil;
    for (FileItem *c in self.children)           // recurse into open subdirs
        if (c.isDir && c.loaded) [c refresh];
}
@end

@implementation ClickOutline {
    NSTimeInterval _typedAt;   // the last letter typed to the tree, for type-select
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
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
    // Cmd+Delete trashes the selected file only while the tree has the
    // keyboard, as in Finder. It used to be the menu item's key, so it
    // trashed the tree's selection from the editor too, where Cmd+Delete
    // means delete to the start of the line.
    NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift |
         NSEventModifierFlagOption | NSEventModifierFlagControl);
    if (mods == NSEventModifierFlagCommand && (event.keyCode == 51 || event.keyCode == 117) &&
        self.onTrash) {
        self.onTrash();
        return;
    }
    // F2 renames the selected file, as in VS Code; in the editor it is
    // Rename Symbol (the menu item, which also sends a tree's F2 here).
    if (mods == 0 && event.keyCode == 120 /* kVK_F2 */ && self.onRename) {
        self.onRename();
        return;
    }
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
