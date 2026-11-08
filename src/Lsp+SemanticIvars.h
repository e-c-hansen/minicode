// Lsp+SemanticIvars.h — the instance variables only Lsp+Semantic.mm uses,
// which imports it, as does Lsp.mm, which lays them out. See the rule at
// the top of Lsp+Private.h.
#pragma once
#import "Lsp+Private.h"

@interface LspSession () {
    void (^_semanticHandler)(NSData *spans);   // onSemanticTokens
    // The server's last tokens for the file on show (for /full/delta), the
    // server and file they are from, the request out (one at a time), and
    // whether the text changed again while it was out.
    Lsp::SemanticTokens _semTokens;
    __weak MCLspServer *_semServer;
    NSString *_semUri;
    int _semRequest;
    CFAbsoluteTime _semAskedAt;
    BOOL _semAgain;
    NSUInteger _semAnswers, _semRequests;
}
@end
