// session_web_tests.inc — the `sessionweb:*` groups of run_tests.cpp: a
// browser's read-only view of a session's host (SessionWeb.h, stage 6b).
// The request rules and refusals as pure functions; the host refusing a
// renderer that only reads anything that would change the session, and its
// views keeping no document open; the lines and terminal rows a page
// draws, kept by the same splices the page's script makes and compared with
// the host's text and screen; and the whole server over real sockets, with
// a scripted WebSocket client in place of the browser. Included after
// session_server_tests.inc, whose ServerRig, SocketRenderer, waitUntil and
// shellSpec it uses.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>

#include "IdeLink.h"
#include "SessionWeb.h"

namespace {

namespace Web = Shared::Web;

// What a page keeps of a document: its lines, as the script splices them.
struct PageDoc {
    Shared::DocId id = 0;
    std::string kind, html;
    std::vector<Json> lines;
    void take(const Json& m) {
        const std::string t = m["t"].asString();
        if (t == "doc") {
            id = (Shared::DocId)m["id"].asInt();
            kind = m["kind"].asString();
            html = m["html"].asString();
            lines = m["lines"].elements();
        } else if (t == "lines" && (Shared::DocId)m["id"].asInt() == id) {
            for (const Json& e : m["edits"].elements()) {
                const std::size_t at = (std::size_t)e[0].asInt(), del = (std::size_t)e[1].asInt();
                if (at > lines.size()) {
                    lines.clear();   // a broken edit: the text check fails
                    return;
                }
                lines.erase(lines.begin() + (long)at,
                            lines.begin() + (long)std::min(lines.size(), at + del));
                lines.insert(lines.begin() + (long)at, e[2].elements().begin(),
                             e[2].elements().end());
            }
        }
    }
    std::string text() const {
        std::string s;
        for (std::size_t i = 0; i < lines.size(); i++) {
            if (i) s += "\n";
            s += lines[i][0].asString();
        }
        return s;
    }
};

std::string runsText(const Json& line) {
    std::string s;
    for (const Json& r : line.elements()) s += r[0].asString();
    return s;
}

// What a page keeps of a terminal: scrollback lines and screen rows.
struct PageTerm {
    Shared::TermId id = 0;
    std::vector<Json> hist, screen;
    Json cursor;
    int snapshots = 0;
    void take(const Json& m) {
        const std::string t = m["t"].asString();
        if (t == "term") {
            id = (Shared::TermId)m["id"].asInt();
            hist = m["hist"].elements();
            screen = m["screen"].elements();
            cursor = m["cursor"];
            snapshots++;
        } else if (t == "termd" && (Shared::TermId)m["id"].asInt() == id) {
            for (const Json& l : m["hist"].elements()) hist.push_back(l);
            for (const Json& r : m["rows"].elements()) {
                const std::size_t row = (std::size_t)r[0].asInt();
                if (row < screen.size()) screen[row] = r[1];
            }
            cursor = m["cursor"];
        }
    }
    std::vector<std::string> rows() const {
        std::vector<std::string> out;
        for (const Json& l : screen) out.push_back(runsText(l));
        return out;
    }
    std::string all() const {
        std::string s;
        for (const Json& l : hist) s += runsText(l) + "\n";
        for (const Json& l : screen) s += runsText(l) + "\n";
        return s;
    }
};

// The host's screen as the page should draw it: each row, trailing blanks
// dropped.
std::vector<std::string> hostRows(ServerRig& rig, Shared::TermId t) {
    std::vector<std::string> out;
    rig.server->host().inspect([&](const Shared::Host& h) {
        if (const TerminalScreen* s = h.screen(t))
            for (int r = 0; r < s->rows(); r++) {
                std::string line = s->rowText(r);
                while (!line.empty() && line.back() == ' ') line.pop_back();
                out.push_back(line);
            }
    });
    return out;
}

// A browser, scripted: HTTP over a TCP socket, then WebSocket frames.
struct PageClient {
    int fd = -1;
    std::string in;
    ~PageClient() { close(); }
    void close() {
        if (fd >= 0) ::close(fd);
        fd = -1;
        in.clear();
    }
    bool connectTo(int port) {
        close();
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return fd >= 0 && ::connect(fd, (sockaddr*)&a, sizeof a) == 0;
    }
    bool write(const std::string& s) {
        std::size_t at = 0;
        while (at < s.size()) {
            const ssize_t n = ::write(fd, s.data() + at, s.size() - at);
            if (n <= 0) return false;
            at += (std::size_t)n;
        }
        return true;
    }
    // Reads what comes within `ms`; false when the other side closed.
    bool readSome(int ms) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, ms) <= 0) return true;
        char buf[65536];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) return false;
        in.append(buf, (std::size_t)n);
        return true;
    }
    // A whole HTTP exchange that ends with the server closing.
    std::string exchange(int port, const std::string& request) {
        if (!connectTo(port) || !write(request)) return "";
        const double end = nowMs() + 5000;
        while (nowMs() < end && readSome(100)) {}
        std::string r = in;
        close();
        return r;
    }
    // The upgrade: true with the 101 read, its answer in `head` either way.
    bool upgrade(int port, const std::string& target, std::string* head = nullptr) {
        if (!connectTo(port)) return false;
        const std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                                std::to_string(port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1:" +
                                std::to_string(port) + "\r\n\r\n";
        if (!write(req)) return false;
        const double end = nowMs() + 5000;
        std::size_t e;
        while ((e = in.find("\r\n\r\n")) == std::string::npos && nowMs() < end)
            if (!readSome(100)) break;
        if (e == std::string::npos) {
            if (head) *head = in;
            return false;
        }
        if (head) *head = in.substr(0, e);
        const bool ok = in.compare(0, 12, "HTTP/1.1 101") == 0 &&
                        in.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") < e;
        in.erase(0, e + 4);
        return ok;
    }
    void sendText(const std::string& text) {
        write(IdeLink::encodeClientFrame(IdeLink::Opcode::Text, text, 0x5a17c0de));
    }
    // The next server frame's text, if one is whole in `in`.
    bool frame(std::string* text, int* op) {
        if (in.size() < 2) return false;
        const unsigned char b0 = (unsigned char)in[0], b1 = (unsigned char)in[1];
        std::size_t len = b1 & 0x7f, at = 2;
        if (len == 126) {
            if (in.size() < 4) return false;
            len = ((unsigned char)in[2] << 8) | (unsigned char)in[3];
            at = 4;
        } else if (len == 127) {
            if (in.size() < 10) return false;
            len = 0;
            for (int i = 2; i < 10; i++) len = (len << 8) | (unsigned char)in[i];
            at = 10;
        }
        if (in.size() < at + len) return false;
        *op = b0 & 0x0f;
        *text = in.substr(at, len);
        in.erase(0, at + len);
        return true;
    }
    // Messages as they come, each to `each`, until `done` or `ms` pass.
    bool pump(const std::function<void(const Json&)>& each, const std::function<bool()>& done,
              int ms = 8000) {
        const double end = nowMs() + ms;
        for (;;) {
            std::string text;
            int op = 0;
            while (frame(&text, &op)) {
                Json j;
                if (op == 1 && Json::parse(text, j)) each(j);
            }
            if (done()) return true;
            if (nowMs() > end || !readSome(20)) return done();
        }
    }
};

