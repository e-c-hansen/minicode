// Browser.cpp — see Browser.h. Compiled out unless MINICODE_ENABLE_BROWSER.
#include "Browser.h"

#ifdef MINICODE_ENABLE_BROWSER

#include <webkit/webkit.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "Appearance.h"
#include "BrowserPages.h"
#include "BrowserTabs.h"
#include "EditorTabs.h"
#include "TabStrip.h"
#include "StartupLog.h"
#include "WebBookmark.h"
#include "ViewZoom.h"

// The toolbar is made at once and the web view when the panel is first
// shown (makeView), as the Mac makes its browser on the first Toggle Browser
// (EditorController+Panels.mm, makeBrowserWithHome:): a WebKitWebView starts
// WebKit's network and web processes, each in a sandbox WebKit waits for,
// and loads its page, none of which a window opening with the panel hidden
// needs (PARITY.md R3).
Browser::Browser(const std::string& homeUrl) : home_(homeUrl) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Toolbar: back / forward / reload / url entry.
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bar, "minicode-browser-bar");   // colors: ThemeCss
    // Inset comes from CSS padding, not margins, so the toolbar's background
    // color reaches its edges.

    back_ = gtk_button_new_from_icon_name("go-previous-symbolic");
    fwd_  = gtk_button_new_from_icon_name("go-next-symbolic");
    GtkWidget* rel = gtk_button_new_from_icon_name("view-refresh-symbolic");
    GtkWidget* back = back_;
    GtkWidget* fwd  = fwd_;
    entry_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry_), homeUrl.c_str());

    gtk_box_append(GTK_BOX(bar), back);
    gtk_box_append(GTK_BOX(bar), fwd);
    gtk_box_append(GTK_BOX(bar), rel);
    gtk_box_append(GTK_BOX(bar), entry_);
    star_ = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_set_tooltip_text(star_, "Bookmark this page");
    gtk_widget_set_focus_on_click(star_, FALSE);
    gtk_widget_set_sensitive(star_, FALSE);
    book_ = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(book_), "user-bookmarks-symbolic");
    gtk_widget_set_tooltip_text(book_, "Bookmarks");
    gtk_widget_set_focus_on_click(book_, FALSE);
    gtk_box_append(GTK_BOX(bar), star_);
    gtk_box_append(GTK_BOX(bar), book_);
    g_signal_connect(star_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<Browser*>(p)->starClicked();
    }), this);
    // The model is read from the folder each time the book opens.
    gtk_menu_button_set_create_popup_func(
        GTK_MENU_BUTTON(book_),
        +[](GtkMenuButton* b, gpointer p) {
            Browser* self = static_cast<Browser*>(p);
            GMenu* m = self->bookMenu ? self->bookMenu() : nullptr;
            if (!m) return;
            gtk_menu_button_set_menu_model(b, G_MENU_MODEL(m));
            g_object_unref(m);
        },
        this, nullptr);

    // The tabs, above the toolbar: the editor's strip, dragged to reorder,
    // and a + for a new one.
    GtkWidget* tabsRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    strip_ = new TabStrip();
    plus_ = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(plus_, "New Browser Tab");
    gtk_widget_add_css_class(plus_, "flat");
    gtk_widget_set_focus_on_click(plus_, FALSE);
    gtk_box_append(GTK_BOX(tabsRow), strip_->widget());
    gtk_box_append(GTK_BOX(tabsRow), plus_);
    gtk_box_append(GTK_BOX(root_), tabsRow);
    g_signal_connect(plus_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        Browser* self = static_cast<Browser*>(p);
        self->newTab();
        self->focusUrlBar();
    }), this);
    strip_->onSelect = [this](int i) { selectTab(i); };
    strip_->onClose = [this](int i) { closeTab(i); };
    strip_->onMove = [this](int from, int to) { moveTab(from, to); };

    // The star and the book made the bar wider than the pane's smallest, 150
    // px (RightAreaLayout), which GTK would not allocate below: the bar is
    // clipped at the right instead, in a scroller of no scroll bar of its own.
    GtkWidget* clip = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(clip), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(clip), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(clip), bar);
    gtk_box_append(GTK_BOX(root_), clip);

    // Yours | Agent | x, only while there is an agent page.
    switcher_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(switcher_, "minicode-browser-bar");
    yours_ = gtk_toggle_button_new_with_label("Yours");
    agentBtn_ = gtk_toggle_button_new_with_label("Agent");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(agentBtn_), GTK_TOGGLE_BUTTON(yours_));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(yours_), TRUE);
    close_ = gtk_button_new_with_label("\xC3\x97");
    gtk_widget_set_tooltip_text(close_, "Close the agent page");
    gtk_widget_set_hexpand(close_, TRUE);
    gtk_widget_set_halign(close_, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(switcher_), yours_);
    gtk_box_append(GTK_BOX(switcher_), agentBtn_);
    gtk_box_append(GTK_BOX(switcher_), close_);
    gtk_widget_set_visible(switcher_, FALSE);
    gtk_box_append(GTK_BOX(root_), switcher_);
    g_signal_connect(yours_, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer p) {
        Browser* self = static_cast<Browser*>(p);
        if (!self->syncingSwitcher_ && gtk_toggle_button_get_active(b)) self->showAgentPage(false);
    }), this);
    g_signal_connect(agentBtn_, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer p) {
        Browser* self = static_cast<Browser*>(p);
        if (!self->syncingSwitcher_ && gtk_toggle_button_get_active(b)) self->showAgentPage(true);
    }), this);
    g_signal_connect(close_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<Browser*>(p)->closeAgentPage();
    }), this);

    stack_ = gtk_stack_new();
    gtk_widget_set_vexpand(stack_, TRUE);
    userSlot_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_stack_add_named(GTK_STACK(stack_), userSlot_, "yours");
    // The find bar, over the page's top right (an overlay: the page keeps its
    // size and the bar covers some of its corner, as on the Mac).
    findBox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(findBox_, "minicode-browser-bar");
    gtk_widget_set_halign(findBox_, GTK_ALIGN_END);
    gtk_widget_set_valign(findBox_, GTK_ALIGN_START);
    gtk_widget_set_margin_top(findBox_, 4);
    gtk_widget_set_margin_end(findBox_, 12);
    findEntry_ = gtk_search_entry_new();
    gtk_widget_set_size_request(findEntry_, 200, -1);
    findCount_ = gtk_label_new("");
    gtk_widget_set_size_request(findCount_, 80, -1);
    GtkWidget* fprev = gtk_button_new_from_icon_name("go-up-symbolic");
    GtkWidget* fnext = gtk_button_new_from_icon_name("go-down-symbolic");
    GtkWidget* fclose = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(fprev, "Previous match (Shift+Enter)");
    gtk_widget_set_tooltip_text(fnext, "Next match (Enter)");
    gtk_widget_set_tooltip_text(fclose, "Close (Esc)");
    for (GtkWidget* b : {fprev, fnext, fclose}) {
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_set_focus_on_click(b, FALSE);
    }
    gtk_box_append(GTK_BOX(findBox_), findEntry_);
    gtk_box_append(GTK_BOX(findBox_), findCount_);
    gtk_box_append(GTK_BOX(findBox_), fprev);
    gtk_box_append(GTK_BOX(findBox_), fnext);
    gtk_box_append(GTK_BOX(findBox_), fclose);
    gtk_widget_set_visible(findBox_, FALSE);
    GtkWidget* overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), stack_);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), findBox_);
    gtk_box_append(GTK_BOX(root_), overlay);
    g_signal_connect(findEntry_, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) {
        static_cast<Browser*>(p)->runFind();
    }), this);
    g_signal_connect(findEntry_, "activate", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) {
        static_cast<Browser*>(p)->findNext(true);
    }), this);
    g_signal_connect(findEntry_, "next-match", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) {
        static_cast<Browser*>(p)->findNext(true);
    }), this);
    g_signal_connect(findEntry_, "previous-match", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) {
        static_cast<Browser*>(p)->findNext(false);
    }), this);
    g_signal_connect(findEntry_, "stop-search", G_CALLBACK(+[](GtkSearchEntry*, gpointer p) {
        static_cast<Browser*>(p)->hideFind();
    }), this);
    GtkEventController* fkeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(fkeys, GTK_PHASE_CAPTURE);
    g_signal_connect(fkeys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint,
                                                           GdkModifierType mods, gpointer p) -> gboolean {
        if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && (mods & GDK_SHIFT_MASK)) {
            static_cast<Browser*>(p)->findNext(false);
            return TRUE;
        }
        return FALSE;
    }), this);
    gtk_widget_add_controller(findEntry_, fkeys);
    g_signal_connect(fprev, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<Browser*>(p)->findNext(false);
    }), this);
    g_signal_connect(fnext, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<Browser*>(p)->findNext(true);
    }), this);
    g_signal_connect(fclose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        static_cast<Browser*>(p)->hideFind();
    }), this);
    // Nothing to go back or forward to until there is a page.
    gtk_widget_set_sensitive(back_, FALSE);
    gtk_widget_set_sensitive(fwd_, FALSE);

    g_signal_connect(entry_, "activate", G_CALLBACK(onGo), this);
    g_signal_connect(back, "clicked", G_CALLBACK(onBack), this);
    g_signal_connect(fwd,  "clicked", G_CALLBACK(onForward), this);
    g_signal_connect(rel,  "clicked", G_CALLBACK(onReload), this);
    g_signal_connect(root_, "map", G_CALLBACK(+[](GtkWidget*, gpointer p) {
        Browser* self = static_cast<Browser*>(p);
        self->shown_ = true;
        if (self->tabs_.empty()) self->newTab(self->home_);   // a pane shown after its last tab closed
        else self->applyLive();
    }), this);
    // Adjust Settings… in the toolbar's right click and the address bar's
    // editing menu, as on the Mac (PARITY.md 2.11); the page's is below.
    Appearance::attachMenu(bar, "browser");
}


