// BracketPairsSuite.cpp — `./build/minicode-guitest bracket_pairs`:
// bracket-pair colors in the editor (editor.bracket-pair-colorization,
// linux/PARITY.md item 1.7, BracketColors.h), the counterpart of the Mac's
// BracketPairsSuite. Three depths take Dark+'s three bracket colors, as the
// tags on the characters and as pixels; a closer with nothing open takes
// the unexpected color; brackets in strings and comments keep the lexer's.
// Lines off screen are not painted until they scroll into view (by the
// wheel, Ctrl+End, or a jump). A ( typed at the top through XTest recolors
// the brackets on screen at once by painting only those lines (counted),
// leaves the lines far below alone until they are seen, and costs little
// CPU; Ctrl+Z puts the colors back. Off by its key, and back; none in a
// plain text file. Opening a 30,000-line file paints only the lines on
// screen, at about the CPU of an open without bracket colors, and typing in
// the middle of a 100,000-line file costs about what it does without them.
#include "Harness.h"

#include "BracketColors.h"
#include "Document.h"
#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

Editor* ed() { return MCGuiApp().editor; }
GtkTextView* tv() { return GTK_TEXT_VIEW(ed()->textView()); }
GtkTextBuffer* buf() { return ed()->buffer(); }
BracketColors* colors() { return ed()->active() ? ed()->active()->bracketColors() : nullptr; }
std::string F(const std::string& name) { return MCGuiProjectFile(name); }

const std::string kGold = "#FFD700", kOrchid = "#DA70D6", kBlue = "#179FFF",
                  kUnexpected = "#FF1212", kString = "#CE9178", kComment = "#6A9955",
                  kEscape = "#D7BA7D", kSelf = "#569CD6";
const char* kBase = "lsp.enabled = false\neditor.auto-close = off\n";
const std::string kOff = std::string(kBase) + "editor.bracket-pair-colorization = off\n";

std::string text() {
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf(), &a, &b);
    char* t = gtk_text_buffer_get_text(buf(), &a, &b, TRUE);
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// Where the n-th (0-based) `what` starts, in characters (the texts here are
// ASCII, so bytes).
size_t nth(const std::string& what, int n = 0) {
    const std::string s = text();
    size_t from = 0, at = std::string::npos;
    for (int k = 0; k <= n; k++) {
        at = s.find(what, from);
        if (at == std::string::npos) return at;
        from = at + what.size();
    }
    return at;
}

size_t lineStart(int line) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(buf(), &it, line);
    return (size_t)gtk_text_iter_get_offset(&it);
}

// The color the character at `i` is drawn in, from its tags: the one of
// highest priority that sets a foreground, as #RRGGBB, "plain" with none.
std::string colorAt(size_t i) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)i);
    GSList* tags = gtk_text_iter_get_tags(&it);   // ascending priority
    std::string out = "plain";
    for (GSList* l = tags; l; l = l->next) {
        gboolean set = FALSE;
        GdkRGBA* c = nullptr;
        g_object_get(l->data, "foreground-set", &set, "foreground-rgba", &c, NULL);
        if (set && c) {
            char hex[8];
            g_snprintf(hex, sizeof hex, "#%02X%02X%02X", (int)std::lround(c->red * 255),
                       (int)std::lround(c->green * 255), (int)std::lround(c->blue * 255));
            out = hex;
        }
        if (c) gdk_rgba_free(c);
    }
    g_slist_free(tags);
    return out == "#D4D4D4" ? "plain" : out;
}

bool hasBracketTag(size_t i) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)i);
    GtkTextTagTable* table = gtk_text_buffer_get_tag_table(buf());
    for (int k = 0; k < 4; k++)
        if (gtk_text_iter_has_tag(&it, BracketColors::tag(table, k))) return true;
    return false;
}

bool onScreen(size_t offset) {
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GdkRectangle r, vis;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    gtk_text_view_get_visible_rect(tv(), &vis);
    return r.y >= vis.y && r.y + r.height <= vis.y + vis.height;
}

// Scrolls `offset` on screen (asked again while GTK's height guesses for
// lines not laid out yet leave it off), true once it is.
bool bringOnScreen(size_t offset) {
    if (onScreen(offset)) return true;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)offset);
    GtkTextMark* m = gtk_text_buffer_get_mark(buf(), "bracket-pairs-target");
    if (!m) m = gtk_text_buffer_create_mark(buf(), "bracket-pairs-target", &it, TRUE);
    gtk_text_buffer_move_mark(buf(), m, &it);
    gtk_text_view_scroll_to_mark(tv(), m, 0.0, TRUE, 0.0, 0.3);
    return false;
}

