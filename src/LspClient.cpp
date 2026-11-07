// LspClient.cpp — see LspClient.h.
#include "LspClient.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <set>
#include <tuple>

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

namespace {
const long long kMaxMessage = 1LL << 30;
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
                // Digits only, and at most 1 GiB: a longer number (std::stoll
                // threw past 19 digits) is a broken header, skipped below.
                std::string v = trim(h.substr(colon + 1));
                long long n = v.empty() ? -1 : 0;
                for (char c : v) {
                    if (c < '0' || c > '9' || n > kMaxMessage) { n = -1; break; }
                    n = n * 10 + (c - '0');
                }
                length = n > kMaxMessage ? -1 : n;
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

bool Reader::next(Incoming& out) {
    std::string body;
    if (!framer_.next(body)) return false;
    out.message = Json();
    out.error.clear();
    if (!Json::parse(body, out.message, &out.error)) {
        out.message = Json();
        if (out.error.empty()) out.error = "not JSON";
    }
    return true;
}

// ------------------------------------------------------------- progress
std::string progressText(const Progress& p) {
    std::string s = p.title;
    if (!p.message.empty()) s += (s.empty() ? "" : ": ") + p.message;
    if (p.percentage >= 0) s += (s.empty() ? "" : " ") + std::to_string(p.percentage) + "%";
    if (s.empty()) return "working\xE2\x80\xA6";
    // Line breaks and tabs would break the status bar's one line.
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    const size_t kMax = 60;
    if (s.size() > kMax) {
        size_t cut = kMax;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        s = s.substr(0, cut) + "\xE2\x80\xA6";
    }
    return s;
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

PositionIndex::PositionIndex(const std::u16string& text) : text_(text) {
    starts_.push_back(0);
    for (size_t i = 0; i < text.size(); i++) {
        char16_t c = text[i];
        if (c != u'\n' && c != u'\r') continue;
        ends_.push_back(i);
        if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') i++;
        starts_.push_back(i + 1);
    }
    ends_.push_back(text.size());
}

size_t PositionIndex::offset(Position p) const {
    size_t line = p.line < 0 ? 0 : (size_t)p.line;
    if (line >= starts_.size()) return text_.size();
    size_t ch = p.character < 0 ? 0 : (size_t)p.character;
    return std::min(starts_[line] + ch, ends_[line]);
}

Position PositionIndex::position(size_t offset) const {
    offset = std::min(offset, text_.size());
    // The last line starting at or before the offset. Between \r and \n is
    // not a line start, so it stays on the \r's line, as positionForOffset.
    auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
    size_t line = (size_t)(it - starts_.begin()) - 1;
    return Position{(int)line, (int)(offset - starts_[line])};
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

namespace {

// The optional built-in servers' files.
struct OptionalRow { const char* ext; const char* server; const char* id; };
const OptionalRow kOptionalRows[] = {
    {"tex", "latex", "latex"},       {"ltx", "latex", "latex"},
    {"latex", "latex", "latex"},     {"sty", "latex", "latex"},
    {"cls", "latex", "latex"},       {"bib", "latex", "bibtex"},
    {"md", "markdown", "markdown"},  {"markdown", "markdown", "markdown"},
    {"toml", "toml", "toml"},
    {"sh", "shell", "shellscript"},  {"bash", "shell", "shellscript"},
};

// The languageId a server's own files get, for an extension the settings
// add to a built-in key.
std::string usualLanguageId(const std::string& server) {
    if (server == "shell") return "shellscript";
    return server;
}

}  // namespace

const std::vector<std::string>& optionalServers() {
    static const std::vector<std::string> s = {"latex", "markdown", "toml", "shell"};
    return s;
}

bool isOptionalServer(const std::string& server) {
    for (const std::string& s : optionalServers())
        if (s == server) return true;
    return false;
}

bool languageForExtension(const std::string& extIn, Language& out,
                          const std::map<std::string, std::vector<std::string>>& extra) {
    const std::string ext = lower(extIn);
    if (ext.empty()) return false;
    for (const auto& [server, exts] : extra)
        for (const std::string& e : exts)
            if (e == ext) {
                Language main;
                out.server = server;
                if (languageForExtension(ext, main) && main.server == server)
                    out.languageId = main.languageId;
                else
                    out.languageId = usualLanguageId(server);
                return true;
            }
    if (languageForExtension(ext, out)) return true;
    for (const OptionalRow& r : kOptionalRows)
        if (ext == r.ext) {
            out.server = r.server;
            out.languageId = r.id;
            return true;
        }
    return false;
}

std::vector<std::string> defaultCommands(const std::string& server) {
    if (server == "latex") return {"texlab"};
    if (server == "markdown") return {"marksman server"};
    if (server == "toml") return {"taplo lsp stdio"};
    if (server == "shell") return {"bash-language-server start"};
    if (server == "cpp") return {"clangd"};
    if (server == "python")
        // basedpyright first: it is the one the install hints recommend
        // (uv brings its own Node for it), and where both are installed it
        // is the one the user most likely chose.
        return {"basedpyright-langserver --stdio", "pyright-langserver --stdio", "pylsp",
                "jedi-language-server"};
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
    if (server == "latex") return "LaTeX";
    if (server == "markdown") return "Markdown";
    if (server == "toml") return "TOML";
    if (server == "shell") return "Shell";
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
// ------------------------------------------------- a second server per language
std::string alsoKey(const std::string& server) { return server + ".also"; }

bool isAlsoKey(const std::string& key) {
    return key.size() > 5 && key.compare(key.size() - 5, 5, ".also") == 0;
}

std::string languageOfKey(const std::string& key) {
    return isAlsoKey(key) ? key.substr(0, key.size() - 5) : key;
}

std::string defaultAlsoCommand(const std::string& server, bool projectConfiguresRuff) {
    if (server == "python" && projectConfiguresRuff) return "ruff server";
    return "";
}

std::string programDisplayName(const std::string& program) {
    size_t slash = program.find_last_of('/');
    std::string name = slash == std::string::npos ? program : program.substr(slash + 1);
    static const std::string suffix = "-langserver";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        name = name.substr(0, name.size() - suffix.size());
    return name;
}

bool offers(const Json& caps, Feature f) {
    auto provides = [](const Json& cap) { return cap.isObject() || cap.asBool(); };
    switch (f) {
    case Feature::Completion: return provides(caps["completionProvider"]);
    case Feature::SignatureHelp: return caps["signatureHelpProvider"].isObject();
    case Feature::Hover: return provides(caps["hoverProvider"]);
    case Feature::Definition: return provides(caps["definitionProvider"]);
    case Feature::References: return provides(caps["referencesProvider"]);
    case Feature::Rename: return provides(caps["renameProvider"]);
    case Feature::CodeActions: return provides(caps["codeActionProvider"]);
    case Feature::DocumentSymbols: return provides(caps["documentSymbolProvider"]);
    case Feature::WorkspaceSymbols: return provides(caps["workspaceSymbolProvider"]);
    case Feature::FoldingRange: return provides(caps["foldingRangeProvider"]);
    }
    return false;
}

int routeRequest(Feature f, const std::vector<ServerView>& servers) {
    for (size_t i = 0; i < servers.size(); i++) {
        const ServerView& v = servers[i];
        if (!v.main) continue;
        if (!v.ready || (v.capabilities && offers(*v.capabilities, f))) return (int)i;
    }
    for (size_t i = 0; i < servers.size(); i++) {
        const ServerView& v = servers[i];
        if (!v.main && v.ready && v.capabilities && offers(*v.capabilities, f)) return (int)i;
    }
    return -1;
}

std::vector<int> serversOffering(Feature f, const std::vector<ServerView>& servers) {
    std::vector<int> out;
    for (size_t i = 0; i < servers.size(); i++) {
        const ServerView& v = servers[i];
        if (!v.ready || (v.capabilities && offers(*v.capabilities, f))) out.push_back((int)i);
    }
    return out;
}

StatusLine statusLine(const std::string& language, const std::vector<ServerStatus>& servers,
                      int errors, int warnings) {
    static const std::string kEllipsis = "\xE2\x80\xA6", kDot = " \xC2\xB7 ";
    StatusLine line;
    std::vector<std::string> parts;
    std::vector<std::string> running;
    long runningAt = -1;
    std::string progress;
    uint64_t progressBegun = 0;
    bool red = false, yellow = false, green = false, mainOff = false;
    for (const ServerStatus& s : servers) {
        const std::string name = s.name.empty() ? language : s.name;
        std::string part;
        switch (s.state) {
        case ServerState::Running:
            if (runningAt < 0) {
                runningAt = (long)parts.size();
                parts.push_back("");   // filled in below
            }
            running.push_back(name);
            if (!s.progress.empty() && (progress.empty() || s.progressBegun > progressBegun)) {
                progress = s.progress;
                progressBegun = s.progressBegun;
            }
            green = true;
            continue;
        case ServerState::Starting:
            part = name + " starting" + kEllipsis;
            yellow = true;
            break;
        case ServerState::Restarting:
            part = name + " crashed, restarting" + kEllipsis;
            yellow = true;
            break;
        case ServerState::NotInstalled:
            part = s.main ? "Install a " + language + " language server" + kEllipsis
                          : name + " not installed";
            break;
        case ServerState::Off:
            if (s.main) mainOff = true;
            break;
        case ServerState::Stopped: part = name + " stopped"; break;
        case ServerState::Failed:
            part = name + " failed to start";
            red = true;
            break;
        case ServerState::Crashed:
            part = name + " crashed";
            red = true;
            break;
        case ServerState::Idle: break;
        }
        if (!part.empty()) parts.push_back(part);
    }
    if (runningAt >= 0) {
        std::string names;
        for (size_t i = 0; i < running.size(); i++) names += (i ? " + " : "") + running[i];
        auto plural = [](int n, const char* one) {
            return std::to_string(n) + " " + one + (n == 1 ? "" : "s");
        };
        std::string counts;
        if (errors) counts = plural(errors, "error");
        if (warnings) counts += (counts.empty() ? "" : ", ") + plural(warnings, "warning");
        std::string what = counts.empty() ? "no problems" : counts;
        if (!progress.empty()) what = progress + (counts.empty() ? "" : ", " + counts);
        parts[(size_t)runningAt] = names + ": " + what;
    } else if (mainOff && parts.empty()) {
        // Off says so only when nothing else runs for the language.
        parts.push_back(language + " language server off");
    }
    for (size_t i = 0; i < parts.size(); i++) line.text += (i ? kDot : "") + parts[i];
    if (line.text.empty()) return line;
    line.dot = red ? StatusDot::Red : yellow ? StatusDot::Yellow
             : green ? StatusDot::Green : StatusDot::Gray;
    return line;
}

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

std::vector<Diagnostic> parseDiagnostics(const Json& params, std::string* uri, int* version) {
    if (uri) *uri = params["uri"].asString();
    if (version) *version = params["version"].isNumber() ? (int)params["version"].asInt() : -1;
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
        for (const Json& tag : d["tags"].elements()) {
            if (tag.asInt() == 1) diag.unnecessary = true;
            if (tag.asInt() == 2) diag.deprecated = true;
        }
        diag.raw = std::make_shared<const Json>(d);
        out.push_back(diag);
    }
    return out;
}

namespace {
// A TextEdit[] (additionalTextEdits), skipping anything malformed.
std::vector<TextEdit> parseTextEdits(const Json& list) {
    std::vector<TextEdit> out;
    for (const Json& e : list.elements()) {
        if (!e.isObject() || !e["range"].isObject() || !e["newText"].isString()) continue;
        TextEdit t;
        t.range = parseRange(e["range"]);
        if (positionBefore(t.range.end, t.range.start)) std::swap(t.range.start, t.range.end);
        t.newText = e["newText"].asString();
        out.push_back(t);
    }
    return out;
}
}  // namespace

std::vector<DiagnosticSpan> diagnosticSpans(const std::u16string* base,
                                            const std::u16string& current,
                                            const std::vector<Diagnostic>& diags) {
    std::vector<DiagnosticSpan> out;
    const std::u16string& from = base ? *base : current;
    // The part that changed since: what lies between the common head and
    // the common tail. Typing between two syncs is one place (several
    // carets make it one wider place), so this is all the diff needed.
    size_t head = 0, tail = 0;
    if (base && *base != current) {
        const size_t n = std::min(from.size(), current.size());
        while (head < n && from[head] == current[head]) head++;
        while (tail < n - head &&
               from[from.size() - 1 - tail] == current[current.size() - 1 - tail])
            tail++;
    }
    const bool changed = base && *base != current;
    const size_t oldEnd = from.size() - tail;           // [head, oldEnd) changed
    const long delta = (long)current.size() - (long)from.size();
    const PositionIndex index(from);
    for (size_t i = 0; i < diags.size(); i++) {
        size_t a = index.offset(diags[i].range.start);
        size_t b = index.offset(diags[i].range.end);
        if (b < a) std::swap(a, b);
        if (changed) {
            // Touching the changed part, at either end, counts: an error at
            // the end of a word being typed is about the word as it was.
            if (a <= oldEnd && b >= head) continue;
            if (a > oldEnd) {
                a = (size_t)((long)a + delta);
                b = (size_t)((long)b + delta);
            }
        }
        out.push_back(DiagnosticSpan{a, b, i});
    }
    return out;
}

// ------------------------------------------------------- DiagnosticStore
bool DiagnosticStore::publish(const std::string& uri, const std::string& server,
                              std::vector<Diagnostic> list,
                              std::shared_ptr<const std::u16string> base,
                              const std::string& sourceName) {
    if (list.empty()) return erase(uri, server);
    if (!sourceName.empty())
        for (Diagnostic& d : list)
            if (d.source.empty()) d.source = sourceName;
    std::vector<Set>& sets = files_[uri];
    generations_[uri] = ++counter_;
    for (Set& set : sets)
        if (set.server == server) {
            set.list = std::move(list);
            set.base = std::move(base);
            return true;
        }
    sets.push_back(Set{server, std::move(list), std::move(base)});
    return true;
}

uint64_t DiagnosticStore::generation(const std::string& uri) const {
    auto it = generations_.find(uri);
    return it == generations_.end() ? 0 : it->second;
}

bool DiagnosticStore::erase(const std::string& uri, const std::string& server) {
    auto it = files_.find(uri);
    if (it == files_.end()) return false;
    std::vector<Set>& sets = it->second;
    auto s = std::find_if(sets.begin(), sets.end(),
                          [&](const Set& x) { return x.server == server; });
    if (s == sets.end()) return false;
    sets.erase(s);
    generations_[uri] = ++counter_;
    if (sets.empty()) files_.erase(it);
    return true;
}

bool DiagnosticStore::eraseServer(const std::string& server) {
    bool any = false;
    for (const std::string& uri : uris()) any = erase(uri, server) || any;
    return any;
}

std::vector<const DiagnosticStore::Set*> DiagnosticStore::forUri(const std::string& uri) const {
    std::vector<const Set*> out;
    auto it = files_.find(uri);
    if (it != files_.end())
        for (const Set& s : it->second) out.push_back(&s);
    return out;
}

const DiagnosticStore::Set* DiagnosticStore::find(const std::string& uri,
                                                  const std::string& server) const {
    for (const Set* s : forUri(uri))
        if (s->server == server) return s;
    return nullptr;
}

std::vector<std::string> DiagnosticStore::uris() const {
    std::vector<std::string> out;
    for (const auto& [uri, sets] : files_) out.push_back(uri);
    return out;
}

std::vector<Diagnostic> DiagnosticStore::merged(const std::string& uri) const {
    std::vector<Diagnostic> out;
    for (const Set* s : forUri(uri)) out.insert(out.end(), s->list.begin(), s->list.end());
    return out;
}

std::vector<MergedSpan> mergedDiagnosticSpans(
    const std::vector<const DiagnosticStore::Set*>& sets, const std::u16string& current) {
    std::vector<MergedSpan> out;
    for (const DiagnosticStore::Set* set : sets)
        for (const DiagnosticSpan& sp : diagnosticSpans(set->base.get(), current, set->list))
            out.push_back(MergedSpan{sp.start, sp.end, &set->list[sp.index]});
    std::stable_sort(out.begin(), out.end(), [](const MergedSpan& a, const MergedSpan& b) {
        return a.start < b.start;
    });
    return out;
}

std::vector<ProblemRow> problemRows(const std::vector<Problem>& problems) {
    // Each file's problems, with its worst severity.
    std::map<std::string, std::vector<size_t>> byFile;
    for (size_t i = 0; i < problems.size(); i++) byFile[problems[i].path].push_back(i);
    struct File {
        const std::string* path;
        std::vector<size_t> list;
        int worst;
    };
    std::vector<File> files;
    for (auto& [path, list] : byFile) {
        int worst = 4;
        for (size_t i : list) worst = std::min(worst, (int)problems[i].severity);
        files.push_back(File{&path, list, worst});
    }
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) {
        int wa = std::min(a.worst, 3), wb = std::min(b.worst, 3);   // info and hint alike
        if (wa != wb) return wa < wb;
        return *a.path < *b.path;
    });
    std::vector<ProblemRow> rows;
    for (File& f : files) {
        std::stable_sort(f.list.begin(), f.list.end(), [&](size_t a, size_t b) {
            const Problem &p = problems[a], &q = problems[b];
            if (p.severity != q.severity) return (int)p.severity < (int)q.severity;
            if (p.line != q.line) return p.line < q.line;
            return p.column < q.column;
        });
        ProblemRow h;
        h.heading = true;
        h.path = *f.path;
        for (size_t i : f.list) {
            if (problems[i].severity == Severity::Error) h.errors++;
            else if (problems[i].severity == Severity::Warning) h.warnings++;
            else h.others++;
        }
        rows.push_back(h);
        for (size_t i : f.list) {
            ProblemRow r;
            r.path = *f.path;
            r.problem = i;
            rows.push_back(r);
        }
    }
    return rows;
}

ProblemCounts countProblems(const std::vector<Problem>& problems) {
    ProblemCounts c;
    std::set<std::string> files;
    for (const Problem& p : problems) {
        switch (p.severity) {
        case Severity::Error: c.errors++; break;
        case Severity::Warning: c.warnings++; break;
        case Severity::Information: c.infos++; break;
        case Severity::Hint: c.hints++; break;
        }
        files.insert(p.path);
    }
    c.files = (int)files.size();
    return c;
}

std::string problemCountsText(const ProblemCounts& c) {
    auto named = [](int n, const char* one, const char* many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    std::vector<std::string> parts;
    if (c.errors) parts.push_back(named(c.errors, "error", "errors"));
    if (c.warnings) parts.push_back(named(c.warnings, "warning", "warnings"));
    if (parts.empty()) {
        if (c.infos) parts.push_back(named(c.infos, "info", "infos"));
        if (c.hints) parts.push_back(named(c.hints, "hint", "hints"));
    }
    if (parts.empty()) return "No problems";
    std::string s = parts[0];
    for (size_t i = 1; i < parts.size(); i++) s += ", " + parts[i];
    return s;
}

std::string problemOneLine(const std::string& message) {
    std::string out;
    bool space = false;
    for (char ch : message) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += ch;
    }
    return out;
}

long stepProblem(const std::vector<std::pair<size_t, size_t>>& spans, size_t caret,
                 long last, bool forward) {
    const long n = (long)spans.size();
    if (n == 0) return -1;
    std::vector<long> order((size_t)n);
    for (long i = 0; i < n; i++) order[(size_t)i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](long a, long b) { return spans[(size_t)a] < spans[(size_t)b]; });
    if (last >= 0 && last < n && spans[(size_t)last].first == caret) {
        long at = std::find(order.begin(), order.end(), last) - order.begin();
        return order[(size_t)((at + (forward ? 1 : n - 1)) % n)];
    }
    if (forward) {
        for (long i : order)
            if (spans[(size_t)i].first > caret) return i;
        return order.front();
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (spans[(size_t)*it].first < caret) return *it;
    return order.back();
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
        c.detail = trim(it["detail"].asString());
        parseMarkup(it["documentation"], c.documentation, c.docIsMarkdown);
        c.raw = std::make_shared<const Json>(it);
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
        if (it["additionalTextEdits"].isArray()) {
            c.hasAdditionalEdits = true;
            c.additionalEdits = parseTextEdits(it["additionalTextEdits"]);
        }
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const CompletionItem& a,
                                                const CompletionItem& b) {
        const std::string& ka = a.sortText.empty() ? a.label : a.sortText;
        const std::string& kb = b.sortText.empty() ? b.label : b.sortText;
        return ka < kb;
    });
    for (size_t i = 0; i < out.size(); i++) out[i].index = (int)i;
    return out;
}

