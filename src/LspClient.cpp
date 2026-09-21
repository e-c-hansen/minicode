// LspClient.cpp — see LspClient.h.
#include "LspClient.h"
#include <algorithm>
#include <cctype>

namespace Lsp {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// clangd marks items whose #include it would add with a leading bullet, and
// pads the others with a space so the labels line up. Neither is part of the
// name.
std::string cleanLabel(const std::string& label) {
    std::string s = trim(label);
    static const std::string bullet = "\xE2\x80\xA2";   // U+2022
    if (s.compare(0, bullet.size(), bullet) == 0) s = trim(s.substr(bullet.size()));
    return s;
}

}  // namespace

// ------------------------------------------------------------------ framing
std::string Framer::frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

void Framer::feed(const char* data, size_t n) {
    // Drop what has been consumed before growing, so a long session doesn't
    // keep every message it ever read.
    if (pos_ > 0 && pos_ >= buf_.size() / 2) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    buf_.append(data, n);
}

bool Framer::next(std::string& body) {
    while (true) {
        size_t headerEnd = buf_.find("\r\n\r\n", pos_);
        if (headerEnd == std::string::npos) return false;
        // Parse the header lines for Content-Length.
        long long length = -1;
        size_t line = pos_;
        while (line < headerEnd) {
            size_t eol = buf_.find("\r\n", line);
            if (eol == std::string::npos || eol > headerEnd) eol = headerEnd;
            std::string h = buf_.substr(line, eol - line);
            size_t colon = h.find(':');
            if (colon != std::string::npos &&
                lower(trim(h.substr(0, colon))) == "content-length") {
                std::string v = trim(h.substr(colon + 1));
                if (!v.empty() && std::all_of(v.begin(), v.end(), [](char c) {
                        return c >= '0' && c <= '9';
                    }))
                    length = std::stoll(v);
            }
            line = eol + 2;
        }
        size_t bodyStart = headerEnd + 4;
        if (length < 0) {
            // A header block with no usable length: skip it rather than stall.
            pos_ = bodyStart;
            continue;
        }
        if (buf_.size() - bodyStart < (size_t)length) return false;
        body = buf_.substr(bodyStart, (size_t)length);
        pos_ = bodyStart + (size_t)length;
        return true;
    }
}

// ------------------------------------------------------ text and positions
size_t offsetForPosition(const std::u16string& text, Position p) {
    size_t i = 0;
    int line = 0;
    while (line < p.line) {
        while (i < text.size() && text[i] != u'\n' && text[i] != u'\r') i++;
        if (i >= text.size()) return text.size();
        if (text[i] == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') i++;
        i++;
        line++;
    }
    size_t end = i;
    while (end < text.size() && text[end] != u'\n' && text[end] != u'\r') end++;
    size_t ch = p.character < 0 ? 0 : (size_t)p.character;
    return std::min(i + ch, end);
}

Position positionForOffset(const std::u16string& text, size_t offset) {
    offset = std::min(offset, text.size());
    Position p;
    size_t lineStart = 0;
    for (size_t i = 0; i < offset; i++) {
        char16_t c = text[i];
        if (c == u'\n' || c == u'\r') {
            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') {
                if (i + 1 >= offset) break;   // offset sits between \r and \n
                i++;
            }
            p.line++;
            lineStart = i + 1;
        }
    }
    p.character = (int)(offset - lineStart);
    return p;
}

std::u16string toUtf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { out += (char16_t)0xFFFD; i++; continue; }
        if (i + n > s.size()) { out += (char16_t)0xFFFD; break; }
        bool bad = false;
        for (size_t k = 1; k < n; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { out += (char16_t)0xFFFD; i++; continue; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
    }
    return out;
}

