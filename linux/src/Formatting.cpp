// Formatting.cpp — see Formatting.h. The Mac's EditorController+Format.mm,
// step for step.
#include "Formatting.h"

#include "Document.h"
#include "Editor.h"
#include "FolderConsent.h"
#include "FormatRunner.h"
#include "Formatter.h"
#include "Lsp.h"
#include "MultiEdit.h"

#include <algorithm>
#include <vector>

namespace {
// Format Document's limit, the Mac's: then SIGTERM, and SIGKILL a second on.
const double kTimeoutSeconds = 10;
// A note that is not a complaint closes by itself.
const guint kNoteMs = 2500;
}  // namespace

Formatting::Formatting(GtkWindow* window, Editor* editor, LspSession* lsp, Host host)
    : window_(window), editor_(editor), lsp_(lsp), host_(std::move(host)) {
    // The note is the text view's popover by gtk_widget_set_parent, which
    // GtkTextView's own dispose does not know about (Lsp.cpp's trap): it
    // goes when the view is unrealized.
    g_signal_connect(editor_->textView(), "unrealize", G_CALLBACK(onViewUnrealize), this);
}

Formatting::~Formatting() {
    closeNote();
    g_signal_handlers_disconnect_by_data(editor_->textView(), this);
    if (note_) gtk_widget_unparent(note_);
    note_ = noteLabel_ = nullptr;
}

void Formatting::onViewUnrealize(GtkWidget*, gpointer selfp) {
    Formatting* self = static_cast<Formatting*>(selfp);
    self->closeNote();
    if (self->note_) gtk_widget_unparent(self->note_);
    self->note_ = self->noteLabel_ = nullptr;
}

bool Formatting::canFormat() const {
    return editor_->active() && editor_->isSource() && !editor_->currentPath().empty() &&
           gtk_text_view_get_editable(GTK_TEXT_VIEW(editor_->textView()));
}

void Formatting::formatDocument() {
    if (!canFormat()) {
        gtk_widget_error_bell(GTK_WIDGET(window_));
        return;
    }
    run(false, nullptr);
}

void Formatting::run(bool quiet, std::function<void()> then) {
    Document* d = editor_->active();
    if (!d || !canFormat()) {
        if (then) then();
        return;
    }
    std::weak_ptr<int> alive = alive_;
    // Format Document while Ctrl+S waits for format.on-save: this run
    // takes over the save, since the one it replaces will be dropped.
    if (!then && saveTab_ && saveTab_ == d->id())
        then = [this, alive] {
            if (!alive.expired()) finishSave();
        };
    closeNote();
    GtkTextBuffer* b = editor_->sourceBuffer();
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(b, &s, &e);
    const long selStart = gtk_text_iter_get_offset(&s), selEnd = gtk_text_iter_get_offset(&e);
    const std::string original = editor_->text();
    // A selection formats its lines, where the formatter can.
    int first = 0, last = 0;
    if (selEnd > selStart)
        FormatRun::selectedLines(original, (size_t)selStart, (size_t)selEnd, &first, &last);
    const unsigned gen = ++generation_;
    const uint64_t id = d->id();
    const std::string path = editor_->currentPath();
    const std::string root = host_.root ? host_.root() : "";
    const std::string setting =
        editor_->documentSettings().formatCommand(Formatter::languageFor(path));
    FormatRunner::format(
        original, path, root, setting, FolderConsent::answer(root), first, last, kTimeoutSeconds,
        [this, alive, gen, id, quiet, then, original, selStart, selEnd](FormatRunner::Result r) {
            if (alive.expired()) return;
            // Another format started, or another tab is on show (which
            // saved this one as it left, if a save waited).
            Document* now = editor_->active();
            if (gen != generation_ || !now || now->id() != id) return;
            if (r.decision.kind == FormatRun::Kind::NeedsConsent) {
                ask(r.decision, quiet, then);
                return;
            }
            // No formatter is planned for this file: a running language
            // server that formats it does, never in place of a project's.
            if (r.decision.serverMayFormat && canFormat() && editor_->text() == original &&
                lsp_->formatDocument(
                    selStart, selEnd,
                    [this, alive, quiet, then, id](const std::string& note, bool error) {
                        if (alive.expired()) return;
                        Document* on = editor_->active();
                        if (!note.empty() && (error || !quiet) && on && on->id() == id)
                            showNote(note, error);
                        if (then) then();
                    }))
                return;
            if (!r.ok) {
                if (!(quiet && r.decision.kind == FormatRun::Kind::Unavailable))
                    showNote(r.message, true);
            } else if (!canFormat() || editor_->text() != original) {
                // Typed into meanwhile, or a preview took over: the output
                // is for text that is no longer there.
            } else if (!apply(r.text)) {
                if (!quiet)
                    showNote(FormatRun::alreadyFormatted(r.decision.tool, r.decision.style), false);
            }
            if (then) then();
        });
}