bool parseMarkup(const Json& j, std::string& text, bool& isMarkdown) {
    if (j.isString()) {
        text = j.asString();
        isMarkdown = false;
        return true;
    }
    if (j.isObject() && j["value"].isString()) {
        text = j["value"].asString();
        isMarkdown = j["kind"].asString() == "markdown";
        return true;
    }
    return false;
}

void applyResolved(CompletionItem& item, const Json& result) {
    item.resolved = true;
    if (!result.isObject()) return;
    std::string detail = trim(result["detail"].asString());
    if (!detail.empty()) item.detail = detail;
    std::string doc;
    bool md = false;
    if (parseMarkup(result["documentation"], doc, md) && !trim(doc).empty()) {
        item.documentation = doc;
        item.docIsMarkdown = md;
    }
    if (result["additionalTextEdits"].isArray()) {
        item.hasAdditionalEdits = true;
        item.additionalEdits = parseTextEdits(result["additionalTextEdits"]);
    }
}

bool completionWantsResolve(const CompletionItem& item, bool serverResolves) {
    return serverResolves && item.raw && !item.resolved && !item.hasAdditionalEdits;
}

CompletionEdits completionEdits(const std::u16string& text, size_t start, size_t end,
                                const std::u16string& insert,
                                const std::vector<TextEdit>& additional) {
    CompletionEdits out;
    start = std::min(start, text.size());
    end = std::min(std::max(end, start), text.size());
    std::vector<OffsetEdit> extra;
    for (const TextEdit& e : additional) {
        size_t a = offsetForPosition(text, e.range.start);
        size_t b = offsetForPosition(text, e.range.end);
        if (b < a) std::swap(a, b);
        extra.push_back(OffsetEdit{a, b - a, toUtf16(e.newText)});
    }
    std::stable_sort(extra.begin(), extra.end(),
                     [](const OffsetEdit& x, const OffsetEdit& y) { return x.start < y.start; });
    // Each one wholly before the main edit (ending at its start counts) or
    // wholly after it, and none overlapping the next.
    bool ok = true;
    for (size_t i = 0; i < extra.size() && ok; i++) {
        const size_t a = extra[i].start, b = a + extra[i].length;
        if (!(b <= start || a >= end)) ok = false;
        if (i > 0 && a < extra[i - 1].start + extra[i - 1].length) ok = false;
    }
    if (!ok) {
        extra.clear();
        out.droppedAdditional = true;
    }
    long shift = 0;   // what the edits before the main one add or remove
    bool placed = false;
    for (const OffsetEdit& e : extra) {
        const bool before = e.start + e.length <= start;
        if (!before && !placed) {
            out.edits.push_back(OffsetEdit{start, end - start, insert});
            placed = true;
        }
        if (before) shift += (long)e.text.size() - (long)e.length;
        out.edits.push_back(e);
    }
    if (!placed) out.edits.push_back(OffsetEdit{start, end - start, insert});
    out.caret = (size_t)((long)start + shift) + insert.size();
    return out;
}

