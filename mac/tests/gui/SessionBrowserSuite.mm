// SessionBrowserSuite.mm — `make guitest SUITES=session_browser`: View >
// Show Session in Browser… (SessionBrowser.h) serving a window's tabs from
// the app's own session host. With editor.session-host off the item asks
// first, and Cancel starts nothing; Turn On writes the setting and starts
// the window's server on 127.0.0.1. What would open the default browser is
// caught (MCSessionBrowserOpener; the harness's openURL: opens nothing
// anyway), and Copy Session Browser Link copies the same address. A
// scripted browser on a thread of its own, over a plain socket, gets the
// page with the token and is refused without it, with a wrong one and with
// another Host; its WebSocket gets the window's document, only that
// window's, and an edit typed in the window reaches it. Then the page itself,
// in a WKWebView that is in no window, as Safari loads it: key events
// dispatched into it type into the window's file (Return taking the core's
// indentation), typing in the window reaches the page, Cmd+Z in the page is
// the host's undo, Cmd+S in it saves the file through the window, and the
// page's tab shows the file's dirty dot until then; page and window end on
// one text. The page's tree lists the window's folder through the host: a
// click on a folder opens it, a click on a file in it opens that file in
// the window's tab, which the page then shows, and a picture says it opens
// in the window. Two windows, each with a terminal on the host and each
// with its page: each page lists only its own window's terminal, a crafted
// show or keys for the other window's terminal id reach nothing, keys for
// its own run there, and a terminal the window closes leaves its page.
// Closing the window stops the server. No browser window opens.
#import "Harness.h"
#import <WebKit/WebKit.h>
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "SessionBrowser.h"
#import "SessionLink.h"
#import "Terminal.h"
#import "TerminalPanel.h"

#include <arpa/inet.h>
#include <atomic>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "IdeLink.h"
#include "Json.h"

