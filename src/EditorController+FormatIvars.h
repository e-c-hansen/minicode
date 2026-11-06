// EditorController+FormatIvars.h — the instance variables only
// EditorController+Format.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    NSUInteger _formatGeneration;          // a format result for an older one is dropped
    NSPopover *_formatNote;                // what Format Document has to say
}
@end
