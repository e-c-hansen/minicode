// PdfView.cpp — see PdfView.h.
#ifdef MINICODE_ENABLE_PDF

#include "PdfView.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {
constexpr int    kMargin  = 12;     // around the column of pages, logical px
constexpr int    kSpacing = 12;     // between pages
constexpr int    kKeep    = 3;      // pages kept rendered either side of the view
constexpr guint  kResizeSettleMs = 120;
constexpr guint  kAnchorGiveUpMs = 1000;
constexpr guint  kBadgeMs = 1200;   // how long the zoom level stays up

// 100% is 96 logical pixels per inch, a page at its printed size on a nominal
// display: 4/3 pixels per point, as in browsers and pdf.js.
constexpr double kPxPerPt = 96.0 / 72.0;
constexpr double kZoomSteps[] = {0.25, 1.0 / 3, 0.5, 2.0 / 3, 0.75, 0.9, 1.0, 1.1,
                                 1.25, 1.5,     1.75, 2.0,     2.5,  3.0, 4.0};

// Bitmap limits, in device pixels (4 bytes each). A page whose whole bitmap
// at the size on screen would be bigger than kBaseCap is rendered whole at
// the resolution that fits in kBaseCap (32 MB), which is soft up close, and
// the part of it in view, plus half a view around that, is rendered again at
// full resolution on top, up to kDetailCap (48 MB; a view bigger than that
// gets just the visible part). At fit width a Letter page stays
// under kBaseCap up to about 1240 logical pixels wide on a 2x display, so
// the default view rarely needs a detail at all. Without the cap, one A4
// page at 400% on a 2x display would be a 228 MB surface.
constexpr double kBaseCap   = 8.0e6;
constexpr double kDetailCap = 12.0e6;
// Bytes of page bitmaps to aim for in all: pages either side of the view keep
// theirs only while it stays under this (kKeep at most), so at 300% or 400%
// one or two neighbours do, and at fit width all kKeep.
constexpr double kBudget = 192.0e6;

bool tooBig(int wPx, int hPx, double ds) {
    return (double)wPx * ds * (double)hPx * ds > kBaseCap;
}

// A cairo image as a texture, sharing its pixels: the texture's bytes hold
// the surface and destroy it when the last user lets go.
GdkTexture* textureFrom(cairo_surface_t* s) {
    if (!s) return nullptr;
    cairo_surface_flush(s);
    const int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    const int stride = cairo_image_surface_get_stride(s);
    GBytes* bytes = g_bytes_new_with_free_func(
        cairo_image_surface_get_data(s), (gsize)stride * h,
        [](gpointer p) { cairo_surface_destroy(static_cast<cairo_surface_t*>(p)); }, s);
    // GDK_MEMORY_DEFAULT is cairo's ARGB32: premultiplied, native byte order.
    GdkTexture* t = gdk_memory_texture_new(w, h, GDK_MEMORY_DEFAULT, bytes, stride);
    g_bytes_unref(bytes);
    return t;
}
}  // namespace

// ---------------------------------------------------------------- page widget
//
// A page's placeholder: as big as the page on screen, and its snapshot is the
// view's snapshotPage. The CSS node draws the page's shadow.

G_DECLARE_FINAL_TYPE(MinicodePdfPage, minicode_pdf_page, MINICODE, PDF_PAGE, GtkWidget)

struct _MinicodePdfPage {
    GtkWidget parent_instance;
    PdfView*  view;
    int       index;
    int       width, height;
};

G_DEFINE_FINAL_TYPE(MinicodePdfPage, minicode_pdf_page, GTK_TYPE_WIDGET)

static void minicode_pdf_page_measure(GtkWidget* w, GtkOrientation o, int, int* min, int* nat,
                                      int* minBase, int* natBase) {
    MinicodePdfPage* self = MINICODE_PDF_PAGE(w);
    *min = *nat = o == GTK_ORIENTATION_HORIZONTAL ? self->width : self->height;
    *minBase = *natBase = -1;
}

static void minicode_pdf_page_snapshot(GtkWidget* w, GtkSnapshot* s) {
    MinicodePdfPage* self = MINICODE_PDF_PAGE(w);
    if (self->view)
        self->view->snapshotPage(self->index, s, gtk_widget_get_width(w),
                                 gtk_widget_get_height(w));
}

static void minicode_pdf_page_class_init(MinicodePdfPageClass* klass) {
    GtkWidgetClass* wc = GTK_WIDGET_CLASS(klass);
    wc->measure = minicode_pdf_page_measure;
    wc->snapshot = minicode_pdf_page_snapshot;
}

static void minicode_pdf_page_init(MinicodePdfPage* self) {
    self->view = nullptr;
    self->index = 0;
    self->width = self->height = 1;
}

