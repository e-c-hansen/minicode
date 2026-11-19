// HintsPanelSuite.cpp — the shortcut hints (`./build/mutible-guitest
// hints_panel`, HintsColumns.h and HintsSearchBox.h), the Mac's hints_panel
// steps (PageMeasureSuite.mm) by real keys and real window sizes.
//
// Columns: the list fills as many columns as the window's width holds,
// three or more on a 2000 pixel window with nothing to scroll, fewer at
// 1400, one on a narrow window (its labels wrapping under themselves when
// even one column is too wide), fewer zoomed in (the text larger), back as
// the window or the zoom goes back, and a heading (or a line of text) is
// never a column's last line; every column's lines line up.
//
// The search box: with the panel up (Ctrl+Shift+H) a letter and an
// Alt+letter (Word Wrap's Alt+Z) type into the box, never the editor or a
// command, the keyboard taken from the editor at the first; what the box
// finds is marked and the rest dimmed (a heading with nothing found under
// it too), keys found by name and as combinations in any order
// ("shift+ctrl+h", "control"), the columns, their text and the panel's
// size unchanged, the highest mark scrolled into sight; Backspace edits
// the box; a Ctrl key (Ctrl+B) still runs its command and a function key
// (F1) is still the palette's, whose typing stays its own; Esc clears the
// box, then closes the panel, the keyboard back in the editor; a click
// puts the keyboard in the box, and Ctrl+Shift+H closes the panel and
// empties it.
#include "Harness.h"

#include "Editor.h"
#include "HintsColumns.h"
#include "Keys.h"
#include "PalettePanel.h"
#include "WindowLayout.h"
#include "Zoom.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& n) { return MCGuiProjectFile(n); }
GtkWidget* box() { return MCGuiApp().hintsSearch; }
HintsColumns* cols() { return MCGuiApp().hintsColumns; }
GtkWidget* area() { return cols()->widget(); }
GtkWidget* panel() { return MCGuiApp().hintsPanel; }
GtkWindow* win() { return MCGuiApp().window; }
bool shown() { return gtk_widget_get_visible(panel()); }
std::string query() { return gtk_editable_get_text(GTK_EDITABLE(box())); }
int columns() { return cols()->count(); }

// Every column's text, one after the other.
std::string hintsText() {
    std::string s;
    for (int c = 0; c < columns(); ++c) s += cols()->text(c) + "\n";
    return s;
}

GtkWidget* focus() { return gtk_root_get_focus(GTK_ROOT(win())); }
bool editorHasKeyboard() { return focus() == ed()->textView(); }
bool boxHasKeyboard() {
    GtkWidget* f = focus();
    return f && (f == box() || gtk_widget_is_ancestor(f, box()));
}

// A column's attributes of one type, as byte ranges in its text.
std::vector<std::pair<size_t, size_t>> ranges(int column, PangoAttrType type) {
    std::vector<std::pair<size_t, size_t>> out;
    PangoLayout* layout = cols()->layout(column);
    PangoAttrList* list = layout ? pango_layout_get_attributes(layout) : nullptr;
    if (!list) return out;
    GSList* all = pango_attr_list_get_attributes(list);
    for (GSList* l = all; l; l = l->next) {
        auto* a = static_cast<PangoAttribute*>(l->data);
        if (a->klass->type == type) out.push_back({a->start_index, a->end_index});
    }
    g_slist_free_full(all, (GDestroyNotify)pango_attribute_destroy);
    return out;
}

// The stretches marked as found, as text.
std::vector<std::string> found() {
    std::vector<std::string> out;
    for (int c = 0; c < columns(); ++c) {
        const std::string t = cols()->text(c);
        for (auto [from, to] : ranges(c, PANGO_ATTR_BACKGROUND))
            if (to <= t.size() && from < to) out.push_back(t.substr(from, to - from));
    }
    return out;
}
bool foundHas(const std::string& what) {
    for (const std::string& f : found())
        if (f == what) return true;
    return false;
}
size_t dimmedCount() {
    size_t n = 0;
    for (int c = 0; c < columns(); ++c) n += ranges(c, PANGO_ATTR_FOREGROUND_ALPHA).size();
    return n;
}
// Whether `what`'s first place in the columns is drawn dimmed.
bool dimmed(const std::string& what) {
    for (int c = 0; c < columns(); ++c) {
        const size_t at = cols()->text(c).find(what);
        if (at == std::string::npos) continue;
        for (auto [from, to] : ranges(c, PANGO_ATTR_FOREGROUND_ALPHA))
            if (from <= at && at < to) return true;
        return false;
    }
    return false;
}
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

