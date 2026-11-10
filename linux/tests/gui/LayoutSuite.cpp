// LayoutSuite.cpp — the window's panes as the Mac lays them out
// (`./build/minicode-guitest layout`, linux/PARITY.md item 1.13), by real
// keys and real drags in a window of its own: the browser beside the editor
// at the same top, the terminal below both across the whole width; the bar
// between the editor and the browser dragged (both follow, and the browser
// keeps its width when hidden and shown again), and dragged within 16 px of
// either edge (that side hides, and comes back when the drag goes back out);
// the terminal's bar dragged to the top (the editor and the browser hide)
// and back down; the owner's report of 2026-11-09, each toggle hiding only
// its own pane and the editor, the browser and the terminal hidden in each
// of the six orders staying hidden, the file tree then having the window,
// each toggle bringing its pane back at its size; a file opened from the
// tree, the preview key, and Focus Editor showing a hidden editor; the
// sidebar's divider dragged to the right edge giving the tree the window,
// and Toggle Editor opening the right side again; a drag from 3 px inside
// the editor's left edge reaching the editor, not that divider, which still
// moves from its own line with no jump; Toggle Sidebar hiding only
// the sidebar; and the find bar on the editor's side. The Mac has these in
// EditorController+Layout.mm and +Panels.mm, and the rules in
// src/RightAreaLayout.h, whose `rightarea:*` core tests hold them.
#include "Harness.h"

#include "Editor.h"
#include "FileTree.h"
#include "Keys.h"
#include "TabStrip.h"
#include "WindowLayout.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#if defined(MINICODE_ENABLE_TERMINAL) && defined(MINICODE_ENABLE_BROWSER)

