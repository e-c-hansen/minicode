// EditorController+TabsIvars.h — the instance variables only
// EditorController+Tabs.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#import "LayoutCatchUp.h"

@interface EditorController () {
    uint64_t _tabStamp;                    // counts activations, for MCTab.lastActive
    NSUndoManager *_noTabUndo;             // the text view's while no tab is open
    NSMutableArray<NSDictionary *> *_closedTabs;   // Reopen Closed Tab's stack, newest last
    // A tab shown again far down its text: the lines above its place laid
    // out in the background (restoreTextPlaceOf:).
    MCLayoutCatchUp *_layoutCatchUp;
}
@end
