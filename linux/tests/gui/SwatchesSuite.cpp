// SwatchesSuite.cpp — `./build/minicode-guitest swatches` (PARITY.md 2.11):
// in the settings file a color value is a swatch of itself, and a real click
// on one opens GTK's color chooser at that color, never at the color of the
// line the caret is on (the Mac's colorPicked: trap: the shared color panel
// was fed the color under the caret); opening it writes nothing; a color
// typed into the chooser's own field rewrites that line and saves the file,
// and Escape closes it. An opacity value is underlined, and a real click on
// it opens a slider at its value; the arrow keys move it, each move
// rewriting the line (uncommented, as a decimal) and saving, and Escape
// closes it. The Mac's swatch_escape suite.
#include "Harness.h"

#include "Document.h"
#include "Editor.h"

#include <cmath>
#include <functional>
#include <string>

namespace {

std::string settingsPath() { return g_getenv("MINICODE_SETTINGS"); }
Editor* ed() { return MCGuiApp().editor; }

const char* kFile = "lsp.enabled = false\n"
                    "editor.ruler-color = #FF0000\n"
                    "syntax.keyword = #00FF00\n"
                    "# editor.opacity = 1\n";

std::string bufferText(GtkTextBuffer* b) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    char* raw = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string out = raw ? raw : "";
    g_free(raw);
    return out;
}

std::string line(int n) {
    const std::string text = bufferText(ed()->buffer());
    size_t pos = 0;
    for (int i = 0; i < n && pos != std::string::npos; i++) {
        pos = text.find('\n', pos);
        if (pos != std::string::npos) pos++;
    }
    if (pos == std::string::npos || pos > text.size()) return "";
    const size_t end = text.find('\n', pos);
    return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// The point `offset` characters into the first `text` in the view's
// buffer, in the view's coordinates (the middle of that character).
bool pointAt(const std::string& text, int offset, double* x, double* y) {
    GtkTextView* view = GTK_TEXT_VIEW(ed()->textView());
    GtkTextIter start, at, end;
    gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(view), &start);
    if (!gtk_text_iter_forward_search(&start, text.c_str(), GTK_TEXT_SEARCH_TEXT_ONLY, &at, &end,
                                      nullptr))
        return false;
    gtk_text_iter_forward_chars(&at, offset);
    GdkRectangle r;
    gtk_text_view_get_iter_location(view, &at, &r);
    int wx, wy;
    gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, r.x + std::max(1, r.width / 2),
                                          r.y + r.height / 2, &wx, &wy);
    *x = wx;
    *y = wy;
    return r.height > 0;
}

void clickAt(const std::string& text, int offset) {
    double x = 0, y = 0;
    GCHECK(pointAt(text, offset, &x, &y));
    MCGuiClick(ed()->textView(), x, y);
}

GtkWidget* find(GtkWidget* root, bool (*is)(GtkWidget*)) {
    if (!root) return nullptr;
    if (is(root)) return root;
    for (GtkWidget* c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c))
        if (GtkWidget* hit = find(c, is)) return hit;
    return nullptr;
}
bool isChooser(GtkWidget* w) { return GTK_IS_COLOR_CHOOSER(w); }
bool isEntry(GtkWidget* w) { return GTK_IS_ENTRY(w) && gtk_widget_get_mapped(w); }
bool isScale(GtkWidget* w) { return GTK_IS_SCALE(w); }

GtkWidget* popover() { return ed()->pickerPopover(); }
GtkWidget* chooser() { return find(popover(), isChooser); }
GtkWidget* slider() { return find(popover(), isScale); }

std::string chooserColor() {
    GtkWidget* c = chooser();
    if (!c) return "";
    GdkRGBA rgba;
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(c), &rgba);
G_GNUC_END_IGNORE_DEPRECATIONS
    char s[16];
    g_snprintf(s, sizeof(s), "#%02X%02X%02X", (int)std::lround(rgba.red * 255),
               (int)std::lround(rgba.green * 255), (int)std::lround(rgba.blue * 255));
    return s;
}

bool open() { return popover() && gtk_widget_get_mapped(popover()); }

bool hasTag(int lineNo, int offset, const char* prefix) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, lineNo, offset);
    GSList* tags = gtk_text_iter_get_tags(&it);
    bool found = false;
    for (GSList* l = tags; l; l = l->next) {
        char* name = nullptr;
        g_object_get(l->data, "name", &name, NULL);
        if (name && g_str_has_prefix(name, prefix)) found = true;
        g_free(name);
    }
    g_slist_free(tags);
    return found;
}

}  // namespace

