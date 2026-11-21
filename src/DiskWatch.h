// DiskWatch.h — what changed on disk under the session's folders, for the
// session's host (SESSION-DESIGN.md, stage 7: the host watches the disk).
// The host watches its own folder (a headless `mutible host`) and the
// folders renderers' trees have open (FsRead's "watch"), and from what
// changed it sends a changed folder's listing again to the trees that show
// it, tells Source Control to read again, and has a document no app's
// window shows follow its file (SessionHost.h, "The disk").
//
// The core's one platform interface for it, with what the two systems share
// above it:
//   - Watcher: the system's notices, behind one small interface (inotify on
//     Linux, FSEvents on the Mac), each on a thread or queue of its own,
//     handing changes over through a pipe the host's poll(2) waits on. A
//     change names a folder whose entries or files changed (the Mac's
//     FSEvents stream names folders; inotify names the folder of each
//     event), spelled under the root as it was given; `below` marks
//     everything under it (the system dropped events, or asked for a scan).
//   - Filter: which folders count. Not .git's insides (only .git itself and
//     .git/refs, .git/refs/heads: an index or HEAD written through its lock,
//     a branch moved), and not a folder the ignore files ignore or a
//     node_modules (the heavy ones: build output, packages), unless a tree
//     has it open. inotify watches one folder at a time, so it watches only
//     the folders the filter takes; FSEvents watches the whole tree, and the
//     host drops what the filter does not take.
//   - Coalescer: a burst (a branch switch, a build, an editor's atomic save
//     in three steps) is one batch: due once nothing came for `quiet`
//     seconds, or `most` seconds after its first change, whichever is first;
//     past `cap` folders, each root changed below.
//
// The filter and the coalescer are pure; the watchers do the I/O. Built on
// Linux and on the Mac with clang (FSEvents' headers are clang's); elsewhere
// (g++ on the Mac) Watcher::make() gives none and the host watches nothing,
// as before.
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "FolderSearch.h"

namespace DiskWatch {

struct Change {
    std::string path;      // a folder, absolute, under a root as it was given
    bool below = false;    // everything under it changed too
    bool operator==(const Change& o) const { return path == o.path && below == o.below; }
};

// A root to watch, with the folders a tree has open in it ('/'-separated
// from the root, "" the root itself).
struct Root {
    std::string path;
    std::vector<std::string> open;
    bool operator==(const Root& o) const { return path == o.path && open == o.open; }
};

// Whether `path` is `dir` or inside it.
bool under(const std::string& path, const std::string& dir);
// `path` from `root` ("" for the root itself): false when it is not under it.
bool relativeTo(const std::string& root, const std::string& path, std::string* rel);
// A path a renderer names for an open folder: '/'-separated names, none
// empty, "." or "..", no NUL, not absolute, at most 4096 bytes; "" is the
// root.
bool cleanRelative(const std::string& rel);

enum class Seen {
    No,         // not watched; a change there counts for nothing
    Yes,        // watched; a change there may change Source Control too
    OpenOnly,   // ignored, but open in a tree: its listing follows, git does not
};

class Filter {
public:
    Filter(std::string root, const std::vector<std::string>& open,
           bool caseFold = FolderSearch::Ignore::defaultCaseFold());
    // A folder at `rel` from the root. Reads the ignore files on the way
    // down, once each (kept).
    Seen seen(const std::string& rel);
    const std::string& root() const { return root_; }

private:
    struct Node {
        Seen seen = Seen::No;
        FolderSearch::Ignore::Rules rules;
    };
    std::string root_;
    bool fold_;
    std::set<std::string> open_, toward_;   // toward_: folders on the way to an open one
    std::map<std::string, Node> cache_;
    const Node& node(const std::string& rel);
};

class Coalescer {
public:
    explicit Coalescer(double quiet = 0.15, double most = 1.0, std::size_t cap = 4096)
        : quiet_(quiet), most_(most), cap_(cap) {}
    // The roots a change past the cap is folded into (the watched ones).
    void setRoots(std::vector<std::string> roots) { roots_ = std::move(roots); }
    // `now` in seconds, on any clock that only goes forward.
    void add(const Change& c, double now);
    // Seconds until the batch is due (0: now), -1 when nothing waits.
    double wait(double now) const;
    bool due(double now) const { return wait(now) == 0; }
    // The batch (taken whether due or not): each folder once, sorted, none
    // under a folder changed `below`.
    std::vector<Change> take();
    std::size_t waiting() const { return paths_.size(); }

private:
    double quiet_, most_;
    std::size_t cap_;
    std::vector<std::string> roots_;
    std::map<std::string, bool> paths_;   // folder -> below
    double first_ = -1, last_ = -1;
    void fold();   // past the cap: each root below
};

class Watcher {
public:
    // The most folders a watcher that watches one folder at a time (inotify)
    // keeps a watch on: the rest further down are left unwatched (watches
    // are counted per user, and other programs need theirs).
    static constexpr std::size_t kMaxFolders = 16384;
    // The system's watcher: null where there is none.
    static std::unique_ptr<Watcher> make();
    virtual ~Watcher() = default;
    // Readable when changes wait: poll(2) it, then take().
    virtual int fd() const = 0;
    // From now on these roots, each with its trees' open folders (replaces
    // what was watched; a root already watched is not walked again, only the
    // folders newly open or no longer open in it). Any thread.
    virtual void watch(const std::vector<Root>& roots) = 0;
    // What came since the last call, without waiting.
    virtual void take(std::vector<Change>* out) = 0;
    // For checks: the folders watched one at a time (inotify), or the
    // streams (FSEvents: one a root); whether the roots asked are being
    // watched yet (the last watch() applied).
    virtual std::size_t folders() const = 0;
    virtual bool settled() const = 0;
};

}  // namespace DiskWatch
