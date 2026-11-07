// EditorController+LargeFileIvars.h — the instance variables only
// EditorController+LargeFile.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    // "Large file: features off" in the status bar, made the first time a
    // large file shows; a click turns the features on for that tab.
    MCLspStatusField *_largeFileNote;
}
@end
