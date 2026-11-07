// EditorController+Tree.mm — EditorController: the file tree: its data source, drag and drop, FSEvents, worktree rows, file operations, opening a folder.
#import "EditorController+Private.h"
#import "EditorController+TreeIvars.h"

@implementation EditorController (Tree)

// The tree's Sort By items, targeting no one (the main menu's View > Sort
// File Tree By reaches the key window's controller); the tree's own menu
// targets them at its controller.
+ (NSMenu *)treeSortMenu {
    NSMenu *sort = [[NSMenu alloc] initWithTitle:@"Sort By"];
    const struct { NSString *title; TreeSort::Key key; } keys[] = {
        {@"Name", TreeSort::Key::Name},
        {@"Date Modified", TreeSort::Key::Modified},
        {@"Size", TreeSort::Key::Size},
        {@"Date Created", TreeSort::Key::Created},
    };
    for (const auto &k : keys)
        [sort addItemWithTitle:k.title action:@selector(sortTreeBy:) keyEquivalent:@""].tag =
            (NSInteger)k.key;
    [sort addItem:[NSMenuItem separatorItem]];
    [sort addItemWithTitle:@"Ascending" action:@selector(setTreeSortDirection:)
             keyEquivalent:@""].tag = 1;
    [sort addItemWithTitle:@"Descending" action:@selector(setTreeSortDirection:)
             keyEquivalent:@""].tag = 0;
    [sort addItem:[NSMenuItem separatorItem]];
    [sort addItemWithTitle:@"Folders First" action:@selector(toggleFoldersFirst:)
             keyEquivalent:@""];
    return sort;
}

