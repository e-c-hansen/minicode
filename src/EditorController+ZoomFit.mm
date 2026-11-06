// EditorController+ZoomFit.mm — EditorController: seeing the whole of
// what a pane shows. Page Zoom for the browser's pages and PDFs (Cmd+=,
// Cmd+-, Cmd+0 while one has the keyboard or the pointer, PageZoom.h; Zoom
// to Fit Width and Fit Page from the menu), and word wrap in the editor.
// Maximize Pane is the layout's (EditorController+Layout.mm).
#import "EditorController+Private.h"
#import "Browser.h"
#import "EditorFont.h"
#import "Latex.h"
#import "PageZoom.h"

@implementation EditorController (ZoomFit)

// ---------------------------------------------------------- page zoom
// What Page Zoom acts on: the page with the keyboard, else the one the
// pointer was last in, else (from the menu) the browser if it shows, then
// a PDF or the LaTeX preview in the editor's slot. A BrowserView or a
// PDFView; nil when there is none.
- (NSView *)pageZoomTarget {
    NSView *v = MCPageViewForResponder(self.window.firstResponder);
    if (!v) v = MCPagePointedAt(self.window);
    if (v) return v;
    if (self.browser && !self.browser.hiddenOrHasHiddenAncestor) return self.browser;
    if (self.isPDF && self.pdfView && !self.pdfView.hiddenOrHasHiddenAncestor) return self.pdfView;
    PDFView *latex = self.latex.latexPDFView;
    if (latex.document && !latex.hiddenOrHasHiddenAncestor) return latex;
    return nil;
}

- (void)notePDFZoom:(PDFView *)pdf {
    [self showStatusNote:[NSString stringWithFormat:@"Zoom %.0f%%", round(pdf.scaleFactor * 100)]];
}

- (void)zoomPage:(int)dir {
    NSView *t = [self pageZoomTarget];
    if ([t isKindOfClass:BrowserView.class]) {
        [(BrowserView *)t zoomStep:dir];
    } else if ([t isKindOfClass:PDFView.class]) {
        PDFView *pdf = (PDFView *)t;
        const CGFloat was = pdf.scaleFactor;
        MCPDFZoom(pdf, dir);
        if (dir != 0 && fabs(pdf.scaleFactor - was) < 0.001) NSBeep();
        [self notePDFZoom:pdf];
    } else {
        NSBeep();
    }
}

- (void)zoomPageIn:(id)sender { [self zoomPage:1]; }
- (void)zoomPageOut:(id)sender { [self zoomPage:-1]; }
- (void)zoomPageToActualSize:(id)sender { [self zoomPage:0]; }

- (void)zoomPageToFitWidth:(id)sender {
    NSView *t = [self pageZoomTarget];
    if ([t isKindOfClass:BrowserView.class]) {
        [(BrowserView *)t zoomToFitWidth];
    } else if ([t isKindOfClass:PDFView.class]) {
        MCPDFFitWidth((PDFView *)t);
        [self notePDFZoom:(PDFView *)t];
    } else {
        NSBeep();
    }
}

// A whole page at once: a PDF's. A web page has no page to fit.
- (void)zoomPageToFit:(id)sender {
    NSView *t = [self pageZoomTarget];
    if (![t isKindOfClass:PDFView.class]) {
        NSBeep();
        return;
    }
    MCPDFFitPage((PDFView *)t);
    [self notePDFZoom:(PDFView *)t];
}

// ---------------------------------------------------------- word wrap
- (void)toggleWordWrap:(id)sender {
    (void)sender;
    MCEditorToggleWordWrap();
    for (NSWindow *w in NSApp.windows)
        if ([w.delegate isKindOfClass:EditorController.class])
            [(EditorController *)w.delegate applyWordWrap];
    [self showStatusNote:MCEditorWordWrap() ? @"Long lines wrap" : @"Long lines run on"];
}

// Wrapping, the text view is exactly as wide as the pane (PanelScrollView
// keeps it so) and its container follows it; not wrapping, the container
// is endless and the view grows to its longest line, never narrower than
// the pane, with a horizontal scroll bar.
- (void)applyWordWrap {
    NSTextView *tv = self.textView;
    if (!tv) return;
    const BOOL wrap = MCEditorWordWrap() || !tv.editable;
    NSTextContainer *tc = tv.textContainer;
    if (wrap == !tv.horizontallyResizable && tc.widthTracksTextView == wrap) return;
    NSScrollView *sv = self.editorScroll;
    if (wrap) {
        tv.horizontallyResizable = NO;
        tv.autoresizingMask = NSViewWidthSizable;
        tc.widthTracksTextView = YES;
        [sv tile];
        tc.size = NSMakeSize(MAX(1, tv.frame.size.width - 2 * tv.textContainerInset.width),
                             CGFLOAT_MAX);
    } else {
        tc.widthTracksTextView = NO;
        tc.size = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
        tv.horizontallyResizable = YES;
        tv.autoresizingMask = NSViewNotSizable;
        [sv tile];
        [tv sizeToFit];
    }
    [sv reflectScrolledClipView:sv.contentView];
}

// ---------------------------------------------------------- the menu
- (BOOL)validateZoomFitItem:(NSMenuItem *)item enabled:(BOOL *)enabled {
    SEL a = item.action;
    if (a == @selector(zoomPageIn:) || a == @selector(zoomPageOut:) ||
        a == @selector(zoomPageToActualSize:) || a == @selector(zoomPageToFitWidth:)) {
        *enabled = [self pageZoomTarget] != nil;
        return YES;
    }
    if (a == @selector(zoomPageToFit:)) {
        *enabled = [[self pageZoomTarget] isKindOfClass:PDFView.class];
        return YES;
    }
    if (a == @selector(toggleWordWrap:)) {
        item.state = MCEditorWordWrap() ? NSControlStateValueOn : NSControlStateValueOff;
        *enabled = YES;
        return YES;
    }
    if (a == @selector(toggleMaximizedPane:)) {
        item.state = self.maximizedPane != MCPaneNone ? NSControlStateValueOn
                                                      : NSControlStateValueOff;
        *enabled = YES;
        return YES;
    }
    return NO;
}

@end
