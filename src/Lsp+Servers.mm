// Lsp+Servers.mm — Objective-C++. LspSession (Servers): the window's
// language servers, one entry per language and second server: which one a
// file is for, starting, restarting after a crash, stopping, the settings'
// commands, where a request goes (Lsp::routeRequest), and what the
// Language Servers popover shows of each.
#import "Lsp+Private.h"
#import "Lsp+ServersIvars.h"

// A server that crashes is started again after 1, 2, then 4 seconds; the
// fourth crash in a row leaves it stopped, so a server that cannot start
// never spins. Three minutes of running clears the count.
static const NSInteger kMaxAutomaticRestarts = 3;
static const NSTimeInterval kCrashForgiven = 180;

static NSString *StateText(MCLspState state) {
    switch (state) {
    case MCLspStateIdle: return @"not started";
    case MCLspStateStarting: return @"starting";
    case MCLspStateRunning: return @"running";
    case MCLspStateNotInstalled: return @"not installed";
    case MCLspStateOff: return @"off";
    case MCLspStateStopped: return @"stopped";
    case MCLspStateFailed: return @"failed to start";
    case MCLspStateRestarting: return @"restarting";
    case MCLspStateCrashed: return @"crashed";
    }
    return @"";
}

@implementation LspSession (Servers)

- (NSString *)missingServer {
    if (!_langKey || !_path || ![AppSettings shared].settings.lspEnabled()) return nil;
    MCLspEntry *e = _entries[_langKey];
    // A server of the user's own has no install commands: the status bar
    // says it is not installed, and no bar offers to install it.
    if (!Install::server(Std(_langKey))) return nil;
    return e.state == MCLspStateNotInstalled ? _langKey : nil;
}

- (void)lookAgainForMissingServer {
    // An optional server installed since the file opened (brew install
    // marksman in the terminal): the file gets it now.
    Lsp::Language optional;
    if (!_langKey && _path && [AppSettings shared].settings.lspEnabled() &&
        [self language:optional forPath:_path installed:YES]) {
        [self documentOpened:_path];
        return;
    }
    BOOL also = [self lookAgainForAlso];
    if (!self.missingServer && !also) return;
    [self attachDocument];   // ensureServer looks again for a missing one
    if (!self.missingServer || also) [self changed];
}

// The open file's second server, when it may have turned up: ruff
// installed since in a project that configures Ruff, or a missing one the
// settings name. YES when attaching again may start it.
- (BOOL)lookAgainForAlso {
    if (!_langKey || !_path || ![AppSettings shared].settings.lspEnabled()) return NO;
    if (_alsoKey) {
        MCLspEntry *e = _entries[_alsoKey];
        return !e.server && e.state == MCLspStateNotInstalled;
    }
    NSString *command = [self alsoCommandForLanguage:_langKey path:_path configured:NULL];
    if (!command) return NO;
    _alsoKey = NS(Lsp::alsoKey(Std(_langKey)));
    MCLspEntry *e = [self entryForKey:_alsoKey];
    if (!e.server) e.alsoCommand = command;
    return YES;
}

- (BOOL)hasServer { return _servers.count > 0; }

- (NSArray<NSString *> *)fileServerNames {
    NSMutableArray *out = [NSMutableArray array];
    for (MCLspServer *server in _servers) [out addObject:server.name];
    return out;
}

// The one of the file's servers that answers a request about `f`, or nil
// (Lsp::routeRequest: the main server, else another that offers it).
- (MCLspServer *)serverFor:(Lsp::Feature)f {
    if (!_uri || !_servers.count) return nil;
    std::vector<Lsp::ServerView> views;
    for (MCLspServer *server in _servers) {
        Lsp::ServerView v;
        v.main = !Lsp::isAlsoKey(Std(server.key));
        v.ready = server.client.state() == Lsp::Client::State::Ready;
        v.capabilities = &server.client.capabilities();
        views.push_back(v);
    }
    int i = Lsp::routeRequest(f, views);
    return i < 0 ? nil : _servers[(NSUInteger)i];
}

