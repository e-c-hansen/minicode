// ThemesSuite.cpp — `./build/minicode-guitest themes` (PARITY.md 2.11): View >
// Theme, chosen by real clicks through the menu bar, writes `theme = <id>`
// into the settings file, and the window follows at once, read back as
// pixels: the editor's text, the gutter, the file tree, the terminal, the
// status bar and the tab strip in the theme's colors, the syntax tags in
// its colors, the keyboard back where it was, and GTK's own widgets
// (the menu bar) dark or light with it (gtk-interface-color-scheme). The row
// of the theme in use is checked. A file with colors of its own asks first:
// Keep My Colors keeps them, Use the Theme's Colors comments them out, Cancel
// writes nothing. A settings file with CRLF line breaks keeps them, one that
// is a symlink into a dotfiles folder stays a link (nothing left beside
// either), and while a tab holds it with unsaved changes the theme is
// refused, the reason in the status bar, the tab and the disk untouched,
// until the edit is undone. The find bar and the Source Control panel follow
// a theme too; a theme chosen while a refresh makes the panel's rows again
// leaves the keyboard in the panel, one chosen from the editor leaves it in
// the editor, and a row of the closed menu taking it (GTK's late submenu
// timer) gives it back. The Mac's checks: config_writes's theme steps.
#include "Harness.h"

#include "Appearance.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "FileTree.h"
#include "FindBar.h"
#include "FindRules.h"
#include "GitPanel.h"
#include "Gutter.h"
#include "Palette.h"
#include "TabStrip.h"
#include "WindowLayout.h"

#ifdef MINICODE_ENABLE_TERMINAL
#include "Terminal.h"
#include <vte/vte.h>
#endif

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

size_t gWindows = 0;
std::string gWhy;
std::string gDisk;   // the settings file before a refused or cancelled theme
bool gPressed = false;
GtkWidget* gPaneBefore = nullptr;    // where the keyboard was before a menu (paneOf)
std::string gText;
GWeakRef gRowBefore;   // the Source Control row with the keyboard before a menu

std::string settingsPath() { return g_getenv("MINICODE_SETTINGS"); }
std::string dots(const std::string& name) { return MCGuiScratch() + "/dots/" + name; }
Editor* ed() { return MCGuiApp().editor; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }
Settings fileSettings() { return Settings::parse(MCGuiReadFile(settingsPath())); }

std::string bufferText(GtkTextBuffer* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

Document* settingsDoc() { return ed() ? ed()->documentFor(settingsPath()) : nullptr; }

bool isLink(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

// A widget as drawn, as RGBA bytes (GutterSuite's).
struct Picture {
    int width = 0, height = 0;
    double scale = 1;
    std::vector<guchar> bytes;
    bool at(double x, double y, int* r, int* g, int* b) const {
        const int px = (int)(x * scale), py = (int)(y * scale);
        if (px < 0 || py < 0 || px >= width || py >= height) return false;
        const guchar* p = &bytes[(size_t)(py * width + px) * 4];
        *b = p[0];
        *g = p[1];
        *r = p[2];
        return true;
    }
};
Picture picture(GtkWidget* w) {
    Picture pic;
    if (!w || !gtk_widget_get_mapped(w)) return pic;
    const int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
    if (ww <= 0 || wh <= 0) return pic;
    GdkPaintable* paintable = gtk_widget_paintable_new(w);
    GtkSnapshot* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snap, ww, wh);
    GskRenderNode* node = gtk_snapshot_free_to_node(snap);
    GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(w));
    if (node && renderer) {
        GdkTexture* t = gsk_renderer_render_texture(renderer, node, nullptr);
        pic.width = gdk_texture_get_width(t);
        pic.height = gdk_texture_get_height(t);
        pic.scale = (double)pic.width / ww;
        pic.bytes.resize((size_t)pic.width * pic.height * 4);
        gdk_texture_download(t, pic.bytes.data(), (size_t)pic.width * 4);
        g_object_unref(t);
    }
    if (node) gsk_render_node_unref(node);
    g_object_unref(paintable);
    return pic;
}

std::string hex(int r, int g, int b) {
    char s[16];
    g_snprintf(s, sizeof(s), "#%02X%02X%02X", r, g, b);
    return s;
}

