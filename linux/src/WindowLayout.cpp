// WindowLayout.cpp — see WindowLayout.h.
#include "WindowLayout.h"

#include <algorithm>
#include <cmath>

namespace {

// The sidebar's divider dragged this close to the right edge snaps the rest
// of the way, collapsing the right side (the port's own rule since the
// first version, where the Mac's split view collapses past the midpoint).
const int kRightSnap = 60;
// The smallest top is the tab strip and this much of the editor, as the
// Mac's kMinTopSlotHeight; the strip's height when it is not up yet.
const int kTopBelowStrip = 40;
const int kStripHeight = 30;

const char* kLayoutKey = "minicode-window-layout";

WindowLayout* layoutOf(GtkWidget* area) {
    return static_cast<WindowLayout*>(g_object_get_data(G_OBJECT(area), kLayoutKey));
}

// A bar: an empty box whose middle kBar pixels are drawn (the stylesheet's
// .minicode-bar pads the grab area out on both sides).
GtkWidget* makeBar(bool vertical) {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bar, "minicode-bar");
    gtk_widget_add_css_class(bar, vertical ? "vertical" : "horizontal");
    gtk_widget_set_cursor_from_name(bar, vertical ? "col-resize" : "row-resize");
    return bar;
}

int measureMin(GtkWidget* w, GtkOrientation o) {
    if (!w || !gtk_widget_get_visible(w)) return 0;
    int min = 0, nat = 0;
    gtk_widget_measure(w, o, -1, &min, &nat, nullptr, nullptr);
    return min;
}

GdkRectangle pixels(const RightArea::Rect& r) {
    GdkRectangle out;
    out.x = (int)std::lround(r.x);
    out.y = (int)std::lround(r.y);
    out.width = std::max(0, (int)std::lround(r.w));
    out.height = std::max(0, (int)std::lround(r.h));
    return out;
}

}  // namespace

WindowLayout::WindowLayout(GtkWidget* sidebar, GtkWidget* editorPane, GtkWidget* tabStrip,
                           GtkWidget* browserPane, GtkWidget* terminalPane)
    : sidebar_(sidebar), editor_(editorPane), strip_(tabStrip), browser_(browserPane),
      terminal_(terminalPane) {
    parts_.browser = browser_ != nullptr;
    parts_.terminal = terminal_ != nullptr;

    // The right side. A box only for holding children: its layout is ours.
    area_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(area_, "minicode-right-area");
    gtk_widget_set_layout_manager(area_, gtk_custom_layout_new(
        [](GtkWidget*) { return GTK_SIZE_REQUEST_CONSTANT_SIZE; }, areaMeasure, areaAllocate));
    g_object_set_data(G_OBJECT(area_), kLayoutKey, this);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    // Panes keep to their own rectangles, should one be given less than it
    // asks for (a window narrower than the panes' minimums).
    for (GtkWidget* pane : {editor_, browser_, terminal_}) {
        if (!pane) continue;
        gtk_widget_set_overflow(pane, GTK_OVERFLOW_HIDDEN);
        gtk_box_append(GTK_BOX(area_), pane);
    }
    // The bars last, over the panes' edges, the terminal's on top where the
    // two meet, as on the Mac.
    browserBar_ = makeBar(true);
    terminalBar_ = makeBar(false);
    gtk_box_append(GTK_BOX(area_), browserBar_);
    gtk_box_append(GTK_BOX(area_), terminalBar_);

    drag_ = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag_), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag_), GTK_PHASE_CAPTURE);
    g_signal_connect(drag_, "drag-begin", G_CALLBACK(onDragBegin), this);
    g_signal_connect(drag_, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag_, "drag-end", G_CALLBACK(onDragEnd), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(drag_));

    // Sidebar | right side.
    hpaned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(hpaned_, "minicode-split");
    gtk_paned_set_start_child(GTK_PANED(hpaned_), sidebar_);
    gtk_paned_set_end_child(GTK_PANED(hpaned_), area_);
    gtk_paned_set_position(GTK_PANED(hpaned_), sidebarWidth_);
    gtk_paned_set_resize_start_child(GTK_PANED(hpaned_), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(hpaned_), TRUE);
    g_signal_connect(hpaned_, "notify::position", G_CALLBACK(onHpanedPosition), this);
    GtkGesture* press = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
    g_signal_connect(press, "pressed", G_CALLBACK(onHpanedPressed), this);
    gtk_widget_add_controller(hpaned_, GTK_EVENT_CONTROLLER(press));
    gtk_widget_set_vexpand(hpaned_, TRUE);

    apply();
}

