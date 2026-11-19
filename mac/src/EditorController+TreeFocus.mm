// EditorController+TreeFocus.mm — EditorController: Focus on Folder and
// Add to Focus. With one folder focused, the tree shows that folder inside
// the window's as its root, with a breadcrumb row across its top
// (TreeFocusBar.h) back up to the whole folder; with several, each folder
// is a root row of its own, labelled with its path from the window's
// folder, under a row of their labels, each with a × that takes it out.
// The focused folders are nodes of the whole tree (_root's), reached by
// listing the folders down to them, so the tree's own listings (FSEvents,
// Refresh, the hidden-files switch) keep them, their rows, git colors and
// sizes up to date as before; the tree only starts lower. The window's
// folder never changes: search, Quick Open, terminals, git, agents and the
// session cover all of it. The rules (the breadcrumb's parts, the set's
// merging of nested folders, a saved focus checked, a reveal widening or
// adding a folder, the labels) are the core's, FileDetails.h's TreeFocus.
#import "EditorController+Private.h"
#import "EditorController+TreeFocusIvars.h"
#import "PalettePanel.h"

#include "FileDetails.h"

namespace {

TreeFocus::Set MCFocusSet(NSArray<NSString *> *folders) {
    TreeFocus::Set set;
    for (NSString *f in folders) set.push_back(Utf8String(f));
    return set;
}

NSArray<NSString *> *MCFocusArray(const TreeFocus::Set &set) {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (const std::string &f : set) [out addObject:@(f.c_str()) ?: @""];
    return out;
}

BOOL MCPathAtOrBelow(NSString *path, FileItem *folder) {
    return [path isEqualToString:folder.path] ||
           [path hasPrefix:[folder.path stringByAppendingString:@"/"]];
}

}  // namespace

@implementation EditorController (TreeFocus)

- (FileItem *)treeTop {
    if (!_treeFocusItems.count) return _root;
    return _treeFocusItems.count == 1 ? _treeFocusItems[0] : nil;
}

- (NSArray<FileItem *> *)treeTopRows {
    if (_treeFocusItems.count > 1) return _treeFocusItems;
    return [self childrenOf:[self treeTop]];
}

- (FileItem *)treeDefaultFolder { return [self treeTop] ?: _treeFocusItems.firstObject; }

- (NSString *)treeFocus { return [_treeFocusPaths componentsJoinedByString:@", "] ?: @""; }

- (NSArray<NSString *> *)treeFocusFolders { return _treeFocusPaths ?: @[]; }

- (MCTreeFocusBar *)treeFocusBar { return _treeFocusBar; }

- (FileItem *)treeRootFor:(NSString *)path {
    if (_treeFocusItems.count < 2 || !path) return nil;
    for (FileItem *r in _treeFocusItems)
        if (MCPathAtOrBelow(path, r)) return r;
    return nil;
}

- (NSString *)treeRootLabelFor:(FileItem *)item {
    if (_treeFocusItems.count < 2 || !item) return nil;
    const NSUInteger i = [_treeFocusItems indexOfObjectIdenticalTo:item];
    return i == NSNotFound ? nil : _treeFocusPaths[i];
}

- (BOOL)treeShowsFolder:(FileItem *)folder {
    if (!folder) return NO;
    if (!_treeFocusItems.count) return YES;
    for (FileItem *top in _treeFocusItems)
        if (folder == top || [folder.path hasPrefix:[top.path stringByAppendingString:@"/"]]) return YES;
    return NO;
}

- (void)reExpandTreeTop:(NSSet<NSString *> *)expanded {
    if (_treeFocusItems.count > 1) {
        for (FileItem *r in _treeFocusItems) {
            if ([expanded containsObject:r.path]) {
                [self.outline expandItem:r];
                [self reExpand:r usingSet:expanded];
            } else {
                [self.outline collapseItem:r];
            }
        }
        return;
    }
    FileItem *top = [self treeTop];
    [self reExpand:top usingSet:expanded];
    if (top == _root && _worktreesItem && [expanded containsObject:_worktreesItem.path]) {
        [self.outline expandItem:_worktreesItem];
        [self reExpand:_worktreesItem usingSet:expanded];
    }
}

// `path` (the tree's spelling) from the window's folder; nil outside it.
- (NSString *)treeRelative:(NSString *)path {
    std::string rel;
    if (!path || !TreeFocus::relative(Utf8String(_root.path), Utf8String(path), &rel)) return nil;
    return @(rel.c_str());
}

