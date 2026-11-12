// TabStrip.cpp — see TabStrip.h.
#include "TabStrip.h"

#include <cmath>

#include "EditorTabs.h"

namespace {
// The close button's two faces.
const char* kCloseMark = "×";   // ×
const char* kDirtyMark = "●";   // ●

// A point of `from` in `to`'s coordinates (the point as it was when not mapped).
double rowX(GtkWidget* from, GtkWidget* to, double x, double y) {
    graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y), out;
    if (!gtk_widget_compute_point(from, to, &in, &out)) return x;
    return out.x;
}

bool within(GtkWidget* w, GtkWidget* area) {
    for (; w; w = gtk_widget_get_parent(w))
        if (w == area) return true;
    return false;
}
}  // namespace

TabStrip::TabStrip() {
    root_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(root_, "minicode-tabs");
    gtk_widget_set_hexpand(root_, TRUE);
    scroller_ = gtk_scrolled_window_new();
    // No scroll bar of its own: the row scrolls with the wheel, and keeps the
    // active tab in sight. Its height is its natural one, with no minimum
    // (EXTERNAL both ways): the window's vertical split measures the editor's
    // side at small heights, and a minimum here added to the LaTeX preview's
    // made GTK warn (HANDOFF.md, "Trying to measure GtkBox").
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_EXTERNAL,
                                   GTK_POLICY_EXTERNAL);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller_), TRUE);
    gtk_widget_set_hexpand(scroller_, TRUE);
    row_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(row_, GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), row_);
    gtk_box_append(GTK_BOX(root_), scroller_);
}

TabStrip::~TabStrip() {
    alive_.reset();   // callbacks waiting in an idle find the strip gone
    if (sightIdle_) g_source_remove(sightIdle_);
    sightIdle_ = 0;
    dropMenu();
    for (const Tab& t : tabs_) {
        GListModel* ctrls = gtk_widget_observe_controllers(t.box);
        for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
            GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
            g_signal_handlers_disconnect_by_data(c, this);
            g_object_unref(c);
        }
        g_object_unref(ctrls);
    }
}

TabStrip::Tab TabStrip::makeTab() {
    Tab t;
    t.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(t.box, "minicode-tab");
    t.title = gtk_label_new("");
    gtk_widget_add_css_class(t.title, "minicode-tab-title");
    gtk_label_set_ellipsize(GTK_LABEL(t.title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(t.title), 30);
    gtk_label_set_single_line_mode(GTK_LABEL(t.title), TRUE);
    t.close = gtk_label_new(kCloseMark);
    gtk_widget_add_css_class(t.close, "minicode-tab-close");
    gtk_box_append(GTK_BOX(t.box), t.title);
    gtk_box_append(GTK_BOX(t.box), t.close);
    // Every button: the first selects, closes from the close button and
    // keeps on a double-click; the middle closes; the right opens the menu.
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    g_signal_connect(click, "released", G_CALLBACK(onReleased), this);
    gtk_widget_add_controller(t.box, GTK_EVENT_CONTROLLER(click));
    return t;
}

void TabStrip::setItems(const std::vector<Item>& items, int active) {
    while (tabs_.size() < items.size()) {
        tabs_.push_back(makeTab());
        gtk_box_append(GTK_BOX(row_), tabs_.back().box);
    }
    while (tabs_.size() > items.size()) {
        GtkWidget* box = tabs_.back().box;
        tabs_.pop_back();
        GListModel* ctrls = gtk_widget_observe_controllers(box);
        for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
            GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
            g_signal_handlers_disconnect_by_data(c, this);
            g_object_unref(c);
        }
        g_object_unref(ctrls);
        if (menu_ && within(gtk_widget_get_parent(menu_), box)) dropMenu();
        gtk_box_remove(GTK_BOX(row_), box);
    }
    items_ = items;
    active_ = active >= 0 && active < (int)items.size() ? active : -1;
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i];
        const Tab& t = tabs_[i];
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(t.title)), it.title.c_str()) != 0)
            gtk_label_set_text(GTK_LABEL(t.title), it.title.c_str());
        gtk_label_set_text(GTK_LABEL(t.close), it.dirty ? kDirtyMark : kCloseMark);
        gtk_widget_set_tooltip_text(t.box, it.tooltip.empty() ? nullptr : it.tooltip.c_str());
        auto mark = [&](const char* cls, bool on) {
            if (on) gtk_widget_add_css_class(t.box, cls);
            else gtk_widget_remove_css_class(t.box, cls);
        };
        mark("active", (int)i == active_);
        mark("preview", it.preview);
        mark("dirty", it.dirty);
    }
    keepActiveInSight();
}

GtkWidget* TabStrip::tabWidget(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i].box : nullptr;
}

GtkWidget* TabStrip::titleWidget(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i].title : nullptr;
}

GtkWidget* TabStrip::closeWidget(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i].close : nullptr;
}

int TabStrip::indexOf(GtkWidget* box) const {
    for (size_t i = 0; i < tabs_.size(); i++)
        if (tabs_[i].box == box) return (int)i;
    return -1;
}