- (NSMenu *)buildTreeContextMenu {
    NSMenu *m = [[NSMenu alloc] init];
    m.delegate = self;   // menuNeedsUpdate: shows a worktree's items only on a worktree
    [m addItemWithTitle:@"Compare with Main" action:@selector(compareWorktreeWithMain:)
          keyEquivalent:@""].tag = kWorktreeMenuTag;
    [m addItemWithTitle:@"Open in New Window" action:@selector(openWorktreeInNewWindow:)
          keyEquivalent:@""].tag = kWorktreeMenuTag;
    NSMenuItem *wtLine = [NSMenuItem separatorItem];
    wtLine.tag = kWorktreeMenuTag;
    [m addItem:wtLine];
    [m addItemWithTitle:@"New File…" action:@selector(newFile:) keyEquivalent:@""];
    [m addItemWithTitle:@"New Folder…" action:@selector(newFolder:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Rename…" action:@selector(renameSelected:) keyEquivalent:@""];
    [m addItemWithTitle:@"Move to Trash" action:@selector(deleteSelected:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Reveal in Finder" action:@selector(revealInFinder:) keyEquivalent:@""];
    [m addItemWithTitle:@"Copy Path" action:@selector(copyPath:) keyEquivalent:@""];
    [m addItemWithTitle:@"Copy Relative Path" action:@selector(copyRelativePath:)
          keyEquivalent:@""];
    [m addItemWithTitle:@"Refresh" action:@selector(refreshTree:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    // How the tree is ordered (remembered per folder), and the sizes and
    // dates at the right of each row (each its own switch, for every window).
    NSMenu *sort = [EditorController treeSortMenu];
    for (NSMenuItem *it in sort.itemArray) it.target = self;
    NSMenuItem *sortItem = [m addItemWithTitle:@"Sort By" action:nil keyEquivalent:@""];
    sortItem.submenu = sort;
    for (NSMenuItem *it in MCTreeColumnMenuItems()) [m addItem:it];
    for (NSMenuItem *it in m.itemArray) if (it.action) it.target = self;
    MCAddAdjustSettingsItem(m, @"tree", self.outline);
    return m;
}

// Cmd+Shift+. : show or hide dotfiles in the tree (like Finder).
- (void)toggleHiddenFiles:(id)sender {
    gShowHidden = !gShowHidden;
    [self refreshTree:nil];
}

// ------------------------------------------------------- NSOutlineView source
- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    return (NSInteger)[self childrenOf:item ?: _root].count;
}
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)i ofItem:(id)item {
    return [self childrenOf:item ?: _root][(NSUInteger)i];
}
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item {
    return ((FileItem *)item).isDir;
}

- (NSView *)outlineView:(NSOutlineView *)ov
     viewForTableColumn:(NSTableColumn *)col
                   item:(id)item {
    MCTreeCell *cell = [ov makeViewWithIdentifier:@"cell" owner:self];
    if (!cell) {
        cell = [[MCTreeCell alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"cell";
    }
    [self configureTreeCell:cell item:item];
    return cell;
}

// ---------------------------------------------------- tree drag and drop
- (id<NSPasteboardWriting>)outlineView:(NSOutlineView *)ov
               pasteboardWriterForItem:(id)item {
    return [NSURL fileURLWithPath:((FileItem *)item).path];
}

- (void)outlineView:(NSOutlineView *)ov draggingSession:(NSDraggingSession *)session
   willBeginAtPoint:(NSPoint)pt forItems:(NSArray *)items {
    ((ClickOutline *)ov).dragBegan = YES;
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (FileItem *it in items) [paths addObject:it.path];
    _draggedPaths = paths;
}

- (void)outlineView:(NSOutlineView *)ov draggingSession:(NSDraggingSession *)session
       endedAtPoint:(NSPoint)pt operation:(NSDragOperation)op {
    _draggedPaths = nil;
}

// What a drop would move or copy: the tree's own rows, else file URLs.
- (NSArray<NSString *> *)droppedPaths:(id<NSDraggingInfo>)info local:(BOOL)local {
    if (local) return _draggedPaths ?: @[];
    NSArray<NSURL *> *urls = [info.draggingPasteboard
        readObjectsForClasses:@[NSURL.class]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) if (u.path) [paths addObject:u.path];
    return paths;
}

// A move into a folder makes sense unless the item is already there, or the
// folder is the item itself or inside it.
static BOOL MCCanDropPath(NSString *src, NSString *dir) {
    NSString *s = src.stringByStandardizingPath, *d = dir.stringByStandardizingPath;
    if ([s.stringByDeletingLastPathComponent isEqualToString:d]) return NO;
    if ([d isEqualToString:s] || [d hasPrefix:[s stringByAppendingString:@"/"]]) return NO;
    return YES;
}

- (NSDragOperation)outlineView:(NSOutlineView *)ov validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item proposedChildIndex:(NSInteger)index {
    // Always a drop onto a folder: a file stands for its folder, the space
    // between rows for the folder they are in, the empty space for the root.
    FileItem *target = item;
    if (target && !target.isDir) target = [ov parentForItem:target];
    [ov setDropItem:target dropChildIndex:NSOutlineViewDropOnItemIndex];
    NSString *dir = target ? target.path : _root.path;
    if (!dir) return NSDragOperationNone;
    BOOL local = info.draggingSource == ov;
    NSArray<NSString *> *paths = [self droppedPaths:info local:local];
    if (!paths.count) return NSDragOperationNone;
    for (NSString *p in paths)
        if (!MCCanDropPath(p, dir)) return NSDragOperationNone;
    return local ? NSDragOperationMove : NSDragOperationCopy;
}

- (BOOL)outlineView:(NSOutlineView *)ov acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item childIndex:(NSInteger)index {
    FileItem *target = item;   // a folder or nil, as validateDrop set it
    NSString *dir = target ? target.path : _root.path;
    BOOL local = info.draggingSource == ov;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSString *> *problems = [NSMutableArray array];
    NSString *last = nil;
    for (NSString *src in [self droppedPaths:info local:local]) {
        if (!MCCanDropPath(src, dir)) continue;
        NSString *dst = [dir stringByAppendingPathComponent:src.lastPathComponent];
        if ([fm fileExistsAtPath:dst]) {
            [problems addObject:[NSString stringWithFormat:
                @"“%@” already exists in “%@”.", dst.lastPathComponent,
                dir.lastPathComponent]];
            continue;
        }
        NSError *err = nil;
        BOOL ok = local ? [fm moveItemAtPath:src toPath:dst error:&err]
                        : [fm copyItemAtPath:src toPath:dst error:&err];
        if (!ok) { [problems addObject:err.localizedDescription]; continue; }
        if (local) [self pathMovedFrom:src to:dst];
        last = dst;
    }
    [self refreshTree:nil];
    if (last) [self revealPath:last andOpen:NO];
    // Not a modal alert inside the drop: Finder's side of the drag would
    // wait on it.
    if (problems.count) {
        NSString *msg = [problems componentsJoinedByString:@"\n"];
        dispatch_async(dispatch_get_main_queue(), ^{ [self warn:msg]; });
    }
    return last != nil;
}

// Every open tab follows a file or folder that moved; the one on show also
// tells the language server and the previews.
- (void)pathMovedFrom:(NSString *)src to:(NSString *)dst {
    [self.navigator pathMovedFrom:src to:dst];
    NSString *(^follow)(NSString *) = ^NSString *(NSString *p) {
        if ([p isEqualToString:src]) return dst;
        NSString *prefix = [src stringByAppendingString:@"/"];
        if ([p hasPrefix:prefix])
            return [dst stringByAppendingPathComponent:[p substringFromIndex:prefix.length]];
        return nil;
    };
    BOOL any = NO, activeMoved = NO;
    for (MCTab *t in _tabs) {
        NSString *moved = t.currentPath ? follow(t.currentPath) : nil;
        if (!moved) continue;
        t.currentPath = moved;
        any = YES;
        if (t == _activeTab) activeMoved = YES;
        // A hidden LaTeX preview: its \input and pictures now resolve from the
        // new folder too.
        else if (t.latex) [t.latex setPath:moved source:t.sourceText ?: @""];
    }
    if (any) { [self reloadTabStrip]; [self saveSession]; }
    if (!activeMoved) {
        if (any) [self.lsp tabsChanged];   // the files open on the servers follow
        return;
    }
    NSString *moved = self.currentPath;
    [self.lsp documentOpened:self.previewMode ? nil : [self languageServerPath:moved]];
    // Relative \input and pictures now resolve from the new folder.
    if (self.isLatex && self.latex) [self.latex setPath:moved source:self.sourceText ?: @""];
    if (self.isMarkdown && self.previewMode) [self recolorEditor];
    [self updateTitle];
}

// Primary open path: driven from ClickOutline's mouseDown, so it always fires.
- (void)handleRowClick:(NSInteger)row atPoint:(NSPoint)pt {
    if (row < 0) return;
    FileItem *node = [self.outline itemAtRow:row];
    if (!node) return;
    if (node.isDir) {
        // Clicking the disclosure triangle: let the native handler toggle it.
        // Clicking the folder name: toggle it ourselves (VS Code behavior).
        NSRect triangle = [self.outline frameOfOutlineCellAtRow:row];
        if (NSPointInRect(pt, triangle)) return;
        if ([self.outline isItemExpanded:node]) [self.outline collapseItem:node];
        else                                    [self.outline expandItem:node];
        return;
    }
    [self openFileAtPath:node.path];
}

// Keyboard activation (Return in the tree): open a file, or toggle a folder.
// Arrow keys only move the selection; they do not open, so you can browse
// freely without triggering loads or unsaved-changes prompts.
- (void)activateRow:(NSInteger)row {
    if (row < 0) return;
    FileItem *node = [self.outline itemAtRow:row];
    if (!node) return;
    if (node.isDir) {
        if ([self.outline isItemExpanded:node]) [self.outline collapseItem:node];
        else                                    [self.outline expandItem:node];
        return;
    }
    [self openFileAtPath:node.path];
    [self focusEditor:nil];   // opened it to work on it
}

// ------------------------------------------------ live file-tree (FSEvents)
static void FSCallback(ConstFSEventStreamRef stream, void *info, size_t n,
                       void *paths, const FSEventStreamEventFlags flags[],
                       const FSEventStreamEventId ids[]) {
    (void)stream; (void)ids;
    EditorController *self = (__bridge EditorController *)info;
    // Folder totals under the folders that changed are counted again.
    char **changed = (char **)paths;
    for (size_t i = 0; i < n; i++) {
        [self folderChangedOnDisk:@(changed[i])
                          subtree:(flags[i] & (kFSEventStreamEventFlagMustScanSubDirs |
                                               kFSEventStreamEventFlagRootChanged)) != 0];
        [self agentFolderChangedOnDisk:@(changed[i])];   // a worktree made or committed to
    }
    [self refreshTree:nil];
    if (self.gitPanelVisible) [self.gitPanel refresh];   // a commit or edit made elsewhere
    [self checkTabsOnDisk];   // a tab's file moved or deleted in the terminal
}

- (void)startWatching:(NSString *)path {
    [self stopWatching];
    if (!path) return;
    FSEventStreamContext ctx = {0, (__bridge void *)self, NULL, NULL, NULL};
    CFStringRef cfPath = (__bridge CFStringRef)path;
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&cfPath, 1, NULL);
    _fsStream = FSEventStreamCreate(
        NULL, &FSCallback, &ctx, paths, kFSEventStreamEventIdSinceNow,
        0.3 /* seconds of coalescing */, kFSEventStreamCreateFlagNone);
    CFRelease(paths);
    if (_fsStream) {
        FSEventStreamSetDispatchQueue(_fsStream, dispatch_get_main_queue());
        FSEventStreamStart(_fsStream);
    }
}

- (void)stopWatching {
    if (!_fsStream) return;
    FSEventStreamStop(_fsStream);
    FSEventStreamInvalidate(_fsStream);
    FSEventStreamRelease(_fsStream);
    _fsStream = NULL;
}

// Re-scan the tree, keeping expanded folders and the selection where possible.
// Refresh from the menu also counts every folder total again.
- (void)refreshTree:(id)sender {
    if (sender) [_sizer reset];
    [self reloadTreeKeepingState:YES];
}

// Rebuild the rows, from the disk when `rescan`, else only in a new order,
// keeping the expanded folders, the selected row and the scroll position.
- (void)reloadTreeKeepingState:(BOOL)rescan {
    NSString *selPath = nil;
    if (self.outline.selectedRow >= 0)
        selPath = [(FileItem *)[self.outline itemAtRow:self.outline.selectedRow] path];

    NSMutableSet<NSString *> *expanded = [NSMutableSet set];
    for (NSInteger r = 0; r < self.outline.numberOfRows; r++) {
        FileItem *it = [self.outline itemAtRow:r];
        if ([self.outline isItemExpanded:it]) [expanded addObject:it.path];
    }
    NSClipView *clip = self.sidebarScroll.contentView;
    const NSPoint origin = clip.bounds.origin;

    if (rescan) [_root refresh];
    if (rescan) [_worktreesItem refresh];
    [self.outline reloadData];
    [self reExpand:_root usingSet:expanded];
    if (_worktreesItem && [expanded containsObject:_worktreesItem.path]) {
        [self.outline expandItem:_worktreesItem];
        [self reExpand:_worktreesItem usingSet:expanded];
    }

    if (selPath) {
        for (NSInteger r = 0; r < self.outline.numberOfRows; r++) {
            if ([[(FileItem *)[self.outline itemAtRow:r] path] isEqual:selPath]) {
                [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:r]
                          byExtendingSelection:NO];
                break;
            }
        }
    }
    if (clip) {
        NSRect want = clip.bounds;
        want.origin = origin;
        [clip scrollToPoint:[clip constrainBoundsRect:want].origin];
        [self.sidebarScroll reflectScrolledClipView:clip];
    }
}

- (void)reExpand:(FileItem *)node usingSet:(NSSet<NSString *> *)expanded {
    for (FileItem *child in node.children) {
        if (child.isDir && [expanded containsObject:child.path]) {
            [self.outline expandItem:child];
            [self reExpand:child usingSet:expanded];
        }
    }
}

// R2.8: the worktrees Claude Code makes under <folder>/.claude/worktrees
// show as one Worktrees node first in the folder, hidden files or not, and
// not again as plain folders inside .claude.
- (NSString *)worktreesFolder {
    return _root.path ? [_root.path stringByAppendingPathComponent:@".claude/worktrees"] : nil;
}

- (NSArray<FileItem *> *)withWorktreesNode:(NSArray<FileItem *> *)shown of:(FileItem *)node {
    NSString *dir = [self worktreesFolder];
    if (!dir) return shown;
    if (node == _root) {
        BOOL any = NO;
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
            BOOL isDir = NO;
            if (![name hasPrefix:@"."] &&
                [NSFileManager.defaultManager fileExistsAtPath:[dir stringByAppendingPathComponent:name]
                                                   isDirectory:&isDir] && isDir) {
                any = YES;
                break;
            }
        }
        if (!any) {
            _worktreesItem = nil;
            return shown;
        }
        if (![_worktreesItem.path isEqualToString:dir]) {
            _worktreesItem = [FileItem new];
            _worktreesItem.path = dir;
            [_worktreesItem readStat];
        }
        return [@[_worktreesItem] arrayByAddingObjectsFromArray:shown];
    }
    if ([node.path isEqualToString:dir.stringByDeletingLastPathComponent]) {
        NSMutableArray *out = [shown mutableCopy];
        for (FileItem *c in shown)
            if ([c.path isEqualToString:dir]) [out removeObject:c];
        return out;
    }
    return shown;
}

- (BOOL)isWorktreeRow:(FileItem *)node {
    return node && _worktreesItem &&
           [node.path.stringByDeletingLastPathComponent isEqualToString:_worktreesItem.path];
}

// The Worktrees node's title, and a worktree's: its agent's description and
// how it stands against the main branch ("3 ahead · modified").
- (NSAttributedString *)worktreeTitleFor:(FileItem *)node color:(NSColor *)text
                                    icon:(NSString **)icon tint:(NSColor **)tint
                                     tip:(NSString **)tip {
    NSFont *font = [NSFont systemFontOfSize:12.5];
    if (node == _worktreesItem) {
        *icon = @"arrow.triangle.branch";
        *tint = Hex(0x9CA3AF);
        *tip = [NSString stringWithFormat:@"The agents' worktrees, in %@\n"
                @"Right click one: Compare with Main, Open in New Window",
                [node.path stringByAbbreviatingWithTildeInPath]];
        return [[NSAttributedString alloc] initWithString:@"Worktrees"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5 weight:NSFontWeightMedium],
                         NSForegroundColorAttributeName: text}];
    }
    if (![self isWorktreeRow:node]) return nil;
    MCAgentWorktree *w = [_agents worktreeAtPath:node.path];
    *icon = @"arrow.triangle.branch";
    *tint = w.running ? Hex(0x73C991) : Hex(0xC09553);
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:w.label ?: node.name
            attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: text}];
    NSString *badge = w.badge;
    if (badge.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"  " stringByAppendingString:badge]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.55]}]];
    // A long description gives way in its middle, so the badge stays in view.
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [s addAttribute:NSParagraphStyleAttributeName value:ps range:NSMakeRange(0, s.length)];
    NSMutableArray *lines = [NSMutableArray arrayWithObject:w.label ?: node.name];
    [lines addObject:[node.path stringByAbbreviatingWithTildeInPath]];
    if (w.branch.length) [lines addObject:[@"Branch " stringByAppendingString:w.branch]];
    if (badge.length) [lines addObject:badge];
    if (w.running) [lines addObject:@"Its agent is running"];
    [lines addObject:@"Right click: Compare with Main, Open in New Window"];
    *tip = [lines componentsJoinedByString:@"\n"];
    return s;
}

