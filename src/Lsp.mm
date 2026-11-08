// Lsp.mm — Objective-C++. See Lsp.h. The per-window glue (LspSession):
// making one, letting it go, and the status line. Its work is in categories
// of their own (Lsp+Private.h lists them): the servers (Lsp+Servers.mm),
// the Python interpreter (Lsp+Python.mm), the documents the servers have
// open (Lsp+Sync.mm), diagnostics and Problems (Lsp+Diagnostics.mm),
// completion and the signature hint (Lsp+Completion.mm), definition, hover
// and symbols (Lsp+Navigation.mm), rename, references and code actions
// (Lsp+Actions.mm), the workspace edit applier (Lsp+WorkspaceEdit.mm), and
// inlay hints (Lsp+InlayHints.mm).
// The server processes are in LspServer.mm, the editor's text view in
// CodeTextView.mm, and the completion list, hover and signature popups in
// LspPopups.mm.
#import "Lsp+Private.h"
// Every category's own instance variables, so this @implementation lays
// them out (Lsp+Private.h).
#import "Lsp+ActionsIvars.h"
#import "Lsp+CompletionIvars.h"
#import "Lsp+DiagnosticsIvars.h"
#import "Lsp+InlayHintsIvars.h"
#import "Lsp+NavigationIvars.h"
#import "Lsp+ServersIvars.h"
#import "Lsp+SyncIvars.h"

NSNotificationName const MCLspSessionDidChangeNotification = @"MCLspSessionDidChange";
NSNotificationName const MCPythonInterpreterDidChangeNotification =
    @"MCPythonInterpreterDidChange";

// The status dot's colors.
static NSColor *DotGreen(void) { return LHex(0x73C991); }
static NSColor *DotYellow(void) { return LHex(0xCCA700); }
static NSColor *DotRed(void) { return LHex(0xF14C4C); }
static NSColor *DotGray(void) { return LHex(0x9CA3AF); }

// ------------------------------------------------------------ LspSession
@implementation MCLspServerInfo
@end
@implementation MCPythonCandidate
@end

@implementation MCLspEntry
@end

@implementation MCLspDoc
@end

NSHashTable<LspSession *> *MCLspAllSessions(void) {
    static NSHashTable<LspSession *> *all;
    if (!all) all = [NSHashTable weakObjectsHashTable];
    return all;
}

@implementation LspSession {
    NSString *_transient;     // a brief message ("No definition found")
}

- (instancetype)initWithTextView:(CodeTextView *)textView root:(NSString *)root {
    if (!(self = [super init])) return nil;
    [MCLspAllSessions() addObject:self];
    _tv = textView;
    _root = [root copy];
    _entries = [NSMutableDictionary dictionary];
    _docs = [NSMutableDictionary dictionary];
    _servers = @[];
    _ruffConfigured = [NSMutableDictionary dictionary];
    _workspaceRequests = [NSMutableArray array];
    _pendingAccepts = [NSMutableArray array];
    _problemFiles = [NSMutableDictionary dictionary];
    _settingsSignature = [self currentSettingsSignature];
    _pythonSetting = NS([AppSettings shared].settings.pythonInterpreter());
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(storageEdited:)
               name:NSTextStorageDidProcessEditingNotification object:textView.textStorage];
    [nc addObserver:self selector:@selector(selectionChanged:)
               name:NSTextViewDidChangeSelectionNotification object:textView];
    [nc addObserver:self selector:@selector(settingsChanged:)
               name:MCSettingsDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(interpreterChosen:)
               name:MCPythonInterpreterDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(environmentConsentChanged:)
               name:MCPythonEnvironmentConsentDidChangeNotification object:nil];
    [nc addObserver:self selector:@selector(windowResignedKey:)
               name:NSWindowDidResignKeyNotification object:nil];
    // A server installed meanwhile (in the terminal, or anywhere while the
    // window was in the background) is picked up without a restart.
    [nc addObserver:self selector:@selector(windowBecameKey:)
               name:NSWindowDidBecomeKeyNotification object:nil];
    [nc addObserver:self selector:@selector(terminalCommandFinished:)
               name:MCTerminalCommandDidFinishNotification object:nil];
    __weak LspSession *weakSelf = self;
    // The list and the signature hint belong to the caret: they go when the
    // keyboard leaves the editor (Cmd+2, a click in the tree) or the window.
    textView.onFocusLost = ^{
        [weakSelf closeCompletion];
        [weakSelf closeSignature];
        [weakSelf closeHoverUnlessPointer];
        [weakSelf closePicker];
    };
    _pointerIndex = NSNotFound;
    textView.onMouseHover = ^(NSUInteger index) { [weakSelf pointerAt:index]; };
    textView.onScrolled = ^{ [weakSelf closeHover]; };
    textView.onCommandClick = ^BOOL(NSUInteger index) {
        LspSession *s = weakSelf;
        if (!s || ![s serverFor:Lsp::Feature::Definition]) return NO;
        [s->_tv setSelectedRange:NSMakeRange(index, 0)];
        [s definitionAtIndex:index];
        return YES;
    };
    [self startInlayHints];
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self shutdown];
    // shutdown reports the problems gone (problemsChanged), which schedules
    // reportProblems on self; a retain taken while deallocating keeps
    // nothing alive, and the perform then fired on a freed session, crashing
    // the app about 50 ms after a window closed.
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
}

