// ProblemsPanel.cpp — see ProblemsPanel.h.
#include "ProblemsPanel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// The squiggles' colors (LspSessionDiagnostics.cpp), so a row and its
// underline agree.
const char* severityColor(int severity) {
    switch (severity) {
    case 1: return "#F14C4C";
    case 2: return "#CCA700";
    case 3: return "#3794FF";
    default: return "#9CA3AF";
    }
}

const char* severityLetter(int severity) {
    switch (severity) {
    case 1: return "E";
    case 2: return "W";
    case 3: return "I";
    default: return "H";
    }
}

const char* severityName(int severity) {
    switch (severity) {
    case 1: return "Error";
    case 2: return "Warning";
    case 3: return "Info";
    default: return "Hint";
    }
}

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string headingCounts(const LspProblem& p) {
    std::string out;
    auto add = [&](const std::string& part) { out += (out.empty() ? "" : ", ") + part; };
    if (p.errors) add(plural(p.errors, "error", "errors"));
    if (p.warnings) add(plural(p.warnings, "warning", "warnings"));
    if (p.others) add(plural(p.others, "other", "others"));
    return out;
}

// "pyright(reportUndefinedVariable)", "clang", or "".
std::string sourceText(const LspProblem& p) {
    if (!p.source.empty() && !p.code.empty()) return p.source + "(" + p.code + ")";
    return p.source.empty() ? p.code : p.source;
}

std::string baseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string escape(const std::string& s) {
    char* e = g_markup_escape_text(s.c_str(), -1);
    std::string out = e ? e : "";
    g_free(e);
    return out;
}

bool sameRow(const LspProblem& a, const LspProblem& b) {
    return a.heading == b.heading && a.path == b.path && a.line == b.line &&
           a.column == b.column && a.endLine == b.endLine && a.endColumn == b.endColumn &&
           a.severity == b.severity && a.fullMessage == b.fullMessage && a.source == b.source &&
           a.code == b.code && a.errors == b.errors && a.warnings == b.warnings &&
           a.others == b.others;
}

bool sameProblem(const LspProblem& a, const LspProblem& b) {
    return !a.heading && !b.heading && a.path == b.path && a.line == b.line &&
           a.column == b.column && a.severity == b.severity && a.message == b.message;
}