static void setPageSize(GtkWidget* w, int width, int height) {
    MinicodePdfPage* self = MINICODE_PDF_PAGE(w);
    if (self->width == width && self->height == height) return;
    self->width = width;
    self->height = height;
    gtk_widget_queue_resize(w);
}

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
    // ever grow. At fit width no bar shows; zoomed in, it scrolls sideways.
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), box_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);

    // The zoom level, shown for a moment in the corner after it changes.
    badge_ = gtk_label_new("");
    gtk_widget_add_css_class(badge_, "minicode-zoom-badge");
    gtk_widget_set_halign(badge_, GTK_ALIGN_END);
    gtk_widget_set_valign(badge_, GTK_ALIGN_END);
    gtk_widget_set_margin_end(badge_, 16);
    gtk_widget_set_margin_bottom(badge_, 16);
    gtk_widget_set_can_target(badge_, FALSE);
    gtk_widget_set_visible(badge_, FALSE);

    overlay_ = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay_), scroller_);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), badge_);
    gtk_widget_set_hexpand(overlay_, TRUE);
    gtk_widget_set_vexpand(overlay_, TRUE);

    GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_));
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    g_signal_connect(h, "changed", G_CALLBACK(onHadjChanged), this);
    g_signal_connect(v, "changed", G_CALLBACK(onVadjChanged), this);
    g_signal_connect(v, "value-changed", G_CALLBACK(onScrolled), this);
    g_signal_connect(h, "value-changed", G_CALLBACK(onScrolled), this);
    g_signal_connect(scroller_, "notify::scale-factor", G_CALLBACK(onScaleChanged), this);
    g_signal_connect(scroller_, "map", G_CALLBACK(onMapChanged), this);
    g_signal_connect(scroller_, "unmap", G_CALLBACK(onMapChanged), this);

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(scroller_, GTK_EVENT_CONTROLLER(click));

    // Ctrl+wheel zooms. In the capture phase, so it sees the wheel before the
    // scrolled window's own controller scrolls with it.
    GtkEventController* wheel =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase(wheel, GTK_PHASE_CAPTURE);
    g_signal_connect(wheel, "scroll", G_CALLBACK(onScroll), this);
    gtk_widget_add_controller(scroller_, wheel);
    // Where the pointer is, so a wheel zoom keeps the point under it.
    GtkEventController* motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "enter", G_CALLBACK(onMotion), this);
    g_signal_connect(motion, "motion", G_CALLBACK(onMotion), this);
    g_signal_connect_swapped(motion, "leave", G_CALLBACK(+[](gpointer self) {
        static_cast<PdfView*>(self)->pointerX_ = -1;
        static_cast<PdfView*>(self)->pointerY_ = -1;
    }), this);
    gtk_widget_add_controller(scroller_, motion);
    // A touchpad pinch, or two fingers on a touch screen.
    GtkGesture* pinch = gtk_gesture_zoom_new();
    g_signal_connect(pinch, "begin", G_CALLBACK(onPinchBegin), this);
    g_signal_connect(pinch, "scale-changed", G_CALLBACK(onPinch), this);
    gtk_widget_add_controller(scroller_, GTK_EVENT_CONTROLLER(pinch));

    // The widget belongs to whatever layout it is put in, and may outlive
    // this object; keep a reference so the destructor can detach cleanly.
    g_object_ref_sink(overlay_);
}

PdfView::~PdfView() {
    if (renderIdle_)  g_source_remove(renderIdle_);
    if (resizeTimer_) g_source_remove(resizeTimer_);
    if (anchorTimer_) g_source_remove(anchorTimer_);
    if (relayoutIdle_) g_source_remove(relayoutIdle_);
    if (badgeTimer_) g_source_remove(badgeTimer_);
    if (reallocIdle_) g_source_remove(reallocIdle_);
    renderIdle_ = resizeTimer_ = anchorTimer_ = relayoutIdle_ = badgeTimer_ = reallocIdle_ = 0;
    shownCb_ = nullptr;
    // A render still running on the worker finishes on its own copy of the
    // document and is then dropped: its completion sees `alive` false.
    *alive_ = false;
    clear();
    g_clear_object(&cancel_);
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
    g_object_unref(overlay_);
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
    if (!doc) {
        g_bytes_unref(bytes);
        if (error) *error = err ? err->message : "not a readable PDF";
        g_clear_error(&err);
        return false;
    }
    // Renders in flight belong to the old document; the worker gets its own
    // document of the new bytes.
    cancelJobs();
    if (bytes_) g_bytes_unref(bytes_);
    bytes_ = bytes;
    worker_ = std::make_shared<WorkerDoc>(bytes_);

    const bool keep = keepPosition && doc_ != nullptr;
    // The point at the view's top-left corner, the sideways scroll included.
    const Pending was = keep ? pointAt(0, 0) : Pending{};
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
        // Keep the old bitmaps on screen until the new ones are ready, so a
        // reload does not flash white; a zero scale marks them stale.
        pages_[i].surfaceScale = 0;
        pages_[i].detail.scale = 0;
        pages_[i].failed = false;
        pages_[i].detail.failed = false;
    }
    if (!keep) zoom_ = 0;   // a new document starts at fit width
    relayout();
    if (keep && was.page < n) {
        hold(was);
    } else {
        hasPending_ = false;
        gtk_adjustment_set_value(
            gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)), 0);
        gtk_adjustment_set_value(
            gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)), 0);
    }
    scheduleRender();
    return true;
}

void PdfView::clear() {
    cancelJobs();
    worker_.reset();
    if (bytes_) g_bytes_unref(bytes_);
    bytes_ = nullptr;
    rebuildPages(0);
    if (doc_) g_object_unref(doc_);
    doc_ = nullptr;
    path_.clear();
    hasPending_ = false;
    zoom_ = 0;
    gtk_widget_set_visible(badge_, FALSE);
}

