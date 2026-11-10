// LspSessionFolderTools.cpp — LspSession and the folder's own tools
// (PARITY.md 2.3): the Mac's Lsp+Python.mm (the interpreter the Python
// servers get, the folder's environment left out until allowed) and the
// part of Lsp+Servers.mm that keeps rust-analyzer and the TypeScript server
// from running the folder's code unasked (Lsp::checkProjectCode). The
// answer is FolderConsent.h's; the question is ConsentBar.h's. The servers
// part only calls in, from the one place a server is chosen (launchServer
// in LspSessionServers.cpp); this part's state is FolderToolsState
// (LspSessionPrivate.h).
#include "LspSessionPrivate.h"

#include "FolderConsent.h"
#include "FolderTools.h"
#include "PythonEnvs.h"

namespace {

std::string dirOf(const std::string& p) {
    gchar* d = g_path_get_dirname(p.c_str());
    std::string out = d;
    g_free(d);
    return out;
}

bool runnable(const std::string& p) {
    return !p.empty() && g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE) &&
           !g_file_test(p.c_str(), G_FILE_TEST_IS_DIR);
}

std::string expandHome(const std::string& p) {
    if (p == "~") return g_get_home_dir();
    if (p.rfind("~/", 0) == 0) return std::string(g_get_home_dir()) + p.substr(1);
    return p;
}

