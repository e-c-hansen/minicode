// TreeSuite.mm — `make guitest SUITES=tree`: the file tree in a window of
// its own on a scratch repository. A file made or removed on disk shows or
// goes with only its folder listed again (MCTreeListedFolders counts the
// listings), a collapsed folder and one under a collapsed folder catch up
// when opened, expansion, selection and folder totals survive; rows take
// their git status's color (a modified file and its folder, an untracked
// one, an ignored folder) and lose it once committed; drag and drop moves a
// row into a folder and refuses a folder into itself or below itself, and
// copies a file from Finder in; Sort By size and date order the rows; the
// size column shows each file's size; hidden files come and go; a file's
// icon is its type's (FileIcon::Type: Python, JSON, a Dockerfile, a
// picture, a README), the one image made for that type, in its family's
// color, a folder's stays a folder; the names use the sidebar's width
// after a new file's refresh, with the name column left out of step first;
// 5,000 rows of many types are made and drawn page by page in a bounded
// time, each with its type's one image.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "TreeDetails.h"
#import "FileTypeIcons.h"
#include <sys/time.h>

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

// The names of the rows directly in `folder` (nil: the root), in order on show.
NSArray<NSString *> *RowNames(EditorController *c, FileItem *folder) {
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    const NSInteger n = [c.outline.dataSource outlineView:c.outline numberOfChildrenOfItem:folder];
    for (NSInteger i = 0; i < n; i++)
        [names addObject:[(FileItem *)[c.outline.dataSource outlineView:c.outline child:i ofItem:folder] name]];
    return names;
}

MCTreeCell *CellAt(EditorController *c, NSString *path) {
    const NSInteger row = RowOf(c, path);
    if (row < 0) return nil;
    id v = [c.outline viewAtColumn:0 row:row makeIfNecessary:YES];
    return [v isKindOfClass:MCTreeCell.class] ? v : nil;
}

