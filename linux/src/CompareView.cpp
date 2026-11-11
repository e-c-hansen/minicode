// CompareView.cpp — see CompareView.h.
#include "CompareView.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>

namespace {

CompareView::Color fromRgba(const Rgba& c) {
    return {c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a};
}

// The git panel's diff colors (GitDiffView): removed, added.
constexpr double kRed[3] = {0xF1 / 255.0, 0x4C / 255.0, 0x4C / 255.0};
constexpr double kGreen[3] = {0x73 / 255.0, 0xC9 / 255.0, 0x91 / 255.0};

void fill(cairo_t* cr, const double rgb[3], double alpha, double x, double y, double w, double h) {
    cairo_set_source_rgba(cr, rgb[0], rgb[1], rgb[2], alpha);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

}  // namespace

CompareView::CompareView() {
    for (Color& c : syntax_) c = fg_;
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(box_, TRUE);
    gtk_widget_set_vexpand(box_, TRUE);
    gtk_widget_add_css_class(box_, "minicode-compare");
    font_ = pango_font_description_from_string("monospace 11");
    vadj_ = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    g_object_ref_sink(vadj_);
    g_signal_connect(vadj_, "value-changed", G_CALLBACK(onAdjusted), this);
    for (int side = 0; side < 2; side++) {
        hadj_[side] = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
        g_object_ref_sink(hadj_[side]);
        g_signal_connect(hadj_[side], "value-changed", G_CALLBACK(onAdjusted), this);
        area_[side] = gtk_drawing_area_new();
        gtk_widget_set_hexpand(area_[side], TRUE);
        gtk_widget_set_vexpand(area_[side], TRUE);
        gtk_widget_set_focusable(area_[side], TRUE);
        gtk_widget_set_can_focus(area_[side], TRUE);
        gtk_widget_set_overflow(area_[side], GTK_OVERFLOW_HIDDEN);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_[side]), onDraw, this, nullptr);
        g_signal_connect(area_[side], "resize", G_CALLBACK(onResize), this);
        GtkEventController* keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
        gtk_widget_add_controller(area_[side], keys);
        GtkEventController* wheel =
            gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
        g_signal_connect(wheel, "scroll", G_CALLBACK(onScroll), this);
        gtk_widget_add_controller(area_[side], wheel);
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
        gtk_widget_add_controller(area_[side], GTK_EVENT_CONTROLLER(click));
        g_object_set_data(G_OBJECT(area_[side]), "compare-side", GINT_TO_POINTER(side + 1));

        // The pane: the drawing, the shared vertical bar on the right one's
        // edge, and its own horizontal bar underneath.
        GtkWidget* grid = gtk_grid_new();
        gtk_widget_set_hexpand(grid, TRUE);
        gtk_widget_set_vexpand(grid, TRUE);
        gtk_grid_attach(GTK_GRID(grid), area_[side], 0, 0, 1, 1);
        if (side == 1) {
            GtkWidget* vbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, vadj_);
            gtk_grid_attach(GTK_GRID(grid), vbar, 1, 0, 1, 1);
        }
        GtkWidget* hbar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, hadj_[side]);
        gtk_grid_attach(GTK_GRID(grid), hbar, 0, 1, 1, 1);
        if (side == 1) gtk_box_append(GTK_BOX(box_), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        gtk_box_append(GTK_BOX(box_), grid);
    }
    measure();
}

CompareView::~CompareView() {
    g_signal_handlers_disconnect_by_data(vadj_, this);
    g_object_unref(vadj_);
    for (GtkAdjustment* a : hadj_) {
        g_signal_handlers_disconnect_by_data(a, this);
        g_object_unref(a);
    }
    if (font_) pango_font_description_free(font_);
}

