// AgentsPanel.cpp — see AgentsPanel.h.
#include "AgentsPanel.h"

#include <ctime>

#include "Appearance.h"
#include "AgentView.h"
#include "FileDetails.h"

namespace {

GtkWidget* makeLabel(const std::string& text, const char* css, float xalign = 0) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    if (css) gtk_widget_add_css_class(l, css);
    return l;
}

// The tree's compact age, a short date past yesterday.
std::string ageText(double then, double now) {
    if (then <= 0) return "";
    GDateTime* a = g_date_time_new_from_unix_local((gint64)then);
    GDateTime* b = g_date_time_new_from_unix_local((gint64)now);
    GDate da, db;
    g_date_set_dmy(&da, (GDateDay)g_date_time_get_day_of_month(a),
                   (GDateMonth)g_date_time_get_month(a), (GDateYear)g_date_time_get_year(a));
    g_date_set_dmy(&db, (GDateDay)g_date_time_get_day_of_month(b),
                   (GDateMonth)g_date_time_get_month(b), (GDateYear)g_date_time_get_year(b));
    int days = (int)g_date_days_between(&da, &db);
    std::string s = Age::compact(now, then, days);
    if (s.empty()) {
        char* f = g_date_time_format(a, "%e %b");
        s = f ? f : "";
        g_free(f);
        s.erase(0, s.find_first_not_of(' '));
    }
    g_date_time_unref(a);
    g_date_time_unref(b);
    return s;
}

}  // namespace

AgentsPanel::AgentsPanel(const std::string& folder) : model_(folder) {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root_, "minicode-procs");
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* title = makeLabel("AGENTS", "minicode-procs-heading");
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_NONE);
    gtk_widget_set_margin_start(title, 10);
    gtk_widget_set_margin_top(title, 8);
    gtk_box_append(GTK_BOX(head), title);
    summary_ = makeLabel("", "minicode-procs-info");
    gtk_widget_set_margin_top(summary_, 8);
    gtk_box_append(GTK_BOX(head), summary_);
    gtk_widget_set_margin_bottom(head, 6);
    gtk_box_append(GTK_BOX(root_), head);
    notice_ = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(notice_), TRUE);
    gtk_label_set_xalign(GTK_LABEL(notice_), 0);
    gtk_widget_add_css_class(notice_, "minicode-procs-info");
    gtk_widget_set_margin_start(notice_, 10);
    gtk_widget_set_margin_end(notice_, 10);
    gtk_box_append(GTK_BOX(root_), notice_);
    list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list_), TRUE);
    gtk_widget_add_css_class(list_, "minicode-procs-list");
    g_signal_connect(list_, "row-activated", G_CALLBACK(onActivated), this);
    scroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll_), list_);
    gtk_widget_set_vexpand(scroll_, TRUE);
    gtk_box_append(GTK_BOX(root_), scroll_);

    actions_ = g_simple_action_group_new();
    auto add = [this](const char* name, void (*fn)(GSimpleAction*, GVariant*, gpointer)) {
        GSimpleAction* a = g_simple_action_new(name, nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(fn), this);
        g_action_map_add_action(G_ACTION_MAP(actions_), G_ACTION(a));
        g_object_unref(a);
    };
    add("open", +[](GSimpleAction*, GVariant*, gpointer p) {
        auto* s = static_cast<AgentsPanel*>(p);
        s->activateRow(s->menuRow_);
    });
    add("evidence", +[](GSimpleAction*, GVariant*, gpointer p) {
        auto* s = static_cast<AgentsPanel*>(p);
        if (s->menuRow_ >= 0 && s->menuRow_ < (int)s->rows_.size()) s->showEvidence(s->rows_[s->menuRow_]);
    });
    add("copy", +[](GSimpleAction*, GVariant*, gpointer p) {
        auto* s = static_cast<AgentsPanel*>(p);
        if (s->menuPath_.empty()) return;
        gdk_clipboard_set_text(gtk_widget_get_clipboard(s->root_), s->menuPath_.c_str());
    });
    gtk_widget_insert_action_group(root_, "agents", G_ACTION_GROUP(actions_));

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(root_, GTK_EVENT_CONTROLLER(click));

    model_.onChange = [this] {
        rebuild();
        if (onModelChange) onModelChange();
    };
    rebuild();
}

