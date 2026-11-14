// EditorController+EditCommandsIvars.h — the instance variables only
// EditorController+EditCommands.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    // Shift+Return in Python: the terminal running the REPL it started, and
    // the code waiting for that REPL to come up.
    __weak TerminalView *_pythonRepl;
    NSMutableArray<NSString *> *_replPending;
    BOOL _replWaiting, _replPolling;
    // A note that the folder's own .venv waits for a yes before terminals
    // activate it, said once per window.
    BOOL _venvNoteShown;
}
@end
