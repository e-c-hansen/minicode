// SessionWeb.h — a browser's view of a session's host (SESSION-DESIGN.md,
// "Browser screen": stage 6b, which reads, and 8b, which edits). `mutible host --folder
// DIR --web PORT`, or `mutible attach --folder DIR --web PORT` for a host
// already running, serves one page on 127.0.0.1:PORT, never another
// interface. The page's script opens a WebSocket back to the same address
// and draws what the host describes: the session's tabs, the folder's
// tree (read by the host one folder at a time, SessionFiles.h), the
// document on show as lines with the core highlighter's spans, a Markdown
// document as MarkdownHtml's page, and each terminal's screen and
// scrollback as rows of styled runs from a headless TerminalScreen. Nothing
// is drawn from pixels. Read-only (the default), the browser can change
// nothing: it says which document and terminal it is looking at, lists
// folders, and reads a file of the folder to show on the page alone
// (`readfile`): the window's tabs are the window's. Editable
// (Options::editable), it also types into the document on show and its
// terminal, undoes through the host's history, asks for a save, opens a
// file as a tab in the window and closes the window's tabs as the window
// does, and asks the window's browser pane to open an http or https
// address. Either way the page lists the window's browser tabs in a pane
// of their own, beside the editor as the window has it, and shows the one
// picked in a frame.
//
// Files for the page's viewers (R3): a picture, a PDF, audio, video or an
// HTML page of the folder is served over HTTP at /v/ID/file/PATH (PATH the
// file's path from the folder, each name percent-encoded), which the page
// puts in an <img>, a frame, <audio> or <video>; the browser sends the
// credential's cookie with each, so an HTML page's own relative pictures
// and style sheets are served too, and no address on the page holds a
// secret. The server reads the bytes from the
// host a piece at a time (FsRead's "read", SessionFiles.h), as the tree's
// listings are read: confined to the window's folder (no "..", no symlink
// out, no dotfile unless the tree shows them), only kinds fileType() names,
// each with its Content-Type, nosniff, and a sandbox CSP on everything but a
// PDF (whose viewer will not run sandboxed), so an SVG or HTML file runs no
// script in the page's origin; HTML is framed sandboxed as well, scripts
// off. A Range request is answered with that part (206), as audio and
// video seek and PDF viewers read.
// A CSV or TSV comes over the socket instead, as rows parsed by the core's
// Csv (`file` -> `table`); so do a Jupyter notebook, as Notebook's Markdown
// rendered by MarkdownHtml for a frame sandboxed with no scripts
// (`notebook`), and a SQLite database's tables and a page of rows, opened
// read-only by SqliteRead (`db`).
//
// Access (SessionWebAccess.h): every address is under the view's own path,
// /v/ID/. A link, http://127.0.0.1:PORT/v/ID/?t=TOKEN with a random token
// of 48 hex digits, is good for one exchange: the page's first load trades
// the token for a credential in an HttpOnly, SameSite=Strict cookie scoped
// to /v/ID/, and its script takes the token out of the address bar and the
// history (history.replaceState to /v/ID/). The socket (/v/ID/ws), files
// (/v/ID/file/PATH) and reloads carry the cookie; the token, spent, is
// refused if it comes again. A credential ends after
// editor.session-web-idle-minutes with nothing from its browser, and its
// page is told so. With Options::ask (the Mac), a browser the app does not
// remember waits on the page until the app's question is answered. The
// Host header must name the loopback (127.0.0.1, localhost or [::1], any
// port, so `ssh -L` to another local port still works), and an Origin
// header, when there is one, the page's own origin (http:// and that
// Host): a page elsewhere, even one that rebinds a name to 127.0.0.1, is
// turned away, and the socket opens only with an Origin, which every
// browser sends. Anything else gets a refusal and nothing more. The page
// carries a Content-Security-Policy with a per-response nonce for its one
// script.
//
// The server is a renderer of the host (SessionClient.h) that says it only
// reads (Client::setReadOnly), or, editable, that it is a guest
// (Client::setGuest): the host refuses anything from it that would change
// the session beyond a guest's edits, undo, terminal input and its view's
// size for a terminal (which the pty takes only while a page of it is the
// one typing; below, `tsize`), and its views
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
//           canBrowse, canOpen, canClose}: once, first
//   palette {palette}: the host's settings file changed (paletteJson)
//   state   {windows, docs, terms}: whenever the session's state changes
//           (looked at every half second, or at once on lookNow()); with
//           Options::window, that window's tabs, documents and terminals;
//           a window with a browser pane
//           has `browser` {tabs: [{url, title, web, loopback}], active},
//           and a window that says which panes it shows has `panes`
//           {browser, terminal, browserShare} (panesViewJson)
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
//           shown]. `cols` and `rows` are the pty's: sent again whole when
//           the host's TermShell says the size changed, at that point in
//           the output
//   termd   {id, hist, rows: [[row, line], ...], cursor, alt, title}: lines
//           that went into the scrollback since, and rows that changed
//   ended   {why}: the host went
//   waiting {text}: the app is asking whether this browser may connect
//           (sent instead of hello until it is answered)
//   denied  {text}: the app said no; the socket closes
//   expired {text}: the credential's idle time passed (or the server knows
//           it no longer); the socket closes
//   tree    {path, root, entries, more | error}: one folder's listing
//           (SessionFiles.h's "list" answer), for a `list`
//   opening {path, abs, ok, why}: a file a page clicked is opening in the
//           window (`abs` its path as the tabs spell it), or why it is not
//   browsing {url, ok, why}: an address a page asked the window's browser
//           pane to open (`browse`), opening or refused
//   filetext {path, abs, kind, lines, html | note, indent} or {path,
//           error}: a file of the folder as it is on disk, for a tab of
//           the page's own (`readfile`; localFileJson)
//   closing {doc, ok, why}: a tab a page asked the window to close
//   table   {path, columns, rows, truncated, size | error}: a CSV or TSV
//           file as rows of cells (tableJson), for a `file`
//   notebook {path, html | error}: a notebook as a page (notebookJson)
//   db      {path, tables, moreTables, table, columns, rows, offset, more
//           | error}: a database's tables and a page of rows (databaseJson)
// and from the browser:
//   show    {doc, term}: what it is looking at (0 for nothing)
//   alive   {}: someone is using the page (sent at most once a minute
//           while it is visible and was used): the credential's idle time
//           starts again, as any other message does
//   list    {path}: one folder of the tree ("" the session's folder), read
//           by the host (FsRead), each time a folder is opened or refreshed
//   readfile {path}: a file of the folder for a tab of this page alone,
//           which changes nothing in the window (any page)
//   file    {path}: a CSV or TSV file of the folder as a table, read by the
//           host (any page)
//   notebook {path}: a notebook of the folder as a page (any page)
//   db      {path, table, offset}: a database's tables and rows from
//           `offset` of `table` (any page; nothing in it can change)
// and, only when editable:
//   open    {path, keep}: a file the tree's row names, opened as a tab in
//           the window (Options::open); a page that only reads is refused
//           (it reads the file itself instead)
//   close   {doc}: the window's tab for that document closed as the
//           window closes it (Options::close); one with unsaved changes is
//           refused, to be saved or closed in the window
//   edit    {doc, base, seq, ops, step {name, coalesce, fresh}, before, after,
//           cmd, at}: an edit of the document on show; cmd "newline", "tab",
//           "backtab" or "backspace" with `at` the selection it acted on
//   undo    {doc, redo}
//   save    {doc}: Options::save (the app that owns the disk), else a note
//   tkey    {term, key, mods}, ttext {term, text, mods}, tpaste {term, text}:
//           keys for the terminal on show, encoded as its screen's modes ask
//           (TerminalScreen::encodeKey, encodeChar, encodePaste)
//   tsize   {term, cols, rows}: the cells the page's terminal pane holds.
//           The server is one renderer for every page, so it tells the host
//           (TermResize) the size of the page that typed into that terminal
//           last: when that page types, or when it resizes while it is still
//           the last to have typed. A page that only looks never sizes the
//           pty (tmux's `window-size latest`, Host::sizer).
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
#include "SessionWebAccess.h"
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
    enum Kind { Page, Socket, File, Refused };
    Kind kind = Refused;
    int status = 403;
    std::string response, why;
    std::string file;   // File: the path from the folder, decoded, not yet checked
    // The request's credential (Page, Socket, File), and its state: a
    // socket is taken whatever the state, so its page can be told.
    std::string credential;
    Access::State state = Access::State::Unknown;
    bool exchanged = false;      // Page: a link's token was spent for it
    std::uint64_t question = 0;  // Page: the question the exchange put (Access::waiting)
};
// Who asked, as the server saw it.
struct Caller {
    std::string peer;      // "127.0.0.1:PORT"
    int peerPort = 0;
    int serverPort = 0;    // the port the server listens on (the cookie's name)
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
};
// `page` is the HTML to serve; "{{NONCE}}" in it becomes the response's
// script nonce, and "{{BASE}}" the view's path (access.base()). A page with
// a live link's token is exchanged here (the token spent, the cookies set);
// a reload, the socket and files need the credential's cookie.
Answer answer(const Request& r, Access& access, const std::string& page, const Caller& from);
// The page's words for a credential that is no more, a browser refused,
// and one waiting for the app (the `expired`, `denied` and `waiting`
// messages, and the refusal of a reload).
extern const char* const kExpiredText;
extern const char* const kDeniedText;
extern const char* const kWaitingText;

