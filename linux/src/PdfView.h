// PdfView.h — every page of a PDF in one vertical scrolling list, drawn with
// poppler-glib. Used for PDFs opened in the editor's slot, and meant to be
// reused by the LaTeX preview, which is why it exposes page geometry, a
// click-to-page-point mapping and a reload that keeps the scroll position.
//
// Pages are fitted to the width of the view until the user zooms (see
// "Zoom" below); then they have a fixed size and the view scrolls both ways.
// Nothing is rendered up front: each page is a placeholder widget of the
// right size, and an idle handler picks the ones on screen (then a couple
// either side), one page at a time, at the display's scale factor so text
// is sharp on HiDPI. The rendering itself runs on a worker thread, against a
// second PopplerDocument opened from the same bytes (a PopplerDocument is not
// safe to share between threads, two documents are), so a heavy page at 400%
// never freezes typing or scrolling; the bitmap comes back to the main loop
// and is dropped if the document, size or zoom changed meanwhile. A page
// poppler cannot render is marked failed and drawn as a grey placeholder, so
// the loop moves on instead of asking for it again forever. Pages far from the viewport give their bitmaps back, so
// a long document does not hold every page in memory. A page too big to hold
// as one bitmap at the zoom asked for is kept as a smaller whole-page bitmap
// plus a sharp one of just the part in view (the limits are at the top of
// PdfView.cpp).
//
// Bitmaps are drawn as GdkTextures, not with cairo in a GtkDrawingArea. A
// drawing area records into a cairo recording surface, which keeps a
// copy-on-write snapshot of the bitmap it was given; every page ever drawn
// keeps its last render node, so each bitmap let go was copied into that node
// and stayed there (1.5 GB after scrolling a 400% document). A texture is
// shared by reference, and the page is redrawn when its bitmap goes, so the
// node lets go of it too.
//
// Coordinates: page indices are 0-based (SyncTeX's are 1-based, add one).
// Points on a page are PDF points (1/72 inch) from the page's top-left, y
// growing downwards, which is both poppler's convention and SyncTeX's.
//
// Compiled only with MINICODE_ENABLE_PDF (poppler-glib found by Meson, or
// `make PDF=1`).
#pragma once

#ifdef MINICODE_ENABLE_PDF

#include <gtk/gtk.h>
#include <poppler.h>

#include <memory>
#include <string>
#include <vector>

class PdfView {
public:
    PdfView();
    ~PdfView();
    PdfView(const PdfView&) = delete;
    PdfView& operator=(const PdfView&) = delete;

    // The widget to put in the layout: the scrolled window, in an overlay
    // that carries the zoom badge. Both have the same origin, so "widget()
    // coordinates" below are the scrolled window's too.
    GtkWidget* widget() const { return overlay_; }

    // Load a PDF from disk. The bytes are read into memory first, so a file
    // rewritten while it is shown (tectonic, a build in the terminal) cannot
    // pull the data out from under poppler. With keepPosition the point at the
    // top of the viewport stays there, as long as its page still exists.
    // On failure the current document is left showing, and false is returned
    // with a reason in *error if given. keepPosition also keeps the zoom;
    // without it the view goes back to fit width.
    bool load(const std::string& path, bool keepPosition, std::string* error = nullptr);
    // Drop the document and every page widget, and go back to fit width.
    void clear();

    bool hasDocument() const { return doc_ != nullptr; }
    const std::string& path() const { return path_; }
    PopplerDocument* document() const { return doc_; }   // owned by the view
    int pageCount() const { return (int)pages_.size(); }
    // A page's size in PDF points.
    bool pageSize(int page, double* width, double* height) const;

    // Map a point in widget() coordinates onto a page. False in the gaps
    // between pages and in the margins.
    bool pageAtPoint(double x, double y, int* page, double* px, double* py) const;
    // The other way: a point on a page (in points) in `target`'s coordinates,
    // for anchoring something to text on the page. False if the page does
    // not exist or is not laid out yet.
    bool pagePointIn(GtkWidget* target, int page, double px, double py,
                     double* x, double* y) const;

