// EditorController+TreeIvars.h — the instance variables only
// EditorController+Tree.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#import "TreeGitColors.h"

@interface EditorController () {
    FSEventStreamRef _fsStream;            // watches the open folder for changes
    // Folders FSEvents named (the tree's spelling), with whether everything
    // below them is to be looked at too, waiting for the listing under way.
    NSMutableDictionary<NSString *, NSNumber *> *_treePending;
    BOOL _treeListing;                     // folders being listed off the main thread
    NSUInteger _treeGeneration;            // moves on with every full rescan and new root
    dispatch_queue_t _treeListQueue;
    MCTreeGitColors *_gitColors;           // the rows' git colors
    // The tree's rows being dragged, while a drag from the tree is on.
    NSArray<NSString *> *_draggedPaths;
}
@end
