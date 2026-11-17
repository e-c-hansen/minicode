// SessionWeb.cpp — see SessionWeb.h.
#include "SessionWeb.h"

#include "IdeLink.h"
#include "MarkdownHtml.h"
#include "SessionServer.h"
#include "Settings.h"
#include "Text.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Shared {
namespace Web {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 &&
                   hexValue(s[i + 2]) >= 0) {
            out += (char)(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string httpResponse(int status, const char* reason, const std::string& type,
                         const std::string& body, const std::string& extra = "") {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
           "X-Content-Type-Options: nosniff\r\nConnection: close\r\n" +
           extra + "\r\n" + body;
}

Answer refuse(int status, const char* reason, const std::string& why) {
    Answer a;
    a.kind = Answer::Refused;
    a.status = status;
    a.why = why;
    a.response = httpResponse(status, reason, "text/plain; charset=utf-8", "minicode: " + why + "\n");
    return a;
}

std::string hexColor(uint32_t rgb) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%06x", rgb & 0xFFFFFF);
    return buf;
}

std::string hexColor(const Rgba& c) { return hexColor((uint32_t)(c.r << 16 | c.g << 8 | c.b)); }

int64_t termColor(const TermColor& c) {
    return c.kind == TermColor::Default ? -1 : (int64_t)c.rgb();
}

int termFlags(const TermStyle& s) {
    int f = 0;
    if (s.bold) f |= kBold;
    if (s.dim) f |= kDim;
    if (s.italic) f |= kItalic;
    if (s.underline) f |= kUnderline;
    if (s.inverse) f |= kInverse;
    if (s.strike) f |= kStrike;
    if (s.hidden) f |= kHidden;
    return f;
}

std::string extensionOfPath(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return lower(name.substr(dot + 1));
}

std::string baseName(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The units of a line's break at the end of [start, end) of `t`.
std::size_t breakUnits(const std::u16string& t, std::size_t start, std::size_t end) {
    if (end <= start) return 0;
    const char16_t c = t[end - 1];
    if (c == u'\n') return end - 1 > start && t[end - 2] == u'\r' ? 2 : 1;
    if (c == u'\r' || c == 0x2029) return 1;
    return 0;
}

const char* const kMarkdownExtensions[] = {"md", "markdown", "mdown", "mkd", "mkdn"};

using Clock = std::chrono::steady_clock;

long long msSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

void setNonBlocking(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

ssize_t sendSome(int fd, const char* p, std::size_t n) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, p, n, MSG_NOSIGNAL);
#else
    return ::send(fd, p, n, 0);
#endif
}

}  // namespace

// ---------------------------------------------------------------- access

std::string newToken() {
    unsigned char bytes[24] = {0};
    bool ok = false;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::size_t got = 0;
        while (got < sizeof bytes) {
            const ssize_t n = ::read(fd, bytes + got, sizeof bytes - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (std::size_t)n;
        }
        ok = got == sizeof bytes;
        ::close(fd);
    }
    if (!ok) {
        // No random source: refuse to make a guessable token.
        std::fprintf(stderr, "minicode: /dev/urandom could not be read\n");
        std::abort();
    }
    return IdeLink::toHex(std::string((const char*)bytes, sizeof bytes));
}

Request readRequest(const std::string& buffer) {
    Request r;
    const std::size_t end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        r.state = buffer.size() > kMaxRequestBytes ? Request::Bad : Request::Incomplete;
        return r;
    }
    if (end > kMaxRequestBytes) {
        r.state = Request::Bad;
        return r;
    }
    r.consumed = end + 4;
    std::istringstream in(buffer.substr(0, end));
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos || line.compare(sp2 + 1, 5, "HTTP/") != 0) {
        r.state = Request::Bad;
        return r;
    }
    r.method = line.substr(0, sp1);
    r.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = r.target.find('?');
    r.path = r.target.substr(0, q);
    if (q != std::string::npos) {
        std::string rest = r.target.substr(q + 1);
        const std::size_t hash = rest.find('#');
        if (hash != std::string::npos) rest.resize(hash);
        std::size_t at = 0;
        while (at <= rest.size()) {
            std::size_t amp = rest.find('&', at);
            if (amp == std::string::npos) amp = rest.size();
            const std::string pair = rest.substr(at, amp - at);
            if (!pair.empty()) {
                const std::size_t eq = pair.find('=');
                const std::string key = percentDecode(pair.substr(0, eq));
                const std::string value =
                    eq == std::string::npos ? std::string() : percentDecode(pair.substr(eq + 1));
                if (!r.query.count(key)) r.query[key] = value;
            }
            at = amp + 1;
        }
    }
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) {
            r.state = Request::Bad;
            return r;
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (r.headers.count(name)) {
            // Two Hosts (or Origins) would let one be checked and the other
            // obeyed; others join as HTTP joins them.
            if (name == "host" || name == "origin") {
                r.state = Request::Bad;
                return r;
            }
            r.headers[name] += ", " + value;
        } else {
            r.headers[name] = value;
        }
    }
    r.state = Request::Complete;
    return r;
}

