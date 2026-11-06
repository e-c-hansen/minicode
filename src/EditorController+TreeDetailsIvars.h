// EditorController+TreeDetailsIvars.h — the instance variables only
// EditorController+TreeDetails.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    // The tree's sizes, dates and order (see TreeDetails.h). Each folder's
    // order on show is worked out again when _sortGeneration moves on.
    TreeSort::Options _treeSort;
    NSUInteger _sortGeneration;
    NSString *_realRoot;                   // the root as FSEvents spells it
    NSTimer *_detailsTimer;                // ages and "Saved ..." each half minute
    MCTreeColumns _shownColumns;           // the tree's columns, as last laid out
}
@end
