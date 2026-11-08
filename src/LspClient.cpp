// LspClient.cpp — see LspClient.h. The Client: the handshake and the
// capabilities it offers (initialize), document sync (didOpen, didChange,
// didSave, didClose), requests matched to their answers, progress,
// registrations, server-to-client requests, and shutdown. The parsing of
// each result is in the file for its feature (LspCompletion, LspFeatures,
// LspEdits, LspDiagnostics, LspSync), not here.
#include "LspClient.h"
#include <algorithm>
#include <atomic>
#include <chrono>

namespace Lsp {

// ------------------------------------------------------------------- client
void Client::write(const Json& msg, bool beforeReadyOk) {
    if (state_ == State::Exited) return;
    if (state_ == State::ShuttingDown && !beforeReadyOk) return;
    std::string body = msg.dump();
    if ((state_ == State::Idle || state_ == State::Initializing) && !beforeReadyOk) {
        queue_.push_back(body);
        return;
    }
    send_(Framer::frame(body));
}

void Client::notify(const std::string& method, Json params) {
    write(Json::object({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}}));
}

void Client::respond(const Json& id, Json result) {
    write(Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}),
          true);
}

void Client::initialize(const std::string& rootPath, int processId,
                        const std::string& clientName) {
    if (state_ != State::Idle) return;
    std::string rootUri = uriFromPath(rootPath);
    std::string name = rootPath;
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < name.size()) name = name.substr(slash + 1);

    Json completionItem = Json::object({
        // Tab stops and placeholders (Snippet.h, SnippetSession.mm).
        {"snippetSupport", true},
        {"insertTextModeSupport", Json::object({{"valueSet", Json::array().push(1).push(2)}})},
        {"insertReplaceSupport", false},
        {"documentationFormat", Json::array().push("markdown").push("plaintext")},
        {"resolveSupport", Json::object({{"properties",
                                          Json::array().push("documentation").push("detail")}})},
    });
    Json signatureInformation = Json::object({
        {"documentationFormat", Json::array().push("markdown").push("plaintext")},
        {"parameterInformation", Json::object({{"labelOffsetSupport", true}})},
        {"activeParameterSupport", true},
    });
    Json kinds = Json::array();
    for (int k = 1; k <= 26; k++) kinds.push(k);
    Json symbolKinds = Json::object({{"valueSet", kinds}});
    Json textDocument = Json::object({
        {"synchronization", Json::object({{"didSave", true},
                                          {"dynamicRegistration", false}})},
        {"completion", Json::object({{"completionItem", completionItem},
                                     {"contextSupport", true},
                                     {"completionList", Json::object({{"itemDefaults",
                                          Json::array().push("insertTextFormat")
                                              .push("insertTextMode")}})}})},
        {"signatureHelp", Json::object({{"signatureInformation", signatureInformation},
                                        {"contextSupport", true}})},
        // Markdown first: hover info is drawn with its code and emphasis.
        {"hover", Json::object({{"contentFormat",
                                 Json::array().push("markdown").push("plaintext")}})},
        {"definition", Json::object({{"linkSupport", true}})},
        {"references", Json::object({{"dynamicRegistration", false}})},
        // Whole lines only: the editor folds lines, never part of one.
        {"foldingRange", Json::object({{"lineFoldingOnly", true},
                                       {"foldingRangeKind", Json::object({{"valueSet",
                                            Json::array().push("comment").push("imports")
                                                .push("region")}})}})},
        {"rename", Json::object({{"prepareSupport", true},
                                 {"prepareSupportDefaultBehavior", 1},
                                 {"honorsChangeAnnotations", false}})},
        {"codeAction", Json::object({
             {"codeActionLiteralSupport", Json::object({{"codeActionKind", Json::object({
                  {"valueSet", Json::array().push("").push("quickfix").push("refactor")
                                   .push("refactor.extract").push("refactor.inline")
                                   .push("refactor.rewrite").push("source")
                                   .push("source.organizeImports")
                                   .push("source.fixAll")}})}})},
             {"isPreferredSupport", true},
             {"disabledSupport", true},
             {"dataSupport", true},
             {"resolveSupport", Json::object({{"properties", Json::array().push("edit")}})},
         })},
        // Format Document asks a server only for a file no formatter is
        // planned for (Formatter::serverMayFormat).
        {"formatting", Json::object({{"dynamicRegistration", false}})},
        {"rangeFormatting", Json::object({{"dynamicRegistration", false}})},
        {"publishDiagnostics",
         Json::object({{"relatedInformation", false},
                       {"versionSupport", true},
                       {"tagSupport",
                        Json::object({{"valueSet", Json::array().push(1).push(2)}})}})},
        // Go to Symbol in File (Shift+Cmd+O) shows a tree's containers.
        {"documentSymbol", Json::object({{"hierarchicalDocumentSymbolSupport", true},
                                         {"symbolKind", symbolKinds}})},
        // Inlay hints are drawn as they come; none is resolved.
        {"inlayHint", Json::object({{"dynamicRegistration", false}})},
    });
    // Semantic tokens: whole documents and their deltas, a token never
    // over two lines, laid over the editor's own colors.
    {
        Json types = Json::array(), modifiers = Json::array();
        for (const char* t : {"namespace", "type", "class", "enum", "interface", "struct",
                              "typeParameter", "parameter", "variable", "property",
                              "enumMember", "event", "function", "method", "macro", "keyword",
                              "modifier", "comment", "string", "number", "regexp", "operator",
                              "decorator", "label"})
            types.push(t);
        for (const char* m : {"declaration", "definition", "readonly", "static", "deprecated",
                              "abstract", "async", "modification", "documentation",
                              "defaultLibrary"})
            modifiers.push(m);
        textDocument.set("semanticTokens", Json::object({
            {"dynamicRegistration", false},
            {"requests", Json::object({{"range", false},
                                       {"full", Json::object({{"delta", true}})}})},
            {"tokenTypes", types},
            {"tokenModifiers", modifiers},
            {"formats", Json::array().push("relative")},
            {"overlappingTokenSupport", false},
            {"multilineTokenSupport", false},
            {"serverCancelSupport", true},
            {"augmentsSyntaxTokens", true},
        }));
    }
    Json workspace = Json::object({{"workspaceFolders", true},
                                  {"configuration", true},
                                  {"applyEdit", true},
                                  // Text edits only: an edit that would
                                  // create, rename or delete a file is
                                  // refused whole.
                                  {"workspaceEdit",
                                   Json::object({{"documentChanges", true},
                                                 {"resourceOperations", Json::array()},
                                                 {"failureHandling", "abort"}})},
                                  {"executeCommand",
                                   Json::object({{"dynamicRegistration", false}})},
                                  {"symbol", Json::object({{"dynamicRegistration", false},
                                                           {"symbolKind", symbolKinds}})},
                                  {"didChangeConfiguration",
                                   Json::object({{"dynamicRegistration", false}})},
                                  {"inlayHint", Json::object({{"refreshSupport", true}})},
                                  {"semanticTokens", Json::object({{"refreshSupport", true}})}});
    // Files changed on disk, for the watchers a server registers, only
    // from an owner that sends them: a server told the client watches
    // stops watching for itself (rust-analyzer does).
    if (fileWatching_)
        workspace.set("didChangeWatchedFiles", Json::object({{"dynamicRegistration", true},
                                                             {"relativePatternSupport", true}}));
    Json caps = Json::object({
        {"textDocument", textDocument},
        {"workspace", workspace},
        // $/progress, shown in the status bar while a server indexes.
        {"window", Json::object({{"workDoneProgress", true}})},
        {"general", Json::object({{"positionEncodings", Json::array().push("utf-16")}})},
    });
    Json params = Json::object({
        {"processId", processId},
        {"clientInfo", Json::object({{"name", clientName}})},
        {"rootUri", rootUri},
        {"rootPath", rootPath},
        {"workspaceFolders",
         Json::array().push(Json::object({{"uri", rootUri}, {"name", name}}))},
        {"capabilities", caps},
    });
    if (!initOptions_.isNull()) params.set("initializationOptions", initOptions_);
    rootPath_ = rootPath;
    state_ = State::Initializing;
    initializeId_ = nextId_++;
    handlers_[initializeId_] = nullptr;   // handled in handleMessage
    write(Json::object({{"jsonrpc", "2.0"}, {"id", initializeId_},
                        {"method", "initialize"}, {"params", params}}),
          true);
}