Browser::~Browser() {
    g_signal_handlers_disconnect_by_data(root_, this);
    while (!tabs_.empty()) {
        Tab* t = tabs_.back();
        tabs_.pop_back();
        destroyTab(t);
    }
    delete strip_;
    delete agentProbe_;
}

// One tab. `web` and `probe` exist only while the tab is live; a suspended
// tab keeps `url`, `title` and `state` (WebKit's serialized history and
// scroll, empty when it has none).
struct Browser::Tab {
    Browser* owner = nullptr;
    std::string url, title, state;
    GtkWidget* web = nullptr;
    PageProbe* probe = nullptr;
    uint64_t used = 0;
    double zoom = 1.0;
};

Browser::Tab* Browser::current() const {
    return active_ >= 0 && active_ < (int)tabs_.size() ? tabs_[(size_t)active_] : nullptr;
}

int Browser::indexOfTab(const Tab* t) const {
    for (size_t i = 0; i < tabs_.size(); i++)
        if (tabs_[i] == t) return (int)i;
    return -1;
}

GtkWidget* Browser::webView() const {
    Tab* t = current();
    return t ? t->web : nullptr;
}

PageProbe* Browser::userProbe() const {
    Tab* t = current();
    return t ? t->probe : nullptr;
}

bool Browser::tabIsLive(int i) const {
    return i >= 0 && i < (int)tabs_.size() && tabs_[(size_t)i]->web != nullptr;
}