bool lateAdditionalEdits(const std::u16string& now, const std::vector<TextEdit>& additional,
                         size_t limit, std::vector<OffsetEdit>& out) {
    out.clear();
    limit = std::min(limit, now.size());
    const PositionIndex index(now);
    std::vector<OffsetEdit> edits;
    for (const TextEdit& e : additional) {
        size_t a = index.offset(e.range.start), b = index.offset(e.range.end);
        if (b < a) std::swap(a, b);
        // A position on the line `limit` is on, past it, may be a column
        // that line had before; past the limit nothing is known.
        if (b > limit) return false;
        edits.push_back(OffsetEdit{a, b - a, toUtf16(e.newText)});
    }
    std::stable_sort(edits.begin(), edits.end(),
                     [](const OffsetEdit& x, const OffsetEdit& y) { return x.start < y.start; });
    for (size_t i = 1; i < edits.size(); i++)
        if (edits[i].start < edits[i - 1].start + edits[i - 1].length) return false;
    out = std::move(edits);
    return true;
}

// ------------------------------------------------------ incremental sync
void ChangeTracker::edit(size_t start, size_t removed, size_t inserted) {
    if (overflowed_) return;
    const size_t end = start + removed;
    // The regions this edit touches or overlaps, in the text now: [i, j).
    size_t i = 0;
    while (i < regions_.size() && regions_[i].newEnd < start) i++;
    size_t j = i;
    while (j < regions_.size() && regions_[j].newStart <= end) j++;
    Region m;
    if (i == j) {
        // Untouched text: where it was in the text as sent follows from
        // the region before it (the text between them is unchanged).
        const size_t oldAt = i == 0 ? start : regions_[i - 1].oldEnd + (start - regions_[i - 1].newEnd);
        m = Region{oldAt, oldAt + removed, start, start + inserted};
    } else {
        const Region& first = regions_[i];
        const Region& last = regions_[j - 1];
        const size_t newStart = std::min(start, first.newStart);
        const size_t newEnd = std::max(end, last.newEnd);
        m.newStart = newStart;
        m.oldStart = first.oldStart - (first.newStart - newStart);
        m.oldEnd = last.oldEnd + (newEnd - last.newEnd);
        m.newEnd = newEnd - removed + inserted;
    }
    const long delta = (long)inserted - (long)removed;
    for (size_t k = j; k < regions_.size(); k++) {
        regions_[k].newStart = (size_t)((long)regions_[k].newStart + delta);
        regions_[k].newEnd = (size_t)((long)regions_[k].newEnd + delta);
    }
    regions_.erase(regions_.begin() + (long)i, regions_.begin() + (long)j);
    regions_.insert(regions_.begin() + (long)i, m);
    if (regions_.size() > kMaxRegions) {
        regions_.clear();
        overflowed_ = true;
    }
}

