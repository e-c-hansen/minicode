// GutterSuite.cpp — `./build/minicode-guitest gutter`: the line numbers
// beside the editor (linux/PARITY.md item 1.4, Gutter.h), the Linux
// counterparts of the Mac's gutter checks (MultiEditSuite's and
// AgentPlacesSuite's lineOfCharacter, FoldingSuite's shown gutter,
// ZoomFitSuite's rulersVisible). What the gutter drew is read back from it
// (Gutter::drawnRows) and held to an index of the whole text made afresh
// (LineIndex::resetUtf8, never the gutter's own) and to where GTK put each
// line: the numbers after typing that adds and removes lines and after a
// paste holding a lone '\r' and U+2029; the caret's line in the active
// color as the caret moves; relative numbering; a click, a drag up and down
// and Shift+click in the gutter selecting whole lines, with real input; the
// gutter gone in a Markdown preview and back with its source; each tab's
// own numbers and width; the settings (off, relative, the colors, read back
// as pixels); the dots of the language server's tags; and the change and
// fold lanes' APIs for items 2.10 and 1.10.
#include "Harness.h"

#include "BufferLines.h"
#include "CodeView.h"
#include "Document.h"
#include "Editor.h"
#include "Gutter.h"
#include "LineIndex.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
Gutter* gut() { return ed()->codeView()->gutter(); }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const char* kBase = "lsp.enabled = false\n";

std::string numbered(const std::string& word, int lines) {
    std::string s;
    for (int i = 1; i <= lines; i++) s += word + " " + std::to_string(i) + "\n";
    return s;
}

std::string bufferText() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string out = t ? t : "";
    g_free(t);
    return out;
}

std::vector<size_t> startsOf(const LineIndex& ix) {
    std::vector<size_t> v;
    for (size_t i = 0; i < ix.lineCount(); i++) v.push_back(ix.lineStart(i));
    return v;
}

size_t caretOffset() {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(buf(), &it, gtk_text_buffer_get_insert(buf()));
    return (size_t)gtk_text_iter_get_offset(&it);
}

// The selection as [start, end) and where the caret (the insert mark) is.
struct Sel {
    size_t start, end, caret;
};
Sel selection() {
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(buf(), &a, &b);
    return {(size_t)gtk_text_iter_get_offset(&a), (size_t)gtk_text_iter_get_offset(&b),
            caretOffset()};
}

// The rows the gutter drew against the text as it is: its index is the same
// as one made from scratch; every line starting on screen has a row, at the
// y GTK put that line in the gutter's coordinates, labelled as the mode
// says, in the active color exactly when the caret is on it. `why` says
// what differed.
std::string gWhy;
double gWidth3 = 0;   // the gutter's width at three digits
bool rowsRight(bool relative = false) {
    gWhy.clear();
    if (!gut()->shown()) { gWhy = "not shown"; return false; }
    // As wide as its digits are in the view's font now.
    if (gtk_widget_get_width(gut()->widget()) != gut()->wantedWidth()) {
        gWhy = "the gutter is " + std::to_string(gtk_widget_get_width(gut()->widget())) +
               " wide, its digits want " + std::to_string(gut()->wantedWidth());
        return false;
    }
    const std::string text = bufferText();
    LineIndex ix;
    ix.resetUtf8(text.data(), text.size());
    BufferLines* lines = BufferLines::of(buf());
    if (!lines || startsOf(lines->index()) != startsOf(ix)) {
        gWhy = "the gutter's index differs from the text's";
        return false;
    }
    const size_t caretLine = ix.lineOf(caretOffset());
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv(), &vis);
    // The lines starting on screen, top to bottom.
    std::vector<size_t> want;
    for (size_t line = 0; line < ix.lineCount(); line++) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)ix.lineStart(line));
        GdkRectangle r;
        gtk_text_view_get_iter_location(tv(), &it, &r);
        if (r.y >= vis.y + vis.height) break;
        if (r.y + r.height > vis.y) want.push_back(line);
    }
    const std::vector<Gutter::Row>& rows = gut()->drawnRows();
    if (rows.size() != want.size()) {
        gWhy = "rows " + std::to_string(rows.size()) + ", lines on screen " +
               std::to_string(want.size());
        return false;
    }
    for (size_t k = 0; k < rows.size(); k++) {
        const Gutter::Row& row = rows[k];
        const size_t line = want[k];
        if (row.line != line) {
            gWhy = "row " + std::to_string(k) + " is line " + std::to_string(row.line) +
                   ", want " + std::to_string(line);
            return false;
        }
        size_t shown = line + 1;
        if (relative && line != caretLine)
            shown = line > caretLine ? line - caretLine : caretLine - line;
        if (row.label != std::to_string(shown)) {
            gWhy = "line " + std::to_string(line) + " labelled " + row.label;
            return false;
        }
        if (row.active != (line == caretLine)) {
            gWhy = "line " + std::to_string(line) + (row.active ? " active" : " not active");
            return false;
        }
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)ix.lineStart(line));
        GdkRectangle r;
        gtk_text_view_get_iter_location(tv(), &it, &r);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_LEFT, 0, r.y, &wx, &wy);
        if (std::fabs(row.y - wy) > 0.5) {
            gWhy = "line " + std::to_string(line) + " drawn at " + std::to_string(row.y) +
                   ", text at " + std::to_string(wy);
            return false;
        }
    }
    return true;
}

