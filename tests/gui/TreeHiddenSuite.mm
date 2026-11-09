// TreeHiddenSuite.mm — `make guitest SUITES=tree_hidden`: View > Show
// Hidden Files is one switch for every window. Two windows on two
// folders, each with dotfiles at the top and in an open subfolder: the
// switch flipped in one relists both at once (round 5's workspace 2: the
// other window kept its rows, and its next changed folder alone showed
// dotfiles), the menu item has a checkmark while it is on, the choice is
// in the app's defaults and read back at launch, and a change on disk
// afterwards keeps the other window consistent.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "TreeDetails.h"

namespace {

NSInteger RowOf(EditorController *c, NSString *path) {
    for (NSInteger r = 0; r < c.outline.numberOfRows; r++)
        if ([((FileItem *)[c.outline itemAtRow:r]).path isEqualToString:path]) return r;
    return -1;
}

FileItem *ItemOf(EditorController *c, NSString *path) {
    const NSInteger r = RowOf(c, path);
    return r >= 0 ? [c.outline itemAtRow:r] : nil;
}

NSControlStateValue CheckOf(id<NSMenuItemValidation> target) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"Show Hidden Files"
                                                  action:@selector(toggleHiddenFiles:)
                                           keyEquivalent:@""];
    item.state = NSControlStateValueMixed;
    [target validateMenuItem:item];
    return item.state;
}

}  // namespace

MC_GUI_SUITE(tree_hidden) {
    __block EditorController *a = nil, *b = nil;
    NSString *one = [MCGuiScratch() stringByAppendingPathComponent:@"hidden-one"];
    NSString *two = [MCGuiScratch() stringByAppendingPathComponent:@"hidden-two"];
    NSString *(^in)(NSString *, NSString *) = ^NSString *(NSString *dir, NSString *rel) {
        return [dir stringByAppendingPathComponent:rel];
    };

    MCGuiStep(@"two folders with dotfiles, a window on each, subfolders open", ^{
        for (NSString *d in @[one, two]) {
            MCGuiWriteFile(in(d, @"a.txt"), @"a\n");
            MCGuiWriteFile(in(d, @".env"), @"X=1\n");
            MCGuiWriteFile(in(d, @"sub/b.txt"), @"b\n");
            MCGuiWriteFile(in(d, @"sub/.secret"), @"s\n");
        }
        GCHECK(!gShowHidden);
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:one]]];
        a = MCGuiController();
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:two]]];
        b = MCGuiController();
        GCHECK(MCGuiControllers().count == before + 2 && a != b);
        for (EditorController *c in @[a, b]) {
            NSString *root = c == a ? one : two;
            FileItem *sub = ItemOf(c, in(root, @"sub"));
            GCHECK(sub != nil);
            [c.outline expandItem:sub];
            GCHECK(RowOf(c, in(root, @"sub/b.txt")) >= 0);
            GCHECK(RowOf(c, in(root, @".env")) < 0 && RowOf(c, in(root, @"sub/.secret")) < 0);
        }
        GCHECK(CheckOf(a) == NSControlStateValueOff);
    });

    MCGuiStep(@"on in the first window: both show them, top and below, at once", ^{
        [a toggleHiddenFiles:nil];
        for (EditorController *c in @[a, b]) {
            NSString *root = c == a ? one : two;
            MCGuiNote(@"%@: .env row %ld, sub/.secret row %ld", root.lastPathComponent,
                      (long)RowOf(c, in(root, @".env")), (long)RowOf(c, in(root, @"sub/.secret")));
            GCHECK(RowOf(c, in(root, @".env")) >= 0);
            GCHECK(RowOf(c, in(root, @"sub/.secret")) >= 0);
            GCHECK([c.outline isItemExpanded:ItemOf(c, in(root, @"sub"))]);
        }
        // A checkmark, from the window and from the app.
        GCHECK(CheckOf(a) == NSControlStateValueOn && CheckOf(b) == NSControlStateValueOn);
        GCHECK(CheckOf((id<NSMenuItemValidation>)NSApp.delegate) == NSControlStateValueOn);
        // Remembered: the next launch reads it back.
        GCHECK([MCAppDefaults() boolForKey:@"MCShowHiddenFiles"]);
        gShowHidden = NO;
        [EditorController loadShowHiddenFiles];
        GCHECK(gShowHidden);
    });

    MCGuiStep(@"off from the second window: gone from both", ^{
        [b toggleHiddenFiles:nil];
        for (EditorController *c in @[a, b]) {
            NSString *root = c == a ? one : two;
            GCHECK(RowOf(c, in(root, @".env")) < 0 && RowOf(c, in(root, @"sub/.secret")) < 0);
            GCHECK(RowOf(c, in(root, @"sub/b.txt")) >= 0);
        }
        GCHECK(CheckOf(a) == NSControlStateValueOff);
        GCHECK(![MCAppDefaults() boolForKey:@"MCShowHiddenFiles"]);
        gShowHidden = YES;
        [EditorController loadShowHiddenFiles];
        GCHECK(!gShowHidden);
        // A change on disk in the other window's subfolder: listed by the
        // same rule as the rest of it.
        MCGuiWriteFile(in(two, @"sub/c.txt"), @"c\n");
    });
    MCGuiWait(@"the new file listed", 15, ^BOOL { return RowOf(b, in(two, @"sub/c.txt")) >= 0; });
    MCGuiStep(@"still no dotfiles there", ^{
        GCHECK(RowOf(b, in(two, @"sub/.secret")) < 0 && RowOf(b, in(two, @".env")) < 0);
    });

    MCGuiStep(@"the windows close", ^{
        [a.window performClose:nil];
        [b.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL {
        return ![MCGuiControllers() containsObject:a] && ![MCGuiControllers() containsObject:b];
    });
}