    // The scroll position as a document point: the page and the y (in points)
    // at the top edge of the viewport. scrollTo restores one, and holds it
    // through the relayout that follows a load or a resize.
    struct Anchor { int page = 0; double y = 0; };
    Anchor anchor() const;
    void scrollTo(const Anchor& a);

    // Called on every press on a page, with the press count (2 for a
    // double-click). The LaTeX preview's double-click-to-edit hangs off this.
    using ClickCb = void (*)(void* user, int page, double x, double y, int nPress);
    void setClickCallback(ClickCb cb, void* user) { clickCb_ = cb; clickUser_ = user; }

    // Zoom. 0 means fit to width, the default and what every newly opened
    // file gets. Otherwise a factor where 1.0 is 100%: a page at its printed
    // size on a nominal 96 dpi display, 4/3 logical pixels per point, as in
    // browsers and pdf.js. Kept by load(path, true), so a LaTeX retypeset and
    // a reload from disk keep it; reset by clear() and by a load without
    // keepPosition. Every call is a no-op without a document.
    static constexpr double kMinZoom = 0.25;
    static constexpr double kMaxZoom = 4.0;
    double zoom() const { return zoom_; }
    // The factor on screen, what fit width comes to included.
    double effectiveZoom() const;
    // Set the zoom (clamped; 0 for fit width) keeping the document point
    // under (x, y), in widget() coordinates, where it is on screen.
    void setZoom(double zoom, double x, double y);
    // The keys: the next step of 25, 33, 50, 67, 75, 90, 100, 110, 125, 150,
    // 175, 200, 250, 300, 400%, or back to fit width, about the view's centre.
    void zoomIn();
    void zoomOut();
    void zoomToFit();

    // Called when widget() is mapped or unmapped, i.e. when the view starts
    // or stops being on screen, so the shell can enable the zoom keys.
    using ShownCb = void (*)(void* user);
    void setShownCallback(ShownCb cb, void* user) { shownCb_ = cb; shownUser_ = user; }
    bool shown() const { return gtk_widget_get_mapped(scroller_); }

    // The bytes held in page bitmaps right now, for tests.
    size_t bitmapBytes() const;

    // Render one page into a new ARGB image `pixelWidth` pixels wide, white
    // background included. The caller destroys it. The view uses this itself;
    // it is public for tests and for anything that needs a page as a bitmap.
    static cairo_surface_t* renderPage(PopplerPage* page, int pixelWidth);

    // Draws page i into its widget, w by h logical pixels. Called by the page
    // widgets' snapshot, and by nothing else.
    void snapshotPage(int i, GtkSnapshot* s, int w, int h);

private:
    // Part of a page rendered at full resolution, for a page whose whole
    // bitmap had to be rendered at less (see renderOne). Its rectangle is in
    // the page's logical pixels at the wPx it was made for.
    struct Detail {
        GdkTexture* tex = nullptr;
        double x = 0, y = 0, w = 0, h = 0;
        int    forW = 0;
        double scale = 0;
        bool   failed = false;            // could not be rendered at forW/scale
    };
    struct Page {
        GtkWidget*  area = nullptr;       // the page's placeholder widget
        double      wPts = 0, hPts = 0;   // page size in points
        int         wPx = 0, hPx = 0;     // display size in logical pixels
        GdkTexture* tex = nullptr;        // rendered bitmap of the whole page, or null
        int         surfaceW = 0;         // the wPx it was rendered for
        double      surfaceScale = 0;     // the device scale it was rendered at
        bool        failed = false;       // poppler could not render it at surfaceW/Scale
        Detail      detail;
    };
    // A document point held in place on screen: the page, a point on it in
    // PDF points, and where in the viewport it goes. With hasX false only the
    // vertical scroll is set.
    struct Pending {
        int    page = 0;
        double x = 0, y = 0;
        double viewX = 0, viewY = 0;
        bool   hasX = false;
    };