WindowLayout::~WindowLayout() {
    // A layout or a drag after this finds no object and does nothing.
    g_object_set_data(G_OBJECT(area_), kLayoutKey, nullptr);
    g_signal_handlers_disconnect_by_data(drag_, this);
    g_signal_handlers_disconnect_by_data(hpaned_, this);
    GListModel* ctrls = gtk_widget_observe_controllers(hpaned_);
    for (guint i = 0; i < g_list_model_get_n_items(ctrls); ++i) {
        GObject* c = static_cast<GObject*>(g_list_model_get_item(ctrls, i));
        g_signal_handlers_disconnect_by_data(c, this);
        g_object_unref(c);
    }
    g_object_unref(ctrls);
}

// ------------------------------------------------------------ the state

void WindowLayout::apply() {
    const RightArea::Frames f = RightArea::layout(state_, 0, 0, limits_, parts_);
    gtk_widget_set_visible(editor_, f.editor);
    if (browser_) gtk_widget_set_visible(browser_, f.browser);
    if (terminal_) gtk_widget_set_visible(terminal_, f.terminal);
    // A bar that hides in the middle of its drag (a side dragged shut) takes
    // nothing with it: the gesture is the right side's, not the bar's.
    gtk_widget_set_visible(browserBar_, f.browserBar);
    gtk_widget_set_visible(terminalBar_, f.terminalBar);
    gtk_widget_queue_allocate(area_);
    if (onChanged) onChanged();
}

bool WindowLayout::focusInside(GtkWidget* pane) const {
    if (!pane) return false;
    GtkRoot* root = gtk_widget_get_root(area_);
    GtkWidget* focus = root ? gtk_root_get_focus(root) : nullptr;
    return focus && (focus == pane || gtk_widget_is_ancestor(focus, pane));
}

void WindowLayout::focusAfterClosingPanel() {
    const bool browser = browserShown();
    if (rightCollapsed()) {
        if (focusTree) focusTree();
    } else if (!browser && editorShown()) {
        if (focusEditor) focusEditor();
    } else if (browser) {
        if (focusBrowser) focusBrowser();
    } else if (terminalShown()) {
        if (focusTerminal) focusTerminal();
    } else if (focusTree) {
        focusTree();
    }
}

void WindowLayout::toggleEditor() {
    if (!editorShown() || rightCollapsed()) {
        revealEditor();
        if (focusEditor) focusEditor();
        return;
    }
    state_.editorHidden = true;
    apply();
    if (terminalShown()) {
        if (focusTerminal) focusTerminal();
    } else if (browserShown()) {
        if (focusBrowser) focusBrowser();
    } else if (focusTree) {
        focusTree();
    }
    collapseRightAreaIfEmpty();
}

void WindowLayout::toggleBrowser() {
    if (!browser_) return;
    const bool had = focusInside(browser_);
    RightArea::toggle(state_, RightArea::Pane::Browser);
    if (state_.browserVisible) showRightArea();
    apply();
    if (state_.browserVisible) {
        if (focusBrowser) focusBrowser();
    } else {
        collapseRightAreaIfEmpty();
        if (had) focusAfterClosingPanel();
    }
}

void WindowLayout::toggleTerminal() {
    if (!terminal_) return;
    const bool had = focusInside(terminal_);
    RightArea::toggle(state_, RightArea::Pane::Terminal);
    if (state_.terminalVisible) showRightArea();
    apply();
    if (state_.terminalVisible) {
        if (focusTerminal) focusTerminal();
    } else {
        collapseRightAreaIfEmpty();
        if (had) focusAfterClosingPanel();
    }
}

