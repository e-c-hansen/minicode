// BackupStore.cpp — see BackupStore.h.
#include "BackupStore.h"

#include <glib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <unistd.h>

namespace {

double wallNow() { return (double)g_get_real_time() / 1e6; }

}  // namespace

BackupStore& BackupStore::shared() {
    // Never deleted: its thread may still be writing when the process
    // ends, and a std::thread destroyed while running ends the process.
    static BackupStore* store = new BackupStore();
    return *store;
}

BackupStore::BackupStore() {
    const char* env = g_getenv("MINICODE_BACKUP_DIR");
    if (env && *env) {
        store_.dir = env;
    } else {
        gchar* d = g_build_filename(g_get_user_data_dir(), "minicode", "Backups", nullptr);
        store_.dir = d;
        g_free(d);
    }
    while (store_.dir.size() > 1 && store_.dir.back() == '/') store_.dir.pop_back();
    store_.fs = Backups::posixFs();
    myStart_ = processStart(getpid());
    worker_ = std::thread([this] { run(); });
    worker_.detach();
}

std::string BackupStore::newId() const {
    gchar* u = g_uuid_string_random();
    std::string id = u;
    g_free(u);
    return id;
}

// ------------------------------------------------------------------ the thread

void BackupStore::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> g(lock_);
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void BackupStore::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> g(lock_);
            wake_.wait(g, [this] { return !queue_.empty(); });
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();
    }
}

void BackupStore::flush() {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    post([done] { done->set_value(); });
    f.wait();
}

uint64_t BackupStore::stamp(const std::string& id) {
    std::lock_guard<std::mutex> g(lock_);
    latest_[id] = ++generation_;
    return generation_;
}

bool BackupStore::isNewest(const std::string& id, uint64_t s) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = latest_.find(id);
    if (it == latest_.end() || it->second != s) return false;
    latest_.erase(it);
    return true;
}

void BackupStore::write(Backups::Entry e, std::string text) {
    if (!Backups::validId(e.id) || e.path.empty()) return;
    e.saved = wallNow();
    e.ownerPid = getpid();
    e.ownerStart = myStart_;
    const uint64_t s = stamp(e.id);
    auto data = std::make_shared<std::string>(std::move(text));
    post([this, e, s, data] {
        if (!isNewest(e.id, s)) return;   // a newer one follows
        if (!store_.save(e, *data))
            fprintf(stderr, "minicode: could not write the backup of %s in %s\n", e.path.c_str(),
                    store_.dir.c_str());
    });
}

void BackupStore::remove(const std::string& id) {
    if (!Backups::validId(id)) return;
    const uint64_t s = stamp(id);
    post([this, id, s] {
        if (!isNewest(id, s)) return;
        store_.discard(id);
    });
}

// ------------------------------------------------------------------ owners

// /proc/<pid>/stat: "pid (comm) state ppid ... starttime ...", the start
// being field 22 in clock ticks since boot. comm may hold spaces and
// parentheses, so the fields are counted from the last ')'.
double BackupStore::processStart(pid_t pid) {
    if (pid <= 0) return 0;
    const std::string path = "/proc/" + std::to_string((long)pid) + "/stat";
    gchar* text = nullptr;
    if (!g_file_get_contents(path.c_str(), &text, nullptr, nullptr)) return 0;
    double start = 0;
    const char* close = strrchr(text, ')');
    if (close && close[1] == ' ') {
        const char* p = close + 2;
        // Field 3 (the state) is the first after ')'; field 22 is 19 on.
        const char state = *p;
        for (int field = 3; field < 22 && *p; field++) {
            p = strchr(p, ' ');
            if (!p) break;
            p++;
        }
        const long ticks = sysconf(_SC_CLK_TCK);
        if (p && *p && state != 'Z' && state != 'X' && ticks > 0)
            start = (double)strtoull(p, nullptr, 10) / (double)ticks;
    }
    g_free(text);
    return start;
}

bool BackupStore::isLive(const Backups::Entry& e) {
    {
        std::lock_guard<std::mutex> g(lock_);
        if (claimed_.count(e.id)) return true;
    }
    if (e.ownerPid <= 0) return false;
    if (e.ownerPid == (int64_t)getpid()) return std::fabs(e.ownerStart - myStart_) < 0.5;
    const double start = processStart((pid_t)e.ownerPid);
    // The same process if it started when the backup says; a reused pid
    // started later. Clock ticks are a hundredth of a second.
    return start > 0 && std::fabs(start - e.ownerStart) < 0.5;
}

Backups::IsLive BackupStore::liveTest() {
    return [this](const Backups::Entry& e) { return isLive(e); };
}

// ------------------------------------------------------------------ restoring

Backups::Restore BackupStore::planForRoot(const std::string& root,
                                          const std::vector<std::string>& sessionFiles) {
    flush();   // this process's own writes are on disk first
    Backups::Restore r = Backups::plan(store_.list(), root, sessionFiles, liveTest());
    for (const std::string& id : r.discard) store_.discard(id);
    std::lock_guard<std::mutex> g(lock_);
    for (const auto& item : r.items) claimed_.insert(item.id);
    return r;
}

bool BackupStore::load(const std::string& id, Backups::Entry* e, std::string* text) {
    return store_.load(id, e, text);
}

std::vector<std::string> BackupStore::foldersToReopenExcept(const std::vector<std::string>& open) {
    flush();
    return Backups::foldersToReopen(store_.list(), open, liveTest());
}

void BackupStore::cleanUpOnce() {
    if (cleaned_) return;
    cleaned_ = true;
    post([this] {
        const double now = wallNow();
        for (const std::string& id : Backups::expired(store_.list(), now, Backups::kMaxAge, liveTest()))
            store_.discard(id);
        // A temporary left by a crash mid-write is debris after a day; a
        // backup that does not read is kept as long as an orphan, in case
        // someone wants to look inside.
        store_.sweepDebris(now, 24 * 3600, Backups::kMaxAge);
    });
}
