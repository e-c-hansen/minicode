// Lsp+Python.mm — Objective-C++. LspSession (Python): the Python
// interpreter the servers, Run File and new terminals use (this folder's
// choice, python.interpreter, else found automatically), the candidates
// Select Python Interpreter lists, and the folder's answer about running
// its own tools (its .venv for the servers, and Format Document's
// formatters too), which is kept per folder. It keeps no state of its own.
#import "Lsp+Private.h"
#import "TreeDetails.h"   // MCAppDefaults

static NSString *const kInterpretersDefaultsKey = @"MCPythonInterpreters";
// Per folder (resolved path): "allow" or "deny" for the folder's own Python
// environment, which a language server would run. Not there: not asked yet.
static NSString *const kEnvConsentDefaultsKey = @"MCPythonEnvironmentConsent";
NSNotificationName const MCPythonEnvironmentConsentDidChangeNotification =
    @"MCPythonEnvironmentConsentDidChange";

// Also Format Document's answer for a formatter in the folder (Lsp.h).
NSString *MCFolderToolsConsent(NSString *root) {
    NSDictionary *all = [MCAppDefaults() dictionaryForKey:kEnvConsentDefaultsKey];
    id v = all[RealPath(root) ?: @""];
    return [v isKindOfClass:[NSString class]] ? v : nil;
}

// Store the folder's answer (nil forgets it), and tell every window.
static void StoreFolderToolsConsent(NSString *root, NSString *answer) {
    NSString *key = RealPath(root) ?: @"";
    NSUserDefaults *d = MCAppDefaults();
    NSMutableDictionary *all = [[d dictionaryForKey:kEnvConsentDefaultsKey] mutableCopy]
                                   ?: [NSMutableDictionary dictionary];
    if (answer) all[key] = answer;
    else [all removeObjectForKey:key];
    [d setObject:all forKey:kEnvConsentDefaultsKey];
    // Every window on this folder follows, its formatters and servers alike.
    [[NSNotificationCenter defaultCenter]
        postNotificationName:MCPythonEnvironmentConsentDidChangeNotification object:key];
}

void MCSetFolderToolsConsent(NSString *root, BOOL allow) {
    StoreFolderToolsConsent(root, allow ? @"allow" : @"deny");
}

void MCForgetFolderToolsConsent(NSString *root) {
    StoreFolderToolsConsent(root, nil);
}


@implementation LspSession (Python)

// ---- the Python interpreter
- (NSString *)rootKey { return RealPath(_root) ?: @""; }

- (NSString *)chosenPythonInterpreter {
    NSDictionary *all = [[NSUserDefaults standardUserDefaults]
                            dictionaryForKey:kInterpretersDefaultsKey];
    id p = all[[self rootKey]];
    return [p isKindOfClass:[NSString class]] && [p length] ? p : nil;
}

- (void)choosePythonInterpreter:(NSString *)path {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary *all = [[d dictionaryForKey:kInterpretersDefaultsKey] mutableCopy]
                                   ?: [NSMutableDictionary dictionary];
    if (path.length) all[[self rootKey]] = path;
    else [all removeObjectForKey:[self rootKey]];
    [d setObject:all forKey:kInterpretersDefaultsKey];
    // Every window on this folder follows, this one included.
    [[NSNotificationCenter defaultCenter]
        postNotificationName:MCPythonInterpreterDidChangeNotification object:[self rootKey]];
}

- (void)interpreterChosen:(NSNotification *)note {
    if ([note.object isEqual:[self rootKey]]) [self applyPythonInterpreter];
}

- (PythonEnvs::Context)pythonContext {
    PythonEnvs::Context ctx;
    BOOL pythonFile = [_langKey isEqualToString:@"python"] && _path;
    ctx.fileDir = Std(pythonFile ? RealPath(_path.stringByDeletingLastPathComponent)
                                 : RealPath(_root));
    ctx.root = Std(RealPath(_root));
    ctx.home = Std(NSHomeDirectory());
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    for (NSString *k in @[@"VIRTUAL_ENV", @"CONDA_PREFIX", @"PYENV_ROOT", @"WORKON_HOME",
                          @"CONDA_ENVS_PATH", @"POETRY_VIRTUALENVS_PATH", @"XDG_DATA_HOME",
                          @"XDG_CACHE_HOME"])
        if ([env[k] length]) ctx.env[Std(k)] = Std(env[k]);
    for (NSString *d in MCSearchDirs())
        if (![d isEqualToString:@"/usr/bin"]) ctx.pathDirs.push_back(Std(d));
    // /usr/bin/python3 is the developer tools' shim; their real Python is
    // offered instead, when they are installed.
    ctx.skip = {"/usr/bin/python3", "/usr/bin/python"};
    ctx.mac = true;
    return ctx;
}