bool loopbackHost(const std::string& hostHeader) {
    std::string h = lower(hostHeader);
    if (h.empty()) return false;
    std::string name;
    std::string port;
    if (h[0] == '[') {
        const std::size_t close = h.find(']');
        if (close == std::string::npos) return false;
        name = h.substr(0, close + 1);
        const std::string rest = h.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return false;
            port = rest.substr(1);
        }
    } else {
        const std::size_t colon = h.find(':');
        name = h.substr(0, colon);
        if (colon != std::string::npos) port = h.substr(colon + 1);
    }
    if (name != "127.0.0.1" && name != "localhost" && name != "[::1]") return false;
    if (h.find(':') != std::string::npos && name != "[::1]" && port.empty()) return false;
    if (port.size() > 5) return false;
    for (char c : port)
        if (c < '0' || c > '9') return false;
    return true;
}

bool loopbackOrigin(const std::string& origin) {
    const std::string o = lower(origin);
    return o.compare(0, 7, "http://") == 0 && loopbackHost(o.substr(7));
}

Answer answer(const Request& r, const std::string& token, const std::string& page) {
    if (r.state != Request::Complete) return refuse(400, "Bad Request", "the request was not HTTP");
    if (r.method != "GET") return refuse(405, "Method Not Allowed", "only GET is served");
    auto header = [&](const char* name) -> const std::string* {
        auto it = r.headers.find(name);
        return it == r.headers.end() ? nullptr : &it->second;
    };
    const std::string* host = header("host");
    if (!host || !loopbackHost(*host))
        return refuse(403, "Forbidden", "the Host must be 127.0.0.1, localhost or [::1]");
    if (const std::string* origin = header("origin"))
        if (!loopbackOrigin(*origin))
            return refuse(403, "Forbidden", "pages from " + *origin + " are not served");
    auto t = r.query.find("t");
    if (t == r.query.end() || t->second.empty())
        return refuse(403, "Forbidden", "no token: open the address minicode printed");
    if (token.empty() || !IdeLink::sameSecret(t->second, token))
        return refuse(403, "Forbidden", "wrong token: open the address minicode printed");
    if (r.path == "/") {
        const std::string nonce = newToken().substr(0, 32);
        std::string body = page;
        for (std::size_t at = body.find("{{NONCE}}"); at != std::string::npos;
             at = body.find("{{NONCE}}", at + nonce.size()))
            body.replace(at, 9, nonce);
        Answer a;
        a.kind = Answer::Page;
        a.status = 200;
        a.response = httpResponse(
            200, "OK", "text/html; charset=utf-8", body,
            "Content-Security-Policy: default-src 'none'; script-src 'nonce-" + nonce +
                "'; style-src 'unsafe-inline'; img-src data:; connect-src 'self' "
                "ws://127.0.0.1:* ws://localhost:*; frame-src 'self'; "
                "frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
                "X-Frame-Options: DENY\r\n");
        return a;
    }
    if (r.path == "/ws") {
        const std::string* upgrade = header("upgrade");
        const std::string* connection = header("connection");
        const std::string* version = header("sec-websocket-version");
        const std::string* key = header("sec-websocket-key");
        std::string raw;
        if (!upgrade || lower(*upgrade) != "websocket" || !connection ||
            lower(*connection).find("upgrade") == std::string::npos || !version ||
            *version != "13" || !key || !IdeLink::fromBase64(*key, raw) || raw.size() != 16)
            return refuse(400, "Bad Request", "not a WebSocket upgrade");
        Answer a;
        a.kind = Answer::Socket;
        a.status = 101;
        a.response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                     IdeLink::acceptKey(*key) + "\r\n\r\n";
        return a;
    }
    return refuse(404, "Not Found", "nothing is served at " + r.path);
}

// ---------------------------------------------------------------- data

