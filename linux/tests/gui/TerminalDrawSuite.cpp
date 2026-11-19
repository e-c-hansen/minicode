// TerminalDrawSuite.cpp — terminal_draw: the terminal's cursor as drawn, read
// off the VTE widget's pixels (linux/HANDOFF.md item 108). VTE's
// vte_terminal_set_colors unsets the cursor's color, and the port set it
// once before that call, so VTE drew the cursor in the cell's background,
// on every theme: no cursor showed. A real bash, started as the app starts
// it, at a prompt cleared to the top row:
//   - on every built-in theme, the focused block cursor on an empty cell is
//     the terminal's text color (Settings::terminalCursor), filling the
//     cell, which stands out from the background;
//   - on a dark theme and a light one, the character under the block shows
//     in the background's color, and with the editor focused the cursor is
//     an outline in the cursor's color around a cell of background.
#include "Harness.h"
#include "AppSeam.h"
#include "Editor.h"
#include "EnvLock.h"
#include "Settings.h"
#include "Terminal.h"
#include "TerminalDock.h"
#include "WindowLayout.h"

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

TerminalDock* dock() { return MCGuiApp().termDock; }
bool ready(Terminal* t) { return t && t->shellPid() > 0 && t->shellState().known && t->shellState().atPrompt; }
GtkWidget* focusWidget() { return gtk_root_get_focus(GTK_ROOT(MCGuiApp().window)); }
bool terminalHasKeyboard() {
    Terminal* t = MCGuiApp().terminal;
    return t && t->owns(focusWidget()) && gtk_window_is_active(MCGuiApp().window);
}

const char* kBase = "lsp.enabled = false\n";
std::string gHome, gOldShell;
bool gHadShell = false;
Terminal* gT = nullptr;
std::string gTheme;

Settings themed() { return Settings::parse("theme = " + gTheme + "\n" + kBase); }

// The VTE widget as drawn, once, and its pixels by the widget's points.
struct Shot {
    int w = 0, h = 0, tw = 0, th = 0;
    std::vector<guchar> px;   // B8G8R8A8, premultiplied (opaque here)
    bool at(int x, int y, int rgb[3]) const {
        if (x < 0 || y < 0 || x >= w || y >= h || px.empty()) return false;
        const guchar* p = &px[((size_t)y * th / h * tw + (size_t)x * tw / w) * 4];
        rgb[0] = p[2];
        rgb[1] = p[1];
        rgb[2] = p[0];
        return true;
    }
};

Shot shoot(GtkWidget* widget) {
    Shot s;
    s.w = gtk_widget_get_width(widget);
    s.h = gtk_widget_get_height(widget);
    GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(widget));
    if (s.w <= 0 || s.h <= 0 || !renderer) return s;
    GdkPaintable* paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snap, s.w, s.h);
    if (GskRenderNode* node = gtk_snapshot_free_to_node(snap)) {
        if (GdkTexture* texture = gsk_renderer_render_texture(renderer, node, nullptr)) {
            s.tw = gdk_texture_get_width(texture);
            s.th = gdk_texture_get_height(texture);
            s.px.resize((size_t)s.tw * s.th * 4);
            gdk_texture_download(texture, s.px.data(), (size_t)s.tw * 4);
            g_object_unref(texture);
        }
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    return s;
}

int dist(const int a[3], const Rgba& c) {
    return std::max({std::abs(a[0] - c.r), std::abs(a[1] - c.g), std::abs(a[2] - c.b)});
}

// The cursor's cell, measured: VTE's cell size and the cursor's row and
// column give the cell to a few pixels (VTE's padding), so the cell-sized
// window with the most pixels unlike the background, within those pixels
// of it, is taken as the cursor's. VTE's padding puts the cells at half a
// pixel, so a block's edges, and all of an outline, are blended with the
// background over two pixels: the counts are over the window and a pixel
// around it (the region), its outer two pixels the ring.
struct Cell {
    bool found = false;
    int cw = 0, ch = 0, x = 0, y = 0;
    int asCursor = 0;     // region pixels within 24 of the cursor's color
    int ringUnlike = 0;   // ring pixels more than 30 from the background
    int inner = 0, innerBg = 0;   // inside the ring, and those within 40 of the background
    int closest = 255;    // the region's pixel nearest the cursor's color
    int center[3] = {0, 0, 0};
    std::string text() const {
        char b[240];
        std::snprintf(b, sizeof b,
                      "cell %dx%d at %d,%d: %d pixels as the cursor (interior %d), ring %d unlike "
                      "the background, %d of %d inside as the background, nearest the cursor by %d, "
                      "center #%02X%02X%02X",
                      cw, ch, x, y, asCursor, (cw - 2) * (ch - 2), ringUnlike, innerBg, inner, closest,
                      center[0], center[1], center[2]);
        return b;
    }
};

