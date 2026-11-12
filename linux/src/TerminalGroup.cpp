// TerminalGroup.cpp — see TerminalGroup.h.
#include "TerminalGroup.h"

#ifdef MINICODE_ENABLE_TERMINAL

#include <algorithm>
#include <cmath>

namespace {
const char* kKey = "minicode-terminal-group";
}

TerminalGroup::TerminalGroup(Terminal* first) {
    area_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_layout_manager(area_, gtk_custom_layout_new(nullptr, measure, allocate));
    g_object_set_data(G_OBJECT(area_), kKey, this);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_widget_set_overflow(area_, GTK_OVERFLOW_HIDDEN);
    // One gesture on the group, not on each bar: a bar moves while it is
    // dragged, and a gesture on a moving widget measures from a moving origin.
    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
    auto onBegin = +[](GtkGestureDrag* gd, double x, double y, gpointer p) {
        TerminalGroup* g = static_cast<TerminalGroup*>(p);
        g->dragging_ = nullptr;
        const double W = g->total(true), H = g->total(false);
        for (Bar* b : g->colBars_)
            if (std::fabs(x - TerminalLayout::boundaryAt(g->layout_.columns, (size_t)b->idx, W, kGap)) <= kGap) {
                g->dragging_ = b;
                b->start = TerminalLayout::boundaryAt(g->layout_.columns, (size_t)b->idx, W, kGap);
            }
        if (!g->dragging_)
            for (Bar* b : g->rowBars_) {
                TerminalLayout::Rect f;
                const double pos = TerminalLayout::boundaryAt(g->layout_.rows[(size_t)b->col], (size_t)b->idx, H, kGap);
                graphene_rect_t r;
                if (std::fabs(y - pos) <= kGap && gtk_widget_compute_bounds(b->widget, g->area_, &r) &&
                    x >= r.origin.x && x <= r.origin.x + r.size.width) {
                    g->dragging_ = b;
                    b->start = pos;
                    (void)f;
                }
            }
        if (g->dragging_) gtk_gesture_set_state(GTK_GESTURE(gd), GTK_EVENT_SEQUENCE_CLAIMED);
        else gtk_gesture_set_state(GTK_GESTURE(gd), GTK_EVENT_SEQUENCE_DENIED);
    };
    g_signal_connect(drag, "drag-begin", G_CALLBACK(onBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag*, double dx, double dy, gpointer p) {
        TerminalGroup* g = static_cast<TerminalGroup*>(p);
        if (g->dragging_) g->dragTo(g->dragging_, g->dragging_->vertical ? dx : dy);
    }), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer p) {
        static_cast<TerminalGroup*>(p)->dragging_ = nullptr;
    }), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(drag));
    cols_ = {{first}};
    active_ = first;
    gtk_widget_set_overflow(first->widget(), GTK_OVERFLOW_HIDDEN);
    gtk_box_append(GTK_BOX(area_), first->widget());
}

TerminalGroup::~TerminalGroup() {
    // The widget may outlive us for a moment (the dock holds a reference
    // while it takes the terminals down): it must not call back.
    g_object_set_data(G_OBJECT(area_), kKey, nullptr);
    for (Bar* b : colBars_) delete b;
    for (Bar* b : rowBars_) delete b;
    for (auto& col : cols_)
        for (Terminal* t : col) delete t;
}

std::vector<Terminal*> TerminalGroup::panes() const {
    std::vector<Terminal*> out;
    for (const auto& col : cols_) out.insert(out.end(), col.begin(), col.end());
    return out;
}

int TerminalGroup::count() const {
    int n = 0;
    for (const auto& col : cols_) n += (int)col.size();
    return n;
}

bool TerminalGroup::find(const Terminal* t, int* col, int* row) const {
    for (size_t c = 0; c < cols_.size(); c++)
        for (size_t r = 0; r < cols_[c].size(); r++)
            if (cols_[c][r] == t) {
                if (col) *col = (int)c;
                if (row) *row = (int)r;
                return true;
            }
    return false;
}

void TerminalGroup::measure(GtkWidget*, GtkOrientation, int, int* min, int* nat, int* minBase,
                            int* natBase) {
    *min = *nat = 0;
    *minBase = *natBase = -1;
}

void TerminalGroup::allocate(GtkWidget* w, int width, int height, int) {
    if (auto* g = static_cast<TerminalGroup*>(g_object_get_data(G_OBJECT(w), kKey)))
        g->place(width, height);
}

