// EditorController+TreeDetails.mm — EditorController: the tree's sizes, dates and order.
#import "EditorController+Private.h"
#import "EditorController+TreeDetailsIvars.h"
#import "FileTypeIcons.h"
#import "RecentFolders.h"

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
    MCNoteRecentFolder(_root.path);   // File > Open Recent, Ctrl+R
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
                if (it.isLink || it.source) e.size = 0;   // not counted, as in the totals (or over SSH)
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
    // A root row of several focused folders says its path from the
    // window's folder ("mac/src"), a little heavier (+TreeFocus).
    NSString *rootLabel = [self treeRootLabelFor:node];
    cell.textField.stringValue = rootLabel ?: node.name ?: @"";
    // In a git folder the name takes its status's color (tree.git-*).
    NSString *gitTip = nil;
    NSColor *gitColor = [self gitColorOf:node tip:&gitTip];
    // Over SSH there is no git here yet: what the host's listing marks as
    // ignored (the ignore files) is drawn as git's ignored, as the page's
    // tree draws it; a link out of the folder is dimmed the same way.
    if (!gitColor && (node.ignored || node.outside)) {
        gitColor = MCColor([[AppSettings shared] settings].treeGitColor(TreeGitColor::Ignored));
        gitTip = node.outside ? @"A link out of the folder" : @"Ignored";
    }
    cell.textField.textColor = gitColor ?: text;
    cell.textField.font = rootLabel ? [NSFont systemFontOfSize:12.5 weight:NSFontWeightMedium]
                                    : [NSFont systemFontOfSize:12.5];
    // A file's icon says what it is (FileIcon::Type: Python, JSON, a
    // Dockerfile, a picture), in its family's color (FileTypeIcons.mm);
    // the images and colors are made once per type.
    NSString *sym = nil;
    NSColor *tint = Hex(0xC09553);
    NSImage *icon = nil;
    if (node.isDir) {
        icon = MCTreeSymbolImage(@"folder.fill");
    } else {
        const FileIcon::Type type = MCFileTypeOf(node.path, node.executable);
        icon = MCFileTypeImage(type);
        tint = MCFileTypeTint(type, MCFileTypeTextIsLight(text));
    }
    NSString *worktreeTip = nil;
    NSAttributedString *worktree = [self worktreeTitleFor:node color:text icon:&sym tint:&tint
                                                      tip:&worktreeTip];
    if (worktree) cell.textField.attributedStringValue = worktree;
    if (sym) icon = MCTreeSymbolImage(sym);
    if (cell.imageView.image != icon) cell.imageView.image = icon;
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
        cell.toolTip = gitTip ? [NSString stringWithFormat:@"%@\n%@", node.name ?: @"", gitTip] : nil;
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
    } else if (node.isDir && node.source) {
        size = @"";   // over SSH: no totals counted (they would be this Mac's disk's)
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
    if (gitTip) [tip addObject:gitTip];
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

// The name column as wide as the sidebar shows: after every reload, and
// whenever the sidebar's frame changes. The column only follows the
// outline's frame by the change in it (NSTableViewLastColumnOnlyAutoresizingStyle),
// so a width it lost once (to its 80-point minimum, or to the default 100
// of a window laid out before it was shown) stayed lost, and the next
// reload cut every name off at about 90 points in a 250-point sidebar
// (C3, 2026-11-16: a file added to the folder). A collapsed sidebar is left
// alone.
- (void)fitTreeColumn {
    NSClipView *clip = self.sidebarScroll.contentView;
    if (!self.outline.outlineTableColumn || NSWidth(clip.bounds) < 1) return;
    [self.outline sizeLastColumnToFit];
}
- (void)sidebarFrameChanged:(NSNotification *)note {
    (void)note;
    [self fitTreeColumn];
    [self treeRevealIfShown];   // dragged open: a file that waited for it
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
    [_sizer invalidate:[self treeSpellingOf:path] subtree:subtree];
}

// A path as FSEvents spells it (/private/tmp/x) as the tree does (/tmp/x).
- (NSString *)treeSpellingOf:(NSString *)diskPath {
    return @(TreeRefresh::treeSpelling(Utf8String(diskPath), Utf8String(_root.path),
                                       Utf8String(_realRoot)).c_str()) ?: diskPath;
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
