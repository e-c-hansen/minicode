// MathTex.h — pure C++. The TeX side of typesetting a Markdown page's math:
// which formulas may go into a batch, the batch document (one formula per
// page, cropped to its box), and what tectonic's log says about each one.
// The GUI runs tectonic and draws the pages; nothing here does any I/O.
#pragma once
#include <string>
#include <vector>

namespace MathTex {

// Bumped whenever the preamble or the rewriting in prepare() changes what a
// formula looks like, so that cached pictures made under the old ones are
// not used.
extern const int kVersion;

// Points of space around every formula's box on its page, for ink that
// strays past the box (a slanted letter's tail, a big operator).
constexpr double kPadPt = 1.0;

// Why `tex` may not go into a batch with other formulas, or "" when it may.
// A brace that never closes would swallow the formulas after it, and a bare
// $ would end the math early and leave the rest of the page in text mode.
std::string check(const std::string& tex);

// The formula as TeX is given it. What MathJax and GitHub accept but LaTeX
// does not is rewritten: \mathbb{1} (which amssymb draws as a wrong glyph)
// and \mathbbm become dsfont's \mathds, an align, gather, equation or
// multline environment becomes its inline form (aligned, gathered,
// multlined, or nothing), \tag{x} in display math becomes a spaced "(x)",
// and \label, \nonumber and \notag are dropped. Greek letters and common
// symbols typed as Unicode (α, ≤, ×, ℝ...) are spelled in TeX, since
// Computer Modern has no glyphs for them. Lines with nothing on them are
// dropped too, since a blank line ends TeX's math.
std::string prepare(const std::string& tex, bool display);

struct Formula {
    std::string tex;
    bool display = false;
};

// The LaTeX document for a batch: formula k (after prepare()) on page k + 1,
// the page exactly its box plus kPadPt on each side, and before each one a
// line in the log that readLog() finds.
std::string document(const std::vector<Formula>& formulas);

// What the log says about one formula of a batch.
struct Box {
    bool reached = false;    // TeX began it
    bool measured = false;   // TeX finished it and shipped its page
    int page = 0;            // its page, 1-based
    // Its box in TeX points, without the pad: width, height above the
    // baseline, depth below it.
    double width = 0, height = 0, depth = 0;
    std::string error;       // TeX's first complaint about it, if any
};

// One Box per formula of a batch of `count`, read from tectonic's log.
std::vector<Box> readLog(const std::string& log, size_t count);

// The first line of tectonic's own output that says what went wrong
// ("error: ...", or why it panicked, as it does when a first run cannot
// reach its bundle), for a run that produced nothing; "" when there is none.
std::string firstError(const std::string& output);

}  // namespace MathTex
