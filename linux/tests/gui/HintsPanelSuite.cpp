// HintsPanelSuite.cpp — the shortcut hints' search box
// (`./build/mutible-guitest hints_panel`, HintsSearchBox.h), the Mac's
// hints_panel steps (PageMeasureSuite.mm) by real keys: with the panel up
// (Ctrl+Shift+H) a letter and an Alt+letter (Word Wrap's Alt+Z) type into
// the box, never the editor or a command, the keyboard taken from the
// editor at the first; what the box finds is marked and the rest dimmed
// (a heading with nothing found under it too), keys found by name and as
// combinations in any order ("shift+ctrl+h", "control"), the panel's text
// and size unchanged; Backspace edits the box; a Ctrl key (Ctrl+B) still
// runs its command and a function key (F1) is still the palette's, whose
// typing stays its own; Esc clears the box, then closes the panel, the
// keyboard back in the editor; a click puts the keyboard in the box, and
// Ctrl+Shift+H closes the panel and empties it.
#include "Harness.h"

#include "Editor.h"
#include "Keys.h"
#include "PalettePanel.h"
#include "WindowLayout.h"
#include "Zoom.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& n) { return MCGuiProjectFile(n); }
GtkWidget* box() { return MCGuiApp().hintsSearch; }
GtkWidget* label() { return MCGuiApp().hintsLabel; }
bool shown() { return gtk_widget_get_visible(MCGuiApp().hintsPanel); }
std::string query() { return gtk_editable_get_text(GTK_EDITABLE(box())); }
std::string hintsText() { return gtk_label_get_text(GTK_LABEL(label())); }

GtkWidget* focus() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool editorHasKeyboard() { return focus() == ed()->textView(); }
bool boxHasKeyboard() {
    GtkWidget* f = focus();
    return f && (f == box() || gtk_widget_is_ancestor(f, box()));
}

// The label's attributes of one type, as byte ranges.
std::vector<std::pair<size_t, size_t>> ranges(PangoAttrType type) {
    std::vector<std::pair<size_t, size_t>> out;
    PangoAttrList* list = gtk_label_get_attributes(GTK_LABEL(label()));
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
    const std::string t = hintsText();
    std::vector<std::string> out;
    for (auto [from, to] : ranges(PANGO_ATTR_BACKGROUND))
        if (to <= t.size() && from < to) out.push_back(t.substr(from, to - from));
    return out;
}
bool foundHas(const std::string& what) {
    for (const std::string& f : found())
        if (f == what) return true;
    return false;
}
size_t dimmedCount() { return ranges(PANGO_ATTR_FOREGROUND_ALPHA).size(); }
// Whether `what`'s first place in the text is drawn dimmed.
bool dimmed(const std::string& what) {
    const size_t at = hintsText().find(what);
    if (at == std::string::npos) return false;
    for (auto [from, to] : ranges(PANGO_ATTR_FOREGROUND_ALPHA))
        if (from <= at && at < to) return true;
    return false;
}
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

// The list's scroll: its adjustment, and how far down it is.
GtkAdjustment* vadj() {
    GtkWidget* scroll = gtk_widget_get_ancestor(label(), GTK_TYPE_SCROLLED_WINDOW);
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
}
double scrolled() { return gtk_adjustment_get_value(vadj()); }
// Whether the first mark is inside the list's visible part.
bool firstMarkInSight() {
    size_t first = SIZE_MAX;
    for (auto [from, to] : ranges(PANGO_ATTR_BACKGROUND)) first = std::min(first, from);
    if (first == SIZE_MAX) return false;
    PangoRectangle at;
    pango_layout_index_to_pos(gtk_label_get_layout(GTK_LABEL(label())), (int)first, &at);
    int ox = 0, oy = 0;
    gtk_label_get_layout_offsets(GTK_LABEL(label()), &ox, &oy);
    const double top = oy + (double)at.y / PANGO_SCALE, bottom = top + (double)at.height / PANGO_SCALE;
    return top >= scrolled() && bottom <= scrolled() + gtk_adjustment_get_page_size(vadj());
}