- (FileItem *)worktreeMenuItem {
    FileItem *it = [self clickedOrSelectedItem];
    return [self isWorktreeRow:it] ? it : nil;
}

// The tree's right click on a worktree (R2.9).
- (void)compareWorktreeWithMain:(id)sender {
    (void)sender;
    FileItem *it = [self worktreeMenuItem];
    if (it) [_agents compareWorktreeAt:it.path];
}
- (void)openWorktreeInNewWindow:(id)sender {
    (void)sender;
    FileItem *it = [self worktreeMenuItem];
    if (it) [_agents openFolderInNewWindow:it.path];
}

// Only a worktree's right click offers its two items.
- (void)menuNeedsUpdate:(NSMenu *)menu {
    if (menu != self.outline.menu) return;
    const BOOL wt = [self worktreeMenuItem] != nil;
    for (NSMenuItem *item in menu.itemArray)
        if (item.tag == kWorktreeMenuTag) item.hidden = !wt;
}

- (void)outlineViewItemDidExpand:(NSNotification *)note {
    if (note.userInfo[@"NSObject"] == _worktreesItem) [_agents.model refreshWorktrees];
}

// -------------------------------------------------- file operations (tree)
// Where a new file/folder should go: the selected folder, the selected file's
// folder, or the tree root.
- (NSString *)targetDirectory {
    NSInteger row = self.outline.clickedRow >= 0 ? self.outline.clickedRow
                                                 : self.outline.selectedRow;
    if (row < 0) return _root.path;
    FileItem *node = [self.outline itemAtRow:row];
    return node.isDir ? node.path : [node.path stringByDeletingLastPathComponent];
}

