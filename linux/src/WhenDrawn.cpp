// WhenDrawn.cpp — see WhenDrawn.h.
#include "WhenDrawn.h"

namespace {

const char* kDrawnKey = "minicode-drawn";

using Work = std::function<void()>;

// From an idle at the default priority: below the frame clock's own work,
// so the frame that called for this is painted first.
void later(Work fn) {
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer p) -> gboolean {
        (*static_cast<Work*>(p))();
        return G_SOURCE_REMOVE;
    }, new Work(std::move(fn)), [](gpointer p) { delete static_cast<Work*>(p); });
}

}  // namespace

void whenDrawn(GtkWidget* widget, std::function<void()> fn) {
    GtkWidget* target = widget;
    if (GtkRoot* root = gtk_widget_get_root(widget)) target = GTK_WIDGET(root);
    if (g_object_get_data(G_OBJECT(target), kDrawnKey)) {
        later(std::move(fn));
        return;
    }
    // A tick callback runs in the frame clock's update phase, before that
    // frame's layout and paint, and only once the widget is mapped; the
    // work goes to an idle, which runs once the frame is out.
    gtk_widget_add_tick_callback(target, [](GtkWidget* w, GdkFrameClock*, gpointer p) -> gboolean {
        g_object_set_data(G_OBJECT(w), kDrawnKey, GINT_TO_POINTER(1));
        later(*static_cast<Work*>(p));
        return G_SOURCE_REMOVE;
    }, new Work(std::move(fn)), [](gpointer p) { delete static_cast<Work*>(p); });
}
