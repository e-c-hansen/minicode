// LspSessionAlso.cpp — LspSession's second server (PARITY.md item 2.2), the
// Mac's in Lsp+Servers.mm: a language may have a server beside its main one.
// lsp.<server>.also names one in the settings (or "off"); without it,
// Python in a project that configures Ruff (ruff.toml, .ruff.toml or a
// [tool.ruff] table, up to the folder or a .git: Formatter::configFile)
// gets Ruff's own server, "ruff server", when ruff is installed; it is
// never offered for installing unasked. Its key is "<server>.also" and it
// runs as any server does (launchServer, LspSessionServers.cpp), its
// diagnostics kept beside the main one's (LspSessionDiagnostics.cpp).
//
// Which of a file's servers answers what (Lsp::routeRequest): completion,
// hover, definition, references and rename go to the main server, also
// while it starts, else to one that offers them; code actions go to every
// server that offers them and come back merged (Lsp::serversOffering,
// mergeCodeActions, in LspSessionActions.cpp). The status bar names every
// server of the file (Lsp::statusLine).
#include "LspSessionPrivate.h"

#include "Formatter.h"

using lspgtk::Server;

namespace {

Formatter::Fs localFs() {
    Formatter::Fs fs;
    fs.exists = [](const std::string& p) { return g_file_test(p.c_str(), G_FILE_TEST_EXISTS) != 0; };
    fs.read = [](const std::string& p) {
        gchar* text = nullptr;
        gsize len = 0;
        std::string out;
        if (g_file_get_contents(p.c_str(), &text, &len, nullptr)) out.assign(text, len);
        g_free(text);
        return out;
    };
    return fs;
}

std::string folderOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

}  // namespace

void LspSession::forgetRuffConfiguration() { also_->ruffConfigured.clear(); }

// Does the project around a file configure Ruff? Remembered per folder
// until the next file comes on show.
bool LspSession::projectConfiguresRuff(const std::string& path) {
    if (path.empty()) return false;
    const std::string dir = folderOf(path);
    auto known = also_->ruffConfigured.find(dir);
    if (known != also_->ruffConfigured.end()) return known->second;
    const std::string root = root_.empty() ? std::string("/") : lspui::realPath(root_);
    const bool yes = !Formatter::configFile("ruff", lspui::realPath(path), root, localFs()).empty();
    also_->ruffConfigured[dir] = yes;
    return yes;
}

std::string LspSession::alsoCommandFor(const std::string& server, const std::string& path) {
    const std::string key = Lsp::alsoKey(server);
    if (!settings_.lspEnabled() || settings_.lspOff(key)) return "";
    std::string command = settings_.lspCommand(key);
    if (command.empty()) {
        command = Lsp::defaultAlsoCommand(server, projectConfiguresRuff(path));
        if (command.empty()) return "";
        // The built-in one only when it is installed: nothing to offer.
        if (lspgtk::resolveCommand(command, firstDirsForKey(key)).empty()) return "";
    }
    // A running one keeps the command it was started with.
    if (!servers_.count(key)) also_->commands[key] = command;
    return command;
}

bool LspSession::alsoWantsPath(const std::string& path, const std::string& key) {
    if (!settings_.lspCommand(key).empty()) return !settings_.lspOff(key);
    return projectConfiguresRuff(path);
}

std::vector<std::string> LspSession::commandsForKey(const std::string& key) const {
    if (Lsp::isAlsoKey(key)) {
        auto it = also_->commands.find(key);
        if (it != also_->commands.end() && !it->second.empty()) return {it->second};
        if (!settings_.lspCommand(key).empty() && !settings_.lspOff(key))
            return {settings_.lspCommand(key)};
        return {};
    }
    if (!settings_.lspCommand(key).empty()) return {settings_.lspCommand(key)};
    return Lsp::defaultCommands(key);
}

// ---- which server answers

std::vector<std::shared_ptr<Server>> LspSession::fileServers() const {
    std::vector<std::shared_ptr<Server>> out;
    if (uri_.empty()) return out;
    for (const std::shared_ptr<Server>& s : {server_, alsoServer_})
        if (s && s->running() && !s->stopping()) out.push_back(s);
    return out;
}

namespace {

std::vector<Lsp::ServerView> viewsOf(const std::vector<std::shared_ptr<Server>>& servers) {
    std::vector<Lsp::ServerView> views;
    for (const std::shared_ptr<Server>& s : servers) {
        Lsp::ServerView v;
        v.main = !Lsp::isAlsoKey(s->key());
        v.ready = s->client().state() == Lsp::Client::State::Ready;
        v.capabilities = &s->client().capabilities();
        views.push_back(v);
    }
    return views;
}

}  // namespace

std::shared_ptr<Server> LspSession::serverFor(Lsp::Feature f) const {
    const std::vector<std::shared_ptr<Server>> servers = fileServers();
    const int i = Lsp::routeRequest(f, viewsOf(servers));
    return i < 0 ? nullptr : servers[(size_t)i];
}

std::vector<std::shared_ptr<Server>> LspSession::serversFor(Lsp::Feature f) const {
    const std::vector<std::shared_ptr<Server>> servers = fileServers();
    std::vector<std::shared_ptr<Server>> out;
    for (int i : Lsp::serversOffering(f, viewsOf(servers))) out.push_back(servers[(size_t)i]);
    return out;
}

std::vector<std::string> LspSession::fileServerNames() const {
    std::vector<std::string> out;
    for (const std::shared_ptr<Server>& s : fileServers()) out.push_back(s->name());
    return out;
}

// ---- the status bar

// The file's servers, the main one first: those running say so together
// with the file's counts ("fakelsp + ruff: 1 error"), the main one missing
// says why first ("No Python language server found · ruff: no problems").
std::string LspSession::serversStatus(int errors, int warnings) const {
    std::vector<Lsp::ServerStatus> list;
    for (const std::shared_ptr<Server>& s : {server_, alsoServer_}) {
        if (!s || !s->running() || s->stopping()) continue;
        Lsp::ServerStatus st;
        st.name = s->name();
        st.main = !Lsp::isAlsoKey(s->key());
        st.state = s->client().state() == Lsp::Client::State::Ready ? Lsp::ServerState::Running
                                                                    : Lsp::ServerState::Starting;
        list.push_back(st);
    }
    const std::string language = Lsp::serverDisplayName(currentKey_);
    std::string line = Lsp::statusLine(language, list, errors, warnings).text;
    if (!server_ && !note_.empty())
        line = line.empty() ? note_ : note_ + " \xC2\xB7 " + line;
    return line;
}
