// LspServers.cpp — see LspClient.h. Which server a file gets: languages by
// extension (built in, optional and the settings' own), default commands
// and display names, the second server per language ("<lang>.also"), what
// a server's capabilities offer (`offers`) and which server a request goes
// to (routeRequest, routeFormatting), the status line, the Python servers'
// kinds, settings and options, and configurationSection.
#include "LspClient.h"
#include "LspInternal.h"
#include <algorithm>
#include <cctype>

namespace Lsp {

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
    case Feature::InlayHint: return provides(caps["inlayHintProvider"]);
    case Feature::SemanticTokens: return offersSemanticTokens(caps);
    case Feature::Formatting: return provides(caps["documentFormattingProvider"]);
    case Feature::RangeFormatting: return provides(caps["documentRangeFormattingProvider"]);
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

int routeFormatting(const std::vector<ServerView>& servers, bool range, bool* useRange) {
    if (useRange) *useRange = false;
    auto usable = [&](size_t i, Feature f) {
        const ServerView& v = servers[i];
        return v.ready && v.capabilities && offers(*v.capabilities, f);
    };
    // Main first, then the others; for a range, a server that formats one
    // before one that formats only the whole file.
    std::vector<size_t> order;
    for (size_t i = 0; i < servers.size(); i++)
        if (servers[i].main) order.push_back(i);
    for (size_t i = 0; i < servers.size(); i++)
        if (!servers[i].main) order.push_back(i);
    if (range)
        for (size_t i : order)
            if (usable(i, Feature::RangeFormatting)) {
                if (useRange) *useRange = true;
                return (int)i;
            }
    for (size_t i : order)
        if (usable(i, Feature::Formatting)) return (int)i;
    return -1;
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
        case ServerState::Waiting:   // the bar over the file asks
            part = name + " waits for your answer about this folder";
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
            // Its inlay hints, named here so an older basedpyright that
            // kept them off shows them too; editor.inlay-hints = off asks
            // for none.
            analysis.set("inlayHints", Json::object({{"variableTypes", true},
                                                     {"functionReturnTypes", true},
                                                     {"callArgumentNames", true}}));
            out.set("python", python);
            out.set("basedpyright", Json::object({{"analysis", analysis}}));
        }
        return out;
    }
    if (kind == PythonServer::Pylsp) {
        // With snippets taken, pylsp completes a function with its
        // parameters as tab stops; Python's servers insert the bare name
        // here, as basedpyright does without completeFunctionParens.
        Json plugins = Json::object({{"jedi_completion",
                                      Json::object({{"include_params", false}})}});
        if (!interpreter.empty())
            plugins.set("jedi", Json::object({{"environment", interpreter}}));
        return Json::object({{"pylsp", Json::object({{"plugins", plugins}})}});
    }
    return Json();
}

Json pythonInitializationOptions(PythonServer kind, const std::string& interpreter) {
    if (kind != PythonServer::Jedi) return Json();
    // The bare name, as with the other Python servers (pythonSettings).
    Json out = Json::object({{"completion", Json::object({{"disableSnippets", true}})}});
    if (!interpreter.empty())
        out.set("workspace", Json::object({{"environmentPath", interpreter}}));
    return out;
}

bool pythonInterpreterNeedsRestart(PythonServer kind) {
    return kind == PythonServer::Jedi || kind == PythonServer::Other;
}

// ------------------------------------------- servers that run the folder's code
namespace {

std::string withoutTrailingSlash(std::string d) {
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d;
}

std::string parentOf(const std::string& dirIn) {
    const std::string dir = withoutTrailingSlash(dirIn);
    if (dir.empty() || dir == "/") return "";
    size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) return "";
    return slash == 0 ? "/" : dir.substr(0, slash);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    return dir == "/" ? "/" + name : dir + "/" + name;
}

// Whether `path` is `dir` or inside it.
bool within(const std::string& path, const std::string& dir) {
    if (dir.empty() || path.empty()) return false;
    if (dir == "/") return path[0] == '/';
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                           path[dir.size()] == '/');
}

}  // namespace

bool serverMayRunProjectCode(const std::string& server) {
    return server == "rust" || server == "typescript";
}

