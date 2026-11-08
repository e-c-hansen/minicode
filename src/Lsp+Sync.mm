// Lsp+Sync.mm — Objective-C++. LspSession (Sync): the documents the
// servers have open. The file on show (attached to its servers or not),
// every other tab's file kept open on the running servers, didChange from
// the text storage's edits (ranged for servers that take them), didSave,
// the server's folding ranges, and the files changed on disk that the
// servers' watchers want (workspace/didChangeWatchedFiles).
#import "Lsp+Private.h"
#import "Lsp+SyncIvars.h"
#import "LspWatcher.h"

@implementation LspSession (Sync)

// ---- documents
- (void)detachDocument {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    [self closeCompletion];
    [self closeSignature];
    [self closeHover];
    [self closePicker];
    // Answers still on their way were for the document leaving the view.
    [self forgetActionRequests];
    [self forgetNavigationRequests];
    [self forgetSemanticTokens];
    // The document stays open on its servers while its tab is open (see
    // syncOpenDocuments), and so do its diagnostics, for when it is back.
    _servers = @[];
    _uri = nil;
    _changes.reset();
    [self forgetCompletionRequests];
    [self clearInlayHints];
    _tv.diagnostics = @[];
}

// Open the current file on its language's servers (the main one and a
// second one when wanted), starting them if need be, and with it every
// other tab's file those servers handle. Safe to call again: servers that
// are running stay, and one that has come up since is added.
- (void)attachDocument {
    if (_path && _langKey && [AppSettings shared].settings.lspEnabled()) {
        NSMutableArray<MCLspServer *> *servers = [NSMutableArray array];
        MCLspServer *main = [self ensureServer:_langKey];
        if (main) [servers addObject:main];
        if (_alsoKey) {
            MCLspServer *also = [self ensureServer:_alsoKey];
            if (also) [servers addObject:also];
        }
        _servers = servers;
        _uri = servers.count ? NS(Lsp::uriFromPath(Std(RealPath(_path)))) : nil;
    }
    [self syncOpenDocuments];
    if (_servers.count && _uri) {
        [self paintDiagnostics];
        [self scheduleInlayHints:0];
    }
}

// Leaves the file on its server: the tab is still open. syncOpenDocuments
// closes it there once its tab goes.
- (void)closeDocument {
    [self detachDocument];
    _path = nil;
    _langKey = nil;
    _alsoKey = nil;
    _languageId = nil;
}

- (void)documentOpened:(NSString *)path {
    [self closeDocument];
    _path = [path copy];
    Lsp::Language lang;
    // A project file may have been added or changed since (a ruff.toml).
    [self forgetRuffConfiguration];
    if (path && [self language:lang forPath:path installed:YES]) {
        _langKey = NS(lang.server);
        _languageId = NS(lang.languageId);
        NSString *alsoCommand = [self alsoCommandForLanguage:_langKey path:path configured:NULL];
        if (alsoCommand) {
            _alsoKey = NS(Lsp::alsoKey(lang.server));
            MCLspEntry *also = [self entryForKey:_alsoKey];
            if (!also.server) also.alsoCommand = alsoCommand;
        }
        BOOL wasRunning = _entries[_langKey].server != nil;
        [self attachDocument];
        [self scheduleFoldingRanges:0.1];
        [self scheduleSemanticTokens:0];
        // In automatic mode another file may sit in another environment.
        // Only a real change reaches the server (same environment: nothing).
        if (wasRunning && [_langKey isEqualToString:@"python"])
            [self applyPythonInterpreterForSwitch:YES];
    } else {
        [self syncOpenDocuments];   // a tab that closed or stopped showing text
    }
    [self changed];
}

- (void)tabsChanged {
    [self syncOpenDocuments];
}

// ---- folding ranges: the server's, when it offers them, take the place of
// the editor's own (FoldView.h) for as long as the text stays as it was.
- (void)scheduleFoldingRanges:(NSTimeInterval)delay {
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(requestFoldingRanges)
                                               object:nil];
    [self performSelector:@selector(requestFoldingRanges) withObject:nil afterDelay:delay];
}

- (void)requestFoldingRanges {
    if (!_uri || !_tv.editable) return;
    MCLspServer *server = [self serverFor:Lsp::Feature::FoldingRange];
    if (!server) return;
    [self flushChanges];
    MCFolds *folds = _tv.folds;
    const uint64_t gen = folds.generation;
    NSString *uri = _uri;
    __weak LspSession *weakSelf = self;
    server.client.foldingRanges(Std(uri), [weakSelf, uri, gen](const Json &r, const Json &e) {
        LspSession *me = weakSelf;
        if (!me || !e.isNull() || ![uri isEqualToString:me->_uri]) return;
        NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
        NSMutableArray<NSString *> *kinds = [NSMutableArray array];
        for (const Lsp::FoldingRange &f : Lsp::parseFoldingRanges(r)) {
            [ranges addObject:[NSValue valueWithRange:NSMakeRange((NSUInteger)f.startLine,
                                                                  (NSUInteger)(f.endLine - f.startLine))]];
            [kinds addObject:NS(f.kind)];
        }
        [me->_tv.folds setServerRanges:ranges kinds:kinds generation:gen];
    });
}