// A severity mark, drawn: a red disc with a cross, a yellow triangle with
// "!", a blue disc with "i", as the Mac's.
void drawSeverity(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
    const int severity = GPOINTER_TO_INT(data);
    if (severity <= 0) return;
    GdkRGBA c;
    gdk_rgba_parse(&c, severityColor(severity));
    const double size = std::min(w, h) - 1, x = (w - size) / 2.0, y = (h - size) / 2.0;
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, 1);
    if (severity == 2) {
        cairo_move_to(cr, x + size / 2, y);
        cairo_line_to(cr, x + size, y + size);
        cairo_line_to(cr, x, y + size);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else if (severity == 4) {
        cairo_arc(cr, x + size / 2, y + size / 2, size / 2 - 0.6, 0, 2 * M_PI);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);
        return;
    } else {
        cairo_arc(cr, x + size / 2, y + size / 2, size / 2, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    if (severity == 1) {
        const double in = size * 0.32;
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_set_line_width(cr, 1.4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, x + in, y + in);
        cairo_line_to(cr, x + size - in, y + size - in);
        cairo_move_to(cr, x + in, y + size - in);
        cairo_line_to(cr, x + size - in, y + in);
        cairo_stroke(cr);
        return;
    }
    // "!" on the triangle, "i" on the disc.
    if (severity == 2) cairo_set_source_rgba(cr, 0x1E / 255.0, 0x1E / 255.0, 0x1E / 255.0, 1);
    else cairo_set_source_rgba(cr, 1, 1, 1, 1);
    const double cx = x + size / 2;
    cairo_set_line_width(cr, 1.4);
    if (severity == 2) {
        cairo_move_to(cr, cx, y + size * 0.38);
        cairo_line_to(cr, cx, y + size * 0.68);
        cairo_stroke(cr);
        cairo_arc(cr, cx, y + size * 0.84, 0.8, 0, 2 * M_PI);
        cairo_fill(cr);
    } else {
        cairo_arc(cr, cx, y + size * 0.28, 0.8, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_move_to(cr, cx, y + size * 0.44);
        cairo_line_to(cr, cx, y + size * 0.76);
        cairo_stroke(cr);
    }
}

// What a row's callbacks hold: the panel, until it is gone (the window's
// objects go before its widgets).
using Ref = std::shared_ptr<ProblemsPanel*>;

}  // namespace

ProblemsPanel::ProblemsPanel(LspSession* lsp, Host host) : lsp_(lsp), host_(std::move(host)) {
    auto ref = std::make_shared<ProblemsPanel*>(this);
    // Kept with the root widget: the panel sets it null when it goes.
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root_, "minicode-problems");
    g_object_set_data_full(G_OBJECT(root_), "minicode-problems-ref", new Ref(ref),
                           [](gpointer p) { delete static_cast<Ref*>(p); });

    // The heading: PROBLEMS, the counts, the ×.
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(header, "minicode-problems-header");
    GtkWidget* title = gtk_label_new("PROBLEMS");
    gtk_widget_add_css_class(title, "minicode-problems-title");
    gtk_box_append(GTK_BOX(header), title);
    counts_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(counts_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(counts_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(counts_, TRUE);
    gtk_widget_add_css_class(counts_, "minicode-problems-counts");
    gtk_box_append(GTK_BOX(header), counts_);
    GtkWidget* close = gtk_button_new_with_label("\xC3\x97");   // ×
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_focusable(close, FALSE);
    gtk_widget_set_tooltip_text(close, "Close Problems (Ctrl+Shift+M)");
    gtk_widget_add_css_class(close, "minicode-problems-close");
    g_signal_connect_data(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
        if (ProblemsPanel* self = *static_cast<Ref*>(p)->get()) {
            const bool had = self->hasFocus();
            self->hide();
            if (had && self->host_.focusAfterClosing) self->host_.focusAfterClosing();
        }
    }), new Ref(ref), [](gpointer p, GClosure*) { delete static_cast<Ref*>(p); }, GConnectFlags(0));
    gtk_box_append(GTK_BOX(header), close);
    gtk_box_append(GTK_BOX(root_), header);

    // The list: a list view over a model that holds only a count; the rows
    // are drawn from rows_ by position, as the Source Control panel's are.
    model_ = gtk_string_list_new(nullptr);
    selection_ = gtk_single_selection_new(G_LIST_MODEL(model_));
    gtk_single_selection_set_autoselect(selection_, FALSE);
    gtk_single_selection_set_can_unselect(selection_, TRUE);
    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect_data(factory, "setup", G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* obj,
                                                            gpointer p) {
        GtkListItem* item = GTK_LIST_ITEM(obj);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(box, "minicode-problem-row");
        GtkWidget* mark = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(mark), 13);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(mark), 13);
        gtk_widget_set_valign(mark, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), mark);
        GtkWidget* text = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(text), 0);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(text, TRUE);
        gtk_box_append(GTK_BOX(box), text);
        GtkWidget* tail = gtk_label_new("");
        gtk_label_set_ellipsize(GTK_LABEL(tail), PANGO_ELLIPSIZE_START);
        gtk_widget_add_css_class(tail, "minicode-problems-muted");
        gtk_box_append(GTK_BOX(box), tail);
        g_object_set_data(G_OBJECT(box), "minicode-list-item", item);
        // A click on a problem opens it, as Enter does.
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
        g_signal_connect_data(click, "released", G_CALLBACK(+[](GtkGestureClick* g, int n, double,
                                                                 double, gpointer p) {
            ProblemsPanel* self = *static_cast<Ref*>(p)->get();
            if (!self || n != 1) return;
            GtkWidget* row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
            auto* li = static_cast<GtkListItem*>(g_object_get_data(G_OBJECT(row), "minicode-list-item"));
            const guint pos = li ? gtk_list_item_get_position(li) : GTK_INVALID_LIST_POSITION;
            if (pos == GTK_INVALID_LIST_POSITION || !self->isProblem((int)pos)) return;
            self->select((int)pos);
            self->open((int)pos);
        }), new Ref(*static_cast<Ref*>(p)), [](gpointer q, GClosure*) { delete static_cast<Ref*>(q); },
           GConnectFlags(0));
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
        gtk_list_item_set_activatable(item, FALSE);
        gtk_list_item_set_child(item, box);
    }), new Ref(ref), [](gpointer p, GClosure*) { delete static_cast<Ref*>(p); }, GConnectFlags(0));
    g_signal_connect_data(factory, "bind", G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* obj,
                                                           gpointer p) {
        if (ProblemsPanel* self = *static_cast<Ref*>(p)->get()) self->bindRow(GTK_LIST_ITEM(obj));
    }), new Ref(ref), [](gpointer p, GClosure*) { delete static_cast<Ref*>(p); }, GConnectFlags(0));
    g_signal_connect_data(factory, "unbind", G_CALLBACK(+[](GtkSignalListItemFactory*, GObject* obj,
                                                             gpointer p) {
        ProblemsPanel* self = *static_cast<Ref*>(p)->get();
        if (!self) return;
        GtkWidget* box = gtk_list_item_get_child(GTK_LIST_ITEM(obj));
        for (GtkWidget*& w : self->bound_)
            if (w == box) w = nullptr;
    }), new Ref(ref), [](gpointer p, GClosure*) { delete static_cast<Ref*>(p); }, GConnectFlags(0));
    list_ = gtk_list_view_new(GTK_SELECTION_MODEL(selection_), factory);
    gtk_widget_add_css_class(list_, "minicode-problems-list");
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect_data(keys, "key-pressed", G_CALLBACK(onKey), new Ref(ref),
                          [](gpointer p, GClosure*) { delete static_cast<Ref*>(p); }, GConnectFlags(0));
    gtk_widget_add_controller(list_, keys);
    scroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), list_);
    gtk_widget_set_vexpand(scroll_, TRUE);
    gtk_box_append(GTK_BOX(root_), scroll_);
    empty_ = gtk_label_new("No problems have been reported in the open files.");
    gtk_label_set_xalign(GTK_LABEL(empty_), 0);
    gtk_widget_set_valign(empty_, GTK_ALIGN_START);
    gtk_widget_set_vexpand(empty_, TRUE);
    gtk_widget_add_css_class(empty_, "minicode-problems-empty");
    gtk_box_append(GTK_BOX(root_), empty_);
    gtk_widget_set_visible(root_, FALSE);

    // The status bar's counts: a click opens or closes the list.
    status_ = gtk_label_new("");
    gtk_widget_set_cursor_from_name(status_, "pointer");
    gtk_widget_set_tooltip_text(status_, "Problems (Ctrl+Shift+M)");
    gtk_widget_set_visible(status_, FALSE);
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect_data(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double,
                                                             gpointer p) {
        if (ProblemsPanel* self = *static_cast<Ref*>(p)->get()) self->toggle(false);
    }), new Ref(ref), [](gpointer p, GClosure*) { delete static_cast<Ref*>(p); }, GConnectFlags(0));
    gtk_widget_add_controller(status_, GTK_EVENT_CONTROLLER(click));
    g_object_set_data_full(G_OBJECT(status_), "minicode-problems-ref", new Ref(ref),
                           [](gpointer p) { delete static_cast<Ref*>(p); });
    ref_ = ref;
}

