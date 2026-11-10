// LspSessionWatch.cpp — LspSession's watched files (PARITY.md item 2.2), the
// Mac's "watched files" in Lsp+Sync.mm: a server registers watchers through
// client/registerCapability (basedpyright "**" and its library folders,
// Ruff its config files), and the files created, changed and deleted on
// disk that they want reach it as workspace/didChangeWatchedFiles, which it
// would otherwise see only when a tab opens them (an agent's edits, a
// branch switch, a pip install). The capability is offered only because
// this port sends them (Client::setFileWatching, in serverForKey): told the
// client watches, rust-analyzer stops watching for itself.
//
// One watcher (lspgtk::FileWatcher, LspWatcher.h) for the window, on the
// folder and the servers' RelativePattern folders outside it, resolved
// (Lsp::watchRoots); remade when those change, and gone when no server
// watches. Batches are matched on the watcher's thread against a copy of
// each server's watchers (watchTargets), so the main thread only writes the
// ready message; a server whose watchers changed since that copy is matched
// here, as before.
#include "LspSessionPrivate.h"

#include <ctime>

using lspgtk::Server;

namespace {

double threadCpuMs() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

bool isFolder(const std::string& p) { return g_file_test(p.c_str(), G_FILE_TEST_IS_DIR); }

}  // namespace

// The servers that registered watchers, running, ready and not on their way
// out.
std::vector<std::shared_ptr<Server>> LspSession::watchingServers() const {
    std::vector<std::shared_ptr<Server>> out;
    for (const auto& [key, s] : servers_)
        if (s && s->running() && !s->stopping() &&
            s->client().state() == Lsp::Client::State::Ready && s->client().watchesFiles())
            out.push_back(s);
    return out;
}

void LspSession::scheduleWatcherUpdate() {
    if (watch_->updateIdle) return;
    watch_->updateIdle = g_idle_add([](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->watch_->updateIdle = 0;
        self->updateFileWatcher();
        return G_SOURCE_REMOVE;
    }, this);
}

void LspSession::updateFileWatcher() {
    if (watch_->stoppingAll) return;
    const std::vector<std::shared_ptr<Server>> servers = watchingServers();
    std::vector<std::string> want;
    if (!servers.empty()) {
        std::vector<std::string> bases;
        for (const auto& s : servers)
            for (const std::string& b : s->client().watchedBases()) bases.push_back(lspui::realPath(b));
        want = Lsp::watchRoots(lspui::realPath(root_), bases, isFolder);
    }
    if (!watch_->watcher || want != watch_->watcher->paths()) {
        watch_->watcher.reset();   // its thread stops, and what it had queued is dropped
        if (want.empty()) return;
        std::weak_ptr<int> alive = life_;
        watch_->watcher = lspgtk::FileWatcher::start(want,
            [alive, this](const std::vector<Lsp::FileChange>& changes,
                          const std::vector<Lsp::WatchedBatch>& batches) {
                if (!alive.expired()) filesChangedOnDisk(changes, batches);
            });
        if (!watch_->watcher) return;
    }
    watch_->watcher->setTargets(watchTargets());
}

// What each watching server's watchers are, for the watcher's thread to
// match batches against: copied here, with the folder and the bases a
// server spelled other than the kernel does (through a link) and their
// resolved spelling, which is how the watcher reports them.
std::vector<Lsp::WatchTarget> LspSession::watchTargets() const {
    std::vector<Lsp::WatchTarget> out;
    for (const auto& s : watchingServers()) {
        Lsp::WatchTarget t = s->client().watchTarget();
        t.id = s->key();
        std::vector<std::string> spelled = s->client().watchedBases();
        spelled.push_back(root_);
        for (const std::string& base : spelled) {
            const std::string real = lspui::realPath(base);
            if (!real.empty() && real != base) t.aliases.push_back({real, base});
        }
        out.push_back(std::move(t));
    }
    return out;
}

void LspSession::stopWatchingFiles() {
    if (watch_->updateIdle) g_source_remove(watch_->updateIdle);
    watch_->updateIdle = 0;
    watch_->watcher.reset();
}

// A batch from the watcher: each server gets the changes its watchers want,
// matched on the watcher's thread, so here the ready message is only
// written. A server whose watchers changed since that copy (or that had
// none in it) is matched here.
void LspSession::filesChangedOnDisk(const std::vector<Lsp::FileChange>& changes,
                                    const std::vector<Lsp::WatchedBatch>& batches) {
    const double c0 = threadCpuMs();
    watch_->batches++;
    for (const auto& s : watchingServers()) {
        const std::string id = s->key();
        const Lsp::WatchedBatch* mine = nullptr;
        for (const Lsp::WatchedBatch& b : batches)
            if (b.id == id) {
                mine = &b;
                break;
            }
        if (mine && s->client().sendWatchedBatch(*mine)) continue;
        Lsp::WatchTarget t = s->client().watchTarget();
        std::vector<std::string> spelled = s->client().watchedBases();
        spelled.push_back(root_);
        for (const std::string& base : spelled) {
            const std::string real = lspui::realPath(base);
            if (!real.empty() && real != base) t.aliases.push_back({real, base});
        }
        std::vector<Lsp::FileChange> respelled = changes;
        for (const auto& [real, base] : t.aliases)
            for (const Lsp::FileChange& c : changes)
                if (c.path.size() > real.size() && c.path.compare(0, real.size(), real) == 0 &&
                    c.path[real.size()] == '/')
                    respelled.push_back({base + c.path.substr(real.size()), c.type});
        s->client().didChangeWatchedFiles(respelled);
        watch_->matchedHere++;
    }
    watch_->lastBatchCpuMs = threadCpuMs() - c0;
}

std::vector<std::string> LspSession::watchedFolders() const {
    return watch_->watcher ? watch_->watcher->paths() : std::vector<std::string>();
}
std::size_t LspSession::watchedFolderCount() const {
    return watch_->watcher ? watch_->watcher->folders() : 0;
}
std::size_t LspSession::watchedBatches() const { return watch_->batches; }
std::size_t LspSession::watchedBatchesMatchedHere() const { return watch_->matchedHere; }
double LspSession::lastWatchedBatchCpuMs() const { return watch_->lastBatchCpuMs; }

void LspSession::injectWatchedPaths(const std::vector<std::string>& paths, int type) {
    if (!watch_->watcher) return;
    std::vector<Lsp::FileChange> changes;
    changes.reserve(paths.size());
    for (const std::string& p : paths) changes.push_back({p, (Lsp::FileChangeType)type});
    watch_->watcher->inject(changes);
}
