#include "ClickCount.h"

#include <cmath>

int ClickCount::press(GtkWidget* widget, guint32 time, const void* target, int gtkN, double x,
                      double y) {
    int n = gtkN < 1 ? 1 : gtkN;
    if (n == 1 && target && target == target_ && count_ == 1 && last_ != 0) {
        int doubleMs = 400, distance = 5;
        if (widget)
            g_object_get(gtk_widget_get_settings(widget), "gtk-double-click-time", &doubleMs,
                         "gtk-double-click-distance", &distance, nullptr);
        if (pairs(last_, time, doubleMs) && std::hypot(x - lastX_, y - lastY_) <= distance) n = 2;
    }
    last_ = time;
    lastX_ = x;
    lastY_ = y;
    target_ = target;
    count_ = n;
    return n;
}

int ClickCount::press(GtkGestureClick* g, const void* target, int gtkN, double x, double y) {
    GtkEventController* c = GTK_EVENT_CONTROLLER(g);
    return press(gtk_event_controller_get_widget(c), gtk_event_controller_get_current_event_time(c),
                 target, gtkN, x, y);
}
