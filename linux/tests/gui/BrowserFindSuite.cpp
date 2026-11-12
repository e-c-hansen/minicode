// BrowserFindSuite.cpp — find in the page and the page's zoom per site
// (`./build/minicode-guitest browser_find`, PARITY.md 4.5; the Mac's
// browser_find). Pages are files in the project folder.
//   - Ctrl+F with the keyboard in the page opens the page's bar, not the
//     editor's; typing finds and counts; Enter and Shift+Enter step; Esc
//     closes it and the page has the keyboard again;
//   - Ctrl+F with the keyboard in the address bar opens the page's bar too;
//   - Ctrl+F in the editor still opens the editor's bar, not the page's;
//   - zoom is kept per site (the host, "file" for local files): it follows
//     a tab switch, a new tab on the same site, and is written to the state
//     file; back to 100% forgets it.
#include "Harness.h"

#include "AppSeam.h"
#include "Browser.h"
#include "Editor.h"
#include "FindBar.h"
#include "BrowserPages.h"
#include "WindowLayout.h"

#include <cmath>
#include <string>

namespace {

Browser* browser() { return MCGuiApp().browser; }
std::string page(const std::string& n) { return "file://" + MCGuiProjectFile(n + ".html"); }
bool focusIn(GtkWidget* root) {
    GtkWidget* f = gtk_root_get_focus(GTK_ROOT(MCGuiApp().window));
    return f && root && (f == root || gtk_widget_is_ancestor(f, root));
}
bool zoomIs(double z) {
    return browser()->webView() && std::fabs(browser()->zoomLevel() - z) < 0.01;
}

}  // namespace

