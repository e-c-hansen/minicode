// LspClient.h — pure C++. The client half of the Language Server Protocol:
// message framing, the initialize/shutdown lifecycle, document sync, request
// ids matched to their responses, and parsing of the results MiniCode uses
// (diagnostics, completion, hover, definition, references, rename, code
// actions, document and workspace symbols) and the WorkspaceEdits rename and
// code actions answer with.
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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
    // DiagnosticTag 1 and 2: code that is never used (pyright's "x is not
    // accessed"), drawn faded, and a deprecated name, struck through.
    bool unnecessary = false, deprecated = false;
    // The diagnostic as the server sent it: textDocument/codeAction sends it
    // back in its context, and servers match their fixes by its fields.
    std::shared_ptr<const Json> raw;
};

// ------------------------------------------------------------- editing
// A TextEdit, and a WorkspaceEdit flattened to the text edits per file:
// `changes` and `documentChanges` both, edits to one file merged, sorted by
// position (edits at one place keep the server's order), and exact
// duplicates dropped. Creating, renaming or deleting a file is not done:
// such an edit sets `resourceOperations`, and the caller refuses it whole.
struct TextEdit {
    Range range;
    std::string newText;
};
struct FileEdit {
    std::string uri;
    std::vector<TextEdit> edits;   // sorted, none overlapping
    // documentChanges' textDocument.version: the version of the document the
    // edits were computed on, or -1 when the server gave none (`changes`, or
    // a null version for a file it does not have open).
    int version = -1;
};
struct WorkspaceEdit {
    std::vector<FileEdit> files;   // in the order the server first named them
    bool resourceOperations = false;
    std::string problem;           // why it cannot be applied; "" when it can
    size_t editCount() const;
};
// `canonical`, when given, turns a URI into the one spelling of its file
// (a symlink resolved), so edits to one file named two ways are merged.
using CanonicalUri = std::function<std::string(const std::string& uri)>;
WorkspaceEdit parseWorkspaceEdit(const Json& edit, const CanonicalUri& canonical = nullptr);
// The versions of the documents a client has open, by URI (Client::versions).
using DocVersions = std::map<std::string, int>;
// The first file of an edit whose text changed after the edit was computed,
// or "" when none did: its documentChanges version is not the version open
// now, or its version when the request went out (`asked`, a snapshot) is
// not the one now. An edit computed on older text would land its offsets on
// the wrong characters. `now` is looked up by URI, then by the file named.
std::string staleEditFile(const WorkspaceEdit& we, const DocVersions& asked,
                          const DocVersions& now);
// a comes before b.
bool positionBefore(Position a, Position b);
// One file's edits as UTF-16 offsets into its text, last first, so applying
// them in order leaves every offset still to come where it was.
struct OffsetEdit {
    size_t start = 0, length = 0;
    std::u16string text;
};
std::vector<OffsetEdit> offsetEdits(const std::u16string& text,
                                    const std::vector<TextEdit>& edits);
std::u16string applyTextEdits(const std::u16string& text, const std::vector<TextEdit>& edits);

// textDocument/prepareRename: a Range, {range, placeholder}, or
// {defaultBehavior: true} (rename the word at the position). Null, or
// anything else, means the symbol there cannot be renamed.
struct PrepareRename {
    bool ok = false;
    bool hasRange = false;   // false: the word at the position
    Range range;
    std::string placeholder;
};
PrepareRename parsePrepareRename(const Json& result);

// References grouped by file: `firstUri` (the file asked from) first, the
// others by path; within a file by position, duplicates dropped.
struct ReferenceFile {
    std::string uri;
    std::vector<Range> ranges;
};
std::vector<ReferenceFile> groupReferences(const std::vector<Location>& locs,
                                           const std::string& firstUri = "",
                                           const CanonicalUri& canonical = nullptr);
// Line `line` of a text, without its line break ("" past the end).
std::u16string lineOf(const std::u16string& text, int line);

