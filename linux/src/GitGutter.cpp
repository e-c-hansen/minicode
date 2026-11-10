// GitGutter.cpp — see GitGutter.h. The Mac's src/GitGutter.mm, step for
// step where GTK allows.
#include "GitGutter.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "GitRun.h"
#include "Gutter.h"
#include "MultiEdit.h"
#include "Settings.h"
#include "Utf8Offsets.h"

#include <limits.h>
#include <stdlib.h>

#include <algorithm>
#include <cstdio>

// What the worker knows of the file last looked at: its repository, its
// index entry and the text that entry names. Made on the worker, read on
// the main thread only through a shared pointer to a finished one.
struct GitGutter::Source {
    std::string path;          // as asked
    std::string top, gitDir, rel;
    bool inRepository = false;
    GitStamp stamp;            // the index file when the entry was read
    GitUi::IndexEntry entry;
    std::string blob;          // the entry's bytes
    bool hasOriginal = false;
};

namespace {

// From the worker back to the main loop, if the gutter still lives.
void deliver(std::shared_ptr<bool> alive, std::function<void()> fn) {
    struct Delivery {
        std::shared_ptr<bool> alive;
        std::function<void()> fn;
    };
    auto* d = new Delivery{std::move(alive), std::move(fn)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* d = static_cast<Delivery*>(p);
        if (*d->alive) d->fn();
        delete d;
        return G_SOURCE_REMOVE;
    }, d, nullptr);
}

void runJob(gpointer data, gpointer) {
    auto* job = static_cast<std::function<void()>*>(data);
    (*job)();
    delete job;
}

std::string realPath(const std::string& p) {
    char* r = realpath(p.c_str(), nullptr);
    if (!r) return std::string();
    std::string out = r;
    free(r);
    return out;
}

std::string dirOf(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : slash == 0 ? "/" : p.substr(0, slash);
}

std::string baseOf(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

// Off the main thread. The previous source is reused while the file and
// the index file are as they were.
std::shared_ptr<const GitGutter::Source> sourceFor(
    const std::string& path, const std::shared_ptr<const GitGutter::Source>& previous) {
    auto s = std::make_shared<GitGutter::Source>();
    s->path = path;
    if (previous && previous->path == path) {
        s->top = previous->top;
        s->gitDir = previous->gitDir;
        s->rel = previous->rel;
        s->inRepository = previous->inRepository;
        if (!s->inRepository) return s;
    } else {
        // Its own repository (a submodule's or a nested one's when it is in
        // one), and its path there, links resolved as git has them.
        GitResult r = runGit(dirOf(path), GitUi::topLevelArgs());
        if (!r.ok() || !GitUi::parseTopLevel(r.out, s->top, s->gitDir)) return s;
        const std::string full = realPath(path), top = realPath(s->top);
        if (full.empty() || top.empty() || full.compare(0, top.size() + 1, top + "/") != 0)
            return s;
        s->rel = full.substr(top.size() + 1);
        s->inRepository = true;
    }
    s->stamp = gitStampOf(s->gitDir + "/index");
    if (previous && previous->path == path && previous->stamp == s->stamp &&
        previous->stamp.sec >= 0) {
        s->entry = previous->entry;
        s->blob = previous->blob;
        s->hasOriginal = previous->hasOriginal;
        return s;
    }
    GitResult ls = runGit(s->top, GitUi::indexEntryArgs(s->rel));
    if (!ls.ok()) return s;
    s->entry = GitUi::parseIndexEntry(ls.out);
    if (!s->entry.ok) return s;   // untracked, a conflict, a link, a submodule
    GitResult blob = runGit(s->top, GitUi::blobArgs(s->entry.object));
    if (!blob.ok()) return s;
    s->blob = std::move(blob.out);
    s->hasOriginal = true;
    return s;
}

const char* kindName(QuickDiff::Kind k) {
    return k == QuickDiff::Kind::Added ? "added" : k == QuickDiff::Kind::Modified ? "modified"
                                                                                : "deleted";
}

}  // namespace

