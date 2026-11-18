// TreeFocusSuite.mm — `make guitest SUITES=tree_focus`: Focus on Folder. A
// folder's right click offers it (a file's does not); taken, the tree's
// root is that folder, a breadcrumb row above it names the way down from
// the window's folder, and the window's folder is unchanged. A file made in
// the focused folder shows; a click on a part of the breadcrumb roots the
// tree there with the folders that were open still open, the first part or
// the × shows the whole folder, and so does Show Whole Folder in Tree. An
// open outside the focus (Quick Open) leaves it; a reveal widens it to the
// nearest folder above both, Reveal Active File in File Tree too. The
// focus comes back with its window and with its folder's session, a saved
// focus on a folder gone since comes back as the whole folder, and the
// focused folder deleted on disk gives the whole folder back.
//
// Add to Focus (2026-11-18): several folders, each a root row labelled with
// its path, under a row of their labels. From the whole folder it is Focus
// on Folder; a second folder (the open file's, from the palette) gives two
// roots, open, and the label row; a file made in a root shows; a folder
// inside a root changes nothing, one holding a root takes its place; a
// reveal outside them adds its folder; a label's × and Remove from Focus
// take one out, down to one giving the breadcrumb back; Focus on Folder
// replaces the set; the last × clears it. The set comes back with its
// window (each root open or closed as it was) and its folder's session,
// folders gone since dropped; a record in the old single-path form still
// restores; a root deleted on disk leaves the others.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "SessionStore.h"
#import "TreeFocusBar.h"

#include "TabSessions.h"

@interface EditorController (TreeFocusSuite)
- (TabSessions::Window)windowRecordNow;
@end
@interface NSObject (TreeFocusSuiteApp)
- (EditorController *)openWindowAtPath:(NSString *)path restoring:(NSString *)record;
@end

