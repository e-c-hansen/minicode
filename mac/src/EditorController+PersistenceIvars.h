// EditorController+PersistenceIvars.h — the instance variables only
// EditorController+Persistence.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    NSDate *_backupFirstPending;           // the oldest buffer change not yet backed up
    // The window as the last run left it (TabSessions::Window, encoded),
    // from restoreWindowRecord: until showWindow has put it all back.
    NSString *_windowRecord;
    NSRect _frameBeforeFullScreen;         // the frame to keep while in full screen
}
@end