// Grow or shrink the list of placeholders to `count`, reusing the ones there
// are. Reuse is what lets a reload with unchanged page sizes keep the exact
// scroll offset without waiting for a new layout.
void PdfView::rebuildPages(int count) {
    while ((int)pages_.size() > count) {
        Page& p = pages_.back();
        MINICODE_PDF_PAGE(p.area)->view = nullptr;
        gtk_box_remove(GTK_BOX(box_), p.area);
        g_clear_object(&p.tex);
        g_clear_object(&p.detail.tex);
        pages_.pop_back();
    }
    while ((int)pages_.size() < count) {
        Page p;
        p.area = GTK_WIDGET(g_object_new(minicode_pdf_page_get_type(), nullptr));
        gtk_widget_set_halign(p.area, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(p.area, "minicode-pdf-page");
        gtk_box_append(GTK_BOX(box_), p.area);
        pages_.push_back(p);
    }
    // Page structs may have moved; the widgets carry the index, not a pointer.
    for (size_t i = 0; i < pages_.size(); ++i) {
        MinicodePdfPage* w = MINICODE_PDF_PAGE(pages_[i].area);
        w->view = this;
        w->index = (int)i;
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

// At fit width, fit the widest page to the view. Other pages keep their size
// relative to it, so a landscape page in a portrait document is not blown up.
// Zoomed, every page is its size in points times the zoom, whatever the width.
void PdfView::relayout() {
    const double avail = availableWidth();
    lastWidth_ = avail;
    double widest = 0;
    for (const Page& p : pages_) widest = std::max(widest, p.wPts);
    if (zoom_ > 0)
        scale_ = zoom_ * kPxPerPt;
    else
        scale_ = (avail > 2 * kMargin + 1 && widest > 0)
                     ? std::max(0.05, (avail - 2 * kMargin) / widest)
                     : 1.0;   // not laid out yet: one pixel per point until it is
    for (Page& p : pages_) {
        p.wPx = std::max(1, (int)std::lround(p.wPts * scale_));
        p.hPx = std::max(1, (int)std::lround(p.hPts * scale_));
        setPageSize(p.area, p.wPx, p.hPx);
        gtk_widget_queue_draw(p.area);
    }
}

double PdfView::pageTop(int i) const {
    double y = kMargin;
    for (int j = 0; j < i && j < (int)pages_.size(); ++j) y += pages_[j].hPx + kSpacing;
    return y;
}

// The box is as wide as the view less the margins, or as the widest page when
// that is wider (the view then scrolls sideways), and GTK centres each page
// in it, rounding down. Worked out from the width the layout was made for, so
// it describes the layout GTK is about to allocate, not only the one on
// screen; the zoom test checks it against gtk_widget_compute_point.
double PdfView::pageLeft(int i) const {
    if (i < 0 || i >= (int)pages_.size()) return kMargin;
    int widest = 0;
    for (const Page& p : pages_) widest = std::max(widest, p.wPx);
    const int boxW = std::max((int)lastWidth_ - 2 * kMargin, widest);
    return kMargin + (boxW - pages_[i].wPx) / 2;
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

// The part of page i on screen, in the page's logical pixels. False if none.
bool PdfView::visibleRect(int i, double* x, double* y, double* w, double* h) const {
    if (i < 0 || i >= (int)pages_.size()) return false;
    GtkAdjustment* ha = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_));
    GtkAdjustment* va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double hv = gtk_adjustment_get_value(ha), vv = gtk_adjustment_get_value(va);
    const double vw = std::max(1.0, gtk_adjustment_get_page_size(ha));
    const double vh = std::max(1.0, gtk_adjustment_get_page_size(va));
    const Page& p = pages_[i];
    const double px = pageLeft(i), py = pageTop(i);
    const double x0 = std::max(hv, px), x1 = std::min(hv + vw, px + p.wPx);
    const double y0 = std::max(vv, py), y1 = std::min(vv + vh, py + p.hPx);
    if (x1 <= x0 || y1 <= y0) return false;
    *x = x0 - px;
    *y = y0 - py;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

// ---------------------------------------------------------------- scroll position

// Where the view is scrolled to, or where a pending hold is taking it, so a
// zoom step that arrives before the last one was laid out (a pinch sends
// many) starts from where the last one meant to be.
void PdfView::scrollValues(double* h, double* v) const {
    GtkAdjustment* ha = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_));
    GtkAdjustment* va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    *h = gtk_adjustment_get_value(ha);
    *v = gtk_adjustment_get_value(va);
    if (!hasPending_ || pages_.empty()) return;
    const int page = std::min(std::max(pending_.page, 0), (int)pages_.size() - 1);
    const Page& p = pages_[page];
    // Clamped to the content's size in this layout, so a target past the end
    // (zooming out at the bottom of the document) is one that can be reached.
    double contentW, contentH;
    contentSize(&contentW, &contentH);
    const double maxV = std::max(0.0, contentH - gtk_adjustment_get_page_size(va));
    const double maxH = std::max(0.0, contentW - gtk_adjustment_get_page_size(ha));
    *v = std::min(maxV, std::max(0.0, pageTop(page) + pending_.y * p.hPx / p.hPts -
                                          pending_.viewY));
    if (pending_.hasX)
        *h = std::min(maxH, std::max(0.0, pageLeft(page) + pending_.x * p.wPx / p.wPts -
                                              pending_.viewX));
}

// The size of everything in the scrolled window, margins included, as this
// layout will be allocated.
void PdfView::contentSize(double* w, double* h) const {
    int widest = 0;
    for (const Page& q : pages_) widest = std::max(widest, q.wPx);
    *w = std::max((int)lastWidth_ - 2 * kMargin, widest) + 2.0 * kMargin;
    *h = pages_.empty() ? 2.0 * kMargin
                        : pageTop((int)pages_.size() - 1) + pages_.back().hPx + kMargin;
}

// The document point at (x, y) in the viewport. Off a page it is still given
// relative to the page it is beside (the one above a gap, the first page for
// the top margin), in points that may be negative or past the page's edge;
// the mapping back is the same arithmetic, so it holds all the same.
PdfView::Pending PdfView::pointAt(double x, double y) const {
    Pending p;
    p.viewX = x;
    p.viewY = y;
    p.hasX = true;
    if (pages_.empty()) return p;
    double h, v;
    scrollValues(&h, &v);
    const double cx = h + x, cy = v + y;
    double top = kMargin;
    int i = 0;
    for (; i + 1 < (int)pages_.size(); ++i) {
        const double next = top + pages_[i].hPx + kSpacing;
        if (cy < next) break;
        top = next;
    }
    const Page& pg = pages_[i];
    p.page = i;
    p.y = (cy - top) * pg.hPts / pg.hPx;
    p.x = (cx - pageLeft(i)) * pg.wPts / pg.wPx;
    return p;
}

PdfView::Anchor PdfView::anchor() const {
    const Pending p = pointAt(0, 0);
    Anchor a;
    a.page = p.page;
    a.y = p.y;
    return a;
}

void PdfView::scrollTo(const Anchor& a) {
    Pending p;
    p.page = a.page;
    p.y = a.y;
    p.hasX = false;   // the sideways scroll stays as it is
    hold(p);
}

void PdfView::hold(const Pending& p) {
    pending_ = p;
    hasPending_ = true;
    applyPendingAnchor();
    // The target may be past the end of the scroll range until the new sizes
    // are allocated; keep trying as the range grows, but not forever, or a
    // later resize would drag the view back here.
    if (anchorTimer_) g_source_remove(anchorTimer_);
    anchorTimer_ = 0;
    if (hasPending_)
        anchorTimer_ = g_timeout_add(kAnchorGiveUpMs, [](gpointer self) -> gboolean {
            PdfView* view = static_cast<PdfView*>(self);
            view->anchorTimer_ = 0;
            view->hasPending_ = false;
            return G_SOURCE_REMOVE;
        }, this);
}

// Scroll to the pending point. The adjustments' ranges are first raised to
// the new layout's size, which GTK only sets when it allocates the viewport:
// GtkAdjustment emits "changed" when the viewport thaws its notifications,
// after the viewport has placed its child, so a value set from there missed
// that placement, and the re-allocation it asked for could be lost (a zoom
// from 40% to 260% left the pages 485 pixels to one side of where the
// adjustment said). Raised beforehand, the value is in range when GTK
// allocates, and the first frame is right.
void PdfView::applyPendingAnchor() {
    if (!hasPending_ || pages_.empty() || applying_) return;
    applying_ = true;
    double tx, ty;
    scrollValues(&tx, &ty);   // the targets, within the new layout's size
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_));
    double cw, ch;
    contentSize(&cw, &ch);
    if (gtk_adjustment_get_upper(v) < ch) gtk_adjustment_set_upper(v, ch);
    if (pending_.hasX && gtk_adjustment_get_upper(h) < cw) gtk_adjustment_set_upper(h, cw);
    const double maxV = gtk_adjustment_get_upper(v) - gtk_adjustment_get_page_size(v);
    const double maxH = gtk_adjustment_get_upper(h) - gtk_adjustment_get_page_size(h);
    const bool hasX = pending_.hasX;
    bool done = maxV + 0.5 >= ty && (!hasX || maxH + 0.5 >= tx);
    // Done first: setting a value runs value-changed, and scrollValues must
    // then report where the view is, not the target again.
    if (done) {
        hasPending_ = false;
        if (anchorTimer_) g_source_remove(anchorTimer_);
        anchorTimer_ = 0;
    }
    gtk_adjustment_set_value(v, ty);
    if (hasX) gtk_adjustment_set_value(h, tx);
    applying_ = false;
}

