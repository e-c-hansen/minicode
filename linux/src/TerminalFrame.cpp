// TerminalFrame.cpp — see TerminalFrame.h.
#include "TerminalFrame.h"

#include <algorithm>

G_DECLARE_FINAL_TYPE(MutibleTermFrame, mutible_term_frame, MUTIBLE, TERM_FRAME, GtkWidget)

struct _MutibleTermFrame {
    GtkWidget parent_instance;
    GtkWidget* child;
    // Held: the child's whole size and how far up and left it is moved;
    // 0 x 0 is the frame's own size.
    int heldW, heldH, offX, offY;
    GdkRGBA background;
    graphene_rect_t childRect;   // as last laid out
};

enum { PROP_0, PROP_HADJUSTMENT, PROP_VADJUSTMENT, PROP_HSCROLL_POLICY, PROP_VSCROLL_POLICY };

static void mutible_term_frame_scrollable_init(GtkScrollableInterface*) {}

G_DEFINE_FINAL_TYPE_WITH_CODE(MutibleTermFrame, mutible_term_frame, GTK_TYPE_WIDGET,
                              G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, mutible_term_frame_scrollable_init))

// The scrolled window's adjustments and policies are the child's: VTE sets
// the vertical one from its scrollback, as it did as the scrolled window's
// own child, and nothing configures the horizontal one (its policy is
// EXTERNAL), so the wheel never pans.
static void mutible_term_frame_set_property(GObject* o, guint id, const GValue* v, GParamSpec* pspec) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(o);
    switch (id) {
    case PROP_HADJUSTMENT:
    case PROP_VADJUSTMENT:
    case PROP_HSCROLL_POLICY:
    case PROP_VSCROLL_POLICY:
        if (self->child) g_object_set_property(G_OBJECT(self->child), pspec->name, v);
        break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, pspec);
    }
}

static void mutible_term_frame_get_property(GObject* o, guint id, GValue* v, GParamSpec* pspec) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(o);
    switch (id) {
    case PROP_HADJUSTMENT:
    case PROP_VADJUSTMENT:
    case PROP_HSCROLL_POLICY:
    case PROP_VSCROLL_POLICY:
        if (self->child) g_object_get_property(G_OBJECT(self->child), pspec->name, v);
        break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, pspec);
    }
}

static void mutible_term_frame_dispose(GObject* o) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(o);
    g_clear_pointer(&self->child, gtk_widget_unparent);
    G_OBJECT_CLASS(mutible_term_frame_parent_class)->dispose(o);
}

static bool held(const MutibleTermFrame* self) { return self->heldW > 0 && self->heldH > 0; }

// The child's own measure, as the scrolled window had it: VTE's natural
// size is its default 80x24 grid whatever it is laid out at (420 pixels
// high in `host_wrap`'s 10-row view), so a held grid asks the view for
// nothing and a pane beside it never moves for another renderer's size.
static void mutible_term_frame_measure(GtkWidget* w, GtkOrientation o, int forSize, int* min, int* nat,
                                       int* minBase, int* natBase) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(w);
    *min = *nat = 0;
    *minBase = *natBase = -1;
    if (!self->child || !gtk_widget_get_visible(self->child)) return;
    gtk_widget_measure(self->child, o, forSize, min, nat, nullptr, nullptr);
}

static void mutible_term_frame_size_allocate(GtkWidget* w, int width, int height, int) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(w);
    if (!self->child || !gtk_widget_get_visible(self->child)) return;
    // Never below the child's minimum (GTK warns at that).
    int minW = 0, minH = 0;
    gtk_widget_measure(self->child, GTK_ORIENTATION_HORIZONTAL, -1, &minW, nullptr, nullptr, nullptr);
    gtk_widget_measure(self->child, GTK_ORIENTATION_VERTICAL, -1, &minH, nullptr, nullptr, nullptr);
    GtkAllocation a{0, 0, width, height};
    if (held(self)) a = GtkAllocation{-self->offX, -self->offY, self->heldW, self->heldH};
    a.width = std::max(a.width, minW);
    a.height = std::max(a.height, minH);
    self->childRect = GRAPHENE_RECT_INIT((float)a.x, (float)a.y, (float)a.width, (float)a.height);
    gtk_widget_size_allocate(self->child, &a, -1);
}