// ==================================================================== life
GitGutter::GitGutter(Editor* editor) : editor_(editor), view_(editor->textView()) {
    alive_ = std::make_shared<bool>(true);
    // One thread: git runs one command at a time, in the order asked.
    pool_ = g_thread_pool_new(runJob, nullptr, 1, FALSE, nullptr);
    g_signal_connect(view_, "notify::buffer", G_CALLBACK(onViewBuffer), this);
    // The popover is the view's child only by gtk_widget_set_parent, which
    // GtkTextView's own dispose does not know about (Lsp.cpp's trap).
    g_signal_connect(view_, "unrealize", G_CALLBACK(onUnrealize), this);
    if (Gutter* g = gutter())
        g->onChangeBar = [this](int index, const graphene_rect_t& rect) { showChange(index, rect); };
    attachBuffer();
}

GitGutter::~GitGutter() {
    *alive_ = false;
    closePopover();
    if (popover_) gtk_widget_unparent(popover_);
    popover_ = nullptr;
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    watch_.reset();
    detachBuffer();
    g_signal_handlers_disconnect_by_data(view_, this);
    if (Gutter* g = gutter()) g->onChangeBar = nullptr;
    if (actions_) {
        for (const char* name : {"nextchange", "previouschange", "revertchange", "stagechange"})
            if (GAction* a = g_action_map_lookup_action(actions_, name))
                g_signal_handlers_disconnect_by_data(a, this);
    }
    // git already running finishes on the pool's thread; what it delivers
    // finds alive_ false and is dropped. The pool frees itself after.
    g_thread_pool_free(pool_, FALSE, FALSE);
}

void GitGutter::post(std::function<void()> job) {
    g_thread_pool_push(pool_, new std::function<void()>(std::move(job)), nullptr);
}

// The buffer the view shows now. The editor sets its own pointer after the
// view's, so in notify::buffer it can still name a buffer just freed.
GtkTextBuffer* GitGutter::viewBuffer() const {
    return gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
}

Gutter* GitGutter::gutter() const {
    return editor_->codeView() ? editor_->codeView()->gutter() : nullptr;
}

const std::vector<QuickDiff::Bar>& GitGutter::bars() const {
    static const std::vector<QuickDiff::Bar> none;
    BufferLines* l = BufferLines::of(viewBuffer());
    return l ? l->changeBars() : none;
}

size_t GitGutter::barCount() const { return on_ ? bars().size() : 0; }

std::vector<std::string> GitGutter::barDescriptions() const {
    std::vector<std::string> out;
    for (const QuickDiff::Bar& b : bars()) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s %zu+%zu (was %zu+%zu)", kindName(b.kind), b.line,
                      b.lines, b.oldLine, b.oldLines);
        out.push_back(buf);
    }
    return out;
}

// ================================================================ settings
void GitGutter::applySettings(const Settings& s) {
    if (Gutter* g = gutter()) {
        g->setChangeLane(s.gitGutter());
        g->setChangeColors(toGdk(s.gitGutterColor(0)), toGdk(s.gitGutterColor(1)),
                           toGdk(s.gitGutterColor(2)));
    }
    deletedColor_ = toGdk(s.gitGutterColor(2));
    const bool was = on_;
    on_ = s.gitGutter();
    if (was && !on_) {
        // Off: the lane goes, and with it the bars on show and the watch.
        generation_++;
        closePopover();
        watch_.reset();
        if (BufferLines* l = BufferLines::of(viewBuffer())) l->setChangeBars({});
        shownPath_.clear();
        updateActions();
        return;
    }
    if (on_) refreshSoon();
}

void GitGutter::refreshSoon() {
    if (on_) scheduleAfter(50);
}

void GitGutter::holdLooksForChecks(bool hold) {
    held_ = hold;
    if (!hold && lookWanted_) {
        lookWanted_ = false;
        look();
    }
}

// ================================================================ watching
void GitGutter::onViewBuffer(GObject*, GParamSpec*, gpointer selfp) {
    static_cast<GitGutter*>(selfp)->attachBuffer();
}

