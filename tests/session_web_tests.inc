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
    int ptyCols = 0, ptyRows = 0;   // as the last whole terminal said
    void take(const Json& m) {
        const std::string t = m["t"].asString();
        if (t == "term") {
            id = (Shared::TermId)m["id"].asInt();
            ptyCols = (int)m["cols"].asInt();
            ptyRows = (int)m["rows"].asInt();
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
        // reader's, and it may edit, undo, type and say its terminal
        // pane's size, nothing more.
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
                               Shared::Kind::TermOpen, Shared::Kind::TermClose}) {
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
        CHECK(kinds(g, Shared::Kind::Error) == errors + 8);
        // Its view's size for a terminal is let through: the pty takes it
        // only while the guest is the one that typed there last
        // (sessionweb:term-size, term:size).
        Message size;
        size.kind = Shared::Kind::TermResize;
        size.term = 1;
        size.cols = 30;
        size.rows = 10;
        say(g, size);
        CHECK(kinds(g, Shared::Kind::Error) == errors + 8);
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

    GROUP("sessionweb:big-ids");
    {
        // A renderer's ids reach 2^53 - 1 (SessionProtocol.h, kIdRange):
        // what the page is sent writes them as exact decimal integers, which
        // JSON.parse gives back as the same number.
        const Shared::DocId far = Shared::kMaxId, even = 2000000000000000;
        Web::DocView v(far, "/p/a.txt");
        v.apply(Ot::Ops{Ot::Op{0, 0, u"hello\n"}});
        CHECK(v.snapshot().dump().find("\"id\":9007199254740991,") != std::string::npos);
        v.takeEdits();
        v.apply(Ot::Ops{Ot::Op{0, 0, u"x"}});
        const Json e = v.takeEdits();
        CHECK(!e.isNull() && e.dump().find("\"id\":9007199254740991,") != std::string::npos);
        Web::DocView r(even, "/p/b.txt");
        CHECK(r.snapshot().dump().find("\"id\":2000000000000000,") != std::string::npos);
        Web::TermView t(far, 20, 4);
        t.feed("hi");
        CHECK(t.snapshot().dump().find("\"id\":9007199254740991") != std::string::npos);
        // And what the page sends back reads as the same id.
        Json back;
        CHECK(Json::parse("{\"doc\":9007199254740991}", back));
        CHECK((Shared::DocId)back["doc"].asInt() == far);
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

    // A terminal's size follows the page that types (tmux's `window-size
    // latest`, Host::sizer): the server is one renderer for every page, so
    // it tells the host the size of the page that typed last. A narrow page
    // that only looks shrinks nothing; the page that types takes the pty to
    // its size, the editor's renderer typing takes it back, and an answer
    // to a query (TermInput flag 1) is not typing. The pages follow the
    // pty's size from TermShell, with the state looked at only once.
    GROUP("sessionweb:term-size");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "editor"));
        const std::string home = scratchFolder();
        auto ptySize = [&] {
            std::pair<int, int> z{0, 0};
            rig.server->host().inspect([&](const Shared::Host& h) {
                if (const TerminalScreen* sc = h.screen(7)) z = {sc->cols(), sc->rows()};
            });
            return z;
        };
        auto sized = [&](int cols, int rows) {
            const bool ok = waitUntil([&] { return ptySize() == std::make_pair(cols, rows); });
            if (!ok) std::printf("  term-size: the pty is %dx%d, not %dx%d\n", ptySize().first,
                                 ptySize().second, cols, rows);
            return ok;
        };
        if (a.client) {
            Shared::TermSpec spec = shellSpec(home, {"sh"});
            spec.cols = 80;
            spec.rows = 24;
            a.client->openTerminal(7, spec, &a);
            CHECK(a.wait([&] { return a.pid > 0 && a.out.find("$ ") != std::string::npos; }));
        }
        Web::Server web;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = rig.proj;
        o.pollMs = 600000;   // the state once: sizes come by TermShell alone
        o.editable = true;
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        if (a.client && web.port() > 0) {
            // Two pages on the one view: a phone's and a desktop's.
            PageClient p1, p2;
            PageTerm t1, t2;
            Json s1, s2;
            auto take1 = [&](const Json& m) {
                if (m["t"].asString() == "state") s1 = m;
                t1.take(m);
            };
            auto take2 = [&](const Json& m) {
                if (m["t"].asString() == "state") s2 = m;
                t2.take(m);
            };
            CHECK(p1.upgrade(web.port(), "/ws?t=" + web.token()));
            CHECK(p2.upgrade(web.port(), "/ws?t=" + web.token()));
            CHECK(p1.pump(take1, [&] { return s1.isObject(); }));
            CHECK(p2.pump(take2, [&] { return s2.isObject(); }));
            p1.sendText("{\"t\":\"show\",\"doc\":0,\"term\":7}");
            p2.sendText("{\"t\":\"show\",\"doc\":0,\"term\":7}");
            CHECK(p1.pump(take1, [&] { return t1.snapshots >= 1; }));
            CHECK(p2.pump(take2, [&] { return t2.snapshots >= 1; }));
            CHECK(t1.ptyCols == 80 && t1.ptyRows == 24);
            p1.sendText("{\"t\":\"tsize\",\"term\":7,\"cols\":30,\"rows\":12}");
            p2.sendText("{\"t\":\"tsize\",\"term\":7,\"cols\":100,\"rows\":30}");
            // Negative: nobody typed in a page, so the opener's size holds
            // (time is the condition: the sizes had this long to arrive).
            p1.pump(take1, [] { return false; }, 300);
            CHECK(ptySize() == std::make_pair(80, 24));

            // The phone's page types: the pty takes its size before the
            // keys, and both pages get the terminal whole at that size.
            p1.sendText("{\"t\":\"ttext\",\"term\":7,\"text\":\"stty size\",\"mods\":0}");
            p1.sendText("{\"t\":\"tkey\",\"term\":7,\"key\":\"Enter\",\"mods\":0}");
            CHECK(sized(30, 12));
            CHECK(a.wait([&] { return a.out.find("12 30\r\n") != std::string::npos; }));
            CHECK(p1.pump(take1, [&] { return t1.ptyCols == 30 && t1.ptyRows == 12; }));
            CHECK(p2.pump(take2, [&] { return t2.ptyCols == 30 && t2.ptyRows == 12; }));
            CHECK(p1.pump(take1, [&] { return t1.all().find("12 30") != std::string::npos; }));
            CHECK(t1.screen.size() == 12);

            // The desktop's page resizing while the phone's typed last:
            // kept, not taken (time is the condition again).
            p2.sendText("{\"t\":\"tsize\",\"term\":7,\"cols\":101,\"rows\":31}");
            p2.pump(take2, [] { return false; }, 300);
            CHECK(ptySize() == std::make_pair(30, 12));
            // The phone's page resizing while it is the typist: taken.
            p1.sendText("{\"t\":\"tsize\",\"term\":7,\"cols\":32,\"rows\":12}");
            CHECK(sized(32, 12));

            // The desktop's page types: its size.
            p2.sendText("{\"t\":\"ttext\",\"term\":7,\"text\":\"stty size\",\"mods\":0}");
            p2.sendText("{\"t\":\"tkey\",\"term\":7,\"key\":\"Enter\",\"mods\":0}");
            CHECK(sized(101, 31));
            CHECK(a.wait([&] { return a.out.find("31 101\r\n") != std::string::npos; }));
            CHECK(p1.pump(take1, [&] { return t1.ptyCols == 101 && t1.ptyRows == 31; }));

            // The editor's renderer types: the opener's size again.
            a.client->terminalInput(7, "stty size\n");
            CHECK(sized(80, 24));
            CHECK(a.wait([&] { return a.out.find("24 80\r\n") != std::string::npos; }));
            CHECK(p2.pump(take2, [&] { return t2.ptyCols == 80 && t2.ptyRows == 24; }));
            CHECK(p2.pump(take2, [&] { return t2.all().find("24 80") != std::string::npos; }));

            // The phone's page types again: its size, said again.
            p1.sendText("{\"t\":\"ttext\",\"term\":7,\"text\":\" \",\"mods\":0}");
            CHECK(sized(32, 12));
            // An answer to a query from the editor's emulator is not
            // typing: the phone's size stays.
            a.client->terminalInput(7, "\x15", true);   // Control U: the line goes, harmless
            CHECK(a.client->settle());
            CHECK(ptySize() == std::make_pair(32, 12));
            p1.close();
            p2.close();
            CHECK(waitUntil([&] { return web.browsers() == 0; }));
            a.client->closeTerminal(7);
        }
        web.stop();
        webThread.join();
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
    // the app to open it (Options::open) from an editable page, and a page
    // that only reads is refused and reads the file for a tab of its own
    // (readfile, R7) instead; nothing of it is reachable without the token
    // or outside the socket.
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
        writeText(root + "/.env", "TOKEN=1\n");
        writeText(root + "/blob.bin", std::string("\x7f" "ELF\0\0\x01", 7));
        writeText(root + "/notes.md", "# Notes\n\nSome *text*.\n");
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
        Web::Server editing;   // the same, editable
        Web::Options e = o;
        e.editable = true;
        e.port = 0;
        CHECK(editing.start(e, &error));
        std::thread editingThread([&] { editing.run(); });
        Web::Server headless;   // no window to open files in
        Web::Options h = e;
        h.open = nullptr;
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
            Json state, hello;
            std::map<std::string, Json> trees, files;
            std::vector<Json> opens;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "state") state = m;
                else if (t == "hello") hello = m;
                else if (t == "tree") trees[m["path"].asString()] = m;
                else if (t == "opening") opens.push_back(m);
                else if (t == "filetext") files[m["path"].asString()] = m;
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(!hello["editable"].asBool() && !hello["canOpen"].asBool() && !hello["canClose"].asBool());
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
            const std::string topNames = "away/(outside) src/ blob.bin main.py notes.md run.log(ignored) ";
            CHECK(names == topNames);
            if (names != topNames) std::printf("  sessionweb:tree: %s\n", names.c_str());
            p.sendText("{\"t\":\"list\",\"path\":\"src\"}");
            CHECK(p.pump(take, [&] { return trees.count("src"); }));
            CHECK(trees["src"]["entries"].size() == 1 && trees["src"]["entries"][0]["name"].asString() == "util.c");
            // Out of the folder: refused, nothing listed.
            for (const char* bad : {"..", "/etc", "away", "src/../.."}) {
                p.sendText(std::string("{\"t\":\"list\",\"path\":\"") + bad + "\"}");
                CHECK(p.pump(take, [&] { return trees.count(bad); }));
                CHECK(trees[bad].has("error") && !trees[bad].has("entries"));
            }
            // A click on a file in a page that only reads: the window opens
            // nothing (a tab opened changes the window), and the page reads
            // the file for a tab of its own instead.
            p.sendText("{\"t\":\"open\",\"path\":\"src/util.c\"}");
            p.sendText("{\"t\":\"open\",\"path\":\"main.py\",\"keep\":true}");
            CHECK(p.pump(take, [&] { return opens.size() == 2; }));
            for (const Json& m : opens)
                CHECK(!m["ok"].asBool() && m["why"].asString().find("only reads") != std::string::npos);
            p.sendText("{\"t\":\"readfile\",\"path\":\"src/util.c\"}");
            p.sendText("{\"t\":\"readfile\",\"path\":\"notes.md\"}");
            CHECK(p.pump(take, [&] { return files.count("src/util.c") && files.count("notes.md"); }));
            {
                const Json& f = files["src/util.c"];
                CHECK(!f.has("error") && f["abs"].asString() == root + "/src/util.c" && f["kind"].asString() == "code");
                PageDoc local;
                Json asDoc = f;
                asDoc.set("t", Json("doc"));
                local.take(asDoc);
                CHECK(local.text() == "int u;\n");
                CHECK(f["lines"].size() == 2 && f["lines"][0][1].size() > 0);   // highlighted: int is a keyword
                CHECK(files["notes.md"]["kind"].asString() == "markdown" &&
                      files["notes.md"]["html"].asString().find("<em>text</em>") != std::string::npos);
            }
            // A folder, a path out of the folder, a link out, a dotfile the
            // tree hides, a binary file and a missing one: refused.
            const std::vector<std::string> badReads = {"src", "../" + elsewhere.substr(5) + "/secret.txt",
                                                       "away/secret.txt", ".env", "src/../.env", "blob.bin",
                                                       "missing.txt", "/etc/hosts"};
            for (const std::string& bad : badReads) {
                Json q = Json::object();
                q.set("t", Json("readfile"));
                q.set("path", Json(bad));
                p.sendText(q.dump());
            }
            CHECK(p.pump(take, [&] {
                for (const std::string& bad : badReads)
                    if (!files.count(bad)) return false;
                return true;
            }));
            for (const std::string& bad : badReads) {
                const Json& f = files[bad];
                if (!f.has("error") || f.has("lines")) std::printf("  sessionweb:tree: read %s: %s\n", bad.c_str(), f.dump().c_str());
                CHECK(f.has("error") && !f.has("lines") && f.dump().find("not yours") == std::string::npos &&
                      f.dump().find("TOKEN") == std::string::npos);
            }
            CHECK(files["blob.bin"]["error"].asString().find("not text") != std::string::npos);
            // The same, called directly: a dotfile where the tree shows them.
            CHECK(!Web::localFileJson(root, ".env", true).has("error"));
            CHECK(Web::localFileJson(root, ".env", false).has("error"));
            CHECK(Web::localFileJson("", "main.py", false).has("error"));
            {
                std::lock_guard<std::mutex> lk(openedMu);
                CHECK(opened.empty());
            }
            p.close();

            // An editable page: a click opens the file in the window; a
            // folder, or a path out of the folder, is not opened.
            PageClient ep;
            CHECK(ep.upgrade(editing.port(), "/ws?t=" + editing.token()));
            hello = Json();
            state = Json();
            opens.clear();
            CHECK(ep.pump(take, [&] { return state.isObject(); }));
            CHECK(hello["canOpen"].asBool() && !hello["canClose"].asBool());
            ep.sendText("{\"t\":\"open\",\"path\":\"src/util.c\"}");
            ep.sendText("{\"t\":\"open\",\"path\":\"main.py\",\"keep\":true}");
            ep.sendText("{\"t\":\"open\",\"path\":\"src\"}");
            ep.sendText("{\"t\":\"open\",\"path\":\"../" + elsewhere.substr(5) + "/secret.txt\"}");
            ep.sendText("{\"t\":\"open\",\"path\":\"away/secret.txt\"}");
            CHECK(ep.pump(take, [&] { return opens.size() == 5; }));
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
            ep.close();

            // A host with no window to open files in says so.
            PageClient q;
            CHECK(q.upgrade(headless.port(), "/ws?t=" + headless.token()));
            Json hstate, hhello, hfile;
            std::vector<Json> hopens;
            bool listed = false;
            auto htake = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "state") hstate = m;
                else if (t == "hello") hhello = m;
                else if (t == "tree") listed = m["entries"].size() == 6;
                else if (t == "opening") hopens.push_back(m);
                else if (t == "filetext") hfile = m;
            };
            CHECK(q.pump(htake, [&] { return hstate.isObject(); }));
            CHECK(!hhello["canOpen"].asBool());   // the page reads files for itself
            q.sendText("{\"t\":\"list\",\"path\":\"\"}");
            q.sendText("{\"t\":\"open\",\"path\":\"main.py\"}");
            q.sendText("{\"t\":\"readfile\",\"path\":\"main.py\"}");
            CHECK(q.pump(htake, [&] { return listed && hopens.size() == 1 && hfile.isObject(); }));
            CHECK(hopens.size() == 1 && !hopens[0]["ok"].asBool() &&
                  hopens[0]["why"].asString().find("headless") != std::string::npos);
            CHECK(hfile["lines"].size() == 2 && hfile["lines"][0][0].asString() == "print(1)");
            q.close();
            CHECK(waitUntil([&] { return web.browsers() == 0 && headless.browsers() == 0 && editing.browsers() == 0; }));
        }
        web.stop();
        webThread.join();
        editing.stop();
        editingThread.join();
        headless.stop();
        headlessThread.join();
        // The page carries the tree and its sidebar.
        const std::string& page = Web::pageHtml();
        CHECK(page.find("id=\"tree\"") != std::string::npos && page.find("id=\"sidebtn\"") != std::string::npos);
        CHECK(Web::paletteJson().has("treeIgnored"));
    }

    // Files for the page's viewers (R3): what is served and how, as pure
    // functions; the host's "read" confined as the tree's listing is; then
    // the whole server over the wire: no file without the token or before a
    // page is open, every escape refused, the bytes whole and in ranges,
    // nosniff and a sandbox CSP, and a CSV as rows over the socket.
    GROUP("sessionweb:files");
    {
        // What the page shows itself, and how it is served.
        CHECK(Web::fileType("a/b/Shot.PNG").view == "image" && Web::fileType("x.png").mime == "image/png");
        CHECK(Web::fileType("x.jpeg").mime == "image/jpeg" && Web::fileType("x.webp").view == "image");
        CHECK(Web::fileType("logo.svg").mime == "image/svg+xml" && Web::fileType("logo.svg").sandbox);
        CHECK(Web::fileType("doc.pdf").view == "pdf" && Web::fileType("doc.pdf").mime == "application/pdf");
        CHECK(!Web::fileType("doc.pdf").sandbox);
        CHECK(Web::fileType("song.mp3").view == "audio" && Web::fileType("song.mp3").maxBytes == 0);
        CHECK(Web::fileType("clip.mov").mime == "video/quicktime" && Web::fileType("clip.webm").view == "video");
        CHECK(Web::fileType("index.html").view == "html" && Web::fileType("index.html").sandbox);
        CHECK(Web::fileType("index.htm").mime == "text/html; charset=utf-8");
        CHECK(Web::fileType("data.csv").view == "table" && Web::fileType("data.csv").mime.empty());
        CHECK(Web::fileType("data.TSV").view == "table");
        CHECK(Web::fileType("site.css").view.empty() && Web::fileType("site.css").mime == "text/css; charset=utf-8");
        for (const char* text : {"main.py", "notes.txt", "Makefile", ".png", "a.js", "x.sqlite", "a.ts"})
            CHECK(Web::fileType(text).view.empty() && Web::fileType(text).mime.empty());
        CHECK(Web::fileType("big.png").maxBytes == std::uint64_t(64) << 20);
        CHECK(Web::fileTarget("ab12", "docs/My Shot+1.png") == "/file/ab12/docs/My%20Shot%2B1.png");
        CHECK(Web::fileTarget("ab12", "caf\xc3\xa9/%.png") == "/file/ab12/caf%C3%A9/%25.png");

        // Ranges: one range, clipped; several, another unit or malformed,
        // the whole file; past the end, unsatisfiable.
        auto range = [](const char* h, std::uint64_t size) {
            const std::string header = h ? h : "";
            return Web::byteRange(h ? &header : nullptr, size);
        };
        Web::ByteRange br = range(nullptr, 1000);
        CHECK(br.kind == Web::ByteRange::Whole && br.start == 0 && br.end == 999 && br.length() == 1000);
        br = range("bytes=100-199", 1000);
        CHECK(br.kind == Web::ByteRange::Part && br.start == 100 && br.end == 199 && br.length() == 100);
        br = range("bytes=900-", 1000);
        CHECK(br.kind == Web::ByteRange::Part && br.start == 900 && br.end == 999);
        br = range("bytes=990-5000", 1000);
        CHECK(br.kind == Web::ByteRange::Part && br.end == 999 && br.length() == 10);
        br = range("bytes=-10", 1000);
        CHECK(br.kind == Web::ByteRange::Part && br.start == 990 && br.end == 999);
        br = range("bytes=-5000", 1000);
        CHECK(br.kind == Web::ByteRange::Part && br.start == 0 && br.end == 999);
        CHECK(range("bytes=1000-", 1000).kind == Web::ByteRange::Unsatisfiable);
        CHECK(range("bytes=0-", 0).kind == Web::ByteRange::Unsatisfiable);
        CHECK(range("bytes=-0", 1000).kind == Web::ByteRange::Unsatisfiable);
        for (const char* whole : {"bytes=0-1,5-9", "items=0-5", "bytes=5-1", "bytes=a-b", "bytes=", "bytes=-", "0-5",
                                  "bytes=99999999999999999999-"})
            CHECK(range(whole, 1000).kind == Web::ByteRange::Whole);

        // The heads.
        std::string head = Web::fileHead(Web::fileType("a.png"), 1000, range(nullptr, 1000));
        CHECK(head.compare(0, 15, "HTTP/1.1 200 OK") == 0);
        CHECK(head.find("\r\nContent-Type: image/png\r\n") != std::string::npos);
        CHECK(head.find("\r\nContent-Length: 1000\r\n") != std::string::npos);
        CHECK(head.find("\r\nX-Content-Type-Options: nosniff\r\n") != std::string::npos);
        CHECK(head.find("\r\nAccept-Ranges: bytes\r\n") != std::string::npos);
        CHECK(head.find("Content-Security-Policy: sandbox;") != std::string::npos);
        CHECK(head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0);
        head = Web::fileHead(Web::fileType("a.mp4"), 1000, range("bytes=100-199", 1000));
        CHECK(head.compare(0, 12, "HTTP/1.1 206") == 0);
        CHECK(head.find("\r\nContent-Range: bytes 100-199/1000\r\n") != std::string::npos);
        CHECK(head.find("\r\nContent-Length: 100\r\n") != std::string::npos);
        head = Web::fileHead(Web::fileType("a.mp4"), 1000, range("bytes=2000-", 1000));
        CHECK(head.compare(0, 12, "HTTP/1.1 416") == 0 && head.find("Content-Range: bytes */1000") != std::string::npos);
        for (const char* sandboxed : {"a.svg", "a.html", "a.xhtml", "a.css", "a.mp3", "a.webm"})
            CHECK(Web::fileHead(Web::fileType(sandboxed), 10, range(nullptr, 10))
                      .find("Content-Security-Policy: sandbox; script-src 'none'") != std::string::npos);
        CHECK(Web::fileHead(Web::fileType("a.pdf"), 10, range(nullptr, 10)).find("Content-Security-Policy") ==
              std::string::npos);

        // The token is the path's first name; the Host and Origin rules hold.
        const std::string token = Web::newToken();
        auto ask = [&](const std::string& target, const std::string& extra = "") {
            return Web::answer(request(getWith(target, extra)), token, "page");
        };
        Web::Answer a = ask("/file/" + token + "/pics/My%20Shot+1.png");
        CHECK(a.kind == Web::Answer::File && a.file == "pics/My Shot+1.png");
        a = ask("/file/" + token + "/a%2F..%2Fb.png");
        CHECK(a.kind == Web::Answer::File && a.file == "a/../b.png");   // the host refuses it (below)
        CHECK(ask("/file/" + token + "/").kind == Web::Answer::File);    // no name: the host refuses it
        a = ask("/file//pic.png");
        CHECK(a.kind == Web::Answer::Refused && a.status == 403);
        a = ask("/file/" + token);
        CHECK(a.kind == Web::Answer::Refused && a.status == 403);
        a = ask("/file/" + token.substr(0, 47) + "0/pic.png");
        CHECK(a.kind == Web::Answer::Refused && a.status == 403);
        a = ask("/file/pic.png?t=" + token);
        CHECK(a.kind == Web::Answer::Refused && a.status == 403);
        a = ask("/file/" + token + "/a%00.png");
        CHECK(a.kind == Web::Answer::Refused && a.status == 400);
        a = Web::answer(request("GET /file/" + token + "/pic.png HTTP/1.1\r\nHost: evil.com\r\n\r\n"), token, "page");
        CHECK(a.kind == Web::Answer::Refused && a.status == 403);
        a = ask("/file/" + token + "/pic.png", "Origin: http://evil.example\r\n");
        CHECK(a.kind == Web::Answer::Refused && a.status == 403);
        a = Web::answer(request("POST /file/" + token + "/pic.png HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), token, "page");
        CHECK(a.kind == Web::Answer::Refused && a.status == 405);

        // A CSV as the page's rows: quotes, a TSV, a cut start.
        Json t = Web::tableJson("d.csv", "name,note\r\n\"Smith, J\",\"said \"\"hi\"\"\"\nx\n", true);
        CHECK(t["columns"].asInt() == 2 && t["rows"].size() == 3 && !t["truncated"].asBool());
        CHECK(t["rows"][1][0].asString() == "Smith, J" && t["rows"][1][1].asString() == "said \"hi\"");
        CHECK(t["rows"][2].size() == 1);
        t = Web::tableJson("d.tsv", "a\tb,c\n1\t2\n", true);
        CHECK(t["columns"].asInt() == 2 && t["rows"][0][1].asString() == "b,c");
        t = Web::tableJson("d.csv", "a,b\n1,2\n3,", false);
        CHECK(t["rows"].size() == 2 && t["truncated"].asBool());
        std::string wide;
        for (int r = 0; r < 300; r++) wide += std::string(999, ',') + "\n";   // 1,000 cells a row
        t = Web::tableJson("d.csv", wide, true);
        CHECK(t["rows"].size() == Web::kMaxTableCells / 1000 && t["truncated"].asBool());
        t = Web::tableJson("d.csv", std::string(5000, 'x') + "\n", true);
        CHECK(t["rows"][0][0].asString().size() == Web::kMaxCellBytes);

        // The host's read: confined as the listing is, a piece at a time.
        const std::string root = Shared::resolveFolder(scratchFolder()), elsewhere = scratchFolder();
        std::string clip;
        for (std::uint32_t i = 0; i < (5u << 20) + 123; i++) clip += (char)((i * 7919u) >> 3);
        writeText(root + "/clip.mp4", clip);
        writeText(root + "/.hidden.png", "h");
        writeText(elsewhere + "/secret.png", "not yours");
        ::mkdir((root + "/sub").c_str(), 0755);
        writeText(root + "/sub/in.png", "inside");
        CHECK(::symlink(elsewhere.c_str(), (root + "/away").c_str()) == 0);
        CHECK(::symlink((elsewhere + "/secret.png").c_str(), (root + "/leak.png").c_str()) == 0);
        CHECK(::symlink((root + "/sub/in.png").c_str(), (root + "/near.png").c_str()) == 0);
        CHECK(::mkfifo((root + "/pipe.png").c_str(), 0600) == 0);
        auto fsRead = [](const std::string& path, double offset, double length, bool hidden = false) {
            Json r = Json::object();
            r.set("op", Json("read"));
            r.set("path", Json(path));
            r.set("offset", Json(offset));
            r.set("length", Json(length));
            r.set("hidden", Json(hidden));
            return r;
        };
        std::string bytes;
        Json got = Shared::readWorkspace(root, fsRead("clip.mp4", 100, 50), &bytes);
        CHECK(got["size"].asInt() == (int64_t)clip.size() && got["offset"].asInt() == 100 &&
              got["length"].asInt() == 50 && bytes == clip.substr(100, 50));
        got = Shared::readWorkspace(root, fsRead("clip.mp4", 0, 0), &bytes);
        CHECK(got["size"].asInt() == (int64_t)clip.size() && got["length"].asInt() == 0 && bytes.empty());
        got = Shared::readWorkspace(root, fsRead("clip.mp4", 0, 1e12), &bytes);
        CHECK(bytes.size() == Shared::kMaxReadBytes && bytes == clip.substr(0, Shared::kMaxReadBytes));
        got = Shared::readWorkspace(root, fsRead("clip.mp4", (double)clip.size() - 3, 100), &bytes);
        CHECK(got["length"].asInt() == 3 && bytes == clip.substr(clip.size() - 3));
        got = Shared::readWorkspace(root, fsRead("clip.mp4", 1e12, 100), &bytes);
        CHECK(!got.has("error") && bytes.empty());
        got = Shared::readWorkspace(root, fsRead("near.png", 0, 100), &bytes);   // a link inside
        CHECK(!got.has("error") && bytes == "inside");
        got = Shared::readWorkspace(root, fsRead(".hidden.png", 0, 100, true), &bytes);
        CHECK(!got.has("error") && bytes == "h");
        int refusedReads = 0;
        for (const char* bad : {"../" , "..", "", "/etc/passwd", "sub/../../x.png", "./clip.mp4", "away/secret.png",
                                "leak.png", ".hidden.png", "sub", "pipe.png", "missing.png", "sub//in.png"}) {
            bytes = "left";
            got = Shared::readWorkspace(root, fsRead(bad, 0, 100), &bytes);
            if (got.has("error") && bytes.empty()) refusedReads++;
            else std::printf("  sessionweb:files: read %s: %s\n", bad, got.dump().c_str());
        }
        CHECK(refusedReads == 13);
        // FsData carries the bytes through the wire's encoding.
        {
            Shared::Message m;
            m.kind = Shared::Kind::FsData;
            m.req = 9;
            m.json = got;
            m.bytes = clip.substr(0, 70000);
            Shared::Message back;
            CHECK(Shared::decode(Shared::encode(m), &back) && back.bytes == m.bytes && back.kind == Shared::Kind::FsData);
        }

        // The server, over the wire, on a host serving that folder.
        writeText(root + "/pic.png", std::string("\x89PNG\r\n\x1a\n", 8) + "pixels");
        writeText(root + "/logo.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>");
        writeText(root + "/page.html", "<p>hi</p><script>alert(1)</script>");
        writeText(root + "/doc.pdf", "%PDF-1.4\n");
        writeText(root + "/data.csv", "name,qty\napple,3\n\"pear, green\",5\n");
        writeText(root + "/notes.txt", "text\n");
        Shared::HostServer host;
        Shared::HostOptions ho;
        ho.folder = root;
        ho.socketDir = scratchFolder() + "/s";
        ho.idleMs = 60000;
        std::string error;
        CHECK(host.start(ho, &error) == Shared::HostServer::Start::Ok);
        std::thread hostThread([&] { host.run(); });
        Web::Server web;
        Web::Options o;
        o.hostSocket = host.socketPath();
        o.folder = root;
        o.pollMs = 100;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        const int port = web.port();
        const std::string base = "/file/" + web.token() + "/";
        auto get = [&](const std::string& target, const std::string& extra = "") {
            PageClient c;
            return c.exchange(port, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                        "\r\n" + extra + "\r\n");
        };
        auto bodyOf = [](const std::string& r) {
            const std::size_t e = r.find("\r\n\r\n");
            return e == std::string::npos ? std::string() : r.substr(e + 4);
        };
        auto headOf = [](const std::string& r) { return r.substr(0, r.find("\r\n\r\n") + 2); };
        // Nothing before a page is open: the session is read only for one.
        CHECK(get(base + "pic.png").compare(0, 12, "HTTP/1.1 503") == 0);
        PageClient p;
        CHECK(p.upgrade(port, "/ws?t=" + web.token()));
        Json state;
        std::map<std::string, Json> trees, tables;
        auto take = [&](const Json& m) {
            const std::string ty = m["t"].asString();
            if (ty == "state") state = m;
            else if (ty == "tree") trees[m["path"].asString()] = m;
            else if (ty == "table") tables[m["path"].asString()] = m;
        };
        CHECK(p.pump(take, [&] { return state.isObject(); }));

        std::string r = get(base + "pic.png");
        CHECK(r.compare(0, 15, "HTTP/1.1 200 OK") == 0);
        CHECK(headOf(r).find("Content-Type: image/png\r\n") != std::string::npos);
        CHECK(headOf(r).find("X-Content-Type-Options: nosniff\r\n") != std::string::npos);
        CHECK(bodyOf(r) == std::string("\x89PNG\r\n\x1a\n", 8) + "pixels");
        // No token, a wrong one, the query's form, another Host: refused.
        CHECK(get("/file//pic.png").compare(0, 12, "HTTP/1.1 403") == 0);
        CHECK(get("/file/" + std::string(48, 'a') + "/pic.png").compare(0, 12, "HTTP/1.1 403") == 0);
        CHECK(get("/file/pic.png?t=" + web.token()).compare(0, 12, "HTTP/1.1 403") == 0);
        {
            PageClient c;
            CHECK(c.exchange(port, "GET " + base + "pic.png HTTP/1.1\r\nHost: evil.com\r\n\r\n").compare(0, 12, "HTTP/1.1 403") == 0);
        }
        // Escapes and what the tree does not show: refused, nothing of them sent.
        for (const std::string& bad : std::vector<std::string>{"../" + elsewhere.substr(5) + "/secret.png", "..%2F..%2Fetc%2Fhosts.png",
                                      "away/secret.png", "leak.png", ".hidden.png", "%2E%2E/x.png", "pipe.png",
                                      "sub%2F..%2F..%2Fx.png", "missing.png"}) {
            r = get(base + bad);
            CHECK(r.compare(0, 12, "HTTP/1.1 404") == 0 && r.find("not yours") == std::string::npos);
            if (r.compare(0, 12, "HTTP/1.1 404") != 0) std::printf("  sessionweb:files: %s: %s\n", bad.c_str(), r.substr(0, 80).c_str());
        }
        CHECK(get(base + "notes.txt").compare(0, 12, "HTTP/1.1 404") == 0);   // text opens as a tab
        CHECK(get(base + "sub/in.png").find("\r\n\r\ninside") != std::string::npos);
        // A script in an SVG or HTML file runs nowhere: sandboxed; a PDF is not.
        CHECK(headOf(get(base + "logo.svg")).find("Content-Security-Policy: sandbox;") != std::string::npos);
        r = get(base + "page.html");
        CHECK(headOf(r).find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
        CHECK(headOf(r).find("Content-Security-Policy: sandbox; script-src 'none'") != std::string::npos);
        r = get(base + "doc.pdf");
        CHECK(headOf(r).find("Content-Type: application/pdf\r\n") != std::string::npos &&
              headOf(r).find("Content-Security-Policy") == std::string::npos && bodyOf(r) == "%PDF-1.4\n");
        // A video whole (three pieces from the host) and in ranges.
        r = get(base + "clip.mp4");
        CHECK(headOf(r).find("Content-Length: " + std::to_string(clip.size()) + "\r\n") != std::string::npos);
        CHECK(bodyOf(r) == clip);
        r = get(base + "clip.mp4", "Range: bytes=100-199\r\n");
        CHECK(r.compare(0, 12, "HTTP/1.1 206") == 0);
        CHECK(headOf(r).find("Content-Range: bytes 100-199/" + std::to_string(clip.size()) + "\r\n") != std::string::npos);
        CHECK(bodyOf(r) == clip.substr(100, 100));
        r = get(base + "clip.mp4", "Range: bytes=-10\r\n");
        CHECK(r.compare(0, 12, "HTTP/1.1 206") == 0 && bodyOf(r) == clip.substr(clip.size() - 10));
        r = get(base + "clip.mp4", "Range: bytes=4194000-\r\n");
        CHECK(r.compare(0, 12, "HTTP/1.1 206") == 0 && bodyOf(r) == clip.substr(4194000));
        r = get(base + "clip.mp4", "Range: bytes=" + std::to_string(clip.size()) + "-\r\n");
        CHECK(r.compare(0, 12, "HTTP/1.1 416") == 0 && bodyOf(r).empty());

        // The tree says how the page shows each file; a CSV comes as rows.
        p.sendText("{\"t\":\"list\",\"path\":\"\"}");
        CHECK(p.pump(take, [&] { return trees.count(""); }));
        std::map<std::string, std::string> views;
        for (const Json& e : trees[""]["entries"].elements()) views[e["name"].asString()] = e["view"].asString();
        CHECK(views["pic.png"] == "image" && views["clip.mp4"] == "video" && views["doc.pdf"] == "pdf");
        CHECK(views["page.html"] == "html" && views["data.csv"] == "table" && views["notes.txt"].empty());
        CHECK(views["sub"].empty());
        p.sendText("{\"t\":\"file\",\"path\":\"data.csv\"}");
        p.sendText("{\"t\":\"file\",\"path\":\"away/secret.csv\"}");
        p.sendText("{\"t\":\"file\",\"path\":\"pic.png\"}");
        CHECK(p.pump(take, [&] { return tables.size() == 3; }));
        const Json& table = tables["data.csv"];
        CHECK(table["columns"].asInt() == 2 && table["rows"].size() == 3 && !table["truncated"].asBool());
        CHECK(table["rows"][2][0].asString() == "pear, green" && table["rows"][1][1].asString() == "3");
        CHECK(tables["away/secret.csv"].has("error") && !tables["away/secret.csv"].has("rows"));
        CHECK(tables["pic.png"].has("error"));
        p.close();
        CHECK(waitUntil([&] { return web.browsers() == 0; }));
        // The page gone, the session's reads end with it.
        CHECK(get(base + "pic.png").compare(0, 12, "HTTP/1.1 503") == 0);
        web.stop();
        webThread.join();
        host.stop();
        hostThread.join();
        // The page's CSP lets the file views load from its own server only.
        const Web::Answer pageAnswer = ask("/?t=" + token);
        CHECK(pageAnswer.response.find("img-src data: 'self'; media-src 'self';") != std::string::npos);
        CHECK(pageAnswer.response.find("frame-src 'self' http: https:;") != std::string::npos);
        CHECK(Web::pageHtml().find("id=\"fileview\"") != std::string::npos);
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

    // R7, the owner's four: the window says which panes it shows, so the
    // page lays out its browser pane beside the editor and its terminal
    // below as the window does; an editable page closes the window's tabs
    // as the window does (never one with unsaved changes, never another
    // window's), and a page that only reads closes none; a terminal the
    // host closes leaves every page at once, not at the next look.
    GROUP("sessionweb:close-and-panes");
    {
        // The report, and the page's copy checked again.
        Json panes = Web::panesReportJson(true, false, 0.35);
        CHECK(panes["browser"].asBool() && !panes["terminal"].asBool() && panes["browserShare"].asNumber() == 0.35);
        Json view = Web::panesViewJson(panes);
        CHECK(view == panes);
        Json odd;
        CHECK(Json::parse("{\"browser\":\"yes\",\"terminal\":1,\"browserShare\":7,\"x\":\"<script>\"}", odd));
        view = Web::panesViewJson(odd);
        CHECK(!view["browser"].asBool() && !view["terminal"].asBool() && view["browserShare"].asNumber() == 0.9);
        CHECK(!view.has("x"));
        CHECK(Web::panesViewJson(Web::panesReportJson(false, true, -3))["browserShare"].asNumber() == 0.1);
        CHECK(Web::panesViewJson(Json::object())["browserShare"].asNumber() == 0.5);
        CHECK(Web::panesViewJson(Json()).isNull() && Web::panesViewJson(Json(2.0)).isNull());
        Message wt;
        wt.kind = Shared::Kind::WindowTabs;
        wt.window = 9;
        wt.json = Json::object();
        wt.json.set("panes", panes);
        CHECK(Shared::roundTrip(wt).json["panes"] == panes);

        // An in-process host: window 5 holds a clean tab and a dirty one,
        // window 6 one of its own.
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
        TestMirror dirty;
        dirty.client = &editor;
        dirty.doc = 2;
        dirty.text = u"x = 1\n";
        editor.open(1, "/w/clean.py", "y = 2\n", nullptr);
        editor.open(2, "/w/dirty.py", Model::utf8FromUtf16(dirty.text), &dirty);
        editor.open(3, "/other/theirs.py", "z = 3\n", nullptr);
        dirty.caret = dirty.text.size();
        dirty.type(u"# unsaved\n");
        auto windowOf = [](std::vector<std::pair<Shared::DocId, std::string>> docs, Json panes) {
            Json tabs = Json::array();
            for (const auto& [d, path] : docs) {
                Json t = Json::object();
                t.set("doc", Json((int64_t)d));
                t.set("path", Json(path));
                tabs.push(std::move(t));
            }
            Json g = Json::object();
            g.set("tabs", std::move(tabs));
            g.set("active", Json((int64_t)(docs.empty() ? 0 : docs[0].first)));
            Json groups = Json::array();
            groups.push(std::move(g));
            Json w = Json::object();
            w.set("root", Json("/w"));
            w.set("focused", Json(0));
            w.set("groups", std::move(groups));
            if (!panes.isNull()) w.set("panes", std::move(panes));
            return w;
        };
        editor.windowTabs(5, windowOf({{1, "/w/clean.py"}, {2, "/w/dirty.py"}},
                                      Web::panesReportJson(true, true, 0.4)));
        editor.windowTabs(6, windowOf({{3, "/other/theirs.py"}}, Json()));
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
        std::mutex closedMu;
        std::vector<Shared::DocId> closed;
        auto serve = [&](Web::Server& web, bool editable) {
            Web::Options o;
            o.folder = "/w";
            o.window = 5;
            o.pollMs = 100;
            o.editable = editable;
            o.connect = connectTo;
            o.close = [&](Shared::DocId d) {
                std::lock_guard<std::mutex> lk(closedMu);
                closed.push_back(d);
                return true;
            };
            std::string error;
            CHECK(web.start(o, &error));
        };
        Web::Server editing, reading;
        serve(editing, true);
        serve(reading, false);
        std::thread t1([&] { editing.run(); }), t2([&] { reading.run(); });
        {
            PageClient p;
            CHECK(p.upgrade(editing.port(), "/ws?t=" + editing.token()));
            Json hello, state;
            std::vector<Json> answers, notes;
            auto take = [&](const Json& m) {
                const std::string t = m["t"].asString();
                if (t == "hello") hello = m;
                else if (t == "state") state = m;
                else if (t == "closing") answers.push_back(m);
                else if (t == "note") notes.push_back(m);
            };
            auto dirtyOf = [&](Shared::DocId d) {
                for (const Json& x : state["docs"].elements())
                    if ((Shared::DocId)x["id"].asInt() == d) return x["dirty"].asBool();
                return false;
            };
            CHECK(p.pump(take, [&] { return state.isObject() && dirtyOf(2); }));
            CHECK(hello["canClose"].asBool());
            // The window's panes, for the page's layout.
            CHECK(state["windows"].size() == 1);
            CHECK(state["windows"][0]["panes"] == Web::panesReportJson(true, true, 0.4));
            // The window hides its terminal panel: the page hears it.
            editor.windowTabs(5, windowOf({{1, "/w/clean.py"}, {2, "/w/dirty.py"}},
                                          Web::panesReportJson(true, false, 0.4)));
            CHECK(editor.settle());
            CHECK(p.pump(take, [&] { return !state["windows"][0]["panes"]["terminal"].asBool(); }));
            // Closing: the clean tab goes to the window; the dirty one, another
            // window's and one that is no document are refused before it.
            p.sendText("{\"t\":\"close\",\"doc\":2}");
            p.sendText("{\"t\":\"close\",\"doc\":3}");
            p.sendText("{\"t\":\"close\",\"doc\":99}");
            p.sendText("{\"t\":\"close\",\"doc\":\"1\"}");
            p.sendText("{\"t\":\"close\",\"doc\":1}");
            CHECK(p.pump(take, [&] { return answers.size() == 5; }));
            if (answers.size() == 5) {
                CHECK(!answers[0]["ok"].asBool() && answers[0]["why"].asString().find("unsaved") != std::string::npos);
                CHECK(answers[0]["why"].asString().find("dirty.py") != std::string::npos);
                for (int i = 1; i < 4; i++)
                    CHECK(!answers[i]["ok"].asBool() && answers[i]["why"].asString().find("not one of this window") !=
                                                            std::string::npos);
                CHECK(answers[4]["ok"].asBool() && answers[4]["doc"].asInt() == 1);
            }
            {
                std::lock_guard<std::mutex> lk(closedMu);
                CHECK(closed.size() == 1 && closed[0] == 1);
            }
            // The window closes it, and reports its tabs without it.
            editor.windowTabs(5, windowOf({{2, "/w/dirty.py"}}, Web::panesReportJson(true, false, 0.4)));
            CHECK(editor.settle());
            CHECK(p.pump(take, [&] { return state["docs"].size() == 1 && state["docs"][0]["id"].asInt() == 2; }));
            p.close();

            // A page that only reads closes nothing of the window's.
            PageClient r;
            CHECK(r.upgrade(reading.port(), "/ws?t=" + reading.token()));
            hello = Json();
            state = Json();
            answers.clear();
            CHECK(r.pump(take, [&] { return state.isObject(); }));
            CHECK(!hello["canClose"].asBool() && !hello["canOpen"].asBool());
            CHECK(!state["windows"][0]["panes"]["terminal"].asBool());   // the layout is a reader's too
            r.sendText("{\"t\":\"close\",\"doc\":2}");
            CHECK(r.pump(take, [&] { return answers.size() == 1; }));
            CHECK(answers.size() == 1 && !answers[0]["ok"].asBool() &&
                  answers[0]["why"].asString().find("only reads") != std::string::npos);
            r.close();
            {
                std::lock_guard<std::mutex> lk(closedMu);
                CHECK(closed.size() == 1);
            }
            CHECK(waitUntil([&] { return editing.browsers() == 0 && reading.browsers() == 0; }));
        }
        editing.stop();
        reading.stop();
        t1.join();
        t2.join();
        {
            std::lock_guard<std::mutex> lk(routesMu);
            routes.clear();
        }
        // The page: a browser pane of its own with its close button, a
        // terminal pane with its own, and buttons that show them again.
        const std::string& page = Web::pageHtml();
        for (const char* id : {"id=\"top\"", "id=\"webtabs\"", "id=\"webclose\"", "id=\"termclose\"",
                               "id=\"webbtn\"", "id=\"termbtn\""})
            CHECK(page.find(id) != std::string::npos);
        CHECK(page.find("\"readfile\"") != std::string::npos && page.find("\"close\"") != std::string::npos);
    }

    // A terminal the host closes (its window closed it, or its shell
    // ended) leaves the pages at once: the view hears it end and looks at
    // the state then, not at its next look (here a minute away). For a
    // headless host's view, which has no window to stop reporting it.
    GROUP("sessionweb:terminal-ends");
    {
        ServerRig rig;
        CHECK(rig.start(60000) == Shared::HostServer::Start::Ok);
        SocketRenderer a;
        CHECK(a.attach(rig.path(), "editor"));
        const std::string home = scratchFolder();
        Web::Server web;
        Web::Options o;
        o.hostSocket = rig.path();
        o.folder = rig.proj;
        o.pollMs = 60000;
        std::string error;
        CHECK(web.start(o, &error));
        std::thread webThread([&] { web.run(); });
        auto idsOf = [](const Json& state) {
            std::string s;
            for (const Json& t : state["terms"].elements()) s += std::to_string(t["id"].asInt()) + " ";
            return s;
        };
        if (a.client && web.port() > 0) {
            a.client->openTerminal(21, shellSpec(home, {"sh"}), &a);
            a.client->openTerminal(22, shellSpec(home, {"sh"}), &a);
            auto prompt = [&](Shared::TermId t) {
                for (const std::string& r : hostRows(rig, t))
                    if (r.find("$") != std::string::npos) return true;
                return false;
            };
            CHECK(a.wait([&] { return prompt(21) && prompt(22); }));
            PageClient p;
            CHECK(p.upgrade(web.port(), "/ws?t=" + web.token()));
            Json state;
            PageTerm term;
            auto take = [&](const Json& m) {
                if (m["t"].asString() == "state") state = m;
                term.take(m);
            };
            CHECK(p.pump(take, [&] { return state.isObject(); }));
            CHECK(idsOf(state) == "21 22 ");
            p.sendText("{\"t\":\"show\",\"doc\":0,\"term\":21}");
            CHECK(p.pump(take, [&] { return term.snapshots == 1 && term.id == 21; }));
            // Its window closes it.
            const double t0 = nowMs();
            a.client->closeTerminal(21);
            CHECK(p.pump(take, [&] { return idsOf(state) == "22 "; }, 5000));
            if (idsOf(state) != "22 ") std::printf("  terminal-ends: %s\n", idsOf(state).c_str());
            CHECK(nowMs() - t0 < 5000);
            // Asked for again by id: nothing of it is left to show.
            const int before = term.snapshots;
            p.sendText("{\"t\":\"show\",\"doc\":0,\"term\":21}");
            p.pump(take, [] { return false; }, 300);
            CHECK(term.snapshots == before);
            // The other ends by itself (its shell exits).
            p.sendText("{\"t\":\"show\",\"doc\":0,\"term\":22}");
            CHECK(p.pump(take, [&] { return term.id == 22; }));
            a.client->terminalInput(22, "exit\n");
            CHECK(p.pump(take, [&] { return state["terms"].size() == 0; }, 5000));
            p.close();
            CHECK(waitUntil([&] { return web.browsers() == 0; }));
        }
        web.stop();
        webThread.join();
    }
}
