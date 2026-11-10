// MenuBarFocus.h — the keyboard back where it was after a choice from the
// menu bar. GTK 4's menu bar (GtkPopoverMenuBar) gives the keyboard to the
// item whose menu it opens and leaves it there once the menu closes, so
// after View > Theme > Monokai, or any command chosen with the mouse, the
// next keys went to the menu bar and typing did nothing until a click
// (found by the themes checks, PARITY.md 2.11). The Mac's menu bar never
// takes the keyboard. Here, once a menu of the bar has closed and the
// command it ran has not put the keyboard somewhere itself, the keyboard
// goes back to the widget that had it before the menu opened.
#pragma once

#include <gtk/gtk.h>

namespace MenuBarFocus {

// Once per window, after its menu bar is set (the application's): watches
// the window's focus and the bar's menus for the window's life.
void attach(GtkWindow* window);

}  // namespace MenuBarFocus
