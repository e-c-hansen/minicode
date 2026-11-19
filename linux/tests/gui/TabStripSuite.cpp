// TabStripSuite.cpp — `./build/mutible-guitest tab_strip`: the editor's row
// of tabs as VS Code's tabSizing "fit" and the Mac's strip lay it out
// (TabStrip.h over EditorTabs::layoutStrip, revealScroll, wheelScroll,
// dragAutoScroll, clipped, fitName and tabNames), by real keys, wheel and
// drags, as the Mac's preview_tabs checks it. Seventeen files in a narrow
// editor: every title whole but one far longer than the widest tab, which is
// cut in the middle; each tab 80 to 320 pixels wide, where GTK put it; the
// row scrolling, the tab on show scrolled into view and a far tab's key
// bringing it in; the wheel scrolling the row and choosing nothing, and
// scrolling on from a reveal not yet laid out; a dot
// for unsaved changes leaving the row where it was scrolled; two index.ts
// files showing the folders that tell them apart, dimmed after the name; a
// tab still dragged to a new place, and a drag held at the row's end
// scrolling the row and dropping the tab there.
#include "Harness.h"

#include "Editor.h"
#include "TabStrip.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
TabStrip* strip() { return ed()->strip(); }
std::string F(const std::string& name) { return MCGuiProjectFile("strip/" + name); }

// The Mac's fifteen, a name shorter than the smallest tab, and one longer
// than the widest.
const char* kLong = "a_rather_long_module_name_for_the_quarterly_reports_and_their_summaries.py";
const std::vector<std::string> kMany = {
    "summary.py", "models.py", "config.json", "settings_panel.py", "README.md", "views.py",
    "urls.py", "serializers.py", "tasks.py", "admin.py", "tests_api.py", "package.json",
    "Makefile", "a.c", kLong, "src/app/index.ts", "src/lib/index.ts"};
const int kShort = 13, kLongAt = 14;
const int N = (int)kMany.size();

std::string base(const std::string& p) { return p.substr(p.rfind('/') + 1); }

int indexOf(const std::string& name) {
    for (int i = 0; i < strip()->count(); i++)
        if (strip()->item(i).tooltip == F(name)) return i;
    return -1;
}

double view() { return gtk_widget_get_width(strip()->rowWidget()); }
double maxScroll() { return std::max(0.0, strip()->contentWidth() - view()); }

// Tab i where GTK put it, in the row widget's coordinates.
graphene_rect_t bounds(int i) {
    graphene_rect_t r = GRAPHENE_RECT_INIT(0, 0, 0, 0);
    if (!gtk_widget_compute_bounds(strip()->tabWidget(i), strip()->rowWidget(), &r))
        r = GRAPHENE_RECT_INIT(-1, 0, 0, 0);
    return r;
}

bool inView(int i) {
    const graphene_rect_t r = bounds(i);
    return r.size.width > 0 && r.origin.x >= -0.5 && r.origin.x + r.size.width <= view() + 0.5;
}

// The strip laid out as it was last told: every tab at the core's place
// (as allocated), its title cut or not as fitName says, and Pango cutting
// nothing more.
bool settled() {
    if (strip()->count() != N || view() <= 0) return false;
    for (int i = 0; i < N; i++) {
        const graphene_rect_t r = bounds(i);
        if (std::lround(r.size.width) != std::lround(strip()->tabWidth(i))) return false;
        if (std::lround(r.origin.x) != std::lround(strip()->tabLeft(i) - strip()->scrollOffset()))
            return false;
        PangoLayout* l = gtk_label_get_layout(GTK_LABEL(strip()->titleWidget(i)));
        if (pango_layout_is_ellipsized(l)) return false;
    }
    return true;
}

// One of the row's overlays (TabStrip.h): the fades and the scroll bar.
GtkWidget* over(const char* cls) {
    for (GtkWidget* c = gtk_widget_get_first_child(strip()->rowWidget()); c;
         c = gtk_widget_get_next_sibling(c))
        if (gtk_widget_has_css_class(c, cls)) return c;
    return nullptr;
}
bool shown(const char* cls) {
    GtkWidget* w = over(cls);
    return w && gtk_widget_get_child_visible(w) && gtk_widget_get_mapped(w);
}

