// FormatOffer.cpp — see FormatOffer.h. The words are the Mac's.
#include "FormatOffer.h"

#include "ConsentBar.h"
#include "Formatter.h"
#include "FormatRunner.h"

namespace {

bool gOffered = false;   // shown once per launch, across windows

}  // namespace

void FormatOffer::forgetOffered() { gOffered = false; }

FormatOffer::FormatOffer(Host host) : host_(std::move(host)) {
    ConsentBar::ensureCss();
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box_, "minicode-consent-bar");
    label_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label_), 0);
    gtk_label_set_wrap(GTK_LABEL(label_), TRUE);
    gtk_widget_set_hexpand(label_, TRUE);
    gtk_widget_set_tooltip_text(
        label_,
        "Turn On writes format.on-save = project in the settings: from the next save on, "
        "Ctrl+S runs the formatter a project sets up, or one format.<lang> names, and leaves "
        "other files as typed. Edit > Format on Save turns it off again. Don't Ask Again stops "
        "this offer; the menu item stays.");
    turnOn_ = gtk_button_new_with_label("Turn On");
    mute_ = gtk_button_new_with_label("Don't Ask Again");
    close_ = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close_, "flat");
    gtk_widget_set_tooltip_text(close_, "Close (asked again after minicode starts again)");
    for (GtkWidget* w : {label_, turnOn_, mute_, close_}) gtk_box_append(GTK_BOX(box_), w);
    for (GtkWidget* b : {turnOn_, mute_, close_}) gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(turnOn_, "clicked",
                             G_CALLBACK(+[](FormatOffer* self) { self->doTurnOn(); }), this);
    g_signal_connect_swapped(mute_, "clicked", G_CALLBACK(+[](FormatOffer* self) {
                                 ConsentBar::setInstallMuted(key(), true);
                                 self->hide();
                             }),
                             this);
    g_signal_connect_swapped(close_, "clicked",
                             G_CALLBACK(+[](FormatOffer* self) { self->hide(); }), this);
    gtk_widget_set_visible(box_, FALSE);
}

FormatOffer::~FormatOffer() = default;

bool FormatOffer::shown() const { return gtk_widget_get_visible(box_); }

std::string FormatOffer::text() const { return gtk_label_get_text(GTK_LABEL(label_)); }

bool FormatOffer::due() const {
    return !gOffered && !ConsentBar::installMuted(key()) &&
           !(host_.formatOnSave && host_.formatOnSave());
}

void FormatOffer::offerAfterSaving(const std::string& path) {
    if (checking_ || path.empty() || !due()) return;
    const std::string lang = Formatter::languageFor(path);
    if (lang.empty()) return;
    checking_ = true;
    std::weak_ptr<int> alive = alive_;
    FormatRunner::offerOnSave(path, host_.root ? host_.root() : "",
                              host_.setting ? host_.setting(lang) : "",
                              [this, alive](std::string question) {
                                  if (alive.expired()) return;
                                  checking_ = false;
                                  // Answered elsewhere meanwhile, or another window offered it.
                                  if (!question.empty() && due()) show(question);
                              });
}

void FormatOffer::show(const std::string& question) {
    gOffered = true;
    gtk_label_set_text(GTK_LABEL(label_), question.c_str());
    gtk_widget_set_visible(box_, TRUE);
}

void FormatOffer::hide() { gtk_widget_set_visible(box_, FALSE); }

void FormatOffer::doTurnOn() {
    std::string why;
    if (!host_.turnOn || !host_.turnOn(&why)) {
        // The settings file has unsaved changes in a tab: said in the status bar.
        if (host_.note && !why.empty()) host_.note(why);
        return;
    }
    hide();
    if (host_.note)
        host_.note("Format on save is on for formatters a project sets up. "
                   "Edit > Format on Save turns it off.");
}

void FormatOffer::settingsChanged() {
    if (shown() && host_.formatOnSave && host_.formatOnSave()) hide();
}
