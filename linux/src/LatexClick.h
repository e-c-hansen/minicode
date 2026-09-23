// LatexClick.h — the poppler and GIO half of the LaTeX preview's click path:
// a page's text with a box per character, and the gzipped SyncTeX file read
// into memory. The rest of the path is pure C++ (PageWords.h, and LatexDoc
// and SyncTex in ../src). Used by Latex.cpp and by the sweep harness
// (tests/latex_sweep.cpp), so both click through exactly the same code.
//
// Compiled only with MINICODE_ENABLE_PDF.
#pragma once

#ifdef MINICODE_ENABLE_PDF

#include <poppler.h>
#include <string>

#include "PageWords.h"

// poppler_page_get_text and poppler_page_get_text_layout, as a PageText.
PageText pageTextOf(PopplerPage* page);

// The whole of a gzip file, decompressed; empty if it cannot be read.
std::string readGzipFile(const std::string& path);

#endif  // MINICODE_ENABLE_PDF
