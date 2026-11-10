// SnippetSession.cpp — see SnippetSession.h.
#include "SnippetSession.h"

#include "Carets.h"
#include "CodeView.h"
#include "Editor.h"
#include "MultiEdit.h"
#include "Utf8Offsets.h"

#include <algorithm>

namespace {

size_t offsetOf(const GtkTextIter* it) { return (size_t)gtk_text_iter_get_offset(it); }

// The accent (#4EA1F7), as the Mac draws the stops.
GdkRGBA accent(float alpha) { return GdkRGBA{0x4E / 255.0f, 0xA1 / 255.0f, 0xF7 / 255.0f, alpha}; }

bool meets(const graphene_rect_t& r, const GdkRectangle& v) {
    return r.origin.x < v.x + v.width && r.origin.x + r.size.width > v.x &&
           r.origin.y < v.y + v.height && r.origin.y + r.size.height > v.y;
}

}  // namespace

// ---------------------------------------------------------------- life

SnippetSession::SnippetSession(Editor* editor) : editor_(editor), view_(editor->textView()) {
    pass_ = editor->codeView()->addPass(CodeView::Layer::BelowText, 6,
                                        [this](GtkSnapshot* s, const GdkRectangle& v) { draw(s, v); });
    g_signal_connect(view_, "notify::buffer", G_CALLBACK(onBuffer), this);
    watchBuffer();
}

// Runs while the window's widgets still exist (Editor's destructor).
SnippetSession::~SnippetSession() {
    if (checkId_) g_source_remove(checkId_);
    checkId_ = 0;
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        g_object_unref(buffer_);
        buffer_ = nullptr;
    }
    g_signal_handlers_disconnect_by_data(view_, this);
    if (CodeView* cv = editor_->codeView()) cv->removePass(pass_);
}

// The view shows another buffer (a tab switch, a Markdown page): the stops
// were in the old one.
void SnippetSession::watchBuffer() {
    GtkTextBuffer* now = gtk_text_view_get_buffer(view());
    if (now == buffer_) return;
    end();
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        g_object_unref(buffer_);
    }
    buffer_ = now;
    if (!buffer_) return;
    g_object_ref(buffer_);
    // Before the buffer changes (both signals are RUN_LAST), while the
    // iterators still name the text as it was.
    g_signal_connect(buffer_, "insert-text", G_CALLBACK(onInsert), this);
    g_signal_connect(buffer_, "delete-range", G_CALLBACK(onDelete), this);
    g_signal_connect(buffer_, "undo", G_CALLBACK(onUndo), this);
    g_signal_connect(buffer_, "redo", G_CALLBACK(onUndo), this);
    g_signal_connect_after(buffer_, "mark-set", G_CALLBACK(onMarkSet), this);
}

void SnippetSession::onBuffer(GObject*, GParamSpec*, gpointer self) {
    static_cast<SnippetSession*>(self)->watchBuffer();
}

void SnippetSession::start(const Snippet::Expansion& expansion, size_t base) {
    end();
    if (!expansion.hasStops() || !buffer_) return;
    size_t points = 0;
    const Snippet::Expansion e = Snippet::inCodePoints(expansion, &points);
    s_ = Snippet::Session(e, base, points);
    active_ = s_.active();
    if (!active_) return;
    selectCurrent();
    gtk_widget_queue_draw(view_);
}

void SnippetSession::end() {
    if (checkId_) g_source_remove(checkId_);
    checkId_ = 0;
    if (!active_) return;
    active_ = false;
    s_.end();
    gtk_widget_queue_draw(view_);
}

void SnippetSession::release(const std::vector<EditSpans::Span>& moved) {
    if (held_ > 0) held_--;
    if (held_ || !active_) return;
    // Last first, so every earlier start is still where it was.
    for (size_t k = moved.size(); k-- > 0;) {
        s_.edited(moved[k].start, moved[k].length, moved[k].newLength);
        if (!s_.active()) {
            active_ = false;
            break;
        }
    }
    gtk_widget_queue_draw(view_);
}