// The panel's text and where its parts are, to tell nothing moved.
struct Geometry {
    std::string text;
    int panelW = 0, panelH = 0, labelW = 0, labelH = 0;
    float labelX = 0, labelY = 0;
    bool operator==(const Geometry& o) const {
        return text == o.text && panelW == o.panelW && panelH == o.panelH && labelW == o.labelW &&
               labelH == o.labelH && labelX == o.labelX && labelY == o.labelY;
    }
};
Geometry geometry() {
    Geometry g;
    g.text = hintsText();
    GtkWidget* panel = MCGuiApp().hintsPanel;
    g.panelW = gtk_widget_get_width(panel);
    g.panelH = gtk_widget_get_height(panel);
    g.labelW = gtk_widget_get_width(label());
    g.labelH = gtk_widget_get_height(label());
    // Where the label is in the panel, its scroll put back: the list
    // scrolling to the first mark moves nothing.
    graphene_point_t p = GRAPHENE_POINT_INIT(0, 0), q;
    if (gtk_widget_compute_point(label(), panel, &p, &q)) {
        g.labelX = q.x;
        g.labelY = q.y + (float)scrolled();
    }
    return g;
}
Geometry gBefore;
bool gWrap = false;

// Types `text` into the box from empty, by keys, and waits until it holds it.
void typeInBox(const std::string& text) {
    MCGuiStep("the box emptied, then \"" + text + "\" typed", [] {
        gtk_editable_set_text(GTK_EDITABLE(box()), "");
    });
    MCGuiStep("typing \"" + text + "\"", [text] { MCGuiType(text); });
    MCGuiWait("the box holds \"" + text + "\"", 10, [text] { return query() == text; });
}

}  // namespace

MC_GUI_SUITE(hints_panel) {
    MCGuiStep("a file open, the keyboard in the editor, the hints closed", [] {
        if (shown()) MCGuiKey("ctrl+shift+h");
        MCGuiWriteFile(F("hints-search.txt"), "hello\n");
        MCGuiOpenFileKept(F("hints-search.txt"));
    });
    MCGuiWait("it is on show and the hints closed", 10, [] {
        return ed()->currentPath() == F("hints-search.txt") && !shown();
    });
    MCGuiStep("a click in its text", [] { MCGuiClick(ed()->textView()); });
    MCGuiWait("the editor has the keyboard", 5, editorHasKeyboard);
    MCGuiStep("Ctrl+Shift+H", [] {
        GCHECK_EQ_STR(Keys::label("mutible.toggleShortcutHints", "+"), "Ctrl+Shift+H");
        MCGuiKey("ctrl+shift+h");
    });
    MCGuiWait("the hints are up", 10, shown);
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(label()) > 100; });
    MCGuiStep("the box sits at the panel's top, empty, without the keyboard", [] {
        if (!GCHECK(box() != nullptr)) return;
        GCHECK(gtk_widget_get_parent(box()) == MCGuiApp().hintsPanel);
        GCHECK(GTK_IS_SEARCH_ENTRY(box()));
        GCHECK(query().empty());
        GCHECK(!boxHasKeyboard());
        GCHECK(editorHasKeyboard());
        graphene_rect_t b, s;
        GtkWidget* scroll = gtk_widget_get_parent(gtk_widget_get_parent(label()));
        GCHECK(gtk_widget_compute_bounds(box(), MCGuiApp().hintsPanel, &b));
        GCHECK(gtk_widget_compute_bounds(scroll, MCGuiApp().hintsPanel, &s));
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
        GCHECK(!dimmed("Keyboard Shortcuts"));   // Save is under it
        GCHECK(!dimmed("Tabs\n"));               // Save all is
        GCHECK(firstMarkInSight());
    });
    MCGuiPause(0.3);   // a negative check: the marks' redraw moves nothing
    MCGuiStep("the panel's text, size and place are as they were", [] {
        const Geometry now = geometry();
        MCGuiNote("panel %dx%d, label %dx%d at %.0f,%.0f (before: %dx%d, %dx%d at %.0f,%.0f)",
                  now.panelW, now.panelH, now.labelW, now.labelH, now.labelX, now.labelY,
                  gBefore.panelW, gBefore.panelH, gBefore.labelW, gBefore.labelH,
                  gBefore.labelX, gBefore.labelY);
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
        MCGuiSnapshot(MCGuiApp().hintsPanel, "hints_search_nothing");
    });
    typeInBox("quit");
    MCGuiStep("Backspace", [] { MCGuiKey("backspace"); });
    MCGuiWait("edits the box", 10, [] { return query() == "qui"; });
    MCGuiStep("and the marks follow", [] {
        GCHECK(foundHas("Qui"));
        GCHECK(!dimmed("Quit"));
        GCHECK(dimmed("Save all"));
        MCGuiSnapshot(MCGuiApp().hintsPanel, "hints_search_qui");
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
}
