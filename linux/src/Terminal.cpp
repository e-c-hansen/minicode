// Terminal.cpp — see Terminal.h. Entirely compiled out unless
// MINICODE_ENABLE_TERMINAL is set.
#include "Terminal.h"

#ifdef MINICODE_ENABLE_TERMINAL

#include "Palette.h"

#include <vte/vte.h>
#include <cstdlib>

namespace {

// The 16 ANSI slots, in VTE's order: black, red, green, yellow, blue, magenta,
// cyan, white, then the eight bright variants. These are the VS Code Dark+
// terminal colors, the same theme the editor's syntax palette comes from. VTE's
// own defaults are far more saturated and look like a different application.
const char* kAnsiPalette[16] = {
    "#000000", "#CD3131", "#0DBC79", "#E5E510",
    "#2472C8", "#BC3FBC", "#11A8CD", "#E5E5E5",
    "#666666", "#F14C4C", "#23D18B", "#F5F543",
    "#3B8EEA", "#D670D6", "#29B8DB", "#FFFFFF",
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

void Terminal::spawnShell() {
    const char* shell = g_getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/bash";

    char* argv[] = { (char*)shell, nullptr };

    // vte_terminal_spawn_async is the current API (the sync variant is
    // deprecated in recent VTE). NULL callback = fire and forget.
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
        nullptr, nullptr);                       // callback / user data
}

void Terminal::focus() {
    if (vte_) gtk_widget_grab_focus(vte_);
}

#endif // MINICODE_ENABLE_TERMINAL
