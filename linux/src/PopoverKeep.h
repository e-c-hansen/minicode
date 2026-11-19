// PopoverKeep.h — an edit box in a popover that keeps what was typed
// through an app switch (linux/HANDOFF.md item 119, the Mac's #223): the
// Markdown page's block editor, the language server's rename field and the
// tree's name box.
//
// An autohide popover closes when the app loses the keyboard, and what was
// typed in it goes too: on X11 GDK grabs the pointer and keyboard for it, so
// the click on another program's window is the popover's outside click; on
// Wayland the compositor dismisses a grabbing popup when another client
// takes the focus. So these popovers are not autohide. A press in their
// window outside them calls `outside` (each box says what that does: the
// page's editor applies its draft, the rename fields cancel), and the press
// goes on to what it was aimed at; Escape still closes them (GtkPopover's
// own binding). While the window is inactive nothing happens to them, and
// the keyboard comes back to the field the window's focus was in.
#pragma once

#include <gtk/gtk.h>
#include <functional>

namespace PopoverKeep {

namespace detail {
struct State {
    std::function<void()> outside;
    GtkWidget* root = nullptr;           // weak: the window the press watch is on
    GtkEventController* press = nullptr;
};

inline void unwatch(State* s) {
    if (s->root && s->press) gtk_widget_remove_controller(s->root, s->press);
    if (s->root) g_object_remove_weak_pointer(G_OBJECT(s->root), (gpointer*)&s->root);
    s->root = nullptr;
    s->press = nullptr;
}
}  // namespace detail

// `popover` (parented already or later) made to stay; `outside` runs on a
// press in its window outside it while it shows.
inline void install(GtkWidget* popover, std::function<void()> outside) {
    gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
    auto* s = new detail::State{std::move(outside)};
    g_object_set_data_full(G_OBJECT(popover), "mutible-popover-keep", s, [](gpointer p) {
        auto* st = static_cast<detail::State*>(p);
        detail::unwatch(st);
        delete st;
    });
    // The watch is on the window only while the popover shows.
    g_signal_connect(popover, "map", G_CALLBACK(+[](GtkWidget* pop, gpointer sp) {
        auto* st = static_cast<detail::State*>(sp);
        detail::unwatch(st);
        GtkWidget* root = GTK_WIDGET(gtk_widget_get_root(pop));
        if (!root) return;
        GtkGesture* g = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g), GTK_PHASE_CAPTURE);
        g_signal_connect(g, "pressed", G_CALLBACK(+[](GtkGestureClick* gc, int, double, double,
                                                      gpointer popp) {
            GtkWidget* p = GTK_WIDGET(popp);
            auto* state = static_cast<detail::State*>(
                g_object_get_data(G_OBJECT(p), "mutible-popover-keep"));
            GdkEvent* ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gc));
            GtkNative* native = gtk_widget_get_native(p);
            // A press in the popover itself (its own surface) is its own.
            if (!state || !gtk_widget_get_mapped(p) || !ev ||
                (native && gdk_event_get_surface(ev) == gtk_native_get_surface(native)))
                return;
            // From an idle: closing takes this watch off the window, which
            // must not happen while it is handling the press.
            g_idle_add([](gpointer pp) -> gboolean {
                GtkWidget* p = GTK_WIDGET(pp);
                auto* st = static_cast<detail::State*>(
                    g_object_get_data(G_OBJECT(p), "mutible-popover-keep"));
                // A copy: `outside` may drop the popover and this state with it.
                std::function<void()> run = st && gtk_widget_get_mapped(p) ? st->outside : nullptr;
                if (run) run();
                g_object_unref(p);
                return G_SOURCE_REMOVE;
            }, g_object_ref(p));
        }), pop);
        gtk_widget_add_controller(root, GTK_EVENT_CONTROLLER(g));
        st->root = root;
        st->press = GTK_EVENT_CONTROLLER(g);
        g_object_add_weak_pointer(G_OBJECT(root), (gpointer*)&st->root);
    }), s);
    g_signal_connect(popover, "unmap", G_CALLBACK(+[](GtkWidget*, gpointer sp) {
        detail::unwatch(static_cast<detail::State*>(sp));
    }), s);
}

}  // namespace PopoverKeep