namespace {

WindowLayout* L() { return MCGuiApp().layout; }
GtkWidget* area() { return L()->area(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

enum Pane { kEditor = 0, kBrowser = 1, kTerminal = 2 };
const char* kNames[3] = {"the editor", "the browser", "the terminal"};
// Toggle Editor, Toggle Browser and Toggle Terminal by their Linux keys.
const char* kKeys[3] = {"ctrl+shift+e", "ctrl+shift+b", "ctrl+shift+t"};

GtkWidget* pane(int p) {
    return p == kEditor ? L()->editorPane() : p == kBrowser ? L()->browserPane() : L()->terminalPane();
}

// On screen with a size.
bool on(GtkWidget* w) {
    return w && gtk_widget_get_mapped(w) && gtk_widget_get_width(w) > 0 &&
           gtk_widget_get_height(w) > 0;
}

graphene_rect_t box(GtkWidget* w) {
    graphene_rect_t r;
    if (!w || !gtk_widget_compute_bounds(w, area(), &r)) graphene_rect_init(&r, 0, 0, 0, 0);
    return r;
}

bool near(double a, double b, double within = 1.0) { return std::fabs(a - b) <= within; }

double W() { return gtk_widget_get_width(area()); }
double H() { return gtk_widget_get_height(area()); }

GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool focusIn(GtkWidget* w) { return Keys::within(focusWidget(), w); }

// Exactly the panes in `shown` (a bit for each) on screen.
bool showing(int shown) {
    for (int p = 0; p < 3; p++)
        if (on(pane(p)) != (((shown >> p) & 1) != 0)) return false;
    return true;
}

std::string which(int shown) {
    std::string s;
    for (int p = 0; p < 3; p++)
        if ((shown >> p) & 1) s += std::string(s.empty() ? "" : ", ") + kNames[p];
    return s.empty() ? "nothing" : s;
}

void note(const char* what) {
    const RightArea::Limits& lim = L()->limits();
    MCGuiNote("%s: smallest editor %.0f, browser %.0f, top %.0f, terminal %.0f", what,
              lim.minEditorWidth, lim.minBrowserWidth, lim.minTop, lim.minTerminal);
    const graphene_rect_t e = box(pane(kEditor)), b = box(pane(kBrowser)), t = box(pane(kTerminal));
    MCGuiNote("%s: area %.0fx%.0f, editor %.0f,%.0f %.0fx%.0f, browser %.0f,%.0f %.0fx%.0f, "
              "terminal %.0f,%.0f %.0fx%.0f",
              what, W(), H(), e.origin.x, e.origin.y, e.size.width, e.size.height, b.origin.x,
              b.origin.y, b.size.width, b.size.height, t.origin.x, t.origin.y, t.size.width,
              t.size.height);
}

// Where the bar between the editor and the browser has its middle, and the
// terminal's bar.
double browserBarX() {
    const graphene_rect_t e = box(pane(kEditor));
    return e.origin.x + e.size.width + WindowLayout::kBar / 2.0;
}
double terminalBarY() { return box(pane(kTerminal)).origin.y - WindowLayout::kBar / 2.0; }

GtkWidget* treeRow(const std::string& name) { return MCGuiFindLabel(MCGuiApp().tree->widget(), name); }

// Each pane in `shown` brought on screen by its key, one at a time.
void showOnly(int shown) {
    for (int p = 0; p < 3; p++) {
        MCGuiStep(std::string("set up: ") + kNames[p] + ((shown >> p) & 1 ? " shown" : " hidden"),
                  [p, shown] {
                      if (on(pane(p)) != (((shown >> p) & 1) != 0)) MCGuiKey(kKeys[p]);
                  });
        MCGuiWait(std::string("set up: ") + kNames[p], 10,
                  [p, shown] { return on(pane(p)) == (((shown >> p) & 1) != 0); });
    }
}

int gWindows = 0;
double gBrowserW = 0, gEditorW = 0, gTerminalH = 0, gSidebarW = 0;
int gEditorHides = 0;
gulong gHideWatch = 0;

}  // namespace

MC_GUI_SUITE(layout) {
    MCGuiStep("files of its own, and a window of its own", [] {
        std::string text;
        for (int i = 1; i <= 80; i++) text += "layout line " + std::to_string(i) + "\n";
        MCGuiWriteFile(F("layout-a.txt"), text);
        MCGuiWriteFile(F("layout-b.txt"), "the second file\n");
        MCGuiWriteFile(F("layout-notes.md"), "# Notes\n\nSome *text*.\n");
        gWindows = (int)MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("the window", 10, [] { return (int)MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("a file open in it", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiOpenFileKept(F("layout-a.txt"));
    });
    MCGuiWait("it shows; the browser and the terminal start hidden", 10, [] {
        return MCGuiApp().editor->currentPath() == F("layout-a.txt") && showing(1 << kEditor) &&
               treeRow("layout-b.txt") != nullptr;
    });

    // ------------------------------------------------ the browser beside
    MCGuiStep("Toggle Browser, by its key", [] { MCGuiKey("ctrl+shift+b"); });
    MCGuiWait("the browser shows, with the keyboard in its address bar", 10, [] {
        return showing((1 << kEditor) | (1 << kBrowser)) && focusIn(pane(kBrowser));
    });
    MCGuiStep("Toggle Terminal, by its key", [] { MCGuiKey("ctrl+shift+t"); });
    MCGuiWait("the terminal shows, with the keyboard", 10, [] {
        return showing(7) && MCGuiApp().terminal->owns(focusWidget());
    });
    MCGuiStep("the browser right of the editor at the same top; the terminal below both, "
              "across the whole width",
              [] {
                  note("all three");
                  const graphene_rect_t e = box(pane(kEditor)), b = box(pane(kBrowser)),
                                        t = box(pane(kTerminal));
                  const double bar = WindowLayout::kBar;
                  GCHECK(near(e.origin.x, 0) && near(e.origin.y, 0));
                  GCHECK(near(b.origin.y, e.origin.y) && near(b.size.height, e.size.height));
                  GCHECK(near(b.origin.x, e.origin.x + e.size.width + bar));
                  GCHECK(near(b.origin.x + b.size.width, W()));
                  GCHECK(near(t.origin.x, 0) && near(t.size.width, W()));
                  GCHECK(near(t.origin.y, e.origin.y + e.size.height + bar));
                  GCHECK(near(t.origin.y + t.size.height, H()));
                  // The browser at half the width, the terminal at its 220.
                  GCHECK(near(b.size.width, std::round(W() / 2)));
                  GCHECK(near(t.size.height, 220));
                  // The tab strip is the editor's, above the text.
                  const graphene_rect_t s = box(MCGuiApp().editor->strip()->widget());
                  GCHECK(s.size.width > 0 && near(s.origin.x, e.origin.x) &&
                         s.origin.x + s.size.width <= e.origin.x + e.size.width + 0.5);
                  const graphene_rect_t tv = box(MCGuiApp().editor->textView());
                  GCHECK(tv.origin.y >= s.origin.y + s.size.height - 0.5);
                  GCHECK(tv.origin.x + tv.size.width <= b.origin.x);
                  // Each bar where it is drawn, between the panes.
                  GCHECK(on(L()->browserBar()) && on(L()->terminalBar()));
                  MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "layout-all-three");
              });

    // ----------------------------------------- the bar between them dragged
    MCGuiStep("the bar between the editor and the browser dragged 200 px left", [] {
        gEditorW = box(pane(kEditor)).size.width;
        gBrowserW = box(pane(kBrowser)).size.width;
        const double x = browserBarX(), y = box(pane(kEditor)).size.height / 2;
        MCGuiDrag(area(), x, y, x - 200, y);
    });
    MCGuiWait("both follow it", 10, [] {
        return near(box(pane(kEditor)).size.width, gEditorW - 200) &&
               near(box(pane(kBrowser)).size.width, gBrowserW + 200);
    });
    MCGuiStep("together they still fill the top", [] {
        note("dragged");
        const graphene_rect_t e = box(pane(kEditor)), b = box(pane(kBrowser));
        GCHECK(near(e.size.width + WindowLayout::kBar + b.size.width, W()));
        gBrowserW = b.size.width;
        MCGuiKey("ctrl+shift+b");
    });
    MCGuiWait("hidden, the browser leaves the editor the whole width", 10, [] {
        return showing((1 << kEditor) | (1 << kTerminal)) && near(box(pane(kEditor)).size.width, W());
    });
    MCGuiStep("Toggle Browser again", [] { MCGuiKey("ctrl+shift+b"); });
    MCGuiWait("it comes back at the width it was dragged to", 10, [] {
        return showing(7) && near(box(pane(kBrowser)).size.width, gBrowserW);
    });

    // ------------------------------------------- dragged to an edge: hidden
    MCGuiStep("the bar dragged to 8 px from the left edge", [] {
        const double x = browserBarX(), y = box(pane(kEditor)).size.height / 2;
        MCGuiDrag(area(), x, y, 8, y);
    });
    MCGuiWait("the editor hides, the browser has the top, with the keyboard", 10, [] {
        return showing((1 << kBrowser) | (1 << kTerminal)) && near(box(pane(kBrowser)).origin.x, 0) &&
               near(box(pane(kBrowser)).size.width, W()) && focusIn(pane(kBrowser));
    });
    MCGuiStep("Toggle Editor brings it back", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("beside the browser, at its smallest, 150 px", 10, [] {
        return showing(7) && near(box(pane(kEditor)).size.width, 150);
    });
    MCGuiStep("the bar dragged to 8 px from the right edge", [] {
        const double x = browserBarX(), y = box(pane(kEditor)).size.height / 2;
        MCGuiDrag(area(), x, y, W() - 8, y);
    });
    MCGuiWait("the browser hides, the editor has the top, with the keyboard", 10, [] {
        return showing((1 << kEditor) | (1 << kTerminal)) && near(box(pane(kEditor)).size.width, W()) &&
               focusIn(pane(kEditor));
    });
    MCGuiStep("Toggle Browser brings it back", [] { MCGuiKey("ctrl+shift+b"); });
    MCGuiWait("beside the editor, at its smallest, 150 px", 10, [] {
        return showing(7) && near(box(pane(kBrowser)).size.width, 150);
    });
    MCGuiStep("dragged to the left edge and back out to the middle, in one drag", [] {
        gEditorHides = 0;
        gHideWatch = g_signal_connect(pane(kEditor), "notify::visible",
                                      G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
                                          if (!gtk_widget_get_visible(GTK_WIDGET(o))) gEditorHides++;
                                      }),
                                      nullptr);
        const double x = browserBarX(), y = box(pane(kEditor)).size.height / 2;
        MCGuiDragThrough(area(), {{x, y}, {6, y}, {W() / 2, y}});
    });
    MCGuiWait("the drag is over", 10, [] { return !MCGuiInputPending(); });
    MCGuiWait("the editor hid on the way and came back, the bar where it was let go", 10, [] {
        return gEditorHides >= 1 && showing(7) && near(browserBarX(), std::round(W() / 2), 2);
    });
    MCGuiStep("(the watch off)", [] {
        g_signal_handler_disconnect(pane(kEditor), gHideWatch);
        gHideWatch = 0;
        note("back out");
    });

    // ------------------------------------- the terminal's bar to the top
    MCGuiStep("the terminal's bar dragged to 5 px from the top", [] {
        gTerminalH = box(pane(kTerminal)).size.height;
        MCGuiDrag(area(), W() / 2, terminalBarY(), W() / 2, 5);
    });
    MCGuiWait("the editor and the browser hide; the terminal has the height and the keyboard", 10, [] {
        const graphene_rect_t t = box(pane(kTerminal));
        return showing(1 << kTerminal) && near(t.origin.y, WindowLayout::kBar) &&
               near(t.size.height, H() - WindowLayout::kBar) &&
               MCGuiApp().terminal->owns(focusWidget());
    });
    MCGuiStep("its bar, at the top edge, dragged down to 300 px", [] {
        MCGuiDrag(area(), W() / 2, WindowLayout::kBar / 2.0, W() / 2, 300);
    });
    MCGuiWait("the editor is back above it, the bar where it was let go", 10, [] {
        return showing((1 << kEditor) | (1 << kTerminal)) && near(terminalBarY(), 300) &&
               near(box(pane(kEditor)).size.height, 300 - WindowLayout::kBar / 2.0);
    });
    MCGuiStep("the terminal's height put back", [] {
        const double y = terminalBarY();
        MCGuiDrag(area(), W() / 2, y, W() / 2, H() - gTerminalH - WindowLayout::kBar / 2.0);
    });
    MCGuiWait("as it was", 10, [] { return near(box(pane(kTerminal)).size.height, gTerminalH); });

    // ---------------------------------- the owner's report: the toggles
    int order[3] = {kEditor, kBrowser, kTerminal};
    do {
        const int o0 = order[0], o1 = order[1], o2 = order[2];
        const std::string named = std::string(kNames[o0]) + ", " + kNames[o1] + ", " + kNames[o2];
        showOnly(7);
        MCGuiStep("the sizes to come back to (" + named + ")", [] {
            gBrowserW = box(pane(kBrowser)).size.width;
            gTerminalH = box(pane(kTerminal)).size.height;
            gSidebarW = gtk_widget_get_width(L()->sidebar());
        });
        const int ord[3] = {o0, o1, o2};
        int shown = 7;
        for (int k = 0; k < 3; k++) {
            const int p = ord[k];
            shown &= ~(1 << p);
            MCGuiStep(std::string("hide ") + kNames[p] + " by its key", [p] { MCGuiKey(kKeys[p]); });
            MCGuiWait("only " + which(shown) + " shows", 10, [shown] { return showing(shown); });
        }
        MCGuiPause(0.4);   // a negative check: nothing comes back by itself
        MCGuiStep("all three still hidden (" + named + "); the file tree has the window and the keyboard",
                  [] {
                      GCHECK(showing(0));
                      GCHECK(L()->rightCollapsed());
                      GCHECK(gtk_widget_get_width(L()->sidebar()) >=
                             gtk_widget_get_width(L()->widget()) - 12);
                      GCHECK(focusIn(MCGuiApp().tree->widget()));
                      MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "layout-all-hidden");
                  });
        for (int k = 0; k < 3; k++) {
            const int p = ord[k];
            shown |= 1 << p;
            MCGuiStep(std::string("show ") + kNames[p] + " by its key", [p] { MCGuiKey(kKeys[p]); });
            MCGuiWait("only " + which(shown) + " shows", 10, [shown] { return showing(shown); });
        }
        MCGuiWait("each back at the size it had, the sidebar too", 10, [] {
            return near(box(pane(kBrowser)).size.width, gBrowserW) &&
                   near(box(pane(kTerminal)).size.height, gTerminalH) &&
                   near(gtk_widget_get_width(L()->sidebar()), gSidebarW) && !L()->rightCollapsed();
        });
    } while (std::next_permutation(order, order + 3));

    // ------------------------------- opening a file reveals the editor
    MCGuiStep("the editor hidden by its key, the terminal and the browser showing", [] {
        MCGuiKey("ctrl+shift+e");
    });
    MCGuiWait("it hides", 10, [] { return showing((1 << kBrowser) | (1 << kTerminal)); });
    MCGuiStep("a file clicked in the tree", [] { MCGuiClick(treeRow("layout-b.txt")); });
    MCGuiWait("it shows in the editor, beside the browser", 10, [] {
        return MCGuiApp().editor->currentPath() == F("layout-b.txt") && showing(7);
    });
    showOnly(0);
    MCGuiWait("with all three hidden, the tree has the window", 10, [] { return L()->rightCollapsed(); });
    MCGuiStep("a file clicked in the tree", [] { MCGuiClick(treeRow("layout-a.txt")); });
    MCGuiWait("the right side opens with the editor, and the file in it", 10, [] {
        return MCGuiApp().editor->currentPath() == F("layout-a.txt") && showing(1 << kEditor) &&
               !L()->rightCollapsed() && near(box(pane(kEditor)).size.width, W());
    });

    MCGuiStep("a Markdown file in a tab of its own", [] { MCGuiOpenFileKept(F("layout-notes.md")); });
    MCGuiWait("it shows", 10, [] {
        return MCGuiApp().editor->currentPath() == F("layout-notes.md") && showing(1 << kEditor);
    });
    showOnly(0);
    MCGuiWait("all three hidden again", 10, [] { return L()->rightCollapsed(); });
    MCGuiStep("the preview toggled by its key", [] {
        gEditorHides = MCGuiApp().editor->inPreview() ? 1 : 0;   // what it showed
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the editor shows again, the other way round", 10, [] {
        return showing(1 << kEditor) && !L()->rightCollapsed() &&
               MCGuiApp().editor->inPreview() == (gEditorHides == 0);
    });

    showOnly(1 << kTerminal);
    MCGuiWait("the terminal alone, with the keyboard", 10, [] {
        return MCGuiApp().terminal->owns(focusWidget());
    });
    MCGuiStep("Focus Editor, by its key", [] { MCGuiKey("ctrl+1"); });
    MCGuiWait("the editor shows again, with the keyboard", 10, [] {
        return showing((1 << kEditor) | (1 << kTerminal)) &&
               gtk_widget_has_focus(MCGuiApp().editor->textView());
    });

    // -------------- the sidebar's divider leaves the editor's edge alone
    // It took presses 14 px into the editor (the theme's padding and GTK's
    // own 6 px), over the gutter's lanes; now 5 px either side of its line.
    MCGuiStep("the keyboard to the tree", [] { MCGuiKey("ctrl+0"); });
    MCGuiWait("the tree has it", 10, [] { return focusIn(MCGuiApp().tree->widget()); });
    MCGuiStep("a drag from 3 px inside the editor's left edge", [] {
        gSidebarW = gtk_widget_get_width(L()->sidebar());
        const graphene_rect_t tv = box(MCGuiApp().editor->textView());
        GCHECK(near(tv.origin.x, 0));   // the editor's text starts at the right side's edge
        const double y = tv.origin.y + 20;
        MCGuiDrag(area(), 3, y, 63, y);
    });
    MCGuiWait("the drag is over", 10, [] { return !MCGuiInputPending(); });
    MCGuiWait("the editor took it: the keyboard is there", 10, [] {
        return focusIn(pane(kEditor));
    });
    MCGuiStep("and the divider did not move", [] {
        GCHECK(near(gtk_widget_get_width(L()->sidebar()), gSidebarW, 0));
    });
    MCGuiStep("a drag from the divider's own line still moves it 30 px", [] {
        const double y = gtk_widget_get_height(L()->widget()) / 2;
        MCGuiDrag(L()->widget(), gSidebarW + 2, y, gSidebarW + 32, y);
    });
    MCGuiWait("it moved, with no jump at the press", 10, [] {
        return near(gtk_widget_get_width(L()->sidebar()), gSidebarW + 30, 1);
    });
    MCGuiStep("and back", [] {
        const double y = gtk_widget_get_height(L()->widget()) / 2;
        MCGuiDrag(L()->widget(), gSidebarW + 32, y, gSidebarW + 2, y);
    });
    MCGuiWait("where it was", 10, [] {
        return near(gtk_widget_get_width(L()->sidebar()), gSidebarW, 1);
    });

    // ------------------------- the sidebar's divider dragged to the right
    MCGuiStep("the sidebar's divider dragged to 20 px from the window's right edge", [] {
        gSidebarW = gtk_widget_get_width(L()->sidebar());
        const double y = gtk_widget_get_height(L()->widget()) / 2;
        MCGuiDrag(L()->widget(), gSidebarW + 2, y, gtk_widget_get_width(L()->widget()) - 20, y);
    });
    MCGuiWait("it snaps to the edge: the tree has the window", 10, [] {
        return L()->rightCollapsed() &&
               gtk_widget_get_width(L()->sidebar()) >= gtk_widget_get_width(L()->widget()) - 12;
    });
    MCGuiStep("Toggle Editor, by its key", [] { MCGuiKey("ctrl+shift+e"); });
    MCGuiWait("the right side opens at the sidebar's old width, the editor shown", 10, [] {
        return !L()->rightCollapsed() && near(gtk_widget_get_width(L()->sidebar()), gSidebarW) &&
               showing((1 << kEditor) | (1 << kTerminal));
    });

    // ---------------------------------- the sidebar and the find bar
    showOnly(7);
    MCGuiStep("the keyboard in the editor, Ctrl+B", [] {
        gEditorW = W();
        MCGuiKey("ctrl+1");
    });
    MCGuiWait("the editor has the keyboard", 10, [] {
        return gtk_widget_has_focus(MCGuiApp().editor->textView());
    });
    MCGuiStep("Toggle Sidebar, by its key", [] { MCGuiKey("ctrl+b"); });
    MCGuiWait("only the sidebar hides; the right side takes its room", 10, [] {
        return !gtk_widget_get_visible(L()->sidebar()) && showing(7) && W() > gEditorW + 100;
    });
    MCGuiStep("Toggle Sidebar again", [] { MCGuiKey("ctrl+b"); });
    MCGuiWait("it is back, the rest as it was", 10, [] {
        return on(L()->sidebar()) && showing(7) && near(W(), gEditorW);
    });
    MCGuiStep("Find, by its key", [] { MCGuiKey("ctrl+f"); });
    MCGuiWait("the find bar opens on the editor's side, above the tabs", 10, [] {
        const graphene_rect_t f = box(MCGuiApp().searchBar), e = box(pane(kEditor));
        const graphene_rect_t s = box(MCGuiApp().editor->strip()->widget());
        return on(MCGuiApp().searchBar) && f.size.height > 0 && near(f.origin.x, e.origin.x) &&
               f.origin.x + f.size.width <= e.origin.x + e.size.width + 0.5 &&
               f.origin.y + f.size.height <= s.origin.y + 0.5;
    });
    MCGuiStep("Find again closes it", [] { MCGuiKey("ctrl+f"); });

    MCGuiStep("the window closed", [] {
        MCGuiKey("ctrl+1");
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("one window fewer", 10, [] { return (int)MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window has the keyboard again", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}

#endif