void GitGutter::attachBuffer() {
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    if (b == buffer_) return;
    detachBuffer();
    generation_++;
    closePopover();
    if (b) {
        buffer_ = GTK_TEXT_BUFFER(g_object_ref(b));
        g_signal_connect_swapped(b, "changed", G_CALLBACK(+[](GitGutter* self) {
            self->edited();
        }), this);
        // The caret's line decides whether Revert and Stage Change can act.
        g_signal_connect_swapped(b, "mark-set", G_CALLBACK(+[](GitGutter* self, GtkTextIter*,
                                                               GtkTextMark* m) {
            if (self->buffer_ && m == gtk_text_buffer_get_insert(self->buffer_))
                self->updateActions();
        }), this);
    }
    updateActions();
    // Another file in the view: its bars (kept with its buffer) are looked
    // at again soon.
    refreshSoon();
}

void GitGutter::detachBuffer() {
    if (!buffer_) return;
    g_signal_handlers_disconnect_by_data(buffer_, this);
    g_object_unref(buffer_);
    buffer_ = nullptr;
}

void GitGutter::edited() {
    generation_++;
    lastEdit_ = g_get_monotonic_time();
    closePopover();
    if (!on_) return;
    pauseWanted_ = true;
    scheduleAfter(300);
}

void GitGutter::scheduleAfter(int ms) {
    if (timer_) return;
    timer_ = g_timeout_add((guint)ms, [](gpointer p) -> gboolean {
        GitGutter* self = static_cast<GitGutter*>(p);
        self->timer_ = 0;
        // Still typing: wait for the pause (one timer, not one a key).
        const gint64 since = (g_get_monotonic_time() - self->lastEdit_) / 1000;
        if (self->pauseWanted_ && since < 300) {
            self->scheduleAfter((int)(300 - since) + 10);
            return G_SOURCE_REMOVE;
        }
        self->pauseWanted_ = false;
        self->look();
        return G_SOURCE_REMOVE;
    }, this);
}

std::string GitGutter::pathToLook() const {
    if (!on_ || gitExecutable().empty()) return std::string();
    Document* d = editor_->active();
    if (!d || !editor_->isSource() || d->path().empty() || d->loading() ||
        d->largeFileFeaturesOff() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)))
        return std::string();
    return d->path();
}

// On the main loop: what to diff; on the worker: the original (read again
// only when the file or the index changed); back on the main loop, the
// text as it is then, if nothing was typed meanwhile, and on the worker
// the diff; back once more, the bars, if still nothing was typed.
void GitGutter::look() {
    if (!on_) return;
    if (held_) {
        lookWanted_ = true;
        return;
    }
    looks_++;
    const unsigned gen = ++generation_;
    const std::string path = pathToLook();
    GtkTextBuffer* buffer = viewBuffer();
    if (path.empty()) {
        watch_.reset();
        shownPath_.clear();
        shownBuffer_ = buffer;
        entry_ = GitUi::IndexEntry();
        setBars(buffer, {});
        return;
    }
    std::shared_ptr<const Source> previous = source_;
    std::shared_ptr<bool> alive = alive_;
    post([this, alive, path, previous, gen, buffer] {
        std::shared_ptr<const Source> src = sourceFor(path, previous);
        deliver(alive, [this, alive, src, path, gen, buffer] {
            source_ = src;
            watch(src);
            // Typed, switched or looked again meanwhile: the next look's.
            if (gen != generation_ || pathToLook() != path || viewBuffer() != buffer) return;
            if (!src->hasOriginal) {
                shownPath_ = path;
                shownBuffer_ = buffer;
                original_ = QuickDiff::Original();
                top_ = src->top;
                rel_ = src->rel;
                entry_ = GitUi::IndexEntry();
                setBars(buffer, {});
                diffs_++;
                return;
            }
            auto text = std::make_shared<std::string>(editor_->text());
            post([this, alive, src, path, gen, buffer, text] {
                auto original = std::make_shared<QuickDiff::Original>(QuickDiff::readOriginal(
                    src->blob, FolderSearch::TextEncoding::Utf8, false));
                auto bars = std::make_shared<std::vector<QuickDiff::Bar>>(
                    QuickDiff::bars(original->text, utf16::fromUtf8(*text)));
                deliver(alive, [this, src, path, gen, buffer, original, bars] {
                    if (gen != generation_ || pathToLook() != path || viewBuffer() != buffer)
                        return;
                    shownPath_ = path;
                    shownBuffer_ = buffer;
                    original_ = std::move(*original);
                    top_ = src->top;
                    rel_ = src->rel;
                    entry_ = src->entry;
                    setBars(buffer, std::move(*bars));
                    diffs_++;
                });
            });
        });
    });
}

