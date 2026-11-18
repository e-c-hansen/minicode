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
            if (m.has("all")) lines = m["all"].elements();
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
    // `origin`: "" for the page's own, "-" for none.
    bool upgrade(int port, const std::string& target, std::string* head = nullptr,
                 const std::string& origin = "") {
        if (!connectTo(port)) return false;
        const std::string own = "http://127.0.0.1:" + std::to_string(port);
        const std::string o = origin.empty() ? own : origin;
        const std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                                std::to_string(port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n" +
                                (o == "-" ? std::string() : "Origin: " + o + "\r\n") + "\r\n";
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

// A page that edits, as the page's script does (SessionWebPage.cpp): the
// server's lines at its version, one edit in flight and the rest waiting,
// each moved past what others did (Ot::transform, the server's winning
// ties), and a command's `fix` taken as a change after its own.
struct PageEditor {
    PageClient* p = nullptr;
    Shared::DocId id = 0;
    std::uint64_t version = 0;
    std::vector<Json> lines;
    struct Edit {
        std::uint64_t seq = 0;
        Ot::Ops ops;
        std::string cmd, name = "Typing";
        bool coalesce = true;
        std::size_t at0 = 0, at1 = 0, after = 0;
    };
    bool flying = false;
    Edit flight;
    std::deque<Edit> queue;
    std::uint64_t seq = 0;
    int refusals = 0, docs = 0;
    std::vector<std::string> notes;
    Json lastSel;
    bool editable = false;

    std::u16string shadow() const {
        static const char16_t* const breaks[] = {u"", u"\n", u"\r\n", u"\r", u"\u2029"};
        std::u16string t;
        for (const Json& l : lines) {
            t += Model::utf16FromUtf8(l[0].asString());
            t += l.size() > 2 ? breaks[(int)l[2].asInt()] : u"\n";
        }
        return t;
    }
    std::u16string text() const {
        std::u16string t = shadow();
        if (flying) Ot::apply(t, flight.ops, Model::Unit::Utf16);
        for (const Edit& e : queue) Ot::apply(t, e.ops, Model::Unit::Utf16);
        return t;
    }
    bool synced() const { return !flying && queue.empty(); }
    void move(Ot::Ops f) {
        if (flying) {
            Ot::Ops a, b;
            Ot::transform(flight.ops, f, Model::Unit::Utf16, &a, &b);
            flight.ops = a;
            f = b;
        }
        for (Edit& e : queue) {
            e.at0 = Ot::mapOffset(e.at0, f, Model::Unit::Utf16);
            e.at1 = Ot::mapOffset(e.at1, f, Model::Unit::Utf16);
            Ot::Ops a, b;
            Ot::transform(e.ops, f, Model::Unit::Utf16, &a, &b);
            e.ops = a;
            f = b;
            e.after = Ot::mapOffset(e.after, f, Model::Unit::Utf16);
        }
    }
    static Ot::Ops opsOf(const Json& j) {
        Ot::Ops out;
        for (const Json& o : j.elements())
            out.push_back(Ot::Op{(std::size_t)o[0].asInt(), (std::size_t)o[1].asInt(),
                                 Model::utf16FromUtf8(o[2].asString())});
        return out;
    }
    void take(const Json& m) {
        const std::string t = m["t"].asString();
        if (t == "hello") editable = m["editable"].asBool();
        if (t == "note") notes.push_back(m["text"].asString());
        if ((Shared::DocId)m["id"].asInt() != id) return;
        if (t == "doc") {
            lines = m["lines"].elements();
            version = (std::uint64_t)m["version"].asInt();
            flying = false;
            queue.clear();
            docs++;
        } else if (t == "lines") {
            if (m.has("all")) lines = m["all"].elements();
            for (const Json& e : m["edits"].elements()) {
                const std::size_t at = (std::size_t)e[0].asInt(), del = (std::size_t)e[1].asInt();
                if (at > lines.size()) return;
                lines.erase(lines.begin() + (long)at, lines.begin() + (long)std::min(lines.size(), at + del));
                lines.insert(lines.begin() + (long)at, e[2].elements().begin(), e[2].elements().end());
            }
            version = (std::uint64_t)m["version"].asInt();
            if (m.has("sel")) lastSel = m["sel"];
            if (m.has("seq")) {
                if (flying && flight.seq == (std::uint64_t)m["seq"].asInt()) flying = false;
                if (m.has("fix")) move(opsOf(m["fix"]));
            } else {
                move(opsOf(m["ops"]));
            }
            sendNext();
        } else if (t == "refused") {
            flying = false;
            queue.clear();
            refusals++;
        }
    }
    void sendNext() {
        if (flying || queue.empty()) return;
        flight = queue.front();
        queue.pop_front();
        flight.seq = ++seq;
        flying = true;
        Json m = Json::object();
        m.set("t", Json("edit"));
        m.set("doc", Json((int64_t)id));
        m.set("base", Json((int64_t)version));
        m.set("seq", Json((int64_t)flight.seq));
        Json ops = Json::array();
        for (const Ot::Op& o : flight.ops) {
            Json e = Json::array();
            e.push(Json((int64_t)o.pos));
            e.push(Json((int64_t)o.del));
            e.push(Json(Model::utf8FromUtf16(o.ins)));
            ops.push(std::move(e));
        }
        m.set("ops", std::move(ops));
        Json step = Json::object();
        step.set("name", Json(flight.name));
        step.set("coalesce", Json(flight.coalesce));
        m.set("step", std::move(step));
        Json before = Json::array(), after = Json::array();
        before.push(Json((int64_t)flight.at0));
        before.push(Json((int64_t)flight.at1));
        after.push(Json((int64_t)flight.after));
        after.push(Json((int64_t)flight.after));
        m.set("before", before);
        m.set("after", after);
        if (!flight.cmd.empty()) {
            m.set("cmd", Json(flight.cmd));
            m.set("at", before);
        }
        p->sendText(m.dump());
    }
    // An edit on the text on show, made at once; the selection before it
    // is what one operation replaced (or `at` for a command), the caret
    // after it the end of what it put in.
    void edit(Ot::Ops ops, const std::string& cmd = "", std::size_t at0 = 0, std::size_t at1 = 0,
              const std::string& name = "Typing", bool coalesce = true) {
        Edit e;
        if (cmd.empty() && ops.size() == 1) {
            at0 = ops[0].pos;
            at1 = ops[0].pos + ops[0].del;
        }
        e.after = ops.size() == 1 ? ops[0].pos + ops[0].ins.size() : at0;
        e.ops = std::move(ops);
        e.cmd = cmd;
        e.at0 = at0;
        e.at1 = at1;
        e.name = name;
        e.coalesce = coalesce;
        queue.push_back(std::move(e));
        sendNext();
    }
};

Web::Request request(const std::string& text) { return Web::readRequest(text); }

void writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

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
        c = parseCommandLine({"host", "--folder", "p", "--web", "8123", "--edit"}, "/w", "/h", exists);
        CHECK(c.error.empty() && c.args["edit"].asBool());
        c = parseCommandLine({"attach", "--folder", "p", "--web", "8123", "--edit"}, "/w", "/h", exists);
        CHECK(c.error.empty() && c.args["edit"].asBool());
        CHECK(parseCommandLine({"host", "--folder", "p", "--edit"}, "/w", "/h", exists).error ==
              "--edit goes with --web PORT");
        CHECK(usageText("host").find("--edit") != std::string::npos);
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
        // Only the page's own origin: the Host it was asked under.
        CHECK(Web::sameOrigin("http://127.0.0.1:8123", "127.0.0.1:8123"));
        CHECK(Web::sameOrigin("http://LOCALHOST:9000", "localhost:9000"));
        CHECK(Web::sameOrigin("http://127.0.0.1", "127.0.0.1:80") && Web::sameOrigin("http://[::1]:5", "[::1]:5"));
        CHECK(!Web::sameOrigin("http://localhost:8123", "127.0.0.1:8123"));
        CHECK(!Web::sameOrigin("http://127.0.0.1:8124", "127.0.0.1:8123"));
        CHECK(!Web::sameOrigin("http://evil.com:8123", "evil.com:8123"));
        CHECK(!Web::sameOrigin("https://127.0.0.1:8123", "127.0.0.1:8123") && !Web::sameOrigin("null", "127.0.0.1"));
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
        // A page from elsewhere is turned away even with the token, and so
        // is another loopback page: only the page's own origin.
        CHECK(refused(ask(getWith("/?t=" + token, "Origin: http://evil.com\r\n")), 403));
        CHECK(refused(ask(getWith("/ws?t=" + token, "Origin: https://127.0.0.1.evil.com\r\n")), 403));
        CHECK(refused(ask(getWith("/?t=" + token, "Origin: http://localhost:8123\r\n")), 403));
        CHECK(refused(ask(getWith("/ws?t=" + token, "Origin: http://127.0.0.1:9999\r\n")), 403));
        // Only GET; an unknown path only once the token is right.
        CHECK(refused(ask("POST /?t=" + token + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 405));
        CHECK(refused(ask(getWith("/x?t=" + token)), 404));
        CHECK(refused(ask(getWith("/x")), 403));
        // No server token at all serves nothing.
        CHECK(Web::answer(request(getWith("/?t=")), "", page).kind == Web::Answer::Refused);
        CHECK(Web::answer(request(getWith("/?t=x")), "", page).kind == Web::Answer::Refused);
        CHECK(refused(Web::answer(request("junk"), token, page), 400));

        // The page, with a fresh nonce in it and in its policy.
        a = ask(getWith("/?t=" + token, "Origin: http://127.0.0.1:8123\r\n"));
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
                               "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                               "Origin: http://127.0.0.1:8123\r\n";
        a = ask(getWith("/ws?t=" + token, up));
        CHECK(a.kind == Web::Answer::Socket && a.status == 101);
        CHECK(a.response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
        // The socket opens only with an Origin, which every browser sends.
        const std::string upNoOrigin = up.substr(0, up.find("Origin:"));
        a = ask(getWith("/ws?t=" + token, upNoOrigin));
        CHECK(refused(a, 403) && a.why.find("own page") != std::string::npos);
        CHECK(refused(ask(getWith("/ws?t=" + token, "Origin: http://127.0.0.1:8123\r\n")), 400));
        CHECK(refused(ask(getWith("/ws?t=" + token, "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                                      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: c2hvcnQ=\r\n"
                                                      "Origin: http://127.0.0.1:8123\r\n")),
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

    GROUP("sessionweb:guest");
    {
        // A browser's view that may edit: its seats are views, as a
        // reader's, and it may edit, undo and type, nothing more.
        std::map<Shared::ClientId, std::vector<Message>> got;
        Shared::Host host([&](Shared::ClientId to, Message&& m) { got[to].push_back(std::move(m)); });
        auto say = [&](Shared::ClientId c, Message m) { host.handle(c, std::move(m)); };
        auto kinds = [&](Shared::ClientId c, Shared::Kind k) {
            int n = 0;
            for (const Message& m : got[c]) n += m.kind == k;
            return n;
        };
        const Shared::ClientId a = host.connect(), g = host.connect();
        Message m;
        m.kind = Shared::Kind::Hello;
        say(a, m);
        m.flag = 2;   // a guest
        say(g, m);
        CHECK(Shared::roundTrip(m).flag == 2);
        m = Message();
        m.kind = Shared::Kind::DocOpen;
        m.doc = 1;
        m.name = "/p/a.txt";
        m.text8 = "abc";
        say(a, m);
        m.text8 = "ignored";
        say(g, m);
        CHECK(got[g].back().kind == Shared::Kind::DocOpened && got[g].back().text16 == u"abc");
        m.doc = 9;
        say(g, m);
        CHECK(!host.hasDocument(9) && kinds(g, Shared::Kind::DocGone) == 1);
        // Its edit goes into the history and reaches the editor.
        Message e;
        e.kind = Shared::Kind::DocEdit;
        e.doc = 1;
        e.seq = 1;
        e.base = 0;
        e.step.id = 1;
        e.step.open = e.step.close = true;
        e.step.name = "Typing";
        e.ops = {Ot::Op{3, 0, u"d"}};
        say(g, e);
        CHECK(host.text(1) == u"abcd" && host.undoInfo(1).canUndo);
        CHECK(kinds(a, Shared::Kind::DocEdited) == 1 && kinds(g, Shared::Kind::Error) == 0);
        Message u;
        u.kind = Shared::Kind::DocUndo;
        u.doc = 1;
        u.req = 5;
        say(g, u);
        CHECK(host.text(1) == u"abc");
        // Nothing else that changes the session.
        const int errors = kinds(g, Shared::Kind::Error);
        for (Shared::Kind k : {Shared::Kind::DocReset, Shared::Kind::DocSaved, Shared::Kind::DocForget,
                               Shared::Kind::WindowTabs, Shared::Kind::WindowClose,
                               Shared::Kind::TermOpen, Shared::Kind::TermResize, Shared::Kind::TermClose}) {
            Message x;
            x.kind = k;
            x.doc = 1;
            x.term = 1;
            x.window = 1;
            x.text8 = "zzz";
            x.cols = 10;
            x.rows = 10;
            x.json = Json::object();
            say(g, x);
        }
        Message outside;
        outside.kind = Shared::Kind::DocEdit;
        outside.doc = 1;
        outside.flag = 1;
        outside.ops = {Ot::Op{0, 0, u"Y"}};
        say(g, outside);
        CHECK(kinds(g, Shared::Kind::Error) == errors + 9);
        CHECK(host.text(1) == u"abc" && host.state()["windows"].size() == 0 && host.terminalCount() == 0);
        // Its seat going keeps the document.
        Message c;
        c.kind = Shared::Kind::DocClose;
        c.doc = 1;
        say(g, c);
        CHECK(host.hasDocument(1));
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
        CHECK(md.htmlStale());
        const Json page = md.snapshot();
        CHECK(page["kind"].asString() == "markdown");
        CHECK(page["html"].asString().find("<h1") != std::string::npos);
        CHECK(page["html"].asString().find("<script") == std::string::npos);
        CHECK(page["html"].asString().find("javascript:") == std::string::npos);
        CHECK(page["lines"].size() == 4);   // its source too, which a page edits
        // Each line's break: none on the last, "\r\n" and U+2029 said.
        Web::DocView br(4, "/p/b.txt");
        br.apply(Ot::Ops{Ot::Op{0, 0, u"a\r\nb\nc\u2029d\re"}});
        const Json bl = br.snapshot()["lines"];
        CHECK(bl.size() == 5 && bl[0].size() == 3 && bl[0][2].asInt() == 2 && bl[1].size() == 2 &&
              bl[2][2].asInt() == 4 && bl[3][2].asInt() == 3 && bl[4][2].asInt() == 0);
        // Versions: each commit is the next, and the log gives what came
        // after any it still holds.
        Web::DocView vv(5, "/p/v.txt");
        vv.commit(Ot::Ops{Ot::Op{0, 0, u"abc"}}, 0);
        vv.commit(Ot::Ops{Ot::Op{3, 0, u"d"}}, 2, 7);
        CHECK(vv.version() == 2 && vv.queued().size() == 2 && vv.queued().back().seq == 7);
        Ot::Ops since;
        CHECK(vv.since(1, &since) && since.size() == 1 && since[0].ins == u"d");
        CHECK(vv.since(2, &since) && since.empty() && !vv.since(3, &since));
        // Too large for the page yet: said, not sent.
        Web::DocView big(3, "/p/big.txt");
        big.apply(Ot::Ops{Ot::Op{0, 0, std::u16string(Web::DocView::kMaxUnits + 1, u'a')}});
        CHECK(big.snapshot()["kind"].asString() == "large" && !big.snapshot().has("lines"));
    }

    GROUP("sessionweb:palette");
    {
        // The page's colors are the editor's, resolved: the theme, then the
        // file's own.
        const Json d = Web::paletteJson();
        CHECK(d["theme"].asString() == "dark-modern" && d["dark"].asBool());
        CHECK(d["syntax"].size() == (std::size_t)kTokenStyleCount);
        CHECK(d["surfaces"].size() == (std::size_t)kSurfaceCount);
        CHECK(d["surfaces"]["terminal"]["background"].asString() == d["terminalBackground"].asString());
        CHECK(d["markdown"]["heading"].isString() && d["markdown"]["codeBackground"].isString());
        CHECK(d["editorFontSize"].asInt() == 13 && d["terminalFontSize"].asInt() == 12);
        CHECK(d["rulers"].size() == 1 && d["rulers"][0].asInt() == 100);
        CHECK(!d.has("tabsFontSize"));
        const Settings s = Settings::parse(
            "theme = light-plus\neditor.font-size = 15\nsyntax.keyword = #112233\n"
            "markdown.heading = #445566\neditor.background = #FEFEFE\nterminal.font-size = 11\n"
            "editor.line-number-color = #010203\ntabs.font-size = 14\neditor.ruler = off\n");
        const Json l = Web::paletteJson(s);
        CHECK(l["theme"].asString() == "light-plus" && !l["dark"].asBool());
        CHECK(l["syntax"][(int)TokenStyle::Keyword].asString() == "#112233");
        CHECK(l["markdown"]["heading"].asString() == "#445566");
        CHECK(l["background"].asString() == "#fefefe" &&
              l["surfaces"]["editor"]["background"].asString() == "#fefefe");
        CHECK(l["lineNumber"].asString() == "#010203");
        CHECK(l["editorFontSize"].asInt() == 15 && l["terminalFontSize"].asInt() == 11 &&
              l["tabsFontSize"].asInt() == 14);
        CHECK(l["rulers"].size() == 0);
        CHECK(l["syntax"][(int)TokenStyle::String].asString() !=
              d["syntax"][(int)TokenStyle::String].asString());   // the theme's own
        // The page applies it: no built-in palette of its own once told.
        CHECK(Web::pageHtml().find("case \"palette\": applyPalette") != std::string::npos);
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

    // A host in this process, as the Mac's (editor.session-host): the view
    // connects through Options::connect, sees one window's tabs, and lets
    // the host go when the last browser leaves.
    GROUP("sessionweb:in-process");
    {
        std::mutex routesMu;
        std::map<Shared::ClientId, Shared::Client*> routes;
        Shared::SessionHost host([&](Shared::ClientId to, Message&& m) {
            std::lock_guard<std::mutex> lk(routesMu);
            auto it = routes.find(to);
            if (it != routes.end()) it->second->deliver(std::move(m));
        });
        Shared::Client editor(Model::Unit::Utf16, "editor");
        const Shared::ClientId editorId = host.connect();
        {
            std::lock_guard<std::mutex> lk(routesMu);
            routes[editorId] = &editor;
        }
        editor.connect([&](Message&& m) { host.post(editorId, std::move(m)); });
        TestMirror mine;
        mine.client = &editor;
        mine.doc = 1;
        mine.text = u"int main() {\n    return 0;\n}\n";
        editor.open(1, "/w/main.cpp", Model::utf8FromUtf16(mine.text), &mine);
        editor.open(2, "/other/secret.txt", "another window's\n", nullptr);
        auto tabsOf = [](Shared::DocId d, const std::string& path) {
            Json t = Json::object();
            t.set("doc", Json((int64_t)d));
            t.set("path", Json(path));
            Json tabs = Json::array();
            tabs.push(std::move(t));
            Json g = Json::object();
            g.set("tabs", std::move(tabs));
            g.set("active", Json((int64_t)d));
            Json groups = Json::array();
            groups.push(std::move(g));
            Json w = Json::object();
            w.set("root", Json("/w"));
            w.set("focused", Json(0));
            w.set("groups", std::move(groups));
            return w;
        };
        editor.windowTabs(5, tabsOf(1, "/w/main.cpp"));
        editor.windowTabs(6, tabsOf(2, "/other/secret.txt"));
        CHECK(editor.settle());

        std::atomic<int> connects{0}, letGos{0};
        Web::Server web;
        Web::Options o;
        o.folder = "/w";
        o.window = 5;
        o.termNote = "Terminals appear here once they run through the session host";
        o.pollMs = 100;
        const std::string settingsFile = scratchFolder() + "/settings.conf";
        writeText(settingsFile, "theme = dark-plus\n");
        o.settingsPath = settingsFile;
        o.connect = [&](Shared::Client& c) -> std::function<void()> {
            connects++;
            const Shared::ClientId id = host.connect();
            {
                std::lock_guard<std::mutex> lk(routesMu);
                routes[id] = &c;
            }
            c.connect([&host, id](Message&& m) { host.post(id, std::move(m)); });
            return [&, id] {
                {
                    std::lock_guard<std::mutex> lk(routesMu);
                    routes.erase(id);
                }
                host.disconnect(id);
                letGos++;
            };
        };
        std::string error;
        CHECK(web.start(o, &error));
        CHECK(o.hostSocket.empty());
        std::thread webThread([&] { web.run(); });
        const int port = web.port();
        {
            PageClient p;
            std::string head;
            CHECK(!p.upgrade(port, "/ws?t=" + std::string(48, '0'), &head) &&
                  head.compare(0, 12, "HTTP/1.1 403") == 0);
            p.close();
            CHECK(connects == 0);
            CHECK(p.upgrade(port, "/ws?t=" + web.token()));
            Json hello, state;
            PageDoc doc;
            int gone = 0;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                else if (t == "gone") gone++;
                doc.take(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(connects == 1);
            CHECK(hello["termNote"].asString() == o.termNote && hello["folder"].asString() == "/w");
            // Only window 5's tab and its document.
            CHECK(state["windows"].size() == 1 && state["docs"].size() == 1 &&
                  state["docs"][0]["id"].asInt() == 1);
            CHECK(state["terms"].size() == 0);
            // Another window's document is never shown, even when asked for.
            p.sendText("{\"t\":\"show\",\"doc\":2,\"term\":0}");
            CHECK(p.pump(take, [&] { return gone == 1; }));
            CHECK(doc.id != 2);
            p.sendText("{\"t\":\"show\",\"doc\":1,\"term\":0}");
            CHECK(p.pump(take, [&] { return doc.id == 1; }));
            CHECK(doc.text() == Model::utf8FromUtf16(mine.text));
            mine.caret = mine.text.size();
            mine.type(u"// typed\n");
            CHECK(editor.settle());
            CHECK(p.pump(take, [&] { return doc.text() == Model::utf8FromUtf16(mine.text); }));
            // The settings file's colors, and a change to it recolors the page.
            CHECK(hello["palette"]["theme"].asString() == "dark-plus");
            Json palette;
            auto takePalette = [&](const Json& m) {
                if (m["t"].asString() == "palette") palette = m["palette"];
                take(m);
            };
            writeText(settingsFile, "theme = light-modern\neditor.background = #FAFAF0\n");
            CHECK(p.pump(takePalette, [&] { return palette.isObject(); }, 5000));
            CHECK(palette["theme"].asString() == "light-modern" && !palette["dark"].asBool() &&
                  palette["background"].asString() == "#fafaf0");
            // Asked to look now: the next change comes without the second's wait.
            palette = Json();
            writeText(settingsFile, "theme = monokai\n");
            web.reloadSettings();
            CHECK(p.pump(takePalette, [&] { return palette.isObject(); }, 900));
            CHECK(palette["theme"].asString() == "monokai");
            p.close();
            CHECK(waitUntil([&] { return web.browsers() == 0 && letGos == 1; }));
        }
        web.stop();
        webThread.join();
        {
            std::lock_guard<std::mutex> lk(routesMu);
            CHECK(routes.size() == 1);
            routes.clear();
        }
    }

    // A page that edits (stage 8b): its edits reach the host and the
    // window's renderer, crossing the renderer's own in flight and ending
    // on one text; Return takes the core's indentation; undo is the host's;
    // a save goes to the app; keys reach the host's terminal. A read-only
    // view refuses all of it, and the socket opens only from the page's own
    // origin.
    GROUP("sessionweb:edit");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "editor"));
        const std::string home = scratchFolder();
        std::atomic<int> saves{0};
        std::atomic<std::uint64_t> savedDoc{0};
        Web::Server web;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = rig.proj;
        o.pollMs = 100;
        o.editable = true;
        o.save = [&](Shared::DocId d) {
            savedDoc = d;
            saves++;
            return true;
        };
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        Web::Server ro;   // the same host, read-only
        Web::Options ro_o = o;
        ro_o.editable = false;
        ro_o.save = nullptr;
        CHECK(ro.start(ro_o, &error));
        std::thread roThread([&] { ro.run(); });
        const int port = web.port();
        if (a.client && port > 0) {
            const std::u16string text = u"def f(x):\n    return x\n";
            a.mirror.doc = 1;
            a.mirror.text = text;
            a.client->open(1, rig.proj + "/f.py", Model::utf8FromUtf16(text), &a.mirror);
            a.client->openTerminal(7, shellSpec(home, {"sh"}), &a);
            CHECK(a.wait([&] { return a.pid > 0 && a.out.find("$ ") != std::string::npos; }));

            // Only the page's own origin opens the socket.
            PageClient bad;
            std::string head;
            CHECK(!bad.upgrade(port, "/ws?t=" + web.token(), &head, "http://evil.example") &&
                  head.compare(0, 12, "HTTP/1.1 403") == 0);
            CHECK(!bad.upgrade(port, "/ws?t=" + web.token(), &head, "-") &&
                  head.compare(0, 12, "HTTP/1.1 403") == 0);
            CHECK(!bad.upgrade(port, "/ws?t=" + web.token(), &head,
                               "http://localhost:" + std::to_string(port)) &&
                  head.compare(0, 12, "HTTP/1.1 403") == 0);
            bad.close();

            PageClient p;
            CHECK(p.upgrade(port, "/ws?t=" + web.token()));
            PageEditor page;
            page.p = &p;
            page.id = 1;
            Json hello, state;
            PageTerm term;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                page.take(m);
                term.take(m);
            };
            auto hostText = [&] { return a.client->hostDocument(1).text; };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(hello["editable"].asBool() && hello["canSave"].asBool());
            p.sendText("{\"t\":\"show\",\"doc\":1,\"term\":7}");
            CHECK(p.pump(take, [&] { return page.docs == 1 && term.snapshots == 1; }));
            CHECK(page.text() == text);

            // Typed in the page: the host and the window's renderer have it.
            page.edit(Ot::Ops{Ot::Op{0, 0, u"# top\n"}});
            CHECK(p.pump(take, [&] { return page.synced(); }));
            CHECK(a.wait([&] { return a.mirror.text == page.text(); }));
            CHECK(hostText() == u"# top\ndef f(x):\n    return x\n");

            // Both type at once, many times, neither waiting for the other.
            std::mt19937 rng(88);
            for (int round = 0; round < 120; round++) {
                const std::u16string mine = page.text();
                Ot::Ops ops = randomOps(rng, mine, Model::Unit::Utf16, 2);
                page.edit(ops);
                if (round % 3 == 0) {
                    a.mirror.moveCaret(rng);
                    a.mirror.edit(rng, round % 2 ? "Typing" : "Edit");
                }
                if (round % 5 == 0) a.client->dispatch();
                p.pump(take, [] { return false; }, round % 7 ? 0 : 15);
            }
            CHECK(p.pump(take, [&] { return page.synced(); }, 15000));
            CHECK(a.client->settle());
            CHECK(p.pump(take, [&] { return page.synced() && page.text() == hostText(); }, 8000));
            CHECK(a.wait([&] { return a.mirror.text == hostText(); }));
            CHECK(page.text() == hostText() && a.mirror.text == hostText());
            CHECK(page.refusals == 0 && page.docs == 1);

            // Return after a line ending in ':': the page guessed no
            // indentation, the core gave four spaces, and the page's next
            // typing lands after them.
            page.edit(Ot::Ops{Ot::Op{0, page.text().size(), u"x = 1\n"}}, "", 0, 0, "Paste", false);
            CHECK(p.pump(take, [&] { return page.synced(); }));
            const std::size_t end = page.text().size();
            page.edit(Ot::Ops{Ot::Op{end, 0, u"def g():"}});
            const std::size_t at = end + 8;
            page.edit(Ot::Ops{Ot::Op{at, 0, u"\n"}}, "newline", at, at);
            page.edit(Ot::Ops{Ot::Op{at + 1, 0, u"pass"}});
            CHECK(p.pump(take, [&] { return page.synced(); }));
            CHECK(p.pump(take, [&] { return page.text() == hostText(); }));
            const std::u16string tail = hostText().substr(end);
            CHECK(tail == u"def g():\n    pass");
            CHECK(page.text() == hostText());

            // Undo is the host's: the last step goes, in the page and the
            // window alike.
            p.sendText("{\"t\":\"undo\",\"doc\":1,\"redo\":false}");
            CHECK(p.pump(take, [&] { return hostText().substr(end) != u"def g():\n    pass"; }));
            CHECK(p.pump(take, [&] { return page.text() == hostText(); }));
            CHECK(a.wait([&] { return a.mirror.text == hostText(); }));
            CHECK(hostText().size() < end + 17);
            p.sendText("{\"t\":\"undo\",\"doc\":1,\"redo\":true}");
            CHECK(p.pump(take, [&] { return hostText().substr(end) == u"def g():\n    pass"; }));
            CHECK(p.pump(take, [&] { return page.text() == hostText(); }));

            // A save goes to the app that owns the disk.
            p.sendText("{\"t\":\"save\",\"doc\":1}");
            CHECK(waitUntil([&] { return saves == 1; }) && savedDoc == 1);

            // Keys for the host's terminal, encoded on the server.
            p.sendText("{\"t\":\"ttext\",\"term\":7,\"text\":\"echo key-$((6*7))\",\"mods\":0}");
            p.sendText("{\"t\":\"tkey\",\"term\":7,\"key\":\"Enter\",\"mods\":0}");
            CHECK(a.wait([&] { return a.out.find("key-42\r\n") != std::string::npos; }));
            p.sendText("{\"t\":\"ttext\",\"term\":7,\"text\":\"c\",\"mods\":4}");   // Control C
            CHECK(p.pump(take, [&] { return term.all().find("key-42") != std::string::npos; }));

            // A read-only view of the same host refuses all of it.
            PageClient q;
            CHECK(q.upgrade(ro.port(), "/ws?t=" + ro.token()));
            PageEditor rpage;
            rpage.p = &q;
            rpage.id = 1;
            Json rstate;
            auto rtake = [&](const Json& m) {
                if (m["t"].asString() == "state") rstate = m;
                rpage.take(m);
            };
            CHECK(q.pump(rtake, [&] { return rstate.isObject(); }));
            CHECK(!rpage.editable);
            q.sendText("{\"t\":\"show\",\"doc\":1,\"term\":7}");
            CHECK(q.pump(rtake, [&] { return rpage.docs == 1; }));
            const std::u16string before = hostText();
            rpage.edit(Ot::Ops{Ot::Op{0, 0, u"NO"}});
            CHECK(q.pump(rtake, [&] { return rpage.refusals == 1; }));
            q.sendText("{\"t\":\"undo\",\"doc\":1,\"redo\":false}");
            q.sendText("{\"t\":\"save\",\"doc\":1}");
            q.sendText("{\"t\":\"ttext\",\"term\":7,\"text\":\"echo pwned\\n\",\"mods\":0}");
            CHECK(q.pump(rtake, [&] { return rpage.notes.size() >= 3; }));
            a.client->settle();
            CHECK(hostText() == before && saves == 1);
            CHECK(a.out.find("pwned") == std::string::npos);
            q.close();
            p.close();
            CHECK(waitUntil([&] { return web.browsers() == 0 && ro.browsers() == 0; }));
            a.client->closeTerminal(7);
        }
        web.stop();
        webThread.join();
        ro.stop();
        roThread.join();
    }

    // One window's view sees one window's terminals (R5): the Mac's window
    // reports its own with its tabs ("terms"), and a page of that window's
    // view lists, attaches to and types into those alone; a crafted show,
    // keys, text or resize for another window's terminal id are refused.
    // A headless host's view (no window) sees every terminal.
    GROUP("sessionweb:window-terminals");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "editor"));
        const std::string home = scratchFolder();
        auto windowOf = [&](std::vector<Shared::TermId> terms) {
            Json w = Json::object();
            w.set("root", Json(rig.proj));
            w.set("focused", Json(0));
            w.set("groups", Json::array());
            Json ids = Json::array();
            for (Shared::TermId t : terms) ids.push(Json((int64_t)t));
            w.set("terms", std::move(ids));
            return w;
        };
        auto renderersOf = [&](Shared::TermId t) {
            const Json st = a.client->sessionState();   // kept: the loop reads into it
            for (const Json& j : st["terminals"].elements())
                if ((Shared::TermId)j["term"].asInt() == t) return (int)j["renderers"].asInt();
            return -1;
        };
        auto hostCols = [&](Shared::TermId t) {
            int cols = 0;
            rig.server->host().inspect([&](const Shared::Host& h) {
                if (const TerminalScreen* s = h.screen(t)) cols = s->cols();
            });
            return cols;
        };
        auto idsOf = [](const Json& state) {
            std::string s;
            for (const Json& t : state["terms"].elements()) s += std::to_string(t["id"].asInt()) + " ";
            return s;
        };
        Web::Server web, all;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = rig.proj;
        o.pollMs = 100;
        o.editable = true;
        o.window = 5;
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        Web::Options allo = o;   // a headless host's view: no window
        allo.window = 0;
        CHECK(all.start(allo, &error));
        std::thread allThread([&] { all.run(); });
        if (a.client && web.port() > 0 && all.port() > 0) {
            a.client->openTerminal(11, shellSpec(home, {"sh"}), &a);   // window 5's
            a.client->openTerminal(12, shellSpec(home, {"sh"}), &a);   // window 6's
            auto prompt = [&](Shared::TermId t) {
                for (const std::string& r : hostRows(rig, t))
                    if (r.find("$") != std::string::npos) return true;
                return false;
            };
            CHECK(a.wait([&] { return prompt(11) && prompt(12); }));
            a.client->windowTabs(5, windowOf({11}));
            a.client->windowTabs(6, windowOf({12}));
            CHECK(a.client->settle());

            PageClient p;
            CHECK(p.upgrade(web.port(), "/ws?t=" + web.token()));
            Json state;
            PageTerm term;
            std::vector<Shared::TermId> snapshots;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "state") state = m;
                if (t == "term") snapshots.push_back((Shared::TermId)m["id"].asInt());
                term.take(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            if (idsOf(state) != "11 ") std::printf("  window-terminals: %s\n", idsOf(state).c_str());
            CHECK(idsOf(state) == "11 ");

            // Window 6's terminal, asked for by id: never shown, never
            // attached to, and nothing typed or sized reaches it.
            p.sendText("{\"t\":\"show\",\"doc\":0,\"term\":12}");
            p.sendText("{\"t\":\"ttext\",\"term\":12,\"text\":\"echo pwned-$((1+1))\",\"mods\":0}");
            p.sendText("{\"t\":\"tkey\",\"term\":12,\"key\":\"Enter\",\"mods\":0}");
            p.sendText("{\"t\":\"tpaste\",\"term\":12,\"text\":\"echo pasted-$((2+2))\\n\"}");
            p.sendText("{\"t\":\"resize\",\"term\":12,\"cols\":40,\"rows\":10}");
            p.sendText("{\"t\":\"tresize\",\"term\":12,\"cols\":40,\"rows\":10}");
            p.pump(take, [] { return false; }, 500);
            a.client->settle();
            CHECK(snapshots.empty());
            CHECK(renderersOf(12) == 1);   // the editor alone
            CHECK(hostCols(12) == 80);
            CHECK(a.out.find("pwned-2") == std::string::npos && a.out.find("pasted-4") == std::string::npos);

            // Its own terminal: shown whole, and typed into.
            p.sendText("{\"t\":\"show\",\"doc\":0,\"term\":11}");
            CHECK(p.pump(take, [&] { return term.snapshots == 1 && term.id == 11; }));
            CHECK(renderersOf(11) == 2);
            p.sendText("{\"t\":\"ttext\",\"term\":11,\"text\":\"echo own-$((6*7))\",\"mods\":0}");
            p.sendText("{\"t\":\"tkey\",\"term\":11,\"key\":\"Enter\",\"mods\":0}");
            CHECK(p.pump(take, [&] { return term.all().find("own-42") != std::string::npos; }));
            // Keys for the other window's terminal while its own is on show.
            p.sendText("{\"t\":\"ttext\",\"term\":12,\"text\":\"echo pwned-$((1+1))\\n\",\"mods\":0}");
            p.pump(take, [] { return false; }, 300);
            a.client->settle();
            CHECK(a.out.find("pwned-2") == std::string::npos);

            // The headless view sees both.
            PageClient q;
            CHECK(q.upgrade(all.port(), "/ws?t=" + all.token()));
            Json allState;
            CHECK(q.pump([&](const Json& m) { if (m["t"].asString() == "state") allState = m; },
                         [&] { return allState.isObject(); }));
            CHECK(idsOf(allState) == "11 12 ");
            q.close();

            // The window opens a terminal: the page lists it once reported;
            // and it closes one: the page drops it.
            a.client->openTerminal(13, shellSpec(home, {"sh"}), &a);
            CHECK(a.wait([&] { return prompt(13); }));
            p.pump(take, [] { return false; }, 300);
            CHECK(idsOf(state) == "11 ");   // not reported yet: not this window's
            const double t0 = nowMs();
            a.client->windowTabs(5, windowOf({11, 13}));
            CHECK(p.pump(take, [&] { return idsOf(state) == "11 13 "; }, 3000));
            CHECK(nowMs() - t0 < 1500);
            a.client->closeTerminal(13);
            CHECK(p.pump(take, [&] { return idsOf(state) == "11 "; }, 3000));

            // A terminal the window stops reporting is let go: what was on
            // show is no longer fed, and typing into it is refused.
            a.client->windowTabs(5, windowOf({}));
            CHECK(p.pump(take, [&] { return state["terms"].size() == 0; }, 3000));
            const std::string seen = term.all();
            p.sendText("{\"t\":\"ttext\",\"term\":11,\"text\":\"echo late-$((3+3))\\n\",\"mods\":0}");
            p.pump(take, [] { return false; }, 300);
            a.client->settle();
            CHECK(a.out.find("late-6") == std::string::npos);
            a.client->terminalInput(11, "echo after-$((4+4))\n");
            CHECK(a.wait([&] { return a.out.find("after-8") != std::string::npos; }));
            p.pump(take, [] { return false; }, 300);
            CHECK(term.all() == seen);
            p.close();
            CHECK(waitUntil([&] { return web.browsers() == 0; }));
            a.client->closeTerminal(11);
            a.client->closeTerminal(12);
        }
        web.stop();
        webThread.join();
        all.stop();
        allThread.join();
    }

    // The page's tree (stage 7's first read): the host lists one folder a
    // time over the socket, confined to its folder; a click on a file asks
    // the app to open it (Options::open), read-only pages included; nothing
    // of it is reachable without the token or outside the socket.
    GROUP("sessionweb:tree");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        const std::string root = rig.proj, elsewhere = scratchFolder();
        writeText(root + "/.gitignore", "*.log\n");
        writeText(root + "/main.py", "print(1)\n");
        writeText(root + "/run.log", "x\n");
        ::mkdir((root + "/src").c_str(), 0755);
        writeText(root + "/src/util.c", "int u;\n");
        writeText(elsewhere + "/secret.txt", "not yours\n");
        CHECK(::symlink(elsewhere.c_str(), (root + "/away").c_str()) == 0);
        std::mutex openedMu;
        std::vector<std::pair<std::string, bool>> opened;
        Web::Server web;   // read-only, as the command line's default
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = root;
        o.pollMs = 100;
        o.open = [&](const std::string& path, bool keep) {
            std::lock_guard<std::mutex> lk(openedMu);
            opened.push_back({path, keep});
            return true;
        };
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        Web::Server headless;   // no window to open files in
        Web::Options h = o;
        h.open = nullptr;
        h.port = 0;
        CHECK(headless.start(h, &error));
        std::thread headlessThread([&] { headless.run(); });
        const int port = web.port();
        {
            // No way in but the socket with the token: no listing over
            // plain HTTP, and no socket without the token.
            PageClient p;
            std::string r = p.exchange(port, "GET /list?t=" + web.token() + "&path= HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            CHECK(r.compare(0, 12, "HTTP/1.1 404") == 0);
            r = p.exchange(port, "GET /list?path= HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            CHECK(r.compare(0, 12, "HTTP/1.1 403") == 0);
            std::string head;
            CHECK(!p.upgrade(port, "/ws", &head) && head.compare(0, 12, "HTTP/1.1 403") == 0);
            p.close();
            CHECK(!p.upgrade(port, "/ws?t=" + web.token(), &head, "http://evil.example") &&
                  head.compare(0, 12, "HTTP/1.1 403") == 0);
            p.close();

            CHECK(p.upgrade(port, "/ws?t=" + web.token()));
            Json state;
            std::map<std::string, Json> trees;
            std::vector<Json> opens;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "state") state = m;
                else if (t == "tree") trees[m["path"].asString()] = m;
                else if (t == "opening") opens.push_back(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            // The folder, as the apps' trees list it.
            p.sendText("{\"t\":\"list\",\"path\":\"\"}");
            CHECK(p.pump(take, [&] { return trees.count(""); }));
            const Json& top = trees[""];
            CHECK(top["root"].asString() == root);
            std::string names;
            for (const Json& e : top["entries"].elements())
                names += e["name"].asString() + (e["dir"].asBool() ? "/" : "") +
                         (e["ignored"].asBool() ? "(ignored)" : "") +
                         (e["outside"].asBool() ? "(outside)" : "") + " ";
            CHECK(names == "away/(outside) src/ main.py run.log(ignored) ");
            if (names != "away/(outside) src/ main.py run.log(ignored) ") std::printf("  sessionweb:tree: %s\n", names.c_str());
            p.sendText("{\"t\":\"list\",\"path\":\"src\"}");
            CHECK(p.pump(take, [&] { return trees.count("src"); }));
            CHECK(trees["src"]["entries"].size() == 1 && trees["src"]["entries"][0]["name"].asString() == "util.c");
            // Out of the folder: refused, nothing listed.
            for (const char* bad : {"..", "/etc", "away", "src/../.."}) {
                p.sendText(std::string("{\"t\":\"list\",\"path\":\"") + bad + "\"}");
                CHECK(p.pump(take, [&] { return trees.count(bad); }));
                CHECK(trees[bad].has("error") && !trees[bad].has("entries"));
            }
            // A click on a file: the app opens it, even from a page that
            // only reads; a folder, or a path out of the folder, is not.
            p.sendText("{\"t\":\"open\",\"path\":\"src/util.c\"}");
            p.sendText("{\"t\":\"open\",\"path\":\"main.py\",\"keep\":true}");
            p.sendText("{\"t\":\"open\",\"path\":\"src\"}");
            p.sendText("{\"t\":\"open\",\"path\":\"../" + elsewhere.substr(5) + "/secret.txt\"}");
            p.sendText("{\"t\":\"open\",\"path\":\"away/secret.txt\"}");
            CHECK(p.pump(take, [&] { return opens.size() == 5; }));
            if (opens.size() == 5) {
                CHECK(opens[0]["ok"].asBool() && opens[0]["abs"].asString() == root + "/src/util.c");
                CHECK(opens[1]["ok"].asBool() && opens[1]["abs"].asString() == root + "/main.py");
                for (int i = 2; i < 5; i++) CHECK(!opens[i]["ok"].asBool() && !opens[i]["why"].asString().empty());
            }
            {
                std::lock_guard<std::mutex> lk(openedMu);
                CHECK(opened.size() == 2);
                if (opened.size() == 2) {
                    CHECK(opened[0].first == root + "/src/util.c" && !opened[0].second);
                    CHECK(opened[1].first == root + "/main.py" && opened[1].second);
                }
            }
            p.close();

            // A host with no window to open files in says so.
            PageClient q;
            CHECK(q.upgrade(headless.port(), "/ws?t=" + headless.token()));
            Json hstate;
            std::vector<Json> hopens;
            bool listed = false;
            auto htake = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "state") hstate = m;
                else if (t == "tree") listed = m["entries"].size() == 4;
                else if (t == "opening") hopens.push_back(m);
            };
            CHECK(q.pump(htake, [&] { return hstate.isObject(); }));
            q.sendText("{\"t\":\"list\",\"path\":\"\"}");
            q.sendText("{\"t\":\"open\",\"path\":\"main.py\"}");
            CHECK(q.pump(htake, [&] { return listed && hopens.size() == 1; }));
            CHECK(hopens.size() == 1 && !hopens[0]["ok"].asBool() &&
                  hopens[0]["why"].asString().find("headless") != std::string::npos);
            q.close();
            CHECK(waitUntil([&] { return web.browsers() == 0 && headless.browsers() == 0; }));
        }
        web.stop();
        webThread.join();
        headless.stop();
        headlessThread.join();
        // The page carries the tree and its sidebar.
        const std::string& page = Web::pageHtml();
        CHECK(page.find("id=\"tree\"") != std::string::npos && page.find("id=\"sidebtn\"") != std::string::npos);
        CHECK(Web::paletteJson().has("treeIgnored"));
    }

    // The window's browser pane (R4): its tabs as the window reports them,
    // checked again for the page, only that window's; an editable page may
    // ask the pane to open an http or https address and nothing else, a
    // read-only one not at all.
    GROUP("sessionweb:browser");
    {
        // Only http and https, with a host, nothing hidden in the address.
        for (const char* ok : {"http://example.com", "https://example.com/a?b=c#d", "HTTPS://Example.com:8443/",
                               "http://127.0.0.1:3000/", "http://[::1]:8080/x", "http://localhost"}) {
            std::string why;
            CHECK(Web::webAddress(ok, &why));
            if (!why.empty()) std::printf("  sessionweb:browser: %s: %s\n", ok, why.c_str());
        }
        for (const char* bad : {"javascript:alert(1)", "JavaScript:alert(1)", " javascript:alert(1)",
                                "javascript://example.com/%0aalert(1)", "file:///etc/passwd", "data:text/html,<p>",
                                "about:blank", "ftp://example.com/", "blob:http://example.com/x", "vbscript:x",
                                "view-source:http://example.com", "http:/example.com", "http://", "https:///x",
                                "http://user:pw@example.com/", "http://example.com@evil.example/",
                                "http://exa mple.com", "http://example.com/\n", "http://example.com/\t",
                                "example.com", "", "//example.com", "http://[::1/"}) {
            std::string why;
            CHECK(!Web::webAddress(bad, &why) && !why.empty());
        }
        CHECK(!Web::webAddress("https://example.com/" + std::string(Web::kMaxAddress, 'a')));
        CHECK(Web::loopbackAddress("http://localhost:3000/") && Web::loopbackAddress("http://127.0.0.1/"));
        CHECK(Web::loopbackAddress("http://127.1.2.3:80") && Web::loopbackAddress("https://[::1]/"));
        CHECK(Web::loopbackAddress("http://app.localhost/") && Web::loopbackAddress("http://0.0.0.0:8000"));
        CHECK(Web::loopbackAddress("HTTP://LOCALHOST"));
        CHECK(!Web::loopbackAddress("http://example.com/") && !Web::loopbackAddress("http://127.0.0.1.evil.example/"));
        CHECK(!Web::loopbackAddress("http://localhost.evil.example/") && !Web::loopbackAddress("file://localhost/x"));
        CHECK(!Web::loopbackAddress("http://128.0.0.1/") && !Web::loopbackAddress("http://127.0.0/"));

        // The report: empty and overlong addresses left out (the tab on show
        // counted among the rest), titles cut at a character, a cap.
        std::vector<Web::BrowserTab> tabs = {{"", "new tab"},
                                             {"https://example.com/", "Example"},
                                             {"https://" + std::string(Web::kMaxAddress, 'a'), "long"},
                                             {"http://localhost:3000/", std::string(299, 'x') + "\xc3\xa9"}};
        Json report = Web::browserReportJson(tabs, 3);
        CHECK(report["tabs"].size() == 2 && report["active"].asInt() == 1);
        CHECK(report["tabs"][0]["url"].asString() == "https://example.com/" &&
              report["tabs"][0]["title"].asString() == "Example");
        CHECK(report["tabs"][1]["title"].asString() == std::string(299, 'x'));
        CHECK(Web::browserReportJson(tabs, 0)["active"].asInt() == -1);
        CHECK(Web::browserReportJson({}, 0)["tabs"].size() == 0);
        std::vector<Web::BrowserTab> many(Web::kMaxBrowserTabs + 20, Web::BrowserTab{"https://example.com/", "e"});
        CHECK(Web::browserReportJson(many, 0)["tabs"].size() == Web::kMaxBrowserTabs);
        // Through the protocol and back unchanged.
        Message wt;
        wt.kind = Shared::Kind::WindowTabs;
        wt.window = 9;
        wt.json = Json::object();
        wt.json.set("browser", report);
        CHECK(Shared::roundTrip(wt).json["browser"] == report);

        // What the page gets: marked, and checked again (a renderer's report).
        Json view = Web::browserViewJson(report);
        CHECK(view["tabs"].size() == 2 && view["active"].asInt() == 1);
        CHECK(view["tabs"][0]["web"].asBool() && !view["tabs"][0]["loopback"].asBool());
        CHECK(view["tabs"][1]["web"].asBool() && view["tabs"][1]["loopback"].asBool());
        Json odd;
        CHECK(Json::parse("{\"tabs\":[{\"url\":\"javascript:alert(1)\",\"title\":\"x\"},7,{\"title\":\"no url\"},"
                          "{\"url\":\"file:///etc/hosts\"}],\"active\":3}",
                          odd));
        view = Web::browserViewJson(odd);
        CHECK(view["tabs"].size() == 2 && view["active"].asInt() == 1);
        CHECK(!view["tabs"][0]["web"].asBool() && !view["tabs"][1]["web"].asBool());
        CHECK(Web::browserViewJson(Json()).isNull() && Web::browserViewJson(Json("x")).isNull());

        // Over the wire, in a host in this process with two windows.
        std::mutex routesMu;
        std::map<Shared::ClientId, Shared::Client*> routes;
        Shared::SessionHost host([&](Shared::ClientId to, Message&& m) {
            std::lock_guard<std::mutex> lk(routesMu);
            auto it = routes.find(to);
            if (it != routes.end()) it->second->deliver(std::move(m));
        });
        Shared::Client editor(Model::Unit::Utf16, "editor");
        const Shared::ClientId editorId = host.connect();
        {
            std::lock_guard<std::mutex> lk(routesMu);
            routes[editorId] = &editor;
        }
        editor.connect([&](Message&& m) { host.post(editorId, std::move(m)); });
        auto windowWith = [](const std::string& url, const std::string& title) {
            Json w = Json::object();
            w.set("root", Json("/w"));
            w.set("focused", Json(0));
            w.set("groups", Json::array());
            w.set("browser", Web::browserReportJson({{url, title}}, 0));
            return w;
        };
        editor.windowTabs(5, windowWith("http://localhost:5173/", "Dev server"));
        editor.windowTabs(6, windowWith("https://other.example/", "Another window's page"));
        CHECK(editor.settle());
        auto connectTo = [&](Shared::Client& c) -> std::function<void()> {
            const Shared::ClientId id = host.connect();
            {
                std::lock_guard<std::mutex> lk(routesMu);
                routes[id] = &c;
            }
            c.connect([&host, id](Message&& m) { host.post(id, std::move(m)); });
            return [&, id] {
                {
                    std::lock_guard<std::mutex> lk(routesMu);
                    routes.erase(id);
                }
                host.disconnect(id);
            };
        };
        std::mutex browsedMu;
        std::vector<std::string> browsed;
        auto serve = [&](Web::Server& web, bool editable, bool withPane) {
            Web::Options o;
            o.folder = "/w";
            o.window = 5;
            o.pollMs = 100;
            o.editable = editable;
            o.connect = connectTo;
            if (withPane)
                o.browse = [&](const std::string& url) {
                    std::lock_guard<std::mutex> lk(browsedMu);
                    browsed.push_back(url);
                    return true;
                };
            std::string error;
            CHECK(web.start(o, &error));
        };
        Web::Server editing, reading, paneless;
        serve(editing, true, true);
        serve(reading, false, true);
        serve(paneless, true, false);
        std::thread t1([&] { editing.run(); }), t2([&] { reading.run(); }), t3([&] { paneless.run(); });
        {
            PageClient p;
            CHECK(p.upgrade(editing.port(), "/ws?t=" + editing.token()));
            Json hello, state;
            std::vector<Json> answers, notes;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                else if (t == "browsing") answers.push_back(m);
                else if (t == "note") notes.push_back(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(hello["canBrowse"].asBool());
            // Only window 5's browser tab, marked for the page.
            CHECK(state["windows"].size() == 1);
            const Json& b = state["windows"][0]["browser"];
            CHECK(b["tabs"].size() == 1 && b["active"].asInt() == 0);
            CHECK(b["tabs"][0]["url"].asString() == "http://localhost:5173/" &&
                  b["tabs"][0]["title"].asString() == "Dev server");
            CHECK(b["tabs"][0]["web"].asBool() && b["tabs"][0]["loopback"].asBool());
            CHECK(state.dump().find("other.example") == std::string::npos);
            // A change to the window's pane reaches the page.
            editor.windowTabs(5, windowWith("https://docs.example/guide", "Guide"));
            CHECK(editor.settle());
            CHECK(p.pump(take, [&] {
                return state["windows"][0]["browser"]["tabs"][0]["url"].asString() == "https://docs.example/guide";
            }));
            // Opening addresses: http and https reach the window; the rest
            // are refused before it.
            p.sendText("{\"t\":\"browse\",\"url\":\"https://example.com/docs\"}");
            p.sendText("{\"t\":\"browse\",\"url\":\"javascript:alert(document.cookie)\"}");
            p.sendText("{\"t\":\"browse\",\"url\":\"file:///etc/passwd\"}");
            p.sendText("{\"t\":\"browse\",\"url\":\"data:text/html,<script>alert(1)</script>\"}");
            p.sendText("{\"t\":\"browse\",\"url\":7}");
            p.sendText("{\"t\":\"browse\",\"url\":\"http://localhost:3000/\"}");
            CHECK(p.pump(take, [&] { return answers.size() == 6; }));
            if (answers.size() == 6) {
                CHECK(answers[0]["ok"].asBool() && answers[0]["url"].asString() == "https://example.com/docs");
                for (int i = 1; i < 5; i++)
                    CHECK(!answers[i]["ok"].asBool() && answers[i]["why"].asString().find("Not opened") == 0);
                CHECK(answers[5]["ok"].asBool());
            }
            {
                std::lock_guard<std::mutex> lk(browsedMu);
                CHECK(browsed.size() == 2);
                if (browsed.size() == 2)
                    CHECK(browsed[0] == "https://example.com/docs" && browsed[1] == "http://localhost:3000/");
            }
            p.close();

            // A read-only page lists the tabs but cannot open an address.
            PageClient r;
            CHECK(r.upgrade(reading.port(), "/ws?t=" + reading.token()));
            hello = Json();
            state = Json();
            notes.clear();
            answers.clear();
            CHECK(r.pump(take, [&] { return state.isObject(); }));
            CHECK(!hello["canBrowse"].asBool());
            CHECK(state["windows"][0]["browser"]["tabs"].size() == 1);
            r.sendText("{\"t\":\"browse\",\"url\":\"https://example.com/\"}");
            CHECK(r.pump(take, [&] { return notes.size() == 1; }));
            CHECK(answers.empty() && notes[0]["text"].asString().find("only reads") != std::string::npos);
            r.close();

            // Without a browser pane to open in (unset browse), it says so.
            PageClient q;
            CHECK(q.upgrade(paneless.port(), "/ws?t=" + paneless.token()));
            hello = Json();
            state = Json();
            answers.clear();
            CHECK(q.pump(take, [&] { return state.isObject(); }));
            CHECK(!hello["canBrowse"].asBool());
            q.sendText("{\"t\":\"browse\",\"url\":\"https://example.com/\"}");
            CHECK(q.pump(take, [&] { return answers.size() == 1; }));
            CHECK(answers.size() == 1 && !answers[0]["ok"].asBool() &&
                  answers[0]["why"].asString().find("no browser pane") != std::string::npos);
            q.close();
            {
                std::lock_guard<std::mutex> lk(browsedMu);
                CHECK(browsed.size() == 2);
            }
            CHECK(waitUntil([&] { return editing.browsers() == 0 && reading.browsers() == 0 && paneless.browsers() == 0; }));
        }
        editing.stop();
        reading.stop();
        paneless.stop();
        t1.join();
        t2.join();
        t3.join();
        {
            std::lock_guard<std::mutex> lk(routesMu);
            routes.clear();
        }
        // A window that reports no pane (a headless host has none) gives the
        // page no browser group; the page's frame is limited by its policy.
        const std::string token = "0123456789abcdef0123456789abcdef0123456789abcdef";
        const Web::Answer a = Web::answer(request(getWith("/?t=" + token)), token, "<p></p>");
        CHECK(a.response.find("frame-src 'self' http: https:;") != std::string::npos);
        CHECK(a.response.find("connect-src 'self' ws://127.0.0.1:* ws://localhost:*;") != std::string::npos);
        CHECK(a.response.find("default-src 'none'") != std::string::npos);
        CHECK(Web::pageHtml().find("id=\"web\"") != std::string::npos &&
              Web::pageHtml().find("Open in a new tab") != std::string::npos);
    }
}
