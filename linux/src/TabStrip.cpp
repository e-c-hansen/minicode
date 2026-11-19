// TabStrip.cpp — see TabStrip.h.
#include "TabStrip.h"

#include <algorithm>
#include <cmath>

#include "EditorTabs.h"

namespace {
// The close button's two faces.
const char* kCloseMark = "×";   // ×
const char* kDirtyMark = "●";   // ●

// The Mac's numbers (mac/src/TabStrip.mm), in GTK's pixels: tabs keep their
// natural width within these (EditorTabs::layoutStrip); the hint's gap after
// the title and the least of it worth showing cut (fitName); the fades at a
// cut-off end, the scroll bar's height, and a drag's scrolling near an end
// (dragAutoScroll), per frame.
constexpr double kMinTab = 80, kMaxTab = 320;
constexpr double kHintGap = 5, kMinHint = 24;
constexpr int kFadeWidth = 18, kBarHeight = 3;
constexpr double kAutoScrollEdge = 28, kAutoScrollStep = 14;
// A wheel's notch (GTK gives 1 a notch, the Mac several lines): about a
// third of a tab.
constexpr double kWheelLine = 48;
// How far a press may move and still be a click (onReleased).
constexpr double kDragSlack = 8;

const char* kStripKey = "mutible-tab-strip";

#if GTK_CHECK_VERSION(4, 12, 0)
constexpr GtkAlign kBaseline = GTK_ALIGN_BASELINE_FILL;
#else
constexpr GtkAlign kBaseline = GTK_ALIGN_BASELINE;
#endif

bool within(GtkWidget* w, GtkWidget* area) {
    for (; w; w = gtk_widget_get_parent(w))
        if (w == area) return true;
    return false;
}

void disconnectControllers(GtkWidget* w, gpointer data) {
    GListModel* ctrls = gtk_widget_observe_controllers(w);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        g_signal_handlers_disconnect_by_data(c, data);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
}

// A string's drawn width in a label's font (its Pango context follows the
// CSS: the preview tab's italics, tabs.font-size, the hint's smaller size),
// in whole pixels as the label measures itself.
double textWidth(GtkWidget* label, const std::string& s) {
    PangoLayout* l = gtk_widget_create_pango_layout(label, s.c_str());
    int w = 0;
    pango_layout_get_size(l, &w, nullptr);
    g_object_unref(l);
    return std::ceil((double)w / PANGO_SCALE);
}

TabStrip* stripOf(GtkWidget* w) {
    return static_cast<TabStrip*>(g_object_get_data(G_OBJECT(w), kStripKey));
}
}  // namespace

TabStrip::TabStrip() {
    root_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(root_, "minicode-tabs");
    gtk_widget_set_hexpand(root_, TRUE);
    // The row: a box only for holding the tabs, laid out by rowAllocate at
    // the core's places and clipped. It asks for no width (a row of any
    // length scrolls) and for no height beyond its natural one: the
    // window's vertical split measures the editor's side at small heights,
    // and a minimum here added to the LaTeX preview's made GTK warn
    // (HANDOFF.md, "Trying to measure GtkBox").
    row_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_layout_manager(row_, gtk_custom_layout_new(
        [](GtkWidget*) { return GTK_SIZE_REQUEST_CONSTANT_SIZE; }, rowMeasure, rowAllocate));
    g_object_set_data(G_OBJECT(row_), kStripKey, this);
    gtk_widget_set_overflow(row_, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_hexpand(row_, TRUE);
    // Over the tabs, last so they draw on top, and never taking a click.
    auto overlay = [&](const char* cls) {
        GtkWidget* w = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(w, cls);
        gtk_widget_set_can_target(w, FALSE);
        gtk_box_append(GTK_BOX(row_), w);
        return w;
    };
    fadeLeft_ = overlay("minicode-tabs-fade-left");
    fadeRight_ = overlay("minicode-tabs-fade-right");
    bar_ = overlay("minicode-tabs-bar");
    gtk_box_append(GTK_BOX(root_), row_);

    // The wheel and a touchpad, both ways (EditorTabs::wheelScroll).
    GtkEventController* scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(onScroll), this);
    gtk_widget_add_controller(row_, scroll);
    // The pointer over the row (the bar), and where a dragged tab's pointer
    // is: in the capture phase, as the tab's own gesture holds the drag.
    GtkEventController* motion = gtk_event_controller_motion_new();
    gtk_event_controller_set_propagation_phase(motion, GTK_PHASE_CAPTURE);
    g_signal_connect(motion, "motion", G_CALLBACK(onMotion), this);
    g_signal_connect(motion, "enter", G_CALLBACK(+[](GtkEventControllerMotion*, double, double,
                                                     gpointer p) {
        TabStrip* self = static_cast<TabStrip*>(p);
        self->inside_ = true;
        gtk_widget_queue_allocate(self->row_);
    }), this);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer p) {
        TabStrip* self = static_cast<TabStrip*>(p);
        self->inside_ = false;
        gtk_widget_queue_allocate(self->row_);
    }), this);
    gtk_widget_add_controller(row_, motion);
}

