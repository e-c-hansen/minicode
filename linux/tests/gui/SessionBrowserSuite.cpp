// SessionBrowserSuite.cpp — `./build/mutible-guitest session_browser`: View >
// Session Browser (linux/src/SessionBrowser.h; the Mac's tests/gui/
// SessionBrowserSuite.mm) driven as a browser drives it. A plain socket
// client, read by the waits so the main loop keeps turning, fetches the page
// and speaks the page's WebSocket (the core's frame code), each client with
// its own cookie jar as a browser has. What is checked:
//   - Copy Session Browser Link: http://127.0.0.1:PORT/v/ID/?t=TOKEN on the
//     clipboard, a fresh token each time, the one port;
//   - the page: 200 with its cookies; the spent link, a wrong Host and a
//     wrong Origin refused;
//   - a browser not remembered waits ("waiting") while the window's dialog
//     asks; Allow gives hello and a state of this window's tabs only (not
//     another window's); Deny turns the next browser away; Always Allow
//     remembers it (the state file) so its next link connects with no dialog;
//     Remembered Browsers… lists it and Forget All empties the file;
//   - an editable page: open opens a tab, an edit then save changes the
//     file, close closes a clean tab and refuses a dirty one;
//   - Show's Read-Only answer (the opener captures the address): save, open,
//     close and edit do nothing to the window;
//   - a LaTeX document's typeset on the page, with a stand-in tectonic
//     (MUTIBLE_TECTONIC, a script that copies a committed PDF and counts its
//     runs): one no tab holds reaches "ready" with its page count and sizes,
//     its PDF is served and a page is drawn as a PNG of the width asked; one
//     the engine fails on reaches the page as "failed" with its text; with
//     a tab's own preview made the page's typeset is that one (no second
//     run) and the tab's later typesets reach the page; five untabbed
//     documents keep four typesets, the oldest dropped;
//   - the page's Source Control (`git` watch) reads again after a commit
//     made in the folder;
//   - the host watches the disk for the page (a `watch` of the root, the host's
//     watcher settled on more than no folder): a file
//     made in the folder reaches it as a new `tree` listing and the file
//     removed as another, a tracked file changed as a newer `git` message,
//     none asked for;
//   - a worktree's review page open in the window is listed in the state's
//     `reviews` (kind, title, on show), `review` with its `page` gives its
//     files, and closing the tab drops it from the list; so is a commit's
//     page (kind commit, its hash (in the id) and title), whose `review` gives its files;
//   - closing the window stops its server (the port refuses).
// Nothing opens a real browser (SessionBrowser::opener), and the state file,
// the files and the windows are put back.
#include "Harness.h"

#include "ChangeReviewPage.h"
#include "Document.h"
#include "Editor.h"
#include "GitPanel.h"
#include "IdeLink.h"
#include "Latex.h"
#include "Json.h"
#include "SessionBrowser.h"
#include "SessionHost.h"   // diskWatchSettled, for the watch checks

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include <cstring>
#include <set>

namespace {

// ------------------------------------------------------- a test client
struct Conn {
    int fd = -1;
    std::string in, head, body;
    bool upgraded = false, closed = false;
    std::vector<Json> msgs;
    size_t seen = 0;
    uint32_t mask = 7;

    bool dial(int port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) {
            close(fd);
            fd = -1;
            return false;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        return true;
    }
    bool sendRaw(const std::string& bytes) {
        size_t done = 0;
        while (done < bytes.size()) {
            ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
            if (n < 0 && errno == EAGAIN) continue;
            if (n <= 0) return false;
            done += (size_t)n;
        }
        return true;
    }
    void sendJson(const Json& j) {
        sendRaw(IdeLink::encodeClientFrame(IdeLink::Opcode::Text, j.dump(), ++mask * 2654435761u));
    }
    void shut() {
        if (fd >= 0) close(fd);
        fd = -1;
    }
    // Reads what has arrived (call from a wait): the HTTP head, then, after
    // an upgrade, the server's unmasked text frames as parsed JSON.
    void pump() {
        if (fd < 0) return;
        char buf[65536];
        for (;;) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) { in.append(buf, (size_t)n); continue; }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) closed = true;
            break;
        }
        if (head.empty()) {
            const size_t end = in.find("\r\n\r\n");
            if (end == std::string::npos) return;
            head = in.substr(0, end + 4);
            in.erase(0, end + 4);
            upgraded = head.rfind("HTTP/1.1 101 ", 0) == 0;
        }
        if (!upgraded) {
            body += in;
            in.clear();
            return;
        }
        for (;;) {
            if (in.size() < 2) return;
            const unsigned char* p = (const unsigned char*)in.data();
            const int op = p[0] & 0x0F;
            uint64_t len = p[1] & 0x7F;
            size_t at = 2;
            if (len == 126) { if (in.size() < 4) return; len = (uint64_t)p[2] << 8 | p[3]; at = 4; }
            else if (len == 127) {
                if (in.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
                at = 10;
            }
            if (in.size() < at + len) return;
            const std::string payload = in.substr(at, (size_t)len);
            in.erase(0, at + (size_t)len);
            if (op == 1) {
                Json j;
                if (Json::parse(payload, j)) msgs.push_back(j);
            }
        }
    }
    int status() const { return head.size() > 12 ? atoi(head.c_str() + 9) : 0; }
    // The first message of type `t` not yet taken.
    bool take(const std::string& t, Json* out) {
        pump();
        for (size_t i = seen; i < msgs.size(); i++)
            if (msgs[i]["t"].asString() == t) {
                *out = msgs[i];
                seen = i + 1;
                return true;
            }
        return false;
    }
    // The latest message of type `t`.
    bool latest(const std::string& t, Json* out) {
        pump();
        bool found = false;
        for (const Json& m : msgs)
            if (m["t"].asString() == t) { *out = m; found = true; }
        return found;
    }
};

// A browser's cookies, kept as a jar does.
struct Jar {
    std::map<std::string, std::string> cookies;
    std::string header() const {
        std::string h;
        for (const auto& kv : cookies) h += (h.empty() ? "" : "; ") + kv.first + "=" + kv.second;
        return h;
    }
    void take(const std::string& head) {
        size_t at = 0;
        while ((at = head.find("Set-Cookie: ", at)) != std::string::npos) {
            at += 12;
            const size_t end = head.find_first_of(";\r", at);
            const std::string nv = head.substr(at, end - at);
            const size_t eq = nv.find('=');
            if (eq != std::string::npos) cookies[nv.substr(0, eq)] = nv.substr(eq + 1);
        }
    }
    // The browser's own cookie (its id), which a new tab of it sends alone.
    Jar browserOnly() const {
        Jar j;
        for (const auto& kv : cookies)
            if (kv.first.find("browser") != std::string::npos) j.cookies.insert(kv);
        return j;
    }
    std::string browserId() const {
        const Jar j = browserOnly();
        return j.cookies.empty() ? std::string() : j.cookies.begin()->second;
    }
};

struct Link {
    int port = 0;
    std::string base, token;   // base: /v/ID/
};
Link parseLink(const std::string& url) {
    Link l;
    const std::string pre = "http://127.0.0.1:";
    if (url.compare(0, pre.size(), pre) != 0) return l;
    const size_t slash = url.find('/', pre.size());
    const size_t q = url.find("?t=");
    if (slash == std::string::npos || q == std::string::npos) return l;
    l.port = atoi(url.substr(pre.size(), slash - pre.size()).c_str());
    l.base = url.substr(slash, q - slash);
    l.token = url.substr(q + 3);
    return l;
}

const char* kAgent = "User-Agent: Mozilla/5.0 (X11; Linux x86_64) Firefox/120.0\r\n";

// Starts a GET of the page for `l`.
void getPage(Conn& c, const Link& l, const Jar& jar, const std::string& host = "") {
    c = Conn();
    GCHECK(c.dial(l.port));
    std::string req = "GET " + l.base + "?t=" + l.token + " HTTP/1.1\r\nHost: " +
                      (host.empty() ? "127.0.0.1:" + std::to_string(l.port) : host) + "\r\n" + kAgent;
    if (!jar.cookies.empty()) req += "Cookie: " + jar.header() + "\r\n";
    req += "Connection: close\r\n\r\n";
    c.sendRaw(req);
}
void openSocket(Conn& c, const Link& l, const Jar& jar, const std::string& origin = "") {
    c = Conn();
    GCHECK(c.dial(l.port));
    const std::string me = "http://127.0.0.1:" + std::to_string(l.port);
    c.sendRaw("GET " + l.base + "ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(l.port) +
              "\r\nOrigin: " + (origin.empty() ? me : origin) +
              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
              kAgent + "Cookie: " + jar.header() + "\r\n\r\n");
}