void CompareView::measure() {
    PangoContext* ctx = gtk_widget_get_pango_context(area_[0]);
    PangoLayout* l = pango_layout_new(ctx);
    pango_layout_set_font_description(l, font_);
    pango_layout_set_text(l, "0", 1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(l, &w, &h);
    g_object_unref(l);
    charW_ = std::max(w, 1);
    rowH_ = std::max(h + 2, 8);   // the one height of every row
    gutterW_ = charW_ * 6 + 8;
}

void CompareView::applySettings(const Settings& s) {
    bg_ = fromRgba(s.background(Surface::Editor));
    fg_ = fromRgba(s.text(Surface::Editor));
    muted_ = fg_;
    muted_.a = 0.5;
    for (int i = 0; i < kTokenStyleCount; i++) syntax_[i] = fromRgba(s.syntax((TokenStyle)i));
    for (int side = 0; side < 2; side++) gtk_widget_queue_draw(area_[side]);
}

// The adjustments cover the text; the widgets cover only what shows.
void CompareView::fitContent() {
    const size_t rows = data_ ? data_->rows.size() : 0;
    const double page = gtk_adjustment_get_page_size(vadj_);
    gtk_adjustment_configure(vadj_, 0, 0, std::max((double)rows * rowH_ + 8, page), rowH_, page * 0.9,
                             page);
    for (int side = 0; side < 2; side++) {
        const size_t cols = data_ ? data_->panes[side].columns : 0;
        const double w = gtk_adjustment_get_page_size(hadj_[side]);
        gtk_adjustment_configure(hadj_[side], 0, 0,
                                 std::max(gutterW_ + 8 + (double)cols * charW_ + 16, w), charW_ * 4,
                                 w * 0.9, w);
    }
    for (int side = 0; side < 2; side++) gtk_widget_queue_draw(area_[side]);
}

void CompareView::show(std::shared_ptr<const CompareData> data) {
    data_ = std::move(data);
    current_ = -1;
    fitContent();
    gtk_adjustment_set_value(vadj_, 0);
    for (GtkAdjustment* a : hadj_) gtk_adjustment_set_value(a, 0);
}

void CompareView::resized(int side, int w, int h) {
    gtk_adjustment_set_page_size(vadj_, h);
    gtk_adjustment_set_page_increment(vadj_, h * 0.9);
    gtk_adjustment_set_page_size(hadj_[side], w);
    gtk_adjustment_set_page_increment(hadj_[side], w * 0.9);
}

void CompareView::onResize(GtkDrawingArea* area, int w, int h, gpointer p) {
    CompareView* self = static_cast<CompareView*>(p);
    self->resized(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "compare-side")) - 1, w, h);
}

void CompareView::onAdjusted(GtkAdjustment*, gpointer p) {
    CompareView* self = static_cast<CompareView*>(p);
    for (int side = 0; side < 2; side++) gtk_widget_queue_draw(self->area_[side]);
}

// The wheel: three rows a notch down, a few characters sideways; the vertical
// bar is the one both panes share.
gboolean CompareView::onScroll(GtkEventControllerScroll* c, double dx, double dy, gpointer p) {
    CompareView* self = static_cast<CompareView*>(p);
    const int side =
        GPOINTER_TO_INT(g_object_get_data(
            G_OBJECT(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(c))), "compare-side")) - 1;
    auto move = [](GtkAdjustment* a, double by) {
        const double max = std::max(0.0, gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a));
        gtk_adjustment_set_value(a, std::clamp(gtk_adjustment_get_value(a) + by, 0.0, max));
    };
    if (dy != 0) move(self->vadj_, dy * self->rowH_ * 3);
    if (dx != 0) move(self->hadj_[side], dx * self->charW_ * 6);
    return TRUE;
}

long CompareView::topRow(bool) const {
    return (long)std::floor(gtk_adjustment_get_value(vadj_) / rowH_);
}

void CompareView::scrollToRow(long row) {
    const double max = std::max(0.0, gtk_adjustment_get_upper(vadj_) - gtk_adjustment_get_page_size(vadj_));
    gtk_adjustment_set_value(vadj_, std::clamp((double)row * rowH_, 0.0, max));
}

