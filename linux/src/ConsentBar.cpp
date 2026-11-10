// ConsentBar.cpp — see ConsentBar.h. The words are the core's
// (FolderTools.h), which are the Mac's, and the install offer's the Mac's
// (InstallPanel.mm).
#include "ConsentBar.h"

#include "FolderConsent.h"
#include "FolderTools.h"
#include "InstallHints.h"
#include "InstallView.h"
#include "LspClient.h"
#include "Palette.h"

#include <cstdio>

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

const char* kInstallClose = "Close (it stays closed until minicode starts again)";
const char* kConsentClose = "Ask again next time minicode starts";

// The offer is made once a language a launch, across windows (the Mac's
// MCInstallBarTakeOffer).
std::set<std::string>& offered() {
    static std::set<std::string>* keys = new std::set<std::string>();
    return *keys;
}

bool takeOffer(const std::string& key) { return offered().insert(key).second; }

std::string mutedFile() {
    gchar* p = g_build_filename(g_get_user_state_dir(), "minicode", "install-hints.json", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

std::set<std::string> readMuted() {
    gchar* text = nullptr;
    gsize len = 0;
    std::set<std::string> out;
    if (g_file_get_contents(mutedFile().c_str(), &text, &len, nullptr))
        out = Install::decodeMuted(std::string(text, len));
    g_free(text);
    return out;
}

}  // namespace

bool ConsentBar::installMuted(const std::string& key) { return readMuted().count(key) > 0; }

void ConsentBar::setInstallMuted(const std::string& key, bool muted) {
    std::set<std::string> all = readMuted();
    if (muted) all.insert(key);
    else all.erase(key);
    const std::string path = mutedFile();
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    const std::string text = Install::encodeMuted(all);
    GError* err = nullptr;
    if (!g_file_set_contents_full(path.c_str(), text.data(), (gssize)text.size(),
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        fprintf(stderr, "minicode: could not write %s: %s\n", path.c_str(),
                err ? err->message : "unknown error");
        g_clear_error(&err);
    }
}

void ConsentBar::forgetInstallOffers() { offered().clear(); }

ConsentBar::ConsentBar(Host host) : host_(std::move(host)) {
    installCss();
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box_, "minicode-consent-bar");
    label_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label_), 0);
    gtk_label_set_wrap(GTK_LABEL(label_), TRUE);
    gtk_widget_set_hexpand(label_, TRUE);
    allow_ = gtk_button_new_with_label("Use It");
    // Install…: its popover is filled each time it opens, so the command
    // suits the installers there are then.
    install_ = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(install_), "Install…");
    popover_ = gtk_popover_new();
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(install_), popover_);
    gtk_menu_button_set_create_popup_func(
        GTK_MENU_BUTTON(install_),
        [](GtkMenuButton*, gpointer self) { static_cast<ConsentBar*>(self)->fillInstallPopover(); },
        this, nullptr);
    gtk_widget_set_visible(install_, FALSE);
    deny_ = gtk_button_new_with_label("Don't Use It");
    close_ = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close_, "flat");
    gtk_widget_set_tooltip_text(close_, kConsentClose);
    for (GtkWidget* w : {label_, allow_, install_, deny_, close_}) gtk_box_append(GTK_BOX(box_), w);
    for (GtkWidget* b : {allow_, install_, deny_, close_}) gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(allow_, "clicked",
                             G_CALLBACK(+[](ConsentBar* self) { self->answer(true); }), this);
    g_signal_connect_swapped(deny_, "clicked", G_CALLBACK(+[](ConsentBar* self) { self->deny(); }),
                             this);
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
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(install_), nullptr, nullptr, nullptr);
}

std::string ConsentBar::text() const { return gtk_label_get_text(GTK_LABEL(label_)); }

InstallView* ConsentBar::installView() const {
    GtkWidget* child = gtk_popover_get_child(GTK_POPOVER(popover_));
    if (!child || !gtk_widget_get_visible(popover_)) return nullptr;
    return static_cast<InstallView*>(g_object_get_data(G_OBJECT(child), "minicode-install-view"));
}

