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
// read-only by SqliteRead (`db`). A LaTeX document opens as text marked
// `latex`, and the page shows the PDF the app's own LaTeX preview typeset
// for it (`latex`), served from memory at /v/ID/typeset/PATH under the
// same rules as /file/: the app publishes its typesets to the server,
// which runs no tectonic of its own (Options::typeset, Typesets). With a
// renderer (Options::typesetPageSizes and typesetPageRender, the Mac's
// CoreGraphics), the page shows the typeset as one picture a page, each a
// PNG drawn at /v/ID/typeset-page/PATH?v=VERSION&p=N&w=WIDTH under the
// same rules, in a scroller of its own, so a new typeset keeps the
// reader's place; without one (a headless host, the Linux port) it frames
// the PDF in the browser's own viewer. The
// page's Source Control lists the folder's uncommitted changes and shows
// their review page, read only (`git`, `review`; SessionWebGit.h), and
// the window's open commit pages and worktree pages, which it reports
// beside its tabs (`reviews`), the same way; a binary picture's two
// versions are served from the read at /v/ID/picture/PAGE/VERSION/FILE/SIDE
// (pictureTarget), under the same rules as /file/.
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
//           {browser, terminal, browserShare} (panesViewJson), and one
//           with commit or worktree pages open has `reviews` [{id, kind,
//           title, onShow}] (reviewsViewJson, SessionWebGit.h)
//   doc     {id, path, kind, version, editable, indent, lines, html | note,
//           latex}:
//           the document the browser shows, whole: kind "code" (lines, each
//           [text, spans] or [text, spans, break], spans [[start, length,
//           style], ...] in UTF-16 units, styles by TokenStyle, break 0 for
//           none (the last line), 2 "\r\n", 3 "\r", 4 U+2029, left out for
//           "\n"), "markdown" (lines, and html, MarkdownHtml's page) or
//           "large" (note: too large to show here yet); indent {tabs, width};
//           `latex` true for a LaTeX document, whose page is its typeset
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
//   latex   {path, rel, state, version, url, pages, pageUrl, text}: a
//           LaTeX document's typeset (typesetJson), for a `latex`, and again
//           each time the app's typeset of it changes
//   git     {version, top, branch, initial, files, count | error}: the
//           folder's uncommitted changes, as the window's Source Control
//           lists them (gitStatusJson, SessionWebGit.h), for a `git`, and
//           again whenever a read shows something new
//   review  {version, files | error}: the review page of those changes
//           (reviewJson); reviewfile {version, file | key, gone}: one file
//           of it (reviewFileJson). With `page`, a window's review page
//           (a commit's, a worktree's; the id from the state's `reviews`)
//           and its header: {page, version, title, detail, body, files |
//           error | pending}
//   reviewread {page, version}: a read of that review page that the page
//           has not had (it asks for the review again)
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
//   latex   {path}: the typeset of the LaTeX document it shows (a path from
//           the folder, or a document's path under it), the app asked for
//           one; any page
//   git     {watch}: the folder's uncommitted changes read now and sent,
//           then again as they change while `watch` (false: no more); any
//           page, since git only reads
//   review  {opened, side, file}: the review of the last read (every file,
//           or the one whose key `file` names), `opened` the gaps shown
//           whole by file key ({key: [id]}; for `file`, [id]), `side` the
//           files side by side (keys; for `file`, a boolean); any page.
//           With `page` (an id the state's `reviews` listed), that window
//           review page instead, read when first asked, again on `reread`
//           (and, for a worktree, when the folder changes); `watch: false`
//           stops it
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
#include <mutex>
#include <string>
#include <vector>

#include <condition_variable>
#include <deque>
#include <thread>

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
    bool typeset = false;   // File: a LaTeX document's typeset PDF (/typeset/), not its bytes
    bool typesetPage = false;   // File: a page of it drawn as a PNG (/typeset-page/; the query says which)
    bool picture = false;   // File: a review's picture (/picture/, `file` what follows it)
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