// The formatter to run is the folder's own (its .venv or node_modules), or
// its configuration runs code, and the folder has no answer: ask, as the
// bar does for its Python environment, and keep the answer for the folder
// (one answer for both). Allow or Don't Allow formats again under the
// answer; Cancel stores nothing and formats nothing (a save that waited
// goes ahead).
void Formatting::ask(const FormatRun::Decision& d, bool quiet, std::function<void()> then) {
    questions_++;
    const FormatRun::Question q = FormatRun::consentQuestion(d);
    GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", q.message.c_str());
    gtk_alert_dialog_set_detail(dlg, q.detail.c_str());
    const char* buttons[] = {"Allow", "Don't Allow", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 2);
    gtk_alert_dialog_set_modal(dlg, TRUE);
    struct Asked {
        Formatting* self;
        std::weak_ptr<int> alive;
        bool quiet;
        std::function<void()> then;
        uint64_t id;
        std::string root;
    };
    Document* on = editor_->active();
    auto* asked = new Asked{this, alive_, quiet, std::move(then), on ? on->id() : 0,
                            host_.root ? host_.root() : ""};
    gtk_alert_dialog_choose(dlg, window_, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer p) {
            Asked* a = static_cast<Asked*>(p);
            const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            if (!a->alive.expired()) {
                Formatting* self = a->self;
                if (choice == 0 || choice == 1) {
                    FolderConsent::set(a->root, choice == 0);
                    Document* now = self->editor_->active();
                    if (now && now->id() == a->id && self->canFormat()) {
                        self->run(a->quiet, a->then);
                        delete a;
                        return;
                    }
                }
                if (a->then) a->then();
            }
            delete a;
        },
        asked);
    g_object_unref(dlg);
}

bool Formatting::formatBeforeSaving() {
    if (!canFormat()) return false;
    // A file with no formatter of its own may still have a server that
    // formats it (run asks it).
    if (Formatter::languageFor(editor_->currentPath()).empty() && !lsp_->canFormatDocument())
        return false;
    saveTab_ = editor_->active()->id();
    std::weak_ptr<int> alive = alive_;
    run(true, [this, alive] {
        if (!alive.expired()) finishSave();
    });
    return true;
}

bool Formatting::beforeSaving(bool organize, bool format) {
    if (organize && canFormat()) {
        const uint64_t tab = editor_->active()->id();
        std::weak_ptr<int> alive = alive_;
        const bool started = lsp_->organizeImportsBeforeSave([this, alive, tab, format] {
            // Dropped meanwhile: the tab left the screen and was saved then.
            if (alive.expired() || saveTab_ != tab || !editor_->active() ||
                editor_->active()->id() != tab)
                return;
            saveTab_ = 0;
            if (format && formatBeforeSaving()) return;
            if (host_.save) host_.save(editor_->active());
        });
        if (started) {
            saveTab_ = tab;
            return true;
        }
    }
    return format && formatBeforeSaving();
}

// The formatter is done (or its result was dropped): the save Ctrl+S asked
// for happens now, if it still waits.
void Formatting::finishSave() {
    const uint64_t t = saveTab_;
    saveTab_ = 0;
    Document* d = t ? editor_->documentById(t) : nullptr;
    if (d && d == editor_->active() && host_.save) host_.save(d);
}

bool Formatting::saveWaits(const Document* d) const { return d && saveTab_ && d->id() == saveTab_; }

// The tab whose save waits for the formatter is leaving the screen: the
// formatter's result is dropped and the tab saved now, unformatted, while
// it is still the one on show.
void Formatting::tabLeaving() {
    closeNote();
    Document* d = editor_->active();
    if (!saveTab_ || !d || saveTab_ != d->id()) return;
    saveTab_ = 0;
    generation_++;
    if (host_.save) host_.save(d);
}

