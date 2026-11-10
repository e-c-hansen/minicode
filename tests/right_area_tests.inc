// right_area_tests.inc — the `rightarea:*` groups of run_tests.cpp: the
// right side of a window (RightAreaLayout.h) laid out as the Mac's
// relayoutRightArea does, its two bars dragged (sizes kept, a side hidden
// within 16 points of an edge and brought back in the same drag), and the
// owner's report of 2026-11-09: each toggle hides only its own pane, and
// with the editor, the browser and the terminal all hidden nothing comes
// back by itself.

namespace {

bool raNear(double a, double b) { return std::fabs(a - b) < 1e-6; }

bool raSame(const RightArea::Rect &r, double x, double y, double w, double h) {
    return raNear(r.x, x) && raNear(r.y, y) && raNear(r.w, w) && raNear(r.h, h);
}

// The rectangles of what shows tile the right side: nothing overlaps and
// nothing is left over.
bool raTiles(const RightArea::Frames &f, double W, double H) {
    double area = 0;
    std::vector<RightArea::Rect> rs;
    if (f.editor) rs.push_back(f.editorRect);
    if (f.browser) rs.push_back(f.browserRect);
    if (f.terminal) rs.push_back(f.terminalRect);
    if (f.browserBar) rs.push_back(f.browserBarRect);
    if (f.terminalBar) rs.push_back(f.terminalBarRect);
    for (const RightArea::Rect &r : rs) {
        if (r.x < -1e-9 || r.y < -1e-9 || r.x + r.w > W + 1e-9 || r.y + r.h > H + 1e-9) return false;
        area += r.w * r.h;
    }
    for (size_t i = 0; i < rs.size(); i++)
        for (size_t j = i + 1; j < rs.size(); j++) {
            const RightArea::Rect &a = rs[i], &b = rs[j];
            const double ox = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
            const double oy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
            if (ox > 1e-9 && oy > 1e-9) return false;
        }
    return raNear(area, rs.empty() ? 0 : W * H);
}

}  // namespace