std::string toUtf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string uriFromPath(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (char ch : path) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string pathFromUri(const std::string& uri) {
    if (lower(uri.substr(0, 7)) != "file://") return "";
    std::string rest = uri.substr(7);
    if (lower(rest.substr(0, 9)) == "localhost") rest = rest.substr(9);
    if (rest.empty() || rest[0] != '/') return "";
    auto val = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < rest.size(); i++) {
        if (rest[i] == '%' && i + 2 < rest.size() &&
            val(rest[i + 1]) >= 0 && val(rest[i + 2]) >= 0) {
            out += (char)(val(rest[i + 1]) * 16 + val(rest[i + 2]));
            i += 2;
        } else if (rest[i] == '?' || rest[i] == '#') {
            break;   // query or fragment: not part of the path
        } else {
            out += rest[i];
        }
    }
    return out;
}

// ---------------------------------------------------------- server choice
bool languageForExtension(const std::string& extIn, Language& out) {
    std::string ext = lower(extIn);
    struct Row { const char* ext; const char* server; const char* id; };
    static const Row rows[] = {
        {"c", "cpp", "c"},
        {"h", "cpp", "cpp"},
        {"cc", "cpp", "cpp"},   {"cpp", "cpp", "cpp"}, {"cxx", "cpp", "cpp"},
        {"c++", "cpp", "cpp"},  {"hpp", "cpp", "cpp"}, {"hh", "cpp", "cpp"},
        {"hxx", "cpp", "cpp"},  {"ipp", "cpp", "cpp"}, {"inl", "cpp", "cpp"},
        {"m", "cpp", "objective-c"},
        {"mm", "cpp", "objective-cpp"},
        {"py", "python", "python"}, {"pyi", "python", "python"},
        {"go", "go", "go"},
        {"rs", "rust", "rust"},
        {"ts", "typescript", "typescript"},
        {"mts", "typescript", "typescript"},
        {"cts", "typescript", "typescript"},
        {"tsx", "typescript", "typescriptreact"},
        {"js", "typescript", "javascript"},
        {"mjs", "typescript", "javascript"},
        {"cjs", "typescript", "javascript"},
        {"jsx", "typescript", "javascriptreact"},
    };
    for (const Row& r : rows)
        if (ext == r.ext) {
            out.server = r.server;
            out.languageId = r.id;
            return true;
        }
    return false;
}

std::vector<std::string> defaultCommands(const std::string& server) {
    if (server == "cpp") return {"clangd"};
    if (server == "python") return {"pyright-langserver --stdio", "pylsp"};
    if (server == "go") return {"gopls"};
    if (server == "rust") return {"rust-analyzer"};
    if (server == "typescript") return {"typescript-language-server --stdio"};
    return {};
}

std::string serverDisplayName(const std::string& server) {
    if (server == "cpp") return "C/C++";
    if (server == "python") return "Python";
    if (server == "go") return "Go";
    if (server == "rust") return "Rust";
    if (server == "typescript") return "JavaScript/TypeScript";
    return server;
}

std::vector<std::string> splitCommand(const std::string& command) {
    std::vector<std::string> words;
    std::string cur;
    bool inWord = false;
    char quote = 0;
    for (size_t i = 0; i < command.size(); i++) {
        char c = command[i];
        if (quote) {
            if (c == quote) { quote = 0; continue; }
            if (c == '\\' && quote == '"' && i + 1 < command.size()) { cur += command[++i]; continue; }
            cur += c;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; inWord = true; continue; }
        if (c == '\\' && i + 1 < command.size()) { cur += command[++i]; inWord = true; continue; }
        if (std::isspace((unsigned char)c)) {
            if (inWord) { words.push_back(cur); cur.clear(); inWord = false; }
            continue;
        }
        cur += c;
        inWord = true;
    }
    if (inWord) words.push_back(cur);
    return words;
}

// ------------------------------------------------------------ result parsing
Json positionJson(Position p) {
    return Json::object({{"line", p.line}, {"character", p.character}});
}