- (FileItem *)clickedOrSelectedItem {
    NSInteger row = self.outline.clickedRow >= 0 ? self.outline.clickedRow
                                                 : self.outline.selectedRow;
    return row >= 0 ? [self.outline itemAtRow:row] : nil;
}

// A simple modal name prompt. Returns nil if cancelled or empty.
- (NSString *)promptForName:(NSString *)title default:(NSString *)initial {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    NSTextField *field =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = initial ?: @"";
    a.accessoryView = field;
    [a.window setInitialFirstResponder:field];
    if ([a runModal] != NSAlertFirstButtonReturn) return nil;
    NSString *name = [field.stringValue stringByTrimmingCharactersInSet:
                      [NSCharacterSet whitespaceCharacterSet]];
    return name.length ? name : nil;
}

- (void)newFile:(id)sender {
    NSString *name = [self promptForName:@"New file name:" default:@""];
    if (!name) return;
    NSString *path = [[self targetDirectory] stringByAppendingPathComponent:name];
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:path]) { [self warn:@"A file with that name already exists."]; return; }
    if (![fm createFileAtPath:path contents:[NSData data] attributes:nil]) {
        [self warn:@"Could not create the file."]; return;
    }
    [self refreshTree:nil];
    [self revealPath:path andOpen:YES];
}

