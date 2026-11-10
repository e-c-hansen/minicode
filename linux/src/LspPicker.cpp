// LspPicker.cpp — see LspPicker.h.
#include "LspPicker.h"

#include "Palette.h"

#include <algorithm>

namespace lspgtk {

namespace {

void installCss() {
    static bool done = false;
    if (done || !gdk_display_get_default()) return;
    done = true;
    const std::string css =
        "popover.minicode-picker > contents, popover.minicode-rename > contents {"
        "  background-color: " + std::string(pal::SidebarBg) + ";"
        "  color: " + pal::EditorText + ";"
        "  border: 1px solid #454545; border-radius: 4px; padding: 0; }"
        "popover.minicode-picker list { background: transparent; }"
        "popover.minicode-picker row { padding: 1px 8px; min-height: 0; }"
        "popover.minicode-picker row:selected { background-color: #04395E; }"
        "popover.minicode-picker row.minicode-picker-heading { padding-top: 4px; }"
        "popover.minicode-rename > contents { padding: 4px; }";
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

const int kRowHeight = 22;   // about one row, to size the list's height

}  // namespace

// ------------------------------------------------------------ Picker

Picker::Picker(GtkWidget* view) : view_(view) { installCss(); }

// Its handlers off first: nothing of it is called while it goes.
Picker::~Picker() {
    if (list_) g_signal_handlers_disconnect_by_data(list_, this);
    if (popover_) gtk_widget_unparent(popover_);
}

void Picker::show(const std::vector<Row>& rows, int select, const GdkRectangle& at, int width,
                  int maxRows) {
    if (!popover_) {
        popover_ = gtk_popover_new();
        gtk_widget_add_css_class(popover_, "minicode-picker");
        // Not autohide: an autohide popover grabs the keyboard, and the
        // editor keeps it (LspSession hands the list its keys).
        gtk_popover_set_autohide(GTK_POPOVER(popover_), FALSE);
        gtk_popover_set_has_arrow(GTK_POPOVER(popover_), FALSE);
        gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_BOTTOM);
        gtk_widget_set_focusable(popover_, FALSE);
        gtk_widget_set_can_focus(popover_, FALSE);
        list_ = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
        gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list_), TRUE);
        gtk_widget_set_focusable(list_, FALSE);
        gtk_widget_set_can_focus(list_, FALSE);
        g_signal_connect(list_, "row-activated", G_CALLBACK(onActivated), this);
        scroll_ = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll_), TRUE);
        gtk_widget_set_can_focus(scroll_, FALSE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), list_);
        gtk_popover_set_child(GTK_POPOVER(popover_), scroll_);
        gtk_widget_set_parent(popover_, view_);
    }
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll_),
                                               std::max(1, maxRows) * kRowHeight);
    gtk_widget_set_size_request(scroll_, width, -1);
    gtk_list_box_remove_all(GTK_LIST_BOX(list_));
    rows_ = rows;
    labels_.clear();
    for (const Row& r : rows_) {
        GtkWidget* label = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(label), r.markup.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
        gtk_list_box_append(GTK_LIST_BOX(list_), label);
        GtkWidget* row = gtk_widget_get_parent(label);
        if (row) {
            gtk_widget_set_focusable(row, FALSE);
            if (r.heading) {
                gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
                gtk_widget_add_css_class(row, "minicode-picker-heading");
            }
        }
        labels_.push_back(label);
    }
    gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &at);
    if (!gtk_widget_get_visible(popover_)) gtk_popover_popup(GTK_POPOVER(popover_));
    // The first row to select, moved past a heading.
    int first = std::max(0, select);
    while (first < (int)rows_.size() && rows_[(size_t)first].heading) first++;
    if (first >= (int)rows_.size()) first = -1;
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_)), 0);
    if (first >= 0) this->select(first);
}

void Picker::close() {
    if (popover_ && gtk_widget_get_visible(popover_)) gtk_popover_popdown(GTK_POPOVER(popover_));
}

bool Picker::visible() const { return popover_ && gtk_widget_get_visible(popover_); }

int Picker::selectedRow() const {
    if (!list_) return -1;
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_));
    return row ? gtk_list_box_row_get_index(row) : -1;
}