double TerminalGroup::total(bool horizontal) const {
    // Not laid out yet (the dock was just shown): a plausible size, so a
    // split made then keeps its shares for the real one.
    const int n = horizontal ? gtk_widget_get_width(area_) : gtk_widget_get_height(area_);
    return n > 0 ? n : (horizontal ? 1000 : 300);
}

void TerminalGroup::cell(const Terminal* t, double* w, double* h) const {
    double cw = 8, ch = 17;
    if (t) t->cellSize(&cw, &ch);
    *w = cw;
    *h = ch;
}

void TerminalGroup::place(int W, int H) {
    if (W <= 0 || H <= 0) return;
    const auto frames = TerminalLayout::frames(layout_, W, H, kGap);
    auto rect = [](const TerminalLayout::Rect& r) {
        GdkRectangle a;
        a.x = (int)std::lround(r.x);
        a.y = (int)std::lround(r.y);
        a.width = std::max(1, (int)std::lround(r.w));
        a.height = std::max(1, (int)std::lround(r.h));
        return a;
    };
    for (size_t c = 0; c < cols_.size() && c < frames.size(); c++)
        for (size_t r = 0; r < cols_[c].size() && r < frames[c].size(); r++) {
            const GdkRectangle a = rect(frames[c][r]);
            gtk_widget_size_allocate(cols_[c][r]->widget(), &a, -1);
        }
    for (Bar* b : colBars_) {
        const double x = TerminalLayout::boundaryAt(layout_.columns, (size_t)b->idx, W, kGap);
        GdkRectangle a{(int)std::lround(x - kGap / 2), 0, (int)kGap, H};
        gtk_widget_size_allocate(b->widget, &a, -1);
    }
    for (Bar* b : rowBars_) {
        const double y = TerminalLayout::boundaryAt(layout_.rows[(size_t)b->col], (size_t)b->idx, H, kGap);
        const TerminalLayout::Rect f = frames[(size_t)b->col][0];
        GdkRectangle a{(int)std::lround(f.x), (int)std::lround(y - kGap / 2),
                       std::max(1, (int)std::lround(f.w)), (int)kGap};
        gtk_widget_size_allocate(b->widget, &a, -1);
    }
}

TerminalGroup::Bar* TerminalGroup::makeBar(bool vertical) {
    Bar* b = new Bar{this, gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), vertical};
    gtk_widget_add_css_class(b->widget, "minicode-terminal-bar");
    gtk_widget_set_cursor_from_name(b->widget, vertical ? "col-resize" : "row-resize");
    gtk_box_append(GTK_BOX(area_), b->widget);
    return b;
}

void TerminalGroup::dragTo(Bar* b, double offset) {
    const double pos = b->start + offset;
    bool moved;
    if (b->vertical) {
        double cw, ch;
        cell(active_, &cw, &ch);
        moved = TerminalLayout::dragBoundary(layout_.columns, (size_t)b->idx, pos, total(true), kGap,
                                             kMinColumns * cw);
    } else {
        double cw, ch;
        cell(active_, &cw, &ch);
        moved = TerminalLayout::dragBoundary(layout_.rows[(size_t)b->col], (size_t)b->idx, pos,
                                             total(false), kGap, kMinRows * ch);
    }
    if (moved) gtk_widget_queue_allocate(area_);
}

// One bar per boundary, kept while the boundary exists: bars are reused in
// order and only the surplus goes, a missing one is made. Last in the box,
// so they sit over the panes' edges.
void TerminalGroup::syncBars() {
    const size_t wantCols = cols_.empty() ? 0 : cols_.size() - 1;
    size_t wantRows = 0;
    for (const auto& col : cols_) wantRows += col.size() - 1;
    dragging_ = nullptr;
    while (colBars_.size() > wantCols) {
        gtk_box_remove(GTK_BOX(area_), colBars_.back()->widget);
        delete colBars_.back();
        colBars_.pop_back();
    }
    while (colBars_.size() < wantCols) colBars_.push_back(makeBar(true));
    while (rowBars_.size() > wantRows) {
        gtk_box_remove(GTK_BOX(area_), rowBars_.back()->widget);
        delete rowBars_.back();
        rowBars_.pop_back();
    }
    while (rowBars_.size() < wantRows) rowBars_.push_back(makeBar(false));
    for (size_t i = 0; i < colBars_.size(); i++) colBars_[i]->idx = (int)i;
    size_t k = 0;
    for (size_t c = 0; c < cols_.size(); c++)
        for (size_t r = 0; r + 1 < cols_[c].size(); r++) {
            rowBars_[k]->col = (int)c;
            rowBars_[k]->idx = (int)r;
            k++;
        }
    for (Bar* b : colBars_) gtk_widget_insert_before(b->widget, area_, nullptr);
    for (Bar* b : rowBars_) gtk_widget_insert_before(b->widget, area_, nullptr);
}