// The terminal's background where the child does not reach (to its right
// and below it, the only sides it can fall short of); never under the
// child, whose own background a translucent color would double.
static void mutible_term_frame_snapshot(GtkWidget* w, GtkSnapshot* s) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(w);
    if (!self->child) return;
    const float width = (float)gtk_widget_get_width(w), height = (float)gtk_widget_get_height(w);
    const graphene_rect_t& c = self->childRect;
    const float right = c.origin.x + c.size.width, bottom = c.origin.y + c.size.height;
    if (self->background.alpha > 0) {
        if (right < width) {
            const graphene_rect_t r = GRAPHENE_RECT_INIT(std::max(0.0f, right), 0, width - std::max(0.0f, right), height);
            gtk_snapshot_append_color(s, &self->background, &r);
        }
        if (bottom < height && right > 0) {
            const float upTo = std::min(right, width);
            const graphene_rect_t r = GRAPHENE_RECT_INIT(0, std::max(0.0f, bottom), upTo, height - std::max(0.0f, bottom));
            gtk_snapshot_append_color(s, &self->background, &r);
        }
    }
    gtk_widget_snapshot_child(w, self->child, s);
}

static void mutible_term_frame_class_init(MutibleTermFrameClass* klass) {
    GObjectClass* oc = G_OBJECT_CLASS(klass);
    oc->set_property = mutible_term_frame_set_property;
    oc->get_property = mutible_term_frame_get_property;
    oc->dispose = mutible_term_frame_dispose;
    g_object_class_override_property(oc, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(oc, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(oc, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(oc, PROP_VSCROLL_POLICY, "vscroll-policy");
    GtkWidgetClass* wc = GTK_WIDGET_CLASS(klass);
    wc->measure = mutible_term_frame_measure;
    wc->size_allocate = mutible_term_frame_size_allocate;
    wc->snapshot = mutible_term_frame_snapshot;
    gtk_widget_class_set_css_name(wc, "terminal-frame");
}

static void mutible_term_frame_init(MutibleTermFrame* self) {
    self->child = nullptr;
    self->heldW = self->heldH = self->offX = self->offY = 0;
    self->background = GdkRGBA{0, 0, 0, 0};
    self->childRect = GRAPHENE_RECT_INIT(0, 0, 0, 0);
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
    // A press in the empty part (a smaller pty's) gives the terminal the
    // keyboard, as a press on it does; one on the child is the child's.
    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double x, double y, gpointer) {
        GtkWidget* frame = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
        MutibleTermFrame* f = MUTIBLE_TERM_FRAME(frame);
        const graphene_point_t p = GRAPHENE_POINT_INIT((float)x, (float)y);
        if (f->child && !graphene_rect_contains_point(&f->childRect, &p)) gtk_widget_grab_focus(f->child);
    }), nullptr);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
}

GtkWidget* terminalFrameNew(GtkWidget* child) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(g_object_new(mutible_term_frame_get_type(), nullptr));
    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void terminalFrameHold(GtkWidget* frame, int width, int height, int x, int y) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(frame);
    if (width <= 0 || height <= 0) width = height = x = y = 0;
    if (self->heldW == width && self->heldH == height && self->offX == x && self->offY == y) return;
    self->heldW = width;
    self->heldH = height;
    self->offX = x;
    self->offY = y;
    // Only the child's place changes: the frame's own measure does not.
    gtk_widget_queue_allocate(frame);
}

void terminalFrameSetBackground(GtkWidget* frame, const GdkRGBA& color) {
    MutibleTermFrame* self = MUTIBLE_TERM_FRAME(frame);
    if (gdk_rgba_equal(&self->background, &color)) return;
    self->background = color;
    gtk_widget_queue_draw(frame);
}

graphene_rect_t terminalFrameChildRect(GtkWidget* frame) {
    return MUTIBLE_TERM_FRAME(frame)->childRect;
}
