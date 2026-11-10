// Gutter.cpp — see Gutter.h. The Mac's src/Gutter.mm is the model, drawing
// for drawing.
#include "Gutter.h"

#include "BufferLines.h"
#include "QuickDiff.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------- the widget
//
// Its measure and snapshot are the Gutter's; the view owns it (as its left
// gutter), and the Gutter clears `owner` before it goes.

G_DECLARE_FINAL_TYPE(MinicodeGutter, minicode_gutter, MINICODE, GUTTER, GtkWidget)

struct _MinicodeGutter {
    GtkWidget parent_instance;
    Gutter*   owner;
};

G_DEFINE_FINAL_TYPE(MinicodeGutter, minicode_gutter, GTK_TYPE_WIDGET)

static void minicode_gutter_measure(GtkWidget* w, GtkOrientation o, int, int* min, int* nat,
                                    int* minBase, int* natBase) {
    MinicodeGutter* self = MINICODE_GUTTER(w);
    *min = *nat = 0;
    *minBase = *natBase = -1;
    if (self->owner) self->owner->measure(o, min, nat);
}

static void minicode_gutter_snapshot(GtkWidget* w, GtkSnapshot* s) {
    MinicodeGutter* self = MINICODE_GUTTER(w);
    if (self->owner) self->owner->snapshot(s, gtk_widget_get_width(w), gtk_widget_get_height(w));
}

static void minicode_gutter_class_init(MinicodeGutterClass* klass) {
    GtkWidgetClass* wc = GTK_WIDGET_CLASS(klass);
    wc->measure = minicode_gutter_measure;
    wc->snapshot = minicode_gutter_snapshot;
    gtk_widget_class_set_css_name(wc, "minicode-gutter");
}

static void minicode_gutter_init(MinicodeGutter* self) { self->owner = nullptr; }

namespace {

// CPU time of the work below, for the checks' bound (BufferLines.h).
struct CpuSpan {
    double t0 = gutterThreadCpuMs();
    ~CpuSpan() { addGutterCpuMs(gutterThreadCpuMs() - t0); }
};

GdkRGBA toGdk(const Rgba& c) {
    return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
}

GdkRGBA hexColor(uint32_t rgb) {
    return GdkRGBA{((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                   (rgb & 0xFF) / 255.0f, 1};
}

size_t offsetOf(const GtkTextIter* it) { return (size_t)gtk_text_iter_get_offset(it); }

}  // namespace

// ---------------------------------------------------------------- life

Gutter::Gutter(GtkTextView* view) : view_(view) {
    widget_ = GTK_WIDGET(g_object_new(minicode_gutter_get_type(), nullptr));
    MINICODE_GUTTER(widget_)->owner = this;
    gtk_widget_add_css_class(widget_, "minicode-gutter");
    gtk_widget_set_cursor_from_name(widget_, "default");
    diagTags_ = {{"lsp-error", 1}, {"lsp-warning", 2}};

    // Click, drag and Shift+click select lines. Claimed at once, so the
    // text view's own selection gestures never see a press here.
    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(onDragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect_swapped(drag, "drag-end", G_CALLBACK(+[](Gutter* self) {
        self->dragging_ = false;
    }), this);
    gtk_widget_add_controller(widget_, GTK_EVENT_CONTROLLER(drag));
    // The pointer over the gutter shows the fold lane's open regions.
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect_swapped(motion, "enter", G_CALLBACK(+[](Gutter* self) {
        self->hovering_ = true;
        if (self->lines() && self->lines()->folds()) self->queueDraw();
    }), this);
    g_signal_connect_swapped(motion, "leave", G_CALLBACK(+[](Gutter* self) {
        self->hovering_ = false;
        if (self->lines() && self->lines()->folds()) self->queueDraw();
    }), this);
    gtk_widget_add_controller(widget_, motion);

    gtk_text_view_set_gutter(view_, GTK_TEXT_WINDOW_LEFT, widget_);

    // The view's buffer (another tab), whether it is editable (the source,
    // or a preview or message), and its scrolling.
    g_signal_connect_swapped(view_, "notify::buffer", G_CALLBACK(+[](Gutter* self) {
        self->attachBuffer(gtk_text_view_get_buffer(self->view_));
    }), this);
    g_signal_connect_swapped(view_, "notify::editable", G_CALLBACK(+[](Gutter* self) {
        self->updateVisibility();
    }), this);
    g_signal_connect_swapped(view_, "notify::vadjustment", G_CALLBACK(+[](Gutter* self) {
        self->attachAdjustment(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(self->view_)));
    }), this);
    attachBuffer(gtk_text_view_get_buffer(view_));
    attachAdjustment(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view_)));
}