// The list's scroll: its adjustment, and how far down it is.
GtkAdjustment* vadj() {
    GtkWidget* scroll = gtk_widget_get_ancestor(area(), GTK_TYPE_SCROLLED_WINDOW);
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
}
double scrolled() { return gtk_adjustment_get_value(vadj()); }
// Whether the list has anything to scroll.
bool scrolls() {
    return gtk_adjustment_get_upper(vadj()) > gtk_adjustment_get_page_size(vadj()) + 0.5;
}
// Whether the highest mark is inside the list's visible part.
bool firstMarkInSight() {
    double top = -1;
    const double height = cols()->lineHeight();
    for (int c = 0; c < columns(); ++c)
        for (auto [from, to] : ranges(c, PANGO_ATTR_BACKGROUND)) {
            const double y = cols()->lineTop(c, (int)from);
            if (top < 0 || y < top) top = y;
        }
    if (top < 0) return false;
    return top >= scrolled() && top + height <= scrolled() + gtk_adjustment_get_page_size(vadj());
}

// The panel's text and where its parts are, to tell nothing moved.
struct Geometry {
    std::string text;
    int columns = 0;
    int panelW = 0, panelH = 0, areaW = 0, areaH = 0;
    float areaX = 0, areaY = 0;
    bool operator==(const Geometry& o) const {
        // Within a pixel: the viewport puts its child at whole pixels.
        return text == o.text && columns == o.columns && panelW == o.panelW &&
               panelH == o.panelH && areaW == o.areaW && areaH == o.areaH &&
               std::fabs(areaX - o.areaX) <= 1 && std::fabs(areaY - o.areaY) <= 1;
    }
};
Geometry geometry() {
    Geometry g;
    g.text = hintsText();
    g.columns = columns();
    g.panelW = gtk_widget_get_width(panel());
    g.panelH = gtk_widget_get_height(panel());
    g.areaW = gtk_widget_get_width(area());
    g.areaH = gtk_widget_get_height(area());
    // Where the text is in the panel, its scroll put back: the list
    // scrolling to the first mark moves nothing.
    graphene_point_t p = GRAPHENE_POINT_INIT(0, 0), q;
    if (gtk_widget_compute_point(area(), panel(), &p, &q)) {
        g.areaX = q.x;
        g.areaY = q.y + (float)scrolled();
    }
    return g;
}
Geometry gBefore;
bool gWrap = false;
int gWideColumns = 0, gColumns1400 = 0;
int gWidth0 = 0, gHeight0 = 0;

// Types `text` into the box from empty, by keys, and waits until it holds it.
void typeInBox(const std::string& text) {
    MCGuiStep("the box emptied, then \"" + text + "\" typed", [] {
        gtk_editable_set_text(GTK_EDITABLE(box()), "");
    });
    MCGuiStep("typing \"" + text + "\"", [text] { MCGuiType(text); });
    MCGuiWait("the box holds \"" + text + "\"", 10, [text] { return query() == text; });
}

// The list laid out for the window as it is and drawn so: its text as wide
// as the core said, the scroll as tall as the text.
bool settled() {
    return gtk_widget_get_width(area()) == cols()->textWidth() &&
           (int)gtk_adjustment_get_upper(vadj()) == std::max(cols()->textHeight(),
                                                            (int)gtk_adjustment_get_page_size(vadj()));
}

// The window `w` by `h` pixels.
void resize(int w, int h) {
    MCGuiStep("the window " + std::to_string(w) + "x" + std::to_string(h),
              [w, h] { gtk_window_set_default_size(win(), w, h); });
    MCGuiWait("it is that size", 10, [w, h] {
        return gtk_widget_get_width(GTK_WIDGET(win())) == w &&
               gtk_widget_get_height(GTK_WIDGET(win())) == h;
    });
}

// The room the panel gives its text in the window as it is (main.cpp's
// hintsRoom: the panel's margins, padding, border and the scroll bar's room).
double room() {
    GtkWidget* over = gtk_widget_get_parent(panel());
    return std::max(120.0, gtk_widget_get_width(over) - 2.0 * 18) - 2 * 18 - 2 - 10;
}