void visibleRange(int* top, int* bottom) {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv(), &vis);
    GtkTextIter a, b;
    gtk_text_view_get_line_at_y(tv(), &a, vis.y, nullptr);
    gtk_text_view_get_line_at_y(tv(), &b, vis.y + vis.height - 1, nullptr);
    *top = gtk_text_iter_get_line(&a);
    *bottom = gtk_text_iter_get_line(&b);
}

size_t visibleLines() {
    int a, b;
    visibleRange(&a, &b);
    return (size_t)(b - a + 1);
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

// Which of `candidates` (0xRRGGBB) the character at i is drawn in, from the
// view's pixels, as the Mac's suite reads them: the pixel in its box
// farthest from the background, read as background + a * (color -
// background) for each candidate, the closest fit winning (antialiasing
// blends a thin glyph's edges into the background). -1 when nothing is
// drawn there.
int drawnIn(size_t i, const std::vector<uint32_t>& candidates) {
    const Picture p = picture(ed()->textView());
    if (!p.width) return -1;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(buf(), &it, (int)i);
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv(), &it, &r);
    int wx = 0, wy = 0;
    gtk_text_view_buffer_to_window_coords(tv(), GTK_TEXT_WINDOW_WIDGET, r.x, r.y, &wx, &wy);
    int br, bg, bb;   // the background, in the empty corner
    if (!p.at(p.width / p.scale - 6, p.height / p.scale - 6, &br, &bg, &bb)) return -1;
    double best = 0;
    int ir = 0, ig = 0, ib = 0;
    for (double y = wy; y < wy + r.height; y += 0.5 / p.scale)
        for (double x = wx; x < wx + r.width; x += 0.5 / p.scale) {
            int cr, cg, cb;
            if (!p.at(x, y, &cr, &cg, &cb)) continue;
            const double d = std::abs(cr - br) + std::abs(cg - bg) + std::abs(cb - bb);
            if (d > best) {
                best = d;
                ir = cr;
                ig = cg;
                ib = cb;
            }
        }
    if (best < 0.2 * 255) return -1;
    int pick = -1;
    double pickErr = 1e18;
    for (size_t k = 0; k < candidates.size(); k++) {
        const uint32_t h = candidates[k];
        const double dr = (double)((h >> 16) & 0xFF) - br, dg = (double)((h >> 8) & 0xFF) - bg,
                     db = (double)(h & 0xFF) - bb;
        const double pr = ir - br, pg = ig - bg, pb = ib - bb;
        const double len = dr * dr + dg * dg + db * db;
        if (len <= 0) continue;
        const double a = (pr * dr + pg * dg + pb * db) / len;
        const double er = pr - a * dr, eg = pg - a * dg, eb = pb - a * db;
        const double err = er * er + eg * eg + eb * eb + (a < 0.3 ? 1e9 : 0);
        if (err < pickErr) {
            pickErr = err;
            pick = (int)k;
        }
    }
    return pick;
}

const int kLines = 3000;
std::string gOriginal;
size_t gPainted0 = 0, gFar = 0, gLast = 0;
double gCpu0 = 0, gColorsCpu0 = 0, gTypeMs = 0, gColorsMs = 0;

std::string depthsFile() {
    std::string body =
        "result = outer(inner[key{0}])\n"
        "s = \"(x)\\n\"  # (comment)\n"
        "def area(self):\n";
    for (int k = 3; k < kLines; k++) body += "value_" + std::to_string(k) + " = call(item)\n";
    body += "oops = 1)\n";   // the last line: a closer with nothing open
    return body;
}

// Every value_k line on screen (past the first few) has its call( at depth
// 0, and the view has moved off the top.
bool screenPainted() {
    int top, bottom;
    visibleRange(&top, &bottom);
    if (top < 5) return false;
    int seen = 0;
    for (int line = top; line <= bottom && line < kLines; line++) {
        if (line < 3) continue;
        const size_t paren = lineStart(line) + ("value_" + std::to_string(line) + " = call").size();
        if (colorAt(paren) != kGold) return false;
        seen++;
    }
    return seen > 0;
}