// ---------------------------------------------------------------- keys

bool SnippetSession::keyPressed(guint key, GdkModifierType mods) {
    if (!active_) return false;
    if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return false;
    const bool shift = mods & GDK_SHIFT_MASK;
    const bool tabKey = key == GDK_KEY_Tab || key == GDK_KEY_KP_Tab;
    const bool tab = tabKey && !shift;
    const bool backTab = key == GDK_KEY_ISO_Left_Tab || (tabKey && shift);
    const bool esc = key == GDK_KEY_Escape && !shift;
    if (!tab && !backTab && !esc) return false;
    // An input method composing keeps its keys.
    if (editor_->composing()) return false;
    if (!editor_->isSource() || !gtk_text_view_get_editable(view())) {
        end();
        return false;
    }
    if (esc) {
        end();
        return true;
    }
    writeTransforms();
    if (!active_) return true;
    if (backTab) {
        if (s_.prev()) selectCurrent();
        return true;
    }
    if (!s_.next()) {
        // Nowhere left (the final stop was typed over): the snippet's end.
        const size_t length = (size_t)gtk_text_buffer_get_char_count(buffer_);
        const size_t at = std::min(s_.span().end, length);
        end();
        selectRanges({{at, at}});
        return true;
    }
    if (s_.atFinal()) {
        // $0 (or ${0:text}, selected): the session is done.
        const std::vector<Snippet::Range> ranges = s_.caretRanges();
        end();
        if (!ranges.empty()) selectRanges({ranges.front()});
        return true;
    }
    selectCurrent();
    return true;
}

// Copies of the stop being left that a transform makes, written from the
// stop's text as it is now, as one undo step.
void SnippetSession::writeTransforms() {
    const std::vector<Snippet::Range> carets = s_.caretRanges();
    if (carets.empty() || !buffer_) return;
    const size_t length = (size_t)gtk_text_buffer_get_char_count(buffer_);
    const Snippet::Range first = carets.front();
    if (first.end > length) return;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)first.start);
    gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)first.end);
    char* raw = gtk_text_buffer_get_slice(buffer_, &a, &b, TRUE);
    const std::u16string value = utf16::fromUtf8(raw ? raw : "");
    g_free(raw);
    const std::vector<Snippet::Session::Replacement> reps = s_.transformsFor(value);
    if (reps.empty()) return;
    std::vector<MultiEdit::Change> changes;
    for (const auto& r : reps) {
        if (r.start + r.length > length) return;
        changes.push_back({r.start, r.length, utf16::toUtf8(r.text)});
    }
    applying_++;
    MultiEdit::apply(buffer_, changes, false);
    applying_--;
}

// The ranges selected: one as the buffer's selection, several as carets.
void SnippetSession::selectRanges(const std::vector<Snippet::Range>& ranges) {
    Carets* carets = editor_->carets();
    if (!carets || !buffer_ || ranges.empty()) return;
    const size_t length = (size_t)gtk_text_buffer_get_char_count(buffer_);
    std::vector<MultiCaret::Caret> list;
    for (const Snippet::Range& r : ranges)
        if (r.end <= length) list.push_back({r.start, r.end});
    if (list.empty()) return;
    applying_++;
    carets->set(list);
    applying_--;
    gtk_text_view_scroll_mark_onscreen(view(), gtk_text_buffer_get_insert(buffer_));
}

void SnippetSession::selectCurrent() {
    const std::vector<Snippet::Range> ranges = s_.caretRanges();
    if (ranges.empty()) {
        end();
        return;
    }
    gtk_widget_queue_draw(view_);
    selectRanges(ranges);
    const Snippet::Stop& stop = s_.currentStop();
    if (!stop.choices.empty() && onChoices) {
        std::vector<std::string> choices;
        for (const std::u16string& c : stop.choices) choices.push_back(utf16::toUtf8(c));
        onChoices(choices, ranges.front().start, ranges.front().end);
    }
}