// ---- files for the page's viewers
// How the page shows a file, by its name's extension (any case): `view`
// "image" (PNG, JPEG, GIF, WebP, BMP, ICO, AVIF, SVG), "pdf", "audio" (MP3,
// M4A, AAC, WAV, Ogg, Opus, FLAC), "video" (MP4, M4V, MOV, WebM, OGV),
// "html" (.html, .htm, .xhtml), "table" (.csv, .tsv), "notebook" (.ipynb)
// or "database" (.sqlite, .sqlite3, .db, .db3), "" for anything else (text
// opens as a tab). `mime` is the Content-Type it is served
// with over HTTP ("" for a table, a notebook and a database, which come
// over the socket, and for anything not served); a style sheet is served (for an HTML page's own)
// with no view. `maxBytes`: the most a whole response may be (0 for none:
// audio and video are served a range at a time as the browser seeks).
// `sandbox`: served with a sandbox CSP (all but PDF).
struct FileType {
    std::string view, mime;
    std::uint64_t maxBytes = 0;
    bool sandbox = true;
};
FileType fileType(const std::string& path);
// The address a file of the folder is served at: the view's path (`base`,
// Access::base), "file/" and `rel`'s names percent-encoded.
std::string fileTarget(const std::string& base, const std::string& rel);
// A Range header for a file of `size` bytes: Whole (no header, or one this
// server does not take: several ranges, another unit, a malformed one, all
// of which HTTP lets a server answer whole), Part (one range, `start` to
// `end` inclusive, the end clipped to the file), or Unsatisfiable (a start
// past the end, or an empty file asked for a part).
struct ByteRange {
    enum Kind { Whole, Part, Unsatisfiable };
    Kind kind = Whole;
    std::uint64_t start = 0, end = 0;   // inclusive; Whole: 0 .. size - 1
    std::uint64_t length() const { return kind == Unsatisfiable ? 0 : end + 1 - start; }
};
ByteRange byteRange(const std::string* header, std::uint64_t size);
// The response's head for `type` and `range` (from byteRange): 200, 206
// with Content-Range, or 416; Content-Length, Accept-Ranges, nosniff,
// no-store, no referrer, and the sandbox CSP when type.sandbox.
std::string fileHead(const FileType& type, std::uint64_t size, const ByteRange& range);
// A refusal for a file request (the same shape as answer()'s).
std::string fileRefusal(int status, const std::string& why);
// A CSV or TSV file's bytes as the page's table: {delimiter, columns, rows:
// [[cell, ...], ...], truncated}. Rows stop at kMaxTableCells cells in all,
// a cell is cut at kMaxCellBytes (on a character); `complete` false: the
// bytes are only the file's start (kMaxTableBytes of it), so the last row
// may be cut and is dropped.
constexpr std::size_t kMaxTableBytes = std::size_t(4) << 20;   // one read
constexpr std::size_t kMaxTableCells = 200000;
constexpr std::size_t kMaxCellBytes = 2000;
Json tableJson(const std::string& path, const std::string& bytes, bool complete);

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
// Which of the window's panes it shows, as a window adds it to its tabs
// ("panes"): its browser pane and its terminal panel open or not, and the
// share of the width the browser takes beside the editor. The page lays
// itself out the same way (the editor, the browser to its right, the
// terminal below both); hiding a pane on the page is the page's own.
Json panesReportJson(bool browser, bool terminal, double browserShare);
// The same, checked again for the page (the report is a renderer's):
// booleans, the share held between 0.1 and 0.9 (0.5 when unsaid). Null
// when there is no report.
Json panesViewJson(const Json& report);

