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
// window's, and an edit typed in the window reaches it. Closing the window
// stops the server. No browser window opens.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "EditorTab.h"
#import "SessionBrowser.h"
#import "SessionLink.h"

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
    void show(long long doc) {
        const std::string text = "{\"t\":\"show\",\"doc\":" + std::to_string(doc) + ",\"term\":0}";
        WriteAll(fd, IdeLink::encodeClientFrame(IdeLink::Opcode::Text, text, 0x5a17c0de));
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

EditorController *gC = nil;
Browser *gBrowser = nil;
NSURL *gOpened = nil;
NSString *gUrl = nil;
int gPort = 0;
std::string gToken;
NSString *gFile = nil;
// What the plain requests got (written on their thread before `done`).
std::string page, noToken, wrongToken, otherHost, wsWrong;

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
        if (!MCSessionHostEnabled()) MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);   // Turn On
        [gC showSessionInBrowser:nil];
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
    MCGuiStep(@"the browser leaves; the window closes, and the server with it", ^{
        GCHECK(gBrowser->text().find("# typed in minicode") == 0);
        gBrowser->finish();
        delete gBrowser;
        gBrowser = nullptr;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Don't Save, if asked
        [gC.window performClose:nil];
    });
    MCGuiWait(@"gone", 10, ^BOOL { return ![MCGuiControllers() containsObject:gC]; });
    MCGuiStep(@"nothing listens on the port any more", ^{
        const int fd = Connect(gPort);
        GCHECK(fd < 0);
        if (fd >= 0) close(fd);
        MCSessionBrowserOpener = nil;
        gC = nil;
        gOpened = nil;
        MCGuiSetKeyWindow(nil);
    });
}
