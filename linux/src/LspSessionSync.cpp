// LspSessionSync.cpp — LspSession's documents (the Mac's Lsp+Sync.mm): the
// file on show, attached to its server or not, and every tab's file kept
// open on its language's running server (PARITY.md item 2.0), with its own
// buffer's text. Each buffer's edits are followed as they are made
// (Lsp::ChangeTracker, in UTF-16 units) and sent a moment after typing
// pauses, as ranges where the server takes them; a file whose tab closes is
// closed on its server; a save sends didSave for the file written, on show
// or not.
#include "LspSessionPrivate.h"

#include <algorithm>
#include <set>

using lspgtk::Server;

namespace {

// A document a server may have: text that was read (not a tab still waiting
// to be, nor a file still coming in), not a large file with its features
// off, and with a file of its own.
bool serverMayHave(Document* d) {
    return d->kind() == Document::Kind::Text && !d->unread() && !d->loading() &&
           !d->path().empty() && d->languageServerMayRead();
}

}  // namespace

// ---- the file on show

void LspSession::detachDocument() {
    cancelCompletionTrigger();
    closeCompletion();
    closeSignature();
    dropPendingAccepts();
    closeHover();
    forgetHover();
    forgetActionRequests();
    clearMarks();
    ++docGen_;
}

// The file leaves the view. It stays open on its server while its tab is
// open (syncOpenDocuments closes it there once the tab goes), and so do its
// diagnostics, for when it is back on show.
void LspSession::closeDocument() {
    detachDocument();
    server_.reset();
    uri_.clear();
    path_.clear();
    currentKey_.clear();
    note_.clear();
}

void LspSession::documentChanged(const std::string& pathIn) {
    const std::string path = pathIn;   // a copy: closeDocument clears path_, which it may be
    closeDocument();   // its marks come off the buffer they are in
    useBuffer(editor_->sourceBuffer());
    path_ = path;
    if (!drawn_) {   // the window is not on screen yet: its server waits
        held_ = true;
        updateStatus();
        return;
    }
    Lsp::Language lang;
    std::shared_ptr<Server> server;
    if (settings_.lspEnabled() && languageOf(path, lang)) {
        currentKey_ = lang.server;   // first: the folder's tools ask whose file is on show
        server = serverForKey(lang.server);
    }
    if (server) {
        server_ = server;
        uri_ = lspui::uriOf(path);
    }
    // It, and every other tab a running server takes (a server just started
    // gets them all); a tab that closed or stopped showing text is closed.
    syncOpenDocuments();
    paintDiagnostics();
    updateStatus();
}

void LspSession::textEdited(const char* inserted, int len) {
    ++docGen_;
    forgetHover();
    actionsFollowCaret();
    // The session's own edits going in (an accept, the applier) are not typing.
    if (!server_ || uri_.empty() || applyingEdit_) return;
    signatureAfterEdit(inserted, len);
    completionAfterEdit(inserted, len);
}

void LspSession::tabsChanged() { scheduleSync(); }

// A file written from any tab, on show or not (Save All, auto-save, closing):
// the servers that have it open hear didSave with the text written, so
// save-triggered checks run. Its edits go first, as a didChange of their own.
void LspSession::documentSaved(Document* d) {
    if (!d) return;
    flushChanges();
    std::string text;
    bool read = false;
    for (auto& [key, e] : sync_->entries) {
        if (key.first != d->id() || !e.server->running() || e.server->stopping()) continue;
        if (!read) text = d->text(), read = true;
        e.server->client().didSave(e.uri, text);
    }
}

// ---- every tab's file

void LspSession::scheduleSync() {
    if (sync_->syncIdle) return;
    sync_->syncIdle = g_idle_add([](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->sync_->syncIdle = 0;
        self->syncOpenDocuments();
        return G_SOURCE_REMOVE;
    }, this);
}

