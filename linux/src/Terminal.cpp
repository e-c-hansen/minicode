// Terminal.cpp — see Terminal.h. Entirely compiled out unless
// MINICODE_ENABLE_TERMINAL is set.
#include "Terminal.h"

#ifdef MINICODE_ENABLE_TERMINAL

#include "Palette.h"
#include "TermLinkPath.h"
#include "TermLinks.h"

#include <vte/vte.h>
#include <cmath>
#include <cstdlib>

namespace {

// The 16 ANSI slots, in VTE's order: black, red, green, yellow, blue, magenta,
// cyan, white, then the eight bright variants.
//
// VS Code ships two Dark+ palettes: the saturated one its integrated terminal
// uses (pure #E5E510 yellow, #CD3131 red) and the muted one its *editor* uses
// for syntax. The editor set is the better neighbour here, since the panel sits
// directly under a view already painted in those exact colors — so most of
// these are literally the values in Palette.h. Slots with no syntax equivalent
// are interpolated in the same register rather than borrowed from the harsher
// terminal set.
const char* kAnsiPalette[16] = {
    "#2A2A2A",   // black: the Markdown code background, not pure black
    "#D16969",   // red: Dark+ regex/invalid, a brick red rather than a signal red
    "#6A9955",   // green: the editor's comment green
    "#D7BA7D",   // yellow: Dark+ annotation tan
    "#569CD6",   // blue: the editor's keyword blue
    "#C586C0",   // magenta: the editor's preprocessor magenta
    "#4EC9B0",   // cyan: the editor's type teal
    "#D4D4D4",   // white: the editor's foreground
    "#808080",   // bright black: dim text still has to be readable
    "#E08A8A",   // bright red
    "#B5CEA8",   // bright green: the editor's number green
    "#DCDCAA",   // bright yellow: the editor's function yellow
    "#9CDCFE",   // bright blue: Dark+ variable blue
    "#D7A3D2",   // bright magenta
    "#7FD8C6",   // bright cyan
    "#F0F0F0",   // bright white
};

// The editor asks GTK for "the monospace font" via
// gtk_text_view_set_monospace, which resolves to the monospace family at the
// desktop font size. VTE has no equivalent flag, so build the same description
// by hand — otherwise the terminal renders a size or two off from the editor
// sitting directly above it. Caller owns the result.
PangoFontDescription* systemMonospaceFont() {
    char* uiFont = nullptr;
    g_object_get(gtk_settings_get_default(), "gtk-font-name", &uiFont, nullptr);
    PangoFontDescription* ui =
        pango_font_description_from_string(uiFont ? uiFont : "Sans 11");
    g_free(uiFont);

    PangoFontDescription* mono = pango_font_description_new();
    pango_font_description_set_family(mono, "monospace");
    const gint size = pango_font_description_get_size(ui);
    if (size > 0) {
        if (pango_font_description_get_size_is_absolute(ui))
            pango_font_description_set_absolute_size(mono, size);
        else
            pango_font_description_set_size(mono, size);
    }
    pango_font_description_free(ui);
    return mono;
}

} // namespace

