// Lsp+NavigationIvars.h — the instance variables only Lsp+Navigation.mm
// uses, which imports it, as does Lsp.mm, which lays them out. See the rule
// at the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@interface LspSession () {
    NSRange _definitionSelection;    // the selection when F12 was asked
    int _definitionGen, _hoverGen;
    MCHoverPopup *_hover;
    BOOL _hoverByMouse;       // shown for the pointer (else for Cmd+I)
    NSRange _hoverRange;      // the text it is about
    NSUInteger _pointerIndex; // the character under the pointer, or NSNotFound
    // `lastHoverText` (Lsp.h) is backed by this one.
    NSString *_lastHoverText;

    // Go to Symbol: answers for an older request are dropped.
    int _symbolGen, _workspaceGen;
    // [server, request id, answer block] still out; a server that exits
    // gets its answer block called with nothing, so the search finishes.
    NSMutableArray<NSArray *> *_workspaceRequests;
}
@end
