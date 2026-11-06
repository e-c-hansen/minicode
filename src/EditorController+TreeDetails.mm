// EditorController+TreeDetails.mm — EditorController: the tree's sizes, dates and order.
#import "EditorController+Private.h"
#import "EditorController+TreeDetailsIvars.h"

@implementation EditorController (TreeDetails)

// ----------------------------------------- sizes, dates and order (the tree)
// Shift+Cmd+I shows each row's size at its right, Option+Shift+Cmd+I when
// it was last saved and Control+Shift+Cmd+I when it was created; the tree's
// context menu sorts it by name, date modified, size or date created. The rules are in
// FileDetails.h, the counting of folder totals in TreeDetails.mm.
- (void)setUpTreeDetails {
    _shownColumns = MCTreeColumnsShown();
    _sizer = [MCFolderSizer new];
    __weak EditorController *weakSelf = self;
    _sizer.onTotals = ^(NSSet<NSString *> *paths) { [weakSelf folderTotalsArrived:paths]; };
    [self rootChanged];
}

// A new root (the window's first, or Open Folder): its own sort order, and
// no totals counted for the old one.
- (void)rootChanged {
    [_sizer reset];
    _treeSort = MCTreeSortForRoot(_root.path);
    _sortGeneration++;
    char real[PATH_MAX];
    _realRoot = _root.path && realpath(_root.path.fileSystemRepresentation, real)
                    ? @(real) : _root.path;
}

// A folder's entries in the order on show, worked out once per change of
// order (or of what is on disk) and kept on the node.
- (NSArray<FileItem *> *)childrenOf:(FileItem *)node {
    [node loadChildren];
    if (node.shown && node.shownGeneration == _sortGeneration) return node.shown;
    node.shown = [self orderOf:node.children];
    node.shownGeneration = _sortGeneration;
    node.shown = [self withWorktreesNode:node.shown of:node];
    return node.shown;
}

- (NSArray<FileItem *> *)orderOf:(NSArray<FileItem *> *)items {
    // Name, A to Z, folders first is the order they were listed in.
    if (_treeSort == TreeSort::Options() || items.count < 2) return items ?: @[];
    const BOOL bySize = _treeSort.key == TreeSort::Key::Size;
    std::vector<TreeSort::Entry> entries;
    entries.reserve(items.count);
    for (FileItem *it in items) {
        TreeSort::Entry e;
        e.name = Utf8String(it.name);
        e.isDir = it.isDir;
        e.modified = it.modified;
        e.created = it.created;
        e.nameRank = it.nameRank;
        e.size = it.size;
        if (it.isDir) {
            e.size = -1;
            if (bySize) {
                FolderSizes::Total t;
                BOOL stale = NO;
                if (it.isLink) e.size = 0;   // not counted, as in the totals
                else if ([_sizer totalFor:it.path total:&t stale:&stale]) {
                    e.size = t.bytes;
                    if (stale) [_sizer request:it.path];
                } else {
                    [_sizer request:it.path];
                }
            }
        }
        entries.push_back(e);
    }
    NSMutableArray<FileItem *> *out = [NSMutableArray arrayWithCapacity:items.count];
    for (size_t i : TreeSort::order(entries, _treeSort)) [out addObject:items[i]];
    return out;
}

static NSString *MCGroupedCount(long long n) {
    static NSNumberFormatter *f;
    if (!f) {
        f = [NSNumberFormatter new];
        f.numberStyle = NSNumberFormatterDecimalStyle;
    }
    return [f stringFromNumber:@(n)];
}

