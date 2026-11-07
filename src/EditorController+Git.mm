// EditorController+Git.mm — EditorController: the Source Control panel's
// object (made with the window, so autofetch runs before the panel is first
// shown), and the commands that act on it whether it shows or not: pushing
// and pulling (git.fetch, git.pull, git.push, git.sync), Stage All and
// Unstage All, Discard Changes and Undo Discard, and the branch picker
// (git.checkout), which is the palette with a list of branches.
#import "EditorController+Private.h"
#import "GitPanel.h"
#import "PalettePanel.h"

@implementation EditorController (Git)

- (MCGitPanel *)ensureGitPanel {
    if (self.gitPanel) return self.gitPanel;
    self.gitPanel = [[MCGitPanel alloc] initWithRoot:_root.path];
    __weak EditorController *weakSelf = self;
    self.gitPanel.onShowDiff = ^(NSString *name, NSString *path, NSData *diff) {
        [weakSelf showDiffNamed:name path:path data:diff];
    };
    self.gitPanel.onShowCommit = ^(NSString *title, NSData *show) {
        [weakSelf showCommitTitled:title data:show];
    };
    // Autofetch only while this window has the keyboard, as VS Code's waits
    // for its window's focus.
    self.gitPanel.isActive = ^BOOL {
        EditorController *s = weakSelf;
        return s && !s->_closing && s.window.isKeyWindow;
    };
    self.gitPanel.hostWindow = self.window;
    self.gitPanel.onNote = ^(NSString *text) { [weakSelf showStatusNote:text]; };
    self.gitPanel.onOpenFile = ^(NSString *path) {
        EditorController *s = weakSelf;
        [s openFileAtPath:[s gitPathInRoot:path]];
    };
    self.gitPanel.onRevealFile = ^(NSString *path) { [weakSelf revealGitPath:path]; };
    self.gitPanel.onChooseBranch = ^{ [weakSelf gitCheckout:nil]; };
    return self.gitPanel;
}

// git names files from its top level with links resolved (/private/tmp);
// the tree and the tabs spell them from the window's folder (/tmp).
- (NSString *)gitPathInRoot:(NSString *)path {
    NSString *rel = [self pathFromRoot:path];
    if (!rel) return path;
    return [rel isEqualToString:@"."] ? _root.path : [_root.path stringByAppendingPathComponent:rel];
}

// A file row's Reveal in File Tree: the tree comes back in the panel's
// place with the file selected, as a tab's Reveal in File Tree does.
- (void)revealGitPath:(NSString *)path {
    if (![self pathFromRoot:path]) { NSBeep(); return; }
    if (self.gitPanelVisible) [self toggleSourceControl:nil];
    if (self.sidebarCollapsed) [self toggleSidebarVisibility:nil];
    [self revealPath:[self gitPathInRoot:path] andOpen:NO];
    [self.window makeFirstResponder:self.outline];
}

- (void)gitFetch:(id)sender { (void)sender; [[self ensureGitPanel] fetch]; }
- (void)gitPull:(id)sender { (void)sender; [[self ensureGitPanel] pull]; }
- (void)gitPush:(id)sender { (void)sender; [[self ensureGitPanel] push]; }
- (void)gitSync:(id)sender { (void)sender; [[self ensureGitPanel] sync]; }
- (void)gitStageAll:(id)sender { (void)sender; [[self ensureGitPanel] stageAll]; }
- (void)gitUnstageAll:(id)sender { (void)sender; [[self ensureGitPanel] unstageAll]; }
- (void)gitUndoDiscard:(id)sender { (void)sender; [[self ensureGitPanel] undoDiscard]; }

// Discard Changes: the row selected in the panel while its list has the
// keyboard, else the file on show (VS Code's git.clean from the palette
// acts on the active editor's file).
- (void)gitClean:(id)sender {
    (void)sender;
    MCGitPanel *panel = [self ensureGitPanel];
    const NSInteger row = [self gitPanelListHasKeyboard] ? [panel selectedFileRow] : -1;
    if (row >= 0) [panel discardAtRow:row];
    else if (self.currentPath) [panel discardPath:self.currentPath];
    else NSBeep();
}

- (BOOL)gitPanelListHasKeyboard {
    return self.gitPanelVisible && self.window.firstResponder == self.gitPanel.list;
}

// The branch picker: the palette over this window, listing the branches
// (the current one first, then by their last commit, then remote ones);
// typing filters them, and a name that is no branch gets a last row that
// makes it.
- (void)gitCheckout:(id)sender {
    (void)sender;
    __weak EditorController *weakSelf = self;
    MCGitPanel *panel = [self ensureGitPanel];
    [panel listBranches:^(MCGitBranches *branches) {
        EditorController *s = weakSelf;
        if (!s || !branches || !s.window) return;
        __weak MCGitPanel *weakPanel = panel;
        MCPalettePick *pick = [MCPalettePick new];
        pick.placeholder = @"Select a branch to switch to, or type a name for a new one";
        pick.labels = branches.labels;
        pick.details = branches.details;
        pick.emptyText = @"No branch matches";
        pick.choose = ^(NSInteger i) { [weakPanel switchToBranchAtIndex:i of:branches]; };
        pick.typedRow = ^NSString *(NSString *text) { return [branches createLabelFor:text]; };
        pick.chooseTyped = ^(NSString *text) { [weakPanel createBranchNamed:text of:branches]; };
        MCPaletteContext *ctx = [MCPaletteContext new];
        ctx.pick = pick;
        MCShowPalette(s.window, @"", ctx);
    }];
}

// The menu bar's items: Undo Discard names the file and needs one to undo;
// Discard Changes needs a file.
- (BOOL)validateGitItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    if (item.action == @selector(gitUndoDiscard:)) {
        NSString *title = self.gitPanel.undoDiscardTitle;
        item.title = title ?: @"Undo Discard";
        *enabled = title != nil;
        return YES;
    }
    if (item.action == @selector(gitClean:)) {
        *enabled = self.currentPath != nil ||
                   ([self gitPanelListHasKeyboard] && [self.gitPanel selectedFileRow] >= 0);
        return YES;
    }
    return NO;
}

@end
