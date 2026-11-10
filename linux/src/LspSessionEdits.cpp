// LspSessionEdits.cpp — LspSession's workspace edit applier (the Mac's
// Lsp+WorkspaceEdit.mm), which rename, quick fixes, a server's own
// workspace/applyEdit, and later Format Document's server fallback share
// (PARITY.md item 2.1). Every file or none:
//   1. each file is read and checked: its tab's text (on show or not), else
//      the disk's; a file with unsaved changes in another window is
//      refused; and where the text is not what the server computed the
//      edit on (Client::textFor, or the disk's for a file it does not have
//      open), the edit goes in only if every range still reads the same
//      (Lsp::editsHold);
//   2. a file with no tab gets one (a kept tab, as VS Code opens them), and
//      each must then show as text to edit, holding that text;
//   3. each file's edits go into its own buffer as one multi-range
//      replacement (MultiEdit::apply, D5): one undo step per file, and the
//      highlighter, the gutter and the server follow each range as they
//      follow typing;
//   4. a failure after edits puts them back (each buffer's own undo), and
//      tabs the edit opened close again.
// The tab on show stays the one on show, and nothing is saved: the user
// reviews and saves. An edit that came with the versions of the server's
// documents when it was asked is refused for a file that moved on since
// (Lsp::staleEditFile). It keeps no state of its own.
#include "LspSessionPrivate.h"

#include "MultiEdit.h"

#include <algorithm>

using lspgtk::Server;

namespace {

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A tab whose text can be read and edited now: read, not still coming in.
bool textTab(Document* d) {
    return d && d->kind() == Document::Kind::Text && !d->unread() && !d->loading();
}

}  // namespace

// A file the server named, under the tree's own spelling of the path when it
// is inside the folder (the server answers with resolved paths), so a tab
// opened for it is the one the tree would open; "" when the URI is not a
// file.
std::string LspSession::treePathForUri(const std::string& uri) const {
    const std::string target = Lsp::pathFromUri(uri);
    if (target.empty()) return "";
    const std::string real = lspui::realPath(target), rootReal = lspui::realPath(root_);
    if (!rootReal.empty() && real.rfind(rootReal + "/", 0) == 0)
        return root_ + real.substr(rootReal.size());
    return target;
}

// The tab of a file in this window, by its path or, failing that, by the
// file it names (a link, /tmp and /private/tmp).
Document* LspSession::documentForPath(const std::string& path) const {
    if (Document* d = editor_->documentFor(path)) return d;
    const std::string real = lspui::realPath(path);
    for (Document* d : editor_->documents())
        if (d->kind() != Document::Kind::Diff && !d->path().empty() &&
            lspui::realPath(d->path()) == real)
            return d;
    return nullptr;
}

// The text the servers have for a file, or null when none has it open.
std::shared_ptr<const std::u16string> LspSession::serverTextOf(const std::string& uri) const {
    for (const auto& [key, s] : servers_) {
        if (!s->running() || s->stopping()) continue;
        if (auto t = s->client().textFor(uri)) return t;
    }
    return nullptr;
}

int LspSession::applyWorkspaceEdit(const Lsp::WorkspaceEdit& we, std::string* failure) {
    return applyEdit(we, nullptr, nullptr, failure);
}

