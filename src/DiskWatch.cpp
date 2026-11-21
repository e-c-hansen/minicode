// DiskWatch.cpp — see DiskWatch.h.
#include "DiskWatch.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

// The system's watcher (a build may name one, or MC_DISKWATCH_NONE).
#if !defined(MC_DISKWATCH_INOTIFY) && !defined(MC_DISKWATCH_FSEVENTS) && !defined(MC_DISKWATCH_NONE)
#if defined(__linux__)
#define MC_DISKWATCH_INOTIFY 1
#elif defined(__APPLE__) && defined(__clang__)
#define MC_DISKWATCH_FSEVENTS 1
#endif
#endif

#if defined(MC_DISKWATCH_INOTIFY)
#include <dirent.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#endif
#if defined(MC_DISKWATCH_FSEVENTS)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <climits>
#include <cstdlib>
#endif

namespace DiskWatch {

// ---------------------------------------------------------------- paths

bool under(const std::string& path, const std::string& dir) {
    if (path.size() < dir.size() || path.compare(0, dir.size(), dir) != 0) return false;
    return path.size() == dir.size() || path[dir.size()] == '/' || (!dir.empty() && dir.back() == '/');
}

bool relativeTo(const std::string& root, const std::string& path, std::string* rel) {
    if (!under(path, root)) return false;
    if (rel) {
        std::size_t at = root.size();
        while (at < path.size() && path[at] == '/') at++;
        *rel = path.substr(at);
    }
    return true;
}

bool cleanRelative(const std::string& rel) {
    if (rel.empty()) return true;
    if (rel.size() > 4096 || rel[0] == '/' || rel.find('\0') != std::string::npos) return false;
    std::size_t at = 0;
    for (;;) {
        const std::size_t slash = rel.find('/', at);
        const std::string name = rel.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (name.empty() || name == "." || name == "..") return false;
        if (slash == std::string::npos) return true;
        at = slash + 1;
    }
}

namespace {

std::string parentOf(const std::string& rel) {
    const std::size_t slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

std::string nameOf(const std::string& rel) {
    const std::size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

std::string join(const std::string& root, const std::string& rel) {
    if (rel.empty()) return root;
    return root.size() && root.back() == '/' ? root + rel : root + "/" + rel;
}

}  // namespace

// ---------------------------------------------------------------- Filter

Filter::Filter(std::string root, const std::vector<std::string>& open, bool caseFold)
    : root_(std::move(root)), fold_(caseFold) {
    for (const std::string& o : open) {
        if (!cleanRelative(o)) continue;
        open_.insert(o);
        for (std::string p = parentOf(o); !p.empty(); p = parentOf(p)) toward_.insert(p);
    }
}

Seen Filter::seen(const std::string& rel) { return node(rel).seen; }

const Filter::Node& Filter::node(const std::string& rel) {
    auto it = cache_.find(rel);
    if (it != cache_.end()) return it->second;
    Node n;
    if (rel.empty()) {
        n.seen = Seen::Yes;
        n.rules = FolderSearch::Ignore::Rules::atTop(root_, fold_);
        return cache_.emplace(rel, std::move(n)).first->second;
    }
    // The parent first (kept), so a walk down reads each folder's ignore
    // files once.
    const Node& parent = node(parentOf(rel));
    const Seen ps = parent.seen;
    const FolderSearch::Ignore::Rules parentRules = parent.rules;
    const bool open = open_.count(rel) > 0 || toward_.count(rel) > 0;
    if (ps == Seen::No && !open) {
        n.seen = Seen::No;
    } else if (rel == ".git" || rel == ".git/refs" || rel == ".git/refs/heads") {
        // An index or HEAD written through its lock, a branch moved.
        n.seen = Seen::Yes;
    } else if (under(rel, ".git")) {
        n.seen = open ? Seen::OpenOnly : Seen::No;
    } else {
        const bool heavy = nameOf(rel) == "node_modules" || parentRules.ignored(rel, true);
        if (open) n.seen = heavy || ps != Seen::Yes ? Seen::OpenOnly : Seen::Yes;
        else n.seen = heavy || ps != Seen::Yes ? Seen::No : Seen::Yes;
    }
    if (n.seen != Seen::No) n.rules = parentRules.enter(join(root_, rel), rel);
    return cache_.emplace(rel, std::move(n)).first->second;
}

// ---------------------------------------------------------------- Coalescer

void Coalescer::add(const Change& c, double now) {
    if (c.path.empty()) return;
    std::string p = c.path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    bool& below = paths_[p];
    below = below || c.below;
    if (first_ < 0) first_ = now;
    last_ = now;
    if (paths_.size() > cap_) fold();
}

void Coalescer::fold() {
    // Too many to name one by one: each root holding any of them, below.
    std::map<std::string, bool> folded;
    for (const auto& [p, b] : paths_)
        for (const std::string& r : roots_)
            if (under(p, r)) folded[r] = true;
    paths_.swap(folded);
}

double Coalescer::wait(double now) const {
    if (paths_.empty()) return -1;
    const double due = std::min(last_ + quiet_, first_ + most_);
    return std::max(0.0, due - now);
}

std::vector<Change> Coalescer::take() {
    std::vector<Change> out;
    std::string below;   // the last folder changed below, while the next are under it
    bool inBelow = false;
    for (const auto& [p, b] : paths_) {
        if (inBelow && under(p, below)) continue;
        out.push_back(Change{p, b});
        if (b) {
            below = p;
            inBelow = true;
        }
    }
    paths_.clear();
    first_ = last_ = -1;
    return out;
}

// ---------------------------------------------------------------- watchers

#if defined(MC_DISKWATCH_INOTIFY) || defined(MC_DISKWATCH_FSEVENTS)
namespace {

// Changes handed from the system's thread or queue to the host's, which
// polls the pipe's read end.
class Queued : public Watcher {
public:
    Queued() {
        if (pipe(wake_) == 0)
            for (int fd : wake_) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
    }
    ~Queued() override {
        for (int fd : wake_)
            if (fd >= 0) ::close(fd);
    }
    int fd() const override { return wake_[0]; }
    void take(std::vector<Change>* out) override {
        char buf[64];
        while (read(wake_[0], buf, sizeof buf) > 0) {}
        std::lock_guard<std::mutex> lk(mu_);
        out->insert(out->end(), pending_.begin(), pending_.end());
        pending_.clear();
        woke_ = false;
    }

protected:
    // The most changes waiting for the host; past it, each root below.
    static constexpr std::size_t kMaxPending = 65536;
    void setRoots(std::vector<std::string> roots) {
        std::lock_guard<std::mutex> lk(mu_);
        roots_ = std::move(roots);
    }
    void post(std::vector<Change>&& v) {
        if (v.empty()) return;
        bool wake = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (pending_.size() + v.size() > kMaxPending) {
                pending_.clear();
                for (const std::string& r : roots_) pending_.push_back(Change{r, true});
            } else {
                pending_.insert(pending_.end(), std::make_move_iterator(v.begin()),
                                std::make_move_iterator(v.end()));
            }
            wake = !woke_;
            woke_ = true;
        }
        if (wake) {
            const char x = 'x';
            (void)!write(wake_[1], &x, 1);
        }
    }

private:
    int wake_[2] = {-1, -1};
    std::mutex mu_;
    std::vector<Change> pending_;
    std::vector<std::string> roots_;
    bool woke_ = false;
};

}  // namespace
#endif

#if defined(MC_DISKWATCH_INOTIFY)
namespace {

// inotify watches one folder at a time: a watch on each folder the filter
// takes, made and dropped as folders come and go and as trees open and
// close them, on a thread of its own (a first walk of a large folder takes
// a while, and the host's thread keeps typing going meanwhile).
class Inotify : public Queued {
public:
    Inotify() {
        ifd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (pipe(cmd_) == 0)
            for (int fd : cmd_) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        thread_ = std::thread([this] { run(); });
    }
    ~Inotify() override {
        stop_ = true;
        nudge();
        if (thread_.joinable()) thread_.join();
        if (ifd_ >= 0) ::close(ifd_);
        for (int fd : cmd_)
            if (fd >= 0) ::close(fd);
    }
    void watch(const std::vector<Root>& roots) override {
        {
            std::lock_guard<std::mutex> lk(askMu_);
            asked_ = roots;
            asks_++;
        }
        std::vector<std::string> paths;
        for (const Root& r : roots) paths.push_back(r.path);
        setRoots(std::move(paths));
        nudge();
    }
    std::size_t folders() const override { return count_.load(); }
    bool settled() const override { return applied_.load() == asks_.load(); }
    bool ok() const { return ifd_ >= 0 && cmd_[0] >= 0; }

private:
    int ifd_ = -1;
    int cmd_[2] = {-1, -1};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex askMu_;
    std::vector<Root> asked_;
    std::atomic<std::uint64_t> asks_{0}, applied_{0};
    std::atomic<std::size_t> count_{0};
    // The thread's own.
    struct Watch {
        std::string path, root;
    };
    std::map<int, Watch> byWd_;
    std::map<std::string, int> byPath_;
    std::vector<Root> roots_;
    std::map<std::string, std::unique_ptr<Filter>> filters_;