void GitGutter::setBars(GtkTextBuffer* buffer, std::vector<QuickDiff::Bar> bars) {
    BufferLines* l = BufferLines::of(buffer);
    if (l && l->changeBars() != bars) {
        // The bar the popover shows may be another one now.
        closePopover();
        l->setChangeBars(std::move(bars));
    }
    updateActions();
}

// The repository folder of the file looked at, while it has one: a stage,
// commit or checkout made anywhere brings a look at once.
void GitGutter::watch(const std::shared_ptr<const Source>& src) {
    const std::string dir = src && src->inRepository ? src->gitDir : std::string();
    if (watch_ && watch_->dir() == dir) return;
    watch_.reset();
    if (dir.empty()) return;
    watch_ = std::make_unique<GitDirWatch>(dir, src->stamp, [this](bool gone) {
        indexChanges_++;
        if (gone) {
            // The repository went: whatever is there now is the next look's.
            source_.reset();
            watch_.reset();   // this callback's own watch; nothing of it is used after
        }
        refreshSoon();
    });
}

// ================================================================ moving
size_t GitGutter::caretLine() const {
    GtkTextBuffer* b = viewBuffer();
    BufferLines* l = BufferLines::of(b);
    if (!l) return 0;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));
    return l->lineOf((size_t)gtk_text_iter_get_offset(&it));
}

int GitGutter::barAtCaret() const {
    if (!on_ || pathToLook().empty()) return -1;
    return QuickDiff::barAt(bars(), caretLine());
}

bool GitGutter::goToNextChange(bool forward) {
    GtkTextBuffer* b = viewBuffer();
    BufferLines* l = BufferLines::of(b);
    const std::vector<QuickDiff::Bar>& bs = bars();
    const int at = !on_ || !l || pathToLook().empty() ? -1
                   : forward ? QuickDiff::nextBar(bs, caretLine())
                             : QuickDiff::previousBar(bs, caretLine());
    if (at < 0) {
        gtk_widget_error_bell(view_);
        return false;
    }
    const QuickDiff::Bar& bar = bs[(size_t)at];
    // A deletion's caret goes to the line below it, where the old lines were.
    const size_t lines = l->lineCount();
    const size_t line = bar.kind == QuickDiff::Kind::Deleted
                            ? std::min(bar.line, lines ? lines - 1 : 0)
                            : bar.line;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(b, &it, (int)l->lineStart(line));
    gtk_text_buffer_place_cursor(b, &it);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(b), 0.1, FALSE,
                                 0, 0);
    return true;
}

// ================================================================ acting
bool GitGutter::revertChange(int index) {
    const std::vector<QuickDiff::Bar>& bs = bars();
    GtkTextBuffer* b = viewBuffer();
    if (!on_ || index < 0 || (size_t)index >= bs.size() || pathToLook().empty() ||
        pathToLook() != shownPath_ || b != shownBuffer_) {
        gtk_widget_error_bell(view_);
        return false;
    }
    const QuickDiff::Bar bar = bs[(size_t)index];
    const std::u16string buffer = utf16::fromUtf8(editor_->text());
    const QuickDiff::Edit e = QuickDiff::revertEdit(bar, original_.text, buffer);
    if (e.start + e.length > buffer.size()) {
        gtk_widget_error_bell(view_);
        return false;
    }
    closePopover();
    MultiEdit::Change c;
    c.start = (size_t)utf16::toCharOffset(buffer, e.start);
    c.length = (size_t)utf16::toCharOffset(buffer, e.start + e.length) - c.start;
    c.text = utf16::toUtf8(e.text);
    const bool ok = MultiEdit::apply(b, {c});
    gtk_widget_grab_focus(view_);
    return ok;
}