// textDocument/codeAction answers with Commands and CodeActions mixed. A bare
// Command comes back as an action with only `command` set.
struct CodeAction {
    std::string title, kind;
    bool preferred = false;
    Json edit;      // a WorkspaceEdit, or null
    Json command;   // a Command ({title, command, arguments}), or null
    std::string disabledReason;   // the server offers it but says why not
    std::shared_ptr<const Json> raw;   // as sent, for codeAction/resolve
    // Which of a file's servers offered it, as mergeCodeActions numbers
    // them (the caller's order); -1 when it came from one server alone.
    int origin = -1;
    // The versions of the server's open documents when the actions were
    // asked for, set by the caller: the edit is for those texts.
    std::shared_ptr<const DocVersions> asked;
    bool hasEdit() const { return edit.isObject(); }
    bool hasCommand() const { return command.isObject() && command["command"].isString(); }
};
CodeAction parseCodeAction(const Json& j);
// The list in the server's order, the preferred ones first; disabled actions
// and ones with neither an edit, a command nor anything to resolve are left
// out.
std::vector<CodeAction> parseCodeActions(const Json& result, bool canResolve = false);
// The diagnostics whose ranges touch `range` (an empty range touches what
// it sits in or at either end of), as the server sent them, for a
// codeAction request's context.
Json diagnosticsInRange(const std::vector<Diagnostic>& diags, Range range);
// Code actions from several servers for one place, as one list. Each
// server's list is as parseCodeActions gave it, and `origin` is set to the
// server's index in `perServer`. Then, keeping the servers' order within
// each: the preferred ones, quick fixes, refactorings, other actions, and
// last the source actions that act on the whole file (Fix all, Organize
// imports). An action with the same title and kind as one already taken
// (two servers offering one fix) is left out.
std::vector<CodeAction> mergeCodeActions(const std::vector<std::vector<CodeAction>>& perServer);
// Does a code action kind fall under `kind` ("source.organizeImports.ruff"
// under "source.organizeImports"; "" is under nothing)?
bool kindUnder(const std::string& actionKind, const std::string& kind);
// Organize Imports: which of a file's code actions to run, or -1. Only
// source.organizeImports and its sub-kinds count. A sub-kind
// (source.organizeImports.ruff) is a tool's own and comes before the bare
// kind a general server offers; then the list's order.
int pickOrganizeImports(const std::vector<CodeAction>& actions);

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
    // Edits elsewhere that go with accepting it: the import an auto-import
    // completion needs, clangd's #include. Positions are in the text as it
    // was when the list was asked for. `hasAdditionalEdits` says the server
    // sent the field (an empty list included); a server that leaves it out
    // may send it on completionItem/resolve instead.
    std::vector<TextEdit> additionalEdits;
    bool hasAdditionalEdits = false;
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

// A server's output read into messages: the framing, then the JSON. One
// per server, used by one thread at a time; the Mac runs it on the
// server's read queue, so a large answer (5,000 completions) is parsed
// there, and hands each message to the Client on the main thread.
struct Incoming {
    Json message;
    std::string error;   // not empty: a body that was not JSON
};
class Reader {
public:
    void feed(const char* data, size_t n) { framer_.feed(data, n); }
    void feed(const std::string& s) { feed(s.data(), s.size()); }
    // The next complete message, parsed, if one has arrived.
    bool next(Incoming& out);
    size_t buffered() const { return framer_.buffered(); }

private:
    Framer framer_;
};

// Work a server reports with $/progress (indexing, analysing): what it
// said at `begin` and since. A `report` keeps the message or percentage it
// leaves out; percentage is -1 when the server gave none.
struct Progress {
    std::string token;   // the token as compact JSON (a string or a number)
    std::string title, message;
    int percentage = -1;
    uint64_t begun = 0;  // increases across every server, so the newest wins
};
// "Indexing: 3/10 files 30%", cut to about 60 characters (whole UTF-8
// characters), or "working…" when the server said nothing.
std::string progressText(const Progress& p);

