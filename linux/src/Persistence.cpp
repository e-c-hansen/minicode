// Persistence.cpp — see Persistence.h.
#include "Persistence.h"

#include "BackupStore.h"
#include "Document.h"
#include "Editor.h"

#include <glib/gstdio.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sys/stat.h>

namespace {

// A changed buffer is written a second after typing stops, and at least
// every five seconds while it goes on, as on the Mac.
constexpr double kBackupDelay = 1.0, kBackupMaxWait = 5.0;
// The largest file put on show at launch: the most the editor opens as text.
constexpr off_t kMaxShownBytes = 16 * 1024 * 1024;

std::string baseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

double wallNow() { return (double)g_get_real_time() / 1e6; }

bool existsNotFolder(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

// What can be read when the window opens: a plain file the editor opens.
bool quickToShow(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= kMaxShownBytes;
}

TabSessions::Sessions readSessions() {
    gchar* text = nullptr;
    gsize len = 0;
    TabSessions::Sessions all;
    if (g_file_get_contents(Persistence::sessionsFile().c_str(), &text, &len, nullptr))
        all = TabSessions::decode(std::string(text, len));
    g_free(text);
    return all;
}

void writeSessions(const TabSessions::Sessions& all) {
    const std::string path = Persistence::sessionsFile();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = TabSessions::encode(all);
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

}  // namespace

Persistence::Persistence(Editor* editor, const std::string& root, Host host)
    : editor_(editor), key_(keyFor(root)), host_(std::move(host)) {}

Persistence::~Persistence() {
    closing_ = true;
    for (guint* t : {&sessionTimer_, &backupTimer_, &autoSaveTimer_, &focusSaveIdle_})
        if (*t) {
            g_source_remove(*t);
            *t = 0;
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

void Persistence::setRoot(const std::string& root) { key_ = keyFor(root); }

// ---------------------------------------------------------------- hooks

void Persistence::changed(Document* d) {
    if (closing_) return;
    scheduleBackups();
    scheduleAutoSave();
    // The tabs changed: remembered a second later, once whatever changed
    // them has settled (an edit alone changes nothing remembered).
    if (!d && !sessionTimer_)
        sessionTimer_ = g_timeout_add(1000, [](gpointer p) -> gboolean {
            Persistence* self = static_cast<Persistence*>(p);
            self->sessionTimer_ = 0;
            self->saveSession();
            return G_SOURCE_REMOVE;
        }, this);
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
TabSessions::Session Persistence::session() {
    TabSessions::Session s;
    auto isFile = [](Document* d) {
        return d->kind() != Document::Kind::Diff && !d->path().empty();
    };
    Document* shown = editor_->active() && isFile(editor_->active()) ? editor_->active() : nullptr;
    if (!shown)
        for (Document* d : editor_->documents())
            if (isFile(d) && (!shown || d->lastUsed > shown->lastUsed)) shown = d;
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
    return s;
}

void Persistence::saveSession() {
    if (sessionTimer_) {
        g_source_remove(sessionTimer_);
        sessionTimer_ = 0;
    }
    if (key_.empty()) return;
    std::vector<TabSessions::Session> others;
    if (host_.sameFolder)
        for (Persistence* p : host_.sameFolder()) others.push_back(p->session());
    TabSessions::Sessions all = readSessions();
    TabSessions::put(all, key_, TabSessions::merged(session(), others), wallNow());
    writeSessions(all);
}

// A window that closes while another stays open on its folder drops out of
// that one's save, so what is remembered is what was open when the folder's
// last window went.
void Persistence::saveSessionClosing() {
    std::vector<Persistence*> others = host_.sameFolder ? host_.sameFolder() : std::vector<Persistence*>();
    if (!others.empty()) others.front()->saveSession();
    else saveSession();
    closing_ = true;
}

void Persistence::restoreSession() {
    const TabSessions::Sessions all = readSessions();
    TabSessions::Session saved;
    const auto found = all.find(key_);
    if (found != all.end()) saved = found->second;
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
        d->setPreviewTab(!previewFile.empty() && f == previewFile && !d->dirty());
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
    if (shown) editor_->activate(shown);
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

bool Persistence::needsBackup(Document* d) const {
    const Document::Backup& b = d->backup();
    return d->dirty() && takesBackup(d) &&
           (!b.written || b.writtenEdits != d->edits() || b.writtenPath != d->path());
}

// After every edit and every change to the tabs: a clean tab's backup goes
// now, and a changed buffer is written when typing pauses. A comparison of
// counts per tab, no text copied, so it can run on every key.
void Persistence::scheduleBackups() {
    if (closing_) return;
    bool pending = false;
    for (Document* d : editor_->documents()) {
        if (d->dirty() && takesBackup(d)) pending = pending || needsBackup(d);
        else if (d->backup().written) discardBackup(d);
    }
    if (backupTimer_) {
        g_source_remove(backupTimer_);
        backupTimer_ = 0;
    }
    if (!pending) {
        backupFirstPending_ = 0;
        return;
    }
    const gint64 now = g_get_monotonic_time();
    if (!backupFirstPending_) backupFirstPending_ = now;
    const double waited = (double)(now - backupFirstPending_) / 1e6;
    const double delay = std::max(0.0, std::min(kBackupDelay, kBackupMaxWait - waited));
    backupTimer_ = g_timeout_add((guint)(delay * 1000), [](gpointer p) -> gboolean {
        Persistence* self = static_cast<Persistence*>(p);
        self->backupTimer_ = 0;
        self->writeBackups();
        return G_SOURCE_REMOVE;
    }, this);
}

// Every changed buffer to the store's thread; the main thread only copies
// the text out of the document.
void Persistence::writeBackups() {
    backupFirstPending_ = 0;
    if (closing_) return;
    BackupStore& store = BackupStore::shared();
    for (Document* d : editor_->documents()) {
        if (!needsBackup(d)) continue;
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

bool Persistence::takesAutoSave(Document* d) const { return d->dirty() && takesBackup(d); }

// After every edit and every change to the tabs, beside scheduleBackups.
void Persistence::scheduleAutoSave() {
    if (autoSaveTimer_) {
        g_source_remove(autoSaveTimer_);
        autoSaveTimer_ = 0;
    }
    if (closing_ || autoSaveMode() != Settings::AutoSave::AfterDelay) return;
    bool any = false;
    for (Document* d : editor_->documents()) any = any || takesAutoSave(d);
    if (!any) return;
    const int ms = std::max(0, editor_->documentSettings().autoSaveDelay());
    autoSaveTimer_ = g_timeout_add((guint)ms, [](gpointer p) -> gboolean {
        Persistence* self = static_cast<Persistence*>(p);
        self->autoSaveTimer_ = 0;
        self->autoSaveNow();
        return G_SOURCE_REMOVE;
    }, this);
}

// Saved on the next turn of the main loop, not inside the focus change, and
// never cancelled by the delay's timer. Ahead of the input events waiting
// then: at the default idle priority every key already queued went first,
// so a key typed right after Ctrl+PageUp was written with the tab it went
// into, as if that tab had lost the keyboard too (auto_save, under load).
void Persistence::autoSaveOnFocusChange() {
    if (closing_ || autoSaveMode() != Settings::AutoSave::FocusChange || focusSaveIdle_) return;
    focusSaveIdle_ = g_idle_add_full(G_PRIORITY_HIGH, [](gpointer p) -> gboolean {
        Persistence* self = static_cast<Persistence*>(p);
        self->focusSaveIdle_ = 0;
        self->autoSaveNow();
        return G_SOURCE_REMOVE;
    }, this, nullptr);
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