bool CompareView::step(bool forward) {
    if (!data_ || data_->hunks.empty()) return false;
    const int h = SideBySide::stepHunk(data_->hunks, current_, forward);
    if (h < 0) return false;
    current_ = (long)data_->hunks[h].first;
    // A few rows of context above the change.
    scrollToRow(std::max(0L, current_ - 3));
    for (int side = 0; side < 2; side++) gtk_widget_queue_draw(area_[side]);
    return true;
}

gboolean CompareView::onKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                            gpointer p) {
    CompareView* self = static_cast<CompareView*>(p);
    const bool shift = mods & GDK_SHIFT_MASK;
    if (key == GDK_KEY_F7) {
        if (!self->step(!shift)) gtk_widget_error_bell(self->box_);
        return TRUE;
    }
    return FALSE;
}

void CompareView::onPressed(GtkGestureClick* g, int, double, double, gpointer) {
    gtk_widget_grab_focus(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)));
}

void CompareView::onDraw(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer p) {
    CompareView* self = static_cast<CompareView*>(p);
    const int side = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "compare-side")) - 1;
    self->draw(side, cr, w, h);
}

namespace {

// Pieces (ascending) overlapping [from, to): the first index to look at.
size_t firstPiece(const std::vector<ComparePane::Piece>& v, size_t from) {
    size_t lo = 0, hi = v.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (v[mid].start + v[mid].length <= from) lo = mid + 1; else hi = mid;
    }
    return lo;
}

}  // namespace

