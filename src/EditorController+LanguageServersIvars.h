// EditorController+LanguageServersIvars.h — the instance variables only
// EditorController+LanguageServers.mm uses, which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@interface EditorController () {
    // The bar over the editor when the open file's language server is
    // missing: the language it was offered for (nil once closed), shown
    // while a file of that language is on show and the server still missing.
    NSString *_installBarKey;
    NSPopover *_installPopover;
    NSString *_missingBefore;              // the open file's missing server, last time
}
@end
