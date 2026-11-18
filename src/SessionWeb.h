// SessionWeb.h — a browser's view of a session's host (SESSION-DESIGN.md,
// "Browser screen": stage 6b, which reads, and 8b, which edits). `minicode host --folder
// DIR --web PORT`, or `minicode attach --folder DIR --web PORT` for a host
// already running, serves one page on 127.0.0.1:PORT, never another
// interface. The page's script opens a WebSocket back to the same address
// and draws what the host describes: the session's tabs, the folder's
// tree (read by the host one folder at a time, SessionFiles.h), the
// document on show as lines with the core highlighter's spans, a Markdown
// document as MarkdownHtml's page, and each terminal's screen and
// scrollback as rows of styled runs from a headless TerminalScreen. Nothing
// is drawn from pixels. Read-only (the default), the browser can change
// nothing: it says which document and terminal it is looking at, lists
// folders, and asks the window to open a file of the folder (a tab opened
// changes no text). Editable
// (Options::editable), it also types into the document on show and its
// terminal, undoes through the host's history, asks for a save, and asks
// the window's browser pane to open an http or https address. Either way
// the page lists the window's browser tabs and shows the one picked in a
// frame.
//
// Access: a random token of 48 hex digits, made at start and printed once
// with the address to open (http://127.0.0.1:PORT/?t=TOKEN), must come in
// the query of the page's address and of the socket's. The Host header must
// name the loopback (127.0.0.1, localhost or [::1], any port, so `ssh -L`
// to another local port still works), and an Origin header, when there is
// one, the page's own origin (http:// and that Host): a page elsewhere,
// even one that rebinds a name to 127.0.0.1, is turned away, and the socket
// opens only with an Origin, which every browser sends. Anything else gets
// a refusal and nothing more. The page carries a Content-Security-Policy
// with a per-response nonce for its one script.
//
// The server is a renderer of the host (SessionClient.h) that says it only
// reads (Client::setReadOnly), or, editable, that it is a guest
// (Client::setGuest): the host refuses anything from it that would change
// the session beyond a guest's edits, undo and terminal input, and its views
// of documents keep none open (Client::view).
//
// Editing: the server is the pages' sequencer, as the host is its own
// renderers'. Each document's view counts a version for every change of its
// text, whoever made it, and keeps a log of them. A page edits its own lines
// at once and sends its edit with the version it had seen (`base`); the
// server moves it past what came since (Ot::transform, its own changes
// winning ties), applies it, records it with the host as one undo step, and
// tells every page: the sender by `seq`, the others with the operations. A
// page keeps one edit in flight and moves its waiting ones past what others
// did, as SessionClient does. Return, Tab, Shift+Tab and Backspace carry a
// command: the server makes what AutoIndent says instead of what the page
// guessed and gives the page `fix`, the change from its guess to that. It attaches over the host's Unix socket while at least one
// browser is connected, and detaches when the last goes, so a host serving
// nobody still ends when its lifetime says.
//
// What goes to the browser, one JSON object per text frame ("t" names it):
//   hello   {machine, folder, palette, termNote, editable, canSave,
//           canBrowse}: once, first
//   palette {palette}: the host's settings file changed (paletteJson)
//   state   {windows, docs, terms}: whenever the session's state changes
//           (looked at every half second, or at once on lookNow()); with
//           Options::window, that window's tabs, documents and terminals;
//           a window with a browser pane
//           has `browser` {tabs: [{url, title, web, loopback}], active}
//   doc     {id, path, kind, version, editable, indent, lines, html | note}:
//           the document the browser shows, whole: kind "code" (lines, each
//           [text, spans] or [text, spans, break], spans [[start, length,
//           style], ...] in UTF-16 units, styles by TokenStyle, break 0 for
//           none (the last line), 2 "\r\n", 3 "\r", 4 U+2029, left out for
//           "\n"), "markdown" (lines, and html, MarkdownHtml's page) or
//           "large" (note: too large to show here yet); indent {tabs, width}
//   lines   {id, version, edits: [[at, del, lines], ...] | all: lines, ops,
//           seq, fix, sel}: one version of the document: the line list's
//           splices (or every line), and the change itself, `ops` [[pos,
//           del, insert], ...] in UTF-16, to every page but the one that made
//           it, which gets its `seq` and `fix` instead; `sel` [anchor, caret]
//           to the page an undo or a command's selection is for
//   md      {id, html}: a Markdown document's page again (at most 4 a second)
//   refused {id, seq, why}: an edit not made (the page drops what it has
//           not had confirmed; a `doc` follows when it fell out of step)
//   note    {text}: something to say (nothing to undo, a save not possible)
//   gone    {id}: the document closed
//   term    {id, cols, rows, base, hist, screen, cursor, alt, title}: a
//           terminal whole: `hist` the newest scrollback lines (oldest
//           first), `screen` its rows, each line [[text, fg, bg, flags],
//           ...] (colors 0xRRGGBB or -1, flags TermFlag), `cursor` [row, col,
//           shown]
//   termd   {id, hist, rows: [[row, line], ...], cursor, alt, title}: lines
//           that went into the scrollback since, and rows that changed
//   ended   {why}: the host went
//   tree    {path, root, entries, more | error}: one folder's listing
//           (SessionFiles.h's "list" answer), for a `list`
//   opening {path, abs, ok, why}: a file a page clicked is opening in the
//           window (`abs` its path as the tabs spell it), or why it is not
//   browsing {url, ok, why}: an address a page asked the window's browser
//           pane to open (`browse`), opening or refused
// and from the browser:
//   show    {doc, term}: what it is looking at (0 for nothing)
//   list    {path}: one folder of the tree ("" the session's folder), read
//           by the host (FsRead), each time a folder is opened or refreshed
//   open    {path, keep}: a file the tree's row names, opened as a tab in
//           the window (Options::open), read-only pages included
// and, only when editable:
//   edit    {doc, base, seq, ops, step {name, coalesce, fresh}, before, after,
//           cmd, at}: an edit of the document on show; cmd "newline", "tab",
//           "backtab" or "backspace" with `at` the selection it acted on
//   undo    {doc, redo}
//   save    {doc}: Options::save (the app that owns the disk), else a note
//   tkey    {term, key, mods}, ttext {term, text, mods}, tpaste {term, text}:
//           keys for the terminal on show, encoded as its screen's modes ask
//           (TerminalScreen::encodeKey, encodeChar, encodePaste)
//   browse  {url}: open an http or https address in the window's browser
//           pane (Options::browse; webAddress refuses every other scheme)
//
// The window's browser pane: a window that has one adds its tabs to the
// tabs it reports (Client::windowTabs) as "browser" (browserReportJson),
// and each window in `state` carries them as browserViewJson gives them,
// so the page lists them beside the editor's tabs and frames the one a
// person picks. A headless host has no browser pane: no window says
// "browser", and the page shows no such group.
//
// Pure C++17 and POSIX: the server's own thread polls the listening socket,
// the browsers' connections and a pipe the host's messages wake it through.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <deque>

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
// The page's own origin: http:// and the Host it was asked under (any
// case, ":80" or not), on the loopback.
bool sameOrigin(const std::string& origin, const std::string& host);

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

