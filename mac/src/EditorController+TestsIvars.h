// EditorController+TestsIvars.h — the instance variables only
// EditorController+Tests.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@class MCTestGutter;

@interface EditorController () {
    // The ▶ beside each test (TestGutter.h), made with the window.
    MCTestGutter *_testGutter;
}
@end
