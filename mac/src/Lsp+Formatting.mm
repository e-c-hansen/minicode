// Lsp+Formatting.mm — Objective-C++. LspSession (Formatting): Format
// Document through the file's language server (textDocument/formatting,
// or rangeFormatting for selected lines), for a file no formatter is
// planned for (Formatter::serverMayFormat): a kind of file minicode has no
// formatter for, such as TOML with taplo running, or Markdown, JSON, YAML
// and CSS in a project not set up for Prettier. A project's own formatter
// is never replaced by a server's, which would format in a style the
// project never chose. The answer's edits go in through the workspace
// edit applier as one undo step, refused if the text changed meanwhile.
// It keeps no state of its own.
#import "Lsp+Private.h"
#include "AutoIndent.h"

@implementation LspSession (Formatting)

// The running server that formats the file on show (the main one first),
// and whether it formats only a range.
- (MCLspServer *)formattingServerForRange:(BOOL)range useRange:(BOOL *)useRange {
    if (!_uri || !_servers.count) return nil;
    std::vector<Lsp::ServerView> views;
    for (MCLspServer *server in _servers) {
        Lsp::ServerView v;
        v.main = !Lsp::isAlsoKey(Std(server.key));
        v.ready = server.client.state() == Lsp::Client::State::Ready;
        v.capabilities = &server.client.capabilities();
        views.push_back(v);
    }
    bool ranged = false;
    int i = Lsp::routeFormatting(views, range, &ranged);
    if (useRange) *useRange = ranged;
    return i < 0 ? nil : _servers[(NSUInteger)i];
}

- (BOOL)canFormatDocument {
    return [self formattingServerForRange:NO useRange:nil] != nil;
}

- (BOOL)formatDocumentInRange:(NSRange)selection
                         done:(void (^)(NSString *note, BOOL error))done {
    BOOL useRange = NO;
    MCLspServer *server = [self formattingServerForRange:selection.length > 0
                                                useRange:&useRange];
    if (!server) return NO;
    [self closeCompletion];
    [self closeSignature];
    [self flushChanges];
    NSString *uri = _uri;
    NSString *before = [_tv.string copy];
    std::u16string text = U16Of(before);
    // The file's own indentation, as the editor's Return and Tab use it.
    AutoIndent::Unit unit = AutoIndent::detect(text, Std(_path.lastPathComponent ?: @""));
    // What every open file's text was when asked, so an answer for text
    // that has changed since is refused (applyWorkspaceEdit:server:asked:).
    auto asked = std::make_shared<const Lsp::DocVersions>(server.client.versions());
    NSString *name = server.name.length ? server.name : @"the language server";

    // Called once: the edit, nothing to change, a refusal, or the deadline
    // (a save waiting on this is never lost to a silent server).
    auto called = std::make_shared<bool>(false);
    void (^once)(NSString *, BOOL) = ^(NSString *note, BOOL error) {
        if (*called) return;
        *called = true;
        done(note, error);
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(),
                   ^{
                       once([NSString stringWithFormat:
                                @"%@ did not format the file within 5 seconds.", name],
                            YES);
                   });
    __weak LspSession *weakSelf = self;
    auto handler = [weakSelf, uri, before, server, asked, name, once, called](const Json &r,
                                                                             const Json &e) {
        Json result = r, error = e;
        // Applied once the reply has been read: applying talks to the server.
        dispatch_async(dispatch_get_main_queue(), ^{
            LspSession *s = weakSelf;
            if (!s || *called) return;
            if (![uri isEqualToString:s->_uri] || ![s->_tv.string isEqualToString:before]) {
                once(nil, NO);   // another file, or typed into: for text no longer there
                return;
            }
            if (NSString *why = ErrorText(error)) {
                once([NSString stringWithFormat:@"%@ could not format the file: %@", name, why],
                     YES);
                return;
            }
            Lsp::WorkspaceEdit we = Lsp::formattingEdit(Std(uri), result, CanonicalURI);
            if (we.editCount() == 0 && we.problem.empty()) {
                once([NSString stringWithFormat:@"Already formatted (%@).", name], NO);
                return;
            }
            NSString *failure = nil;
            NSInteger files = [s applyWorkspaceEdit:we server:server asked:asked.get()
                                         actionName:@"Format Document" failure:&failure];
            if (files < 0) { once(failure, YES); return; }
            if ([s->_tv.string isEqualToString:before])
                once([NSString stringWithFormat:@"Already formatted (%@).", name], NO);
            else
                once(nil, NO);
        });
    };
    if (useRange) {
        Lsp::Range range{Lsp::positionForOffset(text, selection.location),
                         Lsp::positionForOffset(text, NSMaxRange(selection))};
        server.client.rangeFormatting(Std(uri), range, unit.width, !unit.tabs, handler);
    } else {
        server.client.formatting(Std(uri), unit.width, !unit.tabs, handler);
    }
    return YES;
}

@end
