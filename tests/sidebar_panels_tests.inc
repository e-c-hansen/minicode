// sidebar_panels_tests.inc — the `sidebar-panels:*` groups of
// run_tests.cpp: the sidebar's row of panel icons (src/SidebarPanels.h,
// workbench.sidebar-icons): the panels in order with their toggles'
// command ids, which the registry has; the badges' text; the tooltips in
// each port's spelling; the panel on show; and what a click does. Included
// there.
#include "SidebarPanels.h"

static void testSidebarPanels() {
    using SidebarPanels::Panel;
    GROUP("sidebar-panels:info");
    {
        using SidebarPanels::info;
        CHECK(std::string(info(Panel::Tree).name) == "File Tree" && !info(Panel::Tree).commandId);
        CHECK(std::string(info(Panel::SourceControl).name) == "Source Control");
        CHECK(std::string(info(Panel::Agents).name) == "Agents");
        CHECK(std::string(info(Panel::Processes).name) == "Processes");
        // Each panel's toggle is a command of the registry, so its key is
        // what keys.conf leaves it.
        for (Panel p : {Panel::SourceControl, Panel::Agents, Panel::Processes}) {
            const char* id = info(p).commandId;
            CHECK(id && Commands::find(id) != nullptr);
            if (id && !Commands::find(id)) std::printf("    no command %s\n", id);
        }
        CHECK(SidebarPanels::kCount == 4 && (int)Panel::Processes == SidebarPanels::kCount - 1);
    }
    GROUP("sidebar-panels:badge");
    {
        using SidebarPanels::badgeText;
        CHECK(badgeText(0) == "");
        CHECK(badgeText(1) == "1");
        CHECK(badgeText(13) == "13");
        CHECK(badgeText(99) == "99");
        CHECK(badgeText(100) == "99+");
        CHECK(badgeText(4000) == "99+");
    }
    GROUP("sidebar-panels:tip");
    {
        using SidebarPanels::tip;
        CHECK(tip(Panel::SourceControl, "\xE2\x87\xA7\xE2\x8C\x98G", true) ==
              "Source Control \xE2\x87\xA7\xE2\x8C\x98G");
        CHECK(tip(Panel::SourceControl, "Ctrl+Shift+G", false) == "Source Control (Ctrl+Shift+G)");
        CHECK(tip(Panel::Agents, "", false) == "Agents");   // keys.conf took its key
        CHECK(tip(Panel::Tree, "", true) == "File Tree");
    }
    GROUP("sidebar-panels:on-show");
    {
        using SidebarPanels::onShow;
        CHECK(onShow(false, false, false) == Panel::Tree);
        CHECK(onShow(true, false, false) == Panel::SourceControl);
        CHECK(onShow(false, true, false) == Panel::Agents);
        CHECK(onShow(false, false, true) == Panel::Processes);
        CHECK(onShow(true, true, false) == Panel::Agents);
        CHECK(onShow(true, true, true) == Panel::Processes);
    }
    GROUP("sidebar-panels:click");
    {
        using SidebarPanels::click;
        // The icon of the panel on show: the keyboard to it, no toggle.
        for (Panel p : {Panel::Tree, Panel::SourceControl, Panel::Agents, Panel::Processes})
            CHECK(click(p, p).focus);
        // Another panel's: its own toggle, which closes the one on show.
        CHECK(!click(Panel::Tree, Panel::SourceControl).focus &&
              click(Panel::Tree, Panel::SourceControl).toggle == Panel::SourceControl);
        CHECK(click(Panel::SourceControl, Panel::Processes).toggle == Panel::Processes);
        CHECK(click(Panel::Processes, Panel::Agents).toggle == Panel::Agents);
        // The tree's: the toggle of the panel on show, which gives the tree back.
        CHECK(!click(Panel::Agents, Panel::Tree).focus &&
              click(Panel::Agents, Panel::Tree).toggle == Panel::Agents);
        CHECK(click(Panel::Processes, Panel::Tree).toggle == Panel::Processes);
        CHECK(click(Panel::SourceControl, Panel::Tree).toggle == Panel::SourceControl);
    }
}