// After a value was set from an adjustment's "changed" (see above), make
// sure the viewport places its child again.
void PdfView::reallocateSoon() {
    if (reallocIdle_) return;
    reallocIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 10, [](gpointer p) -> gboolean {
        PdfView* self = static_cast<PdfView*>(p);
        self->reallocIdle_ = 0;
        if (GtkWidget* vp = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(self->scroller_)))
            gtk_widget_queue_allocate(vp);
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

// ---------------------------------------------------------------- zoom

double PdfView::effectiveZoom() const { return scale_ / kPxPerPt; }

void PdfView::setZoom(double zoom, double x, double y) {
    if (!doc_ || pages_.empty()) return;
    zoom = zoom > 0 ? std::min(kMaxZoom, std::max(kMinZoom, zoom)) : 0;
    if ((zoom == 0 && zoom_ == 0) || (zoom > 0 && std::fabs(zoom - zoom_) < 1e-6)) {
        showBadge();   // at a limit already: say so
        return;
    }
    const Pending p = pointAt(x, y);
    zoom_ = zoom;
    relayout();
    hold(p);
    settleLater();
    scheduleRender();
    showBadge();
}

void PdfView::zoomIn() {
    const double e = effectiveZoom();
    double to = kMaxZoom;
    for (double s : kZoomSteps)
        if (s > e * 1.001) { to = s; break; }
    setZoom(to, gtk_widget_get_width(scroller_) / 2.0, gtk_widget_get_height(scroller_) / 2.0);
}

void PdfView::zoomOut() {
    const double e = effectiveZoom();
    double to = kMinZoom;
    for (double s : kZoomSteps)
        if (s < e / 1.001) to = s;
    setZoom(to, gtk_widget_get_width(scroller_) / 2.0, gtk_widget_get_height(scroller_) / 2.0);
}

void PdfView::zoomToFit() {
    setZoom(0, gtk_widget_get_width(scroller_) / 2.0, gtk_widget_get_height(scroller_) / 2.0);
}

void PdfView::showBadge() {
    const int pct = (int)std::lround(effectiveZoom() * 100);
    const std::string text = zoom_ > 0 ? std::to_string(pct) + "%"
                                       : "Fit width, " + std::to_string(pct) + "%";
    gtk_label_set_text(GTK_LABEL(badge_), text.c_str());
    gtk_widget_set_visible(badge_, TRUE);
    if (badgeTimer_) g_source_remove(badgeTimer_);
    badgeTimer_ = g_timeout_add(kBadgeMs, [](gpointer self) -> gboolean {
        PdfView* view = static_cast<PdfView*>(self);
        view->badgeTimer_ = 0;
        gtk_widget_set_visible(view->badge_, FALSE);
        return G_SOURCE_REMOVE;
    }, this);
}

// Ctrl+wheel: a wheel notch is 10%, a touchpad's smooth scroll follows the
// fingers. The point under the pointer stays put.
gboolean PdfView::onScroll(GtkEventControllerScroll* c, double, double dy, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    const GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));
    if (!(state & GDK_CONTROL_MASK) || !self->doc_) return FALSE;
    if (dy == 0) return TRUE;
    const double f = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_WHEEL
                         ? std::pow(1.1, -dy)
                         : std::exp(-dy * 0.01);
    double x = self->pointerX_, y = self->pointerY_;
    if (x < 0 || y < 0) {
        x = gtk_widget_get_width(self->scroller_) / 2.0;
        y = gtk_widget_get_height(self->scroller_) / 2.0;
    }
    self->setZoom(std::min(kMaxZoom, std::max(kMinZoom, self->effectiveZoom() * f)), x, y);
    return TRUE;
}

