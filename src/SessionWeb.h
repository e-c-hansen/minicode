// SessionWeb.h — a browser's read-only view of a session's host
// (SESSION-DESIGN.md, "Browser screen", stage 6b). `minicode host --folder
// DIR --web PORT`, or `minicode attach --folder DIR --web PORT` for a host
// already running, serves one page on 127.0.0.1:PORT, never another
// interface. The page's script opens a WebSocket back to the same address
// and draws what the host describes: the session's tabs, the document on
// show as lines with the core highlighter's spans, a Markdown document as
// MarkdownHtml's page, and each terminal's screen and scrollback as rows of
// styled runs from a headless TerminalScreen. Nothing is drawn from pixels,
// and the browser can change nothing: its only message says which document
// and terminal it is looking at.
//
// Access: a random token of 48 hex digits, made at start and printed once
// with the address to open (http://127.0.0.1:PORT/?t=TOKEN), must come in
// the query of the page's address and of the socket's. The Host header must
// name the loopback (127.0.0.1, localhost or [::1], any port, so `ssh -L`
// to another local port still works), and an Origin header, when there is
// one, the same: a page elsewhere that rebinds a name to 127.0.0.1 is turned
// away. Anything else gets a refusal and nothing more. The page carries a
// Content-Security-Policy with a per-response nonce for its one script.
//
// The server is a renderer of the host (SessionClient.h) that says it only
// reads (Client::setReadOnly): the host refuses anything from it that would
// change the session, and its views of documents keep none open
// (Client::view). It attaches over the host's Unix socket while at least one
// browser is connected, and detaches when the last goes, so a host serving
// nobody still ends when its lifetime says.
//
// What goes to the browser, one JSON object per text frame ("t" names it):
//   hello   {machine, folder, palette, termNote}: once, first
//   state   {windows, docs, terms}: whenever the session's state changes
//           (looked at every half second)
//   doc     {id, path, kind, version, lines | html | note}: the document the
//           browser shows, whole: kind "code" (lines, each [text, spans],
//           spans [[start, length, style], ...] in UTF-16 units, styles by
//           TokenStyle), "markdown" (html, MarkdownHtml's page) or "large"
//           (note: too large to show here yet)
//   lines   {id, version, edits: [[at, del, lines], ...]}: the document's
//           lines changed, each edit a splice of the line list
//   gone    {id}: the document closed
//   term    {id, cols, rows, base, hist, screen, cursor, alt, title}: a
//           terminal whole: `hist` the newest scrollback lines (oldest
//           first), `screen` its rows, each line [[text, fg, bg, flags],
//           ...] (colors 0xRRGGBB or -1, flags TermFlag), `cursor` [row, col,
//           shown]
//   termd   {id, hist, rows: [[row, line], ...], cursor, alt, title}: lines
//           that went into the scrollback since, and rows that changed
//   ended   {why}: the host went
// and from the browser, only:
//   show    {doc, term}: what it is looking at (0 for nothing)
//
// Pure C++17 and POSIX: the server's own thread polls the listening socket,
// the browsers' connections and a pipe the host's messages wake it through.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Json.h"
#include "SessionClient.h"
#include "SyntaxHighlighter.h"
#include "TerminalScreen.h"

class Settings;

namespace Shared {

namespace Web {

// ---- access
// A fresh token: 48 hex digits from the system's random source.
std::string newToken();

// An HTTP request as far as it has come.
struct Request {
    enum State { Incomplete, Complete, Bad };
    State state = Incomplete;
    std::string method, target, path;            // path: the target before '?'
    std::map<std::string, std::string> query;    // decoded
    std::map<std::string, std::string> headers;  // names in lower case
    std::size_t consumed = 0;                    // bytes of the buffer it took
};
constexpr std::size_t kMaxRequestBytes = 16384;
Request readRequest(const std::string& buffer);

// A Host header naming the loopback: 127.0.0.1, localhost or [::1], with or
// without a port.
bool loopbackHost(const std::string& host);
// An Origin of http:// and a loopback host.
bool loopbackOrigin(const std::string& origin);

// What to do with a request: serve the page, take the socket (then
// `response` is the 101 answer), or refuse (then `response` is the refusal,
// and `why` says what was wrong, for logs and checks).
struct Answer {
    enum Kind { Page, Socket, Refused };
    Kind kind = Refused;
    int status = 403;
    std::string response, why;
};
// `page` is the HTML to serve; "{{NONCE}}" in it becomes the response's
// script nonce.
Answer answer(const Request& r, const std::string& token, const std::string& page);

// The page itself (SessionWebPage.cpp): one HTML file with its CSS and
// script, no third-party code.
const std::string& pageHtml();

// ---- what the browser draws, as data
// Terminal cell flags in a run.
enum TermFlag {
    kBold = 1, kDim = 2, kItalic = 4, kUnderline = 8, kInverse = 16, kStrike = 32,
    kHidden = 64,
};
// A terminal line as runs [[text, fg, bg, flags], ...], trailing blanks
// dropped; a run of default style is [text].
Json termLineJson(const std::vector<TermCell>& cells);
// The syntax colors (the default theme's, Settings.h) by TokenStyle, and the
// editor's and terminal's background and text, as "#rrggbb".
Json paletteJson(const Settings& s);
Json paletteJson();   // the default theme's

// A document as the browser sees it: the host's text kept from its edits,
// with an incremental highlighter over it.
class DocView {
public:
    // Past these the browser is told the document is too large for it yet.
    static constexpr std::size_t kMaxUnits = 8u << 20;
    static constexpr std::size_t kMaxLines = 200000;