namespace {
// Walks UTF-8 text in UTF-16 units, keeping the byte offset and the LSP
// position (lines end at \n, \r\n or \r, as PositionIndex counts them).
struct Utf8Walk {
    const std::string& s;
    size_t byte = 0, unit = 0, lineStart = 0;
    int line = 0;
    explicit Utf8Walk(const std::string& text) : s(text) {}
    // On to `target` units; false past the end, inside a character, or
    // between \r and \n.
    bool to(size_t target) {
        while (unit < target) {
            if (byte >= s.size()) return false;
            const unsigned char c = (unsigned char)s[byte];
            if (c == '\n') {
                byte++, unit++, line++, lineStart = unit;
                continue;
            }
            if (c == '\r') {
                if (byte + 1 < s.size() && s[byte + 1] == '\n') {
                    if (unit + 1 == target) return false;
                    byte += 2, unit += 2;
                } else {
                    byte++, unit++;
                }
                line++, lineStart = unit;
                continue;
            }
            size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                     : (c & 0xF8) == 0xF0 ? 4 : 1;
            const size_t units = n == 4 ? 2 : 1;
            if (unit + units > target) return false;
            if (byte + n > s.size()) n = s.size() - byte;
            byte += n, unit += units;
        }
        return true;
    }
    Position position() const { return Position{line, (int)(unit - lineStart)}; }
    // Between \r and \n: the next unit is a \n after a \r already passed.
    bool betweenCrLf() const {
        return byte > 0 && byte < s.size() && s[byte - 1] == '\r' && s[byte] == '\n';
    }
};
}  // namespace

bool rangedChanges(const std::string& before, const std::string& after,
                   const std::vector<ChangeTracker::Region>& regions,
                   std::vector<ContentChange>& out) {
    out.clear();
    if (regions.empty()) return before == after;
    struct Placed { Range range; size_t oldA, oldB, newA, newB; };
    std::vector<Placed> placed;
    placed.reserve(regions.size());
    Utf8Walk old(before);
    size_t oldEndByte = 0, newEndByte = 0;   // where the last region ended, in bytes
    size_t lastOldEnd = 0, lastNewEnd = 0;   // ...and in units
    for (size_t k = 0; k < regions.size(); k++) {
        const ChangeTracker::Region& r = regions[k];
        if (r.oldEnd < r.oldStart || r.newEnd < r.newStart) return false;
        if (k > 0 && (r.oldStart < lastOldEnd || r.newStart < lastNewEnd)) return false;
        // The text between regions is the same in both, so it is as long.
        if (r.oldStart - lastOldEnd != r.newStart - lastNewEnd) return false;
        if (!old.to(r.oldStart)) return false;
        Placed p;
        p.range.start = old.position();
        p.oldA = old.byte;
        // The gap's bytes, at the same distance on in the new text.
        p.newA = newEndByte + (old.byte - oldEndByte);
        if (p.newA > after.size()) return false;
        if (before.compare(oldEndByte, old.byte - oldEndByte, after, newEndByte,
                           old.byte - oldEndByte) != 0)
            return false;
        if (!old.to(r.oldEnd) || old.betweenCrLf()) return false;
        p.range.end = old.position();
        p.oldB = old.byte;
        Utf8Walk now(after);
        now.byte = p.newA;
        now.unit = 0;
        if (!now.to(r.newEnd - r.newStart)) return false;
        p.newB = now.byte;
        placed.push_back(p);
        oldEndByte = p.oldB, newEndByte = p.newB;
        lastOldEnd = r.oldEnd, lastNewEnd = r.newEnd;
    }
    // The text after the last region.
    if (before.size() - oldEndByte != after.size() - newEndByte) return false;
    if (before.compare(oldEndByte, std::string::npos, after, newEndByte, std::string::npos) != 0)
        return false;
    // A boundary of the new text between \r and \n would make a server
    // that splits lines on its own read a line break that is not there.
    for (const Placed& p : placed) {
        if (p.newA > 0 && p.newA < after.size() && after[p.newA - 1] == '\r' && after[p.newA] == '\n')
            return false;
        if (p.newB > 0 && p.newB < after.size() && after[p.newB - 1] == '\r' && after[p.newB] == '\n')
            return false;
    }
    out.reserve(placed.size());
    for (size_t k = placed.size(); k-- > 0;)
        out.push_back(ContentChange{placed[k].range,
                                    after.substr(placed[k].newA, placed[k].newB - placed[k].newA)});
    return true;
}

std::u16string applyContentChanges(const std::u16string& text,
                                   const std::vector<ContentChange>& changes) {
    std::u16string out = text;
    for (const ContentChange& c : changes) {
        size_t a = offsetForPosition(out, c.range.start), b = offsetForPosition(out, c.range.end);
        if (b < a) std::swap(a, b);
        out.replace(a, b - a, toUtf16(c.text));
    }
    return out;
}

SignatureHelp parseSignatureHelp(const Json& result) {
    SignatureHelp help;
    if (!result.isObject()) return help;
    for (const Json& s : result["signatures"].elements()) {
        SignatureInfo info;
        info.label = s["label"].asString();
        parseMarkup(s["documentation"], info.documentation, info.docIsMarkdown);
        size_t from = 0;   // a string label is looked for after the last one
        for (const Json& p : s["parameters"].elements()) {
            const Json& l = p["label"];
            std::pair<int, int> range{-1, -1};
            if (l.isArray() && l.size() == 2) {
                range = {(int)l[0].asInt(-1), (int)l[1].asInt(-1)};
            } else if (l.isString() && !l.asString().empty()) {
                size_t at = info.label.find(l.asString(), from);
                if (at == std::string::npos) at = info.label.find(l.asString());
                if (at != std::string::npos) {
                    int a = (int)toUtf16(info.label.substr(0, at)).size();
                    range = {a, a + (int)toUtf16(l.asString()).size()};
                    from = at + l.asString().size();
                }
            }
            info.params.push_back(range);
            std::string doc;
            bool md = false;
            parseMarkup(p["documentation"], doc, md);
            info.paramDocs.push_back(trim(doc));
        }
        help.signatures.push_back(info);
    }
    if (help.signatures.empty()) return help;
    int active = (int)result["activeSignature"].asInt(0);
    if (active < 0 || active >= (int)help.signatures.size()) active = 0;
    help.activeSignature = active;
    // A signature's own activeParameter wins over the result's.
    const Json& sig = result["signatures"][(size_t)active];
    const Json& param = sig.has("activeParameter") ? sig["activeParameter"]
                                                   : result["activeParameter"];
    int ap = param.isNumber() ? (int)param.asInt() : 0;
    if (ap < 0 || ap >= (int)help.signatures[(size_t)active].params.size()) ap = -1;
    help.activeParameter = ap;
    return help;
}

Json configurationSection(const Json& settings, const std::string& section) {
    if (section.empty()) return settings;
    const Json* at = &settings;
    size_t start = 0;
    while (true) {
        size_t dot = section.find('.', start);
        std::string part = section.substr(start, dot == std::string::npos ? std::string::npos
                                                                           : dot - start);
        if (!at->isObject() || !at->has(part)) return Json();
        at = &(*at)[part];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return *at;
}

// ------------------------------------------------------- watched files
namespace {

// The pattern with each {a,b} group spelled out, nested groups too. An
// unclosed brace stands for itself; the list stops growing at 1,024.
void expandBraces(const std::string& p, std::vector<std::string>& out) {
    if (out.size() >= 1024) return;
    size_t open = std::string::npos;
    int depth = 0;
    bool inClass = false;
    for (size_t i = 0; i < p.size(); i++) {
        char c = p[i];
        if (inClass) {
            if (c == ']') inClass = false;
        } else if (c == '[') {
            inClass = p.find(']', i + 1) != std::string::npos;
        } else if (c == '{') {
            if (depth++ == 0) open = i;
        } else if (c == '}' && depth > 0) {
            if (--depth == 0) {
                std::string head = p.substr(0, open), tail = p.substr(i + 1);
                std::string body = p.substr(open + 1, i - open - 1);
                std::vector<std::string> alts;
                int d = 0;
                size_t start = 0;
                for (size_t k = 0; k < body.size(); k++) {
                    if (body[k] == '{') d++;
                    else if (body[k] == '}') d--;
                    else if (body[k] == ',' && d == 0) {
                        alts.push_back(body.substr(start, k - start));
                        start = k + 1;
                    }
                }
                alts.push_back(body.substr(start));
                for (const std::string& a : alts) expandBraces(head + a + tail, out);
                return;
            }
        }
    }
    out.push_back(p);
}

// One character of a [...] set at p[pi] ('[' itself); `end` gets the index
// after the ']'. False when the set is not closed (then '[' is literal).
bool classAt(const std::string& p, size_t pi, char c, bool& matched, size_t& end) {
    size_t i = pi + 1;
    bool negate = i < p.size() && (p[i] == '!' || p[i] == '^');
    if (negate) i++;
    size_t first = i;
    bool hit = false;
    while (i < p.size() && (p[i] != ']' || i == first)) {
        unsigned char lo = (unsigned char)p[i];
        if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']') {
            unsigned char hi = (unsigned char)p[i + 2];
            if ((unsigned char)c >= lo && (unsigned char)c <= hi) hit = true;
            i += 3;
        } else {
            if ((unsigned char)c == lo) hit = true;
            i++;
        }
    }
    if (i >= p.size()) return false;
    end = i + 1;
    matched = hit != negate;
    return true;
}

struct GlobMatcher {
    const std::string& p;
    const std::string& s;
    std::vector<signed char> memo;   // -1 unknown, else the answer
    GlobMatcher(const std::string& pat, const std::string& str)
        : p(pat), s(str), memo((pat.size() + 1) * (str.size() + 1), -1) {}

    bool at(size_t pi, size_t si) {
        signed char& m = memo[pi * (s.size() + 1) + si];
        if (m < 0) m = compute(pi, si) ? 1 : 0;
        return m == 1;
    }

    bool compute(size_t pi, size_t si) {
        const size_t P = p.size(), S = s.size();
        if (pi == P) return si == S;
        char c = p[pi];
        if (c == '*') {
            bool globstar = pi + 1 < P && p[pi + 1] == '*' && (pi == 0 || p[pi - 1] == '/') &&
                            (pi + 2 == P || p[pi + 2] == '/');
            if (globstar) {
                if (pi + 2 == P) return true;   // the rest, whatever it is
                // "**/": no segment, or one more and stay here.
                if (at(pi + 3, si)) return true;
                size_t slash = s.find('/', si);
                return slash != std::string::npos && at(pi, slash + 1);
            }
            size_t next = pi + 1;
            while (next < P && p[next] == '*') next++;   // "a**b" is "a*b"
            for (size_t k = si; k <= S; k++) {
                if (at(next, k)) return true;
                if (k < S && s[k] == '/') break;
            }
            return false;
        }
        if (si == S) {
            // "a/**" also matches "a" itself.
            return p.compare(pi, std::string::npos, "/**") == 0;
        }
        if (c == '?') return s[si] != '/' && at(pi + 1, si + 1);
        if (c == '[') {
            bool matched = false;
            size_t end = 0;
            if (classAt(p, pi, s[si], matched, end))
                return s[si] != '/' && matched && at(end, si + 1);
        }
        return s[si] == c && at(pi + 1, si + 1);
    }
};

std::string withoutLeadingSlash(const std::string& s) {
    return !s.empty() && s[0] == '/' ? s.substr(1) : s;
}

// The path below `base`, or "" when it is not below it.
std::string below(const std::string& path, const std::string& base) {
    std::string b = base;
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    if (b == "/") return withoutLeadingSlash(path);
    if (path.size() <= b.size() + 1 || path.compare(0, b.size(), b) != 0 || path[b.size()] != '/')
        return "";
    return path.substr(b.size() + 1);
}

}  // namespace