DocVersions Client::versions() const {
    DocVersions out;
    for (const auto& kv : docs_) out[kv.first] = kv.second.version;
    return out;
}

int Client::version(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it == docs_.end() ? 0 : it->second.version;
}

const std::u16string* Client::openText(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it == docs_.end() ? nullptr : &it->second.text.text();
}

const Client::Doc* Client::findDoc(const std::string& uri) const {
    auto it = docs_.find(uri);
    if (it != docs_.end()) return &it->second;
    const std::string path = pathFromUri(uri);
    if (path.empty()) return nullptr;
    for (const auto& kv : docs_)
        if (pathFromUri(kv.first) == path) return &kv.second;
    return nullptr;
}

const std::u16string* Client::textFor(const std::string& uri) const {
    const Doc* d = findDoc(uri);
    return d ? &d->text.text() : nullptr;
}

std::vector<std::string> Client::openUris() const {
    std::vector<std::string> out;
    for (const auto& kv : docs_) out.push_back(kv.first);
    return out;
}

void Client::didOpen(const std::string& uri, const std::string& languageId,
                     const std::string& text) {
    if (state_ == State::ShuttingDown || state_ == State::Exited) return;
    if (docs_.count(uri)) { didChange(uri, text); return; }
    Doc& d = docs_[uri];
    d.languageId = languageId;
    d.text = SyncedText(toUtf16(text));
    d.version = ++lastVersion_[uri];
    notify("textDocument/didOpen",
           Json::object({{"textDocument",
                          Json::object({{"uri", uri}, {"languageId", languageId},
                                        {"version", d.version}, {"text", text}})}}));
}