- (void)newFolder:(id)sender {
    NSString *name = [self promptForName:@"New folder name:" default:@""];
    if (!name) return;
    NSString *path = [[self targetDirectory] stringByAppendingPathComponent:name];
    NSError *err = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:path
            withIntermediateDirectories:NO attributes:nil error:&err]) {
        [self warn:err.localizedDescription]; return;
    }
    [self refreshTree:nil];
    [self revealPath:path andOpen:NO];
}

- (void)renameSelected:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    if (!node) { NSBeep(); return; }
    [self renamePath:node.path];
}

// Rename… from the tree and from a tab's menu.
- (void)renamePath:(NSString *)path {
    NSString *name = [self promptForName:@"Rename to:"
                                 default:path.lastPathComponent];
    if (!name) return;
    NSString *dst = [[path stringByDeletingLastPathComponent]
                        stringByAppendingPathComponent:name];
    NSError *err = nil;
    if (![[NSFileManager defaultManager] moveItemAtPath:path
                                                 toPath:dst error:&err]) {
        [self warn:err.localizedDescription]; return;
    }
    [self pathMovedFrom:path to:dst];   // open tabs follow, a folder's too
    [self refreshTree:nil];
    [self revealPath:dst andOpen:NO];
}

- (void)deleteSelected:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    if (!node) { NSBeep(); return; }
    [self trashPath:node.path];
}