Terminal::Terminal(const std::string& cwd) : cwd_(cwd) {
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "minicode-terminal");
    gtk_widget_add_css_class(scroller, "minicode-scroller");

    // The scroller sits in an overlay so the hovered link's underline can be
    // drawn over the terminal: VTE underlines only its own regex matches, and
    // a regex cannot know which paths exist.
    root_ = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(root_), scroller);
    // Only the floor: the height is the paned divider's business now, so the
    // panel must not demand a fixed size (that was what made it unresizable).
    gtk_widget_set_size_request(root_, -1, kMinHeight);

    vte_ = vte_terminal_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), vte_);

    underline_ = gtk_drawing_area_new();
    gtk_widget_set_can_target(underline_, FALSE);   // clicks go through to VTE
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(underline_), drawHover, this, nullptr);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), underline_);

    applyTheme();

    VteTerminal* term = VTE_TERMINAL(vte_);
    vte_terminal_set_scrollback_lines(term, 10000);
    vte_terminal_set_scroll_on_keystroke(term, TRUE);
    // Deliberately not scroll-on-output: a long build can then run while you
    // read something further up without yanking the view back down.
    vte_terminal_set_scroll_on_output(term, FALSE);
    vte_terminal_set_audible_bell(term, FALSE);
    vte_terminal_set_mouse_autohide(term, TRUE);
    vte_terminal_set_allow_hyperlink(term, TRUE);
    vte_terminal_set_cursor_blink_mode(term, VTE_CURSOR_BLINK_OFF);

    g_signal_connect(vte_, "child-exited", G_CALLBACK(onChildExited), this);

    // Copy and paste, the keys every Linux terminal uses: plain Ctrl+C and
    // Ctrl+V belong to the shell. VTE itself binds neither. A controller on
    // the terminal, so they mean this only while it has the keyboard.
    GtkEventController* keys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_C, (GdkModifierType)(
                             GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
                         gtk_callback_action_new([](GtkWidget* w, GVariant*, gpointer) -> gboolean {
                             vte_terminal_copy_clipboard_format(VTE_TERMINAL(w), VTE_FORMAT_TEXT);
                             return TRUE;
                         }, nullptr, nullptr)));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_V, (GdkModifierType)(
                             GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
                         gtk_callback_action_new([](GtkWidget* w, GVariant*, gpointer) -> gboolean {
                             vte_terminal_paste_clipboard(VTE_TERMINAL(w));
                             return TRUE;
                         }, nullptr, nullptr)));
    gtk_widget_add_controller(vte_, keys);

    // Ctrl+click on a link. Capture phase, so it is seen before VTE starts a
    // selection or hands the click to a program that tracks the mouse; a
    // click that opens a link is claimed, and VTE never sees it. Without Ctrl,
    // or off a link, the click is left alone.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int n,
                                                      double x, double y, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        const GdkModifierType mods =
            gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
        if (!(mods & GDK_CONTROL_MASK)) return;
        if (!self->linkAt(x, y, nullptr)) return;
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        if (n == 1) self->openLinkAt(x, y);   // a double click opens it once
    }), this);
    gtk_widget_add_controller(vte_, GTK_EVENT_CONTROLLER(click));
    handlers_.push_back(G_OBJECT(click));

    // Where the pointer is, and whether Ctrl is down, for the underline.
    GtkEventController* motion = gtk_event_controller_motion_new();
    auto moved = +[](GtkEventControllerMotion* m, double x, double y, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        self->pointerIn_ = true;
        self->pointerX_ = x;
        self->pointerY_ = y;
        self->ctrlDown_ = gtk_event_controller_get_current_event_state(
                              GTK_EVENT_CONTROLLER(m)) & GDK_CONTROL_MASK;
        self->setHover(self->ctrlDown_);
    };
    g_signal_connect(motion, "enter", G_CALLBACK(moved), this);
    g_signal_connect(motion, "motion", G_CALLBACK(moved), this);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        self->pointerIn_ = false;
        self->setHover(false);
    }), this);
    gtk_widget_add_controller(vte_, motion);
    handlers_.push_back(G_OBJECT(motion));

    // New output, or scrolling, can move text under a pointer that has not
    // moved. Only looked at while Ctrl is down or a link is showing, so
    // ordinary output costs nothing.
    auto recheck = +[](gpointer, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        if (self->ctrlDown_ || !self->hover_.empty()) self->setHover(self->ctrlDown_);
    };
    g_signal_connect(vte_, "contents-changed", G_CALLBACK(+[](VteTerminal*, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        self->topKnown_ = false;   // see topRow
        if (self->ctrlDown_ || !self->hover_.empty()) self->setHover(self->ctrlDown_);
    }), this);
    if (GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_))) {
        g_signal_connect(adj, "value-changed", G_CALLBACK(recheck), this);
        handlers_.push_back(G_OBJECT(adj));
    }

    // Pressing or letting go of Ctrl without moving the mouse shows or hides
    // the underline too. Key events go to the focused widget, which is often
    // the editor, so this listens on the window (the Mac does the same with a
    // local event monitor). The window exists only once the panel is in one.
    g_signal_connect(vte_, "realize", G_CALLBACK(+[](GtkWidget* w, gpointer p) {
        Terminal* self = static_cast<Terminal*>(p);
        GtkWidget* win = GTK_WIDGET(gtk_widget_get_root(w));
        if (!win || win == self->keysWindow_) return;
        GtkEventController* keysCtl = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keysCtl, GTK_PHASE_CAPTURE);
        auto ctrlKey = +[](GtkEventControllerKey* k, guint keyval, guint, GdkModifierType,
                           gpointer q) -> gboolean {
            Terminal* t = static_cast<Terminal*>(q);
            if (keyval != GDK_KEY_Control_L && keyval != GDK_KEY_Control_R) return FALSE;
            // key-pressed and key-released share the handler; which one ran
            // is in the event itself.
            GdkEvent* ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(k));
            t->ctrlDown_ = ev && gdk_event_get_event_type(ev) == GDK_KEY_PRESS;
            t->setHover(t->ctrlDown_);
            return FALSE;   // Ctrl still reaches whoever has the keyboard
        };
        g_signal_connect(keysCtl, "key-pressed", G_CALLBACK(ctrlKey), self);
        g_signal_connect(keysCtl, "key-released", G_CALLBACK(ctrlKey), self);
        gtk_widget_add_controller(win, keysCtl);
        self->keysWindow_ = win;
        self->windowKeys_ = keysCtl;
        g_object_add_weak_pointer(G_OBJECT(win), (gpointer*)&self->keysWindow_);
    }), this);

    spawnShell();
}