// ------------------------------------------------------ text and positions
// UTF-16 offset <-> LSP position. Lines end at \n, \r\n or \r, as the spec
// says. A character past the end of its line clamps to the line's end, and a
// line past the end clamps to the end of the text.
size_t offsetForPosition(const std::u16string& text, Position p);
Position positionForOffset(const std::u16string& text, size_t offset);
// The same answers for many positions in one text: the line starts are found
// once (O(n)) and each conversion is a binary search, where the two functions
// above scan from the start every time. Use it wherever a loop converts
// (diagnostics, edits, references, Problems). It keeps a reference to the
// text, which must outlive it and stay unchanged.
class PositionIndex {
public:
    explicit PositionIndex(const std::u16string& text);
    size_t offset(Position p) const;
    Position position(size_t offset) const;
    size_t lineCount() const { return starts_.size(); }
    const std::u16string& text() const { return text_; }

private:
    const std::u16string& text_;
    std::vector<size_t> starts_;   // where each line starts
    std::vector<size_t> ends_;     // where each line's text ends (its break)
};
// lineOf (above) over an index already built, for a loop over references.
std::u16string lineOf(const PositionIndex& index, int line);
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
// Which server handles a file extension (lowercase, no dot): one of the five
// main languages (cpp, python, go, rust, typescript). False if none. The
// GTK port uses this one.
bool languageForExtension(const std::string& ext, Language& out);
// The same with the servers the Mac also knows (any language server by
// extension): first the settings' own, `lsp.<name>.extensions` (`extra`,
// server key -> extensions, Settings::lspExtensions), then the five, then
// the optional built-in ones, which run only when installed: texlab for
// LaTeX and BibTeX, marksman for Markdown, taplo for TOML and
// bash-language-server for shell scripts. A server of the user's own sends
// its key as the languageId; a built-in key given more extensions sends its
// usual one.
bool languageForExtension(const std::string& ext, Language& out,
                          const std::map<std::string, std::vector<std::string>>& extra);
// The optional built-in servers: started when one of their commands is
// installed, and never offered for installing unasked (no bar over the
// editor), unless the settings name a command for them.
bool isOptionalServer(const std::string& server);
const std::vector<std::string>& optionalServers();   // latex, markdown, toml, shell
// The commands tried, in order, when the settings name none.
std::vector<std::string> defaultCommands(const std::string& server);
// The display name of a server key ("python" -> "Python").
std::string serverDisplayName(const std::string& server);
// How to install a missing server is in InstallHints.h (Install::server).
// Split a command line into words. Single and double quotes group words;
// a backslash outside single quotes escapes the next character.
std::vector<std::string> splitCommand(const std::string& command);

// ------------------------------------------------- a second server per language
// A language can have a second server beside its main one: lsp.<server>.also
// in the settings, whose entry key is "<server>.also". The main one answers
// what only one server can (completion, hover, signature help, definition,
// references, rename); every one of them publishes diagnostics and is asked
// for code actions.
std::string alsoKey(const std::string& server);          // "python" -> "python.also"
bool isAlsoKey(const std::string& key);
std::string languageOfKey(const std::string& key);      // "python.also" -> "python"
// The second server's command when the settings name none: Ruff's own
// server for Python in a project that configures Ruff (ruff.toml,
// .ruff.toml or a [tool.ruff] table, which Formatter::configFile finds);
// "" otherwise. Only run when the program is found: it is never offered
// for installing unasked.
std::string defaultAlsoCommand(const std::string& server, bool projectConfiguresRuff);
// The status bar's name for a server program ("ruff", "clangd"),
// "-langserver" left off.
std::string programDisplayName(const std::string& program);

// What a request is about, to choose which of a file's servers gets it.
enum class Feature {
    Completion, SignatureHelp, Hover, Definition, References, Rename, CodeActions,
    DocumentSymbols, WorkspaceSymbols, FoldingRange
};
// Does a server whose `initialize` answered with these capabilities offer it?
bool offers(const Json& capabilities, Feature f);
struct ServerView {
    bool main = false;    // the language's main server (lsp.<server>)
    bool ready = false;   // has answered `initialize`
    const Json* capabilities = nullptr;
};
// Which of a file's servers answers a one-server request, or -1: the main
// one while it starts (its requests wait for it) or when it offers the
// feature, else the first other that is ready and offers it.
int routeRequest(Feature f, const std::vector<ServerView>& servers);
// The servers asked for code actions: every one that offers them, or is
// still starting (main or not; its answer is waited for).
std::vector<int> serversOffering(Feature f, const std::vector<ServerView>& servers);