void WindowLayout::toggleSidebar() {
    if (sidebarVisible_) {
        // Hiding the tree with the right side collapsed would leave nothing.
        showRightArea();
        const bool had = focusInside(sidebar_);
        sidebarVisible_ = false;
        gtk_widget_set_visible(sidebar_, FALSE);
        // The keyboard leaves a sidebar that is no longer there.
        if (had) {
            if (editorShown()) {
                if (focusEditor) focusEditor();
            } else {
                focusAfterClosingPanel();
            }
        }
    } else {
        sidebarVisible_ = true;
        gtk_widget_set_visible(sidebar_, TRUE);
    }
    if (onChanged) onChanged();
}

void WindowLayout::revealEditor() {
    const bool was = !editorShown() || rightCollapsed();
    state_.editorHidden = false;
    showRightArea();
    if (was) apply();
}

void WindowLayout::showBrowser() {
    if (!browser_ || state_.browserVisible) return;
    state_.browserVisible = true;
    showRightArea();
    apply();
}

void WindowLayout::hideBrowser() {
    if (!browser_ || !state_.browserVisible) return;
    state_.browserVisible = false;
    apply();
    collapseRightAreaIfEmpty();
    focusAfterClosingPanel();
}

// ------------------------------------------------------- the sidebar split

bool WindowLayout::sidebarShown() const { return sidebarVisible_; }

bool WindowLayout::rightCollapsed() const {
    int maxPos = 0;
    g_object_get(hpaned_, "max-position", &maxPos, NULL);
    return sidebarVisible_ && maxPos > 0 &&
           gtk_paned_get_position(GTK_PANED(hpaned_)) >= maxPos;
}

void WindowLayout::showRightArea() {
    if (!rightCollapsed()) return;
    int maxPos = 0;
    g_object_get(hpaned_, "max-position", &maxPos, NULL);
    int width = sidebarWidth_;
    if (width <= 0 || width > maxPos - kRightSnap) width = std::min(240, maxPos / 3);
    adjustingPaned_ = true;
    gtk_paned_set_position(GTK_PANED(hpaned_), width);
    adjustingPaned_ = false;
}

void WindowLayout::collapseRightAreaIfEmpty() {
    if (!RightArea::empty(state_, parts_) || rightCollapsed()) return;
    if (sidebarVisible_) sidebarWidth_ = gtk_paned_get_position(GTK_PANED(hpaned_));
    // Never a window with nothing in it: a hidden sidebar comes back.
    if (!sidebarVisible_) {
        sidebarVisible_ = true;
        gtk_widget_set_visible(sidebar_, TRUE);
    }
    int maxPos = 0;
    g_object_get(hpaned_, "max-position", &maxPos, NULL);
    adjustingPaned_ = true;
    // Not laid out yet, or the sidebar only now shown: GtkPaned clamps it
    // to the right edge at its next layout.
    gtk_paned_set_position(GTK_PANED(hpaned_), maxPos > 0 ? maxPos : G_MAXINT / 2);
    adjustingPaned_ = false;
    if (focusTree) focusTree();
    if (onChanged) onChanged();
}

// Dragging the sidebar's divider near the right edge collapses the right
// side, leaving the file tree.
void WindowLayout::onHpanedPosition(GObject*, GParamSpec*, gpointer data) {
    auto* self = static_cast<WindowLayout*>(data);
    if (self->adjustingPaned_) return;
    int maxPos = 0;
    g_object_get(self->hpaned_, "max-position", &maxPos, NULL);
    const int pos = gtk_paned_get_position(GTK_PANED(self->hpaned_));
    if (maxPos <= 0) return;
    if (pos < maxPos && maxPos - pos < kRightSnap) {
        self->adjustingPaned_ = true;
        gtk_paned_set_position(GTK_PANED(self->hpaned_), maxPos);
        self->adjustingPaned_ = false;
    }
}