// Every tab's file open on its language's server with its own text, so
// rename and references see unsaved changes in tabs not on show, and the
// server reads those, not the disk. Only a server already running takes a
// file: a tab never starts one unseen. Files whose tabs have gone, or whose
// path or server changed, are closed. Cheap when nothing changed: a file
// already open is left alone (its edits go as they are made).
void LspSession::syncOpenDocuments() {
    if (sync_->syncing) return;
    sync_->syncing = true;
    if (sync_->syncIdle) {
        g_source_remove(sync_->syncIdle);
        sync_->syncIdle = 0;
    }
    // Edits first, as ranges where the server takes them.
    flushChanges();
    struct Want {
        Document* doc;
        std::string uri, languageId;
        std::shared_ptr<Server> server;
    };
    std::map<std::pair<uint64_t, std::string>, Want> wanted;
    std::set<std::string> seen;   // "<key> <uri>": one tab per file and server
    if (settings_.lspEnabled())
        for (Document* d : editor_->documents()) {
            if (!serverMayHave(d)) continue;
            Lsp::Language lang;
            if (!languageOf(d->path(), lang)) continue;
            std::shared_ptr<Server> server = runningServer(lang.server);
            if (!server) continue;
            const std::string uri = lspui::uriOf(d->path());
            if (!seen.insert(lang.server + " " + uri).second) continue;
            wanted[{d->id(), lang.server}] = Want{d, uri, lang.languageId, server};
        }
    // Closed first, so a file that moved to another name or server is closed
    // on the old one before it opens again.
    auto& entries = sync_->entries;
    for (auto it = entries.begin(); it != entries.end();) {
        auto w = wanted.find(it->first);
        SyncState::Entry& e = it->second;
        if (w != wanted.end() && w->second.uri == e.uri && w->second.server == e.server) {
            e.path = w->second.doc->path();
            ++it;
            continue;
        }
        if (e.server->running() && !e.server->stopping()) e.server->client().didClose(e.uri);
        const std::string uri = e.uri;
        it = entries.erase(it);
        forgetDiagnostics(uri);
    }
    for (auto& [key, w] : wanted) {
        if (entries.count(key)) continue;
        SyncState::Entry e;
        e.docId = key.first;
        e.path = w.doc->path();
        e.uri = w.uri;
        e.server = w.server;
        e.buffer = w.doc->buffer();
        const std::string text = w.doc->text();
        e.astral = lspui::hasAstral(text.data(), text.size());
        w.server->client().didOpen(w.uri, w.languageId, text);
        sync_->opened++;
        entries.emplace(key, std::move(e));
    }
    watchBuffers();
    sync_->syncing = false;
}

// Each buffer a server has watched once, whichever servers have it, and
// let go when none has.
void LspSession::watchBuffers() {
    std::set<GtkTextBuffer*> buffers;
    for (auto& [key, e] : sync_->entries) buffers.insert(e.buffer);
    for (auto it = sync_->watched.begin(); it != sync_->watched.end();) {
        if (buffers.count(it->first)) {
            ++it;
            continue;
        }
        g_signal_handlers_disconnect_by_data(it->first, it->second.get());
        g_object_unref(it->first);
        it = sync_->watched.erase(it);
    }
    for (auto& [key, e] : sync_->entries) {
        if (sync_->watched.count(e.buffer)) continue;
        auto watch = std::make_unique<SyncState::Watch>(SyncState::Watch{this, e.docId});
        g_object_ref(e.buffer);
        g_signal_connect(e.buffer, "insert-text", G_CALLBACK(onSyncInsert), watch.get());
        g_signal_connect(e.buffer, "delete-range", G_CALLBACK(onSyncDelete), watch.get());
        sync_->watched.emplace(e.buffer, std::move(watch));
    }
}

// A server going (stopped, exited, refused): its documents are forgotten
// without a didClose, and so is what it published.
void LspSession::forgetServerDocuments(Server* s) {
    auto& entries = sync_->entries;
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->second.server.get() != s) {
            ++it;
            continue;
        }
        const std::string uri = it->second.uri;
        it = entries.erase(it);
        forgetDiagnostics(uri);
    }
    watchBuffers();
}