static BOOL IsRunnable(NSString *path) {
    BOOL isDir = NO;
    return path.length && [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] &&
           !isDir && [[NSFileManager defaultManager] isExecutableFileAtPath:path];
}

- (NSString *)pythonInterpreterWithSource:(NSString **)source {
    return [self pythonInterpreterWithSource:source folderChoice:YES project:YES];
}

- (NSString *)serverPythonInterpreterWithSource:(NSString **)source {
    return [self pythonInterpreterWithSource:source folderChoice:YES
                                     project:[self folderEnvironmentAllowed]];
}

- (NSString *)defaultPythonInterpreterWithSource:(NSString **)source {
    return [self pythonInterpreterWithSource:source folderChoice:NO
                                     project:[self folderEnvironmentAllowed]];
}

// `project`: whether an environment inside the folder may be the automatic
// choice. A server runs the interpreter it is given (pyright asks it for
// its search paths) and is looked for beside it, so for the servers that
// needs the user's yes (folderEnvironmentAllowed); a run the user starts
// (Run File) does not.
- (NSString *)pythonInterpreterWithSource:(NSString **)source folderChoice:(BOOL)useChoice
                                  project:(BOOL)allowProject {
    NSString *missing = nil;
    NSString *chosen = useChoice ? [self chosenPythonInterpreter].stringByExpandingTildeInPath
                                 : nil;
    if (chosen) {
        if (IsRunnable(chosen)) { if (source) *source = @"this folder"; return chosen; }
        missing = @"this folder's choice is missing";
    }
    std::string setting = [AppSettings shared].settings.pythonInterpreter();
    if (!setting.empty()) {
        NSString *p = NS(setting).stringByExpandingTildeInPath;
        if (IsRunnable(p)) { if (source) *source = @"settings"; return p; }
        if (!missing) missing = @"python.interpreter is missing";
    }
    PythonEnvs::Context ctx = [self pythonContext];
    std::string automatic = PythonEnvs::automatic(ctx, allowProject);
    if (!allowProject && !missing) {
        std::string project = PythonEnvs::projectInterpreter(ctx.fileDir, ctx.root);
        if (!project.empty() && project != automatic)
            missing = [[self folderEnvironmentConsent] isEqualToString:@"deny"]
                          ? @"the folder's own environment is not used"
                          : @"the folder's own environment waits for your answer";
    }
    if (source) *source = missing ? [@"automatic; " stringByAppendingString:missing]
                                  : @"automatic";
    return automatic.empty() ? nil : NS(automatic);
}

- (NSString *)terminalPythonInterpreterInDirectory:(NSString *)dir waiting:(BOOL *)waiting {
    if (waiting) *waiting = NO;
    NSString *chosen = [self chosenPythonInterpreter].stringByExpandingTildeInPath;
    if (chosen && IsRunnable(chosen)) return chosen;
    std::string setting = [AppSettings shared].settings.pythonInterpreter();
    if (!setting.empty()) {
        NSString *p = NS(setting).stringByExpandingTildeInPath;
        if (IsRunnable(p)) return p;
    }
    PythonEnvs::Context ctx = [self pythonContext];
    if (dir.length) ctx.fileDir = Std(RealPath(dir));
    const BOOL allowed = [self folderEnvironmentAllowed];
    std::string automatic = PythonEnvs::automatic(ctx, allowed);
    if (!allowed && waiting && ![self folderEnvironmentConsent]) {
        std::string project = PythonEnvs::projectInterpreter(ctx.fileDir, ctx.root);
        if (!project.empty() && project != automatic) *waiting = YES;
    }
    return automatic.empty() ? nil : NS(automatic);
}

// ---- the folder's own Python environment
// "allow", "deny", or nil when the user has not been asked for this folder.
- (NSString *)folderEnvironmentConsent {
    return MCFolderToolsConsent(_root);
}

- (BOOL)folderEnvironmentAllowed {
    return [[self folderEnvironmentConsent] isEqualToString:@"allow"];
}

