// Tabs.cpp — the editor's tabs: opening a file in its tab or a new one, the
// preview tab, switching, closing, reopening a closed tab, and the strip.
// The counterpart of the Mac's EditorController+Tabs.mm, by the same rules,
// which are the core's (../src/EditorTabs.h): the tabs themselves are its
// Model::Window, which names documents by id; the editor makes and deletes
// the documents and shows what the window says. Questions about unsaved
// changes are the shell's (main.cpp): nothing here asks, and nothing here
// drops unsaved changes without being told to.
#include "AgentNotes.h"
#include "SessionLink.h"
#include "Appearance.h"
#include "Editor.h"

#include "GitPanel.h"
#include "LargeFileView.h"
#include "Latex.h"
#include "FileSniff.h"
#include "MediaView.h"
#include "SqliteView.h"
#include "TabStrip.h"

#include <algorithm>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <cctype>
#include <sys/stat.h>

#include <memory>
#include <thread>

#include "CompareView.h"

namespace {

std::string baseName(const std::string& p) {
    const auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Is `path` the folder `dir` or somewhere inside it?
bool isInside(const std::string& path, const std::string& dir) {
    return !dir.empty() && !path.empty() &&
           (path == dir || path.compare(0, dir.size() + 1, dir + "/") == 0);
}

bool isRegularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

// ---------------------------------------------------------------- lookups

Model::Session& Editor::session() {
    static Model::Session s;
    return s;
}

Document* Editor::newDocument(uint64_t heldId) {
    Document* d = heldId ? new Document(this, &tags_, link_, heldId) : new Document(this, &tags_, link_);
    docs_[d->id()] = d;
    return d;
}

void Editor::setSessionLink(std::shared_ptr<SessionLink::Link> link) {
    if (link == link_) return;
    if (link_) link_->windowClosed(winId_);
    link_ = std::move(link);
    if (link_) link_->windowTabs(winId_, *win_);
}

std::vector<Document*> Editor::documentsOf(const std::vector<Model::DocId>& ids) const {
    std::vector<Document*> out;
    for (Model::DocId id : ids)
        if (Document* d = documentById(id)) out.push_back(d);
    return out;
}

std::string Editor::fileOf(const Document* d) {
    return !d || d->kind() == Document::Kind::Diff ? std::string() : d->path();
}

void Editor::syncPaths() const {
    for (const auto& [id, d] : docs_)
        if (win_->isOpen(id) && win_->path(id) != fileOf(d)) win_->setPath(id, fileOf(d));
}

void Editor::setPreview(Document* d, Model::Preview p) {
    if (!d) return;
    win_->setPreview(d->id(), p);
    refreshStrip();
}

Model::Preview Editor::documentPreview(const Document* d) const {
    return d && win_ ? win_->preview(d->id()) : Model::Preview::None;
}

uint64_t Editor::documentLastUsed(const Document* d) const {
    return d && win_ ? win_->lastUsed(d->id()) : 0;
}

Document* Editor::document(int index) const { return documentById(win_->at(index)); }

int Editor::indexOf(const Document* d) const { return d ? win_->indexOf(d->id()) : -1; }

int Editor::activeIndex() const { return indexOf(active_); }

Document* Editor::documentFor(const std::string& path) const {
    if (path.empty()) return nullptr;
    for (Document* d : documents())
        if (d->kind() != Document::Kind::Diff && d->path() == path) return d;
    return nullptr;
}

Document* Editor::documentById(uint64_t id) const {
    const auto it = docs_.find(id);
    return it == docs_.end() ? nullptr : it->second;
}

Document* Editor::previewTab() const {
    return documentById(win_->previewTab(Model::Preview::User));
}

std::vector<Document*> Editor::dirtyDocuments() const {
    std::vector<Document*> out;
    for (Document* d : documents())
        if (d->dirty()) out.push_back(d);
    return out;
}

// ---------------------------------------------------------------- opening

EditorTabs::PreviewSettings Editor::previewSettings() const {
    EditorTabs::PreviewSettings s;
    s.enabled = settings_.enablePreview();
    s.fromQuickOpen = settings_.enablePreviewFromQuickOpen();
    return s;
}

bool Editor::opensInPreview(EditorTabs::OpenFrom from) const {
    return EditorTabs::opensInPreview(from, previewSettings());
}

// The file as text, or a message saying why not. Markdown and LaTeX open in
// their previews, as on the Mac, unless the file is empty: a blank page
// looks like nothing opened.
bool Editor::loadText(Document* d, const std::string& path) {
    // A big file is read on a worker thread and put in a piece at a time
    // (LargeFileView.h); documentLoaded finishes the job.
    if (LargeFileView::readsInBackground(path)) {
        d->beginLoading(path);
        return true;
    }
    std::string content;
    Document::FileEncoding encoding;
    const Document::Read r = Document::readText(path, content, &encoding);
    return textRead(d, path, (int)r, content, encoding);
}

bool Editor::textRead(Document* d, const std::string& path, int result,
                      const std::string& content, const Document::FileEncoding& encoding) {
    const std::string name = baseName(path);
    auto refuse = [&](const std::string& why) {
        d->setMessage(path, "\n  Cannot display “" + name + "”.\n\n  (" + why + ")");
        return false;
    };
    switch ((Document::Read)result) {
    case Document::Read::Ok:
        break;
    case Document::Read::NotRegular:
        return refuse("Not a regular file.");
    case Document::Read::TooLarge:
        return refuse("Larger than 256 MB, too large to edit here.");
    case Document::Read::NotText:
        return refuse("Binary file or unsupported encoding.");
    case Document::Read::Missing:
    case Document::Read::Failed:
        d->setMessage(path, "\n  Could not open: " + path);
        return false;
    }
    d->loadText(path, content, encoding);
    const bool blank = std::all_of(content.begin(), content.end(),
                                   [](unsigned char c) { return std::isspace(c); });
    d->setShowsPreview((d->isMarkdown() || d->isLatex() || d->isNotebook()) && !blank);
    return true;
}

bool Editor::loadInto(Document* d, const std::string& path) {
    // A named pipe, socket or device is refused before anything opens it:
    // opening a pipe waits for a writer, and would hang every window.
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        d->setMessage(path, "\n  Cannot display “" + baseName(path) + "”.\n\n  (Not a regular file.)");
        return false;
    }
    // Images, PDFs, video and audio are routed by extension before any
    // attempt to read them as text. The editor's MediaView shows the tab;
    // one that will not decode falls through to the text path then.
    if (MediaView::handles(path)) {
        d->setMedia(path);
        return true;
    }
    // What the file is, from its first bytes and name (FileSniff, as the
    // Mac routes): a database has its own viewer, and a binary document has
    // none here, so neither is read as text. CSV, JSON and the rest are text, and the
    // document itself decides on the table.
    {
        std::string head;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            head.resize(FileSniff::kHeadBytes);
            head.resize(fread(&head[0], 1, head.size(), f));
            fclose(f);
        }
        const std::string name = baseName(path);
        switch (FileSniff::classify(path, head)) {
        case FileSniff::Kind::Sqlite:
            d->setDatabase(path);   // SqliteView shows it, read only
            return true;
        case FileSniff::Kind::QuickLook:
            d->setMessage(path, "\n  Cannot display “" + name + "”.\n\n  (A binary file or a "
                                "document another program opens; nothing here shows it.)");
            return false;
        default:
            break;
        }
    }
    return loadText(d, path);
}

