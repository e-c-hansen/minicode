// CsvTable.cpp — see CsvTable.h.
#include "CsvTable.h"

#include <cstdlib>
#include <vector>

namespace {

std::string columnName(size_t c) {
    std::string s;
    for (size_t n = c + 1; n > 0; n = (n - 1) / 26) s.insert(s.begin(), (char)('A' + (n - 1) % 26));
    return s;
}

size_t rowOf(GtkListItem* item) {
    GObject* o = G_OBJECT(gtk_list_item_get_item(item));
    return o ? (size_t)strtoull(gtk_string_object_get_string(GTK_STRING_OBJECT(o)), nullptr, 10) : 0;
}

bool continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

}  // namespace

CsvTable::CsvTable() {
    view_ = gtk_column_view_new(nullptr);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view_), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(view_), TRUE);
    gtk_widget_add_css_class(view_, "csv-table");
    scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);
    g_object_ref_sink(scroller_);
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(view_, keys);
}

CsvTable::~CsvTable() { g_object_unref(scroller_); }

gboolean CsvTable::onKey(GtkEventControllerKey*, guint key, guint, GdkModifierType mods,
                         gpointer selfp) {
    CsvTable* self = static_cast<CsvTable*>(selfp);
    if (!(mods & GDK_CONTROL_MASK) || self->editing_ || !self->onUndo) return FALSE;
    const bool shift = mods & GDK_SHIFT_MASK;
    if (key == GDK_KEY_z || key == GDK_KEY_Z) { self->onUndo(shift); return TRUE; }
    if (key == GDK_KEY_y || key == GDK_KEY_Y) { self->onUndo(true); return TRUE; }
    return FALSE;
}

void CsvTable::setupNumber(GtkListItemFactory*, GtkListItem* item, gpointer) {
    GtkWidget* l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_list_item_set_child(item, l);
}

void CsvTable::bindNumber(GtkListItemFactory*, GtkListItem* item, gpointer) {
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)),
                       std::to_string(rowOf(item) + 1).c_str());
}

void CsvTable::setupCell(GtkListItemFactory*, GtkListItem* item, gpointer selfp) {
    GtkWidget* l = gtk_editable_label_new("");
    gtk_widget_set_hexpand(l, TRUE);
    g_object_set_data(G_OBJECT(l), "table", selfp);
    g_signal_connect(l, "notify::editing", G_CALLBACK(onEditing), nullptr);
    gtk_list_item_set_child(item, l);
}

void CsvTable::bindCell(GtkListItemFactory* f, GtkListItem* item, gpointer selfp) {
    CsvTable* self = static_cast<CsvTable*>(selfp);
    GtkWidget* l = gtk_list_item_get_child(item);
    const size_t row = rowOf(item);
    const size_t col = (size_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(f), "col"));
    g_object_set_data(G_OBJECT(l), "row", GSIZE_TO_POINTER(row + 1));
    g_object_set_data(G_OBJECT(l), "col", GSIZE_TO_POINTER(col + 1));
    self->binding_ = true;
    gtk_editable_set_text(GTK_EDITABLE(l), self->doc_.cell(row, col).c_str());
    self->binding_ = false;
}

void CsvTable::onEditing(GObject* o, GParamSpec*, gpointer) {
    GtkWidget* l = GTK_WIDGET(o);
    CsvTable* self = static_cast<CsvTable*>(g_object_get_data(o, "table"));
    if (self->binding_) return;
    if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(l))) {
        self->editing_ = l;
        return;
    }
    if (self->editing_ == l) self->editing_ = nullptr;
    const gsize row = (gsize)g_object_get_data(o, "row");
    const gsize col = (gsize)g_object_get_data(o, "col");
    if (!row || !col) return;
    // Stopping with a cancel restores the old text, so this finds no change.
    self->setCell(row - 1, col - 1, gtk_editable_get_text(GTK_EDITABLE(l)));
}