AgentsPanel::~AgentsPanel() {
    model_.onChange = nullptr;
    if (timer_) g_source_remove(timer_);
    dropMenu();
    if (actions_) g_object_unref(actions_);
}

void AgentsPanel::setActive(bool active) {
    active_ = active;
    model_.setPanelShown(active);
    // Ages and "running" move with the clock.
    if (active && !timer_) timer_ = g_timeout_add_seconds(30, &AgentsPanel::tick, this);
    if (!active && timer_) {
        g_source_remove(timer_);
        timer_ = 0;
    }
    if (active) rebuild();
}

int AgentsPanel::tick(void* p) {
    static_cast<AgentsPanel*>(p)->rebuild();
    return G_SOURCE_CONTINUE;
}

void AgentsPanel::focus() {
    if (!gtk_list_box_get_selected_row(GTK_LIST_BOX(list_)))
        if (GtkListBoxRow* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), 0))
            gtk_list_box_select_row(GTK_LIST_BOX(list_), r);
    gtk_widget_grab_focus(list_);
}

GtkWidget* AgentsPanel::rowWidget(int row) const {
    return GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), row));
}

std::string AgentsPanel::summary() const { return gtk_label_get_text(GTK_LABEL(summary_)); }
std::string AgentsPanel::notice() const {
    return gtk_widget_get_visible(notice_) ? gtk_label_get_text(GTK_LABEL(notice_)) : "";
}

