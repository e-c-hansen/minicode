// PdfView.cpp — see PdfView.h.
#ifdef MINICODE_ENABLE_PDF

#include "PdfView.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int    kMargin  = 12;     // around the column of pages, logical px
constexpr int    kSpacing = 12;     // between pages
constexpr int    kKeep    = 3;      // pages kept rendered either side of the view
constexpr guint  kResizeSettleMs = 120;
constexpr guint  kAnchorGiveUpMs = 1000;
}  // namespace

// ---------------------------------------------------------------- construction

PdfView::PdfView() {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, kSpacing);
    gtk_widget_set_margin_top(box_, kMargin);
    gtk_widget_set_margin_bottom(box_, kMargin);
    gtk_widget_set_margin_start(box_, kMargin);
    gtk_widget_set_margin_end(box_, kMargin);
    gtk_widget_set_valign(box_, GTK_ALIGN_START);

    scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller_, "minicode-scroller");
    gtk_widget_add_css_class(scroller_, "minicode-media");
    // Horizontal AUTOMATIC rather than NEVER: with NEVER the pages' width would
    // become the scroller's minimum width, and the window could then only
    // ever grow. The pages are fitted to the width, so no bar shows in use.
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), box_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);

    GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_));
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    g_signal_connect(h, "changed", G_CALLBACK(onHadjChanged), this);
    g_signal_connect(v, "changed", G_CALLBACK(onVadjChanged), this);
    g_signal_connect(v, "value-changed", G_CALLBACK(onScrolled), this);
    g_signal_connect(scroller_, "notify::scale-factor", G_CALLBACK(onScaleChanged), this);
    g_signal_connect_swapped(scroller_, "map", G_CALLBACK(+[](gpointer self) {
        static_cast<PdfView*>(self)->scheduleRender();
    }), this);

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(scroller_, GTK_EVENT_CONTROLLER(click));

    // The widget belongs to whatever layout it is put in, and may outlive
    // this object; keep a reference so the destructor can detach cleanly.
    g_object_ref_sink(scroller_);
}

PdfView::~PdfView() {
    if (renderIdle_)  g_source_remove(renderIdle_);
    if (resizeTimer_) g_source_remove(resizeTimer_);
    if (anchorTimer_) g_source_remove(anchorTimer_);
    if (relayoutIdle_) g_source_remove(relayoutIdle_);
    renderIdle_ = resizeTimer_ = anchorTimer_ = relayoutIdle_ = 0;
    clear();
    g_signal_handlers_disconnect_by_data(
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)), this);
    g_signal_handlers_disconnect_by_data(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)), this);
    g_signal_handlers_disconnect_by_data(scroller_, this);
    GListModel* ctrls = gtk_widget_observe_controllers(scroller_);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        g_signal_handlers_disconnect_by_data(c, this);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
    g_object_unref(scroller_);
}

// ---------------------------------------------------------------- loading

bool PdfView::load(const std::string& path, bool keepPosition, std::string* error) {
    char* data = nullptr;
    gsize len = 0;
    GError* err = nullptr;
    if (!g_file_get_contents(path.c_str(), &data, &len, &err)) {
        if (error) *error = err ? err->message : "could not read the file";
        g_clear_error(&err);
        return false;
    }
    GBytes* bytes = g_bytes_new_take(data, len);
    PopplerDocument* doc = poppler_document_new_from_bytes(bytes, nullptr, &err);
    g_bytes_unref(bytes);   // the document keeps its own reference
    if (!doc) {
        if (error) *error = err ? err->message : "not a readable PDF";
        g_clear_error(&err);
        return false;
    }

    const bool keep = keepPosition && doc_ != nullptr;
    const Anchor was = keep ? anchor() : Anchor{};
    if (doc_) g_object_unref(doc_);
    doc_ = doc;
    path_ = path;

    const int n = poppler_document_get_n_pages(doc_);
    rebuildPages(n);
    for (int i = 0; i < n; ++i) {
        PopplerPage* p = poppler_document_get_page(doc_, i);
        double w = 612, h = 792;   // US Letter, if a page will not say
        if (p) { poppler_page_get_size(p, &w, &h); g_object_unref(p); }
        pages_[i].wPts = std::max(w, 1.0);
        pages_[i].hPts = std::max(h, 1.0);
        // Keep the old bitmap on screen until the new one is ready, so a
        // reload does not flash white; a zero scale marks it stale.
        pages_[i].surfaceScale = 0;
    }
    relayout();
    if (keep && was.page < n) {
        scrollTo(was);
    } else {
        hasPending_ = false;
        gtk_adjustment_set_value(
            gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)), 0);
    }
    scheduleRender();
    return true;
}

