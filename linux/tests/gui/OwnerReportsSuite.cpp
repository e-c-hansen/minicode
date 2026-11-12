// OwnerReportsSuite.cpp — PARITY.md "Reported by the owner", R4 and R5
// (`./build/minicode-guitest owner_reports`), by real keys and real drags in
// a window of its own.
//
// R4: with no tab the editor is empty and read-only, with no welcome text,
// at the start of a window and after the last tab closes.
//
// R5: hiding the editor (Toggle Editor, Ctrl+W on the last tab's neighbours,
// the terminal's bar dragged to the top, Maximize Pane and back) never brings
// out a pane the user hid. With the tree shown and the terminal, browser and
// Problems hidden the tree fills the window; with the tree hidden too the
// area is bare; with the terminal (or the browser, or Problems) shown it
// takes the space.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"
#include "Keys.h"
#include "TabStrip.h"
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
std::string F(const std::string& n) { return MCGuiProjectFile(n); }
Editor* ed() { return MCGuiApp().editor; }

bool on(GtkWidget* w) {
    return w && gtk_widget_get_mapped(w) && gtk_widget_get_width(w) > 0 &&
           gtk_widget_get_height(w) > 0;
}
bool treeOn() { return on(L()->sidebar()); }
bool editorOn() { return on(L()->editorPane()); }
bool termOn() { return on(L()->terminalPane()); }
bool browserOn() { return on(L()->browserPane()); }
bool problemsOn() { return L()->problemsShown(); }

// The tree has the whole window: it is as wide as the paned, nothing else on.
bool treeFills() {
    static int last = 0;
    if (last++ % 40 == 0)
        MCGuiNote("tree %d paned %d editor %d term %d browser %d problems %d", gtk_widget_get_width(L()->sidebar()),
                  gtk_widget_get_width(L()->widget()), editorOn(), termOn(), browserOn(), problemsOn());
    return treeOn() && !editorOn() && !termOn() && !browserOn() && !problemsOn() &&
           std::fabs(gtk_widget_get_width(L()->sidebar()) - gtk_widget_get_width(L()->widget())) <= 16;
}
// Bare: no tree, nothing in the right area.
bool bare() {
    return !treeOn() && !L()->sidebarShown() && !editorOn() && !termOn() && !browserOn() &&
           !problemsOn();
}
bool emptyEditor() {
    GtkWidget* v = ed()->textView();
    return MCGuiEditorText().empty() && !gtk_text_view_get_editable(GTK_TEXT_VIEW(v));
}

// Ctrl+Shift+M focuses a list that shows without the keyboard, and hides it
// only from there: pressed until it is gone.
void pressUntilProblemsHidden() {
    if (problemsOn()) MCGuiKey("ctrl+shift+m");
    if (problemsOn()) MCGuiKey("ctrl+shift+m");
}

// Terminal, browser and Problems all hidden, the editor and tree shown.
void toBase() {
    MCGuiStep("base: only the editor and the tree", [] {
        if (L()->terminalShown()) MCGuiKey("ctrl+shift+t");
        if (L()->browserShown()) MCGuiKey("ctrl+shift+b");
        pressUntilProblemsHidden();
        if (!L()->editorShown()) MCGuiKey("ctrl+shift+e");
        if (!L()->sidebarShown()) MCGuiKey("ctrl+b");
    });
    MCGuiWait("base", 10, [] {
        return editorOn() && treeOn() && !termOn() && !browserOn() && !problemsOn() &&
               !L()->rightCollapsed();
    });
}

int gWindows = 0;

}  // namespace

