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
// the window's tab, which the page then shows; a picture shows in the page
// itself, its bytes read through the host and served at the page's /file/
// address, and a CSV as the table the core parsed, while the window opens
// each too. The page's address field asks the window's browser pane
// to open an address: javascript: is refused, an http address on this Mac
// opens in the pane, and the page shows the window's browser tab in a pane
// of its own beside the editor and frames it, with its new-tab link and
// the line that a loopback address is this machine's. The page's close
// button hides that pane on the page alone; the window hiding its pane
// hides it there too, and showing it shows it again. A tab's close button
// on the editable page closes the window's tab. Two windows, each with a
// terminal on the host and each with its page: each page lists only its
// own window's terminal, a crafted show or keys for the other window's
// terminal id reach nothing, keys for its own run there, the window hiding
// its terminal panel hides the page's terminal pane, and a terminal the
// window closes leaves its page. Then the page again, read-only: a file
// clicked in its tree opens in a tab of the page's own and not in the
// window, the window's tabs have no close button, and the page's tab
// closes on the page. Closing the window stops the server. No browser
// window opens. On the read-only page, a notebook clicked in the tree shows
// as the page the core rendered, in a frame sandboxed with no scripts (its
// hostile cells escaped, nothing run), and a SQLite database as its tables
// (hostile names as text) and its rows a page at a time, Next going on,
// the file unchanged; and a Markdown file as its page, Show Source flipping
// to its text and Show Page back. A LaTeX document (with a stand-in
// tectonic): the window opens it in its preview, and the read-only page,
// following the window, shows the PDF the window's preview typeset as one
// picture a page (a PNG drawn by the app at its /typeset-page/ address),
// the bytes at its /typeset/ address the window's own; Show Source shows
// its text; the page scrolled to its third page, an agent's rewrite that
// the window reads and typesets again (taller pages) reaches the page as a
// new PDF at a new address, and the page is still on its third page, as
// far into it; a document the window
// does not have open, clicked in the page's tree, is typeset by the
// server's own preview, the window opening nothing and no file left
// beside it. Source Control (R3's fourth slice): the folder made a
// repository with changes, the read-only page's Source Control lists them
// as the window's does, a file clicked shows the review page of the
// changes in a tab of the page's own (removed and added lines, the changed
// words marked, syntax colors, no Stage or Discard anywhere), Side by Side
// and an unchanged stretch opened; an agent's edit on disk reaches the page
// with no reload (the window's FSEvents), and the repository is as it was.
// Then the window's commit page and an agent worktree's page (R3's last
// slice): both listed in the read-only page's tab strip, each shown there
// read-only (only Side by Side; no Apply to Main), a picture's Before and
// After decoded from the server's /picture/ addresses at their pixel
// sizes, the window opening nothing, and each page's tab going when the
// window closes it. Quick Open (stage 7's heavier reads, on the host's
// workers): Command P in the editable page lists the folder's files as the
// host listed them, a query ranks them by the palette's matching with the
// matched letters marked, and Return opens the file in the window, which
// the page then shows. Then the page's writes (stage 7): New File makes an
// untitled tab in the window, which the page shows and types into; Shift
// Command S opens Save As on its name, completes a folder's files from the
// host's listing, asks before writing over a file there (Cancel writes
// nothing), and saves it as made.py through the host, the window's tab that
// file; Find in Folder in the editable page shows each match struck
// through beside its replacement and a box per row, Return in the replace
// field only searches again, and Replace All (asked first, one row left
// out) edits the window's open file as one step on the host and rewrites
// the other file, and Undo in the window takes the edit back. Shift Command
// F in the read-only page searches the
// folder on the host, its rows grouped by file with a line of context
// either side and the matches marked, and a row clicked opens its file in
// a tab of the page's own at that line, the window opening nothing.
#import "Harness.h"
#import <WebKit/WebKit.h>
#import "EditorController.h"
#import "EditorController+Private.h"
#import "ChangeReviewPage.h"
#import "EditorTab.h"
#import "GitPanel.h"   // MCGitExecutable, MCGitRunSync
#import "SessionBrowser.h"
#import "SessionLink.h"
#import "Terminal.h"
#import "TerminalPanel.h"

#include <arpa/inet.h>
#include <atomic>
#include <mutex>
#include <netinet/in.h>
#include <sqlite3.h>
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

// The parts of a link, http://127.0.0.1:PORT/v/ID/?t=TOKEN: the view's path
// and the token.
void LinkParts(NSString *url, std::string *base, std::string *token) {
    NSURLComponents *c = [NSURLComponents componentsWithString:url ?: @""];
    *base = c.path.UTF8String ?: "";
    token->clear();
    for (NSURLQueryItem *q in c.queryItems)
        if ([q.name isEqualToString:@"t"]) *token = q.value.UTF8String ?: "";
}

// Keeps the cookies a response sets, as a browser's jar does: one of a name
// replaces the one kept.
void KeepCookies(const std::string &response, std::string &jar) {
    const std::size_t end = response.find("\r\n\r\n");
    std::size_t at = 0;
    while ((at = response.find("\r\nSet-Cookie: ", at)) != std::string::npos && at < end) {
        at += 14;
        const std::string pair = response.substr(at, response.find(';', at) - at);
        const std::string name = pair.substr(0, pair.find('='));
        std::string kept;
        std::size_t from = 0;
        while (from < jar.size()) {
            std::size_t semi = jar.find("; ", from);
            if (semi == std::string::npos) semi = jar.size();
            const std::string one = jar.substr(from, semi - from);
            if (one.substr(0, one.find('=')) != name) kept += (kept.empty() ? "" : "; ") + one;
            from = semi + 2;
        }
        jar = kept + (kept.empty() ? "" : "; ") + pair;
    }
}

// A browser, scripted on a thread of its own: the page loaded with a link's
// token (the cookies it sets kept), then its socket with them, then every
// message, keeping the hello, the newest state and the document's lines as
// the page splices them, and whether it was told to wait or was refused.
struct Browser {
    int port = 0;
    std::string base, token;
    std::string cookie;   // its jar: set before start() for a browser seen before
    int fd = -1;
    std::thread thread;
    std::atomic<bool> stop{false}, upgraded{false}, ended{false};
    std::atomic<bool> waited{false}, denied{false};
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
        else if (t == "waiting") waited = true;
        else if (t == "denied") denied = true;
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
        // The page first: its token traded for the credential's cookie.
        const std::string host = "127.0.0.1:" + std::to_string(port);
        const std::string page = Exchange(port, "GET " + base + "?t=" + token + " HTTP/1.1\r\nHost: " + host +
                                                    "\r\n" + (cookie.empty() ? "" : "Cookie: " + cookie + "\r\n") +
                                                    "\r\n");
        {
            std::lock_guard<std::mutex> lk(mu);
            KeepCookies(page, cookie);
        }
        const std::string req = "GET " + base + "ws HTTP/1.1\r\nHost: " + host +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\nCookie: " + cookie +
                                "\r\nOrigin: http://" + host + "\r\n\r\n";
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

// A scripted browser on a fresh link of the window's page; `cookie` its jar
// from before ("" for a browser never seen).
Browser *NewBrowser(MCSessionBrowser *b, const std::string &cookie = "") {
    Browser *r = new Browser();
    r->port = b.port;
    LinkParts([b newLink], &r->base, &r->token);
    r->cookie = cookie;
    r->start();
    return r;
}
std::string CookieNamed(Browser *b, const std::string &name) {
    std::lock_guard<std::mutex> lk(b->mu);
    std::size_t at = b->cookie.find(name + "=");
    if (at == std::string::npos) return "";
    return b->cookie.substr(at, b->cookie.find(';', at) - at);
}

EditorController *gC = nil, *gC2 = nil;
TerminalView *gTermA = nil, *gTermB = nil;
Browser *gBrowser = nil, *gBrowser2 = nil;
NSURL *gOpened = nil;
NSString *gUrl = nil;
int gPort = 0;
std::string gToken, gBase;
NSString *gFile = nil;
// What the plain requests got (written on their thread before `done`).
std::string page, spent, noToken, wrongToken, otherHost, wsWrong;

WKWebView *gPage = nil;
NSDictionary *gSeen = nil;   // the page as last read
dispatch_block_t gHeldAsk = nil;   // the window's sheet, held back (MCSessionBrowserAskDelay)

// What the page shows, read in its own world (window.mcSessionView).
NSString *const kRead =
    @"JSON.stringify({status: document.getElementById('status').textContent,"
    @" text: window.mcSessionView ? window.mcSessionView.text() : null,"
    @" synced: window.mcSessionView ? window.mcSessionView.synced() : false,"
    @" lines: [...document.querySelectorAll('#code .l')].map(l => l.textContent).join('\\n'),"
    @" dirty: !!document.querySelector('#tabs button.shown.dirty'),"
    @" shown: !!document.querySelector('#tabs button.shown'),"
    @" tree: window.mcSessionView ? window.mcSessionView.tree().join('|') : '',"
    @" note: window.mcSessionView ? window.mcSessionView.treeNote() : '',"
    @" browseField: !document.getElementById('browse').classList.contains('hidden'),"
    @" toast: document.getElementById('toast').textContent,"
    @" browser: window.mcSessionView ? window.mcSessionView.browser().join('|') : '',"
    @" panes: window.mcSessionView ? window.mcSessionView.panes() : null,"
    @" winPanes: window.mcSessionView ? window.mcSessionView.windowPanes() : null,"
    @" tabs: window.mcSessionView ? window.mcSessionView.tabs().join('|') : '',"
    @" local: window.mcSessionView ? window.mcSessionView.local() : '',"
    @" closers: document.querySelectorAll('#tabs button:not(.local) .x').length,"
    @" web: window.mcSessionView ? window.mcSessionView.web() : null,"
    @" href: location.href, gate: window.mcSessionView ? window.mcSessionView.gate() : '',"
    @" file: window.mcSessionView ? window.mcSessionView.file() : null,"
    @" pwned: String(window.pwned),"
    @" mode: document.getElementById('modebtn').classList.contains('hidden') ? '' : document.getElementById('modebtn').textContent,"
    @" typeset: window.mcSessionView ? window.mcSessionView.typeset() : null,"
    @" fetched: window.__typesetFetch || null,"
    @" pageFetched: window.__pageFetch || null,"
    @" scm: window.mcSessionView ? window.mcSessionView.git() : null,"
    @" review: window.mcSessionView ? window.mcSessionView.review() : null,"
    @" qo: window.mcSessionView && window.mcSessionView.quickOpen ? window.mcSessionView.quickOpen() : null,"
    @" search: window.mcSessionView && window.mcSessionView.search ? window.mcSessionView.search() : null,"
    @" writes: window.mcSessionView && window.mcSessionView.writes ? window.mcSessionView.writes() : null,"
    @" asked: window.__asked || '',"
    @" hit: window.mcSessionView && window.mcSessionView.hitLine ? window.mcSessionView.hitLine() : -1,"
    @" caret: window.mcSessionView && window.mcSessionView.caretLine ? window.mcSessionView.caretLine() : -1,"
    @" mdShown: !document.getElementById('md').classList.contains('hidden')})";

// Script run in the page (a click on its controls), its failure noted.
void Eval(NSString *js) {
    [gPage evaluateJavaScript:js inFrame:nil inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id value, NSError *error) {
                if (error || ([value isKindOfClass:NSNumber.class] && ![value boolValue]))
                    MCGuiNote(@"in the page: %@ %@", js, error.localizedDescription ?: @"found nothing");
            }];
}

// The close button of the page's tab named `name`.
void CloseTab(NSString *name) {
    NSData *quoted = [NSJSONSerialization dataWithJSONObject:@[ name ] options:0 error:nil];
    Eval([NSString stringWithFormat:
        @"(() => { const want = %@[0];"
        @" const b = [...document.querySelectorAll('#tabs button')].find(x => x.firstChild.textContent === want);"
        @" const x = b && b.querySelector('.x'); if (x) x.click(); return !!x; })()",
        [[NSString alloc] initWithData:quoted encoding:NSUTF8StringEncoding]]);
}