namespace {

int Connect(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool WriteAll(int fd, const std::string &s) {
    std::size_t at = 0;
    while (at < s.size()) {
        const ssize_t n = write(fd, s.data() + at, s.size() - at);
        if (n <= 0) return false;
        at += (std::size_t)n;
    }
    return true;
}

// One HTTP exchange that the server ends by closing (any thread).
std::string Exchange(int port, const std::string &request) {
    const int fd = Connect(port);
    if (fd < 0) return "";
    std::string in;
    if (WriteAll(fd, request)) {
        char buf[65536];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) in.append(buf, (std::size_t)n);
    }
    close(fd);
    return in;
}

std::string Get(int port, const std::string &target, const std::string &host) {
    return Exchange(port, "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
}

// A browser's socket, scripted on a thread of its own: the upgrade, then
// every message, keeping the hello, the newest state and the document's
// lines as the page splices them.
struct Browser {
    int port = 0;
    std::string token;
    int fd = -1;
    std::thread thread;
    std::atomic<bool> stop{false}, upgraded{false}, ended{false};
    std::mutex mu;
    Json hello, state;
    long long docId = 0;
    std::vector<std::string> lines;
    int gone = 0;

    void start() {
        fd = Connect(port);
        thread = std::thread([this] { run(); });
    }
    void show(long long doc, long long term = 0) {
        send("{\"t\":\"show\",\"doc\":" + std::to_string(doc) + ",\"term\":" + std::to_string(term) + "}");
    }
    void send(const std::string &text) {
        WriteAll(fd, IdeLink::encodeClientFrame(IdeLink::Opcode::Text, text, 0x5a17c0de));
    }
    // The terminals the newest state lists, "id id ".
    std::string terms() {
        std::lock_guard<std::mutex> lk(mu);
        std::string out;
        for (const Json &t : state["terms"].elements()) out += std::to_string(t["id"].asInt()) + " ";
        return out;
    }
    void finish() {
        stop = true;
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        if (thread.joinable()) thread.join();
        if (fd >= 0) close(fd);
        fd = -1;
    }
    std::string text() {
        std::lock_guard<std::mutex> lk(mu);
        std::string s;
        for (std::size_t i = 0; i < lines.size(); i++) s += (i ? "\n" : "") + lines[i];
        return s;
    }
    void take(const Json &m) {
        std::lock_guard<std::mutex> lk(mu);
        const std::string t = m["t"].asString();
        if (t == "hello") hello = m;
        else if (t == "state") state = m;
        else if (t == "gone") gone++;
        else if (t == "doc") {
            docId = m["id"].asInt();
            lines.clear();
            for (const Json &l : m["lines"].elements()) lines.push_back(l[0].asString());
        } else if (t == "lines" && m["id"].asInt() == docId) {
            if (m.has("all")) {
                lines.clear();
                for (const Json &l : m["all"].elements()) lines.push_back(l[0].asString());
            }
            for (const Json &e : m["edits"].elements()) {
                const std::size_t at = (std::size_t)e[0].asInt(), del = (std::size_t)e[1].asInt();
                if (at > lines.size()) return;
                lines.erase(lines.begin() + (long)at,
                            lines.begin() + (long)std::min(lines.size(), at + del));
                std::vector<std::string> add;
                for (const Json &l : e[2].elements()) add.push_back(l[0].asString());
                lines.insert(lines.begin() + (long)at, add.begin(), add.end());
            }
        }
    }
    void run() {
        if (fd < 0) return;
        const std::string req = "GET /ws?t=" + token + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                                std::to_string(port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1:" +
                                std::to_string(port) + "\r\n\r\n";
        if (!WriteAll(fd, req)) return;
        std::string in;
        char buf[65536];
        while (!stop) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                break;
            }
            in.append(buf, (std::size_t)n);
            if (!upgraded) {
                const std::size_t e = in.find("\r\n\r\n");
                if (e == std::string::npos) continue;
                if (in.compare(0, 12, "HTTP/1.1 101") != 0) break;
                in.erase(0, e + 4);
                upgraded = true;
            }
            for (;;) {   // whole frames (the server's are never masked)
                if (in.size() < 2) break;
                const unsigned char b0 = (unsigned char)in[0], b1 = (unsigned char)in[1];
                std::size_t len = b1 & 0x7f, at = 2;
                if (len == 126) {
                    if (in.size() < 4) break;
                    len = ((unsigned char)in[2] << 8) | (unsigned char)in[3];
                    at = 4;
                } else if (len == 127) {
                    if (in.size() < 10) break;
                    len = 0;
                    for (int i = 2; i < 10; i++) len = (len << 8) | (unsigned char)in[i];
                    at = 10;
                }
                if (in.size() < at + len) break;
                const std::string payload = in.substr(at, len);
                in.erase(0, at + len);
                Json j;
                if ((b0 & 0x0f) == 1 && Json::parse(payload, j)) take(j);
            }
        }
        ended = true;
    }
};

EditorController *gC = nil, *gC2 = nil;
TerminalView *gTermA = nil, *gTermB = nil;
Browser *gBrowser = nil, *gBrowser2 = nil;
NSURL *gOpened = nil;
NSString *gUrl = nil;
int gPort = 0;
std::string gToken;
NSString *gFile = nil;
// What the plain requests got (written on their thread before `done`).
std::string page, noToken, wrongToken, otherHost, wsWrong;

WKWebView *gPage = nil;
NSDictionary *gSeen = nil;   // the page as last read

// What the page shows, read in its own world (window.mcSessionView).
NSString *const kRead =
    @"JSON.stringify({status: document.getElementById('status').textContent,"
    @" text: window.mcSessionView ? window.mcSessionView.text() : null,"
    @" synced: window.mcSessionView ? window.mcSessionView.synced() : false,"
    @" lines: [...document.querySelectorAll('#code .l')].map(l => l.textContent).join('\\n'),"
    @" dirty: !!document.querySelector('#tabs button.shown.dirty'),"
    @" shown: !!document.querySelector('#tabs button.shown'),"
    @" tree: window.mcSessionView ? window.mcSessionView.tree().join('|') : '',"
    @" note: window.mcSessionView ? window.mcSessionView.treeNote() : ''})";

// A click on the page's tree row for `rel`.
void ClickRow(NSString *rel) {
    NSData *quoted = [NSJSONSerialization dataWithJSONObject:@[ rel ] options:0 error:nil];
    NSString *js = [NSString stringWithFormat:
        @"(() => { const want = %@[0];"
        @" const r = [...document.querySelectorAll('#tree .row')].find(x => x.dataset.path === want);"
        @" if (r) r.click(); return !!r; })()",
        [[NSString alloc] initWithData:quoted encoding:NSUTF8StringEncoding]];
    [gPage evaluateJavaScript:js inFrame:nil inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id value, NSError *error) {
                if (error || ![value boolValue]) MCGuiNote(@"no tree row %@ to click", rel);
            }];
}

