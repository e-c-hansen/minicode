// LspSessionSymbols.cpp — LspSession's symbols for the palette (PARITY.md
// item 2.12), the symbol half of the Mac's Lsp+Navigation.mm: Go to Symbol
// in File ("@", Ctrl+Shift+O) asks the file on show's server first
// (textDocument/documentSymbol, a tree flattened parent first by the core),
// the palette's own outline standing in when no server of the file lists
// symbols or the one asked refuses; Go to Symbol in Workspace ("#", Ctrl+T)
// asks every running server that offers workspace/symbol, cancelling the
// question before, and merges their answers (Lsp::mergeSymbols) as each
// comes. A server that exits counts as having answered with nothing, so
// the palette never waits on it. Opening a workspace symbol is
// revealLocation's: the file on show through the editor's reveal, another
// file through onReveal (a jump, for Go Back).
#include "LspSessionPrivate.h"

#include <algorithm>

using lspgtk::Server;

// ---- Go to Symbol in File

bool LspSession::documentSymbols(DocumentSymbolsDone done) {
    if (!done || uri_.empty()) return false;
    std::shared_ptr<Server> server = serverFor(Lsp::Feature::DocumentSymbols);
    if (!server || !server->running() || server->stopping()) return false;
    // A question still out is answered first, as refused: the palette asks
    // once a showing.
    if (sym_->documentDone) {
        DocumentSymbolsDone before = std::move(sym_->documentDone);
        sym_->documentDone = nullptr;
        before({}, true, "");
    }
    flushChanges();
    const int gen = ++sym_->documentGen;
    sym_->documentServer = server.get();
    sym_->documentDone = std::move(done);
    const std::string uri = uri_, name = server->name();
    std::weak_ptr<int> alive = life_;
    std::weak_ptr<Server> asked = server;
    server->client().documentSymbols(uri, [alive, this, gen, uri, name, asked](const Json& r,
                                                                              const Json& e) {
        if (alive.expired() || gen != sym_->documentGen || !sym_->documentDone) return;
        DocumentSymbolsDone finish = std::move(sym_->documentDone);
        sym_->documentDone = nullptr;
        sym_->documentServer = nullptr;
        // Refused, about a file no longer on show, or asked while the server
        // was starting and it turned out not to list symbols: the outline
        // stands in.
        std::shared_ptr<Server> s = asked.lock();
        if (!e.isNull() || uri != uri_ || !s ||
            !s->client().offers(Lsp::Feature::DocumentSymbols)) {
            finish({}, true, "");
            return;
        }
        finish(Lsp::parseDocumentSymbols(r), false, name);
    });
    return true;
}

// ---- Go to Symbol in Workspace

void LspSession::cancelWorkspaceSymbols() {
    sym_->workspaceGen++;
    for (const SymbolsState::Asked& a : sym_->workspaceAsked)
        if (std::shared_ptr<Server> s = a.server.lock())
            if (s->running() && !s->stopping()) s->client().cancel(a.id);
    sym_->workspaceAsked.clear();
    sym_->answers.clear();
    sym_->answered.clear();
    sym_->workspaceDone = nullptr;
}