void AgentsPanel::rebuild() {
    using namespace AgentModelData;
    std::string selected;
    if (GtkListBoxRow* sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_))) {
        int i = gtk_list_box_row_get_index(sel);
        if (i >= 0 && i < (int)rows_.size()) selected = rows_[i].key;
    }
    auto snap = model_.snapshot();
    const double now = (double)time(nullptr);
    rows_.clear();
    struct Walk {
        AgentsPanel* self;
        double now;
        void session(const Work& s, int depth) {
            RowInfo r;
            r.kind = RowInfo::Session;
            r.depth = depth;
            r.key = s.transcript;
            r.text = s.name;
            r.running = s.runningAt(now);
            r.detail = AgentView::detailLine(r.running,
                                             ageText(std::max(s.lastActivity, s.written), now),
                                             s.whereText(), s.filesTouched, s.bashCommands);
            r.tooltip = s.name;
            if (!s.cwd.empty()) r.tooltip += "\n" + s.cwd;
            if (!s.worktreePath.empty()) r.tooltip += "\nWorktree: " + s.worktreePath;
            if (s.badLines) r.tooltip += "\n" + std::to_string(s.badLines) + " unreadable lines skipped";
            bool open = self->expanded_.count(r.key) > 0;
            r.expanded = open;
            self->rows_.push_back(r);
            if (!open) return;
            for (const Group& g : s.groups) {
                RowInfo gr;
                gr.kind = RowInfo::Group;
                gr.depth = depth + 1;
                gr.key = s.transcript + "|" + g.title;
                gr.text = g.title;
                gr.expanded = self->expanded_.count(gr.key) > 0;
                self->rows_.push_back(gr);
                if (!gr.expanded) continue;
                for (const File& f : g.files) {
                    RowInfo fr;
                    fr.kind = RowInfo::File;
                    fr.depth = depth + 2;
                    fr.key = gr.key + "|" + f.path;
                    fr.path = f.path;
                    size_t cut = f.path.find_last_of('/');
                    fr.text = cut == std::string::npos ? f.path : f.path.substr(cut + 1);
                    fr.detail = f.folderText;
                    fr.exists = f.exists;
                    fr.tooltip = f.path + "\n" + std::to_string(f.writes) +
                                 (f.writes == 1 ? " write, " : " writes, ") +
                                 std::to_string(f.edits) + (f.edits == 1 ? " edit" : " edits") +
                                 (f.failed ? ", " + std::to_string(f.failed) + " refused" : "") +
                                 (f.exists ? "" : "\nNo longer on disk");
                    self->rows_.push_back(fr);
                }
            }
            for (const Work& sub : s.subagents) session(sub, depth + 1);
        }
    } walk{this, now};
    for (const Work& s : snap->sessions) walk.session(s, 0);
    // Subagents count in "running" even when their parent is closed.
    size_t total = snap->sessionCount;
    std::string sum;
    if (total) {
        // running counts only shown rows above; count all here.
        size_t r2 = 0;
        std::vector<const Work*> stack;
        for (const Work& s : snap->sessions) stack.push_back(&s);
        while (!stack.empty()) {
            const Work* w = stack.back();
            stack.pop_back();
            if (w->runningAt(now)) r2++;
            for (const Work& c : w->subagents) stack.push_back(&c);
        }
        sum = (r2 ? std::to_string(r2) + " running · " : std::string()) + std::to_string(total) +
              (total == 1 ? " session" : " sessions");
    }
    gtk_label_set_text(GTK_LABEL(summary_), sum.c_str());
    std::string home = g_get_home_dir() ? g_get_home_dir() : "";
    std::string dir = snap->projectsDir;
    if (!home.empty() && dir.compare(0, home.size(), home) == 0) dir = "~" + dir.substr(home.size());
    gtk_label_set_text(GTK_LABEL(notice_),
                       AgentView::emptyText(snap->projectsMissing, snap->loaded, dir).c_str());
    gtk_widget_set_visible(notice_, snap->sessions.empty());
    gtk_widget_set_visible(scroll_, !snap->sessions.empty());

    while (GtkWidget* c = gtk_widget_get_first_child(list_))
        gtk_list_box_remove(GTK_LIST_BOX(list_), c);
    GtkListBoxRow* reselect = nullptr;
    for (const RowInfo& r : rows_) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_margin_start(box, 6 + 14 * r.depth);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 2);
        gtk_widget_set_margin_bottom(box, 2);
        const char* mark = r.kind == RowInfo::File ? "" : (r.expanded ? "▾ " : "▸ ");
        if (r.kind == RowInfo::Session) {
            GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget* t = makeLabel(std::string(mark) + r.text, nullptr);
            gtk_widget_set_hexpand(t, TRUE);
            gtk_box_append(GTK_BOX(line), t);
            gtk_box_append(GTK_BOX(box), line);
            gtk_box_append(GTK_BOX(box), makeLabel(r.detail, r.running ? "minicode-procs-port"
                                                                       : "minicode-procs-info"));
        } else if (r.kind == RowInfo::Group) {
            gtk_box_append(GTK_BOX(box),
                           makeLabel(std::string(mark) + r.text, "minicode-procs-group"));
        } else {
            GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget* n = makeLabel(r.text, r.exists ? nullptr : "minicode-procs-gone");
            if (!r.exists) {
                char* m = g_markup_escape_text(r.text.c_str(), -1);
                std::string mk = std::string("<s>") + m + "</s>";
                g_free(m);
                gtk_label_set_markup(GTK_LABEL(n), mk.c_str());
            }
            gtk_box_append(GTK_BOX(line), n);
            if (!r.detail.empty()) gtk_box_append(GTK_BOX(line), makeLabel(r.detail, "minicode-procs-info"));
            gtk_box_append(GTK_BOX(box), line);
        }
        if (!r.tooltip.empty()) gtk_widget_set_tooltip_text(box, r.tooltip.c_str());
        GtkWidget* row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(list_), row);
        if (r.key == selected) reselect = GTK_LIST_BOX_ROW(row);
    }
    if (reselect) gtk_list_box_select_row(GTK_LIST_BOX(list_), reselect);
}

