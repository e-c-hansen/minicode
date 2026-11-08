// IdeLinkSuite.mm — `make guitest SUITES="idelink mermaid"`.
//
// idelink: the Claude Code IDE link (IdeLinkServer.h) driven as Claude Code
// drives it: a WebSocket client on another thread dials the window's port
// from its lock file (under a scratch CLAUDE_CONFIG_DIR, never ~/.claude),
// with and without the secret, speaks MCP, and calls the tools Claude Code
// calls; the editor's side is checked on the window (tabs, the diff with
// Accept and Reject, the keyboard left where it was).
//
// mermaid: ```mermaid blocks in the Markdown preview (MarkdownMermaid.h),
// drawn by a stand-in mmdc that writes a known picture and records how it
// was run, or fails as mmdc does on a diagram that does not parse.
#import "Harness.h"
#import "EditorController.h"
#import "AgentNotes.h"
#import "AppSettings.h"
#import "CompareView.h"
#import "IdeLinkServer.h"
#import "MarkdownImage.h"
#import "MarkdownMermaid.h"
#include "IdeLink.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// ------------------------------------------------------- a test client
// A blocking WebSocket client, used only off the main thread (the server
// runs on the main queue).
@interface MCTestWSClient : NSObject
@property(nonatomic, readonly) NSString *response;   // the handshake's answer
- (BOOL)connectToPort:(int)port token:(NSString *)token;
- (BOOL)sendText:(NSString *)text;
- (BOOL)sendRaw:(const std::string &)bytes;
// The next frame: @{op, text} (op 1 text, 8 close, 10 pong), or nil after
// `timeout` seconds or when the server closed.
- (NSDictionary *)nextFrame:(double)timeout;
// The next JSON text message, skipping other frames.
- (NSDictionary *)nextMessage:(double)timeout;
// The answer to request `rid`, keeping notifications that come first.
- (NSDictionary *)answerTo:(int)rid timeout:(double)timeout notes:(NSMutableArray *)notes;
- (void)close;
@end

@implementation MCTestWSClient {
    int _fd;
    std::string _in;
    uint32_t _mask;
}

- (BOOL)connectToPort:(int)port token:(NSString *)token {
    _fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(_fd, (struct sockaddr *)&a, sizeof a) != 0) return NO;
    int on = 1;
    setsockopt(_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    NSMutableString *req = [NSMutableString stringWithFormat:
        @"GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        @"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
        @"Sec-WebSocket-Protocol: mcp\r\nUser-Agent: claude-code/test\r\n", port];
    if (token) [req appendFormat:@"X-Claude-Code-Ide-Authorization: %@\r\n", token];
    [req appendString:@"\r\n"];
    if (![self sendRaw:req.UTF8String]) return NO;
    const NSTimeInterval end = NSDate.timeIntervalSinceReferenceDate + 10;
    while (_in.find("\r\n\r\n") == std::string::npos && NSDate.timeIntervalSinceReferenceDate < end)
        if (![self readSome:0.5] && _fd < 0) break;
    const size_t head = _in.find("\r\n\r\n");
    if (head == std::string::npos) return NO;
    _response = [NSString stringWithUTF8String:_in.substr(0, head + 4).c_str()];
    _in.erase(0, head + 4);
    return [_response hasPrefix:@"HTTP/1.1 101 "];
}

// Reads what is there within `timeout`; NO when nothing came (or it closed).
- (BOOL)readSome:(double)timeout {
    if (_fd < 0) return NO;
    struct pollfd p = {_fd, POLLIN, 0};
    if (poll(&p, 1, (int)(timeout * 1000)) <= 0) return NO;
    char buf[65536];
    ssize_t n = read(_fd, buf, sizeof buf);
    if (n <= 0) { close(_fd); _fd = -1; return NO; }
    _in.append(buf, (size_t)n);
    return YES;
}

- (BOOL)sendRaw:(const std::string &)bytes {
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(_fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0) return NO;
        done += (size_t)n;
    }
    return YES;
}

- (BOOL)sendText:(NSString *)text {
    return [self sendRaw:IdeLink::encodeClientFrame(IdeLink::Opcode::Text, text.UTF8String, ++_mask * 2654435761u)];
}

- (NSDictionary *)nextFrame:(double)timeout {
    const NSTimeInterval end = NSDate.timeIntervalSinceReferenceDate + timeout;
    for (;;) {
        if (_in.size() >= 2) {
            const unsigned char *p = (const unsigned char *)_in.data();
            const int op = p[0] & 0x0F;
            uint64_t len = p[1] & 0x7F;
            size_t at = 2;
            if (len == 126 && _in.size() >= 4) { len = (uint64_t)p[2] << 8 | p[3]; at = 4; }
            else if (len == 127 && _in.size() >= 10) {
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
                at = 10;
            } else if (len >= 126) {
                at = SIZE_MAX;
            }
            if (at != SIZE_MAX && _in.size() >= at + len) {
                std::string payload = _in.substr(at, (size_t)len);
                _in.erase(0, at + (size_t)len);
                return @{@"op": @(op),
                         @"text": [[NSString alloc] initWithBytes:payload.data() length:payload.size()
                                                         encoding:NSUTF8StringEncoding] ?: @"",
                         @"raw": [NSData dataWithBytes:payload.data() length:payload.size()]};
            }
        }
        const double left = end - NSDate.timeIntervalSinceReferenceDate;
        if (left <= 0 || _fd < 0) return nil;
        [self readSome:MIN(left, 0.5)];
    }
}

