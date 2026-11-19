// EditorController+PersistenceIvars.h — the instance variables only
// EditorController+Persistence.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"
#include "SavePlan.h"

@interface EditorController () {
    // When unsaved tabs are written (backups, auto-save), and the one timer
    // that ticks it (armSavePlan).
    SavePlan::Planner _plan;
    NSTimer *_planTimer;
    // The window as the last run left it (TabSessions::Window, encoded),
    // from restoreWindowRecord: until showWindow has put it all back.
    NSString *_windowRecord;
    NSRect _frameBeforeFullScreen;         // the frame to keep while in full screen
    // Quitting kept the unsaved tabs' backups for the next launch
    // (keepUnsavedTabsForNextLaunch), so terminating leaves them.
    BOOL _unsavedKeptForNextLaunch;
}
@end
