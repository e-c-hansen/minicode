// terminal_layout_tests.inc — the `termlayout:*` and `tabs:drop` groups of
// run_tests.cpp: a terminal tab's panes (TerminalLayout.h) split, closed,
// dragged and resized from the keyboard, every pane kept at its minimum,
// and where a tab dragged along a strip lands (EditorTabs::dropIndex).

namespace {

double layoutSum(const TerminalLayout::Fractions &fr) {
    double s = 0;
    for (double f : fr) s += f;
    return s;
}

bool layoutNear(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

void testTerminalLayout() {
    using namespace TerminalLayout;

    GROUP("termlayout:lengths");
    {
        // 3 parts of 301 points with 1-point gaps: 99 each, the last the rest.
        std::vector<double> l = lengths(even(3), 301, 1);
        CHECK(l.size() == 3 && l[0] == 99 && l[1] == 99 && l[2] == 101);
        CHECK(l[0] + l[1] + l[2] + 2 == 301);
        CHECK(startOf(even(3), 2, 301, 1) == 200);
        CHECK(boundaryAt(even(3), 0, 301, 1) == 99.5);
        // One part takes it all; nothing to share leaves zeros, never less.
        l = lengths({1.0}, 120, 1);
        CHECK(l.size() == 1 && l[0] == 120);
        l = lengths(even(4), 2, 1);
        CHECK(l.size() == 4);
        for (double x : l) CHECK(x >= 0);
    }

    GROUP("termlayout:split");
    {
        // Halving keeps the other parts' shares.
        Fractions fr = {0.5, 0.5};
        CHECK(split(fr, 0, 1001, 1, 150) == Split::Halved);
        CHECK(fr.size() == 3 && layoutNear(fr[0], 0.25) && layoutNear(fr[1], 0.25) && layoutNear(fr[2], 0.5));
        CHECK(layoutNear(layoutSum(fr), 1));
        // Halves too small, equal shares fit: every part evened.
        fr = {0.7, 0.3};
        CHECK(split(fr, 1, 700, 1, 200) == Split::Evened);
        CHECK(fr.size() == 3 && layoutNear(fr[0], 1.0 / 3) && layoutNear(fr[2], 1.0 / 3));
        // Not even equal shares fit: refused, unchanged.
        fr = {0.5, 0.5};
        CHECK(split(fr, 0, 500, 1, 200) == Split::Refused);
        CHECK(fr.size() == 2 && layoutNear(fr[0], 0.5));
        // The old rule's edge: 3 equal panes of 160 points with a 152-point
        // minimum (20 columns of 7.2 plus padding) fit; a fourth does not.
        fr = {1.0};
        CHECK(split(fr, 0, 482, 1, 152) == Split::Halved);
        CHECK(split(fr, 1, 482, 1, 152) == Split::Evened);
        for (double len : lengths(fr, 482, 1)) CHECK(len >= 152);
        CHECK(split(fr, 2, 482, 1, 152) == Split::Refused);
        CHECK(fr.size() == 3);
        // Not laid out yet: split, nothing to judge by.
        fr = {1.0};
        CHECK(split(fr, 0, 0, 1, 152) == Split::Halved);
        CHECK(fr.size() == 2);
    }

    GROUP("termlayout:remove");
    {
        Fractions fr = {0.2, 0.3, 0.5};
        remove(fr, 1);   // to the part before it
        CHECK(fr.size() == 2 && layoutNear(fr[0], 0.5) && layoutNear(fr[1], 0.5));
        fr = {0.2, 0.3, 0.5};
        remove(fr, 0);   // the first's to the one after
        CHECK(fr.size() == 2 && layoutNear(fr[0], 0.5) && layoutNear(fr[1], 0.5));
        fr = {1.0};
        remove(fr, 0);
        CHECK(fr.empty());
        remove(fr, 3);   // out of range: nothing
        CHECK(fr.empty());
    }

    GROUP("termlayout:drag");
    {
        // Two parts of 1001 points: the bar dragged to 300 makes the first
        // 300 - 0.5 rounded, and the pair still fills the width.
        Fractions fr = {0.5, 0.5};
        CHECK(dragBoundary(fr, 0, 300.5, 1001, 1, 150));
        std::vector<double> l = lengths(fr, 1001, 1);
        CHECK(l[0] == 300 && l[1] == 700);
        CHECK(layoutNear(layoutSum(fr), 1));
        CHECK(boundaryAt(fr, 0, 1001, 1) == 300.5);
        // Past the minimum on either side: clamped there, exactly.
        CHECK(dragBoundary(fr, 0, 10, 1001, 1, 150));
        l = lengths(fr, 1001, 1);
        CHECK(l[0] == 150 && l[1] == 850);
        CHECK(dragBoundary(fr, 0, 2000, 1001, 1, 150.4));
        l = lengths(fr, 1001, 1);
        CHECK(l[0] == 849 && l[1] == 151);
        // Already there: NO.
        CHECK(!dragBoundary(fr, 0, 2000, 1001, 1, 150.4));
        // Three parts: only the pair at the bar changes.
        fr = even(3);
        const double third = lengths(fr, 301, 1)[2];
        CHECK(dragBoundary(fr, 0, 50.5, 301, 1, 20));
        l = lengths(fr, 301, 1);
        CHECK(l[0] == 50 && l[1] == 148 && l[2] == third);
        // No room for two minimums: nothing moves.
        fr = {0.5, 0.5};
        CHECK(!dragBoundary(fr, 0, 40, 100, 1, 60));
        CHECK(!dragBoundary(fr, 1, 40, 100, 1, 10));   // no bar after the last
        // Every point the bar can be dragged to keeps both at the minimum
        // and the whole width filled.
        bool ok = true;
        for (double pos = -50; pos < 700; pos += 7.3) {
            fr = {0.5, 0.5};
            dragBoundary(fr, 0, pos, 640, 1, 152);
            l = lengths(fr, 640, 1);
            ok = ok && l[0] >= 152 && l[1] >= 152 && l[0] + l[1] + 1 == 640;
        }
        CHECK(ok);
    }

    GROUP("termlayout:nudge");
    {
        // VS Code's rule: the boundary moves the way the arrow points, the
        // far edge of a part, or the near edge of the last.
        Fractions fr = even(3);   // 99, 99, 101 of 301
        CHECK(nudge(fr, 0, true, 10, 301, 1, 20));
        std::vector<double> l = lengths(fr, 301, 1);
        CHECK(l[0] == 109 && l[1] == 89 && l[2] == 101);
        CHECK(nudge(fr, 2, true, 10, 301, 1, 20));   // the last: its left edge moves right
        l = lengths(fr, 301, 1);
        CHECK(l[1] == 99 && l[2] == 91);
        CHECK(nudge(fr, 2, false, 30, 301, 1, 20));
        l = lengths(fr, 301, 1);
        CHECK(l[1] == 69 && l[2] == 121);
        // Up to the minimum, then NO.
        fr = {0.5, 0.5};
        int steps = 0;
        while (nudge(fr, 0, false, 30, 401, 1, 152) && steps < 100) steps++;
        l = lengths(fr, 401, 1);
        CHECK(steps == 2 && l[0] == 152 && l[1] == 248);
        // One part: nothing to resize against.
        fr = {1.0};
        CHECK(!nudge(fr, 0, true, 10, 400, 1, 20));
    }

    GROUP("termlayout:frames");
    {
        // Two columns, the second stacked in two: 601 by 301 points.
        Layout l;
        l.columns = {0.5, 0.5};
        l.rows = {Fractions{1.0}, Fractions{0.5, 0.5}};
        std::vector<std::vector<Rect>> f = frames(l, 601, 301, 1);
        CHECK(f.size() == 2 && f[0].size() == 1 && f[1].size() == 2);
        CHECK(f[0][0].x == 0 && f[0][0].w == 300 && f[0][0].h == 301 && f[0][0].y == 0);
        CHECK(f[1][0].x == 301 && f[1][0].w == 300);
        CHECK(f[1][0].y == 0 && f[1][0].h == 150);
        CHECK(f[1][1].y == 151 && f[1][1].h == 150);
        // A pane closed: its stack's other pane takes its height, and a
        // column whose last pane closes gives its width to its neighbour.
        removePane(l, 1, 0);
        CHECK(l.rows[1].size() == 1 && layoutNear(l.rows[1][0], 1));
        removePane(l, 0, 0);
        CHECK(l.columns.size() == 1 && layoutNear(l.columns[0], 1) && l.rows.size() == 1);
        f = frames(l, 601, 301, 1);
        CHECK(f.size() == 1 && f[0][0].w == 601 && f[0][0].h == 301);
    }

    GROUP("tabs:drop");
    {
        using EditorTabs::dropIndex;
        const std::vector<double> w = {100, 100, 100, 100};   // middles 50, 150, 250, 350
        CHECK(dropIndex(w, 0, 10) == 0);     // not past the next tab's middle
        CHECK(dropIndex(w, 0, 160) == 1);    // past tab 1's middle
        CHECK(dropIndex(w, 0, 1000) == 3);   // past every tab: last
        CHECK(dropIndex(w, 3, -5) == 0);
        CHECK(dropIndex(w, 3, 260) == 3);    // past tab 2's middle: stays last
        CHECK(dropIndex(w, 3, 240) == 2);
        CHECK(dropIndex(w, 2, 240) == 2);    // within itself: where it was
        CHECK(dropIndex(w, 1, 150) == 1);
        CHECK(dropIndex(w, 7, 10) == 7);     // out of range: unchanged
        CHECK(dropIndex({}, 0, 10) == 0);
        // Tabs of different widths: their own middles count.
        CHECK(dropIndex({40, 200, 60}, 0, 139) == 0);
        CHECK(dropIndex({40, 200, 60}, 0, 141) == 1);
    }
}