// MCGuiWait, and when it timed out, what rowsRight found wrong last.
void waitFor(const std::string& what, double timeout, std::function<bool()> until) {
    MCGuiWait(what, timeout, until);
    MCGuiStep("(" + what + ")", [until] {
        if (until()) return;
        GdkRectangle vis;
        gtk_text_view_get_visible_rect(tv(), &vis);
        MCGuiNote("rows: %s; %s; gutter %d wide (%d at three digits), %d digits, shown %d, "
                  "preview %d, %zu rows drawn, the first %s, view y %d h %d",
                  gWhy.c_str(), ed()->currentPath().c_str(),
                  gtk_widget_get_width(gut()->widget()), (int)gWidth3, gut()->digits(),
                  gut()->shown(), ed()->inPreview(), gut()->drawnRows().size(),
                  gut()->drawnRows().empty() ? "-" : gut()->drawnRows()[0].label.c_str(), vis.y,
                  vis.height);
    });
}

const Gutter::Row* rowOf(size_t line) {
    for (const Gutter::Row& r : gut()->drawnRows())
        if (r.line == line) return &r;
    return nullptr;
}

// The middle of a line's number in the gutter (the numbers' lane).
double rowY(size_t line) {
    const Gutter::Row* r = rowOf(line);
    return r ? r->y + r->height / 2 : -1;
}
double numberX() { return gut()->numbersRight() - 3; }

size_t lineStart(size_t line) { return BufferLines::of(buf())->lineStart(line); }

// A widget as drawn, as RGBA bytes, `scale` device pixels a logical one.
struct Picture {
    int width = 0, height = 0;
    double scale = 1;
    std::vector<guchar> bytes;   // B, G, R, A (premultiplied), GdkTexture's default
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
bool near(int a, int b) { return std::abs(a - b) <= 6; }

// Whether any pixel in a box is near a color.
bool boxHolds(const Picture& p, double x0, double y0, double x1, double y1, int r, int g, int b) {
    for (double y = y0; y < y1; y += 1.0 / p.scale)
        for (double x = x0; x < x1; x += 1.0 / p.scale) {
            int pr, pg, pb;
            if (p.at(x, y, &pr, &pg, &pb) && near(pr, r) && near(pg, g) && near(pb, b)) return true;
        }
    return false;
}

// Whether any pixel in a box has `channel` (0 red, 1 green) well above the
// other two: an antialiased digit in that color over the gutter's blue.
bool boxHoldsHue(const Picture& p, double x0, double y0, double x1, double y1, int channel) {
    for (double y = y0; y < y1; y += 1.0 / p.scale)
        for (double x = x0; x < x1; x += 1.0 / p.scale) {
            int c[3];
            if (!p.at(x, y, &c[0], &c[1], &c[2])) continue;
            const int other = channel == 0 ? 1 : 0;
            if (c[channel] > c[other] + 0x60 && c[channel] > c[2] + 0x40) return true;
        }
    return false;
}

// A fold lane's source, as item 1.10's will be: regions on lines 4 and 8,
// the one on 8 closed.
struct FakeFolds : GutterFolds {
    bool hasClosedFolds() const override { return true; }
    bool startsRegion(size_t line) const override { return line == 4 || line == 8; }
    bool startsClosedFold(size_t line) const override { return line == 8; }
    void toggle(size_t line) override { toggled.push_back(line); }
    std::vector<size_t> toggled;
};
FakeFolds gFolds;

unsigned gDraws = 0;
int gBarClicks = 0;
int gBarIndex = -1;
Sel gSel{};

const std::string kA = numbered("alpha", 40);
const std::string kLong = numbered("long", 1500);

}  // namespace

