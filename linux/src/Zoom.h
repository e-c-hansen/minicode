// Zoom.h — the editor's zoom, the terminals' zoom and the word wrap toggle
// in the GTK port (linux/PARITY.md item 1.11), for every window at once,
// over the core's rules (../src/ViewZoom.h); the Mac's are
// src/EditorFont.mm.
//
//   - The editor's text is editor.font-size points when the settings file
//     sets it, else the desktop's own size (gtk-font-name's, which the port
//     has always used), plus the zoom. The line numbers follow the view's
//     font by themselves (Gutter.cpp measures it again). The Markdown
//     preview's sizes (its body, headings, code and tables) scale by the
//     core's previewScale, (13 + zoom) / 13, as the Mac's do.
//   - A terminal's font is terminal.font-size points when the file sets it,
//     else the desktop's size, scaled by VTE's font scale to that plus the
//     terminals' own zoom, which the same keys change while the terminal
//     has the keyboard (CommandsLinux.cpp's Terminal rows).
//   - Both zooms last between runs in $XDG_STATE_HOME/minicode/zoom.json
//     (PARITY.md D11), never in the settings file, and are read once, when
//     the first window opens.
//   - Word wrap is editor.word-wrap unless Alt+Z turned it over since, for
//     the session (ViewZoom::Wrap). A preview, a message or the welcome
//     text always wraps; that is Editor.cpp's.
//
// The stylesheet is the display's, so the editor's size is one rule for
// every window's view (attachEditor marks the view it applies to); each
// window listens for the rest (its tags, its terminal, its wrapping).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>

#include "Settings.h"

namespace Zoom {

// The settings as they are now: reads the state file the first time, and
// sets the editor's rule. Called again whenever the settings file changes.
void applySettings(const Settings& settings);

// The view whose text the editor's zoom sizes (the editor's own, not the
// LaTeX log's or the Source Control panel's, which share its class).
void attachEditor(GtkWidget* view);

// Zoom In (+1), Zoom Out (-1), Reset Zoom (0): false, with nothing changed,
// at a limit (6 or 72 points in all).
bool zoomEditor(int step);
bool zoomTerminal(int step);
double editorZoom();
double terminalZoom();

// The desktop's interface size in points (gtk-font-name), the base when
// the settings file sets no size.
double desktopPoints();
// The editor's source text and a terminal's font now, in points.
double editorPoints();
double terminalPoints();
// What the terminal's font is before its zoom: the setting's or the
// desktop's size, in points.
double terminalBasePoints();
// The Markdown preview's factor.
double previewScale();

// Sizes the Markdown tags of a tag table by previewScale: each tag with a
// size of its own keeps the size it had when first seen as its base.
void scaleTags(GtkTextTagTable* table);

// Whether the editor's source wraps now, and Alt+Z's turn.
bool wordWrap();
void toggleWordWrap();

// Something changed: `what` is "editor", "terminal" or "wrap". Every
// window listens; the id takes the listener off.
using Listener = std::function<void(const std::string& what)>;
int listen(Listener l);
void unlisten(int id);

// $XDG_STATE_HOME/minicode/zoom.json.
std::string statePath();

}  // namespace Zoom
