// EditorController+EditorViewIvars.h — the instance variables only
// EditorController+EditorView.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    BOOL _bracketMatchQueued;   // updateBracketMatch waits for the next turn
}
@end
