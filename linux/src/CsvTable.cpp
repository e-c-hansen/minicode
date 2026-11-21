// CsvTable.cpp — see CsvTable.h.
#include "CsvTable.h"

#include <glib/gstdio.h>

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

// The files whose first row the reader said is, or is not, a header, one
// line each ("1 /path" or "0 /path"), the oldest first and dropped past
// kMaxChoices.
constexpr size_t kMaxChoices = 500;

std::string choicesFile() {
    gchar* p = g_build_filename(g_get_user_state_dir(), "mutible", "csv-headers.txt", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

std::vector<std::pair<std::string, bool>> readChoices() {
    std::vector<std::pair<std::string, bool>> out;
    gchar* text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(choicesFile().c_str(), &text, &len, nullptr)) return out;
    const std::string all(text, len);
    g_free(text);
    size_t at = 0;
    while (at < all.size()) {
        size_t nl = all.find('\n', at);
        if (nl == std::string::npos) nl = all.size();
        if (nl - at > 2 && (all[at] == '0' || all[at] == '1') && all[at + 1] == ' ')
            out.push_back({all.substr(at + 2, nl - at - 2), all[at] == '1'});
        at = nl + 1;
    }
    return out;
}

// What the reader said for `path`: 1 a header, 0 not, -1 nothing.
int storedChoice(const std::string& path) {
    if (path.empty()) return -1;
    int found = -1;
    for (const auto& c : readChoices())
        if (c.first == path) found = c.second ? 1 : 0;
    return found;
}

// Remembers `header` for `path`, now the most recent.
void storeChoice(const std::string& path, bool header) {
    if (path.empty() || path.find('\n') != std::string::npos) return;
    std::vector<std::pair<std::string, bool>> list = readChoices();
    for (size_t i = list.size(); i-- > 0;)
        if (list[i].first == path) list.erase(list.begin() + (long)i);
    list.push_back({path, header});
    while (list.size() > kMaxChoices) list.erase(list.begin());
    std::string text;
    for (const auto& c : list) text += std::string(c.second ? "1 " : "0 ") + c.first + "\n";
    const std::string file = choicesFile();
    gchar* dir = g_path_get_dirname(file.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_file_set_contents_full(file.c_str(), text.data(), (gssize)text.size(),
                             G_FILE_SET_CONTENTS_CONSISTENT, 0600, nullptr);
}

// The text without its spaces and line ends at either end.
std::string trimmed(const std::string& s) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    size_t a = 0, b = s.size();
    while (a < b && space(s[a])) a++;
    while (b > a && space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

}  // namespace

CsvTable::CsvTable() {
    view_ = gtk_column_view_new(nullptr);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view_), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(view_), TRUE);
    gtk_widget_add_css_class(view_, "csv-table");
    {
        // A header's names are bold.
        static bool done = false;
        if (!done) {
            done = true;
            GtkCssProvider* p = gtk_css_provider_new();
            gtk_css_provider_load_from_string(p, ".csv-table.csv-named > header label { font-weight: bold; }");
            gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(p);
        }
    }
    scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
    gtk_widget_set_hexpand(scroller_, TRUE);
    gtk_widget_set_vexpand(scroller_, TRUE);
    g_object_ref_sink(scroller_);
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(view_, keys);

    // The right click menu: First Row Is Header, a check.
    menu_ = gtk_popover_new();
    headerItem_ = gtk_check_button_new_with_label("First Row Is Header");
    gtk_popover_set_child(GTK_POPOVER(menu_), headerItem_);
    gtk_widget_set_parent(menu_, view_);
    g_signal_connect(headerItem_, "toggled", G_CALLBACK(onHeaderToggled), this);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onRightClick), this);
    gtk_widget_add_controller(view_, GTK_EVENT_CONTROLLER(click));
}

CsvTable::~CsvTable() {
    gtk_widget_unparent(menu_);
    g_object_unref(scroller_);
}

// A right click opens the menu at the pointer, its check showing the state.
void CsvTable::onRightClick(GtkGestureClick*, int, double x, double y, gpointer selfp) {
    CsvTable* self = static_cast<CsvTable*>(selfp);
    self->syncing_ = true;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(self->headerItem_), self->header_);
    self->syncing_ = false;
    GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(self->menu_), &at);
    gtk_popover_popup(GTK_POPOVER(self->menu_));
}

// The item chosen: the first row becomes, or stops being, the header.
void CsvTable::onHeaderToggled(GtkCheckButton* b, gpointer selfp) {
    CsvTable* self = static_cast<CsvTable*>(selfp);
    if (self->syncing_) return;
    self->setFirstRowIsHeader(gtk_check_button_get_active(b));
    gtk_popover_popdown(GTK_POPOVER(self->menu_));
}

// Shows or hides the first row as the header, and remembers it for the file.
void CsvTable::setFirstRowIsHeader(bool on) {
    storeChoice(path_, on);
    if (on == header_) return;
    commit();
    header_ = on;
    build();
}

std::string CsvTable::columnTitle(size_t c) const {
    GListModel* cols = gtk_column_view_get_columns(GTK_COLUMN_VIEW(view_));
    GtkColumnViewColumn* col = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(cols, (guint)c + 1));
    if (!col) return "";
    const char* t = gtk_column_view_column_get_title(col);
    std::string out = t ? t : "";
    g_object_unref(col);
    return out;
}

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
        // The header's field names the column; A, B, C without one.
        const std::string name = headerRows() ? trimmed(doc_.cell(0, c)) : "";
        GtkColumnViewColumn* col =
            gtk_column_view_column_new((name.empty() ? columnName(c) : name).c_str(), f);
        gtk_column_view_column_set_resizable(col, TRUE);
        gtk_column_view_column_set_fixed_width(col, 140);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view_), col);
        g_object_unref(col);
    }
    std::vector<std::string> nums;
    nums.reserve(doc_.rows());
    syncing_ = true;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(headerItem_), header_);
    syncing_ = false;
    if (headerRows()) gtk_widget_add_css_class(view_, "csv-named");
    else gtk_widget_remove_css_class(view_, "csv-named");
    for (size_t r = headerRows(); r < doc_.rows(); r++) nums.push_back(std::to_string(r));
    std::vector<const char*> ptrs;
    ptrs.reserve(nums.size() + 1);
    for (const std::string& s : nums) ptrs.push_back(s.c_str());
    ptrs.push_back(nullptr);
    rowsModel_ = gtk_string_list_new(ptrs.data());
    GtkSelectionModel* sel = GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(rowsModel_)));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(view_), sel);
    g_object_unref(sel);
}

// Parses `text` and shows it, the header choice as it is.
void CsvTable::load(const std::string& text, char delimiter) {
    editing_ = nullptr;
    text_ = text;
    doc_ = Csv::Document::parse(text, delimiter);
    build();
}

void CsvTable::show(const std::string& text, char delimiter, const std::string& path) {
    path_ = path;
    const Csv::Document parsed = Csv::Document::parse(text, delimiter);
    const int stored = storedChoice(path);
    header_ = stored >= 0 ? stored == 1 : Csv::looksLikeHeader(parsed);
    load(text, delimiter);
}

void CsvTable::reload(const std::string& text) {
    if (text == text_) return;
    GtkAdjustment* v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double keep = gtk_adjustment_get_value(v);
    load(text, doc_.delimiter());
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