// The folder `rel` names, listed down to from the window's folder, as the
// tree has it: nil when one of the folders on the way is not in the tree
// (gone, not a folder, or a dotfile while hidden files are off).
- (FileItem *)treeItemForFocus:(NSString *)rel {
    FileItem *node = _root;
    for (NSString *name in [rel componentsSeparatedByString:@"/"]) {
        if (!name.length) continue;
        [node loadChildren];
        NSString *want = [node.path stringByAppendingPathComponent:name];
        FileItem *next = nil;
        for (FileItem *c in node.children)
            if (c.isDir && [c.path isEqualToString:want]) { next = c; break; }
        if (!next) return nil;
        node = next;
    }
    return node == _root ? nil : node;
}

// The paths of the folders open in the tree now.
- (NSMutableSet<NSString *> *)treeExpandedNow {
    NSMutableSet<NSString *> *open = [NSMutableSet set];
    for (NSInteger r = 0; r < self.outline.numberOfRows; r++) {
        FileItem *it = [self.outline itemAtRow:r];
        if ([self.outline isItemExpanded:it] && it.path) [open addObject:it.path];
    }
    return open;
}

- (BOOL)focusTreeOn:(NSString *)focus {
    return [self focusTreeOn:focus save:YES];
}

- (BOOL)focusTreeOn:(NSString *)focus save:(BOOL)save {
    return [self setTreeFocusFolders:focus.length ? @[focus] : @[] save:save];
}

- (BOOL)setTreeFocusFolders:(NSArray<NSString *> *)folders {
    return [self setTreeFocusFolders:folders save:YES];
}

- (BOOL)addTreeFocus:(NSString *)folder {
    return [self setTreeFocusFolders:MCFocusArray(TreeFocus::added(MCFocusSet(self.treeFocusFolders),
                                                                   Utf8String(folder ?: @"")))
                                save:YES];
}

- (BOOL)removeTreeFocus:(NSString *)folder {
    return [self setTreeFocusFolders:MCFocusArray(TreeFocus::removed(MCFocusSet(self.treeFocusFolders),
                                                                     Utf8String(folder ?: @"")))
                                save:YES];
}

// Shows `folders` (merged by the core's rule; none for the whole folder),
// keeping the folders open in the tree and the selected row, and
// remembering the folders open outside it for when it widens again. A
// folder new among several roots shows open.
- (BOOL)setTreeFocusFolders:(NSArray<NSString *> *)folders save:(BOOL)save {
    const TreeFocus::Set set = TreeFocus::merged(MCFocusSet(folders));
    NSMutableArray<FileItem *> *items = [NSMutableArray array];
    for (const std::string &f : set) {
        FileItem *it = [self treeItemForFocus:@(f.c_str())];
        if (!it) return NO;
        [items addObject:it];
    }
    NSArray<FileItem *> *old = _treeFocusItems ?: @[];
    if ([items isEqualToArray:old]) return YES;
    NSString *selPath = self.outline.selectedRow >= 0
        ? [(FileItem *)[self.outline itemAtRow:self.outline.selectedRow] path] : nil;
    // What was remembered from before, but where the tree showed it, what
    // it shows now: those rows are the truth.
    NSMutableSet<NSString *> *memory = _treeFocusExpanded ?: [NSMutableSet set];
    for (NSString *p in [memory allObjects]) {
        BOOL shown = old.count == 0;
        for (FileItem *t in old) shown = shown || MCPathAtOrBelow(p, t);
        if (shown) [memory removeObject:p];
    }
    [memory unionSet:[self treeExpandedNow]];
    // Widening (or a root taken into a new folder that holds it): the old
    // root shows open, with every folder on the way down to it, so its
    // rows stay in view.
    NSArray<FileItem *> *newTops = items.count ? items : @[_root];
    FileItem *widenedFrom = nil;
    for (FileItem *t in old) {
        for (FileItem *n in newTops) {
            if (n == t) break;
            if (n != _root && ![t.path hasPrefix:[n.path stringByAppendingString:@"/"]]) continue;
            for (FileItem *f = t; f && f != _root && f != n; f = f.parent) [memory addObject:f.path];
            if (old.count == 1) widenedFrom = t;
            break;
        }
    }
    // Among several roots, one that was not a root of several before
    // (added, or the one folder focused till now) shows open.
    if (items.count > 1)
        for (FileItem *n in items)
            if (old.count < 2 || [old indexOfObjectIdenticalTo:n] == NSNotFound) [memory addObject:n.path];
    _treeFocusItems = items.count ? [items copy] : nil;
    _treeFocusPaths = items.count ? MCFocusArray(set) : nil;
    _treeFocusExpanded = memory;

    [self.outline reloadData];
    [self reExpandTreeTop:memory];
    [self fitTreeColumn];
    // The selection where it still shows; widening, else the old root's row.
    NSInteger row = -1;
    for (NSInteger r = 0; selPath && r < self.outline.numberOfRows; r++)
        if ([[(FileItem *)[self.outline itemAtRow:r] path] isEqualToString:selPath]) { row = r; break; }
    if (row < 0 && widenedFrom) row = [self.outline rowForItem:widenedFrom];
    if (row >= 0) {
        [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                  byExtendingSelection:NO];
    } else {
        [self.outline deselectAll:nil];
    }
    [self updateTreeFocusBar];
    [self.outline scrollPoint:NSZeroPoint];
    if (row >= 0) [self.outline scrollRowToVisible:row];
    if (save) [self saveSession];
    return YES;
}