std::vector<std::string> projectFolders(const std::string& fileDirIn, const std::string& rootIn,
                                        const std::string& homeIn) {
    const std::string root = withoutTrailingSlash(rootIn), home = withoutTrailingSlash(homeIn);
    std::vector<std::string> out;
    // The home folder and the folders holding it are the user's own
    // (~/.cargo is cargo's home). A file outside the window's folder has
    // its own folders read, then the window's.
    for (std::string d : {withoutTrailingSlash(fileDirIn), root})
        for (; !d.empty() && !within(home, d) && out.size() < 4096; d = parentOf(d)) {
            if (std::find(out.begin(), out.end(), d) != out.end()) break;
            out.push_back(d);
        }
    return out;
}

std::string rustProjectCodeFile(const std::string& fileDir, const std::string& root,
                                const std::string& home, const ProjectFs& fs) {
    static const char* const kFiles[] = {
        ".cargo/config.toml", ".cargo/config",      // cargo runs the rustc and wrappers named
        "rust-toolchain.toml", "rust-toolchain",    // rustup runs a toolchain named by path
        "rust-analyzer.toml", "rust-project.json",  // rust-analyzer's own build commands
    };
    std::vector<std::string> dirs = projectFolders(fileDir, root, home);
    const std::string r = withoutTrailingSlash(root);
    if (!r.empty() && fs.subfolders && !within(withoutTrailingSlash(home), r))
        for (const std::string& name : fs.subfolders(r)) {
            const std::string sub = joinPath(r, name);
            if (fs.exists(joinPath(sub, "Cargo.toml"))) dirs.push_back(sub);
        }
    for (const std::string& d : dirs)
        for (const char* f : kFiles) {
            const std::string p = joinPath(d, f);
            if (fs.exists(p)) return p;
        }
    return "";
}

std::string workspaceTypescript(const std::string& root, const std::string& home,
                                const ProjectFs& fs) {
    for (const std::string& d : projectFolders(root, root, home)) {
        const std::string lib = joinPath(d, "node_modules/typescript/lib");
        if (fs.exists(joinPath(lib, "tsserver.js"))) return lib;
    }
    return "";
}

std::string ownTypescript(const std::string& serverProgram, const std::string& tsserverProgram,
                          const std::string& rootIn, const ProjectFs& fs) {
    const std::string root = withoutTrailingSlash(rootIn);
    std::vector<std::string> tries;
    // .../node_modules/typescript-language-server/lib/cli.mjs: its package
    // folder holds its own TypeScript (npm puts peers there), or the
    // node_modules it sits in does.
    const std::string pkg = parentOf(parentOf(serverProgram));
    if (!pkg.empty()) {
        tries.push_back(joinPath(pkg, "node_modules/typescript/lib/tsserver.js"));
        tries.push_back(joinPath(parentOf(pkg), "typescript/lib/tsserver.js"));
    }
    // .../typescript/bin/tsserver beside .../typescript/lib/tsserver.js.
    const std::string ts = parentOf(parentOf(tsserverProgram));
    if (!ts.empty()) tries.push_back(joinPath(ts, "lib/tsserver.js"));
    for (const std::string& p : tries)
        if (!within(p, root) && fs.exists(p)) return p;
    return "";
}

ProjectCodeCheck checkProjectCode(const std::string& server, bool allowed,
                                  const std::string& fileDir, const std::string& root,
                                  const std::string& home, const std::string& ownTs,
                                  const ProjectFs& fs) {
    ProjectCodeCheck c;
    if (allowed || !serverMayRunProjectCode(server)) return c;
    if (server == "rust") {
        c.file = rustProjectCodeFile(fileDir, root, home, fs);
        if (!c.file.empty()) {
            c.verdict = ProjectCode::Waits;
            return c;
        }
        // rust-analyzer's own names for them (its manual's Configuration).
        Json off = Json::object({
            {"cargo", Json::object({{"buildScripts", Json::object({{"enable", false}})}})},
            {"procMacro", Json::object({{"enable", false}})},
            {"checkOnSave", false},
        });
        c.verdict = ProjectCode::Restricted;
        c.settings = Json::object({{"rust-analyzer", off}});
        c.initializationOptions = std::move(off);
        return c;
    }
    // typescript-language-server
    c.file = workspaceTypescript(root, home, fs);
    if (c.file.empty()) return c;
    if (ownTs.empty()) {
        c.verdict = ProjectCode::Waits;
        return c;
    }
    c.verdict = ProjectCode::Restricted;
    c.initializationOptions = Json::object({{"tsserver", Json::object({{"path", ownTs}})}});
    return c;
}

}  // namespace Lsp
