// NavHistoryUi.cpp — see NavHistoryUi.h.
#include "NavHistoryUi.h"

#include "Document.h"
#include "Editor.h"

#include <algorithm>

NavHistoryUi::NavHistoryUi(Editor* editor, Host host)
    : editor_(editor), host_(std::move(host)), rec_([this] {
          Nav::Recorder::Host h;
          h.current = [this](Nav::Place* p, size_t* length) { return place(p, length); };
          h.show = [this](const Nav::Place& p) { return show(p); };
          h.exists = [](const std::string& path) {
              return g_file_test(path.c_str(), G_FILE_TEST_EXISTS) != FALSE;
          };
          h.linesOf = [this](const std::vector<size_t>& offsets) {
              std::vector<int> out;
              Document* d = editor_->active();
              if (!d) return out;
              GtkTextBuffer* b = d->buffer();
              const int count = gtk_text_buffer_get_char_count(b);
              for (size_t o : offsets) {
                  GtkTextIter it;
                  gtk_text_buffer_get_iter_at_offset(b, &it, (int)std::min<size_t>(o, (size_t)count));
                  out.push_back(gtk_text_iter_get_line(&it));
              }
              return out;
          };
          h.bell = [this] {
              if (host_.bell) host_.bell();
          };
          return h;
      }()) {
    tabDidSwitch();   // whatever is on show now
}

NavHistoryUi::~NavHistoryUi() {
    if (caretIdle_) g_source_remove(caretIdle_);
    unwatch();
}

// Forgotten, with any caret move not looked at yet.
void NavHistoryUi::clear() {
    if (caretIdle_) g_source_remove(caretIdle_);
    caretIdle_ = 0;
    rec_.clear();
}

// Where the window is: the tab on show, and with its source on show (not a
// Markdown page, a picture, a PDF, a message) its selection.
bool NavHistoryUi::place(Nav::Place* out, size_t* length) {
    Document* d = editor_->active();
    if (!d || d->path().empty() || d->kind() == Document::Kind::Diff) return false;
    out->path = d->path();
    out->hasText = editor_->isSource();
    out->location = out->length = 0;
    out->line = 0;
    *length = 0;
    if (!out->hasText) return true;
    GtkTextBuffer* b = d->buffer();
    GtkTextIter ins, bound;
    gtk_text_buffer_get_iter_at_mark(b, &ins, gtk_text_buffer_get_insert(b));
    gtk_text_buffer_get_iter_at_mark(b, &bound, gtk_text_buffer_get_selection_bound(b));
    const int a = gtk_text_iter_get_offset(&ins), e = gtk_text_iter_get_offset(&bound);
    out->location = (size_t)std::min(a, e);
    out->length = (size_t)std::abs(a - e);
    out->line = gtk_text_iter_get_line(gtk_text_iter_compare(&ins, &bound) <= 0 ? &ins : &bound);
    *length = (size_t)gtk_text_buffer_get_char_count(b);
    return true;
}

// Back at a place: its file on show and, with text, its selection in view
// with the keyboard in the editor (Editor::revealLine, which also turns a
// Markdown page back to its source and waits for a file still loading).
bool NavHistoryUi::show(const Nav::Place& p) {
    if (!host_.openFile || !host_.openFile(p.path)) return false;
    Document* d = editor_->active();
    if (!d || d->path() != p.path) return false;
    if (!p.hasText || d->kind() != Document::Kind::Text) return true;
    if (d->loading()) return editor_->revealLine(p.line + 1, 0, 0);
    GtkTextBuffer* b = d->buffer();
    const int count = gtk_text_buffer_get_char_count(b);
    GtkTextIter a, e;
    gtk_text_buffer_get_iter_at_offset(b, &a, (int)std::min<size_t>(p.location, (size_t)count));
    gtk_text_buffer_get_iter_at_offset(b, &e,
                                       (int)std::min<size_t>(p.location + p.length, (size_t)count));
    const int line = gtk_text_iter_get_line(&a);
    const size_t column = (size_t)gtk_text_iter_get_line_index(&a);
    const size_t length = gtk_text_iter_get_line(&e) == line
                              ? (size_t)gtk_text_iter_get_line_index(&e) - column
                              : 0;
    return editor_->revealLine(line + 1, column, length);
}