    void rebuildPages(int count);
    void relayout();                   // sizes from the current width
    void widthChanged();               // relayout, keeping the anchor
    double availableWidth() const;
    double deviceScale() const;
    double pageTop(int i) const;       // y of page i in content coordinates
    double pageLeft(int i) const;      // x of page i in content coordinates
    void scrollValues(double* h, double* v) const;   // where the view is, or is going
    void contentSize(double* w, double* h) const;    // the whole column, as laid out
    void reallocateSoon();
    Pending pointAt(double x, double y) const;       // x, y in viewport coordinates
    void hold(const Pending& p);       // scroll so p is where it says, and keep it there
    bool visibleRect(int i, double* x, double* y, double* w, double* h) const;
    bool wantDetail(int i, double* x, double* y, double* w, double* h) const;
    void settleLater();                // re-render once resizing or zooming stops
    void showBadge();
    void visibleRange(int* first, int* last) const;
    void scheduleRender();
    bool renderOne();                  // starts one render; false when none started
    struct Job;
    static void renderInThread(GTask* task, gpointer source, gpointer data, GCancellable* c);
    static void renderDone(GObject* source, GAsyncResult* res, gpointer data);
    void finishJob(Job* job);
    void cancelJobs();                 // a new document, or none: drop what is in flight
    void evictFar(int first, int last);
    void applyPendingAnchor();

    static void onHadjChanged(GtkAdjustment* adj, gpointer self);
    static void onVadjChanged(GtkAdjustment* adj, gpointer self);
    static void onScrolled(GtkAdjustment* adj, gpointer self);
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onScaleChanged(GObject* obj, GParamSpec* pspec, gpointer self);
    static gboolean onScroll(GtkEventControllerScroll* c, double dx, double dy, gpointer self);
    static void onMotion(GtkEventControllerMotion* c, double x, double y, gpointer self);
    static void onPinchBegin(GtkGesture* g, GdkEventSequence* seq, gpointer self);
    static void onPinch(GtkGestureZoom* g, double scale, gpointer self);
    static void onMapChanged(GtkWidget* w, gpointer self);

    GtkWidget* overlay_  = nullptr;
    GtkWidget* scroller_ = nullptr;
    GtkWidget* box_      = nullptr;
    GtkWidget* badge_    = nullptr;   // "150%", shown for a moment after a zoom

    PopplerDocument*  doc_ = nullptr;
    GBytes*           bytes_ = nullptr;   // the file, shared with the worker's document
    // The worker's own document, opened from bytes_ on the worker thread and
    // used only there; shared with jobs so it outlives a view destroyed while
    // a render is running.
    struct WorkerDoc;
    std::shared_ptr<WorkerDoc> worker_;
    GCancellable*     cancel_ = nullptr;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    unsigned          docGen_ = 0;        // bumped by every load and clear
    bool              jobInFlight_ = false;
    std::string       path_;
    std::vector<Page> pages_;

    double lastWidth_   = 0;      // available width the layout was made for
    double scale_       = 1.0;    // logical pixels per PDF point
    double zoom_        = 0;      // 0: fit width
    double pinchStart_  = 1.0;    // effectiveZoom() when a pinch began
    double pointerX_    = -1, pointerY_ = -1;   // last pointer position, viewport coords
    guint  badgeTimer_  = 0;
    guint  reallocIdle_ = 0;
    bool   applying_    = false;  // inside applyPendingAnchor
    guint  renderIdle_  = 0;
    guint  resizeTimer_ = 0;      // re-render settles after a resize
    guint  relayoutIdle_ = 0;     // a width change waiting to be laid out
    guint  anchorTimer_ = 0;      // gives up on a pending anchor
    bool   hasPending_  = false;
    Pending pending_;

    ClickCb clickCb_   = nullptr;
    void*   clickUser_ = nullptr;
    ShownCb shownCb_   = nullptr;
    void*   shownUser_ = nullptr;
};

#endif  // MINICODE_ENABLE_PDF