// Move to Trash from the tree and from a tab's menu, after asking.
- (void)trashPath:(NSString *)path {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Move “%@” to the Trash?",
                     path.lastPathComponent];
    [a addButtonWithTitle:@"Move to Trash"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    NSError *err = nil;
    NSURL *url = [NSURL fileURLWithPath:path];
    if (![[NSFileManager defaultManager] trashItemAtURL:url
                                       resultingItemURL:nil error:&err]) {
        [self warn:err.localizedDescription]; return;
    }
    [self.navigator pathRemoved:path];
    // Tabs of what went away close, a folder's too, unless they hold unsaved
    // changes: those stay open, and saving writes the file again.
    NSString *prefix = [path stringByAppendingString:@"/"];
    for (MCTab *t in [_tabs copy]) {
        if (t.dirty || !t.currentPath) continue;
        if ([t.currentPath isEqualToString:path] || [t.currentPath hasPrefix:prefix])
            [self closeTab:t prompt:NO];
    }
    [self refreshTree:nil];
}

- (void)revealInFinder:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    [self revealPathInFinder:node ? node.path : _root.path];
}

- (void)revealPathInFinder:(NSString *)path {
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:
        @[[NSURL fileURLWithPath:path]]];
}

// Puts an absolute path on the clipboard as plain text: the clicked item's,
// else the selected one's, else the open folder's (as Reveal in Finder picks).
- (void)copyPath:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    [self copyText:node ? node.path : _root.path];
}