// The pixel at (x, y) of `w` (negative: from the far edge) is near `want`.
bool pixelIs(const char* what, GtkWidget* w, double x, double y, const Rgba& want) {
    const Picture p = picture(w);
    if (p.width <= 0) {
        gWhy = std::string(what) + " not drawn";
        return false;
    }
    if (x < 0) x += p.width / p.scale;
    if (y < 0) y += p.height / p.scale;
    int r = 0, g = 0, b = 0;
    if (!p.at(x, y, &r, &g, &b) || std::abs(r - want.r) > 6 || std::abs(g - want.g) > 6 ||
        std::abs(b - want.b) > 6) {
        gWhy = std::string(what) + " is " + hex(r, g, b) + ", not " + hex(want.r, want.g, want.b);
        return false;
    }
    return true;
}

GtkWidget* findMenuBar(GtkWidget* root) {
    if (!root) return nullptr;
    if (GTK_IS_POPOVER_MENU_BAR(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findMenuBar(c)) return hit;
    return nullptr;
}
GtkWidget* menuBar() { return findMenuBar(GTK_WIDGET(MCGuiApp().window)); }

// A mapped popover menu under `root` showing `label`.
GtkWidget* menuWith(GtkWidget* root, const std::string& label) {
    if (!root || !gtk_widget_get_mapped(root)) return nullptr;
    if (GTK_IS_POPOVER_MENU(root) && MCGuiFindLabel(root, label)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = menuWith(c, label)) return hit;
    return nullptr;
}
GtkWidget* popoverWith(const std::string& label) {
    return menuWith(GTK_WIDGET(MCGuiApp().window), label);
}
bool anyMenuOpen() {
    std::function<bool(GtkWidget*)> open = [&](GtkWidget* w) {
        if (GTK_IS_POPOVER(w) && gtk_widget_get_mapped(w)) return true;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            if (open(c)) return true;
        return false;
    };
    return open(GTK_WIDGET(MCGuiApp().window));
}

// A label under `root`, on show or not (a closed menu's rows).
GtkWidget* findLabelAnywhere(GtkWidget* root, const std::string& text) {
    if (!root) return nullptr;
    if (GTK_IS_LABEL(root) && text == gtk_label_get_text(GTK_LABEL(root))) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = findLabelAnywhere(c, text)) return hit;
    return nullptr;
}

// The GtkModelButton a menu row's label is in.
GtkWidget* rowOf(GtkWidget* label) {
    for (GtkWidget* w = label; w; w = gtk_widget_get_parent(w))
        if (g_str_equal(G_OBJECT_TYPE_NAME(w), "GtkModelButton")) return w;
    return nullptr;
}
bool rowChecked(GtkWidget* menu, const std::string& title) {
    GtkWidget* row = rowOf(MCGuiFindLabel(menu, title));
    gboolean active = FALSE;
    if (row) g_object_get(row, "active", &active, NULL);
    return active;
}

GtkWidget* paneOf(GtkWidget* w);

// View > Theme opened by real clicks, up to its rows.
void openThemeMenu() {
    MCGuiWait("no menu open", 10, [] { return !anyMenuOpen() && MCGuiOnScreen(menuBar()); });
    MCGuiStep("View", [] {
        gPaneBefore = paneOf(gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)));
        MCGuiClick(MCGuiFindLabel(menuBar(), "View"));
    });
    MCGuiWait("the View menu", 10, [] { return popoverWith("Theme") != nullptr; });
    MCGuiStep("Theme", [] { MCGuiClick(MCGuiFindLabel(popoverWith("Theme"), "Theme")); });
    MCGuiWait("its rows", 10, [] {
        return popoverWith("Dark Modern") != nullptr && popoverWith("Solarized Light") != nullptr;
    });
}

// `title` from View > Theme, open.
void pickTheme(const std::string& title) {
    MCGuiStep(title, [title] { MCGuiClick(MCGuiFindLabel(popoverWith(title), title)); });
    MCGuiWait("the menu closed", 10, [] { return !anyMenuOpen(); });
    // GTK's menu bar kept it on its item (MenuBarFocus.h).
    MCGuiWait("the keyboard back where it was", 10, [] {
        return gPaneBefore && paneOf(gtk_root_get_focus(GTK_ROOT(MCGuiApp().window))) == gPaneBefore;
    });
}

// View > Theme > `title`.
void chooseTheme(const std::string& title) {
    openThemeMenu();
    pickTheme(title);
}