// From the view's dispose: only the connections go, nothing is laid out.
Gutter::~Gutter() {
    if (resizeIdle_) g_source_remove(resizeIdle_);
    g_signal_handlers_disconnect_by_data(view_, this);
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        if (listening_) listening_->unlisten(listenId_);
        g_object_unref(buffer_);
    }
    attachAdjustment(nullptr);
    MINICODE_GUTTER(widget_)->owner = nullptr;
}

BufferLines* Gutter::lines() const { return BufferLines::of(buffer_); }

void Gutter::attachBuffer(GtkTextBuffer* b) {
    if (b == buffer_) return;
    if (buffer_) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
        if (listening_) listening_->unlisten(listenId_);
        listening_ = nullptr;
        g_object_unref(buffer_);
    }
    buffer_ = b;
    if (buffer_) {
        g_object_ref(buffer_);
        // By "changed" the index has the edit (BufferLines follows
        // insert-text and delete-range): the width, the caret's line, and a
        // draw.
        g_signal_connect_swapped(buffer_, "changed", G_CALLBACK(+[](Gutter* self) {
            CpuSpan span;
            self->updateWidth();
            self->caretLine_ = self->caretLine();
            self->queueDraw();
        }), this);
        g_signal_connect_after(buffer_, "mark-set", G_CALLBACK(+[](GtkTextBuffer* buf,
                                                                    GtkTextIter*, GtkTextMark* m,
                                                                    gpointer p) {
            Gutter* self = static_cast<Gutter*>(p);
            if (m != gtk_text_buffer_get_insert(buf)) return;
            CpuSpan span;
            const size_t line = self->caretLine();
            // The active color and the relative numbers follow the caret's line.
            if (line != self->caretLine_) {
                self->caretLine_ = line;
                self->queueDraw();
            }
        }), this);
        auto onTag = +[](GtkTextBuffer*, GtkTextTag* tag, GtkTextIter*, GtkTextIter*, gpointer p) {
            Gutter* self = static_cast<Gutter*>(p);
            if (self->isDiagnosticTag(tag)) self->queueDraw();
        };
        g_signal_connect_after(buffer_, "apply-tag", G_CALLBACK(onTag), this);
        g_signal_connect_after(buffer_, "remove-tag", G_CALLBACK(onTag), this);
        if ((listening_ = lines()))
            listenId_ = listening_->listen([this] { queueDraw(); });
    }
    caretLine_ = caretLine();
    updateVisibility();
    updateWidth();
    queueDraw();
}

void Gutter::attachAdjustment(GtkAdjustment* a) {
    if (a == vadj_) return;
    if (vadj_) {
        g_signal_handlers_disconnect_by_data(vadj_, this);
        g_object_unref(vadj_);
    }
    vadj_ = a;
    if (!vadj_) return;
    g_object_ref(vadj_);
    // A scroll moves the gutter's offset in its GtkTextViewChild, which
    // reallocates it but does not draw it again; the view's reallocation
    // (CodeView::allocated) and these do.
    g_signal_connect_swapped(vadj_, "value-changed", G_CALLBACK(+[](Gutter* self) {
        self->queueDraw();
    }), this);
    g_signal_connect_swapped(vadj_, "changed", G_CALLBACK(+[](Gutter* self) {
        self->queueDraw();
    }), this);
}

void Gutter::applySettings(const Settings& s) {
    const Settings::LineNumbers m = s.lineNumbers();
    setMode(m == Settings::LineNumbers::Off      ? Mode::Off
            : m == Settings::LineNumbers::Relative ? Mode::Relative
                                                   : Mode::On);
    setColors(toGdk(s.gutterBackground()), toGdk(s.lineNumberColor()),
              toGdk(s.lineNumberActiveColor()));
    // The view's font may have changed with the settings.
    gtk_widget_queue_resize(widget_);
}