// The status bar for a file's servers. States are LSP's view of a server
// entry (MCLspState in Lsp.h has the same values in the same order).
enum class ServerState {
    Idle, Starting, Running, NotInstalled, Off, Stopped, Failed, Restarting, Crashed
};
struct ServerStatus {
    std::string name;        // "basedpyright", or "" when none was found
    ServerState state = ServerState::Idle;
    bool main = false;
    // Its newest work in progress (progressText), "" when none, and when
    // that began (Progress::begun).
    std::string progress = std::string();
    uint64_t progressBegun = 0;
};
enum class StatusDot { None, Gray, Green, Yellow, Red };
struct StatusLine {
    std::string text;
    StatusDot dot = StatusDot::None;
};
// `language` names the language ("Python"); `servers` are its main server
// then its others; the counts are every server's problems in the file.
// The running servers share one part ("basedpyright + ruff: 2 errors" or
// "...: no problems"), at the place of the first; each other server says
// its own state ("ruff failed to start"), joined by " · ". A main server
// that is missing reads "Install a Python language server…" (the click
// shows how); a second server that is off or idle says nothing. While a
// running server reports progress, the newest begun stands where "no
// problems" would ("basedpyright: Indexing 40%", any counts after a
// comma). The dot is
// red when any has failed or crashed, else yellow while one starts, else
// green when one runs, else gray.
StatusLine statusLine(const std::string& language, const std::vector<ServerStatus>& servers,
                      int errors, int warnings);

// ------------------------------------------------------------ result parsing
// `version`, when given, is the document version the diagnostics were
// computed for, or -1 when the server left it out.
std::vector<Diagnostic> parseDiagnostics(const Json& params, std::string* uri = nullptr,
                                         int* version = nullptr);
// Where diagnostics fall in the text on show. The server computed them on
// `base`, the text it was last sent (nullptr: unknown, take them as given
// against `current`), while the editor may have been typed in since. A
// diagnostic outside the part that changed between the two moves with the
// text around it; one that touches it is stale and is left out (the server
// has a newer text coming and publishes again for it). UTF-16 offsets into
// `current`, end >= start, with `index` the diagnostic's place in `diags`.
struct DiagnosticSpan {
    size_t start = 0, end = 0, index = 0;
};
std::vector<DiagnosticSpan> diagnosticSpans(const std::u16string* base,
                                            const std::u16string& current,
                                            const std::vector<Diagnostic>& diags);

// What several servers have published, kept per file and per server, so
// one server publishing for a file never replaces another's list (Ruff's
// and basedpyright's both stand). `server` is any id unique among the
// servers.
class DiagnosticStore {
public:
    struct Set {
        std::string server;
        std::vector<Diagnostic> list;
        // The text the server had been sent when it computed them, or null.
        std::shared_ptr<const std::u16string> base;
    };
    // A server's list for a file replaces its last one; an empty list
    // removes it. Diagnostics that came without a source get `sourceName`,
    // so each says which server it is from. Returns whether anything changed.
    bool publish(const std::string& uri, const std::string& server,
                 std::vector<Diagnostic> list, std::shared_ptr<const std::u16string> base,
                 const std::string& sourceName = "");
    bool erase(const std::string& uri, const std::string& server);
    bool eraseServer(const std::string& server);
    void clear() { files_.clear(); generations_.clear(); }
    bool empty() const { return files_.empty(); }
    bool has(const std::string& uri) const { return files_.count(uri) != 0; }
    // A file's sets, in the order their servers first published for it.
    std::vector<const Set*> forUri(const std::string& uri) const;
    // A file's sets from one server only (0 or 1 of them).
    const Set* find(const std::string& uri, const std::string& server) const;
    std::vector<std::string> uris() const;
    // Every server's diagnostics for a file, in one list.
    std::vector<Diagnostic> merged(const std::string& uri) const;
    // Changes whenever a file's diagnostics do (a publish or an erase), and
    // never comes back to an earlier value, even across clear(): what was
    // worked out from them (Problems' rows) can be kept until it moves.
    uint64_t generation(const std::string& uri) const;

private:
    std::map<std::string, std::vector<Set>> files_;
    std::map<std::string, uint64_t> generations_;
    uint64_t counter_ = 0;
};
// diagnosticSpans over each of a file's sets, each against its own base,
// as one list in order of where they start. `diag` points into the sets.
struct MergedSpan {
    size_t start = 0, end = 0;
    const Diagnostic* diag = nullptr;
};
std::vector<MergedSpan> mergedDiagnosticSpans(const std::vector<const DiagnosticStore::Set*>& sets,
                                              const std::u16string& current);