void Client::sendWhole(Doc& d, const std::string& uri, std::u16string text) {
    std::string utf8 = toUtf8(text);
    d.text = SyncedText(std::move(text));
    d.version = ++lastVersion_[uri];
    fullSent_++;
    notify("textDocument/didChange",
           Json::object({
               {"textDocument", Json::object({{"uri", uri}, {"version", d.version}})},
               {"contentChanges", Json::array().push(Json::object({{"text", std::move(utf8)}}))},
           }));
}

void Client::didChange(const std::string& uri, const std::string& text) {
    auto it = docs_.find(uri);
    if (it == docs_.end()) return;
    std::u16string now = toUtf16(text);
    if (it->second.text == now) return;
    sendWhole(it->second, uri, std::move(now));
}

int Client::syncKind() const {
    const Json& sync = capabilities_["textDocumentSync"];
    if (sync.isNumber()) return (int)sync.asInt();
    if (sync.isObject() && sync["change"].isNumber()) return (int)sync["change"].asInt();
    return 1;
}

void Client::didChange(const std::string& uri, const std::string& text,
                       const ChangeTracker& changes) {
    auto it = docs_.find(uri);
    if (it == docs_.end()) return;
    // The whole text is here, so the regions are checked against it first:
    // anything they do not account for sends the whole text.
    std::vector<ContentChange> unused;
    if (syncKind() == 2 && !changes.overflowed() &&
        !rangedChanges(toUtf8(it->second.text.text()), text, changes.regions(), unused)) {
        didChange(uri, text);
        return;
    }
    const std::u16string now = toUtf16(text);
    didChange(uri, now.size(), changes,
              [&now](size_t a, size_t n) { return now.substr(a, n); });
}

