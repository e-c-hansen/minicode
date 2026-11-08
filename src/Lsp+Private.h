// Lsp+Private.h — what LspSession's own files share: Lsp.mm (making and
// letting go of a session, the status line) and its categories,
// Lsp+<Concern>.mm. Only they import it.
//
// The rules are EditorController's (CLAUDE.md, "EditorController and its
// categories"). The ivar block below holds only what two or more
// categories use; a variable of one category's goes in that category's
// own class extension, Lsp+<Concern>Ivars.h, imported by the category and
// by Lsp.mm, whose @implementation must see every class extension for the
// ivars to exist. A category reaches another's state through its methods
// (declared here), never its ivars. The structure:ivars core test checks
// both classes. Helpers several files use are `static inline` here, so a
// file that does not call one is not warned.
#pragma once
#import "LspShared.h"
#import "EditorFont.h"
#import "LspServer.h"
#import "LspPopups.h"

// ------------------------------------------------------------------ helpers
static inline std::u16string U16Of(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}

// The canonical spelling of a path (/tmp -> /private/tmp), so paths from a
// server and from the file tree can be compared. The path itself if it does
// not exist.
static inline NSString *RealPath(NSString *p) {
    if (!p.length) return p;
    char buf[PATH_MAX];
    if (realpath(p.fileSystemRepresentation, buf)) return [NSString stringWithUTF8String:buf];
    return p.stringByStandardizingPath;
}

// A file URI with its path resolved, so one file named two ways (/tmp and
// /private/tmp) is one file; anything else as it was.
static inline std::string CanonicalURI(const std::string &uri) {
    NSString *path = NS(Lsp::pathFromUri(uri));
    if (!path.length) return uri;
    return Lsp::uriFromPath(Std(RealPath(path)));
}

static inline BOOL IsIdentChar(unichar c) {
    return c == '_' || c == '$' ||
           [[NSCharacterSet alphanumericCharacterSet] characterIsMember:c];
}

// A server's error answer as a sentence, or nil when there was none.
static inline NSString *ErrorText(const Json &error) {
    if (error.isNull()) return nil;
    std::string m = error["message"].asString();
    return m.empty() ? @"The language server refused" : NS(m);
}

// ------------------------------------------------------- documentation text
// Documentation from a server, drawn simply: code (spans, blocks and the
// signature) in the monospaced font, the rest as plain text, with Markdown
// read by the preview's own parser. The completion list's documentation
// (Lsp+Completion.mm) and hover info (Lsp+Navigation.mm) both use it.
static inline NSFont *DocFont(BOOL mono, BOOL bold, BOOL italic, CGFloat size) {
    size = round(size * MCEditorScale() * 2) / 2;   // follows the editor's zoom
    NSFont *f = mono ? MCMonoFont(size, bold ? NSFontWeightSemibold : NSFontWeightRegular)
                     : (bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size]);
    if (italic) f = [[NSFontManager sharedFontManager] convertFont:f toHaveTrait:NSItalicFontMask];
    return f;
}

static inline void TrimTrailingSpace(NSMutableAttributedString *s) {
    NSCharacterSet *ws = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    while (s.length && [ws characterIsMember:[s.string characterAtIndex:s.length - 1]])
        [s deleteCharactersInRange:NSMakeRange(s.length - 1, 1)];
}

static inline NSAttributedString *MCDocText(NSString *text, BOOL markdown) {
    NSColor *body = LHex(0xCCCCCC);
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    if (!markdown) {
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:text attributes:@{NSFontAttributeName: DocFont(NO, NO, NO, 12),
                                              NSForegroundColorAttributeName: body}]];
        TrimTrailingSpace(s);
        return s;
    }
    for (const MdRun &r : MarkdownParser::parse(Std(text))) {
        if (r.gap) {   // between blocks: a short gap, not a whole line
            [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:5]}]];
            continue;
        }
        if (r.rule) continue;   // the gaps around it already part what it parts
        BOOL mono = r.code || r.codeBlock || r.math;
        NSString *text = NS(r.text);
        // pylsp escapes underscores even inside code spans (`object\_hook`),
        // where Markdown keeps the backslash; nobody means it there.
        if (r.code && !r.codeBlock)
            text = [text stringByReplacingOccurrencesOfString:@"\\_" withString:@"_"];
        NSFont *font = DocFont(mono, r.bold || r.heading, r.italic, mono ? 11.5 : 12);
        NSColor *color = r.codeBlock ? LHex(0xD4D4D4)
                       : r.code ? LHex(0xCE9178)
                       : r.link ? LHex(0x4EA1F7) : body;
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:text
                attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color}]];
    }
    TrimTrailingSpace(s);
    return s;
}