bool GitGutter::stageChange(int index) {
    const std::vector<QuickDiff::Bar>& bs = bars();
    if (!on_ || index < 0 || (size_t)index >= bs.size() || top_.empty() || !entry_.ok ||
        pathToLook().empty() || pathToLook() != shownPath_ || viewBuffer() != shownBuffer_) {
        gtk_widget_error_bell(view_);
        return false;
    }
    closePopover();
    const std::string staged = QuickDiff::stagedBlob(
        QuickDiff::stagedText(bs[(size_t)index], original_.text, utf16::fromUtf8(editor_->text())),
        original_);
    const std::string top = top_, rel = rel_;
    const GitUi::IndexEntry entry = entry_;
    std::shared_ptr<bool> alive = alive_;
    post([this, alive, staged, top, rel, entry] {
        std::string failure;
        // The index must still hold what the bars were made from.
        GitResult ls = runGit(top, GitUi::indexEntryArgs(rel));
        const GitUi::IndexEntry now = GitUi::parseIndexEntry(ls.out);
        if (!ls.ok() || !now.ok || now.object != entry.object) {
            failure = "The staged version of this file changed since its bars were drawn, so "
                      "nothing was staged. The bars now show it as it is.";
        } else {
            GitResult h = runGit(top, GitUi::hashObjectArgs(rel), &staged);
            if (!h.ok()) {
                failure = h.err.empty() ? std::string("git hash-object failed.") : gitFailure(h);
            } else {
                GitResult u = runGit(top, GitUi::updateIndexArgs(entry, h.out, rel));
                if (!u.ok())
                    failure = u.err.empty() ? std::string("git update-index failed.")
                                            : gitFailure(u);
            }
        }
        deliver(alive, [this, failure, rel] {
            if (!failure.empty()) {
                gtk_widget_error_bell(view_);
                if (onNote) onNote(failure);
            } else if (onNote) {
                onNote("Staged a change to " + baseOf(rel) + ".");
            }
            if (onIndexChanged) onIndexChanged();
            refreshSoon();
            if (onStaged) onStaged(failure);
        });
    });
    return true;
}

// ================================================================ actions
void GitGutter::installActions(GActionMap* window) {
    actions_ = window;
    struct Def {
        const char* name;
        void (*run)(GitGutter*);
    };
    static const Def defs[] = {
        {"nextchange", [](GitGutter* s) { s->goToNextChange(true); }},
        {"previouschange", [](GitGutter* s) { s->goToNextChange(false); }},
        {"revertchange", [](GitGutter* s) { s->revertChange(s->barAtCaret()); }},
        {"stagechange", [](GitGutter* s) { s->stageChange(s->barAtCaret()); }},
    };
    for (const Def& d : defs) {
        GSimpleAction* a = g_simple_action_new(d.name, nullptr);
        g_object_set_data(G_OBJECT(a), "minicode-run", (gpointer)d.run);
        g_signal_connect(a, "activate", G_CALLBACK(+[](GSimpleAction* a, GVariant*, gpointer p) {
            auto run = reinterpret_cast<void (*)(GitGutter*)>(
                g_object_get_data(G_OBJECT(a), "minicode-run"));
            if (run) run(static_cast<GitGutter*>(p));
        }), this);
        g_action_map_add_action(window, G_ACTION(a));
        g_object_unref(a);
    }
    updateActions();
}

