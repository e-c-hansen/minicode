// EditorPage.h — a view of its own in the editor's slot, in a tab, the way
// a diff or a commit is shown: read-only, nothing to save, gone when its tab
// closes. The Evidence view and a worktree's changes (AgentPages.h) are
// pages. EditorController's showPage: puts one in a tab, or selects the tab
// already showing a page with the same key.
#import <Cocoa/Cocoa.h>

@protocol MCEditorPage <NSObject>
@property(nonatomic, readonly, copy) NSString *pageTitle;   // the tab's and the window's title
@property(nonatomic, readonly, copy) NSString *pageKind;    // for `minicode state`: "evidence", ...
@property(nonatomic, readonly, copy) NSString *pageKey;     // the same key shows in the same tab
@property(nonatomic, readonly) NSView *pageView;            // laid out in the editor's slot
@property(nonatomic, readonly) NSView *pageFocusView;       // what Cmd+1 focuses
@optional
- (void)pageApplySettings;   // the editor's colors changed
- (void)pageClosed;          // its tab closed: stop reading and watching
@end
