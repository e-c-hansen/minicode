// AdjustSettingsSuite.cpp — `./build/minicode-guitest adjust_settings`
// (PARITY.md 2.11; the owner, 2026-11-09: "no 'change settings' when right
// clicking on some portion of the GUI. This should point to the relevant part
// of the minicode config."). In a window of its own, a real right click on
// each part of the window (the file tree, the editor, a Markdown page, a tab
// and the tab strip, the status bar, the Source Control panel, the terminal,
// the browser's page and its address bar) opens a menu whose last item is
// Adjust Settings…; a real click on it opens the settings file with the
// keyboard there, the first of that part's keys' value selected, and every
// one of its keys' lines tinted for about three seconds (the Mac's
// showSettingsForSurface:). Then an old settings file: the part's missing
// keys' commented defaults are added at its end under one "# Added by Adjust
// Settings" line, as one undo step, saved at once when the tab had no
// unsaved changes and left unsaved when it had (View > Adjust Settings >
// Terminal, through the menu bar, for that one).
#include "Harness.h"

#include "Appearance.h"
#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "GitPanel.h"
#include "TabStrip.h"
#include "WindowLayout.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#include <vte/vte.h>
#endif
#ifdef MINICODE_ENABLE_BROWSER
#include "Browser.h"
#include <webkit/webkit.h>
#endif

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace {

size_t gWindows = 0;
std::string gText;      // the settings text before an append or an undo
bool gPressed = false;  // an undo was sent
std::string gWhy;       // what was not as it should be at the last look

std::string settingsPath() { return g_getenv("MINICODE_SETTINGS"); }
Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// A whole settings file: every key there, so nothing is added.
std::string fullFile() { return std::string("lsp.enabled = false\n") + Settings::defaultFileText(); }

std::string bufferText(GtkTextBuffer* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

Document* settingsDoc() {
    Editor* e = ed();
    return e ? e->documentFor(settingsPath()) : nullptr;
}

bool showingSettings() {
    return ed() && ed()->active() && ed()->active()->isSettingsFile() && ed()->isSource();
}

// A mapped popover menu under `root` that shows `label`, or null.
GtkWidget* menuWith(GtkWidget* root, const std::string& label) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_POPOVER_MENU(root) && MCGuiFindLabel(root, label)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = menuWith(c, label)) return hit;
    return nullptr;
}

GtkWidget* openMenu() { return menuWith(GTK_WIDGET(MCGuiApp().window), Appearance::kItemLabel); }

// The last item of a menu model, sections flattened: its label, action and
// string target.
struct Item {
    std::string label, action, target;
};
bool lastItem(GMenuModel* model, Item* out) {
    bool found = false;
    for (int i = 0; i < g_menu_model_get_n_items(model); i++) {
        if (GMenuModel* section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION)) {
            Item inner;
            if (lastItem(section, &inner)) {
                *out = inner;
                found = true;
            }
            g_object_unref(section);
            continue;
        }
        gchar* label = nullptr;
        gchar* action = nullptr;
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
        GVariant* target = g_menu_model_get_item_attribute_value(model, i, G_MENU_ATTRIBUTE_TARGET,
                                                                 G_VARIANT_TYPE_STRING);
        out->label = label ? label : "";
        out->action = action ? action : "";
        out->target = target ? g_variant_get_string(target, nullptr) : "";
        if (target) g_variant_unref(target);
        g_free(label);
        g_free(action);
        found = true;
    }
    return found;
}

