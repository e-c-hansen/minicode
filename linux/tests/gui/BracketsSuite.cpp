// BracketsSuite.cpp — `./build/minicode-guitest brackets`: the bracket by
// the caret and its partner boxed, and Go to Bracket (linux/PARITY.md item
// 1.7, BracketView.h), the counterpart of the Mac's bracket matching
// (EditorController+EditorView.mm's updateBracketMatch and goToBracket,
// LargeFileSuite's bracketMatchingApplies). The caret moves by real arrow
// keys and Go to Bracket is a real Ctrl+Shift+\ through the accelerator.
// Checked: the pair boxed for a caret after an opener, before a closer, and
// between two brackets (the one before first); the boxes as drawn, at the
// characters' boxes and as pixels of editor.bracket-match; none away from a
// bracket, where nothing is read or lexed; brackets in strings and comments
// neither boxed nor counted; a partner past 20,000 characters not found; a
// plain text file matching without a grammar; none in a Markdown page, and
// back on its source; Go to Bracket from an opener to its closer and back,
// and from inside a pair to its closer, the bell with none; and its key
// left to the program while the terminal has the keyboard.
#include "Harness.h"

#include "BracketView.h"
#include "Editor.h"
#include "Terminal.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
BracketView* bv() { return ed()->brackets(); }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const char* kBase = "lsp.enabled = false\neditor.bracket-match = #FF00FF\n"
                    "editor.bracket-pair-colorization = off\n";

// line 0: '(' 14, '[' 20, '{' 24, '0' 25, '}' 26, ']' 27, ')' 28
// line 1: f's '(' 5, the string's '(' 7, ')' 12, the comment's '(' 17 and ')' 19
const std::string kPy =
    "result = outer(inner[key{0}])\n"
    "s = f(\"(\", x)  # (c)\n"
    "def f(a, b):\n"
    "    return [a, (b)]\n";

size_t lineStart(int line) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(buf(), &it, line);
    return (size_t)gtk_text_iter_get_offset(&it);
}

size_t caret() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    return (size_t)gtk_text_iter_get_offset(&it);
}

void place(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    gtk_text_buffer_place_cursor(buf(), &it);
}

bool boxed(std::vector<size_t> want) { return bv()->boxed() == want; }

std::string shown(const std::vector<size_t>& v) {
    std::string s;
    for (size_t x : v) s += (s.empty() ? "" : " ") + std::to_string(x);
    return "{" + s + "}";
}

GdkRectangle charRect(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    return r;
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

bool magenta(int r, int g, int b) { return r > 200 && g < 90 && b > 200; }

// A box's left edge, as pixels: magenta down the character box's left
// column, and not left of it.
bool boxDrawnAt(const Picture& p, const GdkRectangle& r) {
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
    int hits = 0, cr, cg, cb;
    for (int dy = 2; dy < r.height - 4; dy++)
        if (p.at(wx + 0.5, wy + dy + 0.5, &cr, &cg, &cb) && magenta(cr, cg, cb)) hits++;
    const bool outside = p.at(wx - 2.5, wy + r.height / 2.0, &cr, &cg, &cb) && !magenta(cr, cg, cb);
    return hits >= r.height / 2 && outside;
}

bool anyMagenta(const Picture& p) {
    int r, g, b;
    for (double y = 0; y < p.height / p.scale; y += 1)
        for (double x = 0; x < p.width / p.scale; x += 1)
            if (p.at(x, y, &r, &g, &b) && magenta(r, g, b)) return true;
    return false;
}

int gLookups = 0, gUpdates = 0;
#ifdef MINICODE_ENABLE_TERMINAL
bool gTerminalWasOpen = false;
#endif

}  // namespace