Position parsePosition(const Json& j) {
    Position p;
    p.line = (int)j["line"].asInt();
    p.character = (int)j["character"].asInt();
    return p;
}

Range parseRange(const Json& j) {
    return Range{parsePosition(j["start"]), parsePosition(j["end"])};
}

std::vector<Diagnostic> parseDiagnostics(const Json& params, std::string* uri) {
    if (uri) *uri = params["uri"].asString();
    std::vector<Diagnostic> out;
    for (const Json& d : params["diagnostics"].elements()) {
        Diagnostic diag;
        diag.range = parseRange(d["range"]);
        int sev = (int)d["severity"].asInt(1);
        if (sev < 1 || sev > 4) sev = 1;   // "if omitted, the client decides"
        diag.severity = (Severity)sev;
        diag.message = d["message"].asString();
        diag.source = d["source"].asString();
        const Json& code = d["code"];
        if (code.isString()) diag.code = code.asString();
        else if (code.isNumber()) diag.code = std::to_string(code.asInt());
        out.push_back(diag);
    }
    return out;
}

std::vector<CompletionItem> parseCompletion(const Json& result, bool* incomplete) {
    if (incomplete) *incomplete = false;
    const Json* list = &result;
    if (result.isObject()) {
        if (incomplete) *incomplete = result["isIncomplete"].asBool();
        list = &result["items"];
    }
    std::vector<CompletionItem> out;
    for (const Json& it : list->elements()) {
        CompletionItem c;
        c.label = cleanLabel(it["label"].asString());
        if (c.label.empty()) continue;
        c.detail = it["detail"].asString();
        c.insertText = it["insertText"].asString();
        c.filterText = it["filterText"].asString();
        c.sortText = it["sortText"].asString();
        c.kind = (int)it["kind"].asInt();
        const Json& edit = it["textEdit"];
        if (edit.isObject()) {
            c.hasEdit = true;
            c.newText = edit["newText"].asString();
            if (edit.has("range")) c.editRange = parseRange(edit["range"]);
            else c.editRange = parseRange(edit["insert"]);   // InsertReplaceEdit
        }
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const CompletionItem& a,
                                                const CompletionItem& b) {
        const std::string& ka = a.sortText.empty() ? a.label : a.sortText;
        const std::string& kb = b.sortText.empty() ? b.label : b.sortText;
        return ka < kb;
    });
    return out;
}

namespace {

// Markdown to readable text: keep code fence contents, drop the fence lines,
// and unescape the backslash escapes servers put before punctuation.
std::string markdownToText(const std::string& md) {
    std::string out, line;
    for (size_t i = 0; i <= md.size(); i++) {
        if (i < md.size() && md[i] != '\n') { line += md[i]; continue; }
        if (trim(line).compare(0, 3, "```") != 0) {
            std::string un;
            for (size_t k = 0; k < line.size(); k++) {
                if (line[k] == '\\' && k + 1 < line.size() &&
                    std::ispunct((unsigned char)line[k + 1])) {
                    un += line[++k];
                } else {
                    un += line[k];
                }
            }
            out += un;
            out += '\n';
        }
        line.clear();
    }
    return out;
}

std::string markedToText(const Json& j) {
    if (j.isString()) return markdownToText(j.asString());
    if (j.isObject()) {
        if (j.has("kind") && j["kind"].asString() == "markdown")
            return markdownToText(j["value"].asString());
        return j["value"].asString();   // plaintext, or {language, value}
    }
    return "";
}

}  // namespace

std::string parseHover(const Json& result) {
    const Json& contents = result["contents"];
    std::string text;
    if (contents.isArray()) {
        for (const Json& part : contents.elements()) {
            std::string t = trim(markedToText(part));
            if (t.empty()) continue;
            if (!text.empty()) text += "\n\n";
            text += t;
        }
    } else {
        text = markedToText(contents);
    }
    // Collapse runs of blank lines, then trim.
    std::string out;
    int newlines = 0;
    for (char c : text) {
        if (c == '\n') { if (++newlines > 2) continue; }
        else newlines = 0;
        out += c;
    }
    return trim(out);
}