MC_GUI_SUITE(swatches) {
    MCGuiStep("a settings file with two colors and an opacity, open", [] {
        if (ed()->dirty()) MCGuiKey("ctrl+s");
        MCGuiSetSettings(kFile);
        MCGuiOpenFileKept(settingsPath());
    });
    MCGuiWait("its swatches and the opacity's underline", 10, [] {
        return ed()->active() && ed()->active()->isSettingsFile() &&
               bufferText(ed()->buffer()) == kFile && hasTag(1, 22, "swatch-#FF0000") &&
               hasTag(2, 19, "swatch-#00FF00") && hasTag(3, 19, Document::kOpacityTag) &&
               !hasTag(3, 4, Document::kOpacityTag);
    });

    // ---- a color
    MCGuiStep("the caret on the red line, by a click on its key", [] {
        gtk_widget_grab_focus(ed()->textView());
        clickAt("editor.ruler-color", 3);
    });
    MCGuiWait("there", 10, [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(ed()->buffer(), &it,
                                         gtk_text_buffer_get_insert(ed()->buffer()));
        return gtk_text_iter_get_line(&it) == 1;
    });
    // GTK's double-click time between the two clicks.
    MCGuiPause(0.5);
    // On its last character, where get_iter_at_location answered the next one.
    MCGuiStep("a click on the green swatch", [] { clickAt("#00FF00", 6); });
    MCGuiWait("the chooser", 10, [] { return open() && chooser(); });
    MCGuiStep("at the swatch's green, not the caret line's red; nothing written", [] {
        GCHECK_EQ_STR(chooserColor(), "#00FF00");
        GCHECK_EQ_STR(bufferText(ed()->buffer()), kFile);
        GCHECK_EQ_STR(MCGuiReadFile(settingsPath()), kFile);
        GCHECK(!ed()->dirty());
    });
    MCGuiWait("its field", 10, [] { return MCGuiOnScreen(find(popover(), isEntry)); });
    MCGuiStep("a color typed into its field", [] {
        MCGuiClick(find(popover(), isEntry));
    });
    MCGuiWait("the field has the keyboard", 10, [] {
        GtkWidget* e = find(popover(), isEntry);
        GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
        return e && f && (f == e || gtk_widget_is_ancestor(f, e));
    });
    MCGuiStep("#123456 and Return", [] {
        MCGuiKey("ctrl+a");
        MCGuiType("#123456\n");
    });
    MCGuiWait("that line rewritten and the file saved, the red line as it was", 10, [] {
        return line(2) == "syntax.keyword = #123456" && line(1) == "editor.ruler-color = #FF0000" &&
               MCGuiReadFile(settingsPath()) == bufferText(ed()->buffer()) && !ed()->dirty();
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the chooser closed", 10, [] { return !open(); });

    // ---- an opacity
    MCGuiWait("the opacity still underlined after the save", 10, [] {
        return hasTag(3, 19, Document::kOpacityTag);
    });
    MCGuiPause(0.5);   // GTK's double-click time since the last click in the view
    MCGuiStep("a click on the opacity's value", [] { clickAt("opacity = 1", 10); });
    MCGuiWait("a slider at 100%", 10, [] {
        return open() && slider() && std::lround(gtk_range_get_value(GTK_RANGE(slider()))) == 100;
    });
    MCGuiStep("nothing written yet", [] {
        GCHECK_EQ_STR(line(3), "# editor.opacity = 1");
        GCHECK(!ed()->dirty());
    });
    MCGuiWait("the slider has the keyboard", 10, [] {
        return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)) == slider();
    });
    MCGuiStep("Left five times", [] {
        for (int i = 0; i < 5; i++) MCGuiKey("left");
    });
    MCGuiWait("95%: the line uncommented, the file saved", 10, [] {
        return line(3) == "editor.opacity = 0.95" &&
               MCGuiReadFile(settingsPath()) == bufferText(ed()->buffer()) && !ed()->dirty();
    });
    MCGuiStep("and in effect", [] {
        const Settings s = Settings::parse(MCGuiReadFile(settingsPath()));
        GCHECK(std::fabs(s.opacity(Surface::Editor) - 0.95) < 0.001);
    });
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("the slider closed", 10, [] { return !open(); });

    // ---- done
    MCGuiStep("the tab closed, the plain settings back", [] {
        GCHECK(!ed()->dirty());
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("closed", 10, [] {
        return !ed()->active() || !ed()->active()->isSettingsFile();
    });
    MCGuiStep("plain", [] { MCGuiSetSettings("lsp.enabled = false\n"); });
}
