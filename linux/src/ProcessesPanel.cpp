// ProcessesPanel.cpp — see ProcessesPanel.h.
#include "ProcessesPanel.h"

#include "Appearance.h"
#include "ProcessMonitor.h"

namespace {

std::string trimmedName(const std::string& s) {
    size_t i = s.find_first_not_of(' ');
    return i == std::string::npos ? s : s.substr(i);
}

void flatten(const Procs::Node& n, int depth, const Procs::Group& g,
             std::vector<ProcessesPanel::RowInfo>& rows, std::vector<Procs::Key>& keys) {
    const Procs::Proc& p = n.proc;
    ProcessesPanel::RowInfo r;
    r.text = std::string(depth * 2, ' ') + Procs::displayName(p);
    r.pid = p.key.pid;
    r.alive = p.alive;
    r.orphan = g.kind == Procs::GroupKind::Orphans;
    r.outside = g.kind == Procs::GroupKind::Outside;
    r.terminal = p.terminal;
    r.ports = p.alive ? Procs::formatPorts(p.ports) : std::string();
    r.memory = p.alive ? Procs::formatBytes(p.memory) : std::string("exited");
    r.cpu = p.alive ? Procs::formatCpu(p.cpu) : std::string();
    std::string cmd = p.command.empty() ? p.name : p.command;
    r.detail = cmd + (p.cwd.empty() ? "" : "  " + p.cwd);
    r.tooltip = "process " + std::to_string(p.key.pid) + "\n" + cmd +
                (p.cwd.empty() ? "" : "\n" + p.cwd);
    rows.push_back(r);
    keys.push_back(p.key);
    for (const Procs::Node& c : n.children) flatten(c, depth + 1, g, rows, keys);
}

GtkWidget* makeLabel(const std::string& text, const char* css, float xalign = 0) {
    GtkWidget* l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    if (css) gtk_widget_add_css_class(l, css);
    return l;
}

}  // namespace

ProcessesPanel::ProcessesPanel() {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root_, "minicode-procs");
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);
    GtkWidget* title = makeLabel("PROCESSES", "minicode-procs-heading");
    gtk_widget_set_margin_start(title, 10);
    gtk_widget_set_margin_top(title, 8);
    gtk_box_append(GTK_BOX(root_), title);
    summary_ = makeLabel("", "minicode-procs-info");
    gtk_widget_set_margin_start(summary_, 10);
    gtk_widget_set_margin_bottom(summary_, 6);
    gtk_box_append(GTK_BOX(root_), summary_);
    list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(list_, "minicode-procs-list");
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root_), scroll);

    actions_ = g_simple_action_group_new();
    GSimpleAction* stop = g_simple_action_new("stop", nullptr);
    g_signal_connect(stop, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer p) {
        auto* self = static_cast<ProcessesPanel*>(p);
        self->stopPid(self->menuPid_);
    }), this);
    g_action_map_add_action(G_ACTION_MAP(actions_), G_ACTION(stop));
    g_object_unref(stop);
    gtk_widget_insert_action_group(root_, "procs", G_ACTION_GROUP(actions_));

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(root_, GTK_EVENT_CONTROLLER(click));
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKey), this);
    gtk_widget_add_controller(list_, keys);
    rebuild();
}

ProcessesPanel::~ProcessesPanel() {
    if (timer_) g_source_remove(timer_);
    dropMenu();
    if (actions_) g_object_unref(actions_);
}

void ProcessesPanel::setActive(bool active) {
    active_ = active;
    ProcessMonitor::shared().setPanelShown(active);
    if (active && !timer_) timer_ = g_timeout_add(500, &ProcessesPanel::tick, this);
    if (!active && timer_) {
        g_source_remove(timer_);
        timer_ = 0;
    }
    if (active) {
        refresh();
        ProcessMonitor::shared().sampleNow([this] {
            if (active_) refresh();
        });
    }
}