void Gutter::setMode(Mode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    updateVisibility();
    queueDraw();
}

void Gutter::setColors(const GdkRGBA& background, const GdkRGBA& number, const GdkRGBA& active) {
    background_ = background;
    number_ = number;
    active_ = active;
    queueDraw();
}

void Gutter::setChangeLane(bool on) {
    if (on == changeLane_) return;
    changeLane_ = on;
    gtk_widget_queue_resize(widget_);
    queueDraw();
}

void Gutter::setDiagnosticTags(std::vector<std::pair<std::string, int>> tags) {
    diagTags_ = std::move(tags);
    diagTagPtrs_.clear();
    queueDraw();
}

void Gutter::updateVisibility() {
    const bool want = mode_ != Mode::Off && buffer_ && lines() &&
                      gtk_text_view_get_editable(view_);
    if (want != (bool)gtk_widget_get_visible(widget_)) gtk_widget_set_visible(widget_, want);
}

void Gutter::queueDraw() {
    if (gtk_widget_get_visible(widget_)) gtk_widget_queue_draw(widget_);
}

// ---------------------------------------------------------------- size

int Gutter::digitsFor(size_t lineCount) const {
    int d = 1;
    for (size_t n = lineCount; n >= 10; n /= 10) d++;
    return std::max(d, kMinDigits);
}

// The numbers' font: the view's, in the monospace family the source shows
// in. Set explicitly rather than taken from the view's style as it is,
// which lags behind gtk_text_view_set_monospace until the next frame: a
// gutter measured beside the welcome text's font came out two pixels narrow.
PangoLayout* Gutter::numberLayout(const char* text) const {
    PangoLayout* layout = gtk_widget_create_pango_layout(GTK_WIDGET(view_), text);
    const PangoFontDescription* base =
        pango_context_get_font_description(gtk_widget_get_pango_context(GTK_WIDGET(view_)));
    PangoFontDescription* fd = base ? pango_font_description_copy(base)
                                    : pango_font_description_new();
    pango_font_description_set_family(fd, "monospace");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);
    return layout;
}

double Gutter::digitWidth() const {
    PangoLayout* layout = numberLayout("8");
    PangoRectangle logical;
    pango_layout_get_extents(layout, nullptr, &logical);
    g_object_unref(layout);
    return (double)logical.width / PANGO_SCALE;
}

double Gutter::laneWidth() const {
    return kDotLane + (changeLane_ ? kChangeLane : 0) + kFoldLane;
}

double Gutter::numbersRight() const { return gtk_widget_get_width(widget_) - kFoldLane; }

// Only when the digit count changes: the view lays its text out again
// beside a gutter of another width.
void Gutter::updateWidth() {
    BufferLines* l = lines();
    const int d = digitsFor(l ? l->lineCount() : 1);
    if (d == digits_) return;
    digits_ = d;
    gtk_widget_queue_resize(widget_);
}

guint Gutter::fontHash() const {
    PangoContext* c = gtk_widget_get_pango_context(GTK_WIDGET(view_));
    const PangoFontDescription* fd = pango_context_get_font_description(c);
    return fd ? pango_font_description_hash(fd) : 0;
}

int Gutter::wantedWidth() const {
    return (int)std::ceil(laneWidth() + digits_ * digitWidth());
}

void Gutter::measure(GtkOrientation o, int* min, int* nat) {
    if (o != GTK_ORIENTATION_HORIZONTAL) return;
    measuredFont_ = fontHash();
    *min = *nat = wantedWidth();
}

// The view's font changed since the gutter was measured (its size, from the
// stylesheet): measure again, after the frame.
void Gutter::checkFont() {
    if (resizeIdle_ || fontHash() == measuredFont_) return;
    resizeIdle_ = g_idle_add([](gpointer p) -> gboolean {
        Gutter* self = static_cast<Gutter*>(p);
        self->resizeIdle_ = 0;
        gtk_widget_queue_resize(self->widget_);
        return G_SOURCE_REMOVE;
    }, this);
}

// ---------------------------------------------------------------- lines