void ConsentBar::refresh() {
    const std::string root = host_.root ? host_.root() : "";
    const bool putOff = FolderConsent::isPutOff(root);
    // The folder's own Python environment comes first: until it is
    // answered the servers run without it, and a server missing from PATH
    // may be in it. Then a server that would run the folder's code. Then
    // the install offer for a missing server.
    std::string env = putOff || !host_.environment ? "" : host_.environment();
    std::string server = putOff || !env.empty() || !host_.server ? "" : host_.server();
    if (env.empty() && server.empty()) {
        asking_.clear();
        if (!refreshInstall()) gtk_widget_set_visible(box_, FALSE);
        return;
    }
    if (!installing_.empty()) {
        installing_.clear();
        gtk_menu_button_popdown(GTK_MENU_BUTTON(install_));
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
        gtk_widget_set_tooltip_text(close_, kConsentClose);
    }
    gtk_widget_set_visible(allow_, TRUE);
    gtk_widget_set_visible(install_, FALSE);
    gtk_widget_set_visible(box_, TRUE);
}

// The Mac's updateInstallBar: a language not offered yet this launch takes
// the bar when its server is missing, and keeps it until the server turns
// up, the bar is closed, or the language is muted. Another file on show
// hides it; its language back on show brings it back.
bool ConsentBar::refreshInstall() {
    const std::string missing = host_.missing ? host_.missing() : "";
    const std::string language = host_.language ? host_.language() : "";
    if (!installKey_.empty() && missing.empty() && language == installKey_) {
        installKey_.clear();   // found, or switched off: the bar has done its job
    } else if (!missing.empty() && missing != installKey_ && !installMuted(missing) &&
               takeOffer(missing)) {
        installKey_ = missing;
    }
    if (installKey_.empty() || installKey_ != missing) {
        if (!installing_.empty()) {
            installing_.clear();
            gtk_menu_button_popdown(GTK_MENU_BUTTON(install_));
        }
        return false;
    }
    if (installing_ != installKey_) {
        installing_ = installKey_;
        const std::string words = "No " + Lsp::serverDisplayName(installing_) +
                                  " language server is installed, so there are no "
                                  "completions or error checks.";
        gtk_label_set_text(GTK_LABEL(label_), words.c_str());
        gtk_widget_set_tooltip_text(label_, words.c_str());
        gtk_button_set_label(GTK_BUTTON(deny_), "Don't Show Again");
        gtk_widget_set_tooltip_text(close_, kInstallClose);
    }
    gtk_widget_set_visible(allow_, FALSE);
    gtk_widget_set_visible(install_, TRUE);
    gtk_widget_set_visible(box_, TRUE);
    return true;
}

// The popover's view, made as it opens.
void ConsentBar::fillInstallPopover() {
    if (installing_.empty()) return;
    const std::string setting = host_.setting ? host_.setting(installing_) : "";
    const std::string message =
        setting.empty() ? ""
                        : "lsp." + installing_ + " in the settings runs " + setting +
                              ", which was not found. The server minicode suggests is below.";
    InstallView* view = InstallView::forServer(installing_, message, host_.runInTerminal);
    GtkWidget* menu = install_;
    view->onDone = [menu] { gtk_menu_button_popdown(GTK_MENU_BUTTON(menu)); };
    gtk_popover_set_child(GTK_POPOVER(popover_), view->widget());   // the old view goes
}

void ConsentBar::answer(bool allow) {
    if (asking_.empty()) return;
    // Every window on the folder follows, this bar included (listen).
    FolderConsent::set(host_.root ? host_.root() : "", allow);
}

// Don't Use It, Don't Allow, or Don't Show Again.
void ConsentBar::deny() {
    if (!installing_.empty()) {
        setInstallMuted(installKey_, true);
        installKey_.clear();
        refresh();
        return;
    }
    answer(false);
}

// ×: the question until minicode starts again, or the install offer, which
// is made once a launch.
void ConsentBar::putOff() {
    if (!installing_.empty()) {
        installKey_.clear();
        refresh();
        return;
    }
    if (asking_.empty()) return;
    FolderConsent::putOff(host_.root ? host_.root() : "");
}
