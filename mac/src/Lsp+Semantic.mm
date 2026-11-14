// Lsp+Semantic.mm — Objective-C++. LspSession (Semantic): the server's
// semantic tokens for the file on show (textDocument/semanticTokens/full,
// then /full/delta from the last answer when the server offers it), as VS
// Code's editor.semanticHighlighting asks for them: when the file is shown,
// a moment after typing stops, and when the server says they changed
// (workspace/semanticTokens/refresh). One request is out at a time; an
// answer is decoded (LspClient.cpp) against the text it was computed on and
// handed to onSemanticTokens only while the view still holds that text,
// else it waits for the next. The editor lays them over the lexer's colors
// (EditorController+Highlighting.mm).
#import "Lsp+Private.h"
#import "Lsp+SemanticIvars.h"

@implementation LspSession (Semantic)

- (void (^)(NSData *))onSemanticTokens { return _semanticHandler; }
- (void)setOnSemanticTokens:(void (^)(NSData *))handler {
    _semanticHandler = handler;   // ARC copies a block it keeps
}
- (NSUInteger)semanticAnswers { return _semAnswers; }
- (NSUInteger)semanticRequests { return _semRequests; }

// A file on show, as source, with a server, the setting on and someone to
// tell.
- (BOOL)semanticWanted {
    return _semanticHandler && _uri && _servers.count && _tv.editable &&
           [AppSettings shared].settings.semanticHighlighting();
}

- (void)scheduleSemanticTokens:(NSTimeInterval)delay {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(requestSemanticTokens)
                                               object:nil];
    // Whether it is wanted is asked when it fires: the view may become
    // editable after the file is opened on the server.
    if (_semanticHandler && _uri && [AppSettings shared].settings.semanticHighlighting())
        [self performSelector:@selector(requestSemanticTokens) withObject:nil afterDelay:delay];
}

- (void)requestSemanticTokens {
    if (![self semanticWanted]) return;
    MCLspServer *server = [self serverFor:Lsp::Feature::SemanticTokens];
    // A server still starting is asked once it is ready (semanticServerReady:).
    if (!server || server.stopping || server.client.state() != Lsp::Client::State::Ready ||
        !server.client.offers(Lsp::Feature::SemanticTokens))
        return;
    // One that can no longer be answered (its server went, or has said
    // nothing for 10 s) is let go; else its answer asks again.
    if (_semRequest) {
        MCLspServer *asked = _semServer;
        if (asked == server && !asked.stopping && CFAbsoluteTimeGetCurrent() - _semAskedAt < 10) {
            _semAgain = YES;
            return;
        }
        if (asked && !asked.stopping) asked.client.cancel(_semRequest);
        _semRequest = 0;
        _semTokens = Lsp::SemanticTokens();
    }
    [self flushChanges];
    if (server != _semServer || ![_uri isEqualToString:_semUri]) {
        _semTokens = Lsp::SemanticTokens();
        _semServer = server;
        _semUri = _uri;
    }
    NSString *uri = _uri;
    const NSUInteger edits = _edits;
    __weak LspSession *weakSelf = self;
    __weak MCLspServer *weakServer = server;
    auto answered = [weakSelf, weakServer, uri, edits](Json &result, const Json &error) {
        [weakSelf semanticAnswer:result error:error server:weakServer uri:uri edits:edits];
    };
    Lsp::Client &client = server.client;
    if (!_semTokens.resultId.empty() && Lsp::offersSemanticTokensDelta(client.capabilities()))
        _semRequest = client.semanticTokensDelta(Std(uri), _semTokens.resultId, answered);
    else
        _semRequest = client.semanticTokensFull(Std(uri), answered);
    if (_semRequest) _semRequests++;
    _semAskedAt = CFAbsoluteTimeGetCurrent();
}

- (void)semanticAnswer:(Json &)result
                 error:(const Json &)error
                server:(MCLspServer *)server
                   uri:(NSString *)uri
                 edits:(NSUInteger)edits {
    // A server on its way out answers nothing anyone waits for: a request
    // given up on is cancelled, except on such a server.
    if (!server || server.stopping) return;
    _semRequest = 0;
    const BOOL again = _semAgain;
    _semAgain = NO;
    // Another file or server since: what replaced it asks for itself.
    if (!server || server != _semServer || ![uri isEqualToString:_semUri]) return;
    // The tokens are kept even when the text has moved on, since the
    // server's next delta is from this answer. One it could not make
    // (ContentModified, a failure) or that does not fit is forgotten, and
    // the next request asks for the whole document.
    const BOOL understood = error.isNull() && _semTokens.apply(result);
    if (!understood) _semTokens = Lsp::SemanticTokens();
    if (again) {
        [self scheduleSemanticTokens:0];
        return;
    }
    // Typing since the request: its own request, a moment after it
    // stops, brings the tokens for the new text.
    if (!understood || edits != _edits || ![uri isEqualToString:_uri] || ![self semanticWanted])
        return;
    const Lsp::SemanticLegend legend = Lsp::SemanticLegend::from(server.client.capabilities());
    const std::u16string text = U16Of(_tv.string);
    const Lsp::PositionIndex index(text);
    const std::vector<Lsp::SemanticSpan> spans =
        Lsp::decodeSemanticTokens(_semTokens.data, legend, index);
    _semAnswers++;
    _semanticHandler([NSData dataWithBytes:spans.data()
                                    length:spans.size() * sizeof(Lsp::SemanticSpan)]);
}

// The file leaves the view: the request out is let go and the tokens
// forgotten (the next file's are its own).
- (void)forgetSemanticTokens {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(requestSemanticTokens)
                                               object:nil];
    MCLspServer *server = _semServer;
    if (_semRequest && server && !server.stopping) server.client.cancel(_semRequest);
    _semRequest = 0;
    _semAgain = NO;
    _semTokens = Lsp::SemanticTokens();
    _semServer = nil;
    _semUri = nil;
}

- (void)semanticServerReady:(MCLspServer *)server {
    if ([_servers containsObject:server]) [self scheduleSemanticTokens:0];
}

- (void)semanticTokensRefreshedBy:(MCLspServer *)server {
    if (server && [_servers containsObject:server]) [self scheduleSemanticTokens:0];
}

// editor.semantic-highlighting turned on asks; turned off, the editor drops
// the colors itself (it reads the setting too).
- (void)semanticSettingsChanged {
    if ([AppSettings shared].settings.semanticHighlighting()) [self scheduleSemanticTokens:0];
    else [self forgetSemanticTokens];
}

@end