namespace {

FileItem *ItemAt(EditorController *c, NSString *path) {
    for (NSInteger r = 0; r < c.outline.numberOfRows; r++) {
        FileItem *it = [c.outline itemAtRow:r];
        if ([it.path isEqualToString:path]) return it;
    }
    return nil;
}

NSInteger RowOf(EditorController *c, NSString *path) {
    FileItem *it = ItemAt(c, path);
    return it ? [c.outline rowForItem:it] : -1;
}

void Select(EditorController *c, NSString *path) {
    const NSInteger row = RowOf(c, path);
    if (row >= 0)
        [c.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    else
        MCGuiNote(@"no row for %@", path);
}

NSString *Selected(EditorController *c) {
    return c.outline.selectedRow >= 0 ? [(FileItem *)[c.outline itemAtRow:c.outline.selectedRow] path] : nil;
}

BOOL Expanded(EditorController *c, NSString *path) {
    FileItem *it = ItemAt(c, path);
    return it && [c.outline isItemExpanded:it];
}

// The names of the rows at the top of the tree, in order.
NSString *TopRows(EditorController *c) {
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    const NSInteger n = [c.outline.dataSource outlineView:c.outline numberOfChildrenOfItem:nil];
    for (NSInteger i = 0; i < n; i++)
        [names addObject:[(FileItem *)[c.outline.dataSource outlineView:c.outline child:i ofItem:nil] name]];
    return [names componentsJoinedByString:@" "];
}

NSString *Labels(EditorController *c) {
    return [[c treeFocusBar].partLabels componentsJoinedByString:@" › "];
}

// A press at the middle of `r` in the bar, as the mouse makes one.
void ClickBar(EditorController *c, NSRect r) {
    MCTreeFocusBar *bar = [c treeFocusBar];
    if (NSIsEmptyRect(r)) {
        MCGuiNote(@"nothing drawn there (the bar is %@)", NSStringFromRect(bar.frame));
        GCHECK(!NSIsEmptyRect(r));
        return;
    }
    const NSPoint p = [bar convertPoint:NSMakePoint(NSMidX(r), NSMidY(r)) toView:nil];
    NSEvent *e = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:p modifierFlags:0
                                   timestamp:0 windowNumber:c.window.windowNumber context:nil
                                 eventNumber:0 clickCount:1 pressure:1];
    [bar mouseDown:e];
}

NSMenuItem *TreeMenuItem(EditorController *c, SEL action) {
    NSMenu *m = c.outline.menu;
    [c menuNeedsUpdate:m];
    for (NSMenuItem *it in m.itemArray)
        if (it.action == action) return it;
    return nil;
}

// The focus the folder's own session keeps, several joined by ", ".
NSString *StoredFocus(NSString *folder) {
    const TabSessions::Store store = MCSessionsRead();
    auto found = store.folders.find(folder.stringByResolvingSymlinksInPath.UTF8String);
    if (found == store.folders.end()) return @"(no session)";
    NSMutableArray<NSString *> *list = [NSMutableArray array];
    for (const std::string &f : found->second.treeFocus) [list addObject:@(f.c_str())];
    return [list componentsJoinedByString:@", "];
}

// The top rows as the tree labels them (a root of several by its path).
NSString *TopLabels(EditorController *c) {
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    const NSInteger n = [c.outline.dataSource outlineView:c.outline numberOfChildrenOfItem:nil];
    for (NSInteger i = 0; i < n; i++) {
        FileItem *it = [c.outline.dataSource outlineView:c.outline child:i ofItem:nil];
        [names addObject:[c treeRootLabelFor:it] ?: it.name];
    }
    return [names componentsJoinedByString:@" "];
}

// What a row's cell says on screen.
NSString *CellText(EditorController *c, NSString *path) {
    const NSInteger row = RowOf(c, path);
    if (row < 0) return @"(no row)";
    NSTableCellView *cell = [c.outline viewAtColumn:0 row:row makeIfNecessary:YES];
    return cell.textField.stringValue ?: @"(no text)";
}

// Closes a window, and checks it went (a window left would be a later
// check's, and the suite's leak).
void CloseChecked(EditorController *w) {
    const NSUInteger before = MCGuiControllers().count;
    [w.window performClose:nil];
    const NSUInteger after = MCGuiControllers().count;
    if (after != before - 1 || [MCGuiControllers() containsObject:w])
        MCGuiNote(@"closing %@ (focus \"%@\"): %lu windows, then %lu", w.rootPath.lastPathComponent,
                  w.treeFocus, (unsigned long)before, (unsigned long)after);
    GCHECK(![MCGuiControllers() containsObject:w]);
}

NSInteger RowsOf(EditorController *c, NSString *path) {
    NSInteger n = 0;
    for (NSInteger r = 0; r < c.outline.numberOfRows; r++)
        if ([[(FileItem *)[c.outline itemAtRow:r] path] isEqualToString:path]) n++;
    return n;
}

}  // namespace

