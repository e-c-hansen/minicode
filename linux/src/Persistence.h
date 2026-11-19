// Persistence.h — what a window keeps between runs and while it is away: the
// GTK port's counterpart of the Mac's EditorController+Persistence.mm
// (linux/PARITY.md, items 1.2 and 1.3, decision D11). One per window;
// main.cpp makes it with the window and calls it from a few hooks.
//
// Sessions. A window opening on a folder gets that folder's tabs back: their
// files in order, the one on show, the preview tab (as one), and each tab's
// selection and view. They are kept in $XDG_STATE_HOME/mutible/sessions.json
// (~/.local/state/mutible), keyed by the folder's resolved path, for the 50
// folders used most recently (TabSessions.h). Only the tab on show is read
// then; the others are read when first shown, and one holding no plain file,
// or one over 16 MB, is never the one put on show at launch. Files gone are
// left out. Windows open on one folder share its session: each save lists
// the saving window's tabs, then the others' not already there, and a window
// that closes while another stays on the folder leaves the saving to that
// one. Saved a second after the tabs change, as the window closes, before
// Open Folder takes it elsewhere, and for every window before a quit.
//
// Backups and hot exit. Every tab with unsaved changes has its text on disk
// while it is edited (BackupStore.h): written a second after typing stops
// and at most five seconds after the first change not yet written (the
// core's SavePlan::Planner decides when, from the monotonic clock, and one
// timer here ticks it), off the main thread, and removed once the tab is clean again or goes (closed after
// the question, or with its window). A crash, a SIGKILL or a power loss
// leaves them, and the folder's next window brings them back as unsaved
// tabs: the backup is the buffer, the disk's text the saved text, there is
// nothing to undo, and the status bar says so when the tab is first shown,
// and whether the file has gone or changed since (then the tab asks Keep
// Mine or Reload when shown, as for any change on disk). At launch a window
// opens for every folder with backups no running window holds. Backups are
// per window: each names its window's key, a launch bringing windows back
// leaves each window's backups to it, and a second backup of a file the
// window brings back (another window's unsaved text in it) comes back
// beside it as "a (unsaved copy).txt", never deleted unseen.
//
// Auto-save (editor.auto-save, Settings.h). after-delay writes every tab
// with unsaved changes once typing has paused for editor.auto-save-delay;
// focus-change writes them when the editor loses the keyboard, the window
// stops being active, or another tab is shown. A plain write: no
// keep-sorted, and never a question. A file gone from disk is left alone,
// one changed on disk since it was read is skipped and named in the status
// bar, and text an input method is still composing is not written. The
// close and quit questions run it first, so only what it skips is asked
// about.
#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Backups.h"
#include "SavePlan.h"
#include "Settings.h"
#include "TabSessions.h"

class Document;
class Editor;

class Persistence {
public:
    struct Host {
        // A message in the status bar for a few seconds.
        std::function<void(const std::string&)> note;
        // The other windows open on the same folder (key()), not this one.
        std::function<std::vector<Persistence*>()> sameFolder;
        // A tab whose Ctrl+S waits for its formatter (Formatting.h): auto-save
        // leaves it, as the Mac's does.
        std::function<bool(const Document*)> saveWaits;
        // The browser pane's tabs and the one on show, for the session.
        std::function<std::vector<TabSessions::BrowserTab>(int* active)> browserTabs;
        // The window's size and panels, for its record (windowRecord).
        std::function<void(TabSessions::Window*)> windowState;
        // The tabs were saved: the windows' records are written again.
        std::function<void()> windowsChanged;
        // The tree's focused folders (Focus on Folder and Add to Focus,
        // FileTree.h), kept in the session as paths from the folder in the
        // order added; and a saved list put back, first thing when a
        // session is restored (the tree keeps those that still name a
        // folder).
        std::function<std::vector<std::string>()> treeFocus;
        std::function<void(const std::vector<std::string>&)> restoreTreeFocus;
    };

    Persistence(Editor* editor, const std::string& root, Host host);
    ~Persistence();
    Persistence(const Persistence&) = delete;
    Persistence& operator=(const Persistence&) = delete;

