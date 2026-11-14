// ClickCount: a double-click counted by the presses' own event times.
//
// GtkGestureClick counts presses with a timer on the main loop. When the first
// click's handler is slow (it opens a file, shows a pane) or the main loop is
// busy, the second press is handled after GTK reset its count and arrives as
// n = 1, and the double-click's action never happens. This counts a press as
// the second of a double-click when the press before was on the same target and
// the two event times are within gtk-double-click-time, with GTK's own count as
// a floor. The Mac decides by time and place too. One counter per widget.
#pragma once
#include <gtk/gtk.h>

class ClickCount {
public:
    // The count for a press: `gtkN` is GTK's, `time` the press's event time
    // (gtk_event_controller_get_current_event_time), `target` what was pressed
    // (a row, a tab index + 1; null never pairs), `widget` gives the settings.
    int press(GtkWidget* widget, guint32 time, const void* target, int gtkN);
    // Same, reading the time from the gesture's current event.
    int press(GtkGestureClick* g, const void* target, int gtkN);
    void forget() { last_ = 0; target_ = nullptr; count_ = 0; }

    // The rule alone, for tests: the second press of a pair?
    static bool pairs(guint32 prevTime, guint32 time, int doubleMs) {
        return time - prevTime <= (guint32)doubleMs;
    }

private:
    guint32     last_ = 0;
    const void* target_ = nullptr;
    int         count_ = 0;
};