bool Editor::openFile(const std::string& path, EditorTabs::OpenFrom from) {
    // The window's rules first (Model::Window::prepareOpen): in preview or
    // not; a code navigation keeps the preview tab it starts from (VS
    // Code's enablePreviewFromCodeNavigation, off); a kept open of the
    // preview tab's file keeps it, and an open in preview never makes a
    // kept tab a preview.
    syncPaths();
    const Model::Window::Open open = win_->prepareOpen(path, from, previewSettings());
    const bool inPreview = open.inPreview;
    if (Document* d = documentById(open.existing)) {
        // Only a message ("Could not open", "Cannot display") is tried again;
        // a tab not read yet is read as it is shown.
        if (d->kind() == Document::Kind::Message && !d->unread()) {
            loadInto(d, path);
            if (d == active_) {
                showActive();
                return d->kind() != Document::Kind::Message;
            }
        }
        activate(d);
        return d->kind() != Document::Kind::Message;
    }
    // A file over 50 MB asks first (LargeFileView.h); Open Anyway opens it then.
    if (!large_->mayOpen(path, from)) {
        refreshStrip();   // a preview tab prepareOpen kept shows it
        return false;
    }
    Document* d = newDocument();
    const bool ok = loadInto(d, path);
    // In preview, it replaces the old preview tab in VS Code's place, unless
    // that one has unsaved changes (it is kept instead); else it goes just
    // right of the tab on show. The replaced tab: no question (it has no
    // unsaved changes) and no place in Reopen Closed Tab, as VS Code leaves
    // a replaced preview editor out of its recently closed list.
    const Model::DocId old =
        win_->add(d->id(), fileOf(d), inPreview ? Model::Preview::User : Model::Preview::None,
                  [this](Model::DocId id) {
                      Document* o = documentById(id);
                      return o && o->dirty();
                  });
    activate(d);
    if (Document* o = documentById(old)) letGo(o);
    return ok;
}

