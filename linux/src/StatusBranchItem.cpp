// StatusBranchItem.cpp — see StatusBranchItem.h.
#include "StatusBranchItem.h"

// A flat button holding one label, which takes the status bar's text color
// through the "minicode-status label" rule.
static GtkWidget* flatButton() {
    GtkWidget* b = gtk_button_new_with_label("");
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_focus_on_click(b, FALSE);
    return b;
}

StatusBranchItem::StatusBranchItem() {
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    name_ = flatButton();
    counts_ = flatButton();
    GtkWidget* label = gtk_button_get_child(GTK_BUTTON(name_));
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box_), name_);
    gtk_box_append(GTK_BOX(box_), counts_);
    gtk_widget_set_visible(box_, FALSE);
    g_signal_connect(name_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        auto* self = static_cast<StatusBranchItem*>(p);
        if (self->onSwitchBranch) self->onSwitchBranch();
    }), this);
    g_signal_connect(counts_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        auto* self = static_cast<StatusBranchItem*>(p);
        if (self->onSync) self->onSync();
    }), this);
}

void StatusBranchItem::show(const Git::StatusBranch& branch) {
    gtk_widget_set_visible(box_, branch.shown);
    if (!branch.shown) return;
    gtk_button_set_label(GTK_BUTTON(name_), branch.name.c_str());
    gtk_button_set_label(GTK_BUTTON(counts_), branch.counts.c_str());
    gtk_widget_set_visible(counts_, !branch.counts.empty());
    gtk_widget_set_tooltip_text(box_, branch.tip.c_str());
}
