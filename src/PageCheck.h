// PageCheck.h — pure C++. The rules behind agents checking pages in the
// browser pane (`minicode browse`, `minicode page`; BrowserPages.mm does the
// WebKit side): which addresses an agent may load, which pages its reads
// may look at (the origin limit), the console record kept for the agent
// page, the caller's place read from a JavaScript stack, and how much of a
// page a screenshot covers. Nothing here runs JavaScript or does I/O.
#pragma once
#include "Json.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace PageCheck {

// The setting that lifts the origin limit (Settings.h, pageAnyOrigin).
constexpr const char* kAnyOriginSetting = "control.page-any-origin";
// The setting that lets `page --paste` type into the agent page (Settings.h,
// pageInput).
constexpr const char* kInputSetting = "control.page-input";
// The most of a page's text `page --text` returns, in bytes.
constexpr size_t kMaxTextBytes = 1024 * 1024;
// Console entries kept per page load, and the most of one entry's text.
constexpr size_t kMaxConsoleEntries = 500;
constexpr size_t kMaxConsoleText = 4096;
// The tallest screenshot, in pixels.
constexpr double kMaxSnapshotPixels = 16384;
// How long --wait waits for a load, in seconds.
constexpr double kWaitSeconds = 15;

// Whether a page at `url` is local: http or https on localhost, 127.0.0.1,
// [::1] or a name ending in .localhost (any port), or a file:// address.
// Read the way a browser reads an address, so "http://localhost@x.com/",
// "http://x.com\@localhost" and "http://127.0.0.1.x.com" are not local.
bool isLocalAddress(const std::string& url);
// The error a read of a page that is not local answers with.
std::string originError(const std::string& url);

// What `minicode browse` loads for what it was given: an http, https or
// file address as it is, about:blank, "localhost:5173" or "127.0.0.1:8000/x"
// (or a bare "localhost") as http, an absolute path of a file that exists
// (`exists`) as a file address, and a name with a dot ("example.com") as
// https. Anything else is an error, never a web search, and no other scheme
// (javascript:, data:, ...) is loaded.
bool agentAddress(const std::string& raw, const std::function<bool(const std::string&)>& exists,
                  std::string& url, std::string& error);
// "/a b/c.html" -> "file:///a%20b/c.html".
std::string fileAddress(const std::string& path);

// ---------------------------------------------------------- the console
struct ConsoleEntry {
    int64_t seq = 0;          // counts up for the life of the page, across loads
    std::string level;        // log, info, warn, error or debug
    std::string text;
    std::string source;       // the script's address, "" when unknown
    int line = 0, column = 0;
    bool uncaught = false;    // an uncaught error or an unhandled rejection
};

// The agent page's record since its page loaded: the last
// kMaxConsoleEntries entries, each numbered.
class ConsoleLog {
public:
    // Adds an entry (its seq is set here) and returns the seq.
    int64_t add(ConsoleEntry e);
    // A new page committed: the entries go, the numbering goes on.
    void newPage();
    // Entries numbered after `since` (0: all), oldest first.
    std::vector<ConsoleEntry> since(int64_t since) const;
    int64_t lastSeq() const { return next_ - 1; }
    // Entries of this page load dropped to keep the last 500.
    size_t dropped() const { return dropped_; }
    size_t size() const { return entries_.size(); }

private:
    std::deque<ConsoleEntry> entries_;
    int64_t next_ = 1;
    size_t dropped_ = 0;
};

// One message from the capture script (BrowserPages.mm): a JSON object
// with "kind" ("console", "uncaught" or "rejection"), "level", "text", and
// "source", "line", "column" or a "stack" to read them from. False for
// anything else. Text is cut to kMaxConsoleText bytes. A message relayed
// from the page's own world (`fromPage`) can only be a console call: an
// "uncaught" one there would be the page pretending.
bool parseConsoleMessage(const std::string& json, ConsoleEntry& out, bool fromPage = false);

// The place that called console.log, from `new Error().stack` as WebKit
// writes it ("fn@https://host/app.js:12:7" a line, the innermost first):
// the first frame with an address, skipping the capture's own frames
// (which have none) and "[native code]".
struct Frame {
    std::string source;
    int line = 0, column = 0;
};
bool callerFrame(const std::string& stack, Frame& out);

// An entry as `page --console` prints it, one JSON object a line.
Json entryJson(const ConsoleEntry& e);

// ---------------------------------------------------------- the page
// `page --url`, `browse --wait`: the address, the HTTP status (null when
// there is none: a file, or no answer yet), the title and whether a load
// is still on its way.
Json pageState(const std::string& url, int status, const std::string& title, bool loading);

// ---------------------------------------------------------- typing
// `page --paste` sends Cmd+V (after Cmd+A with --all) to the agent page, as
// the user would type them, and only into a field the page already has the
// keyboard in: what the user clicked into. What has the keyboard there
// (document.activeElement): its tag name (upper case, as the DOM gives it),
// for an INPUT its type (lower case), whether it is contenteditable, and
// whether it is read-only or disabled.
enum class Focus { None, Field, Frame };
Focus focusKind(const std::string& tag, const std::string& type, bool editable, bool readOnly);
// "field", "frame" or "none", as the answer names it.
const char* focusName(Focus f);
// The errors: the setting is off; nothing in the page takes text.
std::string inputOffError();
std::string noFieldError(const std::string& url);

// Text cut to at most `maxBytes` bytes on a UTF-8 character boundary.
std::string clipUtf8(const std::string& text, size_t maxBytes);

// How tall a screenshot is, in points: the visible area, or with `full`
// the page's height, cut so the picture is at most kMaxSnapshotPixels
// pixels tall at `scale` (pixels per point). `cut` says it was.
struct SnapshotPlan {
    double height = 0;
    bool cut = false;
};
SnapshotPlan snapshotPlan(double visibleHeight, double pageHeight, double scale, bool full);

// What is at a screenshot's path already (lstat: a symlink is Other).
enum class PathAt { Nothing, File, Folder, Other };

// Where a screenshot may be written: an absolute path ending in .png (any
// case) with nothing there, or, with `overwrite` (page --overwrite), a
// plain file there. Never a folder, and never a symlink or other special
// file, which a picture would replace or be written through. False with
// `error` set.
bool screenshotPath(const std::string& path, PathAt existing, bool overwrite, std::string& error);

}  // namespace PageCheck