void PdfView::clear() {
    rebuildPages(0);
    if (doc_) g_object_unref(doc_);
    doc_ = nullptr;
    path_.clear();
    hasPending_ = false;
}

// Grow or shrink the list of placeholders to `count`, reusing the ones there
// are. Reuse is what lets a reload with unchanged page sizes keep the exact
// scroll offset without waiting for a new layout.
void PdfView::rebuildPages(int count) {
    while ((int)pages_.size() > count) {
        Page& p = pages_.back();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p.area), nullptr, nullptr, nullptr);
        gtk_box_remove(GTK_BOX(box_), p.area);
        if (p.surface) cairo_surface_destroy(p.surface);
        pages_.pop_back();
    }
    while ((int)pages_.size() < count) {
        Page p;
        p.area = gtk_drawing_area_new();
        gtk_widget_set_halign(p.area, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(p.area, "minicode-pdf-page");
        gtk_box_append(GTK_BOX(box_), p.area);
        pages_.push_back(p);
    }
    // Page structs may have moved; the draw funcs carry the index, not a pointer.
    for (size_t i = 0; i < pages_.size(); ++i) {
        g_object_set_data(G_OBJECT(pages_[i].area), "minicode-page", GINT_TO_POINTER((int)i));
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pages_[i].area), drawPage, this, nullptr);
    }
}

bool PdfView::pageSize(int page, double* width, double* height) const {
    if (page < 0 || page >= (int)pages_.size()) return false;
    if (width)  *width  = pages_[page].wPts;
    if (height) *height = pages_[page].hPts;
    return true;
}

// ---------------------------------------------------------------- layout

double PdfView::availableWidth() const {
    GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_));
    return gtk_adjustment_get_page_size(h);
}

// Fractional scaling on Wayland is reported by the surface; the widget's
// integer scale factor is the fallback before the view is realized.
double PdfView::deviceScale() const {
    GtkNative* native = gtk_widget_get_native(scroller_);
    GdkSurface* surf = native ? gtk_native_get_surface(native) : nullptr;
    double s = surf ? gdk_surface_get_scale(surf) : 0;
    if (s <= 0) s = gtk_widget_get_scale_factor(scroller_);
    return s > 0 ? s : 1.0;
}

// Fit the widest page to the view. Other pages keep their size relative to
// it, so a landscape page in a portrait document is not blown up.
void PdfView::relayout() {
    const double avail = availableWidth();
    lastWidth_ = avail;
    double widest = 0;
    for (const Page& p : pages_) widest = std::max(widest, p.wPts);
    scale_ = (avail > 2 * kMargin + 1 && widest > 0)
                 ? std::max(0.05, (avail - 2 * kMargin) / widest)
                 : 1.0;   // not laid out yet: one pixel per point until it is
    for (Page& p : pages_) {
        p.wPx = std::max(1, (int)std::lround(p.wPts * scale_));
        p.hPx = std::max(1, (int)std::lround(p.hPts * scale_));
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p.area), p.wPx);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p.area), p.hPx);
        gtk_widget_queue_draw(p.area);
    }
}

double PdfView::pageTop(int i) const {
    double y = kMargin;
    for (int j = 0; j < i && j < (int)pages_.size(); ++j) y += pages_[j].hPx + kSpacing;
    return y;
}

void PdfView::visibleRange(int* first, int* last) const {
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double top = gtk_adjustment_get_value(v);
    const double bottom = top + std::max(1.0, gtk_adjustment_get_page_size(v));
    *first = -1;
    *last = -1;
    double y = kMargin;
    for (int i = 0; i < (int)pages_.size(); ++i) {
        const double end = y + pages_[i].hPx;
        if (end >= top && y <= bottom) {
            if (*first < 0) *first = i;
            *last = i;
        } else if (y > bottom) {
            break;
        }
        y = end + kSpacing;
    }
    if (*first < 0 && !pages_.empty()) *first = *last = 0;
}

// ---------------------------------------------------------------- scroll position

