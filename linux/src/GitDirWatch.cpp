// GitDirWatch.cpp — see GitDirWatch.h.
#include "GitDirWatch.h"

#include <sys/stat.h>

GitStamp gitStampOf(const std::string& path) {
    GitStamp s;
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0) return s;
    s.sec = (long long)st.st_mtim.tv_sec;
    s.nsec = (long long)st.st_mtim.tv_nsec;
    s.size = (long long)st.st_size;
    s.ino = (long long)st.st_ino;
    return s;
}

GitDirWatch::GitDirWatch(const std::string& gitDir, GitStamp indexStamp,
                         std::function<void(bool gone)> onChange)
    : dir_(gitDir), index_(indexStamp), head_(gitStampOf(gitDir + "/HEAD")),
      onChange_(std::move(onChange)) {
    if (dir_.empty()) return;
    dirFile_ = g_file_new_for_path(dir_.c_str());
    monitor_ = g_file_monitor_directory(dirFile_, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    if (monitor_) g_signal_connect(monitor_, "changed", G_CALLBACK(onEvent), this);
}

GitDirWatch::~GitDirWatch() {
    if (monitor_) {
        g_signal_handlers_disconnect_by_data(monitor_, this);
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
    }
    if (dirFile_) g_object_unref(dirFile_);
}

void GitDirWatch::onEvent(GFileMonitor*, GFile* file, GFile*, GFileMonitorEvent ev,
                          gpointer selfp) {
    GitDirWatch* self = static_cast<GitDirWatch*>(selfp);
    // The folder itself went or moved: whatever is there now is the next
    // look's to find.
    const bool gone = (ev == G_FILE_MONITOR_EVENT_DELETED ||
                       ev == G_FILE_MONITOR_EVENT_MOVED_OUT ||
                       ev == G_FILE_MONITOR_EVENT_RENAMED ||
                       ev == G_FILE_MONITOR_EVENT_UNMOUNTED) &&
                      file && g_file_equal(file, self->dirFile_);
    const GitStamp i = gitStampOf(self->dir_ + "/index");
    const GitStamp h = gitStampOf(self->dir_ + "/HEAD");
    if (!gone && i == self->index_ && h == self->head_) return;   // FETCH_HEAD, a lock file
    self->index_ = i;
    self->head_ = h;
    // The callback may delete this watch.
    std::function<void(bool)> call = self->onChange_;
    if (call) call(gone);
}