// ---- status
- (BOOL)isSettingsFile:(NSString *)path {
    NSString *settings = [AppSettings shared].path;
    return path && settings && [RealPath(path) isEqualToString:RealPath(settings)];
}

- (void)updateStatus {
    NSString *s = @"";
    NSColor *dot = nil;
    MCLspEntry *e = _langKey ? _entries[_langKey] : nil;
    NSString *language = _langKey ? NS(Lsp::serverDisplayName(Std(_langKey))) : nil;
    if (_transient) {
        s = _transient;
    } else if (_langKey && ![AppSettings shared].settings.lspEnabled()) {
        s = @"Language servers off";
        dot = DotGray();
    } else if (_langKey && e) {
        std::vector<Lsp::ServerStatus> servers = {[self statusOfKey:_langKey main:YES]};
        if (_alsoKey) servers.push_back([self statusOfKey:_alsoKey main:NO]);
        // The marks drawn, which leave out diagnostics about text typed
        // over since they came, so the count says what the squiggles say.
        int errors = 0, warnings = 0;
        if (_uri)
            for (MCDiagnosticMark *m in _tv.diagnostics) {
                if (m.severity == (NSInteger)Lsp::Severity::Error) errors++;
                else if (m.severity == (NSInteger)Lsp::Severity::Warning) warnings++;
            }
        Lsp::StatusLine line = Lsp::statusLine(Std(language), servers, errors, warnings);
        s = NS(line.text);
        switch (line.dot) {
        case Lsp::StatusDot::None: dot = nil; break;
        case Lsp::StatusDot::Gray: dot = DotGray(); break;
        case Lsp::StatusDot::Green: dot = DotGreen(); break;
        case Lsp::StatusDot::Yellow: dot = DotYellow(); break;
        case Lsp::StatusDot::Red: dot = DotRed(); break;
        }
    } else if ([self isSettingsFile:_path]) {
        // The settings file names the servers, so say where to see them.
        NSInteger running = 0;
        for (MCLspEntry *x in _entries.allValues) if (x.state == MCLspStateRunning) running++;
        s = running ? [NSString stringWithFormat:@"Language servers: %ld running", (long)running]
                    : @"Language servers";
        dot = running ? DotGreen() : DotGray();
    }
    _statusText = s;
    _statusDot = dot;
    if (self.onStatus) self.onStatus(s, dot);
}

// One server entry as the status line sees it. MCLspState and
// Lsp::ServerState list the same states in the same order.
- (Lsp::ServerStatus)statusOfKey:(NSString *)key main:(BOOL)main {
    MCLspEntry *e = _entries[key];
    Lsp::ServerStatus st;
    st.main = main;
    st.state = (Lsp::ServerState)(e ? e.state : MCLspStateIdle);
    st.name = Std(e.server.name ?: e.serverName ?: @"");
    if (e.server)
        if (const Lsp::Progress *p = e.server.client.progress()) {
            st.progress = Lsp::progressText(*p);
            st.progressBegun = p->begun;
        }
    return st;
}

// Something the status bar or the popover shows has changed.
- (void)changed {
    [self updateStatus];
    [[NSNotificationCenter defaultCenter] postNotificationName:MCLspSessionDidChangeNotification
                                                        object:self];
}

- (void)flash:(NSString *)message {
    _transient = [message copy];
    [self updateStatus];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(endFlash)
                                               object:nil];
    [self performSelector:@selector(endFlash) withObject:nil afterDelay:3];
}
- (void)endFlash {
    _transient = nil;
    [self updateStatus];
}

- (NSString *)currentLanguage { return _langKey; }

// ---- rename, references and quick fixes
- (void)note:(NSString *)message {
    if (!message.length) return;
    _lastNote = [message copy];
    if (self.showNote) self.showNote(message);
    else [self flash:message];
}

// The server that does `f` for the file, or nil after a beep and a word on
// why: no server for the file, or none of its servers does it.
- (MCLspServer *)serverFor:(Lsp::Feature)f missing:(NSString *)what {
    if (!_servers.count || !_uri) {
        NSBeep();
        [self flash:_statusText.length ? _statusText : @"No language server for this file"];
        return nil;
    }
    MCLspServer *server = [self serverFor:f];
    if (!server) {
        NSBeep();
        NSMutableArray *names = [NSMutableArray array];
        for (MCLspServer *x in _servers) [names addObject:x.name];
        [self note:[NSString stringWithFormat:@"%@ %@ not %@",
                                              [names componentsJoinedByString:@" and "],
                                              names.count == 1 ? @"does" : @"do", what]];
        return nil;
    }
    return server;
}

@end
