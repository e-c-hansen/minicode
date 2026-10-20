// FileDetails.cpp — see FileDetails.h.
#include "FileDetails.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>

#include <dirent.h>
#include <sys/stat.h>

namespace TreeSort {

bool defaultAscending(Key key) { return key == Key::Name; }

Options choose(const Options &current, Key key) {
    if (current.key == key) return current;
    Options o = current;
    o.key = key;
    o.ascending = defaultAscending(key);
    return o;
}

std::string encode(const Options &o) {
    const char *key = o.key == Key::Name ? "name" : o.key == Key::Modified ? "modified" : "size";
    return std::string(key) + (o.ascending ? ",asc" : ",desc") +
           (o.foldersFirst ? ",folders" : ",mixed");
}

bool decode(const std::string &s, Options *out) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t comma = s.find(',', start);
        parts.push_back(s.substr(start, comma == std::string::npos ? std::string::npos
                                                                   : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() != 3) return false;
    Options o;
    if (parts[0] == "name") o.key = Key::Name;
    else if (parts[0] == "modified") o.key = Key::Modified;
    else if (parts[0] == "size") o.key = Key::Size;
    else return false;
    if (parts[1] == "asc") o.ascending = true;
    else if (parts[1] == "desc") o.ascending = false;
    else return false;
    if (parts[2] == "folders") o.foldersFirst = true;
    else if (parts[2] == "mixed") o.foldersFirst = false;
    else return false;
    *out = o;
    return true;
}

static unsigned char lowerAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

int compareNames(const Entry &a, const Entry &b) {
    if (a.nameRank >= 0 && b.nameRank >= 0)
        return a.nameRank < b.nameRank ? -1 : a.nameRank > b.nameRank ? 1 : 0;
    const std::string &x = a.name, &y = b.name;
    const size_t n = std::min(x.size(), y.size());
    for (size_t i = 0; i < n; i++) {
        unsigned char p = lowerAscii((unsigned char)x[i]), q = lowerAscii((unsigned char)y[i]);
        if (p != q) return p < q ? -1 : 1;
    }
    if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
    return x < y ? -1 : x > y ? 1 : 0;
}

std::vector<size_t> order(const std::vector<Entry> &entries, const Options &o) {
    std::vector<size_t> idx(entries.size());
    std::iota(idx.begin(), idx.end(), 0);
    bool pendingDirs = false;
    if (o.key == Key::Size)
        for (const Entry &e : entries)
            if (e.isDir && e.size < 0) { pendingDirs = true; break; }
    const bool groupDirs = o.foldersFirst || pendingDirs;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t i, size_t j) {
        const Entry &a = entries[i], &b = entries[j];
        if (groupDirs && a.isDir != b.isDir) return a.isDir;
        const int byName = compareNames(a, b);
        switch (o.key) {
        case Key::Name:
            if (byName != 0) return o.ascending ? byName < 0 : byName > 0;
            return false;
        case Key::Modified:
            if (a.modified != b.modified)
                return o.ascending ? a.modified < b.modified : a.modified > b.modified;
            return byName < 0;
        case Key::Size: {
            if (pendingDirs && a.isDir && b.isDir) return byName < 0;
            const int64_t sa = std::max<int64_t>(a.size, 0), sb = std::max<int64_t>(b.size, 0);
            if (sa != sb) return o.ascending ? sa < sb : sa > sb;
            return byName < 0;
        }
        }
        return false;
    });
    return idx;
}

}  // namespace TreeSort

namespace Age {

std::string compact(double now, double then, int daysAgo) {
    const double d = now - then;
    if (d < -60) return "";
    if (d < 60) return "now";
    if (d < 3600) return std::to_string((long)(d / 60)) + " min";
    if (daysAgo == 0) return std::to_string((long)(d / 3600)) + " h";
    if (daysAgo == 1) return "yesterday";
    return "";
}

std::string saved(double now, double then, int daysAgo, const std::string &time,
                  const std::string &date) {
    const double d = now - then;
    if (d >= -60 && d < 60) return "Saved just now";
    if (d >= 60 && d < 3600) return "Saved " + std::to_string((long)(d / 60)) + " min ago";
    if (d >= 0 && daysAgo == 0) return "Saved " + time;
    if (d >= 0 && daysAgo == 1) return "Saved yesterday " + time;
    return "Saved " + date + " " + time;
}

}  // namespace Age