- (void)updateTreeFocusBar {
    if (!_treeFocusItems.count) {
        if (_treeFocusBar && !_treeFocusBar.hidden) {
            _treeFocusBar.hidden = YES;
            [self.sidebarScroll tile];
        }
        return;
    }
    if (!_treeFocusBar) {
        _treeFocusBar = [[MCTreeFocusBar alloc]
            initWithFrame:NSMakeRect(0, 0, self.sidebarScroll.bounds.size.width, kMCTreeFocusBarHeight)];
        __weak EditorController *weakSelf = self;
        _treeFocusBar.onPick = ^(NSString *focus) {
            EditorController *s = weakSelf;
            [s focusTreeOn:focus];
            [s.window makeFirstResponder:s.outline];
        };
        _treeFocusBar.onRemove = ^(NSString *folder) {
            EditorController *s = weakSelf;
            [s removeTreeFocus:folder];
            [s.window makeFirstResponder:s.outline];
        };
        _treeFocusBar.onShowFolder = ^(NSString *folder) {
            EditorController *s = weakSelf;
            [s selectTreeRoot:folder];
            [s.window makeFirstResponder:s.outline];
        };
        _treeFocusBar.onAdd = ^{ [weakSelf chooseFolderToAddToFocus:nil]; };
        self.sidebarScroll.topBar = _treeFocusBar;
    }
    _treeFocusBar.textColor = [[AppSettings shared] text:Surface::Sidebar];
    if (_treeFocusItems.count == 1) {
        NSString *name = _root.path.lastPathComponent.length ? _root.path.lastPathComponent : _root.path;
        [_treeFocusBar setRootName:name focus:_treeFocusPaths.firstObject ?: @""];
    } else {
        [_treeFocusBar setFolders:_treeFocusPaths];
    }
    _treeFocusBar.hidden = NO;
    [self.sidebarScroll tile];
}