void Picker::select(int row) {
    GtkListBoxRow* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), row);
    if (!r) return;
    gtk_list_box_select_row(GTK_LIST_BOX(list_), r);
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(r), list_, &b)) return;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll_));
    const double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    const double top = b.origin.y, bottom = top + b.size.height;
    if (top < v) gtk_adjustment_set_value(adj, top);
    else if (bottom > v + page) gtk_adjustment_set_value(adj, bottom - page);
}

void Picker::moveSelection(int delta) {
    std::vector<int> selectable;
    for (size_t i = 0; i < rows_.size(); i++)
        if (!rows_[i].heading) selectable.push_back((int)i);
    if (selectable.empty() || delta == 0) return;
    const int now = selectedRow();
    int at = 0;
    for (size_t k = 0; k < selectable.size(); k++)
        if (selectable[k] == now) at = (int)k;
    const int n = (int)selectable.size();
    int to = at + delta;
    if (delta == 1 || delta == -1) to = (to % n + n) % n;   // single steps wrap
    else to = std::min(std::max(to, 0), n - 1);            // pages stop at the ends
    select(selectable[(size_t)to]);
}

std::vector<std::string> Picker::rowTexts() const {
    std::vector<std::string> out;
    if (!visible()) return out;
    for (GtkWidget* l : labels_) out.push_back(gtk_label_get_text(GTK_LABEL(l)));
    return out;
}

GtkWidget* Picker::rowWidget(int row) const {
    return row >= 0 && (size_t)row < labels_.size() ? labels_[(size_t)row] : nullptr;
}

void Picker::onActivated(GtkListBox*, GtkListBoxRow* row, gpointer selfp) {
    Picker* self = static_cast<Picker*>(selfp);
    const int i = gtk_list_box_row_get_index(row);
    if (i < 0 || (size_t)i >= self->rows_.size() || self->rows_[(size_t)i].heading) return;
    if (self->onAccept) self->onAccept(i);
}

// ------------------------------------------------------------ RenameBox

RenameBox::RenameBox(GtkWidget* view) : view_(view) { installCss(); }

RenameBox::~RenameBox() {
    if (entry_) g_signal_handlers_disconnect_by_data(entry_, this);
    if (popover_) {
        g_signal_handlers_disconnect_by_data(popover_, this);
        gtk_widget_unparent(popover_);
    }
}

void RenameBox::show(const std::string& name, const GdkRectangle& at) {
    if (!popover_) {
        popover_ = gtk_popover_new();
        gtk_widget_add_css_class(popover_, "minicode-rename");
        gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_BOTTOM);
        entry_ = gtk_entry_new();
        gtk_editable_set_width_chars(GTK_EDITABLE(entry_), 32);
        g_signal_connect(entry_, "activate", G_CALLBACK(onActivate), this);
        gtk_popover_set_child(GTK_POPOVER(popover_), entry_);
        g_signal_connect(popover_, "closed", G_CALLBACK(onClosed), this);
        gtk_widget_set_parent(popover_, view_);
    }
    gtk_editable_set_text(GTK_EDITABLE(entry_), name.c_str());
    gtk_popover_set_pointing_to(GTK_POPOVER(popover_), &at);
    gtk_popover_popup(GTK_POPOVER(popover_));
    gtk_widget_grab_focus(entry_);
    gtk_editable_select_region(GTK_EDITABLE(entry_), 0, -1);
}

void RenameBox::close() {
    if (popover_ && gtk_widget_get_visible(popover_)) gtk_popover_popdown(GTK_POPOVER(popover_));
}

bool RenameBox::visible() const { return popover_ && gtk_widget_get_visible(popover_); }

void RenameBox::onActivate(GtkEntry*, gpointer selfp) {
    RenameBox* self = static_cast<RenameBox*>(selfp);
    std::string name = gtk_editable_get_text(GTK_EDITABLE(self->entry_));
    const size_t a = name.find_first_not_of(" \t");
    const size_t b = name.find_last_not_of(" \t");
    name = a == std::string::npos ? std::string() : name.substr(a, b - a + 1);
    if (name.empty()) {
        gtk_widget_error_bell(self->entry_);
        return;
    }
    self->close();
    if (self->onCommit) self->onCommit(name);
}

void RenameBox::onClosed(GtkPopover*, gpointer selfp) {
    RenameBox* self = static_cast<RenameBox*>(selfp);
    if (self->onClose) self->onClose();
}

}  // namespace lspgtk
