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
// Keys (the owner's, 2026-11-19): Ctrl, Alt and Super in words, each in
// its own colour from the theme (Dark Modern's, then Light Modern's after
// a theme change), Shift as ⇧, thin spaces between the parts ("Ctrl ⇧ P"),
// no "Shift" word and no "+" in any key, and no glyph the fonts lack.
//
// The search box: with the panel up (Ctrl+Shift+H) a letter and an
// Alt+letter (Word Wrap's Alt+Z) type into the box, never the editor or a
// command, the keyboard taken from the editor at the first; what the box
// finds comes first, at the top of the first column under its sections'
// headings (only those with a match, in their order), marked, and the rest
// after it, dimmed, the panel no smaller, the list scrolled to its top;
// keys are found by name and as combinations in any order ("shift+ctrl+h",
// "control"); a keystroke that finds the same rows lays nothing out again
// (and a relayout's CPU time is bounded); nothing found dims everything in
// place; Backspace edits the box; a Ctrl key (Ctrl+B) still runs its command and a function key
// (F1) is still the palette's, whose typing stays its own; Esc clears the
// box, then closes the panel, the keyboard back in the editor, the list
// exactly as it was before the search; a click
// puts the keyboard in the box, and Ctrl+Shift+H closes the panel and
// empties it.
#include "Harness.h"

#include "Commands.h"
#include "HintsSearchBox.h"
#include "ThemeCss.h"

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
std::vector<HintRow> gPlainRows;   // the list without a query
int gLayouts = 0;
std::string gSettings;   // the settings file as the suite found it
bool gWrap = false;
int gWideColumns = 0, gColumns1400 = 0;
int gWidth0 = 0, gHeight0 = 0;

// The key a thin space parts (Commands::kLinuxKeyJoin): K("Ctrl", "⇧", "P").
std::string K(std::initializer_list<const char*> parts) {
    std::string s;
    for (const char* p : parts) s += (s.empty() ? "" : Commands::kLinuxKeyJoin) + std::string(p);
    return s;
}

uint32_t rgbOf(const Rgba& c) { return (uint32_t)(c.r << 16 | c.g << 8 | c.b); }

// The foreground colour drawn at byte `at` of a column (the last such
// attribute over it, as Pango draws it).
uint32_t colorAt(int column, size_t at) {
    uint32_t rgb = 0xFFFFFFFF;
    PangoLayout* layout = cols()->layout(column);
    PangoAttrList* list = layout ? pango_layout_get_attributes(layout) : nullptr;
    if (!list) return rgb;
    PangoAttrIterator* it = pango_attr_list_get_iterator(list);
    do {
        int from = 0, to = 0;
        pango_attr_iterator_range(it, &from, &to);
        if ((size_t)from <= at && at < (size_t)to) {
            if (auto* a = (PangoAttrColor*)pango_attr_iterator_get(it, PANGO_ATTR_FOREGROUND))
                rgb = (uint32_t)((a->color.red >> 8) << 16 | (a->color.green >> 8) << 8 |
                                 (a->color.blue >> 8));
            break;
        }
    } while (pango_attr_iterator_next(it));
    pango_attr_iterator_destroy(it);
    return rgb;
}

// Where `word` first starts a key's part in the columns' keys: column and
// byte, or {-1, 0}.
std::pair<int, size_t> keyWord(const std::string& word) {
    for (const HintRow& r : cols()->rows()) {
        if (r.kind != HintRow::Row || r.column < 0) continue;
        const size_t at = r.key.find(word);
        if (at != std::string::npos) return {r.column, r.start + at};
    }
    return {-1, 0};
}

// Whether a column's bytes [from, to) are all drawn dimmed.
bool dimmedRange(int column, size_t from, size_t to) {
    for (auto [a, b] : ranges(column, PANGO_ATTR_FOREGROUND_ALPHA))
        if (a <= from && to <= b) return true;
    return false;
}

// The headings, in order, of the sections in which `query` finds a row of
// the list without a query.
std::vector<std::string> headingsFound(const std::string& query) {
    const std::vector<bool> f = HintsSearchBox::found(gPlainRows, query);
    std::vector<std::string> out;
    std::string heading;
    bool added = false;
    for (size_t i = 0; i < gPlainRows.size(); ++i) {
        if (gPlainRows[i].kind == HintRow::Heading) {
            heading = gPlainRows[i].text;
            added = false;
        } else if (gPlainRows[i].kind == HintRow::Text && gPlainRows[i].text.empty()) {
            heading.clear();   // the blank line before "Hide these hints" heads no heading
            added = false;
        } else if (i < f.size() && f[i] && !added && !heading.empty()) {
            out.push_back(heading);
            added = true;
        }
    }
    return out;
}

