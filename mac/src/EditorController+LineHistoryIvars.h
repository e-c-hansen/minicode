// EditorController+LineHistoryIvars.h — the instance variables only
// EditorController+LineHistory.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@class MCLineHistory;

@interface EditorController () {
    // Show Line History (EditorLineHistory.h), made with the window.
    MCLineHistory *_lineHistory;
}
@end
