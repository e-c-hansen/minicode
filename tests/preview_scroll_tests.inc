// preview_scroll_tests.inc — the `previewscroll:*` groups of run_tests.cpp:
// the map between a Markdown source's lines and its rendered page that
// scrolls the preview to the side and its source together
// (PreviewScroll.h). Included there.

static void testPreviewScroll() {
    using namespace PreviewScroll;
    auto near = [](double a, double b) { return a > b - 1e-6 && a < b + 1e-6; };

    GROUP("previewscroll:marks");
    {
        // The first run of each line; a line's later runs, lines going
        // back, and tops going up are left out; runs without a line too.
        std::vector<Mark> runs = {{-1, 0},  {0, 10},  {0, 10},  {0, 30},  {2, 40},
                                  {1, 50},  {3, 35},  {4, 60},  {4, 70},  {6, 90}};
        std::vector<Mark> m = marks(runs);
        CHECK(m.size() == 4);
        if (m.size() == 4) {
            CHECK(m[0].line == 0 && near(m[0].top, 10));
            CHECK(m[1].line == 2 && near(m[1].top, 40));
            CHECK(m[2].line == 4 && near(m[2].top, 60));
            CHECK(m[3].line == 6 && near(m[3].top, 90));
        }
        CHECK(marks({}).empty());
    }

    // A page: line 0 at 10 (a heading), the blank line 1 at 40, a
    // paragraph's lines 2 and 3 at 52 and 52 + 20, nothing for 4 and 5 (a
    // fence), the code at 6, and the page ending at 300 with 12 lines.
    Page page;
    page.marks = marks({{0, 10}, {1, 40}, {2, 52}, {3, 72}, {6, 100}});
    page.bottom = 300;
    page.lines = 12;

    GROUP("previewscroll:to-page");
    {
        CHECK(near(yForLine(page, 0), 0));      // the top stays the top
        CHECK(near(yForLine(page, -3), 0));
        CHECK(near(yForLine(page, 1), 40));     // each mark's line at its text
        CHECK(near(yForLine(page, 2), 52));
        CHECK(near(yForLine(page, 3), 72));
        CHECK(near(yForLine(page, 6), 100));
        CHECK(near(yForLine(page, 2.5), 62));   // half a line: half way to the next
        CHECK(near(yForLine(page, 4.5), 72 + 1.5 / 3 * 28));   // lines with no text of their own
        // Past the last mark: on to the page's bottom over the lines left.
        CHECK(near(yForLine(page, 9), 100 + 0.5 * 200));
        CHECK(near(yForLine(page, 12), 300));
        CHECK(near(yForLine(page, 40), 300));
        // Never back up the page as the line grows.
        double last = -1;
        bool rising = true;
        for (double l = 0; l <= 14; l += 0.125) {
            const double y = yForLine(page, l);
            rising = rising && y >= last;
            last = y;
        }
        CHECK(rising);
        // Before the first mark, when it is not line 0 (a page starting
        // after blank lines or front matter): from the page's top.
        Page late;
        late.marks = marks({{4, 20}, {5, 40}});
        late.bottom = 60;
        late.lines = 6;
        CHECK(near(yForLine(late, 2), 10));
        CHECK(near(lineForY(late, 10), 2));
        // No marks: nothing to go by.
        CHECK(near(yForLine(Page{}, 5), 0));
        CHECK(near(lineForY(Page{}, 50), 0));
    }

    GROUP("previewscroll:to-source");
    {
        CHECK(near(lineForY(page, 0), 0));
        CHECK(near(lineForY(page, 5), 0));       // above the first mark, line 0 is at 10
        CHECK(near(lineForY(page, 10), 0));
        CHECK(near(lineForY(page, 40), 1));
        CHECK(near(lineForY(page, 62), 2.5));
        CHECK(near(lineForY(page, 86), 3 + 14.0 / 28 * 3));
        CHECK(near(lineForY(page, 200), 9));
        CHECK(near(lineForY(page, 300), 12));
        CHECK(near(lineForY(page, 900), 12));
    }

    GROUP("previewscroll:both-ways");
    {
        // Each map undoes the other, so neither side drifts when one moves
        // the other and back.
        bool ok = true;
        for (double l = 0; l <= 12; l += 0.05) {
            const double back = lineForY(page, yForLine(page, l));
            if (!near(back, l)) {
                ok = false;
                fprintf(stderr, "    line %g -> y %g -> line %g\n", l, yForLine(page, l), back);
            }
        }
        for (double y = 11.5; y <= 300; y += 1.5) {
            const double back = yForLine(page, lineForY(page, y));
            if (!near(back, y)) {
                ok = false;
                fprintf(stderr, "    y %g -> line %g -> y %g\n", y, lineForY(page, y), back);
            }
        }
        CHECK(ok);
    }

    GROUP("previewscroll:mark-at");
    {
        CHECK(markAt(page, 0) == 0);
        CHECK(markAt(page, 2) == 2);
        CHECK(markAt(page, 2.9) == 2);
        CHECK(markAt(page, 5) == 3);    // a fence line: the paragraph before it
        CHECK(markAt(page, 6) == 4);
        CHECK(markAt(page, 11) == 4);
        Page late;
        late.marks = marks({{4, 20}});
        CHECK(markAt(late, 1) == -1);
        CHECK(markAt(Page{}, 3) == -1);
    }
}