- (NSDictionary *)nextMessage:(double)timeout {
    const NSTimeInterval end = NSDate.timeIntervalSinceReferenceDate + timeout;
    for (;;) {
        NSDictionary *f = [self nextFrame:MAX(0.05, end - NSDate.timeIntervalSinceReferenceDate)];
        if (!f) return nil;
        if ([f[@"op"] intValue] != 1) continue;
        NSData *d = [f[@"text"] dataUsingEncoding:NSUTF8StringEncoding];
        id j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
        if ([j isKindOfClass:NSDictionary.class]) return j;
    }
}

- (NSDictionary *)answerTo:(int)rid timeout:(double)timeout notes:(NSMutableArray *)notes {
    const NSTimeInterval end = NSDate.timeIntervalSinceReferenceDate + timeout;
    for (;;) {
        NSDictionary *m = [self nextMessage:MAX(0.05, end - NSDate.timeIntervalSinceReferenceDate)];
        if (!m) return nil;
        if ([m[@"id"] isKindOfClass:NSNumber.class] && [m[@"id"] intValue] == rid) return m;
        [notes addObject:m];
    }
}

- (void)close {
    if (_fd >= 0) close(_fd);
    _fd = -1;
}
- (void)dealloc { [self close]; }
@end

// ---------------------------------------------------------- helpers
static NSString *JSONLine(NSDictionary *d) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static NSString *Call(int rid, NSString *tool, NSDictionary *args) {
    return JSONLine(@{@"jsonrpc": @"2.0", @"id": @(rid), @"method": @"tools/call",
                      @"params": @{@"name": tool, @"arguments": args ?: @{}}});
}

// The text blocks of a tool's answer.
static NSArray<NSString *> *Texts(NSDictionary *answer) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *c in answer[@"result"][@"content"]) [out addObject:c[@"text"] ?: @""];
    return out;
}

static id JSONOf(NSString *text) {
    return [NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding]
                                           options:0 error:nil];
}

// Runs `block` on a client thread; it calls done itself (on any thread).
static dispatch_queue_t ClientQueue() {
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("idelink.test", DISPATCH_QUEUE_SERIAL);
    return q;
}

static void OnClient(NSString *name, double timeout, void (^block)(void (^finish)(void))) {
    MCGuiAsync(name, timeout, ^(MCGuiDone done) {
        dispatch_async(ClientQueue(), ^{
            block(^{ dispatch_async(dispatch_get_main_queue(), ^{ done(); }); });
        });
    });
}

static NSArray<NSDictionary *> *ShownTabs(EditorController *c) { return [c controlTabs]; }

static NSDictionary *TabTitled(EditorController *c, NSString *title) {
    for (NSDictionary *t in ShownTabs(c))
        if ([t[@"title"] isEqualToString:title]) return t;
    return nil;
}

static MCCompareView *CompareViewOf(EditorController *c) { return [c valueForKey:@"compareView"]; }

@interface EditorController (IdeLinkSuite)
- (void)selectTabAtIndex:(NSInteger)i;
@end

// Shows the tab with this title, as a click on it would; NO when none.
static BOOL ShowTab(EditorController *c, NSString *title) {
    NSArray<NSDictionary *> *tabs = ShownTabs(c);
    for (NSUInteger i = 0; i < tabs.count; i++)
        if ([tabs[i][@"title"] isEqualToString:title]) {
            [c selectTabAtIndex:(NSInteger)i];
            return YES;
        }
    return NO;
}

static int DeadPid() {
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    int st;
    waitpid(pid, &st, 0);
    return pid;
}

