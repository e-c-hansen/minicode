// MultiCaretSuite.cpp — `./build/minicode-guitest multicaret`: several carets
// in the editor (linux/PARITY.md item 1.9, Carets.h), the counterpart of the
// Mac's MultiCaretClickSuite and the caret steps of its EditorKeysSuite and
// MultiEditSuite, every press real (XTest).
//
// Alt+click adds carets and takes one away; they are drawn (and their
// selections) and their lines are active in the gutter; typing goes in at
// every caret and one Ctrl+Z takes it all back; brackets close, are typed
// over and are deleted in pairs at every caret; Return keeps each line's
// indentation; Escape goes back to the first caret and a plain click ends
// them. Shift+Alt drags, Alt drags and middle-button drags make columns,
// a middle click that does not drag still pastes the primary selection,
// and a Shift+Alt click makes a column from the caret. Copy at several
// selections, and Paste of as many lines as carets puts one at each (the
// whole text at each otherwise). The arrows, Home and End move every caret,
// Up and Down keeping their columns; Ctrl+End and Ctrl+Shift+Home leave one.
// The find bar's Alt+Enter selects every match.
#include "CaretSuites.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "FindBar.h"
#include "Gutter.h"

#include <cmath>
#include <set>

using namespace caretsuite;

namespace {

std::string F(const std::string& name) { return MCGuiProjectFile(name); }

// 0 "one = 1\n", 8 "two = 22\n", 17 "three = 333\n", 29 "    four\n",
// 38 "    five\n", 47 "end\n".
const std::string kText = "one = 1\ntwo = 22\nthree = 333\n    four\n    five\nend\n";

void reset() {
    carets()->clear();
    gtk_text_buffer_set_text(buf(), kText.c_str(), -1);
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(buf(), &it);
    gtk_text_buffer_place_cursor(buf(), &it);
}

// The text as it was, a caret at its start, in a step of its own (the
// clicks of the next step land on lines laid out again).
void queueReset() {
    MCGuiStep("the text as it was", [] { reset(); });
    MCGuiWait("back", 5, [] { return text() == kText && caretsText() == "0"; });
}

std::string gClip;

// A widget as drawn, as RGB at a logical point (the Gutter suite's way).
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
bool near(int a, int b) { return std::abs(a - b) <= 24; }
// How many pixels in a box are near a color.
int countNear(const Picture& p, double x0, double y0, double x1, double y1, int r, int g, int b) {
    int n = 0;
    for (double y = y0; y < y1; y += 1.0 / p.scale)
        for (double x = x0; x < x1; x += 1.0 / p.scale) {
            int pr, pg, pb;
            if (p.at(x, y, &pr, &pg, &pb) && near(pr, r) && near(pg, g) && near(pb, b)) n++;
        }
    return n;
}
// The box of the gap before `offset`, in the view's coordinates.
GdkRectangle caretBox(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GdkRectangle strong;
    gtk_text_view_get_cursor_locations(tv(), &it, &strong, nullptr);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, strong.x, strong.y, &wx, &wy);
    return GdkRectangle{wx, wy, std::max(strong.width, 1), strong.height};
}

bool gutterActive(size_t line) {
    for (const Gutter::Row& r : ed()->codeView()->gutter()->drawnRows())
        if (r.line == line) return r.active;
    return false;
}

}  // namespace

