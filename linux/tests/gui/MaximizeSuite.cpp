// MaximizeSuite.cpp — Maximize Pane (`./build/minicode-guitest maximize`,
// linux/PARITY.md item 1.11; Ctrl+Alt+M, the Mac's Ctrl+Cmd+M), the
// counterpart of the Maximize Pane steps of the Mac's zoomfit suite
// (tests/gui/ZoomFitSuite.mm), by real keys and clicks in a window of its
// own: the pane with the keyboard (the browser, the terminal, the editor)
// takes the whole right side with no bars, the others hidden as they are,
// and the same key puts everything back where it was; anywhere else (the
// tree) it is the editor; showing another pane ends it, whether by its
// toggle (which shows a pane hidden only by Maximize Pane rather than
// closing it), Focus Editor, or opening a file from the tree; and hiding
// the maximized pane ends it too.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"
#include "Keys.h"
#include "WindowLayout.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#endif

#include <cmath>
#include <string>

#if defined(MINICODE_ENABLE_TERMINAL) && defined(MINICODE_ENABLE_BROWSER)

namespace {

WindowLayout* L() { return MCGuiApp().layout; }
GtkWidget* area() { return L()->area(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool focusIn(GtkWidget* w) { return Keys::within(focusWidget(), w); }

bool on(GtkWidget* w) {
    return w && gtk_widget_get_mapped(w) && gtk_widget_get_width(w) > 0 &&
           gtk_widget_get_height(w) > 0;
}

graphene_rect_t box(GtkWidget* w) {
    graphene_rect_t r;
    if (!w || !gtk_widget_compute_bounds(w, area(), &r)) graphene_rect_init(&r, 0, 0, 0, 0);
    return r;
}

bool near(double a, double b) { return std::fabs(a - b) <= 1.0; }
bool same(const graphene_rect_t& a, const graphene_rect_t& b) {
    return near(a.origin.x, b.origin.x) && near(a.origin.y, b.origin.y) &&
           near(a.size.width, b.size.width) && near(a.size.height, b.size.height);
}

GtkWidget* editorPane() { return L()->editorPane(); }
GtkWidget* browserPane() { return L()->browserPane(); }
GtkWidget* terminalPane() { return L()->terminalPane(); }

// `pane` alone, the whole of the right side, no bars.
bool alone(GtkWidget* pane) {
    const graphene_rect_t r = box(pane);
    for (GtkWidget* p : {editorPane(), browserPane(), terminalPane()})
        if (p != pane && on(p)) return false;
    return L()->maximized() && on(pane) && near(r.origin.x, 0) && near(r.origin.y, 0) &&
           near(r.size.width, gtk_widget_get_width(area())) &&
           near(r.size.height, gtk_widget_get_height(area())) && !on(L()->browserBar()) &&
           !on(L()->terminalBar());
}

graphene_rect_t gEditor, gBrowser, gTerminal;

// All three as they were before, and no pane maximized.
bool asBefore() {
    return !L()->maximized() && on(editorPane()) && on(browserPane()) && on(terminalPane()) &&
           same(box(editorPane()), gEditor) && same(box(browserPane()), gBrowser) &&
           same(box(terminalPane()), gTerminal) && on(L()->browserBar()) && on(L()->terminalBar());
}

size_t gWindows = 0;

}  // namespace

MC_GUI_SUITE(maximize) {
    MCGuiStep("a file, and a window of its own", [] {
        MCGuiWriteFile(F("max-a.txt"), "the first file\n");
        MCGuiWriteFile(F("max-b.txt"), "the second file\n");
        gWindows = MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("the window", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("the file in it, then the terminal and the browser by their keys", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiOpenFileKept(F("max-a.txt"));
        if (!L()->terminalShown()) MCGuiKey("ctrl+shift+t");
    });
    MCGuiWait("the terminal", 10, [] { return on(terminalPane()); });
    MCGuiStep("the browser", [] {
        if (!L()->browserShown()) MCGuiKey("ctrl+shift+b");
    });
    MCGuiWait("all three, the keyboard in the browser", 10, [] {
        return on(editorPane()) && on(browserPane()) && on(terminalPane()) && focusIn(browserPane());
    });
    MCGuiStep("where each is", [] {
        gEditor = box(editorPane());
        gBrowser = box(browserPane());
        gTerminal = box(terminalPane());
        GCHECK(!L()->maximized());
    });

    // ---- from the browser
    MCGuiStep("Ctrl+Alt+M from the browser", [] { MCGuiKey("ctrl+alt+m"); });
    MCGuiWait("the browser alone, all of the right side, no bars", 10, [] { return alone(browserPane()); });
    MCGuiStep("the others are still what was asked for; the keyboard stays", [] {
        GCHECK(L()->editorShown() && L()->terminalShown());
        GCHECK(focusIn(browserPane()));
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "maximize-browser");
        MCGuiKey("ctrl+alt+m");
    });
    MCGuiWait("again: everything back where it was", 10, asBefore);

    // ---- from the terminal
    MCGuiStep("a click in the terminal, Ctrl+Alt+M", [] {
        MCGuiClick(MCGuiApp().terminal->widget());
        MCGuiKey("ctrl+alt+m");
    });
    MCGuiWait("the terminal alone", 10, [] { return alone(terminalPane()); });
    MCGuiStep("Ctrl+Alt+M from the terminal again", [] {
        GCHECK(MCGuiApp().terminal->owns(focusWidget()));
        MCGuiKey("ctrl+alt+m");
    });
    MCGuiWait("back", 10, asBefore);

    // ---- from the editor; Toggle Terminal shows the terminal
    MCGuiStep("a click in the editor, Ctrl+Alt+M", [] {
        MCGuiClick(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+alt+m");
    });
    MCGuiWait("the editor alone", 10, [] { return alone(editorPane()); });
    MCGuiStep("Toggle Terminal (Ctrl+Shift+T)", [] { MCGuiKey("ctrl+shift+t"); });
    MCGuiWait("the terminal shown, not closed: back as it was, the keyboard in it", 10, [] {
        return asBefore() && L()->terminalShown() && MCGuiApp().terminal->owns(focusWidget());
    });

    // ---- the terminal maximized; Focus Editor ends it
    MCGuiStep("Ctrl+Alt+M from the terminal", [] { MCGuiKey("ctrl+alt+m"); });
    MCGuiWait("the terminal alone", 10, [] { return alone(terminalPane()); });
    MCGuiStep("Focus Editor (Ctrl+1)", [] { MCGuiKey("ctrl+1"); });
    MCGuiWait("the editor back with the keyboard, everything as it was", 10, [] {
        return asBefore() && gtk_widget_has_focus(MCGuiApp().editor->textView());
    });

    // ---- the browser maximized; a file opened from the tree ends it
    MCGuiStep("the browser maximized from its own keyboard", [] {
        MCGuiKey("ctrl+shift+b");   // shown already: hidden...
        MCGuiKey("ctrl+shift+b");   // ...and shown again, with the keyboard
    });
    MCGuiWait("the browser has the keyboard", 10, [] { return asBefore() && focusIn(browserPane()); });
    MCGuiStep("Ctrl+Alt+M", [] { MCGuiKey("ctrl+alt+m"); });
    MCGuiWait("the browser alone", 10, [] { return alone(browserPane()); });
    MCGuiStep("a click on another file in the tree", [] {
        MCGuiApp().tree->revealPath(F("max-b.txt"));
    });
    MCGuiWait("its row", 10, [] { return MCGuiFindLabel(MCGuiApp().tree->widget(), "max-b.txt"); });
    MCGuiStep("clicked", [] { MCGuiClick(MCGuiFindLabel(MCGuiApp().tree->widget(), "max-b.txt")); });
    MCGuiWait("the file on show, everything back", 10, [] {
        return MCGuiApp().editor->currentPath() == F("max-b.txt") && asBefore();
    });

    // ---- from the tree: the editor
    MCGuiStep("Ctrl+Alt+M with the tree's keyboard", [] {
        GCHECK(focusIn(MCGuiApp().tree->widget()));
        MCGuiKey("ctrl+alt+m");
    });
    MCGuiWait("the editor alone", 10, [] { return alone(editorPane()); });
    MCGuiStep("Toggle Editor hides it, which ends Maximize Pane", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("the browser and the terminal back, the editor hidden", 10, [] {
        return !L()->maximized() && !on(editorPane()) && on(browserPane()) && on(terminalPane());
    });
    MCGuiStep("Toggle Editor shows it again, and Toggle Browser after it does not maximize", [] {
        MCGuiKey("ctrl+shift+e");
    });
    MCGuiWait("all three, nothing maximized", 10, [] {
        return !L()->maximized() && on(editorPane()) && on(browserPane()) && on(terminalPane());
    });
    MCGuiStep("the window closed", [] {
        MCGuiKey("ctrl+1");
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("one window fewer", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window has the keyboard again", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}

#endif