void LspSession::workspaceSymbols(const std::string& query, WorkspaceSymbolsDone done) {
    if (!done) return;
    cancelWorkspaceSymbols();
    // Each running server once, in a steady order: the file on show's
    // first, then the others by their settings key.
    std::vector<std::shared_ptr<Server>> servers;
    auto add = [&servers](const std::shared_ptr<Server>& s) {
        if (!s || !s->running() || s->stopping()) return;
        if (s->client().state() != Lsp::Client::State::Ready) return;
        if (!s->client().offers(Lsp::Feature::WorkspaceSymbols)) return;
        if (std::find(servers.begin(), servers.end(), s) == servers.end()) servers.push_back(s);
    };
    add(server_);
    add(alsoServer_);
    for (const auto& [key, s] : servers_) add(s);
    if (servers.empty()) {
        done({}, {}, true,
             servers_.empty() ? "No language server is running in this window"
                              : "No running language server searches the workspace");
        return;
    }
    flushChanges();
    syncOpenDocuments();
    const int gen = ++sym_->workspaceGen;
    sym_->answers.assign(servers.size(), {});
    sym_->answered.assign(servers.size(), false);
    sym_->workspaceDone = std::move(done);
    std::weak_ptr<int> alive = life_;
    for (size_t i = 0; i < servers.size(); i++) {
        SymbolsState::Asked a;
        a.server = servers[i];
        a.index = i;
        a.id = servers[i]->client().workspaceSymbols(
            query, [alive, this, gen, i](const Json& r, const Json& e) {
                if (alive.expired() || gen != sym_->workspaceGen || i >= sym_->answered.size() ||
                    sym_->answered[i])
                    return;
                sym_->answered[i] = true;
                if (e.isNull()) sym_->answers[i] = Lsp::parseWorkspaceSymbols(r);
                std::vector<std::string> paths;
                std::vector<Lsp::Symbol> symbols = mergedWorkspaceSymbols(&paths);
                const bool complete = std::all_of(sym_->answered.begin(), sym_->answered.end(),
                                                  [](bool b) { return b; });
                // A copy: the palette may ask again from inside it.
                WorkspaceSymbolsDone finish = sym_->workspaceDone;
                if (complete) {
                    sym_->workspaceAsked.clear();
                    sym_->workspaceDone = nullptr;
                }
                if (finish) finish(std::move(symbols), std::move(paths), complete, "");
            });
        sym_->workspaceAsked.push_back(a);
    }
}

// The answers so far as one list, each symbol with its file as the tree
// spells it; one with no file is left out.
std::vector<Lsp::Symbol> LspSession::mergedWorkspaceSymbols(std::vector<std::string>* paths) const {
    std::vector<Lsp::Symbol> merged = Lsp::mergeSymbols(
        sym_->answers, [](const std::string& u) { return lspui::canonicalUri(u); });
    std::vector<Lsp::Symbol> symbols;
    for (Lsp::Symbol& s : merged) {
        std::string path = treePathForUri(s.uri);
        if (path.empty()) continue;
        paths->push_back(std::move(path));
        symbols.push_back(std::move(s));
    }
    return symbols;
}

// A server that exits takes its replies with it: what it was asked for
// counts as answered, with nothing.
void LspSession::finishSymbolRequestsOf(Server* s) {
    if (sym_->documentDone && sym_->documentServer == s) {
        DocumentSymbolsDone finish = std::move(sym_->documentDone);
        sym_->documentDone = nullptr;
        sym_->documentServer = nullptr;
        sym_->documentGen++;
        finish({}, true, "");
    }
    for (const SymbolsState::Asked& a : sym_->workspaceAsked) {
        std::shared_ptr<Server> held = a.server.lock();
        if (held && held.get() != s) continue;
        if (a.index >= sym_->answered.size() || sym_->answered[a.index]) continue;
        sym_->answered[a.index] = true;
        const bool complete = std::all_of(sym_->answered.begin(), sym_->answered.end(),
                                          [](bool b) { return b; });
        std::vector<std::string> paths;
        std::vector<Lsp::Symbol> symbols = mergedWorkspaceSymbols(&paths);
        WorkspaceSymbolsDone finish = sym_->workspaceDone;
        if (finish) {
            // Later: the exit is being handled now.
            later([finish, symbols, paths, complete] { finish(symbols, paths, complete, ""); });
        }
        if (complete) {
            sym_->workspaceAsked.clear();
            sym_->workspaceDone = nullptr;
            break;
        }
    }
}

void LspSession::revealSymbol(const Lsp::Symbol& symbol) {
    if (symbol.uri.empty()) return;
    Lsp::Location loc;
    loc.uri = symbol.uri;
    if (symbol.hasRange) loc.range = symbol.selectionRange;
    revealLocation(loc);
}