// Whether a window record of the host's state (Host::state()'s windows) is
// a window still open: its renderer still connected, and the record not
// one a closing window left for the folder's next window ("detached" in
// its tabs, stage 6). The page leaves the others out; their documents and
// terminals stay on the host until a window takes them.
bool windowOpen(const Json& record);

// A file of `folder` read for a tab of the page's own: {t: "filetext",
// path, abs, kind, version, lines, html | note, indent} as a document's
// "doc" is, or {t: "filetext", path, error}. Held inside the folder as the
// tree's reads are (confinedPath), a regular file of at most
// kMaxLocalFileBytes, no dotfile unless `hidden`, and text (no NUL in its
// first 8 KB).
constexpr std::size_t kMaxLocalFileBytes = 8u << 20;
Json localFileJson(const std::string& folder, const std::string& rel, bool hidden);
// The checks localFileJson makes before it reads, for the page's other
// readers: `rel` held inside `folder` (confinedPath), no dotfile name
// unless `hidden`, opened O_NONBLOCK and fstat'ed as a regular file. The
// open descriptor (the caller closes it), `abs` the path spelled under the
// folder, `size` the file's; -1 with `why`, a sentence for the page.
int openLocalFile(const std::string& folder, const std::string& rel, bool hidden,
                  std::string* abs, std::uint64_t* size, std::string* why);

