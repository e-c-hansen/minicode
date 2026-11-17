// SavePlan.h — when a window's unsaved tabs are written: each dirty tab's
// backup (Backups.h) and auto-save (editor.auto-save, Settings.h), decided
// from a clock the port ticks. Pure C++17, no I/O and no timers: the port
// tells the planner what changed and when, asks nextDue() when to tick, and
// tick(now) says what to write then. Stage 3b of SESSION-DESIGN.md: both
// ports run on it (linux/src/Persistence.cpp, the Mac's
// EditorController+Persistence.mm since 3c), and the rules here are the
// Mac's, which tests/save_plan_tests.inc holds them to:
//
// - Backups. After every edit and every change of a dirty flag or the tabs,
//   a tab that is clean again (or holds nothing to keep) has its backup
//   removed at once. A changed dirty buffer is written 1 s after typing
//   stops, and at most 5 s after the first change not yet written, so a
//   long burst of typing is still on disk every 5 s. One write covers every
//   tab that needs one, not only the one edited.
// - Auto-save after-delay: every edit restarts the delay
//   (editor.auto-save-delay, 1000 ms); there is no longest wait, as in VS
//   Code's files.autoSave afterDelay.
// - Auto-save focus-change: the editor losing the keyboard, the window
//   losing focus or another tab showing writes on the next turn; nothing an
//   edit does cancels that write.
// - A tick with auto-save turned off since writes nothing for it.
// - The session (which files the window has open, TabSessions.h), for a
//   port that writes it once its tabs settle: 1 s after the first change
//   not yet written. A window opening on a folder another window has open
//   takes that window's pending save first (takeSession), so it reads the
//   folder's session as it is, not as it was a second ago (GTK's
//   split_editor flake, #81). The Mac writes its session at once.
//
// Times are seconds on any clock that only goes forward (the port's
// monotonic clock); tabs are named by the port's own ids.
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "Settings.h"

namespace SavePlan {

constexpr double kBackupDelay = 1.0;     // after typing stops
constexpr double kBackupMaxWait = 5.0;   // after the first change not yet written
constexpr double kSessionDelay = 1.0;    // after the first change to the tabs not yet written
constexpr double kNever = std::numeric_limits<double>::infinity();

// A tab as the port sees it now.
struct Tab {
    uint64_t id = 0;
    bool dirty = false;
    // Text read from a file, so it has something to keep (not a picture, a
    // diff, a message, or a tab with no file).
    bool keepsText = false;
    // What else the port requires of a tab before it auto-saves it (read,
    // not gone, no Ctrl+S waiting for its formatter, no inline-agent edit
    // waiting): dirty and keepsText are checked here.
    bool mayAutoSave = true;
    uint64_t generation = 0;   // counts every change to its text
    std::string path;
    // Its backup on disk, if any, and the generation and path it holds.
    bool backupWritten = false;
    uint64_t backupGeneration = 0;
    std::string backupPath;
};

// Dirty, with text to keep, and its backup missing or older than the text
// (or written under another path, before a rename).
bool needsBackup(const Tab &t);
// A tab an auto-save writes.
bool autoSaves(const Tab &t);

// What to do now.
struct Plan {
    std::vector<uint64_t> writeBackups;     // write these tabs' backups
    std::vector<uint64_t> discardBackups;   // remove these tabs' backups
    bool autoSave = false;                  // run the auto-save over every tab
    bool saveSession = false;               // write the window's session
    bool empty() const {
        return writeBackups.empty() && discardBackups.empty() && !autoSave && !saveSession;
    }
};

class Planner {
public:
    void setAutoSave(Settings::AutoSave mode, int delayMs);
    Settings::AutoSave autoSaveMode() const { return mode_; }

    // An edit, a dirty flag changed, or the tabs changed: both of the below.
    Plan changed(const std::vector<Tab> &tabs, double now);
    // Backups only: the discards due now, and the next write planned.
    Plan backupsChanged(const std::vector<Tab> &tabs, double now);
    // Auto-save only (also after its setting changed): after-delay planned
    // again from now, or dropped when no tab takes it.
    void autoSaveChanged(const std::vector<Tab> &tabs, double now);
    // The editor lost the keyboard, the window its focus, or another tab is
    // shown: in focus-change mode, an auto-save due now.
    void focusChanged(double now);
    // The tabs changed (opened, closed, moved, shown): the session written
    // kSessionDelay after the first such change not yet written; later
    // changes do not put it off.
    void sessionChanged(double now);
    // Whether a session save was planned, which is no longer planned: the
    // port writes the session now. Before a window on the same folder reads
    // the folder's session, and whenever the port writes it for another
    // reason (a window closing, quitting).
    bool takeSession();

    // What is due at `now`, given the tabs as they are now; what it returns
    // is no longer planned. The port writes each backup it names and records
    // it in the tab (backupWritten, backupGeneration, backupPath).
    Plan tick(const std::vector<Tab> &tabs, double now);
    // When tick next has something to do, kNever for nothing.
    double nextDue() const;
    // Nothing more is planned (the window is closing).
    void cancel();

    // For checks: when each part is due.
    double backupDue() const { return backupDue_; }
    double autoSaveDue() const { return autoSaveDue_; }
    double focusSaveDue() const { return focusDue_; }
    double sessionDue() const { return sessionDue_; }

private:
    Settings::AutoSave mode_ = Settings::AutoSave::Off;
    double delay_ = 1.0;
    double firstPending_ = kNever;   // the first change not yet written
    double backupDue_ = kNever;
    double autoSaveDue_ = kNever;
    double focusDue_ = kNever;
    double sessionDue_ = kNever;
};

}  // namespace SavePlan