size_t Gutter::caretLine() const {
    BufferLines* l = lines();
    if (!l) return 0;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buffer_, &it, gtk_text_buffer_get_insert(buffer_));
    return l->lineOf(offsetOf(&it));
}

double Gutter::toGutterY(int bufferY) const {
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(view_, GTK_TEXT_WINDOW_LEFT, 0, bufferY, &wx, &wy);
    return wy;
}

size_t Gutter::lineAtY(double y) const {
    BufferLines* l = lines();
    if (!l) return 0;
    int bx = 0, by = 0;
    gtk_text_view_window_to_buffer_coords(view_, GTK_TEXT_WINDOW_LEFT, 0, (int)std::floor(y),
                                          &bx, &by);
    // The display line at that height: its first character's logical line.
    GtkTextIter it;
    int top = 0;
    gtk_text_view_get_line_at_y(view_, &it, by, &top);
    for (;;) {
        GtkTextIter next = it;
        if (!gtk_text_view_forward_display_line(view_, &next) &&
            !gtk_text_view_starts_display_line(view_, &next))
            break;
        if (gtk_text_iter_equal(&next, &it)) break;
        GdkRectangle r;
        gtk_text_view_get_iter_location(view_, &next, &r);
        if (r.y > by) break;
        it = next;
    }
    return l->lineOf(offsetOf(&it));
}

bool Gutter::linesYRange(size_t first, size_t last, double* y, double* height) const {
    BufferLines* l = lines();
    if (!l || first >= l->lineCount()) return false;
    last = std::min(last, l->lineCount() - 1);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)l->lineStart(first));
    gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)l->lineStart(last));
    int ay = 0, ah = 0, by = 0, bh = 0;
    gtk_text_view_get_line_yrange(view_, &a, &ay, &ah);
    gtk_text_view_get_line_yrange(view_, &b, &by, &bh);
    if (by + bh <= ay) return false;
    *y = toGutterY(ay);
    *height = by + bh - ay;
    return true;
}

// ---------------------------------------------------------------- dots

// Asked on every tag applied or removed (the highlighter's thousands too),
// so by pointer: the tags are looked up by name once they exist.
bool Gutter::isDiagnosticTag(GtkTextTag* tag) const {
    if (!buffer_) return false;
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    if (diagTagPtrs_.size() != diagTags_.size() || diagTagTable_ != table) {
        diagTagPtrs_.assign(diagTags_.size(), nullptr);
        diagTagTable_ = table;
    }
    for (size_t i = 0; i < diagTags_.size(); i++) {
        if (!diagTagPtrs_[i])
            diagTagPtrs_[i] = gtk_text_tag_table_lookup(table, diagTags_[i].first.c_str());
        if (diagTagPtrs_[i] == tag) return true;
    }
    return false;
}

// The worst severity among the diagnostic tags starting in [start, end): a
// diagnostic's dot goes on the line it starts on, as on the Mac.
int Gutter::dotOn(const GtkTextIter& start, const GtkTextIter& end) const {
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    int worst = 0;
    for (const auto& t : diagTags_) {
        if (worst && worst <= t.second) continue;
        GtkTextTag* tag = gtk_text_tag_table_lookup(table, t.first.c_str());
        if (!tag) continue;
        GtkTextIter it = start;
        bool starts = gtk_text_iter_starts_tag(&it, tag);
        while (!starts && gtk_text_iter_forward_to_tag_toggle(&it, tag) &&
               gtk_text_iter_compare(&it, &end) < 0)
            starts = gtk_text_iter_starts_tag(&it, tag);
        if (starts) worst = t.second;
    }
    return worst;
}

GdkRGBA Gutter::colorOfSeverity(int severity) const {
    // The tag's own underline color, else the Mac's (LspShared.h).
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buffer_);
    for (const auto& t : diagTags_) {
        if (t.second != severity) continue;
        GtkTextTag* tag = gtk_text_tag_table_lookup(table, t.first.c_str());
        if (!tag) continue;
        GdkRGBA* c = nullptr;
        gboolean set = FALSE;
        g_object_get(tag, "underline-rgba", &c, "underline-rgba-set", &set, NULL);
        if (c) {
            GdkRGBA out = *c;
            gdk_rgba_free(c);
            if (set) return out;
        }
    }
    return hexColor(severity == 1 ? 0xF14C4C : 0xCCA700);
}