Terminal::~Terminal() {
    g_cancellable_cancel(spawnCancel_);
    g_object_unref(spawnCancel_);
    g_signal_handlers_disconnect_by_data(vte_, this);
    for (GObject* o : handlers_) g_signal_handlers_disconnect_by_data(o, this);
    if (keysWindow_) {
        gtk_widget_remove_controller(keysWindow_, windowKeys_);
        g_object_remove_weak_pointer(G_OBJECT(keysWindow_), (gpointer*)&keysWindow_);
    }
    if (cursorIdle_) g_source_remove(cursorIdle_);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(underline_), nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------- links

namespace {

// The CSS padding VTE insets its cells by (ThemeCss sets 6px, but a theme or
// a later change could make it anything). GTK 4.10 deprecated the style
// context without a replacement for reading a widget's padding.
GtkBorder cellInset(GtkWidget* vte) {
    GtkBorder b{};
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_style_context_get_padding(gtk_widget_get_style_context(vte), &b);
    G_GNUC_END_IGNORE_DEPRECATIONS
    return b;
}

// Text of the buffer from (r0, c0) up to, not including, (r1, c1). Rows that
// wrapped because they were full come back joined; a real line end is "\n".
std::string textRange(VteTerminal* t, long r0, long c0, long r1, long c1) {
    gsize n = 0;
    char* s = vte_terminal_get_text_range_format(t, VTE_FORMAT_TEXT, r0, c0, r1, c1, &n);
    std::string out = s ? std::string(s, n) : std::string();
    g_free(s);
    return out;
}

void trimEnd(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
}

}  // namespace

// The buffer row at the top of the view, in the numbering that
// vte_terminal_get_text_range_format uses. VTE's vertical adjustment counts
// from the oldest row it still keeps, which is row 0 only until scrollback is
// dropped: `clear` drops it (it sends ESC[3J), and so does output past the
// 10,000 kept lines (taking the adjustment as the row put a click after a
// `clear` rows away from the pointer). VTE has no call that gives the
// offset, so it is found by comparing the text on screen with the buffer's
// rows at each place the screen can start (the cursor is always on the last
// screenful), and kept until the contents change. Fractional while a scroll
// is under way.
double Terminal::topRow() {
    VteTerminal* t = VTE_TERMINAL(vte_);
    GtkAdjustment* adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte_));
    if (!adj) return 0;
    const double value = gtk_adjustment_get_value(adj);
    if (!topKnown_) {
        topKnown_ = true;
        topDelta_ = 0;
        const long rows = vte_terminal_get_row_count(t), cols = vte_terminal_get_column_count(t);
        long ccol = 0, crow = 0;
        vte_terminal_get_cursor_position(t, &ccol, &crow);
        // Where the adjustment stands when scrolled to the end.
        const long bottom = std::lround(gtk_adjustment_get_upper(adj) -
                                        gtk_adjustment_get_page_size(adj));
        const long top = std::lround(value);
        char* v = vte_terminal_get_text_format(t, VTE_FORMAT_TEXT);
        std::string shown = v ? v : "";
        g_free(v);
        trimEnd(shown);
        for (long d = std::max(0L, crow - rows + 1 - bottom); d <= crow - bottom; d++) {
            // VTE's "visible" text runs one row past the screen (for a row
            // partly scrolled into view), which only shows when that row
            // has text, so either reading may be the one that matches.
            std::string screen = textRange(t, top + d, 0, top + d + rows - 1, cols);
            std::string screenPlus = textRange(t, top + d, 0, top + d + rows, cols);
            trimEnd(screen);
            trimEnd(screenPlus);
            if (screen == shown || screenPlus == shown) { topDelta_ = d; break; }
        }
    }
    return value + topDelta_;
}

