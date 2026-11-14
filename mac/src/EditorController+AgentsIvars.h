// EditorController+AgentsIvars.h — the instance variables only
// EditorController+Agents.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    MCChangeSession *_change;              // Request a Change (Shift+Cmd+K), made on first use
}
@end
