// EditorController+GroupsIvars.h — the instance variables only
// EditorController+Groups.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#import "EditorMirror.h"

@interface EditorController () {
    // Two editor groups side by side (Split Editor). The editor is always in
    // the focused one (0 is the left); the other shows its tab in _mirror.
    // While split, _shownTabs and _parkedTabs are the two groups' tabs in
    // strip order and _tabs every document of either; unsplit both are nil
    // and _tabs is the one group.
    BOOL _split;
    NSInteger _focusedGroup;
    NSMutableArray<MCTab *> *_shownTabs;
    NSMutableArray<MCTab *> *_parkedTabs;
    MCTab *_parkedActive;                  // the tab the other group shows
    MCTabStrip *_parkedStrip;              // the other group's strip
    MCEditorMirror *_mirror;
    DragBar *_groupDivider;
    CGFloat _groupFraction;                // the left group's share of the width
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
