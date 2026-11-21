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
//   - closing the window stops its server (the port refuses).
// Nothing opens a real browser (SessionBrowser::opener), and the state file,
// the files and the windows are put back.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"
#include "IdeLink.h"
#include "Json.h"
#include "SessionBrowser.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
