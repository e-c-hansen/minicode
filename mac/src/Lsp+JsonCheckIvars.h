// Lsp+JsonCheckIvars.h — the instance variables only Lsp+JsonCheck.mm uses,
// which imports it, as does Lsp.mm, which lays them out. See the rule at
// the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@interface LspSession () {
    // The file on show, when the editor checks it as JSON itself (no
    // language server has it): its canonical URI, else nil.
    NSString *_jsonUri;
    dispatch_queue_t _jsonQueue;   // the checks, one after another
    uint64_t _jsonGeneration;      // the newest check; older answers are dropped
}
@end