ProblemsPanel::~ProblemsPanel() {
    if (ref_) *ref_ = nullptr;   // the widgets' callbacks find no panel from now on
}

bool ProblemsPanel::hasFocus() const {
    GtkRoot* root = gtk_widget_get_root(root_);
    GtkWidget* focus = root ? gtk_root_get_focus(root) : nullptr;
    return shown_ && focus && (focus == root_ || gtk_widget_is_ancestor(focus, root_));
}

void ProblemsPanel::toggle(bool fromKey) {
    if (shown_ && fromKey && !hasFocus()) {
        focusList();
        return;
    }
    if (shown_) {
        const bool had = hasFocus();
        hide();
        if (had && host_.focusAfterClosing) host_.focusAfterClosing();
        return;
    }
    shown_ = true;
    gtk_widget_set_visible(root_, TRUE);
    if (host_.show) host_.show(true);
    update();
    focusList();
}

void ProblemsPanel::hide() {
    if (!shown_) return;
    shown_ = false;
    gtk_widget_set_visible(root_, FALSE);
    if (host_.show) host_.show(false);
}

void ProblemsPanel::focusList() {
    if (!shown_) return;
    if (selectedRow() < 0) moveBy(1);
    else select(selectedRow());
    gtk_widget_grab_focus(list_);
}

void ProblemsPanel::update() {
    updateCounts();
    if (shown_) setRows(lsp_->problemRows(), countsText_);
}

void ProblemsPanel::updateCounts() {
    const bool known = lsp_->problemsKnown();
    gtk_widget_set_visible(status_, known);
    if (!known) {
        statusText_.clear();
        return;
    }
    const std::vector<LspProblem> rows = lsp_->problemRows();
    int errors = 0, warnings = 0;
    for (const LspProblem& r : rows)
        if (r.heading) {
            errors += r.errors;
            warnings += r.warnings;
        }
    countsText_ = lsp_->problemCountsText();
    statusText_ = countsText_;
    std::string markup;
    if (errors || warnings)
        markup = std::string("<span foreground=\"") + (errors ? "#F14C4C" : "#CCA700") +
                 "\">\xE2\x97\x8F</span> ";   // ●
    markup += escape(countsText_);
    gtk_label_set_markup(GTK_LABEL(status_), markup.c_str());
}