// ---------------------------------------------------------------- drawing

void Gutter::snapshot(GtkSnapshot* s, int width, int height) {
    CpuSpan span;
    draws_++;
    rows_.clear();
    barsDrawn_ = 0;
    checkFont();
    const graphene_rect_t all = GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height);
    gtk_snapshot_append_color(s, &background_, &all);
    BufferLines* l = lines();
    if (!l || mode_ == Mode::Off) return;

    GdkRectangle vis;
    gtk_text_view_get_visible_rect(view_, &vis);
    const int bottom = vis.y + vis.height;
    const size_t primary = caretLine();
    std::vector<size_t> carets{primary};
    if (l->extraCarets) {
        std::vector<size_t> offsets;
        l->extraCarets(offsets);
        for (size_t o : offsets) carets.push_back(l->lineOf(o));
    }
    std::sort(carets.begin(), carets.end());
    GutterFolds* folds = l->folds();
    const bool chevrons = folds && (hovering_ || folds->hasClosedFolds());
    const double right = width - kFoldLane;

    PangoLayout* layout = numberLayout(nullptr);
    char label[32];

    auto drawRow = [&](const GtkTextIter& at, const GdkRectangle& r) {
        const size_t line = l->lineOf(offsetOf(&at));
        size_t shown = line + 1;
        if (mode_ == Mode::Relative && line != primary)
            shown = line > primary ? line - primary : primary - line;
        g_snprintf(label, sizeof label, "%zu", shown);
        const bool active = std::binary_search(carets.begin(), carets.end(), line);
        const double y = toGutterY(r.y);
        pango_layout_set_text(layout, label, -1);
        int lw = 0, lh = 0;
        pango_layout_get_pixel_size(layout, &lw, &lh);
        gtk_snapshot_save(s);
        const graphene_point_t origin = GRAPHENE_POINT_INIT((float)(right - lw), (float)y);
        gtk_snapshot_translate(s, &origin);
        gtk_snapshot_append_layout(s, layout, active ? &active_ : &number_);
        gtk_snapshot_restore(s);
        Row row;
        row.line = line;
        row.label = label;
        row.y = y;
        row.height = r.height;
        row.active = active;
        const double mid = y + r.height / 2.0;
        // The line's characters, for its dot.
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_offset(buffer_, &a, (int)l->lineStart(line));
        gtk_text_buffer_get_iter_at_offset(buffer_, &b, (int)l->lineEnd(line));
        if ((row.dot = dotOn(a, b))) {
            const double d = 6;
            graphene_rect_t box = GRAPHENE_RECT_INIT((float)((kDotLane - d) / 2 + 1),
                                                     (float)(mid - d / 2), (float)d, (float)d);
            GskRoundedRect round;
            gsk_rounded_rect_init_from_rect(&round, &box, (float)(d / 2));
            const GdkRGBA c = colorOfSeverity(row.dot);
            gtk_snapshot_push_rounded_clip(s, &round);
            gtk_snapshot_append_color(s, &c, &box);
            gtk_snapshot_pop(s);
        }
        if (chevrons) {
            const bool closed = folds->startsClosedFold(line);
            if (closed || (hovering_ && folds->startsRegion(line))) {
                row.chevron = closed ? 2 : 1;
                drawChevron(s, width, mid, closed);
            }
        }
        rows_.push_back(row);
    };

    // The display lines on screen, from the first one the top of the view
    // is in (or the start of its paragraph).
    GtkTextIter it;
    int lineTop = 0;
    gtk_text_view_get_line_at_y(view_, &it, vis.y, &lineTop);
    size_t firstLine = l->lineOf(offsetOf(&it)), lastLine = firstLine;
    for (int guard = 0; guard < 100000; guard++) {
        GdkRectangle r;
        gtk_text_view_get_iter_location(view_, &it, &r);
        if (r.y >= bottom) break;
        if (r.y + r.height > vis.y && l->isLineStart(offsetOf(&it))) {
            drawRow(it, r);
            lastLine = rows_.back().line;
        }
        GtkTextIter next = it;
        const bool more = gtk_text_view_forward_display_line(view_, &next);
        if (gtk_text_iter_equal(&next, &it)) break;
        it = next;
        // At the end: an empty last line (after a final break) is a display
        // line of its own; the end of a line with text is not.
        if (!more && !gtk_text_view_starts_display_line(view_, &it)) break;
        if (!more) {
            gtk_text_view_get_iter_location(view_, &it, &r);
            if (r.y < bottom && l->isLineStart(offsetOf(&it))) {
                drawRow(it, r);
                lastLine = rows_.back().line;
            }
            break;
        }
    }
    g_object_unref(layout);
    if (changeLane_ && !l->changeBars().empty())
        drawChangeBars(s, firstLine ? firstLine - 1 : 0, lastLine + 1);
}