int Browser::liveCount() const {
    int n = 0;
    for (const Tab* t : tabs_) n += t->web != nullptr;
    return n;
}

std::string Browser::tabUrl(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i]->url : "";
}

std::string Browser::tabTitle(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i]->title : "";
}

void Browser::noteChanged() {
    if (onChanged) onChanged();
}

void Browser::updateStrip() {
    std::vector<TabStrip::Item> items;
    for (const Tab* t : tabs_) {
        TabStrip::Item it;
        it.title = BrowserTabs::tabTitle(t->title, t->url);
        it.tooltip = t->url;
        items.push_back(it);
    }
    strip_->setItems(items, active_);
}

// ---------------------------------------------------------------- tabs

int Browser::newTab(const std::string& url, bool show) {
    Tab* t = new Tab();
    t->owner = this;
    t->url = url;
    const int at = EditorTabs::insertionIndex(active_, (int)tabs_.size());
    tabs_.insert(tabs_.begin() + at, t);
    if (active_ >= at) active_++;   // the active tab did not move: it is still the same tab
    if (active_ < 0) show = true;
    if (show) {
        selectTab(at);
    } else {
        updateStrip();
        noteChanged();
    }
    return at;
}

void Browser::selectTab(int i) {
    if (i < 0 || i >= (int)tabs_.size()) return;
    showAgentPage(false);
    if (i != active_) hideFind();
    active_ = i;
    tabs_[(size_t)i]->used = ++clock_;
    applyLive();
    for (Tab* t : tabs_)
        if (t->web) gtk_widget_set_visible(t->web, t == tabs_[(size_t)i]);
    updateStrip();
    syncChrome();
    noteChanged();
}

void Browser::stepTab(int delta) {
    const int to = EditorTabs::step(active_, (int)tabs_.size(), delta);
    if (to >= 0) selectTab(to);
}

void Browser::selectTabNumber(int n) {
    const int to = EditorTabs::indexForNumber(n, (int)tabs_.size());
    if (to >= 0) selectTab(to);
}

void Browser::closeActive() {
    if (agentPageShown()) {
        closeAgentPage();
        return;
    }
    closeTab(active_);
}

void Browser::closeTab(int i) {
    if (i < 0 || i >= (int)tabs_.size()) return;
    std::vector<uint64_t> used;
    for (const Tab* t : tabs_) used.push_back(t->used);
    const int next = EditorTabs::activeAfterClose(used, active_, i);
    Tab* gone = tabs_[(size_t)i];
    tabs_.erase(tabs_.begin() + i);
    destroyTab(gone);
    active_ = tabs_.empty() ? -1 : next;
    if (active_ >= (int)tabs_.size()) active_ = (int)tabs_.size() - 1;
    if (active_ >= 0) tabs_[(size_t)active_]->used = ++clock_;
    applyLive();
    for (Tab* t : tabs_)
        if (t->web) gtk_widget_set_visible(t->web, t == current());
    updateStrip();
    syncChrome();
    noteChanged();
    if (tabs_.empty() && onEmpty) onEmpty();
}

void Browser::moveTab(int from, int to) {
    const int n = (int)tabs_.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    Tab* active = current();
    Tab* t = tabs_[(size_t)from];
    tabs_.erase(tabs_.begin() + from);
    tabs_.insert(tabs_.begin() + to, t);
    active_ = indexOfTab(active);
    updateStrip();
    noteChanged();
}