MC_GUI_SUITE(gutter) {
    MCGuiStep("a file of 40 lines in a tab", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(kBase);
        MCGuiWriteFile(F("gut-a.txt"), kA);
        MCGuiWriteFile(F("gut-long.txt"), kLong);
        MCGuiWriteFile(F("gut-notes.md"), "# Notes\n\nOne.\n\nTwo.\n");
        MCGuiWriteFile(F("gut-cr.txt"), "first\rsecond\rthird\r");
        MCGuiOpenFileKept(F("gut-a.txt"));
    });
    waitFor("the gutter shows its numbers, each on its line", 10, [] {
        return ed()->currentPath() == F("gut-a.txt") && rowsRight();
    });
    MCGuiStep("one through as many as fit, in the gutter's lanes, three digits wide", [] {
        if (!gWhy.empty()) MCGuiNote("%s", gWhy.c_str());
        GCHECK(gut()->drawnRows().size() > 10);
        GCHECK_EQ_STR(gut()->drawnRows().front().label, "1");
        GCHECK(gut()->digits() == 3);
        gWidth3 = gtk_widget_get_width(gut()->widget());
        GCHECK(gWidth3 > gut()->laneWidth());
        // The gutter's coordinates are the view's left window's: a row's
        // top in the view is its line's top in the view.
        const Gutter::Row& r = gut()->drawnRows()[5];
        graphene_point_t in = GRAPHENE_POINT_INIT(0, (float)r.y), out;
        GCHECK(gtk_widget_compute_point(gut()->widget(), ed()->textView(), &in, &out));
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf(), &it, 5);
        GdkRectangle loc;
        gtk_text_view_get_iter_location(tv(), &it, &loc);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, loc.x, loc.y, &wx, &wy);
        GCHECK(std::fabs(out.y - wy) < 0.5);
        GCHECK(r.label == "6");
    });

    // ---- the caret's line, by a click and by keys
    MCGuiStep("a click in the text on line 5", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf(), &it, 4);
        GdkRectangle loc;
        gtk_text_view_get_iter_location(tv(), &it, &loc);
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, loc.x + 20,
                                              loc.y + loc.height / 2, &wx, &wy);
        MCGuiClick(ed()->textView(), wx, wy);
    });
    waitFor("line 5's number is in the active color, no other", 5, [] {
        return BufferLines::of(buf())->lineOf(caretOffset()) == 4 && rowsRight();
    });
    MCGuiStep("the colors are the theme's", [] {
        GCHECK(rowOf(4) && rowOf(4)->active && rowOf(3) && !rowOf(3)->active);
        GCHECK(!gdk_rgba_equal(&gut()->numberColor(), &gut()->activeNumberColor()));
    });
    MCGuiStep("Down twice", [] {
        MCGuiKey("down");
        MCGuiKey("down");
    });
    waitFor("the active color follows to line 7", 5, [] {
        return BufferLines::of(buf())->lineOf(caretOffset()) == 6 && rowsRight();
    });

    // ---- typing that adds and removes lines
    MCGuiStep("typing two new lines at the end of line 7", [] {
        MCGuiKey("end");
        MCGuiType("\nnew one\nnew two");
    });
    waitFor("42 lines and the empty one after them, numbered as they are now", 10, [] {
        return bufferText().find("alpha 7\nnew one\nnew two\nalpha 8") != std::string::npos &&
               BufferLines::of(buf())->lineCount() == 43 && rowsRight();
    });
    MCGuiStep("alpha 8 is line 10 now", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)lineStart(9));
        GtkTextIter end = it;
        gtk_text_iter_forward_to_line_end(&end);
        char* t = gtk_text_iter_get_text(&it, &end);
        GCHECK_EQ_STR(std::string(t), "alpha 8");
        g_free(t);
        GCHECK(rowOf(9) && rowOf(9)->label == "10" && rowOf(8)->active);
    });
    MCGuiStep("Backspace back over both new lines", [] {
        for (int i = 0; i < 16; i++) MCGuiKey("backspace");
    });
    waitFor("40 lines again, each numbered", 10, [] {
        return bufferText() == kA && BufferLines::of(buf())->lineCount() == 41 && rowsRight();
    });

    // ---- a lone '\r' and U+2029 end lines; U+2028 does not
    MCGuiStep("a paste holding a lone CR, U+2029 and U+2028, at the start of line 3", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf(), &it, 2);
        gtk_text_buffer_place_cursor(buf(), &it);
        gdk_clipboard_set_text(gtk_widget_get_clipboard(ed()->textView()),
                               "cr\rpara\xE2\x80\xA9line\xE2\x80\xA8sep\n");
        MCGuiKey("ctrl+v");
    });
    waitFor("three more lines, numbered as the Mac numbers them", 10, [] {
        return bufferText().find("cr\rpara\xE2\x80\xA9line\xE2\x80\xA8sep\nalpha 3") !=
                   std::string::npos &&
               BufferLines::of(buf())->lineCount() == 44 && rowsRight();
    });
    MCGuiStep("cr, para, and line+sep are lines 3, 4 and 5; sep has no number of its own", [] {
        GCHECK(rowOf(2) && rowOf(3) && rowOf(4) && rowOf(5));
        GCHECK(rowOf(4)->label == "5" && rowOf(5)->label == "6");
        // U+2028 breaks the display line but not the line: the display
        // line "sep" starts on is not numbered.
        GCHECK(rowOf(5)->y - rowOf(4)->y > rowOf(4)->height * 1.5);
        if (!gWhy.empty()) MCGuiNote("%s", gWhy.c_str());
    });
    MCGuiStep("Return typed right after the lone CR", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)lineStart(3));   // after "cr\r"
        gtk_text_buffer_place_cursor(buf(), &it);
        MCGuiKey("enter");
    });
    waitFor("CR LF is one break: still 44 lines, GTK's empty line unnumbered", 10, [] {
        return bufferText().find("cr\r\npara") != std::string::npos &&
               BufferLines::of(buf())->lineCount() == 44 && rowsRight();
    });
    MCGuiStep("Ctrl+Z twice takes the Return and the paste back", [] {
        MCGuiKey("ctrl+z");
        MCGuiKey("ctrl+z");
    });
    waitFor("the file's text, numbered", 10, [] {
        return bufferText() == kA && rowsRight();
    });
    MCGuiStep("a file with old Mac line ends", [] { MCGuiOpenFileKept(F("gut-cr.txt")); });
    waitFor("three lines and the empty one after the last CR", 10, [] {
        return ed()->currentPath() == F("gut-cr.txt") &&
               BufferLines::of(buf())->lineCount() == 4 && rowsRight() &&
               gut()->drawnRows().size() == 4;
    });

    // ---- each tab its own numbers and width
    MCGuiStep("a file of 1,500 lines in another tab", [] { MCGuiOpenFileKept(F("gut-long.txt")); });
    waitFor("four digits, and the gutter wider", 10, [] {
        return ed()->currentPath() == F("gut-long.txt") && gut()->digits() == 4 &&
               gtk_widget_get_width(gut()->widget()) > gWidth3 && rowsRight();
    });
    MCGuiStep("Ctrl+End, to the last line", [] {
        MCGuiFocusKeyWindow();
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+end");
    });
    MCGuiWaitQuiet("the view settles", 0.3, 5, [] {
        return (long)gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv())));
    });
    // GTK's first scroll to lines it has not laid out yet can land short of
    // the caret (HANDOFF.md, settleOnCaret); the second, over laid-out
    // lines, does not.
    MCGuiStep("and again", [] { MCGuiKey("ctrl+end"); });
    waitFor("the numbers at the end, 1,501 the last (empty) one, active", 10, [] {
        return rowsRight() && !gut()->drawnRows().empty() &&
               gut()->drawnRows().back().label == "1501" && gut()->drawnRows().back().active;
    });
    MCGuiStep("a wheel scroll up", [] { MCGuiScroll(ed()->textView(), -5); });
    MCGuiWaitQuiet("the view settles", 0.3, 5, [] {
        return (long)gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv())));
    });
    waitFor("the gutter follows the scroll", 5, [] { return rowsRight(); });
    MCGuiStep("Ctrl+Page Up, back to the CR file's tab", [] { MCGuiKey("ctrl+pageup"); });
    waitFor("its lines", 10, [] { return ed()->currentPath() == F("gut-cr.txt") && rowsRight(); });
    MCGuiStep("and again, to a's", [] { MCGuiKey("ctrl+pageup"); });
    waitFor("a's numbers, three digits wide again", 10, [] {
        return ed()->currentPath() == F("gut-a.txt") && gut()->digits() == 3 &&
               gtk_widget_get_width(gut()->widget()) == gWidth3 && rowsRight();
    });

    // ---- selecting lines in the gutter
    MCGuiStep("a click on line 6's number", [] { MCGuiClick(gut()->widget(), numberX(), rowY(5)); });
    waitFor("line 6 selected whole, the caret below it, the keyboard in the text", 5, [] {
        const Sel s = selection();
        return s.start == lineStart(5) && s.end == lineStart(6) && s.caret == lineStart(6) &&
               gtk_widget_has_focus(ed()->textView());
    });
    MCGuiStep("a drag from line 3 down to line 8", [] {
        MCGuiDrag(gut()->widget(), numberX(), rowY(2), numberX(), rowY(7));
    });
    waitFor("lines 3 to 8 selected, the caret below 8", 5, [] {
        const Sel s = selection();
        return s.start == lineStart(2) && s.end == lineStart(8) && s.caret == lineStart(8);
    });
    MCGuiStep("Shift+click on line 12", [] {
        MCGuiClickWith("shift", gut()->widget(), numberX(), rowY(11));
    });
    waitFor("lines 3 to 12 selected", 5, [] {
        const Sel s = selection();
        return s.start == lineStart(2) && s.end == lineStart(12);
    });
    MCGuiStep("Shift+click on line 1, above the selection", [] {
        MCGuiClickWith("shift", gut()->widget(), numberX(), rowY(0));
    });
    waitFor("lines 1 to 12, from the line the selection ended on", 5, [] {
        const Sel s = selection();
        return s.start == lineStart(0) && s.end == lineStart(12) && s.caret == lineStart(0);
    });
    MCGuiStep("a drag from line 10 up to line 6", [] {
        MCGuiDrag(gut()->widget(), numberX(), rowY(9), numberX(), rowY(5));
    });
    waitFor("lines 6 to 10 selected, the caret at line 6's start", 5, [] {
        const Sel s = selection();
        return s.start == lineStart(5) && s.end == lineStart(10) && s.caret == lineStart(5);
    });
    MCGuiStep("the caret's line is the active one", [] {
        GCHECK(rowsRight());
        GCHECK(rowOf(5) && rowOf(5)->active);
    });

    // ---- the settings
    MCGuiStep("editor.line-numbers = relative, the caret on line 10", [] {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line(buf(), &it, 9);
        gtk_text_buffer_place_cursor(buf(), &it);
        MCGuiSetSettings(std::string(kBase) + "editor.line-numbers = relative\n");
    });
    waitFor("distances from line 10, which shows its own number", 5, [] {
        return gut()->mode() == Gutter::Mode::Relative && rowsRight(true);
    });
    MCGuiStep("line 7 is 3 away, line 12 is 2 away", [] {
        GCHECK(rowOf(9) && rowOf(9)->label == "10");
        GCHECK(rowOf(6) && rowOf(6)->label == "3");
        GCHECK(rowOf(11) && rowOf(11)->label == "2");
    });
    MCGuiStep("Up", [] { MCGuiKey("up"); });
    waitFor("the distances follow the caret", 5, [] {
        return BufferLines::of(buf())->lineOf(caretOffset()) == 8 && rowsRight(true) &&
               rowOf(9)->label == "1";
    });
    MCGuiStep("editor.line-numbers = off", [] {
        gDraws = gut()->drawCount();
        MCGuiSetSettings(std::string(kBase) + "editor.line-numbers = off\n");
    });
    waitFor("no gutter, the text at the window's edge", 5, [] {
        int wx = 0, wy = 0;
        gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, 0, 0, &wx, &wy);
        return !gut()->shown() && wx <= 1;
    });
    MCGuiStep("colors of its own, and on again", [] {
        MCGuiSetSettings(std::string(kBase) +
                         "editor.gutter-background = #204060\n"
                         "editor.line-number-color = #FF2020\n"
                         "editor.line-number-active-color = #20FF20\n");
    });
    waitFor("shown, in those colors", 5, [] {
        const GdkRGBA& bg = gut()->backgroundColor();
        return gut()->shown() && rowsRight() && std::fabs(bg.red - 0x20 / 255.0) < 0.01 &&
               std::fabs(gut()->numberColor().red - 1.0) < 0.01;
    });
    MCGuiStep("as pixels: the background, a number in red, the caret's in green", [] {
        const Picture p = picture(gut()->widget());
        GCHECK(p.width > 0);
        int r = 0, g = 0, b = 0;
        GCHECK(p.at(1, 1, &r, &g, &b) && near(r, 0x20) && near(g, 0x40) && near(b, 0x60));
        const Gutter::Row* plain = rowOf(2);
        const Gutter::Row* caret = rowOf(8);
        GCHECK(plain && caret && caret->active && !plain->active);
        if (plain && caret) {
            const double x1 = gut()->numbersRight();
            GCHECK(boxHoldsHue(p, Gutter::kDotLane, plain->y, x1, plain->y + plain->height, 0));
            GCHECK(!boxHoldsHue(p, Gutter::kDotLane, plain->y, x1, plain->y + plain->height, 1));
            GCHECK(boxHoldsHue(p, Gutter::kDotLane, caret->y, x1, caret->y + caret->height, 1));
            GCHECK(!boxHoldsHue(p, Gutter::kDotLane, caret->y, x1, caret->y + caret->height, 0));
        }
        MCGuiSnapshot(gut()->widget(), "gutter-colors");
    });
    MCGuiStep("editor.opacity = 0.5", [] {
        MCGuiSetSettings(std::string(kBase) + "editor.opacity = 0.5\n");
    });
    waitFor("the gutter's background half see-through, painted once", 5, [] {
        if (!gut()->shown() || std::fabs(gut()->backgroundColor().alpha - 0.5) > 0.01) return false;
        // The view as drawn, beside the first line: nothing under the
        // gutter's own paint (the view's border window is clear).
        const Picture p = picture(ed()->textView());
        if (p.width <= 0 || gut()->drawnRows().empty()) return false;
        const int px = (int)(2 * p.scale), py = (int)((gut()->drawnRows()[0].y + 2) * p.scale);
        const int alpha = p.bytes[(size_t)(py * p.width + px) * 4 + 3];
        return std::abs(alpha - 128) <= 16;
    });
    MCGuiStep("the theme's colors again", [] { MCGuiSetSettings(kBase); });
    waitFor("as before", 5, [] {
        return gut()->shown() && rowsRight() && gut()->numberColor().red < 0.9;
    });

    // ---- not in a preview, back with the source
    MCGuiStep("a Markdown file, which opens in its preview", [] {
        MCGuiOpenFileKept(F("gut-notes.md"));
    });
    waitFor("no gutter beside the page", 10, [] {
        return ed()->currentPath() == F("gut-notes.md") && ed()->inPreview() && !gut()->shown();
    });
    MCGuiStep("Ctrl+Shift+V, the source", [] {
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+shift+v");
    });
    waitFor("the gutter beside it, numbered", 10, [] {
        return !ed()->inPreview() && rowsRight() && gut()->drawnRows().size() == 6;
    });
    MCGuiStep("Ctrl+Shift+V, the page again", [] { MCGuiKey("ctrl+shift+v"); });
    waitFor("no gutter again", 10, [] { return ed()->inPreview() && !gut()->shown(); });
    MCGuiStep("back to a", [] { MCGuiOpenFileKept(F("gut-a.txt")); });
    waitFor("a on show", 10, [] { return ed()->currentPath() == F("gut-a.txt") && rowsRight(); });

    // ---- the dots: the language server session's squiggle tags
    MCGuiStep("an error on line 7, a warning on line 9, an error over lines 11 and 12", [] {
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_line_offset(buf(), &a, 6, 2);
        gtk_text_buffer_get_iter_at_line_offset(buf(), &b, 6, 5);
        gtk_text_buffer_apply_tag_by_name(buf(), "lsp-error", &a, &b);
        gtk_text_buffer_get_iter_at_line_offset(buf(), &a, 8, 0);
        gtk_text_buffer_get_iter_at_line_offset(buf(), &b, 8, 3);
        gtk_text_buffer_apply_tag_by_name(buf(), "lsp-warning", &a, &b);
        gtk_text_buffer_get_iter_at_line_offset(buf(), &a, 10, 4);
        gtk_text_buffer_get_iter_at_line_offset(buf(), &b, 11, 3);
        gtk_text_buffer_apply_tag_by_name(buf(), "lsp-error", &a, &b);
    });
    waitFor("dots on lines 7, 9 and 11 only, by severity", 5, [] {
        if (!rowsRight()) return false;
        for (const Gutter::Row& r : gut()->drawnRows()) {
            const int want = r.line == 6 || r.line == 10 ? 1 : r.line == 8 ? 2 : 0;
            if (r.dot != want) return false;
        }
        return true;
    });
    MCGuiStep("an error's dot in the squiggle's color", [] {
        const Picture p = picture(gut()->widget());
        GCHECK(boxHolds(p, 0, rowOf(6)->y, Gutter::kDotLane, rowOf(6)->y + rowOf(6)->height,
                        0xF1, 0x4C, 0x4C));
        GCHECK(!boxHolds(p, 0, rowOf(7)->y, Gutter::kDotLane, rowOf(7)->y + rowOf(7)->height,
                         0xF1, 0x4C, 0x4C));
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buf(), &a, &b);
        gtk_text_buffer_remove_tag_by_name(buf(), "lsp-error", &a, &b);
        gtk_text_buffer_remove_tag_by_name(buf(), "lsp-warning", &a, &b);
    });
    waitFor("no dots once the tags go", 5, [] {
        if (!rowsRight()) return false;
        for (const Gutter::Row& r : gut()->drawnRows())
            if (r.dot) return false;
        return true;
    });

    // ---- the change lane (item 2.10 will set the bars from git)
    MCGuiStep("change bars on lines 3 and 4 (added) and above line 11 (deleted)", [] {
        QuickDiff::Bar added;
        added.kind = QuickDiff::Kind::Added;
        added.line = 2;
        added.lines = 2;
        QuickDiff::Bar deleted;
        deleted.kind = QuickDiff::Kind::Deleted;
        deleted.line = 10;
        BufferLines::of(buf())->setChangeBars({added, deleted});
        gut()->onChangeBar = [](int index, const graphene_rect_t&) {
            gBarClicks++;
            gBarIndex = index;
        };
        gut()->setChangeLane(true);
    });
    waitFor("the lane widens the gutter, and both bars are drawn by their lines", 5, [] {
        const graphene_rect_t r = gut()->rectOfChangeBar(0);
        return gtk_widget_get_width(gut()->widget()) == gWidth3 + Gutter::kChangeLane &&
               gut()->changeBarsDrawn() == 2 && rowsRight() && rowOf(2) &&
               std::fabs(r.origin.y - rowOf(2)->y) < 0.5 &&
               r.size.height >= rowOf(2)->height * 2 - 0.5;
    });
    MCGuiStep("a new line typed at the top", [] {
        gtk_widget_grab_focus(ed()->textView());
        MCGuiKey("ctrl+home");
        MCGuiType("top\n");
    });
    waitFor("the bars move down with their lines", 5, [] {
        const auto& bars = BufferLines::of(buf())->changeBars();
        return bars.size() == 2 && bars[0].line == 3 && bars[1].line == 11 && rowsRight() &&
               std::fabs(gut()->rectOfChangeBar(0).origin.y - rowOf(3)->y) < 0.5;
    });
    MCGuiStep("a click on the added bar", [] {
        gSel = selection();
        // In the lane's left part, 13 px from the editor's edge: the window's
        // divider once took presses 14 px into the editor (the theme's 8 px
        // of padding around its handle and GtkPaned's 6), and takes none
        // there now (WindowLayout, ThemeCss.cpp).
        MCGuiClick(gut()->widget(), Gutter::kDotLane + 1, rowY(4));
    });
    waitFor("tells its owner, and selects nothing", 5, [] { return gBarClicks == 1; });
    MCGuiStep("bar 0, the selection as it was", [] {
        GCHECK(gBarIndex == 0);
        const Sel s = selection();
        GCHECK(s.start == gSel.start && s.end == gSel.end);
        gut()->setChangeLane(false);
        gut()->onChangeBar = nullptr;
        BufferLines::of(buf())->setChangeBars({});
        // The typed line, selected and deleted.
        MCGuiKey("ctrl+home");
        MCGuiKey("shift+down");
        MCGuiKey("delete");
    });
    waitFor("the lane gone, the text as it was", 5, [] {
        return bufferText() == kA && gtk_widget_get_width(gut()->widget()) == gWidth3 && rowsRight();
    });

    // ---- the fold lane (item 1.10 will give the regions)
    MCGuiStep("a fold source: a region on line 5, a closed fold on line 9", [] {
        gFolds.toggled.clear();
        MCGuiMoveTo(ed()->textView());
        BufferLines::of(buf())->setFolds(&gFolds);
    });
    waitFor("the closed fold's chevron shows without the pointer", 5, [] {
        return rowsRight() && rowOf(8) && rowOf(8)->chevron == 2 && rowOf(4) && rowOf(4)->chevron == 0;
    });
    MCGuiStep("the pointer over the gutter", [] { MCGuiMoveTo(gut()->widget(), numberX(), rowY(2)); });
    waitFor("the open region's chevron shows too", 5, [] {
        return rowsRight() && rowOf(4)->chevron == 1 && rowOf(8)->chevron == 2 &&
               rowOf(3)->chevron == 0;
    });
    MCGuiStep("a click on line 5's chevron", [] {
        gSel = selection();
        MCGuiClick(gut()->widget(), gtk_widget_get_width(gut()->widget()) - Gutter::kFoldLane / 2,
                   rowY(4));
    });
    waitFor("toggles that region, selecting nothing", 5, [] { return gFolds.toggled.size() == 1; });
    MCGuiStep("line 5's", [] {
        GCHECK(gFolds.toggled[0] == 4);
        const Sel s = selection();
        GCHECK(s.start == gSel.start && s.end == gSel.end);
        BufferLines::of(buf())->setFolds(nullptr);
        MCGuiMoveTo(ed()->textView());
    });
    waitFor("no chevrons without a source", 5, [] {
        if (!rowsRight()) return false;
        for (const Gutter::Row& r : gut()->drawnRows())
            if (r.chevron) return false;
        return true;
    });
    MCGuiStep("the tabs close", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(kBase);
    });
    waitFor("no gutter beside the welcome text", 5, [] { return !gut()->shown(); });
}