Json termLineJson(const std::vector<TermCell>& cells) {
    // The last cell worth drawing: text, or a background or inverse.
    std::size_t last = 0;
    for (std::size_t i = 0; i < cells.size(); i++) {
        const TermCell& c = cells[i];
        if (c.width == 0) continue;
        if ((c.ch != " " && !c.ch.empty()) || c.style.bg.kind != TermColor::Default ||
            c.style.inverse)
            last = i + 1;
    }
    Json runs = Json::array();
    std::string text;
    int64_t fg = -1, bg = -1;
    int flags = 0;
    bool open = false;
    auto close = [&]() {
        if (!open) return;
        Json run = Json::array();
        run.push(Json(text));
        if (fg != -1 || bg != -1 || flags) {
            run.push(Json(fg));
            run.push(Json(bg));
            run.push(Json(flags));
        }
        runs.push(std::move(run));
        text.clear();
        open = false;
    };
    for (std::size_t i = 0; i < last; i++) {
        const TermCell& c = cells[i];
        if (c.width == 0) continue;
        const int64_t f = termColor(c.style.fg), b = termColor(c.style.bg);
        const int fl = termFlags(c.style);
        if (open && (f != fg || b != bg || fl != flags)) close();
        if (!open) {
            fg = f;
            bg = b;
            flags = fl;
            open = true;
        }
        text += c.ch.empty() ? std::string(" ") : c.ch;
    }
    close();
    return runs;
}

Json paletteJson(const Settings& s) {
    Json p = Json::object();
    Json syntax = Json::array();
    for (int i = 0; i < kTokenStyleCount; i++) syntax.push(Json(hexColor(s.syntax((TokenStyle)i))));
    p.set("syntax", std::move(syntax));
    p.set("background", Json(hexColor(s.background(Surface::Editor))));
    p.set("text", Json(hexColor(s.text(Surface::Editor))));
    p.set("terminalBackground", Json(hexColor(s.background(Surface::Terminal))));
    p.set("terminalText", Json(hexColor(s.text(Surface::Terminal))));
    p.set("dark", Json(s.isDark()));
    return p;
}

Json paletteJson() { return paletteJson(Settings::parse("")); }

DocView::DocView(DocId id, std::string path) : id_(id), path_(std::move(path)) {
    ext_ = extensionOfPath(path_);
    for (const char* e : kMarkdownExtensions)
        if (ext_ == e) markdown_ = true;
    if (!markdown_) {
        hl_ = std::make_unique<IncrementalHighlighter<char16_t>>(ext_);
        StringSource<char16_t> src(text_);
        hl_->reset(src, nullptr);
    }
}

bool DocView::large() const {
    return text_.size() > kMaxUnits || (hl_ && hl_->lineCount() > kMaxLines);
}

std::string DocView::kind() const {
    if (large()) return "large";
    return markdown_ ? "markdown" : "code";
}

std::size_t DocView::lineCount() const { return hl_ ? hl_->lineCount() : 0; }

Json DocView::lineJson(std::size_t line) const {
    Json j = Json::array();
    if (!hl_ || line >= hl_->lineCount()) {
        j.push(Json(""));
        j.push(Json::array());
        return j;
    }
    const std::size_t start = hl_->lineStart(line);
    const std::size_t end = line + 1 < hl_->lineCount() ? hl_->lineStart(line + 1) : text_.size();
    const std::size_t len = end - start - breakUnits(text_, start, end);
    std::vector<Token> tokens;
    StringSource<char16_t> src(text_);
    hl_->lineTokens(src, line, line + 1, tokens);
    Json spans = Json::array();
    for (const Token& t : tokens) {
        if (t.style == TokenStyle::Plain || t.start + t.length <= start || t.start >= start + len)
            continue;
        const std::size_t a = std::max(t.start, start), b = std::min(t.start + t.length, start + len);
        Json s = Json::array();
        s.push(Json((int64_t)(a - start)));
        s.push(Json((int64_t)(b - a)));
        s.push(Json((int)t.style));
        spans.push(std::move(s));
    }
    j.push(Json(Model::utf8FromUtf16(text_.substr(start, len))));
    j.push(std::move(spans));
    return j;
}

void DocView::apply(const Ot::Ops& ops) {
    for (const Ot::Op& op : ops) {
        const std::size_t pos = std::min(op.pos, text_.size());
        const std::size_t del = std::min(op.del, text_.size() - pos);
        const bool wasLarge = large();
        const std::size_t oldLines = lineCount();
        text_.replace(pos, del, op.ins);
        changed_ = changed_ || markdown_;
        if (!hl_) continue;
        StringSource<char16_t> src(text_);
        std::vector<Token> scratch;
        const auto range = hl_->edit(src, pos, del, op.ins.size(), scratch);
        if (changed_ || wasLarge || large()) {
            changed_ = true;
            continue;
        }
        const long long grew = (long long)lineCount() - (long long)oldLines;
        const long long span = (long long)range.endLine - (long long)range.firstLine;
        const long long replaced = span - grew;
        if (replaced < 0 || range.endLine > lineCount()) {
            changed_ = true;   // not expected: the browser gets it whole
            continue;
        }
        Json e = Json::array();
        e.push(Json((int64_t)range.firstLine));
        e.push(Json((int64_t)replaced));
        Json lines = Json::array();
        for (std::size_t l = range.firstLine; l < range.endLine; l++) lines.push(lineJson(l));
        e.push(std::move(lines));
        edits_.push(std::move(e));
        // A long run of edits is sent cheaper whole.
        if (edits_.size() > 4000) changed_ = true;
    }
    if (changed_) edits_ = Json::array();
}