namespace {
bool isHigh(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLow(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
// Whether a boundary between `before` and `after` splits \r\n or a pair.
bool splits(char16_t before, char16_t after) {
    return (before == u'\r' && after == u'\n') || (isHigh(before) && isLow(after));
}
}  // namespace

void Client::didChange(const std::string& uri, size_t length, const ChangeTracker& changes,
                       const TextReader& read) {
    auto it = docs_.find(uri);
    if (it == docs_.end()) return;
    Doc& d = it->second;
    const std::u16string& old = d.text.text();
    auto whole = [&] {
        std::u16string now = read(0, length);
        if (d.text == now) return;
        sendWhole(d, uri, std::move(now));
    };
    const std::vector<ChangeTracker::Region>& regions = changes.regions();
    if (!changes.overflowed() && regions.empty()) {
        if (length != old.size()) whole();   // an edit the tracker never heard of
        return;
    }
    if (syncKind() != 2 || changes.overflowed()) {
        whole();
        return;
    }
    // The regions must add up: in order, apart, the text between them as
    // long in both, and what follows the last as long too.
    size_t lastOld = 0, lastNew = 0;
    for (const ChangeTracker::Region& r : regions) {
        if (r.oldEnd < r.oldStart || r.newEnd < r.newStart || r.oldStart < lastOld ||
            r.newStart < lastNew || r.oldStart - lastOld != r.newStart - lastNew ||
            r.oldEnd > old.size() || r.newEnd > length) {
            whole();
            return;
        }
        lastOld = r.oldEnd, lastNew = r.newEnd;
    }
    if (old.size() - lastOld != length - lastNew) {
        whole();
        return;
    }
    struct Placed {
        Range range;
        size_t oldA, oldB;
        std::u16string text;
    };
    std::vector<Placed> placed;
    placed.reserve(regions.size());
    auto oldSplits = [&](size_t at) {
        return at > 0 && at < old.size() && splits(old[at - 1], old[at]);
    };
    size_t bytes = 0;
    for (const ChangeTracker::Region& r : regions) {
        // The new text with a unit either side, for its boundaries.
        const size_t from = r.newStart > 0 ? r.newStart - 1 : 0;
        const size_t to = std::min(length, r.newEnd + 1);
        const std::u16string around = read(from, to - from);
        if (around.size() != to - from) {
            whole();
            return;
        }
        const size_t lead = r.newStart - from, n = r.newEnd - r.newStart;
        // No boundary, old or new, between \r and \n or inside a pair.
        const bool newStartSplits = lead == 1 && around.size() > 1 && splits(around[0], around[1]);
        const bool newEndSplits = lead + n >= 1 && lead + n < around.size() &&
                                  splits(around[lead + n - 1], around[lead + n]);
        if (oldSplits(r.oldStart) || oldSplits(r.oldEnd) || newStartSplits || newEndSplits) {
            whole();
            return;
        }
        Placed p;
        p.range = Range{d.text.position(r.oldStart), d.text.position(r.oldEnd)};
        p.oldA = r.oldStart, p.oldB = r.oldEnd;
        p.text = around.substr(lead, n);
        bytes += p.text.size() + 96;
        placed.push_back(std::move(p));
    }
    // Each entry costs its range besides its text: when they come to the
    // whole text's size, the whole text is as good and simpler to apply.
    if (bytes >= length) {
        whole();
        return;
    }
    // The last region first, so each range holds once the ones before it
    // are applied; the copy takes them in the same order.
    Json list = Json::array();
    for (size_t k = placed.size(); k-- > 0;) {
        Placed& p = placed[k];
        list.push(Json::object({{"range", Json::object({{"start", positionJson(p.range.start)},
                                                         {"end", positionJson(p.range.end)}})},
                                {"text", toUtf8(p.text)}}));
        d.text.replace(p.oldA, p.oldB, p.text);
    }
    d.version = ++lastVersion_[uri];
    rangedSent_++;
    notify("textDocument/didChange",
           Json::object({
               {"textDocument", Json::object({{"uri", uri}, {"version", d.version}})},
               {"contentChanges", std::move(list)},
           }));
    if (checkSync_) {
        std::u16string now = read(0, length);
        if (!(d.text == now)) {
            syncRepairs_++;
            sendWhole(d, uri, std::move(now));
        }
    }
}

void Client::didSave(const std::string& uri, const std::string& text) {
    if (!docs_.count(uri)) return;
    didChange(uri, text);
    Json params = Json::object({{"textDocument", Json::object({{"uri", uri}})}});
    const Json& save = capabilities_["textDocumentSync"]["save"];
    if (save.isObject() && save["includeText"].asBool()) params.set("text", text);
    notify("textDocument/didSave", params);
}

void Client::didClose(const std::string& uri) {
    if (!docs_.erase(uri)) return;
    notify("textDocument/didClose",
           Json::object({{"textDocument", Json::object({{"uri", uri}})}}));
}

int Client::request(const std::string& method, Json params, ResultHandler handler) {
    TakingHandler h;
    if (handler)
        h = [handler = std::move(handler)](Json& result, const Json& error) { handler(result, error); };
    return requestTaking(method, std::move(params), std::move(h));
}

int Client::requestTaking(const std::string& method, Json params, TakingHandler handler) {
    if (state_ == State::ShuttingDown || state_ == State::Exited) return 0;
    int id = nextId_++;
    handlers_[id] = std::move(handler);
    if (state_ != State::Ready) queuedIds_.push_back(id);
    write(Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"method", method},
                        {"params", std::move(params)}}));
    return id;
}

namespace {
Json docPosition(const std::string& uri, Position p) {
    return Json::object({{"textDocument", Json::object({{"uri", uri}})},
                         {"position", positionJson(p)}});
}
}  // namespace

int Client::completion(const std::string& uri, Position p, TakingHandler h,
                       const std::string& triggerCharacter) {
    Json params = docPosition(uri, p);
    Json context = Json::object({{"triggerKind", triggerCharacter.empty() ? 1 : 2}});
    if (!triggerCharacter.empty()) context.set("triggerCharacter", triggerCharacter);
    params.set("context", context);
    return requestTaking("textDocument/completion", params, std::move(h));
}

int Client::hover(const std::string& uri, Position p, ResultHandler h) {
    return request("textDocument/hover", docPosition(uri, p), std::move(h));
}