// The same path from the open folder ("src/main.mm"); "." for the folder.
- (void)copyRelativePath:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    NSString *p = node ? node.path : _root.path;
    [self copyText:[self pathFromRoot:p] ?: p];
}

- (void)copyText:(NSString *)text {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:text forType:NSPasteboardTypeString];
}

// path from the open folder, or nil when it is outside it. Compared with
// symlinks resolved, since the tree may say /tmp where a file says
// /private/tmp.
- (NSString *)pathFromRoot:(NSString *)path {
    NSString *root = _root.path.stringByResolvingSymlinksInPath;
    NSString *p = path.stringByResolvingSymlinksInPath;
    if (!root.length) return nil;
    if ([p isEqualToString:root]) return @".";
    NSString *prefix = [root hasSuffix:@"/"] ? root : [root stringByAppendingString:@"/"];
    return [p hasPrefix:prefix] ? [p substringFromIndex:prefix.length] : nil;
}

// Expand ancestor folders down to path, select it, and optionally open it.
- (void)revealPath:(NSString *)path andOpen:(BOOL)open {
    NSArray<NSString *> *rootParts = _root.path.pathComponents;
    NSArray<NSString *> *parts = path.pathComponents;
    FileItem *node = _root;
    NSString *acc = _root.path;
    // A worktree's files are reached through the Worktrees node.
    if (_worktreesItem && [path hasPrefix:[_worktreesItem.path stringByAppendingString:@"/"]]) {
        node = _worktreesItem;
        acc = _worktreesItem.path;
        rootParts = acc.pathComponents;
        [self.outline expandItem:node];
    }
    for (NSUInteger i = rootParts.count; i < parts.count; i++) {
        acc = [acc stringByAppendingPathComponent:parts[i]];
        [node loadChildren];
        FileItem *next = nil;
        for (FileItem *c in node.children)
            if ([c.path isEqual:acc]) { next = c; break; }
        // Made since the tree listed this folder (by an agent, the terminal,
        // a language server): list the folder again rather than wait for
        // FSEvents. A Problems row or a Quick Open file opened nothing until
        // the tree refreshed.
        if (!next && node.loaded && [[NSFileManager defaultManager] fileExistsAtPath:acc]) {
            [node refresh];
            [self.outline reloadItem:node == _root ? nil : node reloadChildren:YES];
            for (FileItem *c in node.children)
                if ([c.path isEqual:acc]) { next = c; break; }
        }
        if (!next) break;
        if (next.isDir && i < parts.count - 1) [self.outline expandItem:next];
        node = next;
    }
    NSInteger row = [self.outline rowForItem:node];
    if (row >= 0) {
        [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                  byExtendingSelection:NO];
        [self.outline scrollRowToVisible:row];
    }
    if (open && !node.isDir) {
        [self openFileAtPath:node.path];
    } else if (open && ![node.path isEqualToString:path]) {
        // Not in the tree (a dotfile it hides): a file is opened anyway.
        BOOL isDir = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] && !isDir)
            [self openFileAtPath:path];
    }
}

