// EditorController+Git.mm — EditorController: the Source Control panel's
// object (made with the window, so autofetch runs before the panel is first
// shown), and the commands that push and pull (git.fetch, git.pull,
// git.push, git.sync), which act on it whether it shows or not.
#import "EditorController+Private.h"
#import "GitPanel.h"

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
    return self.gitPanel;
}

- (void)gitFetch:(id)sender { (void)sender; [[self ensureGitPanel] fetch]; }
- (void)gitPull:(id)sender { (void)sender; [[self ensureGitPanel] pull]; }
- (void)gitPush:(id)sender { (void)sender; [[self ensureGitPanel] push]; }
- (void)gitSync:(id)sender { (void)sender; [[self ensureGitPanel] sync]; }

@end
