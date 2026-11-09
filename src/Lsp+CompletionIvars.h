// Lsp+CompletionIvars.h — the instance variables only Lsp+Completion.mm
// uses, which imports it, as does Lsp.mm, which lays them out. See the rule
// at the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@class MCPendingAccept;

@interface LspSession () {
    MCCompletionPopup *_popup;
    NSMutableArray<MCPendingAccept *> *_pendingAccepts;
    std::vector<Lsp::CompletionItem> _items, _shown;
    NSUInteger _anchor;       // where the word being completed starts
    BOOL _completing;         // a request is out or the list is showing
    BOOL _haveItems;          // the server has answered it
    BOOL _incomplete;         // ...with a list it says will change as you type
    NSString *_filteredFor;   // the word the list shown was filtered for
    BOOL _navigated;          // the arrow keys moved through it
    int _completionGen, _resolveGen, _signatureGen;
    int _resolveId;
    MCLspServer *_resolveServer;
    MCLspServer *_completionServer;   // the one the list was asked of
    MCSignaturePopup *_signature;
    BOOL _choiceMode;         // the list shows a snippet choice stop's options
    NSRange _choiceRange;     // ...for the stop's text, selected when it opened
}
@end