NSDictionary *SeenPanes(void) {
    return [gSeen[@"panes"] isKindOfClass:NSDictionary.class] ? gSeen[@"panes"] : nil;
}

// An address typed into the page's address field, then Return.
void Browse(NSString *url) {
    NSData *quoted = [NSJSONSerialization dataWithJSONObject:@[ url ] options:0 error:nil];
    NSString *js = [NSString stringWithFormat:
        @"(() => { const f = document.getElementById('browse'); f.value = %@[0];"
        @" f.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true, cancelable: true}));"
        @" return 1; })()",
        [[NSString alloc] initWithData:quoted encoding:NSUTF8StringEncoding]];
    [gPage evaluateJavaScript:js inFrame:nil inContentWorld:WKContentWorld.pageWorld
            completionHandler:^(id, NSError *error) {
                if (error) MCGuiNote(@"browse: %@", error.localizedDescription);
            }];
}

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

// A PDF of one word, as the stand-in tectonic "typesets" it.
NSData *WordPDF(NSString *word) {
    NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 300, 80)];
    tv.string = word;
    tv.font = [NSFont systemFontOfSize:24];
    return [tv dataWithPDFInsideRect:tv.bounds];
}

// A PDF of `pages` pages, `height` points tall and 300 wide, each with
// `word` and its number on it, as the stand-in tectonic "typesets" it.
NSData *PagesPDF(NSString *word, int pages, CGFloat height) {
    NSMutableData *data = [NSMutableData data];
    CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData((__bridge CFMutableDataRef)data);
    CGRect box = CGRectMake(0, 0, 300, height);
    CGContextRef ctx = CGPDFContextCreate(consumer, &box, NULL);
    CGDataConsumerRelease(consumer);
    NSDictionary *attrs = @{NSFontAttributeName : [NSFont systemFontOfSize:24]};
    for (int i = 0; i < pages; i++) {
        CGPDFContextBeginPage(ctx, NULL);
        NSGraphicsContext *was = NSGraphicsContext.currentContext;
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
        [[NSString stringWithFormat:@"%@ %d", word, i + 1] drawAtPoint:NSMakePoint(20, height - 60)
                                                        withAttributes:attrs];
        NSGraphicsContext.currentContext = was;
        CGPDFContextEndPage(ctx);
    }
    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    return data;
}

// The page fetches its first page's picture: the answer's status, type,
// length and first four bytes in window.__pageFetch.
void FetchPage(void) {
    gSeen = nil;
    Eval(@"(() => { window.__pageFetch = null; const src = window.mcSessionView.typeset().pageSrcs[0];"
         @" if (!src) return false;"
         @" fetch(src).then(r => r.arrayBuffer().then(b => { const a = new Uint8Array(b);"
         @" window.__pageFetch = {status: r.status, type: r.headers.get('content-type') || '',"
         @" length: a.length, head: [...a.slice(0, 4)], src: src}; }))"
         @" .catch(e => { window.__pageFetch = {error: String(e)}; });"
         @" return true; })()");
}

// The bytes' sum, as the page's fetch adds them up.
uint32_t ByteSum(NSData *d) {
    uint32_t s = 0;
    const unsigned char *b = (const unsigned char *)d.bytes;
    for (NSUInteger i = 0; i < d.length; i++) s += b[i];
    return s;
}

// The page fetches its typeset frame's address, as its frame does: the
// answer's status, type, length and byte sum in window.__typesetFetch.
// What was read before is dropped: Read() answers later, so a wait
// looking at gSeen at once saw the last fetch's answer (CI, PR #261).
void FetchTypeset(void) {
    gSeen = nil;
    Eval(@"(() => { window.__typesetFetch = null; const src = window.mcSessionView.typeset().src;"
         @" if (!src) return false;"
         @" fetch(src).then(r => r.arrayBuffer().then(b => { const a = new Uint8Array(b); let s = 0;"
         @" for (const x of a) s = (s + x) >>> 0;"
         @" window.__typesetFetch = {status: r.status, type: r.headers.get('content-type') || '',"
         @" length: a.length, sum: s, src: src}; }))"
         @" .catch(e => { window.__typesetFetch = {error: String(e)}; });"
         @" return true; })()");
}

NSDictionary *SeenTypeset(void) {
    return [gSeen[@"typeset"] isKindOfClass:NSDictionary.class] ? gSeen[@"typeset"] : nil;
}
NSDictionary *SeenFetch(void) {
    return [gSeen[@"fetched"] isKindOfClass:NSDictionary.class] ? gSeen[@"fetched"] : nil;
}
// A fetch of the frame's address now on show has answered.
BOOL FetchedShown(void) {
    NSString *src = SeenTypeset()[@"src"];
    return src.length && [SeenFetch()[@"src"] isEqualToString:src];
}

NSTextView *TV(EditorController *c) { return [c valueForKey:@"textView"]; }
MCTab *Active(EditorController *c) { return [c valueForKey:@"activeTab"]; }
// The window's tab for a file named `name`, nil when it has none.
MCTab *MCTabNamed(EditorController *c, NSString *name) {
    for (MCTab *t in [c tabs])
        if ([t.currentPath.lastPathComponent isEqualToString:name]) return t;
    return nil;
}
std::string Text(EditorController *c) { return std::string(TV(c).string.UTF8String ?: ""); }

// A PNG of w by h pixels, every pixel gray.
NSData *GrayPng(NSInteger w, NSInteger h) {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4
                        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:0 bitsPerPixel:0];
    memset(rep.bitmapData, 0x80, (size_t)rep.bytesPerRow * (size_t)h);
    return [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
}

// The window's tab whose page is `page`, nil when none is.
MCTab *TabOfPage(EditorController *c, id page) {
    for (MCTab *t in [c groupTabs])
        if (t.page == page) return t;
    return nil;
}

// A key pressed on the page itself (not in a field), as a browser sends
// it: `options` a JavaScript object of the event's modifiers.
void PageKey(NSString *key, NSString *options) {
    Eval([NSString stringWithFormat:
        @"(() => { document.dispatchEvent(new KeyboardEvent('keydown', Object.assign({key: '%@', bubbles: true,"
        @" cancelable: true}, %@))); return 1; })()", key, options]);
}

// Text typed into the page's field `field` (an input event, as typing
// sends), then, with `key`, that key pressed in it.
void FieldText(NSString *field, NSString *text, NSString *key) {
    NSData *quoted = [NSJSONSerialization dataWithJSONObject:@[ text ] options:0 error:nil];
    Eval([NSString stringWithFormat:
        @"(() => { const f = document.getElementById('%@'); f.value = %@[0];"
        @" f.dispatchEvent(new Event('input', {bubbles: true}));"
        @" %@ return 1; })()", field, [[NSString alloc] initWithData:quoted encoding:NSUTF8StringEncoding],
        key ? [NSString stringWithFormat:@"f.dispatchEvent(new KeyboardEvent('keydown', {key: '%@', bubbles: true,"
                                         @" cancelable: true}));", key] : @""]);
}

NSDictionary *Seen(NSString *key) {
    return [gSeen[key] isKindOfClass:NSDictionary.class] ? gSeen[key] : nil;
}