Document* Editor::agentPreviewTab() const {
    return documentById(win_->previewTab(Model::Preview::Agent));
}

// Out of the window already (Model::Window::add replaced it): what showed it
// lets go, and it is deleted.
void Editor::letGo(Document* d) {
    if (!d || win_->isOpen(d->id())) return;
    if (d == active_) {
        // Not expected (the new tab is on show first), but never left shown.
        stash();
        active_ = documentById(win_->active());
        showActive();
    }
    syncMirror();
    if (d == diffDoc_) diffDoc_ = nullptr;
    destroy(d);
    refreshStrip();
}

Document* Editor::openForAgent(const std::string& path, bool preview, bool quiet) {
    if (Document* have = documentFor(path)) {
        if (!quiet) activate(have);   // shown where it is, a preview still if it was one
        return have;
    }
    if (!quiet && !large_->mayOpen(path, EditorTabs::OpenFrom::Other)) return nullptr;
    Document* d = newDocument();
    if (quiet) d->setUnread(path);
    else loadInto(d, path);   // a message says why; a note waits for text
    // In the agents' preview tab, in the last one's place, but never one
    // with unsaved changes (kept), nor the tab the user is typing in (left
    // alone); replaced, it is not asked about and not remembered.
    const Model::DocId old =
        win_->add(d->id(), fileOf(d), preview ? Model::Preview::Agent : Model::Preview::None,
                  [this](Model::DocId id) {
                      Document* o = documentById(id);
                      return o && o->dirty();
                  },
                  [this, quiet](Model::DocId id) { return quiet && documentById(id) == active_; });
    if (quiet) refreshStrip();
    else activate(d);
    if (Document* o = documentById(old)) letGo(o);
    return d;
}

Document* Editor::addTab(const std::string& path) {
    Document* d = newDocument();
    d->setUnread(path);
    win_->append(d->id(), path);
    refreshStrip();
    return d;
}

Document* Editor::attachTab(uint64_t heldId, const std::string& path, bool hostNamed) {
    if (!heldId || documentById(heldId)) return nullptr;
    Document* d = newDocument(heldId);
    d->attachHeld(path, hostNamed);
    win_->append(d->id(), path);
    refreshStrip();
    return d;
}

Document* Editor::addUntitled(const std::string& name) {
    Document* d = newDocument();
    d->attachHeld(name, false);
    win_->append(d->id(), name);
    refreshStrip();
    activate(d);
    return d;
}

void Editor::detachFromHost(const std::set<uint64_t>& drop) {
    if (!link_ || link_->lost()) return;
    // The record first, while the window still lists its tabs.
    link_->windowDetached(winId_, *win_);
    windowDetached_ = true;
    for (Document* d : documents())
        if (!drop.count(d->id()) && d->sessionLink() == link_.get()) d->detachFromHost();
}

void Editor::rehost(std::shared_ptr<SessionLink::Link> link) {
    if (!link) return;
    SessionLink::Link* old = link_.get();
    for (Document* d : documents())
        if (d->sessionLink() == old || (d->sessionLink() && d->sessionLink()->lost())) d->rehost(link);
    setSessionLink(std::move(link));
    notifyDocument();   // a new id: a new document to a language server
}

void Editor::showDiff(const std::string& title, const std::string& bytes, bool commit) {
    Document* d = diffDoc_;
    if (!d) {
        d = newDocument();
        win_->add(d->id(), "");
        diffDoc_ = d;
    }
    d->setDiff(title, bytes, commit);
    if (d == active_) {
        showActive();
        return;
    }
    activate(d);
}