double points(GtkWidget* label) {
    const PangoFontDescription* d = pango_context_get_font_description(gtk_widget_get_pango_context(label));
    return d ? (double)pango_font_description_get_size(d) / PANGO_SCALE : -1;
}

// A real drag of tab `from` to tab `to`'s near edge (its far edge when
// moving right), in the window's coordinates (TabsSuite's).
void DragTab(int from, int to) {
    GtkWidget* win = GTK_WIDGET(MCGuiApp().window);
    GtkWidget* a = strip()->tabWidget(from);
    GtkWidget* b = strip()->tabWidget(to);
    const double h = gtk_widget_get_height(a) / 2.0;
    graphene_point_t p0 = GRAPHENE_POINT_INIT(15, (float)h), p1, q0, q1;
    q0 = GRAPHENE_POINT_INIT(to > from ? (float)gtk_widget_get_width(b) - 4 : 4.0f, (float)h);
    if (!gtk_widget_compute_point(a, win, &p0, &p1)) p1 = p0;
    if (!gtk_widget_compute_point(b, win, &q0, &q1)) q1 = q0;
    MCGuiDrag(win, p1.x, p1.y, std::max(1.0f, q1.x), p1.y);
}

double gScroll = 0, gMaxSeen = 0;
std::string gDragged;

}  // namespace

