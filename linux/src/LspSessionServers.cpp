// LspSessionServers.cpp — LspSession's servers (the Mac's Lsp+Servers.mm):
// which server a file wants, starting it (launchServer, the one place a
// program is chosen and run, where the folder's own tools are asked about:
// LspSessionFolderTools.cpp), what is done when one exits or refuses the
// handshake, stopping them for a new folder, new settings or a new answer
// about the folder's tools, and the status bar's text. A file's second
// server (Ruff beside the Python server) is chosen in LspSessionAlso.cpp and
// started here like any other.
#include "LspSessionPrivate.h"

using lspgtk::Server;

// ---- status

void LspSession::updateStatus() {
    std::string s;
    if (!transient_.empty()) {
        s = transient_;
    } else if (server_ || alsoServer_) {
        // The squiggles drawn, which leave out diagnostics about text typed
        // over since they came, so the count says what the squiggles say.
        int errors = 0, warnings = 0;
        countDrawn(errors, warnings);
        s = serversStatus(errors, warnings);
    } else {
        s = note_;
    }
    status_ = s;
    if (onStatus) onStatus(s);
}

void LspSession::flash(const std::string& message) {
    transient_ = message;
    updateStatus();
    if (flashId_) g_source_remove(flashId_);
    flashId_ = g_timeout_add_seconds(3, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->flashId_ = 0;
        self->transient_.clear();
        self->updateStatus();
        return G_SOURCE_REMOVE;
    }, this);
}

void LspSession::note(const std::string& message) {
    lastNote_ = message;
    flash(message);
}

bool LspSession::noServer() {
    if ((server_ || alsoServer_) && !uri_.empty()) return false;
    gtk_widget_error_bell(view_);
    flash(note_.empty() ? "No language server for this file" : note_);
    return true;
}

// ---- servers

std::string LspSession::settingsSignature(const Settings& s) const {
    std::string sig = s.lspEnabled() ? "1" : "0";
    for (const std::string& k : Settings::lspServers())
        sig += "|" + s.lspCommand(k) + "|" + s.lspCommand(Lsp::alsoKey(k));
    // Servers of the user's own, the files every server is for, and the
    // Python the Python servers are given.
    for (const std::string& k : s.lspCustomServers()) sig += "|" + k + "=" + s.lspCommand(k);
    for (const auto& e : s.lspExtensions()) {
        sig += "|" + e.first + ":";
        for (const std::string& x : e.second) sig += x + ",";
    }
    sig += "|python.interpreter=" + s.pythonInterpreter();
    return sig;
}

// The five, the servers the settings add (lsp.<name> with
// lsp.<name>.extensions), and an optional built-in one the settings name a
// command for (lsp.markdown = ...); the Mac also starts an optional one it
// finds installed.
bool LspSession::languageOf(const std::string& path, Lsp::Language& lang) const {
    return !path.empty() &&
           Lsp::languageForExtension(lspui::lowerExt(path), lang, settings_.lspExtensions()) &&
           !(Lsp::isOptionalServer(lang.server) &&
             (settings_.lspOff(lang.server) || settings_.lspCommand(lang.server).empty()));
}

std::shared_ptr<Server> LspSession::runningServer(const std::string& key) const {
    auto found = servers_.find(key);
    if (found == servers_.end() || found->second->stopping() || !found->second->running())
        return nullptr;
    return found->second;
}

std::shared_ptr<Server> LspSession::launchServer(const std::string& key, std::string* note) {
    const std::vector<std::string> commands = commandsForKey(key);
    // The folder's own tools (LspSessionFolderTools.cpp): a Python server is
    // looked for beside the interpreter it gets, and one that would run the
    // folder's code starts without it, or waits for the folder's answer.
    const std::vector<std::string> first = firstDirsForKey(key);
    for (const std::string& cmd : commands) {
        std::vector<std::string> argv = lspgtk::resolveCommand(cmd, first);
        if (argv.empty()) continue;
        const Lsp::ProjectCodeCheck pc = projectCodeForKey(key, argv[0]);
        if (pc.verdict == Lsp::ProjectCode::Waits) {
            *note = "The " + Lsp::serverDisplayName(key) +
                    " language server waits for your answer about this folder's tools";
            return nullptr;
        }
        if (std::shared_ptr<Server> server = Server::start(key, argv, root_)) {
            configureServer(server->client(), key, argv, pc);
            return server;
        }
    }
    *note = "No " + Lsp::serverDisplayName(key) + " language server found";
    return nullptr;
}

