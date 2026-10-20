// FileDetails.h — what the file tree needs to show sizes and dates and to
// sort by them, with the parts that can be tested kept here in plain C++:
//
//   - TreeSort: the order of one folder's entries by name, date modified or
//     size, ascending or descending, with folders first or mixed in.
//   - Age: "5 min", "3 h", "yesterday" for a row, "Saved 14:32" for the
//     status bar.
//   - SizeCache and measure: a folder's total size, worked out on a
//     background thread, kept per folder and thrown away along the path to
//     the root when something below changes.
//
// The Mac's EditorController and TreeDetails.mm use it; nothing here touches
// a GUI. measure() reads the disk with POSIX calls, so it runs on Linux too.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace TreeSort {

enum class Key { Name, Modified, Size };

struct Options {
    Key key = Key::Name;
    bool ascending = true;
    bool foldersFirst = true;
    bool operator==(const Options &o) const {
        return key == o.key && ascending == o.ascending && foldersFirst == o.foldersFirst;
    }
    bool operator!=(const Options &o) const { return !(*this == o); }
};

// The direction a key starts in when it is chosen: names A to Z, dates
// newest first and sizes largest first, as in Finder.
bool defaultAscending(Key key);

// The options after the user picks `key`: another key starts in its own
// direction, the same key changes nothing. Folders first is kept.
Options choose(const Options &current, Key key);

// For the user defaults: "name,asc,folders" and the like. decode() leaves
// `out` alone and returns false for anything it does not recognise.
std::string encode(const Options &o);
bool decode(const std::string &s, Options *out);

struct Entry {
    std::string name;
    bool isDir = false;
    int64_t size = -1;       // bytes; -1 while unknown (a folder's total on its way)
    double modified = 0;     // seconds since 1970
    // The entry's place in the platform's own name order (the Mac sorts
    // names with caseInsensitiveCompare:, and the tree's order by name must
    // stay exactly what it was), or -1 to compare the names here instead.
    int64_t nameRank = -1;
};

// Names in order: by nameRank when both have one, otherwise ASCII letters
// without case, then bytes. Negative, zero or positive.
int compareNames(const Entry &a, const Entry &b);

// The order to show `entries` in, as indices into it. Ties on date or size
// are broken by name, A to Z. Sorting by size while a folder's total is
// still unknown keeps the folders by name, ahead of the files, until every
// one of them is known (so the rows move once, not as each total lands);
// files are sorted by size at once, since their sizes come with the listing.
std::vector<size_t> order(const std::vector<Entry> &entries, const Options &o);

}  // namespace TreeSort

namespace Age {

// A row's compact age: "now" under a minute, "5 min" under an hour, "3 h"
// later the same day, "yesterday" the calendar day before. Anything older,
// or more than a minute in the future (a clock out of step), gives "" and
// the caller shows a date. `daysAgo` is calendar days between the two
// (0 today, 1 yesterday), which depends on the time zone, so the caller
// works it out.
std::string compact(double now, double then, int daysAgo);

// The status bar's "Saved ..." for a file last written at `then`: "Saved
// just now", "Saved 5 min ago", then "Saved 14:32" the same day, "Saved
// yesterday 14:32", and "Saved 12 Oct 14:32" before that. `time` and `date`
// are `then` formatted by the caller in the user's locale.
std::string saved(double now, double then, int daysAgo, const std::string &time,
                  const std::string &date);

}  // namespace Age

namespace FolderSizes {

// What a folder looked like when its total was taken. A folder's own
// modification time changes when an entry is added, removed or renamed in
// it, not when a file deeper down grows; that is what invalidate() is for.
struct Stamp {
    uint64_t dev = 0, ino = 0;
    int64_t mtimeNs = 0;
    bool operator==(const Stamp &o) const {
        return dev == o.dev && ino == o.ino && mtimeNs == o.mtimeNs;
    }
    bool operator!=(const Stamp &o) const { return !(*this == o); }
};

struct Total {
    int64_t bytes = 0;
    int64_t files = 0;
    bool capped = false;   // counting stopped early: `bytes` is a lower bound
};

// Totals by folder path, shared by the main thread (lookups, invalidation)
// and the thread that measures (stores). Thread safe.
class SizeCache {
public:
    // The total for `path`, if there is one that is still valid and was
    // taken while the folder looked like `stamp`.
    bool lookup(const std::string &path, const Stamp &stamp, Total *out) const;
    // A total, kept unless something under the folder was invalidated after
    // `since` (from clock(), read before the measuring began), in which case
    // it may be stale and is refused.
    bool store(const std::string &path, const Stamp &stamp, const Total &total,
               uint64_t since);
    uint64_t clock() const;
    // Something changed in `path` (a folder FSEvents named): its total and
    // those of all the folders above it are no longer right.
    void invalidate(const std::string &path);
    // The same, and every folder below `path` too (FSEvents could not say
    // what changed in there).
    void invalidateTree(const std::string &path);
    void clear();
    size_t validCount() const;

private:
    struct Rec {
        Stamp stamp;
        Total total;
        bool valid = false;
        uint64_t invalidatedAt = 0;
    };
    void markLocked(const std::string &path);
    mutable std::mutex mu_;
    std::map<std::string, Rec> recs_;
    uint64_t clock_ = 0;
};

// The folders above `path`, nearest first: "/a/b/c" gives "/a/b", "/a", "/".
std::vector<std::string> ancestors(const std::string &path);

struct Limits {
    int64_t maxEntries = 200000;   // directory entries looked at, per measure
    double maxSeconds = 2.0;
    int maxDepth = 256;
};

// The folder's identity and modification time (lstat). False when it is
// not there or not a folder (a symbolic link to one is not).
bool stampOf(const std::string &path, Stamp *out);

// Adds up the size of every regular file under `path`, hidden ones
// included, as `du` and Finder count them: never through a symbolic link,
// and never into another volume. A hard link counts once per name.
// Subfolders measured on the way are stored in `cache`, and valid ones
// found there are used as they are, so measuring again after a change
// reads only the folders on the path to it. At the limits it stops with
// `capped` set and the total so far. Returns false, storing no total for
// `path`, when `cancel` turns true or `path` is not a folder.
bool measure(const std::string &path, SizeCache &cache, const Limits &limits,
             const std::atomic<bool> *cancel, Total *out);

}  // namespace FolderSizes