// ------------------------------------------------------------ the Problems list
// One diagnostic as the Problems list shows it: the file (a path), where it
// is now (0-based lines, UTF-16 columns), and what it says.
struct Problem {
    std::string path;
    int line = 0, column = 0, endLine = 0, endColumn = 0;
    Severity severity = Severity::Error;
    std::string message, source, code;
};
// A row of the list: a heading for each file with its counts, then that
// file's problems (`problem` is an index into the input). Files with errors
// come first, then warnings, then the rest, by path within each; a file's
// problems go by severity, then line and column, as VS Code orders them.
struct ProblemRow {
    bool heading = false;
    std::string path;          // the heading's file
    size_t problem = 0;        // a problem row's index into the input
    int errors = 0, warnings = 0, others = 0;   // a heading's counts
};
std::vector<ProblemRow> problemRows(const std::vector<Problem>& problems);
struct ProblemCounts {
    int errors = 0, warnings = 0, infos = 0, hints = 0, files = 0;
    int total() const { return errors + warnings + infos + hints; }
};
ProblemCounts countProblems(const std::vector<Problem>& problems);
// "2 errors, 1 warning", "3 infos", "No problems". Information and hints
// are named only when there are no errors or warnings to name.
std::string problemCountsText(const ProblemCounts& counts);
// The message on one line: its line breaks and runs of spaces as one space,
// trimmed (clang and pyright write several lines for one problem).
std::string problemOneLine(const std::string& message);
// F8 and Shift+F8: where to go next among a file's problems, as `spans`
// (start and end offsets, any order). Forward is the first starting after
// the caret, else the first in the file (wrapping); backward the last
// starting before it, else the last. `last` is the index the previous step
// went to: while the caret is still at its start, the step goes on from it,
// so several problems at one place are each visited. -1 when none.
long stepProblem(const std::vector<std::pair<size_t, size_t>>& spans, size_t caret,
                 long last, bool forward);
// A CompletionList, a bare array of items, or null. Items come back sorted by
// sortText (then label), which is the order servers mean them in.
std::vector<CompletionItem> parseCompletion(const Json& result, bool* incomplete = nullptr);
// Fill in what completionItem/resolve answered: detail, documentation and
// additionalTextEdits, when it gave them. Marks the item resolved either way.
void applyResolved(CompletionItem& item, const Json& result);
// Whether accepting the item should wait for completionItem/resolve first:
// the server resolves, the item has not been, and it came without
// additionalTextEdits (servers such as typescript-language-server send an
// auto-import's edit only there).
bool completionWantsResolve(const CompletionItem& item, bool serverResolves);
// Accepting a completion: [start, end) of `text` becomes `insert`, and the
// item's additional edits go in with it. The edits come back in ascending
// order, none overlapping, ready for one multi-range replacement, with
// where the caret goes afterwards (just after the inserted text, moved by
// what the additional edits before it added or removed). Additional edits
// that overlap the main one, or each other, are against the spec and are
// all left out (`droppedAdditional`), so the word still goes in.
struct CompletionEdits {
    std::vector<OffsetEdit> edits;   // ascending
    size_t caret = 0;
    bool droppedAdditional = false;
};
CompletionEdits completionEdits(const std::u16string& text, size_t start, size_t end,
                                const std::u16string& insert,
                                const std::vector<TextEdit>& additional);
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
// textDocument/foldingRange's answer: whole lines, as the client asks
// (lineFoldingOnly), the start line staying in view and the lines after it
// down to endLine hidden. Ranges with no lines to hide, or numbers that are
// not lines, are left out; kind is "comment", "imports", "region" or "".
struct FoldingRange {
    int startLine = 0, endLine = 0;
    std::string kind;
};
std::vector<FoldingRange> parseFoldingRanges(const Json& result);