// ---- the LaTeX preview's typeset (R3, third slice; SessionWebTypeset.cpp)
// A LaTeX document is text: it opens as a tab (a document's `doc`, or a
// page's own `filetext`), marked `latex`, and the page shows the PDF the
// app's own LaTeX preview typeset for it, Show Source flipping to its
// lines. Nothing here runs tectonic: the app publishes each typeset of its
// previews (Server::typesetWorking, typesetReady, typesetFailed), and a
// page's `latex` asks it for one (Options::typeset) only when it has none
// current. The PDF is kept in memory and served at /v/ID/typeset/PATH
// (PATH the document's path from the folder, as /file/ spells it), only
// for a document held inside the folder as the page's other reads are
// (openLocalFile), with a PDF's head (no sandbox: Chrome's viewer will not
// run in one) and ranges. With a renderer, each page is drawn as a PNG at
// /v/ID/typeset-page/PATH?v=PDFVERSION&p=PAGE&w=PIXELS under the same
// checks (PageRenders), so the page keeps the reader's place through a new
// typeset, which the browser's own viewer cannot.
// Whether `path` is a LaTeX document by its extension (.tex, .ltx, .latex,
// any case), as the Mac's preview takes them.
bool typesetSource(const std::string& path);
// The address a document's typeset PDF is served at: the view's path
// (`base`), "typeset/" and `rel`'s names percent-encoded.
std::string typesetTarget(const std::string& base, const std::string& rel);
// `path` as a path from `folder`: a relative path as it is, an absolute one
// under the folder (as spelled, else both resolved) with the folder taken
// off; false for an absolute path elsewhere. The answer is not yet checked
// (confinedPath does that).
bool relativeInFolder(const std::string& folder, const std::string& path, std::string* rel);

// A typeset page's size in points, as it shows (its crop box, turned by
// its rotation).
struct PageSize {
    double width = 0, height = 0;
};
// The address a page of a document's typeset is drawn at, as a PNG:
// typesetTarget's, under "typeset-page/". The page adds
// "?v=PDFVERSION&p=PAGE&w=PIXELS".
std::string typesetPageTarget(const std::string& base, const std::string& rel);
// What a /typeset-page/ address asks for: the typeset's pdfVersion, a page
// from 1, and a width in pixels, clamped to [kMinPageWidth, kMaxPageWidth].
struct PageAsk {
    std::uint64_t version = 0;
    int page = 0;
    int width = 0;
};
constexpr int kMinPageWidth = 64;
constexpr int kMaxPageWidth = 4096;
// The most pixels one page is drawn with (a tall page drawn narrower).
constexpr double kMaxPagePixels = 24e6;
// The most pages a typeset is shown as pictures; past it the page frames
// the PDF instead.
constexpr std::size_t kMaxTypesetPages = 5000;
// The query's v, p and w, each digits alone (p at least 1): false when one
// is missing or is not.
bool typesetPageAsk(const std::map<std::string, std::string>& query, PageAsk* ask);
// The width a page of `size` points is drawn at when `width` pixels are
// asked for: no more than kMaxPagePixels in all, at least kMinPageWidth.
int typesetPageWidth(const PageSize& size, int width);

// The app's typesets, by the document's resolved path (any thread).
class Typesets {
public:
    enum class State { None, Working, Ready, Failed };
    struct Entry {
        State state = State::None;
        std::shared_ptr<const std::string> pdf;   // the last that worked, kept while another runs
        // Its pages' sizes, when the server has a renderer (null without
        // one, or when the PDF's pages could not be read).
        std::shared_ptr<const std::vector<PageSize>> pages;
        std::string error;                        // Failed: why (tectonic's log), cut
        std::uint64_t version = 0;                // counts every change of the entry
        std::uint64_t pdfVersion = 0;             // counts changes of `pdf` alone
    };
    // The most kept: past it the entry changed longest ago goes.
    static constexpr std::size_t kMaxEntries = 64;
    // The most of a failure's text kept, cut on a character.
    static constexpr std::size_t kMaxErrorBytes = 64u << 10;
    // A PDF over this is not kept (the page says it is too large).
    static constexpr std::size_t kMaxPdfBytes = std::size_t(512) << 20;
    // Each true when the entry changed. Ready with the same bytes as the
    // PDF kept changes nothing (no new version, no reload on the page).
    bool working(const std::string& path);
    // `pages`: the new PDF's page sizes, kept with it (taken only when the
    // bytes are new).
    bool ready(const std::string& path, std::shared_ptr<const std::string> pdf,
               std::shared_ptr<const std::vector<PageSize>> pages = nullptr);
    bool failed(const std::string& path, const std::string& error);
    Entry get(const std::string& path) const;
    // Counts every change of any entry.
    std::uint64_t changes() const;
    std::size_t size() const;
    // The key a path is kept under: its real path, else the path.
    static std::string key(const std::string& path);

private:
    struct Kept {
        Entry entry;
        std::uint64_t changed = 0;   // `changes_` when it last changed
    };
    mutable std::mutex mutex_;
    std::map<std::string, Kept> kept_;
    std::uint64_t changes_ = 0, pdfVersions_ = 0;
    Kept& at(const std::string& key);   // with the mutex held
};
// What the page gets for a document's typeset: {t: "latex", path (as the
// page asked), rel, state: "ready" | "working" | "failed" | "none" |
// "refused", version, url (whenever a PDF is kept: typesetTarget with
// "?v=" its pdfVersion, so a new PDF is a new address), pages and pageUrl
// (whenever the PDF's page sizes are kept: [[width, height], ...] in
// points, and typesetPageTarget with "?v=" its pdfVersion), text (failed:
// the log; none: what to do; refused: why)}. `canTypeset`: the app makes
// typesets (Options::typeset), so a document with none is "working".
Json typesetJson(const std::string& asked, const std::string& rel, const Typesets::Entry& e,
                 const std::string& base, bool canTypeset);