TabStrip::~TabStrip() {
    alive_.reset();   // callbacks waiting in an idle find the strip gone
    if (autoScroll_) gtk_widget_remove_tick_callback(row_, autoScroll_);
    autoScroll_ = 0;
    dropMenu();
    g_object_set_data(G_OBJECT(row_), kStripKey, nullptr);   // the layout finds no strip
    disconnectControllers(row_, this);
    for (const Tab& t : tabs_) disconnectControllers(t.box, this);
}

TabStrip::Tab TabStrip::makeTab() {
    Tab t;
    t.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(t.box, "minicode-tab");
    // The title and the hint on one baseline, the hint right after the
    // title; the room left over (a tab at its smallest) after them, so the
    // close button stays at the tab's right end.
    t.names = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(t.names, TRUE);
    gtk_widget_set_valign(t.names, GTK_ALIGN_CENTER);
    t.title = gtk_label_new("");
    gtk_widget_add_css_class(t.title, "minicode-tab-title");
    // Titles come cut already (fitName); this only keeps a label that is a
    // pixel over its room from asking for more.
    gtk_label_set_ellipsize(GTK_LABEL(t.title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_single_line_mode(GTK_LABEL(t.title), TRUE);
    gtk_widget_set_valign(t.title, kBaseline);
    t.hint = gtk_label_new("");
    gtk_widget_add_css_class(t.hint, "minicode-tab-hint");
    gtk_label_set_ellipsize(GTK_LABEL(t.hint), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_single_line_mode(GTK_LABEL(t.hint), TRUE);
    gtk_widget_set_valign(t.hint, kBaseline);
    gtk_widget_set_margin_start(t.hint, (int)kHintGap);
    gtk_widget_set_visible(t.hint, FALSE);
    gtk_box_append(GTK_BOX(t.names), t.title);
    gtk_box_append(GTK_BOX(t.names), t.hint);
    t.close = gtk_label_new(kCloseMark);
    gtk_widget_add_css_class(t.close, "minicode-tab-close");
    gtk_box_append(GTK_BOX(t.box), t.names);
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
    // The tab on show is brought into view when it is another tab now: the
    // row changed length, or its title or path did (a preview replaced). A
    // dot for unsaved changes leaves the row where the user scrolled it.
    const int a = active >= 0 && active < (int)items.size() ? active : -1;
    if (items.size() != items_.size()) reveal_ = true;
    else if (a >= 0 && (items[(size_t)a].title != items_[(size_t)a].title ||
                        items[(size_t)a].tooltip != items_[(size_t)a].tooltip))
        reveal_ = true;
    if (a != active_) reveal_ = true;

    while (tabs_.size() < items.size()) {
        tabs_.push_back(makeTab());
        gtk_widget_insert_before(tabs_.back().box, row_, fadeLeft_);   // under the fades
    }
    while (tabs_.size() > items.size()) {
        GtkWidget* box = tabs_.back().box;
        tabs_.pop_back();
        disconnectControllers(box, this);
        if (menu_ && within(gtk_widget_get_parent(menu_), box)) dropMenu();
        gtk_box_remove(GTK_BOX(row_), box);
    }
    items_ = items;
    active_ = a;
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i];
        const Tab& t = tabs_[i];
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
    fitTitles(true);
    stale_ = true;
    gtk_widget_queue_allocate(row_);
}

GtkWidget* TabStrip::tabWidget(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i].box : nullptr;
}

GtkWidget* TabStrip::titleWidget(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i].title : nullptr;
}

