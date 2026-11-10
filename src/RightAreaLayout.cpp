// RightAreaLayout.cpp — see RightAreaLayout.h.
#include "RightAreaLayout.h"

#include <algorithm>
#include <cmath>

namespace RightArea {

namespace {

double clampTo(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

bool browserShows(const State &s, const Parts &parts) { return s.browserVisible && parts.browser; }
bool terminalShows(const State &s, const Parts &parts) {
    return s.terminalVisible && parts.terminal;
}

}  // namespace

bool shows(const State &s, Pane p, const Parts &parts) {
    switch (p) {
    case Pane::Editor: return !s.editorHidden;
    case Pane::Browser: return browserShows(s, parts);
    case Pane::Terminal: return terminalShows(s, parts);
    }
    return false;
}

bool topShown(const State &s, const Parts &parts) {
    return !s.editorHidden || browserShows(s, parts);
}

bool empty(const State &s, const Parts &parts) {
    return !topShown(s, parts) && !terminalShows(s, parts);
}

bool toggle(State &s, Pane p) {
    switch (p) {
    case Pane::Editor:
        s.editorHidden = !s.editorHidden;
        return !s.editorHidden;
    case Pane::Browser:
        s.browserVisible = !s.browserVisible;
        return s.browserVisible;
    case Pane::Terminal:
        s.terminalVisible = !s.terminalVisible;
        return s.terminalVisible;
    }
    return false;
}

Frames layout(const State &s, double W, double H, const Limits &lim, const Parts &parts) {
    Frames f;
    f.editor = !s.editorHidden;
    f.browser = browserShows(s, parts);
    f.terminal = terminalShows(s, parts);
    f.browserBar = f.editor && f.browser;
    f.terminalBar = f.terminal;
    W = std::max(0.0, W);
    H = std::max(0.0, H);
    const double bar = lim.bar;
    const bool top = f.editor || f.browser;

    // The dock: its remembered height, at least its minimum, and never so
    // tall that the top is under its smallest; the whole height, below its
    // bar, when the top is empty.
    double termH = 0;
    if (f.terminal) {
        termH = top ? clampTo(s.terminalHeight, lim.minTerminal,
                              std::max(lim.minTerminal, H - lim.minTop - bar))
                    : H - bar;
        termH = clampTo(termH, 0, std::max(0.0, H - bar));
        f.terminalRect = Rect{0, H - termH, W, termH};
        f.terminalBarRect = Rect{0, std::max(0.0, H - termH - bar), W, std::min(bar, H)};
    }
    const double topH = f.terminal ? std::max(0.0, H - termH - bar) : H;

    // Side by side, the browser gets its share of the width, neither pane
    // under its minimum while both fit; where they do not, each gets the
    // same part of what there is.
    if (f.browserBar) {
        const double room = std::max(0.0, W - bar);
        double bw = std::round(W * s.browserFraction);
        if (room >= lim.minEditorWidth + lim.minBrowserWidth)
            bw = clampTo(bw, lim.minBrowserWidth, room - lim.minEditorWidth);
        else if (lim.minEditorWidth + lim.minBrowserWidth > 0)
            bw = std::round(room * lim.minBrowserWidth /
                            (lim.minEditorWidth + lim.minBrowserWidth));
        else
            bw = std::round(room / 2);
        const double ew = room - bw;
        f.editorRect = Rect{0, 0, ew, topH};
        f.browserBarRect = Rect{ew, 0, bar, topH};
        f.browserRect = Rect{ew + bar, 0, bw, topH};
    } else if (f.editor) {
        f.editorRect = Rect{0, 0, W, topH};
    } else if (f.browser) {
        f.browserRect = Rect{0, 0, W, topH};
    }
    return f;
}

void minimumSize(const State &s, const Limits &lim, const Minimums &m, double *width,
                 double *height, const Parts &parts) {
    const bool editor = !s.editorHidden, browser = browserShows(s, parts),
               terminal = terminalShows(s, parts);
    double w = 0, h = 0;
    if (editor && browser)
        w = std::max(lim.minEditorWidth, m.editorWidth) +
            std::max(lim.minBrowserWidth, m.browserWidth) + lim.bar;
    else if (editor)
        w = m.editorWidth;
    else if (browser)
        w = m.browserWidth;
    const double topMin =
        std::max(editor ? m.editorHeight : 0.0, browser ? m.browserHeight : 0.0);
    if (terminal) {
        w = std::max(w, m.terminalWidth);
        h = (editor || browser)
                ? topMin + lim.bar + std::max(lim.minTerminal, m.terminalHeight)
                : lim.bar + m.terminalHeight;
    } else {
        h = topMin;
    }
    if (width) *width = w;
    if (height) *height = h;
}

Hid dragBrowserBar(State &s, double x, double W, const Limits &lim, const Parts &parts) {
    if (W <= 0 || !parts.browser) return Hid::Nothing;
    if (x <= lim.snap) {
        if (!s.editorHidden && s.browserVisible) {
            s.editorHidden = true;
            return Hid::Editor;
        }
        return Hid::Nothing;
    }
    if (x >= W - lim.snap) {
        if (s.browserVisible && !s.editorHidden) {
            s.browserVisible = false;
            return Hid::Browser;
        }
        return Hid::Nothing;
    }
    // Back out of an edge in the same drag: the side it hid comes back.
    if (s.editorHidden && s.browserVisible) s.editorHidden = false;
    if (!s.browserVisible && !s.editorHidden) s.browserVisible = true;
    const double room = W - lim.bar;
    const double bw = clampTo(W - x - lim.bar / 2, lim.minBrowserWidth,
                              std::max(lim.minBrowserWidth, room - lim.minEditorWidth));
    s.browserFraction = bw / W;
    return Hid::Nothing;
}

Hid dragTerminalBar(State &s, double y, double H, const Limits &lim, const Parts &parts) {
    if (H <= 0 || !parts.terminal) return Hid::Nothing;
    const bool top = topShown(s, parts);
    if (y <= lim.snap) {
        if (top) {
            s.editorHidden = true;
            s.browserVisible = false;
            return Hid::Top;
        }
        return Hid::Nothing;
    }
    // Down from the top again: the editor comes back.
    if (!top) s.editorHidden = false;
    s.terminalHeight = clampTo(H - y - lim.bar / 2, lim.minTerminal,
                               std::max(lim.minTerminal, H - lim.minTop - lim.bar));
    return Hid::Nothing;
}

}  // namespace RightArea
