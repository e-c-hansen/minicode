// AutoCloseInput.cpp — see AutoCloseInput.h.
#include "AutoCloseInput.h"

#include "BufferText.h"
#include "Carets.h"
#include "Document.h"
#include "Editor.h"

#include <string>
#include <utility>

using MultiCaret::Caret;

namespace {

std::string toUtf8(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

AutoCloseInput::AutoCloseInput(Editor* editor) : editor_(editor), view_(editor->textView()) {
    g_signal_connect(view_, "notify::buffer", G_CALLBACK(onBuffer), this);
    watchBuffer();
}

AutoCloseInput::~AutoCloseInput() {
    if (buffer_) g_signal_handlers_disconnect_by_data(buffer_, this);
    g_signal_handlers_disconnect_by_data(view_, this);
}

void AutoCloseInput::watchBuffer() {
    GtkTextBuffer* now = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view_));
    if (now == buffer_) return;
    if (buffer_) g_signal_handlers_disconnect_by_data(buffer_, this);
    buffer_ = now;
    tracker_.clear();   // another text: nothing it remembers applies
    if (!buffer_) return;
    // Before the buffer changes, while the iterators still describe the old
    // text, as the Mac's storage notification gives the edited range.
    g_signal_connect(buffer_, "insert-text", G_CALLBACK(onInsert), this);
    g_signal_connect(buffer_, "delete-range", G_CALLBACK(onDelete), this);
    g_signal_connect_after(buffer_, "mark-set", G_CALLBACK(onMarkSet), this);
    g_signal_connect(buffer_, "notify::cursor-position", G_CALLBACK(onCursor), this);
}

void AutoCloseInput::onBuffer(GObject*, GParamSpec*, gpointer self) {
    static_cast<AutoCloseInput*>(self)->watchBuffer();
}

void AutoCloseInput::onInsert(GtkTextBuffer*, GtkTextIter* at, char* text, int len,
                              gpointer selfp) {
    AutoCloseInput* self = static_cast<AutoCloseInput*>(selfp);
    if (self->tracker_.empty() || len <= 0) return;
    self->tracker_.edited((size_t)gtk_text_iter_get_offset(at), 0,
                          (size_t)g_utf8_strlen(text, len));
}

void AutoCloseInput::onDelete(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer selfp) {
    AutoCloseInput* self = static_cast<AutoCloseInput*>(selfp);
    if (self->tracker_.empty()) return;
    int s = gtk_text_iter_get_offset(a), e = gtk_text_iter_get_offset(b);
    if (e < s) std::swap(s, e);
    if (e > s) self->tracker_.edited((size_t)s, (size_t)(e - s), 0);
}

// A caret the view moves itself (a click, an arrow, typing) lets go of the
// pairs it left.
void AutoCloseInput::onMarkSet(GtkTextBuffer* b, GtkTextIter*, GtkTextMark* mark, gpointer selfp) {
    AutoCloseInput* self = static_cast<AutoCloseInput*>(selfp);
    if (self->applying_ || self->tracker_.empty() || self->caretsApplying()) return;
    if (mark == gtk_text_buffer_get_insert(b) || mark == gtk_text_buffer_get_selection_bound(b))
        self->caretsAre(self->carets());
}

void AutoCloseInput::onCursor(GObject*, GParamSpec*, gpointer selfp) {
    AutoCloseInput* self = static_cast<AutoCloseInput*>(selfp);
    if (self->applying_ || self->tracker_.empty() || self->caretsApplying()) return;
    self->caretsAre(self->carets());
}

// ---------------------------------------------------------------- state

bool AutoCloseInput::applies() const {
    return buffer_ && editor_->isSource() && buffer_ == editor_->active()->buffer() &&
           gtk_text_view_get_editable(GTK_TEXT_VIEW(view_)) && !editor_->composing();
}

bool AutoCloseInput::closing() const { return editor_->documentSettings().autoClose(); }

const AutoClose::Language& AutoCloseInput::language() const {
    Document* d = editor_->active();
    return AutoClose::languageFor(d ? d->path() : std::string());
}

// Several carets edit and move through Carets.h, which says where they all
// are afterwards (caretsAre); meanwhile the buffer's selection is only the
// first of them.
bool AutoCloseInput::caretsApplying() const {
    Carets* c = editor_->carets();
    return c && c->applying();
}