// ---------------------------------------------------------------- following the text

void SnippetSession::edited(size_t start, size_t removed, size_t inserted) {
    if (!active_ || held_) return;
    s_.edited(start, removed, inserted);
    if (!s_.active()) active_ = false;
    gtk_widget_queue_draw(view_);
}

void SnippetSession::onInsert(GtkTextBuffer*, GtkTextIter* at, char* text, int len, gpointer p) {
    auto* self = static_cast<SnippetSession*>(p);
    if (!self->active_ || len <= 0) return;
    self->edited(offsetOf(at), 0, (size_t)g_utf8_strlen(text, len));
}

void SnippetSession::onDelete(GtkTextBuffer*, GtkTextIter* a, GtkTextIter* b, gpointer p) {
    auto* self = static_cast<SnippetSession*>(p);
    if (!self->active_) return;
    size_t from = offsetOf(a), to = offsetOf(b);
    if (to < from) std::swap(from, to);
    if (to > from) self->edited(from, to - from, 0);
}

// An undo or redo puts back text the stops know nothing of.
void SnippetSession::onUndo(GtkTextBuffer*, gpointer p) {
    auto* self = static_cast<SnippetSession*>(p);
    if (!self->held_) self->end();
}

void SnippetSession::onMarkSet(GtkTextBuffer* b, GtkTextIter*, GtkTextMark* mark, gpointer p) {
    auto* self = static_cast<SnippetSession*>(p);
    if (!self->active_ || self->applying_ || self->held_) return;
    if (mark == gtk_text_buffer_get_insert(b) || mark == gtk_text_buffer_get_selection_bound(b))
        self->scheduleCheck();
}

// A selection that leaves the current stop ends the session: looked at in
// an idle, once the carets have settled (several carets set the buffer's
// selection more than once while they type).
void SnippetSession::scheduleCheck() {
    if (checkId_) return;
    checkId_ = g_idle_add([](gpointer p) -> gboolean {
        auto* self = static_cast<SnippetSession*>(p);
        self->checkId_ = 0;
        self->checkSelection();
        return G_SOURCE_REMOVE;
    }, this);
}

void SnippetSession::checkSelection() {
    Carets* carets = editor_->carets();
    if (!active_ || held_ || !carets) return;
    std::vector<Snippet::Range> sel;
    for (const MultiCaret::Caret& c : carets->current())
        sel.push_back({std::min(c.anchor, c.head), std::max(c.anchor, c.head)});
    if (!s_.holds(sel)) end();
}

// ---------------------------------------------------------------- drawing

void SnippetSession::draw(GtkSnapshot* s, const GdkRectangle& visible) {
    drawnBoxes_ = 0;
    if (!active_ || !buffer_ || gtk_text_view_get_buffer(view()) != buffer_) return;
    const size_t length = (size_t)gtk_text_buffer_get_char_count(buffer_);
    const std::vector<Snippet::Stop>& stops = s_.stops();
    for (size_t k = 0; k < stops.size(); k++) {
        const Snippet::Stop& stop = stops[k];
        const bool current = k == s_.current();
        const bool final = stop.index == 0;
        for (size_t r = 0; r < stop.ranges.size(); r++) {
            if (s_.dead(k, r)) continue;
            const Snippet::Range& rg = stop.ranges[r];
            if (rg.end > length) continue;
            if (rg.start == rg.end)
                drawBar(s, visible, rg.start, accent(final ? 0.35f : (current ? 0.8f : 0.5f)));
            else
                drawRange(s, visible, rg.start, rg.end, accent(current ? 0.14f : 0.0f),
                          accent(current ? 0.55f : 0.3f));
        }
    }
}

