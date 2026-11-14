// EditorController+GitIvars.h — the instance variables only
// EditorController+Git.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@class MCGitGutter;

@interface EditorController () {
    // The editor's change bars (GitGutter.h), made with the panel's object.
    MCGitGutter *_gitGutter;
}
@end