// A chevron in the fold lane: pointing down for an open region, right for a
// closed fold.
void Gutter::drawChevron(GtkSnapshot* s, double width, double mid, bool closed) {
    const double cx = width - kFoldLane / 2 - 1;
    const double r = 3;
    GskPathBuilder* b = gsk_path_builder_new();
    if (closed) {
        gsk_path_builder_move_to(b, (float)(cx - r / 2), (float)(mid - r));
        gsk_path_builder_line_to(b, (float)(cx + r / 2), (float)mid);
        gsk_path_builder_line_to(b, (float)(cx - r / 2), (float)(mid + r));
    } else {
        gsk_path_builder_move_to(b, (float)(cx - r), (float)(mid - r / 2));
        gsk_path_builder_line_to(b, (float)cx, (float)(mid + r / 2));
        gsk_path_builder_line_to(b, (float)(cx + r), (float)(mid - r / 2));
    }
    GskPath* path = gsk_path_builder_free_to_path(b);
    GskStroke* stroke = gsk_stroke_new(1.25f);
    gsk_stroke_set_line_cap(stroke, GSK_LINE_CAP_ROUND);
    gsk_stroke_set_line_join(stroke, GSK_LINE_JOIN_ROUND);
    gtk_snapshot_append_stroke(s, path, stroke, closed ? &active_ : &number_);
    gsk_stroke_free(stroke);
    gsk_path_unref(path);
}

graphene_rect_t Gutter::rectOfChangeBar(int index) const {
    graphene_rect_t none = GRAPHENE_RECT_INIT(0, 0, 0, 0);
    BufferLines* l = lines();
    if (!l || index < 0 || (size_t)index >= l->changeBars().size()) return none;
    const QuickDiff::Bar& b = l->changeBars()[(size_t)index];
    double y = 0, h = 0;
    if (b.kind != QuickDiff::Kind::Deleted) {
        if (!linesYRange(b.line, b.line + std::max<size_t>(b.lines, 1) - 1, &y, &h)) return none;
        return GRAPHENE_RECT_INIT((float)kDotLane, (float)y, (float)kChangeBarWidth, (float)h);
    }
    // A deletion: the boundary below the line above it (or above line 0).
    if (!linesYRange(QuickDiff::anchorLine(b), QuickDiff::anchorLine(b), &y, &h)) return none;
    const double at = b.line == 0 ? y : y + h;
    return GRAPHENE_RECT_INIT((float)kDotLane, (float)(at - 4), 6, 8);
}

