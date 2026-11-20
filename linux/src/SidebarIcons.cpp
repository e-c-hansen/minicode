// SidebarIcons.cpp — see SidebarIcons.h.
#include "SidebarIcons.h"

namespace {

// Each panel's icon names, best first; the last of each is in Adwaita, GTK's
// default, and in Yaru and Papirus. Nearer pictures where a theme has them:
// a branch for Source Control, a processor for Processes; a star for Agents
// (the Mac's "sparkles").
const char* const kTreeIcons[] = {"folder-symbolic", nullptr};
const char* const kScmIcons[] = {"vcs-branch-symbolic", "object-merge-symbolic",
                                 "emblem-shared-symbolic", "media-playlist-shuffle-symbolic", nullptr};
const char* const kAgentIcons[] = {"sparkles-symbolic", "starred-symbolic", nullptr};
const char* const kProcessIcons[] = {"cpu-symbolic", "utilities-system-monitor-symbolic",
                                     "system-run-symbolic", nullptr};
const char* const* const kIcons[SidebarPanels::kCount] = {kTreeIcons, kScmIcons, kAgentIcons,
                                                         kProcessIcons};

GtkWidget* overlayPart(const char* cssClass, GtkAlign h, GtkAlign v) {
    GtkWidget* w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(w, cssClass);
    gtk_widget_set_halign(w, h);
    gtk_widget_set_valign(w, v);
    gtk_widget_set_can_target(w, FALSE);
    gtk_widget_set_visible(w, FALSE);
    return w;
}

}  // namespace

SidebarIcons::SidebarIcons() {
    root_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(root_, "minicode-sidebar-icons");
    for (int i = 0; i < SidebarPanels::kCount; i++) {
        const Panel panel = (Panel)i;
        Item& it = items_[i];
        const char* name = SidebarPanels::info(panel).name;

        GtkWidget* overlay = gtk_overlay_new();
        it.image = gtk_image_new();
        gtk_widget_set_halign(it.image, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(it.image, GTK_ALIGN_CENTER);
        gtk_overlay_set_child(GTK_OVERLAY(overlay), it.image);
        // The panel on show: a short line in the accent along the bottom.
        it.mark = overlayPart("minicode-sidebar-icon-mark", GTK_ALIGN_CENTER, GTK_ALIGN_END);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), it.mark);
        // A count, at the top right.
        it.badge = gtk_label_new(nullptr);
        gtk_widget_add_css_class(it.badge, "minicode-sidebar-icon-badge");
        gtk_widget_set_halign(it.badge, GTK_ALIGN_END);
        gtk_widget_set_valign(it.badge, GTK_ALIGN_START);
        gtk_widget_set_can_target(it.badge, FALSE);
        gtk_widget_set_visible(it.badge, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), it.badge);
        // A session waiting: an orange dot, in the badge's place.
        it.dot = overlayPart("minicode-sidebar-icon-dot", GTK_ALIGN_END, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), it.dot);

        it.button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(it.button), overlay);
        gtk_widget_add_css_class(it.button, "flat");
        gtk_widget_add_css_class(it.button, "minicode-sidebar-icon");
        // A click shows the panel; the keyboard stays where it was until
        // the panel takes it, and Tab never stops here.
        gtk_widget_set_focusable(it.button, FALSE);
        gtk_widget_set_focus_on_click(it.button, FALSE);
        it.tip = name;
        gtk_widget_set_tooltip_text(it.button, name);
        gtk_accessible_update_property(GTK_ACCESSIBLE(it.button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       name, -1);
        g_object_set_data(G_OBJECT(it.button), "minicode-panel", GINT_TO_POINTER(i));
        g_signal_connect(it.button, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer p) {
            auto* self = static_cast<SidebarIcons*>(p);
            const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "minicode-panel"));
            if (self->onPick) self->onPick((Panel)i);
        }), this);
        gtk_box_append(GTK_BOX(root_), it.button);
    }
    theme_ = gtk_icon_theme_get_for_display(gtk_widget_get_display(root_));
    if (theme_) {
        g_object_ref(theme_);
        themeChanged_ = g_signal_connect(theme_, "changed", G_CALLBACK(+[](GtkIconTheme*, gpointer p) {
            static_cast<SidebarIcons*>(p)->resolveIcons();
        }), this);
    }
    resolveIcons();
    setCurrent(Panel::Tree);
}

SidebarIcons::~SidebarIcons() {
    if (theme_) {
        if (themeChanged_) g_signal_handler_disconnect(theme_, themeChanged_);
        g_object_unref(theme_);
    }
    // The buttons outlive this object only while their window is torn down;
    // a click then reaches nothing.
    for (Item& it : items_)
        if (it.button) g_signal_handlers_disconnect_by_data(it.button, this);
}

const SidebarIcons::Item& SidebarIcons::item(Panel p) const {
    const int i = (int)p;
    return items_[i >= 0 && i < SidebarPanels::kCount ? i : 0];
}

SidebarIcons::Item& SidebarIcons::item(Panel p) {
    const int i = (int)p;
    return items_[i >= 0 && i < SidebarPanels::kCount ? i : 0];
}

void SidebarIcons::resolveIcons() {
    for (int i = 0; i < SidebarPanels::kCount; i++) {
        Item& it = items_[i];
        const char* const* names = kIcons[i];
        const char* chosen = nullptr;
        for (const char* const* n = names; *n && !chosen; n++)
            if (theme_ && gtk_icon_theme_has_icon(theme_, *n)) chosen = *n;
        it.found = chosen != nullptr;
        if (!chosen) {   // the last of the list, which GTK draws as missing
            const char* const* n = names;
            while (n[1]) n++;
            chosen = *n;
        }
        if (it.icon == chosen) continue;
        it.icon = chosen;
        gtk_image_set_from_icon_name(GTK_IMAGE(it.image), chosen);
    }
}

void SidebarIcons::setCurrent(Panel p) {
    current_ = p;
    for (int i = 0; i < SidebarPanels::kCount; i++) {
        Item& it = items_[i];
        const bool on = i == (int)p;
        gtk_widget_set_visible(it.mark, on);
        if (on) gtk_widget_add_css_class(it.button, "current");
        else gtk_widget_remove_css_class(it.button, "current");
    }
}

void SidebarIcons::setBadge(Panel p, const std::string& text) {
    Item& it = item(p);
    if (it.badgeText == text) return;
    it.badgeText = text;
    gtk_label_set_text(GTK_LABEL(it.badge), text.c_str());
    gtk_widget_set_visible(it.badge, !text.empty() && !it.dotOn);
}

void SidebarIcons::setDot(Panel p, bool on) {
    Item& it = item(p);
    if (it.dotOn == on) return;
    it.dotOn = on;
    gtk_widget_set_visible(it.dot, on);
    gtk_widget_set_visible(it.badge, !it.badgeText.empty() && !on);
}

void SidebarIcons::setTip(Panel p, const std::string& tip) {
    Item& it = item(p);
    if (it.tip == tip) return;
    it.tip = tip;
    gtk_widget_set_tooltip_text(it.button, tip.c_str());
}
