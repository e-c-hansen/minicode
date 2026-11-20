// TypesetPages.h — Objective-C++. The session's web page's LaTeX pages
// drawn as pictures (SessionWeb.h's Options::typesetPageSizes and
// typesetPageRender), so the browser view keeps the reader's place through
// a new typeset: a typeset PDF's pages' sizes, and one page drawn on white
// at a width in pixels as PNG bytes, with CoreGraphics (CGPDFDocument,
// ImageIO), never PDFKit or AppKit, so the drawing runs on the server's
// own threads. Each thread keeps the last document it parsed.
#pragma once

#include <string>
#include <vector>

#include "SessionWeb.h"

// The PDF's pages' sizes in points (the crop box, turned by the page's
// rotation), in order; false when it is not a PDF CoreGraphics reads or
// has no pages. Any thread.
bool MCTypesetPageSizes(const std::string &pdf, std::vector<Shared::Web::PageSize> *sizes);
// Page `page` (from 0) drawn on white, `width` pixels wide, as a PNG; false
// when it cannot be. Any thread.
bool MCTypesetPageRender(const std::string &pdf, int page, int width, std::string *png);