// The file's servers asked for code actions (Lsp::serversOffering).
- (NSArray<MCLspServer *> *)serversFor:(Lsp::Feature)f {
    if (!_uri) return @[];
    std::vector<Lsp::ServerView> views;
    for (MCLspServer *server in _servers) {
        Lsp::ServerView v;
        v.main = !Lsp::isAlsoKey(Std(server.key));
        v.ready = server.client.state() == Lsp::Client::State::Ready;
        v.capabilities = &server.client.capabilities();
        views.push_back(v);
    }
    NSMutableArray *out = [NSMutableArray array];
    for (int i : Lsp::serversOffering(f, views)) [out addObject:_servers[(NSUInteger)i]];
    return out;
}

- (void)windowBecameKey:(NSNotification *)note {
    if (_tv.window && note.object == _tv.window) [self lookAgainForMissingServer];
}

- (void)terminalCommandFinished:(NSNotification *)note {
    (void)note;
    [self lookAgainForMissingServer];
}

// ---- servers
- (NSString *)currentSettingsSignature {
    const Settings &st = [AppSettings shared].settings;
    NSMutableString *sig = [NSMutableString stringWithFormat:@"%d", st.lspEnabled()];
    std::vector<std::string> keys = Settings::lspServers();
    for (const std::string &k : st.lspCustomServers()) keys.push_back(k);
    for (const std::string &k : keys)
        [sig appendFormat:@"|%s|%s", st.lspCommand(k).c_str(),
                          st.lspCommand(Lsp::alsoKey(k)).c_str()];
    for (const auto &e : st.lspExtensions()) {
        [sig appendFormat:@"|%s:", e.first.c_str()];
        for (const std::string &x : e.second) [sig appendFormat:@"%s,", x.c_str()];
    }
    return sig;
}

// The server a file is for (Lsp::languageForExtension with the settings'
// own servers and extensions). An optional built-in server (texlab,
// marksman, taplo, bash-language-server) the settings name no command for
// counts only when it runs already or one of its commands is installed
// (with `installed`), so a Markdown file never brings a "not installed"
// line or an install bar; turned off, it does not count at all.
- (BOOL)language:(Lsp::Language &)lang forPath:(NSString *)path installed:(BOOL)installed {
    const Settings &st = [AppSettings shared].settings;
    if (!path || !Lsp::languageForExtension(Std(path.pathExtension.lowercaseString), lang,
                                            st.lspExtensions()))
        return NO;
    if (!Lsp::isOptionalServer(lang.server)) return YES;
    if (st.lspOff(lang.server)) return NO;
    if (!installed || !st.lspCommand(lang.server).empty()) return YES;
    if (_entries[NS(lang.server)].server) return YES;
    NSArray *first = [self firstDirsForKey:NS(lang.server)];
    for (const std::string &cmd : Lsp::defaultCommands(lang.server))
        if (MCResolveCommand(cmd, first)) return YES;
    return NO;
}

- (MCLspEntry *)entryForKey:(NSString *)key {
    MCLspEntry *e = _entries[key];
    if (!e) {
        e = [MCLspEntry new];
        e.key = key;
        e.state = MCLspStateIdle;
        _entries[key] = e;
    }
    return e;
}

// The command lines to try for a key: the settings' own, else the defaults.
// A second server runs the one command it was wanted with.
- (std::vector<std::string>)commandsForKey:(const std::string &)key {
    if (Lsp::isAlsoKey(key)) {
        NSString *command = _entries[NS(key)].alsoCommand ?: [self alsoCommandForKey:NS(key)];
        if (command.length) return {Std(command)};
        return {};
    }
    const Settings &st = [AppSettings shared].settings;
    if (!st.lspCommand(key).empty()) return {st.lspCommand(key)};
    return Lsp::defaultCommands(key);
}