void CsvTable::build() {
    // Every column goes, and the rows are numbered anew.
    GListModel* cols = gtk_column_view_get_columns(GTK_COLUMN_VIEW(view_));
    while (g_list_model_get_n_items(cols) > 0) {
        GtkColumnViewColumn* c = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(cols, 0));
        gtk_column_view_remove_column(GTK_COLUMN_VIEW(view_), c);
        g_object_unref(c);
    }
    {
        GtkListItemFactory* f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(setupNumber), this);
        g_signal_connect(f, "bind", G_CALLBACK(bindNumber), this);
        GtkColumnViewColumn* c = gtk_column_view_column_new("", f);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view_), c);
        g_object_unref(c);
    }
    for (size_t c = 0; c < doc_.columns(); c++) {
        GtkListItemFactory* f = gtk_signal_list_item_factory_new();
        g_object_set_data(G_OBJECT(f), "col", GINT_TO_POINTER((int)c));
        g_signal_connect(f, "setup", G_CALLBACK(setupCell), this);
        g_signal_connect(f, "bind", G_CALLBACK(bindCell), this);
        GtkColumnViewColumn* col = gtk_column_view_column_new(columnName(c).c_str(), f);
        gtk_column_view_column_set_resizable(col, TRUE);
        gtk_column_view_column_set_fixed_width(col, 140);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view_), col);
        g_object_unref(col);
    }
    std::vector<std::string> nums;
    nums.reserve(doc_.rows());
    for (size_t r = 0; r < doc_.rows(); r++) nums.push_back(std::to_string(r));
    std::vector<const char*> ptrs;
    ptrs.reserve(nums.size() + 1);
    for (const std::string& s : nums) ptrs.push_back(s.c_str());
    ptrs.push_back(nullptr);
    rowsModel_ = gtk_string_list_new(ptrs.data());
    GtkSelectionModel* sel = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(rowsModel_)));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(view_), sel);
    g_object_unref(sel);
}

void CsvTable::show(const std::string& text, char delimiter) {
    editing_ = nullptr;
    text_ = text;
    doc_ = Csv::Document::parse(text, delimiter);
    build();
}

void CsvTable::reload(const std::string& text) {
    if (text == text_) return;
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double keep = gtk_adjustment_get_value(v);
    show(text, doc_.delimiter());
    gtk_adjustment_set_value(v, keep);
}

void CsvTable::clear() {
    editing_ = nullptr;
    doc_ = Csv::Document();
    text_.clear();
    GListModel* cols = gtk_column_view_get_columns(GTK_COLUMN_VIEW(view_));
    while (g_list_model_get_n_items(cols) > 0) {
        GtkColumnViewColumn* c = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(cols, 0));
        gtk_column_view_remove_column(GTK_COLUMN_VIEW(view_), c);
        g_object_unref(c);
    }
    gtk_column_view_set_model(GTK_COLUMN_VIEW(view_), nullptr);
}

void CsvTable::commit() {
    if (!editing_) return;
    GtkWidget* l = editing_;
    gtk_editable_label_stop_editing(GTK_EDITABLE_LABEL(l), TRUE);   // onEditing commits
    editing_ = nullptr;
}

bool CsvTable::setCell(size_t row, size_t column, const std::string& value) {
    if (!doc_.setCell(row, column, value)) return false;
    const std::string now = doc_.serialize();
    // The bytes both texts share at the start and at the end stay.
    size_t a = 0;
    const size_t most = std::min(text_.size(), now.size());
    while (a < most && text_[a] == now[a]) a++;
    while (a > 0 && a < text_.size() && continuation((unsigned char)text_[a])) a--;
    size_t tail = 0;
    while (tail < most - a && text_[text_.size() - 1 - tail] == now[now.size() - 1 - tail]) tail++;
    while (tail > 0 && continuation((unsigned char)text_[text_.size() - tail])) tail--;
    const std::string removed = text_.substr(a, text_.size() - tail - a);
    const std::string inserted = now.substr(a, now.size() - tail - a);
    const int start = (int)g_utf8_strlen(text_.c_str(), (gssize)a);
    const int count = (int)g_utf8_strlen(removed.c_str(), (gssize)removed.size());
    text_ = now;
    if (onReplace) onReplace(start, count, inserted);
    return true;
}