namespace {
// The largest side read back, as `mutible diff` reads.
constexpr size_t kMaxSideBytes = 16u * 1024 * 1024;

// One side's text: the file on disk (rev ""), or the file as the repository
// has it at a revision or in the index (":"). A file not there (added since,
// deleted, not in that commit), not text or over 16 MB reads as empty, as
// the page showed it.
std::string sideText(const TabSessions::ComparedSide& s) {
    if (s.path.empty()) return "";
    if (s.rev.empty()) {
        struct stat st;
        if (stat(s.path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
            (size_t)st.st_size > kMaxSideBytes)
            return "";
        std::string text;
        if (FILE* f = fopen(s.path.c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() <= kMaxSideBytes)
                text.append(buf, n);
            fclose(f);
        }
        if (text.size() > kMaxSideBytes || !g_utf8_validate(text.data(), (gssize)text.size(), nullptr))
            return "";
        return text;
    }
    // The path with its links resolved the way the repository names files; a
    // file gone since is its folder's real path and its name.
    char* resolved = realpath(s.path.c_str(), nullptr);
    std::string real = resolved ? resolved : "";
    free(resolved);
    if (real.empty()) {
        const size_t slash = s.path.rfind('/');
        if (slash == std::string::npos) return "";
        char* dir = realpath(s.path.substr(0, slash).c_str(), nullptr);
        if (!dir) return "";
        real = std::string(dir) + s.path.substr(slash);
        free(dir);
    }
    const std::string dir = real.substr(0, real.rfind('/'));
    GitResult top = runGit(dir.empty() ? "/" : dir, {"rev-parse", "--show-toplevel"});
    if (!top.ok()) return "";
    std::string root = top.out;
    while (!root.empty() && (root.back() == '\n' || root.back() == '\r')) root.pop_back();
    if (root.empty() || real.compare(0, root.size() + 1, root + "/") != 0) return "";
    const std::string rel = real.substr(root.size() + 1);
    GitResult show = runGit(root, {"show", (s.rev == ":" ? "" : s.rev) + ":" + rel});
    if (!show.ok() || show.out.size() > kMaxSideBytes ||
        !g_utf8_validate(show.out.data(), (gssize)show.out.size(), nullptr))
        return "";
    return show.out;
}
}  // namespace

void Editor::showComparison(const CompareSpec& spec, std::function<void(Document*)> done) {
    openComparison(spec, false, -1, true, std::move(done));
}

// The recipe a comparison tab is kept by in the session (the Mac's
// pageTabOf:into:), or false when it has none.
bool Editor::comparisonPage(const Document* d, TabSessions::PageTab* out) {
    if (!d || !d->compare()) return false;
    const CompareSpec& sp = d->compare()->spec;
    if (sp.left.path.empty() || sp.right.path.empty() || !sp.decisionPrompt.empty() || sp.preview ||
        d->decision() || !TabSessions::isComparedRev(sp.left.rev) ||
        !TabSessions::isComparedRev(sp.right.rev))
        return false;
    TabSessions::PageTab p;
    p.kind = "compare";
    p.title = sp.title;
    p.fileName = sp.fileName.empty() ? sp.path : sp.fileName;
    p.left = sp.left;
    p.left.label = sp.leftLabel;
    p.right = sp.right;
    p.right.label = sp.rightLabel;
    *out = p;
    return true;
}

// A comparison made again from its recipe (the Mac's remakeComparisonOfTab:).
void Editor::remakeComparison(const TabSessions::PageTab& page, int at, bool show,
                              std::function<void(Document*)> done) {
    if (page.kind != "compare") return;
    CompareSpec spec;
    spec.title = page.title;
    spec.fileName = page.fileName;
    spec.leftLabel = page.left.label;
    spec.rightLabel = page.right.label;
    spec.left = page.left;
    spec.right = page.right;
    if (page.right.rev.empty()) spec.path = page.right.path;   // the file on the right, when it is one
    openComparison(spec, true, at < 0 ? 0 : at, show, std::move(done));
}

// The texts are lined up, colored and diffed on a thread (the Mac's
// MCComparison prepare); the tab opens on the main loop when that is done.
void Editor::openComparison(const CompareSpec& spec, bool remake, int at, bool show,
                            std::function<void(Document*)> done) {
    struct Job {
        Editor* editor;
        std::shared_ptr<bool> alive;
        std::shared_ptr<const CompareData> data;
        std::function<void(Document*)> done;
        int at;
        bool show;
    };
    Job* job = new Job{this, compareAlive_, nullptr, std::move(done), at, show};
    std::thread([job, spec = CompareSpec(spec), remake]() mutable {
        if (remake) {
            spec.leftText = sideText(spec.left);
            spec.rightText = sideText(spec.right);
        }
        job->data.reset(prepareComparison(spec));
        g_idle_add(
            [](gpointer p) -> gboolean {
                std::unique_ptr<Job> j(static_cast<Job*>(p));
                if (!*j->alive) return G_SOURCE_REMOVE;
                Editor* e = j->editor;
                Document* d = e->newDocument();
                const CompareSpec& sp = j->data->spec;
                // In the agents' preview tab for comparisons when asked, in
                // the last one's place; never the tab the user is typing in.
                // A tab made again goes where it was.
                Model::DocId old = 0;
                if (j->at >= 0) {
                    e->win_->insert(d->id(), "", j->at);
                } else {
                    old = e->win_->add(
                        d->id(), "", sp.preview ? Model::Preview::AgentDiff : Model::Preview::None,
                        [e](Model::DocId id) {
                            Document* o = e->documentById(id);
                            return o && o->dirty();
                        },
                        [e, quiet = sp.quiet](Model::DocId id) {
                            return quiet && e->documentById(id) == e->active_;
                        });
                }
                d->setCompare(sp.title, j->data);
                if (!sp.decisionPrompt.empty())
                    d->setDecision(std::make_shared<CompareDecision>(
                        j->data, sp.decisionPrompt, sp.onDecision, sp.lastChoiceGrace));
                if ((j->at >= 0 && !j->show) || sp.quiet) e->refreshStrip();
                else e->activate(d);
                if (Document* o = e->documentById(old)) e->letGo(o);
                if (j->done) j->done(d);
                return G_SOURCE_REMOVE;
            },
            job);
    }).detach();
}

// A page in a tab of its own (EditorPage.h), reused by its key: a second
// open shows the tab that has it and drops the new page. In preview (the
// Mac's showPage:preview:), the page replaces the preview tab as a file
// opened from the tree does, unless that tab has unsaved changes; a
// replaced page's tab is deleted, and the page with it (letGo).
Document* Editor::showPage(std::shared_ptr<EditorPage> page, bool preview) {
    for (Document* t : documentsOf(win_->tabs()))
        if (t->editorPage() && t->editorPage()->key() == page->key()) {
            if (!preview && t->isPreviewTab()) keepPreviewTab(t);
            activate(t);
            return t;
        }
    Document* d = newDocument();
    const bool inPreview = preview && previewSettings().enabled;
    const Model::DocId old =
        win_->add(d->id(), "", inPreview ? Model::Preview::User : Model::Preview::None,
                  [this](Model::DocId id) {
                      Document* o = documentById(id);
                      return o && o->dirty();
                  });
    d->setEditorPage(page->title(), page);
    activate(d);
    if (Document* o = documentById(old)) letGo(o);
    return d;
}

// A page of a saved session put back (Persistence::restoreSession): its
// own tab at `index` among the tabs, not on show, unless a tab has its key.
Document* Editor::addPageAt(std::shared_ptr<EditorPage> page, int index) {
    for (Document* t : documentsOf(win_->tabs()))
        if (t->editorPage() && t->editorPage()->key() == page->key()) return t;
    Document* d = newDocument();
    d->setEditorPage(page->title(), page);
    win_->insert(d->id(), "", index);
    return d;
}

EditorPage* Editor::currentPage() const { return active_ ? active_->editorPage().get() : nullptr; }

// ---------------------------------------------------------------- switching

// Put a tab on show. The tab that was on show keeps its text, unsaved
// changes, undo and place; the one shown is checked against the disk, and
// asked about if it changed there while it had unsaved changes.
void Editor::activate(Document* d) {
    // A file the other group has opens in this one too, as VS Code opens a
    // file in the group you are in: one document, a tab in each. The window
    // stamps it as used.
    if (!d || !win_->activate(d->id())) return;
    if (d == active_) {
        refreshStrip();
        return;
    }
    { if (notes_) notes_->activeWillChange(); if (onActiveWillChange) onActiveWillChange(); }
    stash();
    active_ = d;
    showActive();
    d->checkExternalChange();
    { if (onActiveChanged) onActiveChanged(); if (notes_) notes_->activeChanged(); }
}

void Editor::selectTab(int index) {
    Document* d = document(index);
    if (!d) return;
    activate(d);
    focusActiveDocument();
}

// Ctrl+PageDown and Ctrl+PageUp (Ctrl+Tab and Ctrl+Shift+Tab), wrapping.
void Editor::stepTab(int delta) {
    if (tabCount() == 0) {
        gtk_widget_error_bell(view_);
        return;
    }
    selectTab(EditorTabs::step(activeIndex(), tabCount(), delta));
}

// Alt+1 to Alt+8, and Alt+9 (or Alt+0) for the last.
void Editor::selectTabNumber(int n) {
    const int i = EditorTabs::indexForNumber(n, tabCount());
    if (i < 0) {
        gtk_widget_error_bell(view_);
        return;
    }
    selectTab(i);
}

// The keyboard to what the tab shows, as the Mac's focusActiveDocument.
void Editor::focusActiveDocument() {
    Document* d = active_;
    GtkWidget* target = view_;
#ifdef MUTIBLE_ENABLE_PDF
    if (d && d->isLatex() && d->showsPreview() && d->latexPreview())
        target = d->latexPreview()->widget();
    else
#endif
    if (d && d->kind() == Document::Kind::Media)
        target = media_->widget();
    else if (d && d->kind() == Document::Kind::Database)
        target = sqlite_->widget();
    else if (d && d->editorPage())
        target = d->editorPage()->focusWidget();
    else if (d && d->kind() == Document::Kind::Diff && d->compare() && compare_)
        target = compare_->focusWidget();
    else if (d && d->kind() == Document::Kind::Diff && diff_)
        target = diff_->textView();
    if (!gtk_widget_grab_focus(target)) gtk_widget_grab_focus(view_);
}

// ---------------------------------------------------------------- closing

// The focused group's tab of it, else the other group's.
static int gTrimsDone = 0;   // malloc_trim runs after big tabs closed (a check reads it)

void Editor::closeTab(Document* d) {
    if (indexOf(d) >= 0) dropTab(d, true);
    else if (inOther(d)) dropOther(d, true);
}

// Out of the row: remembered for Reopen Closed Tab when `remember` (a file's
// tab, not a diff's), and the tab shown next is the one used most recently.
void Editor::dropTab(Document* d, bool remember) {
    const int index = indexOf(d);
    if (index < 0) return;
    const bool wasActive = d == active_;
    if (wasActive) { if (notes_) notes_->activeWillChange(); if (onActiveWillChange) onActiveWillChange(); }
    if (wasActive) stash();
    // The window remembers a file's tab, unless the other group still has
    // it (not closed, so not one to reopen), and puts the tab used most
    // recently on show; the group closes with its last tab.
    EditorTabs::ClosedTab place;
    const bool keepPlace = remember && !fileOf(d).empty();
    if (keepPlace) place = closedTabOf(d);
    const Model::Window::Closed c = win_->close(d->id(), win_->focused(), keepPlace ? &place : nullptr);
    if (d == diffDoc_ && !win_->isOpen(d->id())) diffDoc_ = nullptr;
    if (wasActive && c.groupClosed) {
        // The group's last tab: the other has the editor.
        active_ = nullptr;
        closeFocusedGroup();
    } else if (wasActive) {
        // The next tab on show first, so the language server lets go of this
        // one's buffer before it goes.
        active_ = documentById(win_->active());
        showActive();
        if (active_) active_->checkExternalChange();
        { if (onActiveChanged) onActiveChanged(); if (notes_) notes_->activeChanged(); }
    }
    if (!isOpen(d)) destroy(d);
    refreshStrip();
}

int Editor::trimsDone() { return gTrimsDone; }

// Gone for good: what the slot held for it comes off first.
void Editor::destroy(Document* d) {
    docs_.erase(d->id());
    if (notes_) notes_->forget(d);
    if (d->decision()) d->decision()->tabClosed();   // closing answers it
    if (d == colorDoc_) {
        if (colorSaveTimer_) g_source_remove(colorSaveTimer_);
        colorSaveTimer_ = 0;
        colorDoc_ = nullptr;
    }
    if (d == mdEditDoc_) dropMdPopover();
#ifdef MUTIBLE_ENABLE_PDF
    if (LatexPreview* l = d->latexPreview())
        if (gtk_widget_get_parent(l->widget()) == slot_)
            gtk_stack_remove(GTK_STACK(slot_), l->widget());
#endif
    if (d->editorPage() && gtk_widget_get_parent(d->editorPage()->widget()) == slot_)
        gtk_stack_remove(GTK_STACK(slot_), d->editorPage()->widget());
    // A big text's pieces go back to glibc, which keeps freed memory for the
    // next allocation: about three times the file stayed resident after the
    // tab closed. Trimmed once the free is done, in an idle.
    const bool big = d->buffer() && gtk_text_buffer_get_char_count(d->buffer()) >= (3 << 20);
    delete d;
#ifdef __GLIBC__
    if (big) {
        g_idle_add_full(G_PRIORITY_LOW, [](gpointer) -> gboolean { malloc_trim(0); gTrimsDone++; return G_SOURCE_REMOVE; },
                        nullptr, nullptr);
    }
#endif
}

void Editor::closeAllTabs() {
    { if (notes_) notes_->activeWillChange(); if (onActiveWillChange) onActiveWillChange(); }
    stash();
    active_ = nullptr;
    diffDoc_ = nullptr;
    const std::vector<Document*> all = documents();
    win_->clear();
    otherActive_ = nullptr;
    dropMirrorMarks();
    refreshMirror();
    showActive();   // the empty editor, with no buffer of theirs on show
    for (Document* d : all) destroy(d);
    { if (onActiveChanged) onActiveChanged(); if (notes_) notes_->activeChanged(); }
}

void Editor::keepPreviewTab(Document* d) {
    if (d && win_->keep(d->id())) refreshStrip();
}

// ---------------------------------------------------------------- places

// What a tab keeps of where its view was, from its place: the selection, the
// first character on show (in the page, for a Markdown file showing it) and
// how far into its line the view started.
EditorTabs::ClosedTab Editor::closedTabOf(Document* d) const {
    EditorTabs::ClosedTab c;
    c.path = d->path();
    const Document::Place& p = d->place();
    if (d->unread() || (d->loading() && p.haveRestore)) {
        if (p.haveRestore) c = p.restore;
        c.path = d->path();
        return c;
    }
    c.preview = d->showsPreview();
    if (d->kind() != Document::Kind::Text) return c;
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(d->buffer(), &a, &b);
    c.selStart = gtk_text_iter_get_offset(&a);
    c.selEnd = gtk_text_iter_get_offset(&b);
    if (c.preview && d->isMarkdown() && p.havePage) {
        c.haveView = true;
        c.top = p.pageTop;
        c.topOffset = p.pageInto;
    } else if (p.haveView && p.top) {
        GtkTextIter top;
        gtk_text_buffer_get_iter_at_mark(d->buffer(), &top, p.top);
        c.haveView = true;
        c.top = gtk_text_iter_get_offset(&top);
        c.topOffset = p.topInto;
    }
    return c;
}

EditorTabs::ClosedTab Editor::placeOf(Document* d) {
    if (d == active_ && !d->unread()) capturePlace(d);
    return closedTabOf(d);
}

void Editor::placeTab(Document* d, const EditorTabs::ClosedTab& c) {
    // Not read yet, or still being read: given its place once it is.
    if (d->unread() || d->loading()) {
        d->place().haveRestore = true;
        d->place().restore = c;
        return;
    }
    if (d->isMarkdown() || d->isLatex() || d->isNotebook()) d->setShowsPreview(c.preview);
    if (d->kind() != Document::Kind::Text) return;
    const int chars = gtk_text_buffer_get_char_count(d->buffer());
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(d->buffer(), &a, (int)std::min<long>(c.selStart, chars));
    gtk_text_buffer_get_iter_at_offset(d->buffer(), &b, (int)std::min<long>(c.selEnd, chars));
    gtk_text_buffer_select_range(d->buffer(), &b, &a);
    Document::Place& p = d->place();
    if (c.haveView && d->isMarkdown() && d->showsPreview()) {
        p.havePage = true;
        p.pageTop = (int)c.top;
        p.pageInto = (int)c.topOffset;
    } else if (c.haveView) {
        GtkTextIter top;
        gtk_text_buffer_get_iter_at_offset(d->buffer(), &top, (int)std::min<long>(c.top, chars));
        if (!p.top) p.top = gtk_text_buffer_create_mark(d->buffer(), nullptr, &top, TRUE);
        else gtk_text_buffer_move_mark(d->buffer(), p.top, &top);
        p.topInto = (int)c.topOffset;
        p.haveView = true;
    }
}

// ---------------------------------------------------------------- reopening

bool Editor::reopenClosedTab() {
    EditorTabs::ClosedTab c;
    while (win_->takeClosed(&c, isRegularFile)) {
        if (Document* open = documentFor(c.path)) {
            activate(open);   // the other group's joins this one
            focusActiveDocument();
            return true;
        }
        Document* d = newDocument();
        loadInto(d, c.path);
        placeTab(d, c);
        win_->insert(d->id(), fileOf(d), EditorTabs::ClosedTabs::indexIn(c, tabCount()));
        activate(d);
        focusActiveDocument();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- moves and the Trash

void Editor::pathMoved(const std::string& from, const std::string& to) {
    for (Document* d : documents()) {
        if (d->kind() == Document::Kind::Diff || !isInside(d->path(), from)) continue;
        const std::string now = to + d->path().substr(from.size());
        const bool wasPreview = d->showsPreview();
        d->setPath(now);
#ifdef MUTIBLE_ENABLE_PDF
        if (d->latexPreview()) d->latexPreview()->setPath(now);   // the preview follows
#endif
        if (d != active_) continue;
        if (d->kind() == Document::Kind::Media) media_->setPath(now);   // watched under its new name
        if (d->kind() == Document::Kind::Database) sqlite_->setPath(now);
        if (wasPreview && !d->showsPreview()) {
            // No longer Markdown or LaTeX: the source back in the slot.
            if (buffer_ != d->buffer()) {
                clearPage(d);
                showSource(d);
            }
            gtk_stack_set_visible_child(GTK_STACK(slot_), scroller_);
        }
        notifyDocument();   // a new name is a new document to a language server
    }
    refreshStrip();
    titleChanged();
}

void Editor::pathTrashed(const std::string& path) {
    const std::vector<Document*> all = documents();
    for (Document* d : all)
        if (d->kind() != Document::Kind::Diff && !d->dirty() && isInside(d->path(), path)) {
            if (inOther(d)) dropOther(d, false);   // from both groups
            if (indexOf(d) >= 0) dropTab(d, false);
        }
}

// ---------------------------------------------------------------- the strip

// What each tab shows: the file's name and, for files of one name, the
// folders that tell them apart, drawn dimmed after it (EditorTabs::tabNames,
// as the Mac's reloadTabStrip), a diff's title, the dot for unsaved changes,
// and italics for the preview tab.
void Editor::refreshStrip() {
    syncPaths();
    const std::vector<Document*> tabs = documentsOf(win_->tabs());
    const std::vector<EditorTabs::TabName> names = win_->tabNames(win_->focused());
    std::vector<TabStrip::Item> items;
    for (size_t i = 0; i < tabs.size() && i < names.size(); i++) {
        const Document* d = tabs[i];
        TabStrip::Item it;
        if (d->kind() == Document::Kind::Diff) {
            it.title = d->diffTitle();
        } else {
            it.title = names[i].name;
            it.hint = names[i].hint;
        }
        if (it.title.empty()) it.title = baseName(d->path());
        it.tooltip = d->path().empty() ? it.title : d->path();
        it.dirty = d->dirty();
        it.preview = win_->isPreview(d->id());
        items.push_back(it);
    }
    strip_->setItems(items, activeIndex());
    gtk_widget_set_visible(strip_->widget(), !tabs.empty());
    refreshMirror();   // the other group's strip and view (Groups.cpp)
    // The session's host keeps the window's tabs as drawn (SessionLink.h).
    if (link_) link_->windowTabs(winId_, *win_);
    if (onDocumentsChanged) onDocumentsChanged(nullptr);
    if (observer_) observer_->tabsChanged();   // every tab's file on its server
}

// A right click on a tab: keeping a preview tab, closing it and its
// neighbours, and the file commands the port has for its file, as the Mac's
// menuForTabAtIndex:. Items name the strip's "tab." actions with the tab's
// id, and what cannot act is left out.
GMenuModel* Editor::menuForTab(int index) {
    Document* d = document(index);
    if (!d) return nullptr;
    GMenu* menu = g_menu_new();
    auto section = [&](std::initializer_list<std::pair<const char*, const char*>> items) {
        GMenu* s = g_menu_new();
        for (const auto& [label, action] : items) {
            GMenuItem* mi = g_menu_item_new(label, nullptr);
            g_menu_item_set_action_and_target_value(mi, (std::string("tab.") + action).c_str(),
                                                    g_variant_new_uint64(d->id()));
            g_menu_append_item(s, mi);
            g_object_unref(mi);
        }
        if (g_menu_model_get_n_items(G_MENU_MODEL(s)) > 0)
            g_menu_append_section(menu, nullptr, G_MENU_MODEL(s));
        g_object_unref(s);
    };
    if (d->isPreviewTab()) section({{"Keep Open", "keep"}});
    std::vector<std::pair<const char*, const char*>> closing = {{"Close", "close"}};
    if (tabCount() > 1) closing.push_back({"Close Other Tabs", "closeothers"});
    if (index < tabCount() - 1) closing.push_back({"Close Tabs to the Right", "closeright"});
    closing.push_back({"Close All Tabs", "closeall"});
    GMenu* s = g_menu_new();
    for (const auto& [label, action] : closing) {
        GMenuItem* mi = g_menu_item_new(label, nullptr);
        g_menu_item_set_action_and_target_value(mi, (std::string("tab.") + action).c_str(),
                                                g_variant_new_uint64(d->id()));
        g_menu_append_item(s, mi);
        g_object_unref(mi);
    }
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(s));
    g_object_unref(s);
    if (!d->path().empty()) {
        if (isRegularFile(d->path()))
            section({{"Copy Path", "copypath"}, {"Open Containing Folder", "reveal"},
                     {"Reveal in File Tree", "revealtree"}});
        else
            section({{"Copy Path", "copypath"}});
    }
    Appearance::addItem(menu, "tabs");   // PARITY.md 2.11; a window action
    return G_MENU_MODEL(menu);
}

// A tab menu's item: handed to the shell once the menu has closed, since
// closing a tab takes the menu's widget with it.
void Editor::tabMenuCommand(const std::string& name, GVariant* param) {
    struct Job {
        std::weak_ptr<int> alive;
        Editor* editor;
        std::string name;
        uint64_t id;
    };
    auto* job = new Job{alive_, this, name, g_variant_get_uint64(param)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        Job* j = static_cast<Job*>(p);
        if (j->alive.expired()) return G_SOURCE_REMOVE;
        Editor* self = j->editor;
        Document* d = self->documentById(j->id);
        if (!d) return G_SOURCE_REMOVE;
        if (self->onTabCommand) self->onTabCommand(j->name, d);
        else if (j->name == "keep") self->keepPreviewTab(d);
        else if (j->name == "close") self->closeTab(d);
        return G_SOURCE_REMOVE;
    }, job, [](gpointer p) { delete static_cast<Job*>(p); });
}
