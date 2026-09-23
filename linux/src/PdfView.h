// PdfView.h — every page of a PDF in one vertical scrolling list, drawn with
// poppler-glib. Used for PDFs opened in the editor's slot, and meant to be
// reused by the LaTeX preview, which is why it exposes page geometry, a
// click-to-page-point mapping and a reload that keeps the scroll position.
//
// Pages are fitted to the width of the view. Nothing is rendered up front:
// each page is a placeholder of the right size, and an idle handler renders
// the ones on screen (then a couple either side), one page per idle pass, at
// the display's scale factor so text is sharp on HiDPI. Pages far from the
// viewport give their bitmaps back, so a long document does not hold every
// page in memory.
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

#include <string>
#include <vector>

class PdfView {
public:
    PdfView();
    ~PdfView();
    PdfView(const PdfView&) = delete;
    PdfView& operator=(const PdfView&) = delete;

    // The scrolled widget to put in the layout.
    GtkWidget* widget() const { return scroller_; }

    // Load a PDF from disk. The bytes are read into memory first, so a file
    // rewritten while it is shown (tectonic, a build in the terminal) cannot
    // pull the data out from under poppler. With keepPosition the point at the
    // top of the viewport stays there, as long as its page still exists.
    // On failure the current document is left showing, and false is returned
    // with a reason in *error if given.
    bool load(const std::string& path, bool keepPosition, std::string* error = nullptr);
    // Drop the document and every page widget.
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

    // Render one page into a new ARGB image `pixelWidth` pixels wide, white
    // background included. The caller destroys it. The view uses this itself;
    // it is public for tests and for anything that needs a page as a bitmap.
    static cairo_surface_t* renderPage(PopplerPage* page, int pixelWidth);

private:
    struct Page {
        GtkWidget*       area = nullptr;      // GtkDrawingArea placeholder
        double           wPts = 0, hPts = 0;  // page size in points
        int              wPx = 0, hPx = 0;    // display size in logical pixels
        cairo_surface_t* surface = nullptr;   // rendered bitmap, or null
        int              surfaceW = 0;        // the wPx it was rendered for
        double           surfaceScale = 0;    // the device scale it was rendered at
    };

    void rebuildPages(int count);
    void relayout();                   // sizes from the current width
    void widthChanged();               // relayout, keeping the anchor
    double availableWidth() const;
    double deviceScale() const;
    double pageTop(int i) const;       // y of page i in content coordinates
    void visibleRange(int* first, int* last) const;
    void scheduleRender();
    bool renderOne();                  // one page per call; false when done
    void evictFar(int first, int last);
    void applyPendingAnchor();

    static void drawPage(GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer self);
    static void onHadjChanged(GtkAdjustment* adj, gpointer self);
    static void onVadjChanged(GtkAdjustment* adj, gpointer self);
    static void onScrolled(GtkAdjustment* adj, gpointer self);
    static void onPressed(GtkGestureClick* g, int n, double x, double y, gpointer self);
    static void onScaleChanged(GObject* obj, GParamSpec* pspec, gpointer self);

    GtkWidget* scroller_ = nullptr;
    GtkWidget* box_      = nullptr;

    PopplerDocument*  doc_ = nullptr;
    std::string       path_;
    std::vector<Page> pages_;

    double lastWidth_   = 0;      // available width the layout was made for
    double scale_       = 1.0;    // logical pixels per PDF point
    guint  renderIdle_  = 0;
    guint  resizeTimer_ = 0;      // re-render settles after a resize
    guint  relayoutIdle_ = 0;     // a width change waiting to be laid out
    guint  anchorTimer_ = 0;      // gives up on a pending anchor
    bool   hasPending_  = false;
    Anchor pending_;

    ClickCb clickCb_   = nullptr;
    void*   clickUser_ = nullptr;
};

#endif  // MINICODE_ENABLE_PDF