// Only the tab on show and the one used before it keep a web view
// (BrowserTabs::livePages); the others are suspended, which ends their
// WebKit processes, and a tab on show without a view gets one.
void Browser::applyLive() {
    if (tabs_.empty()) return;
    std::vector<uint64_t> used;
    for (const Tab* t : tabs_) used.push_back(t->used);
    const std::vector<bool> live = BrowserTabs::livePages(used, active_, BrowserTabs::kLivePages);
    for (size_t i = 0; i < tabs_.size(); i++)
        if (!live[i] && tabs_[i]->web) suspend(tabs_[i]);
    Tab* t = current();
    if (t && !t->web && shown_) makeView(t);
}

void Browser::suspend(Tab* t) {
    if (!t->web) return;
    WebKitWebView* wv = WEBKIT_WEB_VIEW(t->web);
    t->state.clear();
    if (WebKitWebViewSessionState* st = webkit_web_view_get_session_state(wv)) {
        if (GBytes* b = webkit_web_view_session_state_serialize(st)) {
            gsize n = 0;
            const char* d = static_cast<const char*>(g_bytes_get_data(b, &n));
            if (d) t->state.assign(d, n);
            g_bytes_unref(b);
        }
        webkit_web_view_session_state_unref(st);
    }
    t->zoom = webkit_web_view_get_zoom_level(wv);
    const char* uri = webkit_web_view_get_uri(wv);
    if (uri && *uri) t->url = uri;
    delete t->probe;   // stops watching the view before it goes
    t->probe = nullptr;
    g_signal_handlers_disconnect_by_data(t->web, t);
    g_signal_handlers_disconnect_by_data(webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(t->web)), t);
    gtk_box_remove(GTK_BOX(userSlot_), t->web);   // the last reference: WebKit's processes end
    t->web = nullptr;
}

void Browser::destroyTab(Tab* t) {
    if (t->web) {
        delete t->probe;
        t->probe = nullptr;
        g_signal_handlers_disconnect_by_data(t->web, t);
        g_signal_handlers_disconnect_by_data(webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(t->web)), t);
        gtk_box_remove(GTK_BOX(userSlot_), t->web);
        t->web = nullptr;
    }
    delete t;
}

void Browser::onTabUri(GObject* obj, GParamSpec*, gpointer tp) {
    Tab* t = static_cast<Tab*>(tp);
    const char* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(obj));
    if (uri && *uri) t->url = uri;
    Browser* self = t->owner;
    if (t == self->current()) self->syncUrlBar();
    self->updateStrip();
}

void Browser::onTabTitle(GObject* obj, GParamSpec*, gpointer tp) {
    Tab* t = static_cast<Tab*>(tp);
    const char* title = webkit_web_view_get_title(WEBKIT_WEB_VIEW(obj));
    t->title = title ? title : "";
    t->owner->updateStrip();
}

void Browser::onTabLoad(WebKitWebView* wv, WebKitLoadEvent ev, gpointer tp) {
    Tab* t = static_cast<Tab*>(tp);
    Browser* self = t->owner;
    if (ev == WEBKIT_LOAD_COMMITTED) {
        const char* u = webkit_web_view_get_uri(wv);
        const double z = siteZoom(siteOf(u ? u : ""));
        if (std::abs(webkit_web_view_get_zoom_level(wv) - z) > 0.001) webkit_web_view_set_zoom_level(wv, z);
    }
    if (ev == WEBKIT_LOAD_COMMITTED)
        StartupLog::once("browser-page", "browser: first page committed (%s)",
                         webkit_web_view_get_uri(wv) ? webkit_web_view_get_uri(wv) : "?");
    if (t == self->current() && (ev == WEBKIT_LOAD_COMMITTED || ev == WEBKIT_LOAD_FINISHED))
        self->syncChrome();
    if (ev == WEBKIT_LOAD_FINISHED) self->noteChanged();
}

// A view for the tab: a new one that loads its address (restoring its
// history when it was suspended with a record), or `related`'s sibling made
// for a page that page opened.
void Browser::makeView(Tab* t, GtkWidget* related) {
    if (t->web) return;
    StartupLog::mark("browser: web view made, on first show");
    GtkWidget* web = related ? GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", WEBKIT_WEB_VIEW(related), nullptr))
                             : GTK_WIDGET(webkit_web_view_new());
    adoptView(t, web);
    if (related) return;   // WebKit loads what the page opened
    WebKitWebView* wv = WEBKIT_WEB_VIEW(web);
    if (t->zoom != 1.0) webkit_web_view_set_zoom_level(wv, t->zoom);
    bool restored = false;
    if (!t->state.empty()) {
        GBytes* b = g_bytes_new(t->state.data(), t->state.size());
        if (WebKitWebViewSessionState* st = webkit_web_view_session_state_new(b)) {
            webkit_web_view_restore_session_state(wv, st);
            webkit_web_view_session_state_unref(st);
            WebKitBackForwardList* list = webkit_web_view_get_back_forward_list(wv);
            if (WebKitBackForwardListItem* item = webkit_back_forward_list_get_current_item(list)) {
                webkit_web_view_go_to_back_forward_list_item(wv, item);
                restored = true;
            }
        }
        g_bytes_unref(b);
    }
    // The home page is an address, not something typed into the bar, so it
    // is loaded as it is: through load(), the GUI checks' about:blank was a
    // web search for "about:blank", and every check's window went online.
    if (!restored) webkit_web_view_load_uri(wv, (t->url.empty() ? home_ : t->url).c_str());
}