MC_GUI_SUITE(brackets) {
    MCGuiStep("a Python file, the caret after outer( by the arrow keys", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(kBase);
        MCGuiWriteFile(F("brackets.py"), kPy);
        MCGuiOpenFileKept(F("brackets.py"));
        gtk_widget_grab_focus(ed()->textView());
        place(13);
        MCGuiKey("right");
        MCGuiKey("right");
    });
    MCGuiWait("( and its ) boxed", 10, [] { return caret() == 15 && boxed({14, 28}); });
    MCGuiWait("drawn at the two characters' boxes", 5, [] {
        const auto& d = bv()->drawnBoxes();
        if (d.size() != 2) return false;
        const GdkRectangle a = charRect(14), b = charRect(28);
        return d[0].x == a.x && d[0].y == a.y && d[0].width == a.width && d[1].x == b.x &&
               d[1].y == b.y;
    });
    MCGuiWait("as pixels: a box in editor.bracket-match around each", 5, [] {
        const Picture p = picture(ed()->textView());
        return p.width > 0 && boxDrawnAt(p, charRect(14)) && boxDrawnAt(p, charRect(28)) &&
               !boxDrawnAt(p, charRect(20));
    });
    MCGuiStep("a picture to look at", [] {
        MCGuiNote("boxed %s", shown(bv()->boxed()).c_str());
        MCGuiSnapshot(ed()->textView(), "brackets-boxes");
    });

    MCGuiStep("right, after [: the bracket before the caret first", [] {
        place(20);
        MCGuiKey("right");
    });
    MCGuiWait("[ and ] boxed", 5, [] { return caret() == 21 && boxed({20, 27}); });
    MCGuiStep("between { and 0", [] {
        place(24);
        MCGuiKey("right");
    });
    MCGuiWait("{ and } boxed", 5, [] { return boxed({24, 26}); });
    MCGuiStep("before }, a bracket after the caret only", [] { MCGuiKey("right"); });
    MCGuiWait("} and { boxed", 5, [] { return caret() == 26 && boxed({24, 26}); });
    MCGuiStep("between } and ]: the one before the caret first", [] { MCGuiKey("right"); });
    MCGuiWait("} and { still", 5, [] { return caret() == 27 && boxed({24, 26}); });

    MCGuiStep("away from any bracket, at the line's start", [] {
        gLookups = bv()->lookups();
        gUpdates = bv()->updates();
        MCGuiKey("home");
        MCGuiKey("right");
        MCGuiKey("right");
    });
    MCGuiWait("no boxes", 5, [] { return caret() == 2 && boxed({}) && bv()->drawnBoxes().empty(); });
    MCGuiStep("and nothing was read or lexed for it", [] {
        MCGuiNote("%d updates, %d of them looked", bv()->updates() - gUpdates,
                  bv()->lookups() - gLookups);
        GCHECK(bv()->updates() > gUpdates);
        GCHECK_EQ_STR(std::to_string(bv()->lookups()), std::to_string(gLookups));
        GCHECK(!anyMagenta(picture(ed()->textView())));
    });

    MCGuiStep("after ( in a string, with a ) in code after it", [] { place(lineStart(1) + 8); });
    MCGuiWait("nothing: a string's bracket is not one", 5, [] {
        return caret() == lineStart(1) + 8 && boxed({}) && bv()->lookups() > gLookups;
    });
    MCGuiStep("after ( in a comment, its ) just after", [] { place(lineStart(1) + 18); });
    MCGuiWait("nothing there either", 5, [] {
        return caret() == lineStart(1) + 18 && boxed({});
    });
    MCGuiStep("after f(, the string's ( inside not counted", [] { place(lineStart(1) + 6); });
    MCGuiWait("f's own ) boxed", 5, [] { return boxed({lineStart(1) + 5, lineStart(1) + 12}); });
    MCGuiStep("after the ( of def f(", [] { place(lineStart(2) + 6); });
    MCGuiWait("its own ) boxed", 5, [] { return boxed({lineStart(2) + 5, lineStart(2) + 10}); });

    // ---- Go to Bracket
    MCGuiStep("Ctrl+Shift+\\ after outer(", [] {
        place(15);
        MCGuiKey("ctrl+shift+\\");
    });
    MCGuiWait("before its )", 5, [] { return caret() == 28; });
    MCGuiStep("again", [] { MCGuiKey("ctrl+shift+\\"); });
    MCGuiWait("back before the (", 5, [] { return caret() == 14; });
    MCGuiStep("and again", [] { MCGuiKey("ctrl+shift+\\"); });
    MCGuiWait("before the ) once more", 5, [] { return caret() == 28; });
    MCGuiStep("inside def f(a, b), touching no bracket", [] {
        place(lineStart(2) + 7);
        MCGuiKey("ctrl+shift+\\");
    });
    MCGuiWait("before the pair's closer", 5, [] { return caret() == lineStart(2) + 10; });
    MCGuiStep("a file with no bracket after the caret", [] {
        MCGuiWriteFile(F("nobracket.py"), "x = 1\ny = 2\n");
        MCGuiOpenFileKept(F("nobracket.py"));
        gtk_widget_grab_focus(ed()->textView());
        place(2);
        MCGuiKey("ctrl+shift+\\");
    });
    MCGuiPause(0.4);   // a negative check: the key had its time and moved nothing
    MCGuiStep("the caret stays", [] { GCHECK(caret() == 2); });

    // ---- far apart, plain text, a page
    MCGuiStep("a ( whose ) is 25,000 characters on", [] {
        std::string far = "x = f(\n";
        for (int k = 0; k < 1000; k++) far += "    a_value_here_1234,\n";   // 23 a line
        far += "    1)\nnear = (2)\n";
        MCGuiWriteFile(F("far.py"), far);
        MCGuiOpenFileKept(F("far.py"));
        gtk_widget_grab_focus(ed()->textView());
        place(6);
    });
    MCGuiPause(0.3);   // a negative check: the match had its idle and found nothing
    MCGuiStep("not found, as the Mac's 20,000-unit search", [] {
        GCHECK(caret() == 6);
        GCHECK(boxed({}));
        const size_t n = (size_t)gtk_text_buffer_get_char_count(buf());
        place(n - 3);   // after near = (2's (
    });
    MCGuiWait("while a near pair is", 5, [] {
        const size_t n = (size_t)gtk_text_buffer_get_char_count(buf());
        return boxed({n - 4, n - 2});
    });
    MCGuiStep("a plain text file, which has no grammar", [] {
        MCGuiWriteFile(F("plain.txt"), "a (b \"(\" c) d\n");
        MCGuiOpenFileKept(F("plain.txt"));
        gtk_widget_grab_focus(ed()->textView());
        place(3);
    });
    MCGuiWait("every bracket counts: the outer ( has no partner", 5, [] {
        // Without a grammar nothing is a string, so the ( in quotes opens
        // and the only ) closes it.
        return caret() == 3 && boxed({}) && bv()->lookups() > gLookups;
    });
    MCGuiStep("after the ( in quotes", [] { place(7); });
    MCGuiWait("it and the ) are a pair", 5, [] { return boxed({6, 10}); });
    MCGuiStep("a Markdown file opens in its page", [] {
        MCGuiWriteFile(F("brackets.md"), "# Title\n\nSee (this) and [that].\n");
        MCGuiOpenFileKept(F("brackets.md"));
    });
    MCGuiWait("its page, the caret anywhere, no box", 10, [] {
        return ed()->currentPath() == F("brackets.md") && ed()->inPreview() && boxed({});
    });
    MCGuiStep("Ctrl+Shift+V, its source, the caret after (", [] {
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+shift+v");
    });
    MCGuiWait("the source", 10, [] { return !ed()->inPreview() && ed()->isSource(); });
    MCGuiStep("after (", [] { place(lineStart(2) + 5); });
    MCGuiWait("( and ) boxed in the source", 5, [] {
        return boxed({lineStart(2) + 4, lineStart(2) + 9});
    });

