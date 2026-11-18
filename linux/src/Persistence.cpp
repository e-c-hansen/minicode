// Persistence.cpp — see Persistence.h.
#include "Persistence.h"

#include "BackupStore.h"
#include "DocumentView.h"
#include "Editor.h"

#include <glib/gstdio.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <map>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// The largest file put on show at launch: the most the editor opens as text.
constexpr off_t kMaxShownBytes = 16 * 1024 * 1024;

std::string baseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

double wallNow() { return (double)g_get_real_time() / 1e6; }
// The planner's clock (SavePlan.h): seconds that only go forward.
double monoNow() { return (double)g_get_monotonic_time() / 1e6; }

bool existsNotFolder(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

// What can be read when the window opens: a plain file the editor opens.
bool quickToShow(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= kMaxShownBytes;
}

// The whole file (TabSessions::Store): every folder's session and the
// windows to bring back. One from before has folders only.
TabSessions::Store readStore() {
    gchar* text = nullptr;
    gsize len = 0;
    TabSessions::Store all;
    if (g_file_get_contents(Persistence::sessionsFile().c_str(), &text, &len, nullptr))
        all = TabSessions::decodeStore(std::string(text, len));
    g_free(text);
    return all;
}

TabSessions::Sessions readSessions() { return readStore().folders; }

void writeStore(const TabSessions::Store& all) {
    const std::string path = Persistence::sessionsFile();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = TabSessions::encodeStore(all);
    GError* err = nullptr;
    // A temporary renamed over the old file, so a crash midway leaves the
    // old sessions whole; the file names the user's files, so it is theirs
    // alone.
    if (!g_file_set_contents_full(path.c_str(), text.data(), (gssize)text.size(),
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        fprintf(stderr, "minicode: could not write %s: %s\n", path.c_str(),
                err ? err->message : "unknown error");
        g_clear_error(&err);
    }
}

// Each save reads the file, changes its part and writes the file back, and
// another minicode process may be saving at the same moment. The read and
// the write hold a lock on a file beside it, or one process's older copy
// could be written over an entry the other had just saved: the GUI checks'
// second app lost its tabs to the first app's save that way, now and then
// on CI (SessionsSuite.cpp). A lock that cannot be made (a read-only folder)
// leaves the save as it was, unlocked.
void changeStore(const std::function<void(TabSessions::Store&)>& change) {
    const std::string path = Persistence::sessionsFile();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const int lock = open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock >= 0)
        while (flock(lock, LOCK_EX) != 0 && errno == EINTR) {}
    TabSessions::Store all = readStore();
    change(all);
    writeStore(all);
    if (lock >= 0) close(lock);   // which lets go of the lock
}

}  // namespace

Persistence::Persistence(Editor* editor, const std::string& root, Host host)
    : editor_(editor), key_(keyFor(root)), host_(std::move(host)) {
    editor_->window().setRoot(key_);
}

Persistence::~Persistence() {
    closing_ = true;
    if (planTimer_) {
        g_source_remove(planTimer_);
        planTimer_ = 0;
    }
}

std::string Persistence::keyFor(const std::string& root) {
    char buf[PATH_MAX];
    if (!root.empty() && realpath(root.c_str(), buf)) return buf;
    return root;
}