// Pages of typesets drawn as PNGs on threads of their own, at most
// kThreads at once, the pictures kept within a cap of bytes, only the
// newest version's of each document (any thread).
class PageRenders {
public:
    using Draw = std::function<bool(const std::string& pdf, int page, int width, std::string* png)>;
    struct Key {
        std::string doc;            // the document's key in the typesets
        std::uint64_t version = 0;  // the PDF's pdfVersion
        int page = 0;               // from 0
        int width = 0;              // pixels
        bool operator<(const Key& o) const;
    };
    enum class Got { Ready, Pending, Failed, Busy };
    static constexpr int kThreads = 2;
    static constexpr std::size_t kMaxBytes = std::size_t(64) << 20;
    static constexpr std::size_t kMaxWaiting = 64;
    // `draw` runs on the threads; `wake` after each picture is drawn.
    PageRenders(Draw draw, std::function<void()> wake, std::size_t maxBytes = kMaxBytes);
    // Waits for the pictures being drawn; nothing is woken after.
    ~PageRenders();
    PageRenders(const PageRenders&) = delete;
    PageRenders& operator=(const PageRenders&) = delete;
    // A page's picture: Ready with it, Pending while it is drawn (asked
    // for once, `pdf` its bytes), Failed when the drawing failed, Busy when
    // kMaxWaiting are waiting already. An older version of the document's
    // pictures goes once a newer one is asked for.
    Got get(const Key& key, const std::shared_ptr<const std::string>& pdf, std::shared_ptr<const std::string>* png);
    std::size_t bytes() const;    // the pictures' bytes kept
    std::size_t kept() const;     // pictures kept (drawn or failed)
    std::uint64_t drawn() const;  // drawings made, ever
    int mostAtOnce() const;       // the most drawings ever under way at once

private:
    struct Item {
        enum State { Waiting, Drawing, Done, Failed } state = Waiting;
        std::shared_ptr<const std::string> png, pdf;   // pdf: until it is drawn
        std::uint64_t used = 0;
    };
    Draw draw_;
    std::function<void()> wake_;
    std::size_t maxBytes_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<Key, Item> items_;
    std::deque<Key> queue_;
    std::vector<std::thread> threads_;
    std::map<std::string, std::uint64_t> newest_;   // each document's newest version asked for
    bool stop_ = false;
    std::size_t bytes_ = 0;
    std::uint64_t uses_ = 0, drawn_ = 0;
    int drawing_ = 0, mostAtOnce_ = 0;
    void work();
    void trim();   // with the mutex held
};

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