GtkWidget* TabStrip::hintWidget(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i].hint : nullptr;
}

GtkWidget* TabStrip::closeWidget(int i) const {
    return i >= 0 && i < (int)tabs_.size() ? tabs_[(size_t)i].close : nullptr;
}

std::string TabStrip::displayedTitle(int i) const {
    GtkWidget* t = titleWidget(i);
    return t ? gtk_label_get_text(GTK_LABEL(t)) : "";
}

std::string TabStrip::displayedHint(int i) const {
    GtkWidget* h = hintWidget(i);
    return h && gtk_widget_get_visible(h) ? gtk_label_get_text(GTK_LABEL(h)) : "";
}

double TabStrip::tabLeft(int i) const {
    return i >= 0 && i < (int)x_.size() ? x_[(size_t)i] : 0;
}

double TabStrip::tabWidth(int i) const {
    return i >= 0 && i < (int)w_.size() ? w_[(size_t)i] : 0;
}

int TabStrip::indexOf(GtkWidget* box) const {
    for (size_t i = 0; i < tabs_.size(); i++)
        if (tabs_[i].box == box) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- layout

// Tab i as GTK measures it: its padding, gaps and close button (all but the
// title and hint, whatever they show now), plus its whole title and hint.
TabStrip::Measured TabStrip::measure(int i) const {
    const Tab& t = tabs_[(size_t)i];
    const Item& it = items_[(size_t)i];
    int box = 0, names = 0;
    gtk_widget_measure(t.box, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &box, nullptr, nullptr);
    gtk_widget_measure(t.names, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &names, nullptr, nullptr);
    Measured m;
    m.chrome = box - names;
    double text = textWidth(t.title, it.title);
    if (!it.hint.empty()) text += kHintGap + textWidth(t.hint, it.hint);
    m.natural = m.chrome + text;
    return m;
}

// Every tab at its natural width, its whole title and hint, between the
// smallest and the widest tab, never squeezed to fit (layoutStrip).
void TabStrip::measureRow() {
    std::vector<double> natural;
    measured_.clear();
    for (size_t i = 0; i < tabs_.size() && i < items_.size(); i++) {
        measured_.push_back(measure((int)i));
        natural.push_back(measured_.back().natural);
    }
    const EditorTabs::StripLayout l = EditorTabs::layoutStrip(natural, kMinTab, kMaxTab);
    x_ = l.x;
    w_ = l.w;
    content_ = l.content;
}

// The scroll kept in range, or moved to bring the tab on show into view
// when it changed (revealScroll).
void TabStrip::placeScroll(int width) {
    if (width != view_) reveal_ = true;   // a new width: the tab on show into view again
    view_ = width;
    if (reveal_ && active_ >= 0 && active_ < (int)x_.size()) {
        const double left = x_[(size_t)active_];
        scroll_ = EditorTabs::revealScroll(scroll_, left, left + w_[(size_t)active_], content_, width);
    } else {
        scroll_ = EditorTabs::clampScroll(scroll_, content_, width);
    }
    if (width > 0) reveal_ = false;   // only once there is a width to reveal in
}

// What tab i shows at its width: its title and hint whole, but past the
// widest tab cut in the middle, the hint first (fitName).
static EditorTabs::TabName fitted(const TabStrip::Item& it, GtkWidget* title, GtkWidget* hint,
                                  double natural, double chrome, double width) {
    EditorTabs::TabName whole{it.title, it.hint};
    if (natural <= width) return whole;
    return EditorTabs::fitName(whole, std::max(0.0, width - chrome), kHintGap, kMinHint,
                               [title](const std::string& s) { return textWidth(title, s); },
                               [hint](const std::string& s) { return textWidth(hint, s); });
}

// The titles and hints each tab should show at its width, put in place when
// `apply` (else only looked at); whether any differed from what it showed.
bool TabStrip::fitTitles(bool apply) {
    if (apply) measureRow();
    bool differed = false;
    for (size_t i = 0; i < tabs_.size() && i < measured_.size() && i < w_.size(); i++) {
        const Tab& t = tabs_[i];
        const EditorTabs::TabName want = fitted(items_[i], t.title, t.hint, measured_[i].natural,
                                                measured_[i].chrome, w_[i]);
        const bool hintShown = gtk_widget_get_visible(t.hint);
        const bool same = want.name == gtk_label_get_text(GTK_LABEL(t.title)) &&
                          hintShown == !want.hint.empty() &&
                          (!hintShown || want.hint == gtk_label_get_text(GTK_LABEL(t.hint)));
        if (same) continue;
        differed = true;
        if (!apply) continue;
        gtk_label_set_text(GTK_LABEL(t.title), want.name.c_str());
        gtk_label_set_text(GTK_LABEL(t.hint), want.hint.c_str());
        gtk_widget_set_visible(t.hint, !want.hint.empty());
    }
    return differed;
}

// Titles cut anew once the layout is over: the fonts the CSS gives came in
// after setItems measured (a strip not yet on screen, a new tabs.font-size).
void TabStrip::refitLater() {
    if (refit_) return;
    refit_ = 1;
    struct Ctx {
        std::weak_ptr<int> alive;
        TabStrip* self;
    };
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto* c = static_cast<Ctx*>(p);
        if (c->alive.expired()) return G_SOURCE_REMOVE;
        c->self->refit_ = 0;
        c->self->fitTitles(true);
        return G_SOURCE_REMOVE;
    }, new Ctx{alive_, this}, [](gpointer p) { delete static_cast<Ctx*>(p); });
}