MCGuiAppWindow W(size_t i) {
    const std::vector<MCGuiAppWindow> all = MCGuiAppWindows();
    return i < all.size() ? all[i] : MCGuiAppWindow();
}
SessionBrowser* SB() { return W(0).sessionBrowser; }
Editor* Ed() { return W(0).editor; }

std::string gFileA, gFileB, gFileC, gFileD, gFile2, gDir2, gUrl1, gSeenUrl;
int gOpened = 0;
Link gL1, gL2;
Jar gJar1, gJar2, gJar3;
Conn gHttp, gHttp2, gHttp3, gWs1, gWs3;
Json gMsg;
size_t gDocsBefore = 0;
uint64_t gUntitled = 0;
std::string gUntitledPath;

const Json* docIn(const Json& st, const std::string& path) {
    for (const Json& d : st["docs"].elements())
        if (d["path"].asString() == path) return &d;
    return nullptr;
}
uint64_t docIdOf(Conn& c, const std::string& path) {
    Json st;
    if (!c.latest("state", &st)) return 0;
    const Json* d = docIn(st, path);
    return d ? (uint64_t)(*d)["id"].asInt() : 0;
}
bool docDirty(Conn& c, const std::string& path) {
    Json st;
    if (!c.latest("state", &st)) return false;
    const Json* d = docIn(st, path);
    return d && (*d)["dirty"].asBool();
}
void send(Conn& c, const char* type, uint64_t doc) {
    Json o = Json::object();
    o.set("t", Json(type));
    o.set("doc", Json((int64_t)doc));
    c.sendJson(o);
}
// A page's Save As of document `doc` to `rel` (from the folder).
void saveAs(Conn& c, uint64_t doc, const std::string& rel, bool overwrite) {
    Json o = Json::object();
    o.set("t", Json("saveas"));
    o.set("doc", Json((int64_t)doc));
    o.set("path", Json(rel));
    o.set("overwrite", Json(overwrite));
    c.sendJson(o);
}
// An edit inserting `text` at the start of the document `shown`.
void insertAtStart(Conn& c, const Json& shown, const char* text) {
    Json o = Json::object();
    o.set("t", Json("edit"));
    o.set("doc", shown["id"]);
    o.set("seq", Json(1));
    o.set("base", shown["version"]);
    Json op = Json::array();
    op.push(Json(0));
    op.push(Json(0));
    op.push(Json(text));
    Json ops = Json::array();
    ops.push(op);
    o.set("ops", ops);
    c.sendJson(o);
}
void openPath(Conn& c, const char* path, bool keep) {
    Json o = Json::object();
    o.set("t", Json("open"));
    o.set("path", Json(path));
    o.set("keep", Json(keep));
    c.sendJson(o);
}
void activate(GtkWindow* w, const char* action) {
    g_action_group_activate_action(G_ACTION_GROUP(w), action, nullptr);
}
bool dialogShows(const char* label) { return MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), label) != nullptr; }
// Clicks `label` on the newest dialog.
bool clickDialog(const char* label) {
    GtkWidget* b = MCGuiFindButton(GTK_WIDGET(MCGuiTopWindow()), label);
    if (!b) return false;
    MCGuiClick(b);
    return true;
}
bool fileHas(const std::string& path, const std::string& needle) {
    return MCGuiReadFile(path).find(needle) != std::string::npos;
}
bool pageDone(Conn& c) {
    c.pump();
    return c.closed && c.status() != 0;
}

#ifdef MUTIBLE_ENABLE_PDF
// A GET of `target` (an address of the page, with its query) with a jar's cookies.
void getTarget(Conn& c, const Link& l, const Jar& jar, const std::string& target) {
    c = Conn();
    GCHECK(c.dial(l.port));
    c.sendRaw("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(l.port) + "\r\n" + kAgent +
              "Cookie: " + jar.header() + "\r\nConnection: close\r\n\r\n");
}
void askLatex(Conn& c, const std::string& path) {
    Json o = Json::object();
    o.set("t", Json("latex"));
    o.set("path", Json(path));
    c.sendJson(o);
}
// The first `latex` message for `path` in state `state` from message `from` on.
bool latexFrom(Conn& c, size_t from, const std::string& path, const char* state, Json* out) {
    c.pump();
    for (size_t i = from; i < c.msgs.size(); i++) {
        const Json& m = c.msgs[i];
        if (m["t"].asString() == "latex" && m["path"].asString() == path && m["state"].asString() == state) {
            *out = m;
            return true;
        }
    }
    return false;
}
// The last such message.
bool latexLast(Conn& c, const std::string& path, Json* out) {
    c.pump();
    bool found = false;
    for (const Json& m : c.msgs)
        if (m["t"].asString() == "latex" && m["path"].asString() == path) { *out = m; found = true; }
    return found;
}
std::string pngWidth(const std::string& body) {
    if (body.size() < 24 || body.compare(1, 3, "PNG") != 0 || body.compare(12, 4, "IHDR") != 0) return "not a PNG";
    const unsigned char* p = (const unsigned char*)body.data() + 16;
    return std::to_string((unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]);
}
int tectonicRuns() { return (int)MCGuiReadFile(MCGuiScratch() + "/sb_tectonic/runs.txt").size(); }   // a byte a run
void runGit(const std::vector<std::string>& args) {
    std::string cmd = "git -C '" + MCGuiProject() + "' -c user.name=t -c user.email=t@example.com -c init.defaultBranch=main";
    for (const std::string& a : args) cmd += " '" + a + "'";
    cmd += " >/dev/null 2>&1";
    GCHECK(system(cmd.c_str()) == 0);
}

// The `reviews` of the latest state: the entry with this id, or null.
bool reviewListed(Conn& c, const std::string& id, Json* out) {
    Json st;
    if (!c.latest("state", &st)) return false;
    for (const Json& w : st["windows"].elements())
        for (const Json& r : w["reviews"].elements())
            if (r["id"].asString() == id) {
                *out = r;
                return true;
            }
    return false;
}
// How many review pages the latest state lists, in all its windows.
size_t reviewCount(Conn& c) {
    Json st;
    size_t n = 0;
    if (c.latest("state", &st))
        for (const Json& w : st["windows"].elements()) n += w["reviews"].size();
    return n;
}
// The window's worktree page, or null.
Document* worktreeTab() {
    for (Document* d : Ed()->documents())
        if (d->editorPage())
            if (auto* r = dynamic_cast<ChangeReviewPage*>(d->editorPage().get()))
                if (r->isWorktree()) return d;
    return nullptr;
}
std::string gWtPath, gWtId;
std::string gCmHash, gCmId;
Document* commitTab() {
    for (Document* d : Ed()->documents())
        if (d->editorPage())
            if (auto* r = dynamic_cast<ChangeReviewPage*>(d->editorPage().get()))
                if (r->isCommit()) return d;
    return nullptr;
}

std::string gTectonicWas;
bool gHadTectonic = false;
size_t gMark = 0;
int gRuns = 0;
int64_t gVersion = 0;
Json gLatex;
#endif

}  // namespace

