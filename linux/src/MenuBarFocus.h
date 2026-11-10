// MenuBarFocus.h — the keyboard back where it was after a choice from the
// menu bar. GTK 4's menu bar (GtkPopoverMenuBar) gives the keyboard to the
// item whose menu it opens and leaves it there once the menu closes, so
// after View > Theme > Monokai, or any command chosen with the mouse, the
// next keys went to the menu bar and typing did nothing until a click
// (found by the themes checks, PARITY.md 2.11). The Mac's menu bar never
// takes the keyboard. Here, once a menu of the bar has closed and the
// command it ran has not put the keyboard somewhere itself, the keyboard
// goes back to the widget that had it before the menu opened, or, when a
// refresh made that widget again meanwhile (a Source Control row), to the
// nearest of its ancestors still on show. The same happens when a row of
// a menu no longer on show takes the keyboard: GTK's submenu timer, 80 ms
// after the pointer last moved over a row, is not stopped by the menu
// closing, and after a quick click it fired once the menu had gone.
#pragma once

#include <gtk/gtk.h>

namespace MenuBarFocus {

// Once per window, after its menu bar is set (the application's): watches
// the window's focus and the bar's menus for the window's life.
void attach(GtkWindow* window);

}  // namespace MenuBarFocus
