// EditorController+TreeFocusIvars.h — the instance variables only
// EditorController+TreeFocus.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#import "TreeFocusBar.h"

@interface EditorController () {
    // The folders the tree shows (nodes of _root's tree, so the tree's
    // listings keep them up to date), in the order they were added, and
    // their paths from the window's folder: nil for the whole folder; one,
    // the tree rooted there under its breadcrumb; several, each a root row
    // under the label row.
    NSArray<FileItem *> *_treeFocusItems;
    NSArray<NSString *> *_treeFocusPaths;
    // Folders open outside what the tree shows, kept so they open again
    // when the focus widens.
    NSMutableSet<NSString *> *_treeFocusExpanded;
    MCTreeFocusBar *_treeFocusBar;         // the breadcrumb or label row, made when first focused
}
@end
