// TerminalFrame.h — the box a terminal's VTE is laid out in, between the
// scrolled window and VTE (docs/notes/session-host.md, "VTE pans").
//
// A pty on the session's host takes the size of the renderer that typed
// last, so this view sometimes shows a grid of another renderer's size
// (Terminal::followPtySize): larger than the view, VTE is laid out at that
// grid and the part on show follows the cursor; smaller, VTE has exactly
// the pty's columns and rows, so plain output wraps where the pty wraps,
// and the rest of the view is left empty. The scrolled window gives a
// scrollable child its own size whatever the child asks for, so VTE cannot
// be grown there without negative margins (which GTK warned about on every
// change); this frame is the scrollable child instead. It passes the
// scrolled window's adjustments and scroll policies through to VTE (so
// the scroll bar, wheel and scrollback are VTE's as before), lays VTE out
// at its own size, or at the size and offset its owner holds it at, clips
// it (overflow hidden), and paints the color it is given where VTE does
// not reach. A click there gives VTE the keyboard.
#pragma once

#include <gtk/gtk.h>

// A frame holding `child` (a VteTerminal), to be set as a scrolled
// window's child.
GtkWidget* terminalFrameNew(GtkWidget* child);
// The child laid out `width` x `height` pixels (its whole allocation, its
// padding included) with its top left corner at (-x, -y): the part from
// (x, y) on is what shows. A width or height of 0 gives the child the
// frame's own size at (0, 0) again.
void terminalFrameHold(GtkWidget* frame, int width, int height, int x, int y);
// The color painted where the child does not reach (the terminal's own
// background, translucent or not, as VTE paints it).
void terminalFrameSetBackground(GtkWidget* frame, const GdkRGBA& color);
// Where the child is laid out, in the frame's coordinates (for checks).
graphene_rect_t terminalFrameChildRect(GtkWidget* frame);
