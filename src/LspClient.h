// LspClient.h — pure C++. The client half of the Language Server Protocol:
// message framing, the initialize/shutdown lifecycle, document sync, request
// ids matched to their responses, and parsing of the results MiniCode uses
// (diagnostics, completion, hover, definition).
//
// No I/O happens here. The client hands framed bytes to a send callback and is
// fed whatever the server wrote with receive(); the GUI owns the process and
// the pipes (Lsp.mm). That keeps every protocol decision unit-testable against
// a scripted "server".
//
// Positions are LSP's default: zero-based lines and UTF-16 code units within
// the line, which is also how NSString counts, so the GUI's offsets map
// straight across.
#pragma once
#include "Json.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Lsp {

struct Position {
    int line = 0, character = 0;
    bool operator==(const Position& o) const {
        return line == o.line && character == o.character;
    }
};
struct Range {
    Position start, end;
};
struct Location {
    std::string uri;
    Range range;
};

enum class Severity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct Diagnostic {
    Range range;
    Severity severity = Severity::Error;
    std::string message, source, code;
};

struct CompletionItem {
    std::string label, detail, insertText, filterText, sortText;
    int kind = 0;
    // A textEdit from the server: replace `editRange` with `newText`. For an
    // InsertReplaceEdit this is the insert range.
    bool hasEdit = false;
    Range editRange;
    std::string newText;
    // What typing is matched against: filterText, else the label.
    const std::string& filterKey() const { return filterText.empty() ? label : filterText; }
    // What accepting inserts: the edit's text, then insertText, then the label.
    const std::string& textToInsert() const {
        if (hasEdit) return newText;
        return insertText.empty() ? label : insertText;
    }
};

// ------------------------------------------------------------------ framing
// "Content-Length: N\r\n\r\n" + N bytes of JSON. The reader copes with a
// message split across reads, several in one read, other headers, and header
// names in any case.
class Framer {
public:
    static std::string frame(const std::string& body);
    void feed(const char* data, size_t n);
    void feed(const std::string& s) { feed(s.data(), s.size()); }
    // The next complete body, if one has arrived.
    bool next(std::string& body);
    size_t buffered() const { return buf_.size() - pos_; }

private:
    std::string buf_;
    size_t pos_ = 0;
};

// ------------------------------------------------------ text and positions
// UTF-16 offset <-> LSP position. Lines end at \n, \r\n or \r, as the spec
// says. A character past the end of its line clamps to the line's end, and a
// line past the end clamps to the end of the text.
size_t offsetForPosition(const std::u16string& text, Position p);
Position positionForOffset(const std::u16string& text, size_t offset);
// The same, for UTF-8 text (what the tests and the server see).
std::u16string toUtf16(const std::string& utf8);
std::string toUtf8(const std::u16string& utf16);

// file:// URIs, percent-encoding everything outside the unreserved set and '/'.
std::string uriFromPath(const std::string& path);
std::string pathFromUri(const std::string& uri);   // "" if not a file URI

// ---------------------------------------------------------- server choice
struct Language {
    std::string server;       // settings key: lsp.<server>
    std::string languageId;   // LSP languageId for didOpen
};
// Which server handles a file extension (lowercase, no dot). False if none.
bool languageForExtension(const std::string& ext, Language& out);
// The commands tried, in order, when the settings name none.
std::vector<std::string> defaultCommands(const std::string& server);
// The display name of a server key ("python" -> "Python").
std::string serverDisplayName(const std::string& server);
// Split a command line into words. Single and double quotes group words;
// a backslash outside single quotes escapes the next character.
std::vector<std::string> splitCommand(const std::string& command);

// ------------------------------------------------------------ result parsing
std::vector<Diagnostic> parseDiagnostics(const Json& params, std::string* uri = nullptr);
// A CompletionList, a bare array of items, or null. Items come back sorted by
// sortText (then label), which is the order servers mean them in.
std::vector<CompletionItem> parseCompletion(const Json& result, bool* incomplete = nullptr);
// MarkupContent, a MarkedString, or an array of MarkedStrings, flattened to
// text. Markdown code fences are dropped, keeping their contents.
std::string parseHover(const Json& result);
// Location, Location[], LocationLink[], or null.
std::vector<Location> parseLocations(const Json& result);
Json positionJson(Position p);
Position parsePosition(const Json& j);
Range parseRange(const Json& j);

