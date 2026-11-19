// EditorController+TreeReveal.mm — the file tree follows the active tab
// (explorer.auto-reveal, on by default, as VS Code's explorer.autoReveal
// but centered). When another tab comes on show (activateTab:, which every
// way there goes through: a click on a tab, Cmd+1–9, next and previous, a
// click into the other editor group, a close showing the next, Quick Open,
// Go to Definition, a search hit), its file's folders open, listing only
// those on the way (treeOpenFoldersTo:widen:), its row is selected, and the
// tree scrolls the row to its middle only when it is not wholly in view
// (the core's TreeReveal::scrollTop), so clicking between two files on
// show never moves it. The keyboard stays where it is.
//
// The tree is left alone for a file outside every focused folder (the
// focus never widens; only Reveal Active File in File Tree does), a page,
// an untitled tab, or a file outside the window's folder. A click in the
// tree selects its row without scrolling. While the tree is hidden (the
// sidebar closed, or Source Control, Agents or Processes in its place) the
// change waits (TreeReveal::Follower) and is revealed once when the tree
// shows again. A window brought back puts its tree back as it was.
#import "EditorController+Private.h"
#import "EditorController+TreeRevealIvars.h"

@implementation EditorController (TreeReveal)

- (BOOL)treeAutoRevealEnabled {
    return [AppSettings shared].settings.explorerAutoReveal();
}

// Whether the tree's rows are on screen: its scroll view in the sidebar,
// and the sidebar open.
- (BOOL)treeOnShow {
    NSScrollView *s = self.sidebarScroll;
    return s && s.superview && !self.sidebarCollapsed && NSWidth(s.frame) >= 1 &&
           NSHeight(s.frame) >= 1;
}

// The active tab's file as the tree spells it, or nil when the tree has
// none to show: a page or a comparison, an untitled tab, a file gone, or
// one outside the window's folder.
- (NSString *)treeFollowablePath {
    MCTab *t = _activeTab;
    NSString *p = t.currentPath;
    if (!p || t.isDiff || t.gone) return nil;
    NSString *rel = [self pathFromRoot:p];
    if (!rel || [rel isEqualToString:@"."]) return nil;
    return [_root.path stringByAppendingPathComponent:rel];
}

// Scrolls the tree so `row` shows: not at all when it is wholly in view,
// else with the row in the middle (as far as the rows go).
- (void)scrollTreeRowIntoView:(NSInteger)row {
    NSOutlineView *o = self.outline;
    NSClipView *clip = self.sidebarScroll.contentView;
    if (row < 0 || row >= o.numberOfRows || !clip || o.enclosingScrollView != self.sidebarScroll) {
        [o scrollRowToVisible:row];
        return;
    }
    const NSRect r = [o rectOfRow:row];
    const NSRect view = clip.bounds;
    const CGFloat content = MAX(NSHeight(o.frame), NSMaxY([o rectOfRow:o.numberOfRows - 1]));
    const double top = TreeReveal::scrollTop(NSMinY(r), NSHeight(r), NSMinY(view),
                                             NSHeight(view), content);
    if (top == NSMinY(view)) return;
    [clip scrollToPoint:NSMakePoint(NSMinX(view), top)];
    [self.sidebarScroll reflectScrolledClipView:clip];
}

// Shows `path` in the tree without widening the focus: its folders
// opened, its row selected and, unless in view, centered.
- (void)treeRevealFollowed:(NSString *)path {
    FileItem *node = [self treeOpenFoldersTo:path widen:NO];
    if (!node || ![node.path isEqualToString:path]) return;   // outside the focus, or hidden
    const NSInteger row = [self.outline rowForItem:node];
    if (row < 0) return;
    if (self.outline.selectedRow != row || self.outline.numberOfSelectedRows != 1)
        [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                  byExtendingSelection:NO];
    [self scrollTreeRowIntoView:row];
}

- (void)treeFollowActiveTab {
    if (_treeFollowHeld > 0 || ![self treeAutoRevealEnabled]) {
        [self reselectCurrentFileInTree];
        return;
    }
    NSString *path = [self treeFollowablePath];
    if (path && _treeOpeningPath &&
        [path.stringByResolvingSymlinksInPath
            isEqualToString:_treeOpeningPath.stringByResolvingSymlinksInPath]) {
        [self reselectCurrentFileInTree];   // the row clicked: selected, never scrolled
        return;
    }
    if (!_treeFollower.activeChanged(Utf8String(path ?: @""), [self treeOnShow])) {
        [self reselectCurrentFileInTree];
        return;
    }
    [self treeRevealFollowed:path];
}

- (void)treeRevealIfShown {
    if (![self treeOnShow]) return;
    const std::string waiting = _treeFollower.treeShown();
    if (waiting.empty() || ![self treeAutoRevealEnabled]) return;
    NSString *path = [self treeFollowablePath];
    // Still the file on show (it waited for the tree): revealed now.
    if (path && Utf8String(path) == waiting) [self treeRevealFollowed:path];
}

- (void)treeOpensPath:(NSString *)path {
    _treeOpeningPath = path;
}

- (void)holdTreeFollowing:(BOOL)hold {
    _treeFollowHeld += hold ? 1 : -1;
    if (_treeFollowHeld < 0) _treeFollowHeld = 0;
}

- (void)treeAutoRevealSettingsApplied {
    const BOOL on = [self treeAutoRevealEnabled];
    const BOOL turnedOn = on && !_treeAutoRevealOn && _treeAutoRevealSeen;
    _treeAutoRevealOn = on;
    _treeAutoRevealSeen = YES;
    if (!turnedOn || _treeFollowHeld > 0 || ![self treeOnShow]) return;
    if (NSString *path = [self treeFollowablePath]) [self treeRevealFollowed:path];
}

@end