std::vector<Caret> AutoCloseInput::carets() const {
    if (Carets* c = editor_->carets(); c && c->active()) return c->current();
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(buffer_, &ins, gtk_text_buffer_get_insert(buffer_));
    gtk_text_buffer_get_iter_at_mark(buffer_, &bound, gtk_text_buffer_get_selection_bound(buffer_));
    Caret c;
    c.anchor = (size_t)gtk_text_iter_get_offset(&bound);
    c.head = (size_t)gtk_text_iter_get_offset(&ins);
    return MultiCaret::normalize({c}, (size_t)gtk_text_buffer_get_char_count(buffer_));
}

// ---------------------------------------------------------------- the rules

AutoClose::Typed AutoCloseInput::typed(char16_t ch, const std::vector<Caret>& carets) {
    if (!buffer_) return AutoClose::Typed();
    const std::unique_ptr<AutoClose::Context> ctx =
        BufferText::context(editor_->active(), buffer_);
    return AutoClose::type(BufferText::text(buffer_), carets, ch, language(), *ctx, tracker_,
                           closing());
}

MultiCaret::Edit AutoCloseInput::deletePair(const std::vector<Caret>& carets) {
    if (!buffer_ || tracker_.empty() || !closing()) return MultiCaret::Edit();
    return AutoClose::deletePair(BufferText::text(buffer_), carets, language(), tracker_);
}

void AutoCloseInput::remember(const std::vector<AutoClose::Tracker::Closed>& closed) {
    tracker_.add(closed);
}

void AutoCloseInput::caretsAre(const std::vector<Caret>& carets) {
    if (tracker_.empty() || !buffer_) return;
    tracker_.validate(BufferText::text(buffer_), carets);
}

// ---------------------------------------------------------------- typing

bool AutoCloseInput::keyPressed(guint keyval, GdkModifierType mods) {
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return false;
    if (!applies()) return false;
    if (keyval == GDK_KEY_BackSpace) {
        const MultiCaret::Edit e = deletePair(carets());
        if (e.changes.empty()) return false;
        apply(e);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_));
        return true;
    }
    // Every pair's characters are printable ASCII; anything else types as
    // the text view types it, without reading the text.
    const gunichar c = gdk_keyval_to_unicode(keyval);
    if (c < 0x20 || c >= 0x7F) return false;
    const AutoClose::Language& lang = language();
    bool asks = false;
    for (const AutoClose::Pair& p : lang.pairs)
        if ((!p.open.empty() && p.open.back() == c) || (!p.close.empty() && p.close[0] == c))
            asks = true;
    for (const auto& s : lang.surround)
        if (s.first == c) asks = true;
    if (!asks) return false;
    const AutoClose::Typed t = typed((char16_t)c, carets());
    if (t.kind == AutoClose::Typed::Kind::None) return false;
    apply(t.edit);
    if (t.kind == AutoClose::Typed::Kind::Close) remember(t.closed);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view_), gtk_text_buffer_get_insert(buffer_));
    return true;
}

void AutoCloseInput::apply(const MultiCaret::Edit& e) {
    GtkTextBuffer* b = buffer_;
    applying_ = true;
    if (!e.changes.empty()) {
        gtk_text_buffer_begin_user_action(b);
        for (auto it = e.changes.rbegin(); it != e.changes.rend(); ++it) {
            GtkTextIter a;
            gtk_text_buffer_get_iter_at_offset(b, &a, (int)it->start);
            if (it->length) {
                GtkTextIter z;
                gtk_text_buffer_get_iter_at_offset(b, &z, (int)(it->start + it->length));
                gtk_text_buffer_delete(b, &a, &z);
            }
            if (it->text.empty()) continue;
            // The caret where the text goes in, so it ends up just after
            // it, as typing it leaves it. Not over a selection (a surround):
            // emptying it for a moment gives up the PRIMARY selection, and
            // X's late word that it was lost then took the new selection
            // away again, now and then (seen in the auto_close suite).
            if (!gtk_text_buffer_get_has_selection(b)) gtk_text_buffer_place_cursor(b, &a);
            const std::string bytes = toUtf8(it->text);
            gtk_text_buffer_insert(b, &a, bytes.data(), (int)bytes.size());
        }
        gtk_text_buffer_end_user_action(b);
    }
    if (!e.carets.empty()) {
        const Caret& c = e.carets[0];
        GtkTextIter head, anchor;
        gtk_text_buffer_get_iter_at_offset(b, &head, (int)c.head);
        gtk_text_buffer_get_iter_at_offset(b, &anchor, (int)c.anchor);
        gtk_text_buffer_select_range(b, &head, &anchor);
    }
    applying_ = false;
    caretsAre(carets());
}