PdfView::Anchor PdfView::anchor() const {
    Anchor a;
    if (pages_.empty()) return a;
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double value = gtk_adjustment_get_value(v);
    double y = kMargin;
    for (int i = 0; i < (int)pages_.size(); ++i) {
        const double next = y + pages_[i].hPx + kSpacing;
        if (value < next || i + 1 == (int)pages_.size()) {
            a.page = i;
            a.y = (value - y) * pages_[i].hPts / pages_[i].hPx;
            return a;
        }
        y = next;
    }
    return a;
}

void PdfView::scrollTo(const Anchor& a) {
    pending_ = a;
    hasPending_ = true;
    applyPendingAnchor();
    // The target may be past the end of the scroll range until the new sizes
    // are allocated; keep trying as the range grows, but not forever, or a
    // later resize would drag the view back here.
    if (anchorTimer_) g_source_remove(anchorTimer_);
    anchorTimer_ = 0;
    if (hasPending_)
        anchorTimer_ = g_timeout_add(kAnchorGiveUpMs, [](gpointer p) -> gboolean {
            PdfView* self = static_cast<PdfView*>(p);
            self->anchorTimer_ = 0;
            self->hasPending_ = false;
            return G_SOURCE_REMOVE;
        }, this);
}

void PdfView::applyPendingAnchor() {
    if (!hasPending_ || pages_.empty()) return;
    const int page = std::min(std::max(pending_.page, 0), (int)pages_.size() - 1);
    const double target = std::max(0.0, pageTop(page) +
                                   pending_.y * pages_[page].hPx / pages_[page].hPts);
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double maxValue = gtk_adjustment_get_upper(v) - gtk_adjustment_get_page_size(v);
    gtk_adjustment_set_value(v, target);
    if (maxValue + 0.5 >= target) {
        hasPending_ = false;
        if (anchorTimer_) g_source_remove(anchorTimer_);
        anchorTimer_ = 0;
    }
}

// ---------------------------------------------------------------- rendering

cairo_surface_t* PdfView::renderPage(PopplerPage* page, int pixelWidth) {
    double w = 0, h = 0;
    poppler_page_get_size(page, &w, &h);
    if (w <= 0 || h <= 0 || pixelWidth <= 0) return nullptr;
    const double s = pixelWidth / w;
    const int ph = std::max(1, (int)std::ceil(h * s));
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth, ph);
    cairo_t* cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);   // PDFs assume paper underneath
    cairo_paint(cr);
    cairo_scale(cr, s, s);
    poppler_page_render(page, cr);
    cairo_destroy(cr);
    return surf;
}

void PdfView::scheduleRender() {
    if (renderIdle_ || !doc_) return;
    renderIdle_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer p) -> gboolean {
        PdfView* self = static_cast<PdfView*>(p);
        if (self->renderOne()) return G_SOURCE_CONTINUE;
        self->renderIdle_ = 0;
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

// Render the most useful page that needs it: the visible ones top to bottom,
// then outward. One page per call, so scrolling and typing stay responsive
// while a long document fills in.
bool PdfView::renderOne() {
    if (!doc_ || pages_.empty() || !gtk_widget_get_mapped(scroller_)) return false;
    int first, last;
    visibleRange(&first, &last);
    const double ds = deviceScale();
    const bool resizing = resizeTimer_ != 0;

    auto needs = [&](int i) {
        if (i < 0 || i >= (int)pages_.size()) return false;
        const Page& p = pages_[i];
        if (!p.surface) return true;
        // Mid-resize the old bitmap is scaled; it is redone once things settle.
        if (resizing) return false;
        return p.surfaceW != p.wPx || p.surfaceScale != ds;
    };
    int pick = -1;
    for (int i = first; i <= last && pick < 0; ++i)
        if (needs(i)) pick = i;
    for (int d = 1; d <= 2 && pick < 0; ++d) {
        if (needs(last + d)) pick = last + d;
        else if (needs(first - d)) pick = first - d;
    }
    if (pick < 0) {
        evictFar(first, last);
        return false;
    }

    Page& p = pages_[pick];
    PopplerPage* pp = poppler_document_get_page(doc_, pick);
    cairo_surface_t* surf = pp ? renderPage(pp, (int)std::ceil(p.wPx * ds)) : nullptr;
    if (pp) g_object_unref(pp);
    if (surf) cairo_surface_set_device_scale(surf, ds, ds);
    if (p.surface) cairo_surface_destroy(p.surface);
    p.surface = surf;
    p.surfaceW = p.wPx;
    p.surfaceScale = ds;
    gtk_widget_queue_draw(p.area);
    return true;
}

void PdfView::evictFar(int first, int last) {
    for (int i = 0; i < (int)pages_.size(); ++i) {
        if (i >= first - kKeep && i <= last + kKeep) continue;
        Page& p = pages_[i];
        if (p.surface) {
            cairo_surface_destroy(p.surface);
            p.surface = nullptr;
        }
    }
}

void PdfView::drawPage(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "minicode-page"));
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
    if (i < 0 || i >= (int)self->pages_.size()) return;
    const Page& p = self->pages_[i];
    if (!p.surface) {
        self->scheduleRender();
        return;
    }
    // The bitmap's logical width is surfaceW (its device scale covers HiDPI);
    // it is stretched only while a fresh one for a new size is on its way.
    cairo_save(cr);
    if (p.surfaceW != w) {
        const double k = (double)w / p.surfaceW;
        cairo_scale(cr, k, k);
    }
    cairo_set_source_surface(cr, p.surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}