std::string Persistence::sessionsFile() {
    gchar* p = g_build_filename(g_get_user_state_dir(), "minicode", "sessions.json", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

void Persistence::putSession(const std::string& key, const TabSessions::Session& s) {
    changeStore([&](TabSessions::Store& all) { TabSessions::put(all.folders, key, s, wallNow()); });
}

std::vector<TabSessions::Window> Persistence::savedWindows() { return readStore().windows; }

void Persistence::putWindows(const std::vector<TabSessions::Window>& windows) {
    changeStore([&](TabSessions::Store& all) { all.windows = windows; });
}

void Persistence::setRoot(const std::string& root) {
    key_ = keyFor(root);
    editor_->window().setRoot(key_);   // the window's folder, as the session keys it
}

// ---------------------------------------------------------------- hooks

void Persistence::changed(Document* d) {
    if (closing_) return;
    // The backups and auto-save planned again from now (SavePlan.h).
    plan_.setAutoSave(autoSaveMode(), editor_->documentSettings().autoSaveDelay());
    const double now = monoNow();
    // The tabs changed: remembered a second later, once whatever changed
    // them has settled (an edit alone changes nothing remembered).
    if (!d) plan_.sessionChanged(now);
    apply(plan_.changed(planTabs(), now));
    arm();
}

void Persistence::flushSession() {
    if (!closing_ && plan_.takeSession()) saveSession();
}

void Persistence::tabShown() {
    if (closing_) return;
    if (Document* d = editor_->active()) {
        Document::Backup& b = d->backup();
        if (!b.note.empty()) {
            if (host_.note) host_.note(b.note);
            b.note.clear();
        }
    }
    autoSaveOnFocusChange();   // another tab is shown
}

void Persistence::focusMoved(GtkWidget* focus) {
    const bool inEditor = focus && focus == editor_->textView();
    if (editorHadFocus_ && !inEditor) autoSaveOnFocusChange();   // the editor lost the keyboard
    editorHadFocus_ = inEditor;
}

void Persistence::windowActive(bool active) {
    if (!active) autoSaveOnFocusChange();   // another window, or another app
}

// ---------------------------------------------------------------- sessions

// The file on show, or with a diff on show the file shown last; a diff is
// not reopened.
TabSessions::Session Persistence::savedSession() const {
    const TabSessions::Sessions all = readSessions();
    const auto found = all.find(key_);
    return found != all.end() ? found->second : TabSessions::Session();
}

TabSessions::Session Persistence::session() {
    TabSessions::Session s;
    if (host_.browserTabs) s.browser = host_.browserTabs(&s.browserActive);
    if (host_.treeFocus) s.treeFocus = host_.treeFocus();
    auto isFile = [](Document* d) {
        return d->kind() != Document::Kind::Diff && !d->path().empty();
    };
    Document* shown = editor_->active() && isFile(editor_->active()) ? editor_->active() : nullptr;
    if (!shown)
        for (Document* d : editor_->documents())
            if (isFile(d) && (!shown || d->lastUsed() > shown->lastUsed())) shown = d;
    for (Document* d : editor_->documents()) {
        if (!isFile(d)) continue;
        if (d == shown) s.active = (int)s.tabs.size();
        if (d->isPreviewTab()) s.preview = (int)s.tabs.size();
        const EditorTabs::ClosedTab c = editor_->placeOf(d);
        TabSessions::Tab t;
        t.path = d->path();
        t.page = c.preview;
        t.selStart = c.selStart;
        t.selEnd = c.selEnd;
        t.haveView = c.haveView;
        t.top = c.top;
        t.into = (long)c.topOffset;
        s.tabs.push_back(t);
    }
    // The split editor's groups (PARITY.md 4.6).
    if (editor_->isSplit()) {
        s.split = true;
        s.groups = editor_->groupsSession();
    }
    return s;
}

void Persistence::saveSession() {
    plan_.takeSession();   // written now, not again a second later
    if (key_.empty()) return;
    std::vector<TabSessions::Session> others;
    if (host_.sameFolder)
        for (Persistence* p : host_.sameFolder()) others.push_back(p->session());
    putSession(key_, TabSessions::merged(session(), others));
    // And the windows open now, for a launch that opens nothing, so a crash
    // or a SIGKILL loses little of them.
    if (host_.windowsChanged) host_.windowsChanged();
}

// This window as a launch brings it back (TabSessions::Window): its folder
// and its own tabs here, its size and panels from the window.
TabSessions::Window Persistence::windowRecord() {
    TabSessions::Window w;
    w.root = key_;
    w.tabs = session();
    if (host_.windowState) host_.windowState(&w);
    return w;
}

// A window that closes while another stays open on its folder drops out of
// that one's save, so what is remembered is what was open when the folder's
// last window went.
void Persistence::saveSessionClosing() {
    std::vector<Persistence*> others = host_.sameFolder ? host_.sameFolder() : std::vector<Persistence*>();
    if (!others.empty()) others.front()->saveSession();
    else saveSession();
    closing_ = true;
    plan_.cancel();
}

void Persistence::restoreSession(const TabSessions::Session* own) {
    TabSessions::Session saved;
    if (own) {
        saved = *own;   // a window brought back has its own tabs
    } else {
        // Another window on the folder whose tabs changed within the second
        // writes them first, so this one reads the folder as it is
        // (SavePlan::Planner::takeSession).
        if (host_.sameFolder)
            for (Persistence* p : host_.sameFolder()) p->flushSession();
        const TabSessions::Sessions all = readSessions();
        const auto found = all.find(key_);
        if (found != all.end()) saved = found->second;
    }
    // The tree's focus first, as the Mac's restoreSession sets it: a focus
    // alone keeps a session, so it comes back with no tab too.
    if (host_.restoreTreeFocus) host_.restoreTreeFocus(saved.treeFocus);
    const EditorTabs::Session plain = TabSessions::files(saved);

    // Unsaved buffers kept from a crash, a SIGKILL or a power loss: a file of
    // the session with a backup comes back even if it is gone from disk, and
    // a backup of a file the session lost comes back after the session's.
    BackupStore& store = BackupStore::shared();
    store.cleanUpOnce();
    const Backups::Restore plan = store.planForRoot(key_, plain.files);
    std::map<std::string, std::string> backupFor;
    std::vector<std::string> extra;
    for (const Backups::Restore::Item& item : plan.items) {
        backupFor[item.path] = item.id;
        if (!item.inSession) extra.push_back(item.path);
    }
    EditorTabs::Session r = EditorTabs::restorable(plain, [&backupFor](const std::string& f) {
        return backupFor.count(f) > 0 || existsNotFolder(f);
    });
    const std::string previewFile = r.preview >= 0 ? r.files[(size_t)r.preview] : "";
    for (const std::string& f : extra) r.files.push_back(f);
    if (r.files.empty()) return;
    if (r.active < 0) r.active = 0;

    std::map<std::string, const TabSessions::Tab*> tabFor;
    for (const TabSessions::Tab& t : saved.tabs) tabFor.emplace(t.path, &t);
    std::vector<Document*> made(r.files.size(), nullptr);
    std::vector<Document*> changedSince;
    for (size_t i = 0; i < r.files.size(); i++) {
        const std::string& f = r.files[i];
        if (editor_->documentFor(f)) continue;   // open here already
        const auto b = backupFor.find(f);
        Backups::Entry e;
        std::string text;
        const bool haveBackup = b != backupFor.end() && store.load(b->second, &e, &text) &&
                                g_utf8_validate(text.data(), (gssize)text.size(), nullptr);
        // Nothing to show: the backup does not read and the file is gone.
        if (b != backupFor.end() && !haveBackup && !existsNotFolder(f)) continue;
        Document* d = editor_->addTab(f);
        if (haveBackup && takeBackup(d, b->second, e, text)) changedSince.push_back(d);
        const auto t = tabFor.find(f);
        if (t != tabFor.end()) editor_->placeTab(d, TabSessions::place(*t->second));
        // The preview tab comes back as one, unless unsaved changes came back
        // into it (an edit keeps a tab).
        if (!previewFile.empty() && f == previewFile && !d->dirty())
            editor_->setPreview(d, Model::Preview::User);
        made[i] = d;
    }
    // The tab on show is read now, while the window opens. One holding a
    // file too big for that, or no plain file at all, waits for a click,
    // and the nearest other tab is shown.
    Document* shown = nullptr;
    const int n = (int)made.size();
    for (int dist = 0; !shown && dist < n; dist++)
        for (int i : {r.active - dist, r.active + dist})
            if (!shown && i >= 0 && i < n && made[(size_t)i] && quickToShow(made[(size_t)i]->path()))
                shown = made[(size_t)i];
    // A split comes back over the tabs that did (EditorTabs::restoreGroups),
    // each group's tab on show read now; else the one tab on show.
    EditorTabs::GroupLayout groups;
    if (saved.split && !editor_->isSplit()) {
        std::vector<std::string> open;
        for (Document* d : editor_->documents()) open.push_back(d->path());
        groups = EditorTabs::restoreGroups(saved.groups, open);
    }
    if (groups.split) editor_->restoreGroups(groups);
    else if (shown) editor_->activate(shown);
    // Asked about once the window is up and the tab on show (the question
    // is the window's), not while it opens.
    for (Document* d : changedSince) d->askAboutDiskChange();
    scheduleBackups();
}

// ---------------------------------------------------------------- backups

// Only text read from a file has anything to keep.
bool Persistence::takesBackup(Document* d) const {
    return d->kind() == Document::Kind::Text && !d->path().empty();
}

bool Persistence::needsBackup(Document* d) const { return SavePlan::needsBackup(planTab(d)); }

// What the planner knows of a tab: its counts and what its backup holds.
SavePlan::Tab Persistence::planTab(Document* d) const {
    SavePlan::Tab t;
    t.id = d->id();
    t.dirty = d->dirty();
    t.keepsText = takesBackup(d);
    t.generation = d->edits();
    t.path = d->path();
    const Document::Backup& b = d->backup();
    t.backupWritten = b.written;
    t.backupGeneration = b.writtenEdits;
    t.backupPath = b.writtenPath;
    return t;
}

std::vector<SavePlan::Tab> Persistence::planTabs() const {
    std::vector<SavePlan::Tab> out;
    for (Document* d : editor_->documents()) out.push_back(planTab(d));
    return out;
}

Document* Persistence::documentWithId(uint64_t id) const {
    for (Document* d : editor_->documents())
        if (d->id() == id) return d;
    return nullptr;
}

// What the planner says to do now: backups removed, written, and the
// auto-save run.
void Persistence::apply(const SavePlan::Plan& plan) {
    for (uint64_t id : plan.discardBackups)
        if (Document* d = documentWithId(id)) discardBackup(d);
    for (uint64_t id : plan.writeBackups)
        if (Document* d = documentWithId(id)) writeBackup(d);
    if (plan.autoSave) autoSaveNow();
    if (plan.saveSession) saveSession();
}

// One timer for the planner, set for when it next has something to do. Due
// now (a focus-change save, a backup past its longest wait) runs on the next
// turn of the main loop, ahead of the input events waiting then: at the
// default idle priority every key already queued went first, so a key typed
// right after Ctrl+PageUp was written with the tab it went into, as if that
// tab had lost the keyboard too (auto_save, under load).
void Persistence::arm() {
    if (planTimer_) {
        g_source_remove(planTimer_);
        planTimer_ = 0;
    }
    if (closing_) return;
    const double due = plan_.nextDue();
    if (due == SavePlan::kNever) return;
    auto fire = [](gpointer p) -> gboolean {
        Persistence* self = static_cast<Persistence*>(p);
        self->planTimer_ = 0;
        self->tick();
        return G_SOURCE_REMOVE;
    };
    const double wait = due - monoNow();
    if (wait <= 0) {
        planTimer_ = g_idle_add_full(G_PRIORITY_HIGH, fire, this, nullptr);
    } else {
        // Rounded up, so the tick is never early; one that is finds nothing
        // due and is set again.
        const guint ms = (guint)std::min(std::ceil(wait * 1000.0), 3.6e6);
        planTimer_ = g_timeout_add(ms, fire, this);
    }
}

void Persistence::tick() {
    if (closing_) return;
    apply(plan_.tick(planTabs(), monoNow()));
    arm();
}

// After every edit and every change to the tabs: a clean tab's backup goes
// now, and a changed buffer is written when typing pauses (SavePlan.h). A
// comparison of counts per tab, no text copied, so it can run on every key.
void Persistence::scheduleBackups() {
    if (closing_) return;
    apply(plan_.backupsChanged(planTabs(), monoNow()));
    arm();
}

// Every changed buffer to the store's thread now.
void Persistence::writeBackups() {
    if (closing_) return;
    for (Document* d : editor_->documents())
        if (needsBackup(d)) writeBackup(d);
}

// One buffer to the store's thread; the main thread only copies the text
// out of the document.
void Persistence::writeBackup(Document* d) {
    if (closing_ || !needsBackup(d)) return;
    BackupStore& store = BackupStore::shared();
    Document::Backup& b = d->backup();
    if (b.id.empty()) b.id = store.newId();
    const Document::DiskStamp& disk = d->diskStamp();
    Backups::Entry e;
    e.id = b.id;
    e.path = d->path();
    e.root = key_;
    e.diskExisted = disk.exists;
    e.diskMtime = disk.mtime;
    e.diskInode = disk.inode;
    store.write(e, d->text());
    b.written = true;
    b.writtenEdits = d->edits();
    b.writtenPath = d->path();
}

void Persistence::discardBackup(Document* d) {
    Document::Backup& b = d->backup();
    if (b.written) BackupStore::shared().remove(b.id);
    b.written = false;
    b.writtenEdits = 0;
    b.writtenPath.clear();
}

// A tab come back from the session takes its backup: the text, unsaved, over
// the disk's text as it is now. Said in the status bar when it is first
// shown, with whether the file has gone or changed since.
bool Persistence::takeBackup(Document* d, const std::string& id, const Backups::Entry& e,
                             const std::string& text) {
    const std::string path = d->path();
    std::string disk;
    const bool read = Document::readText(path, disk) == Document::Read::Ok;
    const Backups::Disk was = Backups::compare(e, Backups::posixFs().stat(path));
    const bool gone = was == Backups::Disk::Gone;
    const bool changed = was == Backups::Disk::Changed || was == Backups::Disk::Appeared;
    Document::DiskStamp base;
    base.exists = e.diskExisted;
    base.mtime = e.diskMtime;
    base.inode = e.diskInode;
    // This process's from now on, written again soon under its name.
    Document::Backup& b = d->backup();
    b.id = id;
    b.written = true;
    b.writtenEdits = 0;
    b.writtenPath.clear();
    d->restoreText(path, text, read ? &disk : nullptr, base, changed);
    if (!d->dirty()) return false;
    b.note = "Restored unsaved changes to " + baseName(path);
    if (gone) b.note += "; the file is no longer on disk";
    else if (changed) b.note += "; the file has changed on disk since";
    return changed && read;
}

// ---------------------------------------------------------------- auto-save

Settings::AutoSave Persistence::autoSaveMode() const {
    return editor_->documentSettings().autoSave();
}

bool Persistence::takesAutoSave(Document* d) const { return SavePlan::autoSaves(planTab(d)); }

// After the setting changed, and with every edit and change to the tabs:
// after-delay planned again from now.
void Persistence::scheduleAutoSave() {
    if (closing_) return;
    plan_.setAutoSave(autoSaveMode(), editor_->documentSettings().autoSaveDelay());
    plan_.autoSaveChanged(planTabs(), monoNow());
    arm();
}

// Saved on the next turn of the main loop, not inside the focus change, and
// never cancelled by an edit's planning (SavePlan.h).
void Persistence::autoSaveOnFocusChange() {
    if (closing_) return;
    plan_.setAutoSave(autoSaveMode(), editor_->documentSettings().autoSaveDelay());
    plan_.focusChanged(monoNow());
    arm();
}

void Persistence::autoSaveNow() {
    if (closing_ || autoSaveMode() == Settings::AutoSave::Off) return;
    std::vector<std::string> changed;
    const std::vector<Document*> docs = editor_->documents();   // a save edits nothing, but safely
    for (Document* d : docs) {
        if (editor_->indexOf(d) < 0 || !takesAutoSave(d)) continue;
        // Text an input method is still composing is not written; the edit
        // that ends the composition schedules the next auto-save.
        if (d == editor_->active() && editor_->composing()) continue;
        // Its Ctrl+S waits for the formatter, and saves when that is done.
        if (host_.saveWaits && host_.saveWaits(d)) continue;
        // Moved or deleted: not written again unasked.
        if (d->goneFromDisk()) continue;
        // Changed (or made) on disk since it was read: never written over
        // unasked. The tab asks Keep Mine or Reload when it is shown, and is
        // left alone until the answer (Editor::askingAboutDisk).
        if (d->changedOnDisk() || editor_->askingAboutDisk(d)) {
            changed.push_back(baseName(d->path()));
            continue;
        }
        std::string err;
        if (!editor_->saveDocument(d, &err) && host_.note)
            host_.note("Could not auto-save " + baseName(d->path()) + ": " + err);
    }
    if (!changed.empty() && host_.note) {
        std::string names;
        for (const std::string& n : changed) names += (names.empty() ? "" : ", ") + n;
        host_.note("Not auto-saved: " + names + " changed on disk.");
    }
}
