// PageZoom.h — Objective-C++. Zooming what shows a page rather than text:
// the browser pane's web pages and PDFs (a PDF file in the editor's slot,
// the LaTeX preview). Cmd+=, Cmd+- and Cmd+0 zoom one while it has the
// keyboard, or while the pointer was last there: a scroll or a click in it
// with no key pressed since (the registry's Page context, Commands.h). The
// steps are Safari's, from 50% to 300%.
#import <Cocoa/Cocoa.h>
#import <Quartz/Quartz.h>

// The next zoom after `zoom` going up (dir > 0) or down (dir < 0), on
// Safari's steps, 50% to 300%; a zoom between steps goes to the next step
// that way, and one past an end stays where it is. dir 0 is 100%.
double MCZoomStep(double zoom, int dir);
// Zoom to Fit Width goes no smaller than this.
extern const double kMCZoomFitMin;

// The page view holding `responder` (a field editor counts as its field):
// a BrowserView, a PDFView, or the LaTeX preview (a LatexView). nil when it
// is in none.
NSView *MCPageViewForResponder(NSResponder *responder);
// The page view at a point of a window (in its coordinates), as above.
NSView *MCPageViewAtPoint(NSWindow *window, NSPoint inWindow);

// Watches scrolls and clicks in every window from now on (once is enough).
void MCPageWatchPointer(void);
// The page view a scroll or click last landed in, in `window`, if no key
// was pressed since and it still shows; nil otherwise.
NSView *MCPagePointedAt(NSWindow *window);
// A key was pressed that did not zoom a page: the pointer no longer counts.
void MCPageForgetPointer(void);

// A PDF view: a step in or out (dir 0: actual size), the width of its
// widest page, or the whole of the page on show. Each leaves the view
// scaled by hand (autoScales off), except fit width, which is PDFKit's own
// automatic scale in continuous mode, so it follows the pane's width.
void MCPDFZoom(PDFView *view, int dir);
void MCPDFFitWidth(PDFView *view);
void MCPDFFitPage(PDFView *view);
// Overlay scroll bars in a PDF view. PDFKit's automatic scale fits a page
// to the whole view and leaves out a scroll bar that takes room (a Mac
// with no trackpad, or "Show scroll bars: Always"), so the page's right
// edge went under it. Called again on every layout, since AppKit puts
// back the system's style when that setting changes.
void MCPDFUseOverlayScrollers(PDFView *view);
// Scroll to the top of the document (the first page's top edge with the
// page-break margin above it), with no automatic content insets, as
// Preview opens a PDF. PDFView alone left a new document where its scroll
// view's insets and the first rescale to the pane's width put it: part way
// down a tall page, which showed only its foot.
void MCPDFShowFirstPageTop(PDFView *view);
// PDFKit's page analysis only for a scan. A PDFView with a document runs
// Vision over each page it shows (text recognition, tables, forms): a
// machine-learning model compiled and a whole core busy for two seconds
// and more on opening even a two-page PDF of plain text, and again on every
// reload (each LaTeX typeset). The taskbench timed it as a PDF taking 2.2 s
// to open (2026-11-20; the page itself drew in about 30 ms). It only gives
// selectable text to a page with none of its own, so it stays on when the
// first page has no text (a scanned PDF) and is off otherwise. Call before
// handing the view the document.
BOOL MCPDFAnalysesPages(PDFDocument *doc);
void MCPDFAnalyseOnlyScans(PDFView *view, PDFDocument *doc);
