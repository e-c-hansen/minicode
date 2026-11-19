// EditorController+TreeRevealIvars.h — the instance variables only
// EditorController+TreeReveal.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

#include "FileDetails.h"

@interface EditorController () {
    // The active file as the tree follows it (explorer.auto-reveal): a
    // change while the tree is hidden waits for it to show.
    TreeReveal::Follower _treeFollower;
    // The file a click (or Return) in the tree is opening: its row is the
    // one clicked, so the tree selects it and does not scroll.
    NSString *_treeOpeningPath;
    // Above zero while a window's tabs come back (restoreSession): the
    // tree is put back as the record has it, not moved to the file.
    NSInteger _treeFollowHeld;
    // explorer.auto-reveal as applySettings last saw it (once it has), so
    // turning it on shows the file on show at once.
    BOOL _treeAutoRevealOn, _treeAutoRevealSeen;
}
@end
