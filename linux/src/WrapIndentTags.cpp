// WrapIndentTags.cpp — see WrapIndentTags.h.
#include "WrapIndentTags.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace {

// Pango's default tab stops, every 8 spaces (the editor sets none).
constexpr int kTabColumns = 8;
// Indent's one level more, in spaces (VS Code's default tab size, as the
// Mac's WrapIndentContainer).
constexpr int kLevelColumns = 4;
// Leading whitespace past this many columns is wider than half of any
// pane: no tag, which is the fall back.
constexpr int kMaxColumns = 512;
// Retag at once up to this many lines; lines per idle step, and the idle
// retag's budget per main loop turn.
constexpr int kSyncLines = 1000;
constexpr int kChunkLines = 64;
constexpr gint64 kSliceMicros = 5000;

const char kStateKey[] = "mutible-wrap-indent";
const char kTagKey[] = "mutible-wrap-columns";

double threadCpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct TableState {
    WrapIndent::Mode mode = WrapIndent::Mode::Same;
    double space = 0, room = 0;
    std::vector<GtkTextTag*> tags;   // by columns; null where none was made
};

TableState& stateOf(GtkTextTagTable* table) {
    auto* s = static_cast<TableState*>(g_object_get_data(G_OBJECT(table), kStateKey));
    if (!s) {
        s = new TableState();
        g_object_set_data_full(G_OBJECT(table), kStateKey, s,
                               [](gpointer p) { delete static_cast<TableState*>(p); });
    }
    return *s;
}

int indentFor(const TableState& s, int columns) {
    if (s.space <= 0 || columns <= 0) return 0;
    const double room = s.room > 0 ? s.room : 1e9;
    return (int)std::lround(WrapIndent::continuationIndent(columns * s.space, s.mode,
                                                           kLevelColumns * s.space, room));
}

GtkTextTag* tagFor(GtkTextTagTable* table, int columns) {
    TableState& s = stateOf(table);
    if ((size_t)columns >= s.tags.size()) s.tags.resize((size_t)columns + 1, nullptr);
    GtkTextTag*& t = s.tags[(size_t)columns];
    if (!t) {
        const std::string name = "mutible-wrap-" + std::to_string(columns);
        t = gtk_text_tag_table_lookup(table, name.c_str());
        if (!t) {
            // A hanging indent: the first row whole, the rest set in.
            t = gtk_text_tag_new(name.c_str());
            g_object_set(t, "indent", -indentFor(s, columns), NULL);
            gtk_text_tag_table_add(table, t);
            g_object_unref(t);   // the table holds it
        }
        g_object_set_data(G_OBJECT(t), kTagKey, GINT_TO_POINTER(columns));
    }
    return t;
}

}  // namespace

// ---------------------------------------------------------------- the table

bool WrapIndentTable::configure(GtkTextTagTable* table, WrapIndent::Mode mode, double space,
                                double room) {
    TableState& s = stateOf(table);
    s.mode = mode;
    s.space = space;
    s.room = room;
    bool changed = false;
    for (size_t c = 0; c < s.tags.size(); c++) {
        if (!s.tags[c]) continue;
        int now = 0;
        g_object_get(s.tags[c], "indent", &now, NULL);
        const int want = -indentFor(s, (int)c);
        if (now == want) continue;
        g_object_set(s.tags[c], "indent", want, NULL);   // the layouts of its lines go
        changed = true;
    }
    return changed;
}

WrapIndent::Mode WrapIndentTable::mode(GtkTextTagTable* table) { return stateOf(table).mode; }

bool WrapIndentTable::isTag(GtkTextTag* tag) {
    return tag && g_object_get_data(G_OBJECT(tag), kTagKey) != nullptr;
}

int WrapIndentTable::indentOf(GtkTextTagTable* table, int columns) {
    return indentFor(stateOf(table), columns);
}

// ---------------------------------------------------------------- a buffer

WrapIndentTags::WrapIndentTags(GtkTextBuffer* buffer,
                               std::function<void(size_t&, size_t&)> visibleLines)
    : buffer_(buffer), table_(gtk_text_buffer_get_tag_table(buffer)),
      visibleLines_(std::move(visibleLines)) {
    g_signal_connect_after(buffer_, "insert-text", G_CALLBACK(onInsertAfter), this);
    g_signal_connect(buffer_, "delete-range", G_CALLBACK(onDeleteBefore), this);
    g_signal_connect_after(buffer_, "delete-range", G_CALLBACK(onDeleteAfter), this);
    g_signal_connect(buffer_, "apply-tag", G_CALLBACK(onApplyTag), this);
}

WrapIndentTags::~WrapIndentTags() {
    stopIdle();
    g_signal_handlers_disconnect_by_data(buffer_, this);
}

void WrapIndentTags::setOn(bool on) {
    if (on == on_) return;
    on_ = on;
    stopIdle();
    pendFirst_ = pendEnd_ = 0;
    if (on) retag(0, gtk_text_buffer_get_line_count(buffer_));
}

void WrapIndentTags::stopIdle() {
    if (idle_) g_source_remove(idle_);
    idle_ = 0;
}