int colorScheme() {
    int scheme = -1;
    g_object_get(gtk_settings_get_default(), "gtk-interface-color-scheme", &scheme, NULL);
    return scheme;
}

std::string themeState() {
    GVariant* s = g_action_group_get_action_state(G_ACTION_GROUP(MCGuiApplication()), "theme");
    std::string out = s ? g_variant_get_string(s, nullptr) : "";
    if (s) g_variant_unref(s);
    return out;
}

GtkWidget* vte() {
#ifdef MINICODE_ENABLE_TERMINAL
    std::function<GtkWidget*(GtkWidget*)> find = [&](GtkWidget* w) -> GtkWidget* {
        if (!w) return nullptr;
        if (VTE_IS_TERMINAL(w)) return w;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            if (GtkWidget* hit = find(c)) return hit;
        return nullptr;
    };
    return MCGuiApp().terminal ? find(MCGuiApp().terminal->widget()) : nullptr;
#else
    return nullptr;
#endif
}

// Where the keyboard is, as a person sees it: the editor's text, the Source
// Control panel, the tree, the find bar or the terminal (else the widget
// itself, the menu bar's items among them). A pane may move the keyboard to
// a widget of its own meanwhile: the Source Control panel makes its rows
// again on every refresh, and the keyboard with them.
GtkWidget* paneOf(GtkWidget* w) {
    const MCGuiAppWindow app = MCGuiApp();
    std::vector<GtkWidget*> panes = {ed()->textView(), app.tree->widget(), app.findBar->widget()};
    if (app.git) panes.push_back(app.git->widget());
    if (GtkWidget* t = vte()) panes.push_back(t);
    for (GtkWidget* x = w; x; x = gtk_widget_get_parent(x))
        for (GtkWidget* pane : panes)
            if (x == pane) return pane;
    return w;
}

// Whether the menu bar, which GTK's own theme draws, is dark: read from the
// window as drawn, since the bar paints nothing of its own (the window's
// background shows through it).
bool menuBarDark(bool* known) {
    GtkWidget* bar = menuBar();
    GtkWidget* window = GTK_WIDGET(MCGuiApp().window);
    graphene_rect_t r;
    *known = false;
    if (!bar || !gtk_widget_compute_bounds(bar, window, &r)) return false;
    const Picture p = picture(window);
    int red = 0, green = 0, blue = 0;
    *known = p.width > 0 && p.at(r.origin.x + r.size.width - 4, r.origin.y + r.size.height / 2,
                                 &red, &green, &blue);
    return (0.299 * red + 0.587 * green + 0.114 * blue) / 255 < 0.5;
}

// Everything on show in the theme the settings file names (with its own
// colors): the editor's text and gutter, the tree, the terminal, the
// keyword tag, and GTK's darkness. gWhy says what is not.
bool inTheme(const std::string& id) {
    const Settings s = fileSettings();
    if (std::string(s.theme().id) != id) {
        gWhy = std::string("the file names ") + s.theme().id;
        return false;
    }
    if (themeState() != id) {
        gWhy = "View > Theme checks " + themeState();
        return false;
    }
    Rgba editor = s.background(Surface::Editor), gutter = s.gutterBackground(),
         sidebar = s.background(Surface::Sidebar), terminal = s.background(Surface::Terminal);
    if (!pixelIs("the editor's text", ed()->textView(), -30, -30, editor)) return false;
    if (!pixelIs("the gutter", ed()->codeView()->gutter()->widget(), 2, -6, gutter)) return false;
    // At the first row's right end: rows are clear but for the selected one
    // (the file on show, never the first: folders come first) and the hovered.
    if (!pixelIs("the tree", MCGuiApp().tree->widget(), -24, 4, sidebar)) return false;
#ifdef MINICODE_ENABLE_TERMINAL
    if (!pixelIs("the terminal", vte(), -12, -12, terminal)) return false;
#endif
    // The status bar and the tab strip past its tabs, the stylesheet's.
    if (!pixelIs("the status bar", MCGuiApp().statusBar, -40, 1, s.background(Surface::Statusbar)))
        return false;
    if (!pixelIs("the tab strip", ed()->strip()->widget(), -10, 8, sidebar)) return false;
    GtkTextTag* kw = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(ed()->buffer()),
                                               TagNameForStyle(TokenStyle::Keyword));
    GdkRGBA* c = nullptr;
    if (kw) g_object_get(kw, "foreground-rgba", &c, NULL);
    const Rgba want = s.syntax(TokenStyle::Keyword);
    const bool kwRight = c && std::lround(c->red * 255) == want.r &&
                         std::lround(c->green * 255) == want.g &&
                         std::lround(c->blue * 255) == want.b;
    if (c) gdk_rgba_free(c);
    if (!kwRight) {
        gWhy = "the keyword tag is not " + hex(want.r, want.g, want.b);
        return false;
    }
    const int scheme = s.isDark() ? GTK_INTERFACE_COLOR_SCHEME_DARK : GTK_INTERFACE_COLOR_SCHEME_LIGHT;
    if (colorScheme() != scheme) {
        gWhy = "GTK's color scheme is " + std::to_string(colorScheme());
        return false;
    }
    bool known = false;
    const bool dark = menuBarDark(&known);
    if (!known || dark != s.isDark()) {
        gWhy = std::string("the menu bar is ") + (dark ? "dark" : "light");
        return false;
    }
    gWhy.clear();
    return true;
}