- (NSString *)folderEnvironmentAwaitingConsent {
    const Settings &st = [AppSettings shared].settings;
    if (![_langKey isEqualToString:@"python"] || !_path || !st.lspEnabled()) return nil;
    if (st.lspOff("python") && st.lspOff(Lsp::alsoKey("python"))) return nil;
    if ([self folderEnvironmentConsent]) return nil;
    // A choice made for the folder, or in the settings, is the user's own.
    NSString *chosen = [self chosenPythonInterpreter].stringByExpandingTildeInPath;
    if (chosen && IsRunnable(chosen)) return nil;
    if (!st.pythonInterpreter().empty() &&
        IsRunnable(NS(st.pythonInterpreter()).stringByExpandingTildeInPath))
        return nil;
    PythonEnvs::Context ctx = [self pythonContext];
    std::string project = PythonEnvs::projectInterpreter(ctx.fileDir, ctx.root);
    // The same one active where minicode started ($VIRTUAL_ENV) is trusted.
    if (project.empty() || PythonEnvs::automatic(ctx, false) == project) return nil;
    // bin/python's environment folder, as the folder names it (".venv").
    NSString *env = NS(project).stringByDeletingLastPathComponent.stringByDeletingLastPathComponent;
    NSString *root = RealPath(_root);
    if (root.length && [env hasPrefix:[root stringByAppendingString:@"/"]])
        env = [env substringFromIndex:root.length + 1];
    return env;
}

- (void)setFolderEnvironmentAllowed:(BOOL)allow {
    MCSetFolderToolsConsent(_root, allow);
}

// The answer changes which interpreter the Python servers get and where
// they are looked for, so the running ones start again (a missing one may
// be in the environment, so it is looked for again too).
- (void)environmentConsentChanged:(NSNotification *)note {
    if (![note.object isEqual:[self rootKey]]) return;
    for (NSString *key in @[@"python", NS(Lsp::alsoKey("python"))]) {
        MCLspEntry *e = _entries[key];
        if (!e) continue;
        if (e.server || e.state == MCLspStateNotInstalled) {
            if (Lsp::isAlsoKey(Std(key))) e.alsoCommand = [self alsoCommandForKey:key];
            [self restartServer:key];
        }
    }
    if (![_alsoKey length] && [_langKey isEqualToString:@"python"])
        [self lookAgainForMissingServer];
    [self changed];
}

- (NSArray<MCPythonCandidate *> *)pythonCandidates {
    NSMutableArray<MCPythonCandidate *> *out = [NSMutableArray array];
    NSMutableSet *seen = [NSMutableSet set];
    auto add = [&](NSString *path, NSString *kind, NSString *name, NSString *version) {
        if (!path.length || [seen containsObject:path]) return;
        [seen addObject:path];
        MCPythonCandidate *c = [MCPythonCandidate new];
        c.path = path;
        c.kind = kind;
        c.name = name;
        c.version = version ?: @"";
        [out addObject:c];
    };
    for (const PythonEnvs::Interpreter &i : PythonEnvs::find([self pythonContext]))
        add(NS(i.path), NS(i.kind), NS(i.name), NS(i.version));
    NSString *xcode = MCDeveloperToolPath(@"python3");
    if (xcode) add(xcode, @"Xcode tools", @"python3", NS(PythonEnvs::versionOf(Std(xcode))));
    // A choice that is none of these (picked with Other…) stays on the list.
    NSString *chosen = [self chosenPythonInterpreter].stringByExpandingTildeInPath;
    if (chosen && ![seen containsObject:chosen] && IsRunnable(chosen))
        add(chosen, @"chosen", chosen.lastPathComponent, NS(PythonEnvs::versionOf(Std(chosen))));
    return out;
}

// The interpreter may have changed (a choice, the setting, or in automatic
// mode a file in another environment): tell the server, the way it listens.
- (void)applyPythonInterpreter {
    [self applyPythonInterpreterForSwitch:NO];
}

// A switch to another file (forSwitch) changes the server only when that
// file resolves to another interpreter: a file in no environment keeps the
// one the server has, where it used to send "none" (and restart pylsp,
// which cannot be told to forget one) on every switch to such a file.
- (void)applyPythonInterpreterForSwitch:(BOOL)forSwitch {
    MCLspEntry *e = _entries[@"python"];
    NSString *source = nil;
    NSString *interp = [self serverPythonInterpreterWithSource:&source];
    if (!e.server) { [self changed]; return; }
    if (interp == e.interpreter || [interp isEqualToString:e.interpreter] ||
        (forSwitch && !interp && e.interpreter)) {
        if (interp) e.interpreterSource = source;
        [self changed];
        return;
    }
    Lsp::PythonServer kind = e.pythonKind;
    // jedi-language-server reads it only when starting, and pylsp cannot be
    // told to forget one.
    if (Lsp::pythonInterpreterNeedsRestart(kind) || (kind == Lsp::PythonServer::Pylsp && !interp)) {
        [self restartServer:@"python"];
        return;
    }
    e.interpreter = interp;
    e.interpreterSource = source;
    e.server.client.didChangeConfiguration(Lsp::pythonSettings(kind, Std(interp)));
    [self changed];
}

@end