void TabStrip::rowMeasure(GtkWidget* w, GtkOrientation o, int, int* min, int* nat, int* minBase,
                          int* natBase) {
    *min = *nat = 0;
    *minBase = *natBase = -1;
    TabStrip* self = stripOf(w);
    if (!self) return;
    // Asked again only when something in the row changed size (a title, a
    // font), never for a scroll: the next layout measures the tabs anew.
    self->stale_ = true;
    if (o == GTK_ORIENTATION_HORIZONTAL) return;
    for (const Tab& t : self->tabs_) {
        int n = 0;
        gtk_widget_measure(t.box, GTK_ORIENTATION_VERTICAL, -1, nullptr, &n, nullptr, nullptr);
        *nat = std::max(*nat, n);
    }
}

void TabStrip::rowAllocate(GtkWidget* w, int width, int height, int) {
    TabStrip* self = stripOf(w);
    if (!self) return;
    const bool measured = self->stale_;
    if (measured) self->measureRow();
    self->stale_ = false;
    self->placeScroll(width);
    for (size_t i = 0; i < self->tabs_.size() && i < self->x_.size(); i++) {
        GtkWidget* box = self->tabs_[i].box;
        const int tw = (int)self->w_[i];
        int minH = 0;
        gtk_widget_measure(box, GTK_ORIENTATION_VERTICAL, tw, &minH, nullptr, nullptr, nullptr);
        GtkAllocation r = {(int)std::lround(self->x_[i] - self->scroll_), 0, tw, std::max(height, minH)};
        gtk_widget_size_allocate(box, &r, -1);
    }
    // A row that overflows: its cut-off ends fade into the strip, and while
    // the pointer is over it a thin bar says where the view is.
    const EditorTabs::Clipped cut = EditorTabs::clipped(self->scroll_, self->content_, width);
    const bool bar = self->inside_ && self->content_ > width && self->content_ > 0;
    const int barW = bar ? std::max(24, (int)std::floor((double)width * width / self->content_)) : 0;
    struct Over {
        GtkWidget* w;
        bool shown;
        GtkAllocation r;
    } over[] = {
        {self->fadeLeft_, cut.left, {0, 0, kFadeWidth, height}},
        {self->fadeRight_, cut.right, {width - kFadeWidth, 0, kFadeWidth, height}},
        {self->bar_, bar,
         {bar ? (int)std::floor(self->scroll_ / self->content_ * width) : 0,
          std::max(0, height - kBarHeight), barW, kBarHeight}},
    };
    for (Over& o : over) {
        gtk_widget_set_child_visible(o.w, o.shown);
        gtk_widget_measure(o.w, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, nullptr, nullptr, nullptr);
        gtk_widget_size_allocate(o.w, &o.r, -1);
    }
    // Fonts that came after setItems measured (a strip not yet on screen,
    // a new tabs.font-size) cut the titles anew.
    if (measured && self->fitTitles(false)) self->refitLater();
}