void waitInTheme(const std::string& id) {
    MCGuiWait("everything in " + id, 10, [id] { return inTheme(id); });
    MCGuiStep("so: " + id, [] {
        if (!gWhy.empty()) MCGuiNote("not: %s", gWhy.c_str());
        GCHECK(gWhy.empty());
    });
}

// The question about the file's own colors, answered by a real click.
void answer(const std::string& button) {
    MCGuiWait("the question", 10, [] {
        GtkWindow* top = MCGuiTopWindow();
        return top != MCGuiApp().window && MCGuiFindButton(GTK_WIDGET(top), "Keep My Colors");
    });
    MCGuiStep("it says how many, and offers three answers: " + button, [button] {
        GtkWidget* top = GTK_WIDGET(MCGuiTopWindow());
        GCHECK(MCGuiFindLabel(top, "Your settings file sets 1 color of its own") != nullptr);
        for (const char* b : {"Use the Theme's Colors", "Keep My Colors", "Cancel"})
            GCHECK(MCGuiFindButton(top, b) != nullptr);
        MCGuiClick(MCGuiFindButton(top, button));
    });
    MCGuiWait("answered", 10, [] { return MCGuiTopWindow() == MCGuiApp().window; });
}

// Undo, a step at a time, until the tab on show is clean.
void undoUntilClean() {
    for (int i = 0; i < 6; i++) {
        MCGuiStep("Ctrl+Z while unsaved", [] {
            gText = bufferText(ed()->buffer());
            gPressed = ed()->dirty();
            if (gPressed) MCGuiKey("ctrl+z");
        });
        MCGuiWait("it landed", 10, [] { return !gPressed || bufferText(ed()->buffer()) != gText; });
    }
    MCGuiWait("clean", 10, [] { return !ed()->dirty(); });
}

const char* kBase = "lsp.enabled = false\n";

}  // namespace