// A click on the page's tab strip button named `name`.
void ClickTab(NSString *name) {
    NSData *quoted = [NSJSONSerialization dataWithJSONObject:@[ name ] options:0 error:nil];
    Eval([NSString stringWithFormat:
        @"(() => { const want = %@[0];"
        @" const b = [...document.querySelectorAll('#tabs button')].find(x => x.firstChild.textContent === want);"
        @" if (b) b.click(); return !!b; })()",
        [[NSString alloc] initWithData:quoted encoding:NSUTF8StringEncoding]]);
}

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
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"data.csv"], @"name,qty\napple,3\n");
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
        NSString *prefix = [NSString stringWithFormat:@"http://127.0.0.1:%d/v/", gPort];
        GCHECK([gUrl hasPrefix:prefix]);
        LinkParts(gUrl, &gBase, &gToken);
        GCHECK(gToken.size() == 48 && gBase.size() == 20);
    });
    __block std::string copiedToken;
    __block NSUInteger sheetsBefore = 0;
    MCGuiStep(@"a file opened now has a document on the host; the link copied is a fresh one", ^{
        [gC openFileAtPath:gFile];
        GCHECK(Active(gC).session != nil);
        [gC copySessionBrowserLink:nil];
        NSString *copied = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
        std::string copiedBase;
        LinkParts(copied, &copiedBase, &copiedToken);
        MCGuiNote(@"copied %@", copied);
        GCHECK(copiedBase == gBase && copiedToken.size() == 48 && copiedToken != gToken);
        GCHECK(gC.sessionBrowser.port == gPort);   // one server for the window
        [gC.sessionBrowser reportTabs];
        sheetsBefore = MCGuiSheetsShown().count;
    });
    MCGuiAsync(@"plain requests: the page with its token, refusals without", 20, ^(MCGuiDone done) {
        const int port = gPort;
        const std::string token = gToken, base = gBase, other = copiedToken;
        std::thread([=] {
            const std::string host = "127.0.0.1:" + std::to_string(port);
            page = Get(port, base + "?t=" + token, host);
            spent = Get(port, base + "?t=" + token, host);
            noToken = Get(port, base, host);
            wrongToken = Get(port, base + "?t=" + std::string(48, '0'), host);
            otherHost = Get(port, base + "?t=" + other, "attacker.example");
            wsWrong = Exchange(port, "GET " + base + "ws?t=" + other + " HTTP/1.1\r\nHost: " + host +
                                     "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                     "Sec-WebSocket-Version: 13\r\nOrigin: http://" + host + "\r\n\r\n");
            dispatch_async(dispatch_get_main_queue(), ^{ done(); });
        }).detach();
    });
    MCGuiStep(@"what each request got", ^{
        GCHECK(page.compare(0, 12, "HTTP/1.1 200") == 0);
        GCHECK(page.find("<!DOCTYPE html>") != std::string::npos);
        // The cookie that carries the credential: HttpOnly, SameSite=Strict,
        // the view's path.
        GCHECK(page.find("\r\nSet-Cookie: mutible-view-" + std::to_string(gPort) + "=") != std::string::npos);
        GCHECK(page.find("; Path=" + gBase + "; HttpOnly; SameSite=Strict\r\n") != std::string::npos);
        // The link a second time: refused.
        GCHECK(spent.compare(0, 12, "HTTP/1.1 403") == 0 && spent.find("used already") != std::string::npos);
        GCHECK(noToken.compare(0, 12, "HTTP/1.1 403") == 0 && noToken.find("<html") == std::string::npos);
        GCHECK(wrongToken.compare(0, 12, "HTTP/1.1 403") == 0);
        GCHECK(otherHost.compare(0, 12, "HTTP/1.1 403") == 0);
        GCHECK(wsWrong.compare(0, 12, "HTTP/1.1 403") == 0);
        GCHECK(gC.sessionBrowser.browsers == 0);
    });
    MCGuiWait(@"the window asks about the browser that traded the link, on a sheet (Allow)", 20, ^BOOL {
        return MCGuiSheetsShown().count > sheetsBefore && gC.sessionBrowser.asking == 0;
    });
    MCGuiStep(@"the sheet names the browser and where it is; then a scripted browser, Always Allowed", ^{
        MCGuiNote(@"asked: %@ / %@", MCGuiSheetsShown().lastObject, MCGuiLastAlertInformative());
        GCHECK_EQ_STR(MCGuiSheetsShown().lastObject, @"Allow an unknown browser to open this window's session?");
        GCHECK(MCGuiLastSheetWindow() == gC.window);
        GCHECK([MCGuiLastAlertInformative() containsString:@"from this Mac (127.0.0.1:"]);
        GCHECK([MCGuiLastAlertInformative() containsString:@"see and edit"]);
        GCHECK(MCSessionBrowsersRemembered().count == 0);
        sheetsBefore = MCGuiSheetsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertThirdButtonReturn)]);   // Always Allow
        gBrowser = NewBrowser(gC.sessionBrowser);
    });
    MCGuiWait(@"the browser's socket gets the window's tabs", 20, ^BOOL {
        std::lock_guard<std::mutex> lk(gBrowser->mu);
        return gBrowser->state.isObject() && gBrowser->state["docs"].size() > 0;
    });
    __block std::string remembered;   // the Always Allowed browser's id cookie
    MCGuiStep(@"it was asked about once, and is remembered", ^{
        GCHECK(MCGuiSheetsShown().count == sheetsBefore + 1);
        NSArray *list = MCSessionBrowsersRemembered();
        remembered = CookieNamed(gBrowser, "mutible-browser");
        MCGuiNote(@"remembered: %@, its cookie %s", list, remembered.c_str());
        GCHECK(list.count == 1 && !remembered.empty());
        GCHECK(list.count == 1 && remembered == "mutible-browser=" + std::string([list[0][@"id"] UTF8String]));
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
            [tv insertText:@"# typed in mutible\n" replacementRange:NSMakeRange(0, 0)];
        });
        GCHECK([tv.string hasPrefix:@"# typed in mutible\n"]);
    });
    MCGuiWait(@"reaches the browser", 20, ^BOOL { return gBrowser->text() == Text(gC); });
    MCGuiStep(@"the scripted browser leaves, and the page loads in WebKit", ^{
        GCHECK(gBrowser->text().find("# typed in mutible") == 0);
        gBrowser->finish();
        delete gBrowser;
        gBrowser = nullptr;
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
        gPage = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 900, 700) configuration:config];
        // The window's sheet held back, so the page is seen waiting.
        MCSessionBrowserAskDelay = ^(dispatch_block_t ask) { gHeldAsk = ask; };
        sheetsBefore = MCGuiSheetsShown().count;
        gUrl = [gC.sessionBrowser newLink];
        [gPage loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:gUrl]]];
    });
    MCGuiWait(@"the page says it waits for approval in mutible", 30, ^BOOL {
        Read();
        return gHeldAsk != nil && [gSeen[@"gate"] isEqualToString:@"Waiting for approval in mutible"];
    });
    MCGuiStep(@"its address has lost the token; the window's sheet, Allow", ^{
        NSString *at = [NSString stringWithFormat:@"http://127.0.0.1:%d%s", gPort, gBase.c_str()];
        MCGuiNote(@"opened %@, now at %@", gUrl, gSeen[@"href"]);
        GCHECK_EQ_STR(gSeen[@"href"], at);
        GCHECK_EQ_STR(gPage.URL.absoluteString, at);
        GCHECK(gPage.backForwardList.backList.count == 0);
        GCHECK(gC.sessionBrowser.browsers == 0);   // nothing of the session yet
        MCSessionBrowserAskDelay = nil;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);   // Allow
        dispatch_block_t ask = gHeldAsk;
        gHeldAsk = nil;
        ask();
        GCHECK(MCGuiSheetsShown().count == sheetsBefore + 1);
        MCGuiNote(@"asked: %@", MCGuiSheetsShown().lastObject);
        GCHECK_EQ_STR(MCGuiSheetsShown().lastObject, @"Allow a WebKit browser on macOS to open this window's session?");
        GCHECK(MCSessionBrowsersRemembered().count == 1);   // Allow alone remembers nothing
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
            [tv insertText:@"# from mutible\n" replacementRange:NSMakeRange(0, 0)];
        });
    });
    MCGuiWait(@"reaches the page, which shows the file's dirty dot", 20, ^BOOL {
        Read();
        return [gSeen[@"text"] isEqualToString:TV(gC).string] && [gSeen[@"dirty"] boolValue];
    });
    MCGuiStep(@"Return in the page takes the core's indentation", ^{
        GCHECK([gSeen[@"lines"] hasPrefix:@"# from mutible\n"]);
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
        return [gSeen[@"tree"] isEqualToString:@"pkg|calc.py|data.csv|logo.png"];
    });
    MCGuiStep(@"a click on the folder in the page's tree", ^{ ClickRow(@"pkg"); });
    MCGuiWait(@"opens it, read by the host", 20, ^BOOL {
        Read();
        return [gSeen[@"tree"] isEqualToString:@"pkg|pkg/util.py|calc.py|data.csv|logo.png"];
    });
    MCGuiStep(@"a click on a file in it", ^{ ClickRow(@"pkg/util.py"); });
    MCGuiWait(@"opens the file in the window's tab", 20, ^BOOL {
        return [Active(gC).currentPath hasSuffix:@"/pkg/util.py"] && Active(gC).session != nil;
    });
    MCGuiWait(@"and the page shows that tab", 20, ^BOOL {
        Read();
        return [gSeen[@"text"] isEqualToString:TV(gC).string] && [TV(gC).string hasPrefix:@"def helper"];
    });
    MCGuiStep(@"Command P in the page: Quick Open", ^{ PageKey(@"p", @"{metaKey: true}"); });
    MCGuiWait(@"lists the folder's files as the host listed them", 20, ^BOOL {
        Read();
        return [Seen(@"qo")[@"open"] boolValue] &&
               [[Seen(@"qo")[@"rows"] componentsJoinedByString:@"|"] isEqualToString:@"calc.py|data.csv|logo.png|pkg/util.py"];
    });
    MCGuiStep(@"a query typed into it", ^{ FieldText(@"qoq", @"clc", nil); });
    MCGuiWait(@"ranks them by the palette's matching, the matched letters marked", 20, ^BOOL {
        Read();
        return [[Seen(@"qo")[@"rows"] componentsJoinedByString:@"|"] isEqualToString:@"calc.py"] &&
               [[Seen(@"qo")[@"marked"] componentsJoinedByString:@"|"] isEqualToString:@"clc"];
    });
    MCGuiStep(@"Return", ^{
        GCHECK([Active(gC).currentPath hasSuffix:@"/pkg/util.py"]);
        FieldText(@"qoq", @"clc", @"Enter");
    });
    MCGuiWait(@"opens the file in the window", 20, ^BOOL {
        return [Active(gC).currentPath hasSuffix:@"/calc.py"];
    });
    MCGuiWait(@"and the page shows it, Quick Open closed", 20, ^BOOL {
        Read();
        return [gSeen[@"text"] isEqualToString:TV(gC).string] && ![Seen(@"qo")[@"open"] boolValue];
    });
    // Stage 7's page-side writes, on the editable page: New File, Save As
    // through the window, and Replace in Folder on the host.
    __block NSUInteger tabsAtNew = 0;
    NSString *madePath = [dir stringByAppendingPathComponent:@"made.py"];
    NSString *notesPath = [dir stringByAppendingPathComponent:@"notes.txt"];
    MCGuiStep(@"the editable page has New File, Save As and a replace field; New File clicked", ^{
        NSDictionary *w = Seen(@"writes");
        MCGuiNote(@"writes: %@", w);
        GCHECK([w[@"newFile"] boolValue] && [w[@"saveAs"] boolValue] && [w[@"replace"] boolValue]);
        tabsAtNew = [gC tabs].count;
        Eval(@"(() => { document.getElementById('newbtn').click(); return 1; })()");
    });
    MCGuiWait(@"makes an untitled tab in the window, with a document on the host", 20, ^BOOL {
        return [gC tabs].count == tabsAtNew + 1 && Active(gC).untitled && Active(gC).session != nil;
    });
    MCGuiWait(@"which the page shows, named as the window names it", 20, ^BOOL {
        Read();
        NSString *name = Active(gC).currentPath.lastPathComponent ?: @"?";
        return [[gSeen[@"tabs"] componentsSeparatedByString:@"|"] containsObject:name] &&
               [gSeen[@"text"] isEqualToString:@""] && [gSeen[@"shown"] boolValue];
    });
    MCGuiStep(@"text typed into it in the page", ^{
        MCGuiNote(@"untitled tab: %@, page tabs: %@", Active(gC).currentPath, gSeen[@"tabs"]);
        GCHECK(!Active(gC).dirty);
        Keys([NSString stringWithFormat:@"[%@]", Typed(@"made = 1")]);
    });
    MCGuiWait(@"reaches the window's untitled tab", 20, ^BOOL {
        return [TV(gC).string isEqualToString:@"made = 1"];
    });
    MCGuiStep(@"Shift Command S in the page: Save As", ^{ Keys(@"[[\"s\", {metaKey: true, shiftKey: true}]]"); });
    MCGuiWait(@"opens on the untitled tab's name", 20, ^BOOL {
        Read();
        NSDictionary *w = Seen(@"writes");
        NSString *name = Active(gC).currentPath.lastPathComponent ?: @"?";
        return [w[@"dialog"] boolValue] && [w[@"value"] hasSuffix:name] && [name hasPrefix:@"Untitled"];
    });
    MCGuiStep(@"a folder typed: its names complete the path", ^{ FieldText(@"saq", @"pkg/", nil); });
    MCGuiWait(@"from the host's listing", 20, ^BOOL {
        Read();
        return [[Seen(@"writes")[@"completions"] componentsJoinedByString:@"|"] isEqualToString:@"util.py"];
    });
    __block NSData *logoBefore = nil;
    MCGuiStep(@"a file there already (one no tab has), then Return, answered Cancel", ^{
        logoBefore = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"logo.png"]];
        Eval(@"(() => { window.__asked = ''; window.confirm = (m) => { window.__asked = m; return false; }; return 1; })()");
        FieldText(@"saq", @"logo.png", @"Enter");
    });
    MCGuiWait(@"asks before writing over it", 20, ^BOOL {
        Read();
        return [gSeen[@"asked"] hasPrefix:@"logo.png already exists"];
    });
    MCGuiStep(@"and wrote nothing; a new name, then Return", ^{
        GCHECK([[NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"logo.png"]] isEqualToData:logoBefore]);
        GCHECK(Active(gC).untitled);
        FieldText(@"saq", @"made.py", @"Enter");
    });
    MCGuiWait(@"the window writes it through the host, and its tab is that file", 20, ^BOOL {
        NSString *disk = [NSString stringWithContentsOfFile:madePath encoding:NSUTF8StringEncoding error:nil];
        return [disk isEqualToString:@"made = 1"] && [Active(gC).currentPath isEqualToString:madePath] &&
               !Active(gC).untitled && !Active(gC).dirty;
    });
    MCGuiWait(@"the page's tab has its name, the dialog closed", 20, ^BOOL {
        Read();
        return [[gSeen[@"tabs"] componentsSeparatedByString:@"|"] containsObject:@"made.py"] &&
               ![Seen(@"writes")[@"dialog"] boolValue];
    });
    MCGuiStep(@"page, window and host on one text; a file the window has not open, and Find in Folder", ^{
        GCHECK(MCSessionCheckTabs(@[ Active(gC) ]).length == 0);
        MCGuiWriteFile(notesPath, @"made here\nmade there\n");
        PageKey(@"f", @"{metaKey: true, shiftKey: true}");
    });
    MCGuiStep(@"made searched for", ^{ FieldText(@"sq", @"made", @"Enter"); });
    MCGuiWait(@"rows in both files, each with a box, Replace All ready", 20, ^BOOL {
        Read();
        NSDictionary *s = Seen(@"search"), *w = Seen(@"writes");
        return [s[@"done"] boolValue] && [s[@"files"] count] == 2 && [w[@"picks"] count] == 3 &&
               [w[@"replaceAll"] boolValue];
    });
    MCGuiStep(@"a replacement typed, and the last row left out", ^{
        MCGuiNote(@"search: %@", Seen(@"search")[@"files"]);
        FieldText(@"rq", @"built", nil);
        Eval(@"(() => { const b = [...document.querySelectorAll('#slist .srow .rpick')].pop(); if (b) b.click();"
             @" return !!b; })()");
    });
    MCGuiWait(@"each match shows what replaces it; two rows checked", 20, ^BOOL {
        Read();
        NSDictionary *w = Seen(@"writes");
        return [[w[@"previews"] componentsJoinedByString:@"|"] isEqualToString:@"built|built|built"] &&
               [[w[@"picks"] componentsJoinedByString:@","] isEqualToString:@"1,1,0"];
    });
    MCGuiStep(@"Return in the replace field", ^{ FieldText(@"rq", @"built", @"Enter"); });
    MCGuiWait(@"searches again, every row checked", 20, ^BOOL {
        Read();
        NSDictionary *s = Seen(@"search"), *w = Seen(@"writes");
        return [s[@"done"] boolValue] && [[w[@"picks"] componentsJoinedByString:@","] isEqualToString:@"1,1,1"];
    });
    MCGuiStep(@"and wrote nothing; the last row left out, then Replace All, answered Replace", ^{
        NSString *notes = [NSString stringWithContentsOfFile:notesPath encoding:NSUTF8StringEncoding error:nil];
        GCHECK_EQ_STR(notes, @"made here\nmade there\n");
        GCHECK_EQ_STR(TV(gC).string, @"made = 1");
        Eval(@"(() => { const b = [...document.querySelectorAll('#slist .srow .rpick')].pop(); if (b) b.click();"
             @" window.__asked = ''; window.confirm = (m) => { window.__asked = m; return true; };"
             @" document.getElementById('rall').click(); return !!b; })()");
    });
    MCGuiWait(@"the open file edited in the window, the other written but for the row left out", 20, ^BOOL {
        NSString *notes = [NSString stringWithContentsOfFile:notesPath encoding:NSUTF8StringEncoding error:nil];
        return [notes isEqualToString:@"built here\nmade there\n"] && [TV(gC).string isEqualToString:@"built = 1"];
    });
    // Read() answers later: the question is looked for in a wait, never in
    // gSeen as a step finds it (CI on #307 read the Save As question there).
    MCGuiWait(@"it asked first, naming the matches, files and replacement", 20, ^BOOL {
        Read();
        return [gSeen[@"asked"] hasPrefix:@"Replace 2 matches in 2 files with “built”?"];
    });
    MCGuiStep(@"the window's tab is unsaved, made.py on disk as it was; Undo in the window", ^{
        MCGuiNote(@"asked: %@; toast: %@", gSeen[@"asked"], gSeen[@"toast"]);
        GCHECK_EQ_STR([gSeen[@"asked"] componentsSeparatedByString:@"\n"].firstObject,
                      @"Replace 2 matches in 2 files with “built”?");
        GCHECK(Active(gC).dirty);
        NSString *made = [NSString stringWithContentsOfFile:madePath encoding:NSUTF8StringEncoding error:nil];
        GCHECK_EQ_STR(made, @"made = 1");
        GCHECK(MCSessionCheckTabs(@[ Active(gC) ]).length == 0);
        [Active(gC).undoManager undo];
    });
    MCGuiWait(@"takes the host's step back", 20, ^BOOL {
        return [TV(gC).string isEqualToString:@"made = 1"];
    });
    MCGuiStep(@"made.py closed, both files gone again, and back to Files", ^{
        GCHECK([Active(gC).currentPath isEqualToString:madePath]);
        [gC closeTabEverywhere:Active(gC) prompt:NO];
        [NSFileManager.defaultManager removeItemAtPath:madePath error:nil];
        [NSFileManager.defaultManager removeItemAtPath:notesPath error:nil];
        Eval(@"(() => { document.getElementById('filesbtn').click(); return 1; })()");
        [gC openFileAtPath:gFile];
    });
    MCGuiWait(@"the page on the window's file again", 20, ^BOOL {
        Read();
        return [Active(gC).currentPath isEqualToString:gFile] && [gSeen[@"text"] isEqualToString:TV(gC).string] &&
               ![Seen(@"search")[@"mode"] boolValue];
    });
    MCGuiStep(@"a picture in the page's tree", ^{
        GCHECK(MCSessionCheckTabs(@[ Active(gC) ]).length == 0);
        ClickRow(@"logo.png");
    });
    MCGuiWait(@"shows in the page, its bytes read through the host", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"file"] isKindOfClass:NSDictionary.class] ? gSeen[@"file"] : nil;
        return [f[@"view"] isEqualToString:@"image"] && [f[@"width"] intValue] == 1;
    });
    MCGuiStep(@"at the token's /file/ address, in a tab of the page's own", ^{
        NSDictionary *f = gSeen[@"file"];
        MCGuiNote(@"file view: %@, tabs: %@", f, gSeen[@"tabs"]);
        // At the view's own address, with no secret in it: the cookie goes
        // with the picture's request.
        GCHECK_EQ_STR(f[@"src"], ([NSString stringWithFormat:@"%sfile/logo.png", gBase.c_str()]));
        GCHECK(![f[@"src"] containsString:@(gToken.c_str())] && ![f[@"src"] containsString:@"t="]);
        GCHECK_EQ_STR(gSeen[@"local"], @"logo.png");
        GCHECK([[gSeen[@"tabs"] componentsSeparatedByString:@"|"] containsObject:@"+logo.png"]);
        GCHECK_EQ_STR(gSeen[@"note"], @"");
    });
    MCGuiWait(@"and the window opens it too", 20, ^BOOL {
        return [Active(gC).currentPath hasSuffix:@"/logo.png"];
    });
    MCGuiStep(@"a CSV in the page's tree", ^{ ClickRow(@"data.csv"); });
    MCGuiWait(@"shows as the table the core parsed", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"file"] isKindOfClass:NSDictionary.class] ? gSeen[@"file"] : nil;
        return [f[@"view"] isEqualToString:@"table"] &&
               [[f[@"rows"] componentsJoinedByString:@"|"] isEqualToString:@"name,qty|apple,3"];
    });
    MCGuiWait(@"and the window opens it", 20, ^BOOL {
        return [Active(gC).currentPath hasSuffix:@"/data.csv"];
    });
    MCGuiStep(@"the editable page has an address field; javascript: typed into it", ^{
        GCHECK([gSeen[@"browseField"] boolValue]);
        GCHECK_EQ_STR(gSeen[@"browser"], @"");   // no browser pane yet: no group
        Browse(@"javascript:alert(1)");
    });
    MCGuiWait(@"is refused, and the window opens nothing", 20, ^BOOL {
        Read();
        return [gSeen[@"toast"] hasPrefix:@"Not opened"];
    });
    __block NSString *address = nil;
    MCGuiStep(@"an address on this Mac typed into it", ^{
        GCHECK([gC browserPane] == nil || ![[gC browserPane].tabAddresses containsObject:@"javascript:alert(1)"]);
        address = [NSString stringWithFormat:@"http://127.0.0.1:%d/browser-tab", gPort];
        Browse(address);
    });
    MCGuiWait(@"opens in the window's browser pane", 20, ^BOOL {
        return [[gC browserPane].tabAddresses containsObject:address];
    });
    MCGuiWait(@"and the page lists the window's browser tab and frames it", 20, ^BOOL {
        Read();
        NSDictionary *web = [gSeen[@"web"] isKindOfClass:NSDictionary.class] ? gSeen[@"web"] : nil;
        return [[gSeen[@"browser"] componentsSeparatedByString:@"|"] containsObject:address] &&
               [web[@"src"] isEqualToString:address];
    });
    MCGuiStep(@"with its link to a new tab and the loopback line, in a pane beside the editor", ^{
        MCGuiNote(@"browser tabs: %@, panes: %@", gSeen[@"browser"], gSeen[@"panes"]);
        NSDictionary *web = gSeen[@"web"];
        GCHECK_EQ_STR(web[@"open"], address);
        GCHECK([web[@"loopback"] boolValue]);
        GCHECK([SeenPanes()[@"browser"] boolValue] && [SeenPanes()[@"beside"] boolValue]);
        // The editor's strip holds the files, the browser's pane its tabs.
        GCHECK(![gSeen[@"tabs"] containsString:@"127.0.0.1"]);
        GCHECK([gC browserVisible]);
        Eval(@"(() => { document.getElementById('webclose').click(); return 1; })()");
    });
    MCGuiWait(@"the page's close button hides the browser pane on the page", 20, ^BOOL {
        Read();
        return SeenPanes() && ![SeenPanes()[@"browser"] boolValue] && gSeen[@"web"] == NSNull.null;
    });
    MCGuiStep(@"and only there; then the window hides its own pane", ^{
        GCHECK([gC browserVisible]);
        GCHECK([[gC browserPane].tabAddresses containsObject:address]);
        [gC toggleBrowser:nil];
        GCHECK(![gC browserVisible]);
        MCGuiSetKeyWindow(gC.window);
    });
    MCGuiWait(@"which the page hears", 20, ^BOOL {
        Read();
        NSDictionary *w = [gSeen[@"winPanes"] isKindOfClass:NSDictionary.class] ? gSeen[@"winPanes"] : nil;
        return w && ![w[@"browser"] boolValue];
    });
    MCGuiStep(@"and shows it again", ^{
        GCHECK(![SeenPanes()[@"browser"] boolValue]);
        [gC toggleBrowser:nil];
        GCHECK([gC browserVisible]);
        MCGuiSetKeyWindow(gC.window);
    });
    MCGuiWait(@"which the page follows: its pane shows again", 20, ^BOOL {
        Read();
        return [SeenPanes()[@"browser"] boolValue];
    });
    MCGuiStep(@"a file opened in the window, kept", ^{
        [gC openFileAtPath:[dir stringByAppendingPathComponent:@"pkg/util.py"]];
        GCHECK(MCTabNamed(gC, @"util.py") != nil && MCTabNamed(gC, @"util.py").session != nil);
    });
    MCGuiWait(@"is a tab on the page, with a close button", 20, ^BOOL {
        Read();
        return [[gSeen[@"tabs"] componentsSeparatedByString:@"|"] containsObject:@"util.py"] &&
               [gSeen[@"closers"] intValue] > 0;
    });
    MCGuiStep(@"the editable page's close button on it", ^{
        MCGuiNote(@"tabs: %@", gSeen[@"tabs"]);
        GCHECK(!MCTabNamed(gC, @"util.py").dirty);
        CloseTab(@"util.py");
    });
    MCGuiWait(@"closes it in the window", 20, ^BOOL { return MCTabNamed(gC, @"util.py") == nil; });
    MCGuiWait(@"and the page's strip lets it go", 20, ^BOOL {
        Read();
        return gSeen[@"tabs"] && ![gSeen[@"tabs"] containsString:@"util.py"];
    });
    MCGuiStep(@"the window's file on show again", ^{ [gC openFileAtPath:gFile]; });
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
    MCGuiWait(@"the page shows this window's terminal below the editor", 20, ^BOOL {
        Read();
        return [SeenPanes()[@"terminal"] boolValue];
    });
    MCGuiStep(@"the window hides its terminal panel (Toggle Terminal)", ^{
        GCHECK([gC terminalVisible]);
        [gC toggleTerminal:nil];
        GCHECK(![gC terminalVisible]);
        MCGuiSetKeyWindow(gC.window);
    });
    MCGuiWait(@"and the page hides its terminal pane, the shell still running", 20, ^BOOL {
        Read();
        return SeenPanes() && ![SeenPanes()[@"terminal"] boolValue];
    });
    MCGuiStep(@"the window shows it again", ^{
        GCHECK(gTermA.sessionTerminal != 0 && [[gC controlTerminals] containsObject:gTermA]);
        [gC toggleTerminal:nil];
        GCHECK([gC terminalVisible]);
        MCGuiSetKeyWindow(gC.window);
    });
    MCGuiWait(@"and so does the page", 20, ^BOOL {
        Read();
        return [SeenPanes()[@"terminal"] boolValue];
    });
    MCGuiStep(@"a scripted browser on each window's page", ^{
        // The browser Always Allowed: no sheet on either window.
        sheetsBefore = MCGuiSheetsShown().count;
        gBrowser = NewBrowser(gC.sessionBrowser, remembered);
        gBrowser2 = NewBrowser(gC2.sessionBrowser, remembered);
    });
    MCGuiWait(@"each page lists its own window's terminal and not the other's, nobody asked", 20, ^BOOL {
        return gBrowser->terms() == std::to_string(gTermA.sessionTerminal) + " " &&
               gBrowser2->terms() == std::to_string(gTermB.sessionTerminal) + " ";
    });
    MCGuiStep(@"keys for the other window's terminal, asked for by id, then for its own", ^{
        GCHECK(MCGuiSheetsShown().count == sheetsBefore);
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
    MCGuiWait(@"and the page in WebKit drops its terminal pane", 20, ^BOOL {
        Read();
        return SeenPanes() && ![SeenPanes()[@"terminal"] boolValue];
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
    MCGuiStep(@"a browser the window does not know, and Deny", ^{
        sheetsBefore = MCGuiSheetsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Deny
        gBrowser = NewBrowser(gC.sessionBrowser);
    });
    MCGuiWait(@"is refused, and its socket closed", 20, ^BOOL { return gBrowser->denied && gBrowser->ended; });
    MCGuiStep(@"it saw nothing of the session; Remembered Browsers… lists the one, and Forget All", ^{
        {
            std::lock_guard<std::mutex> lk(gBrowser->mu);
            GCHECK(!gBrowser->hello.isObject() && !gBrowser->state.isObject());
        }
        GCHECK(MCGuiSheetsShown().count == sheetsBefore + 1);
        gBrowser->finish();
        delete gBrowser;
        gBrowser = nullptr;
        GCHECK(gC.sessionBrowser.browsers == 1);   // the page in WebKit
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);   // Forget All
        GCHECK([NSApp sendAction:@selector(showRememberedBrowsers:) to:nil from:nil]);
        MCGuiNote(@"listed: %@ / %@", MCGuiAlertsShown().lastObject, MCGuiLastAlertInformative());
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"One browser is remembered");
        GCHECK([MCGuiLastAlertInformative() hasPrefix:@"an unknown browser, from this Mac, since "]);
        GCHECK(MCSessionBrowsersRemembered().count == 0);
        [NSApp sendAction:@selector(showRememberedBrowsers:) to:nil from:nil];
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"No browsers are remembered");
    });
    __block NSString *readUrl = nil;
    MCGuiStep(@"the page again, read-only (the remembered choice)", ^{
        [gPage stopLoading];
        gSeen = nil;
        [MCAppDefaults() setBool:NO forKey:@"MCSessionBrowserEditable"];
        [gC copySessionBrowserLink:nil];
        GCHECK(gC.sessionBrowser != nil && !gC.sessionBrowser.editable);
        readUrl = gC.sessionBrowser.url;
        GCHECK(readUrl != nil && ![readUrl isEqualToString:gUrl]);   // a server anew
        GCHECK(MCTabNamed(gC, @"util.py") == nil);
        [gPage loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:readUrl]]];
    });
    MCGuiWait(@"the read-only page shows the window's tab, with no close buttons on the window's tabs", 30, ^BOOL {
        Read();
        return [gSeen[@"status"] isEqualToString:@"live, read-only"] &&
               [gSeen[@"text"] isEqualToString:TV(gC).string] &&
               [gSeen[@"tree"] isEqualToString:@"pkg|calc.py|data.csv|logo.png"];
    });
    __block NSUInteger tabsBefore = 0;
    MCGuiStep(@"a folder and a file clicked in its tree", ^{
        GCHECK([gSeen[@"closers"] intValue] == 0);
        GCHECK([SeenPanes()[@"browser"] boolValue]);   // the layout is the window's for a reader too
        tabsBefore = [gC tabs].count;
        ClickRow(@"pkg");
    });
    MCGuiWait(@"the folder opens", 20, ^BOOL {
        Read();
        return [gSeen[@"tree"] containsString:@"pkg/util.py"];
    });
    MCGuiStep(@"and the file in it", ^{ ClickRow(@"pkg/util.py"); });
    MCGuiWait(@"shows in a tab of the page's own", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@"pkg/util.py"] && [gSeen[@"text"] hasPrefix:@"def helper"];
    });
    MCGuiStep(@"and not in the window, which opened nothing", ^{
        MCGuiNote(@"page tabs: %@", gSeen[@"tabs"]);
        GCHECK(MCTabNamed(gC, @"util.py") == nil);
        GCHECK([gC tabs].count == tabsBefore);
        GCHECK([[gSeen[@"tabs"] componentsSeparatedByString:@"|"] containsObject:@"+util.py"]);
        CloseTab(@"util.py");
    });
    MCGuiWait(@"its close button closes it on the page, back to the window's tab", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && ![gSeen[@"tabs"] containsString:@"util.py"] &&
               [gSeen[@"text"] isEqualToString:TV(gC).string];
    });
    MCGuiStep(@"Shift Command F in the read-only page: Find in Folder in its sidebar", ^{
        PageKey(@"f", @"{metaKey: true, shiftKey: true}");
    });
    MCGuiWait(@"the sidebar shows Search", 20, ^BOOL {
        Read();
        return [Seen(@"search")[@"mode"] boolValue];
    });
    MCGuiStep(@"a query typed into it", ^{ FieldText(@"sq", @"return", @"Enter"); });
    MCGuiWait(@"the host's rows, grouped by file, a line of context either side, the matches marked", 20, ^BOOL {
        Read();
        NSDictionary *s = Seen(@"search");
        return [s[@"done"] boolValue] && [s[@"files"] count] == 2 &&
               [[s[@"files"] lastObject] isEqualToString:@"pkg/util.py 1"];
    });
    MCGuiStep(@"what the rows show; then the second file's row clicked", ^{
        NSDictionary *s = Seen(@"search");
        MCGuiNote(@"search: %@ / %@ / %@", s[@"summary"], s[@"files"], s[@"rows"]);
        GCHECK([[s[@"files"] firstObject] hasPrefix:@"calc.py "]);
        GCHECK_EQ_STR([s[@"rows"] lastObject], @" 1 def helper():\n>2 return 42");
        GCHECK([s[@"marks"] count] >= 2 && [[s[@"marks"] lastObject] isEqualToString:@"return"]);
        GCHECK([s[@"summary"] hasSuffix:@"in 2 files"]);
        Eval(@"(() => { const r = [...document.querySelectorAll('#slist .srow')].pop(); if (r) r.click(); return !!r; })()");
    });
    MCGuiWait(@"opens its file in a tab of the page's own, at that line", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@"pkg/util.py"] && [gSeen[@"hit"] intValue] == 1 &&
               [gSeen[@"caret"] intValue] == 1;
    });
    MCGuiStep(@"the window opened nothing; the tab closed, and back to Files", ^{
        GCHECK(MCTabNamed(gC, @"util.py") == nil);
        GCHECK([gC tabs].count == tabsBefore);
        CloseTab(@"util.py");
        Eval(@"(() => { document.getElementById('filesbtn').click(); return 1; })()");
    });
    MCGuiWait(@"the window's tab again, and the tree", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && ![Seen(@"search")[@"mode"] boolValue] &&
               [gSeen[@"text"] isEqualToString:TV(gC).string];
    });
    MCGuiStep(@"a picture clicked in the read-only page's tree", ^{ ClickRow(@"logo.png"); });
    MCGuiWait(@"shows in a tab of the page's own, read through the host", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"file"] isKindOfClass:NSDictionary.class] ? gSeen[@"file"] : nil;
        return [gSeen[@"local"] isEqualToString:@"logo.png"] && [f[@"view"] isEqualToString:@"image"] &&
               [f[@"width"] intValue] == 1;
    });
    MCGuiStep(@"and the window opened nothing; its close button closes it", ^{
        GCHECK([gC tabs].count == tabsBefore);
        GCHECK(![Active(gC).currentPath hasSuffix:@"/logo.png"]);
        CloseTab(@"logo.png");
    });
    MCGuiWait(@"back to the window's tab, the picture gone", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && gSeen[@"file"] == NSNull.null &&
               [gSeen[@"text"] isEqualToString:TV(gC).string];
    });
    // A notebook and a database (R3's second slice), with hostile cells,
    // tables and columns: rendered by the core, shown as text, run nowhere.
    NSString *nbPath = [dir stringByAppendingPathComponent:@"lab.ipynb"];
    NSString *dbPath = [dir stringByAppendingPathComponent:@"app.sqlite"];
    __block NSData *dbBefore = nil;
    MCGuiStep(@"a notebook and a database in the folder; the read-only page's tree refreshed", ^{
        MCGuiWriteFile(nbPath, @"{\"nbformat\": 4, \"nbformat_minor\": 5, \"metadata\": {}, \"cells\": ["
            @"{\"cell_type\": \"markdown\", \"metadata\": {}, \"source\": [\"# Lab results\\n\", "
            @"\"<img src=x onerror=\\\"window.pwned=1\\\"><script>window.pwned=2</script>\"]},"
            @"{\"cell_type\": \"code\", \"execution_count\": 1, \"metadata\": {}, \"source\": [\"print(1)\"],"
            @" \"outputs\": [{\"output_type\": \"stream\", \"name\": \"stdout\","
            @" \"text\": [\"</pre><script>window.pwned=3</script>\\n\"]}]}]}");
        [NSFileManager.defaultManager removeItemAtPath:dbPath error:nil];
        sqlite3 *db = nullptr;
        GCHECK(sqlite3_open(dbPath.fileSystemRepresentation, &db) == SQLITE_OK);
        std::string sql = "CREATE TABLE \"<img src=x onerror=window.pwned=4>\"(\"\"\"><script>window.pwned=5</script>\" TEXT);"
                          "INSERT INTO \"<img src=x onerror=window.pwned=4>\" VALUES ('<b onclick=window.pwned=6>bold</b>');"
                          "CREATE TABLE items(n INTEGER, name TEXT);";
        for (int i = 1; i <= 150; i++)
            sql += "INSERT INTO items VALUES (" + std::to_string(i) + ", 'item " + std::to_string(i) + "');";
        GCHECK(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        dbBefore = [NSData dataWithContentsOfFile:dbPath];
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"notes.md"], @"# Notes\n\nSome *text*.\n");
        Eval(@"(() => { document.getElementById('refresh').click(); return 1; })()");
    });
    MCGuiWait(@"the tree lists them", 20, ^BOOL {
        Read();
        return [gSeen[@"tree"] containsString:@"lab.ipynb"] && [gSeen[@"tree"] containsString:@"app.sqlite"] &&
               [gSeen[@"tree"] containsString:@"notes.md"];
    });
    MCGuiStep(@"the notebook clicked in the tree", ^{ ClickRow(@"lab.ipynb"); });
    MCGuiWait(@"shows as the core's page in a frame sandboxed with no scripts", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"file"] isKindOfClass:NSDictionary.class] ? gSeen[@"file"] : nil;
        return [gSeen[@"local"] isEqualToString:@"lab.ipynb"] && [f[@"view"] isEqualToString:@"notebook"] &&
               [f[@"html"] containsString:@"Lab results</h1>"];
    });
    MCGuiStep(@"its hostile cells escaped, nothing run, the window opening nothing", ^{
        NSDictionary *f = gSeen[@"file"];
        GCHECK_EQ_STR(f[@"sandbox"], @"");
        GCHECK(![f[@"html"] containsString:@"<script"]);
        GCHECK(![f[@"html"] containsString:@"onerror="]);
        GCHECK([f[@"html"] containsString:@"&lt;/pre&gt;&lt;script&gt;window.pwned=3"]);
        GCHECK_EQ_STR(gSeen[@"pwned"], @"undefined");
        GCHECK([gC tabs].count == tabsBefore);
        GCHECK(![Active(gC).currentPath hasSuffix:@"/lab.ipynb"]);
        ClickRow(@"app.sqlite");
    });
    MCGuiWait(@"the database shows its tables, the first one's rows, hostile names as text", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"file"] isKindOfClass:NSDictionary.class] ? gSeen[@"file"] : nil;
        return [gSeen[@"local"] isEqualToString:@"app.sqlite"] && [f[@"view"] isEqualToString:@"database"] &&
               [f[@"rows"] count] == 1;
    });
    MCGuiStep(@"its tables, columns and cells as the database has them", ^{
        NSDictionary *f = gSeen[@"file"];
        MCGuiNote(@"database: %@", f);
        GCHECK(([f[@"tables"] isEqual:@[ @"<img src=x onerror=window.pwned=4>", @"items" ]]));
        GCHECK(([f[@"columns"] isEqual:@[ @"\"><script>window.pwned=5</script>" ]]));
        GCHECK_EQ_STR(f[@"rows"][0], @"<b onclick=window.pwned=6>bold</b>");
        GCHECK_EQ_STR(gSeen[@"pwned"], @"undefined");
        Eval(@"(() => { const s = document.getElementById('dbtable'); s.value = 'items';"
             @" s.dispatchEvent(new Event('change')); return 1; })()");
    });
    MCGuiWait(@"another table picked: its first hundred rows", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"file"] isKindOfClass:NSDictionary.class] ? gSeen[@"file"] : nil;
        return [f[@"table"] isEqualToString:@"items"] && [f[@"rows"] count] == 100;
    });
    MCGuiStep(@"rows 1 to 100, then Next", ^{
        NSDictionary *f = gSeen[@"file"];
        GCHECK_EQ_STR(f[@"rows"][0], @"1,item 1");
        GCHECK_EQ_STR(f[@"where"], @"Rows 1–100, more after");
        Eval(@"(() => { const b = [...document.querySelectorAll('#dbbar button')].find(x => x.textContent === 'Next');"
             @" if (b) b.click(); return !!b; })()");
    });
    MCGuiWait(@"the next page: rows 101 to 150", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"file"] isKindOfClass:NSDictionary.class] ? gSeen[@"file"] : nil;
        return [f[@"rows"] count] == 50 && [f[@"rows"][0] isEqualToString:@"101,item 101"];
    });
    MCGuiStep(@"the database unchanged and the window untouched; both tabs closed", ^{
        GCHECK_EQ_STR(gSeen[@"file"][@"where"], @"Rows 101–150");
        GCHECK([[NSData dataWithContentsOfFile:dbPath] isEqualToData:dbBefore]);
        GCHECK_EQ_STR(gSeen[@"pwned"], @"undefined");
        GCHECK([gC tabs].count == tabsBefore);
        CloseTab(@"app.sqlite");
        CloseTab(@"lab.ipynb");
    });
    MCGuiWait(@"back to the window's tab", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && gSeen[@"file"] == NSNull.null &&
               ![gSeen[@"tabs"] containsString:@"app.sqlite"] && ![gSeen[@"tabs"] containsString:@"lab.ipynb"];
    });
    MCGuiStep(@"a Markdown file clicked in the read-only page's tree", ^{ ClickRow(@"notes.md"); });
    MCGuiWait(@"shows as its page, with Show Source", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@"notes.md"] && [gSeen[@"mdShown"] boolValue] &&
               [gSeen[@"mode"] isEqualToString:@"Show Source"];
    });
    MCGuiStep(@"Show Source", ^{
        Eval(@"(() => { document.getElementById('modebtn').click(); return 1; })()");
    });
    MCGuiWait(@"its text, and Show Page to go back", 20, ^BOOL {
        Read();
        return ![gSeen[@"mdShown"] boolValue] && [gSeen[@"lines"] hasPrefix:@"# Notes"] &&
               [gSeen[@"mode"] isEqualToString:@"Show Page"];
    });
    MCGuiStep(@"Show Page, then its close button", ^{
        Eval(@"(() => { document.getElementById('modebtn').click(); return 1; })()");
    });
    MCGuiWait(@"the page again", 20, ^BOOL {
        Read();
        return [gSeen[@"mdShown"] boolValue] && [gSeen[@"mode"] isEqualToString:@"Show Source"];
    });
    MCGuiStep(@"closed", ^{ CloseTab(@"notes.md"); });
    MCGuiWait(@"back to the window's tab once more", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && ![gSeen[@"tabs"] containsString:@"notes.md"];
    });
    // A LaTeX document (R3's third slice): the window's preview's PDF on
    // the page, and a new one when the window typesets again.
    NSString *texPath = [dir stringByAppendingPathComponent:@"paper.tex"];
    NSString *soloPath = [dir stringByAppendingPathComponent:@"solo.tex"];
    NSString *alphaTex = @"\\documentclass{article}\\begin{document}ALPHA\\end{document}\n";
    NSString *betaTex = @"\\documentclass{article}\\begin{document}BETA\\end{document}\n";
    NSString *soloTex = @"\\documentclass{article}\\begin{document}SOLO\\end{document}\n";
    __block MCTab *texTab = nil;
    __block NSData *firstPdf = nil, *secondPdf = nil;
    __block NSString *firstSrc = nil;
    __block NSUInteger texTabsBefore = 0;
    MCGuiStep(@"a stand-in tectonic and two LaTeX documents; the window opens one", ^{
        NSString *pdfs = [MCGuiScratch() stringByAppendingPathComponent:@"typeset-pdfs"];
        [NSFileManager.defaultManager createDirectoryAtPath:pdfs withIntermediateDirectories:YES
                                                  attributes:nil error:nil];
        // Six pages each, the second typeset's taller: the same scroll
        // offset would be another place in it.
        [PagesPDF(@"ALPHA", 6, 400) writeToFile:[pdfs stringByAppendingPathComponent:@"alpha.pdf"] atomically:YES];
        [PagesPDF(@"BETA", 6, 560) writeToFile:[pdfs stringByAppendingPathComponent:@"beta.pdf"] atomically:YES];
        [WordPDF(@"SOLO") writeToFile:[pdfs stringByAppendingPathComponent:@"solo.pdf"] atomically:YES];
        // "Typesets" by copying the PDF its text names.
        NSString *script = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"out=''; prev=''; input=''\n"
            @"for a in \"$@\"; do [ \"$prev\" = --outdir ] && out=\"$a\"; prev=\"$a\"; input=\"$a\"; done\n"
            @"stem=$(basename \"$input\" .tex)\n"
            @"if grep -q ALPHA \"$input\"; then cp '%@/alpha.pdf' \"$out/$stem.pdf\";\n"
            @"elif grep -q BETA \"$input\"; then cp '%@/beta.pdf' \"$out/$stem.pdf\";\n"
            @"else cp '%@/solo.pdf' \"$out/$stem.pdf\"; fi\n",
            pdfs, pdfs, pdfs];
        NSString *tool = [MCGuiScratch() stringByAppendingPathComponent:@"typeset-tectonic"];
        MCGuiWriteFile(tool, script);
        chmod(tool.fileSystemRepresentation, 0755);
        MCSetEnv("MUTIBLE_LATEX_WORK", [MCGuiScratch() stringByAppendingPathComponent:@"typeset-work"].UTF8String);
        MCSetEnv("MUTIBLE_TECTONIC", tool.UTF8String);
        GCHECK_EQ_STR(MCTectonicPath(), tool);
        MCGuiWriteFile(texPath, alphaTex);
        MCGuiWriteFile(soloPath, soloTex);
        texTabsBefore = [gC tabs].count;
        [gC openFileAtPath:texPath];
        texTab = MCTabNamed(gC, @"paper.tex");
        GCHECK(texTab != nil && texTab.isLatex && texTab.previewMode && texTab.latex != nil && texTab.session != nil);
        Eval(@"(() => { document.getElementById('refresh').click(); return 1; })()");
    });
    MCGuiWait(@"the window's preview typesets it", 20, ^BOOL {
        return texTab.latex.typesetPdf != nil && !texTab.latex.typesetting;
    });
    MCGuiWait(@"the read-only page follows the window to it and shows its pages", 30, ^BOOL {
        Read();
        NSDictionary *x = SeenTypeset();
        return [x[@"state"] isEqualToString:@"ready"] && [x[@"src"] containsString:@"/typeset/paper.tex?v="] &&
               [x[@"pagesShown"] boolValue] && [x[@"pages"] intValue] == 6 && ![x[@"frame"] boolValue] &&
               [x[@"shown"] boolValue] && [gSeen[@"mode"] isEqualToString:@"Show Source"];
    });
    MCGuiStep(@"the page fetches its frame's address", ^{
        MCGuiNote(@"typeset: %@", SeenTypeset());
        GCHECK_EQ_STR(gSeen[@"local"], @"");   // the window's tab, not one of the page's own
        firstPdf = texTab.latex.typesetPdf;
        firstSrc = SeenTypeset()[@"src"];
        FetchTypeset();
    });
    MCGuiWait(@"fetched", 20, ^BOOL {
        Read();
        return FetchedShown();
    });
    MCGuiStep(@"the bytes are the window's PDF; then Show Source", ^{
        NSDictionary *f = SeenFetch();
        MCGuiNote(@"fetched: %@", f);
        GCHECK([f[@"status"] intValue] == 200);
        GCHECK_EQ_STR(f[@"type"], @"application/pdf");
        GCHECK([f[@"length"] unsignedIntegerValue] == firstPdf.length);
        GCHECK([f[@"sum"] unsignedIntValue] == ByteSum(firstPdf));
        NSArray *srcs = SeenTypeset()[@"pageSrcs"];
        GCHECK(srcs.count == 6 && [srcs.firstObject containsString:@"/typeset-page/paper.tex?v="] &&
               [srcs.firstObject containsString:@"&p=1&w="]);
        FetchPage();
    });
    MCGuiWait(@"the first page's picture fetched", 20, ^BOOL {
        Read();
        NSDictionary *f = [gSeen[@"pageFetched"] isKindOfClass:NSDictionary.class] ? gSeen[@"pageFetched"] : nil;
        return f != nil;
    });
    MCGuiStep(@"a PNG the app drew; then Show Source", ^{
        NSDictionary *f = gSeen[@"pageFetched"];
        MCGuiNote(@"page fetched: %@", f);
        GCHECK([f[@"status"] intValue] == 200);
        GCHECK_EQ_STR(f[@"type"], @"image/png");
        GCHECK([f[@"length"] intValue] > 100);
        NSArray *head = f[@"head"];
        GCHECK(head.count == 4 && [head[0] intValue] == 0x89 && [head[1] intValue] == 'P' &&
               [head[2] intValue] == 'N' && [head[3] intValue] == 'G');
        Eval(@"(() => { document.getElementById('modebtn').click(); return 1; })()");
    });
    MCGuiWait(@"its text, and Show Page to go back", 20, ^BOOL {
        Read();
        return ![SeenTypeset()[@"shown"] boolValue] && [gSeen[@"lines"] hasPrefix:@"\\documentclass"] &&
               [gSeen[@"lines"] containsString:@"ALPHA"] && [gSeen[@"mode"] isEqualToString:@"Show Page"];
    });
    MCGuiStep(@"Show Page", ^{
        Eval(@"(() => { document.getElementById('modebtn').click(); return 1; })()");
    });
    MCGuiWait(@"the pages again", 20, ^BOOL {
        Read();
        NSDictionary *x = SeenTypeset();
        return [x[@"pagesShown"] boolValue] && [x[@"pages"] intValue] == 6;
    });
    // The reader's place (the owner's ask): halfway into the third page.
    MCGuiStep(@"scrolled halfway into the third page", ^{
        Eval(@"(() => { window.mcSessionView.texScroll(2, 0.5); return 1; })()");
    });
    __block double scrolledTop = 0;
    MCGuiWait(@"the page is on its third page", 20, ^BOOL {
        Read();
        NSDictionary *place = [SeenTypeset()[@"place"] isKindOfClass:NSDictionary.class] ? SeenTypeset()[@"place"] : nil;
        return [place[@"page"] intValue] == 2 && fabs([place[@"frac"] doubleValue] - 0.5) < 0.02;
    });
    MCGuiStep(@"an agent rewrites the file, and the window reads it", ^{
        scrolledTop = [SeenTypeset()[@"scrollTop"] doubleValue];
        MCGuiNote(@"at %@, scrollTop %.0f", SeenTypeset()[@"place"], scrolledTop);
        GCHECK(scrolledTop > 0);
        GCHECK(!texTab.dirty);
        MCGuiWriteFile(texPath, betaTex);
        [gC checkTabsOnDisk];
    });
    MCGuiWait(@"the window typesets the new text, and the page's pictures have a new address", 30, ^BOOL {
        Read();
        NSDictionary *x = SeenTypeset();
        NSData *now = texTab.latex.typesetPdf;
        return now && ![now isEqualToData:firstPdf] && !texTab.latex.typesetting &&
               [x[@"state"] isEqualToString:@"ready"] && [x[@"shown"] boolValue] &&
               [x[@"src"] containsString:@"/typeset/paper.tex?v="] && ![x[@"src"] isEqualToString:firstSrc] &&
               [x[@"pagesShown"] boolValue] && [[x[@"pageSrcs"] firstObject] containsString:
                   [x[@"src"] substringFromIndex:[x[@"src"] rangeOfString:@"?v="].location]];
    });
    MCGuiStep(@"still halfway into the third page, though its pages are taller; fetched again", ^{
        NSDictionary *x = SeenTypeset();
        NSDictionary *place = [x[@"place"] isKindOfClass:NSDictionary.class] ? x[@"place"] : nil;
        MCGuiNote(@"after the new typeset: %@, scrollTop %@ (was %.0f)", place, x[@"scrollTop"], scrolledTop);
        GCHECK([x[@"pages"] intValue] == 6);
        GCHECK([place[@"page"] intValue] == 2);
        GCHECK(fabs([place[@"frac"] doubleValue] - 0.5) < 0.02);
        GCHECK([x[@"scrollTop"] doubleValue] > scrolledTop + 50);   // moved to keep the place
        secondPdf = texTab.latex.typesetPdf;
        FetchTypeset();
    });
    MCGuiWait(@"the new bytes", 20, ^BOOL {
        Read();
        return FetchedShown();
    });
    MCGuiStep(@"they are the window's new PDF; a document the window has not open, clicked in the tree", ^{
        NSDictionary *f = SeenFetch();
        MCGuiNote(@"fetched: %@", f);
        GCHECK([f[@"length"] unsignedIntegerValue] == secondPdf.length);
        GCHECK([f[@"sum"] unsignedIntValue] == ByteSum(secondPdf));
        GCHECK([gSeen[@"tree"] containsString:@"solo.tex"]);
        texTabsBefore = [gC tabs].count;
        ClickRow(@"solo.tex");
    });
    MCGuiWait(@"in a tab of the page's own, typeset by the server's own preview", 30, ^BOOL {
        Read();
        NSDictionary *x = SeenTypeset();
        return [gSeen[@"local"] isEqualToString:@"solo.tex"] && [x[@"state"] isEqualToString:@"ready"] &&
               [x[@"src"] containsString:@"/typeset/solo.tex?v="] && [x[@"pagesShown"] boolValue] &&
               [x[@"pages"] intValue] == 1;
    });
    MCGuiStep(@"the window opened nothing; fetched", ^{
        GCHECK([gC tabs].count == texTabsBefore);
        GCHECK(MCTabNamed(gC, @"solo.tex") == nil);
        FetchTypeset();
    });
    MCGuiWait(@"its bytes", 20, ^BOOL {
        Read();
        return FetchedShown();
    });
    MCGuiStep(@"the stand-in's PDF; the file unchanged and nothing left beside it; both closed", ^{
        NSDictionary *f = SeenFetch();
        NSData *solo = [NSData dataWithContentsOfFile:
            [[MCGuiScratch() stringByAppendingPathComponent:@"typeset-pdfs"] stringByAppendingPathComponent:@"solo.pdf"]];
        GCHECK([f[@"length"] unsignedIntegerValue] == solo.length && [f[@"sum"] unsignedIntValue] == ByteSum(solo));
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:soloPath encoding:NSUTF8StringEncoding error:nil], soloTex);
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil])
            GCHECK(![name hasSuffix:@".mutible.tex"]);
        CloseTab(@"solo.tex");
        GCHECK([gC closeTabEverywhere:texTab prompt:YES]);
        MCSetEnv("MUTIBLE_TECTONIC", nullptr);
        MCSetEnv("MUTIBLE_LATEX_WORK", nullptr);
    });
    MCGuiWait(@"back to the window's tab, the pages gone", 20, ^BOOL {
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && ![SeenTypeset()[@"shown"] boolValue] &&
               ![gSeen[@"tabs"] containsString:@"solo.tex"] && ![gSeen[@"tabs"] containsString:@"paper.tex"];
    });
    // Source Control in the page (R3's fourth slice): the folder becomes a
    // repository with changes; the read-only page lists them and shows
    // their review, and follows an agent's edit with no reload.
    NSString *revPath = [dir stringByAppendingPathComponent:@"review_me.py"];
    __block NSString *git = nil;
    __block NSString *headBefore = nil, *stagedBefore = nil;
    NSString *(^revLines)(NSString *, NSString *) = ^NSString *(NSString *second, NSString *last) {
        NSMutableString *t = [NSMutableString string];
        for (int i = 1; i <= 40; i++) {
            if (i == 2) [t appendFormat:@"%@\n", second];
            else if (i == 39) [t appendFormat:@"%@\n", last];
            else [t appendFormat:@"value_%d = compute(%d)\n", i, i];
        }
        return t;
    };
    MCGuiStep(@"files to commit", ^{
        git = MCGitExecutable();
        if (!git) MCGuiNote(@"no git here: the Source Control checks are skipped");
        MCGuiWriteFile(revPath, revLines(@"value_2 = compute(2)", @"value_39 = compute(39)"));
        MCGuiWriteFile([dir stringByAppendingPathComponent:@"staged.txt"], @"one\n");
    });
    MCGuiAsync(@"git init, add, commit", 30, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        NSString *sh = [NSString stringWithFormat:
            @"cd '%@' && '%@' init -q && '%@' add -A && '%@' -c commit.gpgsign=false commit -qm first && "
            @"printf 'two\\n' > staged.txt && '%@' add staged.txt && printf 'fresh\\n' > fresh.txt",
            dir, git, git, git, git];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int st, NSString *, NSString *err) {
            GCHECK(st == 0);
            if (st) MCGuiNote(@"git: %@", err);
            done();
        });
    });
    MCGuiStep(@"a change of two lines far apart; Source Control in the page's sidebar", ^{
        if (!git) return;
        MCGuiWriteFile(revPath, revLines(@"value_2 = compute(2) + offset", @"value_39 = compute(39) * 2"));
        headBefore = [[NSString alloc] initWithData:MCGitRunSync(dir, @[@"rev-parse", @"HEAD"]).output
                                           encoding:NSUTF8StringEncoding];
        stagedBefore = [[NSString alloc] initWithData:MCGitRunSync(dir, @[@"diff", @"--cached", @"--name-only"]).output
                                             encoding:NSUTF8StringEncoding];
        GCHECK_EQ_STR(stagedBefore, @"staged.txt\n");
        Eval(@"(() => { document.getElementById('scmbtn').click(); return 1; })()");
    });
    MCGuiWait(@"the page lists the changes, staged first, as the window's Source Control does", 30, ^BOOL {
        if (!git) return YES;
        Read();
        NSArray *rows = [gSeen[@"scm"] isKindOfClass:NSDictionary.class] ? gSeen[@"scm"][@"rows"] : nil;
        return [rows containsObject:@"M staged.txt"] && [rows containsObject:@"M review_me.py"] &&
               [rows containsObject:@"U fresh.txt"] && [rows.firstObject isEqualToString:@"M staged.txt"];
    });
    MCGuiStep(@"review_me.py clicked in the list", ^{
        if (!git) return;
        MCGuiNote(@"source control: %@", gSeen[@"scm"]);
        GCHECK([gSeen[@"scm"][@"mode"] boolValue]);
        Eval(@"(() => { const r = [...document.querySelectorAll('#gitlist .row')]"
             @".find(x => x.dataset.path === 'review_me.py'); if (r) r.click(); return !!r; })()");
    });
    // The review_me.py file of the review on show, or nil.
    NSDictionary *(^revFile)(void) = ^NSDictionary *{
        NSDictionary *rv = [gSeen[@"review"] isKindOfClass:NSDictionary.class] ? gSeen[@"review"] : nil;
        for (NSDictionary *f in rv[@"files"])
            if ([f[@"head"] hasPrefix:@"review_me.py"]) return f;
        return nil;
    };
    BOOL (^rowsHave)(NSDictionary *, NSString *) = ^BOOL(NSDictionary *f, NSString *text) {
        for (NSString *r in f[@"rows"])
            if ([r containsString:text]) return YES;
        return NO;
    };
    MCGuiWait(@"the review shows in a tab of the page's own, at that file", 30, ^BOOL {
        if (!git) return YES;
        Read();
        NSDictionary *f = revFile();
        return [gSeen[@"review"][@"shown"] boolValue] && [gSeen[@"local"] isEqualToString:@"/changes"] &&
               rowsHave(f, @"value_2 = compute(2) + offset") && rowsHave(f, @"value_39 = compute(39) * 2");
    });
    MCGuiStep(@"removed and added lines, the words marked, colored, read only; then Side by Side", ^{
        if (!git) return;
        NSDictionary *rv = gSeen[@"review"], *f = revFile();
        MCGuiNote(@"review_me.py: %@", f[@"rows"]);
        GCHECK([gSeen[@"tabs"] containsString:@"+Changes"]);
        GCHECK(rowsHave(f, @"r|2||value_2 = compute(2)"));
        GCHECK(rowsHave(f, @"a||2|value_2 = compute(2) + offset"));
        GCHECK(rowsHave(f, @"gap|"));   // the unchanged middle folded
        GCHECK([rv[@"words"] intValue] >= 2 && [rv[@"colored"] intValue] > 0);
        // Only Side by Side: nothing on the page stages, discards or commits.
        GCHECK([[NSSet setWithArray:rv[@"buttons"]] isEqual:[NSSet setWithObject:@"Side by Side"]]);
        GCHECK([gC tabs].count == tabsBefore);   // the window opened nothing
        Eval(@"(() => { const f = [...document.querySelectorAll('#review .rfile')]"
             @".find(x => x.querySelector('.rpath').textContent.startsWith('review_me.py'));"
             @" const b = f && f.querySelector('.rhead button'); if (b) b.click(); return !!b; })()");
    });
    MCGuiWait(@"the file side by side, its changed pair on one row", 20, ^BOOL {
        if (!git) return YES;
        Read();
        NSDictionary *f = revFile();
        return [f[@"side"] boolValue] && rowsHave(f, @"sm|2|value_2 = compute(2)|2|value_2 = compute(2) + offset");
    });
    __block NSUInteger rowsFolded = 0;
    MCGuiStep(@"the unchanged stretch clicked open", ^{
        if (!git) return;
        rowsFolded = [revFile()[@"rows"] count];
        Eval(@"(() => { const f = [...document.querySelectorAll('#review .rfile')]"
             @".find(x => x.querySelector('.rpath').textContent.startsWith('review_me.py'));"
             @" const g = f && f.querySelector('tr.gap'); if (g) g.click(); return !!g; })()");
    });
    MCGuiWait(@"every line shows, still side by side", 20, ^BOOL {
        if (!git) return YES;
        Read();
        NSDictionary *f = revFile();
        return [f[@"side"] boolValue] && !rowsHave(f, @"gap|") && [f[@"rows"] count] > rowsFolded &&
               rowsHave(f, @"value_20 = compute(20)");
    });
    MCGuiStep(@"an agent edits the file on disk; nobody presses anything", ^{
        if (!git) return;
        MCGuiWriteFile(revPath, revLines(@"value_2 = compute(2) + offset", @"value_39 = AGENT_EDIT(39)"));
    });
    MCGuiWait(@"the page's review follows, keeping its side by side and its open stretch", 30, ^BOOL {
        if (!git) return YES;
        Read();
        NSDictionary *f = revFile();
        return rowsHave(f, @"AGENT_EDIT") && [f[@"side"] boolValue] && !rowsHave(f, @"gap|");
    });
    MCGuiStep(@"the repository as it was; the review closed and the tree back", ^{
        if (!git) return;
        NSString *head = [[NSString alloc] initWithData:MCGitRunSync(dir, @[@"rev-parse", @"HEAD"]).output
                                               encoding:NSUTF8StringEncoding];
        NSString *staged = [[NSString alloc] initWithData:MCGitRunSync(dir, @[@"diff", @"--cached", @"--name-only"]).output
                                                 encoding:NSUTF8StringEncoding];
        GCHECK_EQ_STR(head, headBefore);
        GCHECK_EQ_STR(staged, stagedBefore);
        GCHECK([[NSString stringWithContentsOfFile:revPath encoding:NSUTF8StringEncoding error:nil]
            containsString:@"AGENT_EDIT"]);
        CloseTab(@"Changes");
        Eval(@"(() => { document.getElementById('filesbtn').click(); return 1; })()");
    });
    MCGuiWait(@"back to the window's tab and the tree", 20, ^BOOL {
        if (!git) return YES;
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && ![gSeen[@"tabs"] containsString:@"+Changes"] &&
               ![gSeen[@"scm"][@"mode"] boolValue] && [gSeen[@"tree"] containsString:@"review_me.py"];
    });
    MCGuiStep(@"what the page shows after Source Control", ^{
        MCGuiNote(@"local '%@', tabs %@, source control %@, tree %@", gSeen[@"local"], gSeen[@"tabs"],
                  gSeen[@"scm"][@"mode"], gSeen[@"tree"]);
    });
    // The window's commit page and an agent worktree's page (R3's last
    // slice): the read-only page lists both and shows each, read-only, with
    // a picture's two versions decoded from the server's /picture/ address.
    NSString *wtPath = [dir.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"session-wt"];
    NSData *picSmall = GrayPng(2, 1), *picBig = GrayPng(4, 3), *picShot = GrayPng(3, 3);
    __block NSString *commitHash = nil, *mainBranch = nil;
    __block MCChangeReviewPage *worktreePage = nil;
    __block NSUInteger tabsWithPages = 0;
    MCGuiStep(@"a picture committed, then a bigger one with a change", ^{
        if (!git) return;
        [picSmall writeToFile:[dir stringByAppendingPathComponent:@"pic.png"] atomically:YES];
    });
    MCGuiAsync(@"two commits and an agent's worktree with a new picture", 30, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        NSString *sh = [NSString stringWithFormat:
            @"cd '%@' && '%@' add -A && '%@' -c commit.gpgsign=false commit -qm pictures && "
            @"cp '%@/pic-big.png' pic.png && rm '%@/pic-big.png' && "
            @"printf 'value_2 = compute(2) + COMMITTED\\n' >> review_me.py && "
            @"'%@' -c commit.gpgsign=false commit -qam 'Bigger picture' && "
            @"'%@' worktree add -q -b agent-gui '%@' HEAD && "
            @"cd '%@' && printf 'agent work\\n' >> review_me.py && cp '%@/shot.png' shot.png",
            dir, git, git, dir.stringByDeletingLastPathComponent, dir.stringByDeletingLastPathComponent, git, git,
            wtPath, wtPath, dir.stringByDeletingLastPathComponent];
        [picBig writeToFile:[dir.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pic-big.png"]
                 atomically:YES];
        [picShot writeToFile:[dir.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"shot.png"]
                  atomically:YES];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int st, NSString *, NSString *err) {
            GCHECK(st == 0);
            if (st) MCGuiNote(@"git: %@", err);
            done();
        });
    });
    MCGuiStep(@"the window opens the commit's page and the worktree's", ^{
        if (!git) return;
        NSString *(^out)(NSArray *) = ^NSString *(NSArray *args) {
            NSString *s = [[NSString alloc] initWithData:MCGitRunSync(dir, args).output encoding:NSUTF8StringEncoding];
            return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        };
        commitHash = out(@[@"rev-parse", @"HEAD"]);
        mainBranch = out(@[@"rev-parse", @"--abbrev-ref", @"HEAD"]);
        GCHECK(commitHash.length == 40);
        [gC showCommitReview:commitHash title:@"Bigger picture" preview:NO];
        worktreePage = [[MCChangeReviewPage alloc] initForWorktree:wtPath label:@"agent-gui" branch:@"agent-gui"
                                                           mainRef:mainBranch];
        [gC showPage:worktreePage preview:NO];
        [worktreePage reload];
        tabsWithPages = [gC tabs].count;
        MCGuiNote(@"window tabs with the pages: %lu (before %lu)", (unsigned long)tabsWithPages,
                  (unsigned long)tabsBefore);
    });
    NSString *wtTitle = @"session-wt vs ";   // the worktree page's title, then the main branch
    MCGuiWait(@"the read-only page lists both of the window's pages in its tab strip", 20, ^BOOL {
        if (!git) return YES;
        Read();
        return [gSeen[@"tabs"] containsString:@"Bigger picture"] && [gSeen[@"tabs"] containsString:wtTitle];
    });
    MCGuiStep(@"the commit's tab clicked in the page", ^{
        if (!git) return;
        MCGuiNote(@"tabs %@", gSeen[@"tabs"]);
        GCHECK(![gSeen[@"tabs"] containsString:@"+Bigger picture"]);   // the window's, not the page's own
        ClickTab(@"Bigger picture");
    });
    // The file of the review on show whose header starts with `name`.
    NSDictionary *(^shownFile)(NSString *) = ^NSDictionary *(NSString *name) {
        NSDictionary *rv = [gSeen[@"review"] isKindOfClass:NSDictionary.class] ? gSeen[@"review"] : nil;
        for (NSDictionary *f in rv[@"files"])
            if ([f[@"head"] hasPrefix:name]) return f;
        return nil;
    };
    BOOL (^decoded)(NSDictionary *, NSString *, int) = ^BOOL(NSDictionary *f, NSString *side, int width) {
        for (NSDictionary *i in f[@"images"])
            if ([i[@"side"] isEqualToString:side]) return [i[@"width"] intValue] == width;
        return NO;
    };
    MCGuiWait(@"the commit's page shows: its header, its change, its picture before and after", 30, ^BOOL {
        if (!git) return YES;
        Read();
        NSDictionary *rv = gSeen[@"review"], *pic = shownFile(@"pic.png"), *text = shownFile(@"review_me.py");
        return [rv[@"shown"] boolValue] && [rv[@"page"] isEqual:[@"commit:" stringByAppendingString:commitHash]] &&
               [rv[@"title"] isEqualToString:@"Bigger picture"] && rowsHave(text, @"COMMITTED") &&
               decoded(pic, @"old", 2) && decoded(pic, @"new", 4);
    });
    MCGuiStep(@"labelled as the window labels them, at the server's addresses, read only", ^{
        if (!git) return;
        NSDictionary *rv = gSeen[@"review"], *pic = shownFile(@"pic.png");
        MCGuiNote(@"commit page: %@ / %@; pictures %@", rv[@"title"], rv[@"detail"], pic[@"images"]);
        NSArray *images = pic[@"images"];
        GCHECK(images.count == 2);
        if (images.count == 2) {
            GCHECK([images[0][@"label"] hasPrefix:@"Before: 2 × 1 pixels"]);
            GCHECK([images[1][@"label"] hasPrefix:@"After: 4 × 3 pixels"]);
            GCHECK([images[0][@"src"] containsString:@"/picture/"] && [images[1][@"src"] hasSuffix:@"/new"]);
        }
        GCHECK([[NSSet setWithArray:rv[@"buttons"]] isSubsetOfSet:[NSSet setWithObject:@"Side by Side"]]);
        GCHECK([gSeen[@"local"] hasPrefix:@"/review/commit:"]);
        GCHECK([gC tabs].count == tabsWithPages);   // the window opened nothing
        ClickTab([wtTitle stringByAppendingString:mainBranch ?: @"main"]);
    });
    MCGuiWait(@"the worktree's page shows: compared with main, the agent's change, its new picture", 30, ^BOOL {
        if (!git) return YES;
        Read();
        NSDictionary *rv = gSeen[@"review"], *shot = shownFile(@"shot.png"), *text = shownFile(@"review_me.py");
        return [rv[@"shown"] boolValue] && [rv[@"page"] isEqual:[@"worktree:" stringByAppendingString:wtPath]] &&
               [rv[@"title"] containsString:@"compared with"] && rowsHave(text, @"agent work") &&
               decoded(shot, @"new", 3);
    });
    MCGuiStep(@"no Apply to Main on the page; the window closes the commit's page", ^{
        if (!git) return;
        NSDictionary *rv = gSeen[@"review"];
        MCGuiNote(@"worktree page: %@ / %@, buttons %@", rv[@"title"], rv[@"detail"], rv[@"buttons"]);
        GCHECK([[NSSet setWithArray:rv[@"buttons"]] isSubsetOfSet:[NSSet setWithObject:@"Side by Side"]]);
        GCHECK(!worktreePage.applied);
        GCHECK([shownFile(@"shot.png")[@"images"] count] == 1);
        MCTab *t = nil;
        for (MCTab *x in [gC groupTabs])
            if ([x.page isKindOfClass:MCChangeReviewPage.class] && ((MCChangeReviewPage *)x.page).commit.length) t = x;
        GCHECK(t != nil);
        if (t) [gC closeTabEverywhere:t prompt:NO];
    });
    MCGuiWait(@"its tab leaves the page; the worktree's page stays on show", 20, ^BOOL {
        if (!git) return YES;
        Read();
        return ![gSeen[@"tabs"] containsString:@"Bigger picture"] && [gSeen[@"review"][@"shown"] boolValue] &&
               [gSeen[@"local"] hasPrefix:@"/review/worktree:"];
    });
    MCGuiStep(@"the window closes the worktree's page", ^{
        if (!git) return;
        MCTab *t = TabOfPage(gC, worktreePage);
        GCHECK(t != nil);
        if (t) [gC closeTabEverywhere:t prompt:NO];
    });
    MCGuiWait(@"the page is back on the window's tab", 20, ^BOOL {
        if (!git) return YES;
        Read();
        return [gSeen[@"local"] isEqualToString:@""] && ![gSeen[@"tabs"] containsString:wtTitle] &&
               ![gSeen[@"review"][@"shown"] boolValue];
    });
    MCGuiAsync(@"the worktree removed; the repository's commits as made", 30, ^(MCGuiDone done) {
        if (!git) { done(); return; }
        worktreePage = nil;
        GCHECK([gC tabs].count == tabsBefore);
        NSString *head = [[[NSString alloc] initWithData:MCGitRunSync(dir, @[@"rev-parse", @"HEAD"]).output
                                                encoding:NSUTF8StringEncoding]
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        GCHECK_EQ_STR(head, commitHash);
        NSString *sh = [NSString stringWithFormat:@"cd '%@' && '%@' worktree remove --force '%@'", dir, git, wtPath];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", sh], nil, ^(int st, NSString *, NSString *err) {
            GCHECK(st == 0);
            if (st) MCGuiNote(@"git: %@", err);
            done();
        });
    });
    MCGuiStep(@"the window closes, and the server with it", ^{
        GCHECK([gC tabs].count == tabsBefore);
        [MCAppDefaults() removeObjectForKey:@"MCSessionBrowserEditable"];
        gPort = gC.sessionBrowser.port;   // the read-only server's
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
