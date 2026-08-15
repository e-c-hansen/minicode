// Terminal.cpp — see Terminal.h. Entirely compiled out unless
// MINICODE_ENABLE_TERMINAL is set.
#include "Terminal.h"

#ifdef MINICODE_ENABLE_TERMINAL

#include "Palette.h"

#include <vte/vte.h>
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
    root_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(root_, "minicode-terminal");
    gtk_widget_add_css_class(root_, "minicode-scroller");
    // Only the floor: the height is the paned divider's business now, so the
    // panel must not demand a fixed size (that was what made it unresizable).
    gtk_widget_set_size_request(root_, -1, kMinHeight);

    vte_ = vte_terminal_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), vte_);

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

    spawnShell();
}

// Colors and font. Everything here mirrors the macOS terminal panel or the
// editor next to it; nothing is left at a VTE default.
void Terminal::applyTheme() {
    VteTerminal* term = VTE_TERMINAL(vte_);

    GdkRGBA fg, bg, cursor, cursorFg, highlight;
    gdk_rgba_parse(&fg,        pal::TermText);
    gdk_rgba_parse(&bg,        pal::TermBg);
    gdk_rgba_parse(&cursor,    pal::TermCursor);
    gdk_rgba_parse(&cursorFg,  pal::TermBg);
    gdk_rgba_parse(&highlight, pal::Selection);   // same blue as editor selection

    GdkRGBA palette[16];
    for (int i = 0; i < 16; ++i) gdk_rgba_parse(&palette[i], kAnsiPalette[i]);

    vte_terminal_set_colors(term, &fg, &bg, palette, 16);
    vte_terminal_set_color_cursor(term, &cursor);
    vte_terminal_set_color_cursor_foreground(term, &cursorFg);
    vte_terminal_set_color_highlight(term, &highlight);
    // Bold text picks the bright slot instead of only thickening, which is what
    // makes the usual colored shell prompts look right.
    vte_terminal_set_bold_is_bright(term, TRUE);

    PangoFontDescription* font = systemMonospaceFont();
    vte_terminal_set_font(term, font);
    pango_font_description_free(font);
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
    if (pid != -1 && !error) return;
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
        nullptr,                                 // cancellable
        onSpawned, this);                        // callback / user data
}

void Terminal::focus() {
    if (vte_) gtk_widget_grab_focus(vte_);
}

#endif // MINICODE_ENABLE_TERMINAL
