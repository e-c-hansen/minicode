// LspWatcher.cpp — see LspWatcher.h.
#include "LspWatcher.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace lspgtk {

struct FileWatcher::Shared {
    // Shared with the main thread.
    std::atomic<bool> active{true};
    std::mutex lock;
    std::shared_ptr<const std::vector<Lsp::WatchTarget>> targets;   // under the lock
    WatchHandler handler;                 // called on the main thread only
    std::atomic<std::size_t> batches{0};
    std::atomic<std::size_t> folderCount{0};
    GMainContext* context = nullptr;      // the thread's own
    GMainLoop* loop = nullptr;
    std::thread thread;
    std::weak_ptr<Shared> self;           // for the thread's callbacks, which get a raw pointer

    // The thread's alone.
    struct Event {
        std::string path;
        bool created = false;   // created, moved in, or renamed into place
    };
    std::vector<std::string> roots;
    std::map<std::string, GFileMonitor*> monitors;   // by folder
    Lsp::FileEventClassifier classifier;
    std::vector<Event> pending;
    GSource* timer = nullptr;
    bool full = false;   // kMaxFolders reached (said once)
};

namespace {

using Shared = FileWatcher::Shared;

void onChanged(GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent event, gpointer data);

std::string pathOf(GFile* f) {
    char* p = f ? g_file_get_path(f) : nullptr;
    std::string out = p ? p : "";
    g_free(p);
    return out;
}

bool isInside(const std::string& path, const std::string& dir) {
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                           path[dir.size()] == '/');
}

// A monitor on `dir`, made on the watcher's thread so that its events come
// there. False when the folder cannot be watched (gone, unreadable) or the
// watcher has as many as it keeps.
bool watchFolder(Shared* sh, const std::string& dir) {
    if (sh->monitors.count(dir)) return true;
    if (sh->monitors.size() >= FileWatcher::kMaxFolders) {
        if (!sh->full)
            g_printerr("MiniCode: the language servers' file watcher keeps %zu folders at most; "
                       "folders past that are not watched\n", FileWatcher::kMaxFolders);
        sh->full = true;
        return false;
    }
    GFile* f = g_file_new_for_path(dir.c_str());
    GError* error = nullptr;
    GFileMonitor* m = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
    g_object_unref(f);
    if (!m) {
        g_clear_error(&error);
        return false;
    }
    g_signal_connect(m, "changed", G_CALLBACK(onChanged), sh);
    sh->monitors[dir] = m;
    sh->folderCount = sh->monitors.size();
    return true;
}

// `top` and every folder below it a watcher reports for, each with a
// monitor; with `report`, everything found below it as created (a folder
// that appeared: what is in it came with it). The monitor comes before the
// folder is read, so nothing written meanwhile is missed (it may be seen
// twice, which the classifier and the coalescing make one).
void watchTree(Shared* sh, const std::string& top, std::vector<Lsp::FileChange>* report) {
    std::vector<std::string> stack{top};
    while (!stack.empty() && sh->active.load()) {
        const std::string dir = stack.back();
        stack.pop_back();
        if (!Lsp::watchDescends(dir) || !watchFolder(sh, dir)) continue;
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string path = dir + "/" + name;
            bool folder = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN) {
                struct stat st;
                folder = lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (report && !Lsp::watchExcluded(path))
                report->push_back({path, sh->classifier.classify(path, true, true)});
            if (folder) stack.push_back(path);   // never through a symlink: d_type says link
        }
        closedir(d);
    }
}

// A folder that went (deleted, or moved away): its monitor and those below.
void unwatchTree(Shared* sh, const std::string& dir) {
    for (auto it = sh->monitors.lower_bound(dir); it != sh->monitors.end();) {
        if (!isInside(it->first, dir)) {
            if (it->first.compare(0, dir.size(), dir) != 0) break;   // past the prefix
            ++it;
            continue;
        }
        g_signal_handlers_disconnect_by_data(it->second, sh);
        g_file_monitor_cancel(it->second);
        g_object_unref(it->second);
        it = sh->monitors.erase(it);
    }
    sh->folderCount = sh->monitors.size();
}

struct Post {
    std::shared_ptr<Shared> sh;
    std::vector<Lsp::FileChange> changes;
    std::vector<Lsp::WatchedBatch> batches;
};

// On the watcher's thread: coalesce, match against every target, and hand
// the batch and the ready messages to the main thread.
void deliver(const std::shared_ptr<Shared>& sh, std::vector<Lsp::FileChange> changes) {
    changes = Lsp::coalesceFileChanges(changes);
    if (changes.empty() || !sh->active.load()) return;
    std::shared_ptr<const std::vector<Lsp::WatchTarget>> targets;
    {
        std::lock_guard<std::mutex> hold(sh->lock);
        targets = sh->targets;
    }
    auto* post = new Post{sh, std::move(changes), {}};
    if (targets)
        for (const Lsp::WatchTarget& t : *targets)
            post->batches.push_back(Lsp::prepareWatched(t, post->changes));
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        Post* post = static_cast<Post*>(p);
        if (post->sh->active.load()) {
            post->sh->batches++;
            if (post->sh->handler) post->sh->handler(post->changes, post->batches);
        }
        return G_SOURCE_REMOVE;
    }, post, [](gpointer p) { delete static_cast<Post*>(p); });
}

