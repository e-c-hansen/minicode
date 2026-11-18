// EditorController+TreeFocus.mm — EditorController: Focus on Folder. The
// tree shows one folder inside the window's as its root, with a breadcrumb
// row across its top (TreeFocusBar.h) back up to the whole folder. The
// focused folder is a node of the whole tree (_root's), reached by listing
// the folders down to it, so the tree's own listings (FSEvents, Refresh,
// the hidden-files switch) keep it, its rows, their git colors and sizes up
// to date as before; the tree only starts lower. The window's folder never
// changes: search, Quick Open, terminals, git, agents and the session cover
// all of it. The rules (the breadcrumb's parts, a saved focus checked, a
// focus widened to show a path) are the core's, FileDetails.h's TreeFocus.
#import "EditorController+Private.h"
#import "EditorController+TreeFocusIvars.h"

#include "FileDetails.h"

@implementation EditorController (TreeFocus)

- (FileItem *)treeTop { return _treeFocusItem ?: _root; }

- (NSString *)treeFocus { return _treeFocusItem ? (_treeFocusPath ?: @"") : @""; }

- (MCTreeFocusBar *)treeFocusBar { return _treeFocusBar; }

- (BOOL)treeShowsFolder:(FileItem *)folder {
    FileItem *top = [self treeTop];
    if (!folder) return NO;
    if (folder == top || top == _root) return YES;
    return [folder.path hasPrefix:[top.path stringByAppendingString:@"/"]];
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

// Roots the tree at `focus` ("" for the whole folder), keeping the folders
// open in it and the selected row, and remembering the folders open
// outside it for when it widens again.
- (BOOL)focusTreeOn:(NSString *)focus save:(BOOL)save {
    NSString *rel = @(TreeFocus::normalize(Utf8String(focus ?: @"")).c_str()) ?: @"";
    FileItem *target = rel.length ? [self treeItemForFocus:rel] : nil;
    if (rel.length && !target) return NO;
    if (target == _treeFocusItem) return YES;
    FileItem *oldTop = [self treeTop];
    NSString *selPath = self.outline.selectedRow >= 0
        ? [(FileItem *)[self.outline itemAtRow:self.outline.selectedRow] path] : nil;
    // What was remembered from before, but where the tree showed it, what
    // it shows now: those rows are the truth.
    NSMutableSet<NSString *> *memory = _treeFocusExpanded ?: [NSMutableSet set];
    NSString *oldPrefix = [oldTop.path stringByAppendingString:@"/"];
    for (NSString *p in [memory allObjects])
        if (oldTop == _root || [p isEqualToString:oldTop.path] || [p hasPrefix:oldPrefix])
            [memory removeObject:p];
    [memory unionSet:[self treeExpandedNow]];
    // Widening: the folder that was the root shows open, with every folder
    // on the way down to it, so its rows stay in view.
    NSString *newTopPath = target ? target.path : _root.path;
    const BOOL widening = oldTop != _root &&
        (target == nil || [oldTop.path hasPrefix:[newTopPath stringByAppendingString:@"/"]]);
    if (widening)
        for (FileItem *f = oldTop; f && f != _root && f != target; f = f.parent)
            [memory addObject:f.path];
    _treeFocusItem = target;
    _treeFocusPath = target ? rel : nil;
    _treeFocusExpanded = memory;

    [self.outline reloadData];
    FileItem *top = [self treeTop];
    [self reExpand:top usingSet:memory];
    if (top == _root && _worktreesItem && [memory containsObject:_worktreesItem.path]) {
        [self.outline expandItem:_worktreesItem];
        [self reExpand:_worktreesItem usingSet:memory];
    }
    // The selection where it still shows; widening, else the old root's row.
    NSInteger row = -1;
    for (NSInteger r = 0; selPath && r < self.outline.numberOfRows; r++)
        if ([[(FileItem *)[self.outline itemAtRow:r] path] isEqualToString:selPath]) { row = r; break; }
    if (row < 0 && widening) row = [self.outline rowForItem:oldTop];
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
    if (!_treeFocusItem) {
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
        self.sidebarScroll.topBar = _treeFocusBar;
    }
    _treeFocusBar.textColor = [[AppSettings shared] text:Surface::Sidebar];
    NSString *name = _root.path.lastPathComponent.length ? _root.path.lastPathComponent : _root.path;
    [_treeFocusBar setRootName:name focus:_treeFocusPath ?: @""];
    _treeFocusBar.hidden = NO;
    [self.sidebarScroll tile];
}

// The folder Focus on Folder roots the tree at: the clicked or selected
// folder, a selected file's folder, else the active file's folder.
- (NSString *)folderToFocus {
    FileItem *node = [self clickedOrSelectedItem];
    if (node && [self isWorktreeRowOrNode:node]) return nil;
    if (node) return [self treeRelative:node.isDir ? node.path : node.path.stringByDeletingLastPathComponent];
    NSString *fromRoot = self.currentPath ? [self pathFromRoot:self.currentPath] : nil;
    if (!fromRoot) return nil;
    return [fromRoot isEqualToString:@"."] ? @"" : @(TreeFocus::parent(Utf8String(fromRoot)).c_str());
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

- (void)showWholeFolderInTree:(id)sender {
    (void)sender;
    if (!_treeFocusItem) return;
    [self focusTreeOn:@""];
}

- (BOOL)validateTreeFocusItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    if (item.action == @selector(showWholeFolderInTree:)) *enabled = _treeFocusItem != nil;
    else if (item.action == @selector(focusOnFolder:)) *enabled = [self folderToFocus] != nil;
    else return NO;
    return YES;
}

- (BOOL)treeFocusShowsPath:(NSString *)path widen:(BOOL)widen {
    if (!_treeFocusItem) return YES;
    NSString *rel = [self treeRelative:path];
    if (!rel) return YES;   // outside the window's folder: as before, nothing to show
    const std::string focus = Utf8String(_treeFocusPath), r = Utf8String(rel);
    if (r != focus && TreeFocus::contains(focus, r)) return YES;
    if (!widen) return NO;
    // A reveal: the nearest folder that shows it as a row.
    return [self focusTreeOn:@(TreeFocus::widened(focus, r).c_str())];
}

// Whether the focused folder is still where the tree listed it: each
// folder on the way down still holds the next.
- (BOOL)treeFocusStillListed {
    for (FileItem *f = _treeFocusItem; f && f != _root; f = f.parent) {
        FileItem *up = f.parent;
        if (!up || !up.loaded || [up.children indexOfObjectIdenticalTo:f] == NSNotFound) return NO;
    }
    return YES;
}

- (NSSet<NSString *> *)dropLostTreeFocus {
    if (!_treeFocusItem || [self treeFocusStillListed]) return nil;
    NSSet<NSString *> *open = [_treeFocusExpanded copy] ?: [NSSet set];
    _treeFocusItem = nil;
    _treeFocusPath = nil;
    _treeFocusExpanded = nil;
    [self updateTreeFocusBar];
    [self showStatusNote:@"The focused folder is gone: the tree shows the whole folder"];
    [self saveSession];
    return open;
}

- (void)forgetTreeFocus {
    _treeFocusItem = nil;
    _treeFocusPath = nil;
    _treeFocusExpanded = nil;
    [self updateTreeFocusBar];
}

- (void)restoreTreeFocus:(NSString *)saved {
    const std::string focus = TreeFocus::restored(
        Utf8String(saved ?: @""),
        [self](const std::string &f) { return [self treeItemForFocus:@(f.c_str())] != nil; });
    [self focusTreeOn:@(focus.c_str()) save:NO];
}

- (NSSet<NSString *> *)treeFocusRememberedExpanded {
    return _treeFocusItem ? [_treeFocusExpanded copy] ?: [NSSet set] : [NSSet set];
}

- (void)rememberTreeExpanded:(NSSet<NSString *> *)paths {
    if (!paths.count) return;
    if (!_treeFocusExpanded) _treeFocusExpanded = [NSMutableSet set];
    [_treeFocusExpanded unionSet:paths];
}

@end