    DocView(DocId id, std::string path);
    DocId id() const { return id_; }
    const std::string& path() const { return path_; }
    // "code", "markdown" or "large".
    std::string kind() const;
    // The host's edit (its UTF-16 operations, in order). For a code
    // document the lines it changed are kept as edits for takeEdits().
    void apply(const Ot::Ops& ops);
    std::uint64_t version() const { return version_; }
    void setVersion(std::uint64_t v) { version_ = v; }
    const std::u16string& text() const { return text_; }
    // The whole document for the browser ("doc").
    Json snapshot() const;
    // Line edits since the last call ("lines"), null when none, or when the
    // document is not code (then snapshot() again: `changed()` says so).
    Json takeEdits();
    bool changed() const { return changed_; }
    void clearChanged() { changed_ = false; }
    // A line as the browser gets it: [text, spans].
    Json lineJson(std::size_t line) const;
    std::size_t lineCount() const;

private:
    DocId id_;
    std::string path_, ext_;
    bool markdown_ = false;
    std::u16string text_;
    std::unique_ptr<IncrementalHighlighter<char16_t>> hl_;
    std::uint64_t version_ = 0;
    Json edits_ = Json::array();
    bool changed_ = false;
    bool large() const;
};

// A terminal as the browser sees it: a screen fed what the host forwards.
class TermView {
public:
    // Scrollback lines sent with a whole terminal, and kept by the page.
    static constexpr int kHistoryLines = 2000;

    TermView(TermId id, int cols, int rows);
    TermId id() const { return id_; }
    void feed(const std::string& bytes);
    void resize(int cols, int rows);
    void setTitle(std::string t) { title_ = std::move(t); }
    TerminalScreen& screen() { return screen_; }
    // The whole terminal ("term"), and what changed since the last of
    // either ("termd"), null when nothing did.
    Json snapshot();
    Json takeUpdate();

private:
    TermId id_;
    TerminalScreen screen_;
    std::string title_;
    long long sentBase_ = 0;
    int cursorRow_ = -1, cursorCol_ = -1;
    bool cursorShown_ = false, alt_ = false;
    bool fresh_ = true;   // a new screen: the next update is the whole terminal
    std::string sentTitle_;
    Json cursorJson() const;
};

// ---- the server
struct Options {
    std::string hostSocket;   // the host's Unix socket
    // Or a host in this process (the Mac's, editor.session-host): connects
    // the view's client to it (its Hello sent through it, the host's
    // messages for it delivered to Client::deliver from then on) and
    // returns what lets it go again. Used instead of hostSocket; the host
    // then serves no one folder, so its state's folder is not checked.
    std::function<std::function<void()>(Client& client)> connect;
    std::string folder;       // resolved: the host must serve it
    // Only this window's tabs and their documents (0: every window's and
    // every document).
    std::uint64_t window = 0;
    // A muted line where the terminals go while there are none ("" for
    // none).
    std::string termNote;
    std::string machine;      // the header's name ("" for this machine's)
    int port = 0;             // 0: any free port (checks)
    std::string token;        // "" for a fresh one
    int pollMs = 500;         // how often the session's state is looked at
};

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Listens on 127.0.0.1: false, with why, when it cannot.
    bool start(const Options& o, std::string* error);
    int port() const { return port_; }
    const std::string& token() const { return opt_.token; }
    // What to open: http://127.0.0.1:PORT/?t=TOKEN.
    std::string url() const;
    // Serves until stop() (false) or the host goes (true).
    bool run();
    // Any thread.
    void stop();
    // Browsers connected over the socket now (any thread).
    int browsers() const { return browsers_.load(); }

private:
    struct Conn;
    struct Session;
    Options opt_;
    int listen_ = -1, port_ = 0;
    int wake_[2] = {-1, -1};
    std::atomic<bool> stop_{false};
    std::atomic<int> browsers_{0};
    std::vector<std::unique_ptr<Conn>> conns_;
    std::unique_ptr<Session> session_;
    std::string helloText_;
    void wake();
    void acceptAll();
    void readFrom(Conn& c);
    void writeTo(Conn& c);
    void handleHttp(Conn& c);
    void handleMessage(Conn& c, const std::string& text);
    void sendJson(Conn& c, const Json& j);
    void sendText(Conn& c, const std::string& text);
    bool attach();
    void detach();
    void lookAtState();
    void flush();
    void showTo(Conn& c);
};

}  // namespace Web
}  // namespace Shared