// revealPath: opens only what the tree lists, and the tree leaves dotfiles
// out unless asked, so a file from Finder is opened directly: selected in the
// tree when the tree has it, and shown either way.
- (BOOL)showFile:(NSString *)path {
    [self revealPath:path andOpen:NO];
    [self openFileAtPath:path];
    return [self.currentPath isEqualToString:path];
}

- (void)warn:(NSString *)msg {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = msg;
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}

// ------------------------------------------------------------- open folder
- (void)openFolder:(id)sender {
    if (![self confirmCloseAllTabs]) return;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK)
        [self changeRootTo:panel.URLs.firstObject.path];
}

// `minicode -r folder`: this window moves to the folder, asking about
// unsaved tabs first as Open Folder does.
- (BOOL)switchToFolder:(NSString *)dir {
    if ([_root.path.stringByResolvingSymlinksInPath
            isEqualToString:dir.stringByResolvingSymlinksInPath]) return YES;
    if (![self confirmCloseAllTabs]) return NO;
    [self changeRootTo:dir];
    return YES;
}

- (void)changeRootTo:(NSString *)dir {
    [self saveSession];       // the old folder's tabs, for when it is back
    [self dropAllTabs];       // no stale preview or image over the new folder
    [self.navigator clear];   // places in the old folder
    _root = [FileItem new];
    _root.path = dir; _root.isDir = YES;
    [self rootChanged];
    [_root loadChildren];
    [self.lsp setRoot:dir];   // servers belong to the old folder
    [self.outline reloadData];
    [self.terminalPanel setDirectory:dir];   // keep terminal cwds in sync
    self.terminalPanel.projectRoot = dir;
    self.gitPanel.root = dir;           // the panel follows the folder
    [_agents setRoot:dir];              // and so do the agents
    [self startWatching:dir];           // watch the new folder
    [self updateTitle];
    [self restoreSession];              // the new folder's tabs from last time
}

// ----------------------------------------------------- the control channel
// What ControlCommands.mm asks of a window (`minicode open`, `term`, `state`
// and the rest), and what the agent notes need (MCAgentNotesHost). None of
// it moves the keyboard: an agent's command lands beside the user's work,
// never in its way, so whatever had the keyboard has it again afterwards.

@end