// What the next layout would do, done now: the tab on show revealed as
// setItems asked, over sizes measured anew if GTK has not laid them out
// yet. A wheel or a drag scrolls from there, so a reveal still waiting for
// its frame cannot undo it (a wheel turned right after a title changed
// was put back by the layout that followed).
void TabStrip::settle() {
    if (!reveal_ && !stale_) return;
    if (stale_) measureRow();   // stale_ stays: the layout checks the fonts too
    if (view_ > 0) placeScroll(view_);
}

void TabStrip::scrollTo(double scroll) {
    const double next = EditorTabs::clampScroll(scroll, content_, view_);
    if (next == scroll_) return;
    scroll_ = next;
    gtk_widget_queue_allocate(row_);
}

// A wheel or a touchpad scrolls a row that overflows, the vertical wheel
// too (EditorTabs::wheelScroll); over a row that fits, the event goes on.
gboolean TabStrip::onScroll(GtkEventControllerScroll* c, double dx, double dy, gpointer p) {
    TabStrip* self = static_cast<TabStrip*>(p);
    self->settle();
    if (self->content_ <= self->view_) return FALSE;
    bool precise = false;
#if GTK_CHECK_VERSION(4, 8, 0)
    precise = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_SURFACE;
#else
    (void)c;
#endif
    // GTK's deltas point the way the view goes; the core's the way the
    // content goes (with the fingers).
    self->inside_ = true;
    self->scrollTo(EditorTabs::wheelScroll(self->scroll_, -dx, -dy, precise, kWheelLine,
                                           self->content_, self->view_));
    return TRUE;
}

// While a tab is dragged, held near an end of a row that scrolls, the row
// scrolls that way each frame (EditorTabs::dragAutoScroll).
void TabStrip::onMotion(GtkEventControllerMotion*, double x, double, gpointer p) {
    TabStrip* self = static_cast<TabStrip*>(p);
    if (!self->inside_) {
        self->inside_ = true;
        gtk_widget_queue_allocate(self->row_);
    }
    if (self->dragFrom_ < 0) return;
    self->dragViewX_ = x;
    if (!self->dragging_ && std::abs(x + self->scroll_ - self->dragX_) > kDragSlack)
        self->dragging_ = true;
    if (self->dragging_ && !self->autoScroll_ &&
        EditorTabs::dragAutoScroll(x, self->view_, kAutoScrollEdge, kAutoScrollStep) != 0)
        self->autoScroll();
}

void TabStrip::autoScroll() {
    autoScroll_ = gtk_widget_add_tick_callback(row_, [](GtkWidget*, GdkFrameClock*, gpointer p) -> gboolean {
        TabStrip* self = static_cast<TabStrip*>(p);
        self->settle();
        const double step = self->dragging_ && self->dragFrom_ >= 0
                                ? EditorTabs::dragAutoScroll(self->dragViewX_, self->view_,
                                                             kAutoScrollEdge, kAutoScrollStep)
                                : 0;
        const double next = EditorTabs::clampScroll(self->scroll_ + step, self->content_, self->view_);
        if (step == 0 || next == self->scroll_) {
            self->autoScroll_ = 0;
            return G_SOURCE_REMOVE;
        }
        self->scrollTo(next);
        return G_SOURCE_CONTINUE;
    }, this, nullptr);
}

// ---------------------------------------------------------------- the pointer

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
        // In the whole row's coordinates: the tab's own x from its left
        // edge, which is at its place in the row however far it scrolled.
        self->settle();
        self->dragFrom_ = i;
        self->dragX_ = self->tabLeft(i) + x;
        self->dragging_ = false;
    }
    // GTK's count is reset by a slow first click (selecting a tab shows a
    // file); the presses' event times decide too.
    n = self->clicks_.press(g, box, n, x, y);
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
    self->dragging_ = false;   // the drag's scrolling stops at its next frame
    if (i < 0) return;
    if (from == i && self->onMove) {
        const double rx = self->tabLeft(i) + x;
        if (std::abs(rx - self->dragX_) > kDragSlack) {   // a drag, not a click
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
    if (!onMove || from < 0 || from >= (int)tabs_.size() || w_.size() != tabs_.size()) return;
    const int to = EditorTabs::dropIndex(w_, from, rowX);
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