void PdfView::onMotion(GtkEventControllerMotion*, double x, double y, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    self->pointerX_ = x;
    self->pointerY_ = y;
}

void PdfView::onPinchBegin(GtkGesture*, GdkEventSequence*, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    self->pinchStart_ = self->effectiveZoom();
}

// The scale is relative to the fingers' spread when the pinch began; the
// point between them stays put.
void PdfView::onPinch(GtkGestureZoom* g, double scale, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    double x, y;
    if (!gtk_gesture_get_bounding_box_center(GTK_GESTURE(g), &x, &y)) {
        x = gtk_widget_get_width(self->scroller_) / 2.0;
        y = gtk_widget_get_height(self->scroller_) / 2.0;
    }
    self->setZoom(std::min(kMaxZoom, std::max(kMinZoom, self->pinchStart_ * scale)), x, y);
}

void PdfView::onMapChanged(GtkWidget* w, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    if (gtk_widget_get_mapped(w)) self->scheduleRender();
    if (self->shownCb_) self->shownCb_(self->shownUser_);
}

// ---------------------------------------------------------------- rendering

namespace {
// cairo refuses an image surface wider or taller than this, and hands back a
// surface in an error state with no pixels.
constexpr int kCairoMax = 32767;

// A white ARGB surface, or null when cairo could not make one.
cairo_surface_t* paperSurface(int w, int h) {
    if (w <= 0 || h <= 0 || w > kCairoMax || h > kCairoMax) return nullptr;
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return nullptr;
    }
    return surf;
}

// Render `page` scaled by s, offset so (dx0, dy0) is the surface's corner.
cairo_surface_t* renderRegion(PopplerPage* page, double s, int dx0, int dy0, int w, int h) {
    cairo_surface_t* surf = paperSurface(w, h);
    if (!surf) return nullptr;
    cairo_t* cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1, 1, 1);   // PDFs assume paper underneath
    cairo_paint(cr);
    cairo_translate(cr, -dx0, -dy0);
    cairo_scale(cr, s, s);
    poppler_page_render(page, cr);
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(cr);
    if (!ok) { cairo_surface_destroy(surf); return nullptr; }
    return surf;
}
}  // namespace

cairo_surface_t* PdfView::renderPage(PopplerPage* page, int pixelWidth) {
    double w = 0, h = 0;
    poppler_page_get_size(page, &w, &h);
    // A zero-size MediaBox is laid out as 1 point (load), so render it as one.
    w = std::max(w, 1.0);
    h = std::max(h, 1.0);
    if (pixelWidth <= 0) return nullptr;
    // A very tall or wide page is fitted under cairo's limit, softer than
    // asked for but whole, rather than not at all.
    double pw = std::min<double>(pixelWidth, kCairoMax);
    if (pw * h / w > kCairoMax) pw = kCairoMax * w / h;
    const int iw = std::max(1, (int)std::floor(pw));
    const double s = iw / w;
    const int ih = std::max(1, std::min(kCairoMax, (int)std::ceil(h * s)));
    return renderRegion(page, s, 0, 0, iw, ih);
}