Cell measure() {
    Cell m;
    if (!gT || !gT->vteWidget()) return m;
    VteTerminal* vte = VTE_TERMINAL(gT->vteWidget());
    const Settings s = themed();
    const Rgba bg = s.background(Surface::Terminal), cursor = s.terminalCursor();
    m.cw = (int)vte_terminal_get_char_width(vte);
    m.ch = (int)vte_terminal_get_char_height(vte);
    long col = 0, row = 0;
    vte_terminal_get_cursor_position(vte, &col, &row);
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte));
    const long top = adj ? std::lround(gtk_adjustment_get_value(adj)) : 0;
    if (m.cw <= 2 || m.ch <= 2 || row < top) return m;
    const int x0 = (int)col * m.cw, y0 = (int)(row - top) * m.ch;
    const Shot shot = shoot(gT->vteWidget());
    if (shot.px.empty()) return m;
    int best = -1, rgb[3];
    for (int dy = -3; dy <= 8; dy++)
        for (int dx = -3; dx <= 8; dx++) {
            int unlike = 0;
            for (int y = 0; y < m.ch; y++)
                for (int x = 0; x < m.cw; x++)
                    if (shot.at(x0 + dx + x, y0 + dy + y, rgb) && dist(rgb, bg) > 30) unlike++;
            if (unlike > best) {
                best = unlike;
                m.x = x0 + dx;
                m.y = y0 + dy;
            }
        }
    m.found = true;
    const int w = m.cw + 2, h = m.ch + 2;   // the region, from (m.x - 1, m.y - 1)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!shot.at(m.x - 1 + x, m.y - 1 + y, rgb)) continue;
            const bool ring = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
            if (dist(rgb, cursor) <= 24) m.asCursor++;
            m.closest = std::min(m.closest, dist(rgb, cursor));
            if (ring && dist(rgb, bg) > 30) m.ringUnlike++;
            if (!ring) {
                m.inner++;
                if (dist(rgb, bg) <= 40) m.innerBg++;
            }
        }
    shot.at(m.x + m.cw / 2, m.y + m.ch / 2, rgb);
    std::copy(rgb, rgb + 3, m.center);
    return m;
}

// The cursor on an empty cell with the keyboard: a block of its color, the
// whole of it but its blended edges.
bool solidBlock(const Cell& m) {
    return m.found && m.asCursor * 10 >= (m.cw - 2) * (m.ch - 2) * 9 &&
           dist(m.center, themed().terminalCursor()) <= 24;
}
// On a character: mostly the cursor's color, the glyph in the background's.
bool blockWithText(const Cell& m) {
    return m.found && m.asCursor * 2 >= m.cw * m.ch && m.innerBg >= 3;
}
// Without the keyboard: a ring unlike the background, at least half of it,
// around a cell mostly of background (the window may take in a side of the
// line), and as near the cursor's color as a line blended over two pixels
// comes (half way from the background).
bool outline(const Cell& m) {
    const Settings s = themed();
    const Rgba bg = s.background(Surface::Terminal), cursor = s.terminalCursor();
    const int bgRgb[3] = {bg.r, bg.g, bg.b};
    return m.found && m.ringUnlike >= m.cw + m.ch && dist(m.center, bg) <= 24 &&
           m.innerBg * 10 >= m.inner * 6 && m.closest * 2 <= dist(bgRgb, cursor) + 8;
}

// The whole widget in this theme's background by now (a blank pixel).
bool repainted() {
    if (!gT || !gT->vteWidget()) return false;
    const Shot shot = shoot(gT->vteWidget());
    int rgb[3];
    return shot.at(shot.w - 4, shot.h - 4, rgb) && dist(rgb, themed().background(Surface::Terminal)) <= 6;
}

void useTheme(const std::string& id) {
    MCGuiStep("theme = " + id, [id] {
        gTheme = id;
        MCGuiSetSettings("theme = " + id + "\n" + kBase);
    });
    MCGuiWait("the terminal drawn in " + id + "'s colors", 10, repainted);
}