// ------------------------------------------------------------ symbols
// LSP's SymbolKind numbers, the ones named here; any of 1 to 26 may come.
enum SymbolKind {
    kSymFile = 1, kSymModule = 2, kSymNamespace = 3, kSymPackage = 4, kSymClass = 5,
    kSymMethod = 6, kSymProperty = 7, kSymField = 8, kSymConstructor = 9, kSymEnum = 10,
    kSymInterface = 11, kSymFunction = 12, kSymVariable = 13, kSymConstant = 14,
    kSymString = 15, kSymNumber = 16, kSymBoolean = 17, kSymArray = 18, kSymObject = 19,
    kSymKey = 20, kSymNull = 21, kSymEnumMember = 22, kSymStruct = 23, kSymEvent = 24,
    kSymOperator = 25, kSymTypeParameter = 26,
};
// One symbol, from textDocument/documentSymbol, workspace/symbol or the
// built-in outline (SymbolOutline.h).
struct Symbol {
    std::string name, detail;
    std::string container;   // the enclosing symbol's name, "" at the top
    int kind = 0;            // SymbolKind, 0 when unknown
    int depth = 0;           // levels below the top in a tree of them
    std::string uri;         // where it is (workspace symbols); "" for the file's own
    Range range;             // the whole declaration
    Range selectionRange;    // its name; the range when the server gave only one
    bool hasRange = true;    // false: a WorkspaceSymbol that names only its file
};
// textDocument/documentSymbol answers with a tree of DocumentSymbols or a
// flat list of SymbolInformation. A tree is flattened parent first, siblings
// in order of position, each child's container its parent's name and its
// depth one more; a flat list is put in order of position, its
// containerName kept. Null, or anything else, is no symbols.
std::vector<Symbol> parseDocumentSymbols(const Json& result);
// workspace/symbol: SymbolInformation or WorkspaceSymbol (whose location may
// be only {uri}), in the server's order.
std::vector<Symbol> parseWorkspaceSymbols(const Json& result);
// Several servers' answers as one list, in the servers' order; a symbol
// with the same name, file and start as one already taken is left out.
// `canonical` as for parseWorkspaceEdit.
std::vector<Symbol> mergeSymbols(const std::vector<std::vector<Symbol>>& perServer,
                                 const CanonicalUri& canonical = nullptr);
// The innermost symbol whose range holds p (its start included, its end
// too), or -1: the deepest, then the one starting last.
int symbolAt(const std::vector<Symbol>& symbols, Position p);
// What a kind is called, singular and plural, as VS Code's lists name them:
// "function", "functions"; "symbol", "symbols" for one it does not know.
std::string symbolKindName(int kind);
std::string symbolKindPlural(int kind);
// A one-character mark for a kind, drawn before the symbol's name, and the
// color it is drawn in (0xRRGGBB, VS Code's symbol icon colors).
std::u16string symbolKindGlyph(int kind);
unsigned symbolKindColor(int kind);
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

// ------------------------------------------------------- watched files
// workspace/didChangeWatchedFiles: a server registers watchers through
// client/registerCapability (basedpyright "**" and its library folders,
// Ruff its config files), and the client tells it of files created,
// changed and deleted on disk, which it would otherwise only see when a
// tab opens them (an agent's edits, a branch switch, a pip install).

// The LSP's glob: `*` any characters within a path segment, `?` one, `**`
// any number of whole segments (none included), `{a,b}` alternatives
// (nested too), `[a-z]` and `[!a-z]` one character of a set. Everything
// else stands for itself; matching is case-sensitive and whole.
bool globMatch(const std::string& pattern, const std::string& path);