// The formatter's output in place of the source as the line runs that
// changed (FormatRun::Replacement), one user action, so one Ctrl+Z takes it
// back; the caret and the selection go where their text went. The line at
// the top of the view stays there: GtkTextView keeps its first line's mark
// through edits around it.
bool Formatting::apply(const std::string& text) {
    GtkTextBuffer* b = editor_->sourceBuffer();
    const std::string before = editor_->text();
    const FormatRun::Replacement r(before, text);
    if (r.empty()) return false;
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(b, &ins, gtk_text_buffer_get_insert(b));
    gtk_text_buffer_get_iter_at_mark(b, &bound, gtk_text_buffer_get_selection_bound(b));
    const size_t insAt = (size_t)gtk_text_iter_get_offset(&ins);
    const size_t boundAt = (size_t)gtk_text_iter_get_offset(&bound);
    std::vector<MultiEdit::Change> changes;
    changes.reserve(r.changes().size());
    for (const FormatRun::Replacement::Change& c : r.changes())
        changes.push_back(MultiEdit::Change{c.start, c.length, c.text});
    if (!MultiEdit::apply(b, changes, false)) return false;
    const size_t newIns = r.map(insAt);
    const size_t newBound = insAt == boundAt ? newIns : r.map(boundAt);
    gtk_text_buffer_get_iter_at_offset(b, &ins, (int)newIns);
    gtk_text_buffer_get_iter_at_offset(b, &bound, (int)newBound);
    gtk_text_buffer_select_range(b, &ins, &bound);
    return true;
}

// ------------------------------------------------------------------- notes

bool Formatting::noteShown() const { return note_ && gtk_widget_get_visible(note_); }

void Formatting::closeNote() {
    if (noteTimer_) g_source_remove(noteTimer_);
    noteTimer_ = 0;
    if (noteBuffer_) {
        if (noteBufferHandler_) g_signal_handler_disconnect(noteBuffer_, noteBufferHandler_);
        g_object_unref(noteBuffer_);
    }
    noteBuffer_ = nullptr;
    noteBufferHandler_ = 0;
    if (note_) gtk_popover_popdown(GTK_POPOVER(note_));
}

// A note from Format Document by the caret: the formatter's complaint, or
// that there was nothing to change (which closes itself). It never takes
// the keyboard; it closes when the text or the caret moves, another tab is
// shown, or the next format starts.
void Formatting::showNote(const std::string& message, bool error) {
    lastNote_ = message;
    notes_++;
    closeNote();
    GtkWidget* view = editor_->textView();
    if (!gtk_widget_get_mapped(view)) {
        gtk_widget_error_bell(view);
        return;
    }
    if (!note_) {
        note_ = gtk_popover_new();
        gtk_widget_add_css_class(note_, "minicode-hover");
        gtk_popover_set_autohide(GTK_POPOVER(note_), FALSE);
        gtk_popover_set_position(GTK_POPOVER(note_), GTK_POS_BOTTOM);
        gtk_widget_set_can_focus(note_, FALSE);
        noteLabel_ = gtk_label_new("");
        gtk_label_set_wrap(GTK_LABEL(noteLabel_), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(noteLabel_), 80);
        gtk_label_set_xalign(GTK_LABEL(noteLabel_), 0.0);
        gtk_label_set_selectable(GTK_LABEL(noteLabel_), FALSE);
        gtk_popover_set_child(GTK_POPOVER(note_), noteLabel_);
        gtk_widget_set_parent(note_, view);
    }
    gtk_label_set_text(GTK_LABEL(noteLabel_), message.c_str());
    // Under the caret's line when it is on screen, else at the top of the pane.
    GtkTextView* tv = GTK_TEXT_VIEW(view);
    GtkTextBuffer* b = gtk_text_view_get_buffer(tv);
    GtkTextIter caret;
    gtk_text_buffer_get_iter_at_mark(b, &caret, gtk_text_buffer_get_insert(b));
    GdkRectangle at, visible;
    gtk_text_view_get_iter_location(tv, &caret, &at);
    gtk_text_view_get_visible_rect(tv, &visible);
    if (at.y + at.height < visible.y || at.y > visible.y + visible.height) {
        at.x = visible.x + visible.width / 2;
        at.y = visible.y;
        at.height = 1;
    }
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, at.x, at.y, &wx, &wy);
    GdkRectangle r = {wx, wy, std::max(1, at.width), std::max(1, at.height)};
    gtk_popover_set_pointing_to(GTK_POPOVER(note_), &r);
    gtk_popover_popup(GTK_POPOVER(note_));
    // The next edit, or the caret moving, closes it.
    noteBuffer_ = GTK_TEXT_BUFFER(g_object_ref(b));
    noteBufferHandler_ = g_signal_connect_swapped(
        b, "mark-set", G_CALLBACK(+[](Formatting* self, GtkTextIter*, GtkTextMark* m) {
            GtkTextBuffer* buf = self->noteBuffer_;
            if (buf && m == gtk_text_buffer_get_insert(buf)) self->closeNote();
        }),
        this);
    if (!error)
        noteTimer_ = g_timeout_add(kNoteMs, [](gpointer p) -> gboolean {
            Formatting* self = static_cast<Formatting*>(p);
            self->noteTimer_ = 0;
            self->closeNote();
            return G_SOURCE_REMOVE;
        }, this);
}