// The one wrap tag a line's first character should have (by the core's
// WrapIndent::leadingColumns), and no other.
void WrapIndentTags::retagLine(int line) {
    GtkTextIter start;
    if (!gtk_text_buffer_get_iter_at_line(buffer_, &start, line)) return;
    char16_t lead[kMaxColumns + 1];
    size_t n = 0;
    GtkTextIter it = start;
    while (n <= (size_t)kMaxColumns && !gtk_text_iter_ends_line(&it)) {
        const gunichar c = gtk_text_iter_get_char(&it);
        if (c != ' ' && c != '\t') break;
        lead[n++] = (char16_t)c;
        gtk_text_iter_forward_char(&it);
    }
    int columns = 0;
    if (n > 0 && n <= (size_t)kMaxColumns && !gtk_text_iter_ends_line(&it))
        columns = WrapIndent::leadingColumns(lead, n, kTabColumns);
    GtkTextTag* want = columns > 0 && columns <= kMaxColumns ? tagFor(table_, columns) : nullptr;
    GtkTextIter next = start;
    if (!gtk_text_iter_forward_line(&next)) gtk_text_buffer_get_end_iter(buffer_, &next);
    bool have = false;
    applying_++;
    GSList* tags = gtk_text_iter_get_tags(&start);
    for (GSList* l = tags; l; l = l->next) {
        GtkTextTag* t = GTK_TEXT_TAG(l->data);
        if (!WrapIndentTable::isTag(t)) continue;
        if (t == want) have = true;
        else gtk_text_buffer_remove_tag(buffer_, t, &start, &next);
    }
    g_slist_free(tags);
    // The whole line, its break included, so neighbours alike join up.
    if (want && !have) gtk_text_buffer_apply_tag(buffer_, want, &start, &next);
    applying_--;
    retagged_++;
}

void WrapIndentTags::retag(int first, int end) {
    if (end <= first) return;
    if (end - first <= kSyncLines) {
        for (int l = first; l < end; l++) retagLine(l);
        return;
    }
    size_t vf = 0, ve = 0;
    if (visibleLines_) visibleLines_(vf, ve);
    const int a = std::max(first, (int)std::min<size_t>(vf, INT_MAX));
    const int b = std::min(end, (int)std::min<size_t>(ve, INT_MAX));
    for (int l = a; l < b && l - a < kSyncLines; l++) retagLine(l);
    schedule(first, end);
}

void WrapIndentTags::schedule(int first, int end) {
    if (pendFirst_ >= pendEnd_) {
        pendFirst_ = first;
        pendEnd_ = end;
    } else {
        pendFirst_ = std::min(pendFirst_, first);
        pendEnd_ = std::max(pendEnd_, end);
    }
    if (!idle_) idle_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, onIdle, this, nullptr);
}

void WrapIndentTags::noteLines(int line, int added) {
    if (pendFirst_ >= pendEnd_) return;
    if (line < pendFirst_) pendFirst_ = line;
    if (line < pendEnd_) pendEnd_ = std::max(pendEnd_ + added, line + 1);
}

gboolean WrapIndentTags::onIdle(gpointer p) {
    WrapIndentTags* self = static_cast<WrapIndentTags*>(p);
    const gint64 t0 = g_get_monotonic_time();
    const int lines = gtk_text_buffer_get_line_count(self->buffer_);
    self->pendEnd_ = std::min(self->pendEnd_, lines);
    while (self->pendFirst_ < self->pendEnd_) {
        const int stop = std::min(self->pendEnd_, self->pendFirst_ + kChunkLines);
        for (int l = self->pendFirst_; l < stop; l++) self->retagLine(l);
        self->pendFirst_ = stop;
        if (g_get_monotonic_time() - t0 >= kSliceMicros) break;
    }
    if (self->pendFirst_ < self->pendEnd_) return G_SOURCE_CONTINUE;
    self->pendFirst_ = self->pendEnd_ = 0;
    self->idle_ = 0;
    return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------- edits

void WrapIndentTags::onInsertAfter(GtkTextBuffer*, GtkTextIter* end, char* text, int len,
                                   gpointer p) {
    WrapIndentTags* self = static_cast<WrapIndentTags*>(p);
    if (!self->on_) return;
    const double t0 = threadCpuMs();
    const int last = gtk_text_iter_get_line(end);
    GtkTextIter start = *end;
    gtk_text_iter_backward_chars(&start, (int)g_utf8_strlen(text, len));
    const int first = gtk_text_iter_get_line(&start);
    self->noteLines(first, last - first);
    self->retag(first, last + 1);
    self->lastEditMs_ = threadCpuMs() - t0;
}

void WrapIndentTags::onDeleteBefore(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer p) {
    WrapIndentTags* self = static_cast<WrapIndentTags*>(p);
    self->deleting_ = std::abs(gtk_text_iter_get_line(b) - gtk_text_iter_get_line(a));
}

void WrapIndentTags::onDeleteAfter(GtkTextBuffer*, GtkTextIter* a, GtkTextIter*, gpointer p) {
    WrapIndentTags* self = static_cast<WrapIndentTags*>(p);
    const int removed = self->deleting_;
    self->deleting_ = 0;
    if (!self->on_) return;
    const double t0 = threadCpuMs();
    const int line = gtk_text_iter_get_line(a);
    self->noteLines(line, -removed);
    self->retagLine(line);
    self->lastEditMs_ = threadCpuMs() - t0;
}

// A paste from another buffer brings its tags along: never these.
void WrapIndentTags::onApplyTag(GtkTextBuffer* b, GtkTextTag* tag, GtkTextIter*, GtkTextIter*,
                                gpointer p) {
    WrapIndentTags* self = static_cast<WrapIndentTags*>(p);
    if (!self->applying_ && WrapIndentTable::isTag(tag)) g_signal_stop_emission_by_name(b, "apply-tag");
}
