// Lsp+JsonCheck.mm — Objective-C++. LspSession (JsonCheck): a JSON file no
// language server checks is checked by the editor itself, as VS Code's own
// JSON support does. Its parse error (JsonOutline::problems: the place and
// the message, a trailing comma on the comma) goes into the diagnostics
// store as the list of a server of its own, "mutible.json", so it is drawn,
// listed in Problems and counted in the status bar as a server's would be,
// and follows typing until the next check. Checked when the file comes on
// show and 0.3 s after typing, on a queue; nothing once it parses. JSON
// with comments (JsonOutline::allowsComments: tsconfig.json, .vscode's
// files, .jsonc) may have comments and trailing commas. A server that takes
// the file (lsp.<name>.extensions) replaces the check, and a tab's list
// goes when the tab closes.
#import "Lsp+Private.h"
#import "Lsp+JsonCheckIvars.h"
#include "JsonOutline.h"

static const char *const kJsonServer = "mutible.json";
// Past this many characters no check: such a file is in large-file mode
// before long, and its parse is a second outline's worth of memory.
static const NSUInteger kJsonCheckLimit = 8u << 20;

@implementation LspSession (JsonCheck)

- (NSString *)jsonCheckedUri { return _jsonUri; }

// The file on show came, or its servers changed: checked by the editor
// when it is JSON and no server has it; a server's list replaces the
// editor's.
- (void)jsonCheckDocument {
    NSString *uri = _path && JsonOutline::checksAsJson(Std(_path))
                        ? NS(Lsp::uriFromPath(Std(RealPath(_path)))) : nil;
    if (uri && _servers.count) {
        if (_store.erase(Std(uri), kJsonServer)) [self problemsChanged];
        uri = nil;
    }
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(checkJsonNow)
                                               object:nil];
    _jsonGeneration++;   // an answer on its way was for what was here before
    _jsonUri = uri;
    if (!uri) return;
    [self paintDiagnostics];   // what an earlier check found, until this one answers
    [self checkJsonAfter:0];
}

// The file left the view (documentOpened: brings the next one).
- (void)jsonDocumentClosed {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(checkJsonNow)
                                               object:nil];
    _jsonGeneration++;
    _jsonUri = nil;
}

- (void)checkJsonAfter:(NSTimeInterval)delay {
    if (!_jsonUri) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(checkJsonNow)
                                               object:nil];
    [self performSelector:@selector(checkJsonNow) withObject:nil afterDelay:delay];
}

- (void)checkJsonNow {
    NSString *uri = _jsonUri;
    CodeTextView *tv = _tv;
    if (!uri || !tv) return;
    const uint64_t gen = ++_jsonGeneration;
    NSString *text = [tv.string copy];   // the storage's string is live
    if (text.length > kJsonCheckLimit) {
        [self jsonChecked:std::vector<JsonOutline::Problem>() uri:uri base:nullptr];
        return;
    }
    const bool jsonc = JsonOutline::allowsComments(Std(_path));
    if (!_jsonQueue)
        _jsonQueue = dispatch_queue_create("ai.ible.mutible.json-check", DISPATCH_QUEUE_SERIAL);
    __weak LspSession *weakSelf = self;
    dispatch_async(_jsonQueue, ^{
        const char *utf8 = text.UTF8String;
        const std::vector<JsonOutline::Problem> found =
            JsonOutline::problems(utf8 ? utf8 : "", jsonc);
        // The text they were found in, so they are drawn where that text
        // is after any typing since (Lsp::diagnosticSpans).
        auto base = found.empty() ? nullptr : std::make_shared<const std::u16string>(U16Of(text));
        dispatch_async(dispatch_get_main_queue(), ^{
            LspSession *s = weakSelf;
            if (!s || gen != s->_jsonGeneration) return;   // a newer check is on its way
            [s jsonChecked:found uri:uri base:base];
        });
    });
}

- (void)jsonChecked:(const std::vector<JsonOutline::Problem> &)found
                uri:(NSString *)uri
               base:(std::shared_ptr<const std::u16string>)base {
    std::vector<Lsp::Diagnostic> list;
    for (const JsonOutline::Problem &p : found) {
        Lsp::Diagnostic d;
        d.range.start = Lsp::Position{(int)p.line, (int)p.column};
        d.range.end = Lsp::Position{(int)p.endLine, (int)p.endColumn};
        d.severity = Lsp::Severity::Error;
        d.message = p.message;
        d.source = "json";   // as VS Code names its own
        list.push_back(d);
    }
    if (!_store.publish(Std(uri), kJsonServer, list, base)) return;   // nothing then, nothing now
    if ([uri isEqualToString:_jsonUri]) [self paintDiagnostics];
    [self problemsChanged];
}

// The tab's path for a file the editor checked, by its canonical URI, or
// nil when no tab has it.
- (NSString *)jsonTabPathForURI:(NSString *)uri {
    if (!uri || !_store.find(Std(uri), kJsonServer)) return nil;
    for (NSString *path in self.openPaths ? self.openPaths() : @[])
        if ([NS(Lsp::uriFromPath(Std(RealPath(path)))) isEqualToString:uri]) return path;
    return nil;
}

// The tabs changed: the lists of files no tab has any more go.
- (void)forgetJsonChecksOfClosedTabs {
    BOOL changed = NO;
    for (const std::string &key : _store.uris()) {
        NSString *uri = NS(key);
        if (!_store.find(key, kJsonServer) || [uri isEqualToString:_jsonUri]) continue;
        if (![self jsonTabPathForURI:uri]) changed |= _store.erase(key, kJsonServer);
    }
    if (changed) [self problemsChanged];
}

@end