Json DocView::takeEdits() {
    if (changed_ || edits_.size() == 0) {
        edits_ = Json::array();
        return Json();
    }
    Json j = Json::object();
    j.set("t", Json("lines"));
    j.set("id", Json((int64_t)id_));
    j.set("version", Json((int64_t)version_));
    j.set("edits", std::move(edits_));
    edits_ = Json::array();
    return j;
}

Json DocView::snapshot() const {
    Json j = Json::object();
    j.set("t", Json("doc"));
    j.set("id", Json((int64_t)id_));
    j.set("path", Json(path_));
    const std::string k = kind();
    j.set("kind", Json(k));
    j.set("version", Json((int64_t)version_));
    if (k == "large") {
        j.set("note", Json(baseName(path_) + " is too large to show here yet (" +
                           std::to_string(text_.size()) + " characters)."));
    } else if (k == "markdown") {
        j.set("html", Json(MarkdownHtml::page(Model::utf8FromUtf16(text_), baseName(path_))));
    } else {
        Json lines = Json::array();
        for (std::size_t l = 0; l < lineCount(); l++) lines.push(lineJson(l));
        j.set("lines", std::move(lines));
    }
    return j;
}

TermView::TermView(TermId id, int cols, int rows)
    : id_(id), screen_(std::max(2, cols), std::max(1, rows)) {
    screen_.setScrollbackLimit(kHistoryLines * 2);
}

void TermView::feed(const std::string& bytes) { screen_.feed(bytes); }

void TermView::resize(int cols, int rows) {
    screen_ = TerminalScreen(std::max(2, cols), std::max(1, rows));
    screen_.setScrollbackLimit(kHistoryLines * 2);
    fresh_ = true;
}

Json TermView::cursorJson() const {
    Json c = Json::array();
    c.push(Json(screen_.cursorRow()));
    c.push(Json(screen_.cursorCol()));
    c.push(Json(screen_.cursorVisible()));
    return c;
}

Json TermView::snapshot() {
    screen_.takeDirty();
    fresh_ = false;
    Json j = Json::object();
    j.set("t", Json("term"));
    j.set("id", Json((int64_t)id_));
    j.set("cols", Json(screen_.cols()));
    j.set("rows", Json(screen_.rows()));
    j.set("base", Json((int64_t)screen_.historyBase()));
    std::vector<TermCell> scratch;
    Json hist = Json::array();
    const int n = std::min(screen_.historySize(), kHistoryLines);
    for (int l = -n; l < 0; l++) hist.push(termLineJson(screen_.lineCells(l, scratch)));
    j.set("hist", std::move(hist));
    Json rows = Json::array();
    for (int r = 0; r < screen_.rows(); r++) rows.push(termLineJson(screen_.row(r)));
    j.set("screen", std::move(rows));
    j.set("cursor", cursorJson());
    j.set("alt", Json(screen_.altScreen()));
    j.set("title", Json(title_));
    sentBase_ = screen_.historyBase();
    cursorRow_ = screen_.cursorRow();
    cursorCol_ = screen_.cursorCol();
    cursorShown_ = screen_.cursorVisible();
    alt_ = screen_.altScreen();
    sentTitle_ = title_;
    return j;
}