int Client::definition(const std::string& uri, Position p, ResultHandler h) {
    return request("textDocument/definition", docPosition(uri, p), std::move(h));
}

int Client::references(const std::string& uri, Position p, bool includeDeclaration,
                       ResultHandler h) {
    Json params = docPosition(uri, p);
    params.set("context", Json::object({{"includeDeclaration", includeDeclaration}}));
    return request("textDocument/references", params, std::move(h));
}

int Client::foldingRanges(const std::string& uri, ResultHandler h) {
    return request("textDocument/foldingRange",
                   Json::object({{"textDocument", Json::object({{"uri", uri}})}}), std::move(h));
}

int Client::semanticTokensFull(const std::string& uri, TakingHandler h) {
    return requestTaking("textDocument/semanticTokens/full",
                         Json::object({{"textDocument", Json::object({{"uri", uri}})}}),
                         std::move(h));
}

int Client::semanticTokensDelta(const std::string& uri, const std::string& previousResultId,
                                TakingHandler h) {
    return requestTaking("textDocument/semanticTokens/full/delta",
                         Json::object({{"textDocument", Json::object({{"uri", uri}})},
                                       {"previousResultId", previousResultId}}),
                         std::move(h));
}

static Json formattingOptions(int tabSize, bool insertSpaces) {
    return Json::object({{"tabSize", tabSize > 0 ? tabSize : 4}, {"insertSpaces", insertSpaces}});
}

int Client::formatting(const std::string& uri, int tabSize, bool insertSpaces,
                       ResultHandler h) {
    return request("textDocument/formatting",
                   Json::object({{"textDocument", Json::object({{"uri", uri}})},
                                 {"options", formattingOptions(tabSize, insertSpaces)}}),
                   std::move(h));
}

int Client::rangeFormatting(const std::string& uri, Range range, int tabSize,
                            bool insertSpaces, ResultHandler h) {
    return request("textDocument/rangeFormatting",
                   Json::object({{"textDocument", Json::object({{"uri", uri}})},
                                 {"range", Json::object({{"start", positionJson(range.start)},
                                                         {"end", positionJson(range.end)}})},
                                 {"options", formattingOptions(tabSize, insertSpaces)}}),
                   std::move(h));
}

int Client::prepareRename(const std::string& uri, Position p, ResultHandler h) {
    return request("textDocument/prepareRename", docPosition(uri, p), std::move(h));
}

int Client::rename(const std::string& uri, Position p, const std::string& newName,
                   ResultHandler h) {
    Json params = docPosition(uri, p);
    params.set("newName", newName);
    return request("textDocument/rename", params, std::move(h));
}

int Client::codeActions(const std::string& uri, Range range, Json diagnostics,
                        ResultHandler h, const std::vector<std::string>& only) {
    if (!diagnostics.isArray()) diagnostics = Json::array();
    Json context = Json::object({{"diagnostics", diagnostics}, {"triggerKind", 1}});
    if (!only.empty()) {
        Json kinds = Json::array();
        for (const std::string& k : only) kinds.push(k);
        context.set("only", kinds);
    }
    Json params = Json::object({
        {"textDocument", Json::object({{"uri", uri}})},
        {"range", Json::object({{"start", positionJson(range.start)},
                                {"end", positionJson(range.end)}})},
        {"context", context},
    });
    return request("textDocument/codeAction", params, std::move(h));
}

int Client::resolveCodeAction(const CodeAction& action, ResultHandler h) {
    if (!canResolveCodeAction() || !action.raw) return 0;
    return request("codeAction/resolve", *action.raw, std::move(h));
}

int Client::documentSymbols(const std::string& uri, ResultHandler h) {
    return request("textDocument/documentSymbol",
                   Json::object({{"textDocument", Json::object({{"uri", uri}})}}), std::move(h));
}

int Client::workspaceSymbols(const std::string& query, ResultHandler h) {
    return request("workspace/symbol", Json::object({{"query", query}}), std::move(h));
}

int Client::inlayHints(const std::string& uri, Range range, ResultHandler h) {
    Json params = Json::object({
        {"textDocument", Json::object({{"uri", uri}})},
        {"range", Json::object({{"start", positionJson(range.start)},
                                {"end", positionJson(range.end)}})},
    });
    return request("textDocument/inlayHint", params, std::move(h));
}

int Client::executeCommand(const Json& command, ResultHandler h) {
    Json params = Json::object({{"command", command["command"].asString()}});
    if (command["arguments"].isArray()) params.set("arguments", command["arguments"]);
    return request("workspace/executeCommand", params, std::move(h));
}