void Browser::adoptView(Tab* t, GtkWidget* web) {
    t->web = web;
    gtk_widget_set_vexpand(web, TRUE);
    gtk_box_append(GTK_BOX(userSlot_), web);
    gtk_widget_set_visible(web, t == current());
    t->probe = new PageProbe(web, false);
    g_signal_connect(web, "notify::uri", G_CALLBACK(onTabUri), t);
    g_signal_connect(web, "notify::title", G_CALLBACK(onTabTitle), t);
    g_signal_connect(web, "load-changed", G_CALLBACK(onTabLoad), t);
    WebKitFindController* fc = webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(web));
    g_signal_connect(fc, "counted-matches", G_CALLBACK(+[](WebKitFindController*, guint n, gpointer tp) {
        Tab* tab = static_cast<Tab*>(tp);
        Browser* b = tab->owner;
        if (tab != b->current() || !b->findShown()) return;
        gtk_label_set_text(GTK_LABEL(b->findCount_), n == 0 ? "No results" : n == 1 ? "1 match" : (std::to_string(n) + " matches").c_str());
    }), t);
    g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(+[](WebKitFindController*, gpointer tp) {
        Tab* tab = static_cast<Tab*>(tp);
        Browser* b = tab->owner;
        if (tab == b->current() && b->findShown()) gtk_label_set_text(GTK_LABEL(b->findCount_), "No results");
    }), t);
    // The page's right click: WebKit's items, then Adjust Settings….
    g_signal_connect(web, "context-menu", G_CALLBACK(+[](WebKitWebView* wv, WebKitContextMenu* m,
                                                         WebKitHitTestResult*, gpointer) -> gboolean {
        Appearance::addToWebMenu(m, GTK_WIDGET(wv));
        return FALSE;   // shown as WebKit shows it
    }), t);
    // A Ctrl-clicked or middle-clicked link opens a tab behind (suspended:
    // it loads when shown), as the Mac's decidePolicyForNavigationAction:.
    g_signal_connect(web, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* d,
                                                          WebKitPolicyDecisionType type, gpointer tp) -> gboolean {
        if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
            type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
            return FALSE;
        WebKitNavigationAction* a =
            webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
        if (!a || webkit_navigation_action_get_navigation_type(a) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
            return FALSE;
        const bool middle = webkit_navigation_action_get_mouse_button(a) == 2;
        const bool ctrl = (webkit_navigation_action_get_modifiers(a) & GDK_CONTROL_MASK) != 0;
        if (!middle && !ctrl) return FALSE;
        WebKitURIRequest* req = webkit_navigation_action_get_request(a);
        const char* uri = req ? webkit_uri_request_get_uri(req) : nullptr;
        if (!uri || !*uri) return FALSE;
        webkit_policy_decision_ignore(d);
        static_cast<Tab*>(tp)->owner->newTab(uri, false);
        return TRUE;
    }), t);
    // A target=_blank link or window.open: the page comes up beside its
    // opener, in the view WebKit asks for, related to the opener's.
    g_signal_connect(web, "create", G_CALLBACK(+[](WebKitWebView* opener, WebKitNavigationAction*,
                                                   gpointer tp) -> GtkWidget* {
        Tab* from = static_cast<Tab*>(tp);
        Browser* self = from->owner;
        Tab* t = new Tab();
        t->owner = self;
        const int openerAt = self->indexOfTab(from);
        const int at = openerAt < 0 ? (int)self->tabs_.size() : openerAt + 1;
        const Tab* shownBefore = self->current();
        self->tabs_.insert(self->tabs_.begin() + at, t);
        self->active_ = self->indexOfTab(shownBefore);
        self->shown_ = true;
        self->makeView(t, GTK_WIDGET(opener));
        self->selectTab(at);   // suspends the oldest of three, never this one
        return t->web;
    }), t);
    syncChrome();
}

// Mirrors the macOS -navigateToString:. Anything with an explicit scheme is
// taken as-is; something that looks like a domain gets https://; anything else
// is treated as a web search rather than mangled into a broken URL.
void Browser::load(const std::string& raw) {
    static const char* kWs = " \t\r\n";
    const auto first = raw.find_first_not_of(kWs);
    if (first == std::string::npos) return;
    const std::string s = raw.substr(first, raw.find_last_not_of(kWs) - first + 1);

    auto startsWith = [&s](const char* p) { return s.rfind(p, 0) == 0; };
    const bool hasScheme = startsWith("http://") || startsWith("https://") ||
                           startsWith("file://");

    std::string url = s;
    if (!hasScheme) {
        const bool looksLikeDomain = s.find('.') != std::string::npos &&
                                     s.find(' ') == std::string::npos;
        if (looksLikeDomain) {
            url = "https://" + s;
        } else {
            char* q = g_uri_escape_string(s.c_str(), nullptr, TRUE);
            url = std::string("https://duckduckgo.com/?q=") + (q ? q : "");
            g_free(q);
        }
    }
    showAgentPage(false);   // an address typed or linked is the user's page
    shown_ = true;          // the first page is this one, not the home page
    Tab* t = current();
    if (!t) {
        newTab(url);
        return;
    }
    t->url = url;
    t->state.clear();
    if (t->web) webkit_web_view_load_uri(WEBKIT_WEB_VIEW(t->web), url.c_str());
    else applyLive();
    updateStrip();
    syncChrome();
    noteChanged();
}

