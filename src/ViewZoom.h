// ViewZoom.h — pure C++. The editor's zoom, the terminals' zoom and the
// word wrap toggle, as rules a port shows. The GTK port keeps its state in
// linux/src/Zoom.cpp; the Mac keeps its own (src/EditorFont.mm, its zoom in
// the user defaults), which these follow rule for rule.
//
// The editor's text is its base size (editor.font-size) plus the zoom,
// which Zoom In and Zoom Out change a point at a time and Reset Zoom puts
// back to 0, for every window at once, never in the settings file. The
// whole size stays within 6 to 72 points (Settings::kMinFontSize and
// kMaxFontSize): a step past a limit changes nothing. The Markdown preview
// has sizes of its own (its body, headings, code), drawn at the Mac's
// 13-point scale, so it follows the zoom as a factor, (13 + zoom) / 13,
// never under one half. The terminals have a zoom of their own, kept apart,
// on terminal.font-size, so shrinking one leaves the other alone.
//
// Word wrap is editor.word-wrap, unless the toggle (Option+Z on the Mac,
// Alt+Z on Linux) turned it over since, for every window, until the setting
// itself changes or the app quits, as VS Code's toggle does. A read-only
// view (a preview) always wraps; that is the port's to apply.
#pragma once
#include <string>

namespace ViewZoom {

constexpr double kMinSize = 6, kMaxSize = 72;
// The size the Markdown preview's own sizes are drawn at, unzoomed.
constexpr double kPreviewBase = 13;

// base + zoom, within the limits.
double size(double base, double zoom);
// The zoom after `points` more (0: back to none) for a text whose unzoomed
// size is `base`, kept so that the whole size stays within the limits.
// False, *out left alone, when a step changes nothing (at a limit); a reset
// is always true, even at 0 (the key did what it says), as the Mac's
// MCEditorZoomBy answers.
bool step(double base, double zoom, double points, double* out);
// The Markdown preview's factor, (13 + zoom) / 13, at least 0.5.
double previewScale(double zoom);
// `points` times previewScale, to the half point (the Mac's MCZoomed).
double zoomed(double points, double zoom);

// A web page's zoom after a step in (dir > 0) or out (dir < 0), on
// Safari's steps from 50% to 300%; a zoom between steps goes to the next
// step that way, and one past an end stays where it is; dir 0 is 100%. The
// Mac's MCZoomStep (src/PageZoom.mm).
double pageStep(double zoom, int dir);

// What a port keeps between runs: both zooms, in points.
struct State {
    double editor = 0, terminal = 0;
    bool operator==(const State& o) const { return editor == o.editor && terminal == o.terminal; }
};
// {"editor":2,"terminal":-1}; decode gives 0 for what is missing or wrong,
// and a value past what any base could use is brought back within it.
std::string encode(const State& s);
State decode(const std::string& json);

// The word wrap toggle over editor.word-wrap.
class Wrap {
public:
    // Whether the editor wraps, given the setting as it is now. A setting
    // changed since the toggle wins, and the toggle is forgotten.
    bool on(bool setting);
    // Turns it over from what it is now.
    void toggle(bool setting);
    // Whether the toggle stands (for the menu's check mark, and tests).
    bool overridden() const { return override_ >= 0; }

private:
    int override_ = -1;   // the toggle's choice, -1 for the setting's
    int setting_ = -1;    // the setting when the toggle was used
};

}  // namespace ViewZoom