// ------------------------------------------------------------ the session's objects
// One language's server in one window: the process when there is one, and
// what the status bar and the popover say about it when there is not.
@interface MCLspEntry : NSObject
@property(nonatomic, copy) NSString *key;
@property(nonatomic, strong) MCLspServer *server;
@property(nonatomic, assign) MCLspState state;
@property(nonatomic, copy) NSString *message;   // why it failed or crashed
@property(nonatomic, copy) NSString *command;   // the command line it runs
@property(nonatomic, copy) NSString *serverName;   // "pyright", kept after it exits
@property(nonatomic, assign) NSInteger crashes;
// Bumped by every start and stop, so a restart scheduled after a crash can
// tell it has been overtaken.
@property(nonatomic, assign) NSUInteger generation;
// Python: the interpreter the running server was given, where it came from,
// and which server it is.
@property(nonatomic, copy) NSString *interpreter, *interpreterSource;
@property(nonatomic, assign) Lsp::PythonServer pythonKind;
// A second server (key "python.also"): the command it was started with,
// decided by the settings or the project when it is first wanted.
@property(nonatomic, copy) NSString *alsoCommand;
@end

// A file open in a tab of this window, open on its language's servers so
// they see every tab's unsaved text (rename, references), not only the one
// on show. One per file and server, keyed by DocKey.
@interface MCLspDoc : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, copy) NSString *uri;   // canonical
@property(nonatomic, strong) MCLspServer *server;
@end

static inline NSString *DocKey(NSString *uri, MCLspServer *server) {
    return [NSString stringWithFormat:@"%@ %@", server.serverId, uri];
}

// Every window's session, so a workspace edit can see other windows' tabs.
NSHashTable<LspSession *> *MCLspAllSessions(void);

// ------------------------------------------------------------ shared state
@interface LspSession () {
    // `root` (Lsp.h) is backed by this one; every category reads it.
    NSString *_root;
    __weak CodeTextView *_tv;
    NSMutableDictionary<NSString *, MCLspEntry *> *_entries;
    NSString *_path;          // the file in the view, even with no server
    NSString *_langKey;       // its language's server key, or nil
    NSString *_uri;           // its URI, when a server has it open
    // The servers it is open on: its language's main one first, when that
    // runs, then the second (lsp.<server>.also). Requests only one server
    // answers go where routeRequest says; code actions go to each.
    NSArray<MCLspServer *> *_servers;
    NSString *_alsoKey;       // its language's second server's key, when wanted for it
    // Every server's diagnostics, by canonical URI and server: for the
    // files open in tabs (with the text they were computed on, so they are
    // drawn on the text as it is now), and for files no tab has open (a
    // server checking the whole folder), listed in Problems only.
    Lsp::DiagnosticStore _store;
    NSMutableDictionary<NSString *, MCLspDoc *> *_docs;   // by DocKey
    NSUInteger _edits;        // counts edits to the text on show
    BOOL _applyingEdit;       // a WorkspaceEdit is going into the text
}
@end

// ------------------------------------------------------------ what the files call of each other
// Each category's methods the other files call. Lsp.h declares the ones
// the rest of the app calls, in an @interface of the same name, which is
// what the warning below is about.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-duplicate-category-definition"

// Lsp.mm
@interface LspSession ()
- (void)updateStatus;
- (void)changed;   // something the status bar or the popover shows
- (void)flash:(NSString *)message;
- (void)note:(NSString *)message;
- (MCLspServer *)serverFor:(Lsp::Feature)f missing:(NSString *)what;
@end

