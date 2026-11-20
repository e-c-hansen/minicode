// SidebarPanels.h — pure C++, header only. What the sidebar's row of panel
// icons says and does (workbench.sidebar-icons; the Mac's #225, drawn by
// mac/src/SidebarIcons.mm and linux/src/SidebarIcons.cpp): one icon for each
// panel that takes the sidebar's place, the file tree, Source Control,
// Agents, and Processes, in that order; the panel on show; each icon's
// tooltip, its panel's name and key; the count a badge shows; and what a
// click on an icon does. The ports draw it and run the toggles.
#pragma once

#include <cstddef>
#include <string>

namespace SidebarPanels {

enum class Panel { Tree = 0, SourceControl, Agents, Processes };
constexpr int kCount = 4;

struct Info {
    const char* name;        // the tooltip's first words
    const char* commandId;   // the panel's toggle in the registry; null for the tree
};

// The panel's name and its toggle's command id (src/Commands.cpp). The tree
// has none: Focus File Tree focuses whichever panel shows, so it is not the
// tree's key.
inline const Info& info(Panel p) {
    static const Info kInfo[kCount] = {
        {"File Tree", nullptr},
        {"Source Control", "workbench.view.scm"},
        {"Agents", "mutible.toggleAgents"},
        {"Processes", "mutible.toggleProcesses"},
    };
    const int i = (int)p;
    return kInfo[i >= 0 && i < kCount ? i : 0];
}

// A badge's text: "" for none, the count up to 99, then "99+".
inline std::string badgeText(size_t n) {
    if (n == 0) return std::string();
    return n > 99 ? std::string("99+") : std::to_string(n);
}

// An icon's tooltip, its panel's name and its key as the port spells keys:
// "Source Control ⇧⌘G" on the Mac, "Source Control (Ctrl+Shift+G)"
// elsewhere, as the ports' other tooltips; the name alone with no key
// (keys.conf took it, or the tree).
inline std::string tip(Panel p, const std::string& key, bool mac) {
    const std::string name = info(p).name;
    if (key.empty()) return name;
    return mac ? name + " " + key : name + " (" + key + ")";
}

// The panel on show, from the ports' flags. Each toggle closes the others,
// so at most one is set; were two set, Processes, then Agents, then Source
// Control is the one drawn on top, as the Mac's sidebarPanelOnShow reads.
inline Panel onShow(bool sourceControl, bool agents, bool processes) {
    if (processes) return Panel::Processes;
    if (agents) return Panel::Agents;
    if (sourceControl) return Panel::SourceControl;
    return Panel::Tree;
}

// What a click on `clicked` does while `shown` is on show: the icon of the
// panel on show gives that panel the keyboard (Focus File Tree's way);
// another panel's runs its toggle, which closes the one on show; the tree's
// runs the toggle of the panel on show, which gives the tree back.
struct Click {
    bool focus = false;          // give the panel on show the keyboard
    Panel toggle = Panel::Tree;  // else run this panel's toggle
};
inline Click click(Panel shown, Panel clicked) {
    Click c;
    if (clicked == shown) c.focus = true;
    else c.toggle = clicked == Panel::Tree ? shown : clicked;
    return c;
}

}  // namespace SidebarPanels