// ---- what the page renders itself (R3, second slice): read here, from
// the folder the server serves, as readfile is, and confined the same way
// (openLocalFile).
// A Jupyter notebook as a page: {t: "notebook", path, html} (Notebook's
// Markdown through MarkdownHtml: cells, outputs as text, pictures as
// data: images, every text escaped, valid UTF-8), drawn in a frame
// sandboxed with no scripts, or {t: "notebook", path, error}. At most
// kMaxNotebookBytes.
constexpr std::size_t kMaxNotebookBytes = 32u << 20;
Json notebookJson(const std::string& folder, const std::string& rel, bool hidden);
// A SQLite database (SqliteRead, read-only, each read stopped at its
// deadline): {t: "db", path, tables: [{name, type}], moreTables, table,
// columns, rows, offset, more} for `table` ("" the first) from row
// `offset`, at most kDbPageRows rows and about kDbPageBytes of text. A
// cell is a string (text, an integer or a real as SQLite spells it), null,
// {blob: bytes} or, cut at SqliteRead::kMaxCellBytes, {text, bytes}. Or
// {t: "db", path, error} (and the tables, when only the rows failed).
constexpr std::size_t kDbPageRows = 100;
constexpr std::size_t kDbPageBytes = std::size_t(4) << 20;
Json databaseJson(const std::string& folder, const std::string& rel, bool hidden,
                  const std::string& table, std::int64_t offset);

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
    // A replay from the host (TermAttach): a fresh screen fed it. The
    // TermShell after it says the size it was made at (follow()).
    void replay(const std::string& bytes);
    // The pty's size from TermShell: the screen reflows to it there in the
    // output, as the host's did, and the next update is the whole terminal.
    void follow(int cols, int rows);
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
    // The last replay, until the TermShell after it: made again at that
    // size when it differs from the size the view guessed.
    std::string replay_;
    bool replayPending_ = false;
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
    // tab rather than the preview tab. Called on the server's thread, only
    // for a page that may edit (one that only reads changes nothing in the
    // window: it reads the file for a tab of its own); false when it
    // cannot. Unset (a headless host, which decides no tabs): a page's
    // `open` says so, and the hello's canOpen is false.
    std::function<bool(const std::string& path, bool keep)> open;
    // Closes the window's tab for a document as the window's own close
    // does. Called on the server's thread, only for a page that may edit
    // and a document of the window's with no unsaved changes; false when
    // it cannot. Unset: the hello's canClose is false.
    std::function<bool(DocId)> close;
    // Opens an http or https address (webAddress has held it) in the
    // window's browser pane, a tab already at it shown again, else a new
    // tab. Called on the server's thread, only for a page that may edit;
    // false when it cannot. Unset (a headless host, which has no browser
    // pane): a page's `browse` says so, and the hello's canBrowse is false.
    std::function<bool(const std::string& url)> browse;
    // Dotfiles in the tree (the window's Show Hidden Files).
    bool showHidden = false;
    int port = 0;             // 0: any free port (checks)
    std::string token;        // a link of the caller's own besides (checks); "" for none
    // How long a page's credential lasts with nothing from its browser, in
    // minutes (fractions allowed); < 0: editor.session-web-idle-minutes in
    // the settings file (60 when unset); 0: for ever.
    double idleMinutes = -1;
    // Asks the person at the app whether a browser it does not remember may
    // connect (the Mac's sheet). Called on the server's thread; the app
    // answers with Server::answer, from any thread. Unset (a headless host,
    // where nobody is there to ask): the link alone admits.
    std::function<void(const Access::Question&)> ask;
    // Whether the app remembered a browser id as allowed (any thread).
    std::function<bool(const std::string& browserId)> remembered;
    // Called on the server's thread with a fresh address each time a link
    // is spent, for the next browser (the headless host prints it). Unset:
    // the app makes links when asked (newLink).
    std::function<void(const std::string& url)> nextLink;
    int pollMs = 500;         // how often the session's state is looked at
    // The settings file the page's colors come from ("" for
    // $MUTIBLE_SETTINGS, else ~/.config/mutible/settings.conf), looked at
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
    // The newest link's token that has not been used, made when there is
    // none (any thread).
    std::string token();
    // What to open: http://127.0.0.1:PORT/v/ID/?t=TOKEN with that token.
    std::string url();
    // A fresh link, good for one browser (any thread): the Mac's Show and
    // Copy make one each time.
    std::string newLink();
    // The view's own path, /v/ID/.
    const std::string& base() const { return access_.base(); }
    // The app's answer to a question Options::ask put (any thread).
    void answer(std::uint64_t question, bool allow);
    // Who may use the page (checks look at it; any thread).
    Access& access() { return access_; }
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
    Access access_;
    std::uint64_t asked_ = 0;   // the last question handed to Options::ask
    double idleFromSettings_ = 60;
    std::chrono::steady_clock::time_point lastExpire_;
    void applyIdle();
    std::string linkUrl(const std::string& token) const;
    // A page's socket admitted: attached to the host, hello and state sent.
    void welcome(Conn& c);
    // Pages waiting for the app's answer, and credentials whose time is up.
    void lookAtAccess();
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
    void onTermSize(Conn& c, const Json& j);
    // Tells the host this renderer's view size for `t`, when it changed.
    void sizeTerminal(TermId t, int cols, int rows);
    // The most cells a page may say its pane holds, either way.
    static constexpr int kMaxTermCells = 1000;
    void onList(Conn& c, const Json& j);
    void onOpen(Conn& c, const Json& j);
    void onReadFile(Conn& c, const Json& j);
    void onClose(Conn& c, const Json& j);
    void onBrowse(Conn& c, const Json& j);
    void onFile(Conn& c, const Json& j);
    void takeReads();
    // A file for a viewer over HTTP: begun on its request, then read from
    // the host a piece at a time while the connection keeps up.
    struct FileSend;
    void beginFile(Conn& c, const std::string& rel, const std::string* range);
    void pumpFiles();
    void pumpFile(Conn& c);
    // At most this many files served at once; each holds at most two
    // pieces (kFilePiece) unsent.
    static constexpr int kMaxFileSends = 32;
    static constexpr std::size_t kFilePiece = std::size_t(2) << 20;
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
