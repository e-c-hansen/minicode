// EditorController+Git.mm — EditorController: the Source Control panel's
// object (made with the window, so autofetch runs before the panel is first
// shown), and the commands that act on it whether it shows or not: pushing
// and pulling (git.fetch, git.pull, git.push, git.sync), Stage All and
// Unstage All, Discard Changes and Undo Discard, and the branch picker
// (git.checkout), which is the palette with a list of branches; Commit and
// Amend, Undo Last Commit, Stash and Pop Stash; and the editor's change
// bars (GitGutter.h): their object, made with the panel's, and Next Change,
// Previous Change, Revert Change and Stage Change.
#import "EditorController+Private.h"
#import "EditorController+GitIvars.h"
#import "ChangeReviewPage.h"
#import "CompareView.h"
#import "EditorConflicts.h"
#import "GitGutter.h"
#import "GitPanel.h"
#import "PalettePanel.h"
#include "MergeConflicts.h"

@implementation EditorController (Git)

- (MCGitPanel *)ensureGitPanel {
    if (self.gitPanel) return self.gitPanel;
    self.gitPanel = [[MCGitPanel alloc] initWithRoot:_root.path];
    __weak EditorController *weakSelf = self;
    if (!_gitGutter && self.textView && self.gutter) {
        _gitGutter = [[MCGitGutter alloc] initWithHost:self];
        _gitGutter.onNote = ^(NSString *text) { [weakSelf showStatusNote:text]; };
        [[NSNotificationCenter defaultCenter] addObserver:_gitGutter
                                                 selector:@selector(refreshSoon)
                                                     name:MCGitPanelDidRefreshNotification
                                                   object:self.gitPanel];
    }
    // The editor's merge conflicts (EditorConflicts.h): only in a file shown
    // as source text, as the change bars; a choice says what is left.
    MCEditorConflicts *conflicts = self.textView.conflicts;
    conflicts.wanted = ^BOOL { return [weakSelf gitGutterPath] != nil; };
    conflicts.onResolved = ^(NSUInteger left) {
        EditorController *s = weakSelf;
        NSString *name = s.currentPath.lastPathComponent ?: @"the file";
        [s showStatusNote:[NSString stringWithUTF8String:MergeConflicts::resolvedNote(name.UTF8String, left).c_str()]];
    };
    self.gitPanel.onShowDiff = ^(NSString *name, NSString *path, NSData *diff) {
        [weakSelf showDiffNamed:name path:path data:diff];
    };
    self.gitPanel.onReview = ^(NSString *path, BOOL staged, BOOL preview) {
        [weakSelf showChangeReviewAt:path staged:staged preview:preview];
    };
    self.gitPanel.onShowCommit = ^(NSString *title, NSData *show) {
        [weakSelf showCommitTitled:title data:show];
    };
    self.gitPanel.onReviewCommit = ^(NSString *hash, NSString *title, BOOL preview) {
        [weakSelf showCommitReview:hash title:title preview:preview];
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

// The review of every uncommitted change (ChangeReviewPage.h), one tab per
// window, at a file (a click on its row in the panel) or from its top: in
// the preview tab for a single click, kept for Return or a double-click.
- (void)showChangeReviewAt:(NSString *)rel staged:(BOOL)staged {
    [self showChangeReviewAt:rel staged:staged preview:NO];
}
- (void)showChangeReviewAt:(NSString *)rel staged:(BOOL)staged preview:(BOOL)preview {
    MCGitPanel *panel = [self ensureGitPanel];
    NSString *key = [@"review:" stringByAppendingString:_root.path ?: @""];
    MCChangeReviewPage *page = nil;
    for (MCTab *t in [self tabs])
        if ([t.page isKindOfClass:MCChangeReviewPage.class] && [t.page.pageKey isEqualToString:key])
            page = (MCChangeReviewPage *)t.page;
    if (!page) {
        page = [[MCChangeReviewPage alloc] initWithRoot:_root.path panel:panel];
        __weak EditorController *weakSelf = self;
        page.onOpenFile = ^(NSString *path, NSInteger line) {
            EditorController *s = weakSelf;
            NSString *mine = [s gitPathInRoot:path];
            [s openFileAtPath:mine];
            if (line > 0 && [s.currentPath isEqualToString:mine]) [s goToLine:(int)line column:0];
        };
        page.onShowComparison = ^(MCComparison *c) { [weakSelf showComparison:c preview:NO]; };
        page.hasUnsavedText = ^BOOL(NSString *path) {
            EditorController *s = weakSelf;
            NSString *mine = [s gitPathInRoot:path];
            for (MCTab *t in [s tabs])
                if (t.dirty && ([t.currentPath isEqualToString:mine] || [t.currentPath isEqualToString:path]))
                    return YES;
            return NO;
        };
    }
    [self showPage:page preview:preview];
    [page revealFile:rel staged:staged];
}

// A commit from the graph as a readable change, each commit a page of its
// own (`commit:<root>:<hash>`): a single click shows it in the preview tab
// (a commit page there shows the new commit in its place, as the next
// single click on a file replaces it), Return or a double-click keeps it.
- (void)showCommitReview:(NSString *)hash title:(NSString *)title {
    [self showCommitReview:hash title:title preview:YES];
}
- (void)showCommitReview:(NSString *)hash title:(NSString *)title preview:(BOOL)preview {
    MCGitPanel *panel = [self ensureGitPanel];
    MCChangeReviewPage *page = nil;
    MCTab *tab = nil;
    NSString *key = [MCChangeReviewPage commitKeyForRoot:_root.path hash:hash];
    for (MCTab *t in [self tabs])
        if ([t.page isKindOfClass:MCChangeReviewPage.class] && [t.page.pageKey isEqualToString:key]) {
            page = (MCChangeReviewPage *)t.page;
            tab = t;
        }
    // Another commit in the preview tab: this one takes its place.
    if (!page && preview)
        for (MCTab *t in [self groupTabs])
            if (t.preview && [t.page isKindOfClass:MCChangeReviewPage.class] &&
                [((MCChangeReviewPage *)t.page).pageKind isEqualToString:@"commit"]) {
                page = (MCChangeReviewPage *)t.page;
                tab = t;
            }
    if (!page) {
        page = [[MCChangeReviewPage alloc] initForCommitsWithRoot:_root.path panel:panel];
        __weak EditorController *weakSelf = self;
        page.onOpenFile = ^(NSString *path, NSInteger line) {
            EditorController *s = weakSelf;
            NSString *mine = [s gitPathInRoot:path];
            [s openFileAtPath:mine];
            if (line > 0 && [s.currentPath isEqualToString:mine]) [s goToLine:(int)line column:0];
        };
        page.onShowComparison = ^(MCComparison *c) { [weakSelf showComparison:c preview:NO]; };
    }
    [page showCommit:hash title:title];
    if (tab) {
        tab.diffName = page.pageTitle;
        [self reloadTabStrip];
    }
    [self showPage:page preview:preview];
    [self updateTitle];
}

- (void)gitReviewChanges:(id)sender {
    (void)sender;
    [self showChangeReviewAt:nil staged:NO];
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

// ------------------------------------------------- the change bars' host
- (CodeTextView *)gitGutterTextView { return self.textView; }
- (MCLineGutter *)gitGutterView { return self.gutter; }
- (NSString *)gitGutterPath {
    if (!self.currentPath || !self.textView.editable || self.previewMode || self.isDiff ||
        [self largeFileFeaturesOff])
        return nil;
    return self.currentPath;
}
- (BOOL)gitGutterHasBom { return _activeTab.bom; }
- (NSInteger)gitGutterEncoding { return _activeTab.encoding; }
- (void)gitGutterChangedIndex { [self.gitPanel refresh]; }
- (MCGitGutter *)gitGutter { return _gitGutter; }   // tests, by KVC

// Option+F5 and Shift+Option+F5 (VS Code's workbench.action.editor.
// nextChange and previousChange): the caret to the next or previous change
// bar, wrapping; in a file with merge conflicts (which has no bars), to
// the next or previous conflict.
- (void)nextChange:(id)sender {
    (void)sender;
    if (self.textView.conflicts.count > 0) [self.textView.conflicts goToNext:YES];
    else [_gitGutter goToNextChange:YES];
}
- (void)previousChange:(id)sender {
    (void)sender;
    if (self.textView.conflicts.count > 0) [self.textView.conflicts goToNext:NO];
    else [_gitGutter goToNextChange:NO];
}
// The change on the caret's line (VS Code's revertChange and stageChange,
// which its change widget offers).
- (void)revertChange:(id)sender { (void)sender; [_gitGutter revertChange:[_gitGutter barAtCaret]]; }
- (void)stageChange:(id)sender { (void)sender; [_gitGutter stageChange:[_gitGutter barAtCaret]]; }

- (void)gitCommitAmend:(id)sender { (void)sender; [[self ensureGitPanel] commitAmend]; }
- (void)gitUndoCommit:(id)sender { (void)sender; [[self ensureGitPanel] undoLastCommit]; }
- (void)gitStash:(id)sender { (void)sender; [[self ensureGitPanel] stash]; }
- (void)gitStashPop:(id)sender { (void)sender; [[self ensureGitPanel] popStash]; }

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
    if (item.action == @selector(nextChange:) || item.action == @selector(previousChange:)) {
        *enabled = (_gitGutter.barCount > 0 || self.textView.conflicts.count > 0) &&
                   self.gitGutterPath != nil;
        return YES;
    }
    if (item.action == @selector(revertChange:) || item.action == @selector(stageChange:)) {
        *enabled = self.gitGutterPath != nil && [_gitGutter barAtCaret] >= 0;
        return YES;
    }
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