Web::Request request(const std::string& text) { return Web::readRequest(text); }

std::string getWith(const std::string& target, const std::string& extra = "") {
    return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:8123\r\n" + extra + "\r\n";
}

}  // namespace

static void testSessionWeb() {
    using Shared::Message;
    GROUP("sessionweb:request");
    {
        Web::Request r = request("GET /ws?t=ab%2Fc&x=1&t=second HTTP/1.1\r\nHost: localhost:9\r\n"
                                 "X-Thing:  a b \r\n\r\nleft over");
        CHECK(r.state == Web::Request::Complete);
        CHECK(r.method == "GET" && r.path == "/ws" && r.target == "/ws?t=ab%2Fc&x=1&t=second");
        CHECK(r.query["t"] == "ab/c" && r.query["x"] == "1");   // the first t
        CHECK(r.headers["host"] == "localhost:9" && r.headers["x-thing"] == "a b");
        CHECK(std::string("GET /ws?t=ab%2Fc&x=1&t=second HTTP/1.1\r\nHost: localhost:9\r\nX-Thing:  a b \r\n\r\nleft over")
                  .substr(r.consumed) == "left over");
        CHECK(request("GET / HTTP/1.1\r\nHost: x").state == Web::Request::Incomplete);
        CHECK(request(std::string(Web::kMaxRequestBytes + 10, 'a')).state == Web::Request::Bad);
        CHECK(request("GARBAGE\r\n\r\n").state == Web::Request::Bad);
        CHECK(request("GET / HTTP/1.1\r\nno colon\r\n\r\n").state == Web::Request::Bad);
        // Two Hosts: one could be checked and the other obeyed.
        CHECK(request("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: evil.com\r\n\r\n").state ==
              Web::Request::Bad);
        CHECK(request("GET /?t=a+b%zz HTTP/1.1\r\nHost: a\r\n\r\n").query["t"] == "a b%zz");

        const std::string token = Web::newToken();
        CHECK(token.size() == 48 && token.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK(Web::newToken() != token);
    }

    GROUP("sessionweb:command-line");
    {
        using namespace ControlChannel;
        auto exists = [](const std::string&) { return true; };
        CliCommand c = parseCommandLine({"host", "--folder", "p", "--web", "8123"}, "/w", "/h", exists);
        CHECK(c.error.empty() && c.args["web"].asInt() == 8123 && c.args["folder"].asString() == "/w/p");
        c = parseCommandLine({"attach", "--folder", "p", "--web=8123"}, "/w", "/h", exists);
        CHECK(c.error.empty() && c.args["web"].asInt() == 8123 && !c.args.has("dump"));
        CHECK(!parseCommandLine({"attach", "--folder", "p", "--web", "8123", "--dump"}, "/w", "/h", exists)
                   .error.empty());
        for (const char* bad : {"0", "65536", "80x", "", "-1"})
            CHECK(parseCommandLine({"host", "--folder", "p", "--web", bad}, "/w", "/h", exists).error ==
                  "--web needs a port number (1 to 65535)");
        CHECK(usageText("host").find("--web PORT") != std::string::npos);
        CHECK(usageText("attach").find("--web PORT") != std::string::npos);
    }

    GROUP("sessionweb:refusals");
    {
        CHECK(Web::loopbackHost("127.0.0.1") && Web::loopbackHost("127.0.0.1:8123"));
        CHECK(Web::loopbackHost("localhost:80") && Web::loopbackHost("LOCALHOST"));
        CHECK(Web::loopbackHost("[::1]") && Web::loopbackHost("[::1]:8123"));
        CHECK(!Web::loopbackHost("") && !Web::loopbackHost("evil.com"));
        CHECK(!Web::loopbackHost("127.0.0.1.evil.com") && !Web::loopbackHost("localhost.evil.com:80"));
        CHECK(!Web::loopbackHost("127.0.0.2") && !Web::loopbackHost("0.0.0.0:8123"));
        CHECK(!Web::loopbackHost("127.0.0.1:") && !Web::loopbackHost("127.0.0.1:80x"));
        CHECK(!Web::loopbackHost("[::1]x") && !Web::loopbackHost("[::2]"));
        CHECK(Web::loopbackOrigin("http://127.0.0.1:8123") && Web::loopbackOrigin("http://localhost"));
        CHECK(!Web::loopbackOrigin("https://evil.com") && !Web::loopbackOrigin("null"));
        CHECK(!Web::loopbackOrigin("http://evil.com") && !Web::loopbackOrigin("127.0.0.1"));

        const std::string token = "0123456789abcdef0123456789abcdef0123456789abcdef";
        const std::string page = "<p>{{NONCE}} {{NONCE}}</p>";
        auto ask = [&](const std::string& text) { return Web::answer(request(text), token, page); };
        auto refused = [](const Web::Answer& a, int status) {
            return a.kind == Web::Answer::Refused && a.status == status &&
                   a.response.compare(0, 12, "HTTP/1.1 " + std::to_string(status)) == 0 &&
                   a.response.find("<p>") == std::string::npos;
        };
        // No token, an empty one, a wrong one, one a character short.
        Web::Answer a = ask(getWith("/"));
        CHECK(refused(a, 403) && a.why.find("no token") == 0);
        CHECK(refused(ask(getWith("/?t=")), 403));
        a = ask(getWith("/?t=" + std::string(48, 'f')));
        CHECK(refused(a, 403) && a.why.find("wrong token") == 0);
        CHECK(refused(ask(getWith("/?t=" + token.substr(1))), 403));
        CHECK(refused(ask(getWith("/?T=" + token)), 403));
        // The Host must be the loopback, whatever the token.
        a = Web::answer(request("GET /?t=" + token + " HTTP/1.1\r\nHost: evil.com\r\n\r\n"), token, page);
        CHECK(refused(a, 403) && a.why.find("Host") != std::string::npos);
        CHECK(refused(Web::answer(request("GET /?t=" + token + " HTTP/1.1\r\n\r\n"), token, page), 403));
        CHECK(refused(Web::answer(request("GET /?t=" + token + " HTTP/1.1\r\nHost: 10.0.0.2:8123\r\n\r\n"),
                                  token, page),
                      403));
        // A page from elsewhere is turned away even with the token.
        CHECK(refused(ask(getWith("/?t=" + token, "Origin: http://evil.com\r\n")), 403));
        CHECK(refused(ask(getWith("/ws?t=" + token, "Origin: https://127.0.0.1.evil.com\r\n")), 403));
        // Only GET; an unknown path only once the token is right.
        CHECK(refused(ask("POST /?t=" + token + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 405));
        CHECK(refused(ask(getWith("/x?t=" + token)), 404));
        CHECK(refused(ask(getWith("/x")), 403));
        // No server token at all serves nothing.
        CHECK(Web::answer(request(getWith("/?t=")), "", page).kind == Web::Answer::Refused);
        CHECK(Web::answer(request(getWith("/?t=x")), "", page).kind == Web::Answer::Refused);
        CHECK(refused(Web::answer(request("junk"), token, page), 400));

        // The page, with a fresh nonce in it and in its policy.
        a = ask(getWith("/?t=" + token, "Origin: http://localhost:8123\r\n"));
        CHECK(a.kind == Web::Answer::Page && a.status == 200);
        const std::size_t csp = a.response.find("script-src 'nonce-");
        CHECK(csp != std::string::npos);
        if (csp != std::string::npos) {
            const std::string nonce = a.response.substr(csp + 18, 32);
            CHECK(a.response.find("<p>" + nonce + " " + nonce + "</p>") != std::string::npos);
            CHECK(ask(getWith("/?t=" + token)).response.find(nonce) == std::string::npos);
        }
        CHECK(a.response.find("{{NONCE}}") == std::string::npos);
        CHECK(a.response.find("frame-ancestors 'none'") != std::string::npos);
        // The socket: the upgrade's headers, and the RFC's accept key.
        const std::string up = "Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                               "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
        a = ask(getWith("/ws?t=" + token, up));
        CHECK(a.kind == Web::Answer::Socket && a.status == 101);
        CHECK(a.response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
        CHECK(refused(ask(getWith("/ws?t=" + token)), 400));
        CHECK(refused(ask(getWith("/ws?t=" + token, "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                                      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: c2hvcnQ=\r\n")),
                      400));
        CHECK(refused(ask(getWith("/ws", up)), 403));
        CHECK(refused(ask(getWith("/ws?t=nope", up)), 403));
        // The real page has its nonce slot and draws nothing it is not sent.
        CHECK(Web::pageHtml().find("<script nonce=\"{{NONCE}}\">") != std::string::npos);
        CHECK(Web::pageHtml().find("http://") == std::string::npos);
        CHECK(Web::pageHtml().find("https://") == std::string::npos);
    }

    GROUP("sessionweb:viewer-reads-only");
    {
        std::map<Shared::ClientId, std::vector<Message>> got;
        Shared::Host host([&](Shared::ClientId to, Message&& m) { got[to].push_back(std::move(m)); });
        auto say = [&](Shared::ClientId c, Message m) { host.handle(c, std::move(m)); };
        auto kinds = [&](Shared::ClientId c, Shared::Kind k) {
            int n = 0;
            for (const Message& m : got[c]) n += m.kind == k;
            return n;
        };
        const Shared::ClientId a = host.connect(), v = host.connect();
        Message m;
        m.kind = Shared::Kind::Hello;
        say(a, m);
        m.flag = 1;   // only reads
        say(v, m);
        m = Message();
        m.kind = Shared::Kind::DocOpen;
        m.doc = 1;
        m.name = "/p/a.txt";
        m.text8 = "abc";
        say(a, m);
        // A view of it: its text, as it is.
        m = Message();
        m.kind = Shared::Kind::DocOpen;
        m.doc = 1;
        m.text8 = "ignored";
        say(v, m);
        CHECK(!got[v].empty() && got[v].back().kind == Shared::Kind::DocOpened &&
              got[v].back().text16 == u"abc");
        CHECK(host.text(1) == u"abc");
        // A view never makes a document.
        m.doc = 9;
        say(v, m);
        CHECK(!host.hasDocument(9) && kinds(v, Shared::Kind::DocGone) == 1);
        // Anything that would change the session is refused.
        const int errors = kinds(v, Shared::Kind::Error);
        Message e;
        e.kind = Shared::Kind::DocEdit;
        e.doc = 1;
        e.ops = {Ot::Op{0, 1, u"X"}};
        say(v, e);
        e = Message();
        e.kind = Shared::Kind::DocEdit;
        e.doc = 1;
        e.flag = 1;
        e.ops = {Ot::Op{0, 0, u"Y"}};
        say(v, e);
        for (Shared::Kind k : {Shared::Kind::DocReset, Shared::Kind::DocUndo, Shared::Kind::DocSaved,
                               Shared::Kind::DocForget, Shared::Kind::WindowTabs,
                               Shared::Kind::WindowClose, Shared::Kind::TermOpen,
                               Shared::Kind::TermInput, Shared::Kind::TermResize,
                               Shared::Kind::TermClose}) {
            Message x;
            x.kind = k;
            x.doc = 1;
            x.term = 1;
            x.window = 1;
            x.text8 = "zzz";
            x.bytes = "rm -rf ~\n";
            x.cols = 10;
            x.rows = 10;
            x.json = Json::object();
            say(v, x);
        }
        CHECK(kinds(v, Shared::Kind::Error) == errors + 12);
        CHECK(host.text(1) == u"abc" && !host.dirty(1) && host.state()["windows"].size() == 0);
        CHECK(host.terminalCount() == 0);
        // Its report of how far it has read is not an edit, and is taken.
        e = Message();
        e.kind = Shared::Kind::DocEdit;
        e.doc = 1;
        say(v, e);
        CHECK(kinds(v, Shared::Kind::Error) == errors + 12);
        // The editing renderer's edits reach the view.
        e = Message();
        e.kind = Shared::Kind::DocEdit;
        e.doc = 1;
        e.seq = 1;
        e.ops = {Ot::Op{3, 0, u"d"}};
        say(a, e);
        CHECK(host.text(1) == u"abcd" && kinds(v, Shared::Kind::DocEdited) == 1);
        // A view closing keeps the document; the last editor closing ends
        // it, and the views hear so.
        Message c;
        c.kind = Shared::Kind::DocClose;
        c.doc = 1;
        say(v, c);
        CHECK(host.hasDocument(1));
        m.doc = 1;
        say(v, m);
        say(a, c);
        CHECK(!host.hasDocument(1) && kinds(v, Shared::Kind::DocGone) == 2);
        // A document a renderer left on detaching stays for the next, even
        // when a view of it comes and goes.
        m = Message();
        m.kind = Shared::Kind::DocOpen;
        m.doc = 2;
        m.name = "/p/b.txt";
        m.text8 = "kept";
        say(a, m);
        host.disconnect(a);
        m.text8.clear();
        say(v, m);
        CHECK(got[v].back().kind == Shared::Kind::DocOpened && got[v].back().text16 == u"kept");
        c.doc = 2;
        say(v, c);
        CHECK(host.hasDocument(2) && host.text(2) == u"kept");
        // The flag survives the wire.
        Message h;
        h.kind = Shared::Kind::Hello;
        h.flag = 1;
        CHECK(Shared::roundTrip(h).flag == 1);
        Message g;
        g.kind = Shared::Kind::DocGone;
        g.doc = 5;
        CHECK(Shared::roundTrip(g).kind == Shared::Kind::DocGone && Shared::roundTrip(g).doc == 5);
    }

    GROUP("sessionweb:doc-lines");
    {
        // Random edits; the page's lines, spliced, stay the text, and each
        // line's spans are a fresh highlight's.
        std::mt19937 rng(61);
        const char16_t* pieces[] = {u"def ", u"x", u"\n", u"\r\n", u"# note\n", u"\"s\"", u"(1)",
                                    u"    ", u"é", u"😀", u"'''\n", u"return 2\n"};
        for (int round = 0; round < 40; round++) {
            Web::DocView v(1, round % 2 ? "/p/a.py" : "/p/notes.txt");
            v.apply(Ot::Ops{Ot::Op{0, 0, u"def f():\n    return 1\n"}});
            PageDoc page;
            page.take(v.snapshot());
            v.takeEdits();
            for (int step = 0; step < 30; step++) {
                Ot::Ops ops;
                std::u16string t = v.text();
                const int n = 1 + (int)(rng() % 3);
                for (int k = 0; k < n; k++) {
                    std::size_t pos = rng() % (t.size() + 1);
                    // Never inside a surrogate pair.
                    if (pos > 0 && pos < t.size() && (t[pos] & 0xFC00) == 0xDC00) pos--;
                    std::size_t del = rng() % 4 == 0 ? 0 : rng() % 6;
                    del = std::min(del, t.size() - pos);
                    if (pos + del < t.size() && (t[pos + del] & 0xFC00) == 0xDC00) del++;
                    const std::u16string ins = rng() % 3 ? pieces[rng() % 12] : u"";
                    Ot::Op o{pos, del, ins};
                    t.replace(pos, del, ins);
                    ops.push_back(o);
                }
                v.apply(ops);
                if (v.changed()) {
                    page.take(v.snapshot());
                    v.clearChanged();
                    v.takeEdits();
                } else {
                    const Json e = v.takeEdits();
                    if (!e.isNull()) page.take(e);
                }
            }
            Web::DocView fresh(1, v.path());
            fresh.apply(Ot::Ops{Ot::Op{0, 0, v.text()}});
            const Json want = fresh.snapshot()["lines"];
            bool same = want.size() == page.lines.size();
            for (std::size_t i = 0; same && i < page.lines.size(); i++) same = page.lines[i] == want[i];
            CHECK(same);
            if (!same) break;
        }
        // Spans name the highlighter's styles, in UTF-16 units.
        Web::DocView py(1, "/p/x.py");
        py.apply(Ot::Ops{Ot::Op{0, 0, u"é = 1  # c\ndef g(): pass"}});
        const Json snap = py.snapshot();
        CHECK(snap["kind"].asString() == "code" && snap["lines"].size() == 2);
        const Json& l0 = snap["lines"][0];
        CHECK(l0[0].asString() == "é = 1  # c");
        bool comment = false, number = false;
        for (const Json& s : l0[1].elements()) {
            comment = comment || (s[0].asInt() == 7 && s[1].asInt() == 3 && s[2].asInt() == (int)TokenStyle::Comment);
            number = number || (s[0].asInt() == 4 && s[2].asInt() == (int)TokenStyle::Number);
        }
        CHECK(comment && number);
        CHECK(snap["lines"][1][1][0][2].asInt() == (int)TokenStyle::Keyword);
        // Markdown is MarkdownHtml's page, with what it sanitizes gone.
        Web::DocView md(2, "/p/README.md");
        md.apply(Ot::Ops{Ot::Op{0, 0, u"# Title\n\n<script>alert(1)</script> [x](javascript:alert(2))\n"}});
        CHECK(md.changed());
        const Json page = md.snapshot();
        CHECK(page["kind"].asString() == "markdown");
        CHECK(page["html"].asString().find("<h1") != std::string::npos);
        CHECK(page["html"].asString().find("<script") == std::string::npos);
        CHECK(page["html"].asString().find("javascript:") == std::string::npos);
        // Too large for the page yet: said, not sent.
        Web::DocView big(3, "/p/big.txt");
        big.apply(Ot::Ops{Ot::Op{0, 0, std::u16string(Web::DocView::kMaxUnits + 1, u'a')}});
        CHECK(big.snapshot()["kind"].asString() == "large" && !big.snapshot().has("lines"));
    }

    GROUP("sessionweb:term-rows");
    {
        Web::TermView v(1, 20, 5);
        PageTerm page;
        page.take(v.snapshot());
        CHECK(page.screen.size() == 5 && page.hist.empty());
        v.feed("hello\r\n\x1b[1;31mred\x1b[0m plain\r\n");
        page.take(v.takeUpdate());
        CHECK(page.rows()[0] == "hello" && page.rows()[1] == "red plain");
        // The red run carries its color and bold.
        const Json& red = page.screen[1][0];
        CHECK(red[0].asString() == "red" && red[1].asInt() >= 0 && (red[3].asInt() & Web::kBold));
        CHECK(page.screen[1][1].size() == 1);   // " plain": the default style
        CHECK(v.takeUpdate().isNull());          // nothing changed since
        for (int i = 0; i < 30; i++) v.feed("line " + std::to_string(i) + "\r\n");
        page.take(v.takeUpdate());
        std::vector<std::string> want;
        for (int r = 0; r < v.screen().rows(); r++) {
            std::string s = v.screen().rowText(r);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            want.push_back(s);
        }
        CHECK(page.rows() == want);
        CHECK(page.hist.size() == (std::size_t)v.screen().historySize());
        CHECK(page.all().find("hello\nred plain\nline 0\n") == 0);
        CHECK(page.cursor[0].asInt() == v.screen().cursorRow() && page.cursor[2].asBool());
        // A clear is the whole terminal again.
        v.feed("\x1b[2J\x1b[3J\x1b[H");
        const Json u = v.takeUpdate();
        CHECK(u["t"].asString() == "term");
        page.take(u);
        CHECK(page.hist.empty() && page.rows()[0].empty());
        // Wide characters are one cell's text.
        v.feed("日本x");
        page.take(v.takeUpdate());
        CHECK(page.rows()[0] == "日本x");
    }

    GROUP("sessionweb:server");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "editor"));
        const std::string home = scratchFolder();
        auto server = std::make_unique<Web::Server>();
        Web::Server& web = *server;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = rig.proj;
        o.machine = "testbox";
        o.pollMs = 100;
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        const int port = web.port();
        CHECK(port > 0 && web.url() == "http://127.0.0.1:" + std::to_string(port) + "/?t=" + web.token());
        CHECK(web.token().size() == 48);
        if (a.client && port > 0) {
            const std::u16string text = u"def f(x):\n    return x + 1\n";
            a.mirror.doc = 1;
            a.mirror.text = text;
            a.client->open(1, rig.proj + "/f.py", Model::utf8FromUtf16(text), &a.mirror);
            a.client->openTerminal(7, shellSpec(home, {"sh"}), &a);
            CHECK(a.wait([&] { return a.pid > 0 && a.out.find("$ ") != std::string::npos; }));

            // Over the wire: refusals, and nothing attached for them.
            PageClient p;
            std::string r = p.exchange(port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            CHECK(r.compare(0, 12, "HTTP/1.1 403") == 0 && r.find("<html") == std::string::npos);
            r = p.exchange(port, "GET /?t=" + web.token() + " HTTP/1.1\r\nHost: attacker.example\r\n\r\n");
            CHECK(r.compare(0, 12, "HTTP/1.1 403") == 0);
            r = p.exchange(port, "GET /?t=" + std::string(48, '0') + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            CHECK(r.compare(0, 12, "HTTP/1.1 403") == 0);
            std::string head;
            CHECK(!p.upgrade(port, "/ws?t=wrong", &head) && head.compare(0, 12, "HTTP/1.1 403") == 0);
            p.close();
            r = p.exchange(port, "GET /?t=" + web.token() + " HTTP/1.1\r\nHost: localhost:" +
                                     std::to_string(port) + "\r\n\r\n");
            CHECK(r.compare(0, 12, "HTTP/1.1 200") == 0 && r.find("<!DOCTYPE html>") != std::string::npos);
            CHECK(rig.server->renderers() == 1 && web.browsers() == 0);

            // A browser: hello, then the session's state.
            CHECK(p.upgrade(port, "/ws?t=" + web.token()));
            Json hello, state;
            PageDoc doc;
            PageTerm term;
            int gone = 0;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                else if (t == "gone") gone++;
                doc.take(m);
                term.take(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(hello["machine"].asString() == "testbox" && hello["folder"].asString() == rig.proj);
            CHECK(hello["palette"]["syntax"].size() == (std::size_t)kTokenStyleCount);
            CHECK(state["docs"].size() == 1 && state["docs"][0]["path"].asString() == rig.proj + "/f.py");
            CHECK(state["terms"].size() == 1 && state["terms"][0]["id"].asInt() == 7);
            CHECK(waitUntil([&] { return rig.server->renderers() == 2; }) && web.browsers() == 1);

            // It shows the document and the terminal: whole, then live.
            p.sendText("{\"t\":\"show\",\"doc\":1,\"term\":7}");
            CHECK(p.pump(take, [&] { return doc.id == 1 && term.snapshots == 1; }));
            CHECK(doc.kind == "code" && doc.text() == Model::utf8FromUtf16(text));
            CHECK(!doc.lines.empty() && doc.lines[0][1][0][2].asInt() == (int)TokenStyle::Keyword);
            a.mirror.caret = 0;
            for (int i = 0; i < 25; i++) {
                a.mirror.type(i % 6 == 5 ? u"\n" : u"z");
                if (i % 4 == 0) a.client->dispatch();
            }
            a.mirror.caret = a.mirror.text.size();
            a.mirror.type(u"# end\n");
            CHECK(a.client->settle());
            const std::string hostText = Model::utf8FromUtf16(a.client->hostDocument(1).text);
            CHECK(p.pump(take, [&] { return doc.text() == hostText; }));
            CHECK(doc.text() == hostText);

            a.client->terminalInput(7, "echo web-$((6*7)); i=0; while [ $i -lt 40 ]; do echo row$i; i=$((i+1)); done\n");
            CHECK(p.pump(take, [&] { return term.all().find("row39") != std::string::npos; }));
            CHECK(p.pump(take, [&] { return term.rows() == hostRows(rig, 7); }));
            CHECK(term.rows() == hostRows(rig, 7));
            CHECK(term.all().find("web-42\nrow0\nrow1\n") != std::string::npos);   // scrollback kept

            // The browser can change nothing: anything but "show" is
            // dropped, and the host refuses its renderer's edits anyway.
            p.sendText("{\"t\":\"edit\",\"doc\":1,\"ops\":[[0,0,\"X\"]]}");
            p.sendText("{\"t\":\"input\",\"term\":7,\"bytes\":\"echo pwned\\n\"}");
            p.write(IdeLink::encodeClientFrame(IdeLink::Opcode::Binary, "\x01\x02", 7));
            p.pump(take, [] { return false; }, 400);
            CHECK(Model::utf8FromUtf16(a.client->hostDocument(1).text) == hostText);
            CHECK(a.out.find("pwned") == std::string::npos);

            // A second browser gets the same, whole, and both follow.
            PageClient q;
            CHECK(q.upgrade(port, "/ws?t=" + web.token()));
            PageDoc doc2;
            Json state2;
            auto take2 = [&](const Json& m) {
                if (m["t"].asString() == "state") state2 = m;
                doc2.take(m);
            };
            CHECK(q.pump(take2, [&] { return state2.isObject(); }));
            q.sendText("{\"t\":\"show\",\"doc\":1,\"term\":0}");
            CHECK(q.pump(take2, [&] { return doc2.text() == hostText; }));
            a.mirror.type(u"more\n");
            CHECK(a.client->settle());
            const std::string hostText2 = Model::utf8FromUtf16(a.client->hostDocument(1).text);
            CHECK(p.pump(take, [&] { return doc.text() == hostText2; }));
            CHECK(q.pump(take2, [&] { return doc2.text() == hostText2; }));
            CHECK(rig.server->renderers() == 2);   // one renderer for every browser

            // A Markdown document is MarkdownHtml's page; closing it is said.
            a.client->open(2, rig.proj + "/README.md", "# Notes\n\n*hi*\n", nullptr);
            CHECK(a.client->settle());
            CHECK(p.pump(take, [&] { return state["docs"].size() == 2; }));
            p.sendText("{\"t\":\"show\",\"doc\":2,\"term\":7}");
            CHECK(p.pump(take, [&] { return doc.id == 2; }));
            CHECK(doc.kind == "markdown" && doc.html.find("<em>hi</em>") != std::string::npos);
            a.client->close(2);
            CHECK(p.pump(take, [&] { return gone == 1; }));
            // A document that is not there is said to be gone, never made.
            p.sendText("{\"t\":\"show\",\"doc\":99,\"term\":7}");
            CHECK(p.pump(take, [&] { return gone == 2; }));
            CHECK(a.client->sessionState()["documents"].size() == 1);

            // The browsers leave: the view detaches from the host.
            p.close();
            q.close();
            CHECK(waitUntil([&] { return web.browsers() == 0 && rig.server->renderers() == 1; }));
            a.client->closeTerminal(7);
        }
        web.stop();
        webThread.join();
        server.reset();
        // Gone: nothing listens.
        PageClient late;
        CHECK(!late.connectTo(port));
    }
}