void testRightArea() {
    using namespace RightArea;
    Limits mac;   // the Mac's numbers: bars over the panes
    mac.minTop = 70;
    Limits gtk = mac;   // the GTK port's: a 4-point gap for each bar
    gtk.bar = 4;

    GROUP("rightarea:layout");
    {
        // The editor alone has it all.
        State s;
        Frames f = layout(s, 800, 600, mac);
        CHECK(f.editor && !f.browser && !f.terminal && !f.browserBar && !f.terminalBar);
        CHECK(raSame(f.editorRect, 0, 0, 800, 600));

        // The browser beside the editor at its share, on the right, at the
        // same top and height; the terminal below both, the full width.
        s.browserVisible = true;
        s.terminalVisible = true;
        f = layout(s, 800, 600, mac);
        CHECK(f.editor && f.browser && f.terminal && f.browserBar && f.terminalBar);
        CHECK(raSame(f.editorRect, 0, 0, 400, 380));
        CHECK(raSame(f.browserRect, 400, 0, 400, 380));
        CHECK(raSame(f.terminalRect, 0, 380, 800, 220));
        CHECK(raTiles(f, 800, 600));
        // With the GTK port's gaps: the bars sit between the panes.
        f = layout(s, 804, 604, gtk);
        CHECK(raSame(f.editorRect, 0, 0, 398, 380));
        CHECK(raSame(f.browserBarRect, 398, 0, 4, 380));
        CHECK(raSame(f.browserRect, 402, 0, 402, 380));
        CHECK(raSame(f.terminalBarRect, 0, 380, 804, 4));
        CHECK(raSame(f.terminalRect, 0, 384, 804, 220));
        CHECK(raTiles(f, 804, 604));

        // A wider window keeps the browser's share, not its width.
        s.browserFraction = 0.25;
        f = layout(s, 1200, 600, mac);
        CHECK(raSame(f.browserRect, 900, 0, 300, 380));
        f = layout(s, 2400, 600, mac);
        CHECK(raSame(f.browserRect, 1800, 0, 600, 380));

        // Neither side goes under 150 while both fit.
        s.browserFraction = 0.05;
        f = layout(s, 800, 600, mac);
        CHECK(raSame(f.browserRect, 650, 0, 150, 380));
        s.browserFraction = 0.95;
        f = layout(s, 800, 600, mac);
        CHECK(raSame(f.editorRect, 0, 0, 150, 380));
        // Too narrow for both: each the same part of it, as the Mac's half.
        f = layout(s, 200, 600, mac);
        CHECK(raSame(f.editorRect, 0, 0, 100, 380) && raSame(f.browserRect, 100, 0, 100, 380));

        // The editor hidden: the browser has the top's whole width.
        s.browserFraction = 0.5;
        s.editorHidden = true;
        f = layout(s, 800, 600, mac);
        CHECK(!f.editor && f.browser && !f.browserBar);
        CHECK(raSame(f.browserRect, 0, 0, 800, 380));
        // The top empty: the terminal has the whole height, its bar at the
        // top edge, still there to drag down.
        s.browserVisible = false;
        f = layout(s, 804, 604, gtk);
        CHECK(!f.editor && !f.browser && f.terminal && f.terminalBar);
        CHECK(raSame(f.terminalBarRect, 0, 0, 804, 4));
        CHECK(raSame(f.terminalRect, 0, 4, 804, 600));
        CHECK(raTiles(f, 804, 604));

        // The dock keeps its height, at least 80, and the top at least its
        // smallest (the tab strip and 40 points).
        s = State();
        s.terminalVisible = true;
        s.terminalHeight = 30;
        f = layout(s, 800, 600, mac);
        CHECK(raSame(f.terminalRect, 0, 520, 800, 80));
        s.terminalHeight = 5000;
        f = layout(s, 800, 600, mac);
        CHECK(raSame(f.editorRect, 0, 0, 800, 70) && raSame(f.terminalRect, 0, 70, 800, 530));
        // A short window: the dock's minimum first.
        f = layout(s, 800, 100, mac);
        CHECK(raSame(f.terminalRect, 0, 20, 800, 80));

        // A build without a browser or a terminal shows neither.
        s = State();
        s.browserVisible = s.terminalVisible = true;
        Parts none;
        none.browser = none.terminal = false;
        f = layout(s, 800, 600, mac, none);
        CHECK(f.editor && !f.browser && !f.terminal && raSame(f.editorRect, 0, 0, 800, 600));
        CHECK(!shows(s, Pane::Browser, none) && shows(s, Pane::Browser));

        // Not laid out yet: no negative sizes.
        f = layout(s, 0, 0, gtk);
        CHECK(f.editorRect.w >= 0 && f.browserRect.w >= 0 && f.terminalRect.h >= 0);
    }

    GROUP("rightarea:minimum");
    {
        Minimums m;
        m.editorWidth = 120;
        m.editorHeight = 10;
        m.browserWidth = 200;
        m.browserHeight = 36;
        m.terminalWidth = 50;
        m.terminalHeight = 80;
        State s;
        double w = -1, h = -1;
        minimumSize(s, gtk, m, &w, &h);
        CHECK(raNear(w, 120) && raNear(h, 10));
        // Side by side: each side's minimum, the larger of 150 and its own.
        s.browserVisible = true;
        minimumSize(s, gtk, m, &w, &h);
        CHECK(raNear(w, 150 + 200 + 4) && raNear(h, 36));
        s.terminalVisible = true;
        minimumSize(s, gtk, m, &w, &h);
        CHECK(raNear(w, 354) && raNear(h, 36 + 4 + 80));
        // The terminal alone.
        s.editorHidden = true;
        s.browserVisible = false;
        minimumSize(s, gtk, m, &w, &h);
        CHECK(raNear(w, 50) && raNear(h, 84));
        // Nothing: nothing needed.
        s.terminalVisible = false;
        minimumSize(s, gtk, m, &w, &h);
        CHECK(raNear(w, 0) && raNear(h, 0));
    }

    GROUP("rightarea:browser-bar");
    {
        // Dragged, both sides follow and the share is kept for later.
        State s;
        s.browserVisible = true;
        CHECK(dragBrowserBar(s, 600, 800, mac) == Hid::Nothing);
        CHECK(raNear(s.browserFraction, 0.25));
        Frames f = layout(s, 800, 600, mac);
        CHECK(raSame(f.editorRect, 0, 0, 600, 600) && raSame(f.browserRect, 600, 0, 200, 600));
        // With a gap the bar's middle lands where it was dragged.
        CHECK(dragBrowserBar(s, 500, 1000, gtk) == Hid::Nothing);
        f = layout(s, 1000, 600, gtk);
        CHECK(raNear(f.browserBarRect.x + f.browserBarRect.w / 2, 500));
        // Hidden and shown again, the browser keeps its share.
        const double share = s.browserFraction;
        toggle(s, Pane::Browser);
        toggle(s, Pane::Browser);
        CHECK(raNear(s.browserFraction, share));
        // Past 150 from either edge it stops at 150.
        dragBrowserBar(s, 100, 800, mac);
        f = layout(s, 800, 600, mac);
        CHECK(raSame(f.editorRect, 0, 0, 150, 600));
        dragBrowserBar(s, 700, 800, mac);
        f = layout(s, 800, 600, mac);
        CHECK(raSame(f.browserRect, 650, 0, 150, 600));

        // Within 16 points of the left edge the editor goes; back out in
        // the same drag, it comes back.
        CHECK(dragBrowserBar(s, 16, 800, mac) == Hid::Editor);
        CHECK(s.editorHidden && s.browserVisible);
        CHECK(dragBrowserBar(s, 10, 800, mac) == Hid::Nothing && s.editorHidden);
        CHECK(dragBrowserBar(s, 300, 800, mac) == Hid::Nothing);
        CHECK(!s.editorHidden && s.browserVisible && raNear(s.browserFraction, 500.0 / 800));
        // Within 16 points of the right edge the browser goes, and comes back.
        CHECK(dragBrowserBar(s, 790, 800, mac) == Hid::Browser);
        CHECK(!s.editorHidden && !s.browserVisible);
        CHECK(dragBrowserBar(s, 400, 800, mac) == Hid::Nothing && s.browserVisible);
        // Without a browser, or before a layout, nothing moves.
        Parts noBrowser;
        noBrowser.browser = false;
        State before = s;
        CHECK(dragBrowserBar(s, 5, 800, mac, noBrowser) == Hid::Nothing);
        CHECK(dragBrowserBar(s, 5, 0, mac) == Hid::Nothing);
        CHECK(s.editorHidden == before.editorHidden && raNear(s.browserFraction, before.browserFraction));
    }

    GROUP("rightarea:terminal-bar");
    {
        State s;
        s.terminalVisible = true;
        // Dragged, the dock takes the height below the bar.
        CHECK(dragTerminalBar(s, 300, 600, mac) == Hid::Nothing);
        CHECK(raNear(s.terminalHeight, 300));
        CHECK(dragTerminalBar(s, 302, 604, gtk) == Hid::Nothing);
        Frames f = layout(s, 800, 604, gtk);
        CHECK(raNear(f.terminalBarRect.y + f.terminalBarRect.h / 2, 302));
        // No higher than leaves the top its smallest, no lower than 80.
        dragTerminalBar(s, 20, 600, mac);
        CHECK(raNear(s.terminalHeight, 530));
        dragTerminalBar(s, 590, 600, mac);
        CHECK(raNear(s.terminalHeight, 80));
        // To the top: the top goes, the browser with the editor, as the Mac's.
        s.browserVisible = true;
        s.terminalHeight = 250;
        CHECK(dragTerminalBar(s, 16, 600, mac) == Hid::Top);
        CHECK(s.editorHidden && !s.browserVisible && s.terminalVisible);
        CHECK(raNear(s.terminalHeight, 250));   // the height to come back to
        CHECK(!empty(s));
        // Down again in the same drag (or a later one, from the top edge):
        // the editor is back.
        CHECK(dragTerminalBar(s, 200, 600, mac) == Hid::Nothing);
        CHECK(!s.editorHidden && raNear(s.terminalHeight, 400));
    }

    GROUP("rightarea:toggles");
    {
        // The owner's report (2026-11-09): hiding the editor, the browser
        // and the terminal, in every order, leaves all three hidden, and
        // each toggle brings back only its own pane.
        const Pane panes[3] = {Pane::Editor, Pane::Browser, Pane::Terminal};
        int orders = 0;
        int order[3] = {0, 1, 2};
        do {
            State s;
            s.browserVisible = s.terminalVisible = true;
            s.browserFraction = 0.3;
            s.terminalHeight = 260;
            for (int k = 0; k < 3; k++) {
                CHECK(!toggle(s, panes[order[k]]));
                for (int j = 0; j < 3; j++)
                    CHECK(shows(s, panes[order[j]]) == (j > k));
                CHECK(empty(s) == (k == 2));
            }
            // And back, each alone, at the size it had.
            for (int k = 0; k < 3; k++) {
                CHECK(toggle(s, panes[order[k]]));
                for (int j = 0; j < 3; j++)
                    CHECK(shows(s, panes[order[j]]) == (j <= k));
            }
            Frames f = layout(s, 1000, 700, mac);
            CHECK(raSame(f.browserRect, 700, 0, 300, 440) && raSame(f.terminalRect, 0, 440, 1000, 260));
            orders++;
        } while (std::next_permutation(order, order + 3));
        CHECK(orders == 6);

        // A window without a browser or terminal is empty once the editor
        // hides, whatever the state says of the others.
        State s;
        s.browserVisible = s.terminalVisible = true;
        s.editorHidden = true;
        Parts none;
        none.browser = none.terminal = false;
        CHECK(empty(s, none) && !empty(s));
        CHECK(topShown(s) && !topShown(s, none));
    }
}