// A second server's command when nothing has decided it yet (the popover,
// Start): the settings' own, else the built-in one as if the project asked.
- (NSString *)alsoCommandForKey:(NSString *)key {
    const Settings &st = [AppSettings shared].settings;
    std::string k = Std(key);
    if (!st.lspCommand(k).empty() && !st.lspOff(k)) return NS(st.lspCommand(k));
    std::string def = Lsp::defaultAlsoCommand(Lsp::languageOfKey(k), true);
    return def.empty() ? nil : NS(def);
}

// The next file asks again (documentOpened:): a ruff.toml may have come.
- (void)forgetRuffConfiguration {
    [_ruffConfigured removeAllObjects];
}

// Does the project around a file configure Ruff (Formatter::configFile)?
// Remembered per folder until the next file opens.
- (BOOL)projectConfiguresRuff:(NSString *)path {
    if (!path.length) return NO;
    NSString *dir = path.stringByDeletingLastPathComponent;
    NSNumber *known = _ruffConfigured[dir];
    if (known) return known.boolValue;
    NSFileManager *fm = [NSFileManager defaultManager];
    Formatter::Fs fs;
    fs.exists = [fm](const std::string &f) {
        return (bool)[fm fileExistsAtPath:NS(f)];
    };
    fs.read = [](const std::string &f) {
        NSString *t = [NSString stringWithContentsOfFile:NS(f) encoding:NSUTF8StringEncoding
                                                   error:nil];
        return t ? Std(t) : std::string();
    };
    NSString *root = RealPath(_root) ?: _root ?: @"/";
    BOOL yes = !Formatter::configFile("ruff", Std(RealPath(path) ?: path), Std(root), fs).empty();
    _ruffConfigured[dir] = @(yes);
    return yes;
}

// The second server a language wants for a file, as the command to run, or
// nil: lsp.<server>.also from the settings, else the built-in rule (Ruff's
// own server in a project that configures Ruff, and only when ruff is
// installed: a default is never offered for installing). *configured says
// the built-in rule would want one, installed or not.
- (NSString *)alsoCommandForLanguage:(NSString *)lang path:(NSString *)path
                          configured:(BOOL *)configured {
    const Settings &st = [AppSettings shared].settings;
    std::string key = Lsp::alsoKey(Std(lang));
    if (configured) *configured = NO;
    if (!st.lspEnabled() || st.lspOff(key)) return nil;
    if (!st.lspCommand(key).empty()) return NS(st.lspCommand(key));
    std::string command = Lsp::defaultAlsoCommand(Std(lang), [self projectConfiguresRuff:path]);
    if (command.empty()) return nil;
    if (configured) *configured = YES;
    if (!MCResolveCommand(command, [self firstDirsForKey:NS(key)])) return nil;
    return NS(command);
}

// Whether a running second server takes a file from a tab: always when the
// settings name it, else when the project around the file configures Ruff.
- (BOOL)alsoWantsPath:(NSString *)path key:(NSString *)key {
    const Settings &st = [AppSettings shared].settings;
    if (!st.lspCommand(Std(key)).empty()) return !st.lspOff(Std(key));
    return [self projectConfiguresRuff:path];
}

// Where a server is looked for before the usual folders: for Python, the
// interpreter's own bin, where pylsp (or ruff) may be installed.
- (NSArray<NSString *> *)firstDirsForKey:(NSString *)key {
    if (Lsp::languageOfKey(Std(key)) != "python") return nil;
    NSString *source = nil;
    NSString *interp = [self serverPythonInterpreterWithSource:&source];
    return interp ? @[interp.stringByDeletingLastPathComponent] : nil;
}

// The server for a key, started if it is not running and nothing says it
// should not be.
- (MCLspServer *)ensureServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    if (e.server) return e.server;
    // Not installed is looked for again: it may have been installed since.
    if (e.state != MCLspStateIdle && e.state != MCLspStateNotInstalled) return nil;
    return [self startEntry:e];
}