std::vector<std::string> LspSession::openDocuments() const {
    std::vector<std::string> out;
    for (const auto& [key, e] : sync_->entries) out.push_back(key.second + " " + e.path);
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t LspSession::rangedChangesSent() const {
    std::size_t n = 0;
    for (const auto& [key, s] : servers_) n += s->client().rangedChangesSent();
    return n;
}

std::size_t LspSession::wholeChangesSent() const {
    std::size_t n = 0;
    for (const auto& [key, s] : servers_) n += s->client().fullChangesSent();
    return n;
}

// ---- edits

// Before the buffer changes (both signals are RUN_LAST, and these run
// first), while the iterators still name the text as it was.
void LspSession::onSyncInsert(GtkTextBuffer*, GtkTextIter* at, char* text, int len,
                              gpointer watchp) {
    auto* w = static_cast<SyncState::Watch*>(watchp);
    w->self->noteEdit(w->docId, (size_t)gtk_text_iter_get_offset(at), 0, text, len);
}

void LspSession::onSyncDelete(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* z, gpointer watchp) {
    auto* w = static_cast<SyncState::Watch*>(watchp);
    int from = gtk_text_iter_get_offset(a), to = gtk_text_iter_get_offset(z);
    if (to < from) std::swap(from, to);
    if (to > from) w->self->noteEdit(w->docId, (size_t)from, (size_t)(to - from), nullptr, 0);
}

// The buffer counts characters and the server UTF-16 units: the same while
// no character outside the Basic Multilingual Plane is in the text. One
// that is makes the next send the whole text (ChangeTracker::invalidate).
void LspSession::noteEdit(uint64_t docId, size_t start, size_t removed, const char* inserted,
                          int len) {
    const bool astral = inserted && len > 0 && lspui::hasAstral(inserted, (size_t)len);
    const size_t units = inserted && len > 0 ? (size_t)g_utf8_strlen(inserted, len) : 0;
    bool any = false;
    for (auto& [key, e] : sync_->entries) {
        if (e.docId != docId) continue;
        any = true;
        e.pending = true;
        if (astral) e.astral = true;
        if (e.astral) e.changes.invalidate();
        else e.changes.edit(start, removed, units);
    }
    if (any) scheduleChange();
}

// didChange, a moment after typing pauses, for every file edited since: the
// edits as ranges for a server that takes them (and when they come to less
// than the text), else the whole text. Only the changed pieces are read
// (Client::didChange's TextReader). Anything that asks a server about the
// text flushes first, so with no edits this costs nothing.
void LspSession::flushChanges() {
    if (sync_->changeTimer) g_source_remove(sync_->changeTimer);
    sync_->changeTimer = 0;
    for (auto& [key, e] : sync_->entries) {
        if (!e.pending) continue;
        e.pending = false;
        if (!e.server->running() || e.server->stopping()) {
            e.changes.reset();
            continue;
        }
        Lsp::Client& c = e.server->client();
        if (e.astral) {
            const std::string text = lspui::bufferText(e.buffer);
            c.didChange(e.uri, text);
            e.astral = lspui::hasAstral(text.data(), text.size());
        } else {
            GtkTextBuffer* b = e.buffer;
            const size_t length = (size_t)gtk_text_buffer_get_char_count(b);
            c.didChange(e.uri, length, e.changes, [b, length](size_t start, size_t n) {
                start = std::min(start, length);
                n = std::min(n, length - start);
                GtkTextIter x, y;
                gtk_text_buffer_get_iter_at_offset(b, &x, (int)start);
                gtk_text_buffer_get_iter_at_offset(b, &y, (int)(start + n));
                char* s = gtk_text_buffer_get_slice(b, &x, &y, TRUE);
                std::u16string out = utf16::fromUtf8(s ? s : "");
                g_free(s);
                return out;
            });
        }
        e.changes.reset();
    }
}

void LspSession::scheduleChange() {
    if (sync_->changeTimer) g_source_remove(sync_->changeTimer);
    sync_->changeTimer = g_timeout_add(300, [](gpointer p) -> gboolean {
        LspSession* self = static_cast<LspSession*>(p);
        self->sync_->changeTimer = 0;
        self->flushChanges();
        return G_SOURCE_REMOVE;
    }, this);
}