namespace {
// A provider capability: true, or an object of options.
bool provides(const Json& cap) { return cap.isObject() || cap.asBool(); }
}  // namespace

bool Client::hasRename() const { return provides(capabilities_["renameProvider"]); }
bool Client::canPrepareRename() const {
    return capabilities_["renameProvider"]["prepareProvider"].asBool();
}
bool Client::hasReferences() const { return provides(capabilities_["referencesProvider"]); }
bool Client::hasCodeActions() const { return provides(capabilities_["codeActionProvider"]); }
bool Client::canResolveCodeAction() const {
    return capabilities_["codeActionProvider"]["resolveProvider"].asBool();
}

bool Client::canResolveCompletion() const {
    return capabilities_["completionProvider"]["resolveProvider"].asBool();
}

int Client::resolveCompletion(const CompletionItem& item, ResultHandler h) {
    if (!canResolveCompletion() || !item.raw) return 0;
    return request("completionItem/resolve", *item.raw, std::move(h));
}

int Client::signatureHelp(const std::string& uri, Position p, ResultHandler h,
                          const std::string& triggerCharacter, bool isRetrigger) {
    Json params = docPosition(uri, p);
    int kind = isRetrigger && triggerCharacter.empty() ? 3
             : triggerCharacter.empty() ? 1 : 2;
    Json context = Json::object({{"triggerKind", kind}, {"isRetrigger", isRetrigger}});
    if (!triggerCharacter.empty()) context.set("triggerCharacter", triggerCharacter);
    params.set("context", context);
    return request("textDocument/signatureHelp", params, std::move(h));
}

void Client::didChangeConfiguration(Json settings) {
    settings_ = std::move(settings);
    notify("workspace/didChangeConfiguration",
           Json::object({{"settings", settings_.isNull() ? Json::object() : settings_}}));
}

void Client::cancel(int id) {
    if (!handlers_.erase(id)) return;
    auto q = std::find(queuedIds_.begin(), queuedIds_.end(), id);
    if (q != queuedIds_.end()) return;   // not sent yet; its answer is ignored
    notify("$/cancelRequest", Json::object({{"id", id}}));
}

void Client::shutdown(std::function<void()> done) {
    if (state_ == State::Exited || state_ == State::Idle) {
        state_ = State::Exited;
        if (done) done();
        return;
    }
    if (state_ == State::ShuttingDown) return;
    if (state_ == State::Initializing) {
        // No handshake yet, so no shutdown to ask for: just exit.
        exitNow();
        if (done) done();
        return;
    }
    shutdownDone_ = std::move(done);
    state_ = State::ShuttingDown;
    shutdownId_ = nextId_++;
    handlers_.clear();
    handlers_[shutdownId_] = nullptr;
    write(Json::object({{"jsonrpc", "2.0"}, {"id", shutdownId_}, {"method", "shutdown"}}),
          true);
}

void Client::exitNow() {
    if (state_ == State::Exited) return;
    if (state_ == State::Ready) {
        shutdownId_ = nextId_++;
        write(Json::object({{"jsonrpc", "2.0"}, {"id", shutdownId_},
                            {"method", "shutdown"}}),
              true);
    }
    write(Json::object({{"jsonrpc", "2.0"}, {"method", "exit"}}), true);
    state_ = State::Exited;
    handlers_.clear();
    queue_.clear();
}

namespace {
std::vector<std::string> strings(const Json& list) {
    std::vector<std::string> out;
    for (const Json& t : list.elements())
        if (t.isString()) out.push_back(t.asString());
    return out;
}
}  // namespace

std::vector<std::string> Client::completionTriggers() const {
    return strings(capabilities_["completionProvider"]["triggerCharacters"]);
}

bool Client::hasSignatureHelp() const {
    return capabilities_["signatureHelpProvider"].isObject();
}

std::vector<std::string> Client::signatureTriggers() const {
    return strings(capabilities_["signatureHelpProvider"]["triggerCharacters"]);
}

std::vector<std::string> Client::signatureRetriggers() const {
    return strings(capabilities_["signatureHelpProvider"]["retriggerCharacters"]);
}

void Client::receive(const char* data, size_t n) {
    reader_.feed(data, n);
    Incoming in;
    while (state_ != State::Exited && reader_.next(in)) deliver(in);
}

void Client::deliver(Incoming& in) {
    if (state_ == State::Exited) return;
    if (!in.error.empty()) {
        if (onProtocolError) onProtocolError("unreadable message: " + in.error);
        return;
    }
    handleMessage(in.message);
}