// The servers' Python context: the Python file on show's folder (else the
// window's), the window's folder, and the environment minicode started in.
PythonEnvs::Context pythonContext(const std::string& path, bool pythonFile,
                                  const std::string& root) {
    PythonEnvs::Context ctx;
    ctx.root = lspui::realPath(root);
    ctx.fileDir = pythonFile && !path.empty() ? lspui::realPath(dirOf(path)) : ctx.root;
    ctx.home = g_get_home_dir();
    for (const char* k : {"VIRTUAL_ENV", "CONDA_PREFIX", "PYENV_ROOT", "WORKON_HOME",
                          "CONDA_ENVS_PATH", "POETRY_VIRTUALENVS_PATH", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME"})
        if (const char* v = g_getenv(k))
            if (*v) ctx.env[k] = v;
    ctx.mac = false;
    return ctx;
}

}  // namespace

void LspSession::watchFolderTools() {
    tools_->consentUsed = FolderConsent::answer(root_);
    std::weak_ptr<int> alive = life_;
    tools_->listener = FolderConsent::listen([this, alive](const std::string& key) {
        if (!alive.expired()) folderToolsChanged(key);
    });
}

void LspSession::unwatchFolderTools() {
    FolderConsent::unlisten(tools_->listener);
    tools_->listener = 0;
}

std::string LspSession::serverPythonInterpreter(std::string* source) const {
    // python.interpreter, when it can run, is the user's own choice.
    const std::string setting = settings_.pythonInterpreter();
    std::string missing;
    if (!setting.empty()) {
        const std::string p = expandHome(setting);
        if (runnable(p)) {
            if (source) *source = "settings";
            return p;
        }
        missing = "python.interpreter is missing";
    }
    const std::string answer = FolderConsent::answer(root_);
    const bool allowed = answer == "allow";
    const PythonEnvs::Context ctx = pythonContext(path_, currentKey_ == "python", root_);
    const std::string automatic = PythonEnvs::automatic(ctx, allowed);
    if (!allowed && missing.empty()) {
        const std::string project = PythonEnvs::projectInterpreter(ctx.fileDir, ctx.root);
        if (!project.empty() && project != automatic)
            missing = answer == "deny" ? "the folder's own environment is not used"
                                       : "the folder's own environment waits for your answer";
    }
    if (source) *source = missing.empty() ? "automatic" : "automatic; " + missing;
    return automatic;
}

std::string LspSession::folderEnvironmentAwaitingConsent() const {
    if (currentKey_ != "python" || path_.empty() || !settings_.lspEnabled() ||
        settings_.lspOff("python"))
        return "";
    // A choice made in the settings is the user's own.
    const std::string setting = settings_.pythonInterpreter();
    const bool chose = !setting.empty() && runnable(expandHome(setting));
    return FolderTools::environmentToAsk(pythonContext(path_, true, root_),
                                         FolderConsent::answer(root_), chose);
}

std::string LspSession::folderCodeAwaitingConsent() const {
    if (currentKey_.empty() || path_.empty() || !settings_.lspEnabled()) return "";
    auto it = tools_->projectCode.find(currentKey_);
    if (it == tools_->projectCode.end()) return "";
    const bool asks = it->second.verdict == Lsp::ProjectCode::Waits ||
                      (it->second.verdict == Lsp::ProjectCode::Restricted &&
                       servers_.count(currentKey_));
    // Read last: the status, and so this, is asked on every change.
    return asks && FolderConsent::answer(root_).empty() ? currentKey_ : "";
}

std::string LspSession::folderCodeQuestion() const {
    const std::string key = folderCodeAwaitingConsent();
    if (key.empty()) return "";
    const Lsp::ProjectCodeCheck& pc = tools_->projectCode.at(key);
    // The project's file that decided, as the window's folder names it.
    std::string file = pc.file;
    const std::string root = lspui::realPath(root_);
    if (!root.empty() && file.rfind(root + "/", 0) == 0) file = file.substr(root.size() + 1);
    return FolderTools::projectCodeQuestion(key, pc.verdict == Lsp::ProjectCode::Waits, file);
}

std::vector<std::string> LspSession::firstDirsForKey(const std::string& key) const {
    if (Lsp::languageOfKey(key) != "python") return {};
    const std::string interp = serverPythonInterpreter();
    if (interp.empty()) return {};
    return {dirOf(interp)};
}

Lsp::ProjectCodeCheck LspSession::projectCodeForKey(const std::string& key,
                                                     const std::string& program) {
    const Lsp::ProjectCodeCheck pc = checkProjectCodeFor(key, program);
    tools_->projectCode[key] = pc;   // for the bar's question
    return pc;
}

void LspSession::forgetProjectCode(const std::string& key) { tools_->projectCode.erase(key); }

void LspSession::forgetAllProjectCode() { tools_->projectCode.clear(); }

Lsp::ProjectCodeCheck LspSession::checkProjectCodeFor(const std::string& key,
                                                      const std::string& program) const {
    if (Lsp::isAlsoKey(key) || !Lsp::serverMayRunProjectCode(key)) return Lsp::ProjectCodeCheck();
    Lsp::ProjectFs fs;
    fs.exists = [](const std::string& p) { return g_file_test(p.c_str(), G_FILE_TEST_EXISTS) != 0; };
    fs.subfolders = [](const std::string& d) {
        std::vector<std::string> out;
        GDir* dir = g_dir_open(d.c_str(), 0, nullptr);
        if (!dir) return out;
        while (const char* name = g_dir_read_name(dir)) {
            if (out.size() >= 1000) break;
            if (g_file_test((d + "/" + name).c_str(), G_FILE_TEST_IS_DIR)) out.push_back(name);
        }
        g_dir_close(dir);
        return out;
    };
    const std::string root = lspui::realPath(root_);
    // The file on show's folder, when it is this server's: cargo reads its
    // configuration from where the crate is, up.
    const std::string dir =
        currentKey_ == key && !path_.empty() ? lspui::realPath(dirOf(path_)) : root;
    std::string ownTs;
    if (key == "typescript")
        ownTs = Lsp::ownTypescript(lspui::realPath(program),
                                   lspui::realPath(lspgtk::findProgram("tsserver")), root, fs);
    return Lsp::checkProjectCode(key, FolderConsent::answer(root_) == "allow", dir, root,
                                 lspui::realPath(g_get_home_dir()), ownTs, fs);
}

void LspSession::configureServer(Lsp::Client& c, const std::string& key,
                                  const std::vector<std::string>& argv,
                                  const Lsp::ProjectCodeCheck& pc) {
    tools_->consentUsed = FolderConsent::answer(root_);
    if (key == "python") {
        const std::string interp = serverPythonInterpreter();
        const Lsp::PythonServer kind = Lsp::pythonServerKind(argv);
        c.setInitializationOptions(Lsp::pythonInitializationOptions(kind, interp));
        c.setSettings(Lsp::pythonSettings(kind, interp));
    }
    if (pc.verdict == Lsp::ProjectCode::Restricted) {
        c.setInitializationOptions(pc.initializationOptions);
        if (!pc.settings.isNull()) c.setSettings(pc.settings);
    }
}

// Another answer for the folder: the servers that ran (or waited, or were
// not found) under the old one start again under the new, as the Mac's
// environmentConsentChanged: does. A question put off with × changes no
// answer; the status (and so the bar) is told either way.
void LspSession::folderToolsChanged(const std::string& key) {
    if (key != FolderConsent::keyFor(root_)) return;
    const std::string answer = FolderConsent::answer(root_);
    if (answer == tools_->consentUsed) {
        updateStatus();
        return;
    }
    tools_->consentUsed = answer;
    bool affected = false;
    for (const char* k : {"python", "rust", "typescript"})
        if (servers_.count(k) || notes_.count(k)) affected = true;
    if (!affected) {
        updateStatus();
        return;
    }
    const std::string path = path_;   // a copy: closeDocument clears path_
    closeDocument();
    stopServers({"python", "rust", "typescript"});
    documentChanged(path);
}
