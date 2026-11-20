// SidebarIcons.h — the sidebar's row of panel icons on Linux
// (workbench.sidebar-icons; the Mac's #225, mac/src/SidebarIcons.mm): one
// icon for each panel that takes the sidebar's place, the file tree, Source
// Control, Agents, and Processes (the core's SidebarPanels.h, which says
// what each icon's tooltip is and what a click does), so they can be seen
// and clicked as VS Code's and Zed's can, not only reached by their keys.
//
// The row is quiet: the icon theme's symbolic icons in the sidebar's text
// color (ThemeCss.cpp), dimmed but for the panel on show and the one under
// the pointer, the panel on show underlined in the accent; a count of
// changes on Source Control, an orange dot on Agents while a session waits,
// and a count of running servers on Processes. Each icon's tooltip names its
// panel and key. The icons never take the keyboard. It draws only: main.cpp
// (updateSidebarIcons) says what each shows and runs the toggles.
//
// Each icon is the first name the display's icon theme has from a short
// list (Adwaita has no branch or processor icon; Yaru and Papirus have
// nearer ones), so no picture is mutible's own, as the tree's (TreeIcons.h).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

#include "SidebarPanels.h"

class SidebarIcons {
public:
    using Panel = SidebarPanels::Panel;

    SidebarIcons();
    // The widgets go with the window; this only lets go of the icon theme.
    ~SidebarIcons();
    SidebarIcons(const SidebarIcons&) = delete;
    SidebarIcons& operator=(const SidebarIcons&) = delete;

    // The row, to put above the sidebar's panels.
    GtkWidget* widget() const { return root_; }

    // A click on an icon.
    std::function<void(Panel)> onPick;

    // The panel on show, marked.
    void setCurrent(Panel p);
    Panel current() const { return current_; }
    // A count on the icon ("3", "99+"); "" for none.
    void setBadge(Panel p, const std::string& text);
    void setDot(Panel p, bool on);
    void setTip(Panel p, const std::string& tip);

    // For the checks: an icon's button, and what it shows.
    GtkWidget* button(Panel p) const { return item(p).button; }
    std::string badge(Panel p) const { return item(p).badgeText; }
    bool dot(Panel p) const { return item(p).dotOn; }
    std::string tip(Panel p) const { return item(p).tip; }
    // The icon name it draws, and whether the theme had one of its names.
    const char* iconName(Panel p) const { return item(p).icon; }
    bool iconFound(Panel p) const { return item(p).found; }
    // The widgets of the mark (the accent line), the badge and the dot.
    GtkWidget* markWidget(Panel p) const { return item(p).mark; }
    GtkWidget* badgeWidget(Panel p) const { return item(p).badge; }
    GtkWidget* dotWidget(Panel p) const { return item(p).dot; }

private:
    struct Item {
        GtkWidget* button = nullptr;
        GtkWidget* image = nullptr;
        GtkWidget* mark = nullptr;
        GtkWidget* badge = nullptr;
        GtkWidget* dot = nullptr;
        std::string badgeText, tip;
        bool dotOn = false;
        const char* icon = nullptr;
        bool found = false;
    };
    Item items_[SidebarPanels::kCount];
    Panel current_ = Panel::Tree;
    GtkWidget* root_ = nullptr;
    GtkIconTheme* theme_ = nullptr;
    gulong themeChanged_ = 0;

    const Item& item(Panel p) const;
    Item& item(Panel p);
    // Each icon the first of its names the theme has.
    void resolveIcons();
};