@interface LspSession (Servers)
- (MCLspServer *)serverFor:(Lsp::Feature)f;
- (NSArray<MCLspServer *> *)serversFor:(Lsp::Feature)f;
- (void)windowBecameKey:(NSNotification *)note;
- (void)terminalCommandFinished:(NSNotification *)note;
- (NSString *)currentSettingsSignature;
- (BOOL)language:(Lsp::Language &)lang forPath:(NSString *)path installed:(BOOL)installed;
- (MCLspEntry *)entryForKey:(NSString *)key;
- (NSString *)alsoCommandForKey:(NSString *)key;
- (NSString *)alsoCommandForLanguage:(NSString *)lang path:(NSString *)path
                          configured:(BOOL *)configured;
- (BOOL)alsoWantsPath:(NSString *)path key:(NSString *)key;
- (void)forgetRuffConfiguration;
- (MCLspServer *)ensureServer:(NSString *)key;
- (void)stopAllServers;
- (void)settingsChanged:(NSNotification *)note;
@end

@interface LspSession (Python)
- (void)interpreterChosen:(NSNotification *)note;
- (void)environmentConsentChanged:(NSNotification *)note;
- (void)applyPythonInterpreter;
- (void)applyPythonInterpreterForSwitch:(BOOL)forSwitch;
@end

@interface LspSession (Sync)
- (void)detachDocument;
- (void)attachDocument;
- (void)closeDocument;
- (void)syncOpenDocuments;
- (NSString *)tabPathForURI:(NSString *)uri;
- (void)updateFileWatcher;
- (void)stopWatchingFilesAfter:(dispatch_block_t)stopping;
- (void)flushChanges;
- (void)storageEdited:(NSNotification *)note;
- (void)windowResignedKey:(NSNotification *)note;
- (void)selectionChanged:(NSNotification *)note;
@end

@interface LspSession (Diagnostics)
- (BOOL)uri:(const std::string &)a isSameFileAs:(NSString *)b;
- (void)diagnosticsArrived:(const std::string &)uri
                      list:(const std::vector<Lsp::Diagnostic> &)list
                      base:(const std::u16string *)base
                    server:(MCLspServer *)server;
- (void)problemsChanged;
- (void)paintDiagnostics;
@end

@interface LspSession (Completion)
- (void)serverMayComplete:(MCLspServer *)server;
- (void)forgetCompletionRequests;
- (void)popupsFollowEdit:(NSTextStorage *)ts;
- (void)popupsFollowSelection;
- (void)noteEditForPendingAccepts:(NSUInteger)location;
- (void)closeCompletion;
- (void)closeSignature;
@end

@interface LspSession (Navigation)
- (void)forgetNavigationRequests;
- (void)definitionAtIndex:(NSUInteger)index;
- (NSString *)treePathForURI:(const std::string &)uri;
- (void)revealLocation:(const Lsp::Location &)loc;
- (void)pointerAt:(NSUInteger)index;
- (NSRange)wordAt:(NSUInteger)index;
- (void)showHoverAtCaret:(NSUInteger)index;
- (BOOL)hoverTakesCommand:(SEL)sel;
- (void)closeHoverUnlessPointer;
- (void)closeHover;
- (void)finishWorkspaceSymbolsOf:(MCLspServer *)server;
@end

@interface LspSession (Actions)
- (void)forgetActionRequests;
- (NSString *)runningCommandTitle;
- (BOOL)pickerTakesCommand:(SEL)sel;
- (void)closePicker;
@end

@interface LspSession (InlayHints)
- (void)startInlayHints;
- (void)scheduleInlayHints:(NSTimeInterval)delay;
- (void)clearInlayHints;
@end

@interface LspSession (Semantic)
- (void)scheduleSemanticTokens:(NSTimeInterval)delay;
- (void)forgetSemanticTokens;
- (void)semanticServerReady:(MCLspServer *)server;
- (void)semanticTokensRefreshedBy:(MCLspServer *)server;
- (void)semanticSettingsChanged;
@end

@interface LspSession (WorkspaceEdit)
- (NSString *)textOfPath:(NSString *)path uri:(const std::string &)uri;
- (NSInteger)applyWorkspaceEdit:(const Lsp::WorkspaceEdit &)we
                     actionName:(NSString *)name
                        failure:(NSString **)failure;
- (NSInteger)applyWorkspaceEdit:(const Lsp::WorkspaceEdit &)we
                         server:(MCLspServer *)server
                          asked:(const Lsp::DocVersions *)asked
                     actionName:(NSString *)name
                        failure:(NSString **)failure;
@end

#pragma clang diagnostic pop