Json TermView::takeUpdate() {
    const TerminalScreen::Dirty d = screen_.takeDirty();
    const long long scrolled = screen_.historyBase() - sentBase_;
    if (fresh_ || d.all || scrolled < 0 || scrolled > kHistoryLines) return snapshot();
    const bool cursorMoved = screen_.cursorRow() != cursorRow_ ||
                             screen_.cursorCol() != cursorCol_ ||
                             screen_.cursorVisible() != cursorShown_;
    bool anyRow = scrolled > 0;
    for (std::size_t r = 0; !anyRow && r < d.rows.size(); r++) anyRow = d.rows[r] != 0;
    if (!anyRow && !cursorMoved && screen_.altScreen() == alt_ && title_ == sentTitle_)
        return Json();
    Json j = Json::object();
    j.set("t", Json("termd"));
    j.set("id", Json((int64_t)id_));
    std::vector<TermCell> scratch;
    Json hist = Json::array();
    const int n = (int)std::min<long long>(scrolled, screen_.historySize());
    for (int l = -n; l < 0; l++) hist.push(termLineJson(screen_.lineCells(l, scratch)));
    j.set("hist", std::move(hist));
    Json rows = Json::array();
    for (int r = 0; r < screen_.rows(); r++) {
        // Every row moved up when lines went into the scrollback.
        if (scrolled == 0 && ((std::size_t)r >= d.rows.size() || !d.rows[r])) continue;
        Json row = Json::array();
        row.push(Json(r));
        row.push(termLineJson(screen_.row(r)));
        rows.push(std::move(row));
    }
    j.set("rows", std::move(rows));
    j.set("cursor", cursorJson());
    j.set("alt", Json(screen_.altScreen()));
    j.set("title", Json(title_));
    sentBase_ = screen_.historyBase();
    cursorRow_ = screen_.cursorRow();
    cursorCol_ = screen_.cursorCol();
    cursorShown_ = screen_.cursorVisible();
    alt_ = screen_.altScreen();
    sentTitle_ = title_;
    return j;
}

// ---------------------------------------------------------------- the server

struct Server::Conn {
    int fd = -1;
    bool socket = false;          // upgraded to a WebSocket
    bool closeAfterWrite = false;
    bool dead = false;
    std::string in, out;
    IdeLink::FrameReader frames{1 << 20};
    DocId doc = 0;
    TermId term = 0;
    bool welcomed = false;
};

struct Server::Session : Client::Listener, Client::TermListener {
    std::unique_ptr<HostLink> link;
    std::unique_ptr<Client> client;
    std::map<DocId, std::unique_ptr<DocView>> docs;
    std::map<TermId, std::unique_ptr<TermView>> terms;
    std::set<DocId> gone;
    std::map<TermId, TermShell> shells;
    Json state;                     // the host's, last looked at
    std::string sentState;          // what the browsers were last sent
    std::atomic<bool> closed{false};
    bool pending = false;           // something came that is not sent yet
    Clock::time_point lastLook, lastTermFlush, lastMarkdown;

    void hostEdited(DocId d, const Ot::Ops& ops, int, const Selection*) override {
        auto it = docs.find(d);
        if (it == docs.end()) return;
        it->second->apply(ops);
        pending = true;
    }
    void hostGone(DocId d) override {
        docs.erase(d);
        gone.insert(d);
        pending = true;
    }
    void termOutput(TermId t, const std::string& bytes, long long, bool, bool replay) override {
        auto it = terms.find(t);
        if (it != terms.end()) {
            if (replay) {
                // A fresh copy of the host's screen: start over at its size.
                const TerminalScreen& s = it->second->screen();
                it->second->resize(s.cols(), s.rows());
            }
            it->second->feed(bytes);
        }
        if (!replay) client->ackTerminal(t, bytes.size());
        pending = true;
    }
    void termShell(TermId t, const TermShell& s) override {
        shells[t] = s;
        auto it = terms.find(t);
        if (it != terms.end()) it->second->setTitle(s.title);
        pending = true;
    }
};

Server::Server() {
    if (pipe(wake_) == 0)
        for (int fd : wake_) setNonBlocking(fd);
}

Server::~Server() {
    detach();
    for (auto& c : conns_)
        if (c->fd >= 0) ::close(c->fd);
    if (listen_ >= 0) ::close(listen_);
    for (int fd : wake_)
        if (fd >= 0) ::close(fd);
}

bool Server::start(const Options& o, std::string* error) {
    opt_ = o;
    if (opt_.token.empty()) opt_.token = newToken();
    if (opt_.machine.empty()) {
        char name[256] = {0};
        if (gethostname(name, sizeof name - 1) == 0) opt_.machine = name;
        const std::size_t dot = opt_.machine.find('.');
        if (dot != std::string::npos && dot > 0) opt_.machine.resize(dot);
    }
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_ < 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)o.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never another interface
    if (bind(listen_, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(listen_, 16) != 0) {
        if (error)
            *error = "127.0.0.1:" + std::to_string(o.port) + ": " + std::strerror(errno);
        ::close(listen_);
        listen_ = -1;
        return false;
    }
    socklen_t len = sizeof addr;
    getsockname(listen_, (sockaddr*)&addr, &len);
    port_ = ntohs(addr.sin_port);
    setNonBlocking(listen_);
    // The host machine's theme when its settings file says one.
    Settings settings = Settings::parse("");
    std::string path;
    if (const char* s = std::getenv("MINICODE_SETTINGS")) path = s;
    else if (const char* home = std::getenv("HOME")) path = std::string(home) + "/.config/minicode/settings.conf";
    if (!path.empty()) {
        std::ifstream f(path);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            settings = Settings::parse(ss.str());
        }
    }
    Json hello = Json::object();
    hello.set("t", Json("hello"));
    hello.set("machine", Json(opt_.machine));
    hello.set("folder", Json(opt_.folder));
    hello.set("palette", paletteJson(settings));
    helloText_ = hello.dump();
    return true;
}