- (MCLspServer *)startEntry:(MCLspEntry *)e {
    e.generation++;
    e.message = nil;
    const Settings &st = [AppSettings shared].settings;
    std::string key = Std(e.key);
    if (!st.lspEnabled() || st.lspOff(key)) {
        e.state = MCLspStateOff;
        return nil;
    }
    NSArray *first = [self firstDirsForKey:e.key];
    MCLspServer *server = nil;
    NSArray *argv = nil;
    for (const std::string &cmd : [self commandsForKey:key]) {
        argv = MCResolveCommand(cmd, first);
        if (!argv) continue;
        e.command = NS(cmd);
        server = [[MCLspServer alloc] initWithKey:e.key argv:argv root:_root];
        break;
    }
    if (!argv) {
        e.state = MCLspStateNotInstalled;
        return nil;
    }
    if (!server) {
        e.state = MCLspStateFailed;
        e.message = [NSString stringWithFormat:@"%@ could not be run", argv.firstObject];
        return nil;
    }
    e.server = server;
    e.serverName = server.name;
    e.state = MCLspStateStarting;
    Lsp::Client &c = server.client;
    if ([e.key isEqualToString:@"python"]) {
        NSString *source = nil;
        NSString *interp = [self serverPythonInterpreterWithSource:&source];
        Lsp::PythonServer kind = Lsp::pythonServerKind(ArgvWords(argv));
        e.pythonKind = kind;
        e.interpreter = interp;
        e.interpreterSource = source;
        c.setInitializationOptions(Lsp::pythonInitializationOptions(kind, Std(interp)));
        c.setSettings(Lsp::pythonSettings(kind, Std(interp)));
    }
    __weak LspSession *weakSelf = self;
    __weak MCLspServer *weakServer = server;
    // A server being stopped (a new folder, a settings change) can still
    // answer or publish before it goes, when its replacement may have the
    // same file open under the same URI; what it sends then is dropped.
    c.onReady = [weakSelf, weakServer] {
        MCLspServer *s = weakServer;
        if (s && !s.stopping) [weakSelf serverReady:s];
    };
    c.onDiagnostics = [weakSelf, weakServer](const std::string &uri,
                                             const std::vector<Lsp::Diagnostic> &list) {
        MCLspServer *s = weakServer;
        if (s && !s.stopping)
            [weakSelf diagnosticsArrived:uri list:list base:s.client.textFor(uri) server:s];
    };
    // A server that refuses the handshake would otherwise read "starting…"
    // for good while every request is dropped: stop it and say so.
    c.onInitializeFailed = [weakSelf, weakServer](const std::string &message) {
        MCLspServer *s = weakServer;
        if (s && !s.stopping) [weakSelf server:s failedToStart:message];
    };
    // Errors the server shows (pyright saying it cannot find the Python it
    // was given) are kept for the popover.
    c.onShowMessage = [weakSelf, weakServer](int type, const std::string &message) {
        MCLspServer *s = weakServer;
        if (s && !s.stopping && type == 1) [weakSelf server:s said:NS(message)];
    };
    // An edit the server makes itself, usually while running a quick fix's
    // command. Applied once the reply has been read, never inside it.
    c.onApplyEdit = [weakSelf, weakServer](const Json &params,
                                           Lsp::Client::ApplyEditReply reply) {
        MCLspServer *s = weakServer;
        if (!s || s.stopping) { reply(false, "the server is stopping"); return; }
        Json p = params;
        dispatch_async(dispatch_get_main_queue(), ^{
            (void)s;   // keeps the client the reply goes through alive
            LspSession *me = weakSelf;
            if (!me) { reply(false, "the window closed"); return; }
            NSString *failure = nil;
            NSString *label = NS(p["label"].asString());
            NSInteger n = [me applyWorkspaceEdit:Lsp::parseWorkspaceEdit(p["edit"], CanonicalURI)
                                          server:s
                                           asked:nullptr
                                      actionName:label.length ? label
                                                 : me.runningCommandTitle ?: @"Edit"
                                         failure:&failure];
            if (n < 0) [me note:failure];
            reply(n >= 0, n < 0 ? Std(failure) : "");
        });
    };
    // A server registered or dropped file watchers: the folders watched
    // follow, once the request it came in has been answered.
    c.setFileWatching(true);
    c.onWatchersChanged = [weakSelf] {
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf updateFileWatcher]; });
    };
    // workspace/semanticTokens/refresh: the file on show's tokens again.
    c.onSemanticTokensRefresh = [weakSelf, weakServer] {
        dispatch_async(dispatch_get_main_queue(), ^{
            MCLspServer *s = weakServer;
            if (s && !s.stopping) [weakSelf semanticTokensRefreshedBy:s];
        });
    };
    // $/progress: the status bar and the popover say what it is doing.
    c.onProgress = [weakSelf, weakServer] {
        MCLspServer *s = weakServer;
        if (s && !s.stopping) [weakSelf progressChanged:s];
    };
    c.onProtocolError = [weakServer](const std::string &problem) {
        NSLog(@"MiniCode: %@: %s", weakServer.name, problem.c_str());
    };
    server.onExit = ^(MCLspServer *s) { [weakSelf serverExited:s]; };
    // The folder as the server sees it is spelled as didOpen spells files
    // (resolved), or it takes /tmp/x/a.py and /private/tmp/x/a.py for two
    // files and finds every reference twice. Paths it sends back are mapped
    // to the tree's spelling (treePathForURI:).
    c.initialize(Std(RealPath(_root) ?: _root), (int)getpid());
    return server;
}