// The directory the shell is in. VTE records the OSC 7 that Ubuntu's
// /etc/profile.d/vte-2.91.sh has bash print at each prompt; a shell without
// that hook still has a working directory the kernel knows; failing both,
// the folder the shell started in.
std::string Terminal::shellDirectory() const {
#if VTE_CHECK_VERSION(0, 78, 0)
    if (GUri* uri = vte_terminal_ref_termprop_uri(VTE_TERMINAL(vte_),
                                                  VTE_TERMPROP_CURRENT_DIRECTORY_URI)) {
        char* s = g_uri_to_string(uri);
        g_uri_unref(uri);
        char* host = nullptr;
        char* path = s ? g_filename_from_uri(s, &host, nullptr) : nullptr;
        g_free(s);
        // Another machine's directory (ssh) says nothing about files here.
        const bool local = !host || !*host || g_strcmp0(host, "localhost") == 0 ||
                           g_strcmp0(host, g_get_host_name()) == 0;
        std::string dir = (path && local) ? path : "";
        g_free(host);
        g_free(path);
        if (!dir.empty()) return dir;
    }
#endif
    if (shellPid_ > 0) {
        const std::string link = "/proc/" + std::to_string(shellPid_) + "/cwd";
        if (char* dir = g_file_read_link(link.c_str(), nullptr)) {
            std::string out = dir;
            g_free(dir);
            return out;
        }
    }
    return cwd_;
}

bool Terminal::findLink(double x, double y, Link* out, std::vector<Span>* spans) {
    VteTerminal* t = VTE_TERMINAL(vte_);
    const long cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
    const long cols = vte_terminal_get_column_count(t);
    if (cw <= 0 || ch <= 0 || cols <= 0) return false;

    // Which cell: VTE lays its cells out from the padding, in whole cells,
    // and the vertical adjustment's value is the buffer row at the top.
    const GtkBorder pad = cellInset(vte_);
    const double fx = (x - pad.left) / cw, fy = (y - pad.top) / ch;
    if (fx < 0 || fy < 0 || fx >= cols || fy >= vte_terminal_get_row_count(t)) return false;
    const long row = (long)std::floor(topRow() + fy), col = (long)fx;
    if (row < 0) return false;

    // The whole line the cell is on, which may have wrapped over several
    // rows: a compiler's long path in a narrow panel. A row that wrapped
    // joins the next without a "\n" (rows VTE no longer keeps, or never
    // had, read as empty lines). Far enough either way for any real
    // reference, and bounded so a huge wrapped line costs little.
    const long kReach = 16;
    long first = row, last = row;
    while (first > 0 && row - first < kReach) {
        const std::string s = textRange(t, first - 1, 0, first, 0);
        if (s.empty() || s.back() == '\n') break;
        first--;
    }
    while (last - row < kReach) {
        const std::string s = textRange(t, last, 0, last + 1, 0);
        if (s.empty() || s.back() == '\n') break;
        last++;
    }
    std::string line = textRange(t, first, 0, last, cols);
    while (!line.empty() && line.back() == '\n') line.pop_back();
    // Everything before the clicked cell is where it sits in the line.
    const size_t offset = textRange(t, first, 0, row, col).size();
    if (offset >= line.size()) return false;

    const std::vector<TermLinks::Link> links = TermLinks::find(line);
    const TermLinks::Link* l = TermLinks::at(links, offset);
    if (!l) return false;

    Link link;
    link.line = l->line;
    link.column = l->column;
    if (l->kind == TermLinks::Link::Url) {
        link.isUrl = true;
        link.target = l->target;
    } else {
        TermLinkPath::Target found;
        if (!TermLinkPath::resolve(l->target, shellDirectory(), root_dir_,
                                   g_get_home_dir(), &found))
            return false;
        link.target = found.path;
        link.isDir = found.isDir;
        link.insideRoot = found.insideRoot;
    }
    if (out) *out = link;
    if (!spans) return true;

    // Back from bytes of the line to cells, for the underline: a wide
    // character takes two, a combining mark none, and a wide character that
    // would straddle the right edge starts the next row instead.
    spans->clear();
    const size_t end = l->start + l->length;
    long r = first, c = 0;
    for (size_t i = 0; i < end && i < line.size();) {
        const char* p = line.c_str() + i;
        gunichar u = g_utf8_get_char_validated(p, (gssize)(line.size() - i));
        size_t n = 1;
        if (u == (gunichar)-1 || u == (gunichar)-2) u = '?';
        else n = (size_t)(g_utf8_next_char(p) - p);
        const long w = u == '\t' ? 8 - c % 8
                     : g_unichar_iszerowidth(u) ? 0 : g_unichar_iswide(u) ? 2 : 1;
        if (c + w > cols) { r++; c = 0; }
        if (i >= l->start && w > 0) {
            if (spans->empty() || spans->back().row != r) spans->push_back({r, c, c + w});
            else spans->back().col1 = c + w;
        }
        c += w;
        i += n;
    }
    return true;
}