// ------------------------------------------------------------ the suite
MC_GUI_SUITE(idelink) {
    static NSString *configDir, *ideDir, *settingsBefore;
    static MCTestWSClient *client;
    static int port;
    static NSString *token;
    static NSMutableArray *notes;
    static __weak NSResponder *firstResponderBefore;
    notes = [NSMutableArray array];

    MCGuiStep(@"a scratch Claude Code folder, with lock files of others and a stale one of minicode's", ^{
        configDir = [MCGuiScratch() stringByAppendingPathComponent:@"claude-config"];
        ideDir = [configDir stringByAppendingPathComponent:@"ide"];
        setenv("CLAUDE_CONFIG_DIR", configDir.fileSystemRepresentation, 1);
        [[NSFileManager defaultManager] createDirectoryAtPath:ideDir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        GCHECK_EQ_STR(MCIdeLinkFolder(), ideDir);
        const int dead = DeadPid();
        MCGuiWriteFile([ideDir stringByAppendingPathComponent:@"11111.lock"],
                       [NSString stringWithFormat:@"{\"pid\":%d,\"ideName\":\"minicode\",\"workspaceFolders\":[],"
                                                  @"\"transport\":\"ws\",\"authToken\":\"x\"}", dead]);
        MCGuiWriteFile([ideDir stringByAppendingPathComponent:@"22222.lock"],
                       [NSString stringWithFormat:@"{\"pid\":%d,\"ideName\":\"Visual Studio Code\","
                                                  @"\"workspaceFolders\":[],\"transport\":\"ws\"}", dead]);
        MCGuiWriteFile([ideDir stringByAppendingPathComponent:@"33333.lock"],
                       [NSString stringWithFormat:@"{\"pid\":%d,\"ideName\":\"minicode\","
                                                  @"\"workspaceFolders\":[],\"transport\":\"ws\"}", (int)getppid()]);
        settingsBefore = [NSString stringWithContentsOfFile:NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"]
                                                   encoding:NSUTF8StringEncoding error:nil] ?: @"";
        GCHECK(MCIdeLinkFor(MCGuiController()) == nil);   // off by default
        MCGuiWriteFile(MCGuiProjectFile(@"ide/a.py"), @"def alpha():\n    return 1\n\ndef beta():\n    return 2\n");
        MCGuiWriteFile(MCGuiProjectFile(@"ide/b.py"), @"x = 1\nneedle = 2\ny = 3\n");
    });

    MCGuiStep(@"agent.ide-link = on gives the window a link and a lock file", ^{
        MCGuiSetSettings([settingsBefore stringByAppendingString:@"\nagent.ide-link = on\n"]);
    });
    MCGuiWait(@"the link starts", 5, ^BOOL { return MCIdeLinkFor(MCGuiController()) != nil; });
    MCGuiStep(@"the lock file says who and where, readable only by the user", ^{
        MCIdeLink *link = MCIdeLinkFor(MCGuiController());
        port = link.port;
        token = link.authToken;
        GCHECK(port > 0 && token.length >= 32);
        GCHECK_EQ_STR(link.lockPath, ([ideDir stringByAppendingPathComponent:
                                          [NSString stringWithFormat:@"%d.lock", port]]));
        struct stat st;
        GCHECK(stat(link.lockPath.fileSystemRepresentation, &st) == 0 && (st.st_mode & 0777) == 0600);
        NSDictionary *lock = JSONOf([NSString stringWithContentsOfFile:link.lockPath encoding:NSUTF8StringEncoding error:nil]);
        GCHECK([lock[@"pid"] intValue] == getpid());
        GCHECK_EQ_STR(lock[@"ideName"], @"minicode");
        GCHECK_EQ_STR(lock[@"transport"], @"ws");
        GCHECK_EQ_STR(lock[@"authToken"], token);
        GCHECK([lock[@"workspaceFolders"] containsObject:MCGuiProject()]);
        GCHECK([lock[@"workspaceFolders"] containsObject:MCGuiProject().stringByResolvingSymlinksInPath]);
        NSFileManager *fm = [NSFileManager defaultManager];
        // Its own dead one went; another IDE's and a live minicode's stayed.
        GCHECK(![fm fileExistsAtPath:[ideDir stringByAppendingPathComponent:@"11111.lock"]]);
        GCHECK([fm fileExistsAtPath:[ideDir stringByAppendingPathComponent:@"22222.lock"]]);
        GCHECK([fm fileExistsAtPath:[ideDir stringByAppendingPathComponent:@"33333.lock"]]);
        // Terminals of the window get the port.
        NSArray *env = MCIdeLinkTerminalEnvironment([MCGuiProject() stringByAppendingPathComponent:@"ide"]);
        NSString *want = [NSString stringWithFormat:@"CLAUDE_CODE_SSE_PORT=%d", port];
        GCHECK(env.count == 1 && [env.firstObject isEqualToString:want]);
        GCHECK(MCIdeLinkTerminalEnvironment(@"/usr").count == 0);
        // The menu item shows it on.
        NSMenuItem *item = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if (i.action == @selector(toggleIdeLink:)) item = i;
        GCHECK(item != nil);
        if (item) {
            [(id)item.target validateMenuItem:item];
            GCHECK(item.state == NSControlStateValueOn);
        }
    });

    OnClient(@"no secret, a wrong one, or a page's Host: turned away", 20, ^(void (^finish)(void)) {
        MCTestWSClient *bad = [MCTestWSClient new];
        BOOL ok = [bad connectToPort:port token:nil];
        BOOL saw401 = [bad.response hasPrefix:@"HTTP/1.1 401"];
        MCTestWSClient *wrong = [MCTestWSClient new];
        BOOL ok2 = [wrong connectToPort:port token:[token stringByAppendingString:@"x"]];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK(!ok && saw401);
            GCHECK(!ok2);
        });
        finish();
    });

    MCGuiStep(@"a Python file open with a selection", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:MCGuiProjectFile(@"ide/a.py")]);
        NSTextView *tv = [c valueForKey:@"textView"];
        [c.window makeFirstResponder:tv];
        tv.selectedRange = NSMakeRange(4, 5);   // "alpha"
        firstResponderBefore = c.window.firstResponder;
    });

    OnClient(@"the secret opens the link; MCP initialize; the selection arrives", 30, ^(void (^finish)(void)) {
        client = [MCTestWSClient new];
        BOOL ok = [client connectToPort:port token:token];
        NSString *accept = client.response;
        [client sendText:JSONLine(@{@"jsonrpc": @"2.0", @"id": @1, @"method": @"initialize",
                                    @"params": @{@"protocolVersion": @"2025-06-18", @"capabilities": @{},
                                                 @"clientInfo": @{@"name": @"claude-code", @"version": @"2.1.319"}}})];
        NSDictionary *init = [client answerTo:1 timeout:5 notes:notes];
        [client sendText:JSONLine(@{@"jsonrpc": @"2.0", @"method": @"notifications/initialized"})];
        [client sendText:JSONLine(@{@"jsonrpc": @"2.0", @"method": @"ide_connected", @"params": @{@"pid": @1234}})];
        NSDictionary *sel = [client nextMessage:5];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK(ok);
            GCHECK([accept containsString:@"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"]);
            GCHECK([accept containsString:@"Sec-WebSocket-Protocol: mcp\r\n"]);
            GCHECK_EQ_STR(init[@"result"][@"serverInfo"][@"name"], @"minicode");
            GCHECK_EQ_STR(init[@"result"][@"protocolVersion"], @"2025-06-18");
            GCHECK_EQ_STR(sel[@"method"], @"selection_changed");
            GCHECK_EQ_STR(sel[@"params"][@"text"], @"alpha");
            GCHECK([sel[@"params"][@"filePath"] hasSuffix:@"ide/a.py"]);
            GCHECK([sel[@"params"][@"selection"][@"start"][@"line"] intValue] == 0 &&
                   [sel[@"params"][@"selection"][@"start"][@"character"] intValue] == 4 &&
                   [sel[@"params"][@"selection"][@"end"][@"character"] intValue] == 9);
        });
        finish();
    });
    MCGuiWait(@"the link counts the client", 5, ^BOOL { return MCIdeLinkFor(MCGuiController()).clientCount == 1; });

    MCGuiStep(@"the selection moves: selection_changed follows", ^{
        NSTextView *tv = [MCGuiController() valueForKey:@"textView"];
        tv.selectedRange = NSMakeRange(31, 4);   // "beta" on line 3
    });
    OnClient(@"selection_changed for the new selection", 10, ^(void (^finish)(void)) {
        NSDictionary *m = [client nextMessage:5];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK_EQ_STR(m[@"method"], @"selection_changed");
            GCHECK_EQ_STR(m[@"params"][@"text"], @"beta");
            GCHECK([m[@"params"][@"selection"][@"start"][@"line"] intValue] == 3);
        });
        finish();
    });

    OnClient(@"tools/list, getCurrentSelection, getWorkspaceFolders, getOpenEditors, a ping", 20,
             ^(void (^finish)(void)) {
        [client sendText:JSONLine(@{@"jsonrpc": @"2.0", @"id": @2, @"method": @"tools/list"})];
        NSDictionary *list = [client answerTo:2 timeout:5 notes:notes];
        [client sendText:Call(3, @"getCurrentSelection", nil)];
        NSDictionary *sel = [client answerTo:3 timeout:5 notes:notes];
        [client sendText:Call(4, @"getWorkspaceFolders", nil)];
        NSDictionary *folders = [client answerTo:4 timeout:5 notes:notes];
        [client sendText:Call(5, @"getOpenEditors", nil)];
        NSDictionary *editors = [client answerTo:5 timeout:5 notes:notes];
        [client sendRaw:IdeLink::encodeClientFrame(IdeLink::Opcode::Ping, "hi", 77)];
        NSDictionary *pong = [client nextFrame:5];
        dispatch_async(dispatch_get_main_queue(), ^{
            NSMutableSet *names = [NSMutableSet set];
            for (NSDictionary *t in list[@"result"][@"tools"]) [names addObject:t[@"name"]];
            for (NSString *n in @[@"openFile", @"openDiff", @"getDiagnostics", @"close_tab",
                                  @"closeAllDiffTabs", @"getCurrentSelection", @"getLatestSelection",
                                  @"getOpenEditors", @"getWorkspaceFolders", @"checkDocumentDirty"])
                GCHECK([names containsObject:n]);
            GCHECK(![names containsObject:@"saveDocument"] && ![names containsObject:@"executeCode"]);
            NSDictionary *s = JSONOf(Texts(sel).firstObject);
            GCHECK([s[@"success"] boolValue] && [s[@"text"] isEqualToString:@"beta"]);
            NSDictionary *f = JSONOf(Texts(folders).firstObject);
            GCHECK_EQ_STR(f[@"folders"][0][@"path"], MCGuiProject());
            NSDictionary *e = JSONOf(Texts(editors).firstObject);
            BOOL found = NO;
            for (NSDictionary *t in e[@"tabs"]) found = found || [t[@"fileName"] hasSuffix:@"ide/a.py"];
            GCHECK(found);
            GCHECK([pong[@"op"] intValue] == 10 && [pong[@"text"] isEqualToString:@"hi"]);
        });
        finish();
    });

    OnClient(@"getDiagnostics, for every file and for one", 10, ^(void (^finish)(void)) {
        [client sendText:Call(6, @"getDiagnostics", nil)];
        NSDictionary *all = [client answerTo:6 timeout:5 notes:notes];
        NSString *uri = [@"file://" stringByAppendingString:MCGuiProjectFile(@"ide/a.py")];
        [client sendText:Call(7, @"getDiagnostics", @{@"uri": uri})];
        NSDictionary *one = [client answerTo:7 timeout:5 notes:notes];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK([JSONOf(Texts(all).firstObject) isKindOfClass:NSArray.class]);
            NSArray *o = JSONOf(Texts(one).firstObject);
            GCHECK(o.count == 1 && [o[0][@"uri"] isEqualToString:uri] && [o[0][@"diagnostics"] count] == 0);
        });
        finish();
    });

    OnClient(@"openFile with startText opens b.py beside, highlighting the text", 10, ^(void (^finish)(void)) {
        [client sendText:Call(8, @"openFile", @{@"filePath": @"ide/b.py", @"preview": @NO,
                                                 @"startText": @"needle", @"makeFrontmost": @YES})];
        NSDictionary *a = [client answerTo:8 timeout:5 notes:notes];
        [client sendText:Call(9, @"openFile", @{@"filePath": @"ide/nothere.py"})];
        NSDictionary *missing = [client answerTo:9 timeout:5 notes:notes];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK_EQ_STR(Texts(a).firstObject, @"Opened file and selected text \"needle\"");
            GCHECK([missing[@"result"][@"isError"] boolValue]);
            EditorController *c = MCGuiController();
            BOOL open = NO;
            for (NSDictionary *t in ShownTabs(c)) open = open || [t[@"title"] isEqualToString:@"b.py"];
            GCHECK(open);
        });
        finish();
    });
    MCGuiStep(@"the keyboard stayed where it was", ^{
        GCHECK(MCGuiController().window.firstResponder == firstResponderBefore);
    });

    OnClient(@"openDiff waits; Accept answers FILE_SAVED with the text, the file untouched", 30,
             ^(void (^finish)(void)) {
        [client sendText:Call(10, @"openDiff", @{@"old_file_path": MCGuiProjectFile(@"ide/a.py"),
                                                  @"new_file_path": MCGuiProjectFile(@"ide/a.py"),
                                                  @"new_file_contents": @"def alpha():\n    return 42\n",
                                                  @"tab_name": @"✻ [Claude Code] a.py (abc123) ⧉"})];
        NSDictionary *early = [client answerTo:10 timeout:1.5 notes:notes];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK(early == nil);   // waits for the user
            EditorController *c = MCGuiController();
            NSDictionary *tab = TabTitled(c, @"✻ [Claude Code] a.py (abc123) ⧉");
            GCHECK(tab != nil);
            // Opened beside the file the user is in, not in front of it.
            GCHECK(![tab[@"active"] boolValue]);
            GCHECK(ShowTab(c, @"✻ [Claude Code] a.py (abc123) ⧉"));
            MCCompareView *v = CompareViewOf(c);
            GCHECK(v.comparison.decisionPrompt.length > 0 && !v.acceptButton.hidden && !v.rejectButton.hidden);
            GCHECK_EQ_STR(v.comparison.rightText, @"def alpha():\n    return 42\n");
            MCGuiSnapshot(c.window.contentView, @"idelink-diff");
            [v.acceptButton performClick:nil];
            dispatch_async(ClientQueue(), ^{
                NSDictionary *answer = [client answerTo:10 timeout:5 notes:notes];
                dispatch_async(dispatch_get_main_queue(), ^{
                    NSArray *t = Texts(answer);
                    GCHECK(t.count == 2 && [t[0] isEqualToString:@"FILE_SAVED"] &&
                           [t[1] isEqualToString:@"def alpha():\n    return 42\n"]);
                    GCHECK([[NSString stringWithContentsOfFile:MCGuiProjectFile(@"ide/a.py")
                                                      encoding:NSUTF8StringEncoding error:nil]
                               containsString:@"return 1"]);
                    GCHECK(CompareViewOf(MCGuiController()).acceptButton.hidden);
                });
                finish();
            });
        });
    });

    OnClient(@"close_tab closes that diff tab, which answers its openDiff DIFF_REJECTED", 30,
             ^(void (^finish)(void)) {
        [client sendText:Call(11, @"close_tab", @{@"tab_name": @"✻ [Claude Code] a.py (abc123) ⧉"})];
        NSDictionary *closed = [client answerTo:11 timeout:5 notes:notes];
        [client sendText:Call(12, @"openDiff", @{@"old_file_path": MCGuiProjectFile(@"ide/b.py"),
                                                  @"new_file_path": MCGuiProjectFile(@"ide/b.py"),
                                                  @"new_file_contents": @"x = 2\n",
                                                  @"tab_name": @"diff two"})];
        [client answerTo:999 timeout:1.0 notes:notes];   // let it open
        [client sendText:Call(13, @"close_tab", @{@"tab_name": @"diff two"})];
        NSMutableArray *answers = [NSMutableArray array];
        NSDictionary *first = [client answerTo:12 timeout:5 notes:answers];
        NSDictionary *second = [client answerTo:13 timeout:5 notes:answers];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK_EQ_STR(Texts(closed).firstObject, @"TAB_CLOSED");
            GCHECK(TabTitled(MCGuiController(), @"✻ [Claude Code] a.py (abc123) ⧉") == nil);
            GCHECK_EQ_STR(Texts(first).firstObject, @"DIFF_REJECTED");
            GCHECK_EQ_STR(Texts(second).firstObject, @"TAB_CLOSED");
            GCHECK(TabTitled(MCGuiController(), @"diff two") == nil);
            // a.py's own tab is still open: close_tab closes only diffs.
            BOOL open = NO;
            for (NSDictionary *t in ShownTabs(MCGuiController())) open = open || [t[@"title"] isEqualToString:@"a.py"];
            GCHECK(open);
        });
        finish();
    });

    OnClient(@"Reject answers DIFF_REJECTED; closeAllDiffTabs closes the rest", 30, ^(void (^finish)(void)) {
        [client sendText:Call(14, @"openDiff", @{@"old_file_path": MCGuiProjectFile(@"ide/new.py"),
                                                  @"new_file_path": MCGuiProjectFile(@"ide/new.py"),
                                                  @"new_file_contents": @"print('new')\n",
                                                  @"tab_name": @"diff three"})];
        [client answerTo:999 timeout:1.0 notes:notes];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK(ShowTab(MCGuiController(), @"diff three"));
            MCCompareView *v = CompareViewOf(MCGuiController());
            GCHECK([v.comparison.decisionPrompt containsString:@"create new.py"]);
            [v.rejectButton performClick:nil];
            dispatch_async(ClientQueue(), ^{
                NSDictionary *answer = [client answerTo:14 timeout:5 notes:notes];
                [client sendText:Call(15, @"closeAllDiffTabs", nil)];
                NSDictionary *all = [client answerTo:15 timeout:5 notes:notes];
                dispatch_async(dispatch_get_main_queue(), ^{
                    GCHECK_EQ_STR(Texts(answer).firstObject, @"DIFF_REJECTED");
                    GCHECK_EQ_STR(Texts(all).firstObject, @"CLOSED_1_DIFF_TABS");
                    GCHECK(TabTitled(MCGuiController(), @"diff three") == nil);
                    GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:MCGuiProjectFile(@"ide/new.py")]);
                });
                finish();
            });
        });
    });

    // Per hunk: each change between the panes has its own Accept and Reject;
    // once both are decided the answer is the file made of the accepted one.
    OnClient(@"accept one of two hunks, reject the other: the answer holds only the first", 30,
             ^(void (^finish)(void)) {
        [client sendText:Call(20, @"openDiff", @{@"old_file_path": MCGuiProjectFile(@"ide/a.py"),
                                                  @"new_file_path": MCGuiProjectFile(@"ide/a.py"),
                                                  @"new_file_contents": @"def alpha():\n    return 10\n\ndef beta():\n    return 20\n",
                                                  @"tab_name": @"diff hunks"})];
        [client answerTo:999 timeout:1.0 notes:notes];
        dispatch_async(dispatch_get_main_queue(), ^{
            GCHECK(ShowTab(MCGuiController(), @"diff hunks"));
            MCCompareView *v = CompareViewOf(MCGuiController());
            GCHECK(v.comparison.hunkCount == 2);
            const NSRect accept0 = [v rectForHunk:0 accept:YES];
            const NSRect reject1 = [v rectForHunk:1 accept:NO];
            GCHECK(!NSIsEmptyRect(accept0) && !NSIsEmptyRect(reject1));
            MCGuiSnapshot(MCGuiController().window.contentView, @"idelink-hunks");
            GCHECK([v pressHunkButtonAtPoint:NSMakePoint(NSMidX(accept0), NSMidY(accept0))]);
            GCHECK([v.comparison decisionOfHunk:0] == 1 && v.comparison.decision == 0);
            GCHECK_EQ_STR(v.acceptButton.title, @"Accept the Rest");
            // A press on the decided hunk's mark makes it undecided again.
            GCHECK([v pressHunkButtonAtPoint:NSMakePoint(NSMidX(accept0), NSMidY(accept0))]);
            GCHECK([v.comparison decisionOfHunk:0] == 0 && !v.comparison.hunksDecided);
            GCHECK([v pressHunkButtonAtPoint:NSMakePoint(NSMidX(accept0), NSMidY(accept0))]);
            GCHECK([v pressHunkButtonAtPoint:NSMakePoint(NSMidX(reject1), NSMidY(reject1))]);
            GCHECK(v.comparison.decision == 1);
            dispatch_async(ClientQueue(), ^{
                NSDictionary *answer = [client answerTo:20 timeout:5 notes:notes];
                dispatch_async(dispatch_get_main_queue(), ^{
                    NSArray *t = Texts(answer);
                    GCHECK(t.count == 2 && [t[0] isEqualToString:@"FILE_SAVED"] &&
                           [t[1] isEqualToString:@"def alpha():\n    return 10\n\ndef beta():\n    return 2\n"]);
                    if (t.count == 2) MCGuiNote(@"answered %@", t[1]);
                    GCHECK([[NSString stringWithContentsOfFile:MCGuiProjectFile(@"ide/a.py")
                                                      encoding:NSUTF8StringEncoding error:nil]
                               containsString:@"return 1\n"]);
                    MCCompareView *after = CompareViewOf(MCGuiController());
                    GCHECK(after.acceptButton.hidden && NSIsEmptyRect([after rectForHunk:0 accept:YES]));
                });
                finish();
            });
        });
    });

    OnClient(@"a diff left waiting when Claude Code goes says so", 20, ^(void (^finish)(void)) {
        [client sendText:Call(16, @"openDiff", @{@"new_file_path": MCGuiProjectFile(@"ide/b.py"),
                                                  @"new_file_contents": @"x = 3\n", @"tab_name": @"diff four"})];
        [client answerTo:999 timeout:1.0 notes:notes];
        dispatch_sync(dispatch_get_main_queue(), ^{ GCHECK(ShowTab(MCGuiController(), @"diff four")); });
        [client close];
        finish();
    });
    MCGuiWait(@"the bar no longer offers Accept", 5, ^BOOL {
        MCCompareView *v = CompareViewOf(MCGuiController());
        return v.comparison.decision == 2 && v.acceptButton.hidden;
    });
    MCGuiWait(@"the link lets the client go", 5, ^BOOL { return MCIdeLinkFor(MCGuiController()).clientCount == 0; });

    // With MC_GUI_NODE naming node (22 or later), node's own WebSocket client
    // (undici, another implementation than the one above) dials in as Claude
    // Code does: the lock file, the "mcp" subprotocol, the token header.
    NSString *node = NSProcessInfo.processInfo.environment[@"MC_GUI_NODE"];
    if (node.length) {
        MCGuiAsync(@"node's WebSocket client speaks MCP to the link", 30, ^(MCGuiDone done) {
            NSString *js = [MCGuiScratch() stringByAppendingPathComponent:@"idelink-client.mjs"];
            MCGuiWriteFile(js,
                @"import fs from 'node:fs';\n"
                @"const dir = process.env.CLAUDE_CONFIG_DIR + '/ide';\n"
                @"const name = fs.readdirSync(dir).find(f => f.endsWith('.lock') &&\n"
                @"  JSON.parse(fs.readFileSync(dir + '/' + f)).ideName === 'minicode' &&\n"
                @"  JSON.parse(fs.readFileSync(dir + '/' + f)).pid === Number(process.argv[2]));\n"
                @"const lock = JSON.parse(fs.readFileSync(dir + '/' + name));\n"
                @"const ws = new WebSocket('ws://127.0.0.1:' + name.replace('.lock', ''),\n"
                @"  {protocols: ['mcp'], headers: {'X-Claude-Code-Ide-Authorization': lock.authToken}});\n"
                @"const out = {};\n"
                @"ws.onopen = () => { out.protocol = ws.protocol;\n"
                @"  ws.send(JSON.stringify({jsonrpc: '2.0', id: 1, method: 'initialize', params: {protocolVersion: '2025-06-18', capabilities: {}, clientInfo: {name: 'node', version: '1'}}})); };\n"
                @"ws.onmessage = (e) => { const m = JSON.parse(e.data);\n"
                @"  if (m.id === 1) { out.server = m.result.serverInfo.name;\n"
                @"    ws.send(JSON.stringify({jsonrpc: '2.0', method: 'notifications/initialized'}));\n"
                @"    ws.send(JSON.stringify({jsonrpc: '2.0', id: 2, method: 'tools/call', params: {name: 'getWorkspaceFolders', arguments: {}}})); }\n"
                @"  else if (m.id === 2) { out.folders = JSON.parse(m.result.content[0].text).folders.map(f => f.path);\n"
                @"    console.log(JSON.stringify(out)); ws.close(); }\n"
                @"  else if (m.method) { out.notes = (out.notes || []).concat(m.method); } };\n"
                @"ws.onerror = (e) => { console.log(JSON.stringify({error: String(e.message || e)})); process.exit(1); };\n"
                @"setTimeout(() => { console.log(JSON.stringify({error: 'timeout', out})); process.exit(2); }, 10000);\n"
                @"ws.onclose = () => process.exit(0);\n");
            MCGuiRunProcess(@[node, js, [NSString stringWithFormat:@"%d", getpid()]], nil,
                            ^(int status, NSString *out, NSString *err) {
                NSDictionary *r = JSONOf(out);
                if (status != 0) MCGuiNote(@"node: %d %@ %@", status, out, err);
                GCHECK(status == 0);
                GCHECK_EQ_STR(r[@"protocol"], @"mcp");
                GCHECK_EQ_STR(r[@"server"], @"minicode");
                GCHECK([r[@"folders"] containsObject:MCGuiProject()]);
                done();
            });
        });
    }

    MCGuiStep(@"the menu item turns the link off: the lock file goes", ^{
        static NSString *lock;
        lock = MCIdeLinkFor(MCGuiController()).lockPath;
        NSMenuItem *item = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if (i.action == @selector(toggleIdeLink:)) item = i;
        [NSApp sendAction:item.action to:item.target from:item];
        GCHECK(![AppSettings shared].settings.ideLink());
        GCHECK(MCIdeLinkFor(MCGuiController()) == nil);
        GCHECK(![[NSFileManager defaultManager] fileExistsAtPath:lock]);
        GCHECK(MCIdeLinkTerminalEnvironment(MCGuiProject()).count == 0);
        // Other IDEs' lock files are still there.
        GCHECK([[NSFileManager defaultManager] fileExistsAtPath:[ideDir stringByAppendingPathComponent:@"22222.lock"]]);
        MCGuiSetSettings(settingsBefore);
        // Its tabs go: a later suite's fake language server finds files by name (a.py).
        [MCGuiController() closeAllTabs:nil];
    });
}