// Polls for a look that arrived since (the monitor's own timer takes them).
int ProcessesPanel::tick(void* p) {
    auto* self = static_cast<ProcessesPanel*>(p);
    if (ProcessMonitor::shared().samples() != self->seen_) self->refresh();
    return G_SOURCE_CONTINUE;
}

void ProcessesPanel::focus() { gtk_widget_grab_focus(list_); }

void ProcessesPanel::refresh() {
    seen_ = ProcessMonitor::shared().samples();
    rebuild();
}

void ProcessesPanel::rebuild() {
    int selectedPid = 0;
    if (GtkListBoxRow* sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_))) {
        int i = gtk_list_box_row_get_index(sel);
        if (i >= 0 && i < (int)rows_.size()) selectedPid = rows_[i].pid;
    }
    rows_.clear();
    keys_.clear();
    Procs::View v = ProcessMonitor::shared().view(windowId ? windowId() : 0,
                                                  folder ? folder() : std::string());
    for (const Procs::Group& g : v.groups) {
        RowInfo h;
        h.header = true;
        h.text = g.title;
        h.memory = Procs::summary(g.memory, g.count);
        h.terminal = g.terminal;
        h.orphan = g.kind == Procs::GroupKind::Orphans;
        h.outside = g.kind == Procs::GroupKind::Outside;
        rows_.push_back(h);
        keys_.push_back(Procs::Key{});
        for (const Procs::Node& n : g.roots) flatten(n, 0, g, rows_, keys_);
    }
    gtk_label_set_text(GTK_LABEL(summary_), Procs::summary(v.memory, v.count).c_str());

    while (GtkWidget* c = gtk_widget_get_first_child(list_))
        gtk_list_box_remove(GTK_LIST_BOX(list_), c);
    GtkListBoxRow* reselect = nullptr;
    for (const RowInfo& r : rows_) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 8);
        if (r.header) {
            GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget* t = makeLabel(r.text, "minicode-procs-group");
            gtk_widget_set_hexpand(t, TRUE);
            gtk_box_append(GTK_BOX(line), t);
            gtk_box_append(GTK_BOX(line), makeLabel(r.memory, "minicode-procs-info", 1));
            gtk_widget_set_margin_top(line, 6);
            gtk_box_append(GTK_BOX(box), line);
        } else {
            GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget* n = makeLabel(r.text, r.alive ? nullptr : "minicode-procs-gone");
            gtk_widget_set_hexpand(n, TRUE);
            gtk_box_append(GTK_BOX(line), n);
            if (!r.ports.empty())
                gtk_box_append(GTK_BOX(line), makeLabel(":" + r.ports, "minicode-procs-port"));
            gtk_box_append(GTK_BOX(line), makeLabel(r.memory, "minicode-procs-info", 1));
            gtk_box_append(GTK_BOX(line), makeLabel(r.cpu, "minicode-procs-info", 1));
            gtk_box_append(GTK_BOX(box), line);
            gtk_box_append(GTK_BOX(box), makeLabel(r.detail, "minicode-procs-info"));
            gtk_widget_set_tooltip_text(box, r.tooltip.c_str());
        }
        GtkWidget* row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), !r.header);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_append(GTK_LIST_BOX(list_), row);
        if (!r.header && r.pid == selectedPid) reselect = GTK_LIST_BOX_ROW(row);
    }
    if (reselect) gtk_list_box_select_row(GTK_LIST_BOX(list_), reselect);
}