std::vector<Location> parseLocations(const Json& result) {
    std::vector<Location> out;
    auto one = [&](const Json& j) {
        if (!j.isObject()) return;
        Location l;
        if (j.has("targetUri")) {   // LocationLink
            l.uri = j["targetUri"].asString();
            l.range = parseRange(j.has("targetSelectionRange")
                                     ? j["targetSelectionRange"] : j["targetRange"]);
        } else {
            l.uri = j["uri"].asString();
            l.range = parseRange(j["range"]);
        }
        if (!l.uri.empty()) out.push_back(l);
    };
    if (result.isArray()) for (const Json& j : result.elements()) one(j);
    else one(result);
    return out;
}

std::vector<CompletionItem> filterCompletions(const std::vector<CompletionItem>& items,
                                              const std::string& prefix) {
    if (prefix.empty()) return items;
    std::string p = lower(prefix);
    std::vector<CompletionItem> starts, contains;
    for (const CompletionItem& it : items) {
        std::string key = lower(it.filterKey());
        if (key.compare(0, p.size(), p) == 0) { starts.push_back(it); continue; }
        size_t k = 0;
        for (char c : key)
            if (k < p.size() && c == p[k]) k++;
        if (k == p.size()) contains.push_back(it);
    }
    starts.insert(starts.end(), contains.begin(), contains.end());
    return starts;
}

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
        {"snippetSupport", false},
        {"insertReplaceSupport", false},
        {"documentationFormat", Json::array().push("plaintext")},
    });
    Json textDocument = Json::object({
        {"synchronization", Json::object({{"didSave", true},
                                          {"dynamicRegistration", false}})},
        {"completion", Json::object({{"completionItem", completionItem},
                                     {"contextSupport", true}})},
        {"hover", Json::object({{"contentFormat",
                                 Json::array().push("plaintext").push("markdown")}})},
        {"definition", Json::object({{"linkSupport", true}})},
        {"publishDiagnostics", Json::object({{"relatedInformation", false}})},
    });
    Json caps = Json::object({
        {"textDocument", textDocument},
        {"workspace", Json::object({{"workspaceFolders", true},
                                    {"configuration", true}})},
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
    rootPath_ = rootPath;
    state_ = State::Initializing;
    initializeId_ = nextId_++;
    handlers_[initializeId_] = nullptr;   // handled in handleMessage
    write(Json::object({{"jsonrpc", "2.0"}, {"id", initializeId_},
                        {"method", "initialize"}, {"params", params}}),
          true);
}

int Client::version(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it == docs_.end() ? 0 : it->second.version;
}

const std::string* Client::openText(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it == docs_.end() ? nullptr : &it->second.text;
}

void Client::didOpen(const std::string& uri, const std::string& languageId,
                     const std::string& text) {
    if (state_ == State::ShuttingDown || state_ == State::Exited) return;
    if (docs_.count(uri)) { didChange(uri, text); return; }
    Doc& d = docs_[uri];
    d.languageId = languageId;
    d.text = text;
    d.version = 1;
    notify("textDocument/didOpen",
           Json::object({{"textDocument",
                          Json::object({{"uri", uri}, {"languageId", languageId},
                                        {"version", d.version}, {"text", text}})}}));
}

void Client::didChange(const std::string& uri, const std::string& text) {
    auto it = docs_.find(uri);
    if (it == docs_.end() || it->second.text == text) return;
    Doc& d = it->second;
    d.text = text;
    d.version++;
    notify("textDocument/didChange",
           Json::object({
               {"textDocument", Json::object({{"uri", uri}, {"version", d.version}})},
               {"contentChanges", Json::array().push(Json::object({{"text", text}}))},
           }));
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

int Client::completion(const std::string& uri, Position p, ResultHandler h,
                       const std::string& triggerCharacter) {
    Json params = docPosition(uri, p);
    Json context = Json::object({{"triggerKind", triggerCharacter.empty() ? 1 : 2}});
    if (!triggerCharacter.empty()) context.set("triggerCharacter", triggerCharacter);
    params.set("context", context);
    return request("textDocument/completion", params, std::move(h));
}

int Client::hover(const std::string& uri, Position p, ResultHandler h) {
    return request("textDocument/hover", docPosition(uri, p), std::move(h));
}

int Client::definition(const std::string& uri, Position p, ResultHandler h) {
    return request("textDocument/definition", docPosition(uri, p), std::move(h));
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

std::vector<std::string> Client::completionTriggers() const {
    std::vector<std::string> out;
    for (const Json& t : capabilities_["completionProvider"]["triggerCharacters"].elements())
        if (t.isString()) out.push_back(t.asString());
    return out;
}

void Client::receive(const char* data, size_t n) {
    framer_.feed(data, n);
    std::string body;
    while (framer_.next(body)) {
        Json msg;
        std::string err;
        if (!Json::parse(body, msg, &err)) {
            if (onProtocolError) onProtocolError("unreadable message: " + err);
            continue;
        }
        handleMessage(msg);
        if (state_ == State::Exited) return;
    }
}

void Client::handleMessage(const Json& msg) {
    if (!msg.isObject()) return;
    bool hasMethod = msg["method"].isString();
    bool hasId = msg.has("id") && !msg["id"].isNull();

    if (hasMethod && hasId) { handleServerRequest(msg); return; }

    if (hasMethod) {
        const std::string& method = msg["method"].asString();
        const Json& params = msg["params"];
        if (method == "textDocument/publishDiagnostics") {
            std::string uri;
            std::vector<Diagnostic> diags = parseDiagnostics(params, &uri);
            if (onDiagnostics) onDiagnostics(uri, diags);
        } else if (method == "window/showMessage") {
            if (onShowMessage)
                onShowMessage((int)params["type"].asInt(), params["message"].asString());
        }
        return;   // logMessage, progress, telemetry: nothing to do
    }

    if (!hasId || !msg["id"].isNumber()) return;
    int id = (int)msg["id"].asInt();
    const Json& result = msg["result"];
    const Json& error = msg["error"];

    if (id == initializeId_ && state_ == State::Initializing) {
        handlers_.erase(id);
        if (!error.isNull()) {
            state_ = State::Exited;
            queue_.clear();
            if (onProtocolError)
                onProtocolError("initialize failed: " + error["message"].asString());
            return;
        }
        capabilities_ = result["capabilities"];
        serverName_ = result["serverInfo"]["name"].asString();
        state_ = State::Ready;
        write(Json::object({{"jsonrpc", "2.0"}, {"method", "initialized"},
                            {"params", Json::object()}}));
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
    ResultHandler h = std::move(it->second);
    handlers_.erase(it);
    if (h) h(result, error);
}

void Client::handleServerRequest(const Json& msg) {
    const std::string& method = msg["method"].asString();
    const Json& id = msg["id"];
    if (method == "workspace/configuration") {
        // One entry per requested item; null means "use your defaults".
        Json out = Json::array();
        for (size_t i = 0; i < msg["params"]["items"].size(); i++) out.push(Json());
        respond(id, out);
    } else if (method == "workspace/workspaceFolders") {
        std::string uri = uriFromPath(rootPath_);
        respond(id, Json::array().push(Json::object({{"uri", uri}, {"name", rootPath_}})));
    } else if (method == "window/workDoneProgress/create" ||
               method == "client/registerCapability" ||
               method == "client/unregisterCapability" ||
               method == "window/showMessageRequest" ||
               method == "workspace/diagnostic/refresh" ||
               method == "workspace/semanticTokens/refresh" ||
               method == "workspace/inlayHint/refresh" ||
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
