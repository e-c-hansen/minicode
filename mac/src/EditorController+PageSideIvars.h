// EditorController+PageSideIvars.h — the instance variables only Open
// Preview to the Side for LaTeX and HTML files uses
// (EditorController+PageSide.mm), which imports it, as does
// EditorController.mm, which lays them out. See the rule above the shared
// block in EditorController+Private.h.
#pragma once
#import "EditorController+Private.h"

@class MCPageSidePane, MCHtmlPage;

@interface EditorController () {
    // The split between the source and its page (PageSidePane.h), made on
    // first use; the tabs say whether they want it (MCTab sidePreview).
    MCPageSidePane *_pageSide;
    // An HTML file's page beside its source, showing the buffer as typed:
    // the window's one, as the editor's text view is.
    MCHtmlPage *_sideHtml;
}
@end