bool Browser::agentPageShown() const {
    return agentProbe_ && std::string(gtk_stack_get_visible_child_name(GTK_STACK(stack_)) ?: "") == "agent";
}

PageProbe* Browser::openAgentPage() {
    if (agentProbe_) return agentProbe_;
    agent_ = PageProbe::makeAgentView(&agentProbe_);
    gtk_widget_set_vexpand(agent_, TRUE);
    gtk_stack_add_named(GTK_STACK(stack_), agent_, "agent");
    gtk_widget_set_visible(switcher_, TRUE);
    return agentProbe_;
}

void Browser::showAgentPage(bool agent) {
    if (agent && !agentProbe_) return;
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), agent ? "agent" : "yours");
    syncingSwitcher_ = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(agent ? agentBtn_ : yours_), TRUE);
    syncingSwitcher_ = false;
}

void Browser::closeAgentPage() {
    if (!agentProbe_) return;
    showAgentPage(false);
    gtk_widget_set_visible(switcher_, FALSE);
    delete agentProbe_;   // stops watching the view before it goes
    agentProbe_ = nullptr;
    gtk_stack_remove(GTK_STACK(stack_), agent_);
    agent_ = nullptr;
}

// Keep the URL bar showing where we actually are.
void Browser::syncUrlBar() {
    Tab* t = current();
    if (!t) {
        gtk_editable_set_text(GTK_EDITABLE(entry_), "");
        refreshStar();
        return;
    }
    const char* uri = t->web ? webkit_web_view_get_uri(WEBKIT_WEB_VIEW(t->web)) : nullptr;
    const std::string shown = uri && *uri ? uri : t->url;
    gtk_editable_set_text(GTK_EDITABLE(entry_), shown.c_str());
    refreshStar();
}

void Browser::starClicked() {
    Tab* t = current();
    if (!t || !onStar) return;
    const char* uri = t->web ? webkit_web_view_get_uri(WEBKIT_WEB_VIEW(t->web)) : nullptr;
    onStar(uri && *uri ? uri : t->url, starOn_);
}

// The star: filled while the page on show is bookmarked anywhere in the
// folder, and only there to click for a page that may be bookmarked.
void Browser::refreshStar() {
    if (!star_) return;
    Tab* t = current();
    std::string url;
    if (t) {
        const char* uri = t->web ? webkit_web_view_get_uri(WEBKIT_WEB_VIEW(t->web)) : nullptr;
        url = uri && *uri ? uri : t->url;
    }
    const bool web = WebBookmark::isWebAddress(url);
    starOn_ = web && isBookmarked && isBookmarked(url);
    gtk_button_set_icon_name(GTK_BUTTON(star_), starOn_ ? "starred-symbolic" : "non-starred-symbolic");
    gtk_widget_set_tooltip_text(star_, starOn_ ? "Bookmarked: click to move the bookmark to the Trash"
                                               : "Bookmark this page");
    gtk_widget_set_sensitive(star_, web);
}

// URL bar plus back/forward sensitivity. Only call this where the back/forward
// list is up to date (see the note in Browser.h).
void Browser::syncChrome() {
    syncUrlBar();
    Tab* t = current();
    WebKitWebView* wv = t && t->web ? WEBKIT_WEB_VIEW(t->web) : nullptr;
    gtk_widget_set_sensitive(back_, wv && webkit_web_view_can_go_back(wv));
    gtk_widget_set_sensitive(fwd_, wv && webkit_web_view_can_go_forward(wv));
}

void Browser::onGo(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    const char* txt = gtk_editable_get_text(GTK_EDITABLE(self->entry_));
    if (txt) self->load(txt);
}

void Browser::onBack(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    if (GtkWidget* w = self->webView()) webkit_web_view_go_back(WEBKIT_WEB_VIEW(w));
}

void Browser::onForward(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    if (GtkWidget* w = self->webView()) webkit_web_view_go_forward(WEBKIT_WEB_VIEW(w));
}

void Browser::onReload(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    if (GtkWidget* w = self->webView()) webkit_web_view_reload(WEBKIT_WEB_VIEW(w));
    else if (self->tabs_.empty()) self->newTab(self->home_);
    else {
        self->shown_ = true;
        self->applyLive();
    }
}