void checkSolid(const std::string& id) {
    MCGuiWait(id + ": a block cursor shows", 10, [] { return solidBlock(measure()); });
    MCGuiStep(id + ": it is the text color, solid, and stands out", [id] {
        const Cell m = measure();
        const Settings s = themed();
        MCGuiNote("%s: cursor %s on %s; %s", id.c_str(), Settings::formatColor(s.terminalCursor()).c_str(),
                  Settings::formatColor(s.background(Surface::Terminal)).c_str(), m.text().c_str());
        GCHECK(solidBlock(m));
        GCHECK(dist(m.center, s.terminalCursor()) <= 24);
        GCHECK(dist(m.center, s.background(Surface::Terminal)) > 60);
    });
}

}  // namespace

MC_GUI_SUITE(terminal_draw) {
    MCGuiStep("a bash in the only terminal, the dock shown", [] {
        GCHECK(dock() != nullptr);
        if (!dock()) return;
        const char* old = g_getenv("SHELL");
        gHadShell = old != nullptr;
        gOldShell = old ? old : "";
        gHome = g_get_home_dir();
        GCHECK(gHome.find(MCGuiScratch()) == 0);   // never the user's own
        EnvLock::envSet("SHELL", "/bin/bash", TRUE);
        MCGuiWriteFile(gHome + "/.bashrc", "PS1='$ '\n");
        while (dock()->count() > 1) dock()->close(dock()->count() - 1, false);
        if (dock()->at(0)->made()) {   // another suite's shell: a fresh bash
            dock()->newTerminal();
            dock()->close(0, false);
        }
        if (!MCGuiApp().layout->terminalShown()) MCGuiApp().layout->toggleTerminal();
        gT = dock()->at(0);
        gT->focus();
        MCGuiFocusKeyWindow();
    });
    MCGuiWait("bash marks its prompt", 30, [] { return ready(gT); });
    MCGuiStep("the terminal takes the keyboard", [] { gT->focus(); });
    MCGuiWait("it has it", 10, terminalHasKeyboard);
    MCGuiStep("the screen cleared (Ctrl+L): the prompt on the top row", [] { MCGuiKey("ctrl+l"); });
    MCGuiWait("the cursor on the top row", 10, [] {
        long col = 0, row = 0;
        VteTerminal* vte = VTE_TERMINAL(gT->vteWidget());
        vte_terminal_get_cursor_position(vte, &col, &row);
        GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte));
        return adj && row == std::lround(gtk_adjustment_get_value(adj)) && col > 0;
    });

    // Every built-in theme: the block on an empty cell.
    for (const Theme& t : Settings::themes()) {
        useTheme(t.id);
        checkSolid(t.id);
    }

    // A dark theme and a light one: the character under the block, and the
    // outline without the keyboard.
    for (const char* id : {"dark-modern", "light-modern"}) {
        const std::string name = id;
        useTheme(name);
        checkSolid(name);
        MCGuiStep(name + ": x typed, the cursor moved back onto it", [] {
            MCGuiType("x");
            MCGuiKey("left");
        });
        MCGuiWait(name + ": the block shows its character", 10, [] { return blockWithText(measure()); });
        MCGuiStep(name + ": in the background's color", [name] {
            const Cell m = measure();
            MCGuiNote("%s, on x: %s", name.c_str(), m.text().c_str());
            GCHECK(blockWithText(m));
            MCGuiKey("right");   // past the x again, so Ctrl+U takes it
            MCGuiKey("ctrl+u");
        });
        MCGuiWait(name + ": the x taken back", 10, [] { return gT->recentText(1).find('x') == std::string::npos; });
        MCGuiStep(name + ": the editor takes the keyboard", [] {
            gtk_widget_grab_focus(MCGuiApp().editor->textView());
        });
        MCGuiWait(name + ": the terminal has lost it", 10, [] { return !terminalHasKeyboard(); });
        MCGuiWait(name + ": the cursor is an outline", 10, [] { return outline(measure()); });
        MCGuiStep(name + ": in the cursor's color", [name] {
            const Cell m = measure();
            MCGuiNote("%s, without the keyboard: %s", name.c_str(), m.text().c_str());
            GCHECK(outline(m));
        });
        MCGuiStep(name + ": the terminal takes the keyboard back", [] { gT->focus(); });
        MCGuiWait(name + ": it has it", 10, terminalHasKeyboard);
    }

    MCGuiStep("the settings and SHELL as they were", [] {
        MCGuiSetSettings(kBase);
        if (gHadShell) EnvLock::envSet("SHELL", gOldShell.c_str(), TRUE);
        else EnvLock::envUnset("SHELL");
    });
}
