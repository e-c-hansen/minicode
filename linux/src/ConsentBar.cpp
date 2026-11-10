// ConsentBar.cpp — see ConsentBar.h. The words are the core's
// (FolderTools.h), which are the Mac's.
#include "ConsentBar.h"

#include "FolderConsent.h"
#include "FolderTools.h"
#include "Palette.h"

namespace {

void installCss() {
    static bool done = false;
    if (done || !gdk_display_get_default()) return;
    done = true;
    // The sidebar's colors, as the Mac's bar (applyInstallBarColors), with
    // the dividers' line under it.
    const std::string css = std::string(".minicode-consent-bar { background-color: ") +
                            pal::SidebarBg + "; color: " + pal::TreeText +
                            "; border-bottom: 1px solid " + pal::Divider +
                            "; padding: 3px 8px; }"
                            ".minicode-consent-bar button { padding: 1px 10px; min-height: 0; }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

}  // namespace

ConsentBar::ConsentBar(Host host) : host_(std::move(host)) {
    installCss();
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box_, "minicode-consent-bar");
    label_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label_), 0);
    gtk_label_set_wrap(GTK_LABEL(label_), TRUE);
    gtk_widget_set_hexpand(label_, TRUE);
    allow_ = gtk_button_new_with_label("Use It");
    deny_ = gtk_button_new_with_label("Don't Use It");
    close_ = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close_, "flat");
    gtk_widget_set_tooltip_text(close_, "Ask again next time minicode starts");
    for (GtkWidget* w : {label_, allow_, deny_, close_}) gtk_box_append(GTK_BOX(box_), w);
    for (GtkWidget* b : {allow_, deny_, close_}) gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(allow_, "clicked",
                             G_CALLBACK(+[](ConsentBar* self) { self->answer(true); }), this);
    g_signal_connect_swapped(deny_, "clicked",
                             G_CALLBACK(+[](ConsentBar* self) { self->answer(false); }), this);
    g_signal_connect_swapped(close_, "clicked", G_CALLBACK(+[](ConsentBar* self) { self->putOff(); }),
                             this);
    gtk_widget_set_visible(box_, FALSE);
    // Another window's answer, or Format Document's, or Ask Again.
    listener_ = FolderConsent::listen([this](const std::string&) { refresh(); });
}

ConsentBar::~ConsentBar() {
    FolderConsent::unlisten(listener_);
    // The window's widgets outlive this object (main.cpp deletes it from
    // window-removed): its buttons must not call back into it.
    for (GtkWidget* b : {allow_, deny_, close_}) g_signal_handlers_disconnect_by_data(b, this);
}

std::string ConsentBar::text() const { return gtk_label_get_text(GTK_LABEL(label_)); }

void ConsentBar::refresh() {
    const std::string root = host_.root ? host_.root() : "";
    const bool putOff = FolderConsent::isPutOff(root);
    // The folder's own Python environment comes first: until it is
    // answered the servers run without it, and a server missing from PATH
    // may be in it. Then a server that would run the folder's code.
    std::string env = putOff || !host_.environment ? "" : host_.environment();
    std::string server = putOff || !env.empty() || !host_.server ? "" : host_.server();
    if (env.empty() && server.empty()) {
        if (!asking_.empty()) {
            asking_.clear();
            gtk_widget_set_visible(box_, FALSE);
        }
        return;
    }
    const std::string ask = env.empty() ? server : env;
    const std::string words =
        env.empty() ? (host_.serverQuestion ? host_.serverQuestion() : "")
                    : FolderTools::environmentQuestion(env);
    if (ask != asking_ || words != text()) {
        asking_ = ask;
        server_ = env.empty();
        gtk_label_set_text(GTK_LABEL(label_), words.c_str());
        const std::string tip = server_ ? FolderTools::projectCodeTooltip(words, "Edit")
                                        : FolderTools::environmentTooltip(env, "Edit");
        gtk_widget_set_tooltip_text(label_, tip.c_str());
        gtk_button_set_label(GTK_BUTTON(allow_), server_ ? "Allow" : "Use It");
        gtk_button_set_label(GTK_BUTTON(deny_), server_ ? "Don't Allow" : "Don't Use It");
    }
    gtk_widget_set_visible(box_, TRUE);
}

void ConsentBar::answer(bool allow) {
    if (asking_.empty()) return;
    // Every window on the folder follows, this bar included (listen).
    FolderConsent::set(host_.root ? host_.root() : "", allow);
}

void ConsentBar::putOff() {
    if (asking_.empty()) return;
    FolderConsent::putOff(host_.root ? host_.root() : "");
}