// ---- the window's browser pane
// One of its tabs, as the window reports it.
struct BrowserTab {
    std::string url, title;
};
constexpr std::size_t kMaxBrowserTabs = 100;
constexpr std::size_t kMaxAddress = 8192;    // bytes; a longer address is left out
constexpr std::size_t kMaxPageTitle = 300;   // bytes, cut at a character
// What a window adds to its tabs as "browser": {tabs: [{url, title}],
// active}, `active` the index of the tab on show (-1 for none) among the
// tabs kept. Tabs with no address or one over kMaxAddress are left out,
// titles cut to kMaxPageTitle, at most kMaxBrowserTabs.
Json browserReportJson(const std::vector<BrowserTab>& tabs, int active);
// What the page gets for a window from that report: {tabs: [{url, title,
// web, loopback}], active}, each tab checked again (the report comes from a
// renderer), `web` when webAddress takes it (only those are framed),
// `loopback` when it names this machine. Null when there is no report.
Json browserViewJson(const Json& report);
// An address a page may frame, or ask the window to open: http or https
// (any case), a host, no user name or password before it, no space or
// control character anywhere, at most kMaxAddress bytes. javascript:,
// file:, data: and every other scheme are refused; `why` says why.
bool webAddress(const std::string& url, std::string* why = nullptr);
// An http or https address on this machine: localhost (or a name under
// .localhost), 127.0.0.0/8, [::1], 0.0.0.0 or [::]. A page reached
// through a tunnel from elsewhere cannot reach it.
bool loopbackAddress(const std::string& url);

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
// Every color the page draws with, resolved as the editor resolves them
// (Settings.h: the theme, then the file's own colors), as "#rrggbb": the
// syntax colors by TokenStyle, each surface's background and text, the
// gutter, line numbers, ruler and bracket colors, the Markdown preview's,
// and the editor's and terminal's font sizes in points.
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
    // An edit (UTF-16 operations, in order). The lines it changed are kept
    // as edits for takeEdits().
    void apply(const Ot::Ops& ops);
    // ---- versions (editing): one for every change of the text
    struct Change {
        std::uint64_t version = 0;
        int by = 0;              // the page that made it (0: the host's)
        std::uint64_t seq = 0;   // that page's number for it
        Ot::Ops ops;             // in the text before it
        Ot::Ops fix;             // for that page: from what it guessed to what was made
        Json lines;              // the splices, or every line when `whole`
        bool whole = false;
        Json sel;                // [anchor, caret] for page `selFor`, else null
        int selFor = 0;
    };
    // The log reaches back this many versions at most.
    static constexpr std::size_t kMaxLog = 20000;
    // `ops` applied as the next version, made by page `by` (0: the host),
    // and queued for the pages.
    Change& commit(const Ot::Ops& ops, int by, std::uint64_t seq = 0);
    // What was applied after version `base`, as one list; false when the
    // log no longer reaches back so far (or `base` is ahead).
    bool since(std::uint64_t base, Ot::Ops* out) const;
    std::deque<Change>& queued() { return queue_; }
    void dropQueued() { queue_.clear(); }
    std::uint64_t version() const { return version_; }
    // A Markdown document's page is out of date.
    bool htmlStale() const { return htmlStale_; }
    void clearHtml() { htmlStale_ = false; }
    std::string html() const;
    const std::u16string& text() const { return text_; }
    // The whole document for the browser ("doc").
    Json snapshot() const;
    // Line edits since the last call ("lines"), null when none, or when the
    // lines are better sent whole (then snapshot() again: `changed()` says
    // so).
    Json takeEdits();
    bool changed() const { return changed_; }
    void clearChanged() { changed_ = false; }
    // A line as the browser gets it: [text, spans] or [text, spans, break].
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
    bool changed_ = false, htmlStale_ = false;
    std::deque<std::pair<int, Ot::Ops>> log_;   // versions logStart_ + 1 ... version_
    std::uint64_t logStart_ = 0;
    std::deque<Change> queue_;
    bool large() const;
    Json allLines() const;
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
    // Only this window's tabs, their documents, and the terminals the
    // window reports with them (the "terms" array of its WindowTabs json);
    // a page's show, keys or text for any other terminal are refused, and
    // the view never attaches to one. 0: every window's tabs, every
    // document and every terminal (a headless host, which has no windows).
    std::uint64_t window = 0;
    // A muted line where the terminals go while there are none ("" for
    // none).
    std::string termNote;
    std::string machine;      // the header's name ("" for this machine's)
    // Whether pages may edit, undo, save and type into terminals; false:
    // they only look, and the host is told this renderer only reads.
    bool editable = false;
    // Saves a document the way the app that owns the disk does (the Mac's
    // Cmd+S, its questions included), called on the server's thread once
    // the host has every edit the pages made; false when it cannot. Unset:
    // a page's save is answered with a note.
    std::function<bool(DocId)> save;
    // Opens a file of the folder (the path held inside it, spelled under
    // `folder`) as a click in the window's own tree does: `keep` for a kept
    // tab rather than the preview tab. Called on the server's thread; false
    // when it cannot. Pages that only read may open files too: a tab opened
    // or shown changes no text. Unset (a headless host, which decides no
    // tabs): a page's click on a file says so.
    std::function<bool(const std::string& path, bool keep)> open;
    // Opens an http or https address (webAddress has held it) in the
    // window's browser pane, a tab already at it shown again, else a new
    // tab. Called on the server's thread, only for a page that may edit;
    // false when it cannot. Unset (a headless host, which has no browser
    // pane): a page's `browse` says so, and the hello's canBrowse is false.
    std::function<bool(const std::string& url)> browse;
    // Dotfiles in the tree (the window's Show Hidden Files).
    bool showHidden = false;
    int port = 0;             // 0: any free port (checks)
    std::string token;        // "" for a fresh one
    int pollMs = 500;         // how often the session's state is looked at
    // The settings file the page's colors come from ("" for
    // $MINICODE_SETTINGS, else ~/.config/minicode/settings.conf), looked at
    // every second: a change recolors every page.
    std::string settingsPath;
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
    // The settings file changed (any thread): read it now rather than at
    // the next look.
    void reloadSettings();
    // The session's state changed (a window reported its tabs): look at it
    // now rather than at the next poll (any thread).
    void lookNow();
    // The palette the pages were last sent (the server's thread, or before
    // run()).
    const Json& palette() const { return palette_; }

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
    Json palette_;
    std::string settingsPath_, settingsStamp_;
    std::chrono::steady_clock::time_point lastSettingsLook_;
    std::atomic<bool> reload_{false};
    std::atomic<bool> look_{false};
    // Reads the settings file when it changed (or `force`): true when the
    // palette did.
    bool lookAtSettings(bool force);
    std::string helloText() const;
    void wake();
    void acceptAll();
    void readFrom(Conn& c);
    void writeTo(Conn& c);
    void handleHttp(Conn& c);
    void handleMessage(Conn& c, const std::string& text);
    void onEdit(Conn& c, const Json& j);
    void onUndo(Conn& c, const Json& j);
    void onSave(Conn& c, const Json& j);
    void onTermInput(Conn& c, const std::string& type, const Json& j);
    void onList(Conn& c, const Json& j);
    void onOpen(Conn& c, const Json& j);
    void onBrowse(Conn& c, const Json& j);
    void takeReads();
    void note(Conn& c, const std::string& text);
    void refuseEdit(Conn& c, DocId doc, std::uint64_t seq, const std::string& why, bool resync);
    void takeOutcomes();
    int nextConn_ = 0;
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