enum class FileChangeType { Created = 1, Changed = 2, Deleted = 3 };
struct FileChange {
    std::string path;   // absolute, spelled as the folder the server was given
    FileChangeType type = FileChangeType::Changed;
};
// One registered watcher. `base` is a RelativePattern's folder (a path; the
// pattern is matched against the path below it), empty for a plain glob.
// `kind` is WatchKind's bits: 1 created, 2 changed, 4 deleted.
struct FileWatcher {
    std::string pattern, base;
    int kind = 7;
};
// The watchers of a registration's registerOptions ({watchers: [...]}); a
// watcher whose pattern is neither a string nor a RelativePattern with a
// file URI (or workspace folder) for its base is left out.
std::vector<FileWatcher> parseFileWatchers(const Json& registerOptions);
// Whether a watcher wants a change. A plain glob is matched against the
// absolute path (so "**/*.py" works), and one with no "/" against the file's
// name alone; one that is not absolute is matched below `root` too.
bool watcherMatches(const FileWatcher& w, const std::string& root, const FileChange& change);
// What one file-system event means once the path has been looked at: gone
// is Deleted; there, it is Created when the event says it was created or
// renamed into place, else Changed. FSEvents keeps a file's "created" flag
// on its later events for a while (a write half a second after creating
// it still says created), so a path already reported as there is Changed
// whatever the flags say; the classifier remembers which those are (up to
// 200,000 paths, then it starts again).
class FileEventClassifier {
public:
    FileChangeType classify(const std::string& path, bool exists, bool createdOrRenamed);
private:
    std::set<std::string> present_;
};
// Several events for one path in one batch become one, as VS Code's
// coalescer does: created then deleted is nothing, deleted then created is
// Changed, created then changed stays Created, otherwise the later wins.
// Paths keep the order they first appeared in.
std::vector<FileChange> coalesceFileChanges(const std::vector<FileChange>& changes);
// Paths never reported, as VS Code's files.watcherExclude: inside .git,
// .hg/store, and node_modules' packages (node_modules/x/**).
bool watchExcluded(const std::string& path);

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
    // text; it is a no-op for a document that is not open. Versions never go
    // back for a URI, across a close and a reopen either.
    void didOpen(const std::string& uri, const std::string& languageId,
                 const std::string& text);
    void didChange(const std::string& uri, const std::string& text);
    void didSave(const std::string& uri, const std::string& text);
    void didClose(const std::string& uri);
    bool isOpen(const std::string& uri) const { return docs_.count(uri) != 0; }
    int version(const std::string& uri) const;
    // Every open document's version, to compare with later (staleEditFile).
    DocVersions versions() const;
    const std::string* openText(const std::string& uri) const;
    // The text last sent for a document, found by its URI as given or, when
    // the server spells it another way, by the file it names. What the
    // positions in a server's answer about that document refer to.
    const std::string* textFor(const std::string& uri) const;
    std::vector<std::string> openUris() const;

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
    // textDocument/references, the declaration included when asked.
    int references(const std::string& uri, Position p, bool includeDeclaration,
                   ResultHandler h);
    // textDocument/foldingRange for a whole document.
    int foldingRanges(const std::string& uri, ResultHandler h);
    // textDocument/prepareRename and textDocument/rename.
    int prepareRename(const std::string& uri, Position p, ResultHandler h);
    int rename(const std::string& uri, Position p, const std::string& newName,
               ResultHandler h);
    // textDocument/codeAction for a range, with the diagnostics there (from
    // diagnosticsInRange) in its context.
    // `only` limits the kinds asked for (["source.organizeImports"]).
    int codeActions(const std::string& uri, Range range, Json diagnostics, ResultHandler h,
                    const std::vector<std::string>& only = {});
    // codeAction/resolve for an action from parseCodeActions; 0 (and no
    // request) when the server does not resolve or the action has no raw form.
    int resolveCodeAction(const CodeAction& action, ResultHandler h);
    // workspace/executeCommand for a Command ({command, arguments}).
    int executeCommand(const Json& command, ResultHandler h);
    // textDocument/documentSymbol and workspace/symbol (the query as typed).
    int documentSymbols(const std::string& uri, ResultHandler h);
    int workspaceSymbols(const std::string& query, ResultHandler h);
    // Drop interest in a request (its handler will not run). Sends
    // $/cancelRequest if it was already sent.
    void cancel(int id);

    // `shutdown`, then `exit` once the server acknowledges it. `done` runs
    // after `exit` has been sent.
    void shutdown(std::function<void()> done = nullptr);
    // `exit` right away, for when there is no time to wait (app quitting).
    void exitNow();

    // Bytes from the server's stdout: framed, parsed, and handled.
    void receive(const char* data, size_t n);
    void receive(const std::string& s) { receive(s.data(), s.size()); }
    // One message a Reader made elsewhere (another thread): handled as
    // receive() would have. Nothing is done once the client has Exited.
    void deliver(const Incoming& in);

    // $/progress: the newest work begun and not yet ended, or null; every
    // begin, report and end runs onProgress.
    const Progress* progress() const;
    size_t progressCount() const { return progress_.size(); }
    std::function<void()> onProgress;

    // Server capabilities, valid once Ready.
    const Json& capabilities() const { return capabilities_; }
    std::vector<std::string> completionTriggers() const;
    // signatureHelpProvider's trigger and retrigger characters; both empty
    // when the server offers no signature help.
    bool hasSignatureHelp() const;
    std::vector<std::string> signatureTriggers() const;
    std::vector<std::string> signatureRetriggers() const;
    bool hasRename() const;
    bool canPrepareRename() const;
    bool hasReferences() const;
    bool hasCodeActions() const;
    bool canResolveCodeAction() const;
    bool offers(Feature f) const { return Lsp::offers(capabilities_, f); }
    std::string serverName() const { return serverName_; }

    // Callbacks.
    std::function<void()> onReady;
    // The server answered `initialize` with an error. The client is Exited
    // and drops everything from here on, so the owner should stop the
    // process and say so, rather than wait for a Ready that never comes.
    std::function<void(const std::string& message)> onInitializeFailed;
    // Diagnostics for a document. Ones published for an older version than
    // the client has since sent are dropped before they get here (the
    // server publishes again for the newer text); while the callback runs,
    // textFor(uri) is the text they were computed on.
    std::function<void(const std::string& uri, const std::vector<Diagnostic>&)> onDiagnostics;
    size_t staleDiagnosticsDropped() const { return staleDiagnostics_; }
    std::function<void(int type, const std::string& message)> onShowMessage;
    std::function<void(const std::string& problem)> onProtocolError;
    // workspace/applyEdit from the server (often while a command it was
    // asked to run is still going). The owner applies params["edit"] and
    // calls `reply` once, now or later; it answers the server {applied,
    // failureReason}. Without a handler the edit is refused.
    using ApplyEditReply = std::function<void(bool applied, const std::string& failureReason)>;
    std::function<void(const Json& params, ApplyEditReply reply)> onApplyEdit;

    // Watched files. With file watching on (set before initialize, by an
    // owner that will call didChangeWatchedFiles), `initialize` offers
    // dynamic registration of workspace/didChangeWatchedFiles; off (the
    // default) a server watches for itself if it can. Registrations are
    // kept by id until unregistered; onWatchersChanged runs after either.
    void setFileWatching(bool on) { fileWatching_ = on; }
    bool watchesFiles() const { return !watchers_.empty(); }
    std::vector<FileWatcher> fileWatchers() const;
    // The folders of the RelativePatterns registered (a server's library
    // folders may lie outside the root), each once.
    std::vector<std::string> watchedBases() const;
    std::function<void()> onWatchersChanged;
    // Sends the changes some watcher wants, in one notification, and says
    // how many it sent. Nothing before Ready or without registrations.
    size_t didChangeWatchedFiles(const std::vector<FileChange>& changes);

    size_t pendingRequests() const { return handlers_.size(); }
    size_t queuedMessages() const { return queue_.size(); }