void Read() {
    [gPage evaluateJavaScript:kRead inFrame:nil inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id value, NSError *error) {
                if (![value isKindOfClass:NSString.class]) return;
                NSData *d = [(NSString *)value dataUsingEncoding:NSUTF8StringEncoding];
                id j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
                if ([j isKindOfClass:NSDictionary.class]) gSeen = j;
                (void)error;
            }];
}

// Keys for the page's editor: keydown events dispatched at its field, as a
// browser sends them (`keys` a JavaScript array of [key, options]).
void Keys(NSString *keys) {
    NSString *js = [NSString stringWithFormat:
        @"(() => { const f = document.getElementById('input');"
        @" for (const [k, o] of %@) f.dispatchEvent(new KeyboardEvent('keydown',"
        @" Object.assign({key: k, bubbles: true, cancelable: true}, o || {}))); return 1; })()", keys];
    [gPage evaluateJavaScript:js inFrame:nil inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id, NSError *error) {
                if (error) MCGuiNote(@"keys: %@", error.localizedDescription);
            }];
}

// A string's characters as [key] entries.
NSString *Typed(NSString *s) {
    NSMutableArray *parts = [NSMutableArray array];
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length) options:NSStringEnumerationByComposedCharacterSequences
                       usingBlock:^(NSString *c, NSRange, NSRange, BOOL *) {
                           NSData *d = [NSJSONSerialization dataWithJSONObject:@[ c ] options:0 error:nil];
                           [parts addObject:[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]];
                       }];
    return [parts componentsJoinedByString:@","];
}

NSTextView *TV(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }
std::string Text(EditorController *c) { return std::string(TV(c).string.UTF8String ?: ""); }

}  // namespace

