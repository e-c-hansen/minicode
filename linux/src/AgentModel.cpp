// AgentModel.cpp — see AgentModel.h.
#include "AgentModel.h"

#include <gio/gio.h>
#include <glib.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <set>
#include <thread>

#include "AgentTranscript.h"
#include "AgentView.h"

using namespace AgentModelData;

namespace {

std::string realPath(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

std::string homeDir() {
    const char* h = g_get_home_dir();
    return h ? h : "";
}

double nowSeconds() { return (double)time(nullptr); }

}  // namespace

std::string AgentModel::defaultProjectsDir() {
    if (const char* e = getenv("MINICODE_CLAUDE_PROJECTS"); e && *e) return e;
    return homeDir() + "/.claude/projects";
}

std::string AgentModel::defaultTmpRoot() {
    if (const char* e = getenv("MINICODE_CLAUDE_TMP"); e && *e) return e;
    return "/tmp/claude-" + std::to_string((unsigned)getuid());
}

struct AgentModel::Impl {
    AgentModel* owner = nullptr;
    std::string projects, tmp;

    // Worker thread state.
    std::mutex m;
    std::condition_variable cv;
    bool quit = false, want = false, folderChanged = false;
    std::string folder;
    std::thread worker;

    // Published snapshot.
    mutable std::mutex snapM;
    std::shared_ptr<const Snapshot> snap = std::make_shared<Snapshot>();
    unsigned generation = 0;
    std::string lastSig;

    // Main-thread state.
    std::vector<GFileMonitor*> monitors;
    std::set<std::string> watched;
    unsigned pollId = 0, coalesceId = 0;
    bool alive = true;
    std::shared_ptr<bool> flag = std::make_shared<bool>(true);   // for idle callbacks