bool globMatch(const std::string& pattern, const std::string& path) {
    std::vector<std::string> alts;
    expandBraces(pattern, alts);
    for (const std::string& a : alts) {
        GlobMatcher m(a, path);
        if (m.at(0, 0)) return true;
    }
    return false;
}

std::vector<FileWatcher> parseFileWatchers(const Json& registerOptions) {
    std::vector<FileWatcher> out;
    for (const Json& w : registerOptions["watchers"].elements()) {
        FileWatcher fw;
        const Json& g = w["globPattern"];
        if (g.isString()) {
            fw.pattern = g.asString();
        } else if (g.isObject() && g["pattern"].isString()) {
            fw.pattern = g["pattern"].asString();
            // baseUri is a URI or a WorkspaceFolder ({uri, name}).
            const Json& b = g["baseUri"];
            fw.base = pathFromUri(b.isString() ? b.asString() : b["uri"].asString());
            if (fw.base.empty()) continue;
        } else {
            continue;
        }
        if (fw.pattern.empty()) continue;
        if (w["kind"].isNumber()) fw.kind = (int)w["kind"].asInt() & 7;
        out.push_back(fw);
    }
    return out;
}

bool watcherMatches(const FileWatcher& w, const std::string& root, const FileChange& change) {
    int bit = change.type == FileChangeType::Created ? 1
              : change.type == FileChangeType::Changed ? 2 : 4;
    if (!(w.kind & bit)) return false;
    const std::string& path = change.path;
    if (!w.base.empty()) {
        std::string rel = below(path, w.base);
        return !rel.empty() && globMatch(w.pattern, rel);
    }
    if (w.pattern[0] == '/') return globMatch(withoutLeadingSlash(w.pattern), withoutLeadingSlash(path));
    if (w.pattern.find('/') == std::string::npos) {
        size_t slash = path.rfind('/');
        return globMatch(w.pattern, slash == std::string::npos ? path : path.substr(slash + 1));
    }
    if (globMatch(w.pattern, withoutLeadingSlash(path))) return true;
    std::string rel = root.empty() ? "" : below(path, root);
    return !rel.empty() && globMatch(w.pattern, rel);
}

FileChangeType FileEventClassifier::classify(const std::string& path, bool exists,
                                             bool createdOrRenamed) {
    if (!exists) {
        present_.erase(path);
        return FileChangeType::Deleted;
    }
    if (present_.size() >= 200000) present_.clear();
    bool known = !present_.insert(path).second;
    return createdOrRenamed && !known ? FileChangeType::Created : FileChangeType::Changed;
}

std::vector<FileChange> coalesceFileChanges(const std::vector<FileChange>& changes) {
    std::vector<FileChange> out;
    std::map<std::string, size_t> where;   // path -> index in out
    std::vector<bool> dropped;
    for (const FileChange& c : changes) {
        auto it = where.find(c.path);
        if (it == where.end() || dropped[it->second]) {
            where[c.path] = out.size();
            out.push_back(c);
            dropped.push_back(false);
            continue;
        }
        FileChange& was = out[it->second];
        if (was.type == FileChangeType::Created && c.type == FileChangeType::Deleted)
            dropped[it->second] = true;
        else if (was.type == FileChangeType::Deleted && c.type == FileChangeType::Created)
            was.type = FileChangeType::Changed;
        else if (was.type == FileChangeType::Created && c.type == FileChangeType::Changed)
            ;   // still new to the server
        else
            was.type = c.type;
    }
    std::vector<FileChange> kept;
    for (size_t i = 0; i < out.size(); i++)
        if (!dropped[i]) kept.push_back(out[i]);
    return kept;
}

bool watchExcluded(const std::string& path) {
    auto has = [&](const std::string& part) {
        // "/part/" anywhere, or the path ending in "/part" for a folder.
        size_t at = path.find("/" + part + "/");
        if (at != std::string::npos) return true;
        return path.size() >= part.size() + 1 &&
               path.compare(path.size() - part.size() - 1, std::string::npos, "/" + part) == 0;
    };
    if (has(".git") || has(".hg/store")) return true;
    // node_modules/<package>/anything: the package folders themselves are
    // reported (an install adding one), not what is inside them.
    for (size_t at = path.find("/node_modules/"); at != std::string::npos;
         at = path.find("/node_modules/", at + 1)) {
        size_t pkg = at + 14;
        size_t slash = path.find('/', pkg);
        if (slash != std::string::npos && slash + 1 < path.size()) return true;
    }
    return false;
}

// ------------------------------------------------------- Python servers
PythonServer pythonServerKind(const std::vector<std::string>& argv) {
    for (const std::string& word : argv) {
        size_t slash = word.find_last_of('/');
        std::string name = lower(slash == std::string::npos ? word : word.substr(slash + 1));
        // pyright may be run as node plus its script, so its folder counts.
        std::string full = lower(word);
        if (full.find("basedpyright") != std::string::npos) return PythonServer::BasedPyright;
        if (full.find("pyright") != std::string::npos) return PythonServer::Pyright;
        if (name == "pylsp" || name.find("python-lsp-server") != std::string::npos ||
            name.find("python_lsp_server") != std::string::npos)
            return PythonServer::Pylsp;
        if (name.find("jedi-language-server") != std::string::npos ||
            name.find("jedi_language_server") != std::string::npos)
            return PythonServer::Jedi;
    }
    return PythonServer::Other;
}

const char* pythonServerName(PythonServer kind) {
    switch (kind) {
    case PythonServer::Pyright: return "pyright";
    case PythonServer::BasedPyright: return "basedpyright";
    case PythonServer::Pylsp: return "pylsp";
    case PythonServer::Jedi: return "jedi-language-server";
    case PythonServer::Other: break;
    }
    return "other";
}

Json pythonSettings(PythonServer kind, const std::string& interpreter) {
    // pyright's own defaults, spelled out: search the usual source folders,
    // read types from libraries without stubs, and check only open files,
    // which keeps a large folder from being analysed whole.
    Json analysis = Json::object({{"autoSearchPaths", true},
                                  {"useLibraryCodeForTypes", true},
                                  {"diagnosticMode", "openFilesOnly"}});
    if (kind == PythonServer::Pyright || kind == PythonServer::BasedPyright) {
        Json python = Json::object();
        if (!interpreter.empty()) python.set("pythonPath", interpreter);
        Json out = Json::object();
        if (kind == PythonServer::Pyright) {
            python.set("analysis", analysis);
            out.set("python", python);
        } else {
            // basedpyright reads its analysis settings under its own name,
            // and checks far more than pyright unless told otherwise; a
            // pyproject.toml or pyrightconfig.json still wins over this.
            analysis.set("typeCheckingMode", "standard");
            out.set("python", python);
            out.set("basedpyright", Json::object({{"analysis", analysis}}));
        }
        return out;
    }
    if (kind == PythonServer::Pylsp && !interpreter.empty()) {
        return Json::object({{"pylsp", Json::object({{"plugins", Json::object({
            {"jedi", Json::object({{"environment", interpreter}})}})}})}});
    }
    return Json();
}