void AgentsPanel::activateRow(int i) {
    if (i < 0 || i >= (int)rows_.size()) return;
    const RowInfo r = rows_[i];
    if (r.kind == RowInfo::File) {
        if (!r.exists) {
            gtk_widget_error_bell(root_);
            return;
        }
        if (onOpenFile) onOpenFile(r.path);
        return;
    }
    if (r.kind == RowInfo::Session) showEvidence(r);
    if (!expanded_.erase(r.key)) expanded_.insert(r.key);
    rebuild();
}

// A click on a session shows what it did (the Mac's Show Evidence).
void AgentsPanel::showEvidence(const RowInfo& r) {
    if (r.kind == RowInfo::Session && !r.key.empty() && onShowEvidence) onShowEvidence(r.key, r.text);
}

void AgentsPanel::onActivated(GtkListBox*, GtkListBoxRow* row, gpointer p) {
    auto* self = static_cast<AgentsPanel*>(p);
    self->activateRow(gtk_list_box_row_get_index(row));
}

void AgentsPanel::popup(GMenu* menu, double x, double y) {
    dropMenu();
    Appearance::addItem(menu, "tree");
    menu_ = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(menu_), FALSE);
    gtk_widget_set_halign(menu_, GTK_ALIGN_START);
    gtk_widget_set_parent(menu_, root_);
    GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(menu_), &at);
    gtk_popover_popup(GTK_POPOVER(menu_));
}

void AgentsPanel::dropMenu() {
    if (!menu_) return;
    GtkWidget* m = menu_;
    menu_ = nullptr;
    gtk_popover_popdown(GTK_POPOVER(m));
    if (gtk_widget_get_parent(m)) gtk_widget_unparent(m);
}

// A file: Open, Copy Path; a session: Copy Transcript Path. Nothing here
// writes, moves or deletes Claude Code's files.
void AgentsPanel::openMenuFor(int row) {
    GMenu* menu = g_menu_new();
    menuRow_ = row;
    menuPath_.clear();
    menuLabels_.clear();
    if (row >= 0 && row < (int)rows_.size()) {
        const RowInfo& r = rows_[row];
        GMenu* sec = g_menu_new();
        if (r.kind == RowInfo::File) {
            g_menu_append(sec, "Open", r.exists ? "agents.open" : "agents.none");
            g_menu_append(sec, "Copy Path", "agents.copy");
            menuPath_ = r.path;
            menuLabels_ = "Open|Copy Path";
        } else if (r.kind == RowInfo::Session) {
            g_menu_append(sec, "Show Evidence", "agents.evidence");
            g_menu_append(sec, "Copy Transcript Path", "agents.copy");
            menuPath_ = r.key;
            menuLabels_ = "Show Evidence|Copy Transcript Path";
        }
        if (g_menu_model_get_n_items(G_MENU_MODEL(sec)))
            g_menu_append_section(menu, nullptr, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }
    popup(menu, 12, 40);
    g_object_unref(menu);
}

void AgentsPanel::onPressed(GtkGestureClick*, int, double x, double y, gpointer p) {
    auto* self = static_cast<AgentsPanel*>(p);
    graphene_point_t in = {(float)x, (float)y}, out;
    if (gtk_widget_get_visible(self->scroll_) &&
        gtk_widget_compute_point(self->root_, self->list_, &in, &out))
        if (GtkListBoxRow* row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->list_), (int)out.y)) {
            gtk_list_box_select_row(GTK_LIST_BOX(self->list_), row);
            self->openMenuFor(gtk_list_box_row_get_index(row));
            return;
        }
    GMenu* menu = g_menu_new();   // Adjust Settings… alone
    self->menuLabels_.clear();
    self->popup(menu, x, y);
    g_object_unref(menu);
}