// The sidebar's width when a drag starts, so collapsing and reopening comes
// back to it, not to somewhere along the way.
void WindowLayout::onHpanedPressed(GtkGestureClick*, int, double, double, gpointer data) {
    auto* self = static_cast<WindowLayout*>(data);
    if (!self->rightCollapsed())
        self->sidebarWidth_ = gtk_paned_get_position(GTK_PANED(self->hpaned_));
}

// ------------------------------------------------------ the right side

RightArea::Minimums WindowLayout::measureMinimums() const {
    RightArea::Minimums m;
    m.editorWidth = measureMin(editor_, GTK_ORIENTATION_HORIZONTAL);
    m.editorHeight = measureMin(editor_, GTK_ORIENTATION_VERTICAL);
    m.browserWidth = measureMin(browser_, GTK_ORIENTATION_HORIZONTAL);
    m.browserHeight = measureMin(browser_, GTK_ORIENTATION_VERTICAL);
    m.terminalWidth = measureMin(terminal_, GTK_ORIENTATION_HORIZONTAL);
    m.terminalHeight = measureMin(terminal_, GTK_ORIENTATION_VERTICAL);
    return m;
}

RightArea::Limits WindowLayout::limitsFor(const RightArea::Minimums& m) const {
    RightArea::Limits lim;
    lim.bar = kBar;
    int strip = 0;
    if (strip_ && gtk_widget_get_visible(strip_)) {
        int min = 0;
        gtk_widget_measure(strip_, GTK_ORIENTATION_VERTICAL, -1, &min, &strip, nullptr, nullptr);
    }
    lim.minTop = std::max<double>({(double)kTopBelowStrip + (strip > 0 ? strip : kStripHeight),
                                   m.editorHeight, m.browserHeight});
    lim.minEditorWidth = std::max(lim.minEditorWidth, m.editorWidth);
    lim.minBrowserWidth = std::max(lim.minBrowserWidth, m.browserWidth);
    lim.minTerminal = std::max(lim.minTerminal, m.terminalHeight);
    return lim;
}

void WindowLayout::areaMeasure(GtkWidget* w, GtkOrientation o, int, int* min, int* nat,
                               int* minBase, int* natBase) {
    *minBase = *natBase = -1;
    *min = *nat = 0;
    WindowLayout* self = layoutOf(w);
    if (!self) return;
    const RightArea::Minimums m = self->measureMinimums();
    double width = 0, height = 0;
    RightArea::minimumSize(self->state_, self->limitsFor(m), m, &width, &height, self->parts_);
    *min = *nat = (int)std::ceil(o == GTK_ORIENTATION_HORIZONTAL ? width : height);
}

void WindowLayout::areaAllocate(GtkWidget* w, int width, int height, int) {
    if (WindowLayout* self = layoutOf(w)) self->allocateArea(width, height);
}

void WindowLayout::allocateArea(int W, int H) {
    // Measured first, as GTK wants before an allocation; the limits come
    // from the same numbers.
    const RightArea::Minimums m = measureMinimums();
    limits_ = limitsFor(m);
    frames_ = RightArea::layout(state_, W, H, limits_, parts_);
    auto place = [](GtkWidget* child, bool on, const RightArea::Rect& r) {
        if (!child || !on || !gtk_widget_get_visible(child)) return;
        const GdkRectangle a = pixels(r);
        gtk_widget_size_allocate(child, &a, -1);
    };
    place(editor_, frames_.editor, frames_.editorRect);
    place(browser_, frames_.browser, frames_.browserRect);
    place(terminal_, frames_.terminal, frames_.terminalRect);
    for (Dragging which : {Dragging::Browser, Dragging::Terminal}) {
        GtkWidget* bar = which == Dragging::Browser ? browserBar_ : terminalBar_;
        if (!gtk_widget_get_visible(bar)) continue;
        int min = 0, nat = 0;
        gtk_widget_measure(bar, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, nullptr, nullptr);
        gtk_widget_measure(bar, GTK_ORIENTATION_VERTICAL, -1, &min, &nat, nullptr, nullptr);
        GdkRectangle a;
        if (grabRect(which, &a)) gtk_widget_size_allocate(bar, &a, -1);
    }
}