    void nudge() {
        const char x = 'x';
        (void)!write(cmd_[1], &x, 1);
    }
    void run() {
        std::vector<char> buf(64 * 1024);
        for (;;) {
            pollfd p[2] = {{cmd_[0], POLLIN, 0}, {ifd_, POLLIN, 0}};
            const int n = poll(p, 2, -1);
            if (stop_) return;
            if (n < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (p[0].revents) {
                char c[64];
                while (read(cmd_[0], c, sizeof c) > 0) {}
                std::vector<Root> roots;
                std::uint64_t asks;
                {
                    std::lock_guard<std::mutex> lk(askMu_);
                    roots = asked_;
                    asks = asks_.load();
                }
                apply(roots);
                applied_ = asks;
            }
            if (p[1].revents) {
                std::vector<Change> out;
                events(buf, &out);
                post(std::move(out));
            }
        }
    }
    Filter& filterOf(const std::string& root) {
        auto it = filters_.find(root);
        if (it == filters_.end()) {
            std::vector<std::string> open;
            for (const Root& r : roots_)
                if (r.path == root) open = r.open;
            it = filters_.emplace(root, std::make_unique<Filter>(root, open)).first;
        }
        return *it->second;
    }
    bool add(const std::string& root, const std::string& path) {
        if (byPath_.count(path)) return true;
        if (byPath_.size() >= kMaxFolders) return false;
        const int wd = inotify_add_watch(ifd_, path.c_str(),
                                         IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                                             IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR |
                                             IN_DONT_FOLLOW | IN_EXCL_UNLINK);
        if (wd < 0) return false;
        // A folder watched under two spellings (a link): one watch, the
        // first name.
        if (byWd_.count(wd)) return true;
        byWd_[wd] = Watch{path, root};
        byPath_[path] = wd;
        count_ = byPath_.size();
        return true;
    }
    void remove(const std::string& path) {
        auto it = byPath_.find(path);
        if (it == byPath_.end()) return;
        inotify_rm_watch(ifd_, it->second);
        byWd_.erase(it->second);
        byPath_.erase(it);
        count_ = byPath_.size();
    }
    // Watches on `path` and below that the filter no longer takes (all of
    // them with `all`).
    void dropUnder(const std::string& root, const std::string& path, bool all) {
        std::vector<std::string> gone;
        for (auto it = byPath_.lower_bound(path); it != byPath_.end() && under(it->first, path); ++it) {
            std::string rel;
            if (all || !relativeTo(root, it->first, &rel) || filterOf(root).seen(rel) == Seen::No)
                gone.push_back(it->first);
        }
        for (const std::string& g : gone) remove(g);
    }
    // A watch on `rel` and every folder below it the filter takes, unless
    // it is watched already (then so is what is below it).
    void walk(const std::string& root, const std::string& start) {
        if (byPath_.count(join(root, start))) return;
        Filter& f = filterOf(root);
        std::vector<std::string> stack{start};
        while (!stack.empty() && byPath_.size() < kMaxFolders) {
            const std::string rel = stack.back();
            stack.pop_back();
            if (f.seen(rel) == Seen::No) continue;
            const std::string abs = join(root, rel);
            if (byPath_.count(abs) || !add(root, abs)) continue;
            DIR* d = opendir(abs.c_str());
            if (!d) continue;
            while (dirent* e = readdir(d)) {
                const std::string name = e->d_name;
                if (name == "." || name == "..") continue;
                bool dir = e->d_type == DT_DIR;
                if (e->d_type == DT_UNKNOWN) {
                    struct stat st;
                    dir = lstat((abs + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
                }
                if (dir) stack.push_back(rel.empty() ? name : rel + "/" + name);
            }
            closedir(d);
        }
    }
    void apply(const std::vector<Root>& roots) {
        const std::vector<Root> before = roots_;
        roots_ = roots;
        // Roots no longer asked for: their watches go, but those under a
        // root still asked for.
        for (const Root& old : before) {
            bool kept = false;
            for (const Root& r : roots) kept = kept || r.path == old.path;
            if (kept) continue;
            filters_.erase(old.path);
            std::vector<std::string> gone;
            for (auto it = byPath_.lower_bound(old.path); it != byPath_.end() && under(it->first, old.path); ++it) {
                bool other = false;
                for (const Root& r : roots) other = other || under(it->first, r.path);
                if (!other) gone.push_back(it->first);
            }
            for (const std::string& g : gone) remove(g);
        }
        for (const Root& r : roots) {
            const Root* was = nullptr;
            for (const Root& old : before)
                if (old.path == r.path) was = &old;
            if (!was) {
                filters_.erase(r.path);
                walk(r.path, "");
                continue;
            }
            if (was->open == r.open) continue;
            filters_.erase(r.path);   // made again with the trees' folders now
            const std::set<std::string> now(r.open.begin(), r.open.end()), then(was->open.begin(), was->open.end());
            for (const std::string& o : then)
                if (!now.count(o) && cleanRelative(o)) dropUnder(r.path, join(r.path, o), false);
            for (const std::string& o : now)
                if (!then.count(o) && cleanRelative(o)) walk(r.path, o);
        }
    }
    void events(std::vector<char>& buf, std::vector<Change>* out) {
        for (;;) {
            const ssize_t n = read(ifd_, buf.data(), buf.size());
            if (n <= 0) return;
            for (ssize_t at = 0; at < n;) {
                const inotify_event* ev = reinterpret_cast<const inotify_event*>(buf.data() + at);
                at += (ssize_t)(sizeof(inotify_event) + ev->len);
                if (ev->mask & IN_Q_OVERFLOW) {
                    for (const Root& r : roots_) out->push_back(Change{r.path, true});
                    continue;
                }
                auto it = byWd_.find(ev->wd);
                if (it == byWd_.end()) continue;
                const std::string dir = it->second.path, root = it->second.root;
                if (ev->mask & IN_IGNORED) {
                    // The folder went (or its watch was dropped).
                    byPath_.erase(dir);
                    byWd_.erase(it);
                    count_ = byPath_.size();
                    out->push_back(Change{dir, true});
                    continue;
                }
                if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                    out->push_back(Change{dir, true});
                    if (ev->mask & IN_MOVE_SELF) dropUnder(root, dir, true);   // its names are stale
                    continue;
                }
                out->push_back(Change{dir, false});
                const std::string name = ev->len ? std::string(ev->name) : std::string();
                if (name.empty()) continue;
                if (name == ".gitignore" || name == ".ignore") filters_.erase(root);   // read again
                if (!(ev->mask & IN_ISDIR)) continue;
                const std::string child = dir + "/" + name;
                if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    dropUnder(root, child, true);
                    out->push_back(Change{child, true});
                } else if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
                    std::string rel;
                    if (relativeTo(root, child, &rel)) walk(root, rel);
                    out->push_back(Change{child, true});
                }
            }
        }
    }
};

}  // namespace

std::unique_ptr<Watcher> Watcher::make() {
    auto w = std::make_unique<Inotify>();
    if (!w->ok()) return nullptr;
    return w;
}

#elif defined(MC_DISKWATCH_FSEVENTS)
namespace {

void drained(void*) {}

// FSEvents watches a whole tree: one stream a root, its callbacks on a
// serial queue of the watcher's own. It names folders (no file events),
// spelled as the system has them (/private/tmp for /tmp), so each is
// spelled again under the root as it was given.
class FsEvents : public Queued {
public:
    FsEvents() { queue_ = dispatch_queue_create("ai.ible.mutible.diskwatch", DISPATCH_QUEUE_SERIAL); }
    ~FsEvents() override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [path, s] : streams_) stop(*s);
        streams_.clear();
        dispatch_release(queue_);
    }
    void watch(const std::vector<Root>& roots) override {
        std::vector<std::string> paths;
        for (const Root& r : roots) paths.push_back(r.path);
        setRoots(paths);
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (std::find(paths.begin(), paths.end(), it->first) != paths.end()) {
                ++it;
                continue;
            }
            stop(*it->second);
            it = streams_.erase(it);
        }
        for (const std::string& p : paths)
            if (!streams_.count(p)) start(p);
    }
    std::size_t folders() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return streams_.size();
    }
    bool settled() const override { return true; }