// Only the rows on screen are shaped: the rows' places are arithmetic.
void CompareView::draw(int side, cairo_t* cr, int width, int height) {
    cairo_set_source_rgba(cr, bg_.r, bg_.g, bg_.b, 1);
    cairo_paint(cr);
    if (!data_) return;
    const ComparePane& pane = data_->panes[side];
    GtkAdjustment* va = vadj_;
    GtkAdjustment* ha = hadj_[side];
    const double top = gtk_adjustment_get_value(va);
    const double page = height;
    const double hx = gtk_adjustment_get_value(ha);
    const long rows = (long)data_->rows.size();
    const long first = std::max(0L, (long)std::floor(top / rowH_));
    const long last = std::min(rows - 1, (long)std::ceil((top + page) / rowH_));
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font_);
    PangoTabArray* tabs = pango_tab_array_new(1, FALSE);
    pango_tab_array_set_tab(tabs, 0, PANGO_TAB_LEFT, charW_ * 8);
    pango_layout_set_tabs(layout, tabs);
    pango_tab_array_free(tabs);
    const double textX = gutterW_ + 4 - hx;
    const double* tint = side == 0 ? kRed : kGreen;

    for (long r = first; r <= last; r++) {
        const double y = (double)r * rowH_ - top;
        const SideBySide::Kind kind = data_->rows[r].kind;
        const bool filler = pane.line[r] < 0;
        if (filler) {
            // Hatched: the other side's lines have no partner here.
            cairo_save(cr);
            cairo_rectangle(cr, 0, y, width, rowH_);
            cairo_clip(cr);
            cairo_set_source_rgba(cr, fg_.r, fg_.g, fg_.b, 0.06);
            cairo_set_line_width(cr, 1);
            for (double x = -rowH_; x < width; x += 6) {
                cairo_move_to(cr, x, y + rowH_);
                cairo_line_to(cr, x + rowH_, y);
            }
            cairo_stroke(cr);
            cairo_restore(cr);
            continue;
        }
        if (kind != SideBySide::Kind::Same) fill(cr, tint, 0.14, 0, y, width, rowH_);
        const size_t from = pane.starts[r], to = pane.starts[r + 1] - 1;
        // The changed words, stronger.
        const std::string text = pane.rowText(r);
        pango_layout_set_text(layout, text.data(), (int)text.size());
        for (size_t i = firstPiece(pane.words, from); i < pane.words.size() &&
                                                      pane.words[i].start < to; i++) {
            const ComparePane::Piece& w = pane.words[i];
            PangoRectangle a, b;
            pango_layout_index_to_pos(layout, (int)(w.start - from), &a);
            pango_layout_index_to_pos(layout, (int)(w.start + w.length - from), &b);
            const double x0 = textX + a.x / (double)PANGO_SCALE, x1 = textX + b.x / (double)PANGO_SCALE;
            fill(cr, tint, 0.38, x0, y, std::max(x1 - x0, 1.0), rowH_);
        }
        // The syntax colors.
        PangoAttrList* attrs = pango_attr_list_new();
        for (size_t i = firstPiece(pane.syntax, from); i < pane.syntax.size() &&
                                                       pane.syntax[i].start < to; i++) {
            const ComparePane::Piece& s = pane.syntax[i];
            const Color& c = syntax_[std::clamp(s.style, 0, kTokenStyleCount - 1)];
            PangoAttribute* at = pango_attr_foreground_new((guint16)(c.r * 65535),
                                                           (guint16)(c.g * 65535),
                                                           (guint16)(c.b * 65535));
            at->start_index = (guint)(std::max(s.start, from) - from);
            at->end_index = (guint)(std::min(s.start + s.length, to) - from);
            pango_attr_list_insert(attrs, at);
        }
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
        cairo_set_source_rgba(cr, fg_.r, fg_.g, fg_.b, 1);
        cairo_move_to(cr, textX, y + 1);
        pango_cairo_show_layout(cr, layout);
        pango_layout_set_attributes(layout, nullptr);
    }
    // The line numbers over the text, which scrolls sideways beneath them.
    cairo_set_source_rgba(cr, bg_.r, bg_.g, bg_.b, 1);
    cairo_rectangle(cr, 0, 0, gutterW_, height);
    cairo_fill(cr);
    for (long r = first; r <= last; r++) {
        if (pane.line[r] < 0) continue;
        const double y = (double)r * rowH_ - top;
        if (data_->rows[r].kind != SideBySide::Kind::Same) fill(cr, tint, 0.14, 0, y, gutterW_, rowH_);
        char num[24];
        const int n = g_snprintf(num, sizeof num, "%d", pane.line[r] + 1);
        pango_layout_set_text(layout, num, n);
        PangoRectangle ext;
        pango_layout_get_pixel_extents(layout, nullptr, &ext);
        cairo_set_source_rgba(cr, muted_.r, muted_.g, muted_.b, muted_.a);
        cairo_move_to(cr, gutterW_ - 6 - ext.width, y + 1);
        pango_cairo_show_layout(cr, layout);
    }
    // The current hunk's first row, marked on the gutter's edge.
    if (current_ >= 0 && current_ >= first && current_ <= last) {
        cairo_set_source_rgba(cr, 0.31, 0.63, 0.97, 1);
        cairo_rectangle(cr, 0, (double)current_ * rowH_ - top, 2, rowH_);
        cairo_fill(cr);
    }
    g_object_unref(layout);
}

std::vector<std::string> CompareView::wordsOnRow(bool left, size_t row) const {
    std::vector<std::string> out;
    if (!data_ || row >= data_->rows.size()) return out;
    const ComparePane& p = data_->panes[left ? 0 : 1];
    const size_t from = p.starts[row], to = p.starts[row + 1] - 1;
    for (const ComparePane::Piece& w : p.words)
        if (w.start >= from && w.start < to) out.push_back(p.text.substr(w.start, w.length));
    return out;
}

int CompareView::styleAt(bool left, size_t row, size_t col) const {
    if (!data_ || row >= data_->rows.size()) return -1;
    const ComparePane& p = data_->panes[left ? 0 : 1];
    const size_t at = p.starts[row] + col;
    for (const ComparePane::Piece& s : p.syntax)
        if (at >= s.start && at < s.start + s.length) return s.style;
    return -1;
}