// A callback once the event is over: it may rebuild the strip, which would
// otherwise take away the widget whose gesture is still emitting.
void TabStrip::later(std::function<void(int)> cb, int index) {
    if (!cb) return;
    struct Job {
        std::weak_ptr<int> alive;
        std::function<void(int)> cb;
        int index;
    };
    auto* job = new Job{alive_, std::move(cb), index};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer d) -> gboolean {
        Job* j = static_cast<Job*>(d);
        if (!j->alive.expired()) j->cb(j->index);
        return G_SOURCE_REMOVE;
    }, job, [](gpointer d) { delete static_cast<Job*>(d); });
}

void TabStrip::onPressed(GtkGestureClick* g, int n, double x, double y, gpointer selfp) {
    TabStrip* self = static_cast<TabStrip*>(selfp);
    GtkWidget* box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    const int i = self->indexOf(box);
    if (i < 0) return;
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    self->pressClose_ = self->pressMiddle_ = -1;
    if (button == GDK_BUTTON_SECONDARY) {   // the menu, without selecting the tab
        self->showMenu(i, x, y);
        return;
    }
    if (button == GDK_BUTTON_MIDDLE) {
        self->pressMiddle_ = i;
        return;
    }
    if (button != GDK_BUTTON_PRIMARY) return;
    GtkWidget* hit = gtk_widget_pick(box, x, y, GTK_PICK_DEFAULT);
    if (within(hit, self->tabs_[(size_t)i].close)) {
        self->pressClose_ = i;   // closes on release, over the button
        return;
    }
    if (self->onMove) {
        self->dragFrom_ = i;
        self->dragX_ = rowX(box, self->row_, x, y);
    }
    if (n == 2) self->later(self->onDoubleClick, i);
    else if (n == 1) self->later(self->onSelect, i);
}

void TabStrip::onReleased(GtkGestureClick* g, int, double x, double y, gpointer selfp) {
    TabStrip* self = static_cast<TabStrip*>(selfp);
    GtkWidget* box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    const int i = self->indexOf(box);
    const int close = self->pressClose_, middle = self->pressMiddle_;
    const int from = self->dragFrom_;
    self->pressClose_ = self->pressMiddle_ = -1;
    self->dragFrom_ = -1;
    if (i < 0) return;
    if (from == i && self->onMove) {
        const double rx = rowX(box, self->row_, x, y);
        if (std::abs(rx - self->dragX_) > 8) {   // a drag, not a click
            self->dropAt(i, rx);
            return;
        }
    }
    if (middle == i) {
        self->later(self->onClose, i);
        return;
    }
    if (close == i && within(gtk_widget_pick(box, x, y, GTK_PICK_DEFAULT), self->tabs_[(size_t)i].close))
        self->later(self->onClose, i);
}

void TabStrip::dropAt(int from, double rowX) {
    if (!onMove || from < 0 || from >= (int)tabs_.size()) return;
    std::vector<double> widths;
    for (const Tab& t : tabs_) widths.push_back((double)gtk_widget_get_width(t.box));
    const int to = EditorTabs::dropIndex(widths, from, rowX);
    if (to != from) later([this, to](int a) { if (onMove) onMove(a, to); }, from);
}

// The tab's menu, under the pointer: a popover parented to the tab, so its
// items' actions resolve through the strip.
void TabStrip::showMenu(int index, double x, double y) {
    dropMenu();
    if (!menuFor) return;
    GMenuModel* model = menuFor(index);
    if (!model) return;
    menu_ = gtk_popover_menu_new_from_model(model);
    g_object_unref(model);
    gtk_popover_set_has_arrow(GTK_POPOVER(menu_), FALSE);
    gtk_widget_set_halign(menu_, GTK_ALIGN_START);
    GtkWidget* box = tabs_[(size_t)index].box;
    gtk_widget_set_parent(menu_, box);
    GdkRectangle at = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(menu_), &at);
    gtk_popover_popup(GTK_POPOVER(menu_));
}

// The popover is the tab's child only by gtk_widget_set_parent, so it comes
// off by hand before the tab goes (the LSP popovers' trap, Lsp.cpp).
void TabStrip::dropMenu() {
    if (!menu_) return;
    GtkWidget* m = menu_;
    menu_ = nullptr;
    gtk_popover_popdown(GTK_POPOVER(m));
    if (gtk_widget_get_parent(m)) gtk_widget_unparent(m);
}

// After the row is laid out, scroll it so the active tab is in sight.
void TabStrip::keepActiveInSight() {
    if (sightIdle_) return;
    sightIdle_ = g_idle_add_full(G_PRIORITY_LOW, [](gpointer p) -> gboolean {
        TabStrip* self = static_cast<TabStrip*>(p);
        self->sightIdle_ = 0;
        GtkWidget* tab = self->tabWidget(self->active_);
        if (!tab) return G_SOURCE_REMOVE;
        graphene_rect_t r;
        if (!gtk_widget_compute_bounds(tab, self->row_, &r)) return G_SOURCE_REMOVE;
        GtkAdjustment* h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller_));
        gtk_adjustment_clamp_page(h, r.origin.x, r.origin.x + r.size.width);
        return G_SOURCE_REMOVE;
    }, this, nullptr);
}