class GitReads;   // SessionWebGit.h

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
    // Asks the app for a typeset of a LaTeX document of the folder (the
    // path held inside it, spelled under `folder`), for a page that shows
    // it (`latex`, any page: a typeset changes nothing of the user's). The
    // app answers through typesetWorking, typesetReady and typesetFailed,
    // from any thread: with what its own preview of the file has (never
    // typesetting again when that is current), else a typeset of its own.
    // Called on the server's thread each time a page asks. Unset (a
    // headless host, which typesets nothing): the page says the window's
    // preview makes it, and shows what was published, if anything.
    std::function<void(const std::string& path)> typeset;
    // The typeset's pages as pictures, so the page keeps the reader's place
    // when a new typeset comes (both, or neither). typesetPageSizes reads a
    // PDF's pages' sizes in points, in order (false when it cannot); called
    // on the thread that publishes a new PDF (typesetReady). typesetPageRender
    // draws page `page` (from 0) on white, `width` pixels wide, as a PNG
    // (false when it cannot); called on a thread of the server's own, at
    // most PageRenders::kThreads at once. Unset (a headless host, the Linux
    // port): the page frames the PDF in the browser's own viewer.
    std::function<bool(const std::string& pdf, std::vector<PageSize>* sizes)> typesetPageSizes;
    std::function<bool(const std::string& pdf, int page, int width, std::string* png)> typesetPageRender;
    // Runs git for the page's Source Control (R3's fourth slice,
    // SessionWebGit.h): `args` (the core's fixed ones, never a page's) in
    // `dir`, `input` on its stdin; true when it exited 0, with its output,
    // its error text and its exit status (-1 when unknown). Called on a
    // thread of the server's own. Unset: the core's runGit (the first git
    // on PATH). The Mac gives its own (MCGitRunSync: the git it found, its
    // environment and time limits).
    std::function<bool(const std::string& dir, const std::vector<std::string>& args, const std::string& input,
                       std::string* out, std::string* err, int* status)>
        git;
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
    // At most this many connections at once (pages' sockets, files being
    // sent, requests not yet answered); one past it is closed as it is
    // accepted, so a local process cannot use up the app's descriptors
    // (round 10's defect 10: the Mac serves from inside the app).
    int maxConnections = 64;
    // A connection that has not sent a whole request within this, or whose
    // answer is not taken by then, is closed. A page's socket once upgraded
    // and a file being sent are not held to it.
    int requestDeadlineMs = 5000;
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
    // Connections open now, of every kind (any thread).
    int connections() const { return connections_.load(); }
    // The settings file changed (any thread): read it now rather than at
    // the next look.
    void reloadSettings();
    // The session's state changed (a window reported its tabs): look at it
    // now rather than at the next poll (any thread).
    void lookNow();
    // The palette the pages were last sent (the server's thread, or before
    // run()).
    const Json& palette() const { return palette_; }
    // A typeset of the LaTeX document at `path` began, worked (`pdf`, the
    // bytes tectonic wrote), or failed (`error`, its log or why it did not
    // run), as the app's preview saw it (any thread). A page showing that
    // document hears of it at once.
    void typesetWorking(const std::string& path);
    void typesetReady(const std::string& path, std::shared_ptr<const std::string> pdf);
    void typesetFailed(const std::string& path, const std::string& error);
    const Typesets& typesets() const { return typesets_; }
    // The folder changed on disk, or the window's git did something (any
    // thread): pages showing Source Control get a fresh read, at most one a
    // second. A host that watches nothing never calls it; its pages read
    // again when asked (the page's refresh, a reconnect).
    void gitChanged();

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
    std::atomic<int> connections_{0};
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
    // A page shows a LaTeX document (`latex {path}`): what is published of
    // it now, the app asked for a typeset, and every change after.
    void onLatex(Conn& c, const Json& j);
    void sendTypesets();
    Typesets typesets_;
    // The typeset's pages drawn (Options::typesetPageRender), made at start.
    std::unique_ptr<PageRenders> renders_;
    std::uint64_t typesetsSeen_ = 0;
    // Source Control (`git`, `review`; SessionWebGit.h): read on a thread
    // of GitReads' own, sent to the pages watching when a read shows
    // something new, or to the page that asked.
    void onGit(Conn& c, const Json& j);
    void onReview(Conn& c, const Json& j);
    void askGit();
    void takeGit();
    // The window's review pages (a commit's, a worktree's), each read on a
    // thread of its own once a page asks for it, by the id the state gave.
    // `number` names it in its pictures' addresses (0 is the review of
    // uncommitted changes).
    struct PageReads {
        std::unique_ptr<GitReads> reads;
        std::uint64_t finished = 0;
        int number = 0;
        bool worktree = false;
    };
    std::map<std::string, PageReads> pages_;
    int nextPage_ = 1;
    void onReviewPage(Conn& c, const Json& j, const std::string& page);
    PageReads* pageReads(const std::string& page);
    void takePages();
    // A review's picture over HTTP, from the read's bytes in memory.
    void beginPicture(Conn& c, const std::string& spec, const std::string* range);
    std::unique_ptr<GitReads> git_;
    std::uint64_t gitFinished_ = 0;
    std::atomic<bool> gitDirty_{false};
    std::chrono::steady_clock::time_point lastGitAsk_;
    void takeReads();
    // A file for a viewer over HTTP: begun on its request, then read from
    // the host a piece at a time while the connection keeps up.
    struct FileSend;
    void beginFile(Conn& c, const std::string& rel, const std::string* range);
    // A typeset PDF, from memory, a piece at a time as for a file.
    void beginTypeset(Conn& c, const std::string& rel, const std::string* range);
    // A page of a typeset drawn as a PNG (`query` the address's), sent
    // from memory once drawn.
    void beginTypesetPage(Conn& c, const std::string& rel, const std::map<std::string, std::string>& query);
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