// A label of the label row clicked: its root row selected and scrolled to.
- (void)selectTreeRoot:(NSString *)folder {
    const NSUInteger i = [_treeFocusPaths indexOfObject:folder ?: @""];
    if (i == NSNotFound || i >= _treeFocusItems.count) return;
    const NSInteger row = [self.outline rowForItem:_treeFocusItems[i]];
    if (row < 0) return;
    [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    [self.outline scrollRowToVisible:row];
}

// The folder Focus on Folder roots the tree at: the clicked or selected
// folder, a selected file's folder, else the active file's folder.
- (NSString *)folderToFocus {
    FileItem *node = [self clickedOrSelectedItem];
    if (node && [self isWorktreeRowOrNode:node]) return nil;
    if (node) return [self treeRelative:node.isDir ? node.path : node.path.stringByDeletingLastPathComponent];
    return [self activeFileFolder];
}

// The active file's folder from the window's ("" at its top); nil for none
// or outside it.
- (NSString *)activeFileFolder {
    NSString *fromRoot = self.currentPath ? [self pathFromRoot:self.currentPath] : nil;
    if (!fromRoot) return nil;
    return [fromRoot isEqualToString:@"."] ? @"" : @(TreeFocus::parent(Utf8String(fromRoot)).c_str());
}

// `rel` when Add to Focus would show something new by it: a folder not
// shown yet, and not the window's own ("" would show the whole folder,
// which is Show Whole Folder in Tree's).
- (NSString *)addableFocus:(NSString *)rel {
    return rel.length && TreeFocus::rootOf(MCFocusSet(self.treeFocusFolders), Utf8String(rel)) < 0 ? rel : nil;
}

- (NSString *)folderToAddToFocusFrom:(FileItem *)node {
    if (!node || [self isWorktreeRowOrNode:node]) return nil;
    return [self addableFocus:[self treeRelative:node.isDir ? node.path
                                                            : node.path.stringByDeletingLastPathComponent]];
}

- (NSString *)folderToAddToFocus {
    // A right click goes by its row alone.
    if (self.outline.clickedRow >= 0)
        return [self folderToAddToFocusFrom:[self.outline itemAtRow:self.outline.clickedRow]];
    FileItem *selected = self.outline.selectedRow >= 0 ? [self.outline itemAtRow:self.outline.selectedRow] : nil;
    if (NSString *rel = [self folderToAddToFocusFrom:selected]) return rel;
    return [self addableFocus:[self activeFileFolder]];
}

- (void)focusOnFolder:(id)sender {
    (void)sender;
    NSString *rel = [self folderToFocus];
    if (!rel) {
        NSBeep();
        return;
    }
    if (![self focusTreeOn:rel]) {
        NSBeep();
        [self showStatusNote:@"That folder is not in the tree"];
    }
}

- (void)addFolderToFocus:(id)sender {
    (void)sender;
    NSString *rel = [self folderToAddToFocus];
    // Nothing new to add by the selection or the open file (with the tree
    // focused, every row is inside it): pick one instead.
    if (!rel && self.outline.clickedRow < 0) {
        [self chooseFolderToAddToFocus:nil];
        return;
    }
    if (!rel) {
        NSBeep();
        [self showStatusNote:@"The tree shows that folder already"];
        return;
    }
    if (![self addTreeFocus:rel]) {
        NSBeep();
        [self showStatusNote:@"That folder is not in the tree"];
    }
}

// Add Folder to Focus…: mutible's folder picker (PalettePanel.h,
// MCFolderPick) scoped to the window's folder: the folders of the open
// tabs not shown yet first, most recent first, then the window's
// subfolders; a typed path ("linux/src") goes deeper. The folder chosen is
// added as Add to Focus adds one (nested folders merged by the core).
- (void)chooseFolderToAddToFocus:(id)sender {
    (void)sender;
    if (!_root.path.length) {
        NSBeep();
        return;
    }
    NSMutableArray<NSString *> *recent = [NSMutableArray array];
    for (NSString *p in [self recentTabPaths]) {
        NSString *rel = [self treeRelative:p.stringByDeletingLastPathComponent];
        if (![self addableFocus:rel]) continue;
        NSString *dir = [_root.path stringByAppendingPathComponent:rel];
        if (![recent containsObject:dir]) [recent addObject:dir];
    }
    MCFolderPick *pick = [MCFolderPick new];
    pick.recentFolders = recent;
    pick.baseFolder = _root.path;
    pick.scopeFolder = _root.path;
    pick.placeholder = @"Add a folder to the tree's focus: type its name, or a path such as src/";
    pick.typedRowTitle = @"Add This Folder";
    __weak EditorController *weakSelf = self;
    pick.choose = ^(NSString *path) { [weakSelf addPickedFolderToFocus:path]; };
    MCShowFolderPicker(self.window, pick);
}

- (void)addPickedFolderToFocus:(NSString *)path {
    NSString *rel = [self treeRelative:path];
    if (!rel.length) {
        NSBeep();
        return;
    }
    if (![self addTreeFocus:rel]) {
        NSBeep();
        [self showStatusNote:@"That folder is not in the tree"];
        return;
    }
    [self.window makeFirstResponder:self.outline];
}

// The root row clicked or selected, while several folders are focused.
- (NSString *)focusedRootToRemove {
    return [self treeRootLabelFor:[self clickedOrSelectedItem]];
}

- (void)removeFolderFromFocus:(id)sender {
    (void)sender;
    NSString *rel = [self focusedRootToRemove];
    if (!rel) {
        NSBeep();
        return;
    }
    [self removeTreeFocus:rel];
}

- (void)showWholeFolderInTree:(id)sender {
    (void)sender;
    if (!_treeFocusItems.count) return;
    [self setTreeFocusFolders:@[] save:YES];
}

- (BOOL)validateTreeFocusItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    if (item.action == @selector(showWholeFolderInTree:)) *enabled = _treeFocusItems.count > 0;
    else if (item.action == @selector(focusOnFolder:)) *enabled = [self folderToFocus] != nil;
    else if (item.action == @selector(addFolderToFocus:))
        *enabled = [self folderToAddToFocus] != nil || (self.outline.clickedRow < 0 && _root.path.length);
    else if (item.action == @selector(chooseFolderToAddToFocus:)) *enabled = _root.path.length > 0;
    else if (item.action == @selector(removeFolderFromFocus:)) *enabled = [self focusedRootToRemove] != nil;
    else return NO;
    return YES;
}