void ProcessesPanel::popup(GMenu* menu, double x, double y) {
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

void ProcessesPanel::dropMenu() {
    if (!menu_) return;
    GtkWidget* m = menu_;
    menu_ = nullptr;
    gtk_popover_popdown(GTK_POPOVER(m));
    if (gtk_widget_get_parent(m)) gtk_widget_unparent(m);
}

void ProcessesPanel::openMenuFor(int pid) {
    GMenu* menu = g_menu_new();
    const RowInfo* row = nullptr;
    for (const RowInfo& r : rows_)
        if (!r.header && r.pid == pid) row = &r;
    menuPid_ = row ? pid : 0;
    menuLabels_.clear();
    if (row) {
        GMenu* sec = g_menu_new();
        std::string label = row->orphan ? "Stop" : "Stop…";
        g_menu_append(sec, label.c_str(), row->alive ? "procs.stop" : "procs.none");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(sec));
        g_object_unref(sec);
        menuLabels_ = label;
    }
    popup(menu, 12, 40);
    g_object_unref(menu);
}

void ProcessesPanel::onPressed(GtkGestureClick*, int, double x, double y, gpointer p) {
    auto* self = static_cast<ProcessesPanel*>(p);
    graphene_point_t in = {(float)x, (float)y}, out;
    if (gtk_widget_compute_point(self->root_, self->list_, &in, &out))
        if (GtkListBoxRow* row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(self->list_), (int)out.y)) {
            int i = gtk_list_box_row_get_index(row);
            if (i >= 0 && i < (int)self->rows_.size() && !self->rows_[i].header) {
                gtk_list_box_select_row(GTK_LIST_BOX(self->list_), row);
                self->openMenuFor(self->rows_[i].pid);
                return;
            }
        }
    GMenu* menu = g_menu_new();   // Adjust Settings… alone
    self->popup(menu, x, y);
    g_object_unref(menu);
}

gboolean ProcessesPanel::onKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType,
                               gpointer p) {
    auto* self = static_cast<ProcessesPanel*>(p);
    if (keyval != GDK_KEY_Delete && keyval != GDK_KEY_KP_Delete) return FALSE;
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->list_));
    int i = sel ? gtk_list_box_row_get_index(sel) : -1;
    if (i < 0 || i >= (int)self->rows_.size()) {
        gtk_widget_error_bell(self->root_);
        return TRUE;
    }
    self->stopPid(self->rows_[i].pid);
    return TRUE;
}

// Stop, asking first unless the process is an orphan (the Mac's rule).
void ProcessesPanel::stopPid(int pid) {
    int at = -1;
    for (size_t i = 0; i < rows_.size(); i++)
        if (!rows_[i].header && rows_[i].pid == pid) at = (int)i;
    if (at < 0 || !rows_[at].alive) {
        gtk_widget_error_bell(root_);
        return;
    }
    const RowInfo r = rows_[at];
    const Procs::Key key = keys_[at];
    const std::string name = trimmedName(r.text);
    auto go = [this, r, key, name] {
        ProcessMonitor::shared().stop(key, [this, r, name](const std::string& refusal) {
            if (onNote)
                onNote(refusal.empty() ? "Stopping " + name + " (process " +
                                             std::to_string(r.pid) + ")"
                                       : name + " was not stopped: " + refusal);
        });
    };
    if (r.orphan || !hostWindow) {
        go();
        return;
    }
    std::string where = r.outside ? "was not started in minicode"
                                  : "was started in terminal " + std::to_string(r.terminal);
    GtkAlertDialog* dlg = gtk_alert_dialog_new("Stop %s?", name.c_str());
    std::string detail = "“" + r.detail + "” (process " + std::to_string(r.pid) + ") " +
                         where + ". It is sent SIGTERM, and SIGKILL 3 seconds later if it is "
                         "still running; what it started stops with it.";
    gtk_alert_dialog_set_detail(dlg, detail.c_str());
    const char* buttons[] = {"Stop", "Cancel", nullptr};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_default_button(dlg, 1);
    auto* ctx = new std::function<void()>(go);
    gtk_alert_dialog_choose(dlg, hostWindow, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer d) {
            auto* f = static_cast<std::function<void()>*>(d);
            GError* err = nullptr;
            int b = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
            if (err) g_error_free(err);
            else if (b == 0) (*f)();
            delete f;
        }, ctx);
    g_object_unref(dlg);
}