MC_GUI_SUITE(session_browser) {
    MCGuiStep("a fresh world: no state file, the opener captured, files in two folders", [] {
        GCHECK(!MCGuiFileExists(SessionBrowser::stateFile()));
        // The scratch world's, never the owner's.
        GCHECK(SessionBrowser::stateFile().find(MCGuiScratch()) == 0);
        SessionBrowser::opener = [](const std::string& url) {
            gSeenUrl = url;
            gOpened++;
        };
        gFileA = MCGuiProjectFile("sb_a.txt");
        gFileB = MCGuiProjectFile("sb_b.txt");
        gFileC = MCGuiProjectFile("sb_c.txt");
        gFileD = MCGuiProjectFile("sb_d.txt");
        for (const std::string& f : {gFileA, gFileB, gFileC, gFileD})
            MCGuiWriteFile(f, "text of " + f.substr(f.rfind('/') + 1) + "\n");
        gDir2 = MCGuiScratch() + "/sb_other";
        gFile2 = gDir2 + "/other.txt";
        MCGuiWriteFile(gFile2, "the other window's file\n");
        MCGuiOpenFileKept(gFileA);
        MCGuiOpenFileKept(gFileB);
    });
    MCGuiWait("the two tabs are open", 10, [] {
        return Ed()->documentFor(gFileA) && Ed()->documentFor(gFileB);
    });
    MCGuiStep("a second window", [] { MCGuiOpenWindow(gDir2); });
    MCGuiWait("it shows", 10, [] { return MCGuiAppWindows().size() == 2; });
    MCGuiStep("with a third file", [] { MCGuiOpenFileKept(gFile2); });
    MCGuiWait("the second window has the tab", 10, [] {
        return W(1).editor && W(1).editor->documentFor(gFile2);
    });

    // ---- 1. Copy Session Browser Link
    MCGuiStep("Copy Session Browser Link starts the window's server", [] {
        GCHECK(SB() == nullptr);
        activate(W(0).window, "copysessionbrowserlink");
        GCHECK(SB() != nullptr);
        if (!SB()) return;
        GCHECK(SB()->running());
        gUrl1 = SB()->url();
        gL1 = parseLink(gUrl1);
        GCHECK(gL1.port != 0 && gL1.port == SB()->port());
        GCHECK(gL1.base.compare(0, 3, "/v/") == 0 && gL1.base.back() == '/');
        GCHECK(!gL1.token.empty());
    });
    MCGuiAsync("the clipboard holds the link", 10, [](MCGuiDone done) {
        GdkClipboard* cb = gtk_widget_get_clipboard(GTK_WIDGET(W(0).window));
        static MCGuiDone d;
        d = done;
        gdk_clipboard_read_text_async(cb, nullptr, [](GObject* src, GAsyncResult* r, gpointer) {
            char* t = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), r, nullptr);
            GCHECK_EQ_STR(t ? t : "", gUrl1);
            g_free(t);
            d();
        }, nullptr);
    });
    MCGuiStep("a second Copy gives another token on the same server", [] {
        activate(W(0).window, "copysessionbrowserlink");
        gL2 = parseLink(SB()->url());
        GCHECK(SB()->url() != gUrl1);
        GCHECK(!gL2.token.empty() && gL2.token != gL1.token);
        GCHECK(gL2.port == gL1.port);
        GCHECK(gL2.base == gL1.base);
        GCHECK(gOpened == 0);   // Copy opens nothing
        getPage(gHttp2, gL2, Jar(), "evil.example");
    });

    // ---- 2. the page
    MCGuiWait("the wrong Host's answer", 10, [] { gHttp2.pump(); return gHttp2.status() != 0; });
    MCGuiStep("403 for the wrong Host; the first link is fetched", [] {
        GCHECK(gHttp2.status() == 403);
        gHttp2.shut();
        getPage(gHttp, gL1, gJar1);
    });
    MCGuiWait("the page", 10, [] { return pageDone(gHttp); });
    MCGuiStep("200 with the page and its cookies; the same link again", [] {
        GCHECK(gHttp.status() == 200);
        GCHECK(gHttp.head.find("Set-Cookie:") != std::string::npos);
        GCHECK(gHttp.body.find("<html") != std::string::npos || gHttp.body.find("<!DOCTYPE") != std::string::npos ||
               gHttp.body.find("<!doctype") != std::string::npos);
        gJar1.take(gHttp.head);
        GCHECK(!gJar1.cookies.empty());
        gHttp.shut();
        getPage(gHttp3, gL1, Jar());   // another browser with the spent link
    });
    MCGuiWait("the second fetch", 10, [] { gHttp3.pump(); return gHttp3.status() != 0; });
    MCGuiStep("the spent link gets 403; a socket from a wrong Origin", [] {
        GCHECK(gHttp3.status() == 403);
        gHttp3.shut();
        openSocket(gWs1, gL1, gJar1, "http://evil.example");
    });

    // ---- 3. the dialog, Allow, hello and the window's own state
    MCGuiWait("the wrong Origin's answer", 10, [] { gWs1.pump(); return gWs1.status() != 0; });
    MCGuiStep("403; the right Origin waits", [] {
        GCHECK(gWs1.status() == 403);
        gWs1.shut();
        openSocket(gWs1, gL1, gJar1);
    });
    MCGuiWait("waiting", 10, [] { return gWs1.take("waiting", &gMsg); });
    MCGuiWait("the dialog asks on the window", 10, [] {
        return dialogShows("Allow") && dialogShows("Deny") && dialogShows("Always Allow");
    });
    MCGuiStep("the browser is asking, not connected; Allow", [] {
        GCHECK(SB()->asking() == 1);
        GCHECK(SB()->browsers() == 0);
        GCHECK(clickDialog("Allow"));
    });
    MCGuiWait("hello", 10, [] { return gWs1.take("hello", &gMsg); });
    MCGuiWait("a state with this window's tabs", 10, [] {
        return docIdOf(gWs1, gFileA) != 0 && docIdOf(gWs1, gFileB) != 0;
    });
    MCGuiStep("hello says editable; the state has this window's tabs only", [] {
        GCHECK(gMsg["editable"].asBool());
        GCHECK(gMsg["canSave"].asBool());
        GCHECK(gMsg["canOpen"].asBool());
        GCHECK(gMsg["canClose"].asBool());
        GCHECK(gMsg["canNew"].asBool());
        GCHECK(gMsg["canSaveAs"].asBool());
        GCHECK(SB()->browsers() == 1);
        GCHECK(SB()->asking() == 0);
        Json st;
        GCHECK(gWs1.latest("state", &st));
        std::set<std::string> page;
        for (const Json& d : st["docs"].elements()) page.insert(d["path"].asString());
        GCHECK(page.size() == Ed()->documents().size());
        GCHECK(page.count(gFile2) == 0);
        GCHECK(page.count(gFileA) == 1);
        GCHECK(W(1).editor->documentFor(gFile2) != nullptr);   // really open, in the other window
        MCGuiFocusKeyWindow();
    });

    // ---- 5. an editable page
    MCGuiStep("open a file from the page", [] {
        GCHECK(Ed()->documentFor(gFileC) == nullptr);
        openPath(gWs1, "sb_c.txt", true);
    });
    MCGuiWait("opening", 10, [] { return gWs1.take("opening", &gMsg); });
    MCGuiWait("the tab opens in the window", 10, [] { return Ed()->documentFor(gFileC) != nullptr; });
    MCGuiStep("a file outside the folder is refused", [] {
        GCHECK(gMsg["ok"].asBool());
        openPath(gWs1, "../sb_other/other.txt", true);
    });
    MCGuiWait("refused", 10, [] { return gWs1.take("opening", &gMsg); });
    MCGuiStep("show sb_a.txt on the page", [] {
        GCHECK(!gMsg["ok"].asBool());
        GCHECK(Ed()->documentFor(gFile2) == nullptr);
        send(gWs1, "show", docIdOf(gWs1, gFileA));
    });
    MCGuiWait("its text", 10, [] { return gWs1.take("doc", &gMsg); });
    MCGuiStep("an edit from the page", [] {
        GCHECK_EQ_STR(gMsg["path"].asString(), gFileA);
        insertAtStart(gWs1, gMsg, "EDITED ");
    });
    MCGuiWait("the window's document has it, unsaved", 10, [] {
        Document* d = Ed()->documentFor(gFileA);
        return d && d->dirty();
    });
    MCGuiWait("the page hears the tab is dirty", 10, [] { return docDirty(gWs1, gFileA); });
    MCGuiStep("the file on disk is as it was until saved; then save", [] {
        GCHECK(!fileHas(gFileA, "EDITED"));
        send(gWs1, "save", docIdOf(gWs1, gFileA));
    });
    MCGuiWait("the file on disk changed", 10, [] { return fileHas(gFileA, "EDITED text of sb_a.txt"); });
    MCGuiWait("the tab is clean", 10, [] {
        Document* d = Ed()->documentFor(gFileA);
        return d && !d->dirty();
    });
    MCGuiWait("the page hears it is clean", 10, [] { return !docDirty(gWs1, gFileA); });
    MCGuiStep("close the clean tab from the page", [] { send(gWs1, "close", docIdOf(gWs1, gFileA)); });
    MCGuiWait("closing", 10, [] { return gWs1.take("closing", &gMsg); });
    MCGuiWait("the tab closed in the window", 10, [] { return Ed()->documentFor(gFileA) == nullptr; });
    MCGuiStep("show sb_b.txt", [] {
        GCHECK(gMsg["ok"].asBool());
        send(gWs1, "show", docIdOf(gWs1, gFileB));
    });
    MCGuiWait("its text", 10, [] { return gWs1.take("doc", &gMsg); });
    MCGuiStep("edit it, never saved", [] { insertAtStart(gWs1, gMsg, "DIRTY "); });
    MCGuiWait("the page hears it is dirty", 10, [] { return docDirty(gWs1, gFileB); });
    MCGuiStep("close the dirty tab from the page", [] { send(gWs1, "close", docIdOf(gWs1, gFileB)); });
    MCGuiWait("closing", 10, [] { return gWs1.take("closing", &gMsg); });
    MCGuiStep("refused: the tab stays unsaved; save it from the page", [] {
        GCHECK(!gMsg["ok"].asBool());
        GCHECK(gMsg["why"].asString().find("unsaved") != std::string::npos);
        Document* d = Ed()->documentFor(gFileB);
        GCHECK(d != nullptr);
        GCHECK(d && d->dirty());
        GCHECK(!fileHas(gFileB, "DIRTY"));
        send(gWs1, "save", docIdOf(gWs1, gFileB));
    });
    MCGuiWait("saved", 10, [] { return fileHas(gFileB, "DIRTY text of sb_b.txt"); });
    MCGuiWait("clean on the page", 10, [] { return !docDirty(gWs1, gFileB); });
    MCGuiStep("now it closes", [] { send(gWs1, "close", docIdOf(gWs1, gFileB)); });
    MCGuiWait("closing", 10, [] { return gWs1.take("closing", &gMsg); });
    MCGuiWait("sb_b.txt closed", 10, [] { return Ed()->documentFor(gFileB) == nullptr; });

    // ---- 5b. New File and Save As from the page
    MCGuiStep("New File from the page", [] {
        gDocsBefore = Ed()->documents().size();
        Json o = Json::object();
        o.set("t", Json("newfile"));
        gWs1.sendJson(o);
    });
    MCGuiWait("an untitled tab on show in the window", 10, [] {
        Document* d = Ed()->active();
        if (!d || Ed()->documents().size() != gDocsBefore + 1 || d->diskStamp().exists) return false;
        gUntitled = d->id();
        gUntitledPath = d->path();
        return true;
    });
    MCGuiWait("the page lists it", 10, [] {
        Json st;
        if (!gWs1.latest("state", &st)) return false;
        for (const Json& d : st["docs"].elements())
            if ((uint64_t)d["id"].asInt() == gUntitled) return true;
        return false;
    });
    MCGuiStep("it is named Untitled-N and not on disk; show it", [] {
        GCHECK(gUntitledPath.find("/Untitled-") != std::string::npos);
        GCHECK(access(gUntitledPath.c_str(), F_OK) != 0);
        send(gWs1, "show", gUntitled);
    });
    MCGuiWait("its text", 10, [] { return gWs1.take("doc", &gMsg); });
    MCGuiStep("typed into through the page", [] { insertAtStart(gWs1, gMsg, "typed on the page\n"); });
    MCGuiWait("the tab has it, unsaved", 10, [] {
        Document* d = Ed()->documentById(gUntitled);
        return d && d->dirty() && d->text() == "typed on the page\n";
    });
    MCGuiStep("Save As over a file there, without overwrite", [] {
        MCGuiWriteFile(MCGuiProjectFile("sb_exists.txt"), "old\n");
        saveAs(gWs1, gUntitled, "sb_exists.txt", false);
    });
    MCGuiWait("savedas", 10, [] { return gWs1.take("savedas", &gMsg); });
    MCGuiStep("answered exists; the file and the tab are as they were", [] {
        GCHECK_EQ_STR(gMsg["reason"].asString(), "exists");
        GCHECK(!gMsg["ok"].asBool());
        GCHECK(fileHas(MCGuiProjectFile("sb_exists.txt"), "old"));
        Document* d = Ed()->documentById(gUntitled);
        GCHECK(d && d->path() == gUntitledPath);
        saveAs(gWs1, gUntitled, "../sb_outside.txt", false);
    });
    MCGuiWait("savedas", 10, [] { return gWs1.take("savedas", &gMsg); });
    MCGuiStep("a path outside the folder is refused; a dotfile too", [] {
        GCHECK(!gMsg["ok"].asBool());
        GCHECK_EQ_STR(gMsg["reason"].asString(), "name");
        saveAs(gWs1, gUntitled, ".sb_hidden.txt", false);
    });
    MCGuiWait("savedas", 10, [] { return gWs1.take("savedas", &gMsg); });
    MCGuiStep("refused as a dotfile, nothing written; Save As to a new name", [] {
        GCHECK(!gMsg["ok"].asBool());
        GCHECK_EQ_STR(gMsg["reason"].asString(), "dotfile");
        GCHECK(access(MCGuiProjectFile(".sb_hidden.txt").c_str(), F_OK) != 0);
        saveAs(gWs1, gUntitled, "sb_made.txt", false);
    });
    MCGuiWait("savedas", 10, [] { return gWs1.take("savedas", &gMsg); });
    MCGuiStep("ok with the path; the file holds the typed text", [] {
        GCHECK(gMsg["ok"].asBool());
        if (!gMsg["ok"].asBool()) MCGuiNote("%s", gMsg.dump().c_str());
        GCHECK_EQ_STR(gMsg["path"].asString(), MCGuiProjectFile("sb_made.txt"));
        GCHECK_EQ_STR(MCGuiReadFile(MCGuiProjectFile("sb_made.txt")), "typed on the page\n");
    });
    MCGuiWait("the window's tab names that path, clean", 10, [] {
        Document* d = Ed()->documentById(gUntitled);
        return d && d->path() == MCGuiProjectFile("sb_made.txt") && !d->dirty();
    });
    MCGuiWait("the page lists it under the new path, clean", 10, [] {
        const std::string p = MCGuiProjectFile("sb_made.txt");
        return docIdOf(gWs1, p) == gUntitled && !docDirty(gWs1, p);
    });
    MCGuiStep("show it again", [] { send(gWs1, "show", gUntitled); });
    MCGuiWait("its text", 10, [] { return gWs1.take("doc", &gMsg); });
    MCGuiStep("typed again", [] { insertAtStart(gWs1, gMsg, "more "); });
    MCGuiWait("the tab has it", 10, [] {
        Document* d = Ed()->documentById(gUntitled);
        return d && d->dirty() && d->text() == "more typed on the page\n";
    });
    MCGuiStep("Save As over sb_exists.txt, with overwrite", [] { saveAs(gWs1, gUntitled, "sb_exists.txt", true); });
    MCGuiWait("savedas", 10, [] { return gWs1.take("savedas", &gMsg); });
    MCGuiStep("ok; sb_exists.txt replaced, sb_made.txt left as it was, the tab on the new file", [] {
        GCHECK(gMsg["ok"].asBool());
        if (!gMsg["ok"].asBool()) MCGuiNote("%s", gMsg.dump().c_str());
        GCHECK_EQ_STR(MCGuiReadFile(MCGuiProjectFile("sb_exists.txt")), "more typed on the page\n");
        GCHECK_EQ_STR(MCGuiReadFile(MCGuiProjectFile("sb_made.txt")), "typed on the page\n");
        Document* d = Ed()->documentById(gUntitled);
        GCHECK(d && d->path() == MCGuiProjectFile("sb_exists.txt") && !d->dirty());
    });
    MCGuiStep("the tab closes from the page; the files go", [] {
        send(gWs1, "close", gUntitled);
    });
    MCGuiWait("closing", 10, [] { return gWs1.take("closing", &gMsg); });
    MCGuiWait("closed in the window", 10, [] { return Ed()->documentById(gUntitled) == nullptr; });
    MCGuiStep("files removed", [] {
        unlink(MCGuiProjectFile("sb_exists.txt").c_str());
        unlink(MCGuiProjectFile("sb_made.txt").c_str());
    });

    // ---- 4. Deny, Always Allow
    MCGuiStep("a second browser: a fresh link, its page fetched", [] {
        GCHECK(gMsg["ok"].asBool());
        gWs1.shut();
        activate(W(0).window, "copysessionbrowserlink");
        gL2 = parseLink(SB()->url());
        getPage(gHttp2, gL2, gJar2);
    });
    MCGuiWait("page", 10, [] { return pageDone(gHttp2); });
    MCGuiWait("the dialog asks", 10, [] { return dialogShows("Deny"); });
    MCGuiStep("Deny", [] {
        GCHECK(gHttp2.status() == 200);
        gJar2.take(gHttp2.head);
        gHttp2.shut();
        GCHECK(clickDialog("Deny"));
    });
    MCGuiWait("no longer asking", 10, [] { return SB()->asking() == 0; });
    MCGuiStep("its socket is told it was denied", [] { openSocket(gWs3, gL2, gJar2); });
    MCGuiWait("denied", 10, [] { return gWs3.take("denied", &gMsg); });
    MCGuiStep("nothing connected or remembered; a third browser", [] {
        GCHECK(SB()->browsers() == 0);
        GCHECK(SessionBrowser::remembered().empty());
        gWs3.shut();
        activate(W(0).window, "copysessionbrowserlink");
        gL2 = parseLink(SB()->url());
        getPage(gHttp2, gL2, gJar3);
    });
    MCGuiWait("page", 10, [] { return pageDone(gHttp2); });
    MCGuiWait("the dialog asks", 10, [] { return dialogShows("Always Allow"); });
    MCGuiStep("Always Allow", [] {
        GCHECK(gHttp2.status() == 200);
        gJar3.take(gHttp2.head);
        gHttp2.shut();
        GCHECK(clickDialog("Always Allow"));
        openSocket(gWs3, gL2, gJar3);
    });
    MCGuiWait("hello", 10, [] { return gWs3.take("hello", &gMsg); });
    MCGuiWait("remembered", 10, [] { return SessionBrowser::remembered().size() == 1; });
    MCGuiStep("the state file holds its id; its next link", [] {
        GCHECK(!gJar3.browserId().empty());
        GCHECK(fileHas(SessionBrowser::stateFile(), gJar3.browserId()));
        GCHECK(SessionBrowser::remembered()[0].id == gJar3.browserId());
        gWs3.shut();
        activate(W(0).window, "copysessionbrowserlink");
        gL2 = parseLink(SB()->url());
        gJar3 = gJar3.browserOnly();   // a new tab of the browser: only its own cookie
        getPage(gHttp2, gL2, gJar3);
    });
    MCGuiWait("page", 10, [] { return pageDone(gHttp2); });
    MCGuiStep("allowed without a question", [] {
        GCHECK(gHttp2.status() == 200);
        gJar3.take(gHttp2.head);
        gHttp2.shut();
        openSocket(gWs3, gL2, gJar3);
    });
    MCGuiWait("hello at once", 10, [] { return gWs3.take("hello", &gMsg); });
    MCGuiStep("no dialog, nothing waiting", [] {
        GCHECK(SB()->asking() == 0);
        GCHECK(!dialogShows("Allow"));
        GCHECK(SB()->browsers() == 1);
    });

    // ---- 6. read-only, through Show's dialog
    MCGuiStep("Show Session in Browser…", [] {
        gWs3.shut();
        gOpened = 0;
        activate(W(0).window, "showsessioninbrowser");
    });
    MCGuiWait("the question shows", 10, [] { return dialogShows("Read-Only") && dialogShows("Editable"); });
    MCGuiStep("answer Read-Only", [] { GCHECK(clickDialog("Read-Only")); });
    MCGuiWait("the opener got the address", 10, [] { return gOpened == 1; });
    MCGuiStep("read-only on a new server; the remembered browser's page", [] {
        GCHECK(!SB()->editable());
        GCHECK(!SessionBrowser::editableChoice());
        gL1 = parseLink(gSeenUrl);
        GCHECK(gL1.port != 0 && gL1.port == SB()->port());
        getPage(gHttp, gL1, gJar3);
    });
    MCGuiWait("page", 10, [] { return pageDone(gHttp); });
    MCGuiStep("200; its socket", [] {
        GCHECK(gHttp.status() == 200);
        gJar3.take(gHttp.head);
        gHttp.shut();
        openSocket(gWs3, gL1, gJar3);
    });
    MCGuiWait("hello", 10, [] { return gWs3.take("hello", &gMsg); });
    MCGuiWait("the tabs", 10, [] { return docIdOf(gWs3, gFileC) != 0; });
    MCGuiStep("hello says read-only; open is refused", [] {
        GCHECK(!gMsg["editable"].asBool());
        GCHECK(!gMsg["canSave"].asBool());
        GCHECK(!gMsg["canOpen"].asBool());
        GCHECK(!gMsg["canClose"].asBool());
        GCHECK(!gMsg["canNew"].asBool());
        GCHECK(!gMsg["canSaveAs"].asBool());
        openPath(gWs3, "sb_d.txt", true);
    });
    MCGuiWait("the answer to open", 10, [] { return gWs3.take("opening", &gMsg); });
    MCGuiStep("no new tab in the window; close is refused", [] {
        GCHECK(!gMsg["ok"].asBool());
        GCHECK(Ed()->documentFor(gFileD) == nullptr);
        send(gWs3, "close", docIdOf(gWs3, gFileC));
    });
    MCGuiWait("the answer to close", 10, [] { return gWs3.take("closing", &gMsg); });
    MCGuiStep("sb_c.txt stays open; show it", [] {
        GCHECK(!gMsg["ok"].asBool());
        GCHECK(Ed()->documentFor(gFileC) != nullptr);
        send(gWs3, "show", docIdOf(gWs3, gFileC));
    });
    MCGuiWait("its text", 10, [] { return gWs3.take("doc", &gMsg); });
    MCGuiStep("an edit and a save from the read-only page", [] {
        insertAtStart(gWs3, gMsg, "NO ");
        send(gWs3, "save", (uint64_t)gMsg["id"].asInt());
    });
    MCGuiPause(1.5);   // a negative check: nothing may reach the window in this time
    MCGuiStep("the window's tab is untouched", [] {
        Document* d = Ed()->documentFor(gFileC);
        GCHECK(d != nullptr);
        GCHECK(d && !d->dirty());
        GCHECK(!fileHas(gFileC, "NO "));
        gWs3.shut();
    });

    // ---- 4b. Quick Open and Find in Folder on the window's host (stage 7's workers)
    MCGuiStep("two files in a folder, the read-only page's socket again", [] {
        MCGuiWriteFile(MCGuiProjectFile("sbq/calc.py"), "def calc():\n    zqx_marker = 1\n    return zqx_marker\n");
        MCGuiWriteFile(MCGuiProjectFile("sbq/pkg/util.py"), "def helper():\n    return 42  # zqx_marker\n");
        openSocket(gWs3, gL1, gJar3);
    });
    MCGuiWait("hello", 10, [] { return gWs3.take("hello", &gMsg); });
    MCGuiStep("Quick Open asks for the files named like sbq", [] {
        // Not an empty query: the page shows only its first rows, and the
        // suites before this one may leave the folder hundreds of files
        // that sort before sbq/ (CI's run listed 235).
        Json o = Json::object();
        o.set("t", Json("files"));
        o.set("seq", Json(1));
        o.set("query", Json("sbq"));
        gWs3.sendJson(o);
    });
    MCGuiWait("the host's listing, not one still under way", 20, [] {
        Json m;
        while (gWs3.take("files", &m)) {
            if (m["seq"].asInt() != 1 || m["listing"].asBool()) continue;
            gMsg = m;
            return true;
        }
        return false;
    });
    MCGuiStep("it lists both, the nested one too", [] {
        std::string paths;
        for (const Json& r : gMsg["rows"].elements()) paths += r["path"].asString() + "|";
        MCGuiNote("quick open: %s", paths.c_str());
        GCHECK(gMsg["error"].isNull());
        GCHECK(paths.find("sbq/calc.py|") != std::string::npos);
        GCHECK(paths.find("sbq/pkg/util.py|") != std::string::npos);
        Json o = Json::object();
        o.set("t", Json("files"));
        o.set("seq", Json(2));
        o.set("query", Json("sbqclc"));
        gWs3.sendJson(o);
    });
    MCGuiWait("a query ranks them", 20, [] {
        Json m;
        while (gWs3.take("files", &m))
            if (m["seq"].asInt() == 2) {
                gMsg = m;
                return true;
            }
        return false;
    });
    MCGuiStep("calc.py first, the matched letters marked; nothing opened in the window", [] {
        const Json& rows = gMsg["rows"];
        if (!GCHECK(rows.size() >= 1)) return;
        MCGuiNote("ranked: %s", rows.dump().substr(0, 300).c_str());
        GCHECK_EQ_STR(rows.elements()[0]["path"].asString(), "sbq/calc.py");
        GCHECK(rows.elements()[0]["pos"].size() == 6);
        GCHECK(Ed()->documentFor(MCGuiProjectFile("sbq/calc.py")) == nullptr);
        Json o = Json::object();
        o.set("t", Json("search"));
        o.set("seq", Json(3));
        o.set("query", Json("zqx_marker"));
        gWs3.sendJson(o);
    });
    MCGuiWait("Find in Folder's last answer", 20, [] {
        gWs3.pump();
        for (const Json& m : gWs3.msgs)
            if (m["t"].asString() == "found" && m["seq"].asInt() == 3 && m["done"].asBool()) return true;
        return false;
    });
    MCGuiStep("rows from both files, with context and marks; the host's counts", [] {
        std::string seen;
        int rows = 0, withHl = 0, nested = 0;
        Json last;
        for (const Json& m : gWs3.msgs) {
            if (m["t"].asString() != "found" || m["seq"].asInt() != 3) continue;
            last = m;
            for (const Json& r : m["rows"].elements()) {
                rows++;
                if (r["hl"].size() > 0) withHl++;
                if (r["path"].asString() == "sbq/pkg/util.py") nested++;
                seen += r["path"].asString() + ":" + std::to_string(r["line"].asInt()) + " ";
            }
        }
        MCGuiNote("search: %s / %s", seen.c_str(), last.dump().substr(0, 300).c_str());
        GCHECK(rows == 3 && withHl == 3 && nested == 1);
        GCHECK(last["error"].isNull() && !last["truncated"].asBool());
        GCHECK(last["filesMatched"].asInt() == 2 && last["totalMatches"].asInt() == 3);
        GCHECK(Ed()->documentFor(MCGuiProjectFile("sbq/pkg/util.py")) == nullptr);
    });
    MCGuiStep("a one-letter query is too short to search", [] {
        Json o = Json::object();
        o.set("t", Json("search"));
        o.set("seq", Json(4));
        o.set("query", Json("z"));
        gWs3.sendJson(o);
    });
    MCGuiWait("the answer says so", 20, [] {
        gWs3.pump();
        for (const Json& m : gWs3.msgs)
            if (m["t"].asString() == "found" && m["seq"].asInt() == 4) return m["short"].asBool() && m["done"].asBool();
        return false;
    });
    MCGuiStep("the socket and the files go back", [] {
        gWs3.shut();
        const std::string cmd = "rm -rf '" + MCGuiProjectFile("sbq") + "'";
        GCHECK(system(cmd.c_str()) == 0);
    });

