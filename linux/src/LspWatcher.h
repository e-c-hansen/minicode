// LspWatcher.h — files changed on disk, for the language servers'
// workspace/didChangeWatchedFiles (PARITY.md item 2.2), the GTK port's
// counterpart of the Mac's src/LspWatcher.{h,mm}. One per LspSession, made
// once a server registers a watcher (LspSessionWatch.cpp).
//
// inotify, which GFileMonitor uses on Linux, watches one folder at a time,
// where the Mac's FSEvents stream takes a whole tree. So the watcher keeps a
// GFileMonitor for each folder below the ones it is given, every folder
// Lsp::watchDescends allows (never .git or a node_modules package's own
// folders), at most kMaxFolders of them, and adds and drops monitors as
// folders come and go; a folder that appears is read, and what is in it is
// reported as created (an unpacked archive, a branch switch). All of that
// runs on a thread of its own, with its own main context, as the Mac's
// stream runs on a queue: every monitor is made there, its events arrive
// there, and after 0.3 s of quiet a batch is looked at (lstat, so a rename
// reads as a deletion and a creation, and a path already reported as there
// as a change: Lsp::FileEventClassifier), coalesced
// (Lsp::coalesceFileChanges), matched against a copy of each server's
// watchers (setTargets, Lsp::prepareWatched), and handed to the main thread
// with each server's notification built, so the main thread only writes it.
// Paths are spelled as given (resolved: the session watches realpaths, and
// the targets' aliases give a server's own spelling).
#pragma once

#include <gio/gio.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "LspClient.h"

namespace lspgtk {

// On the main thread: the batch, and one WatchedBatch per target the
// watcher held when it matched (none before the first setTargets).
using WatchHandler = std::function<void(const std::vector<Lsp::FileChange>& changes,
                                        const std::vector<Lsp::WatchedBatch>& batches)>;

class FileWatcher {
public:
    // The most folders one watcher keeps a monitor on; past it, folders
    // further down are left unwatched (inotify's watches are counted per
    // user, and other programs need theirs).
    static constexpr std::size_t kMaxFolders = 16384;

    // Watches `paths` (folders, resolved; ones that are not folders are
    // left out) and the folders below them until destroyed; the handler
    // runs on the main thread. Null when none of the folders exists.
    static std::unique_ptr<FileWatcher> start(const std::vector<std::string>& paths,
                                              WatchHandler handler);
    // Stops the thread and waits for it; a batch already on its way to the
    // main thread is dropped there.
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    const std::vector<std::string>& paths() const { return paths_; }
    // The servers' watchers batches are matched against from now on
    // (copied; any thread).
    void setTargets(const std::vector<Lsp::WatchTarget>& targets);
    // Batches handed over so far, and folders with a monitor now (checks).
    std::size_t batches() const;
    std::size_t folders() const;
    // For checks: `changes` go through what a batch from the disk goes
    // through after its paths were looked at (coalescing, matching, the
    // main thread), on the watcher's thread.
    void inject(const std::vector<Lsp::FileChange>& changes);

    struct Shared;   // what the thread and the main thread share

private:
    FileWatcher() = default;
    std::vector<std::string> paths_;
    std::shared_ptr<Shared> shared_;
};

}  // namespace lspgtk