// Frames the view draws from now on: counted by a tick callback, which
// framesDrawn takes off again once two have passed.
int gFrames = 0;
guint gTick = 0;
void countFrames() {
    gFrames = 0;
    if (gTick) return;
    gTick = gtk_widget_add_tick_callback(
        ed()->textView(),
        [](GtkWidget*, GdkFrameClock*, gpointer) -> gboolean {
            gFrames++;
            return G_SOURCE_CONTINUE;
        },
        nullptr, nullptr);
}
bool framesDrawn() {
    if (gFrames < 2) return false;
    if (gTick) gtk_widget_remove_tick_callback(ed()->textView(), gTick);
    gTick = 0;
    return true;
}

// ---- the costs at the end
double gOpenMs[2] = {0, 0};
size_t gOpenLines = 0;
double gKeysMs[2] = {0, 0};
size_t gBigChars = 0;
const std::string kTyped = "x = f(a[1]) ";   // no Return: AutoIndent stays out of it

}  // namespace

MC_GUI_SUITE(bracket_pairs) {
    MCGuiStep("a Python file with nested brackets, 3,000 lines", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings(kBase);
        gOriginal = depthsFile();
        MCGuiWriteFile(F("depths.py"), gOriginal);
        MCGuiOpenFileKept(F("depths.py"));
        gtk_widget_grab_focus(ed()->textView());
    });
    MCGuiWait("its brackets colored", 10, [] {
        return ed()->currentPath() == F("depths.py") && colors() && colors()->on() &&
               colorAt(nth("(")) == kGold;
    });
    MCGuiStep("three depths in three colors; strings, comments, escapes and self their own", [] {
        GCHECK_EQ_STR(colorAt(nth("(")), kGold);      // outer(
        GCHECK_EQ_STR(colorAt(nth("[")), kOrchid);    // inner[
        GCHECK_EQ_STR(colorAt(nth("{")), kBlue);      // key{
        GCHECK_EQ_STR(colorAt(nth("}")), kBlue);
        GCHECK_EQ_STR(colorAt(nth("]")), kOrchid);
        GCHECK_EQ_STR(colorAt(nth(")")), kGold);
        GCHECK_EQ_STR(colorAt(nth("\"(x)") + 1), kString);
        GCHECK_EQ_STR(colorAt(nth("(comment)")), kComment);
        GCHECK(!hasBracketTag(nth("\"(x)") + 1) && !hasBracketTag(nth("(comment)")));
        GCHECK_EQ_STR(colorAt(nth("\\n")), kEscape);
        GCHECK_EQ_STR(colorAt(nth("self")), kSelf);
        // Far down, off screen, not painted yet: only the lines on screen
        // are, as they come into view.
        gFar = nth("value_2900 = call(") + 17;
        GCHECK(!onScreen(gFar));
        GCHECK(!hasBracketTag(gFar));
        GCHECK(colors()->linesPainted() < 200);
    });
    MCGuiWait("and they are drawn so", 5, [] {
        const std::vector<uint32_t> three = {0xFFD700, 0xDA70D6, 0x179FFF, 0xD4D4D4};
        const int a = drawnIn(14, three), b = drawnIn(20, three), d = drawnIn(24, three);
        if (a == 0 && b == 1 && d == 2) {
            MCGuiSnapshot(ed()->textView(), "bracket-pairs");
            return true;
        }
        return false;
    });
    MCGuiStep("the wheel, ten clicks down", [] { MCGuiScroll(ed()->textView(), 10); });
    MCGuiWait("the lines it brings on screen are painted", 5, screenPainted);
    MCGuiWait("a line far below, scrolled into view: each line's call at depth 0", 10, [] {
        return bringOnScreen(gFar) && colorAt(gFar) == kGold;
    });
    MCGuiStep("Ctrl+End", [] {
        gLast = nth("oops = 1)") + 8;
        GCHECK(!hasBracketTag(gLast));   // not seen yet
        MCGuiKey("ctrl+end");
    });
    MCGuiWait("a closer with nothing open: the unexpected color", 5, [] {
        return colorAt(gLast) == kUnexpected;
    });

    MCGuiStep("Ctrl+Home", [] { MCGuiKey("ctrl+home"); });
    MCGuiWait("at the top", 5, [] { return onScreen(0) && !onScreen(gFar); });
    MCGuiStep("type ( there", [] {
        gPainted0 = colors()->linesPainted();
        gCpu0 = MCGuiThreadCpuMs();
        gColorsCpu0 = BracketColors::cpuMs();
        MCGuiType("(");
    });
    MCGuiWait("the new one gold, the brackets on screen one deeper", 5, [] {
        return (size_t)gtk_text_buffer_get_char_count(buf()) == gOriginal.size() + 1 &&
               colorAt(0) == kGold && colorAt(15) == kOrchid;
    });
    MCGuiStep("only the lines on screen painted again, the rest as they were", [] {
        gTypeMs = MCGuiThreadCpuMs() - gCpu0;
        gColorsMs = BracketColors::cpuMs() - gColorsCpu0;
        GCHECK_EQ_STR(colorAt(nth("(", 1)), kOrchid);   // outer(
        GCHECK_EQ_STR(colorAt(nth("[")), kBlue);
        GCHECK_EQ_STR(colorAt(nth("{")), kGold);         // the cycle comes round
        // The last line on screen, one depth deeper now.
        int top, bottom;
        visibleRange(&top, &bottom);
        const int last = std::min(bottom, kLines - 1);
        const size_t paren = lineStart(last) + ("value_" + std::to_string(last) + " = call").size();
        GCHECK(last > 3);
        GCHECK_EQ_STR(colorAt(paren), kOrchid);
        const size_t lines = colors()->linesPainted() - gPainted0;
        MCGuiNote("the ( repainted %zu lines for their brackets (%zu on screen); the main "
                  "thread %.2f ms of CPU from the key to here, the bracket colors %.2f of it",
                  lines, visibleLines(), gTypeMs, gColorsMs);
        GCHECK(lines > 0 && lines <= visibleLines() + 2);
        // Far below, the old color until it is seen; the last closer, which
        // now closes the new one, still unexpected.
        gFar = nth("value_2900 = call(") + 17;
        gLast = nth("oops = 1)") + 8;
        GCHECK_EQ_STR(colorAt(gFar), kGold);
        GCHECK_EQ_STR(colorAt(gLast), kUnexpected);
        GCHECK_TIME(gColorsMs, 20, "the bracket colors' work for a ( at the top of 3,000 lines");
    });
    MCGuiWait("scrolled into view, its new depth's color", 10, [] {
        return bringOnScreen(gFar) && colorAt(gFar) == kOrchid;
    });
    MCGuiWait("at the end, the last closer closes the new (", 10, [] {
        return bringOnScreen(gLast) && colorAt(gLast) == kGold;
    });

    MCGuiWait("back at the top", 10, [] { return bringOnScreen(0); });
    MCGuiStep("Ctrl+Z", [] { MCGuiKey("ctrl+z"); });
    MCGuiWait("the colors back", 5, [] {
        return text() == gOriginal && colorAt(14) == kGold && colorAt(20) == kOrchid;
    });
    MCGuiStep("where the ends are now", [] {
        gLast = nth("oops = 1)") + 8;
        gFar = nth("value_2900 = call(") + 17;
    });
    MCGuiWait("at the end too, once seen", 10, [] {
        return bringOnScreen(gLast) && colorAt(gLast) == kUnexpected;
    });
    MCGuiWait("and far below, its old color", 10, [] {
        return bringOnScreen(gFar) && colorAt(gFar) == kGold;
    });

    MCGuiWait("the top", 10, [] { return bringOnScreen(0); });
    MCGuiStep("off by its key", [] { MCGuiSetSettings(kOff); });
    MCGuiWait("the brackets are plain text, everywhere", 5, [] {
        return !colors()->on() && colorAt(14) == "plain" && colorAt(20) == "plain" &&
               !hasBracketTag(gFar) && !hasBracketTag(gLast);
    });
    MCGuiStep("the lexer's own stay", [] {
        GCHECK_EQ_STR(colorAt(nth("\\n")), kEscape);
        GCHECK_EQ_STR(colorAt(nth("\"(x)") + 1), kString);
        MCGuiSetSettings(kBase);
    });
    MCGuiWait("on again", 5, [] { return colors()->on() && colorAt(20) == kOrchid; });
    MCGuiStep("a plain text file", [] {
        MCGuiWriteFile(F("depths.txt"), "f(a[b{c}])\n");
        MCGuiOpenFileKept(F("depths.txt"));
    });
    MCGuiWait("has none: no grammar", 5, [] {
        return ed()->currentPath() == F("depths.txt") && !colors()->on() &&
               colorAt(1) == "plain" && !hasBracketTag(3);
    });
    // A tab closed before the view has drawn it aborts in GTK's text layout
    // on main as of November 2026 (reported; not this item's), so the view
    // draws before each close here.
    MCGuiStep("drawn", countFrames);
    MCGuiWait("the view has drawn it", 5, framesDrawn);

    // ---- what they cost
    MCGuiStep("a 30,000-line file opened without them", [] {
        std::string t;
        for (int k = 0; k < 30000; k++) t += "    x = g(h[1], {'k': (2)})\n";
        MCGuiWriteFile(F("wide.py"), t);
        ed()->closeAllTabs();
        MCGuiSetSettings(kOff);
        const double t0 = MCGuiThreadCpuMs();
        MCGuiOpenFileKept(F("wide.py"));
        gOpenMs[0] = MCGuiThreadCpuMs() - t0;
        GCHECK(colors() && !colors()->on());
        countFrames();
    });
    MCGuiWait("the view has drawn it", 5, framesDrawn);
    MCGuiStep("closed", [] {
        ed()->closeAllTabs();
        countFrames();
    });
    MCGuiWait("the view has drawn that too", 5, framesDrawn);
    MCGuiStep("and with them", [] {
        MCGuiSetSettings(kBase);
        gColorsCpu0 = BracketColors::cpuMs();
        const double t0 = MCGuiThreadCpuMs();
        MCGuiOpenFileKept(F("wide.py"));
        gOpenMs[1] = MCGuiThreadCpuMs() - t0;
        GCHECK(colors() && colors()->on());
    });
    MCGuiWait("the lines on screen painted", 5, [] {
        return colors() && colors()->linesPainted() > 0 && colorAt(9) == kGold &&
               colorAt(nth("(2")) == kBlue;
    });
    MCGuiStep("only those, at about the cost of an open without them", [] {
        // The painting runs once the view shows the file: its CPU joins the open's.
        gOpenMs[1] += BracketColors::cpuMs() - gColorsCpu0;
        gOpenLines = colors()->linesPainted();
        MCGuiNote("opening 30,000 lines: %.0f ms of CPU with bracket colors (%zu lines painted "
                  "for them), %.0f without",
                  gOpenMs[1], gOpenLines, gOpenMs[0]);
        GCHECK(gOpenLines > 0 && gOpenLines < 200);
        GCHECK_TIME(gOpenMs[1], 1.3 * gOpenMs[0] + 100, "opening with bracket colors");
        countFrames();
    });
    MCGuiWait("the view has drawn it", 5, framesDrawn);
    MCGuiStep("closed", [] { ed()->closeAllTabs(); });

    MCGuiStep("100,000 lines", [] {
        std::string big;
        for (int k = 0; k < 100000; k++)
            big += k % 7 == 0 ? "def f(a, b):  # (\n" : "    x = g(h[1], {'k': (2)})\n";
        gBigChars = big.size();
        MCGuiWriteFile(F("big.py"), big);
        MCGuiSetSettings(kBase);
        MCGuiOpenFileKept(F("big.py"));
        gtk_widget_grab_focus(ed()->textView());
    });
    for (int on = 1; on >= 0; on--) {
        MCGuiStep(on ? "the caret at line 50,000, with bracket colors" : "again without them", [on] {
            if (!on) MCGuiSetSettings(kOff);
            GCHECK(colors()->on() == (bool)on);
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(buf(), &it, 50000);
            gtk_text_buffer_place_cursor(buf(), &it);
        });
        MCGuiWait("on screen", 10, [] { return bringOnScreen(lineStart(50000)); });
        MCGuiStep(on ? "type a call with brackets" : "type it again", [] {
            gCpu0 = MCGuiThreadCpuMs();
            MCGuiType(kTyped);
        });
        MCGuiWait("it arrives", 10, [on] {
            const bool there = (size_t)gtk_text_buffer_get_char_count(buf()) ==
                               gBigChars + (size_t)(2 - on) * kTyped.size();
            if (there) gKeysMs[on] = MCGuiThreadCpuMs() - gCpu0;
            return there;
        });
        if (on)
            MCGuiWait("colored", 5, [] { return colorAt(lineStart(50000) + 5) == kGold; });
    }
    MCGuiStep("about the same CPU a key", [] {
        const double keys = (double)kTyped.size();
        MCGuiNote("typing in 100,000 lines: %.2f ms of main-thread CPU a key with bracket "
                  "colors, %.2f without",
                  gKeysMs[1] / keys, gKeysMs[0] / keys);
        GCHECK_TIME(gKeysMs[1], 1.3 * gKeysMs[0] + 5 * keys, "typing with bracket colors");
        countFrames();
    });
    MCGuiWait("the view has drawn it", 5, framesDrawn);
    MCGuiStep("put back", [] {
        ed()->closeAllTabs();
        MCGuiSetSettings("lsp.enabled = false\n");
    });
}