// The columns as laid out now, for the notes; and the checks every layout
// keeps: the text no wider than the room (unless one column wraps), as
// wide as the panel's text, every column's last line a shortcut, never a
// heading or a line of text, and every column's lines as tall.
std::string describe() {
    std::string s = std::to_string(columns()) + " columns at x";
    for (int c = 0; c < columns(); ++c) s += " " + std::to_string((int)cols()->x(c));
    s += ", text " + std::to_string(cols()->textWidth()) + "x" +
         std::to_string(cols()->textHeight()) + " in room " + std::to_string((int)room()) +
         ", lines " + std::to_string((int)cols()->lineHeight()) + " high at " +
         std::to_string(cols()->points()) + " pt, scroll " +
         std::to_string((int)gtk_adjustment_get_upper(vadj())) + " in " +
         std::to_string((int)gtk_adjustment_get_page_size(vadj()));
    return s;
}
void checkLayout(const std::string& name) {
    MCGuiNote("%s: %s", name.c_str(), describe().c_str());
    if (!cols()->wraps()) GCHECK(cols()->textWidth() <= room() + 0.5);
    GCHECK(gtk_widget_get_width(area()) == cols()->textWidth());
    const std::vector<HintRow>& rows = cols()->rows();
    for (int c = 0; c < columns(); ++c) {
        const HintRow* last = nullptr;
        size_t n = 0;
        for (const HintRow& r : rows)
            if (r.column == c && !(r.kind == HintRow::Text && r.text.empty())) {
                last = &r;
                ++n;
            }
        if (!GCHECK(last != nullptr)) continue;
        if (last->kind != HintRow::Row)
            MCGuiNote("column %d ends on \"%s\"", c, last->text.c_str());
        GCHECK(last->kind == HintRow::Row);
        // Every line of the column, as Pango lays it out, ends before the
        // next column begins: the widths measured are the widths drawn.
        int w = 0, h = 0;
        pango_layout_get_pixel_size(cols()->layout(c), &w, &h);
        if (c + 1 < columns()) {
            if (cols()->x(c) + w > cols()->x(c + 1))
                MCGuiNote("column %d at %.0f is %d wide, the next at %.0f", c, cols()->x(c), w,
                          cols()->x(c + 1));
            GCHECK(cols()->x(c) + w <= cols()->x(c + 1));
        }
        // No column taller than the text, the tallest as tall.
        const int lines = pango_layout_get_line_count(cols()->layout(c));
        GCHECK(lines * cols()->lineHeight() <= cols()->textHeight());
        (void)n;
    }
    MCGuiSnapshot(GTK_WIDGET(win()), "hints_" + name);
}

}  // namespace