- (void)configureTreeCell:(MCTreeCell *)cell item:(FileItem *)node {
    AppSettings *cfg = [AppSettings shared];
    NSColor *text = [cfg text:Surface::Sidebar];
    cell.textField.stringValue = node.name ?: @"";
    cell.textField.textColor = text;
    cell.textField.font = [NSFont systemFontOfSize:12.5];
    NSString *sym = node.isDir ? @"folder.fill" : @"doc.text";
    NSColor *tint = node.isDir ? Hex(0xC09553) : Hex(0x8A99A8);
    NSString *worktreeTip = nil;
    NSAttributedString *worktree = [self worktreeTitleFor:node color:text icon:&sym tint:&tint
                                                      tip:&worktreeTip];
    if (worktree) cell.textField.attributedStringValue = worktree;
    cell.imageView.image = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:nil];
    cell.imageView.contentTintColor = tint;
    if (worktree) {
        [cell setSize:nil saved:nil created:nil color:nil];
        cell.textField.attributedStringValue = worktree;   // after setSize, which sets clipping
        cell.toolTip = worktreeTip;
        return;
    }
    const MCTreeColumns columns = MCTreeColumnsShown();
    if (!columns) {
        [cell setSize:nil saved:nil created:nil color:nil];
        cell.toolTip = nil;
        return;
    }
    NSDate *now = [NSDate date];
    NSDate *modified = node.modified > 0
        ? [NSDate dateWithTimeIntervalSince1970:node.modified] : nil;
    NSDate *created = node.created > 0
        ? [NSDate dateWithTimeIntervalSince1970:node.created] : nil;
    NSString *saved = nil, *born = nil;
    if (columns & MCTreeColumnSaved) saved = modified ? MCCompactAge(modified, now) : @"";
    if (columns & MCTreeColumnCreated) born = created ? MCCompactAge(created, now) : @"";
    NSString *size = @"", *sizeTip = nil;
    if (!(columns & MCTreeColumnSize)) {
        size = nil;   // no column, and no folder totals counted for it
    } else if (node.isDir && node.isLink) {
        size = @"—";
        sizeTip = @"A symbolic link: what it points to is not counted.";
    } else if (node.isDir) {
        FolderSizes::Total t;
        BOOL stale = NO;
        if ([_sizer totalFor:node.path total:&t stale:&stale]) {
            NSString *bytes = MCFormatByteCount(t.bytes);
            // A thin space after ≥, so "≥ 999.9 MB" fits its column.
            size = t.capped ? [@"≥\u2009" stringByAppendingString:bytes] : bytes;
            sizeTip = [NSString stringWithFormat:@"%@%@ bytes in %@ %@%@",
                       t.capped ? @"At least " : @"", MCGroupedCount(t.bytes),
                       MCGroupedCount(t.files), t.files == 1 ? @"file" : @"files",
                       t.capped ? @" (counting stopped there; the folder is very large)" : @""];
            if (stale) [_sizer request:node.path];
        } else {
            size = @"…";
            sizeTip = @"Counting…";
            [_sizer request:node.path];
        }
    } else if (node.size >= 0) {
        size = MCFormatByteCount(node.size);
        sizeTip = [NSString stringWithFormat:@"%@ bytes", MCGroupedCount(node.size)];
    }
    [cell setSize:size saved:saved created:born color:[text colorWithAlphaComponent:0.55]];
    NSMutableArray<NSString *> *tip = [NSMutableArray arrayWithObject:node.name ?: @""];
    if (modified) [tip addObject:[@"Modified " stringByAppendingString:MCFullDate(modified)]];
    if (created) [tip addObject:[@"Created " stringByAppendingString:MCFullDate(created)]];
    if (sizeTip) [tip addObject:sizeTip];
    cell.toolTip = [tip componentsJoinedByString:@"\n"];
}

// The rows on screen again, in place: new colors, sizes or ages.
- (void)refreshTreeCells {
    [self.outline enumerateAvailableRowViewsUsingBlock:^(NSTableRowView *rv, NSInteger row) {
        MCTreeCell *cell = [rv viewAtColumn:0];
        FileItem *item = [self.outline itemAtRow:row];
        if ([cell isKindOfClass:MCTreeCell.class] && item)
            [self configureTreeCell:cell item:item];
    }];
}

// Folder totals arrived. Sorted by size, the rows move if their order
// changed (the selection, expansion and scroll position stay); otherwise
// the rows on screen just show the new sizes.
- (void)folderTotalsArrived:(NSSet<NSString *> *)paths {
    (void)paths;
    if (_treeSort.key == TreeSort::Key::Size) {
        _sortGeneration++;
        BOOL changed = NO;
        [self reorderNode:_root changed:&changed];
        if (changed) {
            [self reloadTreeKeepingState:NO];
            return;
        }
    }
    if (MCTreeDetailsShown()) [self refreshTreeCells];
}