// ---------------------------------------------------------------- the worker

struct PdfView::WorkerDoc {
    GBytes*          bytes = nullptr;
    PopplerDocument* doc = nullptr;     // opened on the worker thread, used only there
    bool             broken = false;    // poppler refused the bytes there
    std::mutex       lock;              // one render at a time
    explicit WorkerDoc(GBytes* b) : bytes(g_bytes_ref(b)) {}
    ~WorkerDoc() {
        if (doc) g_object_unref(doc);
        g_bytes_unref(bytes);
    }
};

// One render: set up on the main thread, drawn on the worker, handed back
// to finishJob on the main thread. The view pointer is used only after
// checking `alive`.
struct PdfView::Job {
    std::shared_ptr<WorkerDoc> worker;
    std::shared_ptr<bool>      alive;
    PdfView* view = nullptr;
    unsigned gen = 0;
    int      page = 0;
    bool     detail = false;
    int      pixelW = 0;                 // whole page: the bitmap's width
    int      dx0 = 0, dy0 = 0, dw = 0, dh = 0;   // detail: its rectangle in device pixels
    double   s = 0;                      // detail: device pixels per point
    int      forW = 0;                   // the page's wPx when asked
    double   ds = 0;                     // the device scale when asked
    cairo_surface_t* surf = nullptr;     // the result, or null if it failed
    ~Job() { if (surf) cairo_surface_destroy(surf); }
};

void PdfView::renderInThread(GTask* task, gpointer, gpointer data, GCancellable* c) {
    Job* job = static_cast<Job*>(data);
    if (!g_cancellable_is_cancelled(c)) {
        WorkerDoc& w = *job->worker;
        std::lock_guard<std::mutex> hold(w.lock);
        if (!w.doc && !w.broken) {
            w.doc = poppler_document_new_from_bytes(w.bytes, nullptr, nullptr);
            w.broken = !w.doc;
        }
        PopplerPage* pp = w.doc ? poppler_document_get_page(w.doc, job->page) : nullptr;
        if (pp && !g_cancellable_is_cancelled(c)) {
            job->surf = job->detail
                ? renderRegion(pp, job->s, job->dx0, job->dy0, job->dw, job->dh)
                : renderPage(pp, job->pixelW);
        }
        if (pp) g_object_unref(pp);
    }
    g_task_return_boolean(task, TRUE);
}

void PdfView::renderDone(GObject*, GAsyncResult* res, gpointer) {
    Job* job = static_cast<Job*>(g_task_get_task_data(G_TASK(res)));
    if (*job->alive) job->view->finishJob(job);
    // The task owns the job (its data's destroy notify) and frees it.
}

void PdfView::finishJob(Job* job) {
    if (job->gen != docGen_) return;   // from a document since replaced
    jobInFlight_ = false;
    const int i = job->page;
    // Dropped if the page was resized, zoomed or rescaled meanwhile; the
    // next pass asks again at the new size.
    if (i < (int)pages_.size() && pages_[i].wPx == job->forW && deviceScale() == job->ds) {
        Page& p = pages_[i];
        if (job->detail) {
            Detail& d = p.detail;
            g_clear_object(&d.tex);
            d.forW = p.wPx;
            d.scale = job->ds;
            d.failed = !job->surf;
            if (job->surf) {
                d.tex = textureFrom(job->surf);
                job->surf = nullptr;
                d.x = job->dx0 / job->ds;
                d.y = job->dy0 / job->ds;
                d.w = job->dw / job->ds;
                d.h = job->dh / job->ds;
            }
        } else {
            g_clear_object(&p.tex);
            p.surfaceW = p.wPx;
            p.surfaceScale = job->ds;
            p.failed = !job->surf;
            if (job->surf) {
                p.tex = textureFrom(job->surf);
                job->surf = nullptr;
            }
        }
        gtk_widget_queue_draw(p.area);
    }
    scheduleRender();
}

void PdfView::cancelJobs() {
    if (cancel_) {
        g_cancellable_cancel(cancel_);
        g_object_unref(cancel_);
    }
    cancel_ = g_cancellable_new();
    ++docGen_;
    jobInFlight_ = false;
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

// The rectangle of page i a detail should cover: what is on screen plus half
// a view each way, cut to the page, or just what is on screen when that
// would pass kDetailCap. False when the page needs no detail.
bool PdfView::wantDetail(int i, double* x, double* y, double* w, double* h) const {
    const Page& p = pages_[i];
    const double ds = deviceScale();
    if (!tooBig(p.wPx, p.hPx, ds)) return false;
    double vx, vy, vw, vh;
    if (!visibleRect(i, &vx, &vy, &vw, &vh)) return false;
    GtkAdjustment* ha = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_));
    GtkAdjustment* va = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double mx = gtk_adjustment_get_page_size(ha) / 2;
    const double my = gtk_adjustment_get_page_size(va) / 2;
    const double x0 = std::max(0.0, vx - mx), y0 = std::max(0.0, vy - my);
    const double x1 = std::min((double)p.wPx, vx + vw + mx);
    const double y1 = std::min((double)p.hPx, vy + vh + my);
    if ((x1 - x0) * ds * (y1 - y0) * ds <= kDetailCap) {
        *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    } else {
        *x = vx; *y = vy; *w = vw; *h = vh;
    }
    return true;
}