// 0.3 s after the first event of a batch: each path looked at, classified,
// folders that came watched (their files reported) and folders that went
// let go, then the batch delivered.
gboolean flush(gpointer data) {
    Shared* sh = static_cast<Shared*>(data);
    if (sh->timer) {
        g_source_unref(sh->timer);
        sh->timer = nullptr;
    }
    std::shared_ptr<Shared> keep = sh->self.lock();
    if (!keep || !sh->active.load()) return G_SOURCE_REMOVE;
    std::vector<Shared::Event> events;
    events.swap(sh->pending);
    std::vector<Lsp::FileChange> changes;
    for (const Shared::Event& e : events) {
        if (Lsp::watchExcluded(e.path)) continue;
        struct stat st;
        const bool exists = lstat(e.path.c_str(), &st) == 0;
        changes.push_back({e.path, sh->classifier.classify(e.path, exists, e.created)});
        if (!exists) {
            unwatchTree(sh, e.path);
        } else if (S_ISDIR(st.st_mode) && !sh->monitors.count(e.path)) {
            watchTree(sh, e.path, &changes);
        }
    }
    deliver(keep, std::move(changes));
    return G_SOURCE_REMOVE;
}

void push(Shared* sh, GFile* f, bool created) {
    std::string path = pathOf(f);
    if (path.empty()) return;
    sh->pending.push_back({std::move(path), created});
    if (!sh->timer) {
        sh->timer = g_timeout_source_new(300);
        g_source_set_callback(sh->timer, flush, sh, nullptr);
        g_source_attach(sh->timer, sh->context);
    }
}

// Only what says a file's content or existence changed; attributes alone
// (a touch, permissions) are left out, as on the Mac.
void onChanged(GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent event, gpointer data) {
    Shared* sh = static_cast<Shared*>(data);
    if (!sh->active.load()) return;
    switch (event) {
    case G_FILE_MONITOR_EVENT_CHANGED:
        push(sh, file, false);
        break;
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        push(sh, file, true);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        push(sh, file, false);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:
        push(sh, file, false);
        if (other) push(sh, other, true);
        break;
    default:
        break;
    }
}

void run(std::shared_ptr<Shared> sh) {
    g_main_context_push_thread_default(sh->context);
    for (const std::string& root : sh->roots) watchTree(sh.get(), root, nullptr);
    if (sh->active.load()) g_main_loop_run(sh->loop);
    for (auto& [dir, m] : sh->monitors) {
        g_signal_handlers_disconnect_by_data(m, sh.get());
        g_file_monitor_cancel(m);
        g_object_unref(m);
    }
    sh->monitors.clear();
    sh->folderCount = 0;
    if (sh->timer) {
        g_source_destroy(sh->timer);
        g_source_unref(sh->timer);
        sh->timer = nullptr;
    }
    // What was queued for this context (a cancelled monitor's last events,
    // an inject) goes now, doing nothing.
    while (g_main_context_iteration(sh->context, FALSE)) {
    }
    g_main_context_pop_thread_default(sh->context);
}

}  // namespace

std::unique_ptr<FileWatcher> FileWatcher::start(const std::vector<std::string>& paths,
                                                WatchHandler handler) {
    // Existing folders, each once, none inside another (the outer one's
    // monitors reach it).
    std::vector<std::string> sorted = paths;
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::string> kept;
    for (const std::string& p : sorted) {
        struct stat st;
        if (p.empty() || stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        bool inside = false;
        for (const std::string& k : kept)
            if (isInside(p, k)) inside = true;
        if (!inside) kept.push_back(p);
    }
    if (kept.empty()) return nullptr;
    std::unique_ptr<FileWatcher> w(new FileWatcher());
    w->paths_ = kept;
    auto sh = std::make_shared<Shared>();
    sh->self = sh;
    sh->roots = kept;
    sh->handler = std::move(handler);
    sh->context = g_main_context_new();
    sh->loop = g_main_loop_new(sh->context, FALSE);
    sh->thread = std::thread(run, sh);
    w->shared_ = sh;
    return w;
}

FileWatcher::~FileWatcher() {
    if (!shared_) return;
    shared_->active = false;
    // Quit from the thread's own context, so a quit that comes before the
    // loop has started is not lost: the loop then runs this first thing.
    g_main_context_invoke(shared_->context, [](gpointer loop) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(loop));
        return G_SOURCE_REMOVE;
    }, shared_->loop);
    if (shared_->thread.joinable()) shared_->thread.join();
    g_main_loop_unref(shared_->loop);
    g_main_context_unref(shared_->context);
    shared_->loop = nullptr;
    shared_->context = nullptr;
}

void FileWatcher::setTargets(const std::vector<Lsp::WatchTarget>& targets) {
    auto copy = std::make_shared<const std::vector<Lsp::WatchTarget>>(targets);
    std::lock_guard<std::mutex> hold(shared_->lock);
    shared_->targets = std::move(copy);
}

std::size_t FileWatcher::batches() const { return shared_->batches.load(); }
std::size_t FileWatcher::folders() const { return shared_->folderCount.load(); }

void FileWatcher::inject(const std::vector<Lsp::FileChange>& changes) {
    struct Job {
        std::shared_ptr<Shared> sh;
        std::vector<Lsp::FileChange> changes;
    };
    g_main_context_invoke_full(shared_->context, G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        Job* job = static_cast<Job*>(p);
        if (job->sh->active.load()) deliver(job->sh, std::move(job->changes));
        return G_SOURCE_REMOVE;
    }, new Job{shared_, changes}, [](gpointer p) { delete static_cast<Job*>(p); });
}

}  // namespace lspgtk
