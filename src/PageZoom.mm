// PageZoom.mm — see PageZoom.h.
#import "PageZoom.h"
#import "Browser.h"
#import "Latex.h"

const double kMCZoomFitMin = 0.25;

// Safari's View > Zoom In and Zoom Out.
static const double kSteps[] = {0.5, 0.67, 0.75, 0.85, 1.0, 1.15, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};

double MCZoomStep(double zoom, int dir) {
    const size_t n = sizeof kSteps / sizeof kSteps[0];
    // Past an end (Zoom to Fit Width can go below the first step) a step
    // away from it stays put.
    if (dir > 0) {
        for (size_t i = 0; i < n; i++)
            if (kSteps[i] > zoom + 0.004) return kSteps[i];
        return MAX(zoom, kSteps[n - 1]);
    }
    if (dir < 0) {
        for (size_t i = n; i-- > 0;)
            if (kSteps[i] < zoom - 0.004) return kSteps[i];
        return MIN(zoom, kSteps[0]);
    }
    return 1.0;
}

// ------------------------------------------------------------ which page
static NSView *PageViewFor(NSView *v) {
    for (; v; v = v.superview) {
        if ([v isKindOfClass:PDFView.class]) return v;
        if ([v isKindOfClass:BrowserView.class]) return v;
        // The LaTeX preview: its toolbar and log count as its PDF.
        if ([v respondsToSelector:@selector(latexPDFView)])
            return [(id)v latexPDFView];
    }
    return nil;
}

NSView *MCPageViewForResponder(NSResponder *r) {
    if ([r isKindOfClass:NSTextView.class] && [(NSTextView *)r isFieldEditor] &&
        [[(NSTextView *)r delegate] isKindOfClass:NSView.class])
        r = (NSView *)[(NSTextView *)r delegate];
    if (![r isKindOfClass:NSView.class]) return nil;
    NSView *page = PageViewFor((NSView *)r);
    return page && !page.hiddenOrHasHiddenAncestor ? page : nil;
}

NSView *MCPageViewAtPoint(NSWindow *window, NSPoint p) {
    NSView *content = window.contentView;
    NSView *hit = [content hitTest:[content.superview convertPoint:p fromView:nil]];
    NSView *page = PageViewFor(hit);
    return page && !page.hiddenOrHasHiddenAncestor ? page : nil;
}

// ------------------------------------------------------ the pointer
static __weak NSView *gPointed;
static __weak NSWindow *gPointedWindow;

void MCPageWatchPointer(void) {
    static id monitor;
    if (monitor) return;
    const NSEventMask mask = NSEventMaskScrollWheel | NSEventMaskLeftMouseDown |
                             NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown;
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *e) {
        // A scroll's momentum keeps coming after the fingers lift; it is
        // the same gesture, landing where it began.
        if (e.type == NSEventTypeScrollWheel && e.momentumPhase != NSEventPhaseNone) return e;
        NSView *page = e.window ? MCPageViewAtPoint(e.window, e.locationInWindow) : nil;
        gPointed = page;
        gPointedWindow = page ? e.window : nil;
        return e;
    }];
}

NSView *MCPagePointedAt(NSWindow *window) {
    NSView *page = gPointed;
    if (!page || !window || gPointedWindow != window || page.window != window ||
        page.hiddenOrHasHiddenAncestor)
        return nil;
    return page;
}

void MCPageForgetPointer(void) {
    gPointed = nil;
    gPointedWindow = nil;
}

// ------------------------------------------------------------ PDFs
// The page on show, with its size as drawn (rotation applied).
static NSSize PageSize(PDFView *v, PDFPage **pageOut) {
    PDFPage *page = v.currentPage ?: (v.document.pageCount ? [v.document pageAtIndex:0] : nil);
    if (pageOut) *pageOut = page;
    if (!page) return NSZeroSize;
    NSSize s = [page boundsForBox:v.displayBox].size;
    if (page.rotation % 180 != 0) s = NSMakeSize(s.height, s.width);
    return s;
}

// The room for a page across and down, inside the margins PDFKit keeps
// around pages.
static NSSize Room(PDFView *v) {
    NSSize room = v.bounds.size;
    NSEdgeInsets m = v.pageBreakMargins;
    room.width -= m.left + m.right;
    room.height -= m.top + m.bottom;
    // A vertical scroller that takes room (not an overlay one).
    if ([NSScroller preferredScrollerStyle] == NSScrollerStyleLegacy)
        room.width -= [NSScroller scrollerWidthForControlSize:NSControlSizeRegular
                                                scrollerStyle:NSScrollerStyleLegacy];
    return NSMakeSize(MAX(1, room.width), MAX(1, room.height));
}

static void SetScale(PDFView *v, CGFloat scale) {
    PDFDestination *was = v.currentDestination;
    v.autoScales = NO;
    v.scaleFactor = MAX(v.minScaleFactor, MIN(v.maxScaleFactor, scale));
    if (was.page) [v goToDestination:was];
}

void MCPDFZoom(PDFView *v, int dir) {
    if (!v.document) return;
    SetScale(v, dir == 0 ? 1.0 : MCZoomStep(v.scaleFactor, dir));
}

void MCPDFFitWidth(PDFView *v) {
    if (!v.document) return;
    PDFDestination *was = v.currentDestination;
    v.autoScales = YES;   // continuous: as wide as the view, following it
    if (was.page) [v goToDestination:was];
}

void MCPDFFitPage(PDFView *v) {
    PDFPage *page = nil;
    const NSSize size = PageSize(v, &page);
    if (!page || size.width <= 0 || size.height <= 0) return;
    const NSSize room = Room(v);
    v.autoScales = NO;
    v.scaleFactor = MAX(v.minScaleFactor, MIN(v.maxScaleFactor,
                                              MIN(room.width / size.width, room.height / size.height)));
    [v goToPage:page];
}
