// BrowserTabsSuite.cpp — the browser pane's tabs (`./build/minicode-guitest
// browser_tabs`, PARITY.md 4.5; the Mac's browser_tabs). Pages are files in
// the project folder; nothing leaves this machine.
//   - the pane starts with one tab (the home page), a + makes more, a click
//     on a tab shows it, a drag along the row reorders;
//   - with five tabs only two keep a web view, and a suspended tab comes
//     back at its address and title when shown;
//   - Ctrl+PageDown, Alt+N, Ctrl+T (in the page) and Ctrl+W (the tab; the
//     last hides the pane) with the keyboard in the pane;
//   - a target=_blank link opens beside its opener and is shown, a Ctrl-click
//     opens a suspended tab behind;
//   - the session record: every tab's address, title and the one on show,
//     through the sessions file's JSON and back into a pane, suspended.
#include "Harness.h"

#include "AppSeam.h"
#include "Browser.h"
#include "BrowserPages.h"
#include "Persistence.h"
#include "TabSessions.h"
#include "TabStrip.h"
#include "WindowLayout.h"

#include <string>

namespace {

Browser* browser() { return MCGuiApp().browser; }
std::string page(int n) { return "file://" + MCGuiProjectFile("p" + std::to_string(n) + ".html"); }
bool at(int tab, const std::string& what) {
    return browser()->tabUrl(tab).find(what) != std::string::npos;
}
bool shown() { return MCGuiApp().layout->browserShown(); }
int gBefore = 0;

}  // namespace