BOOL SameColor(NSColor *a, NSColor *b) {
    NSColor *x = [a colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    NSColor *y = [b colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (!x || !y) return NO;
    return fabs(x.redComponent - y.redComponent) < 0.01 &&
           fabs(x.greenComponent - y.greenComponent) < 0.01 &&
           fabs(x.blueComponent - y.blueComponent) < 0.01 &&
           fabs(x.alphaComponent - y.alphaComponent) < 0.01;
}

NSColor *GitColor(TreeGitColor k) { return MCColor([[AppSettings shared] settings].treeGitColor(k)); }

BOOL RowColored(EditorController *c, NSString *path, NSColor *color) {
    MCTreeCell *cell = CellAt(c, path);
    return cell && SameColor(cell.textField.textColor, color);
}

// The listings of folders inside `repo` since `from`, a value of
// MCTreeListingCount (which only grows). MCTreeListedFolders keeps only the
// last 500, so an index into it stopped moving once earlier suites had
// filled it, and a full run saw nothing listed.
NSArray<NSString *> *ListedSince(NSUInteger from, NSString *repo) {
    NSArray<NSString *> *all = MCTreeListedFolders();
    const NSUInteger fresh = MCTreeListingCount() - from;
    NSMutableArray<NSString *> *mine = [NSMutableArray array];
    for (NSUInteger i = all.count > fresh ? all.count - fresh : 0; i < all.count; i++)
        if ([all[i] isEqualToString:repo] || [all[i] hasPrefix:[repo stringByAppendingString:@"/"]])
            [mine addObject:all[i]];
    return mine;
}

// How far right the name of the row for `path` reaches, in the sidebar's
// visible width: the cell the outline gives it and the name field inside.
// NO when the row is not there.
BOOL NameReach(EditorController *c, NSString *path, CGFloat *cellRight, CGFloat *nameRight) {
    const NSInteger row = RowOf(c, path);
    if (row < 0) return NO;
    [c.outline layoutSubtreeIfNeeded];
    *cellRight = NSMaxX([c.outline frameOfCellAtColumn:0 row:row]);
    MCTreeCell *cell = CellAt(c, path);
    *nameRight = cell ? NSMaxX([c.outline convertRect:cell.textField.frame fromView:cell]) : 0;
    return YES;
}

void Touch(NSString *path, time_t when) {
    struct timeval tv[2] = {{when, 0}, {when, 0}};
    utimes(path.fileSystemRepresentation, tv);
}

}  // namespace

// What a drag brings: the tree's own rows, or file URLs from Finder. Only
// what the tree's drop methods ask is answered.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
@interface MCTreeSuiteDrag : NSObject <NSDraggingInfo>
@property(nonatomic, strong) id source;
@property(nonatomic, strong) NSPasteboard *board;
@end
@implementation MCTreeSuiteDrag
- (id)draggingSource { return self.source; }
- (NSPasteboard *)draggingPasteboard { return self.board; }
- (NSDragOperation)draggingSourceOperationMask { return NSDragOperationEvery; }
- (NSWindow *)draggingDestinationWindow { return nil; }
- (NSPoint)draggingLocation { return NSZeroPoint; }
- (NSInteger)draggingSequenceNumber { return 1; }
@end
#pragma clang diagnostic pop

MC_GUI_SUITE(tree) {
    __block EditorController *c = nil;
    __block NSString *git = nil;
    __block NSUInteger mark = 0;
    NSString *repo = [MCGuiScratch() stringByAppendingPathComponent:@"treerepo"];
    NSString *src = [repo stringByAppendingPathComponent:@"src"];
    NSString *docs = [repo stringByAppendingPathComponent:@"docs"];
    NSString *deep = [src stringByAppendingPathComponent:@"deep"];
    auto file = ^NSString *(NSString *rel) { return [repo stringByAppendingPathComponent:rel]; };

    MCGuiStep(@"a repository: src (a, b, deep/d), docs, three files", ^{
        git = MCGitExecutable();
        if (!git) MCGuiNote(@"no git here: the git color checks are skipped");
        MCGuiWriteFile(file(@"src/a.txt"), @"alpha\n");
        MCGuiWriteFile(file(@"src/b.txt"), @"bravo\n");
        MCGuiWriteFile(file(@"src/deep/d.txt"), @"delta\n");
        MCGuiWriteFile(file(@"docs/README.md"), @"# docs\n");
        // One file of each of a few kinds, for the icons (the tree reads
        // only their names).
        for (NSString *name in @[@"shot.png", @"clip.mp4", @"table.csv", @"app.sqlite",
                                 @"main.cpp", @"page.html", @"tool.py", @"package.json",
                                 @"Dockerfile", @"run", @"notes"])
            MCGuiWriteFile(file([@"docs" stringByAppendingPathComponent:name]), @"x");
        // A file with no extension that can run is an executable.
        [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions: @0755}
                                         ofItemAtPath:file(@"docs/run") error:nil];
        MCGuiWriteFile(file(@".gitignore"), @"build/\n");
        MCGuiWriteFile(file(@"small.txt"), @"s\n");
        MCGuiWriteFile(file(@"medium.txt"), [@"" stringByPaddingToLength:3000 withString:@"m" startingAtIndex:0]);
        MCGuiWriteFile(file(@"large.txt"), [@"" stringByPaddingToLength:90000 withString:@"l" startingAtIndex:0]);
        Touch(file(@"small.txt"), 1600000000);
        Touch(file(@"large.txt"), 1700000000);
        Touch(file(@"medium.txt"), 1650000000);
    });
    MCGuiAsync(@"git init, add, commit", 30, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        NSString *sh = [NSString stringWithFormat:
            @"cd '%@' && '%@' init -q -b main && '%@' add -A && '%@' commit -qm first", repo, git, git, git];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int s, NSString *, NSString *err) {
            GCHECK(s == 0);
            if (s) MCGuiNote(@"git: %@", err);
            done();
        });
    });
    MCGuiStep(@"a.txt changed; a window on the repository; src, deep and docs open", ^{
        MCGuiWriteFile(file(@"src/a.txt"), @"ALPHA\n");
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:repo]]];
        GCHECK(MCGuiControllers().count == before + 1);
        c = MCGuiController();
        [c.outline expandItem:ItemAt(c, src)];
        [c.outline expandItem:ItemAt(c, deep)];
        [c.outline expandItem:ItemAt(c, docs)];
        GCHECK(RowOf(c, file(@"src/deep/d.txt")) >= 0 && RowOf(c, file(@"docs/README.md")) >= 0);
        GCHECK([RowNames(c, nil) isEqualToArray:(@[@"docs", @"src", @"large.txt", @"medium.txt",
                                                   @"small.txt"])]);
    });

    // --- the git colors
    MCGuiWait(@"the modified file and its folder in the modified color", 15, ^BOOL {
        return !git || (RowColored(c, file(@"src/a.txt"), GitColor(TreeGitColor::Modified)) &&
                        RowColored(c, src, GitColor(TreeGitColor::Modified)));
    });
    MCGuiStep(@"clean rows keep the tree's text color; the tooltip names the status", ^{
        if (!git) return;
        NSColor *text = [[AppSettings shared] text:Surface::Sidebar];
        GCHECK(RowColored(c, file(@"src/b.txt"), text));
        GCHECK(RowColored(c, docs, text) && RowColored(c, file(@"small.txt"), text));
        GCHECK([CellAt(c, file(@"src/a.txt")).toolTip containsString:@"Modified (git)"]);
        GCHECK([c treeGitRuns] >= 1);
        // An untracked file, and an ignored folder with output in it.
        MCGuiWriteFile(file(@"notes.txt"), @"n\n");
        MCGuiWriteFile(file(@"build/out.o"), @"o\n");
    });
    MCGuiWait(@"the untracked file and the ignored folder colored", 15, ^BOOL {
        return !git || (RowColored(c, file(@"notes.txt"), GitColor(TreeGitColor::Untracked)) &&
                        RowColored(c, file(@"build"), GitColor(TreeGitColor::Ignored)));
    });
    MCGuiAsync(@"commit everything", 30, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        NSString *sh = [NSString stringWithFormat:@"cd '%@' && '%@' add -A && '%@' commit -qm second",
                        repo, git, git];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int s, NSString *, NSString *) {
            GCHECK(s == 0);
            done();
        });
    });
    MCGuiWait(@"committed rows lose their color; the ignored folder keeps its own", 15, ^BOOL {
        if (!git) return YES;
        NSColor *text = [[AppSettings shared] text:Surface::Sidebar];
        return RowColored(c, file(@"src/a.txt"), text) && RowColored(c, src, text) &&
               RowColored(c, file(@"notes.txt"), text) &&
               RowColored(c, file(@"build"), GitColor(TreeGitColor::Ignored));
    });
    MCGuiWaitQuiet(@"the commit's last status runs out first", 1.0, 20, ^NSInteger {
        return (NSInteger)[c treeGitRuns];
    });
    MCGuiStep(@"each file's icon is its type's one image, in its family's color", ^{
        NSDictionary<NSString *, NSString *> *want = @{
            @"docs/tool.py": @"python", @"docs/package.json": @"json",
            @"docs/Dockerfile": @"dockerfile", @"docs/shot.png": @"image",
            @"docs/README.md": @"readme", @"docs/clip.mp4": @"video",
            @"docs/table.csv": @"csv", @"docs/app.sqlite": @"database",
            @"docs/main.cpp": @"cpp", @"docs/page.html": @"html",
            @"docs/run": @"executable", @"docs/notes": @"text",
        };
        const BOOL dark = MCFileTypeTextIsLight([[AppSettings shared] text:Surface::Sidebar]);
        for (NSString *rel in want) {
            MCTreeCell *cell = CellAt(c, file(rel));
            GCHECK(cell != nil);
            NSImage *image = cell.imageView.image;
            GCHECK_EQ_STR(image.accessibilityDescription, want[rel]);
            FileItem *item = ItemAt(c, file(rel));
            const FileIcon::Type type = MCFileTypeOf(file(rel), item.executable);
            GCHECK(image == MCFileTypeImage(type));
            const BOOL tinted = SameColor(cell.imageView.contentTintColor, MCFileTypeTint(type, dark));
            if (!tinted) MCGuiNote(@"%@ tinted %@", rel, cell.imageView.contentTintColor);
            GCHECK(tinted);
        }
        // Every type has its image, named for it and made once; a light
        // tree takes deeper colors than a dark one.
        NSMutableSet<NSString *> *names = [NSMutableSet set];
        for (FileIcon::Type t : FileIcon::allTypes()) {
            NSImage *image = MCFileTypeImage(t);
            GCHECK(image != nil && image == MCFileTypeImage(t));
            GCHECK_EQ_STR(image.accessibilityDescription, @(FileIcon::typeName(t)));
            [names addObject:image.accessibilityDescription ?: @""];
        }
        GCHECK(names.count == FileIcon::allTypes().size());
        GCHECK(!SameColor(MCFileTypeTint(FileIcon::Type::Python, YES),
                          MCFileTypeTint(FileIcon::Type::Python, NO)));
        GCHECK(!SameColor(MCFileTypeTint(FileIcon::Type::Python, dark),
                          MCFileTypeTint(FileIcon::Type::JavaScript, dark)));
        GCHECK(CellAt(c, docs).imageView.image == MCTreeSymbolImage(@"folder.fill"));
        // A symbol this macOS lacks falls back to the page.
        GCHECK(MCTreeSymbolImage(@"no.such.symbol.here") == MCTreeSymbolImage(@"doc.text"));
    });
    MCGuiStep(@"a change only inside the ignored folder runs no status", ^{
        if (!git) return;
        mark = [c treeGitRuns];
        MCGuiWriteFile(file(@"build/more.o"), @"o\n");
    });
    MCGuiPause(1.5);   // before a negative check: nothing is to happen
    MCGuiStep(@"no status ran for it", ^{
        if (!git) return;
        GCHECK([c treeGitRuns] == mark);
    });

    // --- only the folder that changed is listed
    MCGuiStep(@"select b.txt; make src/c.txt on disk", ^{
        [c.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)RowOf(c, file(@"src/b.txt"))]
               byExtendingSelection:NO];
        mark = MCTreeListingCount();
        MCGuiWriteFile(file(@"src/c.txt"), @"charlie\n");
    });
    MCGuiWait(@"c.txt shows", 10, ^BOOL { return RowOf(c, file(@"src/c.txt")) >= 0; });
    MCGuiStep(@"only src was listed; expansion and selection stayed", ^{
        NSArray<NSString *> *listed = ListedSince(mark, repo);
        MCGuiNote(@"listed: %@", [listed componentsJoinedByString:@", "]);
        GCHECK(listed.count >= 1);
        GCHECK([[NSSet setWithArray:listed] isEqualToSet:[NSSet setWithObject:src]]);
        GCHECK([c.outline isItemExpanded:ItemAt(c, src)] && [c.outline isItemExpanded:ItemAt(c, deep)]);
        GCHECK([c.outline isItemExpanded:ItemAt(c, docs)]);
        FileItem *sel = c.outline.selectedRow >= 0 ? [c.outline itemAtRow:c.outline.selectedRow] : nil;
        GCHECK_EQ_STR(sel.path, file(@"src/b.txt"));
        mark = MCTreeListingCount();
        [[NSFileManager defaultManager] removeItemAtPath:file(@"src/c.txt") error:nil];
    });
    MCGuiWait(@"c.txt goes", 10, ^BOOL { return RowOf(c, file(@"src/c.txt")) < 0; });
    MCGuiStep(@"again only src", ^{
        NSArray<NSString *> *listed = ListedSince(mark, repo);
        GCHECK([[NSSet setWithArray:listed] isEqualToSet:[NSSet setWithObject:src]]);
        mark = MCTreeListingCount();
        // A file at the top: the root is listed, and src stays open.
        MCGuiWriteFile(file(@"top.txt"), @"t\n");
    });
    MCGuiWait(@"top.txt shows", 10, ^BOOL { return RowOf(c, file(@"top.txt")) >= 0; });
    MCGuiStep(@"only the root was listed; src and deep still open, b.txt still selected", ^{
        NSArray<NSString *> *listed = ListedSince(mark, repo);
        GCHECK([[NSSet setWithArray:listed] isEqualToSet:[NSSet setWithObject:repo]]);
        GCHECK([c.outline isItemExpanded:ItemAt(c, src)] && [c.outline isItemExpanded:ItemAt(c, deep)]);
        FileItem *sel = c.outline.selectedRow >= 0 ? [c.outline itemAtRow:c.outline.selectedRow] : nil;
        GCHECK_EQ_STR(sel.path, file(@"src/b.txt"));
        // A file deep in a folder never opened lists nothing at all.
        mark = MCTreeListingCount();
        MCGuiWriteFile(file(@"build/obj/x/y.o"), @"y\n");
    });
    MCGuiPause(1.0);   // before a negative check: nothing is to be listed
    MCGuiStep(@"nothing listed for it", ^{
        NSArray<NSString *> *listed = ListedSince(mark, repo);
        MCGuiNote(@"listed: %@", [listed componentsJoinedByString:@", "]);
        GCHECK(listed.count == 0);
        // A collapsed folder (docs) and one under a collapsed folder
        // (deep, under src) change while closed.
        [c.outline collapseItem:ItemAt(c, docs)];
        [c.outline collapseItem:ItemAt(c, src)];
        mark = MCTreeListingCount();
        MCGuiWriteFile(file(@"docs/guide.md"), @"g\n");
        MCGuiWriteFile(file(@"src/deep/e.txt"), @"e\n");
    });
    // The events must arrive while the folders are closed: waited for by
    // the listings they cause (both folders were loaded, so both are
    // listed again, closed as they are).
    MCGuiWait(@"docs and deep listed again while closed", 10, ^BOOL {
        NSArray<NSString *> *listed = ListedSince(mark, repo);
        return [listed containsObject:docs] && [listed containsObject:deep];
    });
    MCGuiStep(@"opened again, they show what changed while closed", ^{
        GCHECK(![c.outline isItemExpanded:ItemAt(c, docs)] && ![c.outline isItemExpanded:ItemAt(c, src)]);
        [c.outline expandItem:ItemAt(c, docs)];
        [c.outline expandItem:ItemAt(c, src)];
        GCHECK(RowOf(c, file(@"docs/guide.md")) >= 0);
        GCHECK([c.outline isItemExpanded:ItemAt(c, deep)]);
        GCHECK(RowOf(c, file(@"src/deep/e.txt")) >= 0);
    });

    // --- folder totals keep up
    MCGuiStep(@"sizes on", ^{ MCSetTreeColumnsShown(MCTreeColumnSize); });
    MCGuiWait(@"src's total and each file's size", 10, ^BOOL {
        MCTreeCell *s = CellAt(c, src), *l = CellAt(c, file(@"large.txt"));
        return s && l && ![s.sizeField.stringValue isEqualToString:@"…"] && s.sizeField.stringValue.length &&
               [l.sizeField.stringValue isEqualToString:MCFormatByteCount(90000)];
    });
    __block NSString *srcTotal = nil;
    MCGuiStep(@"a big file in src", ^{
        srcTotal = CellAt(c, src).sizeField.stringValue;
        MCGuiNote(@"src was %@", srcTotal);
        MCGuiWriteFile(file(@"src/big.txt"), [@"" stringByPaddingToLength:500000 withString:@"b" startingAtIndex:0]);
    });
    MCGuiWait(@"src's total grows", 10, ^BOOL {
        NSString *now = CellAt(c, src).sizeField.stringValue;
        return now.length && ![now isEqualToString:srcTotal] && ![now isEqualToString:@"…"] &&
               RowOf(c, file(@"src/big.txt")) >= 0;
    });

    // --- sorting
    MCGuiStep(@"Sort By size: largest first, folders first", ^{
        MCGuiNote(@"src now %@", CellAt(c, src).sizeField.stringValue);
        [c sortTreeBy:[[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""]];   // Name
        NSMenuItem *size = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        size.tag = 2;
        [c sortTreeBy:size];
        GCHECK([c treeSortOptions].key == TreeSort::Key::Size && ![c treeSortOptions].ascending);
    });
    MCGuiWait(@"the rows by size", 10, ^BOOL {
        NSArray<NSString *> *names = RowNames(c, nil);
        NSArray<NSString *> *files = [names filteredArrayUsingPredicate:
            [NSPredicate predicateWithFormat:@"SELF IN %@", @[@"large.txt", @"medium.txt", @"small.txt"]]];
        // src (over 500 KB) before docs among the folders.
        return [files isEqualToArray:(@[@"large.txt", @"medium.txt", @"small.txt"])] &&
               [names indexOfObject:@"src"] < [names indexOfObject:@"docs"];
    });
    MCGuiStep(@"Sort By date modified: newest first; then back to name", ^{
        NSMenuItem *date = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        date.tag = 1;
        [c sortTreeBy:date];
        NSArray<NSString *> *names = RowNames(c, nil);
        NSArray<NSString *> *files = [names filteredArrayUsingPredicate:
            [NSPredicate predicateWithFormat:@"SELF IN %@", @[@"large.txt", @"medium.txt", @"small.txt"]]];
        GCHECK([files isEqualToArray:(@[@"large.txt", @"medium.txt", @"small.txt"])]);
        // A file saved now moves to the top of the files on disk's word.
        MCGuiWriteFile(file(@"small.txt"), @"newer\n");
    });
    MCGuiWait(@"small.txt first among the files", 10, ^BOOL {
        NSArray<NSString *> *names = RowNames(c, nil);
        NSArray<NSString *> *files = [names filteredArrayUsingPredicate:
            [NSPredicate predicateWithFormat:@"SELF IN %@", @[@"large.txt", @"medium.txt", @"small.txt"]]];
        return [files isEqualToArray:(@[@"small.txt", @"large.txt", @"medium.txt"])];
    });
    MCGuiStep(@"back to name; sizes off", ^{
        [c sortTreeBy:[[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""]];
        NSMenuItem *asc = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        asc.tag = 1;
        [c setTreeSortDirection:asc];
        GCHECK([c treeSortOptions] == TreeSort::Options());
        MCSetTreeColumnsShown(0);
        NSArray<NSString *> *names = RowNames(c, nil);
        GCHECK([names indexOfObject:@"docs"] < [names indexOfObject:@"src"]);
    });

    // --- drag and drop
    MCGuiStep(@"a row dragged onto a folder moves there; a folder into itself or below is refused", ^{
        [c showFile:file(@"top.txt")];
        FileItem *top = ItemAt(c, file(@"top.txt")), *docsItem = ItemAt(c, docs);
        FileItem *srcItem = ItemAt(c, src), *deepItem = ItemAt(c, deep);
        GCHECK(top && docsItem && srcItem && deepItem);
        MCTreeSuiteDrag *drag = [MCTreeSuiteDrag new];
        drag.source = c.outline;
        // src onto itself, and onto deep inside it: refused.
        [c outlineView:c.outline draggingSession:(NSDraggingSession *)nil willBeginAtPoint:NSZeroPoint
              forItems:@[srcItem]];
        GCHECK([c outlineView:c.outline validateDrop:drag proposedItem:srcItem proposedChildIndex:-1] ==
               NSDragOperationNone);
        GCHECK([c outlineView:c.outline validateDrop:drag proposedItem:deepItem proposedChildIndex:-1] ==
               NSDragOperationNone);
        // ...and a file inside deep stands for deep.
        GCHECK([c outlineView:c.outline validateDrop:drag
                    proposedItem:ItemAt(c, file(@"src/deep/d.txt")) proposedChildIndex:-1] ==
               NSDragOperationNone);
        [c outlineView:c.outline draggingSession:(NSDraggingSession *)nil endedAtPoint:NSZeroPoint
             operation:NSDragOperationNone];
        // top.txt onto docs: a move; onto the root where it is: refused.
        [c outlineView:c.outline draggingSession:(NSDraggingSession *)nil willBeginAtPoint:NSZeroPoint
              forItems:@[top]];
        GCHECK([c outlineView:c.outline validateDrop:drag proposedItem:nil proposedChildIndex:0] ==
               NSDragOperationNone);
        GCHECK([c outlineView:c.outline validateDrop:drag proposedItem:docsItem proposedChildIndex:-1] ==
               NSDragOperationMove);
        GCHECK([c outlineView:c.outline acceptDrop:drag item:docsItem childIndex:-1]);
        [c outlineView:c.outline draggingSession:(NSDraggingSession *)nil endedAtPoint:NSZeroPoint
             operation:NSDragOperationMove];
        NSFileManager *fm = NSFileManager.defaultManager;
        GCHECK(![fm fileExistsAtPath:file(@"top.txt")] && [fm fileExistsAtPath:file(@"docs/top.txt")]);
        GCHECK_EQ_STR(c.currentPath, file(@"docs/top.txt"));   // its tab followed
        GCHECK(RowOf(c, file(@"docs/top.txt")) >= 0 && RowOf(c, file(@"top.txt")) < 0);
    });
    MCGuiStep(@"a file from Finder dropped on a folder is copied in", ^{
        NSString *outside = [MCGuiScratch() stringByAppendingPathComponent:@"from-finder.txt"];
        MCGuiWriteFile(outside, @"f\n");
        MCTreeSuiteDrag *drag = [MCTreeSuiteDrag new];
        drag.board = [NSPasteboard pasteboardWithUniqueName];
        [drag.board clearContents];
        [drag.board writeObjects:@[[NSURL fileURLWithPath:outside]]];
        FileItem *srcItem = ItemAt(c, src);
        GCHECK([c outlineView:c.outline validateDrop:drag proposedItem:srcItem proposedChildIndex:-1] ==
               NSDragOperationCopy);
        GCHECK([c outlineView:c.outline acceptDrop:drag item:srcItem childIndex:-1]);
        NSFileManager *fm = NSFileManager.defaultManager;
        GCHECK([fm fileExistsAtPath:outside] && [fm fileExistsAtPath:file(@"src/from-finder.txt")]);
        GCHECK(RowOf(c, file(@"src/from-finder.txt")) >= 0);
        [drag.board releaseGlobally];
    });

    // --- hidden files
    MCGuiStep(@"hidden files on: .gitignore shows; off: it goes", ^{
        GCHECK(RowOf(c, file(@".gitignore")) < 0);
        [c toggleHiddenFiles:nil];
        GCHECK(RowOf(c, file(@".gitignore")) >= 0);
        GCHECK([c.outline isItemExpanded:ItemAt(c, src)]);
        [c toggleHiddenFiles:nil];
        GCHECK(RowOf(c, file(@".gitignore")) < 0);
    });

    // --- a burst of changes costs a few listings of the one folder
    MCGuiWaitQuiet(@"the drops' own changes arrive first", 0.7, 20, ^NSInteger {
        return (NSInteger)MCTreeListingCount();
    });
    MCGuiStep(@"200 files written in src at once", ^{
        mark = MCTreeListingCount();
        for (int i = 0; i < 200; i++)
            MCGuiWriteFile(file([NSString stringWithFormat:@"src/burst/f%03d.txt", i]), @"x\n");
    });
    MCGuiWait(@"burst shows", 10, ^BOOL { return RowOf(c, file(@"src/burst")) >= 0; });
    MCGuiWaitQuiet(@"the last batches arrive before they are counted", 0.7, 20, ^NSInteger {
        return (NSInteger)MCTreeListingCount();
    });
    MCGuiStep(@"src (and the root's row) only, a few times", ^{
        NSArray<NSString *> *listed = ListedSince(mark, repo);
        MCGuiNote(@"burst listings: %@", [listed componentsJoinedByString:@", "]);
        GCHECK([[NSSet setWithArray:listed] isSubsetOfSet:[NSSet setWithObject:src]]);
        GCHECK(listed.count <= 6);
    });

    // --- names use the sidebar's width after a refresh (C3, 2026-11-16: a
    // file added to the folder cut every name off at about 90 points in a
    // 250-point sidebar). The name column had lost its width (to its
    // 80-point minimum) while the outline kept the sidebar's, and only
    // followed changes in that; the reload the new file caused gave every
    // row a cell of the column's width.
    __block CGFloat visible = 0;
    MCGuiStep(@"the name column fills the sidebar; then it is left at its minimum", ^{
        visible = NSWidth(c.sidebarScroll.contentView.bounds);
        CGFloat cellRight = 0, nameRight = 0;
        GCHECK(NameReach(c, file(@"small.txt"), &cellRight, &nameRight));
        MCGuiNote(@"sidebar %.0f, column %.0f, cell to %.0f, name to %.0f", visible,
                  c.outline.outlineTableColumn.width, cellRight, nameRight);
        GCHECK(visible > 150);
        GCHECK(cellRight >= visible - 40);
        GCHECK(nameRight >= visible - 45);
        c.outline.outlineTableColumn.width = c.outline.outlineTableColumn.minWidth;
    });
    MCGuiStep(@"a new file at the top", ^{
        MCGuiWriteFile(file(@"old-latin1.log"), @"x\n");
    });
    MCGuiWait(@"old-latin1.log shows", 10, ^BOOL { return RowOf(c, file(@"old-latin1.log")) >= 0; });
    MCGuiStep(@"every top row's name reaches the sidebar's right edge again", ^{
        GCHECK(fabs(NSWidth(c.sidebarScroll.contentView.bounds) - visible) < 1);
        MCGuiNote(@"column %.0f", c.outline.outlineTableColumn.width);
        for (NSString *name in @[@"small.txt", @"medium.txt", @"old-latin1.log"]) {
            CGFloat cellRight = 0, nameRight = 0;
            GCHECK(NameReach(c, file(name), &cellRight, &nameRight));
            if (cellRight < visible - 40 || nameRight < visible - 45)
                MCGuiNote(@"%@: cell to %.0f, name to %.0f of %.0f", name, cellRight, nameRight, visible);
            GCHECK(cellRight >= visible - 40);
            GCHECK(nameRight >= visible - 45);
        }
    });
    MCGuiStep(@"a narrower sidebar: the column follows it", ^{
        CGFloat side = NSWidth(c.splitView.subviews.firstObject.frame);
        [c.splitView setPosition:side - 60 ofDividerAtIndex:0];
        [c.splitView layoutSubtreeIfNeeded];
        const CGFloat now = NSWidth(c.sidebarScroll.contentView.bounds);
        CGFloat cellRight = 0, nameRight = 0;
        GCHECK(NameReach(c, file(@"small.txt"), &cellRight, &nameRight));
        MCGuiNote(@"sidebar %.0f, cell to %.0f", now, cellRight);
        GCHECK(cellRight <= now + 1 && cellRight >= now - 40);
        [c.splitView setPosition:side ofDividerAtIndex:0];
    });

    // --- 5,000 rows of many types, made and drawn page by page: each row
    // takes its type's one image (none made per row), in a bounded time.
    NSString *many = [repo stringByAppendingPathComponent:@"many"];
    MCGuiStep(@"5,000 files of twenty types in many/", ^{
        NSFileManager *fm = [NSFileManager defaultManager];
        [fm createDirectoryAtPath:many withIntermediateDirectories:YES attributes:nil error:nil];
        NSArray<NSString *> *exts = @[@"py", @"js", @"ts", @"json", @"md", @"png", @"mp4", @"cpp",
                                      @"h", @"rs", @"go", @"sh", @"yml", @"toml", @"csv", @"pdf",
                                      @"zip", @"txt", @"lock", @"swift"];
        for (int i = 0; i < 5000; i++) {
            NSString *name = [NSString stringWithFormat:@"f%04d.%@", i, exts[(NSUInteger)i % exts.count]];
            [fm createFileAtPath:[many stringByAppendingPathComponent:name] contents:nil attributes:nil];
        }
    });
    MCGuiWait(@"many/ shows", 15, ^BOOL { return RowOf(c, many) >= 0; });
    MCGuiStep(@"open many/, and draw every page of the tree", ^{
        [c.outline expandItem:ItemAt(c, many)];
        const NSInteger rows = c.outline.numberOfRows;
        MCGuiNote(@"%ld rows", (long)rows);
        GCHECK(rows >= 5000);
        NSClipView *clip = c.sidebarScroll.contentView;
        const CGFloat page = MAX(NSHeight(clip.bounds), 100);
        const CGFloat total = NSHeight(c.outline.frame);
        NSBitmapImageRep *rep = [c.outline bitmapImageRepForCachingDisplayInRect:clip.documentVisibleRect];
        __block NSUInteger checked = 0, wrong = 0, pages = 0;
        // Makes the rows on show (each cell configured) and draws them.
        auto drawPage = ^(CGFloat y) {
            [c.outline scrollPoint:NSMakePoint(0, y)];
            const NSRect visible = clip.documentVisibleRect;
            const NSRange range = [c.outline rowsInRect:visible];
            for (NSUInteger r = range.location; r < NSMaxRange(range); r++)
                [c.outline rowViewAtRow:(NSInteger)r makeIfNecessary:YES];
            [c.outline cacheDisplayInRect:visible toBitmapImageRep:rep];
            pages++;
        };
        drawPage(0);   // warm: the first page also makes the reused cells
        double cpu = 0;
        for (CGFloat y = 0; y < total; y += page) {
            const double c0 = MCGuiThreadCpuMs();
            drawPage(y);
            cpu += MCGuiThreadCpuMs() - c0;
            // Not timed: every row on show has its type's one image.
            const NSRange range = [c.outline rowsInRect:clip.documentVisibleRect];
            for (NSUInteger r = range.location; r < NSMaxRange(range); r++) {
                FileItem *item = [c.outline itemAtRow:(NSInteger)r];
                MCTreeCell *cell = [c.outline viewAtColumn:0 row:(NSInteger)r makeIfNecessary:NO];
                if (!item || item.isDir || ![cell isKindOfClass:MCTreeCell.class]) continue;
                checked++;
                if (cell.imageView.image != MCFileTypeImage(MCFileTypeOf(item.path, item.executable)))
                    wrong++;
            }
        }
        MCGuiNote(@"%lu pages of %.0f points: %.0f ms of CPU; %lu rows checked, %lu wrong",
                  (unsigned long)pages, page, cpu, (unsigned long)checked, (unsigned long)wrong);
        GCHECK(checked >= 5000);
        GCHECK(wrong == 0);
        GCHECK_TIME(cpu, 3000, @"making and drawing every page of a 5,000-row tree");
        [c.outline collapseItem:ItemAt(c, many)];
        [c.outline scrollPoint:NSZeroPoint];
    });

    MCGuiStep(@"close the window", ^{
        [c.window performClose:nil];
        MCGuiSetKeyWindow(nil);
    });
}