MC_GUI_SUITE(owner_reports) {
    MCGuiStep("a file, and a window of its own", [] {
        MCGuiWriteFile(F("owner-a.txt"), "owner file\n");
        gWindows = (int)MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("the window", 10, [] { return (int)MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("(focus it)", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });

    // ---------------------------------------------------------- R4
    // A new window on a folder reopens its session's tabs; none stay here.
    MCGuiStep("the session's tabs closed", [] { ed()->closeAllTabs(); });
    MCGuiWait("no tab: an empty read-only editor, no welcome text", 10, [] {
        return ed()->tabCount() == 0 && emptyEditor();
    });
    MCGuiStep("a file, then its tab closed by Ctrl+W", [] { MCGuiOpenFileKept(F("owner-a.txt")); });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("owner-a.txt"); });
    MCGuiStep("Ctrl+W", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("empty again, the editor still shown", 10, [] {
        return ed()->tabCount() == 0 && emptyEditor() && editorOn();
    });
    MCGuiStep("a snapshot of the empty editor", [] {
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "r4-empty-editor");
    });

    // ---------------------------------------------------------- R5
    toBase();
    MCGuiStep("Toggle Editor with the tree shown", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("the tree fills the window; nothing hidden came out", 10, treeFills);
    MCGuiStep("snapshot", [] { MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "r5-tree-fills"); });
    MCGuiStep("Toggle Editor brings the editor back", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("editor and tree", 10, [] { return editorOn() && treeOn() && !termOn() && !browserOn(); });

    // The tree hidden first, then the editor: bare.
    MCGuiStep("Toggle Sidebar", [] { MCGuiKey("ctrl+b"); });
    MCGuiWait("the tree is gone", 10, [] { return !treeOn(); });
    MCGuiStep("Toggle Editor", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("a bare area: no tree, no terminal, no browser, no editor", 10, bare);
    MCGuiStep("Toggle Editor again", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("only the editor, the tree still hidden", 10, [] {
        return editorOn() && !treeOn() && !termOn() && !browserOn();
    });
    // The tree still hidden: Toggle Editor again, bare, then Toggle Sidebar
    // gives the tree the window.
    MCGuiStep("Toggle Editor", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("bare", 10, bare);
    MCGuiStep("Toggle Sidebar", [] { MCGuiKey("ctrl+b"); });
    MCGuiWait("the tree fills the window", 10, treeFills);
    MCGuiStep("Toggle Editor back", [] { MCGuiKey("ctrl+shift+e"); });
    toBase();

    // Closing the last tab leaves the editor.
    MCGuiStep("a file, Ctrl+W", [] { MCGuiOpenFileKept(F("owner-a.txt")); });
    MCGuiWait("shown", 10, [] { return ed()->currentPath() == F("owner-a.txt"); });
    MCGuiStep("Ctrl+W", [] { MCGuiKey("ctrl+w"); });
    MCGuiWait("the editor and the tree, nothing hidden out", 10, [] {
        return ed()->tabCount() == 0 && editorOn() && treeOn() && !termOn() && !browserOn();
    });

    // Maximize Pane and back, with only the editor.
    MCGuiStep("Maximize Pane from the editor", [] {
        MCGuiClick(ed()->textView());
        MCGuiKey("ctrl+alt+m");
        MCGuiKey("ctrl+alt+m");
    });
    MCGuiWait("as before", 10, [] { return editorOn() && treeOn() && !termOn() && !browserOn(); });

    // The terminal's role: shown, then the editor hidden: it takes the space.
    MCGuiStep("the terminal shown, the editor hidden", [] {
        MCGuiKey("ctrl+shift+t");
        MCGuiKey("ctrl+shift+e");
    });
    MCGuiWait("the terminal takes the right side", 10, [] {
        return termOn() && !editorOn() && !browserOn() && treeOn();
    });
    MCGuiStep("the terminal hidden", [] { MCGuiKey("ctrl+shift+t"); });
    MCGuiWait("the tree fills the window; the browser stays hidden", 10, treeFills);
    toBase();

    // The terminal's bar dragged to the top, then the terminal hidden.
    MCGuiStep("the terminal shown, its bar dragged to the top", [] {
        MCGuiKey("ctrl+shift+t");
    });
    MCGuiWait("both", 10, [] { return termOn() && editorOn(); });
    MCGuiStep("drag", [] {
        const graphene_rect_t t = [] {
            graphene_rect_t r;
            if (!gtk_widget_compute_bounds(L()->terminalPane(), area(), &r))
                graphene_rect_init(&r, 0, 0, 0, 0);
            return r;
        }();
        const double w = gtk_widget_get_width(area());
        MCGuiDrag(area(), w / 2, t.origin.y - WindowLayout::kBar / 2.0, w / 2, 5);
    });
    MCGuiWait("the terminal alone", 10, [] { return termOn() && !editorOn() && !browserOn(); });
    MCGuiStep("the terminal hidden", [] { MCGuiKey("ctrl+shift+t"); });
    MCGuiWait("the tree fills the window; nothing else", 10, treeFills);
    toBase();

    // The browser's role.
    MCGuiStep("the browser shown, the editor hidden, the browser hidden", [] {
        MCGuiKey("ctrl+shift+b");
    });
    MCGuiWait("browser shown", 10, [] { return browserOn(); });
    MCGuiStep("the editor hidden", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("the browser takes the side", 10, [] { return browserOn() && !editorOn() && !termOn(); });
    MCGuiStep("the browser hidden", [] { MCGuiKey("ctrl+shift+b"); });
    MCGuiWait("the tree fills the window", 10, treeFills);
    MCGuiStep("tree hidden, browser shown and hidden", [] {
        MCGuiKey("ctrl+b");
        MCGuiKey("ctrl+shift+b");
    });
    MCGuiWait("browser alone", 10, [] { return browserOn() && !treeOn(); });
    MCGuiStep("the browser hidden", [] { MCGuiKey("ctrl+shift+b"); });
    MCGuiWait("bare: the tree stays hidden", 10, bare);
    MCGuiStep("Toggle Sidebar", [] { MCGuiKey("ctrl+b"); });
    MCGuiWait("the tree fills the window", 10, treeFills);
    MCGuiStep("Toggle Editor", [] { MCGuiKey("ctrl+shift+e"); });
    toBase();

    // Problems' role.
    MCGuiStep("Problems shown, the editor hidden", [] { MCGuiKey("ctrl+shift+m"); });
    MCGuiWait("shown", 10, [] { return problemsOn(); });
    MCGuiStep("the editor hidden", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("Problems takes the side", 10, [] { return !editorOn() && problemsOn() && !termOn(); });
    MCGuiStep("Problems hidden", [] { pressUntilProblemsHidden(); });
    MCGuiWait("the tree fills the window; nothing came out", 10, treeFills);
    MCGuiStep("tree hidden, Problems shown and hidden", [] {
        MCGuiKey("ctrl+b");
        MCGuiKey("ctrl+shift+m");
    });
    MCGuiWait("Problems alone", 10, [] { return problemsOn() && !treeOn(); });
    MCGuiStep("Problems hidden", [] { pressUntilProblemsHidden(); });
    MCGuiWait("bare", 10, bare);
    MCGuiStep("Toggle Sidebar", [] { MCGuiKey("ctrl+b"); });
    MCGuiWait("the tree fills the window", 10, treeFills);
    MCGuiStep("Toggle Editor", [] { MCGuiKey("ctrl+shift+e"); });
    toBase();

    MCGuiStep("the window closed", [] {
        MCGuiKey("ctrl+1");
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("one window fewer", 10, [] { return (int)MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window has the keyboard again", [] {
        GCHECK(true);   // reached the end
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}

#endif