// New rows, keeping the selection on the same problem when it is still
// listed (else near where it was).
void ProblemsPanel::setRows(std::vector<LspProblem> rows, const std::string& counts) {
    gtk_label_set_text(GTK_LABEL(counts_), rows.empty() ? "" : counts.c_str());
    // The same rows: nothing to bind again (which would take the keyboard
    // from a row that has it).
    if (rows.size() == rows_.size() && !rows.empty()) {
        bool same = true;
        for (size_t i = 0; i < rows.size() && same; i++) same = sameRow(rows[i], rows_[i]);
        if (same) return;
    }
    const bool hadFocus = hasFocus();
    const int wasRow = selectedRow();
    LspProblem was;
    const bool had = isProblem(wasRow);
    if (had) was = rows_[(size_t)wasRow];
    rows_ = std::move(rows);
    bound_.assign(rows_.size(), nullptr);
    gtk_widget_set_visible(scroll_, !rows_.empty());
    gtk_widget_set_visible(empty_, rows_.empty());
    // The model holds only a count; every row is bound again.
    std::vector<const char*> items(rows_.size(), "");
    items.push_back(nullptr);
    gtk_string_list_splice(model_, 0, g_list_model_get_n_items(G_LIST_MODEL(model_)),
                           items.data());
    int select = -1;
    if (had)
        for (size_t i = 0; i < rows_.size(); i++)
            if (sameProblem(rows_[i], was)) {
                select = (int)i;
                break;
            }
    if (select < 0 && wasRow >= 0) {
        // Gone: the problem now at its place, or the one before.
        for (int r = std::min(wasRow, (int)rows_.size() - 1); r >= 0 && select < 0; r--)
            if (isProblem(r)) select = r;
        for (int r = wasRow; r < (int)rows_.size() && select < 0; r++)
            if (isProblem(r)) select = r;
    }
    if (select >= 0) {
        gtk_single_selection_set_selected(selection_, (guint)select);
        if (hadFocus)
            gtk_list_view_scroll_to(GTK_LIST_VIEW(list_), (guint)select, GTK_LIST_SCROLL_FOCUS,
                                    nullptr);
    } else {
        gtk_single_selection_set_selected(selection_, GTK_INVALID_LIST_POSITION);
    }
    // The rows were made again under the keyboard: it stays in the list.
    if (hadFocus) gtk_widget_grab_focus(rows_.empty() ? root_ : list_);
}

bool ProblemsPanel::isProblem(int row) const {
    return row >= 0 && row < (int)rows_.size() && !rows_[(size_t)row].heading;
}

int ProblemsPanel::selectedRow() const {
    const guint s = gtk_single_selection_get_selected(selection_);
    return s == GTK_INVALID_LIST_POSITION || s >= rows_.size() ? -1 : (int)s;
}

void ProblemsPanel::select(int row) {
    if (!isProblem(row)) return;
    gtk_single_selection_set_selected(selection_, (guint)row);
    // The file's heading in view with it, when it is just above.
    if (row > 0 && !isProblem(row - 1))
        gtk_list_view_scroll_to(GTK_LIST_VIEW(list_), (guint)(row - 1), GTK_LIST_SCROLL_NONE,
                                nullptr);
    gtk_list_view_scroll_to(GTK_LIST_VIEW(list_), (guint)row, GTK_LIST_SCROLL_FOCUS, nullptr);
}

// One problem up or down, headings skipped; false at either end.
bool ProblemsPanel::moveBy(int step) {
    const int n = (int)rows_.size();
    int r = selectedRow();
    if (r < 0) r = step > 0 ? -1 : n;
    for (r += step; r >= 0 && r < n; r += step) {
        if (!isProblem(r)) continue;
        select(r);
        return true;
    }
    return false;
}

void ProblemsPanel::open(int row) {
    if (!isProblem(row)) return;
    const LspProblem p = rows_[(size_t)row];
    lsp_->openProblem(p);
}

std::string ProblemsPanel::folderOf(const std::string& path) const {
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash);
    const std::string root = host_.root ? host_.root() : std::string();
    if (!root.empty() && dir == root) return "";
    if (!root.empty() && dir.rfind(root + "/", 0) == 0) return dir.substr(root.size() + 1);
    const char* home = g_get_home_dir();
    if (home && *home && dir.rfind(std::string(home) + "/", 0) == 0)
        return "~" + dir.substr(strlen(home));
    return dir;
}

