// ThemeCss.cpp — see ThemeCss.h.
#include "ThemeCss.h"

namespace theme {

std::string cssColor(const Rgba& c) {
    int milli = (int)(c.a * 1000 + 0.5);
    if (milli < 0) milli = 0;
    if (milli > 1000) milli = 1000;
    std::string frac = std::to_string(milli % 1000);
    frac.insert(0, 3 - frac.size(), '0');
    return "rgba(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," +
           std::to_string(c.b) + "," + std::to_string(milli / 1000) + "." + frac +
           ")";
}

std::string stylesheet(const Settings& s) {
    auto bg = [&](Surface x) { return cssColor(s.background(x)); };
    auto fg = [&](Surface x) { return cssColor(s.text(x)); };
    std::string css;

    // The window itself paints nothing once any panel is see-through;
    // otherwise its default background would show through the panel instead
    // of the desktop.
    if (!s.windowIsOpaque())
        css += "window.minicode-window, window.minicode-window.background "
               "{ background-color: transparent; }";

    // Title bar and menu bar. Left to the desktop theme unless asked for, the
    // same rule as the macOS build's customTitlebar.
    if (s.customTitlebar()) {
        css += "window.minicode-window .titlebar, window.minicode-window headerbar,"
               " window.minicode-window menubar {"
               " background-color: " + bg(Surface::Titlebar) + ";"
               " background-image: none; box-shadow: none; color: " +
               fg(Surface::Titlebar) + "; }";
        css += "window.minicode-window menubar > item,"
               " window.minicode-window headerbar label { color: " +
               fg(Surface::Titlebar) + "; }";
    }

    // Editor. The text node paints the background; the view and its scroller
    // stay clear so a translucent color is only painted once.
    // The text color has to be on the view node: GtkTextView takes its default
    // foreground from there, not from the text node, so under a light desktop
    // theme plain source text came out black.
    css += ".minicode-editor { background-color: transparent; color: " +
           fg(Surface::Editor) + "; }";
    css += ".minicode-editor text { background-color: " + bg(Surface::Editor) +
           "; color: " + fg(Surface::Editor) + "; }";
    css += ".minicode-editor text selection { background-color: #264F78; }";
    css += ".minicode-editor-scroller { background-color: transparent; }";
    // Images and PDFs sit on the editor's background, in the editor's slot.
    css += ".minicode-media { background-color: " + bg(Surface::Editor) + "; }";
    css += ".minicode-pdf-page { box-shadow: 0 1px 4px rgba(0,0,0,0.5); }";
    // Markdown preview and plain messages: proportional body text at the
    // macOS body size (15pt). Code and table runs re-apply monospace via
    // their GtkTextTags.
    css += ".minicode-prose text { font-family: sans-serif; font-size: 15pt; }";

    // File tree: the scroller paints, the list and its rows stay clear.
    css += ".minicode-sidebar { background-color: " + bg(Surface::Sidebar) + "; }";
    css += ".minicode-tree, .minicode-tree > row { background-color: transparent; }";
    css += ".minicode-tree-label { color: " + fg(Surface::Sidebar) + "; }";
    css += ".minicode-dir-icon  { color: #C09553; }";
    css += ".minicode-file-icon { color: #8A99A8; }";

    css += ".minicode-status { background-color: " + bg(Surface::Statusbar) +
           "; color: " + fg(Surface::Statusbar) +
           "; padding: 2px 8px; font-size: 12px; }";
    css += ".minicode-status label { color: " + fg(Surface::Statusbar) + "; }";
    // The LaTeX preview's own status line carries a button. It is kept to the
    // line's height, or the preview's minimum height outgrows a short pane.
    css += ".minicode-latex-bar button { min-height: 0; padding: 1px 8px; color: " +
           fg(Surface::Statusbar) + "; }";

    // Terminal. VTE paints its own background from the color set in
    // Terminal::applySettings, so the panel behind it stays clear. The 6px
    // inset matches the macOS terminal; VTE takes it from the CSS padding.
    css += ".minicode-terminal { background-color: transparent; }";
    // The theme gives vte-terminal a background of its own (white in a light
    // theme), which would show through a translucent terminal color.
    css += ".minicode-terminal vte-terminal { padding: 6px; background-color: transparent; }";

    // Browser toolbar. The URL field keeps the theme's colors unless a text
    // color was asked for, as on macOS.
    css += ".minicode-browser-bar { background-color: " + bg(Surface::Browser) +
           "; padding: 4px; }";
    if (s.textIsSet(Surface::Browser))
        css += ".minicode-browser-bar entry { color: " + fg(Surface::Browser) + "; }";

    // Pane dividers. GTK's default is a hairline in the desktop theme's color:
    // invisible against dark chrome, and a poor drag target. 4px in the divider
    // gray, lighting up blue under the pointer, says "drag me".
    // The theme pads a thin handle out into a wider, invisible grab zone that
    // overlaps the neighboring panels; content-box keeps the color off that
    // zone, which would otherwise show through a see-through panel.
    css += ".minicode-split > separator { min-width: 4px; min-height: 4px;"
           " background-color: #333333; background-clip: content-box; }";
    css += ".minicode-split > separator:hover { background-color: #007ACC; }";

    // Scrollbars over the dark panels. The desktop theme's are tuned for a
    // light background and all but disappear on #1E1E1E.
    css += ".minicode-scroller scrollbar { background-color: transparent; border: none; }";
    css += ".minicode-scroller scrollbar slider { background-color: rgba(255,255,255,0.22);"
           " border: none; min-width: 8px; min-height: 8px; }";
    css += ".minicode-scroller scrollbar slider:hover {"
           " background-color: rgba(255,255,255,0.38); }";

    // The floating shortcut list: an overlay, not a pane, so not configurable.
    css += ".minicode-hints { background-color: #252526; border: 1px solid #333333;"
           " border-radius: 8px; padding: 14px 18px; color: #D4D4D4;"
           " font-family: monospace; box-shadow: 0 6px 20px rgba(0,0,0,0.55); }";
    return css;
}

}  // namespace theme