// ---------------------------------------------------------------- signals

// The adjustment changes while the scroller is being allocated, and new page
// sizes set from inside an allocation are not laid out again (the box kept
// its old width, found by a resize test). So the relayout waits for an idle
// that runs before the next frame is drawn.
void PdfView::onHadjChanged(GtkAdjustment* adj, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    const double w = gtk_adjustment_get_page_size(adj);
    if (std::fabs(w - self->lastWidth_) < 0.5 || self->pages_.empty()) return;
    if (self->relayoutIdle_) return;
    self->relayoutIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 10, [](gpointer p) -> gboolean {
        static_cast<PdfView*>(p)->relayoutIdle_ = 0;
        static_cast<PdfView*>(p)->widthChanged();
        return G_SOURCE_REMOVE;
    }, self, nullptr);
}

void PdfView::widthChanged() {
    if (std::fabs(availableWidth() - lastWidth_) < 0.5 || pages_.empty()) return;
    const bool had = lastWidth_ > 2 * kMargin + 1;
    const Anchor a = anchor();
    relayout();
    if (had && !hasPending_) scrollTo(a);
    else applyPendingAnchor();
    // Scale the bitmaps while the width is moving; re-render when it stops.
    if (resizeTimer_) g_source_remove(resizeTimer_);
    resizeTimer_ = g_timeout_add(kResizeSettleMs, [](gpointer p) -> gboolean {
        PdfView* self = static_cast<PdfView*>(p);
        self->resizeTimer_ = 0;
        self->scheduleRender();
        return G_SOURCE_REMOVE;
    }, this);
    scheduleRender();   // pages with no bitmap at all still get one now
}

void PdfView::onVadjChanged(GtkAdjustment*, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    self->applyPendingAnchor();
    self->scheduleRender();
}

void PdfView::onScrolled(GtkAdjustment*, gpointer selfp) {
    static_cast<PdfView*>(selfp)->scheduleRender();
}

void PdfView::onScaleChanged(GObject*, GParamSpec*, gpointer selfp) {
    static_cast<PdfView*>(selfp)->scheduleRender();
}

bool PdfView::pageAtPoint(double x, double y, int* page, double* px, double* py) const {
    const graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y);
    for (int i = 0; i < (int)pages_.size(); ++i) {
        const Page& p = pages_[i];
        graphene_point_t out;
        if (!gtk_widget_compute_point(scroller_, p.area, &in, &out)) continue;
        if (out.x < 0 || out.y < 0 || out.x >= p.wPx || out.y >= p.hPx) continue;
        if (page) *page = i;
        if (px) *px = out.x * p.wPts / p.wPx;
        if (py) *py = out.y * p.hPts / p.hPx;
        return true;
    }
    return false;
}

void PdfView::onPressed(GtkGestureClick*, int n, double x, double y, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    if (!self->clickCb_) return;
    int page;
    double px, py;
    if (self->pageAtPoint(x, y, &page, &px, &py))
        self->clickCb_(self->clickUser_, page, px, py, n);
}

#endif  // MINICODE_ENABLE_PDF