double Client::now() const {
    if (clock_) return clock_();
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const Progress* Client::progress() const {
    if (progress_.empty()) return nullptr;
    const double t = now();
    const Progress* best = nullptr;
    for (const auto& kv : progress_) {
        if (t - kv.second.heard > kProgressStale) continue;
        if (!best || kv.second.begun > best->begun) best = &kv.second;
    }
    return best;
}

void Client::handleProgress(const Json& params) {
    static std::atomic<uint64_t> begun{0};
    const Json& value = params["value"];
    const std::string& kind = value["kind"].asString();
    if (!params.has("token") || kind.empty()) return;   // a partial result, not ours
    const std::string token = params["token"].dump();
    auto percentage = [&](int keep) {
        if (!value["percentage"].isNumber()) return keep;
        double v = value["percentage"].asNumber();
        return v < 0 ? 0 : v > 100 ? 100 : (int)v;
    };
    if (kind == "begin") {
        Progress& p = progress_[token];
        p = Progress();
        p.token = token;
        p.title = value["title"].asString();
        p.message = value["message"].asString();
        p.percentage = percentage(-1);
        p.begun = ++begun;
        p.heard = now();
    } else if (kind == "report") {
        auto it = progress_.find(token);
        if (it == progress_.end()) return;   // never begun: nothing to update
        it->second.heard = now();
        if (value["message"].isString()) it->second.message = value["message"].asString();
        it->second.percentage = percentage(it->second.percentage);
    } else if (kind == "end") {
        if (!progress_.erase(token)) return;
    } else {
        return;
    }
    if (onProgress) onProgress();
}

void Client::handleMessage(Json& msg) {
    if (!msg.isObject()) return;
    bool hasMethod = msg["method"].isString();
    bool hasId = msg.has("id") && !msg["id"].isNull();

    if (hasMethod && hasId) { handleServerRequest(msg); return; }

    if (hasMethod) {
        const std::string& method = msg["method"].asString();
        const Json& params = msg["params"];
        if (method == "textDocument/publishDiagnostics") {
            std::string uri;
            int version = -1;
            std::vector<Diagnostic> diags = parseDiagnostics(params, &uri, &version);
            // Computed on a text the client has since replaced: their
            // positions would land on the wrong characters, and the server
            // publishes again for the text it has now.
            const Doc* doc = findDoc(uri);
            if (doc && version >= 0 && version < doc->version) {
                staleDiagnostics_++;
                return;
            }
            if (onDiagnostics) onDiagnostics(uri, diags);
        } else if (method == "window/showMessage") {
            if (onShowMessage)
                onShowMessage((int)params["type"].asInt(), params["message"].asString());
        } else if (method == "$/progress") {
            handleProgress(params);
        }
        return;   // logMessage, telemetry: nothing to do
    }

    if (!hasId || !msg["id"].isNumber()) return;
    int id = (int)msg["id"].asInt();
    Json none;
    Json* resultMember = msg.member("result");
    Json& result = resultMember ? *resultMember : none;
    const Json& error = msg["error"];

    if (id == initializeId_ && state_ == State::Initializing) {
        handlers_.erase(id);
        if (!error.isNull()) {
            state_ = State::Exited;
            queue_.clear();
            const std::string message = error["message"].asString();
            if (onProtocolError) onProtocolError("initialize failed: " + message);
            if (onInitializeFailed) onInitializeFailed(message);
            return;
        }
        capabilities_ = result["capabilities"];
        serverName_ = result["serverInfo"]["name"].asString();
        state_ = State::Ready;
        write(Json::object({{"jsonrpc", "2.0"}, {"method", "initialized"},
                            {"params", Json::object()}}));
        // Settings go before anything queued, so the first didOpen is
        // already analysed with them (pylsp reads settings only this way).
        if (settings_.isObject() && settings_.size() > 0)
            notify("workspace/didChangeConfiguration",
                   Json::object({{"settings", settings_}}));
        std::vector<std::string> queued;
        queued.swap(queue_);
        queuedIds_.clear();
        for (const std::string& body : queued) send_(Framer::frame(body));
        if (onReady) onReady();
        return;
    }

    if (id == shutdownId_ && state_ == State::ShuttingDown) {
        handlers_.erase(id);
        write(Json::object({{"jsonrpc", "2.0"}, {"method", "exit"}}), true);
        state_ = State::Exited;
        auto done = std::move(shutdownDone_);
        shutdownDone_ = nullptr;
        if (done) done();
        return;
    }

    auto it = handlers_.find(id);
    if (it == handlers_.end()) return;   // cancelled, or never ours
    TakingHandler h = std::move(it->second);
    handlers_.erase(it);
    if (h) h(result, error);
}

std::vector<FileWatcher> Client::fileWatchers() const {
    std::vector<FileWatcher> out;
    for (const auto& kv : watchers_) out.insert(out.end(), kv.second.begin(), kv.second.end());
    return out;
}

std::vector<std::string> Client::watchedBases() const {
    std::vector<std::string> out;
    for (const FileWatcher& w : fileWatchers())
        if (!w.base.empty() && std::find(out.begin(), out.end(), w.base) == out.end())
            out.push_back(w.base);
    return out;
}

size_t Client::didChangeWatchedFiles(const std::vector<FileChange>& changes) {
    if (state_ != State::Ready || watchers_.empty()) return 0;
    const std::vector<FileChange> wanted = matchWatched(watchTarget(), changes);
    if (wanted.empty()) return 0;
    send_(watchedFilesMessage(wanted));
    watchedSent_ += wanted.size();
    return wanted.size();
}

WatchTarget Client::watchTarget() const {
    WatchTarget t;
    t.generation = watchersGeneration_;
    t.watchers = fileWatchers();
    t.root = rootPath_;
    return t;
}

bool Client::sendWatchedBatch(const WatchedBatch& batch) {
    if (state_ != State::Ready || batch.generation != watchersGeneration_) return false;
    if (watchers_.empty()) return batch.count == 0;
    if (batch.count == 0 || batch.message.empty()) return true;
    send_(batch.message);
    watchedSent_ += batch.count;
    return true;
}

void Client::handleServerRequest(const Json& msg) {
    const std::string& method = msg["method"].asString();
    const Json& id = msg["id"];
    if (method == "workspace/configuration") {
        // One entry per requested item, from the settings by section; null
        // means "use your defaults".
        Json out = Json::array();
        for (const Json& item : msg["params"]["items"].elements())
            out.push(configurationSection(settings_, item["section"].asString()));
        respond(id, out);
    } else if (method == "workspace/applyEdit") {
        if (!onApplyEdit) {
            respond(id, Json::object({{"applied", false},
                                      {"failureReason", "the client cannot apply edits now"}}));
            return;
        }
        Json theId = id;
        auto answered = std::make_shared<bool>(false);
        onApplyEdit(msg["params"], [this, theId, answered](bool applied,
                                                         const std::string& reason) {
            if (*answered) return;
            *answered = true;
            Json result = Json::object({{"applied", applied}});
            if (!applied && !reason.empty()) result.set("failureReason", reason);
            respond(theId, result);
        });
    } else if (method == "workspace/inlayHint/refresh") {
        respond(id, Json());
        if (onInlayHintRefresh) onInlayHintRefresh();
    } else if (method == "workspace/workspaceFolders") {
        std::string uri = uriFromPath(rootPath_);
        respond(id, Json::array().push(Json::object({{"uri", uri}, {"name", rootPath_}})));
    } else if (method == "client/registerCapability" ||
               method == "client/unregisterCapability") {
        // Only watched files are registered; anything else is acknowledged
        // and ignored, as before (the server then works without it).
        bool changed = false;
        if (method == "client/registerCapability") {
            for (const Json& r : msg["params"]["registrations"].elements()) {
                if (r["method"].asString() != "workspace/didChangeWatchedFiles") continue;
                watchers_[r["id"].asString()] = parseFileWatchers(r["registerOptions"]);
                changed = true;
            }
        } else {
            // The specification spells it "unregisterations".
            const Json& p = msg["params"];
            const Json& list = p.has("unregisterations") ? p["unregisterations"]
                                                         : p["unregistrations"];
            for (const Json& r : list.elements())
                if (watchers_.erase(r["id"].asString())) changed = true;
        }
        respond(id, Json());
        if (changed) watchersGeneration_++;
        if (changed && onWatchersChanged) onWatchersChanged();
    } else if (method == "workspace/semanticTokens/refresh") {
        respond(id, Json());
        if (onSemanticTokensRefresh) onSemanticTokensRefresh();
    } else if (method == "window/workDoneProgress/create" ||
               method == "window/showMessageRequest" ||
               method == "workspace/diagnostic/refresh" ||
               method == "workspace/codeLens/refresh") {
        respond(id, Json());
    } else {
        write(Json::object({{"jsonrpc", "2.0"}, {"id", id},
                            {"error", Json::object({{"code", -32601},
                                                    {"message", "method not found"}})}}),
              true);
    }
}

}  // namespace Lsp