MC_GUI_SUITE(multicaret) {
    MCGuiStep("a Python file of six lines, the editor with the keyboard", [] {
        MCGuiWriteFile(F("carets.py"), kText);
        MCGuiOpenFileKept(F("carets.py"));
    });
    MCGuiWait("it shows", 10, [] { return ed()->currentPath() == F("carets.py") && text() == kText; });
    MCGuiWait("laid out", 5, [] { return gtk_widget_get_height(view()) > 100; });
    MCGuiStep("a click at the start", [] { clickAt(0); });
    MCGuiWait("the editor has the keyboard", 5, [] { return editorHasKeyboard(); });
    waitCarets("the caret at 0", "0");

    // ------------------------------------------------ Alt+click and typing
    MCGuiStep("Alt+click at the start of the next two lines", [] {
        clickAt(8, "alt");
        clickAt(17, "alt");
    });
    waitCarets("three carets", "0 8 17");
    MCGuiWait("the two besides GTK's drawn, their lines active in the gutter", 5, [] {
        const std::vector<size_t>& d = carets()->drawnCarets();
        return d.size() == 2 && d[0] == 8 && d[1] == 17 && gutterActive(0) && gutterActive(1) &&
               gutterActive(2) && !gutterActive(3);
    });
    MCGuiStep("as pixels: a caret in the text's color at the start of each line", [] {
        const Picture p = picture(view());
        GdkRGBA fg;
        gtk_widget_get_color(view(), &fg);
        const int r = (int)(fg.red * 255), g = (int)(fg.green * 255), b = (int)(fg.blue * 255);
        for (size_t at : {(size_t)8, (size_t)17}) {
            const GdkRectangle c = caretBox(at);
            // The bar itself, and nothing of that color a few pixels left of
            // it (the gutter's edge, or the text's background).
            const int on = countNear(p, c.x, c.y + 2, c.x + 1.5, c.y + c.height - 2, r, g, b);
            MCGuiNote("caret at %zu: %d pixels in the text's color", at, on);
            GCHECK(on >= c.height / 2);
        }
        // Where no caret is (before the blanks of the fourth line), none.
        const GdkRectangle none = caretBox(29);
        GCHECK(countNear(p, none.x, none.y + 2, none.x + 1.5, none.y + none.height - 2, r, g, b) < 3);
    });
    MCGuiStep("typing x", [] { MCGuiType("x"); });
    MCGuiWait("x at every caret, the carets after it", 5, [] {
        return text() == "xone = 1\nxtwo = 22\nxthree = 333\n    four\n    five\nend\n" &&
               caretsText() == "1 10 20";
    });
    MCGuiStep("one Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("takes back all three, and the carets with them", 5,
              [] { return text() == kText && !carets()->active(); });

    // ------------------------------------------------ never across a line break
    // A click where an earlier one was, within the double-click time, would
    // be a double-click: time is the condition.
    MCGuiPause(0.5);
    MCGuiStep("carets at the start of lines 2 to 4; Backspace", [] {
        clickAt(8);
        clickAt(17, "alt");
        clickAt(29, "alt");
    });
    waitCarets("three", "8 17 29");
    MCGuiStep("Backspace", [] { MCGuiKey("backspace"); });
    MCGuiPause(0.3);   // a negative check: nothing joins the lines
    MCGuiStep("deletes nothing: no caret goes past its line's start", [] {
        GCHECK_EQ_STR(text(), kText);
        GCHECK_EQ_STR(caretsText(), "8 17 29");
    });
    MCGuiStep("Delete", [] { MCGuiKey("delete"); });
    MCGuiWait("the first character of each line", 5, [] {
        return text() == "one = 1\nwo = 22\nhree = 333\n   four\n    five\nend\n" &&
               caretsText() == "8 16 27";
    });
    MCGuiStep("End; Delete", [] {
        MCGuiKey("end");
        MCGuiKey("delete");
    });
    MCGuiPause(0.3);   // a negative check: nothing joins the lines
    MCGuiStep("deletes nothing at the lines' ends", [] {
        GCHECK_EQ_STR(text(), "one = 1\nwo = 22\nhree = 333\n   four\n    five\nend\n");
    });
    MCGuiStep("Escape; Ctrl+Z", [] {
        MCGuiKey("escape");
        MCGuiKey("ctrl+z");
    });
    MCGuiWait("back", 5, [] { return text() == kText; });

    // ------------------------------------------------ brackets at every caret
    MCGuiStep("carets at the end of the first three lines", [] {
        clickAt(7);
        clickAt(16, "alt");
        clickAt(28, "alt");
    });
    waitCarets("three", "7 16 28");
    MCGuiStep("( closes at every caret", [] { MCGuiType("("); });
    MCGuiWait("() at each, the carets inside", 5, [] {
        return text() == "one = 1()\ntwo = 22()\nthree = 333()\n    four\n    five\nend\n" &&
               caretsText() == "8 19 33";
    });
    MCGuiStep("Backspace", [] { MCGuiKey("backspace"); });
    MCGuiWait("takes each pair whole", 5, [] { return text() == kText && caretsText() == "7 16 28"; });
    MCGuiStep("(x) typed", [] { MCGuiType("(x)"); });
    MCGuiWait("the ) typed over at each, not doubled", 5, [] {
        return text() == "one = 1(x)\ntwo = 22(x)\nthree = 333(x)\n    four\n    five\nend\n" &&
               caretsText() == "10 22 37";
    });
    MCGuiStep("Alt+click on the second caret", [] { clickAt(22, "alt"); });
    waitCarets("takes it away", "10 37");
    MCGuiStep("Escape", [] { MCGuiKey("escape"); });
    MCGuiWait("one caret, where the first was", 5,
              [] { return !carets()->active() && caretsText() == "10"; });

    // ------------------------------------------------ Return, a click
    queueReset();
    MCGuiStep("carets at the end of the indented lines; Return; y", [] {
        clickAt(37);
        clickAt(46, "alt");
    });
    waitCarets("two", "37 46");
    MCGuiStep("Return and y", [] {
        MCGuiKey("enter");
        MCGuiType("y");
    });
    MCGuiWait("each new line keeps its line's indentation", 5, [] {
        return text() == "one = 1\ntwo = 22\nthree = 333\n    four\n    y\n    five\n    y\nend\n";
    });
    MCGuiStep("a plain click", [] { clickAt(2); });
    MCGuiWait("ends them", 5, [] { return !carets()->active() && caretsText() == "2"; });

    // ------------------------------------------------ columns
    queueReset();
    // VS Code's box selection: from the caret, wherever the drag begins.
    MCGuiStep("a click at column 1; Shift+Alt drag from the second line to column 3 of the third",
              [] {
        clickAt(1);
        double x0, y0, x1, y1;
        pointAt(10, &x0, &y0);
        pointAt(20, &x1, &y1);
        MCGuiDragWith("shift+alt", view(), x0, y0, x1, y1);
    });
    waitCarets("a selection on each line, columns 1 to 3", "1>3 9>11 18>20");
    MCGuiWait("the two besides GTK's selection drawn", 5,
              [] { return carets()->drawnSelections() == 2; });
    MCGuiStep("as pixels: the second line's selection in the selection's color", [] {
        const Picture p = picture(view());
        const GdkRectangle a = caretBox(9), b = caretBox(11);
        const int on = countNear(p, a.x + 1, a.y + 1, b.x - 1, a.y + a.height - 1, 0x26, 0x4F, 0x78);
        MCGuiNote("selection on line 2: %d pixels in #264F78", on);
        GCHECK(on > (b.x - a.x - 2) * 2);
    });
    MCGuiStep("Z typed over them", [] { MCGuiType("Z"); });
    MCGuiWait("each replaced", 5, [] {
        return text() == "oZ = 1\ntZ = 22\ntZee = 333\n    four\n    five\nend\n" &&
               caretsText() == "2 9 17";
    });
    queueReset();
    MCGuiStep("middle-button drag down from column 4 of the indented lines", [] {
        double x0, y0, x1, y1;
        pointAt(33, &x0, &y0);
        pointAt(42, &x1, &y1);
        MCGuiDragWith("", view(), x0, y0, x1, y1, 2);
    });
    waitCarets("a caret on each, column 4", "33 42");
    MCGuiStep("- typed", [] { MCGuiType("-"); });
    MCGuiWait("at both", 5, [] {
        return text() == "one = 1\ntwo = 22\nthree = 333\n    -four\n    -five\nend\n";
    });
    queueReset();
    MCGuiStep("one selected; a middle click, not dragged, at the end of the third line", [] {
        MCGuiKey("shift+right");
        MCGuiKey("shift+right");
        MCGuiKey("shift+right");
    });
    waitCarets("one selected", "0>3");
    MCGuiStep("the middle click", [] {
        double x, y;
        pointAt(28, &x, &y);
        MCGuiClick(view(), x, y, 2);
    });
    MCGuiWait("pastes the primary selection there, as GTK does", 5, [] {
        return text() == "one = 1\ntwo = 22\nthree = 333one\n    four\n    five\nend\n";
    });
    queueReset();
    MCGuiStep("Alt drag from the start of the first line to the start of the second", [] {
        double x0, y0, x1, y1;
        pointAt(0, &x0, &y0);
        pointAt(8, &x1, &y1);
        MCGuiDragWith("alt", view(), x0, y0, x1, y1);
    });
    waitCarets("a fresh column", "0 8");
    MCGuiStep("a click at column 2; Shift+Alt click at column 2 two lines down", [] {
        clickAt(2);
        clickAt(19, "shift+alt");
    });
    waitCarets("the column from the caret", "2 10 19");

    // ------------------------------------------------ the clipboard
    queueReset();
    MCGuiStep("Shift+Alt drag over the first three letters of three lines; Ctrl+C", [] {
        double x0, y0, x1, y1;
        pointAt(0, &x0, &y0);
        pointAt(20, &x1, &y1);
        MCGuiDragWith("shift+alt", view(), x0, y0, x1, y1);
    });
    waitCarets("three selections", "0>3 8>11 17>20");
    MCGuiStep("Ctrl+C", [] { MCGuiKey("ctrl+c"); });
    MCGuiAsync("the clipboard", 5, [](MCGuiDone done) { readClipboard(&gClip, done); });
    MCGuiStep("holds the three, a line each", [] { GCHECK_EQ_STR(gClip, "one\ntwo\nthr"); });
    MCGuiStep("three carets at the lines' ends; Ctrl+V", [] {
        clickAt(7);
        clickAt(16, "alt");
        clickAt(28, "alt");
    });
    waitCarets("three", "7 16 28");
    MCGuiStep("Ctrl+V", [] { MCGuiKey("ctrl+v"); });
    MCGuiWait("a line of the clipboard at each", 5, [] {
        return text() == "one = 1one\ntwo = 22two\nthree = 333thr\n    four\n    five\nend\n";
    });
    queueReset();
    MCGuiStep("two carets; Ctrl+V", [] {
        clickAt(37);
        clickAt(46, "alt");
    });
    waitCarets("two", "37 46");
    MCGuiStep("Ctrl+V", [] { MCGuiKey("ctrl+v"); });
    MCGuiWait("all of it at each", 5, [] {
        return text() == "one = 1\ntwo = 22\nthree = 333\n    fourone\ntwo\nthr\n    fiveone\ntwo\nthr\nend\n";
    });
    queueReset();
    MCGuiStep("a column of two selections; Ctrl+X", [] {
        clickAt(4);
        double x0, y0, x1, y1;
        pointAt(4, &x0, &y0);
        pointAt(14, &x1, &y1);
        MCGuiDragWith("shift+alt", view(), x0, y0, x1, y1);
    });
    waitCarets("two selections", "4>6 12>14");
    MCGuiStep("Ctrl+X", [] { MCGuiKey("ctrl+x"); });
    MCGuiWait("both cut", 5, [] {
        return text() == "one 1\ntwo 22\nthree = 333\n    four\n    five\nend\n" && caretsText() == "4 10";
    });
    MCGuiAsync("the clipboard", 5, [](MCGuiDone done) { readClipboard(&gClip, done); });
    MCGuiStep("holds both", [] { GCHECK_EQ_STR(gClip, "= \n= "); });

    // ------------------------------------------------ moving
    queueReset();
    MCGuiStep("carets at the start of three lines; Right", [] {
        clickAt(0);
        clickAt(8, "alt");
        clickAt(17, "alt");
    });
    waitCarets("three", "0 8 17");
    MCGuiStep("Right", [] { MCGuiKey("right"); });
    waitCarets("each a character on", "1 9 18");
    MCGuiStep("End", [] { MCGuiKey("end"); });
    waitCarets("each at its line's end", "7 16 28");
    MCGuiStep("Shift+Home", [] { MCGuiKey("shift+home"); });
    MCGuiWait("each line selected back to its start", 5,
              [] { return caretsText() == "7>0 16>8 28>17"; });
    MCGuiStep("Left", [] { MCGuiKey("left"); });
    waitCarets("the selections collapse to their left ends", "0 8 17");
    MCGuiStep("Down twice", [] {
        MCGuiKey("right");
        MCGuiKey("right");
        MCGuiKey("down");
    });
    waitCarets("each a line down at its column", "10 19 31");
    MCGuiStep("Up", [] { MCGuiKey("up"); });
    waitCarets("back", "2 10 19");
    MCGuiStep("Ctrl+End", [] { MCGuiKey("ctrl+end"); });
    MCGuiWait("one caret, at the end", 5, [] {
        return !carets()->active() && caretsText() == std::to_string(g_utf8_strlen(kText.c_str(), -1));
    });
    MCGuiStep("three carets; Ctrl+Shift+Home", [] {
        clickAt(2);
        clickAt(10, "alt");
        clickAt(19, "alt");
    });
    waitCarets("three", "2 10 19");
    MCGuiStep("Ctrl+Shift+Home", [] { MCGuiKey("ctrl+shift+home"); });
    MCGuiWait("one selection from the farthest caret to the start", 5,
              [] { return !carets()->active() && caretsText() == "19>0"; });
    MCGuiStep("and the text never changed", [] { GCHECK_EQ_STR(text(), kText); });

    // ------------------------------------------------ the find bar
    MCGuiStep("Ctrl+F; \"o\"", [] {
        clickAt(0);
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the find bar's query has the keyboard", 5, [] {
        FindBar* bar = MCGuiApp().findBar;
        return bar->shown() && bar->editing();
    });
    MCGuiStep("the query", [] {
        MCGuiKey("ctrl+a");
        MCGuiType("o");
    });
    MCGuiWait("three matches", 5, [] {
        FindBar* bar = MCGuiApp().findBar;
        return bar->query() == "o" && bar->resultsCurrent() && bar->matches().size() == 3;
    });
    MCGuiStep("Alt+Enter", [] { MCGuiKey("alt+enter"); });
    MCGuiWait("a selection at every match, the editor with the keyboard", 5,
              [] { return editorHasKeyboard() && caretsText() == "0>1 10>11 34>35"; });
    MCGuiStep("0 typed", [] { MCGuiType("0"); });
    MCGuiWait("over each", 5, [] {
        return text() == "0ne = 1\ntw0 = 22\nthree = 333\n    f0ur\n    five\nend\n";
    });
    MCGuiStep("Escape twice; the tab closes", [] {
        MCGuiKey("escape");
        MCGuiKey("escape");
    });
    MCGuiWait("the bar shut", 5, [] { return !MCGuiApp().findBar->shown(); });
    MCGuiStep("closed", [] { ed()->closeAllTabs(); });
}
