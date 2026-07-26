// Browser.h — a simple embedded web browser panel: a WebKitGTK web view with a
// URL entry and back / forward / reload buttons. Mirrors the macOS Browser.mm
// (a WKWebView with minimal chrome).
//
// Compiled only when MINICODE_ENABLE_BROWSER is defined (webkitgtk-6.0 found at
// configure time). Callers must guard use with the same macro.
#pragma once

#ifdef MINICODE_ENABLE_BROWSER

#include <gtk/gtk.h>
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
    void load(const std::string& url);

    GtkWidget* root_  = nullptr;
    GtkWidget* entry_ = nullptr;
    GtkWidget* web_   = nullptr;   // WebKitWebView
};

#endif // MINICODE_ENABLE_BROWSER