std::string Server::url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/?t=" + opt_.token;
}

void Server::wake() {
    const char b = 1;
    while (write(wake_[1], &b, 1) < 0 && errno == EINTR) {}
}

void Server::stop() {
    stop_ = true;
    wake();
}

void Server::acceptAll() {
    for (;;) {
        const int fd = ::accept(listen_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        setNonBlocking(fd);
        auto c = std::make_unique<Conn>();
        c->fd = fd;
        conns_.push_back(std::move(c));
    }
}

void Server::sendText(Conn& c, const std::string& text) {
    if (c.dead || !c.socket || c.closeAfterWrite) return;
    c.out += IdeLink::encodeFrame(IdeLink::Opcode::Text, text);
    // A browser that reads nothing for this long is dropped.
    if (c.out.size() > (std::size_t(256) << 20)) c.dead = true;
}

void Server::sendJson(Conn& c, const Json& j) { sendText(c, j.dump()); }

void Server::readFrom(Conn& c) {
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(c.fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) {
            c.dead = true;
            return;
        }
        if (c.socket) c.frames.feed(buf, (std::size_t)n);
        else c.in.append(buf, (std::size_t)n);
    }
    if (!c.socket) handleHttp(c);
    if (!c.socket || c.dead) return;
    IdeLink::Message m;
    for (;;) {
        const auto r = c.frames.next(m);
        if (r == IdeLink::FrameReader::NeedMore) break;
        if (r == IdeLink::FrameReader::Error) {
            c.out += IdeLink::encodeClose(c.frames.errorCode(), c.frames.error());
            c.closeAfterWrite = true;
            break;
        }
        if (m.op == IdeLink::Opcode::Text) {
            handleMessage(c, m.payload);
        } else if (m.op == IdeLink::Opcode::Ping) {
            c.out += IdeLink::encodeFrame(IdeLink::Opcode::Pong, m.payload);
        } else if (m.op == IdeLink::Opcode::Close) {
            c.out += IdeLink::encodeClose(1000);
            c.closeAfterWrite = true;
            break;
        }
    }
}

void Server::handleHttp(Conn& c) {
    const Request r = readRequest(c.in);
    if (r.state == Request::Incomplete) return;
    const Answer a = answer(r, opt_.token, pageHtml());
    c.out += a.response;
    if (a.kind != Answer::Socket) {
        c.closeAfterWrite = true;
        c.in.clear();
        return;
    }
    const std::string rest = c.in.substr(r.consumed);
    c.in.clear();
    c.socket = true;
    c.frames.feed(rest);
    browsers_++;
    if (!session_ && !attach()) {
        Json e = Json::object();
        e.set("t", Json("ended"));
        e.set("why", Json("No host serves " + opt_.folder + " now."));
        sendJson(c, e);
        c.closeAfterWrite = true;
        return;
    }
    sendText(c, helloText_);
    if (!session_->sentState.empty()) sendText(c, session_->sentState);
    c.welcomed = true;
}

void Server::handleMessage(Conn& c, const std::string& text) {
    Json j;
    if (!Json::parse(text, j) || !j.isObject()) return;
    // The only thing a browser says: what it looks at.
    if (j["t"].asString() != "show") return;
    c.doc = (DocId)std::max<int64_t>(0, j["doc"].asInt());
    c.term = (TermId)std::max<int64_t>(0, j["term"].asInt());
    showTo(c);
}

bool Server::attach() {
    std::string error;
    auto link = HostLink::open(opt_.hostSocket, &error);
    if (!link) return false;
    auto s = std::make_unique<Session>();
    s->client = std::make_unique<Client>(Model::Unit::Utf16, "browser");
    s->client->setReadOnly(true);
    s->client->setWake([this] { wake(); });
    Client* client = s->client.get();
    Session* raw = s.get();
    link->start([client](Message&& m) { client->deliver(std::move(m)); },
                [this, raw] {
                    raw->closed = true;
                    wake();
                });
    HostLink* l = link.get();
    client->connect([l](Message&& m) { l->send(m); });
    s->link = std::move(link);
    session_ = std::move(s);
    lookAtState();
    if (!session_ || !session_->state.isObject() ||
        session_->state["folder"].asString() != opt_.folder) {
        detach();
        return false;
    }
    return true;
}