// Items whose filter key starts with `prefix` (ignoring case), then those that
// merely contain it in order (a subsequence), keeping the server's order within
// each group. An empty prefix keeps everything.
std::vector<CompletionItem> filterCompletions(const std::vector<CompletionItem>& items,
                                              const std::string& prefix);

// ------------------------------------------------------------------- client
class Client {
public:
    enum class State { Idle, Initializing, Ready, ShuttingDown, Exited };

    using Sender = std::function<void(const std::string& framed)>;
    using ResultHandler = std::function<void(const Json& result, const Json& error)>;

    explicit Client(Sender send) : send_(std::move(send)) {}

    State state() const { return state_; }

    // Sends `initialize`. Notifications and requests made before the server
    // answers are queued and go out, in order, right after `initialized`.
    void initialize(const std::string& rootPath, int processId,
                    const std::string& clientName = "MiniCode");

    // Full-document sync. didChange bumps the version and sends the whole
    // text; it is a no-op for a document that is not open.
    void didOpen(const std::string& uri, const std::string& languageId,
                 const std::string& text);
    void didChange(const std::string& uri, const std::string& text);
    void didSave(const std::string& uri, const std::string& text);
    void didClose(const std::string& uri);
    bool isOpen(const std::string& uri) const { return docs_.count(uri) != 0; }
    int version(const std::string& uri) const;
    const std::string* openText(const std::string& uri) const;

    // Requests. The handler runs when the response arrives (with a null
    // result and the error object on failure). Returns the request id.
    int request(const std::string& method, Json params, ResultHandler handler);
    int completion(const std::string& uri, Position p, ResultHandler h,
                   const std::string& triggerCharacter = "");
    int hover(const std::string& uri, Position p, ResultHandler h);
    int definition(const std::string& uri, Position p, ResultHandler h);
    // Drop interest in a request (its handler will not run). Sends
    // $/cancelRequest if it was already sent.
    void cancel(int id);

    // `shutdown`, then `exit` once the server acknowledges it. `done` runs
    // after `exit` has been sent.
    void shutdown(std::function<void()> done = nullptr);
    // `exit` right away, for when there is no time to wait (app quitting).
    void exitNow();

    // Bytes from the server's stdout.
    void receive(const char* data, size_t n);
    void receive(const std::string& s) { receive(s.data(), s.size()); }

    // Server capabilities, valid once Ready.
    const Json& capabilities() const { return capabilities_; }
    std::vector<std::string> completionTriggers() const;
    std::string serverName() const { return serverName_; }

    // Callbacks.
    std::function<void()> onReady;
    // The server answered `initialize` with an error. The client is Exited
    // and drops everything from here on, so the owner should stop the
    // process and say so, rather than wait for a Ready that never comes.
    std::function<void(const std::string& message)> onInitializeFailed;
    std::function<void(const std::string& uri, const std::vector<Diagnostic>&)> onDiagnostics;
    std::function<void(int type, const std::string& message)> onShowMessage;
    std::function<void(const std::string& problem)> onProtocolError;

    size_t pendingRequests() const { return handlers_.size(); }
    size_t queuedMessages() const { return queue_.size(); }

private:
    struct Doc { std::string languageId, text; int version = 0; };

    Sender send_;
    State state_ = State::Idle;
    Framer framer_;
    int nextId_ = 1;
    int initializeId_ = 0, shutdownId_ = 0;
    std::map<int, ResultHandler> handlers_;
    std::vector<std::string> queue_;            // bodies waiting for Ready
    std::vector<int> queuedIds_;                // requests among them
    std::map<std::string, Doc> docs_;
    Json capabilities_;
    std::string serverName_;
    std::string rootPath_;
    std::function<void()> shutdownDone_;

    void write(const Json& msg, bool beforeReadyOk = false);
    void notify(const std::string& method, Json params);
    void handleMessage(const Json& msg);
    void handleServerRequest(const Json& msg);
    void respond(const Json& id, Json result);
};

}  // namespace Lsp