int LspSession::applyEdit(const Lsp::WorkspaceEdit& we, Server* server,
                          const Lsp::DocVersions* asked, std::string* failure) {
    std::string unused;
    if (!failure) failure = &unused;
    failure->clear();
    if (!we.problem.empty()) {
        *failure = we.problem;
        return -1;
    }
    if (we.files.empty()) return 0;
    if (server) {
        const std::string stale = Lsp::staleEditFile(we, asked ? *asked : Lsp::DocVersions(),
                                                     server->client().versions());
        if (!stale.empty()) {
            *failure = baseName(Lsp::pathFromUri(stale)) +
                       " changed while the language server worked on it; try again";
            return -1;
        }
    }

    // 1. Read and check every file.
    struct File {
        std::string path;
        std::string text;   // UTF-8, as read
        std::u16string units;
    };
    std::vector<File> files;
    for (const Lsp::FileEdit& f : we.files) {
        File file;
        file.path = treePathForUri(f.uri);
        const std::string name = baseName(file.path);
        if (file.path.empty()) {
            *failure = "The edit names " + f.uri + ", which is not a file";
            return -1;
        }
        Document* d = documentForPath(file.path);
        std::string disk;
        bool diskRead = false;
        if (textTab(d)) {
            file.text = d->text();
        } else {
            if (Document::readText(file.path, file.text) != Document::Read::Ok) {
                *failure = "Could not read " + name;
                return -1;
            }
            disk = file.text;
            diskRead = true;
        }
        // Another window's tab with unsaved changes would lose this edit, or
        // lose them, when one of the two is saved.
        for (LspSession* other : lspAllSessions()) {
            if (other == this) continue;
            Document* o = other->documentForPath(file.path);
            if (o && o->dirty()) {
                *failure = name + " has unsaved changes in another window; save them there "
                                  "and try again";
                return -1;
            }
        }
        // The server's positions are in the text it has: a tab's as last
        // sent, or the disk's for a file no server has open.
        file.units = utf16::fromUtf8(file.text);
        std::u16string base;
        if (auto t = serverTextOf(f.uri)) {
            base = *t;
        } else {
            if (!diskRead && Document::readText(file.path, disk) != Document::Read::Ok) {
                *failure = "Could not read " + name;
                return -1;
            }
            base = utf16::fromUtf8(disk);
        }
        if (!Lsp::editsHold(file.units, base, f.edits)) {
            *failure = name + " has changes the language server has not seen; save it and try again";
            return -1;
        }
        files.push_back(std::move(file));
    }

    Document* home = editor_->active();
    std::vector<uint64_t> opened;   // tabs this edit made
    std::vector<size_t> applied;
    std::string why;
    bool ok = true;
    applyingEdit_ = true;

    // 2. A tab for every file, each holding the text read above.
    for (size_t i = 0; i < files.size() && ok; i++) {
        Document* d = documentForPath(files[i].path);
        if (!textTab(d)) {
            const bool hadTab = d != nullptr;
            editor_->openFile(files[i].path, EditorTabs::OpenFrom::Other);
            d = documentForPath(files[i].path);
            if (!hadTab && d) opened.push_back(d->id());
        }
        const std::string name = baseName(files[i].path);
        if (!textTab(d) || d->showsPreview()) {
            why = name + " is not open as text to edit";
            ok = false;
        } else if (d->text() != files[i].text) {
            why = name + " changed while the edit was being made; try again";
            ok = false;
        }
    }

    // 3. Each file's edits as one multi-range replacement: one undo step.
    for (size_t i = 0; i < files.size() && ok; i++) {
        Document* d = documentForPath(files[i].path);
        std::vector<MultiEdit::Change> changes;
        for (const Lsp::PointEdit& p : Lsp::pointEdits(files[i].units, we.files[i].edits))
            changes.push_back({p.start, p.length, p.text});
        if (!d || !MultiEdit::apply(d->buffer(), changes, true)) {
            why = baseName(files[i].path) + " could not be edited";
            ok = false;
            break;
        }
        applied.push_back(i);
    }

    // 4. A failure part way: the files already edited go back as they were.
    if (!ok) {
        for (auto k = applied.rbegin(); k != applied.rend(); ++k) {
            Document* d = documentForPath(files[*k].path);
            if (d && gtk_text_buffer_get_can_undo(d->buffer())) gtk_text_buffer_undo(d->buffer());
            if (!d || d->text() != files[*k].text) {
                g_printerr("MiniCode: could not put %s back after a failed edit\n",
                           files[*k].path.c_str());
                why += "; " + baseName(files[*k].path) +
                       " was changed and could not be put back, so check it before saving";
            }
        }
    }
    // Back to the tab the edit was asked from, and the tabs a refused edit
    // opened closed again (they hold nothing of it).
    if (home && editor_->indexOf(home) >= 0 && editor_->active() != home) editor_->activate(home);
    if (!ok)
        for (uint64_t id : opened)
            if (Document* d = editor_->documentById(id)) editor_->closeTab(d);
    applyingEdit_ = false;
    flushChanges();
    syncOpenDocuments();
    if (!ok) {
        *failure = why.empty() ? "The edit could not be made" : why;
        return -1;
    }
    return (int)applied.size();
}
