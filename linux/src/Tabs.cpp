// Tabs.cpp — the editor's tabs: opening a file in its tab or a new one, the
// preview tab, switching, closing, reopening a closed tab, and the strip.
// The counterpart of the Mac's EditorController+Tabs.mm, by the same rules,
// which are the core's (../src/EditorTabs.h). Questions about unsaved
// changes are the shell's (main.cpp): nothing here asks, and nothing here
// drops unsaved changes without being told to.
#include "Editor.h"

#include "GitPanel.h"
#include "LargeFileView.h"
#include "Latex.h"
#include "MediaView.h"
#include "TabStrip.h"

#include <algorithm>
#include <cctype>
#include <sys/stat.h>

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

Document* Editor::document(int index) const {
    return index >= 0 && index < (int)tabs_.size() ? tabs_[(size_t)index] : nullptr;
}

int Editor::indexOf(const Document* d) const {
    for (size_t i = 0; i < tabs_.size(); i++)
        if (tabs_[i] == d) return (int)i;
    return -1;
}

int Editor::activeIndex() const { return indexOf(active_); }

Document* Editor::documentFor(const std::string& path) const {
    if (path.empty()) return nullptr;
    for (Document* d : tabs_)
        if (d->kind() != Document::Kind::Diff && d->path() == path) return d;
    return nullptr;
}

Document* Editor::documentById(uint64_t id) const {
    for (Document* d : tabs_)
        if (d->id() == id) return d;
    return nullptr;
}

Document* Editor::previewTab() const {
    for (Document* d : tabs_)
        if (d->isPreviewTab()) return d;
    return nullptr;
}

std::vector<Document*> Editor::dirtyDocuments() const {
    std::vector<Document*> out;
    for (Document* d : tabs_)
        if (d->dirty()) out.push_back(d);
    return out;
}

// ---------------------------------------------------------------- opening

bool Editor::opensInPreview(EditorTabs::OpenFrom from) const {
    EditorTabs::PreviewSettings s;
    s.enabled = settings_.enablePreview();
    s.fromQuickOpen = settings_.enablePreviewFromQuickOpen();
    return EditorTabs::opensInPreview(from, s);
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
    const Document::Read r = Document::readText(path, content);
    return textRead(d, path, (int)r, content);
}

bool Editor::textRead(Document* d, const std::string& path, int result,
                      const std::string& content) {
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
    d->loadText(path, content);
    const bool blank = std::all_of(content.begin(), content.end(),
                                   [](unsigned char c) { return std::isspace(c); });
    d->setShowsPreview((d->isMarkdown() || d->isLatex()) && !blank);
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
    return loadText(d, path);
}