    void run();
    void publish(std::shared_ptr<Snapshot> s, const std::string& sig);
    void startWatching();
    void stopWatching();
    void request();
    static void onEvent(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer p);
};

namespace {
struct Delivery {
    AgentModel* model;
    std::shared_ptr<bool> alive;
};
}  // namespace

void AgentModel::Impl::request() {
    {
        std::lock_guard<std::mutex> l(m);
        want = true;
    }
    cv.notify_one();
}

void AgentModel::Impl::run() {
    AgentIndex::Options opt;
    opt.projectsDir = projects;
    opt.tmpRoot = tmp;
    AgentIndex::Index index(AgentTranscript::posixFs(), opt);
    const std::string home = realPath(homeDir());
    std::string real;
    for (;;) {
        {
            std::unique_lock<std::mutex> l(m);
            cv.wait(l, [this] { return quit || want; });
            if (quit) return;
            want = false;
            if (folderChanged) {
                folderChanged = false;
                real = realPath(folder);
                // Not / or the home folder: every session is inside those.
                index.setFolder(AgentView::indexesFolder(real, home) ? real : std::string());
                lastSig.clear();
            }
        }
        struct stat st;
        const bool missing = stat(projects.c_str(), &st) != 0;
        index.refresh();
        const double now = nowSeconds();
        auto s = std::make_shared<Snapshot>();
        std::string sig = missing ? "M" : "P";
        std::vector<AgentIndex::Index::Row> rows = index.rows();
        // Rows are each session followed by its own subagents; build flat
        // first, then nest by depth.
        std::vector<Work> flat;
        for (const AgentIndex::Index::Row& r : rows) {
            const AgentIndex::Session& x = *r.session;
            Work w;
            w.transcript = x.transcript;
            w.sessionId = x.sessionId;
            w.agentId = x.agentId;
            w.parent = x.parent;
            w.name = x.description.empty() ? "(no prompt)" : x.description;
            w.title = x.title;
            w.agentType = x.agentType;
            w.cwd = x.cwd;
            w.gitBranch = x.gitBranch;
            w.worktreePath = x.worktreePath;
            w.worktreeBranch = x.worktreeBranch;
            w.scratchpad = x.scratchpad;
            w.started = x.started;
            w.lastActivity = x.lastActivity;
            w.written = x.written;
            w.finished = x.finished;
            w.badLines = x.badLines;
            w.filesTouched = x.filesTouched();
            w.bashCommands = x.bashCommands;
            w.depth = r.depth;
            for (const AgentView::FileGroup& g : AgentView::groupFiles(x.files)) {
                Group fg;
                fg.place = g.place;
                fg.title = AgentIndex::placeName(g.place);
                for (size_t i : g.files) {
                    const AgentIndex::FileTouch& t = x.files[i];
                    File f;
                    f.path = t.path;
                    f.place = t.place;
                    f.writes = t.writes;
                    f.edits = t.edits;
                    f.failed = t.failed;
                    f.exists = stat(t.path.c_str(), &st) == 0;
                    f.folderText = AgentView::folderText(t.path, t.place, real, x.worktreePath,
                                                         x.scratchpad, home);
                    sig += f.exists ? '1' : '0';
                    fg.files.push_back(std::move(f));
                }
                w.groups.push_back(std::move(fg));
            }
            sig += x.transcript + '|' + std::to_string(x.lastActivity) + '|' +
                   std::to_string(x.written) + '|' + std::to_string(x.files.size()) + '|' +
                   std::to_string(x.bashCommands) + '|' + (x.finished ? "f" : "") +
                   x.description + x.scratchpad + '\n';
            flat.push_back(std::move(w));
        }
        // Nest from the end: a row's parent is the nearest row above it one
        // level up.
        std::vector<int> parentOf(flat.size(), -1);
        {
            std::vector<int> chain;
            for (size_t i = 0; i < flat.size(); i++) {
                chain.resize((size_t)flat[i].depth);
                if (flat[i].depth > 0 && !chain.empty()) parentOf[i] = chain.back();
                chain.push_back((int)i);
            }
        }
        for (size_t i = flat.size(); i-- > 0;) {
            s->sessionCount++;
            if (parentOf[i] >= 0) flat[(size_t)parentOf[i]].subagents.insert(
                flat[(size_t)parentOf[i]].subagents.begin(), std::move(flat[i]));
        }
        for (size_t i = 0; i < flat.size(); i++)
            if (parentOf[i] < 0) s->sessions.push_back(std::move(flat[i]));
        s->scratchpads = index.scratchpads();
        for (const std::string& p : s->scratchpads) sig += p + '\n';
        s->projectsMissing = missing;
        s->loaded = true;
        s->projectsDir = projects;
        s->tmpRoot = tmp;
        (void)now;
        publish(std::move(s), sig);
    }
}

void AgentModel::Impl::publish(std::shared_ptr<Snapshot> s, const std::string& sig) {
    if (sig == lastSig && snap->loaded) return;   // nothing shown changed
    lastSig = sig;
    s->generation = ++generation;
    {
        std::lock_guard<std::mutex> l(snapM);
        snap = std::move(s);
    }
    auto* d = new Delivery{owner, flag};
    g_idle_add(+[](gpointer p) -> gboolean {
        std::unique_ptr<Delivery> d(static_cast<Delivery*>(p));
        if (*d->alive && d->model->onChange) d->model->onChange();
        return G_SOURCE_REMOVE;
    }, d);
}

void AgentModel::Impl::onEvent(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer p) {
    auto* self = static_cast<Impl*>(p);
    if (self->coalesceId) return;
    // About 0.3 s after the first event of a burst.
    self->coalesceId = g_timeout_add(300, +[](gpointer q) -> gboolean {
        auto* i = static_cast<Impl*>(q);
        i->coalesceId = 0;
        i->startWatching();   // a new project folder, or the folder returning
        i->request();
        return G_SOURCE_REMOVE;
    }, self);
}

void AgentModel::Impl::stopWatching() {
    for (GFileMonitor* mon : monitors) {
        g_file_monitor_cancel(mon);
        g_object_unref(mon);
    }
    monitors.clear();
    watched.clear();
}

// The transcripts folder, the scratchpads' folder and each project folder
// below the first; a folder not there is watched through the nearest one
// above it that is.
void AgentModel::Impl::startWatching() {
    std::vector<std::string> want;
    for (const std::string& root : {projects, tmp}) {
        std::string p = root;
        while (p.size() > 1 && access(p.c_str(), F_OK) != 0) {
            size_t cut = p.find_last_of('/');
            p = cut == 0 || cut == std::string::npos ? std::string("/") : p.substr(0, cut);
        }
        if (p.size() > 1) want.push_back(p);
    }
    if (access(projects.c_str(), F_OK) == 0) {
        if (GDir* d = g_dir_open(projects.c_str(), 0, nullptr)) {
            while (const char* n = g_dir_read_name(d)) {
                std::string sub = projects + "/" + n;
                struct stat st;
                if (stat(sub.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) want.push_back(sub);
            }
            g_dir_close(d);
        }
    }
    std::set<std::string> now(want.begin(), want.end());
    if (now == watched) return;
    stopWatching();
    for (const std::string& p : now) {
        GFile* f = g_file_new_for_path(p.c_str());
        GFileMonitor* mon = g_file_monitor_directory(f, G_FILE_MONITOR_NONE, nullptr, nullptr);
        g_object_unref(f);
        if (!mon) continue;
        g_signal_connect(mon, "changed", G_CALLBACK(&Impl::onEvent), this);
        monitors.push_back(mon);
    }
    watched = now;
}

AgentModel::AgentModel(const std::string& folder)
    : impl_(new Impl), projectsDir_(defaultProjectsDir()), tmpRoot_(defaultTmpRoot()) {
    impl_->owner = this;
    impl_->projects = projectsDir_;
    impl_->tmp = tmpRoot_;
    impl_->folder = folder;
    impl_->folderChanged = true;
    impl_->want = true;
    impl_->worker = std::thread([this] { impl_->run(); });
    impl_->startWatching();
}

AgentModel::~AgentModel() {
    *impl_->flag = false;
    impl_->alive = false;
    if (impl_->pollId) g_source_remove(impl_->pollId);
    if (impl_->coalesceId) g_source_remove(impl_->coalesceId);
    impl_->stopWatching();
    {
        std::lock_guard<std::mutex> l(impl_->m);
        impl_->quit = true;
    }
    impl_->cv.notify_one();
    impl_->worker.join();
}

std::shared_ptr<const Snapshot> AgentModel::snapshot() const {
    std::lock_guard<std::mutex> l(impl_->snapM);
    return impl_->snap;
}

void AgentModel::setFolder(const std::string& folder) {
    {
        std::lock_guard<std::mutex> l(impl_->m);
        impl_->folder = folder;
        impl_->folderChanged = true;
        impl_->want = true;
    }
    impl_->cv.notify_one();
}

void AgentModel::refresh() {
    impl_->startWatching();
    impl_->request();
}

void AgentModel::setPanelShown(bool shown) {
    if (shown && !impl_->pollId) {
        impl_->pollId = g_timeout_add_seconds(2, +[](gpointer p) -> gboolean {
            auto* i = static_cast<Impl*>(p);
            i->startWatching();
            i->request();
            return G_SOURCE_CONTINUE;
        }, impl_.get());
        refresh();
    } else if (!shown && impl_->pollId) {
        g_source_remove(impl_->pollId);
        impl_->pollId = 0;
    }
}