// The list as laid out for the query: what was found first, in their
// sections, then the rest. Checks that every row not of the rest comes
// before every row of the rest, that the first part's headings are
// `headings`, that every row found is marked and every row of the rest
// dimmed, and that the first part starts the first column; notes the
// first part.
void checkFirst(const std::string& query) {
    const std::vector<HintRow>& rows = cols()->rows();
    std::vector<std::string> heads, first;
    bool inRest = false, ordered = true;
    size_t restRows = 0, restDimmed = 0;
    for (const HintRow& r : rows) {
        if (r.rest) inRest = true;
        else if (inRest) ordered = false;
        const bool blank = r.kind == HintRow::Text && r.text.empty();
        if (!r.rest && r.kind == HintRow::Heading) heads.push_back(r.text);
        if (!r.rest && !blank) first.push_back(r.kind == HintRow::Row ? r.key + "\t" + r.text : r.text);
        if (r.rest && !blank) {
            ++restRows;
            if (dimmedRange(r.column, r.start, r.end)) ++restDimmed;
        }
    }
    MCGuiNote("first for \"%s\": %s", query.c_str(), joined(first).c_str());
    MCGuiNote("then %zu rows of the rest, %zu of them dimmed", restRows, restDimmed);
    GCHECK(ordered);
    GCHECK(!first.empty());
    GCHECK(restRows > 20 && restDimmed == restRows);
    const std::vector<std::string> want = headingsFound(query);
    if (heads != want) MCGuiNote("headings %s, expected %s", joined(heads).c_str(), joined(want).c_str());
    GCHECK(heads == want);
    // The first part starts the first column, at its top.
    if (GCHECK(!rows.empty())) {
        GCHECK(rows[0].column == 0 && rows[0].start == 0 && !rows[0].rest);
    }
}

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
        GCHECK(hintsText().find(K({"Ctrl", "⇧", "P"}) + "\tCommand palette") != std::string::npos);
        GCHECK(hintsText().find("   (") == std::string::npos);
        // A pane's state at a stop of its own.
        GCHECK(hintsText().find("Git panel\thidden") != std::string::npos);
    });
    MCGuiStep("the keys: Ctrl, Alt and Super words in colours of their own, ⇧, thin spaces", [] {
        const std::string t = hintsText();
        GCHECK(t.find(K({"Ctrl", "⇧", "H"}) + "\tHide these hints") != std::string::npos);
        GCHECK(t.find(K({"Alt", "Z"}) + "\t") != std::string::npos);
        GCHECK(t.find(K({"⇧", "⇥"})) != std::string::npos);
        // No "Shift" word and no "+" in any key.
        size_t keys = 0, ctrl = 0, alt = 0;
        for (const HintRow& r : cols()->rows()) {
            if (r.kind != HintRow::Row || r.key.empty()) continue;
            ++keys;
            if (r.key.find("Shift") != std::string::npos || r.key.find('+') != std::string::npos)
                MCGuiNote("key \"%s\" for %s", r.key.c_str(), r.text.c_str());
            GCHECK(r.key.find("Shift") == std::string::npos);
            GCHECK(r.key.find('+') == std::string::npos);
            if (r.key.find("Ctrl") != std::string::npos) ++ctrl;
            if (r.key.find("Alt") != std::string::npos) ++alt;
        }
        MCGuiNote("%zu keys, %zu with Ctrl, %zu with Alt", keys, ctrl, alt);
        GCHECK(keys > 60 && ctrl > 30 && alt > 5);
        // Every glyph drawn is in a font (⇧ ⏎ ⇥ ⎋ ⌫ ⌦ and the arrows).
        for (int c = 0; c < columns(); ++c) {
            const int unknown = pango_layout_get_unknown_glyphs_count(cols()->layout(c));
            if (unknown) MCGuiNote("column %d draws %d unknown glyphs", c, unknown);
            GCHECK(unknown == 0);
        }
        // Each modifier its own colour, the same everywhere; the rest of a key blue.
        const theme::HintsModifierColors want = theme::hintsModifierColors(Settings::parse(""));
        GCHECK(rgbOf(cols()->modifierColors().ctrl) == rgbOf(want.ctrl));
        const auto [cc, ca] = keyWord("Ctrl");
        const auto [ac, aa] = keyWord("Alt");
        const auto [sc, sa] = keyWord("⇧");
        if (GCHECK(cc >= 0 && ac >= 0 && sc >= 0)) {
            MCGuiNote("Ctrl #%06X, Alt #%06X, ⇧ #%06X", colorAt(cc, ca), colorAt(ac, aa),
                      colorAt(sc, sa));
            GCHECK(colorAt(cc, ca) == rgbOf(want.ctrl));
            GCHECK(colorAt(ac, aa) == rgbOf(want.alt));
            GCHECK(colorAt(sc, sa) == theme::kHintsKeyColor);
        }
        size_t seen = 0, same = 0;
        for (const HintRow& r : cols()->rows()) {
            if (r.kind != HintRow::Row) continue;
            for (size_t at = r.key.find("Ctrl"); at != std::string::npos;
                 at = r.key.find("Ctrl", at + 4)) {
                ++seen;
                if (colorAt(r.column, r.start + at) == rgbOf(want.ctrl)) ++same;
            }
        }
        GCHECK(seen > 30 && same == seen);
        MCGuiSnapshot(GTK_WIDGET(win()), "hints_keys_dark");
    });
    MCGuiStep("Light Modern", [] {
        gchar* text = nullptr;
        if (g_file_get_contents(g_getenv("MUTIBLE_SETTINGS"), &text, nullptr, nullptr)) gSettings = text;
        g_free(text);
        MCGuiSetSettings("theme = light-modern\n" + gSettings);
    });
    MCGuiWait("the keys take its colours", 10, [] {
        const theme::HintsModifierColors want =
            theme::hintsModifierColors(Settings::parse("theme = light-modern\n"));
        const auto [cc, ca] = keyWord("Ctrl");
        return cc >= 0 && colorAt(cc, ca) == rgbOf(want.ctrl);
    });
    MCGuiStep("each its own, readable on the panel", [] {
        const theme::HintsModifierColors want =
            theme::hintsModifierColors(Settings::parse("theme = light-modern\n"));
        const auto [cc, ca] = keyWord("Ctrl");
        const auto [ac, aa] = keyWord("Alt");
        const Rgba bg = Rgba::hex(theme::kHintsBackground);
        MCGuiNote("Light Modern: Ctrl #%06X (%.1f:1), Alt #%06X (%.1f:1), Super #%06X (%.1f:1)",
                  rgbOf(want.ctrl), theme::contrast(want.ctrl, bg), rgbOf(want.alt),
                  theme::contrast(want.alt, bg), rgbOf(want.super), theme::contrast(want.super, bg));
        GCHECK(colorAt(ac, aa) == rgbOf(want.alt));
        GCHECK(rgbOf(want.ctrl) != rgbOf(theme::hintsModifierColors(Settings::parse("")).ctrl));
        GCHECK(theme::contrast(want.ctrl, bg) >= 4.5 && theme::contrast(want.alt, bg) >= 4.5);
        (void)cc;
        (void)ca;
        MCGuiSnapshot(GTK_WIDGET(win()), "hints_keys_light");
        MCGuiSetSettings(gSettings);
    });
    MCGuiWait("Dark Modern's again", 10, [] {
        const auto [cc, ca] = keyWord("Ctrl");
        return cc >= 0 &&
               colorAt(cc, ca) == rgbOf(theme::hintsModifierColors(Settings::parse("")).ctrl);
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
        gPlainRows = cols()->rows();
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

    // ---- what the box finds first, the rest after it dimmed
    typeInBox("save");
    MCGuiWait("laid out for it", 5, settled);
    MCGuiStep("what the box finds comes first, marked, under its sections' headings", [] {
        const std::vector<std::string> f = found();
        MCGuiNote("found for save: %s", joined(f).c_str());
        GCHECK(f.size() >= 2);   // Save, Save all
        for (const std::string& s : f) GCHECK(g_ascii_strcasecmp(s.c_str(), "save") == 0);
        // Its matches are in two sections, whose headings both lead.
        GCHECK(headingsFound("save").size() >= 2);
        checkFirst("save");
        // The Save rows at the top of the first column, under the first heading.
        const std::vector<HintRow>& rows = cols()->rows();
        if (GCHECK(rows.size() > 2)) {
            GCHECK(rows[0].kind == HintRow::Heading && rows[0].text == "Keyboard Shortcuts");
            GCHECK(rows[1].kind == HintRow::Row && rows[1].text == "Save" && rows[1].column == 0);
            GCHECK(cols()->lineTop(0, (int)rows[1].start) == cols()->lineHeight());
        }
        GCHECK(!dimmed("Save all"));
        GCHECK(dimmed("Find in file"));
        GCHECK(dimmed("Windows\n"));             // a heading with nothing found under it
        GCHECK(dimmed("Git panel\thidden"));      // a row with its state
        GCHECK(!dimmed("Keyboard Shortcuts"));   // Save is under it
        GCHECK(!dimmed("Tabs\n"));               // Save all is
        // The list at its top, the first mark in sight; the panel no smaller.
        GCHECK(scrolled() == 0);
        GCHECK(firstMarkInSight());
        const Geometry now = geometry();
        MCGuiNote("%d columns, panel %dx%d, text %dx%d (before: %d columns, %dx%d, %dx%d)",
                  now.columns, now.panelW, now.panelH, now.areaW, now.areaH, gBefore.columns,
                  gBefore.panelW, gBefore.panelH, gBefore.areaW, gBefore.areaH);
        GCHECK(now.columns <= gBefore.columns);
        GCHECK(now.panelW >= gBefore.panelW && now.panelH >= gBefore.panelH);
        MCGuiSnapshot(GTK_WIDGET(win()), "hints_search_save");
        gLayouts = cols()->layouts();
    });
    MCGuiStep("a space typed: the same rows found", [] { MCGuiType(" "); });
    MCGuiWait("the box holds \"save \"", 10, [] { return query() == "save "; });
    MCGuiStep("so nothing is laid out again; Backspace", [] {
        MCGuiNote("layouts: %d, then %d", gLayouts, cols()->layouts());
        GCHECK(cols()->layouts() == gLayouts);
        GCHECK(foundHas("Save"));
        MCGuiKey("backspace");
    });
    MCGuiWait("the box holds \"save\"", 10, [] { return query() == "save"; });
    MCGuiStep("still none; what a relayout and a mark cost on this thread", [] {
        GCHECK(cols()->layouts() == gLayouts);
        // A query that finds other rows lays the list out again; one that
        // finds the same only marks. CPU time, the median of five each (the
        // worst of five was one preemption away from failing under load: a
        // mark at 12 ms against 5 to 6 quietly), and a mark compared with
        // the relayouts measured beside it, as the room for CI's machine.
        std::vector<double> relayouts, markings;
        for (int i = 0; i < 5; ++i) {
            double c0 = MCGuiThreadCpuMs();
            cols()->mark("control");
            relayouts.push_back(MCGuiThreadCpuMs() - c0);
            c0 = MCGuiThreadCpuMs();
            cols()->mark("control ");
            markings.push_back(MCGuiThreadCpuMs() - c0);
            c0 = MCGuiThreadCpuMs();
            cols()->mark("save");
            relayouts.push_back(MCGuiThreadCpuMs() - c0);
        }
        auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const double relayout = median(relayouts), marks = median(markings);
        MCGuiNote("a relayout %.2f ms of CPU, a mark alone %.2f ms (medians; %d layouts)", relayout,
                  marks, cols()->layouts() - gLayouts);
        GCHECK(cols()->layouts() - gLayouts == 10);
        // The relative check is the one that holds: on this ThinkPad both
        // medians grew 2.5 times under other builds' load (a relayout 11.6
        // to 27.5 ms, a mark 6.2 to 15.6) while a mark stayed 0.5 to 0.7 of
        // a relayout. The bounds only catch a gross regression (two frames).
        GCHECK(marks < relayout);   // marking alone is the cheaper path
        GCHECK_TIME(relayout, 50, "a keystroke that finds other rows");
        GCHECK_TIME(marks, 32, "a keystroke that finds the same rows");
    });
    typeInBox("shift+ctrl+h");
    MCGuiWait("laid out for it", 5, settled);
    MCGuiStep("a combination finds its key, its modifiers in any order, at the top", [] {
        MCGuiNote("found for shift+ctrl+h: %s", joined(found()).c_str());
        GCHECK(foundHas(K({"Ctrl", "⇧", "H"})));   // as Commands::keyLabel draws it
        GCHECK(!dimmed("Hide these hints"));
        GCHECK(dimmed("New file"));
        checkFirst("shift+ctrl+h");
        // Its row was near the end; now it is at the top, and so is the list.
        MCGuiNote("scrolled to %.0f of %.0f", scrolled(), gtk_adjustment_get_upper(vadj()));
        GCHECK(scrolled() == 0 && firstMarkInSight());
    });
    typeInBox("control");
    MCGuiWait("laid out for it", 5, settled);
    MCGuiStep("a key's name finds it", [] {
        GCHECK(foundHas("Ctrl"));
        GCHECK(!dimmed("Save all"));
        checkFirst("control");
    });
    typeInBox("zzqx");
    MCGuiWait("laid out as before", 5, [] { return settled() && geometry() == gBefore; });
    MCGuiStep("nothing found: everything dims in place, the text as it was", [] {
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
    MCGuiWait("laid out for it", 5, settled);
    MCGuiStep("and what it finds follows", [] {
        GCHECK(foundHas("Qui"));
        GCHECK(!dimmed("Quit"));
        GCHECK(dimmed("Save all"));
        checkFirst("qui");
        GCHECK(scrolled() == 0);
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
    MCGuiWait("the list exactly as it was before the search", 5,
              [] { return settled() && geometry() == gBefore; });
    MCGuiStep("the panel stays, unmarked, as it was", [] {
        GCHECK(shown());
        GCHECK(found().empty() && dimmedCount() == 0);
        GCHECK(hintsText() == gBefore.text);
        GCHECK(geometry() == gBefore);
        GCHECK(scrolled() == 0);
        bool none = true;
        for (const HintRow& r : cols()->rows()) none = none && !r.rest;
        GCHECK(none);
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