- (MCLspEntry *)entryOf:(MCLspServer *)server {
    MCLspEntry *e = _entries[server.key];
    return e.server == server ? e : nil;
}

- (void)serverReady:(MCLspServer *)server {
    MCLspEntry *e = [self entryOf:server];
    if (!e) return;
    e.state = MCLspStateRunning;
    [self changed];
    [self serverMayComplete:server];
    [self semanticServerReady:server];
}

// A server began, reported or ended work. Coalesced to one redraw a
// run-loop turn, since a server indexing reports many times a second.
- (void)progressChanged:(MCLspServer *)server {
    if (![self entryOf:server] || _progressPending) return;
    _progressPending = YES;
    __weak LspSession *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        LspSession *me = weakSelf;
        if (!me) return;
        me->_progressPending = NO;
        [me changed];
    });
}

- (void)server:(MCLspServer *)server said:(NSString *)message {
    MCLspEntry *e = [self entryOf:server];
    if (!e) return;
    e.message = message;
    [self changed];
}

- (void)server:(MCLspServer *)server failedToStart:(const std::string &)message {
    MCLspEntry *e = [self entryOf:server];
    if (e) {
        // Not tried again until it is started by hand, or the folder or the
        // settings change.
        e.state = MCLspStateFailed;
        e.message = message.empty() ? nil : NS(message);
        e.server = nil;
        e.generation++;
    }
    [self forgetDocumentsOf:server];
    [self dropServer:server];
    [server stop];   // its client has already exited, so this just ends its input
    [self changed];
}

- (void)serverExited:(MCLspServer *)server {
    [self forgetDocumentsOf:server];
    [self finishWorkspaceSymbolsOf:server];
    __weak LspSession *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf updateFileWatcher]; });
    MCLspEntry *e = [self entryOf:server];
    if (!e || server.stopping) {
        [self dropServer:server];
        [self changed];
        return;
    }
    // It went on its own: a crash. Start it again a few times, waiting
    // longer each time, then leave it to the popover's Start.
    e.server = nil;
    if (-[server.started timeIntervalSinceNow] > kCrashForgiven) e.crashes = 0;
    e.crashes++;
    e.message = server.exitDescription;
    NSLog(@"MiniCode: %@ %@", server.name, server.exitDescription);
    if (e.crashes <= kMaxAutomaticRestarts) {
        e.state = MCLspStateRestarting;
        NSUInteger gen = ++e.generation;
        double delay = pow(2.0, (double)(e.crashes - 1));
        __weak LspSession *weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            LspSession *s = weakSelf;
            if (!s || s->_entries[e.key] != e || e.generation != gen ||
                e.state != MCLspStateRestarting)
                return;
            [s startEntry:e];
            [s attachDocument];
            [s changed];
        });
    } else {
        e.state = MCLspStateCrashed;
    }
    [self dropServer:server];
    [self changed];
}

