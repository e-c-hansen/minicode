// StartupLog.h — where the time to a usable window goes (PARITY.md R3).
//
// With MINICODE_STARTUP_LOG=1 in the environment, minicode prints a line to
// stderr for each phase of its start, with the time since main began and the
// main thread's CPU time then:
//
//   minicode startup:    41.2 ms  cpu   30.1 ms  gtk: started
//
// The first line also says how long the process ran before main (loading
// its libraries). The phases are the application's startup and command
// line, the settings and keys, each pane of the first window, the session's
// tabs, the first file read and highlighted, the first frames drawn (with
// each frame's layout and paint), the tree's first listing, and whatever
// else starts at launch or later on its own (the browser's web view and its
// first page, the terminal's shell, language servers, git). A line marked
// "once" names the first time only.
//
// Without the variable every call is a test of one flag. The first frame is
// recorded either way, for the GUI checks (StartupSuite.cpp).
//
// Run from a build folder on the desktop, the owner's numbers:
//   MINICODE_STARTUP_LOG=1 ./build/minicode ~/minicode
#pragma once

#include <gtk/gtk.h>

namespace StartupLog {

// First thing in main: the clocks start, and the variable is read.
void begin();
bool on();

// A phase ended now.
void mark(const char* what);
void markf(const char* format, ...) G_GNUC_PRINTF(1, 2);
// The same, only the first time `key` is seen (an event that recurs, such
// as a language server starting, is named once).
void once(const char* key, const char* format, ...) G_GNUC_PRINTF(2, 3);

// Times the first frames `window` draws: each frame's layout and paint, and
// the first one's end as "first frame drawn". The first window only.
void watchFrames(GtkWidget* window);

// Since begin(), in milliseconds: wall time, and the main thread's CPU.
double wallMs();
double cpuMs();

// The first frame's end, or a negative time before it is drawn.
struct FirstFrame {
    double wallMs = -1;     // since begin()
    double cpuMs = -1;      // the main thread's CPU since begin()
    double paintMs = -1;    // that frame's own layout and paint, wall time
};
FirstFrame firstFrame();

}  // namespace StartupLog