#ifdef MINICODE_ENABLE_TERMINAL
    // ---- the terminal's rule
    MCGuiStep("with the terminal's keyboard", [] {
        gchar** accels =
            gtk_application_get_accels_for_action(MCGuiApplication(), "win.gotobracket");
        guint key = 0;
        GdkModifierType mods = (GdkModifierType)0;
        GCHECK(accels && accels[0] && gtk_accelerator_parse(accels[0], &key, &mods) &&
               key == GDK_KEY_bar && mods == GDK_CONTROL_MASK);
        g_strfreev(accels);
        // An earlier suite may have left it open: Ctrl+` would then hide it.
        gTerminalWasOpen = gtk_widget_get_mapped(MCGuiApp().terminal->widget());
        if (gTerminalWasOpen) MCGuiApp().terminal->focus();
        else MCGuiKey("ctrl+`");
    });
    MCGuiWait("the terminal has it", 10, [] {
        GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
        return MCGuiApp().terminal->owns(focus);
    });
    MCGuiStep("Go to Bracket's key is the program's there", [] {
        gchar** accels =
            gtk_application_get_accels_for_action(MCGuiApplication(), "win.gotobracket");
        GCHECK(accels && !accels[0]);
        g_strfreev(accels);
        if (!gTerminalWasOpen) MCGuiKey("ctrl+`");   // as it was
        else gtk_widget_grab_focus(ed()->textView());
    });
#endif
    MCGuiStep("put back", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings("lsp.enabled = false\n");
    });
}