- (void)stopEntry:(MCLspEntry *)e {
    e.generation++;
    MCLspServer *s = e.server;
    if (!s) return;
    e.server = nil;
    [self forgetDocumentsOf:s];
    [self finishWorkspaceSymbolsOf:s];
    [self dropServer:s];
    [s stop];
    [self updateFileWatcher];
}

- (void)stopAllServers {
    [self stopWatchingFilesAfter:^{
        for (MCLspEntry *e in self->_entries.allValues) [self stopEntry:e];
    }];
    [_entries removeAllObjects];
    [_docs removeAllObjects];
    _store.clear();
    [_ruffConfigured removeAllObjects];
    [self problemsChanged];
}

// A server going away takes its documents with it: nothing to close, and
// what it said about them no longer holds. A restart opens them again.
- (void)forgetDocumentsOf:(MCLspServer *)server {
    for (NSString *key in _docs.allKeys)
        if (_docs[key].server == server) [_docs removeObjectForKey:key];
    _store.eraseServer(Std(server.serverId));
    [self problemsChanged];
}

// A server the file on show was open on is going: the file carries on with
// its other servers, if any, or detaches.
- (void)dropServer:(MCLspServer *)server {
    if (![_servers containsObject:server]) return;
    BOOL others = _servers.count > 1;
    [self detachDocument];
    if (others && _path) [self attachDocument];
}

- (void)restartServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    [self stopEntry:e];
    e.crashes = 0;
    e.state = MCLspStateIdle;
    [self startEntry:e];
    [self attachDocument];
    [self changed];
}

- (void)stopServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    [self stopEntry:e];
    e.crashes = 0;
    e.message = nil;
    e.state = MCLspStateStopped;
    [self changed];
}

- (void)startServer:(NSString *)key {
    MCLspEntry *e = [self entryForKey:key];
    if (e.server) return;
    if (Lsp::isAlsoKey(Std(key)) && !e.alsoCommand) e.alsoCommand = [self alsoCommandForKey:key];
    e.crashes = 0;
    e.state = MCLspStateIdle;
    [self startEntry:e];
    [self attachDocument];
    [self changed];
}

- (void)restartCurrentServer {
    if (!_langKey) {
        NSBeep();
        [self flash:@"No language server for this file"];
        return;
    }
    [self restartServer:_langKey];
}

- (NSArray<MCLspServerInfo *> *)serverInfos {
    NSMutableArray<NSString *> *keys = [NSMutableArray array];
    if (_langKey) [keys addObject:_langKey];
    std::vector<std::string> known = Settings::lspServers();
    for (const std::string &k : [AppSettings shared].settings.lspCustomServers())
        known.push_back(k);
    for (const std::string &k : known)
        if (![keys containsObject:NS(k)]) [keys addObject:NS(k)];
    NSMutableArray<MCLspServerInfo *> *out = [NSMutableArray array];
    for (NSString *key in keys) {
        [out addObject:[self infoForKey:key]];
        MCLspServerInfo *also = [self alsoInfoForLanguage:key];
        if (also) [out addObject:also];
    }
    return out;
}

