// BufferLines.cpp — see BufferLines.h.
#include "BufferLines.h"

#include <cstring>
#include <ctime>

namespace {

GQuark linesQuark() {
    static GQuark q = g_quark_from_static_string("minicode-buffer-lines");
    return q;
}

double gCpuMs = 0;

// CPU time of the work below, added to gCpuMs when it ends.
struct CpuSpan {
    double t0 = gutterThreadCpuMs();
    ~CpuSpan() { gCpuMs += gutterThreadCpuMs() - t0; }
};

}  // namespace

double gutterThreadCpuMs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

double gutterCpuMs() { return gCpuMs; }
void addGutterCpuMs(double ms) { gCpuMs += ms; }

BufferLines* BufferLines::attach(GtkTextBuffer* buffer) {
    if (BufferLines* had = of(buffer)) return had;
    BufferLines* lines = new BufferLines(buffer);
    // Freed when the buffer is finalized, after its handlers are gone.
    g_object_set_qdata_full(G_OBJECT(buffer), linesQuark(), lines,
                            [](gpointer p) { delete static_cast<BufferLines*>(p); });
    return lines;
}

BufferLines* BufferLines::of(GtkTextBuffer* buffer) {
    if (!buffer) return nullptr;
    return static_cast<BufferLines*>(g_object_get_qdata(G_OBJECT(buffer), linesQuark()));
}

BufferLines::BufferLines(GtkTextBuffer* buffer) {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buffer, &a, &b);
    if (!gtk_text_iter_equal(&a, &b)) {
        char* text = gtk_text_buffer_get_text(buffer, &a, &b, TRUE);
        index_.resetUtf8(text, strlen(text));
        g_free(text);
    }
    // Before the buffer's own handlers (both signals are RUN_LAST), while
    // the iterators still describe the old text.
    g_signal_connect(buffer, "insert-text", G_CALLBACK(onInsert), this);
    g_signal_connect(buffer, "delete-range", G_CALLBACK(onDelete), this);
}

void BufferLines::onInsert(GtkTextBuffer*, GtkTextIter* at, char* text, int len, gpointer p) {
    CpuSpan span;
    BufferLines* self = static_cast<BufferLines*>(p);
    const size_t pos = (size_t)gtk_text_iter_get_offset(at);
    gunichar before = 0;
    if (pos > 0) {
        GtkTextIter prev = *at;
        gtk_text_iter_backward_char(&prev);
        before = gtk_text_iter_get_char(&prev);
    }
    const gunichar after = gtk_text_iter_get_char(at);   // 0 at the end
    const size_t linesBefore = self->bars_.empty() ? 0 : self->index_.lineOf(pos);
    const size_t had = self->index_.length();
    self->index_.editUtf8(pos, 0, text, len < 0 ? strlen(text) : (size_t)len, before, after);
    if (!self->bars_.empty())
        self->shiftBars(pos, self->index_.length() - had, linesBefore);
}

void BufferLines::onDelete(GtkTextBuffer*, GtkTextIter* from, GtkTextIter* to, gpointer p) {
    CpuSpan span;
    BufferLines* self = static_cast<BufferLines*>(p);
    GtkTextIter a = *from, b = *to;
    gtk_text_iter_order(&a, &b);
    const size_t pos = (size_t)gtk_text_iter_get_offset(&a);
    const size_t end = (size_t)gtk_text_iter_get_offset(&b);
    if (end == pos) return;
    gunichar before = 0;
    if (pos > 0) {
        GtkTextIter prev = a;
        gtk_text_iter_backward_char(&prev);
        before = gtk_text_iter_get_char(&prev);
    }
    const gunichar after = gtk_text_iter_get_char(&b);
    // Everything going: a whole new text (set_text) has no bars until its
    // own diff, as on the Mac.
    if (pos == 0 && end >= self->index_.length()) self->bars_.clear();
    const size_t linesBefore = self->bars_.empty() ? 0 : self->index_.lineOf(pos);
    const size_t removedLines = self->bars_.empty() ? 0 : self->index_.lineOf(end) - linesBefore;
    self->index_.editUtf8(pos, end - pos, nullptr, 0, before, after);
    if (!self->bars_.empty()) {
        QuickDiff::shift(self->bars_, linesBefore, removedLines, 0);
        self->changed();
    }
}

void BufferLines::shiftBars(size_t pos, size_t newLen, size_t linesBefore) {
    const size_t added = index_.lineOf(pos + newLen) - linesBefore;
    if (added == 0) return;   // typing within a line moves no bar
    QuickDiff::shift(bars_, linesBefore, 0, added);
    changed();
}

void BufferLines::setChangeBars(std::vector<QuickDiff::Bar> bars) {
    if (bars == bars_) return;
    bars_ = std::move(bars);
    changed();
}

void BufferLines::setFolds(GutterFolds* folds) {
    folds_ = folds;
    changed();
}

void BufferLines::changed() {
    // A copy: a listener may stop listening.
    const auto listeners = listeners_;
    for (const auto& l : listeners) l.second();
}

int BufferLines::listen(std::function<void()> onChange) {
    listeners_.emplace_back(++nextListener_, std::move(onChange));
    return nextListener_;
}

void BufferLines::unlisten(int id) {
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->first == id) {
            listeners_.erase(it);
            return;
        }
}
