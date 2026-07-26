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

    GtkWidget* back = gtk_button_new_from_icon_name("go-previous-symbolic");
    GtkWidget* fwd  = gtk_button_new_from_icon_name("go-next-symbolic");
    GtkWidget* rel  = gtk_button_new_from_icon_name("view-refresh-symbolic");
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

    load(homeUrl);
}

void Browser::load(const std::string& url) {
    std::string u = url;
    if (u.find("://") == std::string::npos) u = "https://" + u;
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web_), u.c_str());
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
