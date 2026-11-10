// RulerSuite.cpp — `./build/minicode-guitest ruler`: editor.ruler and
// editor.ruler-color (linux/PARITY.md item 1.4, CodeView.h), as the Mac's
// PageMeasureSuite and ZoomFitSuite check them: a 1 px line at each column,
// at the x where that column's character starts, read back as pixels of the
// view as drawn; under the text, only while the view is editable (none in a
// Markdown preview), none when off, and the default a single ruler at 100.
// The ruler is CodeView's first drawing pass, so this also checks the seam
// later items draw through.
#include "Harness.h"

#include "CodeView.h"
#include "Editor.h"
#include "Gutter.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
CodeView* cv() { return ed()->codeView(); }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const char* kBase = "lsp.enabled = false\n";

// The x, in buffer coordinates, of the character at `column` on line 0.
double charX(int column) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, 0, column);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    return r.x;
}

// The view as drawn, as bytes (GdkTexture's default: B, G, R, A).
struct Picture {
    int width = 0, height = 0;
    double scale = 1;
    std::vector<guchar> bytes;
    bool at(double x, double y, int* r, int* g, int* b) const {
        const int px = (int)std::floor(x * scale), py = (int)std::floor(y * scale);
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
bool near(int a, int b) { return std::abs(a - b) <= 8; }

// The ruler at buffer x is drawn as magenta in the view, below the text,
// with the background a few pixels to its left.
bool rulerDrawnAt(const Picture& p, double bufferX) {
    int wx = 0, wy = 0;
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv(), &vis);
    // Low in the view, below the file's few lines.
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, (int)std::lround(bufferX),
                                          vis.y + vis.height - 20, &wx, &wy);
    int r, g, b;
    if (!p.at(wx + 0.5, wy, &r, &g, &b) || !(near(r, 0xFF) && near(g, 0x00) && near(b, 0xFF)))
        return false;
    return p.at(wx - 3, wy, &r, &g, &b) && !(near(r, 0xFF) && near(b, 0xFF));
}

bool anyMagenta(const Picture& p) {
    int r, g, b;
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv(), &vis);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, 0, vis.y + vis.height - 20,
                                          &wx, &wy);
    for (double x = 0; x < p.width / p.scale; x += 1)
        if (p.at(x, wy, &r, &g, &b) && near(r, 0xFF) && near(g, 0) && near(b, 0xFF)) return true;
    return false;
}

const std::string kLine(70, 'x');

}  // namespace

MC_GUI_SUITE(ruler) {
    MCGuiStep("rulers at 20 and 40 in magenta, and a file with long lines", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(std::string(kBase) + "editor.ruler = 20,40\neditor.ruler-color = #FF00FF\n");
        MCGuiWriteFile(F("ruler.txt"), kLine + "\n" + kLine + "\nshort\n");
        MCGuiWriteFile(F("ruler.md"), "# Ruler\n\n" + kLine + "\n");
        MCGuiOpenFileKept(F("ruler.txt"));
    });
    MCGuiWait("two rulers drawn, each where its column's character starts", 10, [] {
        const std::vector<double>& xs = cv()->rulerXs();
        return ed()->currentPath() == F("ruler.txt") && xs.size() == 2 &&
               std::fabs(xs[0] - charX(20)) < 0.5 && std::fabs(xs[1] - charX(40)) < 0.5;
    });
    MCGuiWait("as pixels of the view: magenta lines at those columns", 5, [] {
        const Picture p = picture(ed()->textView());
        return p.width > 0 && cv()->rulerXs().size() == 2 && rulerDrawnAt(p, cv()->rulerXs()[0]) &&
               rulerDrawnAt(p, cv()->rulerXs()[1]);
    });
    MCGuiStep("right of the gutter, the columns' own x", [] {
        GCHECK(cv()->rulers() == (std::vector<int>{20, 40}));
        GCHECK(std::fabs(cv()->columnX(20) - charX(20)) < 0.5);
        GCHECK(charX(20) > charX(0) + 100);
        // The gutter is left of the text, so the line is right of it.
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET,
                                              (int)cv()->rulerXs()[0], 0, &wx, &wy);
        GCHECK(wx > gtk_widget_get_width(cv()->gutter()->widget()));
        MCGuiSnapshot(ed()->textView(), "ruler");
    });
    MCGuiStep("under the text: the line's characters are drawn over it", [] {
        // Column 20 of line 0 is an 'x'; its pixels are the text's color
        // somewhere in the character's box, not only magenta.
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line_offset(ed()->buffer(), &it, 0, 20);
        GdkRectangle r;
        gtk_text_view_get_iter_location(tv(), &it, &r);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
        const Picture p = picture(ed()->textView());
        bool light = false;
        for (double y = wy; y < wy + r.height; y += 1)
            for (double x = wx; x < wx + r.width; x += 1) {
                int pr, pg, pb;
                if (p.at(x, y, &pr, &pg, &pb) && pr > 0xA0 && pg > 0xA0 && pb > 0xA0) light = true;
            }
        GCHECK(light);
    });

    MCGuiStep("editor.ruler = off", [] {
        MCGuiSetSettings(std::string(kBase) + "editor.ruler = off\neditor.ruler-color = #FF00FF\n");
    });
    MCGuiWait("no ruler, and no magenta in the view", 5, [] {
        return cv()->rulers().empty() && cv()->rulerXs().empty() &&
               !anyMagenta(picture(ed()->textView()));
    });
    MCGuiStep("the default settings", [] { MCGuiSetSettings(kBase); });
    MCGuiWait("one ruler at 100, in the theme's color", 5, [] {
        return cv()->rulers() == std::vector<int>{100} &&
               std::fabs(cv()->rulerColor().red - 0x5A / 255.0) < 0.01;
    });

    MCGuiStep("rulers again, and a Markdown file, which opens in its preview", [] {
        MCGuiSetSettings(std::string(kBase) + "editor.ruler = 20,40\neditor.ruler-color = #FF00FF\n");
        MCGuiOpenFileKept(F("ruler.md"));
    });
    MCGuiWait("no ruler on the page", 10, [] {
        return ed()->currentPath() == F("ruler.md") && ed()->inPreview() &&
               cv()->rulerXs().empty() && !anyMagenta(picture(ed()->textView()));
    });
    MCGuiStep("Ctrl+Shift+V, its source", [] {
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the rulers beside the source", 10, [] {
        const std::vector<double>& xs = cv()->rulerXs();
        return !ed()->inPreview() && xs.size() == 2 && rulerDrawnAt(picture(ed()->textView()), xs[0]);
    });
    MCGuiStep("the tabs close, the settings as they were", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(kBase);
    });
    MCGuiWait("no ruler beside the welcome text", 5, [] { return cv()->rulerXs().empty(); });
}
