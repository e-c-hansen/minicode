// Browser.h — a simple embedded web browser panel: a WebKitGTK web view with a
// URL entry and back / forward / reload buttons. Mirrors the macOS Browser.mm
// (a WKWebView with minimal chrome).
//
// Compiled only when MINICODE_ENABLE_BROWSER is defined (webkitgtk-6.0 found at
// configure time). Callers must guard use with the same macro.
#pragma once

#ifdef MINICODE_ENABLE_BROWSER

#include <gtk/gtk.h>
#include <webkit/webkit.h>   // WebKitWebView / WebKitLoadEvent in the signatures
#include <functional>
#include <string>
#include <vector>

#include "TabSessions.h"

class PageProbe;
class TabStrip;

class Browser {
public:
    // The panel's widget at once; the web view, and with it WebKit's
    // processes and the home page, when the panel is first shown or a page
    // is loaded into it (PARITY.md R3).
    explicit Browser(const std::string& homeUrl);
    ~Browser();
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;
    GtkWidget* widget() const { return root_; }
    // The web view, or null before the panel was first shown (for the checks).
    GtkWidget* webView() const;
    void focusUrlBar();
    // Go to a URL, or to what the URL bar would make of text typed into it.
    void load(const std::string& url);
    // Page Zoom on Safari's steps (ViewZoom::pageStep): in (+1), out (-1),
    // back to 100% (0). Not kept per site yet (PARITY.md 4.5).
    void zoom(int step);
    double zoomLevel() const;

    // ---- Tabs (PARITY.md 4.5; the Mac's BrowserView, src/BrowserTabs.h and
    // EditorTabs.h hold the rules). Only the tab on show and the one used
    // before it keep a web view; the rest are suspended to address, title
    // and WebKit's session state and load when shown. A new tab goes right
    // of the active one and is shown unless `show` is false (a link opened
    // behind). Returns its index.
    int newTab(const std::string& url = "", bool show = true);
    // Closes the agent page when it shows, else the tab on show; the last
    // tab's close calls onEmpty (the window hides the pane).
    void closeActive();
    void closeTab(int index);
    void selectTab(int index);
    void stepTab(int delta);          // wrapping
    void selectTabNumber(int n);      // 1 to 8, 9 the last
    void moveTab(int from, int to);
    int tabCount() const { return (int)tabs_.size(); }
    int activeTab() const { return active_; }
    bool tabIsLive(int index) const;  // keeps a web view
    int liveCount() const;
    std::string tabUrl(int index) const;
    std::string tabTitle(int index) const;
    TabStrip* strip() const { return strip_; }
    GtkWidget* newTabButton() const { return plus_; }
    // Called when the last tab is closed.
    std::function<void()> onEmpty;
    // Called when the tabs (or what they show) changed, for the session.
    std::function<void()> onChanged;
    // The window's session: every tab's address, title and WebKit's record
    // (BrowserTabs::keptRecords decides which records are kept), and the
    // tab to show. Old sessions have none; restore() takes them back
    // suspended, only the one on show loads, when the pane is shown.
    std::vector<TabSessions::BrowserTab> records(int* active);
    void restore(const std::vector<TabSessions::BrowserTab>& tabs, int active);

    // The agent page (PARITY.md 3.7): a second web view beside the user's,
    // made by `minicode browse`, with a Yours | Agent switcher and an x that
    // closes it. An address typed in the bar, or a link from elsewhere, goes
    // to the user's page, never the agent's.
    PageProbe* userProbe() const;
    PageProbe* agentProbe() const { return agentProbe_; }
    bool hasAgentPage() const { return agentProbe_ != nullptr; }
    bool agentPageShown() const;
    // The agent page, made when there is none (the switcher appears).
    PageProbe* openAgentPage();
    // Shows it (or the user's page), keeping the keyboard where it is.
    void showAgentPage(bool agent);
    void closeAgentPage();
    // For the checks.
    GtkWidget* yoursButton() const { return yours_; }
    GtkWidget* agentButton() const { return agentBtn_; }
    GtkWidget* closeButton() const { return close_; }
    GtkWidget* switcher() const { return switcher_; }

private:
    static void onGo(GtkWidget* w, gpointer self);
    static void onBack(GtkWidget* w, gpointer self);
    static void onForward(GtkWidget* w, gpointer self);
    static void onReload(GtkWidget* w, gpointer self);
    // notify::uri fires as soon as the URI is set, which is right for the URL
    // bar but too early for the buttons: WebKit has not updated the
    // back/forward list yet, so can_go_back/forward still read the previous
    // page's values. load-changed at WEBKIT_LOAD_COMMITTED is the point where
    // the list is current, and is what -webView:didCommitNavigation: maps to.

    struct Tab;
    void makeView(Tab* t, GtkWidget* related = nullptr);
    void adoptView(Tab* t, GtkWidget* web);
    void suspend(Tab* t);
    void destroyTab(Tab* t);
    void applyLive();
    void updateStrip();
    void noteChanged();
    Tab* current() const;
    int indexOfTab(const Tab* t) const;
    static void onTabUri(GObject* obj, GParamSpec* pspec, gpointer tab);
    static void onTabTitle(GObject* obj, GParamSpec* pspec, gpointer tab);
    static void onTabLoad(WebKitWebView* wv, WebKitLoadEvent ev, gpointer tab);
    void syncUrlBar();   // URL bar text only
    void syncChrome();   // URL bar text + back/forward sensitivity

    std::string home_;
    GtkWidget* root_  = nullptr;
    GtkWidget* entry_ = nullptr;
    std::vector<Tab*> tabs_;
    int active_ = -1;
    uint64_t clock_ = 0;
    bool shown_ = false;           // views may be made (the pane was shown or a page loaded)
    TabStrip* strip_ = nullptr;
    GtkWidget* plus_ = nullptr;
    GtkWidget* back_  = nullptr;
    GtkWidget* fwd_   = nullptr;
    GtkWidget* switcher_ = nullptr;   // Yours | Agent | x, shown with an agent page
    GtkWidget* yours_ = nullptr;
    GtkWidget* agentBtn_ = nullptr;
    GtkWidget* close_ = nullptr;
    GtkWidget* stack_ = nullptr;      // "yours" (userSlot_) and "agent"
    GtkWidget* userSlot_ = nullptr;   // holds web_ once it is made
    GtkWidget* agent_ = nullptr;
    PageProbe* agentProbe_ = nullptr;
    bool syncingSwitcher_ = false;
};

#endif // MINICODE_ENABLE_BROWSER
