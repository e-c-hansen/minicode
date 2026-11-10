// GitDirWatch.h — a repository's git folder watched for a change to its
// index or HEAD, linux/PARITY.md item 2.10: the counterpart of the Mac's
// MCGitDirWatch in src/GitGutter.mm, shared here by the change bars
// (GitGutter.h) and the tree's git colors (TreeGitColors.h).
//
// git writes the index and HEAD by renaming a lock file over them, which a
// GFileMonitor on the folder reports; only then are the two files' stats
// compared, so FETCH_HEAD, COMMIT_EDITMSG or a lock file alone calls
// nothing. So a stage, a commit, a reset or a checkout made anywhere (a
// terminal, Claude Code, the Source Control panel hidden) is seen at once,
// with no key pressed. The folder is the one `git rev-parse
// --absolute-git-dir` names: a linked worktree's own folder for one, which
// holds its index and HEAD.
#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>

// What a file's stat says, enough to tell a rewrite: its time to the
// nanosecond, size and inode. sec is -1 for a file that is not there.
struct GitStamp {
    long long sec = -1, nsec = 0, size = 0, ino = 0;
    bool operator==(const GitStamp& o) const {
        return sec == o.sec && nsec == o.nsec && size == o.size && ino == o.ino;
    }
    bool operator!=(const GitStamp& o) const { return !(*this == o); }
};
GitStamp gitStampOf(const std::string& path);

class GitDirWatch {
public:
    // Watches `gitDir`; `indexStamp` is the index as the caller last read
    // it, so a change made since is caught at the first event. `onChange`
    // runs on the main loop, with `gone` when the folder itself went or
    // moved (the repository deleted): the owner then drops the watch.
    GitDirWatch(const std::string& gitDir, GitStamp indexStamp,
                std::function<void(bool gone)> onChange);
    ~GitDirWatch();
    GitDirWatch(const GitDirWatch&) = delete;
    GitDirWatch& operator=(const GitDirWatch&) = delete;

    const std::string& dir() const { return dir_; }
    bool watching() const { return monitor_ != nullptr; }

private:
    static void onEvent(GFileMonitor* m, GFile* file, GFile* other, GFileMonitorEvent ev,
                        gpointer self);

    std::string dir_;
    GFile* dirFile_ = nullptr;
    GFileMonitor* monitor_ = nullptr;
    GitStamp index_, head_;
    std::function<void(bool)> onChange_;
};