void Server::detach() {
    if (!session_) return;
    if (session_->link) session_->link->close();   // the reader first: it delivers to the client
    session_->client.reset();
    session_.reset();
}

void Server::lookAtState() {
    Session& s = *session_;
    s.lastLook = Clock::now();
    const Json st = s.client->sessionState();
    if (!st.isObject()) return;
    s.state = st;
    Json out = Json::object();
    out.set("t", Json("state"));
    Json windows = Json::array();
    for (const Json& w : st["windows"].elements()) windows.push(w["tabs"]);
    out.set("windows", std::move(windows));
    Json docs = Json::array();
    for (const Json& d : st["documents"].elements()) {
        Json o = Json::object();
        o.set("id", d["doc"]);
        o.set("path", d["path"]);
        o.set("dirty", d["dirty"]);
        docs.push(std::move(o));
    }
    out.set("docs", std::move(docs));
    Json terms = Json::array();
    for (const Json& t : st["terminals"].elements()) {
        const TermId id = (TermId)t["term"].asInt();
        const TermShell sh = termShellFrom(t["shell"]);
        const int cols = (int)t["cols"].asInt(80), rows = (int)t["rows"].asInt(24);
        Json o = Json::object();
        o.set("id", Json((int64_t)id));
        o.set("cols", Json(cols));
        o.set("rows", Json(rows));
        o.set("title", Json(sh.title));
        o.set("cwd", Json(sh.cwd));
        o.set("busy", Json(sh.busy));
        o.set("command", Json(sh.command));
        terms.push(std::move(o));
        // Resized by a renderer that edits: a fresh replay at the new size.
        auto it = s.terms.find(id);
        if (it != s.terms.end() &&
            (it->second->screen().cols() != cols || it->second->screen().rows() != rows)) {
            it->second->resize(cols, rows);
            s.client->attachTerminal(id, &s);
        }
    }
    out.set("terms", std::move(terms));
    std::string text = out.dump();
    if (text == s.sentState) return;
    s.sentState = std::move(text);
    for (auto& c : conns_)
        if (c->welcomed) sendText(*c, s.sentState);
}

void Server::flush() {
    if (!session_) return;
    Session& s = *session_;
    s.pending = false;
    for (DocId id : s.gone) {
        Json g = Json::object();
        g.set("t", Json("gone"));
        g.set("id", Json((int64_t)id));
        for (auto& c : conns_)
            if (c->welcomed && c->doc == id) sendJson(*c, g);
    }
    s.gone.clear();
    const bool markdownDue = msSince(s.lastMarkdown) >= 250;
    for (auto& [id, v] : s.docs) {
        v->setVersion(s.client->version(id));
        bool anyone = false;
        for (auto& c : conns_) anyone = anyone || (c->welcomed && c->doc == id);
        if (v->changed()) {
            if (!anyone) {
                v->clearChanged();
                continue;
            }
            if (v->kind() == "markdown" && !markdownDue) {
                s.pending = true;   // a page at most four times a second
                continue;
            }
            if (v->kind() == "markdown") s.lastMarkdown = Clock::now();
            const std::string text = v->snapshot().dump();
            v->clearChanged();
            v->takeEdits();
            for (auto& c : conns_)
                if (c->welcomed && c->doc == id) sendText(*c, text);
            continue;
        }
        const Json e = v->takeEdits();
        if (e.isNull() || !anyone) continue;
        const std::string text = e.dump();
        for (auto& c : conns_)
            if (c->welcomed && c->doc == id) sendText(*c, text);
    }
    s.lastTermFlush = Clock::now();
    for (auto& [id, v] : s.terms) {
        const Json u = v->takeUpdate();
        if (u.isNull()) continue;
        const std::string text = u.dump();
        for (auto& c : conns_)
            if (c->welcomed && c->term == id) sendText(*c, text);
    }
}