bool Terminal::linkAt(double x, double y, Link* out) {
    return findLink(x, y, out, nullptr);
}

bool Terminal::openLinkAt(double x, double y) {
    Link link;
    if (!findLink(x, y, &link, nullptr)) return false;
    hover_.clear();
    gtk_widget_queue_draw(underline_);
    if (onLink_) onLink_(link);
    return true;
}

// Underline the link under the pointer while Ctrl is held, and show the
// pointing hand over it.
void Terminal::setHover(bool ctrl) {
    std::vector<Span> spans;
    if (ctrl && pointerIn_ && gtk_widget_get_mapped(vte_))
        findLink(pointerX_, pointerY_, nullptr, &spans);
    bool same = spans.size() == hover_.size();
    for (size_t i = 0; same && i < spans.size(); i++)
        same = spans[i].row == hover_[i].row && spans[i].col0 == hover_[i].col0 &&
               spans[i].col1 == hover_[i].col1;
    hover_ = std::move(spans);
    if (!same) gtk_widget_queue_draw(underline_);

    // VTE sets its own cursor as the pointer moves, possibly after this
    // handler has run, so the hand goes on once the event is done with.
    const bool hand = !hover_.empty();
    if (!hand && !handCursor_) return;
    handCursor_ = hand;
    if (cursorIdle_) return;
    cursorIdle_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE, [](gpointer p) -> gboolean {
        Terminal* self = static_cast<Terminal*>(p);
        self->cursorIdle_ = 0;
        // "text" is VTE's own pointer over the cells.
        gtk_widget_set_cursor_from_name(self->vte_, self->handCursor_ ? "pointer" : "text");
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}

void Terminal::drawHover(GtkDrawingArea*, cairo_t* cr, int, int, gpointer p) {
    Terminal* self = static_cast<Terminal*>(p);
    if (self->hover_.empty()) return;
    VteTerminal* t = VTE_TERMINAL(self->vte_);
    const long cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
    const double top = self->topRow();
    const GtkBorder pad = cellInset(self->vte_);
    const int height = gtk_widget_get_height(self->vte_);
    gdk_cairo_set_source_rgba(cr, &self->linkColor_);
    for (const Span& s : self->hover_) {
        // In the terminal's coordinates, then the overlay's; one pixel, just
        // above the bottom of the cell, where a text underline would go.
        const double y = pad.top + (s.row - top) * ch + ch - 2;
        if (y < 0 || y >= height) continue;
        graphene_point_t in = GRAPHENE_POINT_INIT((float)(pad.left + s.col0 * cw), (float)y);
        graphene_point_t at;
        if (!gtk_widget_compute_point(self->vte_, self->underline_, &in, &at)) continue;
        cairo_rectangle(cr, std::floor(at.x), std::floor(at.y),
                        (double)(s.col1 - s.col0) * cw, 1);
    }
    cairo_fill(cr);
}

bool Terminal::owns(GtkWidget* w) const {
    return w && (w == root_ || gtk_widget_is_ancestor(w, root_));
}

// Colors and font. Everything here mirrors the macOS terminal panel or the
// editor next to it; nothing is left at a VTE default.
void Terminal::applyTheme() {
    VteTerminal* term = VTE_TERMINAL(vte_);

    GdkRGBA cursor, highlight;
    gdk_rgba_parse(&cursor,    pal::TermCursor);
    gdk_rgba_parse(&highlight, pal::Selection);   // same blue as editor selection
    for (int i = 0; i < 16; ++i) gdk_rgba_parse(&palette_[i], kAnsiPalette[i]);

    applySettings(Settings::parse(""));   // the defaults until the app applies its own
    vte_terminal_set_color_cursor(term, &cursor);
    vte_terminal_set_color_highlight(term, &highlight);
    // Bold text picks the bright slot instead of only thickening, which is what
    // makes the usual colored shell prompts look right.
    vte_terminal_set_bold_is_bright(term, TRUE);

    PangoFontDescription* font = systemMonospaceFont();
    vte_terminal_set_font(term, font);
    pango_font_description_free(font);
}

void Terminal::applySettings(const Settings& s) {
    auto gdk = [](const Rgba& c) {
        return GdkRGBA{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (float)c.a};
    };
    const Rgba bgColor = s.background(Surface::Terminal);
    GdkRGBA fg = gdk(s.text(Surface::Terminal));
    GdkRGBA bg = gdk(bgColor);
    // The block cursor's text is the background color, solid so it stays legible.
    GdkRGBA cursorFg = gdk(Rgba::hex(bgColor.rgb()));
    vte_terminal_set_colors(VTE_TERMINAL(vte_), &fg, &bg, palette_, 16);
    vte_terminal_set_color_cursor_foreground(VTE_TERMINAL(vte_), &cursorFg);
    linkColor_ = fg;   // the underline under a hovered link
}

// Write a status line of our own into the terminal, in the palette's green —
// the same color and register the macOS panel uses for these
// (Terminal.mm, THex(0x6A9955), which is now ANSI slot 2).
void Terminal::feedNotice(const std::string& text) {
    const std::string line = "\r\n\033[32m" + text + "\033[0m\r\n";
    vte_terminal_feed(VTE_TERMINAL(vte_), line.c_str(), (gssize)line.size());
}

// The shell exiting used to leave a dead rectangle until the app was
// restarted: Ctrl+D and the panel was finished. The macOS build restarts it
// (Terminal.mm -startShell), so this does too.
void Terminal::onChildExited(VteTerminal*, gint /*status*/, gpointer selfp) {
    Terminal* self = static_cast<Terminal*>(selfp);
    self->shellPid_ = -1;

    // A shell that dies the instant it starts — $SHELL uninstalled, a profile
    // that exits, a bad interpreter — would otherwise respawn in a tight loop
    // and peg a core. Three fast deaths in a row is taken as "not coming back".
    const gint64 now = g_get_monotonic_time();
    if (now - self->lastSpawn_ < 1000000) {   // under a second counts as fast
        if (++self->rapidExits_ >= 3) {
            self->feedNotice("[shell keeps exiting immediately — not restarting]");
            return;
        }
    } else {
        self->rapidExits_ = 0;
    }

    self->feedNotice("[shell exited — restarting]");
    self->spawnShell();
}

// Reports a spawn that never got off the ground. Without this the panel just
// sits there blank and there is nothing to tell you why.
void Terminal::onSpawned(VteTerminal*, GPid pid, GError* error, gpointer selfp) {
    if (pid != -1 && !error) {
        static_cast<Terminal*>(selfp)->shellPid_ = pid;   // for its directory
        return;
    }
    // The window closed while the shell was starting: this object is gone.
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
    Terminal* self = static_cast<Terminal*>(selfp);
    self->feedNotice(std::string("[could not start the shell: ") +
                     (error && error->message ? error->message : "unknown error") + "]");
}

void Terminal::spawnShell() {
    const char* shell = g_getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/bash";

    char* argv[] = { (char*)shell, nullptr };

    lastSpawn_ = g_get_monotonic_time();

    // vte_terminal_spawn_async is the current API (the sync variant is
    // deprecated in recent VTE).
    vte_terminal_spawn_async(
        VTE_TERMINAL(vte_),
        VTE_PTY_DEFAULT,
        cwd_.empty() ? nullptr : cwd_.c_str(),  // working directory
        argv,
        nullptr,                                 // envv (inherit)
        G_SPAWN_DEFAULT,
        nullptr, nullptr, nullptr,               // child setup
        -1,                                      // timeout
        spawnCancel_,                            // cancelled if the window closes first
        onSpawned, this);                        // callback / user data
}

void Terminal::focus() {
    if (vte_) gtk_widget_grab_focus(vte_);
}

#endif // MINICODE_ENABLE_TERMINAL