// The settings file on show is at `surface`'s keys: the keyboard in it, the
// first key's value selected, every key's line tinted. `why` says what is not.
bool atKeys(const std::string& surface, std::string* why) {
    if (!showingSettings()) {
        *why = "the settings file is not on show";
        return false;
    }
    GtkTextBuffer* b = ed()->buffer();
    const std::string text = bufferText(b);
    std::vector<int> want = Settings::surfaceKeyLines(text, surface);
    if (want.empty()) {
        *why = "no keys of " + surface;
        return false;
    }
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(b, &s, &e)) {
        *why = "nothing selected";
        return false;
    }
    if (gtk_text_iter_get_line(&s) != want[0]) {
        *why = "the selection is on line " + std::to_string(gtk_text_iter_get_line(&s)) +
               ", not " + std::to_string(want[0]);
        return false;
    }
    // The value of that line.
    GtkTextIter ls = s, le = s;
    gtk_text_iter_set_line_offset(&ls, 0);
    if (!gtk_text_iter_ends_line(&le)) gtk_text_iter_forward_to_line_end(&le);
    char* raw = gtk_text_buffer_get_text(b, &ls, &le, FALSE);
    const std::string line = raw ? raw : "";
    g_free(raw);
    ValueSpan v;
    if (!Settings::findValue(std::u16string(line.begin(), line.end()), v) ||
        gtk_text_iter_get_line_offset(&s) != (int)v.start ||
        gtk_text_iter_get_line_offset(&e) != (int)(v.start + v.length)) {
        *why = "not the value of \"" + line + "\" selected";
        return false;
    }
    std::vector<int> tinted = Appearance::tintedLines(b);
    std::sort(want.begin(), want.end());
    if (tinted != want) {
        *why = "tinted " + std::to_string(tinted.size()) + " lines, not the " +
               std::to_string(want.size()) + " of its keys";
        return false;
    }
    if (gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) != ed()->textView()) {
        *why = "the keyboard is elsewhere";
        return false;
    }
    return true;
}

// The middle of `text`'s first character in a text view's buffer, in the
// view's coordinates, when it is laid out on screen.
bool pointAtText(GtkWidget* view, const std::string& text, double* x, double* y) {
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start, at, end;
    gtk_text_buffer_get_start_iter(b, &start);
    if (!gtk_text_iter_forward_search(&start, text.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY, &at, &end,
                                      nullptr))
        return false;
    GdkRectangle r;
    gtk_text_view_get_iter_location(GTK_TEXT_VIEW(view), &at, &r);
    int wx, wy;
    gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(view), GTK_TEXT_WINDOW_WIDGET, r.x + 2,
                                          r.y + r.height / 2, &wx, &wy);
    *x = wx;
    *y = wy;
    return r.height > 0 && wy > 0 && wy < gtk_widget_get_height(view);
}

// One part of the window: a real right click at (x, y) of what `target`
// gives (its center at -1, and a value under -1 that many pixels from the
// far edge), its menu's last item checked, chosen by a real click, and the
// settings file checked at the part's keys. `action` says whether the item
// names win.adjustsettings itself (WebKit's menu names its own actions).
using Point = std::function<bool(GtkWidget*, double*, double*)>;
void rightClickChoose(const std::string& name, std::function<GtkWidget*()> target,
                      const std::string& surface, double x = -1, double y = -1,
                      bool action = true, Point at = nullptr) {
    MCGuiWait(name + " on screen", 15, [target, at] {
        double px, py;
        return MCGuiOnScreen(target()) && (!at || at(target(), &px, &py));
    });
    MCGuiStep("a right click on " + name, [target, x, y, at] {
        GtkWidget* w = target();
        double px = x < -1 ? gtk_widget_get_width(w) + x : x;
        double py = y < -1 ? gtk_widget_get_height(w) + y : y;
        if (at) at(w, &px, &py);
        MCGuiClick(w, px, py, 3);
    });
    MCGuiWait(name + "'s menu", 15, [] { return openMenu() != nullptr; });
    MCGuiStep("its last item is Adjust Settings… for " + surface + ", chosen", [name, surface, action] {
        GtkWidget* menu = openMenu();
        GCHECK(menu != nullptr);
        if (!menu) return;
        Item last;
        GCHECK(lastItem(gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(menu)), &last));
        MCGuiNote("%s: last item \"%s\" %s(%s)", name.c_str(), last.label.c_str(),
                  last.action.c_str(), last.target.c_str());
        GCHECK_EQ_STR(last.label, Appearance::kItemLabel);
        if (action) {
            GCHECK_EQ_STR(last.action, "win.adjustsettings");
            GCHECK_EQ_STR(last.target, surface);
        }
        MCGuiClick(MCGuiFindLabel(menu, Appearance::kItemLabel));
    });
    MCGuiWait("the settings file at the " + surface + " keys", 15, [surface] {
        gWhy.clear();
        return atKeys(surface, &gWhy);
    });
    MCGuiStep("checked: " + surface, [surface] {
        if (!gWhy.empty()) MCGuiNote("while waiting: %s", gWhy.c_str());
        GCHECK(gWhy.empty());
        GCHECK(!openMenu());   // the menu went
    });
}