// Every tab's file stays open on its language's server, with the tab's text,
// so rename and references see unsaved changes in tabs not on show, and a
// cross-file rename's edits (unsaved) are what the server reads, not the
// disk. The file on show is the text view's; the others come from
// bufferText. Only servers already running (or starting) for a file on show
// take files from background tabs: a tab never starts a server unseen.
// Files whose tabs have gone are closed. Cheap when nothing changed: an
// unchanged text sends nothing (Client::didChange compares).
- (void)syncOpenDocuments {
    // The file on show's edits go first, as ranges where the server takes
    // them; its didOpen below then has nothing left to send.
    [self flushChanges];
    NSMutableDictionary<NSString *, MCLspDoc *> *wanted = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSString *> *texts = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSString *> *languageIds = [NSMutableDictionary dictionary];
    auto want = ^(NSString *uri, NSString *path, MCLspServer *server) {
        MCLspDoc *d = [MCLspDoc new];
        d.path = path;
        d.uri = uri;
        d.server = server;
        wanted[DocKey(uri, server)] = d;
    };
    if ([AppSettings shared].settings.lspEnabled()) {
        if (_servers.count && _uri) {
            for (MCLspServer *server in _servers) want(_uri, _path, server);
            texts[_uri] = _tv.string;
            languageIds[_uri] = _languageId;
        }
        NSArray<NSString *> *paths = self.openPaths ? self.openPaths() : @[];
        for (NSString *path in paths) {
            Lsp::Language lang;
            if (![self language:lang forPath:path installed:NO]) continue;
            NSString *uri = NS(Lsp::uriFromPath(Std(RealPath(path))));
            if (texts[uri]) continue;    // the file on show, or a tab seen already
            NSString *text = self.bufferText ? self.bufferText(path) : nil;
            if (!text) continue;         // not read yet, or not shown as text
            BOOL any = NO;
            for (NSString *key in @[NS(lang.server), NS(Lsp::alsoKey(lang.server))]) {
                MCLspServer *server = _entries[key].server;
                if (!server || server.stopping) continue;
                if (Lsp::isAlsoKey(Std(key)) && ![self alsoWantsPath:path key:key]) continue;
                want(uri, path, server);
                any = YES;
            }
            if (!any) continue;
            texts[uri] = text;
            languageIds[uri] = NS(lang.languageId);
        }
    }
    // Closed first, so a file that moved to another server is closed on
    // the old one before the new one opens it.
    for (NSString *key in _docs.allKeys) {
        if (wanted[key]) continue;
        MCLspDoc *had = _docs[key];
        if (!had.server.stopping) had.server.client.didClose(Std(had.uri));
        [_docs removeObjectForKey:key];
        if (_store.erase(Std(had.uri), Std(had.server.serverId))) [self problemsChanged];
    }
    for (NSString *key in wanted) {
        MCLspDoc *d = wanted[key];
        // didOpen of an open document is a didChange (and nothing at all
        // when the text is what the server has).
        d.server.client.didOpen(Std(d.uri), Std(languageIds[d.uri]), Std(texts[d.uri]));
        _docs[key] = d;
    }
}

// The path a tab has a file under, for a canonical URI, or nil.
- (NSString *)tabPathForURI:(NSString *)uri {
    for (MCLspDoc *d in _docs.allValues)
        if ([d.uri isEqualToString:uri]) return d.path;
    return nil;
}

- (void)documentSaved {
    if (!_servers.count || !_uri) return;
    // What changed goes as a didChange of its own (ranges where the server
    // takes them), so didSave's own sync finds nothing to send.
    [self flushChanges];
    std::string text = Std(_tv.string);
    for (MCLspServer *server in _servers) server.client.didSave(Std(_uri), text);
}

// A file written from any tab, on show or not (Save All, auto-save, closing):
// every server that has it open gets didSave with the text written, so
// save-triggered checks (rust-analyzer's cargo check, gopls) run. A file no
// server has open sends nothing.
- (void)documentSavedAtPath:(NSString *)path text:(NSString *)text {
    if (!path || !text) return;
    NSString *uri = NS(Lsp::uriFromPath(Std(RealPath(path))));
    if (_uri && [uri isEqualToString:_uri]) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                                   object:nil];
    }
    std::string utf8 = Std(text);
    for (MCLspDoc *d in _docs.allValues) {
        if (![d.uri isEqualToString:uri] || d.server.stopping) continue;
        d.server.client.didSave(Std(d.uri), utf8);
    }
}

// ---- watched files
// The servers that registered watchers, running and not on their way out.
- (NSArray<MCLspServer *> *)watchingServers {
    NSMutableArray *out = [NSMutableArray array];
    for (MCLspEntry *e in _entries.allValues) {
        MCLspServer *s = e.server;
        if (s && !s.stopping && s.client.state() == Lsp::Client::State::Ready &&
            s.client.watchesFiles())
            [out addObject:s];
    }
    return out;
}

