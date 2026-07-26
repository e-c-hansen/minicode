// Browser.cpp — see Browser.h. Compiled out unless MINICODE_ENABLE_BROWSER.
#include "Browser.h"

#ifdef MINICODE_ENABLE_BROWSER

#include <webkit/webkit.h>
#include <string>

Browser::Browser(const std::string& homeUrl) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Toolbar: back / forward / reload / url entry.
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);

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

    web_ = GTK_WIDGET(webkit_web_view_new());
    gtk_widget_set_vexpand(web_, TRUE);

    gtk_box_append(GTK_BOX(root_), bar);
    gtk_box_append(GTK_BOX(root_), web_);

    g_signal_connect(entry_, "activate", G_CALLBACK(onGo), this);
    g_signal_connect(back, "clicked", G_CALLBACK(onBack), this);
    g_signal_connect(fwd,  "clicked", G_CALLBACK(onForward), this);
    g_signal_connect(rel,  "clicked", G_CALLBACK(onReload), this);
    g_signal_connect(web_, "notify::uri", G_CALLBACK(onUriChanged), this);
    g_signal_connect(web_, "load-changed", G_CALLBACK(onLoadChanged), this);

    load(homeUrl);
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
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web_), url.c_str());
}

// Keep the URL bar showing where we actually are.
void Browser::syncUrlBar() {
    const char* uri = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(web_));
    if (uri && *uri) gtk_editable_set_text(GTK_EDITABLE(entry_), uri);
}

// URL bar plus back/forward sensitivity. Only call this where the back/forward
// list is up to date (see the note in Browser.h).
void Browser::syncChrome() {
    WebKitWebView* wv = WEBKIT_WEB_VIEW(web_);
    syncUrlBar();
    gtk_widget_set_sensitive(back_, webkit_web_view_can_go_back(wv));
    gtk_widget_set_sensitive(fwd_,  webkit_web_view_can_go_forward(wv));
}

void Browser::onUriChanged(GObject*, GParamSpec*, gpointer selfp) {
    static_cast<Browser*>(selfp)->syncUrlBar();
}

void Browser::onLoadChanged(WebKitWebView*, WebKitLoadEvent ev, gpointer selfp) {
    if (ev == WEBKIT_LOAD_COMMITTED || ev == WEBKIT_LOAD_FINISHED)
        static_cast<Browser*>(selfp)->syncChrome();
}

void Browser::onGo(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    const char* txt = gtk_editable_get_text(GTK_EDITABLE(self->entry_));
    if (txt) self->load(txt);
}

void Browser::onBack(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    webkit_web_view_go_back(WEBKIT_WEB_VIEW(self->web_));
}

void Browser::onForward(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    webkit_web_view_go_forward(WEBKIT_WEB_VIEW(self->web_));
}

void Browser::onReload(GtkWidget* /*w*/, gpointer selfp) {
    Browser* self = static_cast<Browser*>(selfp);
    webkit_web_view_reload(WEBKIT_WEB_VIEW(self->web_));
}

void Browser::focusUrlBar() {
    if (entry_) gtk_widget_grab_focus(entry_);
}

#endif // MINICODE_ENABLE_BROWSER
