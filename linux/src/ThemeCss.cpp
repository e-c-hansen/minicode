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

// VS Code's editor.selectionBackground in Dark+ and in Light+, by the
// theme's darkness as drawn (View > Theme, PARITY.md 2.11).
Rgba editorSelection(const Settings& s) { return Rgba::hex(s.isDark() ? 0x264F78 : 0xADD6FF); }

std::string stylesheet(const Settings& s) {
    auto bg = [&](Surface x) { return cssColor(s.background(x)); };
    auto fg = [&](Surface x) { return cssColor(s.text(x)); };
    // The few colors no key sets follow the theme's darkness: the selection
    // is VS Code's editor.selectionBackground in Dark+ and Light+, and the
    // scroll bars and the close button's light are white or black tints.
    const bool dark = s.isDark();
    const std::string selection = dark ? "#264F78" : "#ADD6FF";   // editorSelection's
    const std::string slider = dark ? "rgba(255,255,255," : "rgba(0,0,0,";
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
    css += ".minicode-editor text selection { background-color: " + selection + "; }";
    css += ".minicode-editor-scroller { background-color: transparent; }";
    // The split editor's other group (Groups.cpp) shares its document's
    // buffer, so the selection there is the focused group's: not drawn.
    css += ".minicode-editor.minicode-mirror text selection { background-color: transparent; }";
    css += ".minicode-mirror-note { color: alpha(currentColor, 0.6); }";
    // The line-number gutter (Gutter.cpp) paints editor.gutter-background
    // itself; the view's border window under it stays clear.
    css += ".minicode-editor > border { background-color: transparent; }";
    // The tabs above the editor, as the Mac's strip draws them: the strip in
    // the sidebar's color, the active tab in the editor's with an accent
    // line along its top, so it runs into the text; the others' titles in a
    // dimmer sidebar text; the preview tab's title in italics; the close
    // button lit under the pointer, and a dot in its place for unsaved
    // changes.
    Rgba dim = s.text(Surface::Sidebar);
    dim.a *= 0.7;
    css += ".minicode-tabs { background-color: " + bg(Surface::Sidebar) +
           "; border-bottom: 1px solid #333333; }";
    css += ".minicode-tab { padding: 5px 6px 5px 12px; border-right: 1px solid #333333;"
           " font-size: 12px; }";
    css += ".minicode-tab label { color: " + cssColor(dim) + "; }";
    css += ".minicode-tab.active { background-color: " + bg(Surface::Editor) +
           "; box-shadow: inset 0 2px #4EA1F7; }";
    css += ".minicode-tab.active label { color: " + fg(Surface::Editor) + "; }";
    css += ".minicode-tab.preview .minicode-tab-title { font-style: italic; }";
    css += ".minicode-tab-close { min-width: 16px; padding: 0 2px; border-radius: 3px; }";
    css += ".minicode-tab-close:hover { background-color: " + std::string(dark ? "rgba(255,255,255,0.12)" : "rgba(0,0,0,0.10)") + "; }";
    // Images and PDFs sit on the editor's background, in the editor's slot.
    css += ".minicode-media { background-color: " + bg(Surface::Editor) + "; }";
    css += ".minicode-pdf-page { box-shadow: 0 1px 4px rgba(0,0,0,0.5); }";
    // An audio file's name, and why a video or audio file cannot play.
    css += ".minicode-media-note { color: " + fg(Surface::Editor) + "; font-size: 12pt; }";
    // The zoom level, for a moment after it changes, in a PDF's corner.
    css += ".minicode-zoom-badge { background-color: rgba(30,30,30,0.85); color: #E0E0E0;"
           " border-radius: 6px; padding: 4px 10px; font-size: 12px; }";
    // Markdown preview and plain messages: proportional body text at the
    // macOS body size (15pt). Code and table runs re-apply monospace via
    // their GtkTextTags.
    css += ".minicode-prose text { font-family: sans-serif; font-size: 15pt; }";
    // Markdown tables: a grid of cells, lines between them drawn once (each
    // cell draws its right and bottom edges, the table its top and left),
    // the header row shaded, text in the editor's colors.
    css += ".minicode-md-table { border-top: 1px solid #444444; border-left: 1px solid #444444; }";
    css += ".minicode-md-cell { border-right: 1px solid #444444; border-bottom: 1px solid #444444;"
           " padding: 4px 8px; }";
    css += ".minicode-md-cell label { color: " + fg(Surface::Editor) + "; }";
    css += ".minicode-md-head { background-color: " + cssColor(s.markdownCodeBackground()) + "; }";
    css += ".minicode-md-head label { color: " + cssColor(s.markdown(MarkdownColor::Heading)) + "; }";
    // A task's box in the Markdown preview: an outline in the done color,
    // filled with the accent blue and a white tick when checked, in place of
    // the desktop theme's light check box.
    css += "checkbutton.minicode-md-task { padding: 0; margin: 0 4px 0 0; min-height: 0;"
           " background: none; }";
    css += "checkbutton.minicode-md-task check { background-image: none; background-color:"
           " transparent; border: 1.5px solid " + cssColor(s.markdown(MarkdownColor::Done)) +
           "; border-radius: 3px; box-shadow: none; min-width: 13px; min-height: 13px;"
           " margin: 0; padding: 0; color: transparent; -gtk-icon-size: 12px; }";
    css += "checkbutton.minicode-md-task:hover check { border-color: #4EA1F7; }";
    css += "checkbutton.minicode-md-task check:checked { background-color: #4EA1F7;"
           " border-color: #4EA1F7; color: #FFFFFF; }";

    // File tree: the scroller paints, the list and its rows stay clear.
    css += ".minicode-sidebar { background-color: " + bg(Surface::Sidebar) + "; }";
    css += ".minicode-tree, .minicode-tree > row { background-color: transparent; }";
    // The selected row. The rule above cleared the desktop theme's selection
    // color along with the rest, since this stylesheet outranks the theme
    // whatever the selectors, so a clicked row looked like every other one.
    // The blue of VS Code's list while the tree has the keyboard, and a tint
    // of the sidebar's own text color when it does not, so it shows on any
    // sidebar color the settings give; translucent, so a see-through sidebar
    // stays see-through. The focus ring is for keyboard use (GTK shows it
    // only after a key press).
    Rgba tint = s.text(Surface::Sidebar);
    auto tinted = [&](double a) { tint.a = a; return cssColor(tint); };
    css += ".minicode-tree > row:hover { background-color: " + tinted(0.04) + "; }";
    css += ".minicode-tree > row:selected { background-color: " + tinted(0.20) + "; }";
    css += ".minicode-tree:focus-within > row:selected {"
           " background-color: rgba(0,122,204,0.50); }";
    css += ".minicode-tree > row:focus-visible { outline: 1px solid #007ACC;"
           " outline-offset: -1px; }";
    // Where a dragged row or file would land (FileTree's drag and drop): the
    // folder's row in the accent blue, or the whole tree for the root. The
    // theme's own drop outline around the list is turned off. Keyed on
    // :drop(active) so the rule outranks the selected row's color above.
    css += ".minicode-tree:drop(active) { box-shadow: none; outline: none; }";
    css += ".minicode-tree:drop(active) > row.minicode-drop-target {"
           " background-color: rgba(78,161,247,0.30); box-shadow: inset 0 0 0 1px #4EA1F7; }";
    css += ".minicode-tree.minicode-drop-root:drop(active) {"
           " outline: 2px solid #4EA1F7; outline-offset: -2px; }";
    css += ".minicode-tree-label { color: " + fg(Surface::Sidebar) + "; }";
    // The rows' git colors (TreeGitColors.h): the tree.git-* keys, VS
    // Code's dark or light defaults by the theme.
    const struct { const char* cls; TreeGitColor color; } gitRows[] = {
        {"modified", TreeGitColor::Modified}, {"added", TreeGitColor::Added},
        {"untracked", TreeGitColor::Untracked}, {"deleted", TreeGitColor::Deleted},
        {"ignored", TreeGitColor::Ignored}, {"conflict", TreeGitColor::Conflict}};
    for (const auto& r : gitRows)
        css += std::string(".minicode-tree-label.minicode-git-") + r.cls + " { color: " +
               cssColor(s.treeGitColor(r.color)) + "; }";
    // The folder arrows, which otherwise take the desktop theme's text color
    // (black under a light theme, nearly invisible on the dark sidebar).
    css += ".minicode-tree expander { color: " + fg(Surface::Sidebar) + "; }";
    // The Source Control panel, in the tree's place: the sidebar's colors,
    // painted once by the panel. Its accents are the Mac's inline hex.
    css += ".minicode-git { background-color: " + bg(Surface::Sidebar) + "; color: " +
           fg(Surface::Sidebar) + "; }";
    // The Processes panel, in the same place with the same colors.
    css += ".minicode-procs, .minicode-procs list, .minicode-procs row { background-color: " +
           bg(Surface::Sidebar) + "; color: " + fg(Surface::Sidebar) + "; }";
    css += ".minicode-procs row:selected { background-color: " + selection + "; }";
    css += ".minicode-procs label { color: " + fg(Surface::Sidebar) + "; font-size: 12px; }";
    css += ".minicode-procs label.minicode-procs-info { color: #9CA3AF; font-size: 11px; }";
    css += ".minicode-procs label.minicode-procs-port { color: #4EA1F7; font-size: 11px; }";
    css += ".minicode-procs label.minicode-procs-gone { color: #6B7280; }";
    css += ".minicode-procs label.minicode-procs-group { font-weight: 600; }";
    css += ".minicode-procs label.minicode-procs-heading { color: #9CA3AF; font-size: 10px;"
           " font-weight: 600; letter-spacing: 0.6px; }";
    css += ".minicode-git label { color: " + fg(Surface::Sidebar) + "; }";
    css += ".minicode-git label.minicode-git-branch { font-weight: 600; font-size: 12px; }";
    css += ".minicode-git label.minicode-git-error { color: #F48771; font-size: 11px; }";
    css += ".minicode-git label.minicode-git-info,"
           " .minicode-git label.minicode-git-summary { color: #9CA3AF; font-size: 11px; }";
    css += ".minicode-git label.minicode-git-notice { color: #9CA3AF; font-size: 12px; }";
    css += ".minicode-git label.minicode-git-heading { color: #9CA3AF; font-size: 10px;"
           " font-weight: 600; letter-spacing: 0.6px; }";
    css += ".minicode-git label.minicode-git-placeholder { color: #6B7280; font-size: 12px; }";
    css += ".minicode-git-message { border: 1px solid #3C3C3C; border-radius: 4px;"
           " background-color: " + bg(Surface::Editor) + "; }";
    css += ".minicode-git-message:focus-within { border-color: #007ACC; }";
    css += ".minicode-git-message scrolledwindow, .minicode-git-message textview,"
           " .minicode-git-message text { background-color: transparent; color: " +
           fg(Surface::Sidebar) + "; font-size: 12px; }";
    css += ".minicode-git-message text selection { background-color: " + selection + "; }";
    css += ".minicode-git checkbutton label { color: #9CA3AF; font-size: 11px; }";
    // The button and the check box would otherwise take the desktop theme's
    // light look on the dark sidebar; tints of the sidebar's text fit any.
    css += ".minicode-git button.minicode-git-commit { background-image: none;"
           " background-color: " + tinted(0.12) + "; color: " + fg(Surface::Sidebar) + ";"
           " border: 1px solid " + tinted(0.18) + "; box-shadow: none; min-height: 24px; }";
    css += ".minicode-git button.minicode-git-commit:hover { background-color: " +
           tinted(0.18) + "; }";
    css += ".minicode-git button.minicode-git-commit:active { background-color: " +
           tinted(0.26) + "; }";
    css += ".minicode-git button.minicode-git-commit label { color: " + fg(Surface::Sidebar) +
           "; }";
    css += ".minicode-git checkbutton check { background-image: none; background-color: "
           "transparent; border: 1px solid " + tinted(0.45) + "; color: " +
           fg(Surface::Sidebar) + "; box-shadow: none; min-width: 12px; min-height: 12px; }";
    css += ".minicode-git checkbutton check:checked { background-color: #007ACC;"
           " border-color: #007ACC; color: #FFFFFF; }";
    css += ".minicode-git-rule { background-color: #333333; min-height: 1px; }";
    css += ".minicode-git-list, .minicode-git-list > row { background-color: transparent;"
           " padding: 0; min-height: 0; }";
    css += ".minicode-git-list > row:hover { background-color: " + tinted(0.04) + "; }";
    css += ".minicode-git-list > row:selected { background-color: " + tinted(0.20) + "; }";
    css += ".minicode-git-list:focus-within > row:selected {"
           " background-color: rgba(0,122,204,0.50); }";
    css += ".minicode-git-list > row:focus-visible { outline: 1px solid #007ACC;"
           " outline-offset: -1px; }";
    // The Problems list (PARITY.md 2.2): the terminal's surface, as the dock
    // it shares, with the sidebar's for its heading, as on the Mac.
    css += ".minicode-problems { background-color: " + bg(Surface::Terminal) + "; color: " +
           fg(Surface::Terminal) + "; border-top: 1px solid #333333; }";
    css += ".minicode-problems label { color: " + fg(Surface::Terminal) + "; font-size: 12px; }";
    css += ".minicode-problems-header { background-color: " + bg(Surface::Sidebar) +
           "; padding: 2px 6px 2px 12px; min-height: 22px; }";
    css += ".minicode-problems-header label { color: " + fg(Surface::Sidebar) + "; }";
    css += ".minicode-problems label.minicode-problems-title { font-size: 11px; font-weight: 600; }";
    css += ".minicode-problems label.minicode-problems-counts { font-size: 11px; opacity: 0.7; }";
    css += ".minicode-problems button.minicode-problems-close { min-height: 0; min-width: 0;"
           " padding: 0 6px; background: none; box-shadow: none; color: " +
           fg(Surface::Sidebar) + "; }";
    css += ".minicode-problems label.minicode-problems-muted,"
           " .minicode-problems label.minicode-problems-empty { opacity: 0.55; }";
    css += ".minicode-problems label.minicode-problems-empty { margin: 8px 0 0 46px; }";
    css += ".minicode-problems-list, .minicode-problems-list > row { background-color: transparent;"
           " padding: 0; min-height: 22px; }";
    css += ".minicode-problems-list > row:selected { background-color: " + tinted(0.20) + "; }";
    css += ".minicode-problems-list:focus-within > row:selected {"
           " background-color: rgba(0,122,204,0.50); }";
    css += ".minicode-problem-row { padding-right: 12px; }";
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
    // The sidebar's divider (WindowLayout.cpp) takes presses on its line and
    // 5 px before it, painted as the sidebar (the Mac's split view grabs 5 px
    // either side), and never over the editor, whose left edge holds the
    // gutter's lanes. The handle is a wide one, which GTK does not grow by its
    // own 6 px, and its box is all its own: with the theme's padding and that
    // 6 px it took presses 14 px into the editor, and a negative margin
    // instead made GtkPaned, which measures a drag from the handle's box,
    // jump 5 px at the press.
    css += ".minicode-split > separator { min-width: 4px; min-height: 4px; margin: 0;"
           " padding: 0 0 0 5px; border: none; background-image: none;"
           " background-color: " + bg(Surface::Sidebar) + ";"
           " box-shadow: inset -4px 0 #333333; }";
    css += ".minicode-split > separator:hover { box-shadow: inset -4px 0 #007ACC; }";
    css += ".minicode-split > separator:dir(rtl) { padding: 0 5px 0 0;"
           " box-shadow: inset 4px 0 #333333; }";
    css += ".minicode-split > separator:dir(rtl):hover { box-shadow: inset 4px 0 #007ACC; }";
    // The right side's bars (WindowLayout.cpp), the same 4px line in the
    // middle of a grab area 4px wider on each side that lies over the panes'
    // edges; lit while hovered or dragged.
    css += ".minicode-bar { background-color: #333333; background-clip: content-box; }";
    css += ".minicode-bar.vertical { padding: 0 4px; }";
    css += ".minicode-bar.horizontal { padding: 4px 0; }";
    css += ".minicode-bar:hover, .minicode-bar.dragging { background-color: #007ACC; }";

    // Scrollbars over the panels: white sliders on a dark theme, where the
    // desktop theme's all but disappear on #1E1E1E, and black ones on a
    // light theme (View > Theme, PARITY.md 2.11).
    css += ".minicode-scroller scrollbar { background-color: transparent; border: none; }";
    css += ".minicode-scroller scrollbar slider { background-color: " + slider + "0.22);"
           " border: none; min-width: 8px; min-height: 8px; }";
    css += ".minicode-scroller scrollbar slider:hover {"
           " background-color: " + slider + "0.38); }";

    // The floating shortcut list: an overlay, not a pane, so not configurable.
    css += ".minicode-hints { background-color: #252526; border: 1px solid #333333;"
           " border-radius: 8px; padding: 14px 18px; color: #D4D4D4;"
           " font-family: monospace; box-shadow: 0 6px 20px rgba(0,0,0,0.55); }";
    return css;
}

}  // namespace theme
