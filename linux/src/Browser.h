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
#include <string>

class Browser {
public:
    explicit Browser(const std::string& homeUrl);
    GtkWidget* widget() const { return root_; }
    void focusUrlBar();

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
    static void onUriChanged(GObject* obj, GParamSpec* pspec, gpointer self);
    static void onLoadChanged(WebKitWebView* wv, WebKitLoadEvent ev, gpointer self);

    void load(const std::string& url);
    void syncUrlBar();   // URL bar text only
    void syncChrome();   // URL bar text + back/forward sensitivity

    GtkWidget* root_  = nullptr;
    GtkWidget* entry_ = nullptr;
    GtkWidget* web_   = nullptr;   // WebKitWebView
    GtkWidget* back_  = nullptr;
    GtkWidget* fwd_   = nullptr;
};

#endif // MINICODE_ENABLE_BROWSER