std::shared_ptr<Server> LspSession::serverForKey(const std::string& key) {
    auto found = servers_.find(key);
    if (found != servers_.end()) return found->second;
    auto noted = notes_.find(key);
    if (noted != notes_.end()) {
        note_ = noted->second;
        return nullptr;
    }
    if (settings_.lspOff(key)) {
        notes_[key] = "";   // switched off on purpose: say nothing
        return nullptr;
    }
    std::string note;
    std::shared_ptr<Server> server = launchServer(key, &note);
    if (!server) {
        note_ = note;
        notes_[key] = note;
        return nullptr;
    }
    std::weak_ptr<int> alive = life_;
    Lsp::Client& c = server->client();
    // These live in the server's own client, so the server is alive when
    // they run. A server being stopped (a new folder, a settings change) can
    // still answer or publish before it goes, and by then its replacement
    // may have the same file open under the same URI, so anything from a
    // stopping server is dropped rather than drawn over the new one's.
    Server* self = server.get();
    c.onReady = [alive, this, self] {
        if (alive.expired() || self->stopping()) return;
        updateStatus();
        problemsChanged();   // the counts show once a server runs
    };
    // Files changed on disk, for the watchers the server registers
    // (LspSessionWatch.cpp): offered because this port sends them.
    c.setFileWatching(true);
    c.onWatchersChanged = [alive, this, self] {
        if (!alive.expired() && !self->stopping()) scheduleWatcherUpdate();
    };
    c.onDiagnostics = [alive, this, self](const std::string& uri,
                                          const std::vector<Lsp::Diagnostic>& list) {
        if (!alive.expired() && !self->stopping()) diagnosticsArrived(self, uri, list);
    };
    // A server that refuses the handshake would otherwise read "starting…"
    // for good while every request is dropped: stop it and say so.
    c.onInitializeFailed = [alive, this, self](const std::string& message) {
        if (!alive.expired() && !self->stopping()) initializeFailed(self, message);
    };
    std::string name = server->name();
    c.onProtocolError = [name](const std::string& problem) {
        g_printerr("MiniCode: %s: %s\n", name.c_str(), problem.c_str());
    };
    server->onExit = [alive, this](Server* s) {
        if (!alive.expired()) serverExited(s);
    };
    // workspace/applyEdit (often while a command the server was asked to run
    // goes on): the same applier as rename's (LspSessionActions.cpp).
    std::weak_ptr<Server> weakServer = server;
    c.onApplyEdit = [alive, this, weakServer](const Json& params,
                                              Lsp::Client::ApplyEditReply reply) {
        if (alive.expired()) {
            reply(false, "the window closed");
            return;
        }
        serverAppliesEdit(weakServer, params, reply);
    };
    c.initialize(root_, (int)getpid());
    servers_[key] = server;
    return server;
}

void LspSession::initializeFailed(Server* s, const std::string& message) {
    std::string note = s->name() + " failed to start";
    if (!message.empty()) note += ": " + message;
    std::shared_ptr<Server> keep;
    auto it = servers_.find(s->key());
    if (it != servers_.end() && it->second.get() == s) {
        keep = it->second;
        servers_.erase(it);
    }
    notes_[s->key()] = note;   // not tried again until the folder or settings change
    forgetServerDocuments(s);
    scheduleWatcherUpdate();
    if (alsoServer_.get() == s) {
        alsoServer_.reset();
        if (!server_) uri_.clear();
        updateStatus();
    }
    if (server_.get() == s) {
        server_.reset();
        note_ = note;
        if (!alsoServer_) {
            uri_.clear();
            clearMarks();
        } else {
            paintDiagnostics();
        }
        closeCompletion();
        updateStatus();
    }
    s->stop();   // its client has already exited, so this just ends its input
}

void LspSession::serverExited(Server* s) {
    std::shared_ptr<Server> keep;
    auto it = servers_.find(s->key());
    if (it != servers_.end() && it->second.get() == s) {
        keep = it->second;
        servers_.erase(it);
        if (!s->stopping()) notes_[s->key()] = s->name() + " stopped";
    }
    forgetServerDocuments(s);
    scheduleWatcherUpdate();
    if (alsoServer_.get() == s) {
        alsoServer_.reset();
        if (!server_) {
            uri_.clear();
            clearMarks();
        }
        updateStatus();
    }
    if (server_.get() == s) {
        server_.reset();
        note_ = s->stopping() ? "" : notes_[s->key()];
        if (!alsoServer_) {
            uri_.clear();
            clearMarks();
        } else {
            paintDiagnostics();
        }
        closeCompletion();
        updateStatus();
    }
}

void LspSession::stopServers(const std::vector<std::string>& keys) {
    for (const std::string& k : keys) {
        auto it = servers_.find(k);
        if (it != servers_.end()) {
            forgetServerDocuments(it->second.get());
            it->second->stop();
            servers_.erase(it);
        }
        notes_.erase(k);
        forgetProjectCode(k);
    }
    scheduleWatcherUpdate();
}

void LspSession::stopAllServers() {
    watch_->stoppingAll = true;
    for (auto& kv : servers_) {
        forgetServerDocuments(kv.second.get());
        kv.second->stop();
    }
    servers_.clear();
    notes_.clear();
    watch_->stoppingAll = false;
    stopWatchingFiles();
    forgetAllProjectCode();
    forgetAllDiagnostics();
}

void LspSession::applySettings(const Settings& settings) {
    settings_ = settings;
    std::string sig = settingsSignature(settings);
    if (sig == signature_) return;
    signature_ = sig;
    std::string path = path_;
    closeDocument();
    stopAllServers();
    documentChanged(path);
}

void LspSession::setRoot(const std::string& root) {
    if (root == root_) return;
    std::string path = path_;
    closeDocument();
    stopAllServers();
    root_ = root;
    documentChanged(path);   // the open file gets a server rooted in the new folder
}

void LspSession::shutdown() {
    closeDocument();
    stopAllServers();
}