bool Editor::openFile(const std::string& path, EditorTabs::OpenFrom from) {
    const bool inPreview = opensInPreview(from);
    // VS Code's workbench.editor.enablePreviewFromCodeNavigation, off by
    // default: a preview tab a navigation starts from is kept, so the
    // target does not replace the file it was reached from.
    if (from == EditorTabs::OpenFrom::CodeNavigation && inPreview && active_ &&
        active_->isPreviewTab() && documentFor(path) != active_)
        keepPreviewTab(active_);
    if (Document* d = documentFor(path)) {
        // A kept open of the preview tab's file keeps it; an open in preview
        // never makes a kept tab a preview.
        if (d->isPreviewTab() && !EditorTabs::staysPreview(true, inPreview)) keepPreviewTab(d);
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
    if (!large_->mayOpen(path, from)) return false;
    Document* d = new Document(this, &tags_);
    const bool ok = loadInto(d, path);
    Document* old = inPreview ? previewTab() : nullptr;
    // An old preview tab with unsaved changes is never dropped.
    if (old && old->dirty()) {
        keepPreviewTab(old);
        old = nullptr;
    }
    int at;
    if (inPreview) {
        const EditorTabs::PreviewPlace place =
            EditorTabs::placePreview(activeIndex(), old ? indexOf(old) : -1, tabCount());
        // `index` counts without the old tab, which is still in the row.
        at = place.index;
        if (place.replace >= 0 && place.index >= place.replace) at++;
        d->setPreviewTab(true);
    } else {
        at = EditorTabs::insertionIndex(activeIndex(), tabCount());
    }
    tabs_.insert(tabs_.begin() + at, d);
    activate(d);
    // The preview tab a newer one replaced: no question (it has no unsaved
    // changes) and no place in Reopen Closed Tab, as VS Code leaves a
    // replaced preview editor out of its recently closed list.
    if (old && old->isPreviewTab() && !old->dirty()) dropTab(old, false);
    return ok;
}

Document* Editor::addTab(const std::string& path) {
    Document* d = new Document(this, &tags_);
    d->setUnread(path);
    tabs_.push_back(d);
    refreshStrip();
    return d;
}

void Editor::showDiff(const std::string& title, const std::string& bytes, bool commit) {
    Document* d = diffDoc_;
    if (!d) {
        d = new Document(this, &tags_);
        tabs_.insert(tabs_.begin() + EditorTabs::insertionIndex(activeIndex(), tabCount()), d);
        diffDoc_ = d;
    }
    d->setDiff(title, bytes, commit);
    if (d == active_) {
        showActive();
        return;
    }
    activate(d);
}

// ---------------------------------------------------------------- switching

// Put a tab on show. The tab that was on show keeps its text, unsaved
// changes, undo and place; the one shown is checked against the disk, and
// asked about if it changed there while it had unsaved changes.
void Editor::activate(Document* d) {
    if (!d || indexOf(d) < 0) return;
    d->lastUsed = ++stamp_;
    if (d == active_) {
        refreshStrip();
        return;
    }
    if (onActiveWillChange) onActiveWillChange();
    stash();
    active_ = d;
    showActive();
    d->checkExternalChange();
    if (onActiveChanged) onActiveChanged();
}

void Editor::selectTab(int index) {
    Document* d = document(index);
    if (!d) return;
    activate(d);
    focusActiveDocument();
}

// Ctrl+PageDown and Ctrl+PageUp (Ctrl+Tab and Ctrl+Shift+Tab), wrapping.
void Editor::stepTab(int delta) {
    if (tabs_.empty()) {
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
#ifdef MINICODE_ENABLE_PDF
    if (d && d->isLatex() && d->showsPreview() && d->latexPreview())
        target = d->latexPreview()->widget();
    else
#endif
    if (d && d->kind() == Document::Kind::Media)
        target = media_->widget();
    else if (d && d->kind() == Document::Kind::Diff && diff_)
        target = diff_->textView();
    if (!gtk_widget_grab_focus(target)) gtk_widget_grab_focus(view_);
}

// ---------------------------------------------------------------- closing

void Editor::closeTab(Document* d) {
    if (indexOf(d) < 0) return;
    dropTab(d, true);
}

// Out of the row: remembered for Reopen Closed Tab when `remember` (a file's
// tab, not a diff's), and the tab shown next is the one used most recently.
void Editor::dropTab(Document* d, bool remember) {
    const int index = indexOf(d);
    if (index < 0) return;
    const bool wasActive = d == active_;
    if (wasActive && onActiveWillChange) onActiveWillChange();
    if (wasActive) stash();
    if (remember && d->kind() != Document::Kind::Diff && !d->path().empty()) {
        EditorTabs::ClosedTab c = closedTabOf(d);
        c.index = index;
        closed_.remember(c);
    }
    std::vector<uint64_t> stamps;
    for (Document* t : tabs_) stamps.push_back(t->lastUsed);
    const int next = EditorTabs::activeAfterClose(stamps, activeIndex(), index);
    tabs_.erase(tabs_.begin() + index);
    if (d == diffDoc_) diffDoc_ = nullptr;
    if (wasActive) {
        // The next tab on show first, so the language server lets go of this
        // one's buffer before it goes.
        active_ = nullptr;
        if (next >= 0) {
            active_ = tabs_[(size_t)next];
            active_->lastUsed = ++stamp_;
        }
        showActive();
        if (active_) active_->checkExternalChange();
        if (onActiveChanged) onActiveChanged();
    }
    destroy(d);
    refreshStrip();
}

// Gone for good: what the slot held for it comes off first.
void Editor::destroy(Document* d) {
    if (d == colorDoc_) {
        if (colorSaveTimer_) g_source_remove(colorSaveTimer_);
        colorSaveTimer_ = 0;
        colorDoc_ = nullptr;
    }
    if (d == mdEditDoc_) dropMdPopover();
#ifdef MINICODE_ENABLE_PDF
    if (LatexPreview* l = d->latexPreview())
        if (gtk_widget_get_parent(l->widget()) == slot_)
            gtk_stack_remove(GTK_STACK(slot_), l->widget());
#endif
    delete d;
}

void Editor::closeAllTabs() {
    if (onActiveWillChange) onActiveWillChange();
    stash();
    active_ = nullptr;
    diffDoc_ = nullptr;
    const std::vector<Document*> all = tabs_;
    tabs_.clear();
    showActive();   // the welcome text, with no buffer of theirs on show
    for (Document* d : all) destroy(d);
    if (onActiveChanged) onActiveChanged();
}

void Editor::keepPreviewTab(Document* d) {
    if (!d || !d->isPreviewTab()) return;
    d->setPreviewTab(false);
    refreshStrip();
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
    if (d->isMarkdown() || d->isLatex()) d->setShowsPreview(c.preview);
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
    while (closed_.take(&c, isRegularFile)) {
        if (Document* open = documentFor(c.path)) {
            selectTab(indexOf(open));
            return true;
        }
        Document* d = new Document(this, &tags_);
        loadInto(d, c.path);
        placeTab(d, c);
        tabs_.insert(tabs_.begin() + EditorTabs::ClosedTabs::indexIn(c, tabCount()), d);
        activate(d);
        focusActiveDocument();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- moves and the Trash

void Editor::pathMoved(const std::string& from, const std::string& to) {
    for (Document* d : tabs_) {
        if (d->kind() == Document::Kind::Diff || !isInside(d->path(), from)) continue;
        const std::string now = to + d->path().substr(from.size());
        const bool wasPreview = d->showsPreview();
        d->setPath(now);
#ifdef MINICODE_ENABLE_PDF
        if (d->latexPreview()) d->latexPreview()->setPath(now);   // the preview follows
#endif
        if (d != active_) continue;
        if (d->kind() == Document::Kind::Media) media_->setPath(now);   // watched under its new name
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
    const std::vector<Document*> all = tabs_;
    for (Document* d : all)
        if (d->kind() != Document::Kind::Diff && !d->dirty() && isInside(d->path(), path))
            dropTab(d, false);
}

// ---------------------------------------------------------------- the strip

// What each tab shows: the file's name, with as much of its folder as tells
// it from another of the same name (EditorTabs::labels), a diff's title, the
// dot for unsaved changes, and italics for the preview tab.
void Editor::refreshStrip() {
    std::vector<std::string> paths;
    for (Document* d : tabs_)
        paths.push_back(d->kind() == Document::Kind::Diff ? std::string() : d->path());
    const std::vector<std::string> names = EditorTabs::labels(paths);
    std::vector<TabStrip::Item> items;
    for (size_t i = 0; i < tabs_.size(); i++) {
        const Document* d = tabs_[i];
        TabStrip::Item it;
        it.title = d->kind() == Document::Kind::Diff ? d->diffTitle() : names[i];
        if (it.title.empty()) it.title = baseName(d->path());
        it.tooltip = d->path().empty() ? it.title : d->path();
        it.dirty = d->dirty();
        it.preview = d->isPreviewTab();
        items.push_back(it);
    }
    strip_->setItems(items, activeIndex());
    gtk_widget_set_visible(strip_->widget(), !tabs_.empty());
    if (onDocumentsChanged) onDocumentsChanged(nullptr);
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