MC_GUI_SUITE(tab_strip) {
    MCGuiStep("seventeen files open in tabs of their own", [] {
        ed()->closeAllTabs();
        for (const std::string& n : kMany) MCGuiWriteFile(F(n), base(n) + "\n");
        for (const std::string& n : kMany) MCGuiOpenFileKept(F(n));
    });
    MCGuiWait("all their tabs, the last on show, laid out", 15, [] {
        return ed()->tabCount() == N && strip()->activeIndex() == N - 1 && settled();
    });
    MCGuiStep("every title whole, but the one past the widest tab, cut in the middle", [] {
        MCGuiNote("row %.0f wide, its tabs %.0f, scrolled %.0f", view(), strip()->contentWidth(),
                  strip()->scrollOffset());
        for (int i = 0; i < N; i++) {
            const std::string shown = strip()->displayedTitle(i), want = base(kMany[(size_t)i]);
            GCHECK_EQ_STR(strip()->item(i).title, want);
            if (i == kLongAt) {
                MCGuiNote("the long name shows as \"%s\"", shown.c_str());
                GCHECK(shown != want && shown.size() < want.size());
                GCHECK(shown.find("\xE2\x80\xA6") != std::string::npos);
                GCHECK(shown.compare(0, 8, want, 0, 8) == 0);   // its start
                GCHECK(shown.size() > 8 &&
                       shown.compare(shown.size() - 8, 8, want, want.size() - 8, 8) == 0);   // and end
                GCHECK_EQ_STR(strip()->item(i).tooltip, F(kLong));
            } else {
                if (shown != want) MCGuiNote("tab %d shows \"%s\", not %s", i, shown.c_str(), want.c_str());
                GCHECK_EQ_STR(shown, want);
            }
        }
    });
    MCGuiStep("each tab 80 to 320 wide, end to end, where GTK put it", [] {
        std::string widths;
        for (int i = 0; i < N; i++) {
            const double w = strip()->tabWidth(i);
            widths += std::to_string((int)w) + " ";
            GCHECK(w >= 80 && w <= 320);
            GCHECK(std::lround(bounds(i).size.width) == std::lround(w));
            if (i > 0) GCHECK(strip()->tabLeft(i) == strip()->tabLeft(i - 1) + strip()->tabWidth(i - 1));
        }
        MCGuiNote("widths: %s", widths.c_str());
        GCHECK(strip()->tabWidth(kShort) == 80);    // a.c: the smallest tab
        GCHECK(strip()->tabWidth(kLongAt) == 320);  // the long name: the widest
        GCHECK(strip()->tabWidth(3) > strip()->tabWidth(6));   // settings_panel.py, urls.py
        GCHECK(strip()->tabLeft(N - 1) + strip()->tabWidth(N - 1) == strip()->contentWidth());
    });
    MCGuiStep("the row is wider than the strip: it scrolls, the tab on show in view", [] {
        GCHECK(view() > 0 && strip()->contentWidth() > view());
        GCHECK(strip()->scrollOffset() == maxScroll());
        GCHECK(inView(N - 1));
        GCHECK(!inView(0));
        GCHECK(shown("minicode-tabs-fade-left") && !shown("minicode-tabs-fade-right"));
        MCGuiSnapshot(strip()->widget(), "tab_strip-1-end");
    });
    MCGuiStep("the two index.ts tabs show the folders that tell them apart", [] {
        GCHECK_EQ_STR(strip()->item(N - 2).title, "index.ts");
        GCHECK_EQ_STR(strip()->item(N - 1).title, "index.ts");
        GCHECK_EQ_STR(strip()->item(N - 2).hint, "app");
        GCHECK_EQ_STR(strip()->item(N - 1).hint, "lib");
        GCHECK_EQ_STR(strip()->displayedHint(N - 2), "app");
        GCHECK_EQ_STR(strip()->displayedHint(N - 1), "lib");
        GCHECK(MCGuiOnScreen(strip()->hintWidget(N - 1)));
        // After the name, smaller.
        graphene_rect_t t, h;
        GCHECK(gtk_widget_compute_bounds(strip()->titleWidget(N - 1), strip()->tabWidget(N - 1), &t) &&
               gtk_widget_compute_bounds(strip()->hintWidget(N - 1), strip()->tabWidget(N - 1), &h) &&
               h.origin.x >= t.origin.x + t.size.width);
        const double tp = points(strip()->titleWidget(N - 1)), hp = points(strip()->hintWidget(N - 1));
        MCGuiNote("title %.1f pt, hint %.1f pt", tp, hp);
        GCHECK(hp > 0 && hp < tp);
        // Names no other tab shares have none.
        for (int i = 0; i < N - 2; i++) GCHECK(strip()->item(i).hint.empty() && strip()->displayedHint(i).empty());
    });

    // ---- a far tab's key brings it into view
    MCGuiStep("the keyboard in the editor; Alt+1, the first tab", [] {
        MCGuiKey("ctrl+1");
        MCGuiKey("alt+1");
    });
    MCGuiWait("the first tab on show, the row back at its start", 10, [] {
        return strip()->activeIndex() == 0 && settled() && strip()->scrollOffset() == 0;
    });
    MCGuiStep("the last tab out of view, the fade at the right end", [] {
        GCHECK(inView(0) && !inView(N - 1));
        GCHECK(!shown("minicode-tabs-fade-left") && shown("minicode-tabs-fade-right"));
        MCGuiSnapshot(strip()->widget(), "tab_strip-2-start");
    });
    MCGuiStep("Ctrl+Page Up wraps to the last tab", [] { MCGuiKey("ctrl+pageup"); });
    MCGuiWait("the last tab on show, whole, the row scrolled to its end", 10, [] {
        return strip()->activeIndex() == N - 1 && settled() && inView(N - 1) &&
               strip()->scrollOffset() == maxScroll();
    });

    // ---- the wheel
    MCGuiStep("two wheel clicks up over the strip", [] {
        gScroll = strip()->scrollOffset();
        MCGuiScroll(strip()->titleWidget(N - 1), -2);
    });
    MCGuiWait("the row scrolled toward its start", 5, [] { return strip()->scrollOffset() < gScroll; });
    MCGuiStep("by two notches, choosing nothing; the bar shows under the pointer", [] {
        MCGuiNote("scrolled from %.0f to %.0f", gScroll, strip()->scrollOffset());
        GCHECK(std::abs(strip()->scrollOffset() - (gScroll - 2 * 48)) < 1);
        GCHECK(strip()->activeIndex() == N - 1);
        GCHECK(shown("minicode-tabs-bar") && shown("minicode-tabs-fade-left") &&
               shown("minicode-tabs-fade-right"));
        MCGuiSnapshot(strip()->widget(), "tab_strip-3-wheel");
        gScroll = strip()->scrollOffset();
        MCGuiScroll(strip()->titleWidget(N - 2), 1);
    });
    MCGuiWait("one click down scrolls back toward the end", 5, [] {
        return std::abs(strip()->scrollOffset() - (gScroll + 48)) < 1;
    });

    // ---- an unsaved change leaves the row where it was scrolled
    MCGuiStep("typing in the last tab, scrolled partly out of view", [] {
        GCHECK(!inView(N - 1));
        gScroll = strip()->scrollOffset();
        MCGuiKey("ctrl+1");
        MCGuiType("x");
    });
    MCGuiWait("its dot", 10, [] { return strip()->item(N - 1).dirty; });
    MCGuiPause(0.3);   // a negative check: frames enough for a layout that should not move it
    MCGuiStep("the row did not move; undo", [] {
        MCGuiNote("scrolled %.0f, was %.0f", strip()->scrollOffset(), gScroll);
        GCHECK(strip()->scrollOffset() == gScroll);
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("clean again", 10, [] { return !strip()->item(N - 1).dirty; });

    // ---- a wheel turned before the strip's next frame: the reveal the
    // switch asked for comes first, and the wheel scrolls on from it (the
    // reveal still waiting had put a turned wheel back, in browser_tabs)
    MCGuiStep("Alt+1 and two wheel clicks down, sent together", [] {
        MCGuiKey("alt+1");
        MCGuiScroll(strip()->titleWidget(N - 2), 2);
    });
    MCGuiWait("the first tab on show, the row two notches from its start", 10, [] {
        return strip()->activeIndex() == 0 && settled() && std::abs(strip()->scrollOffset() - 2 * 48) < 1;
    });

    // ---- dragging along a scrolled row
    MCGuiStep("the wheel back to the start", [] { MCGuiScroll(strip()->titleWidget(3), -5); });
    MCGuiWait("at the start", 10, [] { return strip()->activeIndex() == 0 && settled() && strip()->scrollOffset() == 0; });
    MCGuiStep("summary.py's tab dragged past config.json's", [] { DragTab(0, 2); });
    MCGuiWait("it is third now", 10, [] { return indexOf("summary.py") == 2; });
    MCGuiStep("the editor's tabs moved with it", [] {
        GCHECK_EQ_STR(ed()->document(2)->path(), F("summary.py"));
        GCHECK_EQ_STR(strip()->item(0).title, "models.py");
    });
    MCGuiWait("laid out", 10, settled);
    MCGuiStep("a tab held down and the pointer taken to the strip's right end", [] {
        gDragged = strip()->item(1).title;
        gMaxSeen = strip()->scrollOffset();
        MCGuiPress(strip()->tabWidget(1), 20, -1);
    });
    MCGuiStep("moved", [] {
        MCGuiMoveTo(strip()->rowWidget(), view() - 3, gtk_widget_get_height(strip()->rowWidget()) / 2.0);
    });
    MCGuiWait("the row scrolls to its end while the tab is held there", 15, [] {
        gMaxSeen = std::max(gMaxSeen, strip()->scrollOffset());
        return strip()->scrollOffset() == maxScroll();
    });
    MCGuiStep("let go", [] {
        MCGuiNote("scrolled to %.0f of %.0f", gMaxSeen, maxScroll());
        MCGuiRelease(1);
    });
    MCGuiWait("the tab dropped at the end of the row", 10, [] {
        return strip()->item(N - 1).title == gDragged && ed()->document(N - 1) &&
               base(ed()->document(N - 1)->path()) == gDragged;
    });
    MCGuiWait("laid out, in view", 10, [] { return settled() && inView(strip()->activeIndex()); });
    MCGuiStep("tidy up: the tabs closed", [] {
        MCGuiSnapshot(strip()->widget(), "tab_strip-4-dropped");
        ed()->closeAllTabs();
    });
    MCGuiWait("none", 10, [] { return ed()->tabCount() == 0; });
}