void Server::showTo(Conn& c) {
    if (!session_ || !c.welcomed) return;
    Session& s = *session_;
    // Everyone else brought up to now first, so the views' "since the last
    // send" starts here for the newcomer too.
    s.lastMarkdown = Clock::time_point();
    flush();
    if (c.doc) {
        auto it = s.docs.find(c.doc);
        if (it == s.docs.end()) {
            std::string path;
            for (const Json& d : s.state["documents"].elements())
                if ((DocId)d["doc"].asInt() == c.doc) path = d["path"].asString();
            s.docs[c.doc] = std::make_unique<DocView>(c.doc, path);
            s.client->view(c.doc, &s);
            s.client->settle();   // its text arrives as one insertion
            it = s.docs.find(c.doc);
            if (it == s.docs.end()) {
                s.gone.erase(c.doc);   // DocGone: said below, to this browser
            } else {
                it->second->clearChanged();
                it->second->takeEdits();
            }
        }
        if (it == s.docs.end()) {
            Json g = Json::object();
            g.set("t", Json("gone"));
            g.set("id", Json((int64_t)c.doc));
            sendJson(c, g);
            c.doc = 0;
        } else {
            it->second->setVersion(s.client->version(c.doc));
            sendJson(c, it->second->snapshot());
        }
    }
    if (c.term) {
        auto it = s.terms.find(c.term);
        if (it == s.terms.end()) {
            int cols = 0, rows = 0;
            for (const Json& t : s.state["terminals"].elements())
                if ((TermId)t["term"].asInt() == c.term) {
                    cols = (int)t["cols"].asInt(80);
                    rows = (int)t["rows"].asInt(24);
                }
            if (cols > 0) {
                s.terms[c.term] = std::make_unique<TermView>(c.term, cols, rows);
                auto sh = s.shells.find(c.term);
                if (sh != s.shells.end()) s.terms[c.term]->setTitle(sh->second.title);
                s.client->attachTerminal(c.term, &s);
                s.client->settle();   // the replay comes before the answer to it
                it = s.terms.find(c.term);
                // Everyone else (nobody yet) has had nothing of it.
            }
        }
        if (it != s.terms.end()) sendJson(c, it->second->snapshot());
        else c.term = 0;
        // snapshot() reset what "since" means for the other browsers on this
        // terminal: they had everything up to the flush above, and the
        // snapshot added nothing after it.
    }
}

void Server::writeTo(Conn& c) {
    while (!c.out.empty()) {
        const ssize_t n = sendSome(c.fd, c.out.data(), c.out.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) {
            c.dead = true;
            return;
        }
        c.out.erase(0, (std::size_t)n);
    }
    if (c.closeAfterWrite) c.dead = true;
}

bool Server::run() {
    struct stat st;
    Clock::time_point lastSocketLook = Clock::now();
    std::vector<pollfd> polls;
    for (;;) {
        if (stop_) return false;
        int timeout = 1000;
        if (session_) {
            const long long toLook = opt_.pollMs - msSince(session_->lastLook);
            timeout = (int)std::max(0LL, std::min<long long>(timeout, toLook));
            if (session_->pending) timeout = std::min(timeout, 30);
        }
        polls.clear();
        polls.push_back(pollfd{wake_[0], POLLIN, 0});
        polls.push_back(pollfd{listen_, POLLIN, 0});
        for (auto& c : conns_)
            polls.push_back(pollfd{c->fd, (short)(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
        int n;
        do n = poll(polls.data(), (nfds_t)polls.size(), timeout);
        while (n < 0 && errno == EINTR);
        char buf[64];
        while (read(wake_[0], buf, sizeof buf) > 0) {}
        if (stop_) return false;
        if (n > 0) {
            if (polls[1].revents) acceptAll();
            for (std::size_t i = 2; i < polls.size(); i++) {
                Conn& c = *conns_[i - 2];
                if (polls[i].revents & (POLLIN | POLLHUP | POLLERR)) readFrom(c);
            }
        }
        if (session_) {
            session_->client->dispatch();
            if (session_->closed) {
                Json e = Json::object();
                e.set("t", Json("ended"));
                e.set("why", Json("The host for " + opt_.folder + " stopped."));
                for (auto& c : conns_) {
                    sendJson(*c, e);
                    c->closeAfterWrite = true;
                    writeTo(*c);
                }
                detach();
                return true;
            }
            if (msSince(session_->lastLook) >= opt_.pollMs) lookAtState();
            if (session_->pending && msSince(session_->lastTermFlush) >= 30) flush();
        } else if (msSince(lastSocketLook) >= 1000) {
            // Nobody looking: the host's socket going means it ended.
            lastSocketLook = Clock::now();
            if (lstat(opt_.hostSocket.c_str(), &st) != 0) return true;
        }
        for (auto& c : conns_)
            if (!c->out.empty() && !c->dead) writeTo(*c);
        // Connections gone; the session goes with the last browser.
        bool lostBrowser = false;
        for (auto it = conns_.begin(); it != conns_.end();) {
            if ((*it)->dead) {
                if ((*it)->socket) {
                    browsers_--;
                    lostBrowser = true;
                }
                ::close((*it)->fd);
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
        if (lostBrowser && browsers_ == 0) detach();
    }
}

}  // namespace Web
}  // namespace Shared