private:
    struct Doc { std::string languageId, text; int version = 0; };

    Sender send_;
    State state_ = State::Idle;
    Reader reader_;
    std::map<std::string, Progress> progress_;   // by token
    int nextId_ = 1;
    int initializeId_ = 0, shutdownId_ = 0;
    std::map<int, ResultHandler> handlers_;
    std::vector<std::string> queue_;            // bodies waiting for Ready
    std::vector<int> queuedIds_;                // requests among them
    std::map<std::string, Doc> docs_;
    // The last version sent for each URI, kept across didClose, so a
    // reopened document goes on counting and a publish for the text before
    // the close (v4, arriving after a reopen) is seen as stale.
    std::map<std::string, int> lastVersion_;
    Json capabilities_;
    Json initOptions_, settings_;
    bool fileWatching_ = false;
    std::string serverName_;
    std::string rootPath_;
    std::map<std::string, std::vector<FileWatcher>> watchers_;   // by registration id
    size_t staleDiagnostics_ = 0;
    const Doc* findDoc(const std::string& uri) const;
    std::function<void()> shutdownDone_;

    void write(const Json& msg, bool beforeReadyOk = false);
    void notify(const std::string& method, Json params);
    void handleMessage(const Json& msg);
    void handleServerRequest(const Json& msg);
    void handleProgress(const Json& params);
    void respond(const Json& id, Json result);
};

}  // namespace Lsp