- (BOOL)treeFocusShowsPath:(NSString *)path widen:(BOOL)widen {
    if (!_treeFocusItems.count) return YES;
    NSString *rel = [self treeRelative:path];
    if (!rel) return YES;   // outside the window's folder: as before, nothing to show
    const TreeFocus::Set set = MCFocusSet(_treeFocusPaths);
    const std::string r = Utf8String(rel);
    // One folder is the tree's root, no row of its own; several are rows.
    if (TreeFocus::shows(set, r)) return YES;
    if (!widen) return NO;
    // A reveal: one folder widens to the nearest folder that shows it as a
    // row; several take in the path's folder.
    return [self setTreeFocusFolders:MCFocusArray(TreeFocus::revealed(set, r)) save:YES];
}

// Whether a focused folder is still where the tree listed it: each folder
// on the way down still holds the next.
- (BOOL)treeFocusStillListed:(FileItem *)item {
    for (FileItem *f = item; f && f != _root; f = f.parent) {
        FileItem *up = f.parent;
        if (!up || !up.loaded || [up.children indexOfObjectIdenticalTo:f] == NSNotFound) return NO;
    }
    return YES;
}

- (NSSet<NSString *> *)dropLostTreeFocus {
    if (!_treeFocusItems.count) return nil;
    NSMutableArray<FileItem *> *items = [NSMutableArray array];
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSUInteger i = 0; i < _treeFocusItems.count; i++) {
        if (![self treeFocusStillListed:_treeFocusItems[i]]) continue;
        [items addObject:_treeFocusItems[i]];
        [paths addObject:_treeFocusPaths[i]];
    }
    if (items.count == _treeFocusItems.count) return nil;
    NSSet<NSString *> *open = [_treeFocusExpanded copy] ?: [NSSet set];
    const BOOL several = _treeFocusItems.count > 1;
    _treeFocusItems = items.count ? items : nil;
    _treeFocusPaths = items.count ? paths : nil;
    if (!items.count) _treeFocusExpanded = nil;
    [self updateTreeFocusBar];
    [self showStatusNote:items.count ? @"A focused folder is gone: the tree shows the others"
                         : several   ? @"The focused folders are gone: the tree shows the whole folder"
                                     : @"The focused folder is gone: the tree shows the whole folder"];
    [self saveSession];
    return open;
}

- (void)forgetTreeFocus {
    _treeFocusItems = nil;
    _treeFocusPaths = nil;
    _treeFocusExpanded = nil;
    [self updateTreeFocusBar];
}

- (void)restoreTreeFocus:(NSArray<NSString *> *)saved {
    const TreeFocus::Set set = TreeFocus::restoredSet(
        MCFocusSet(saved ?: @[]),
        [self](const std::string &f) { return [self treeItemForFocus:@(f.c_str())] != nil; });
    [self setTreeFocusFolders:MCFocusArray(set) save:NO];
}

// Only those outside what the tree shows: the rows say what is open there
// (a root closed since it was added stays closed).
- (NSSet<NSString *> *)treeFocusRememberedExpanded {
    NSMutableSet<NSString *> *out = [NSMutableSet set];
    if (!_treeFocusItems.count) return out;
    for (NSString *p in _treeFocusExpanded) {
        BOOL shown = NO;
        for (FileItem *t in _treeFocusItems) shown = shown || MCPathAtOrBelow(p, t);
        if (!shown) [out addObject:p];
    }
    return out;
}

- (void)rememberTreeExpanded:(NSSet<NSString *> *)paths {
    if (!paths.count) return;
    if (!_treeFocusExpanded) _treeFocusExpanded = [NSMutableSet set];
    [_treeFocusExpanded unionSet:paths];
}

@end
