// EditorController+TreeIvars.h — the instance variables only
// EditorController+Tree.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    FSEventStreamRef _fsStream;            // watches the open folder for changes
    // The tree's rows being dragged, while a drag from the tree is on.
    NSArray<NSString *> *_draggedPaths;
}
@end