#ifdef MUTIBLE_ENABLE_PDF
    // ---- 5. LaTeX typesets and the page's Source Control (a stand-in tectonic)
    MCGuiStep("a stand-in tectonic; the page's socket", [] {
        const std::string dir = MCGuiScratch() + "/sb_tectonic";
        MCGuiWriteFile(dir + "/main.pdf",
                       MCGuiReadFile(MCGuiBuildDir() + "/../../tests/latex/fixtures/main.pdf"));
        GCHECK(MCGuiReadFile(dir + "/main.pdf").compare(0, 5, "%PDF-") == 0);
        MCGuiWriteFile(dir + "/tectonic",
            "#!/bin/sh\n"
            "while [ $# -gt 1 ]; do case \"$1\" in --outdir) out=$2; shift;; esac; shift; done\n"
            "file=$1; case $file in /*) ;; *) file=$PWD/$file;; esac\n"
            "stem=$(basename \"$file\" .tex)\n"
            "d=$(dirname \"$0\")\n"
            "printf x >> \"$d/runs.txt\"; sleep 0.3\n"
            "if grep -q FAILMARK \"$file\"; then echo \"stand-in tectonic: the marker is here\" >&2; exit 1; fi\n"
            "cp \"$d/main.pdf\" \"$out/$stem.pdf\"\n");
        g_chmod((dir + "/tectonic").c_str(), 0755);
        const char* was = g_getenv("MUTIBLE_TECTONIC");
        gHadTectonic = was != nullptr;
        gTectonicWas = was ? was : "";
        EnvLock::envSet("MUTIBLE_TECTONIC", (dir + "/tectonic").c_str(), TRUE);
        for (const char* n : {"sbt_nt1", "sbt_nt2", "sbt_nt3", "sbt_nt4", "sbt_tab"})
            MCGuiWriteFile(MCGuiProjectFile(std::string(n) + ".tex"),
                           std::string("\\documentclass{article}\\begin{document}") + n + "\\end{document}\n");
        MCGuiWriteFile(MCGuiProjectFile("sbt_bad.tex"), "\\documentclass{article} FAILMARK\n");
        openSocket(gWs3, gL1, gJar3);
    });
    MCGuiWait("hello", 10, [] { return gWs3.take("hello", &gMsg); });
    MCGuiStep("ask for a typeset of a file no tab holds", [] {
        GCHECK(SB()->hiddenTypesets() == 0);
        gMark = gWs3.msgs.size();
        askLatex(gWs3, "sbt_nt1.tex");
    });
    MCGuiWait("it is ready", 20, [] { return latexFrom(gWs3, gMark, "sbt_nt1.tex", "ready", &gLatex); });
    MCGuiStep("two pages of 612 by 792, the PDF fetched", [] {
        GCHECK_EQ_STR(std::to_string(gLatex["pages"].size()), "2");
        if (gLatex["pages"].size() == 2) {
            GCHECK_EQ_STR(std::to_string((int)gLatex["pages"][0][0].asNumber()), "612");
            GCHECK_EQ_STR(std::to_string((int)gLatex["pages"][1][1].asNumber()), "792");
        }
        GCHECK(gLatex["url"].asString().find("/typeset/sbt_nt1.tex") != std::string::npos);
        GCHECK_EQ_STR(std::to_string(tectonicRuns()), "1");
        getTarget(gHttp, gL1, gJar3, gLatex["url"].asString());
    });
    MCGuiWait("the PDF", 10, [] { return pageDone(gHttp); });
    MCGuiStep("200 and %PDF-; a page's picture 400 wide", [] {
        GCHECK_EQ_STR(std::to_string(gHttp.status()), "200");
        GCHECK_EQ_STR(gHttp.body.substr(0, 5), "%PDF-");
        gHttp.shut();
        getTarget(gHttp, gL1, gJar3, gLatex["pageUrl"].asString() + "&p=2&w=400");
    });
    MCGuiWait("the picture", 10, [] { return pageDone(gHttp); });
    MCGuiStep("200, a PNG 400 wide", [] {
        GCHECK_EQ_STR(std::to_string(gHttp.status()), "200");
        GCHECK_EQ_STR(pngWidth(gHttp.body), "400");
        gHttp.shut();
    });

    MCGuiStep("a typeset the engine fails on", [] {
        gMark = gWs3.msgs.size();
        askLatex(gWs3, "sbt_bad.tex");
    });
    MCGuiWait("it fails", 20, [] { return latexFrom(gWs3, gMark, "sbt_bad.tex", "failed", &gLatex); });
    MCGuiStep("with the engine's text", [] {
        GCHECK(gLatex["text"].asString().find("stand-in tectonic: the marker is here") != std::string::npos);
        GCHECK(gLatex["url"].asString().empty());
    });

    // At most four untabbed typesets, the oldest dropped: nt1, bad, nt2, nt3
    // are kept; nt4 drops nt1.
    // One at a time: a page follows the one document it last asked for.
    MCGuiStep("two more documents", [] {
        GCHECK_EQ_STR(std::to_string(SB()->hiddenTypesets()), "2");
        gMark = gWs3.msgs.size();
        askLatex(gWs3, "sbt_nt2.tex");
    });
    MCGuiWait("nt2 ready", 8, [] { return latexFrom(gWs3, gMark, "sbt_nt2.tex", "ready", &gLatex); });
    MCGuiStep("and nt3", [] {
        gMark = gWs3.msgs.size();
        askLatex(gWs3, "sbt_nt3.tex");
    });
    MCGuiWait("nt3 ready", 8, [] { return latexFrom(gWs3, gMark, "sbt_nt3.tex", "ready", &gLatex); });
    MCGuiStep("four kept; the fifth", [] {
        GCHECK_EQ_STR(std::to_string(SB()->hiddenTypesets()), "4");
        gMark = gWs3.msgs.size();
        gRuns = tectonicRuns();
        askLatex(gWs3, "sbt_nt4.tex");
    });
    MCGuiWait("it is ready", 20, [] { return latexFrom(gWs3, gMark, "sbt_nt4.tex", "ready", &gLatex); });
    MCGuiStep("still four; a kept one asked again", [] {
        GCHECK_EQ_STR(std::to_string(SB()->hiddenTypesets()), "4");
        GCHECK_EQ_STR(std::to_string(tectonicRuns()), std::to_string(gRuns + 1));
        gMark = gWs3.msgs.size();
        askLatex(gWs3, "sbt_nt2.tex");
    });
    MCGuiWait("answered from the kept typeset", 10, [] { return latexFrom(gWs3, gMark, "sbt_nt2.tex", "ready", &gLatex); });
    MCGuiStep("no new run; the oldest (nt1) is gone, so it runs again", [] {
        GCHECK_EQ_STR(std::to_string(tectonicRuns()), std::to_string(gRuns + 1));
        gMark = gWs3.msgs.size();
        askLatex(gWs3, "sbt_nt1.tex");
    });
    MCGuiWait("nt1 ready again", 20, [] {
        return latexFrom(gWs3, gMark, "sbt_nt1.tex", "ready", &gLatex) && tectonicRuns() == gRuns + 2;
    });
    MCGuiStep("still four", [] { GCHECK_EQ_STR(std::to_string(SB()->hiddenTypesets()), "4"); });

    // A tab's own preview: the page's typeset is the tab's.
    MCGuiStep("open sbt_tab.tex in a tab (its preview typesets)", [] {
        Ed()->closeAllTabs();
        Ed()->openFile(MCGuiProjectFile("sbt_tab.tex"), EditorTabs::OpenFrom::Other);   // window 0's, not the newest window's
    });
    MCGuiWait("the tab shows", 10, [] { return Ed()->currentPath() == MCGuiProjectFile("sbt_tab.tex"); });
    MCGuiWait("the tab's preview made its PDF", 20, [] {
        return Ed()->latex() && Ed()->latex()->typesetPdf() && !Ed()->latex()->typesetting();
    });
    MCGuiStep("the page asks for it", [] {
        gRuns = tectonicRuns();
        gMark = gWs3.msgs.size();
        askLatex(gWs3, "sbt_tab.tex");
    });
    MCGuiWait("it is ready", 20, [] { return latexFrom(gWs3, gMark, "sbt_tab.tex", "ready", &gLatex); });
    MCGuiStep("from the tab's typeset: no second run, no hidden one made", [] {
        GCHECK_EQ_STR(std::to_string(tectonicRuns()), std::to_string(gRuns));
        GCHECK_EQ_STR(std::to_string(SB()->hiddenTypesets()), "4");
        GCHECK_EQ_STR(std::to_string(gLatex["pages"].size()), "2");
        gVersion = gLatex["version"].asInt();
        gMark = gWs3.msgs.size();
        GCHECK(Ed()->latex() != nullptr);
        if (Ed()->latex()) Ed()->latex()->compileNow();   // a later typeset of the tab
    });
    MCGuiWait("the later typeset reaches the page", 20, [] {
        Json j;
        return latexFrom(gWs3, gMark, "sbt_tab.tex", "working", &j) && latexLast(gWs3, "sbt_tab.tex", &gLatex) &&
               gLatex["state"].asString() == "ready" && gLatex["version"].asInt() > gVersion;
    });
    MCGuiStep("one more run, the tab's", [] {
        GCHECK_EQ_STR(std::to_string(tectonicRuns()), std::to_string(gRuns + 1));
        Ed()->closeAllTabs();
    });

    // ---- 6. Source Control after a commit
    MCGuiStep("a repository with a file not yet committed; the page watches git", [] {
        MCGuiWriteFile(MCGuiProjectFile("sbt_base.txt"), "base\n");
        runGit({"init", "-q"});
        runGit({"add", "sbt_base.txt"});
        runGit({"commit", "-q", "-m", "base"});
        MCGuiWriteFile(MCGuiProjectFile("sbt_new.txt"), "new\n");
        Json o = Json::object();
        o.set("t", Json("git"));
        o.set("watch", Json(true));
        gWs3.sendJson(o);
    });
    MCGuiWait("a git message that lists it", 20, [] {
        return gWs3.latest("git", &gMsg) && gMsg.dump().find("sbt_new.txt") != std::string::npos;
    });
    MCGuiStep("commit it in the folder", [] {
        gVersion = gMsg["version"].asInt();
        runGit({"add", "sbt_new.txt"});
        runGit({"commit", "-q", "-m", "new"});
    });
    MCGuiWait("a newer git message without it", 20, [] {
        if (!gWs3.latest("git", &gMsg)) return false;
        return gMsg["version"].asInt() > gVersion && gMsg.dump().find("sbt_new.txt") == std::string::npos;
    });

    // ---- 6b. The in-process host watches the disk for the page
    MCGuiStep("the page watches the folder's root and lists it, as the real page does", [] {
        Json o = Json::object();
        o.set("t", Json("watch"));
        Json paths = Json::array();
        paths.push(Json(""));
        o.set("paths", std::move(paths));
        gWs3.sendJson(o);
        Json l = Json::object();
        l.set("t", Json("list"));
        l.set("path", Json(""));
        gWs3.sendJson(l);
    });
    MCGuiWait("the listing, without the file", 20, [] {
        return gWs3.latest("tree", &gMsg) && gMsg["path"].asString().empty() && gMsg["entries"].isArray();
    });
    // The watcher walks the folders on a thread of its own: a file made
    // before it watches them would have no notice, so wait until it watches
    // what the host asked of it last (SessionHost::diskWatchSettled).
    MCGuiWait("the in-process host's watcher watching the folder", 20, [] {
        // The window the page serves (W(0), not the second window's link).
        SessionLink::Link* l = W(0).editor ? W(0).editor->sessionLink() : nullptr;
        Shared::SessionHost* h = l ? l->inProcessHost() : nullptr;
        std::size_t folders = 0;
        // A short wait for the host's thread to answer (0 never sees it).
        return h && h->diskWatchSettled(300, &folders) && folders > 0;
    });
    MCGuiStep("a file made on disk", [] { MCGuiWriteFile(MCGuiProjectFile("sbt_watch.txt"), "made\n"); });
    MCGuiWait("a new listing of the root that has it, no refresh sent", 20, [] {
        return gWs3.latest("tree", &gMsg) && gMsg.dump().find("sbt_watch.txt") != std::string::npos;
    });
    MCGuiStep("the file removed", [] { unlink(MCGuiProjectFile("sbt_watch.txt").c_str()); });
    MCGuiWait("a new listing without it", 30, [] {
        return gWs3.latest("tree", &gMsg) && gMsg.dump().find("sbt_watch.txt") == std::string::npos;
    });
    MCGuiStep("a tracked file changed in the repository", [] {
        gVersion = gWs3.latest("git", &gMsg) ? gMsg["version"].asInt() : 0;
        MCGuiWriteFile(MCGuiProjectFile("sbt_base.txt"), "changed on disk\n");
    });
    MCGuiWait("a newer git message that lists it", 30, [] {
        if (!gWs3.latest("git", &gMsg)) return false;
        return gMsg["version"].asInt() > gVersion && gMsg.dump().find("sbt_base.txt") != std::string::npos;
    });

    // ---- 7. A worktree's review page
    MCGuiStep("a worktree one commit ahead of main, its review page open in the window", [] {
        gWtPath = MCGuiScratch() + "/sbt_wt";
        runGit({"worktree", "add", "-q", gWtPath, "-b", "sbt-branch"});
        MCGuiWriteFile(gWtPath + "/sbt_wt.txt", "from the worktree\n");
        const std::string cmd = "git -C '" + gWtPath +
                                "' add sbt_wt.txt && git -C '" + gWtPath +
                                "' -c user.name=t -c user.email=t@example.com commit -q -m wt >/dev/null 2>&1";
        GCHECK(system(cmd.c_str()) == 0);
        gWtId = "worktree:" + gWtPath;
        GCHECK(MCGuiApp().git != nullptr);
        auto page = std::make_shared<ChangeReviewPage>(MCGuiApp().git, gWtPath, "sbt-wt", "sbt-branch", "main");
        Ed()->showPage(page);
    });
    MCGuiWait("the state lists it, on show, with its title", 20, [] {
        Json r;
        return reviewListed(gWs3, gWtId, &r) && r["onShow"].asBool();
    });
    MCGuiStep("kind worktree, the page's title; the uncommitted changes' page is not one", [] {
        Json r;
        if (!GCHECK(reviewListed(gWs3, gWtId, &r))) return;
        GCHECK_EQ_STR(r["kind"].asString(), "worktree");
        GCHECK_EQ_STR(r["title"].asString(), "sbt-wt vs main");
        GCHECK(reviewCount(gWs3) == 1);
        gMark = gWs3.msgs.size();
        Json o = Json::object();
        o.set("t", Json("review"));
        o.set("page", Json(gWtId));
        gWs3.sendJson(o);
    });
    MCGuiWait("the read is done (pending, then reviewread)", 20, [] {
        gWs3.pump();
        for (size_t i = gMark; i < gWs3.msgs.size(); i++)
            if (gWs3.msgs[i]["t"].asString() == "reviewread" && gWs3.msgs[i]["page"].asString() == gWtId) return true;
        return false;
    });
    MCGuiStep("ask again, as the page does", [] {
        gMark = gWs3.msgs.size();
        Json o = Json::object();
        o.set("t", Json("review"));
        o.set("page", Json(gWtId));
        gWs3.sendJson(o);
    });
    MCGuiWait("the review of that page, with its file", 20, [] {
        gWs3.pump();
        for (size_t i = gMark; i < gWs3.msgs.size(); i++) {
            const Json& m = gWs3.msgs[i];
            if (m["t"].asString() == "review" && m["page"].asString() == gWtId && !m["pending"].asBool()) {
                gMsg = m;
                return true;
            }
        }
        return false;
    });
    MCGuiStep("its header says what it compares and it has the one file; the tab closes", [] {
        GCHECK_EQ_STR(gMsg["title"].asString(), "sbt_wt compared with main");
        GCHECK(gMsg["files"].size() == 1);
        GCHECK(gMsg.dump().find("sbt_wt.txt") != std::string::npos);
        GCHECK(gMsg["error"].isNull());
        Document* d = worktreeTab();
        if (!GCHECK(d != nullptr)) return;
        Ed()->closeTab(d);
    });
    MCGuiWait("the state no longer lists it", 20, [] {
        Json r;
        return !reviewListed(gWs3, gWtId, &r) && reviewCount(gWs3) == 0;
    });
    MCGuiStep("the worktree, the repository and the files go back", [] {
        Json o = Json::object();
        o.set("t", Json("review"));
        o.set("page", Json(gWtId));
        o.set("watch", Json(false));
        gWs3.sendJson(o);
        GCHECK(worktreeTab() == nullptr);
    });

    // ---- 7b. A commit's review page
    MCGuiStep("a commit page open in the window (HEAD, the commit made in section 6)", [] {
        const std::string cmd = "git -C '" + MCGuiProject() + "' rev-parse HEAD > '" + MCGuiScratch() + "/sbt_head.txt'";
        GCHECK(system(cmd.c_str()) == 0);
        gCmHash = MCGuiReadFile(MCGuiScratch() + "/sbt_head.txt");
        while (!gCmHash.empty() && (gCmHash.back() == '\n' || gCmHash.back() == ' ')) gCmHash.pop_back();
        unlink((MCGuiScratch() + "/sbt_head.txt").c_str());
        GCHECK_EQ_STR(std::to_string(gCmHash.size()), "40");
        gCmId = "commit:" + gCmHash;
        GCHECK(MCGuiApp().git != nullptr);
        auto page = std::make_shared<ChangeReviewPage>(MCGuiApp().git, MCGuiProject(), gCmHash, "new");
        Ed()->showPage(page);
    });
    MCGuiWait("the state lists it, on show", 20, [] {
        Json r;
        return reviewListed(gWs3, gCmId, &r) && r["onShow"].asBool();
    });
    MCGuiStep("kind commit, its hash (in the id) and title; ask for its review", [] {
        Json r;
        if (!GCHECK(reviewListed(gWs3, gCmId, &r))) return;
        GCHECK_EQ_STR(r["kind"].asString(), "commit");
        GCHECK_EQ_STR(r["id"].asString(), "commit:" + gCmHash);   // the view carries the hash in the id
        GCHECK_EQ_STR(r["title"].asString(), gCmHash.substr(0, 7) + " new");
        GCHECK(reviewCount(gWs3) == 1);
        gMark = gWs3.msgs.size();
        Json o = Json::object();
        o.set("t", Json("review"));
        o.set("page", Json(gCmId));
        gWs3.sendJson(o);
    });
    MCGuiWait("the read is done (pending, then reviewread)", 20, [] {
        gWs3.pump();
        for (size_t i = gMark; i < gWs3.msgs.size(); i++)
            if (gWs3.msgs[i]["t"].asString() == "reviewread" && gWs3.msgs[i]["page"].asString() == gCmId) return true;
        return false;
    });
    MCGuiStep("ask again, as the page does", [] {
        gMark = gWs3.msgs.size();
        Json o = Json::object();
        o.set("t", Json("review"));
        o.set("page", Json(gCmId));
        gWs3.sendJson(o);
    });
    MCGuiWait("the review of that commit", 20, [] {
        gWs3.pump();
        for (size_t i = gMark; i < gWs3.msgs.size(); i++) {
            const Json& m = gWs3.msgs[i];
            if (m["t"].asString() == "review" && m["page"].asString() == gCmId && !m["pending"].asBool()) {
                gMsg = m;
                return true;
            }
        }
        return false;
    });
    MCGuiStep("it has the commit's one file; the tab closes", [] {
        MCGuiNote("commit review: %s", gMsg.dump().substr(0, 300).c_str());
        GCHECK(gMsg["files"].size() == 1);
        GCHECK(gMsg.dump().find("sbt_new.txt") != std::string::npos);
        GCHECK(gMsg["error"].isNull());
        Document* d = commitTab();
        if (!GCHECK(d != nullptr)) return;
        Ed()->closeTab(d);
    });
    MCGuiWait("the state no longer lists it", 20, [] {
        Json r;
        return !reviewListed(gWs3, gCmId, &r) && reviewCount(gWs3) == 0;
    });
    MCGuiStep("the page's watch is dropped", [] {
        Json o = Json::object();
        o.set("t", Json("review"));
        o.set("page", Json(gCmId));
        o.set("watch", Json(false));
        gWs3.sendJson(o);
        GCHECK(commitTab() == nullptr);
    });

    MCGuiStep("the stand-in, the repository and the files go back", [] {
        Json o = Json::object();
        o.set("t", Json("git"));
        o.set("watch", Json(false));
        gWs3.sendJson(o);
        gWs3.shut();
        if (gHadTectonic) EnvLock::envSet("MUTIBLE_TECTONIC", gTectonicWas.c_str(), TRUE);
        else EnvLock::envUnset("MUTIBLE_TECTONIC");
        const std::string cmd = "rm -rf '" + MCGuiProject() + "/.git' '" + MCGuiScratch() + "/sb_tectonic' '" + gWtPath + "'";
        GCHECK(system(cmd.c_str()) == 0);
        for (const char* n : {"sbt_nt1.tex", "sbt_nt2.tex", "sbt_nt3.tex", "sbt_nt4.tex", "sbt_tab.tex", "sbt_bad.tex",
                              "sbt_base.txt", "sbt_new.txt"})
            unlink(MCGuiProjectFile(n).c_str());
    });