Json pythonInitializationOptions(PythonServer kind, const std::string& interpreter) {
    if (kind == PythonServer::Jedi && !interpreter.empty())
        return Json::object({{"workspace",
                              Json::object({{"environmentPath", interpreter}})}});
    return Json();
}

bool pythonInterpreterNeedsRestart(PythonServer kind) {
    return kind == PythonServer::Jedi || kind == PythonServer::Other;
}

namespace {

// A Markdown horizontal rule: three or more of one of - * _, spaces allowed.
bool isRuleLine(const std::string& line) {
    std::string t = trim(line);
    if (t.empty() || (t[0] != '-' && t[0] != '*' && t[0] != '_')) return false;
    int n = 0;
    for (char c : t) {
        if (c == t[0]) n++;
        else if (c != ' ' && c != '\t') return false;
    }
    return n >= 3;
}

// Markdown to readable text: keep code fence contents, drop the fence lines
// and rules (pyright puts one between the signature and the docs), and
// unescape the backslash escapes servers put before punctuation.
std::string markdownToText(const std::string& md) {
    std::string out, line;
    bool inFence = false;
    for (size_t i = 0; i <= md.size(); i++) {
        if (i < md.size() && md[i] != '\n') { line += md[i]; continue; }
        bool fence = trim(line).compare(0, 3, "```") == 0;
        if (fence) inFence = !inFence;
        if (!fence && !inFence && isRuleLine(line)) {
            out += '\n';
        } else if (!fence) {
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

HoverDoc parseHoverDoc(const Json& result) {
    HoverDoc doc;
    if (!result.isObject()) return doc;
    auto add = [&](const Json& j) {
        HoverDoc::Part part;
        if (j.isString()) {   // a MarkedString: Markdown
            part.text = j.asString();
            part.markdown = true;
        } else if (j.isObject() && j.has("language")) {
            std::string value = trim(j["value"].asString());
            if (!value.empty())
                part.text = "```" + j["language"].asString() + "\n" + value + "\n```";
            part.markdown = true;
        } else if (!parseMarkup(j, part.text, part.markdown)) {
            return;
        }
        part.text = trim(part.text);
        if (!part.text.empty()) doc.parts.push_back(part);
    };
    const Json& contents = result["contents"];
    if (contents.isArray()) {
        for (const Json& part : contents.elements()) add(part);
    } else {
        add(contents);
    }
    if (result["range"].isObject()) {
        doc.hasRange = true;
        doc.range = parseRange(result["range"]);
    }
    return doc;
}

std::vector<FoldingRange> parseFoldingRanges(const Json& result) {
    std::vector<FoldingRange> out;
    if (!result.isArray()) return out;
    for (size_t i = 0; i < result.size(); i++) {
        const Json& r = result[i];
        if (!r["startLine"].isNumber() || !r["endLine"].isNumber()) continue;
        const double s = r["startLine"].asNumber(), e = r["endLine"].asNumber();
        if (s < 0 || e <= s || s > 1e9 || e > 1e9 || s != (double)(long)s || e != (double)(long)e)
            continue;
        FoldingRange f;
        f.startLine = (int)s;
        f.endLine = (int)e;
        if (r["kind"].isString()) f.kind = r["kind"].asString();
        out.push_back(f);
    }
    return out;
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

// ------------------------------------------------------------- editing
bool positionBefore(Position a, Position b) {
    return a.line < b.line || (a.line == b.line && a.character < b.character);
}

// ------------------------------------------------------------- symbols
namespace {
bool rangeHolds(const Range& r, Position p) {
    return !positionBefore(p, r.start) && !positionBefore(r.end, p);
}

void addDocumentSymbol(const Json& j, const std::string& container, int depth,
                       std::vector<Symbol>& out) {
    if (!j.isObject() || !j["name"].isString()) return;
    Symbol s;
    s.name = j["name"].asString();
    s.detail = j["detail"].asString();
    s.container = container;
    s.kind = (int)j["kind"].asInt();
    s.depth = depth;
    s.range = parseRange(j["range"]);
    s.selectionRange = j.has("selectionRange") ? parseRange(j["selectionRange"]) : s.range;
    out.push_back(s);
    std::vector<const Json*> kids;
    for (const Json& c : j["children"].elements()) kids.push_back(&c);
    std::stable_sort(kids.begin(), kids.end(), [](const Json* a, const Json* b) {
        return positionBefore(parsePosition((*a)["range"]["start"]),
                              parsePosition((*b)["range"]["start"]));
    });
    const std::string name = s.name;
    for (const Json* c : kids) addDocumentSymbol(*c, name, depth + 1, out);
}

// SymbolInformation, or a WorkspaceSymbol (whose location may have no range).
bool symbolInformation(const Json& j, Symbol& s) {
    if (!j.isObject() || !j["name"].isString()) return false;
    s.name = j["name"].asString();
    s.kind = (int)j["kind"].asInt();
    s.container = j["containerName"].asString();
    s.detail = j["detail"].asString();
    const Json& loc = j["location"];
    s.uri = loc["uri"].asString();
    s.hasRange = loc["range"].isObject();
    if (s.hasRange) s.range = parseRange(loc["range"]);
    s.selectionRange = s.range;
    return true;
}
}  // namespace

std::vector<Symbol> parseDocumentSymbols(const Json& result) {
    std::vector<Symbol> out;
    if (!result.isArray()) return out;
    bool tree = false;
    for (const Json& j : result.elements())
        if (j.isObject() && j.has("range") && !j.has("location")) tree = true;
    if (tree) {
        std::vector<const Json*> top;
        for (const Json& j : result.elements()) top.push_back(&j);
        std::stable_sort(top.begin(), top.end(), [](const Json* a, const Json* b) {
            return positionBefore(parsePosition((*a)["range"]["start"]),
                                  parsePosition((*b)["range"]["start"]));
        });
        for (const Json* j : top) addDocumentSymbol(*j, "", 0, out);
        return out;
    }
    for (const Json& j : result.elements()) {
        Symbol s;
        if (!symbolInformation(j, s)) continue;
        s.uri.clear();   // the file asked about
        out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const Symbol& a, const Symbol& b) {
        return positionBefore(a.range.start, b.range.start);
    });
    return out;
}

std::vector<Symbol> parseWorkspaceSymbols(const Json& result) {
    std::vector<Symbol> out;
    for (const Json& j : result.elements()) {
        Symbol s;
        if (symbolInformation(j, s) && !s.uri.empty()) out.push_back(s);
    }
    return out;
}

std::vector<Symbol> mergeSymbols(const std::vector<std::vector<Symbol>>& perServer,
                                 const CanonicalUri& canonical) {
    std::vector<Symbol> out;
    std::set<std::tuple<std::string, std::string, int, int>> seen;
    for (const std::vector<Symbol>& list : perServer)
        for (const Symbol& s : list) {
            const std::string uri = canonical && !s.uri.empty() ? canonical(s.uri) : s.uri;
            auto key = std::make_tuple(s.name, uri, s.hasRange ? s.range.start.line : -1,
                                       s.hasRange ? s.range.start.character : -1);
            if (!seen.insert(key).second) continue;
            out.push_back(s);
        }
    return out;
}

int symbolAt(const std::vector<Symbol>& symbols, Position p) {
    int best = -1;
    for (size_t i = 0; i < symbols.size(); i++) {
        const Symbol& s = symbols[i];
        if (!s.hasRange || !rangeHolds(s.range, p)) continue;
        if (best < 0) { best = (int)i; continue; }
        const Symbol& b = symbols[(size_t)best];
        // The deeper one; at one depth (a flat list) the one inside the other,
        // else the one starting last.
        bool inside = rangeHolds(b.range, s.range.start) && rangeHolds(b.range, s.range.end);
        if (s.depth > b.depth ||
            (s.depth == b.depth && (inside || !positionBefore(s.range.start, b.range.start))))
            best = (int)i;
    }
    return best;
}

namespace {
struct KindInfo {
    const char* name;
    const char* plural;
    const char16_t* glyph;
    unsigned color;
};
// VS Code's symbol icon colors: purple for functions and methods, orange for
// classes, enums, structs and events, blue for variables, fields and
// interfaces, plain for the rest.
const KindInfo kKinds[27] = {
    {"symbol", "symbols", u"•", 0xC5C5C5},
    {"file", "files", u"≣", 0xC5C5C5},
    {"module", "modules", u"{}", 0xC5C5C5},
    {"namespace", "namespaces", u"{}", 0xC5C5C5},
    {"package", "packages", u"{}", 0xC5C5C5},
    {"class", "classes", u"C", 0xEE9D28},
    {"method", "methods", u"ƒ", 0xB180D7},
    {"property", "properties", u"p", 0x75BEFF},
    {"field", "fields", u"f", 0x75BEFF},
    {"constructor", "constructors", u"ƒ", 0xB180D7},
    {"enumeration", "enumerations", u"E", 0xEE9D28},
    {"interface", "interfaces", u"I", 0x75BEFF},
    {"function", "functions", u"ƒ", 0xB180D7},
    {"variable", "variables", u"x", 0x75BEFF},
    {"constant", "constants", u"π", 0xC5C5C5},
    {"string", "strings", u"#", 0xC5C5C5},
    {"number", "numbers", u"#", 0xC5C5C5},
    {"boolean", "booleans", u"◐", 0xC5C5C5},
    {"array", "arrays", u"[]", 0xC5C5C5},
    {"object", "objects", u"{}", 0xC5C5C5},
    {"key", "keys", u"k", 0xC5C5C5},
    {"null", "nulls", u"∅", 0xC5C5C5},
    {"enumeration member", "enumeration members", u"e", 0x75BEFF},
    {"struct", "structs", u"S", 0xEE9D28},
    {"event", "events", u"ϟ", 0xEE9D28},
    {"operator", "operators", u"±", 0xC5C5C5},
    {"type parameter", "type parameters", u"T", 0xC5C5C5},
};
const KindInfo& kindInfo(int kind) { return kKinds[kind >= 1 && kind <= 26 ? kind : 0]; }
}  // namespace

std::string symbolKindName(int kind) { return kindInfo(kind).name; }
std::string symbolKindPlural(int kind) { return kindInfo(kind).plural; }
std::u16string symbolKindGlyph(int kind) { return kindInfo(kind).glyph; }
unsigned symbolKindColor(int kind) { return kindInfo(kind).color; }

size_t WorkspaceEdit::editCount() const {
    size_t n = 0;
    for (const FileEdit& f : files) n += f.edits.size();
    return n;
}

WorkspaceEdit parseWorkspaceEdit(const Json& j, const CanonicalUri& canonical) {
    WorkspaceEdit we;
    if (!j.isObject()) return we;
    std::map<std::string, size_t> at;
    auto add = [&](const std::string& named, const Json& edits) {
        if (named.empty()) return;
        std::string uri = canonical ? canonical(named) : named;
        auto it = at.find(uri);
        if (it == at.end()) {
            it = at.emplace(uri, we.files.size()).first;
            we.files.push_back(FileEdit{uri, {}});
        }
        for (const Json& e : edits.elements()) {
            if (!e.isObject() || !e["range"].isObject() || !e["newText"].isString()) continue;
            TextEdit t;
            t.range = parseRange(e["range"]);
            if (positionBefore(t.range.end, t.range.start)) std::swap(t.range.start, t.range.end);
            t.newText = e["newText"].asString();
            we.files[it->second].edits.push_back(t);
        }
    };
    // documentChanges wins when a server sends both, as the spec says.
    const Json& dc = j["documentChanges"];
    if (dc.isArray()) {
        for (const Json& c : dc.elements()) {
            if (c["kind"].isString()) {   // create, rename or delete a file
                we.resourceOperations = true;
                continue;
            }
            const std::string named = c["textDocument"]["uri"].asString();
            add(named, c["edits"]);
            const Json& v = c["textDocument"]["version"];
            if (!named.empty() && v.isNumber()) {
                FileEdit& f = we.files[at[canonical ? canonical(named) : named]];
                // Two entries for one file at different versions: the edit
                // cannot be consistent with any one text.
                if (f.version >= 0 && f.version != (int)v.asInt())
                    we.problem = "The server's edits to " + pathFromUri(f.uri) +
                                 " are for different versions of it";
                f.version = (int)v.asInt();
            }
        }
    } else if (j["changes"].isObject()) {
        for (const auto& kv : j["changes"].items()) add(kv.first, kv.second);
    }
    std::vector<FileEdit> kept;
    for (FileEdit& f : we.files) {
        std::stable_sort(f.edits.begin(), f.edits.end(), [](const TextEdit& a, const TextEdit& b) {
            return positionBefore(a.range.start, b.range.start);
        });
        std::vector<TextEdit> unique;
        for (const TextEdit& e : f.edits) {
            bool dup = false;
            for (auto k = unique.rbegin(); k != unique.rend() && k->range.start == e.range.start; ++k)
                if (k->range.end == e.range.end && k->newText == e.newText) { dup = true; break; }
            if (!dup) unique.push_back(e);
        }
        f.edits.swap(unique);
        for (size_t i = 1; i < f.edits.size() && we.problem.empty(); i++)
            if (positionBefore(f.edits[i].range.start, f.edits[i - 1].range.end))
                we.problem = "The server's edits to " + pathFromUri(f.uri) + " overlap";
        if (!f.edits.empty()) kept.push_back(std::move(f));
    }
    we.files.swap(kept);
    if (we.resourceOperations)
        we.problem = "The edit creates, renames or deletes files, which minicode does not do";
    return we;
}

namespace {
int versionIn(const DocVersions& v, const std::string& uri) {
    auto it = v.find(uri);
    if (it != v.end()) return it->second;
    const std::string path = pathFromUri(uri);
    if (!path.empty())
        for (const auto& kv : v)
            if (pathFromUri(kv.first) == path) return kv.second;
    return -1;   // not open
}
}  // namespace

std::string staleEditFile(const WorkspaceEdit& we, const DocVersions& asked,
                          const DocVersions& now) {
    for (const FileEdit& f : we.files) {
        const int current = versionIn(now, f.uri);
        bool stale = f.version >= 0 && f.version != current;
        const int then = versionIn(asked, f.uri);
        if (then >= 0 && then != current) stale = true;   // changed, or closed since
        if (stale) return f.uri;
    }
    return "";
}

std::vector<OffsetEdit> offsetEdits(const std::u16string& text,
                                    const std::vector<TextEdit>& edits) {
    std::vector<OffsetEdit> out;
    out.reserve(edits.size());
    const PositionIndex index(text);
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        size_t a = index.offset(it->range.start);
        size_t b = index.offset(it->range.end);
        if (b < a) b = a;
        out.push_back(OffsetEdit{a, b - a, toUtf16(it->newText)});
    }
    return out;
}

std::u16string applyTextEdits(const std::u16string& text, const std::vector<TextEdit>& edits) {
    std::u16string out = text;
    for (const OffsetEdit& e : offsetEdits(text, edits)) out.replace(e.start, e.length, e.text);
    return out;
}

PrepareRename parsePrepareRename(const Json& result) {
    PrepareRename p;
    if (!result.isObject()) return p;
    if (result.has("defaultBehavior")) {
        p.ok = result["defaultBehavior"].asBool();
    } else if (result["range"].isObject()) {
        p.ok = p.hasRange = true;
        p.range = parseRange(result["range"]);
        p.placeholder = result["placeholder"].asString();
    } else if (result["start"].isObject() && result["end"].isObject()) {
        p.ok = p.hasRange = true;
        p.range = parseRange(result);
    }
    return p;
}

std::vector<ReferenceFile> groupReferences(const std::vector<Location>& locs,
                                           const std::string& firstUriIn,
                                           const CanonicalUri& canonical) {
    std::vector<ReferenceFile> files;
    std::map<std::string, size_t> at;
    const std::string firstUri = canonical && !firstUriIn.empty() ? canonical(firstUriIn)
                                                                  : firstUriIn;
    for (const Location& l : locs) {
        std::string uri = canonical ? canonical(l.uri) : l.uri;
        auto it = at.find(uri);
        if (it == at.end()) {
            it = at.emplace(uri, files.size()).first;
            files.push_back(ReferenceFile{uri, {}});
        }
        files[it->second].ranges.push_back(l.range);
    }
    for (ReferenceFile& f : files) {
        std::stable_sort(f.ranges.begin(), f.ranges.end(), [](const Range& a, const Range& b) {
            return positionBefore(a.start, b.start) ||
                   (a.start == b.start && positionBefore(a.end, b.end));
        });
        f.ranges.erase(std::unique(f.ranges.begin(), f.ranges.end(),
                                   [](const Range& a, const Range& b) {
                                       return a.start == b.start && a.end == b.end;
                                   }),
                       f.ranges.end());
    }
    std::stable_sort(files.begin(), files.end(), [&](const ReferenceFile& a,
                                                     const ReferenceFile& b) {
        bool fa = a.uri == firstUri, fb = b.uri == firstUri;
        if (fa != fb) return fa;
        std::string pa = pathFromUri(a.uri), pb = pathFromUri(b.uri);
        return (pa.empty() ? a.uri : pa) < (pb.empty() ? b.uri : pb);
    });
    return files;
}

std::u16string lineOf(const std::u16string& text, int line) {
    return lineOf(PositionIndex(text), line);
}

std::u16string lineOf(const PositionIndex& index, int line) {
    if (line < 0 || (size_t)line >= index.lineCount()) return u"";
    const std::u16string& text = index.text();
    size_t start = index.offset(Position{line, 0});
    size_t end = start;
    while (end < text.size() && text[end] != u'\n' && text[end] != u'\r') end++;
    return text.substr(start, end - start);
}

CodeAction parseCodeAction(const Json& j) {
    CodeAction a;
    if (!j.isObject()) return a;
    a.title = trim(j["title"].asString());
    if (j["command"].isString()) {   // a bare Command
        a.command = j;
        return a;
    }
    a.kind = j["kind"].asString();
    a.preferred = j["isPreferred"].asBool();
    if (j["edit"].isObject()) a.edit = j["edit"];
    if (j["command"].isObject()) a.command = j["command"];
    if (j["disabled"].isObject()) {
        a.disabledReason = j["disabled"]["reason"].asString();
        if (a.disabledReason.empty()) a.disabledReason = "unavailable";
    }
    a.raw = std::make_shared<const Json>(j);
    return a;
}

std::vector<CodeAction> parseCodeActions(const Json& result, bool canResolve) {
    std::vector<CodeAction> out;
    for (const Json& j : result.elements()) {
        CodeAction a = parseCodeAction(j);
        if (a.title.empty() || !a.disabledReason.empty()) continue;
        if (!a.hasEdit() && !a.hasCommand() && !(canResolve && a.raw)) continue;
        out.push_back(std::move(a));
    }
    std::stable_partition(out.begin(), out.end(), [](const CodeAction& a) { return a.preferred; });
    return out;
}

Json diagnosticsInRange(const std::vector<Diagnostic>& diags, Range range) {
    Json out = Json::array();
    for (const Diagnostic& d : diags) {
        if (positionBefore(d.range.end, range.start) || positionBefore(range.end, d.range.start))
            continue;
        if (d.raw) {
            out.push(*d.raw);
            continue;
        }
        Json j = Json::object({{"range", Json::object({{"start", positionJson(d.range.start)},
                                                       {"end", positionJson(d.range.end)}})},
                               {"severity", (int)d.severity},
                               {"message", d.message}});
        if (!d.source.empty()) j.set("source", d.source);
        if (!d.code.empty()) j.set("code", d.code);
        out.push(j);
    }
    return out;
}

bool kindUnder(const std::string& actionKind, const std::string& kind) {
    if (kind.empty() || actionKind.size() < kind.size()) return false;
    if (actionKind.compare(0, kind.size(), kind) != 0) return false;
    return actionKind.size() == kind.size() || actionKind[kind.size()] == '.';
}

std::vector<CodeAction> mergeCodeActions(const std::vector<std::vector<CodeAction>>& perServer) {
    // 0 preferred, 1 quick fixes, 2 refactorings, 3 the rest, 4 source.
    auto rank = [](const CodeAction& a) {
        if (a.preferred) return 0;
        if (kindUnder(a.kind, "quickfix")) return 1;
        if (kindUnder(a.kind, "refactor")) return 2;
        if (kindUnder(a.kind, "source")) return 4;
        return 3;
    };
    std::vector<CodeAction> all;
    for (size_t i = 0; i < perServer.size(); i++)
        for (const CodeAction& a : perServer[i]) {
            bool dup = false;
            for (const CodeAction& b : all)
                if (b.title == a.title && b.kind == a.kind) { dup = true; break; }
            if (dup) continue;
            all.push_back(a);
            all.back().origin = (int)i;
        }
    std::stable_sort(all.begin(), all.end(), [&](const CodeAction& a, const CodeAction& b) {
        return rank(a) < rank(b);
    });
    return all;
}

int pickOrganizeImports(const std::vector<CodeAction>& actions) {
    static const std::string kKind = "source.organizeImports";
    int bare = -1;
    for (size_t i = 0; i < actions.size(); i++) {
        const std::string& k = actions[i].kind;
        if (!kindUnder(k, kKind)) continue;
        if (k.size() > kKind.size()) return (int)i;
        if (bare < 0) bare = (int)i;
    }
    return bare;
}

std::vector<CompletionItem> filterCompletions(const std::vector<CompletionItem>& items,
                                              const std::string& prefix) {
    if (prefix.empty()) return items;
    std::string p = lower(prefix);
    // exact, same-case prefix, any-case prefix, subsequence
    std::vector<CompletionItem> tiers[4];
    for (const CompletionItem& it : items) {
        const std::string& raw = it.filterKey();
        if (raw == prefix || it.textToInsert() == prefix) { tiers[0].push_back(it); continue; }
        if (raw.compare(0, prefix.size(), prefix) == 0) { tiers[1].push_back(it); continue; }
        std::string key = lower(raw);
        if (key.compare(0, p.size(), p) == 0) { tiers[2].push_back(it); continue; }
        size_t k = 0;
        for (char c : key)
            if (k < p.size() && c == p[k]) k++;
        if (k == p.size()) tiers[3].push_back(it);
    }
    std::vector<CompletionItem> out;
    for (auto& t : tiers) out.insert(out.end(), t.begin(), t.end());
    return out;
}

int exactCompletion(const std::vector<CompletionItem>& shown, const std::string& typed) {
    if (typed.empty()) return -1;
    for (size_t i = 0; i < shown.size(); i++)
        if (shown[i].filterKey() == typed || shown[i].textToInsert() == typed) return (int)i;
    return -1;
}

bool completionKeyAccepts(CompletionOrigin origin, bool navigated, CompletionKey key,
                          bool selectedEqualsTyped) {
    switch (key) {
    case CompletionKey::Tab:
        return true;
    case CompletionKey::ShiftTab:
        return false;
    case CompletionKey::Return:
        // A word typed out in full: Return is a new line whatever the list.
        if (selectedEqualsTyped) return false;
        return origin != CompletionOrigin::Typing || navigated;
    }
    return false;
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
                                     {"contextSupport", true}})},
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
        {"publishDiagnostics",
         Json::object({{"relatedInformation", false},
                       {"versionSupport", true},
                       {"tagSupport",
                        Json::object({{"valueSet", Json::array().push(1).push(2)}})}})},
        // Go to Symbol in File (Shift+Cmd+O) shows a tree's containers.
        {"documentSymbol", Json::object({{"hierarchicalDocumentSymbolSupport", true},
                                         {"symbolKind", symbolKinds}})},
    });
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
                                   Json::object({{"dynamicRegistration", false}})}});
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