#ifdef MINICODE_ENABLE_TERMINAL
GtkWidget* findVte(GtkWidget* root) {
    if (!root) return nullptr;
    if (VTE_IS_TERMINAL(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findVte(c)) return hit;
    return nullptr;
}
#endif

GtkWidget* findTextView(GtkWidget* root) {
    if (!root) return nullptr;
    if (GTK_IS_TEXT_VIEW(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findTextView(c)) return hit;
    return nullptr;
}

GtkWidget* findEntry(GtkWidget* root) {
    if (!root) return nullptr;
    if (GTK_IS_ENTRY(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findEntry(c)) return hit;
    return nullptr;
}

GtkWidget* findMenuBar(GtkWidget* root) {
    if (!root) return nullptr;
    if (GTK_IS_POPOVER_MENU_BAR(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findMenuBar(c)) return hit;
    return nullptr;
}

// A mapped popover (a menu bar's menu) under the window showing `label`.
GtkWidget* popoverWith(const std::string& label) {
    return menuWith(GTK_WIDGET(MCGuiApp().window), label);
}

}  // namespace

MC_GUI_SUITE(adjust_settings) {
    MCGuiStep("files, a whole settings file, and a window of its own", [] {
        MCGuiWriteFile(F("adjust.txt"), "def plain():\n    return 1\n");
        MCGuiWriteFile(F("adjust.md"), "# A page\n\nSome *text* to right click on.\n");
        MCGuiSetSettings(fullFile());
        gWindows = MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("the window", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("its key window", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });

    // ---- the file tree
    rightClickChoose("the tree's src row", [] {
        return MCGuiFindLabel(MCGuiApp().tree->widget(), "src");   // first, folders first
    }, "tree");

    // ---- the editor, on a file of source
    MCGuiStep("a file of source in the editor", [] { MCGuiOpenFileKept(F("adjust.txt")); });
    MCGuiWait("on show", 10, [] { return ed()->currentPath() == F("adjust.txt"); });
    MCGuiStep("its menu is the editor's", [] {
        GCHECK_EQ_STR(Appearance::editorMenuSurface(ed()->textView()), "editor");
    });
    rightClickChoose("the editor's text", [] { return ed()->textView(); }, "editor", -1, -1, true,
                     [](GtkWidget* view, double* x, double* y) {
        return pointAtText(view, "plain", x, y);
    });

    // ---- a Markdown page in the same view
    MCGuiStep("a Markdown file, which opens in its page", [] { MCGuiOpenFileKept(F("adjust.md")); });
    MCGuiWait("its page on show", 10, [] {
        return ed()->currentPath() == F("adjust.md") && ed()->inPreview();
    });
    MCGuiStep("its menu is the page's", [] {
        GCHECK_EQ_STR(Appearance::editorMenuSurface(ed()->textView()), "markdown");
    });
    rightClickChoose("the Markdown page", [] { return ed()->textView(); }, "markdown", -1, -1, true,
                     [](GtkWidget* view, double* x, double* y) {
        return pointAtText(view, "right click", x, y);
    });
    MCGuiStep("back on the source of a file, the menu is the editor's again", [] {
        MCGuiOpenFileKept(F("adjust.txt"));
    });
    MCGuiWait("so", 10, [] {
        return ed()->currentPath() == F("adjust.txt") &&
               Appearance::editorMenuSurface(ed()->textView()) == "editor";
    });

    // ---- a tab, and the strip's empty part
    rightClickChoose("a tab's title", [] { return ed()->strip()->titleWidget(0); }, "tabs");
    rightClickChoose("the tab strip past the tabs", [] { return ed()->strip()->widget(); }, "tabs",
                     -20, -1);

    // ---- the status bar
    rightClickChoose("the status bar", [] { return MCGuiApp().statusBar; }, "statusbar", 6, -1);

    // ---- the Source Control panel
    MCGuiStep("the Source Control panel by its key", [] { MCGuiKey("ctrl+shift+g"); });
    rightClickChoose("the Source Control panel", [] {
        GitPanel* git = MCGuiApp().git;
        return git ? git->widget() : nullptr;
    }, "git", 40, 4);
    rightClickChoose("the commit message box", [] {
        GitPanel* git = MCGuiApp().git;
        return git ? findTextView(git->widget()) : nullptr;
    }, "git");
    MCGuiStep("the tree again", [] { MCGuiKey("ctrl+shift+g"); });
    MCGuiWait("the tree on show", 10, [] { return MCGuiOnScreen(MCGuiApp().tree->widget()); });

#ifdef MINICODE_ENABLE_TERMINAL
    // ---- the terminal
    MCGuiStep("the terminal by its key", [] {
        if (!MCGuiApp().layout->terminalShown()) MCGuiKey("ctrl+shift+t");
    });
    MCGuiWait("made and on screen", 15, [] {
        Terminal* t = MCGuiApp().terminal;
        return t && t->made() && MCGuiOnScreen(findVte(t->widget()));
    });
    rightClickChoose("the terminal", [] { return findVte(MCGuiApp().terminal->widget()); },
                     "terminal");
    MCGuiStep("its menu also has Copy, Paste and Select All", [] {
        MCGuiClick(findVte(MCGuiApp().terminal->widget()), -1, -1, 3);
    });
    MCGuiWait("open", 10, [] { return openMenu() != nullptr; });
    MCGuiStep("so, and Escape closes it", [] {
        GtkWidget* m = openMenu();
        for (const char* item : {"Copy", "Paste", "Select All"})
            GCHECK(m && MCGuiFindLabel(m, item));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return openMenu() == nullptr; });
#endif

#ifdef MINICODE_ENABLE_BROWSER
    // ---- the browser: its page and its address bar
    MCGuiStep("the browser by its key", [] {
        if (!MCGuiApp().layout->browserShown()) MCGuiKey("ctrl+shift+b");
    });
    MCGuiWait("its page loaded", 20, [] {
        Browser* b = MCGuiApp().browser;
        GtkWidget* web = b ? b->webView() : nullptr;
        return web && MCGuiOnScreen(web) && !webkit_web_view_is_loading(WEBKIT_WEB_VIEW(web)) &&
               webkit_web_view_get_uri(WEBKIT_WEB_VIEW(web));
    });
    rightClickChoose("the browser's page", [] { return MCGuiApp().browser->webView(); }, "browser",
                     -1, -1, false);
    rightClickChoose("the browser's address bar", [] {
        return findEntry(MCGuiApp().browser->widget());
    }, "browser");
#endif

    // ---- the tint lasts about three seconds
    rightClickChoose("the status bar again", [] { return MCGuiApp().statusBar; }, "statusbar", 6,
                     -1);
    // Time is the condition here: the tint is still there 1.5 s on.
    MCGuiPause(1.5);
    MCGuiStep("still tinted", [] {
        GCHECK(!Appearance::tintedLines(ed()->buffer()).empty());
    });
    MCGuiWait("gone by 3.3 s and a little", 4, [] {
        return Appearance::tintedLines(ed()->buffer()).empty();
    });
    MCGuiStep("nothing else changed: the file is as on disk, nothing to undo but nothing", [] {
        Document* d = settingsDoc();
        GCHECK(d && !d->dirty());
        GCHECK_EQ_STR(bufferText(ed()->buffer()), MCGuiReadFile(settingsPath()));
    });

    // ---- an old file, its tab clean: the keys added, one undo step, saved
    MCGuiStep("a settings file older than the status bar's keys", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
    });
    MCGuiWait("its clean tab follows the disk", 10, [] {
        Document* d = settingsDoc();
        return d && !d->dirty() && bufferText(d->buffer()) == "lsp.enabled = false\n";
    });
    rightClickChoose("the status bar, on an old file", [] { return MCGuiApp().statusBar; },
                     "statusbar", 6, -1);
    MCGuiStep("the keys at the end under one line, the file saved at once", [] {
        const std::string want =
            "lsp.enabled = false\n" +
            Settings::adjustSettingsAddition("lsp.enabled = false\n", "statusbar");
        GCHECK_EQ_STR(bufferText(ed()->buffer()), want);
        GCHECK(want.find("\n\n# Added by Adjust Settings: the Status Bar keys this file did not "
                         "have yet.\n") != std::string::npos);
        GCHECK(want.find("# statusbar.background = ") != std::string::npos);
    });
    MCGuiWait("on disk too, the tab clean", 10, [] {
        Document* d = settingsDoc();
        return d && !d->dirty() && MCGuiReadFile(settingsPath()) == bufferText(ed()->buffer());
    });
    MCGuiStep("Ctrl+Z", [] {
        GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
        MCGuiNote("active %d, focus %s, key window %p, app window %p, top %p",
                  gtk_window_is_active(MCGuiApp().window), f ? G_OBJECT_TYPE_NAME(f) : "none",
                  (void*)MCGuiKeyWindow(), (void*)MCGuiApp().window, (void*)MCGuiTopWindow());
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("takes the whole addition back in one step", 10, [] {
        return bufferText(ed()->buffer()) == "lsp.enabled = false\n";
    });
    MCGuiStep("Ctrl+Shift+Z", [] { MCGuiKey("ctrl+shift+z"); });
    MCGuiWait("puts it back, as on disk", 10, [] {
        Document* d = settingsDoc();
        return d && !d->dirty() && bufferText(ed()->buffer()) == MCGuiReadFile(settingsPath());
    });

    // ---- the same with unsaved changes: added, not saved
    MCGuiStep("an unsaved line typed at its top", [] {
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(ed()->buffer(), &start);
        gtk_text_buffer_place_cursor(ed()->buffer(), &start);
        MCGuiType("# mine\n");
    });
    MCGuiWait("unsaved", 10, [] {
        Document* d = settingsDoc();
        return d && d->dirty() && bufferText(ed()->buffer()).rfind("# mine\n", 0) == 0;
    });
    MCGuiStep("the disk before, and View > Adjust Settings > Terminal by the menu bar", [] {
        gText = bufferText(ed()->buffer());
        MCGuiClick(MCGuiFindLabel(findMenuBar(GTK_WIDGET(MCGuiApp().window)), "View"));
    });
    MCGuiWait("the View menu", 10, [] { return popoverWith("Adjust Settings") != nullptr; });
    MCGuiStep("Adjust Settings", [] { MCGuiClick(MCGuiFindLabel(popoverWith("Adjust Settings"),
                                                                "Adjust Settings")); });
    MCGuiWait("its rows", 10, [] { return popoverWith("Syntax Colors") != nullptr; });
    MCGuiStep("Terminal", [] {
        GtkWidget* m = popoverWith("Syntax Colors");
        for (const AppearanceSurface& s : Settings::appearanceSurfaces())
            GCHECK(MCGuiFindLabel(m, s.title) != nullptr);
        MCGuiClick(MCGuiFindLabel(m, "Terminal"));
    });
    MCGuiWait("the terminal's keys added at the end, the caret on them", 10, [] {
        std::string why;
        return bufferText(ed()->buffer()) ==
                   gText + Settings::adjustSettingsAddition(gText, "terminal") &&
               atKeys("terminal", &why);
    });
    // Time is the condition: a save would have come at once.
    MCGuiPause(0.5);
    MCGuiStep("not saved: the tab had unsaved changes", [] {
        Document* d = settingsDoc();
        GCHECK(d && d->dirty());
        GCHECK(MCGuiReadFile(settingsPath()).find("# mine") == std::string::npos);
        GCHECK(MCGuiReadFile(settingsPath()).find("terminal.background") == std::string::npos);
    });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("takes only the addition back", 10, [] {
        return bufferText(ed()->buffer()) == gText;
    });

    // ---- done: the edits dropped, the window closed
    // The typed line undone too, a step at a time (GTK takes typing back a
    // piece at a time), never past the file as on disk.
    for (int i = 0; i < 8; i++) {
        MCGuiStep("Ctrl+Z while unsaved", [] {
            gText = bufferText(ed()->buffer());
            gPressed = settingsDoc() && settingsDoc()->dirty();
            if (gPressed) MCGuiKey("ctrl+z");
        });
        MCGuiWait("it landed", 10, [] { return !gPressed || bufferText(ed()->buffer()) != gText; });
    }
    MCGuiWait("clean", 10, [] {
        Document* d = settingsDoc();
        return d && !d->dirty();
    });
    MCGuiStep("the window closed", [] {
        MCGuiSetSettings("lsp.enabled = false\n");
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("one window fewer", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window has the keyboard again", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
    });
}