// A box on each line the range covers, from the x of its first character
// on that line to the x of the one after its last.
void SnippetSession::drawRange(GtkSnapshot* s, const GdkRectangle& visible, size_t a, size_t b,
                               const GdkRGBA& fill, const GdkRGBA& line) {
    GtkTextIter it, stop;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)a);
    gtk_text_buffer_get_iter_at_offset(buffer_, &stop, (int)b);
    for (int guard = 0; gtk_text_iter_compare(&it, &stop) < 0 && guard < 500; guard++) {
        GtkTextIter segEnd = it;
        if (!gtk_text_iter_ends_line(&segEnd)) gtk_text_iter_forward_to_line_end(&segEnd);
        if (gtk_text_iter_compare(&segEnd, &stop) > 0) segEnd = stop;
        GdkRectangle r0, r1;
        gtk_text_view_get_iter_location(view(), &it, &r0);
        gtk_text_view_get_iter_location(view(), &segEnd, &r1);
        // A wrapped line: to the right edge on each row but the last.
        int rows = r0.height > 0 ? std::max(1, (r1.y - r0.y) / r0.height + 1) : 1;
        for (int row = 0; row < rows; row++) {
            const int y = r0.y + row * r0.height;
            const int x0 = row == 0 ? r0.x : 0;
            int x1 = row == rows - 1 ? r1.x : visible.x + visible.width;
            if (x1 <= x0) x1 = x0 + 4;   // an empty line inside the range
            graphene_rect_t box = GRAPHENE_RECT_INIT((float)x0 + 0.5f, (float)y + 0.5f,
                                                     (float)(x1 - x0) - 1.0f,
                                                     std::max(4.0f, (float)r0.height - 2.0f));
            if (!meets(box, visible)) continue;
            if (fill.alpha > 0) gtk_snapshot_append_color(s, &fill, &box);
            GskRoundedRect rr;
            gsk_rounded_rect_init_from_rect(&rr, &box, 2);
            const float widths[4] = {1, 1, 1, 1};
            const GdkRGBA colors[4] = {line, line, line, line};
            gtk_snapshot_append_border(s, &rr, widths, colors);
            drawnBoxes_++;
        }
        it = segEnd;
        if (gtk_text_iter_compare(&it, &stop) < 0 && !gtk_text_iter_forward_line(&it)) break;
    }
}

// An empty stop's place: a thin bar at the caret's x on its line.
void SnippetSession::drawBar(GtkSnapshot* s, const GdkRectangle& visible, size_t at,
                             const GdkRGBA& color) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buffer_, &it, (int)at);
    GdkRectangle r;
    gtk_text_view_get_iter_location(view(), &it, &r);
    graphene_rect_t bar = GRAPHENE_RECT_INIT((float)r.x - 0.5f, (float)r.y, 1.5f, (float)r.height);
    if (!meets(bar, visible)) return;
    gtk_snapshot_append_color(s, &color, &bar);
    drawnBoxes_++;
}

// ---------------------------------------------------------------- for the checks

int SnippetSession::currentStopIndex() const { return active_ ? s_.currentStop().index : -1; }

std::vector<std::pair<size_t, size_t>> SnippetSession::currentRanges() const {
    std::vector<std::pair<size_t, size_t>> out;
    if (!active_) return out;
    for (const Snippet::Range& r : s_.caretRanges()) out.push_back({r.start, r.end});
    return out;
}

std::vector<std::pair<size_t, size_t>> SnippetSession::allRanges() const {
    std::vector<std::pair<size_t, size_t>> out;
    if (!active_) return out;
    const std::vector<Snippet::Stop>& stops = s_.stops();
    for (size_t r = 0; r < stops[s_.current()].ranges.size(); r++)
        if (!s_.dead(s_.current(), r))
            out.push_back({stops[s_.current()].ranges[r].start, stops[s_.current()].ranges[r].end});
    for (size_t k = 0; k < stops.size(); k++) {
        if (k == s_.current()) continue;
        for (size_t r = 0; r < stops[k].ranges.size(); r++)
            if (!s_.dead(k, r)) out.push_back({stops[k].ranges[r].start, stops[k].ranges[r].end});
    }
    return out;
}
