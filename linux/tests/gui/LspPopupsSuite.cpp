// LspPopupsSuite.cpp — lsp_popups: the language server's popups by the caret
// in the theme's colors, the rename field over the name, and the rename
// field kept through an app switch (linux/HANDOFF.md item 119; the Mac's
// #212 and #223, checked there by lsp_fixes). Against the scripted server,
// with real keys:
//
//   - In dark-modern and in light-modern, the hover box (Ctrl+Alt+I), the
//     completion list (Ctrl+Space), the signature hint (typing "("), the
//     references list (Shift+F12) and the rename box (F2) are drawn in the
//     theme's editorWidget.background (FindRules::palette, which the find
//     bar reads), never one dark color on every theme; the rename field in
//     its input.background.
//   - The rename field lies over the name's own line, its text in the
//     editor's font, and leaves the next line in view.
//   - A name typed into the rename field stays, with the field, while
//     another program has the keyboard, and the field has the keyboard
//     again when the window does; Escape still closes it.
#include "Harness.h"

#include "Editor.h"
#include "FakeLspLog.h"
#include "FindRules.h"
#include "Lsp.h"
#include "Settings.h"

#include <cstdlib>
#include <functional>
#include <string>

namespace {

Editor* ed() { return MCGuiApp().editor; }
LspSession* lsp() { return MCGuiApp().lsp; }
std::string F(const std::string& name) { return MCGuiProjectFile("popups/" + name); }
const char* kServer = "popups";
const std::string kText = "def helper(a, b):\n    return a\n\n\nhelper(1, 2)\nhel\n";
std::string gSettings;

std::string script() {
    const std::string rng = "{\"start\": {\"line\": 0, \"character\": 4}, \"end\": "
                            "{\"line\": 0, \"character\": 10}}";
    const std::string rng2 = "{\"start\": {\"line\": 4, \"character\": 0}, \"end\": "
                             "{\"line\": 4, \"character\": 6}}";
    return "{\"capabilities\": {\"textDocumentSync\": {\"openClose\": true, \"change\": 1},"
           " \"hoverProvider\": true, \"referencesProvider\": true,"
           " \"renameProvider\": {\"prepareProvider\": true},"
           " \"completionProvider\": {\"triggerCharacters\": [\".\"]},"
           " \"signatureHelpProvider\": {\"triggerCharacters\": [\"(\"]}},"
           " \"rules\": ["
           "{\"method\": \"textDocument/hover\", \"result\": {\"contents\": "
           "{\"kind\": \"markdown\", \"value\": \"helper(a, b) returns a\"}}},"
           "{\"method\": \"textDocument/completion\", \"result\": [{\"label\": \"helper\"},"
           " {\"label\": \"help\"}, {\"label\": \"hello\"}]},"
           "{\"method\": \"textDocument/signatureHelp\", \"result\": {\"signatures\": [{\"label\":"
           " \"helper(a, b)\", \"parameters\": [{\"label\": \"a\"}, {\"label\": \"b\"}]}],"
           " \"activeSignature\": 0, \"activeParameter\": 0}},"
           "{\"method\": \"textDocument/references\", \"result\": ["
           "{\"uri\": \"${uri}\", \"range\": " + rng + "}, {\"uri\": \"${uri}\", \"range\": " + rng2 +
           "}]},"
           "{\"method\": \"textDocument/prepareRename\", \"result\": {\"range\": " + rng +
           ", \"placeholder\": \"helper\"}},"
           "{\"method\": \"textDocument/rename\", \"result\": null}]}";
}

// The keyboard in the rename field (in the GtkText inside the entry).
bool fieldHasKeyboard() {
    GtkWidget* e = lsp()->renameEntry();
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
    return e && f && (f == e || gtk_widget_is_ancestor(f, e)) && gtk_window_is_active(MCGuiApp().window);
}

std::string textNow() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(ed()->buffer(), &a, &b);
    char* t = gtk_text_buffer_get_text(ed()->buffer(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

void caretAt(int line, int column) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, line, column);
    gtk_text_buffer_place_cursor(ed()->buffer(), &it);
}

// The mapped popover with this CSS class, anywhere under the window.
GtkWidget* popover(const char* cls) {
    std::function<GtkWidget*(GtkWidget*)> find = [&](GtkWidget* w) -> GtkWidget* {
        if (GTK_IS_POPOVER(w) && gtk_widget_get_mapped(w) && gtk_widget_has_css_class(w, cls))
            return w;
        for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
            if (GtkWidget* hit = find(c)) return hit;
        return nullptr;
    };
    return find(GTK_WIDGET(MCGuiApp().window));
}

std::string hex(int r, int g, int b) {
    char s[16];
    g_snprintf(s, sizeof s, "#%02X%02X%02X", r, g, b);
    return s;
}

// The popover's own color just inside its child's top left corner (the
// child's padding, or a list's background), as drawn.
bool popoverColor(GtkWidget* pop, int* r, int* g, int* b, double dx = 2, double dy = 2) {
    GtkWidget* child = pop ? gtk_popover_get_child(GTK_POPOVER(pop)) : nullptr;
    graphene_rect_t c;
    if (!child || !gtk_widget_compute_bounds(child, pop, &c)) return false;
    int px[4] = {0, 0, 0, 0};
    const double x = dx >= 0 ? c.origin.x + dx : c.origin.x + c.size.width + dx;
    const double y = dy >= 0 ? c.origin.y + dy : c.origin.y + c.size.height + dy;
    if (!MCGuiPixel(pop, (int)x, (int)y, px)) return false;
    *r = px[0];
    *g = px[1];
    *b = px[2];
    return true;
}

bool near(int r, int g, int b, uint32_t want) {
    const int wr = (int)(want >> 24), wg = (int)((want >> 16) & 0xFF), wb = (int)((want >> 8) & 0xFF);
    return std::abs(r - wr) <= 6 && std::abs(g - wg) <= 6 && std::abs(b - wb) <= 6;
}

std::string want(uint32_t c) { return hex((int)(c >> 24), (int)((c >> 16) & 0xFF), (int)((c >> 8) & 0xFF)); }

FindRules::Palette palette() {
    const Settings s = Settings::parse(MCGuiReadFile(g_getenv("MUTIBLE_SETTINGS")));
    return FindRules::palette(s.theme().id, s.isDark());
}

// The popup's background, against the theme's widget background.
void checkPopup(const std::string& theme, const char* what, const char* cls, double dx = 2,
                double dy = 2) {
    GtkWidget* pop = popover(cls);
    GCHECK(pop != nullptr);
    if (!pop) return;
    MCGuiSnapshot(pop, "lsp-popups-" + theme + "-" + what);
    int r = 0, g = 0, b = 0;
    GCHECK(popoverColor(pop, &r, &g, &b, dx, dy));
    const uint32_t bg = palette().widgetBg;
    MCGuiNote("%s on %s: %s, the theme's widget background %s", what, theme.c_str(),
              hex(r, g, b).c_str(), want(bg).c_str());
    GCHECK_EQ_STR(near(r, g, b, bg) ? want(bg) : hex(r, g, b), want(bg));
}

// The popover drawn: a picture of one not drawn yet is empty.
bool drawn(const char* cls) {
    int r = 0, g = 0, b = 0;
    GtkWidget* pop = popover(cls);
    return pop && popoverColor(pop, &r, &g, &b) && (r || g || b);
}

void themeRound(const std::string& theme) {
    MCGuiStep("theme " + theme, [theme] {
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) +
                         "\ntheme = " + theme + "\n");
        ed()->focusActiveDocument();
        caretAt(0, 6);
        MCGuiKey("ctrl+alt+i");
    });
    MCGuiWait("the hover box", 10, [] { return lsp()->hoverVisible() && drawn("minicode-hover"); });
    MCGuiStep("the hover box in the theme's colors", [theme] {
        checkPopup(theme, "hover", "minicode-hover");
        MCGuiKey("Escape");
    });
    MCGuiWait("closed", 5, [] { return !lsp()->hoverVisible(); });

    MCGuiStep("Ctrl+Space at the end of the last line", [] {
        caretAt(5, 3);
        MCGuiKey("ctrl+space");
    });
    MCGuiWait("the completion list", 10, [] {
        return lsp()->completionVisible() && drawn("minicode-completion");
    });
    MCGuiStep("the list in the theme's colors (its last row's right end)", [theme] {
        checkPopup(theme, "completion", "minicode-completion", -3, -3);
        MCGuiKey("Escape");
    });
    MCGuiWait("closed", 5, [] { return !lsp()->completionVisible(); });

    MCGuiStep("\"(\" typed after the call's name", [] {
        caretAt(5, 3);
        MCGuiType("(");
    });
    MCGuiWait("the signature hint", 10, [] {
        return !lsp()->signatureText().empty() && drawn("minicode-signature");
    });
    MCGuiStep("the hint in the theme's colors", [theme] {
        checkPopup(theme, "signature", "minicode-signature");
        MCGuiKey("Escape");
    });
    MCGuiWait("closed", 5, [] { return lsp()->signatureText().empty(); });
    MCGuiStep("the \"(\" taken back; Shift+F12 on helper", [] {
        MCGuiKey("ctrl+z");
        caretAt(0, 6);
        MCGuiKey("shift+F12");
    });
    MCGuiWait("the references", 10, [] {
        return lsp()->pickerKind() == "references" && drawn("minicode-picker");
    });
    MCGuiStep("the list in the theme's colors (its last row's right end)", [theme] {
        checkPopup(theme, "references", "minicode-picker", -3, -3);
        MCGuiKey("Escape");
    });
    MCGuiWait("closed", 5, [] { return !lsp()->pickerVisible(); });

    MCGuiStep("F2", [] { MCGuiKey("F2"); });
    MCGuiWait("the rename box", 10, [] {
        return lsp()->renameBoxVisible() && lsp()->renameEntry() &&
               MCGuiOnScreen(lsp()->renameEntry());
    });
    MCGuiStep("the rename box in the theme's colors, its field in input's", [theme] {
        GtkWidget* pop = popover("minicode-rename");
        GtkWidget* entry = lsp()->renameEntry();
        MCGuiSnapshot(pop, "lsp-popups-" + theme + "-rename");
        graphene_rect_t e;
        int px[4] = {0, 0, 0, 0};
        GCHECK(pop && entry && gtk_widget_compute_bounds(entry, pop, &e));
        GCHECK(MCGuiPixel(pop, (int)(e.origin.x + e.size.width - 4), (int)(e.origin.y + e.size.height / 2), px));
        const uint32_t in = palette().inputBg;
        MCGuiNote("rename field on %s: %s, the theme's input background %s", theme.c_str(),
                  hex(px[0], px[1], px[2]).c_str(), want(in).c_str());
        GCHECK_EQ_STR(near(px[0], px[1], px[2], in) ? want(in) : hex(px[0], px[1], px[2]), want(in));
    });
    MCGuiStep("the field over the name's line, the next line left in view", [] {
        GtkTextView* tv = GTK_TEXT_VIEW(ed()->textView());
        GtkTextIter name, next;
        gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &name, 0, 4);
        gtk_text_buffer_get_iter_at_line(ed()->buffer(), &next, 1);
        GdkRectangle n, x;
        gtk_text_view_get_iter_location(tv, &name, &n);
        gtk_text_view_get_iter_location(tv, &next, &x);
        int nx, ny, xx, xy;
        gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, n.x, n.y, &nx, &ny);
        gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET, x.x, x.y, &xx, &xy);
        graphene_rect_t e;
        GCHECK(gtk_widget_compute_bounds(lsp()->renameEntry(), GTK_WIDGET(tv), &e));
        MCGuiNote("name's line %d-%d, next line from %d; the field %.0f-%.0f", ny, ny + n.height,
                  xy, e.origin.y, e.origin.y + e.size.height);
        GCHECK(e.origin.y < ny + n.height && e.origin.y + e.size.height > ny);   // meets the line
        GCHECK(e.origin.y + e.size.height <= xy + x.height / 2.0);              // not the next
        MCGuiKey("Escape");
    });
    MCGuiWait("closed", 5, [] { return !lsp()->renameBoxVisible(); });
}

}  // namespace

