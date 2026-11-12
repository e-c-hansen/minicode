// Math.h — the Markdown preview's math typeset by tectonic, as the Mac's
// MarkdownMath.mm does (CLAUDE.md, "Math in the Markdown preview"). The TeX
// side (which formulas may go in a batch, the document, the nonce, the log's
// boxes) is the core's MathTex; this is the running of tectonic and the
// keeping and drawing of the pictures.
//
// Formulas asked for by a page (want()) are gathered for 50 ms into one
// batch, typeset by one tectonic run on a worker thread (a run at a time,
// `-Z continue-on-errors --untrusted --print --keep-logs --reruns 0` in a
// folder of its own, 15 s without output once in the formulas, 60 s before),
// and the PDF is cut into one picture per formula with poppler. The whole
// PDFs are kept in $MINICODE_MATH_CACHE or $XDG_CACHE_HOME/minicode/math with
// an index (50 MB, trimmed to 40), so a reopen runs nothing. A picture is the
// page's ink as a mask filled with the text color asked for, so a formula
// takes the color around it. Needs poppler (MINICODE_ENABLE_PDF); without it
// available() is false and a formula stays its TeX.
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <string>

namespace Math {

// tectonic is found (MINICODE_TECTONIC, the downloaded copy, PATH) and poppler
// is built in.
bool available();

// poppler is built in, so a formula can be typeset when tectonic is found.
bool built();

struct State {
    enum Kind { Ready, Waiting, Failed } kind = Waiting;
    GdkTexture* texture = nullptr;   // Ready: owned by the service
    int width = 0, height = 0;       // Ready: logical pixels
    int depth = 0;                   // Ready: logical pixels below the baseline
    std::string why;                 // Failed: TeX's complaint
};

// The formula in the color "#rrggbb": kept (Ready), queued or out now
// (Waiting), or failed lately. One neither kept nor failed nor on its way is
// queued.
State want(const std::string& tex, bool display, const std::string& color);

// Formulas queued or being typeset now (the "Typesetting N formulas" line).
int outstanding();

// Called on the main thread after each run ends.
int subscribe(std::function<void()> fn);
void unsubscribe(int id);

// Checks: forget what is kept in memory and every failure (the disk cache
// stays), and how many times tectonic was started.
void forgetMemory();
int runs();

}  // namespace Math