namespace FolderSizes {

std::vector<std::string> ancestors(const std::string &path) {
    std::vector<std::string> out;
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    for (;;) {
        size_t slash = p.rfind('/');
        if (slash == std::string::npos) break;
        if (slash == 0) {
            if (p != "/") out.push_back("/");
            break;
        }
        p = p.substr(0, slash);
        out.push_back(p);
    }
    return out;
}

bool SizeCache::lookup(const std::string &path, const Stamp &stamp, Total *out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = recs_.find(path);
    if (it == recs_.end() || !it->second.valid || it->second.stamp != stamp) return false;
    if (out) *out = it->second.total;
    return true;
}

bool SizeCache::store(const std::string &path, const Stamp &stamp, const Total &total,
                      uint64_t since) {
    std::lock_guard<std::mutex> lock(mu_);
    Rec &r = recs_[path];
    if (r.invalidatedAt > since) return false;
    r.stamp = stamp;
    r.total = total;
    r.valid = true;
    return true;
}

uint64_t SizeCache::clock() const {
    std::lock_guard<std::mutex> lock(mu_);
    return clock_;
}

void SizeCache::markLocked(const std::string &path) {
    Rec &r = recs_[path];
    r.valid = false;
    r.invalidatedAt = clock_;
}

void SizeCache::invalidate(const std::string &path) {
    std::lock_guard<std::mutex> lock(mu_);
    ++clock_;
    markLocked(path);
    for (const std::string &a : ancestors(path)) markLocked(a);
}

void SizeCache::invalidateTree(const std::string &path) {
    std::lock_guard<std::mutex> lock(mu_);
    ++clock_;
    markLocked(path);
    for (const std::string &a : ancestors(path)) markLocked(a);
    const std::string prefix = path == "/" ? "/" : path + "/";
    for (auto it = recs_.lower_bound(prefix);
         it != recs_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        it->second.valid = false;
        it->second.invalidatedAt = clock_;
    }
}

void SizeCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    ++clock_;
    // Kept as invalid records, so a measure that began before the clear
    // cannot store what it found.
    for (auto &kv : recs_) {
        kv.second.valid = false;
        kv.second.invalidatedAt = clock_;
    }
}

size_t SizeCache::validCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = 0;
    for (const auto &kv : recs_) n += kv.second.valid ? 1 : 0;
    return n;
}

static int64_t mtimeNs(const struct stat &st) {
#ifdef __APPLE__
    return (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

static Stamp stampFrom(const struct stat &st) {
    Stamp s;
    s.dev = (uint64_t)st.st_dev;
    s.ino = (uint64_t)st.st_ino;
    s.mtimeNs = mtimeNs(st);
    return s;
}

bool stampOf(const std::string &path, Stamp *out) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (out) *out = stampFrom(st);
    return true;
}

namespace {

enum class Status { Done, Capped, Cancelled };

struct Walk {
    SizeCache &cache;
    const Limits &limits;
    const std::atomic<bool> *cancel;
    uint64_t since = 0;
    uint64_t dev = 0;
    int64_t entries = 0;
    std::chrono::steady_clock::time_point deadline;

    Walk(SizeCache &c, const Limits &l, const std::atomic<bool> *stop)
        : cache(c), limits(l), cancel(stop) {}

    Status walk(const std::string &dir, int depth, Total &t) {
        if (depth > limits.maxDepth) return Status::Capped;
        DIR *d = opendir(dir.c_str());
        if (!d) return Status::Done;   // unreadable: nothing we can count
        Status result = Status::Done;
        while (struct dirent *de = readdir(d)) {
            const char *name = de->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                result = Status::Cancelled;
                break;
            }
            if (++entries > limits.maxEntries ||
                ((entries & 63) == 0 && std::chrono::steady_clock::now() > deadline)) {
                result = Status::Capped;
                break;
            }
            std::string child = dir == "/" ? "/" + std::string(name) : dir + "/" + name;
            struct stat st;
            if (lstat(child.c_str(), &st) != 0) continue;
            if (S_ISLNK(st.st_mode)) continue;
            if (S_ISREG(st.st_mode)) {
                t.bytes += (int64_t)st.st_size;
                t.files++;
                continue;
            }
            if (!S_ISDIR(st.st_mode) || (uint64_t)st.st_dev != dev) continue;
            const Stamp cs = stampFrom(st);
            Total sub;
            if (cache.lookup(child, cs, &sub)) {
                t.bytes += sub.bytes;
                t.files += sub.files;
                if (sub.capped) t.capped = true;
                continue;
            }
            Status s = walk(child, depth + 1, sub);
            t.bytes += sub.bytes;
            t.files += sub.files;
            if (s != Status::Done) {
                t.capped = true;
                result = s;
                break;
            }
            if (sub.capped) t.capped = true;
            cache.store(child, cs, sub, since);
        }
        closedir(d);
        return result;
    }
};

}  // namespace

bool measure(const std::string &path, SizeCache &cache, const Limits &limits,
             const std::atomic<bool> *cancel, Total *out) {
    Stamp stamp;
    if (!stampOf(path, &stamp)) return false;
    Walk w(cache, limits, cancel);
    w.since = cache.clock();
    w.dev = stamp.dev;
    w.deadline = std::chrono::steady_clock::now() +
                 std::chrono::microseconds((int64_t)(limits.maxSeconds * 1e6));
    Total t;
    Status s = w.walk(path, 0, t);
    if (s == Status::Cancelled) return false;
    if (s == Status::Capped) t.capped = true;
    cache.store(path, stamp, t, w.since);
    if (out) *out = t;
    return true;
}

}  // namespace FolderSizes
