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
#include <memory>
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
    // Documentation, as text or Markdown. Servers often leave it (and the
    // detail) out of the list and fill it in on completionItem/resolve.
    std::string documentation;
    bool docIsMarkdown = false;
    bool resolved = false;      // completionItem/resolve has answered for it
    // The item as the server sent it, which is what resolve sends back
    // (servers keep their own bookkeeping in its `data`). Shared, so the
    // copies filtering makes on every keystroke stay cheap.
    std::shared_ptr<const Json> raw;
    // Its place in the list parseCompletion returned, so a filtered copy
    // can be matched back to the item it came from.
    int index = -1;
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

// textDocument/signatureHelp, for the hint shown while typing a call's
// arguments. Parameter ranges are UTF-16 offsets into the label, so they map
// straight onto an NSString; {-1, -1} when a parameter's label could not be
// found in the signature's.
struct SignatureInfo {
    std::string label, documentation;
    bool docIsMarkdown = false;
    std::vector<std::pair<int, int>> params;
    std::vector<std::string> paramDocs;
};
struct SignatureHelp {
    std::vector<SignatureInfo> signatures;
    int activeSignature = 0;   // clamped into range; 0 when there are none
    int activeParameter = -1;  // for the active signature; -1 for none
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
// How to install a missing server is in InstallHints.h (Install::server).
// Split a command line into words. Single and double quotes group words;
// a backslash outside single quotes escapes the next character.
std::vector<std::string> splitCommand(const std::string& command);

// ------------------------------------------------------------ result parsing
std::vector<Diagnostic> parseDiagnostics(const Json& params, std::string* uri = nullptr);
// A CompletionList, a bare array of items, or null. Items come back sorted by
// sortText (then label), which is the order servers mean them in.
std::vector<CompletionItem> parseCompletion(const Json& result, bool* incomplete = nullptr);
// Fill in what completionItem/resolve answered: detail and documentation,
// when it gave them. Marks the item resolved either way.
void applyResolved(CompletionItem& item, const Json& result);
// MarkupContent ({kind, value}) or a plain string. False for anything else.
bool parseMarkup(const Json& j, std::string& text, bool& isMarkdown);
SignatureHelp parseSignatureHelp(const Json& result);
// MarkupContent, a MarkedString, or an array of MarkedStrings, flattened to
// text. Markdown code fences are dropped, keeping their contents.
std::string parseHover(const Json& result);
// The same answer kept as the server wrote it, for drawing: each part's text
// and whether it is Markdown ({language, value} becomes a fenced block), and
// the range the server says it describes, when it gives one.
struct HoverDoc {
    struct Part {
        std::string text;
        bool markdown = false;
    };
    std::vector<Part> parts;
    bool hasRange = false;
    Range range;
};
HoverDoc parseHoverDoc(const Json& result);
// Location, Location[], LocationLink[], or null.
std::vector<Location> parseLocations(const Json& result);
Json positionJson(Position p);
Position parsePosition(const Json& j);
Range parseRange(const Json& j);

// The items that match `prefix`, best first: the word exactly as typed
// (case and all), then filter keys that start with it in the same case, then
// those that start with it in any case, then those that merely contain it in
// order (a subsequence, any case). Within each group the server's order
// (sortText, which parseCompletion sorted by) is kept, so the first item is
// always the one to select. An empty prefix keeps everything as it was.
std::vector<CompletionItem> filterCompletions(const std::vector<CompletionItem>& items,
                                              const std::string& prefix);
// The index of an item that is exactly `typed` (its filter key or its
// insert text, case-sensitive), or -1. With such an item in the list,
// Return is a new line unless the arrows chose another.
int exactCompletion(const std::vector<CompletionItem>& shown, const std::string& typed);

// How the completion list came up: by itself while a word was typed, after a
// trigger character ("." "->" "::", or the first letter after one), or asked
// for (Ctrl+Space, Option+Esc).
enum class CompletionOrigin { Typing, Trigger, Explicit };
enum class CompletionKey { Return, Tab, ShiftTab };
// Whether a key pressed while the list shows takes the selected item. When
// it does not, the list closes and the key does what it does in the editor
// (a new line, indentation). Tab always takes it. Return takes it when the
// list was asked for or came after a trigger character, or once the user
// has moved through it with the arrow keys; a list that came up by itself
// while typing leaves Return a new line, as does an item that is exactly
// the word already typed. Shift+Tab never takes it.
bool completionKeyAccepts(CompletionOrigin origin, bool navigated, CompletionKey key,
                          bool selectedEqualsTyped);

// The part of a settings tree a workspace/configuration item asks for:
// "python.analysis" is settings["python"]["analysis"], "" the whole tree.
// Null when it is not there.
Json configurationSection(const Json& settings, const std::string& section);

// ------------------------------------------------------- Python servers
// Each Python server is told which interpreter to use in its own way:
// pyright and basedpyright read python.pythonPath through
// workspace/configuration, pylsp reads pylsp.plugins.jedi.environment from
// didChangeConfiguration, and jedi-language-server reads
// workspace.environmentPath from its initializationOptions only.
enum class PythonServer { Other, Pyright, BasedPyright, Pylsp, Jedi };
// Which one a command line runs, from its words' file names.
PythonServer pythonServerKind(const std::vector<std::string>& argv);
const char* pythonServerName(PythonServer kind);
// The settings tree to answer workspace/configuration and send in
// didChangeConfiguration. An empty interpreter leaves the server to choose.
Json pythonSettings(PythonServer kind, const std::string& interpreter);
// initializationOptions for `initialize`, or null.
Json pythonInitializationOptions(PythonServer kind, const std::string& interpreter);
// Whether a different interpreter needs the server restarted, rather than
// reaching it through didChangeConfiguration.
bool pythonInterpreterNeedsRestart(PythonServer kind);

// ------------------------------------------------------------------- client
class Client {
public:
    enum class State { Idle, Initializing, Ready, ShuttingDown, Exited };

    using Sender = std::function<void(const std::string& framed)>;
    using ResultHandler = std::function<void(const Json& result, const Json& error)>;

    explicit Client(Sender send) : send_(std::move(send)) {}

    State state() const { return state_; }

    // Sent with `initialize` when not null; set it before calling initialize.
    void setInitializationOptions(Json options) { initOptions_ = std::move(options); }
    // The settings workspace/configuration is answered from (by section).
    // When it is a non-empty object it is also sent as didChangeConfiguration
    // right after `initialized`, for servers that only read settings that way.
    void setSettings(Json settings) { settings_ = std::move(settings); }
    const Json& settings() const { return settings_; }
    // New settings: stored, and sent as workspace/didChangeConfiguration.
    void didChangeConfiguration(Json settings);

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
    // completionItem/resolve for an item from parseCompletion. 0 (and no
    // request) when the server does not resolve or the item has no raw form.
    int resolveCompletion(const CompletionItem& item, ResultHandler h);
    bool canResolveCompletion() const;
    // textDocument/signatureHelp. triggerKind is 1 (invoked), 2 (a trigger
    // character) or 3 (the content changed while the hint was showing).
    int signatureHelp(const std::string& uri, Position p, ResultHandler h,
                      const std::string& triggerCharacter = "", bool isRetrigger = false);
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
    // signatureHelpProvider's trigger and retrigger characters; both empty
    // when the server offers no signature help.
    bool hasSignatureHelp() const;
    std::vector<std::string> signatureTriggers() const;
    std::vector<std::string> signatureRetriggers() const;
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
    Json initOptions_, settings_;
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