// Render the most useful thing that needs it: the visible pages top to
// bottom, then their details, then the pages either side. One piece at a
// time, drawn on a worker thread (renderInThread), so scrolling and typing
// stay responsive while a long or heavy document fills in; its completion
// (finishJob) schedules the next.
bool PdfView::renderOne() {
    if (!doc_ || pages_.empty() || !gtk_widget_get_mapped(scroller_)) return false;
    // One render at a time; its completion calls back here for the next.
    if (jobInFlight_) return false;
    int first, last;
    visibleRange(&first, &last);
    const double ds = deviceScale();
    const bool resizing = resizeTimer_ != 0;
    // Pages either side that keep a bitmap: kKeep, or fewer when pages are
    // big, so the bitmaps stay near kBudget in all.
    double pageBytes = 0;
    for (int i = first; i <= last; ++i)
        pageBytes = std::max(pageBytes, 4 * std::min(kBaseCap, (double)pages_[i].wPx * ds *
                                                                   pages_[i].hPx * ds));
    int keep = kKeep;
    if (pageBytes > 0)
        keep = std::max(0, std::min(kKeep, (int)((kBudget / pageBytes - (last - first + 1)) / 2)));
    // Let go of what is out of range before rendering more.
    evictFar(first - keep, last + keep);
    for (int i = 0; i < (int)pages_.size(); ++i) {
        Detail& d = pages_[i].detail;
        double x, y, w, h;
        if (d.tex && (i < first || i > last || !wantDetail(i, &x, &y, &w, &h))) {
            g_clear_object(&d.tex);
            d = Detail();
            gtk_widget_queue_draw(pages_[i].area);
        }
    }

    auto needs = [&](int i) {
        if (i < 0 || i >= (int)pages_.size()) return false;
        const Page& p = pages_[i];
        const bool current = p.surfaceW == p.wPx && p.surfaceScale == ds;
        // A page that would not render at this size is not asked for again.
        if (!p.tex) return !(p.failed && current);
        // Mid-resize or mid-zoom the old bitmap is scaled; it is redone once
        // things settle.
        if (resizing) return false;
        return !current;
    };
    auto needsDetail = [&](int i) {
        if (resizing) return false;
        double x, y, w, h;
        if (!wantDetail(i, &x, &y, &w, &h)) return false;
        const Detail& d = pages_[i].detail;
        const bool current = d.forW == pages_[i].wPx && d.scale == ds;
        if (!d.tex) return !(d.failed && current);
        if (!current) return true;
        // Redo it once the view has moved past what it covers.
        double vx, vy, vw, vh;
        visibleRect(i, &vx, &vy, &vw, &vh);
        return vx < d.x - 0.5 || vy < d.y - 0.5 || vx + vw > d.x + d.w + 0.5 ||
               vy + vh > d.y + d.h + 0.5;
    };
    int pick = -1;
    bool detail = false;
    for (int i = first; i <= last && pick < 0; ++i)
        if (needs(i)) pick = i;
    for (int i = first; i <= last && pick < 0; ++i)
        if (needsDetail(i)) { pick = i; detail = true; }
    for (int d = 1; d <= std::min(2, keep) && pick < 0; ++d) {
        if (needs(last + d)) pick = last + d;
        else if (needs(first - d)) pick = first - d;
    }
    if (pick < 0) return false;

    const Page& p = pages_[pick];
    auto* job = new Job;
    job->worker = worker_;
    job->alive = alive_;
    job->view = this;
    job->gen = docGen_;
    job->page = pick;
    job->detail = detail;
    job->forW = p.wPx;
    job->ds = ds;
    if (detail) {
        double x, y, w, h;
        wantDetail(pick, &x, &y, &w, &h);
        const int dx0 = (int)std::floor(x * ds), dy0 = (int)std::floor(y * ds);
        const int dx1 = (int)std::ceil((x + w) * ds), dy1 = (int)std::ceil((y + h) * ds);
        job->dx0 = dx0;
        job->dy0 = dy0;
        job->dw = std::max(0, dx1 - dx0);
        job->dh = std::max(0, dy1 - dy0);
        // The same scale as a whole-page bitmap of this width (renderPage),
        // so the detail lines up with the page under it. poppler walks the
        // whole page, but cairo only rasterizes what lands in the surface.
        job->s = p.wPx * ds / p.wPts;
    } else {
        // Full resolution when it fits under kBaseCap; otherwise the widest
        // bitmap that does, drawn at the page's size (softer, and the detail
        // covers the part in view).
        const double full = (double)p.wPx * ds * p.hPx * ds;
        int pw = (int)std::ceil(p.wPx * ds);
        if (full > kBaseCap) pw = std::max(1, (int)std::floor(pw * std::sqrt(kBaseCap / full)));
        job->pixelW = pw;
    }
    jobInFlight_ = true;
    GTask* task = g_task_new(nullptr, cancel_, renderDone, nullptr);
    g_task_set_task_data(task, job, [](gpointer j) { delete static_cast<Job*>(j); });
    g_task_run_in_thread(task, renderInThread);
    g_object_unref(task);
    // Nothing more this pass: the job's completion schedules the next one.
    return false;
}