void GitGutter::enable(const char* action, bool on) {
    if (!actions_) return;
    if (GAction* a = g_action_map_lookup_action(actions_, action))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

// The Mac's validation: Next and Previous Change with bars, Revert and
// Stage Change with a bar on the caret's line.
void GitGutter::updateActions() {
    if (!actions_) return;
    const bool any = on_ && !bars().empty() && !pathToLook().empty();
    enable("nextchange", any);
    enable("previouschange", any);
    const bool here = any && barAtCaret() >= 0;
    enable("revertchange", here);
    enable("stagechange", here);
}

// ================================================================ the popover
void GitGutter::onUnrealize(GtkWidget*, gpointer selfp) {
    GitGutter* self = static_cast<GitGutter*>(selfp);
    self->closePopover();
    if (self->popover_) gtk_widget_unparent(self->popover_);
    self->popover_ = nullptr;
}

bool GitGutter::popoverShown() const {
    return popover_ && gtk_widget_get_visible(popover_) && popoverBar_ >= 0;
}

void GitGutter::closePopover() {
    popoverBar_ = -1;
    if (popover_ && gtk_widget_get_visible(popover_)) gtk_popover_popdown(GTK_POPOVER(popover_));
}

void GitGutter::showChange(int index, const graphene_rect_t& rect) {
    const std::vector<QuickDiff::Bar>& bs = bars();
    Gutter* g = gutter();
    if (index < 0 || (size_t)index >= bs.size() || !g || !gtk_widget_get_mapped(view_)) return;
    const QuickDiff::Bar b = bs[(size_t)index];
    closePopover();
    popoverTitle_ = QuickDiff::describe(b);
    std::string text;
    for (const std::u16string& l : QuickDiff::oldLines(b, original_.text)) {
        if (!text.empty()) text += "\n";
        text += utf16::toUtf8(l);
    }
    popoverText_ = text;

    if (!popover_) {
        popover_ = gtk_popover_new();
        gtk_widget_add_css_class(popover_, "minicode-change-popover");
        gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_RIGHT);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        titleLabel_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(titleLabel_), 0);
        gtk_widget_set_hexpand(titleLabel_, TRUE);
        gtk_widget_add_css_class(titleLabel_, "heading");
        revertButton_ = gtk_button_new_with_label("Revert Change");
        stageButton_ = gtk_button_new_with_label("Stage Change");
        for (GtkWidget* button : {revertButton_, stageButton_})
            gtk_widget_add_css_class(button, "flat");
        g_signal_connect_swapped(revertButton_, "clicked", G_CALLBACK(+[](GitGutter* self) {
            self->revertChange(self->popoverBar_);
        }), this);
        g_signal_connect_swapped(stageButton_, "clicked", G_CALLBACK(+[](GitGutter* self) {
            self->stageChange(self->popoverBar_);
        }), this);
        gtk_box_append(GTK_BOX(head), titleLabel_);
        gtk_box_append(GTK_BOX(head), revertButton_);
        gtk_box_append(GTK_BOX(head), stageButton_);
        gtk_box_append(GTK_BOX(box), head);
        // The lines as they were, in the editor's font, on a tint of the
        // deleted color.
        oldView_ = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(oldView_), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(oldView_), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(oldView_), TRUE);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(oldView_), 4);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(oldView_), 4);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(oldView_), 4);
        oldScroll_ = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(oldScroll_), oldView_);
        gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(oldScroll_), 400);
        gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(oldScroll_), 400);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(oldScroll_), TRUE);
        gtk_box_append(GTK_BOX(box), oldScroll_);
        gtk_popover_set_child(GTK_POPOVER(popover_), box);
        gtk_widget_set_parent(popover_, view_);
        g_signal_connect_swapped(popover_, "closed", G_CALLBACK(+[](GitGutter* self) {
            self->popoverBar_ = -1;
        }), this);
    }
    popoverBar_ = index;
    gtk_label_set_text(GTK_LABEL(titleLabel_), popoverTitle_.c_str());
    gtk_widget_set_sensitive(stageButton_, entry_.ok);
    GtkTextBuffer* ob = gtk_text_view_get_buffer(GTK_TEXT_VIEW(oldView_));
    gtk_text_buffer_set_text(ob, text.c_str(), -1);
    GtkTextTag* tint = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(ob), "old");
    if (!tint) tint = gtk_text_buffer_create_tag(ob, "old", nullptr);
    GdkRGBA bg = deletedColor_;
    bg.alpha = 0.15f;
    g_object_set(tint, "paragraph-background-rgba", &bg, nullptr);
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(ob, &a, &z);
    gtk_text_buffer_apply_tag(ob, tint, &a, &z);
    // Twelve lines at most, then it scrolls.
    int lines = 1;
    for (char c : text) lines += c == '\n';
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(oldScroll_),
                                               std::min(lines, 12) * 18 + 10);
    gtk_widget_set_visible(oldScroll_, !text.empty());

    // Beside the bar: its rect from the gutter's coordinates to the view's.
    graphene_point_t from = GRAPHENE_POINT_INIT(rect.origin.x, rect.origin.y), to;
    if (!gtk_widget_compute_point(g->widget(), view_, &from, &to)) to = from;
    GdkRectangle r = {(int)to.x, (int)to.y, std::max(1, (int)rect.size.width),
                      std::max(1, (int)rect.size.height)};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &r);
    gtk_popover_popup(GTK_POPOVER(popover_));
}