TerminalLayout::Split TerminalGroup::plan(const Terminal* of, bool down) const {
    int c, r;
    if (!find(of, &c, &r)) return TerminalLayout::Split::Refused;
    double cw, ch;
    cell(of, &cw, &ch);
    TerminalLayout::Layout copy = layout_;
    if (down)
        return TerminalLayout::split(copy.rows[(size_t)c], (size_t)r, total(false), kGap, kMinRows * ch);
    return TerminalLayout::split(copy.columns, (size_t)c, total(true), kGap, kMinColumns * cw);
}

TerminalLayout::Split TerminalGroup::split(Terminal* of, Terminal* fresh, bool down) {
    int c, r;
    if (!find(of, &c, &r)) return TerminalLayout::Split::Refused;
    double cw, ch;
    cell(of, &cw, &ch);
    TerminalLayout::Split how;
    if (down) {
        how = TerminalLayout::split(layout_.rows[(size_t)c], (size_t)r, total(false), kGap, kMinRows * ch);
        if (how == TerminalLayout::Split::Refused) return how;
        cols_[(size_t)c].insert(cols_[(size_t)c].begin() + r + 1, fresh);
    } else {
        how = TerminalLayout::split(layout_.columns, (size_t)c, total(true), kGap, kMinColumns * cw);
        if (how == TerminalLayout::Split::Refused) return how;
        cols_.insert(cols_.begin() + c + 1, std::vector<Terminal*>{fresh});
        layout_.rows.insert(layout_.rows.begin() + c + 1, TerminalLayout::Fractions{1.0});
    }
    gtk_widget_set_overflow(fresh->widget(), GTK_OVERFLOW_HIDDEN);
    gtk_box_append(GTK_BOX(area_), fresh->widget());
    syncBars();
    gtk_widget_queue_allocate(area_);
    return how;
}

Terminal* TerminalGroup::take(Terminal* t) {
    int c, r;
    if (!find(t, &c, &r)) return nullptr;
    Terminal* next = nullptr;
    if (cols_[(size_t)c].size() > 1) next = cols_[(size_t)c][r > 0 ? (size_t)r - 1 : 1];
    else if (cols_.size() > 1) next = cols_[c > 0 ? (size_t)c - 1 : 1][0];
    TerminalLayout::removePane(layout_, (size_t)c, (size_t)r);
    cols_[(size_t)c].erase(cols_[(size_t)c].begin() + r);
    if (cols_[(size_t)c].empty()) cols_.erase(cols_.begin() + c);
    gtk_box_remove(GTK_BOX(area_), t->widget());
    if (active_ == t) active_ = next;
    syncBars();
    gtk_widget_queue_allocate(area_);
    return next;
}

bool TerminalGroup::resize(const Terminal* of, Dir d, int cells) {
    int c, r;
    if (!find(of, &c, &r)) return false;
    double cw, ch;
    cell(of, &cw, &ch);
    bool moved;
    if (d == Dir::Left || d == Dir::Right)
        moved = TerminalLayout::nudge(layout_.columns, (size_t)c, d == Dir::Right, cells * cw,
                                      total(true), kGap, kMinColumns * cw);
    else
        moved = TerminalLayout::nudge(layout_.rows[(size_t)c], (size_t)r, d == Dir::Down, cells * ch,
                                      total(false), kGap, kMinRows * ch);
    if (moved) gtk_widget_queue_allocate(area_);
    return moved;
}

bool TerminalGroup::aloneInColumn(const Terminal* of) const {
    int c, r;
    return find(of, &c, &r) && layout_.rows[(size_t)c].size() == 1;
}

bool TerminalGroup::frameOf(const Terminal* t, TerminalLayout::Rect* out) const {
    int c, r;
    if (!find(t, &c, &r)) return false;
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(t->widget(), area_, &b)) return false;
    *out = {b.origin.x, b.origin.y, b.size.width, b.size.height};
    return true;
}

bool TerminalGroup::barCenter(bool columnBar, int index, double* x, double* y) const {
    const auto& bars = columnBar ? colBars_ : rowBars_;
    if (index < 0 || index >= (int)bars.size()) return false;
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(bars[(size_t)index]->widget, area_, &b)) return false;
    *x = b.origin.x + b.size.width / 2;
    *y = b.origin.y + b.size.height / 2;
    return true;
}

#endif