// The folder (unless it is / or there is none) and the servers'
// RelativePattern folders, resolved, as FSEvents reports them; only
// existing folders, none inside another, as MCLspWatcher keeps them.
- (NSArray<NSString *> *)wantedWatchFolders {
    NSArray<MCLspServer *> *servers = [self watchingServers];
    if (!servers.count) return @[];
    NSMutableOrderedSet<NSString *> *want = [NSMutableOrderedSet orderedSet];
    NSString *root = RealPath(_root);
    if (root.length && ![root isEqualToString:@"/"]) [want addObject:root];
    for (MCLspServer *s in servers)
        for (const std::string &base : s.client.watchedBases()) {
            NSString *b = RealPath(NS(base));
            if (b.length && ![b isEqualToString:@"/"] && want.count < 64) [want addObject:b];
        }
    NSMutableArray<NSString *> *kept = [NSMutableArray array];
    for (NSString *p in [want.array sortedArrayUsingSelector:@selector(compare:)]) {
        BOOL dir = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&dir] || !dir) continue;
        BOOL inside = NO;
        for (NSString *k in kept)
            if ([p isEqualToString:k] || [p hasPrefix:[k stringByAppendingString:@"/"]]) inside = YES;
        if (!inside) [kept addObject:p];
    }
    return kept;
}

- (void)updateFileWatcher {
    if (_stoppingAll) return;
    NSArray<NSString *> *want = [self wantedWatchFolders];
    if (_watcher && [want isEqualToArray:_watcher.paths]) return;
    [_watcher stop];
    _watcher = nil;
    if (!want.count) return;
    __weak LspSession *weakSelf = self;
    _watcher = [[MCLspWatcher alloc] initWithPaths:want
                                           handler:^(const std::vector<Lsp::FileChange> &changes) {
        [weakSelf filesChangedOnDisk:changes];
    }];
}

// stopAllServers: every server stops without the watcher being remade for
// each, then the watcher goes.
- (void)stopWatchingFilesAfter:(dispatch_block_t)stopping {
    _stoppingAll = YES;
    stopping();
    _stoppingAll = NO;
    [_watcher stop];
    _watcher = nil;
}

// A batch from the watcher: each server gets the changes its watchers
// want. A base a server spelled another way than FSEvents does (through a
// link) gets the change under its own spelling too.
- (void)filesChangedOnDisk:(const std::vector<Lsp::FileChange> &)changes {
    _watchedBatches++;
    for (MCLspServer *s in [self watchingServers]) {
        std::vector<Lsp::FileChange> mine = changes;
        for (const std::string &base : s.client.watchedBases()) {
            std::string real = Std(RealPath(NS(base)));
            if (real.empty() || real == base) continue;
            for (const Lsp::FileChange &c : changes)
                if (c.path.size() > real.size() && c.path.compare(0, real.size(), real) == 0 &&
                    c.path[real.size()] == '/')
                    mine.push_back({base + c.path.substr(real.size()), c.type});
        }
        s.client.didChangeWatchedFiles(mine);
    }
}

- (NSArray<NSString *> *)watchedFolders { return _watcher.paths ?: @[]; }
- (NSUInteger)watchedBatches { return _watchedBatches; }

- (void)setRoot:(NSString *)root {
    [self closeDocument];
    [self stopAllServers];
    _root = [root copy];
    [self changed];
}

- (void)shutdown {
    [self closeDocument];
    [self stopAllServers];
}

// didChange, a moment after typing pauses: the edits since the text was
// last sent, as ranges, for servers that take them (and when they come to
// less than the text), else the whole text. Anything that asks the server
// about the text flushes first.
- (void)flushChanges {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    if (!_servers.count || !_uri) { _changes.reset(); return; }
    std::string text = Std(_tv.string);
    for (MCLspServer *server in _servers) server.client.didChange(Std(_uri), text, _changes);
    _changes.reset();
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    // Typing closes hover info and the lists; recoloring alone does not.
    if (ts.editedMask & NSTextStorageEditedCharacters) {
        _edits++;
        // editedRange is the edit's text now; changeInLength says how much
        // it replaced (several edits in one processEditing come as their
        // union, which is one edit as far as the text outside it goes).
        const NSRange er = ts.editedRange;
        const NSInteger removed = (NSInteger)er.length - ts.changeInLength;
        if (er.location != NSNotFound && removed >= 0)
            _changes.edit(er.location, (size_t)removed, er.length);
        else
            _changes.invalidate();
        [self noteEditForPendingAccepts:er.location];
        [self closeHover];
        [self closePicker];
    }
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || !_servers.count || !_uri) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    [self performSelector:@selector(flushChanges) withObject:nil afterDelay:0.3];
    [self scheduleFoldingRanges:0.6];
    [self scheduleSemanticTokens:0.3];
    // A rename or a fix going in is not typing: no completion or signature.
    if (_applyingEdit) return;
    [self popupsFollowEdit:ts];
}

- (void)windowResignedKey:(NSNotification *)note {
    if (!_tv.window || note.object != _tv.window) return;
    [self closeCompletion];
    [self closeSignature];
    [self closeHover];
    [self closePicker];
}

- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    [self closeHover];
    if (!_applyingEdit) [self closePicker];
    [self popupsFollowSelection];
}

@end