MC_GUI_SUITE(lsp_popups) {
    MCGuiStep("a Python file, the scripted server", [] {
        MCGuiSetKeyWindow(nullptr);
        ed()->closeAllTabs();
        gSettings = MCGuiReadFile(g_getenv("MUTIBLE_SETTINGS"));
        MCGuiWriteFile(F("p.py"), kText);
        MCGuiSetSettings("lsp.enabled = true\nlsp.python = " + MCGuiFakeLsp(kServer, script()) + "\n");
        MCGuiOpenFileKept(F("p.py"));
    });
    MCGuiWait("p.py open on the server", 20, [] {
        std::string t;
        return ed()->currentPath() == F("p.py") && MCGuiFakeLspText(kServer, "p.py", &t) && t == kText;
    });
    themeRound("dark-modern");
    themeRound("light-modern");

    // ---- the rename field through an app switch
    MCGuiStep("F2, a new name typed", [] {
        ed()->focusActiveDocument();
        caretAt(0, 6);
        MCGuiKey("F2");
    });
    MCGuiWait("the rename box", 10, [] {
        return lsp()->renameBoxVisible() && fieldHasKeyboard();
    });
    MCGuiStep("typed", [] { MCGuiType("assist"); });
    MCGuiWait("in the field", 5, [] {
        return std::string(gtk_editable_get_text(GTK_EDITABLE(lsp()->renameEntry()))) == "assist";
    });
    MCGuiStep("another program takes the keyboard", [] { MCGuiFocusElsewhere(); });
    MCGuiWait("the window has lost it", 5, [] { return !gtk_window_is_active(MCGuiApp().window); });
    MCGuiPause(0.5);   // a negative check: nothing may close the field meanwhile
    MCGuiStep("the field still there, its name kept; the window given back", [] {
        GCHECK(lsp()->renameBoxVisible());
        GCHECK_EQ_STR(gtk_editable_get_text(GTK_EDITABLE(lsp()->renameEntry())), "assist");
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("the window has the keyboard", 5, [] { return gtk_window_is_active(MCGuiApp().window); });
    MCGuiWait("the field has the keyboard again", 5, [] {
        return lsp()->renameBoxVisible() && fieldHasKeyboard();
    });
    MCGuiStep("the field and its name kept; typing goes on in it", [] {
        GCHECK_EQ_STR(gtk_editable_get_text(GTK_EDITABLE(lsp()->renameEntry())), "assist");
        MCGuiType("2");
    });
    MCGuiWait("typed on", 5, [] {
        return lsp()->renameBoxVisible() &&
               std::string(gtk_editable_get_text(GTK_EDITABLE(lsp()->renameEntry()))) == "assist2";
    });
    MCGuiStep("Escape", [] { MCGuiKey("Escape"); });
    MCGuiWait("closed, nothing renamed", 5, [] { return !lsp()->renameBoxVisible(); });
    MCGuiStep("the text as it was; the settings back", [] {
        GCHECK_EQ_STR(textNow(), kText);
        ed()->closeAllTabs();
        MCGuiSetSettings(gSettings);
    });
}
