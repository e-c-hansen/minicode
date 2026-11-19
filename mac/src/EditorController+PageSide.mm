// EditorController+PageSide.mm — EditorController: Open Preview to the Side
// (Control+Command+V) for a LaTeX or an HTML file, the source with its page
// beside it, as Markdown's pane (EditorController+Markdown.mm) puts the
// rendered Markdown there. The source keeps the keyboard.
//
// LaTeX: the page is the tab's own preview (LatexView), moved beside the
// source; it typesets when the file is saved (and when the pane opens or
// the tab shows again), as the full preview does when it shows. A
// double-click on the page edits the source beside it as one undo step
// there (latexDidEditSource:); Command-click shows the source line (the
// controller's latexShowSource:line:column:), and the page keeps the
// caret's line in view (SyncTeX's forward search, quietly).
//
// HTML: the window's page for the side (MCHtmlPage showText:), which shows
// the buffer as typed, again a moment after typing stops, keeping its
// scroll; scripts follow html.scripts as the full page's do.
#import "EditorController+PageSideIvars.h"
#import "EditorTab.h"
#import "FileViewers.h"
#import "HtmlPageView.h"
#import "Latex.h"
#import "PageSidePane.h"

// The HTML page follows typing this long after it stops; LaTeX's page
// follows the caret this long after it settles.
static const NSTimeInterval kHtmlSideDelay = 0.4;
static const NSTimeInterval kLatexFollowDelay = 0.25;

@implementation EditorController (PageSide)

// The active tab wants the page beside its source and has the source on
// show.
- (BOOL)pageSideWanted {
    MCTab *t = _activeTab;
    return t.sidePreview && (t.isLatex || t.viewerKind == MCViewerHtml) && !t.previewMode &&
           !t.showingMessage && !t.isDiff && t.currentPath != nil;
}

// The page is on show beside the active tab's source.
- (BOOL)pageSideShowing {
    return _pageSide.shown && [self pageSideWanted];
}

- (BOOL)pageSideShowsLatex {
    return [self pageSideShowing] && self.isLatex && self.latex != nil;
}

- (NSView *)pageSideDivider { return _pageSide.divider; }

// The HTML page beside the source, made on first use: it reads the
// window's folder, and its links open as the full page's do.
- (MCHtmlPage *)sideHtmlPage {
    if (_sideHtml) return _sideHtml;
    _sideHtml = [[MCHtmlPage alloc] initWithFrame:NSZeroRect];
    __weak EditorController *weakSelf = self;
    _sideHtml.readAccessFolder = ^NSString * { return weakSelf.rootPath; };
    _sideHtml.onOpenLink = ^(NSURL *url) {
        [weakSelf openMarkdownLink:url.absoluteString ?: @""];
    };
    _sideHtml.hidden = YES;
    [self.rightArea addSubview:_sideHtml positioned:NSWindowBelow relativeTo:self.termDivider];
    return _sideHtml;
}

- (MCPageSidePane *)pageSidePane {
    if (_pageSide) return _pageSide;
    _pageSide = [[MCPageSidePane alloc] initInView:self.rightArea];
    __weak EditorController *weakSelf = self;
    _pageSide.onDrag = ^(CGFloat x) { [weakSelf pageSideBarDraggedTo:x]; };
    return _pageSide;
}

// relayoutRightArea: the source's slot, `rect`, split with the page when
// the tab wants it and its text is on show. Returns the source's part.
// Runs after the LaTeX preview was framed for the editor's slot, so its
// frame here wins.
- (NSRect)layoutPageSideIn:(NSRect)rect textShown:(BOOL)textShown {
    NSView *page = nil;
    if (textShown && [self pageSideWanted])
        page = self.isLatex ? (NSView *)self.latex : (NSView *)_sideHtml;
    if (_sideHtml && page != _sideHtml) _sideHtml.hidden = YES;
    if (!page) {
        if (_pageSide) [_pageSide layoutInRect:rect page:nil];
        return rect;
    }
    return [[self pageSidePane] layoutInRect:rect page:page];
}