const std::string* Client::openText(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it == docs_.end() ? nullptr : &it->second.text;
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

const std::string* Client::textFor(const std::string& uri) const {
    const Doc* d = findDoc(uri);
    return d ? &d->text : nullptr;
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
    d.text = text;
    d.version = ++lastVersion_[uri];
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
    d.version = ++lastVersion_[uri];
    fullSent_++;
    notify("textDocument/didChange",
           Json::object({
               {"textDocument", Json::object({{"uri", uri}, {"version", d.version}})},
               {"contentChanges", Json::array().push(Json::object({{"text", text}}))},
           }));
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
    if (it == docs_.end() || it->second.text == text) return;
    std::vector<ContentChange> ranged;
    if (syncKind() != 2 || changes.overflowed() || changes.regions().empty() ||
        !rangedChanges(it->second.text, text, changes.regions(), ranged)) {
        didChange(uri, text);
        return;
    }
    // Each entry costs its range besides its text: when they come to the
    // whole text's size, the whole text is as good and simpler to apply.
    size_t bytes = 0;
    for (const ContentChange& c : ranged) bytes += c.text.size() + 96;
    if (bytes >= text.size()) {
        didChange(uri, text);
        return;
    }
    Doc& d = it->second;
    d.text = text;
    d.version = ++lastVersion_[uri];
    rangedSent_++;
    Json list = Json::array();
    for (ContentChange& c : ranged)
        list.push(Json::object({{"range", Json::object({{"start", positionJson(c.range.start)},
                                                         {"end", positionJson(c.range.end)}})},
                                {"text", std::move(c.text)}}));
    notify("textDocument/didChange",
           Json::object({
               {"textDocument", Json::object({{"uri", uri}, {"version", d.version}})},
               {"contentChanges", std::move(list)},
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

const Progress* Client::progress() const {
    const Progress* best = nullptr;
    for (const auto& kv : progress_)
        if (!best || kv.second.begun > best->begun) best = &kv.second;
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
    } else if (kind == "report") {
        auto it = progress_.find(token);
        if (it == progress_.end()) return;   // never begun: nothing to update
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
    const std::vector<FileWatcher> all = fileWatchers();
    Json list = Json::array();
    size_t n = 0;
    for (const FileChange& c : changes) {
        bool wanted = false;
        for (const FileWatcher& w : all)
            if (watcherMatches(w, rootPath_, c)) { wanted = true; break; }
        if (!wanted) continue;
        list.push(Json::object({{"uri", uriFromPath(c.path)}, {"type", (int)c.type}}));
        n++;
    }
    if (n) notify("workspace/didChangeWatchedFiles", Json::object({{"changes", list}}));
    return n;
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
        if (changed && onWatchersChanged) onWatchersChanged();
    } else if (method == "window/workDoneProgress/create" ||
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
