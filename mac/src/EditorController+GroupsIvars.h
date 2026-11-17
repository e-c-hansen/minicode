// EditorController+GroupsIvars.h — the instance variables only
// EditorController+Groups.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#import "EditorMirror.h"

@interface EditorController () {
    // Two editor groups side by side (Split Editor), whose tabs are the
    // window's Model::Window (_win). The editor is always in the focused
    // group; the other shows its tab in _mirror, under _parkedStrip.
    MCTabStrip *_parkedStrip;              // the other group's strip
    MCEditorMirror *_mirror;
    DragBar *_groupDivider;
    NSRect _groupSlot;                     // both groups, as last laid out
    NSRect _liveSlot;                      // the focused group's, below its strip
    // What the mirror shows: the tab, and the text that tab had when the
    // mirror took its copy (a newer one means it changed: copied again).
    __weak MCTab *_mirrorTab;
    NSString *_mirrorText;
    // The selection the other group had for its tab when it last had the
    // keyboard, so a file in both groups keeps one selection per group.
    __weak MCTab *_parkedSelectionTab;
    NSRange _parkedSelection;
    BOOL _mirrorSyncQueued;
}
@end