// The source is on show (refreshDisplay): the page beside it, typeset or
// loaded from the buffer as it is now.
- (void)refreshPageSide {
    if (![self pageSideWanted]) return;
    NSString *text = self.textView.string ?: @"";
    if (self.isLatex) {
        if (!self.latex) [self makeLatexPreview];
        else {
            [self.latex setPath:self.currentPath source:text];
            [self.latex refreshStatus];
        }
    } else {
        [[self sideHtmlPage] showText:text path:self.currentPath];
    }
    [self relayoutRightArea];
}

// The buffer changed (textDidChange:): an HTML page beside it shows the
// new text once typing pauses. LaTeX's page waits for a save.
- (void)pageSideSourceChanged {
    if (![self pageSideShowing] || self.isLatex) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(pageSideShowTypedHtml)
                                               object:nil];
    [self performSelector:@selector(pageSideShowTypedHtml) withObject:nil
               afterDelay:kHtmlSideDelay];
}

- (void)pageSideShowTypedHtml {
    if (![self pageSideShowing] || self.isLatex) return;
    [[self sideHtmlPage] showText:self.textView.string ?: @"" path:self.currentPath];
}

// The caret moved: LaTeX's page keeps its line in view, once it settles.
- (void)pageSideSelectionChanged {
    if (![self pageSideShowsLatex]) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(pageSideFollowCaret)
                                               object:nil];
    [self performSelector:@selector(pageSideFollowCaret) withObject:nil
               afterDelay:kLatexFollowDelay];
}

- (void)pageSideFollowCaret {
    if (![self pageSideShowsLatex]) return;
    NSTextView *tv = self.textView;
    const NSInteger line = [self lineOfCharacter:tv.selectedRange.location inString:tv.string];
    [self.latex keepSourceLineInView:line + 1];
}

// A tab was saved: LaTeX's page beside the source typesets what was saved.
- (void)pageSideSaved:(MCTab *)t {
    if (t != _activeTab || ![self pageSideShowsLatex]) return;
    [self.latex setPath:self.currentPath source:t.savedText ?: self.textView.string];
}

// The text the LaTeX page's double-click must be current with: the
// buffer, while the page sits beside the source; nil otherwise.
- (NSString *)pageSideBufferFor:(MCTab *)t {
    if (t != _activeTab || ![self pageSideShowsLatex] || !self.textView.editable) return nil;
    return self.textView.string;
}

// The active tab leaves the screen: the HTML page beside it stops any
// sound or video (it stays loaded, hidden, for the next showing).
- (void)pageSideTabWillLeave:(MCTab *)t {
    (void)t;
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(pageSideShowTypedHtml)
                                               object:nil];
    if (_sideHtml && !_sideHtml.hidden) [_sideHtml clear];
}

// Control+Command+V again, or the bar dragged to the right edge.
- (void)closePageSide {
    MCTab *t = _activeTab;
    if (!t.sidePreview) return;
    t.sidePreview = NO;
    NSResponder *fr = self.window.firstResponder;
    NSView *page = self.isLatex ? (NSView *)self.latex : (NSView *)_sideHtml;
    const BOOL hadKeyboard = page && [fr isKindOfClass:NSView.class] &&
                             [(NSView *)fr isDescendantOf:page];
    if (_sideHtml) {
        [_sideHtml clear];
        _sideHtml.hidden = YES;
    }
    [self relayoutRightArea];
    if (hadKeyboard) [self.window makeFirstResponder:self.textView];
    [self saveSession];
    if (self.hintsVisible) [self updateHints];
}

- (void)pageSideBarDraggedTo:(CGFloat)x {
    if (![_pageSide dragTo:x]) {
        [self closePageSide];
        return;
    }
    [self relayoutRightArea];
}

@end