size_t PdfView::bitmapBytes() const {
    size_t n = 0;
    auto bytes = [](GdkTexture* t) -> size_t {
        return t ? (size_t)gdk_texture_get_width(t) * gdk_texture_get_height(t) * 4 : 0;
    };
    for (const Page& p : pages_) n += bytes(p.tex) + bytes(p.detail.tex);
    return n;
}

// Drop the bitmaps of pages outside [from, to].
void PdfView::evictFar(int from, int to) {
    for (int i = 0; i < (int)pages_.size(); ++i) {
        if (i >= from && i <= to) continue;
        Page& p = pages_[i];
        if (p.tex) {
            g_clear_object(&p.tex);
            // Redrawn, so its render node lets go of the texture too.
            gtk_widget_queue_draw(p.area);
        }
    }
}

void PdfView::snapshotPage(int i, GtkSnapshot* s, int w, int h) {
    static const GdkRGBA white = {1, 1, 1, 1};
    graphene_rect_t all = GRAPHENE_RECT_INIT(0, 0, (float)w, (float)h);
    gtk_snapshot_append_color(s, &white, &all);   // PDFs assume paper underneath
    if (i < 0 || i >= (int)pages_.size()) return;
    const Page& p = pages_[i];
    if (!p.tex) {
        if (p.failed) {
            // poppler could not render it: grey, with a cross, rather than a
            // blank page that looks like it is still coming.
            static const GdkRGBA grey = {0.85f, 0.85f, 0.85f, 1};
            static const GdkRGBA line = {0.6f, 0.6f, 0.6f, 1};
            gtk_snapshot_append_color(s, &grey, &all);
            cairo_t* cr = gtk_snapshot_append_cairo(s, &all);
            gdk_cairo_set_source_rgba(cr, &line);
            cairo_set_line_width(cr, 2);
            cairo_move_to(cr, 0, 0);
            cairo_line_to(cr, w, h);
            cairo_move_to(cr, w, 0);
            cairo_line_to(cr, 0, h);
            cairo_stroke(cr);
            cairo_destroy(cr);
        }
        scheduleRender();
        return;
    }
    // The whole page at the widget's width. Its pixel size is the page's
    // size times the device scale, or less for a reduced one, and GSK scales
    // it; it is also stretched while a fresh one for a new size is on its way.
    const double tw = gdk_texture_get_width(p.tex), th = gdk_texture_get_height(p.tex);
    graphene_rect_t r = GRAPHENE_RECT_INIT(0, 0, (float)w, (float)(w * th / tw));
    gtk_snapshot_append_scaled_texture(s, p.tex, GSK_SCALING_FILTER_LINEAR, &r);

    const Detail& d = p.detail;
    if (d.tex && d.forW > 0) {
        const double k = (double)w / d.forW;
        graphene_rect_t dr = GRAPHENE_RECT_INIT((float)(d.x * k), (float)(d.y * k),
                                                (float)(d.w * k), (float)(d.h * k));
        gtk_snapshot_append_scaled_texture(s, d.tex, GSK_SCALING_FILTER_LINEAR, &dr);
    }
}

// ---------------------------------------------------------------- signals

// The adjustment changes while the scroller is being allocated, and new page
// sizes set from inside an allocation are not laid out again (the box kept
// its old width, found by a resize test). So the relayout waits for an idle
// that runs before the next frame is drawn.
void PdfView::onHadjChanged(GtkAdjustment* adj, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    if (self->hasPending_) {
        self->applyPendingAnchor();   // a zoom's sideways position, once it fits
        self->reallocateSoon();
    }
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
    // Zoomed, the pages keep their size and move sideways to stay centred, so
    // the whole top-left point is held; at fit width only the vertical one.
    const Pending p = pointAt(0, 0);
    relayout();
    if (had && !hasPending_) {
        if (zoom_ > 0) {
            hold(p);
        } else {
            Anchor a;
            a.page = p.page;
            a.y = p.y;
            scrollTo(a);
        }
    } else {
        applyPendingAnchor();
    }
    settleLater();
    scheduleRender();   // pages with no bitmap at all still get one now
}

// Scale the bitmaps while the width or zoom is moving; re-render when it stops.
void PdfView::settleLater() {
    if (resizeTimer_) g_source_remove(resizeTimer_);
    resizeTimer_ = g_timeout_add(kResizeSettleMs, [](gpointer p) -> gboolean {
        PdfView* self = static_cast<PdfView*>(p);
        self->resizeTimer_ = 0;
        self->scheduleRender();
        return G_SOURCE_REMOVE;
    }, this);
}

void PdfView::onVadjChanged(GtkAdjustment*, gpointer selfp) {
    PdfView* self = static_cast<PdfView*>(selfp);
    if (self->hasPending_) {
        self->applyPendingAnchor();
        self->reallocateSoon();
    }
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

bool PdfView::pagePointIn(GtkWidget* target, int page, double px, double py,
                          double* x, double* y) const {
    if (page < 0 || page >= (int)pages_.size()) return false;
    const Page& p = pages_[page];
    const graphene_point_t in = GRAPHENE_POINT_INIT((float)(px * p.wPx / p.wPts),
                                                    (float)(py * p.hPx / p.hPts));
    graphene_point_t out;
    if (!gtk_widget_compute_point(p.area, target, &in, &out)) return false;
    *x = out.x;
    *y = out.y;
    return true;
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