// --------------------------------------------------------------- mermaid
MC_GUI_SUITE(mermaid) {
    static NSString *log, *settingsBefore;

    MCGuiStep(@"a stand-in mmdc and a page with two diagrams", ^{
        NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"mermaid"];
        NSString *png = [dir stringByAppendingPathComponent:@"picture.png"];
        log = [dir stringByAppendingPathComponent:@"runs.log"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        // A 240 x 120 pixel picture: 120 x 60 points at mmdc's scale of 2.
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:nil pixelsWide:240 pixelsHigh:120 bitsPerSample:8 samplesPerPixel:4
                            hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0
                        bitsPerPixel:0];
        memset(rep.bitmapData, 0x80, (size_t)rep.bytesPerRow * 120);
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:png atomically:YES];
        NSString *script = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"echo \"$@\" >> '%@'\n"
            @"while [ $# -gt 0 ]; do case \"$1\" in -i) in=\"$2\"; shift;; -o) out=\"$2\"; shift;; esac; shift; done\n"
            @"if grep -q broken \"$in\"; then\n"
            @"  printf '\\nError: Parse error on line 2:\\n...broken\\n---^\\nExpecting NODE\\n"
            @"Parser.parseError (https://x.invalid/a.mjs:1:2)\\n' >&2; exit 1\n"
            @"fi\n"
            @"cp '%@' \"$out\"\n", log, png];
        NSString *mmdc = [dir stringByAppendingPathComponent:@"mmdc"];
        MCGuiWriteFile(mmdc, script);
        chmod(mmdc.fileSystemRepresentation, 0755);
        setenv("MINICODE_MMDC", mmdc.fileSystemRepresentation, 1);
        setenv("MINICODE_MERMAID_CACHE", [dir stringByAppendingPathComponent:@"cache"].fileSystemRepresentation, 1);
        settingsBefore = [NSString stringWithContentsOfFile:NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"]
                                                   encoding:NSUTF8StringEncoding error:nil] ?: @"";
        MCGuiWriteFile(MCGuiProjectFile(@"diagrams.md"),
                       @"# Diagrams\n\nBefore.\n\n```mermaid\nflowchart LR\n  A --> B\n```\n\n"
                       @"Between.\n\n```mermaid\nflowchart LR\n  broken -->\n```\n\nAfter.\n");
        GCHECK([MCGuiController() showFile:MCGuiProjectFile(@"diagrams.md")]);
        NSTextView *tv = [MCGuiController() valueForKey:@"textView"];
        // Until mmdc is done, the source shows as code.
        GCHECK([tv.string containsString:@"A --> B"]);
    });

    // Runs go one at a time: wait for the picture and for the broken
    // diagram's complaint, which comes from the second run (under load the
    // first could land well before it).
    MCGuiWait(@"the diagram arrives as a picture, and the broken one's complaint", 15, ^BOOL {
        NSTextView *tv = [MCGuiController() valueForKey:@"textView"];
        if ([tv.string containsString:@"A --> B"]) return NO;
        NSRange broken = [tv.string rangeOfString:@"broken -->"];
        if (broken.location == NSNotFound) return NO;
        NSString *tip = [tv.textStorage attribute:NSToolTipAttributeName atIndex:broken.location
                                   effectiveRange:nil];
        return tip.length > 0;
    });

    MCGuiStep(@"a picture at its size in points, editable from its line; the broken one is code with mmdc's words", ^{
        NSTextView *tv = [MCGuiController() valueForKey:@"textView"];
        NSTextStorage *st = tv.textStorage;
        __block MCMarkdownImage *pic = nil;
        __block NSUInteger at = NSNotFound;
        [st enumerateAttribute:NSAttachmentAttributeName inRange:NSMakeRange(0, st.length) options:0
                    usingBlock:^(id value, NSRange range, BOOL *stop) {
            if ([value isKindOfClass:MCMarkdownImage.class]) { pic = value; at = range.location; *stop = YES; }
        }];
        GCHECK(pic != nil);
        GCHECK(NSEqualSizes(pic.picture.size, NSMakeSize(120, 60)));
        if (at != NSNotFound) {
            GCHECK([[st attribute:@"MCMarkdownSourceLine" atIndex:at effectiveRange:nil] intValue] == 5);
        }
        NSRange broken = [tv.string rangeOfString:@"broken -->"];
        GCHECK(broken.location != NSNotFound);
        if (broken.location != NSNotFound) {
            NSString *tip = [st attribute:NSToolTipAttributeName atIndex:broken.location effectiveRange:nil];
            GCHECK([tip hasPrefix:@"Parse error on line 2:"] && ![tip containsString:@"x.invalid"]);
        }
        NSString *runs = [NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nil];
        NSArray *lines = [runs componentsSeparatedByString:@"\n"];
        GCHECK(lines.count == 3);   // two runs and the last line break
        GCHECK([runs containsString:@"-b transparent -s 2 -q"]);
        GCHECK([runs containsString:@"-t dark"]);   // the default theme is dark
        MCGuiSnapshot(tv, @"mermaid-preview");
    });

    MCGuiStep(@"opened again, the diagram comes from the cache with no run", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:MCGuiProjectFile(@"diagrams.md")]);
        [c togglePreview:nil];
        [c togglePreview:nil];
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK(![tv.string containsString:@"A --> B"]);
        NSString *runs = [NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nil];
        GCHECK([runs componentsSeparatedByString:@"\n"].count == 3);
        NSArray *cached = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:MCMermaidCacheDirectory() error:nil];
        GCHECK(cached.count == 1);
    });

    MCGuiStep(@"markdown.mermaid = false shows the source", ^{
        MCGuiSetSettings([settingsBefore stringByAppendingString:@"\nmarkdown.mermaid = false\n"]);
        EditorController *c = MCGuiController();
        [c togglePreview:nil];
        [c togglePreview:nil];
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK([tv.string containsString:@"A --> B"]);
    });

    MCGuiStep(@"no mmdc: the source, and nothing run", ^{
        MCGuiSetSettings(settingsBefore);
        setenv("MINICODE_MMDC", "/nonexistent/mmdc", 1);
        GCHECK(MCMermaidPath() == nil);
        MCGuiWriteFile(MCGuiProjectFile(@"other.md"), @"```mermaid\nsequenceDiagram\n  A->>B: hi\n```\n");
        EditorController *c = MCGuiController();
        GCHECK([c showFile:MCGuiProjectFile(@"other.md")]);
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK([tv.string containsString:@"A->>B: hi"]);
        unsetenv("MINICODE_MMDC");
    });

    // With MC_GUI_REAL_MMDC naming a real mmdc (a wrapper that adds -p for a
    // browser, say), a real diagram is drawn and pictured, for looking at.
    NSString *real = NSProcessInfo.processInfo.environment[@"MC_GUI_REAL_MMDC"];
    if (real.length) {
        MCGuiStep(@"a real mmdc draws a real diagram", ^{
            setenv("MINICODE_MMDC", real.fileSystemRepresentation, 1);
            MCGuiWriteFile(MCGuiProjectFile(@"real.md"),
                           @"# A real diagram\n\nText before it.\n\n```mermaid\nflowchart LR\n"
                           @"  A[Start] --> B{Is it?}\n  B -->|Yes| C[OK]\n  B -->|No| D[Not OK]\n```\n\n"
                           @"```mermaid\nsequenceDiagram\n  Alice->>Bob: Hello\n  Bob-->>Alice: Hi\n```\n\nText after it.\n");
            GCHECK([MCGuiController() showFile:MCGuiProjectFile(@"real.md")]);
        });
        MCGuiWait(@"both diagrams arrive", 60, ^BOOL {
            NSTextView *tv = [MCGuiController() valueForKey:@"textView"];
            return ![tv.string containsString:@"Is it?"] && ![tv.string containsString:@"Alice->>Bob"];
        });
        MCGuiStep(@"a picture of the window", ^{
            MCGuiSnapshot(MCGuiController().window.contentView, @"mermaid-real");
            unsetenv("MINICODE_MMDC");
        });
    }
    MCGuiStep(@"done with the stand-in", ^{
        unsetenv("MINICODE_MERMAID_CACHE");
        [MCGuiController() closeAllTabs:nil];
    });
}
