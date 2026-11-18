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

// The focus the folder's own session keeps.
NSString *StoredFocus(NSString *folder) {
    const TabSessions::Store store = MCSessionsRead();
    auto found = store.folders.find(folder.stringByResolvingSymlinksInPath.UTF8String);
    return found == store.folders.end() ? @"(no session)" : @(found->second.treeFocus.c_str());
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
        GCHECK(w.tabs.treeFocus == "mac/src");
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
        w.tabs.treeFocus = "mac/gone";
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
    MCGuiStep(@"close the window", ^{
        [c.window performClose:nil];
        MCGuiSetKeyWindow(nil);
    });
}