void ProblemsPanel::bindRow(GtkListItem* item) {
    GtkWidget* box = gtk_list_item_get_child(item);
    const guint pos = gtk_list_item_get_position(item);
    if (!box || pos >= rows_.size()) return;
    const LspProblem& p = rows_[pos];
    GtkWidget* mark = gtk_widget_get_first_child(box);
    GtkWidget* text = gtk_widget_get_next_sibling(mark);
    GtkWidget* tail = gtk_widget_get_next_sibling(text);
    gtk_list_item_set_selectable(item, !p.heading);
    gtk_list_item_set_focusable(item, !p.heading);
    if (p.heading) {
        gtk_widget_set_visible(mark, FALSE);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_remove_css_class(box, "problem");
        gtk_widget_add_css_class(box, "heading");
        std::string markup = "<b>" + escape(baseName(p.path)) + "</b>";
        const std::string folder = folderOf(p.path);
        if (!folder.empty())
            markup += "  <span alpha=\"60%\">" + escape(folder) + "</span>";
        gtk_label_set_markup(GTK_LABEL(text), markup.c_str());
        gtk_label_set_text(GTK_LABEL(tail), headingCounts(p).c_str());
        gtk_widget_set_tooltip_text(box, p.path.c_str());
    } else {
        gtk_widget_set_visible(mark, TRUE);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mark), drawSeverity,
                                       GINT_TO_POINTER(p.severity), nullptr);
        gtk_widget_set_margin_start(box, 26);
        gtk_widget_remove_css_class(box, "heading");
        gtk_widget_add_css_class(box, "problem");
        gtk_label_set_text(GTK_LABEL(text), p.message.c_str());
        const std::string source = sourceText(p);
        const std::string where = "[Ln " + std::to_string(p.line + 1) + ", Col " +
                                  std::to_string(p.column + 1) + "]";
        gtk_label_set_text(GTK_LABEL(tail), (source.empty() ? where : source + "  " + where).c_str());
        const std::string tip = std::string(severityName(p.severity)) + ": " + p.fullMessage +
                                (source.empty() ? "" : " (" + source + ")") + "\n" +
                                baseName(p.path) + ":" + std::to_string(p.line + 1) + ":" +
                                std::to_string(p.column + 1);
        gtk_widget_set_tooltip_text(box, tip.c_str());
        gtk_widget_queue_draw(mark);
    }
    if (pos < bound_.size()) bound_[pos] = box;
}

gboolean ProblemsPanel::onKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state,
                              gpointer p) {
    ProblemsPanel* self = *static_cast<Ref*>(p)->get();
    if (!self) return FALSE;
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
    switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up:
        self->moveBy(-1);
        return TRUE;
    case GDK_KEY_Down: case GDK_KEY_KP_Down:
        self->moveBy(1);
        return TRUE;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up:
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: {
        const int step = keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_KP_Page_Up ? -1 : 1;
        const int rows = std::max(1, gtk_widget_get_height(self->scroll_) / 24 - 1);
        for (int i = 0; i < rows; i++)
            if (!self->moveBy(step)) break;
        return TRUE;
    }
    case GDK_KEY_Home: case GDK_KEY_KP_Home:
    case GDK_KEY_End: case GDK_KEY_KP_End: {
        const bool home = keyval == GDK_KEY_Home || keyval == GDK_KEY_KP_Home;
        gtk_single_selection_set_selected(self->selection_, GTK_INVALID_LIST_POSITION);
        self->moveBy(home ? 1 : -1);
        return TRUE;
    }
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter:
        self->open(self->selectedRow());
        return TRUE;
    case GDK_KEY_Escape:
        if (self->host_.focusEditor) self->host_.focusEditor();
        return TRUE;
    default:
        return FALSE;
    }
}

std::vector<std::string> ProblemsPanel::rowTexts() const {
    std::vector<std::string> out;
    for (const LspProblem& p : rows_) {
        if (p.heading)
            out.push_back("# " + baseName(p.path) + " \xE2\x80\x94 " + headingCounts(p));
        else
            out.push_back(std::string(severityLetter(p.severity)) + " " +
                          std::to_string(p.line + 1) + ":" + std::to_string(p.column + 1) + " " +
                          p.message + " [" + sourceText(p) + "]");
    }
    return out;
}

std::string ProblemsPanel::countsText() const { return gtk_label_get_text(GTK_LABEL(counts_)); }
std::string ProblemsPanel::statusText() const { return statusText_; }

GtkWidget* ProblemsPanel::rowWidget(int row) const {
    return row >= 0 && row < (int)bound_.size() ? bound_[(size_t)row] : nullptr;
}
