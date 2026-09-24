// ScrollSettle.h — a workaround for a GTK 4.22 fault that logged two
// criticals whenever the editor was switched to an image (or a PDF, the LaTeX
// preview, the browser) while its view was still scrolling to the caret:
//
//   g_signal_handler_disconnect: assertion 'handler_id > 0' failed
//   gdk_frame_clock_idle_end_updating: assertion 'priv->updating_count > 0' failed
//
// GtkTextView scrolls with an animation (revealing a Find in Folder match, a
// go to definition, a find step). When its GtkScrolledWindow is unmapped
// mid-animation, the scrolled window turns the animation off on its
// adjustments, and GtkAdjustment does that by jumping to the target value
// first, then disconnecting its frame-clock handler. The jump emits
// value-changed, the text view's reaction to it ends the animation already
// (handler disconnected, frame clock released), and the adjustment then does
// both again with a handler id of 0. Reproduced every time by a test that
// scrolled and switched within 100 ms; no animation, no criticals.
//
// The child is unmapped before the scrolled window looks at its adjustments,
// so setting each adjustment to its own current value from the child's unmap
// ends the animation once, the ordinary way, and there is nothing left for
// the scrolled window to end. The view stops where the animation had got to.
#pragma once

#include <gtk/gtk.h>

inline void settleScrolling(GtkWidget*, gpointer swp) {
    GtkScrolledWindow* s = GTK_SCROLLED_WINDOW(swp);
    GtkAdjustment* adjs[] = {gtk_scrolled_window_get_hadjustment(s),
                             gtk_scrolled_window_get_vadjustment(s)};
    for (GtkAdjustment* a : adjs)
        if (a) gtk_adjustment_set_value(a, gtk_adjustment_get_value(a));
}

inline void settleScrollingOnUnmap(GtkScrolledWindow* sw) {
    if (GtkWidget* child = gtk_scrolled_window_get_child(sw))
        g_signal_connect(child, "unmap", G_CALLBACK(settleScrolling), sw);
}
