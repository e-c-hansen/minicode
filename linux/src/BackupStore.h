// BackupStore.h — the backups of unsaved tabs, one store for the process
// over the core's Backups (../src/Backups.h): the GTK port's counterpart of
// the Mac's MCBackupStore (src/BackupStore.mm). Persistence.h decides what
// is written and when; this writes it, on a thread of its own, and answers
// what a window opening a folder brings back.
//
// The folder is $MINICODE_BACKUP_DIR, else $XDG_DATA_HOME/minicode/Backups
// (~/.local/share/minicode/Backups), made 0700; each backup is one file,
// 0600, written to a temporary name, fsync'd and renamed, as the core does.
//
// A backup names the process holding its buffer (pid and start time): one
// whose owner still runs is live, and nobody else restores or removes it.
// Here the start time is the process's start in seconds since boot, from
// /proc/<pid>/stat, not wall-clock time as on the Mac, so a clock set
// forward or back never makes a running window's backups look orphaned.
#pragma once

#include "Backups.h"

#include <sys/types.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class BackupStore {
public:
    static BackupStore& shared();

    const std::string& directory() const { return store_.dir; }
    std::string newId() const;   // letters, digits and '-' (Backups::validId)

    // Written on the store's thread; the newest request for an id wins, so a
    // write still queued when a newer one comes is skipped. The entry's
    // owner and time are filled in here.
    void write(Backups::Entry e, std::string text);
    void remove(const std::string& id);
    // Until everything queued so far is on disk.
    void flush();

    // What a window opening `root` (resolved) brings back: Backups::plan over
    // the backups no running process holds, with the older backups of one
    // file removed. The ones it brings back are this process's from now on.
    Backups::Restore planForRoot(const std::string& root,
                                 const std::vector<std::string>& sessionFiles);
    bool load(const std::string& id, Backups::Entry* e, std::string* text);
    // Folders with backups no running window holds, newest first, leaving
    // out `open` (resolved): each gets a window at launch.
    std::vector<std::string> foldersToReopenExcept(const std::vector<std::string>& open);
    // Once a process: backups nobody held for 30 days, temporaries a crash
    // left over a day, and backups that do not read after 30 days.
    void cleanUpOnce();

    bool isLive(const Backups::Entry& e);
    // A process's start, in seconds since boot, or 0 when there is no such
    // process (or only a zombie of it).
    static double processStart(pid_t pid);

private:
    BackupStore();
    void post(std::function<void()> job);
    void run();
    uint64_t stamp(const std::string& id);
    bool isNewest(const std::string& id, uint64_t stamp);
    Backups::IsLive liveTest();

    Backups::Store store_;
    double myStart_ = 0;
    bool cleaned_ = false;

    std::mutex lock_;   // the queue, the stamps and the claimed ids
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::thread worker_;
    std::map<std::string, uint64_t> latest_;   // id -> the newest request
    uint64_t generation_ = 0;
    // Backups a window of this process brought back: held from then on,
    // even before the first write puts this process's name in them.
    std::set<std::string> claimed_;
};