MC_GUI_SUITE(browser_find) {
    MCGuiStep("pages are written and the pane is shown", [] {
        GCHECK(browser() != nullptr);
        MCGuiWriteFile(MCGuiProjectFile("find.html"),
                       "<title>Find</title><p>needle one</p><p>hay</p><p>Needle two</p><p>needle three</p>");
        MCGuiWriteFile(MCGuiProjectFile("zoom2.html"), "<title>Zoom2</title><p>second</p>");
        MCGuiWriteFile(MCGuiProjectFile("note.txt"), "alpha\nbeta\n");
        MCGuiApp().layout->showBrowser();
    });
    MCGuiWait("the first page is made", 60, [] { return browser()->userProbe() != nullptr; });
    MCGuiStep("a page with three needles", [] { browser()->load(page("find")); });
    MCGuiWait("it is loaded", 60, [] { return browser()->userProbe()->title() == "Find"; });
    MCGuiStep("the site's key is the host, or file for local files", [] {
        GCHECK_EQ_STR(Browser::siteOf("https://docs.example.org:8080/a?b#c"), "docs.example.org");
        GCHECK_EQ_STR(Browser::siteOf("file:///tmp/x.html"), "file");
        GCHECK_EQ_STR(Browser::siteOf("about:blank"), "");
        gtk_widget_grab_focus(browser()->webView());
    });
    MCGuiWait("the page has the keyboard", 30, [] { return focusIn(browser()->webView()); });
    MCGuiStep("Ctrl+F in the page", [] {
        GCHECK(!browser()->findShown());
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the page's bar is up with the keyboard in its query", 30, [] {
        return browser()->findShown() && focusIn(browser()->findEntry());
    });
    MCGuiStep("the editor's bar did not open", [] { GCHECK(!MCGuiApp().findBar->shown()); });
    MCGuiStep("typing \"needle\"", [] { MCGuiType("needle"); });
    MCGuiWait("three matches, case ignored", 30, [] {
        static int n = 0;
        if (++n % 100 == 0)
            MCGuiNote("query [%s] count [%s]", gtk_editable_get_text(GTK_EDITABLE(browser()->findEntry())),
                      browser()->findCountText().c_str());
        return browser()->findCountText() == "3 matches";
    });
    MCGuiStep("Enter and Shift+Enter step without losing the bar", [] {
        MCGuiKey("return");
        MCGuiKey("shift+return");
    });
    MCGuiPause(0.3);   // negative check: the count and bar stay
    MCGuiStep("the count is still 3 and the bar is up", [] {
        GCHECK_EQ_STR(browser()->findCountText(), "3 matches");
        GCHECK(browser()->findShown());
    });
    MCGuiStep("a query that is not there", [] {
        gtk_editable_set_text(GTK_EDITABLE(browser()->findEntry()), "zzzz");
    });
    MCGuiWait("No results", 30, [] { return browser()->findCountText() == "No results"; });
    MCGuiStep("Esc", [] { MCGuiKey("escape"); });
    MCGuiWait("the bar closes and the page has the keyboard", 30, [] {
        return !browser()->findShown() && focusIn(browser()->webView());
    });
    MCGuiStep("Ctrl+F from the address bar", [] {
        browser()->focusUrlBar();
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("opens the page's bar, not the editor's", 30, [] {
        return browser()->findShown() && focusIn(browser()->findEntry()) && !MCGuiApp().findBar->shown();
    });
    MCGuiStep("Esc", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 30, [] { return !browser()->findShown(); });

    // ---- the editor's Ctrl+F is its own
    MCGuiStep("a file in the editor, the keyboard in it", [] {
        MCGuiOpenFile(MCGuiProjectFile("note.txt"));
    });
    MCGuiWait("it shows", 30, [] { return MCGuiApp().editor->currentPath() == MCGuiProjectFile("note.txt"); });
    MCGuiStep("Ctrl+F", [] {
        gtk_widget_grab_focus(MCGuiApp().editor->textView());
        MCGuiKey("ctrl+f");
    });
    MCGuiWait("the editor's bar opens, the page's does not", 30, [] { return MCGuiApp().findBar->shown(); });
    MCGuiStep("the page's bar is closed", [] { GCHECK(!browser()->findShown()); });
    MCGuiStep("Esc closes the editor's bar", [] { MCGuiKey("escape"); });
    MCGuiWait("closed", 30, [] { return !MCGuiApp().findBar->shown(); });

    // ---- zoom per site
    MCGuiStep("zoom in once with the keyboard in the page", [] {
        GCHECK(zoomIs(1.0));
        gtk_widget_grab_focus(browser()->webView());
    });
    MCGuiWait("the page has the keyboard", 30, [] { return focusIn(browser()->webView()); });
    MCGuiStep("Ctrl+=", [] { MCGuiKey("ctrl+equal"); });
    MCGuiWait("115%", 30, [] { return zoomIs(1.15); });
    MCGuiStep("a new tab on the same site", [] { browser()->newTab(page("zoom2")); });
    MCGuiWait("it loads at the site's zoom", 60, [] {
        return browser()->userProbe() && browser()->userProbe()->title() == "Zoom2" && zoomIs(1.15);
    });
    MCGuiStep("back to the first tab (tab 2 and 1 live), then the state file", [] {
        browser()->selectTab(0);
        const std::string f = MCGuiReadFile(std::string(g_get_user_state_dir()) + "/minicode/browser-zoom.txt");
        GCHECK(f.find("file\t1.15") != std::string::npos);
    });
    MCGuiWait("the first tab is still at 115%", 30, [] { return zoomIs(1.15); });
    MCGuiStep("Ctrl+Alt+0 forgets it", [] { browser()->zoom(0); });
    MCGuiWait("100%", 30, [] { return zoomIs(1.0); });
    MCGuiStep("the state file no longer names the site", [] {
        const std::string f = MCGuiReadFile(std::string(g_get_user_state_dir()) + "/minicode/browser-zoom.txt");
        GCHECK(f.find("file\t") == std::string::npos);
        GCHECK(Browser::siteZoom("file") == 1.0);
    });
    MCGuiStep("leaves one tab, as the suites after it expect", [] {
        while (browser()->tabCount() > 1) browser()->closeTab(browser()->tabCount() - 1);
        browser()->load("about:blank");
    });
}