// Only the bars whose lines are among [firstLine, lastLine] are drawn.
void Gutter::drawChangeBars(GtkSnapshot* s, size_t firstLine, size_t lastLine) {
    const std::vector<QuickDiff::Bar>& bars = lines()->changeBars();
    auto from = std::lower_bound(bars.begin(), bars.end(), firstLine,
                                 [](const QuickDiff::Bar& b, size_t line) {
                                     return b.line + std::max<size_t>(b.lines, 1) <= line;
                                 });
    // VS Code's colors (editorGutter.*Background), until item 2.10 sets the
    // settings' own.
    static const GdkRGBA colors[3] = {hexColor(0x487E02), hexColor(0x1B81A8), hexColor(0xF14C4C)};
    for (auto it = from; it != bars.end() && QuickDiff::anchorLine(*it) <= lastLine; ++it) {
        const graphene_rect_t r = rectOfChangeBar((int)(it - bars.begin()));
        if (r.size.width <= 0 || r.size.height <= 0) continue;
        const GdkRGBA& c = colors[it->kind == QuickDiff::Kind::Added      ? 0
                                  : it->kind == QuickDiff::Kind::Modified ? 1
                                                                          : 2];
        barsDrawn_++;
        if (it->kind != QuickDiff::Kind::Deleted) {
            gtk_snapshot_append_color(s, &c, &r);
            continue;
        }
        // VS Code's: a small triangle pointing into the text.
        GskPathBuilder* b = gsk_path_builder_new();
        gsk_path_builder_move_to(b, r.origin.x, r.origin.y);
        gsk_path_builder_line_to(b, r.origin.x + r.size.width, r.origin.y + r.size.height / 2);
        gsk_path_builder_line_to(b, r.origin.x, r.origin.y + r.size.height);
        gsk_path_builder_close(b);
        GskPath* path = gsk_path_builder_free_to_path(b);
        gtk_snapshot_append_fill(s, path, GSK_FILL_RULE_WINDING, &c);
        gsk_path_unref(path);
    }
}

// ---------------------------------------------------------------- the mouse

void Gutter::selectLines(size_t anchor, size_t line) {
    BufferLines* l = lines();
    if (!l) return;
    const size_t first = std::min(anchor, line), last = std::max(anchor, line);
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_offset(buffer_, &start, (int)l->lineStart(first));
    gtk_text_buffer_get_iter_at_offset(buffer_, &end, (int)l->lineEnd(last));
    // The caret at the end the pointer is at, as VS Code's: below the last
    // line when dragging down, at the first line's start when dragging up.
    if (line < anchor) gtk_text_buffer_select_range(buffer_, &start, &end);
    else gtk_text_buffer_select_range(buffer_, &end, &start);
}

void Gutter::onDragBegin(GtkGestureDrag* g, double x, double y, gpointer p) {
    Gutter* self = static_cast<Gutter*>(p);
    BufferLines* l = self->lines();
    if (!l) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_grab_focus(GTK_WIDGET(self->view_));
    self->dragging_ = false;
    self->dragX_ = x;
    self->dragY_ = y;
    const size_t line = self->lineAtY(y);
    // The change lane: a click on a bar shows its change.
    if (self->changeLane_ && self->onChangeBar && !l->changeBars().empty() &&
        x >= kDotLane - 1 && x < kDotLane + kChangeLane + 1) {
        const int at = QuickDiff::barAt(l->changeBars(), line);
        if (at >= 0) {
            self->onChangeBar(at, self->rectOfChangeBar(at));
            return;
        }
    }
    // The fold lane: a click on a region's line folds or unfolds it.
    if (GutterFolds* folds = l->folds())
        if (x >= gtk_widget_get_width(self->widget_) - kFoldLane - 2 &&
            (folds->startsClosedFold(line) || folds->startsRegion(line))) {
            folds->toggle(line);
            return;
        }
    const GdkModifierType mods =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (mods & GDK_SHIFT_MASK) {
        // From the line the selection starts on to this one (from the line
        // it ends on, when this one is above it).
        GtkTextIter a, b;
        gtk_text_buffer_get_selection_bounds(self->buffer_, &a, &b);
        const size_t s0 = offsetOf(&a), s1 = offsetOf(&b);
        self->dragAnchor_ = l->lineOf(s0);
        if (line < self->dragAnchor_) self->dragAnchor_ = l->lineOf(s1 > s0 ? s1 - 1 : s0);
    } else {
        self->dragAnchor_ = line;
    }
    self->dragging_ = true;
    self->selectLines(self->dragAnchor_, line);
}

void Gutter::onDragUpdate(GtkGestureDrag*, double, double dy, gpointer p) {
    Gutter* self = static_cast<Gutter*>(p);
    if (!self->dragging_ || !self->lines()) return;
    const double y = self->dragY_ + dy;
    self->selectLines(self->dragAnchor_, self->lineAtY(y));
    // Past the top or bottom edge, the view follows the selection.
    if (y < 0 || y > gtk_widget_get_height(self->widget_))
        gtk_text_view_scroll_mark_onscreen(self->view_, gtk_text_buffer_get_insert(self->buffer_));
}