MC_GUI_SUITE(themes) {
    MCGuiStep("a file of source, the plain settings, and a window of its own", [] {
        MCGuiWriteFile(F("themes.py"), "def shown():\n    return 1\n");
        MCGuiSetSettings(kBase);
        gWindows = MCGuiAppWindows().size();
        MCGuiKey("ctrl+shift+n");
    });
    MCGuiWait("the window", 10, [] { return MCGuiAppWindows().size() == gWindows + 1; });
    MCGuiStep("the file in it, and the terminal", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        MCGuiOpenFileKept(F("themes.py"));
#ifdef MINICODE_ENABLE_TERMINAL
        if (!MCGuiApp().layout->terminalShown()) MCGuiKey("ctrl+shift+t");
#endif
    });
    MCGuiWait("both on screen", 15, [] {
        return ed()->currentPath() == F("themes.py") && ed()->codeView()->gutter()->shown()
#ifdef MINICODE_ENABLE_TERMINAL
               && MCGuiOnScreen(vte())
#endif
            ;
    });
    MCGuiStep("the keyboard in the editor", [] { MCGuiKey("ctrl+1"); });
    waitInTheme("dark-plus");

    // ---- a light theme and a dark one, from the menu bar
    chooseTheme("Solarized Light");
    MCGuiWait("written", 10, [] {
        return MCGuiReadFile(settingsPath()) == "theme = solarized-light\n\nlsp.enabled = false\n";
    });
    waitInTheme("solarized-light");
    MCGuiStep("a picture", [] { MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "theme-solarized-light"); });
    openThemeMenu();
    MCGuiStep("its row is the one checked", [] {
        GtkWidget* m = popoverWith("Solarized Light");
        GCHECK(rowChecked(m, "Solarized Light"));
        GCHECK(!rowChecked(m, "Dark+") && !rowChecked(m, "Monokai"));
        MCGuiKey("escape");
    });
    MCGuiWait("closed", 10, [] { return !anyMenuOpen(); });
    chooseTheme("Monokai");
    waitInTheme("monokai");
    MCGuiStep("a picture", [] { MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "theme-monokai"); });

    // ---- a file with a color of its own: the question
    MCGuiStep("the file's own editor background", [] {
        MCGuiSetSettings(std::string(kBase) + "editor.background = #102030\n");
    });
    chooseTheme("Light+");
    answer("Keep My Colors");
    MCGuiWait("the theme written, the color kept, and shown", 10, [] {
        const std::string text = MCGuiReadFile(settingsPath());
        return text.find("theme = light-plus\n") != std::string::npos &&
               text.find("\neditor.background = #102030\n") != std::string::npos &&
               inTheme("light-plus");
    });
    MCGuiStep("so; dark as drawn, whatever the theme", [] {
        if (!gWhy.empty()) MCGuiNote("not: %s", gWhy.c_str());
        GCHECK(gWhy.empty());
        GCHECK(fileSettings().isDark());
        GCHECK(colorScheme() == GTK_INTERFACE_COLOR_SCHEME_DARK);
    });
    chooseTheme("Quiet Light");
    answer("Use the Theme's Colors");
    MCGuiWait("the theme written, the color commented out", 10, [] {
        const std::string text = MCGuiReadFile(settingsPath());
        return text.find("theme = quiet-light\n") != std::string::npos &&
               text.find("# editor.background = #102030") != std::string::npos &&
               Settings::colorOverrides(text) == 0;
    });
    waitInTheme("quiet-light");
    MCGuiStep("a color of its own again", [] {
        gDisk = std::string(kBase) + "editor.background = #102030\n";
        MCGuiSetSettings(gDisk);
    });
    chooseTheme("Dark Modern");
    answer("Cancel");
    // Time is the condition: a write would have come at once.
    MCGuiPause(0.5);
    MCGuiStep("Cancel wrote nothing", [] {
        GCHECK_EQ_STR(MCGuiReadFile(settingsPath()), gDisk);
        GCHECK_EQ_STR(themeState(), "dark-plus");
    });

    // ---- CRLF kept
    MCGuiStep("a settings file with CRLF line breaks", [] {
        MCGuiSetSettings("lsp.enabled = false\r\n# theme = dark-plus\r\n");
    });
    chooseTheme("Monokai");
    MCGuiWait("the line uncommented, CRLF kept", 10, [] {
        return MCGuiReadFile(settingsPath()) == "lsp.enabled = false\r\ntheme = monokai\r\n";
    });
    waitInTheme("monokai");

    // ---- a symlink into a dotfiles folder stays a link
    MCGuiStep("settings.conf a symlink into a dotfiles folder", [] {
        MCGuiWriteFile(dots("settings.conf"), kBase);
        unlink(settingsPath().c_str());
        GCHECK(symlink(dots("settings.conf").c_str(), settingsPath().c_str()) == 0);
        MCGuiReloadSettings();
        GCHECK(isLink(settingsPath()));
    });
    chooseTheme("Tomorrow Night Blue");
    MCGuiWait("the dotfiles copy written", 10, [] {
        return MCGuiReadFile(dots("settings.conf")).find("theme = tomorrow-night-blue\n") !=
               std::string::npos;
    });
    MCGuiStep("the link still a link, nothing left beside either", [] {
        GCHECK(isLink(settingsPath()));
        for (const std::string& dir : {MCGuiScratch() + "/dots", MCGuiScratch()}) {
            GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
            while (const char* name = d ? g_dir_read_name(d) : nullptr)
                GCHECK(!strstr(name, ".minicode-"));
            if (d) g_dir_close(d);
        }
    });
    waitInTheme("tomorrow-night-blue");
    MCGuiStep("the plain file back", [] {
        unlink(settingsPath().c_str());
        MCGuiSetSettings(kBase);
        GCHECK(!isLink(settingsPath()));
    });
    waitInTheme("dark-plus");

    // ---- refused while a tab holds the file with unsaved changes
    MCGuiStep("the settings file in a tab, an edit unsaved", [] {
        MCGuiOpenFileKept(settingsPath());
    });
    MCGuiWait("on show", 10, [] { return settingsDoc() && ed()->active() == settingsDoc(); });
    MCGuiStep("typed at its top", [] {
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(ed()->buffer(), &start);
        gtk_text_buffer_place_cursor(ed()->buffer(), &start);
        gtk_widget_grab_focus(ed()->textView());
        MCGuiType("x");
    });
    MCGuiWait("unsaved", 10, [] { return ed()->dirty() && bufferText(ed()->buffer())[0] == 'x'; });
    MCGuiStep("the disk as it is", [] { gDisk = MCGuiReadFile(settingsPath()); });
    chooseTheme("Solarized Dark");
    MCGuiWait("the reason in the status bar", 10, [] {
        const char* text = gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel));
        return strstr(text, "unsaved changes") != nullptr;
    });
    MCGuiStep("refused: the disk, the tab and the theme as they were", [] {
        MCGuiNote("status: %s", gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)));
        GCHECK_EQ_STR(gtk_label_get_text(GTK_LABEL(MCGuiApp().statusLabel)),
                      "settings.conf was not changed: a tab holds it with unsaved changes. Save or "
                      "close that tab, then try again.");
        GCHECK_EQ_STR(MCGuiReadFile(settingsPath()), gDisk);
        GCHECK(ed()->dirty() && bufferText(ed()->buffer()) == "x" + gDisk);
        GCHECK_EQ_STR(themeState(), "dark-plus");
    });
    undoUntilClean();
    chooseTheme("Solarized Dark");
    MCGuiWait("written now, and the tab follows the disk", 10, [] {
        return MCGuiReadFile(settingsPath()).find("theme = solarized-dark\n") != std::string::npos &&
               !ed()->dirty() && bufferText(ed()->buffer()) == MCGuiReadFile(settingsPath());
    });
    MCGuiStep("back to the source file", [] { MCGuiOpenFileKept(F("themes.py")); });
    waitInTheme("solarized-dark");

    // ---- the find bar and the Source Control panel follow too
    // The project a repository of its own with one new file in it, so that
    // the panel lists a change whatever the checkout the checks run in
    // holds (the rest ignored; all of it removed at the end of the block).
    MCGuiAsync("the project a repository, one new file in it", 20, [](MCGuiDone done) {
        MCGuiWriteFile(F(".gitignore"), "*\n!themes-new.txt\n");
        MCGuiWriteFile(F("themes-new.txt"), "new\n");
        MCGuiRunProcess({"git", "init", "-q"}, {}, [done](int status, std::string, std::string err) {
            if (status != 0) MCGuiNote("git init: %s", err.c_str());
            GCHECK(status == 0);
            done();
        });
    });
    MCGuiStep("the find bar by its key", [] { MCGuiKey("ctrl+f"); });
    MCGuiWait("open", 10, [] { return MCGuiApp().findBar->shown(); });
    MCGuiStep("and the Source Control panel by its", [] { MCGuiKey("ctrl+shift+g"); });
    // Opening the panel puts the keyboard on its first file once git has
    // answered (GitPanel::focus); waited for, so that it is not the menu
    // below that it lands after.
    MCGuiWait("both on screen, the keyboard on the new file's row", 10, [] {
        GitPanel* git = MCGuiApp().git;
        if (!git || !MCGuiOnScreen(git->widget()) || !MCGuiOnScreen(MCGuiApp().findBar->widget()) ||
            git->focusPending())
            return false;
        bool listed = false;
        for (const std::string& row : git->rowDescriptions())
            listed = listed || row.find("themes-new.txt") != std::string::npos;
        GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
        return listed && paneOf(focus) == git->widget() && !GTK_IS_LIST_VIEW(focus);
    });
    // A refresh while a menu is open (the window coming back to the front,
    // a file monitor, an action) makes the panel's rows again, the one with
    // the keyboard among them: the keyboard comes back to the list, where
    // it was left on the menu bar.
    MCGuiStep("the row with the keyboard", [] {
        g_weak_ref_set(&gRowBefore, gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)));
    });
    openThemeMenu();
    MCGuiStep("git asked again meanwhile", [] { MCGuiApp().git->refresh(); });
    MCGuiWait("the rows made again", 10, [] {
        GtkWidget* row = GTK_WIDGET(g_weak_ref_get(&gRowBefore));
        const bool gone = !row || !gtk_widget_get_mapped(row);
        if (row) g_object_unref(row);
        return gone;
    });
    pickTheme("Light Modern");
    MCGuiWait("both in Light Modern", 10, [] {
        gWhy.clear();
        const Settings s = fileSettings();
        if (std::string(s.theme().id) != "light-modern") {
            gWhy = "not written";
            return false;
        }
        GtkWidget* bar = MCGuiApp().findBar->widget();
        const FindRules::Palette p = FindRules::palette("light-modern", s.isDark());
        return pixelIs("the find bar", bar, gtk_widget_get_width(bar) / 2.0, 2,
                       Rgba::hex(p.widgetBg >> 8)) &&   // RRGGBBAA
               pixelIs("the Source Control panel", MCGuiApp().git->widget(), -6, 4,
                       s.background(Surface::Sidebar));
    });
    MCGuiStep("so", [] {
        if (!gWhy.empty()) MCGuiNote("not: %s", gWhy.c_str());
        MCGuiSnapshot(GTK_WIDGET(MCGuiApp().window), "theme-light-modern");
    });

    // With the panel showing and the keyboard in the editor, a theme from
    // the menu bar puts it back in the editor, not in the panel's list.
    MCGuiStep("the keyboard in the editor, the panel still showing", [] { MCGuiKey("ctrl+1"); });
    MCGuiWait("so", 10, [] {
        return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == ed()->textView() &&
               MCGuiOnScreen(MCGuiApp().git->widget());
    });
    chooseTheme("Dark Modern");
    MCGuiWait("written", 10, [] {
        return MCGuiReadFile(settingsPath()).find("theme = dark-modern\n") != std::string::npos;
    });
    // GTK's model button opens its submenu 80 ms after the pointer last
    // moved over it, by a timer the menu closing does not stop, and that
    // grabs the keyboard for the row: after a quick click, with the loop
    // kept busy by the theme's restyle and a refresh, it fired once the
    // menu had closed, and typing went to a row no longer on show. The
    // same grab, made at once:
    MCGuiStep("a row of the closed menu takes the keyboard, as that timer's did", [] {
        GtkWidget* row = rowOf(findLabelAnywhere(menuBar(), "Dark Modern"));
        GCHECK(row && !gtk_widget_get_mapped(row));
        if (row) gtk_widget_grab_focus(row);
        GCHECK(row && gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == row);
    });
    MCGuiWait("the keyboard back in the editor", 10, [] {
        return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == ed()->textView();
    });
    MCGuiStep("the tree again, the find bar closed", [] {
        MCGuiKey("ctrl+shift+g");
        MCGuiKey("ctrl+1");
        MCGuiKey("escape");
    });
    MCGuiWait("so", 10, [] {
        return !MCGuiApp().findBar->shown() && MCGuiOnScreen(MCGuiApp().tree->widget());
    });
    MCGuiAsync("the project no repository again", 20, [](MCGuiDone done) {
        MCGuiRunProcess({"rm", "-rf", ".git", ".gitignore", "themes-new.txt"}, {},
                        [done](int status, std::string, std::string) {
            GCHECK(status == 0);
            done();
        });
    });

    // ---- done
    MCGuiStep("the plain settings, and the window closed", [] {
        MCGuiSetSettings(kBase);
        MCGuiKey("ctrl+shift+w");
    });
    MCGuiWait("one window fewer", 10, [] { return MCGuiAppWindows().size() == gWindows; });
    MCGuiStep("the first window has the keyboard again, in Dark+", [] {
        MCGuiSetKeyWindow(nullptr);
        MCGuiFocusKeyWindow();
        GCHECK_EQ_STR(themeState(), "dark-plus");
        GCHECK(colorScheme() == GTK_INTERFACE_COLOR_SCHEME_DARK);
    });
}