// What a server entry is doing, from its process when it runs, else from
// the first of its commands that is installed.
- (MCLspServerInfo *)infoForKey:(NSString *)key {
    const Settings &st = [AppSettings shared].settings;
    std::string k = Std(key);
    MCLspEntry *e = _entries[key];
    MCLspServerInfo *info = [MCLspServerInfo new];
    info.key = key;
    info.language = NS(Lsp::serverDisplayName(Lsp::languageOfKey(k)));
    info.current = [key isEqualToString:_langKey] || [key isEqualToString:_alsoKey];
    info.state = e ? e.state : MCLspStateIdle;
    info.message = e.message;
    if (e.server) {
        info.serverName = e.server.name;
        info.program = e.server.program;
        info.command = e.command;
        info.pid = e.server.pid;
        if (const Lsp::Progress *p = e.server.client.progress())
            info.progress = NS(Lsp::progressText(*p));
    } else {
        // What would run: the first command that is installed.
        std::vector<std::string> commands = [self commandsForKey:k];
        NSArray *first = [self firstDirsForKey:key];
        for (const std::string &cmd : commands) {
            NSArray *argv = MCResolveCommand(cmd, first);
            if (!argv) continue;
            info.command = NS(cmd);
            info.program = argv.firstObject;
            info.serverName = ServerDisplayName(key, argv);
            break;
        }
        if (!info.command) {
            NSMutableArray *all = [NSMutableArray array];
            for (const std::string &cmd : commands) [all addObject:NS(cmd)];
            info.command = [all componentsJoinedByString:@", or "];
            if (info.state == MCLspStateIdle) info.state = MCLspStateNotInstalled;
        }
    }
    if (!st.lspEnabled() || st.lspOff(k)) info.state = MCLspStateOff;
    if (info.state == MCLspStateNotInstalled) {
        const Install::Tool *tool = Install::server(k);
        if (tool && !tool->ways.empty()) info.installHint = NS(tool->ways.front().command);
    }
    if ([key isEqualToString:@"python"]) {
        if (e.server) {
            info.interpreter = e.interpreter;
            info.interpreterSource = e.interpreterSource;
        } else {
            NSString *source = nil;
            info.interpreter = [self serverPythonInterpreterWithSource:&source];
            info.interpreterSource = source;
        }
    }
    info.stateText = StateText(info.state);
    return info;
}

// A language's second server, for the popover: when the settings name one
// (or turn it off), when one has been started, or where the built-in rule
// has one (Ruff for Python), even if this project does not want it, so the
// popover says when it would run. Nil otherwise.
- (MCLspServerInfo *)alsoInfoForLanguage:(NSString *)lang {
    const Settings &st = [AppSettings shared].settings;
    std::string key = Lsp::alsoKey(Std(lang));
    NSString *alsoKey = NS(key);
    MCLspEntry *e = _entries[alsoKey];
    BOOL named = !st.lspCommand(key).empty();
    std::string builtIn = Lsp::defaultAlsoCommand(Std(lang), true);
    if (!named && !e && builtIn.empty()) return nil;
    BOOL configured = NO;
    BOOL here = [lang isEqualToString:_langKey] && _path;
    if (here) [self alsoCommandForLanguage:lang path:_path configured:&configured];
    MCLspServerInfo *info = [self infoForKey:alsoKey];
    info.language = [NSString stringWithFormat:@"%@, second server",
                                               NS(Lsp::serverDisplayName(Std(lang)))];
    if (!e.server && !named && !configured && (info.state == MCLspStateIdle ||
                                                info.state == MCLspStateNotInstalled)) {
        // The built-in rule does not want it here: say when it would.
        info.state = MCLspStateIdle;
        info.installHint = nil;
        info.notUsedHere = YES;
        info.stateText = @"not used here";
        if (!info.message.length)
            info.message = @"Ruff's own server runs beside the main one in a project that "
                           @"configures Ruff (a ruff.toml, a .ruff.toml, or a [tool.ruff] "
                           @"table in pyproject.toml), when ruff is installed; "
                           @"lsp.python.also in the settings file runs it everywhere.";
    }
    return info;
}

- (void)settingsChanged:(NSNotification *)note {
    (void)note;
    NSString *sig = [self currentSettingsSignature];
    NSString *python = NS([AppSettings shared].settings.pythonInterpreter());
    if (![sig isEqualToString:_settingsSignature]) {
        _settingsSignature = sig;
        _pythonSetting = python;
        NSString *path = _path;
        [self closeDocument];
        [self stopAllServers];
        [self documentOpened:path];
        return;
    }
    if (![python isEqualToString:_pythonSetting]) {
        _pythonSetting = python;
        [self applyPythonInterpreter];
    }
    [self semanticSettingsChanged];
}

@end
