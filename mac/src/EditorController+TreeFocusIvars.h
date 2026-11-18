// EditorController+TreeFocusIvars.h — the instance variables only
// EditorController+TreeFocus.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#import "TreeFocusBar.h"

@interface EditorController () {
    // The folder the tree is rooted at (a node of _root's tree, so the
    // tree's listings keep it up to date), nil for the whole folder, and
    // its path from the window's folder.
    FileItem *_treeFocusItem;
    NSString *_treeFocusPath;
    // Folders open outside what the tree shows, kept so they open again
    // when the focus widens.
    NSMutableSet<NSString *> *_treeFocusExpanded;
    MCTreeFocusBar *_treeFocusBar;         // the breadcrumb row, made when first focused
}
@end