private:
    struct Stream {
        FsEvents* self = nullptr;
        std::string given, real;
        FSEventStreamRef ref = nullptr;
    };
    mutable std::mutex mu_;
    dispatch_queue_t queue_;
    std::map<std::string, std::unique_ptr<Stream>> streams_;

    void start(const std::string& path) {
        auto s = std::make_unique<Stream>();
        s->self = this;
        s->given = path;
        char real[PATH_MAX];
        s->real = realpath(path.c_str(), real) ? std::string(real) : path;
        CFStringRef p = CFStringCreateWithCString(nullptr, s->real.c_str(), kCFStringEncodingUTF8);
        if (!p) return;
        CFArrayRef paths = CFArrayCreate(nullptr, (const void**)&p, 1, &kCFTypeArrayCallBacks);
        CFRelease(p);
        FSEventStreamContext ctx{0, s.get(), nullptr, nullptr, nullptr};
        s->ref = FSEventStreamCreate(nullptr, &FsEvents::callback, &ctx, paths, kFSEventStreamEventIdSinceNow,
                                     0.05, kFSEventStreamCreateFlagNoDefer);
        CFRelease(paths);
        if (!s->ref) return;
        FSEventStreamSetDispatchQueue(s->ref, queue_);
        if (!FSEventStreamStart(s->ref)) {
            FSEventStreamInvalidate(s->ref);
            FSEventStreamRelease(s->ref);
            return;
        }
        streams_[path] = std::move(s);
    }
    void stop(Stream& s) {
        FSEventStreamStop(s.ref);
        FSEventStreamInvalidate(s.ref);
        FSEventStreamRelease(s.ref);
        // A callback already under way on the queue finishes before the
        // stream's record goes.
        dispatch_sync_f(queue_, nullptr, drained);
    }
    static void callback(ConstFSEventStreamRef, void* info, size_t n, void* eventPaths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        Stream* s = static_cast<Stream*>(info);
        char** paths = static_cast<char**>(eventPaths);
        std::vector<Change> out;
        for (size_t i = 0; i < n; i++) {
            if (flags[i] & kFSEventStreamEventFlagHistoryDone) continue;
            std::string p = paths[i];
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            std::string rel;
            if (relativeTo(s->real, p, &rel)) p = join(s->given, rel);
            else if (!under(p, s->given)) continue;
            const bool below = (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                                            kFSEventStreamEventFlagKernelDropped)) != 0;
            out.push_back(Change{p, below});
        }
        s->self->post(std::move(out));
    }
};

}  // namespace

std::unique_ptr<Watcher> Watcher::make() { return std::make_unique<FsEvents>(); }

#else

std::unique_ptr<Watcher> Watcher::make() { return nullptr; }

#endif

}  // namespace DiskWatch