void Browser::focusUrlBar() {
    if (entry_) gtk_widget_grab_focus(entry_);
}

void Browser::focusPage() {
    GtkWidget* w = webView();
    if (!w) { focusUrlBar(); return; }
    if (gtk_widget_grab_focus(w) && gtk_widget_has_focus(w)) return;
    // The pane was just shown and its page is not mapped yet: once it is.
    g_idle_add_full(G_PRIORITY_LOW, +[](gpointer p) -> gboolean {
        GtkWidget* page = GTK_WIDGET(p);
        if (gtk_widget_get_mapped(page)) gtk_widget_grab_focus(page);
        return G_SOURCE_REMOVE;
    }, g_object_ref(w), g_object_unref);
}

namespace {
// An address without a lone trailing slash, so https://host and
// https://host/ (what WebKit reports) are one page.
std::string withoutSlash(std::string s) {
    if (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
}  // namespace

void Browser::openAddresses(const std::vector<std::string>& urls) {
    shown_ = true;   // the first page is this one, not the home page
    showAgentPage(false);
    int show = -1;
    int at = -1;   // where the next new tab goes
    for (const std::string& u : urls) {
        if (u.empty()) continue;
        int have = -1;
        for (size_t i = 0; i < tabs_.size() && have < 0; i++)
            if (withoutSlash(tabs_[i]->url) == withoutSlash(u)) have = (int)i;
        if (have >= 0) {
            if (show < 0) show = have;
            continue;
        }
        Tab* cur = current();
        if (show < 0 && cur && (cur->url.empty() || withoutSlash(cur->url) == withoutSlash(home_))) {
            // The empty (or home page) tab on show takes the first address.
            cur->url = u;
            cur->title.clear();
            cur->state.clear();
            if (cur->web) webkit_web_view_load_uri(WEBKIT_WEB_VIEW(cur->web), u.c_str());
            show = active_;
            at = active_ + 1;
            continue;
        }
        Tab* t = new Tab();
        t->owner = this;
        t->url = u;
        if (at < 0) at = EditorTabs::insertionIndex(active_, (int)tabs_.size());
        const Tab* shownBefore = cur;
        tabs_.insert(tabs_.begin() + at, t);
        active_ = shownBefore ? indexOfTab(shownBefore) : -1;
        if (show < 0) show = at;
        else if (show >= at) show++;
        at++;
    }
    if (show < 0) return;
    if (active_ < 0) active_ = 0;
    // New tabs went right of the one on show, so active_ still names it.
    selectTab(show);
}

// ---------------------------------------------------------------- zoom per site

namespace {
std::map<std::string, double>& siteZooms() {
    static std::map<std::string, double> m;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        gchar* text = nullptr;
        std::string path = std::string(g_get_user_state_dir()) + "/minicode/browser-zoom.txt";
        if (g_file_get_contents(path.c_str(), &text, nullptr, nullptr)) {
            std::string s = text;
            g_free(text);
            size_t pos = 0;
            while (pos < s.size()) {
                size_t nl = s.find('\n', pos);
                if (nl == std::string::npos) nl = s.size();
                std::string line = s.substr(pos, nl - pos);
                pos = nl + 1;
                size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                double z = g_ascii_strtod(line.c_str() + tab + 1, nullptr);
                if (z >= 0.25 && z <= 5.0) m[line.substr(0, tab)] = z;
            }
        }
    }
    return m;
}
void saveSiteZooms() {
    std::string out;
    for (const auto& kv : siteZooms()) {
        char num[32];
        g_ascii_formatd(num, sizeof num, "%.4g", kv.second);
        out += kv.first + "\t" + num + "\n";
    }
    std::string dir = std::string(g_get_user_state_dir()) + "/minicode";
    g_mkdir_with_parents(dir.c_str(), 0700);
    g_file_set_contents((dir + "/browser-zoom.txt").c_str(), out.data(), (gssize)out.size(), nullptr);
}
}  // namespace

std::string Browser::siteOf(const std::string& url) {
    GUri* u = g_uri_parse(url.c_str(), G_URI_FLAGS_PARSE_RELAXED, nullptr);
    if (!u) return "";
    std::string scheme = g_uri_get_scheme(u) ? g_uri_get_scheme(u) : "";
    const char* h = g_uri_get_host(u);
    std::string site = h && *h ? h : (scheme == "file" ? "file" : "");
    g_uri_unref(u);
    return site;
}

double Browser::siteZoom(const std::string& host) {
    auto& m = siteZooms();
    auto it = m.find(host);
    return it == m.end() ? 1.0 : it->second;
}

void Browser::zoom(int step) {
    GtkWidget* w = webView();
    if (!w) return;   // no page yet
    WebKitWebView* wv = WEBKIT_WEB_VIEW(w);
    const double level = ViewZoom::pageStep(webkit_web_view_get_zoom_level(wv), step);
    webkit_web_view_set_zoom_level(wv, level);
    const char* uri = webkit_web_view_get_uri(wv);
    const std::string site = siteOf(uri ? uri : "");
    if (site.empty()) return;
    if (level > 0.995 && level < 1.005) siteZooms().erase(site);
    else siteZooms()[site] = level;
    saveSiteZooms();
    for (Tab* t : tabs_) {   // the site's other pages follow
        if (!t->web || t->web == w) continue;
        const char* u = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(t->web));
        if (siteOf(u ? u : "") == site) webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(t->web), level);
    }
}