bool WindowLayout::grabRect(Dragging which, GdkRectangle* out) const {
    if (which == Dragging::Browser) {
        if (!frames_.browserBar) return false;
        GdkRectangle r = pixels(frames_.browserBarRect);
        r.x -= kGrab;
        r.width += 2 * kGrab;
        *out = r;
        return true;
    }
    if (which == Dragging::Terminal) {
        if (!frames_.terminalBar) return false;
        GdkRectangle r = pixels(frames_.terminalBarRect);
        // At the top edge, with the top empty, the grab area reaches down
        // into the terminal only.
        if (r.y >= kGrab) r.y -= kGrab, r.height += kGrab;
        r.height += kGrab;
        *out = r;
        return true;
    }
    return false;
}

// ---------------------------------------------------------- the bars' drag

void WindowLayout::onDragBegin(GtkGestureDrag* g, double x, double y, gpointer data) {
    auto* self = static_cast<WindowLayout*>(data);
    self->dragging_ = Dragging::None;
    // The terminal's bar first: it wins where the two meet.
    for (Dragging which : {Dragging::Terminal, Dragging::Browser}) {
        GdkRectangle r;
        if (!self->grabRect(which, &r)) continue;
        if (x < r.x || x >= r.x + r.width || y < r.y || y >= r.y + r.height) continue;
        self->dragging_ = which;
        break;
    }
    if (self->dragging_ == Dragging::None) {
        // Not on a bar: the press is the panes' own.
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    if (self->dragging_ == Dragging::Browser) {
        const RightArea::Rect& b = self->frames_.browserBarRect;
        self->dragStart_ = x;
        self->dragGrab_ = x - (b.x + b.w / 2);
        gtk_widget_add_css_class(self->browserBar_, "dragging");
    } else {
        const RightArea::Rect& b = self->frames_.terminalBarRect;
        self->dragStart_ = y;
        self->dragGrab_ = y - (b.y + b.h / 2);
        gtk_widget_add_css_class(self->terminalBar_, "dragging");
    }
}

void WindowLayout::onDragUpdate(GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* self = static_cast<WindowLayout*>(data);
    if (self->dragging_ == Dragging::None) return;
    self->dragTo(self->dragStart_ + (self->dragging_ == Dragging::Browser ? dx : dy) -
                 self->dragGrab_);
}

void WindowLayout::onDragEnd(GtkGestureDrag*, double, double, gpointer data) {
    auto* self = static_cast<WindowLayout*>(data);
    gtk_widget_remove_css_class(self->browserBar_, "dragging");
    gtk_widget_remove_css_class(self->terminalBar_, "dragging");
    self->dragging_ = Dragging::None;
}

void WindowLayout::dragTo(double pos) {
    const RightArea::State before = state_;
    const double W = gtk_widget_get_width(area_), H = gtk_widget_get_height(area_);
    const RightArea::Hid hid =
        dragging_ == Dragging::Browser ? RightArea::dragBrowserBar(state_, pos, W, limits_, parts_)
                                       : RightArea::dragTerminalBar(state_, pos, H, limits_, parts_);
    const bool shownChanged = before.editorHidden != state_.editorHidden ||
                              before.browserVisible != state_.browserVisible;
    if (shownChanged) {
        apply();
    } else {
        gtk_widget_queue_allocate(area_);
    }
    // The keyboard leaves what went, as the Mac's bars move it.
    switch (hid) {
    case RightArea::Hid::Editor:
        if (focusBrowser) focusBrowser();
        break;
    case RightArea::Hid::Browser:
        if (focusEditor) focusEditor();
        break;
    case RightArea::Hid::Top:
        if (focusTerminal) focusTerminal();
        break;
    case RightArea::Hid::Nothing:
        break;
    }
}