MC_GUI_SUITE(tree_focus) {
    __block EditorController *c = nil;
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"focusrepo"];
    auto file = ^NSString *(NSString *rel) { return [repo stringByAppendingPathComponent:rel]; };

    MCGuiStep(@"a folder: mac/src (a.mm, deep/d.txt), mac/tests/t.mm, src/core.cpp, README.md", ^{
        MCGuiWriteFile(file(@"mac/src/a.mm"), @"// a\n");
        MCGuiWriteFile(file(@"mac/src/deep/d.txt"), @"d\n");
        MCGuiWriteFile(file(@"mac/tests/t.mm"), @"// t\n");
        MCGuiWriteFile(file(@"src/core.cpp"), @"// core\n");
        MCGuiWriteFile(file(@"README.md"), @"# focus\n");
    });
    MCGuiStep(@"a window on it; mac, mac/tests and src open", ^{
        const NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c.outline expandItem:ItemAt(c, file(@"mac"))];
        [c.outline expandItem:ItemAt(c, file(@"mac/tests"))];
        [c.outline expandItem:ItemAt(c, file(@"src"))];
        GCHECK_EQ_STR(TopRows(c), @"mac src README.md");
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK(RowOf(c, file(@"mac/tests/t.mm")) >= 0 && RowOf(c, file(@"src/core.cpp")) >= 0);
    });
    MCGuiStep(@"a folder's right click offers Focus on Folder; a file's does not", ^{
        Select(c, file(@"mac/src"));
        NSMenuItem *focus = TreeMenuItem(c, @selector(focusOnFolder:));
        NSMenuItem *whole = TreeMenuItem(c, @selector(showWholeFolderInTree:));
        GCHECK(focus && !focus.hidden);
        GCHECK(whole && whole.hidden);   // nothing to widen yet
        Select(c, file(@"README.md"));
        GCHECK(TreeMenuItem(c, @selector(focusOnFolder:)).hidden);
        // The menu bar's (and the palette's) item is there for a file too:
        // it focuses the file's folder. Show Whole Folder is off.
        NSMenuItem *wholeMain = [[NSMenuItem alloc] initWithTitle:@"" action:@selector(showWholeFolderInTree:)
                                                    keyEquivalent:@""];
        GCHECK(![c validateMenuItem:wholeMain]);
    });
    MCGuiStep(@"Focus on Folder on mac/src: the tree starts there, the breadcrumb says where", ^{
        Select(c, file(@"mac/src"));
        [c focusOnFolder:nil];
        GCHECK_EQ_STR(c.treeFocus, @"mac/src");
        GCHECK_EQ_STR(TopRows(c), @"deep a.mm");
        GCHECK_EQ_STR(c.rootPath, repo);   // the window's folder is unchanged
        MCTreeFocusBar *bar = [c treeFocusBar];
        GCHECK(bar && !bar.hidden && bar.superview == c.sidebarScroll);
        GCHECK_EQ_STR(Labels(c), @"focusrepo › mac › src");
        // The rows start below the bar, never under it.
        const NSRect clip = c.sidebarScroll.contentView.frame;
        if (NSIntersectsRect(NSInsetRect(bar.frame, 0, 0.5), clip))
            MCGuiNote(@"bar %@, clip view %@", NSStringFromRect(bar.frame), NSStringFromRect(clip));
        GCHECK(!NSIntersectsRect(NSInsetRect(bar.frame, 0, 0.5), clip));
        GCHECK(bar.frame.size.height == kMCTreeFocusBarHeight);
        GCHECK(!TreeMenuItem(c, @selector(showWholeFolderInTree:)).hidden);
        // Kept in the folder's session at once.
        GCHECK_EQ_STR(StoredFocus(repo), @"mac/src");
        MCGuiSnapshot(c.sidebarScroll, @"tree_focus-bar");
        [c.outline expandItem:ItemAt(c, file(@"mac/src/deep"))];
        MCGuiWriteFile(file(@"mac/src/new.txt"), @"n\n");
    });
    MCGuiWait(@"a file made in the focused folder shows", 10, ^BOOL {
        return RowOf(c, file(@"mac/src/new.txt")) >= 0;
    });
    MCGuiStep(@"the breadcrumb's mac roots the tree at mac; what was open stays open", ^{
        GCHECK(Expanded(c, file(@"mac/src/deep")));
        ClickBar(c, [[c treeFocusBar] rectOfPart:1]);
        GCHECK_EQ_STR(c.treeFocus, @"mac");
        GCHECK_EQ_STR(Labels(c), @"focusrepo › mac");
        GCHECK_EQ_STR(TopRows(c), @"src tests");
        GCHECK(Expanded(c, file(@"mac/src")) && Expanded(c, file(@"mac/src/deep")));
        GCHECK(Expanded(c, file(@"mac/tests")));   // open before the focus, open again
        GCHECK_EQ_STR(Selected(c), file(@"mac/src"));   // where the tree was rooted
    });
    MCGuiStep(@"its first part shows the whole folder, as it was left", ^{
        ClickBar(c, [[c treeFocusBar] rectOfPart:0]);
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK([c treeFocusBar].hidden);
        GCHECK_EQ_STR(TopRows(c), @"mac src README.md");
        GCHECK(Expanded(c, file(@"mac")) && Expanded(c, file(@"src")) && Expanded(c, file(@"mac/src")));
        GCHECK(RowOf(c, file(@"src/core.cpp")) >= 0);
        // The folder's session keeps no focus (or, with no tab, is gone).
        NSString *stored = StoredFocus(repo);
        if (stored.length && ![stored isEqualToString:@"(no session)"]) MCGuiNote(@"kept: %@", stored);
        GCHECK(!stored.length || [stored isEqualToString:@"(no session)"]);
    });
    MCGuiStep(@"the × returns to the whole folder; so does Show Whole Folder in Tree", ^{
        GCHECK([c focusTreeOn:@"mac/src"]);
        GCHECK_EQ_STR(TopRows(c), @"deep a.mm new.txt");
        ClickBar(c, [[c treeFocusBar] closeRect]);
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK_EQ_STR(TopRows(c), @"mac src README.md");
        GCHECK([c focusTreeOn:@"mac/src"]);
        [c showWholeFolderInTree:nil];
        GCHECK_EQ_STR(c.treeFocus, @"");
        // A folder not in the tree is refused, the tree left as it was.
        GCHECK(![c focusTreeOn:@"mac/nope"]);
        GCHECK_EQ_STR(c.treeFocus, @"");
    });
    MCGuiStep(@"an open outside the focus leaves it; a reveal widens to the nearest folder above both", ^{
        GCHECK([c focusTreeOn:@"mac/src"]);
        [c revealPath:file(@"src/core.cpp") open:YES from:EditorTabs::OpenFrom::QuickOpen];
        GCHECK_EQ_STR(c.currentPath, file(@"src/core.cpp"));
        GCHECK_EQ_STR(c.treeFocus, @"mac/src");
        // A file inside the focus is selected in it as before.
        [c revealPath:file(@"mac/src/a.mm") andOpen:NO];
        GCHECK_EQ_STR(c.treeFocus, @"mac/src");
        GCHECK_EQ_STR(Selected(c), file(@"mac/src/a.mm"));
        [c revealPath:file(@"mac/tests/t.mm") andOpen:NO];
        GCHECK_EQ_STR(c.treeFocus, @"mac");
        GCHECK_EQ_STR(Selected(c), file(@"mac/tests/t.mm"));
        // Reveal Active File in File Tree: core.cpp is beside mac.
        [c revealActiveFileInTree:nil];
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK_EQ_STR(Selected(c), file(@"src/core.cpp"));
    });
    MCGuiStep(@"with no folder selected, the command focuses the selected file's folder", ^{
        [c revealPath:file(@"mac/tests/t.mm") andOpen:NO];
        GCHECK_EQ_STR(Selected(c), file(@"mac/tests/t.mm"));
        [c focusOnFolder:nil];
        GCHECK_EQ_STR(c.treeFocus, @"mac/tests");
        GCHECK_EQ_STR(TopRows(c), @"t.mm");
    });
    __block NSString *record = nil;
    MCGuiStep(@"brought back with its window", ^{
        GCHECK([c focusTreeOn:@"mac/src"]);
        const TabSessions::Window w = [c windowRecordNow];
        GCHECK(w.tabs.treeFocus == std::vector<std::string>{"mac/src"});
        record = @(TabSessions::encodeWindows({w}).c_str());
        EditorController *back = [(NSObject *)NSApp.delegate openWindowAtPath:repo restoring:record];
        GCHECK_EQ_STR(back.treeFocus, @"mac/src");
        GCHECK_EQ_STR(TopRows(back), @"deep a.mm new.txt");
        GCHECK(Expanded(back, file(@"mac/src/deep")));
        GCHECK([back treeFocusBar] && ![back treeFocusBar].hidden);
        GCHECK_EQ_STR(Labels(back), @"focusrepo › mac › src");
        [back.window performClose:nil];
        MCGuiSetKeyWindow(c.window);
    });
    MCGuiStep(@"a window brought back focused on a folder gone since shows the whole folder", ^{
        TabSessions::Window w = TabSessions::decodeWindows(record.UTF8String).at(0);
        w.tabs.treeFocus = {"mac/gone"};
        EditorController *back = [(NSObject *)NSApp.delegate
            openWindowAtPath:repo restoring:@(TabSessions::encodeWindows({w}).c_str())];
        GCHECK_EQ_STR(back.treeFocus, @"");
        GCHECK_EQ_STR(TopRows(back), @"mac src README.md");
        GCHECK(![back treeFocusBar] || [back treeFocusBar].hidden);
        [back.window performClose:nil];
        MCGuiSetKeyWindow(c.window);
    });
    MCGuiStep(@"the folder's own session brings it back: closed and opened again", ^{
        GCHECK_EQ_STR(c.treeFocus, @"mac/src");
        [c.window performClose:nil];
        MCGuiSetKeyWindow(nil);
        GCHECK_EQ_STR(StoredFocus(repo), @"mac/src");
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        c = MCGuiController();
        GCHECK_EQ_STR(c.rootPath, repo);
        GCHECK_EQ_STR(c.treeFocus, @"mac/src");
        GCHECK_EQ_STR(TopRows(c), @"deep a.mm new.txt");
    });
    MCGuiStep(@"focused on deep, deep deleted on disk", ^{
        GCHECK([c focusTreeOn:@"mac/src/deep"]);
        GCHECK_EQ_STR(TopRows(c), @"d.txt");
        [[NSFileManager defaultManager] removeItemAtPath:file(@"mac/src/deep") error:nil];
    });
    MCGuiWait(@"the whole folder again", 10, ^BOOL { return c.treeFocus.length == 0; });
    MCGuiStep(@"the rows are the whole folder's; the bar is gone", ^{
        GCHECK_EQ_STR(TopRows(c), @"mac src README.md");
        GCHECK([c treeFocusBar].hidden);
        GCHECK(RowOf(c, file(@"mac/src/deep")) < 0);
        NSString *stored = StoredFocus(repo);
        if (stored.length && ![stored isEqualToString:@"(no session)"]) MCGuiNote(@"kept: %@", stored);
        GCHECK(!stored.length || [stored isEqualToString:@"(no session)"]);
    });
    MCGuiStep(@"a folder's session focused on a folder gone since opens on the whole folder", ^{
        GCHECK([c focusTreeOn:@"mac/tests"]);
        [c.window performClose:nil];
        MCGuiSetKeyWindow(nil);
        GCHECK_EQ_STR(StoredFocus(repo), @"mac/tests");
        GCHECK([[NSFileManager defaultManager] moveItemAtPath:file(@"mac/tests")
                                                       toPath:file(@"mac/checks") error:nil]);
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        c = MCGuiController();
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK_EQ_STR(TopRows(c), @"mac src README.md");
    });

    // ---- several folders (Add to Focus)
    MCGuiStep(@"more folders: linux/src (l.c, sub/s.c), docs/d.md; mac open", ^{
        MCGuiWriteFile(file(@"linux/src/l.c"), @"// l\n");
        MCGuiWriteFile(file(@"linux/src/sub/s.c"), @"// s\n");
        MCGuiWriteFile(file(@"docs/d.md"), @"# d\n");
        [c refreshTree:nil];
        [c.outline expandItem:ItemAt(c, file(@"mac"))];
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK(RowOf(c, file(@"mac/src")) >= 0);
    });
    MCGuiStep(@"from the whole folder, Add to Focus is Focus on Folder: the breadcrumb", ^{
        Select(c, file(@"mac/src"));
        NSMenuItem *add = TreeMenuItem(c, @selector(addFolderToFocus:));
        GCHECK(add && !add.hidden);
        GCHECK(TreeMenuItem(c, @selector(removeFolderFromFocus:)).hidden);
        [c addFolderToFocus:nil];
        GCHECK_EQ_STR(c.treeFocus, @"mac/src");
        GCHECK_EQ_STR(TopRows(c), @"a.mm new.txt");
        GCHECK(![c treeFocusBar].showsFolders);
        GCHECK_EQ_STR(Labels(c), @"focusrepo › mac › src");
        // A folder inside the focus adds nothing: its right click has no Add to Focus.
        MCGuiWriteFile(file(@"mac/src/inner/i.txt"), @"i\n");
        [c refreshTree:nil];
        Select(c, file(@"mac/src/inner"));
        GCHECK(TreeMenuItem(c, @selector(addFolderToFocus:)).hidden);
        GCHECK(!TreeMenuItem(c, @selector(focusOnFolder:)).hidden);
    });
    MCGuiStep(@"the palette's Add to Focus takes the open file's folder: two roots", ^{
        [c revealPath:file(@"linux/src/l.c") open:YES from:EditorTabs::OpenFrom::QuickOpen];
        GCHECK_EQ_STR(c.currentPath, file(@"linux/src/l.c"));
        GCHECK_EQ_STR(c.treeFocus, @"mac/src");   // an open leaves the focus
        Select(c, file(@"mac/src/a.mm"));         // a row the tree shows already
        GCHECK_EQ_STR([c folderToAddToFocus], @"linux/src");
        [c addFolderToFocus:nil];
        GCHECK((c.treeFocusFolders.count == 2));
        GCHECK_EQ_STR(c.treeFocus, @"mac/src, linux/src");
        GCHECK_EQ_STR(TopLabels(c), @"mac/src linux/src");
        GCHECK_EQ_STR(CellText(c, file(@"linux/src")), @"linux/src");
        GCHECK_EQ_STR(CellText(c, file(@"linux/src/l.c")), @"l.c");
        // Both open, their rows under them; the window's folder unchanged.
        GCHECK(Expanded(c, file(@"mac/src")) && Expanded(c, file(@"linux/src")));
        GCHECK(RowOf(c, file(@"mac/src/a.mm")) >= 0 && RowOf(c, file(@"linux/src/l.c")) >= 0);
        GCHECK(RowOf(c, file(@"src/core.cpp")) < 0 && RowOf(c, file(@"README.md")) < 0);
        GCHECK_EQ_STR(c.rootPath, repo);
        // The label row: one label per folder, each with its ×.
        MCTreeFocusBar *bar = [c treeFocusBar];
        GCHECK(bar && !bar.hidden && bar.showsFolders);
        GCHECK_EQ_STR(Labels(c), @"mac/src › linux/src");
        GCHECK(!NSIsEmptyRect([bar rectOfPart:1]) && !NSIsEmptyRect([bar removeRectOfPart:1]));
        GCHECK(NSMaxX([bar removeRectOfPart:1]) <= NSMinX([bar closeRect]));
        GCHECK_EQ_STR(StoredFocus(repo), @"mac/src, linux/src");
        MCGuiSnapshot(c.sidebarScroll, @"tree_focus-roots");
        // A root's right click: Remove from Focus, no Add to Focus.
        Select(c, file(@"linux/src"));
        GCHECK(!TreeMenuItem(c, @selector(removeFolderFromFocus:)).hidden);
        GCHECK(TreeMenuItem(c, @selector(addFolderToFocus:)).hidden);
        Select(c, file(@"linux/src/sub"));
        GCHECK(TreeMenuItem(c, @selector(removeFolderFromFocus:)).hidden);
        MCGuiWriteFile(file(@"linux/src/new.c"), @"// n\n");
    });
    MCGuiWait(@"a file made in a root shows", 10, ^BOOL {
        return RowOf(c, file(@"linux/src/new.c")) >= 0;
    });
    MCGuiStep(@"New File's folder and Copy Path go by the row, else the first root", ^{
        [c.outline deselectAll:nil];
        GCHECK_EQ_STR([c targetDirectory], file(@"mac/src"));
        Select(c, file(@"linux/src/l.c"));
        GCHECK_EQ_STR([c targetDirectory], file(@"linux/src"));
    });
    MCGuiStep(@"a folder inside a root changes nothing; one holding a root takes its place", ^{
        GCHECK([c addTreeFocus:@"linux/src/sub"]);
        GCHECK_EQ_STR(c.treeFocus, @"mac/src, linux/src");
        GCHECK([c addTreeFocus:@"mac"]);
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src");
        GCHECK_EQ_STR(TopLabels(c), @"mac linux/src");
        // mac/src shows inside mac, open, and no file shows twice.
        GCHECK(Expanded(c, file(@"mac")) && Expanded(c, file(@"mac/src")));
        GCHECK(RowsOf(c, file(@"mac/src/a.mm")) == 1);
        GCHECK([c addTreeFocus:@"mac/checks"]);
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src");
    });
    MCGuiStep(@"a reveal outside every root adds its folder; an open does not", ^{
        [c revealPath:file(@"src/core.cpp") andOpen:NO];
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src, src");
        GCHECK_EQ_STR(Selected(c), file(@"src/core.cpp"));
        [c revealPath:file(@"docs/d.md") open:YES from:EditorTabs::OpenFrom::QuickOpen];
        GCHECK_EQ_STR(c.currentPath, file(@"docs/d.md"));
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src, src");
        // Inside a root: selected, nothing added.
        [c revealPath:file(@"linux/src/sub/s.c") andOpen:NO];
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src, src");
        GCHECK_EQ_STR(Selected(c), file(@"linux/src/sub/s.c"));
    });
    MCGuiStep(@"a label's × takes its folder out; a click on a label selects its root", ^{
        GCHECK_EQ_STR(Labels(c), @"mac › linux/src › src");
        ClickBar(c, [[c treeFocusBar] removeRectOfPart:2]);
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src");
        GCHECK_EQ_STR(TopLabels(c), @"mac linux/src");
        ClickBar(c, [[c treeFocusBar] rectOfPart:1]);
        GCHECK_EQ_STR(Selected(c), file(@"linux/src"));
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src");
    });
    MCGuiStep(@"Remove from Focus on a root: down to one, the breadcrumb is back", ^{
        Select(c, file(@"linux/src"));
        [c removeFolderFromFocus:nil];
        GCHECK_EQ_STR(c.treeFocus, @"mac");
        GCHECK(![c treeFocusBar].showsFolders && ![c treeFocusBar].hidden);
        GCHECK_EQ_STR(Labels(c), @"focusrepo › mac");
        GCHECK_EQ_STR(TopRows(c), @"checks src");
        GCHECK(Expanded(c, file(@"mac/src")));
    });
    MCGuiStep(@"Focus on Folder on a root replaces the set", ^{
        GCHECK([c addTreeFocus:@"linux/src"]);
        GCHECK_EQ_STR(c.treeFocus, @"mac, linux/src");
        Select(c, file(@"linux/src"));
        [c focusOnFolder:nil];
        GCHECK_EQ_STR(c.treeFocus, @"linux/src");
        GCHECK_EQ_STR(TopRows(c), @"sub l.c new.c");
    });
    MCGuiStep(@"the last × shows the whole folder; so does Show Whole Folder in Tree", ^{
        GCHECK(([c setTreeFocusFolders:@[@"mac/src", @"linux/src"]]));
        ClickBar(c, [[c treeFocusBar] closeRect]);
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK([c treeFocusBar].hidden);
        GCHECK_EQ_STR(TopRows(c), @"docs linux mac src README.md");
        // The roots show open where they are.
        GCHECK(Expanded(c, file(@"mac/src")) && Expanded(c, file(@"linux/src")));
        GCHECK(([c setTreeFocusFolders:@[@"mac/src", @"linux/src"]]));
        [c showWholeFolderInTree:nil];
        GCHECK_EQ_STR(c.treeFocus, @"");
        GCHECK((![c setTreeFocusFolders:@[@"mac/src", @"nope"]]));   // refused, as it was
        GCHECK_EQ_STR(c.treeFocus, @"");
    });
    __block NSString *rootsRecord = nil;
    MCGuiStep(@"brought back with its window: the set, each root open or closed as it was", ^{
        GCHECK(([c setTreeFocusFolders:@[@"mac/src", @"linux/src"]]));
        [c.outline collapseItem:ItemAt(c, file(@"linux/src"))];
        const TabSessions::Window w = [c windowRecordNow];
        GCHECK((w.tabs.treeFocus == std::vector<std::string>{"mac/src", "linux/src"}));
        rootsRecord = @(TabSessions::encodeWindows({w}).c_str());
        EditorController *back = [(NSObject *)NSApp.delegate openWindowAtPath:repo restoring:rootsRecord];
        GCHECK_EQ_STR(back.treeFocus, @"mac/src, linux/src");
        GCHECK_EQ_STR(TopLabels(back), @"mac/src linux/src");
        GCHECK(Expanded(back, file(@"mac/src")) && !Expanded(back, file(@"linux/src")));
        GCHECK([back treeFocusBar].showsFolders);
        CloseChecked(back);
        MCGuiSetKeyWindow(c.window);
    });
    MCGuiStep(@"a record with folders gone since keeps the others", ^{
        TabSessions::Window w = TabSessions::decodeWindows(rootsRecord.UTF8String).at(0);
        w.tabs.treeFocus = {"mac/src", "gone", "linux/src"};
        EditorController *back = [(NSObject *)NSApp.delegate
            openWindowAtPath:repo restoring:@(TabSessions::encodeWindows({w}).c_str())];
        GCHECK_EQ_STR(back.treeFocus, @"mac/src, linux/src");
        CloseChecked(back);
        w.tabs.treeFocus = {"gone", "linux/src"};
        back = [(NSObject *)NSApp.delegate
            openWindowAtPath:repo restoring:@(TabSessions::encodeWindows({w}).c_str())];
        GCHECK_EQ_STR(back.treeFocus, @"linux/src");
        GCHECK(![back treeFocusBar].showsFolders);
        GCHECK_EQ_STR(Labels(back), @"focusrepo › linux › src");
        CloseChecked(back);
        MCGuiSetKeyWindow(c.window);
    });
    MCGuiStep(@"a record in the old single-path form still restores", ^{
        TabSessions::Window w = TabSessions::decodeWindows(rootsRecord.UTF8String).at(0);
        w.tabs.treeFocus = {"mac/src"};
        std::string text = TabSessions::encodeWindows({w});
        const size_t at = text.find("[\"mac/src\"]");
        GCHECK(at != std::string::npos);
        if (at == std::string::npos) return;
        text.replace(at, std::string("[\"mac/src\"]").size(), "\"mac/src\"");
        EditorController *back = [(NSObject *)NSApp.delegate openWindowAtPath:repo restoring:@(text.c_str())];
        GCHECK_EQ_STR(back.treeFocus, @"mac/src");
        GCHECK_EQ_STR(Labels(back), @"focusrepo › mac › src");
        CloseChecked(back);
        MCGuiSetKeyWindow(c.window);
    });
    MCGuiStep(@"the folder's own session brings the set back", ^{
        GCHECK_EQ_STR(c.treeFocus, @"mac/src, linux/src");
        CloseChecked(c);
        MCGuiSetKeyWindow(nil);
        GCHECK_EQ_STR(StoredFocus(repo), @"mac/src, linux/src");
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        c = MCGuiController();
        GCHECK_EQ_STR(c.treeFocus, @"mac/src, linux/src");
        GCHECK_EQ_STR(TopLabels(c), @"mac/src linux/src");
        GCHECK(Expanded(c, file(@"mac/src")));
    });
    MCGuiStep(@"three roots; docs deleted on disk", ^{
        GCHECK([c addTreeFocus:@"docs"]);
        GCHECK_EQ_STR(c.treeFocus, @"mac/src, linux/src, docs");
        [[NSFileManager defaultManager] removeItemAtPath:file(@"docs") error:nil];
    });
    MCGuiWait(@"the other two stay", 10, ^BOOL {
        return [c.treeFocus isEqualToString:@"mac/src, linux/src"];
    });
    MCGuiStep(@"their rows and label row; the session keeps the two", ^{
        GCHECK_EQ_STR(TopLabels(c), @"mac/src linux/src");
        GCHECK_EQ_STR(Labels(c), @"mac/src › linux/src");
        GCHECK_EQ_STR(StoredFocus(repo), @"mac/src, linux/src");
    });

    MCGuiStep(@"close the window", ^{
        [c.window performClose:nil];
        MCGuiSetKeyWindow(nil);
    });
}