MC_GUI_SUITE(hints_panel) {
    MCGuiStep("a file open, the keyboard in the editor, the hints closed", [] {
        if (shown()) MCGuiKey("ctrl+shift+h");
        Zoom::zoomEditor(0);
        gtk_window_get_default_size(win(), &gWidth0, &gHeight0);
        MCGuiNote("the window was %dx%d (default %dx%d)", gtk_widget_get_width(GTK_WIDGET(win())),
                  gtk_widget_get_height(GTK_WIDGET(win())), gWidth0, gHeight0);
        MCGuiWriteFile(F("hints-search.txt"), "hello\n");
        MCGuiOpenFileKept(F("hints-search.txt"));
    });
    MCGuiWait("it is on show and the hints closed", 10, [] {
        return ed()->currentPath() == F("hints-search.txt") && !shown();
    });
    MCGuiStep("a click in its text", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);

    // ---- columns, as many as the width holds
    resize(2000, 1000);
    MCGuiStep("Ctrl+Shift+H", [] {
        GCHECK_EQ_STR(Keys::label("mutible.toggleShortcutHints", "+"), "Ctrl+Shift+H");
        MCGuiKey("ctrl+shift+h");
    });
    MCGuiWait("the hints are up", 10, shown);
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(area()) > 100 && settled(); });
    MCGuiStep("2000 wide: three or more columns, nothing to scroll", [] {
        checkLayout("2000x1000");
        gWideColumns = columns();
        GCHECK(gWideColumns >= 3);
        GCHECK(!scrolls());
        GCHECK(!cols()->wraps());
        // Tab stops, not spaces: a key, a tab, what it does.
        GCHECK(hintsText().find("Ctrl+Shift+P\tCommand palette") != std::string::npos);
        GCHECK(hintsText().find("   (") == std::string::npos);
        // A pane's state at a stop of its own.
        GCHECK(hintsText().find("Git panel\thidden") != std::string::npos);
    });
    resize(1400, 900);
    MCGuiWait("fewer columns", 10, [] { return columns() < gWideColumns && settled(); });
    MCGuiStep("1400 wide: fewer, two or more", [] {
        checkLayout("1400x900");
        gColumns1400 = columns();
        GCHECK(gColumns1400 >= 2 && gColumns1400 < gWideColumns);
    });
    resize(640, 800);
    MCGuiWait("one column", 10, [] { return columns() == 1 && settled(); });
    MCGuiStep("narrow: one column", [] {
        checkLayout("640x800");
        GCHECK(scrolls());
        GCHECK(gtk_widget_get_width(panel()) <= 640 - 18);
    });
    resize(440, 700);
    MCGuiWait("one column that wraps", 10, [] { return cols()->wraps() && settled(); });
    MCGuiStep("narrower than a column: labels wrap under themselves", [] {
        checkLayout("440x700");
        GCHECK(columns() == 1);
        GCHECK(cols()->textWidth() <= room() + 0.5);
        // Every line a wrap began starts at the label's stop, past the keys.
        PangoLayoutIter* it = pango_layout_get_iter(cols()->layout(0));
        int wrapped = 0, atStop = 0;
        double stop = -1;
        do {
            PangoLayoutLine* line = pango_layout_iter_get_line_readonly(it);
            if (pango_layout_line_is_paragraph_start(line)) continue;
            PangoRectangle logical;
            pango_layout_iter_get_line_extents(it, nullptr, &logical);
            const double x = (double)logical.x / PANGO_SCALE;
            if (stop < 0) stop = x;
            ++wrapped;
            if (x == stop && x > 40) ++atStop;
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        MCGuiNote("%d lines wrapped, %d of them at x %.0f", wrapped, atStop, stop);
        GCHECK(wrapped > 0 && atStop == wrapped);
    });
    resize(1400, 900);
    MCGuiWait("the columns back", 10, [] { return columns() == gColumns1400 && settled(); });
    MCGuiStep("zoomed in three steps", [] {
        for (int i = 0; i < 3; ++i) GCHECK(Zoom::zoomEditor(1));
    });
    MCGuiWait("fewer columns", 10, [] { return columns() < gColumns1400 && settled(); });
    MCGuiStep("zoomed in: the text larger, in fewer columns", [] {
        checkLayout("1400x900_zoomed");
        GCHECK(columns() >= 1);
        GCHECK(cols()->points() > Zoom::desktopPoints());
        Zoom::zoomEditor(0);
    });
    MCGuiWait("the zoom reset, the columns back", 10,
              [] { return columns() == gColumns1400 && settled(); });

    // ---- the search box, on a window short enough to scroll
    resize(1400, 600);
    MCGuiWait("the list follows", 10, [] {
        return settled() && gtk_adjustment_get_page_size(vadj()) < cols()->textHeight();
    });
    MCGuiStep("1400x600: as many columns, and the list scrolls", [] {
        checkLayout("1400x600");
        GCHECK(columns() == gColumns1400);
        GCHECK(scrolls());
    });
    MCGuiStep("the box sits at the panel's top, empty, without the keyboard", [] {
        if (!GCHECK(box() != nullptr)) return;
        GCHECK(gtk_widget_get_parent(box()) == panel());
        GCHECK(GTK_IS_SEARCH_ENTRY(box()));
        GCHECK(query().empty());
        GCHECK(!boxHasKeyboard());
        GCHECK(editorHasKeyboard());
        graphene_rect_t b, s;
        GtkWidget* scroll = gtk_widget_get_ancestor(area(), GTK_TYPE_SCROLLED_WINDOW);
        GCHECK(gtk_widget_compute_bounds(box(), panel(), &b));
        GCHECK(gtk_widget_compute_bounds(scroll, panel(), &s));
        MCGuiNote("box at y %.0f, %.0f high; the list at y %.0f", b.origin.y, b.size.height,
                  s.origin.y);
        GCHECK(b.origin.y + b.size.height <= s.origin.y + 0.5);
        GCHECK(MCGuiOnScreen(box()));
        // Nothing marked or dimmed with the box empty.
        GCHECK(found().empty() && dimmedCount() == 0);
        gBefore = geometry();
        gWrap = Zoom::wordWrap();
        GCHECK_EQ_STR(Keys::label("editor.action.toggleWordWrap", "+"), "Alt+Z");
    });

    // ---- keys go to the box
    MCGuiStep("a letter, then Alt+Z (Word Wrap's key elsewhere)", [] {
        MCGuiType("s");
        MCGuiKey("alt+z");
    });
    MCGuiWait("both are the box's", 10, [] { return query() == "sz"; });
    MCGuiStep("the box has the keyboard; the editor and Word Wrap saw nothing", [] {
        MCGuiNote("box after two keys: \"%s\", keyboard in the box %d", query().c_str(),
                  boxHasKeyboard());
        GCHECK(boxHasKeyboard());
        GCHECK_EQ_STR(MCGuiEditorText(), "hello\n");
        GCHECK(Zoom::wordWrap() == gWrap);
        GCHECK(shown());
    });

    // ---- marks and dims, the layout kept
    typeInBox("save");
    MCGuiStep("what the box finds is marked, the rest dimmed, nothing moved", [] {
        const std::vector<std::string> f = found();
        MCGuiNote("found for save: %s", joined(f).c_str());
        GCHECK(f.size() >= 2);   // Save, Save all
        for (const std::string& s : f) GCHECK(g_ascii_strcasecmp(s.c_str(), "save") == 0);
        GCHECK(!dimmed("Save all"));
        GCHECK(dimmed("Find in file"));
        GCHECK(dimmed("Windows\n"));             // a heading with nothing found under it
        GCHECK(dimmed("Git panel\thidden"));      // a row with its state
        GCHECK(!dimmed("Keyboard Shortcuts"));   // Save is under it
        GCHECK(!dimmed("Tabs\n"));               // Save all is
        GCHECK(firstMarkInSight());
    });
    MCGuiPause(0.3);   // a negative check: the marks' redraw moves nothing
    MCGuiStep("the panel's text, size and place are as they were", [] {
        const Geometry now = geometry();
        MCGuiNote("%d columns, panel %dx%d, text %dx%d at %.0f,%.0f (before: %d columns, "
                  "%dx%d, %dx%d at %.0f,%.0f)",
                  now.columns, now.panelW, now.panelH, now.areaW, now.areaH, now.areaX, now.areaY,
                  gBefore.columns, gBefore.panelW, gBefore.panelH, gBefore.areaW, gBefore.areaH,
                  gBefore.areaX, gBefore.areaY);
        GCHECK(now.text == gBefore.text);
        GCHECK(now == gBefore);
    });
    typeInBox("shift+ctrl+h");
    MCGuiStep("a combination finds its key, its modifiers in any order", [] {
        MCGuiNote("found for shift+ctrl+h: %s", joined(found()).c_str());
        GCHECK(foundHas("Ctrl+Shift+H"));   // as Commands::keyLabel draws it
        GCHECK(!dimmed("Hide these hints"));
        GCHECK(dimmed("New file"));
        // Its row is near the end: the list scrolled to it.
        MCGuiNote("scrolled to %.0f of %.0f", scrolled(), gtk_adjustment_get_upper(vadj()));
        GCHECK(scrolled() > 0 && firstMarkInSight());
        GCHECK(geometry() == gBefore);
    });
    typeInBox("control");
    MCGuiStep("a key's name finds it", [] {
        GCHECK(foundHas("Ctrl"));
        GCHECK(!dimmed("Save all"));
    });
    typeInBox("zzqx");
    MCGuiStep("nothing found: everything dims, the text stays", [] {
        MCGuiNote("dimmed: %zu", dimmedCount());
        GCHECK(found().empty() && dimmedCount() > 20);
        GCHECK(scrolled() == 0);   // back to the top
        GCHECK(hintsText() == gBefore.text);
        GCHECK(geometry() == gBefore);
        MCGuiSnapshot(MCGuiApp().hintsPanel, "hints_search_nothing");
    });
    typeInBox("quit");
    MCGuiStep("Backspace", [] { MCGuiKey("backspace"); });
    MCGuiWait("edits the box", 10, [] { return query() == "qui"; });
    MCGuiStep("and the marks follow", [] {
        const Geometry now = geometry();
        if (!(now == gBefore))
            MCGuiNote("%d columns, panel %dx%d, text %dx%d at %.0f,%.0f, same text %d", now.columns,
                      now.panelW, now.panelH, now.areaW, now.areaH, now.areaX, now.areaY,
                      now.text == gBefore.text);
        GCHECK(foundHas("Qui"));
        GCHECK(!dimmed("Quit"));
        GCHECK(dimmed("Save all"));
        GCHECK(geometry() == gBefore);
        MCGuiSnapshot(GTK_WIDGET(win()), "hints_search_qui");
    });

    // ---- Ctrl keys and function keys stay theirs
    MCGuiStep("Ctrl+B", [] {
        GCHECK(MCGuiApp().layout->sidebarShown());
        MCGuiKey("ctrl+b");
    });
    MCGuiWait("the sidebar goes, as ever", 10, [] { return !MCGuiApp().layout->sidebarShown(); });
    MCGuiStep("the box and the panel as they were", [] {
        GCHECK_EQ_STR(query(), "qui");
        GCHECK(shown());
        MCGuiKey("ctrl+b");
    });
    MCGuiWait("the sidebar back", 10, [] { return MCGuiApp().layout->sidebarShown(); });
    MCGuiStep("F1, the palette's other key", [] { MCGuiKey("f1"); });
    MCGuiWait("the palette opens over the hints", 10,
              [] { return MCGuiApp().palette->isOpen(); });
    MCGuiStep("typing goes to the palette", [] { MCGuiType("zq"); });
    MCGuiWait("the palette has it", 10, [] {
        return std::string(gtk_editable_get_text(GTK_EDITABLE(MCGuiApp().palette->entry())))
                   .find("zq") != std::string::npos;
    });
    MCGuiStep("not the box; Esc closes the palette", [] {
        GCHECK_EQ_STR(query(), "qui");
        MCGuiKey("escape");
    });
    MCGuiWait("the palette closed, the hints still up", 10,
              [] { return !MCGuiApp().palette->isOpen() && shown(); });
    MCGuiStep("the box kept its text", [] { GCHECK_EQ_STR(query(), "qui"); });

    // ---- Esc
    MCGuiStep("the keyboard in the editor again, then Esc", [] {
        gtk_widget_grab_focus(ed()->textView());
    });
    MCGuiWait("the editor has it", 5, editorHasKeyboard);
    MCGuiStep("Esc", [] { MCGuiKey("escape"); });
    MCGuiWait("clears the box", 10, [] { return query().empty(); });
    MCGuiStep("the panel stays, unmarked", [] {
        GCHECK(shown());
        GCHECK(found().empty() && dimmedCount() == 0);
        MCGuiKey("escape");
    });
    MCGuiWait("a second Esc closes the panel", 10, [] { return !shown(); });
    MCGuiStep("the keyboard back in the editor, its text untouched", [] {
        GCHECK(!boxHasKeyboard());
        GCHECK(editorHasKeyboard());
        GCHECK_EQ_STR(MCGuiEditorText(), "hello\n");
    });

    // ---- a click, and Ctrl+Shift+H
    MCGuiStep("the hints again", [] { MCGuiKey("ctrl+shift+h"); });
    MCGuiWait("up, the box empty", 10, [] { return shown() && query().empty(); });
    MCGuiClickWhenReady("the box", [] { return box(); });
    MCGuiWait("a click puts the keyboard in the box", 5, boxHasKeyboard);
    MCGuiStep("typing \"tab\"", [] { MCGuiType("tab"); });
    MCGuiWait("it finds something", 10, [] { return query() == "tab" && !found().empty(); });
    MCGuiStep("Ctrl+Shift+H", [] { MCGuiKey("ctrl+shift+h"); });
    MCGuiWait("closes the panel", 10, [] { return !shown(); });
    MCGuiStep("and empties the box, the keyboard in the editor", [] {
        GCHECK(query().empty());
        GCHECK(!boxHasKeyboard());
        GCHECK(editorHasKeyboard());
        GCHECK_EQ_STR(MCGuiEditorText(), "hello\n");
        GCHECK(Zoom::wordWrap() == gWrap);
    });
    MCGuiStep("the window as it was", [] {
        gtk_window_set_default_size(win(), gWidth0 > 0 ? gWidth0 : 1100, gHeight0 > 0 ? gHeight0 : 720);
    });
    MCGuiWait("it is", 10, [] {
        return gtk_widget_get_width(GTK_WIDGET(win())) == (gWidth0 > 0 ? gWidth0 : 1100);
    });
}