#endif

    // ---- 4 (end). Remembered Browsers…, Forget All
    MCGuiStep("Remembered Browsers… lists it", [] { activate(W(0).window, "rememberedsessionbrowsers"); });
    MCGuiWait("the list shows", 10, [] { return dialogShows("Forget All") && dialogShows("Done"); });
    MCGuiStep("Forget All", [] {
        GCHECK(SessionBrowser::remembered().size() == 1);
        GCHECK(clickDialog("Forget All"));
    });
    MCGuiWait("none remembered", 10, [] { return SessionBrowser::remembered().empty(); });
    MCGuiStep("the file no longer holds the id", [] {
        GCHECK(!fileHas(SessionBrowser::stateFile(), gJar3.browserId()));
    });

    // ---- 7. closing the window stops its server
    MCGuiStep("the second window serves too, then closes", [] {
        activate(W(1).window, "copysessionbrowserlink");
        SessionBrowser* b = W(1).sessionBrowser;
        GCHECK(b != nullptr);
        if (!b) return;
        gL2 = parseLink(b->url());
        GCHECK(gL2.port != 0 && gL2.port != SB()->port());
        Conn c;
        GCHECK(c.dial(gL2.port));
        c.shut();
        W(1).editor->closeAllTabs();
        gtk_window_close(W(1).window);
    });
    MCGuiWait("the window is gone", 10, [] { return MCGuiAppWindows().size() == 1; });
    MCGuiStep("its port refuses", [] {
        Conn c;
        GCHECK(!c.dial(gL2.port));
        c.shut();
        MCGuiFocusKeyWindow();
    });

    // ---- 8. put back
    MCGuiStep("what the suite changed goes back", [] {
        SB()->setEditable(true);
        SB()->stop();
        GCHECK(!SB()->running());
        SessionBrowser::setEditableChoice(true);
        SessionBrowser::forgetAll();
        SessionBrowser::opener = nullptr;
        Ed()->closeAllTabs();
        unlink(SessionBrowser::stateFile().c_str());
        for (const std::string& f : {gFileA, gFileB, gFileC, gFileD}) unlink(f.c_str());
        MCGuiFocusKeyWindow();
    });
    MCGuiStep("nothing left", [] {
        GCHECK(!MCGuiFileExists(SessionBrowser::stateFile()));
        GCHECK(MCGuiAppWindows().size() == 1);
        GCHECK(Ed()->documents().empty());
    });
}