// ---------------------------------------------------------------- find in the page

bool Browser::findShown() const { return findBox_ && gtk_widget_get_visible(findBox_); }

std::string Browser::findCountText() const {
    return findCount_ ? gtk_label_get_text(GTK_LABEL(findCount_)) : "";
}

void Browser::showFind() {
    if (!webView()) return;
    gtk_widget_set_visible(findBox_, TRUE);
    gtk_widget_grab_focus(findEntry_);
    gtk_editable_select_region(GTK_EDITABLE(findEntry_), 0, -1);
    runFind();
}

void Browser::hideFind() {
    if (!findShown()) return;
    for (Tab* t : tabs_)
        if (t->web) webkit_find_controller_search_finish(webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(t->web)));
    gtk_widget_set_visible(findBox_, FALSE);
    gtk_label_set_text(GTK_LABEL(findCount_), "");
    if (GtkWidget* w = webView()) gtk_widget_grab_focus(w);
}

void Browser::runFind() {
    GtkWidget* w = webView();
    if (!w) return;
    WebKitFindController* fc = webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(w));
    const char* q = gtk_editable_get_text(GTK_EDITABLE(findEntry_));
    if (!q || !*q) {
        webkit_find_controller_search_finish(fc);
        gtk_label_set_text(GTK_LABEL(findCount_), "");
        return;
    }
    const guint32 opts = (guint32)(WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND);
    webkit_find_controller_search(fc, q, opts, 10000);
    webkit_find_controller_count_matches(fc, q, opts, 10000);   // counted-matches: the label
}

void Browser::findNext(bool forward) {
    GtkWidget* w = webView();
    if (!w) return;
    if (!findShown()) {
        showFind();
        return;
    }
    WebKitFindController* fc = webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(w));
    if (forward) webkit_find_controller_search_next(fc);
    else webkit_find_controller_search_previous(fc);
}

double Browser::zoomLevel() const {
    GtkWidget* w = webView();
    return w ? webkit_web_view_get_zoom_level(WEBKIT_WEB_VIEW(w)) : 1.0;
}

// ---------------------------------------------------------------- session

std::vector<TabSessions::BrowserTab> Browser::records(int* active) {
    std::vector<TabSessions::BrowserTab> out;
    std::vector<size_t> sizes;
    std::vector<uint64_t> used;
    for (Tab* t : tabs_) {
        std::string state = t->state;
        std::string url = t->url;
        if (t->web) {   // a live tab's record is as it is now
            WebKitWebView* wv = WEBKIT_WEB_VIEW(t->web);
            const char* uri = webkit_web_view_get_uri(wv);
            if (uri && *uri) url = uri;
            state.clear();
            if (WebKitWebViewSessionState* st = webkit_web_view_get_session_state(wv)) {
                if (GBytes* b = webkit_web_view_session_state_serialize(st)) {
                    gsize n = 0;
                    const char* d = static_cast<const char*>(g_bytes_get_data(b, &n));
                    if (d) state.assign(d, n);
                    g_bytes_unref(b);
                }
                webkit_web_view_session_state_unref(st);
            }
        }
        TabSessions::BrowserTab r;
        r.url = url;
        r.title = t->title;
        r.state = state;   // raw for now: sizes, then base64 for the kept
        out.push_back(r);
        sizes.push_back(state.size());
        used.push_back(t->used);
    }
    const std::vector<bool> keep = BrowserTabs::keptRecords(sizes, used, active_, 2u << 20, 4u << 20);
    for (size_t i = 0; i < out.size(); i++) {
        if (keep[i] && !out[i].state.empty()) {
            gchar* b64 = g_base64_encode(reinterpret_cast<const guchar*>(out[i].state.data()), out[i].state.size());
            out[i].state = b64;
            g_free(b64);
        } else {
            out[i].state.clear();
        }
    }
    if (active) *active = active_;
    return out;
}

void Browser::restore(const std::vector<TabSessions::BrowserTab>& saved, int active) {
    if (saved.empty() || !tabs_.empty()) return;
    for (const TabSessions::BrowserTab& r : saved) {
        Tab* t = new Tab();
        t->owner = this;
        t->url = r.url;
        t->title = r.title;
        if (!r.state.empty()) {
            gsize n = 0;
            guchar* d = g_base64_decode(r.state.c_str(), &n);
            if (d) t->state.assign(reinterpret_cast<const char*>(d), n);
            g_free(d);
        }
        tabs_.push_back(t);
    }
    selectTab(BrowserTabs::restoredActive(active, (int)tabs_.size()));
}

#endif // MINICODE_ENABLE_BROWSER