void NavHistoryUi::tabDidSwitch() {
    Document* d = editor_->active();
    watch(d ? d->buffer() : nullptr);
    rec_.tabDidSwitch();
    rec_.noteCaret();
}

void NavHistoryUi::paletteOpened() {
    flushCaret();
    paletteFromSet_ = rec_.current(&paletteFrom_);
    rec_.beginQuiet();
}

void NavHistoryUi::paletteClosed() {
    // What the palette previewed and put back was this turn's: not a move.
    if (caretIdle_) g_source_remove(caretIdle_);
    caretIdle_ = 0;
    rec_.endQuiet();
}

void NavHistoryUi::paletteJump(const std::function<void()>& body) {
    if (paletteFromSet_) rec_.jumpFrom(paletteFrom_, body);
    else rec_.jump(body);
    paletteFromSet_ = false;
}

// ------------------------------------------------------------ the buffer on show

void NavHistoryUi::watch(GtkTextBuffer* b) {
    if (b == watched_) return;
    unwatch();
    if (!b) return;
    watched_ = b;
    g_object_add_weak_pointer(G_OBJECT(b), reinterpret_cast<gpointer*>(&watched_));
    g_signal_connect(b, "mark-set", G_CALLBACK(onMarkSet), this);
    g_signal_connect_after(b, "insert-text", G_CALLBACK(onInserted), this);
    g_signal_connect(b, "delete-range", G_CALLBACK(onDeleting), this);
    replacingAll_ = false;
}

void NavHistoryUi::unwatch() {
    if (!watched_) return;
    g_signal_handlers_disconnect_by_data(watched_, this);
    g_object_remove_weak_pointer(G_OBJECT(watched_), reinterpret_cast<gpointer*>(&watched_));
    watched_ = nullptr;
}

// The caret is looked at once a turn of the main loop, not at every mark
// GTK moves: undoing Replace All of 20,000 matches moves the caret 20,000
// times in one go. What comes first in the same turn (a tab switch, a
// jump, the palette opening) looks at it first (flushCaret).
void NavHistoryUi::onMarkSet(GtkTextBuffer* b, GtkTextIter*, GtkTextMark* mark, gpointer selfp) {
    if (mark != gtk_text_buffer_get_insert(b) && mark != gtk_text_buffer_get_selection_bound(b))
        return;
    auto* self = static_cast<NavHistoryUi*>(selfp);
    if (self->caretIdle_) return;
    self->caretIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer p) -> gboolean {
        auto* s = static_cast<NavHistoryUi*>(p);
        s->caretIdle_ = 0;
        s->rec_.caretMoved();
        return G_SOURCE_REMOVE;
    }, self, nullptr);
}

void NavHistoryUi::flushCaret() {
    if (!caretIdle_) return;
    g_source_remove(caretIdle_);
    caretIdle_ = 0;
    rec_.caretMoved();
}

// Edits move the places in the file with their text. Replacing the whole
// text (a reload from the disk) is another text coming in, which no place
// follows, as on the Mac.
void NavHistoryUi::onInserted(GtkTextBuffer* b, GtkTextIter* end, char* text, int len,
                              gpointer selfp) {
    auto* self = static_cast<NavHistoryUi*>(selfp);
    Document* d = self->editor_->active();
    const bool whole = self->replacingAll_;
    self->replacingAll_ = false;
    if (whole || !d || d->buffer() != b || d->path().empty()) return;
    const long chars = g_utf8_strlen(text, len);
    const long at = gtk_text_iter_get_offset(end) - chars;
    self->rec_.edited(d->path(), (size_t)std::max(0L, at), 0, (size_t)chars);
}

void NavHistoryUi::onDeleting(GtkTextBuffer* b, GtkTextIter* start, GtkTextIter* end,
                              gpointer selfp) {
    auto* self = static_cast<NavHistoryUi*>(selfp);
    Document* d = self->editor_->active();
    if (!d || d->buffer() != b || d->path().empty()) return;
    int a = gtk_text_iter_get_offset(start), e = gtk_text_iter_get_offset(end);
    if (a > e) std::swap(a, e);
    if (a == 0 && e == gtk_text_buffer_get_char_count(b) && e > 0) {
        self->replacingAll_ = true;
        return;
    }
    self->replacingAll_ = false;
    self->rec_.edited(d->path(), (size_t)a, (size_t)(e - a), 0);
}