    // A folder's key: its path with symlinks resolved, as the Mac's.
    static std::string keyFor(const std::string& root);
    // $XDG_STATE_HOME/mutible/sessions.json.
    static std::string sessionsFile();
    // `s` as the session of the folder keyed `key` in that file (none when
    // it has no tabs), the other folders' entries kept: read and written
    // back under a lock, since every mutible process saves into the one
    // file.
    static void putSession(const std::string& key, const TabSessions::Session& s);
    // The windows a launch that opens nothing brings back, front first, kept
    // in the same file under "windows" (TabSessions::Store), as the Mac keeps
    // them in its user defaults: read, and written over under the lock.
    static std::vector<TabSessions::Window> savedWindows();
    static void putWindows(const std::vector<TabSessions::Window>& windows);
    const std::string& key() const { return key_; }
    // Open Folder: the old folder's session saved first, then this, then
    // restoreSession for the new one.
    void setRoot(const std::string& root);
    // What the folder's session holds now on disk (the browser's tabs are
    // taken from it at launch).
    TabSessions::Session savedSession() const;

    // ---- hooks
    // Editor::onDocumentsChanged: an edit (`d`) or a change to the tabs.
    void changed(Document* d);
    // Editor::onActiveChanged: another tab is on show.
    void tabShown();
    // The window's keyboard moved to `focus`, or the window stopped (or
    // started) being active.
    void focusMoved(GtkWidget* focus);
    void windowActive(bool active);

    // ---- sessions
    // This window's own tabs as they are now.
    TabSessions::Session session();
    // Written with the other windows' on the folder after this one's.
    void saveSession();
    // The session save planned for a second after the tabs changed, written
    // now if there is one (another window is about to read the folder's).
    void flushSession();
    // The window is going: another window on the folder saves instead, or
    // this one does; nothing is written for it after this.
    void saveSessionClosing();
    // This window's record: folder, own tabs, size and panels, and its
    // backup key.
    TabSessions::Window windowRecord();
    // The window's name in the backups (Backups::Entry::window), kept in its
    // record (TabSessions::Window::backupKey): made when first asked for,
    // and a window brought back from a record takes its record's before it
    // restores, so it gets its own unsaved buffers back and leaves another
    // window's on the same folder to that window (round 10's workspace item
    // 1, the Mac's backupKey).
    const std::string& backupKey();
    void setBackupKey(const std::string& key);
    // The folder's tabs back in this window (or `own`, a window's own tabs
    // from its record), with any backups of its files no running process
    // holds.
    void restoreSession(const TabSessions::Session* own = nullptr);

    // ---- backups
    void scheduleBackups();
    // Every buffer that needs one written now.
    void writeBackups();

    // ---- auto-save
    void scheduleAutoSave();
    void autoSaveOnFocusChange();
    // Every tab that takes an auto-save, written now, unasked.
    void autoSaveNow();

private:
    bool takesBackup(Document* d) const;
    bool needsBackup(Document* d) const;
    void writeBackup(Document* d);
    void discardBackup(Document* d);
    // The planner's view of the tabs, and what it says to do.
    SavePlan::Tab planTab(Document* d) const;
    std::vector<SavePlan::Tab> planTabs() const;
    Document* documentWithId(uint64_t id) const;
    void apply(const SavePlan::Plan& plan);
    // The one timer, set for the planner's next due time, and its firing.
    void arm();
    void tick();
    // A backup into a tab made for it; true when its file changed on disk
    // since the backup was taken (it is then asked about when shown).
    bool takeBackup(Document* d, const std::string& id, const Backups::Entry& e,
                    const std::string& text);
    // Backups of files this window brought back from another backup (or
    // holds from the session's host): each a tab of its own beside the file,
    // "a (unsaved copy).txt" (Backups::draftPath), or removed when its text
    // is the file's tab's. A note for the status bar when any came back.
    std::string restoreConflicts(const std::vector<Backups::Restore::Item>& conflicts);
    Settings::AutoSave autoSaveMode() const;
    bool takesAutoSave(Document* d) const;

    Editor* editor_;
    std::string key_;
    std::string backupKey_;
    Host host_;
    bool closing_ = false;
    // When backups, auto-saves and the session are written (SavePlan.h), on
    // the monotonic clock, and the one timer that ticks it.
    SavePlan::Planner plan_;
    guint planTimer_ = 0;
    bool editorHadFocus_ = false;
};
