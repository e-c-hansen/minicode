// FileDetails.h — what the file tree needs to show sizes and dates and to
// sort by them, with the parts that can be tested kept here in plain C++:
//
//   - TreeSort: the order of one folder's entries by name, date modified,
//     size, or date created, ascending or descending, with folders first or mixed in.
//   - Age: "5 min", "3 h", "yesterday" for a row, "Saved 14:32" for the
//     status bar.
//   - SizeCache and measure: a folder's total size, worked out on a
//     background thread, kept per folder and thrown away along the path to
//     the root when something below changes.
//   - TreeRefresh: which folders the tree lists again for one batch of
//     FSEvents, so a change lists only the folders it touched; and for the
//     GTK port's tree, the splices that put a fresh listing in a folder's
//     rows and when a changing folder is listed again.
//
// The Mac's EditorController and TreeDetails.mm use it, and the GTK port's
// FileTree.cpp its TreeRefresh; nothing here touches a GUI. measure() reads
// the disk with POSIX calls, so it runs on Linux too.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace TreeSort {

// The values are the tags of the Sort By menu's items; never renumber them.
enum class Key { Name, Modified, Size, Created };

struct Options {
    Key key = Key::Name;
    bool ascending = true;
    bool foldersFirst = true;
    bool operator==(const Options &o) const {
        return key == o.key && ascending == o.ascending && foldersFirst == o.foldersFirst;
    }
    bool operator!=(const Options &o) const { return !(*this == o); }
};

// The direction a key starts in when it is chosen: names A to Z, both
// dates newest first and sizes largest first, as in Finder.
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
    double created = 0;      // seconds since 1970 (the birth time); 0 when unknown
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

namespace TreeRefresh {

// A folder FSEvents named: something in it changed (an entry was made,
// removed, renamed, or written). With `subtree` (MustScanSubDirs, or events
// the system dropped) anything below it may have changed too.
struct Event {
    std::string path;
    bool subtree = false;
};

// A path as the disk spells it, in the tree's spelling: `realRoot` (the
// root with symbolic links resolved, /private/tmp/x) at its front becomes
// `root` (/tmp/x), and trailing slashes go. Other paths only lose their
// trailing slashes.
std::string treeSpelling(const std::string &path, const std::string &root,
                         const std::string &realRoot);

struct Plan {
    // The root itself under a subtree event: rescan everything.
    bool everything = false;
    // Loaded folders to list again, sorted (so a folder comes before the
    // folders inside it), each once.
    std::vector<std::string> list;
    // Entries whose own row needs a fresh stat (a folder's date changes
    // when an entry in it does), named in a folder that is listed but not
    // listed again here. Sorted, each once.
    std::vector<std::string> restat;
};

// What one batch of events asks of a tree whose loaded folders (the root
// among them) are `loaded`, all in the tree's spelling: each named folder
// that is loaded is listed again, with every loaded folder below it for a
// subtree event; a named folder whose parent is loaded has its row
// restatted unless the parent is listed anyway. A subtree event at or above
// the root is everything. Other events outside the root are left out, and
// nothing deeper than the loaded folders is ever listed.
Plan plan(const std::vector<Event> &events, const std::set<std::string> &loaded,
          const std::string &root);

// ---- A folder's rows as one sorted list (the GTK port's tree, R1 in
// linux/PARITY.md). GIO reports every entry made, removed or renamed in a
// folder on show, thousands a second in a folder being rebuilt, and the port
// once patched its rows for each one, at a cost growing with the folder:
// deleting 30,000 files of an open folder held the window for two minutes.
// Now a change only asks for the folder to be listed again (Pacer), and the
// fresh listing goes in as the fewest edits (splices).

// One entry: whether it is a folder, the caller's collation key for its name
// (GLib's, of the case-folded name, in the GTK port), and the name itself.
struct Row {
    bool folder = false;
    std::string key;
    std::string name;
};

// The tree's order: folders first, then keys by their bytes, then names by
// theirs, so two names with one key (README and readme) still have an order.
bool rowBefore(const Row &a, const Row &b);

// One edit of the list: `removed` rows at `at` go, and the fresh listing's
// rows [from, from + added) take their place.
struct Splice {
    std::size_t at = 0;
    std::size_t removed = 0;
    std::size_t from = 0;
    std::size_t added = 0;
};

// The edits that turn the rows on show, `now`, into a fresh listing, both
// sorted by rowBefore with no row twice: one for each run of changed rows
// between rows that stay, and never a row that stays taken out and put back
// (the tree would forget what was open below it, and the selection). An
// entry whose kind changed (a file replaced by a folder) goes and comes back.
// Positions are in `now`, and the edits come last first, so each one's place
// is still right once those before it are made.
std::vector<Splice> splices(const std::vector<Row> &now, const std::vector<Row> &fresh);

// When a folder that reported a change is listed again: `firstDelay` after
// the first change, so a file made in a terminal shows at once (the batch of
// events behind it gathered), and while changes keep coming no sooner than
// `minInterval` after the last listing began, one listing at a time. The
// Mac's FSEvents stream coalesces for 0.3 s. Times are milliseconds on a
// clock that only goes forward.
class Pacer {
public:
    explicit Pacer(double firstDelay = 100, double minInterval = 300)
        : firstDelay_(firstDelay), minInterval_(minInterval) {}
    // A change at `now`: the wait before listing, for the caller to start a
    // timer with; negative when a listing is already waiting or under way
    // (finished() asks for the next one then).
    double changed(double now);
    // A listing begins at `now`: the timer fired, or the folder is listed
    // for the first time.
    void started(double now);
    // The listing is on show: the wait before the next one when changes came
    // in meanwhile, else negative.
    double finished(double now);
    // A listing is waiting for its timer or under way.
    bool pending() const { return state_ != State::Idle; }

private:
    enum class State { Idle, Waiting, Listing, ListingChanged };
    double wait(double now) const;
    double firstDelay_, minInterval_;
    double lastStart_ = 0;
    bool started_ = false;
    State state_ = State::Idle;
};

}  // namespace TreeRefresh