- (void)reorderNode:(FileItem *)node changed:(BOOL *)changed {
    if (!node.loaded) return;
    NSArray<FileItem *> *before = node.shown;
    NSArray<FileItem *> *now = [self childrenOf:node];
    if (before && ![before isEqualToArray:now]) *changed = YES;
    for (FileItem *c in now)
        if (c.isDir && c.loaded) [self reorderNode:c changed:changed];
}

// FSEvents named a folder (as the disk spells it, /private/tmp for /tmp)
// in which something changed: its total and those above it are stale.
- (void)folderChangedOnDisk:(NSString *)path subtree:(BOOL)subtree {
    while (path.length > 1 && [path hasSuffix:@"/"]) path = [path substringToIndex:path.length - 1];
    NSString *root = _root.path;
    if (_realRoot && root && ![_realRoot isEqualToString:root]) {
        if ([path isEqualToString:_realRoot])
            path = root;
        else if ([path hasPrefix:[_realRoot stringByAppendingString:@"/"]])
            path = [root stringByAppendingString:[path substringFromIndex:_realRoot.length]];
    }
    [_sizer invalidate:path subtree:subtree];
}

- (void)setTreeSort:(TreeSort::Options)options {
    if (options == _treeSort) return;
    _treeSort = options;
    MCSetTreeSortForRoot(_root.path, options);
    _sortGeneration++;
    [self reloadTreeKeepingState:NO];
    if (self.outline.selectedRow >= 0) [self.outline scrollRowToVisible:self.outline.selectedRow];
}
- (void)sortTreeBy:(id)sender {
    const NSInteger tag = [sender tag];
    const TreeSort::Key key = tag == 1 ? TreeSort::Key::Modified
                            : tag == 2 ? TreeSort::Key::Size
                            : tag == 3 ? TreeSort::Key::Created : TreeSort::Key::Name;
    [self setTreeSort:TreeSort::choose(_treeSort, key)];
}
- (void)setTreeSortDirection:(id)sender {
    TreeSort::Options o = _treeSort;
    o.ascending = [sender tag] == 1;
    [self setTreeSort:o];
}
- (void)toggleFoldersFirst:(id)sender {
    TreeSort::Options o = _treeSort;
    o.foldersFirst = !o.foldersFirst;
    [self setTreeSort:o];
}
- (TreeSort::Options)treeSortOptions { return _treeSort; }   // tests

// Shift+Cmd+I (sizes), Option+Shift+Cmd+I (last saved) and
// Control+Shift+Cmd+I (created), for every window: they all hear the
// notification. The item's tag is the column.
- (void)toggleTreeColumn:(id)sender {
    MCToggleTreeColumn((MCTreeColumns)[sender tag]);
}
- (void)treeDetailsChanged:(NSNotification *)note {
    (void)note;
    // A column switched on gets room: a sidebar too narrow for the columns
    // now on widens, up to two fifths of the window.
    const MCTreeColumns now = MCTreeColumnsShown();
    if ((now & ~_shownColumns) && !self.sidebarCollapsed) {
        NSView *side = self.splitView.subviews.firstObject;
        const CGFloat want = ceil(MCTreeCellWidthFor(now) + 24);   // indentation, scroller
        const CGFloat cap = floor(NSWidth(self.splitView.bounds) * 0.4);
        if (side && NSWidth(side.frame) < want && NSWidth(side.frame) < cap)
            [self.splitView setPosition:MIN(want, cap) ofDividerAtIndex:0];
    }
    _shownColumns = now;
    [self refreshTreeCells];
    [self updateSavedStatus];
    [self updateDetailsTimer];
    if (self.hintsVisible) [self updateHints];
}

// Ages ("5 min") and "Saved ..." move on by themselves while details show.
- (void)updateDetailsTimer {
    const BOOL want = MCTreeDetailsShown() && !_closing;
    if (want == (_detailsTimer != nil)) return;
    if (!want) {
        [_detailsTimer invalidate];
        _detailsTimer = nil;
        return;
    }
    __weak EditorController *weakSelf = self;
    _detailsTimer = [NSTimer scheduledTimerWithTimeInterval:30 repeats:YES
                                                     block:^(NSTimer *t) {
        (void)t;
        [weakSelf refreshTreeCells];
        [weakSelf updateSavedStatus];
    }];
    _detailsTimer.tolerance = 5;
}

@end
