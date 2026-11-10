// WhenDrawn.h — work a window puts off until it has drawn its first frame
// (PARITY.md R3).
//
// What need not happen before the window is usable waits for this: a
// language server for the tab on show, for one, is started from the idle
// after the window's first frame, so its fork and its first burst of work
// never hold the frame up. The Mac starts its servers as a document shows;
// here they start a few milliseconds later, once the window is on screen.
#pragma once

#include <gtk/gtk.h>

#include <functional>

// Calls `fn` once the window holding `widget` (or the widget itself, before
// it is in one) has drawn a frame: from an idle after that frame, or from
// the next idle when it has drawn already. `fn` must check for itself that
// what it acts on is still there.
void whenDrawn(GtkWidget* widget, std::function<void()> fn);