MC_GUI_SUITE(browser_tabs) {
    MCGuiStep("pages are written and the pane is shown", [] {
        GCHECK(browser() != nullptr);
        for (int n = 1; n <= 6; n++)
            MCGuiWriteFile(MCGuiProjectFile("p" + std::to_string(n) + ".html"),
                           "<title>Page " + std::to_string(n) + "</title><p>page " + std::to_string(n) + "</p>");
        MCGuiWriteFile(MCGuiProjectFile("links.html"),
                       "<title>Links</title><body style='margin:0'>"
                       "<a href='p2.html' target='_blank' style='display:block;height:60px;background:#ccc'>blank</a>"
                       "<a href='p3.html' style='display:block;height:60px;background:#aaa'>plain</a></body>");
        MCGuiApp().layout->showBrowser();
    });
    MCGuiWait("the first page is made", 60, [] { return browser()->userProbe() != nullptr; });
    MCGuiStep("one tab, the home page, in the strip", [] {
        GCHECK(browser()->tabCount() == 1);
        GCHECK(browser()->strip()->count() == 1);
        GCHECK(browser()->activeTab() == 0);
        GCHECK(browser()->tabIsLive(0));
        browser()->load(page(1));
    });
    MCGuiWait("page 1 is loaded", 60, [] { return browser()->userProbe()->title() == "Page 1"; });
    MCGuiStep("the tab is called by the page's title", [] {
        GCHECK(browser()->strip()->item(0).title == "Page 1");
        MCGuiClick(browser()->newTabButton());
    });
    MCGuiWait("the + made a tab, shown", 30, [] { return browser()->tabCount() == 2 && browser()->activeTab() == 1; });
    MCGuiWait("its page is made", 60, [] { return browser()->userProbe() != nullptr; });
    MCGuiStep("load page 2 there", [] { browser()->load(page(2)); });
    MCGuiWait("page 2 is loaded", 60, [] { return browser()->userProbe()->title() == "Page 2"; });
    MCGuiStep("a third tab", [] { browser()->newTab(); browser()->load(page(3)); });
    MCGuiWait("page 3 is loaded", 60, [] { return browser()->userProbe() && browser()->userProbe()->title() == "Page 3"; });
    MCGuiStep("a fourth tab", [] { browser()->newTab(); browser()->load(page(4)); });
    MCGuiWait("page 4 is loaded", 60, [] { return browser()->userProbe() && browser()->userProbe()->title() == "Page 4"; });
    MCGuiStep("a fifth tab", [] { browser()->newTab(); browser()->load(page(5)); });
    MCGuiWait("page 5 is loaded", 60, [] { return browser()->userProbe() && browser()->userProbe()->title() == "Page 5"; });
    MCGuiStep("five tabs, only two with a web view: the one on show and the one before", [] {
        GCHECK(browser()->tabCount() == 5);
        GCHECK(browser()->liveCount() == 2);
        GCHECK(browser()->tabIsLive(4));
        GCHECK(browser()->tabIsLive(3));
        GCHECK(!browser()->tabIsLive(0));
        GCHECK(at(0, "p1.html"));          // a suspended tab keeps its address
        GCHECK(browser()->tabTitle(0) == "Page 1");
        MCGuiClick(browser()->strip()->tabWidget(0), 20, -1);   // shows the first
    });
    MCGuiWait("the suspended tab is shown again, at its address and title", 60, [] {
        return browser()->activeTab() == 0 && browser()->tabIsLive(0) && browser()->userProbe() &&
               browser()->userProbe()->title() == "Page 1";
    });
    MCGuiStep("still only two live: the first and the one shown before it", [] {
        GCHECK(browser()->liveCount() == 2);
        GCHECK(browser()->userProbe()->url().find("p1.html") != std::string::npos);
        GCHECK(!browser()->tabIsLive(3));
        GCHECK(browser()->tabIsLive(4));
    });

    // ---- keys, with the keyboard in the pane
    MCGuiStep("Ctrl+PageDown steps to the next tab", [] {
        browser()->focusUrlBar();
        MCGuiKey("ctrl+pagedown");
    });
    MCGuiWait("tab 2 is on show", 30, [] { return browser()->activeTab() == 1; });
    MCGuiStep("Alt+3 selects the third", [] { MCGuiKey("alt+3"); });
    MCGuiWait("tab 3 is on show", 30, [] { return browser()->activeTab() == 2; });
    MCGuiStep("Ctrl+T in the page makes a tab right of it", [] {
        gBefore = browser()->tabCount();
        browser()->focusUrlBar();
        MCGuiKey("ctrl+t");
    });
    MCGuiWait("the new tab is there and shown", 30, [] {
        return browser()->tabCount() == gBefore + 1 && browser()->activeTab() == 3;
    });
    MCGuiStep("Ctrl+W closes it", [] {
        browser()->focusUrlBar();
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("it is gone and the pane stays", 30, [] { return browser()->tabCount() == gBefore && shown(); });
    MCGuiStep("the editor's tabs were not touched by those keys", [] { GCHECK(MCGuiApp().browser->tabCount() == 5); });

    // ---- dragging a tab along the row
    MCGuiStep("a drag of the first tab past the last", [] {
        GtkWidget* first = browser()->strip()->tabWidget(0);
        GtkWidget* last = browser()->strip()->tabWidget(browser()->strip()->count() - 1);
        graphene_point_t in = GRAPHENE_POINT_INIT(0, 0), out;
        gtk_widget_compute_point(last, first, &in, &out);
        const double h = gtk_widget_get_height(first) / 2.0;
        MCGuiDrag(first, 15, h, out.x + gtk_widget_get_width(last) - 4, h);
    });
    MCGuiWait("the first page is now the last tab", 30, [] { return at(4, "p1.html") && at(0, "p2.html"); });
    MCGuiStep("the tab on show moved with it", [] { GCHECK(browser()->activeTab() == 4); });

    // ---- links
    MCGuiStep("the links page in the tab on show", [] {
        browser()->load("file://" + MCGuiProjectFile("links.html"));
    });
    MCGuiWait("it is loaded", 60, [] { return browser()->userProbe()->title() == "Links"; });
    MCGuiStep("a target=_blank link is clicked", [] {
        gBefore = browser()->tabCount();
        MCGuiClick(browser()->webView(), 60, 30);
    });
    MCGuiWait("its page comes up beside the opener, shown", 60, [] {
        return browser()->tabCount() == gBefore + 1 && browser()->activeTab() == 5 &&
               browser()->userProbe() && browser()->userProbe()->title() == "Page 2";
    });
    MCGuiStep("only two keep a web view still; back to the opener", [] {
        GCHECK(browser()->liveCount() == 2);
        browser()->selectTab(4);
    });
    MCGuiWait("the opener is shown", 60, [] { return browser()->userProbe() && browser()->userProbe()->title() == "Links"; });
    MCGuiStep("a Ctrl-click on the plain link", [] {
        gBefore = browser()->tabCount();
        MCGuiClickWith("ctrl", browser()->webView(), 60, 90);
    });
    MCGuiWait("a tab opened behind, not shown, not loaded", 60, [] { return browser()->tabCount() == gBefore + 1; });
    MCGuiStep("it is right of the active tab and suspended", [] {
        GCHECK(browser()->activeTab() == 4);
        GCHECK(at(5, "p3.html"));
        GCHECK(!browser()->tabIsLive(5));
        GCHECK(browser()->userProbe()->title() == "Links");
    });

    // ---- the session record
    MCGuiStep("the session record round-trips through the sessions file's JSON", [] {
        int active = -1;
        const std::vector<TabSessions::BrowserTab> recs = browser()->records(&active);
        GCHECK(recs.size() == (size_t)browser()->tabCount());
        GCHECK(active == browser()->activeTab());
        TabSessions::Sessions all;
        TabSessions::Session s;
        s.browser = recs;
        s.browserActive = active;
        TabSessions::put(all, "/x", s, 1);
        const TabSessions::Sessions back = TabSessions::decode(TabSessions::encode(all));
        GCHECK(back.count("/x") == 1);
        const TabSessions::Session& r = back.at("/x");
        GCHECK(r.browser.size() == recs.size());
        GCHECK(r.browserActive == active);
        GCHECK(r.browser[1].url.find("p3.html") != std::string::npos);
        GCHECK(r.browser[active].title == "Links");
        // Into another pane: all suspended, none with a web view.
        Browser other("about:blank");
        other.restore(r.browser, r.browserActive);
        GCHECK(other.tabCount() == browser()->tabCount());
        GCHECK(other.activeTab() == active);
        GCHECK(other.liveCount() == 0);
        GCHECK(other.tabUrl(5) == browser()->tabUrl(5));
        GCHECK(other.strip()->item(active).title == "Links");
    });
    MCGuiWait("the window saved its session with the browser in it", 30, [] {
        return MCGuiReadFile(Persistence::sessionsFile()).find("\"browser\"") != std::string::npos;
    });

    // ---- closing
    MCGuiStep("closing down to one tab", [] {
        while (browser()->tabCount() > 1) browser()->closeTab(browser()->tabCount() - 1);
        GCHECK(browser()->tabCount() == 1);
        GCHECK(shown());
        GCHECK(browser()->liveCount() >= 1);
    });
    MCGuiWait("the last tab has a page", 60, [] { return browser()->userProbe() != nullptr; });
    MCGuiStep("Ctrl+W on the last tab hides the pane", [] {
        browser()->focusUrlBar();
        MCGuiKey("ctrl+w");
    });
    MCGuiWait("the pane is hidden, no tab left", 30, [] { return !shown() && browser()->tabCount() == 0; });
    MCGuiStep("showing the pane again starts a tab", [] { MCGuiApp().layout->showBrowser(); });
    MCGuiWait("one tab with a page", 60, [] { return shown() && browser()->tabCount() == 1 && browser()->userProbe() != nullptr; });
}