MC_GUI_SUITE(session_browser) {
    NSString *dir = [MCGuiScratch() stringByAppendingPathComponent:@"session-browser"];
    MCGuiStep(@"a window on a folder, and nothing that opens a browser", ^{
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
        gFile = [dir stringByAppendingPathComponent:@"calc.py"];
        MCGuiWriteFile(gFile, @"def total(xs):\n    return sum(xs)\n");
        [NSFileManager.defaultManager createDirectoryAtPath:[dir stringByAppendingPathComponent:@"pkg"]
                                withIntermediateDirectories:YES attributes:nil error:nil];
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"pkg/util.py"], @"def helper():\n    return 42\n");
        // A real 1x1 picture, so the window's viewer has something to show.
        NSData *png = [[NSData alloc] initWithBase64EncodedString:
            @"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
                                                         options:0];
        [png writeToFile:[dir stringByAppendingPathComponent:@"logo.png"] atomically:YES];
        MCSessionBrowserOpener = ^(NSURL *url) { gOpened = url; };
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:dir]]];
        GCHECK(MCGuiControllers().count == before + 1);
        gC = MCGuiController();
        MCGuiSetKeyWindow(gC.window);
    });
    MCGuiStep(@"with the setting off the item asks, and Cancel starts nothing", ^{
        if (MCSessionHostEnabled()) {
            MCGuiNote(@"the session host is on for the whole run: the question is not asked");
            return;
        }
        const NSUInteger asked = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Cancel
        [gC showSessionInBrowser:nil];
        GCHECK(MCGuiAlertsShown().count == asked + 1);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"The session host is off");
        GCHECK(gC.sessionBrowser == nil);
        GCHECK(gOpened == nil);
        GCHECK(!MCSessionHostEnabled());
    });
    MCGuiStep(@"Turn On writes the setting, and the page's address goes to the browser", ^{
        // Turn On (when asked), then Editable, the first choice until one is made.
        if (!MCSessionHostEnabled())
            MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn), @(NSAlertFirstButtonReturn)]);
        else MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [gC showSessionInBrowser:nil];
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"Show this window's session in the browser");
        GCHECK(gC.sessionBrowser.editable);
        GCHECK(MCSessionHostEnabled());
        GCHECK(gC.sessionBrowser != nil);
        gUrl = gC.sessionBrowser.url;
        gPort = gC.sessionBrowser.port;
        MCGuiNote(@"opened: %@", gOpened.absoluteString);
        GCHECK(gPort > 0);
        GCHECK_EQ_STR(gOpened.absoluteString, gUrl);
        NSString *prefix = [NSString stringWithFormat:@"http://127.0.0.1:%d/?t=", gPort];
        GCHECK([gUrl hasPrefix:prefix]);
        NSString *token = [gUrl componentsSeparatedByString:@"?t="].lastObject;
        GCHECK(token.length == 48);
        gToken = token.UTF8String ?: "";
    });
    MCGuiStep(@"a file opened now has a document on the host; the link copies the same address", ^{
        [gC openFileAtPath:gFile];
        GCHECK(Active(gC).session != nil);
        [gC copySessionBrowserLink:nil];
        GCHECK_EQ_STR([NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString], gUrl);
        GCHECK(gC.sessionBrowser.port == gPort);   // one server for the window
        [gC.sessionBrowser reportTabs];
    });
    MCGuiAsync(@"plain requests: the page with its token, refusals without", 20, ^(MCGuiDone done) {
        const int port = gPort;
        const std::string token = gToken;
        std::thread([=] {
            page = Get(port, "/?t=" + token, "127.0.0.1:" + std::to_string(port));
            noToken = Get(port, "/", "127.0.0.1:" + std::to_string(port));
            wrongToken = Get(port, "/?t=" + std::string(48, '0'), "127.0.0.1:" + std::to_string(port));
            otherHost = Get(port, "/?t=" + token, "attacker.example");
            wsWrong = Exchange(port, "GET /ws?t=wrong HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                                     "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\n\r\n");
            dispatch_async(dispatch_get_main_queue(), ^{ done(); });
        }).detach();
    });
    MCGuiStep(@"what each request got", ^{
        GCHECK(page.compare(0, 12, "HTTP/1.1 200") == 0);
        GCHECK(page.find("<!DOCTYPE html>") != std::string::npos);
        GCHECK(noToken.compare(0, 12, "HTTP/1.1 403") == 0 && noToken.find("<html") == std::string::npos);
        GCHECK(wrongToken.compare(0, 12, "HTTP/1.1 403") == 0);
        GCHECK(otherHost.compare(0, 12, "HTTP/1.1 403") == 0);
        GCHECK(wsWrong.compare(0, 12, "HTTP/1.1 403") == 0);
        GCHECK(gC.sessionBrowser.browsers == 0);
        gBrowser = new Browser();
        gBrowser->port = gPort;
        gBrowser->token = gToken;
        gBrowser->start();
    });
    MCGuiWait(@"the browser's socket gets the window's tabs", 20, ^BOOL {
        std::lock_guard<std::mutex> lk(gBrowser->mu);
        return gBrowser->state.isObject() && gBrowser->state["docs"].size() > 0;
    });
    __block long long docId = 0;
    MCGuiStep(@"the hello, and the state: the window's one document, no terminals", ^{
        Json hello, state;
        {
            std::lock_guard<std::mutex> lk(gBrowser->mu);
            hello = gBrowser->hello;
            state = gBrowser->state;
        }
        MCGuiNote(@"state: %s", state.dump().c_str());
        GCHECK(hello["termNote"].asString() == std::string(MCSessionBrowserTermNote.UTF8String));
        GCHECK(state["docs"].size() == 1);
        GCHECK(state["terms"].size() == 0);
        docId = state["docs"][0]["id"].asInt();
        GCHECK(docId == (long long)Active(gC).session.docId);
        GCHECK(state["docs"][0]["path"].asString().find("calc.py") != std::string::npos);
        GCHECK(gC.sessionBrowser.browsers == 1);
        gBrowser->show(docId);
    });
    MCGuiWait(@"the page's lines are the window's text", 20, ^BOOL {
        return gBrowser->docId == docId && gBrowser->text() == Text(gC);
    });
    MCGuiStep(@"typing in the window", ^{
        NSTextView *tv = TV(gC);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"# typed in minicode\n" replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK([tv.string hasPrefix:@"# typed in minicode\n"]);
    });
    MCGuiWait(@"reaches the browser", 20, ^BOOL { return gBrowser->text() == Text(gC); });
    MCGuiStep(@"the scripted browser leaves, and the page loads in WebKit", ^{
        GCHECK(gBrowser->text().find("# typed in minicode") == 0);
        gBrowser->finish();
        delete gBrowser;
        gBrowser = nullptr;
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
        gPage = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 900, 700) configuration:config];
        [gPage loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:gUrl]]];
    });
    MCGuiWait(@"the page shows the window's file, editable", 30, ^BOOL {
        Read();
        return [gSeen[@"status"] isEqualToString:@"live, editing"] &&
               [gSeen[@"text"] isEqualToString:TV(gC).string];
    });
    MCGuiStep(@"keys typed into the page", ^{
        GCHECK_EQ_STR(gSeen[@"lines"], TV(gC).string);
        GCHECK(!MCSessionHostEnabled() || [gSeen[@"shown"] boolValue]);
        // Command Down to the end, a line, Return.
        Keys([NSString stringWithFormat:@"[[\"ArrowDown\", {metaKey: true}], %@, [\"Enter\"]]",
                                        Typed(@"# from the browser")]);
    });
    MCGuiWait(@"reach the window's file", 20, ^BOOL {
        return [TV(gC).string hasSuffix:@"# from the browser\n"];
    });
    MCGuiWait(@"and the page and the window agree, the host with them", 20, ^BOOL {
        Read();
        return [gSeen[@"synced"] boolValue] && [gSeen[@"text"] isEqualToString:TV(gC).string];
    });
    MCGuiStep(@"what the host has; then typing in the window", ^{
        NSString *diff = MCSessionCheckTabs(@[ Active(gC) ]);
        if (diff.length) MCGuiNote(@"host: %@", diff);
        GCHECK(diff.length == 0);
        GCHECK(Active(gC).dirty);
        NSTextView *tv = TV(gC);
        MCGuiUndoGroup(tv.undoManager, ^{
            [tv insertText:@"# from minicode\n" replacementRange:NSMakeRange(0, 0)];
        });
    });
    MCGuiWait(@"reaches the page, which shows the file's dirty dot", 20, ^BOOL {
        Read();
        return [gSeen[@"text"] isEqualToString:TV(gC).string] && [gSeen[@"dirty"] boolValue];
    });
    MCGuiStep(@"Return in the page takes the core's indentation", ^{
        GCHECK([gSeen[@"lines"] hasPrefix:@"# from minicode\n"]);
        Keys([NSString stringWithFormat:@"[[\"ArrowDown\", {metaKey: true}], %@, [\"Enter\"], %@]",
                                        Typed(@"def g():"), Typed(@"pass")]);
    });
    MCGuiWait(@"in the window's file", 20, ^BOOL {
        return [TV(gC).string hasSuffix:@"def g():\n    pass"];
    });
    MCGuiWait(@"and the page", 20, ^BOOL {
        Read();
        return [gSeen[@"synced"] boolValue] && [gSeen[@"text"] isEqualToString:TV(gC).string];
    });
    MCGuiStep(@"Command Z in the page: the host's undo", ^{
        GCHECK(MCSessionCheckTabs(@[ Active(gC) ]).length == 0);
        Keys(@"[[\"z\", {metaKey: true}]]");
    });
    MCGuiWait(@"takes the last typing back in the window", 20, ^BOOL {
        return ![TV(gC).string containsString:@"pass"];
    });
    MCGuiWait(@"and the page", 20, ^BOOL {
        Read();
        return [gSeen[@"synced"] boolValue] && [gSeen[@"text"] isEqualToString:TV(gC).string];
    });
    MCGuiStep(@"Command S in the page", ^{
        GCHECK([TV(gC).string containsString:@"# from the browser\n"]);
        Keys(@"[[\"s\", {metaKey: true}]]");
    });
    MCGuiWait(@"saves the file through the window", 20, ^BOOL {
        NSString *disk = [NSString stringWithContentsOfFile:gFile encoding:NSUTF8StringEncoding error:nil];
        return [disk isEqualToString:TV(gC).string] && !Active(gC).dirty;
    });
    MCGuiWait(@"and the page's dot goes", 20, ^BOOL {
        Read();
        return gSeen[@"dirty"] && ![gSeen[@"dirty"] boolValue];
    });
    MCGuiStep(@"page, window, host and disk on one text", ^{
        NSString *disk = [NSString stringWithContentsOfFile:gFile encoding:NSUTF8StringEncoding error:nil];
        GCHECK_EQ_STR(gSeen[@"text"], TV(gC).string);
        GCHECK_EQ_STR(disk, TV(gC).string);
        GCHECK(MCSessionCheckTabs(@[ Active(gC) ]).length == 0);
    });
    MCGuiWait(@"the page's tree lists the window's folder, folders first", 20, ^BOOL {
        Read();
        return [gSeen[@"tree"] isEqualToString:@"pkg|calc.py|logo.png"];
    });
    MCGuiStep(@"a click on the folder in the page's tree", ^{ ClickRow(@"pkg"); });
    MCGuiWait(@"opens it, read by the host", 20, ^BOOL {
        Read();
        return [gSeen[@"tree"] isEqualToString:@"pkg|pkg/util.py|calc.py|logo.png"];
    });
    MCGuiStep(@"a click on a file in it", ^{ ClickRow(@"pkg/util.py"); });
    MCGuiWait(@"opens the file in the window's tab", 20, ^BOOL {
        return [Active(gC).currentPath hasSuffix:@"/pkg/util.py"] && Active(gC).session != nil;
    });
    MCGuiWait(@"and the page shows that tab", 20, ^BOOL {
        Read();
        return [gSeen[@"text"] isEqualToString:TV(gC).string] && [TV(gC).string hasPrefix:@"def helper"];
    });
    MCGuiStep(@"a picture in the page's tree", ^{
        GCHECK(MCSessionCheckTabs(@[ Active(gC) ]).length == 0);
        ClickRow(@"logo.png");
    });
    MCGuiWait(@"says it opens in the window", 20, ^BOOL {
        Read();
        return [gSeen[@"note"] isEqualToString:@"logo.png opens in the minicode window."];
    });
    MCGuiStep(@"a terminal in this window, and a second window with a terminal of its own", ^{
        GCHECK(MCSessionHostEnabled());
        [gC newTerminal:nil];
        gTermA = [gC valueForKey:@"terminal"];
        GCHECK(gTermA.onSessionHost && gTermA.sessionTerminal != 0);
        NSString *dir2 = [MCGuiScratch() stringByAppendingPathComponent:@"session-browser-2"];
        [NSFileManager.defaultManager createDirectoryAtPath:dir2 withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
        NSUInteger before = MCGuiControllers().count;
        [(id<NSApplicationDelegate>)NSApp.delegate application:NSApp
                                                      openURLs:@[[NSURL fileURLWithPath:dir2]]];
        GCHECK(MCGuiControllers().count == before + 1);
        for (EditorController *c in MCGuiControllers())
            if (c != gC && [c.rootPath.lastPathComponent isEqualToString:@"session-browser-2"]) gC2 = c;
        GCHECK(gC2 != nil);
        [gC2 newTerminal:nil];
        gTermB = [gC2 valueForKey:@"terminal"];
        GCHECK(gTermB.onSessionHost && gTermB.sessionTerminal != 0);
        GCHECK(gTermB.sessionTerminal != gTermA.sessionTerminal);
        // The second window's page too (Copy starts it, editable as last chosen).
        [gC2 copySessionBrowserLink:nil];
        GCHECK(gC2.sessionBrowser != nil && gC2.sessionBrowser.port != gPort);
        MCGuiSetKeyWindow(gC.window);
    });
    MCGuiWait(@"both shells at their prompts", 20, ^BOOL {
        return gTermA.atPrompt && !gTermA.busy && gTermB.atPrompt && !gTermB.busy;
    });
    MCGuiStep(@"a scripted browser on each window's page", ^{
        gBrowser = new Browser();
        gBrowser->port = gPort;
        gBrowser->token = gToken;
        gBrowser->start();
        NSString *url2 = gC2.sessionBrowser.url;
        gBrowser2 = new Browser();
        gBrowser2->port = gC2.sessionBrowser.port;
        gBrowser2->token = [url2 componentsSeparatedByString:@"?t="].lastObject.UTF8String ?: "";
        gBrowser2->start();
    });
    MCGuiWait(@"each page lists its own window's terminal and not the other's", 20, ^BOOL {
        return gBrowser->terms() == std::to_string(gTermA.sessionTerminal) + " " &&
               gBrowser2->terms() == std::to_string(gTermB.sessionTerminal) + " ";
    });
    MCGuiStep(@"keys for the other window's terminal, asked for by id, then for its own", ^{
        MCGuiNote(@"this page: %s, the other: %s", gBrowser->terms().c_str(), gBrowser2->terms().c_str());
        const std::string b = std::to_string(gTermB.sessionTerminal);
        const std::string a = std::to_string(gTermA.sessionTerminal);
        gBrowser->show(0, (long long)gTermB.sessionTerminal);
        gBrowser->send("{\"t\":\"ttext\",\"term\":" + b + ",\"text\":\"echo pwned-$((1+1))\",\"mods\":0}");
        gBrowser->send("{\"t\":\"tkey\",\"term\":" + b + ",\"key\":\"Enter\",\"mods\":0}");
        gBrowser->send("{\"t\":\"tpaste\",\"term\":" + b + ",\"text\":\"echo pasted-$((2+2))\\n\"}");
        // The server takes a page's messages in order: these come after.
        gBrowser->show(0, (long long)gTermA.sessionTerminal);
        gBrowser->send("{\"t\":\"ttext\",\"term\":" + a + ",\"text\":\"echo page-$((6*7))\",\"mods\":0}");
        gBrowser->send("{\"t\":\"tkey\",\"term\":" + a + ",\"key\":\"Enter\",\"mods\":0}");
    });
    MCGuiWait(@"its own terminal runs what the page typed", 20, ^BOOL {
        return [[gTermA recentText:20] containsString:@"\npage-42"] && gTermA.atPrompt && !gTermA.busy;
    });
    MCGuiStep(@"and a command typed in the other window's terminal", ^{
        [gTermB runCommand:@"echo b-side-$((5*5))"];
    });
    MCGuiWait(@"runs there", 20, ^BOOL {
        return [[gTermB recentText:20] containsString:@"\nb-side-25"] && gTermB.atPrompt && !gTermB.busy;
    });
    MCGuiStep(@"nothing the first page sent reached the other window's terminal", ^{
        NSString *seen = [gTermB recentText:40];
        const std::string host = MCSessionInspectTerminal(gTermB.sessionTerminal).screen;
        if ([seen containsString:@"pwned"] || [seen containsString:@"pasted"]) MCGuiNote(@"B: %@", seen);
        GCHECK(![seen containsString:@"pwned"] && ![seen containsString:@"pasted"]);
        GCHECK(host.find("pwned") == std::string::npos && host.find("pasted") == std::string::npos);
        GCHECK(gBrowser2->terms() == std::to_string(gTermB.sessionTerminal) + " ");
        GCHECK([(MCTerminalPanel *)[gC valueForKey:@"terminalPanel"] closeTerminal:gTermA]);
        gTermA = nil;
    });
    MCGuiWait(@"the terminal this window closed leaves its page", 20, ^BOOL {
        return gBrowser->terms().empty();
    });
    MCGuiStep(@"the second window closes its terminal and itself", ^{
        GCHECK(gBrowser2->terms() == std::to_string(gTermB.sessionTerminal) + " ");
        gBrowser->finish();
        delete gBrowser;
        gBrowser = nullptr;
        gBrowser2->finish();
        delete gBrowser2;
        gBrowser2 = nullptr;
        GCHECK([(MCTerminalPanel *)[gC2 valueForKey:@"terminalPanel"] closeTerminal:gTermB]);
        gTermB = nil;
        [gC2.window performClose:nil];
    });
    MCGuiWait(@"the second window is gone", 10, ^BOOL { return ![MCGuiControllers() containsObject:gC2]; });
    MCGuiStep(@"the window closes, and the server with it", ^{
        [gPage stopLoading];
        gPage = nil;
        gSeen = nil;
        GCHECK(!Active(gC).dirty);   // saved from the page: nothing to ask on closing
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return ![MCGuiControllers() containsObject:gC]; });
    MCGuiStep(@"nothing listens on the port any more", ^{
        const int fd = Connect(gPort);
        GCHECK(fd < 0);
        if (fd >= 0) close(fd);
        MCSessionBrowserOpener = nil;
        gC = nil;
        gC2 = nil;
        gOpened = nil;
        MCGuiSetKeyWindow(nil);
    });
}
