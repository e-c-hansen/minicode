// Harness.h — offscreen GUI checks for the GTK port, the counterpart of the
// Mac's tests/gui/Harness.h.
//
//   meson test -C build --suite gui          every suite
//   ./build/mutible-guitest save tabs       some, with the output as it comes
//   SUITES="save tabs" meson test ...        the same through Meson
//
// build/mutible-guitest is the whole app (every object of the mutible
// target, with main.cpp's main renamed mutible_main) plus the suites named
// in tests/gui/suites.txt. It runs in two stages, in one program:
//
//   - The wrapper (Wrapper.cpp, the program as started) makes a scratch
//     world under the build folder (HOME, the XDG folders, ZDOTDIR, a short
//     TMPDIR and XDG_RUNTIME_DIR under /tmp, the settings file, git's
//     identity with no global config), starts its own Xvfb on a display
//     number the server picked with -displayfd, so runs at the same moment
//     never collide, unsets WAYLAND_DISPLAY, sets GDK_BACKEND=x11 and
//     GTK_A11Y=none, and runs this program again under dbus-run-session. It
//     is a child subreaper, so whatever the app starts stays its descendant;
//     at the end every descendant is stopped by PID, Xvfb last, and the
//     scratch folders are deleted (MC_GUI_KEEP=1 keeps them).
//   - The checks (Harness.cpp, MC_GUI_CHILD set) first refuse any display
//     but that Xvfb: the peer of DISPLAY's socket must be the very process
//     the wrapper started, and it must be an Xvfb. Then mutible_main runs
//     with the scratch project folder as its argument, so the window opens
//     as it would for a user, and the suites are played from the GLib main
//     loop once it is on screen.
//
// A suite is a function registered with MC_GUI_SUITE that queues steps. The
// runner plays them one at a time, and the main loop keeps turning between
// them (X events, the pty, GIO, timers all run). A failed check prints its
// file and line; the exit status is non-zero when any check failed or a wait
// timed out.
//
//   MC_GUI_SUITE(example) {
//       MCGuiStep("opens the file", [] { MCGuiOpenFile(MCGuiProjectFile("a.txt")); });
//       MCGuiWait("it shows", 5, [] { return MCGuiApp().editor->currentPath() != ""; });
//       MCGuiStep("types", [] { MCGuiType("hello"); });
//       MCGuiWait("the text arrives", 5, [] { return MCGuiEditorText() == "hello"; });
//       MCGuiStep("saves", [] { MCGuiKey("ctrl+s"); });
//   }
//
// Adding a suite: a file tests/gui/<Name>Suite.cpp with MC_GUI_SUITE(name),
// and one line in tests/gui/suites.txt (`merge=union` in .gitattributes, so
// branches adding different suites merge cleanly; `make test` fails when a
// suite file is not listed). A suite whose name starts with "_" runs only
// when named: it is a part another suite starts with MCGuiRunChild.
//
// Environment: SUITES or MC_GUI_SUITES, as arguments do; MC_GUI_SKIP_SUITES
// leaves suites out (CI runs the list as two processes at once, one naming
// its half, the other skipping it), and a name in either list that no suite
// has fails the run; MC_GUI_SHUFFLE=<seed> plays the chosen suites in an
// order drawn from the seed, the same on every machine and the Mac's
// harness (the soak workflow's rounds; tests/GuiRetry.h); MC_GUI_IN_ORDER=1
// plays the named suites in the order named instead, to replay a soak
// round's order (its log names it) up to the suite that failed;
// MC_GUI_RETRIES=<n> (CI sets 3) has the wrapper, once the run is over,
// run each failed suite once more, alone, as a fresh run of this program
// (its own world and display; MC_GUI_RETRY_OF names it there), its output
// marked "    retry| ": passing then, it is a "FLAKY [suite]" line and the
// run passes; more than n failed suites, a crash, the run's limit or a
// failure outside any suite retries nothing (docs/ci.md, "Retries");
// MC_GUI_KEEP=1 keeps
// the scratch folders; MC_GUI_VERBOSE=1 names each step; MC_GUI_SNAPSHOTS=dir
// is where MCGuiSnapshot writes, and where a failed check leaves a picture of
// the window on top (fail-<n>-<suite>.png, the first ten); MC_SKIP_TIMING=1
// passes every timing bound;
// MC_GUI_TOTAL_SECONDS is the limit on the whole run (20 minutes) and
// MC_GUI_STEP_SECONDS the limit on one step (a minute, on top of a wait's own
// timeout), which also catches a step that never returns to the main loop.
// For slower machines (CI's job sets them): MC_GUI_WAIT_SCALE multiplies
// every wait's and async step's timeout (a wait polls, so more room only
// delays a failure); MC_TIMING_SCALE multiplies every GCHECK_TIME bound,
// and a time over the bound but within the scaled one passes with a
// "SLOW [suite]" line, so a bound being outgrown shows before it fails.
//
// Between suites the runner compares the world with what the suite found
// (app windows, another window such as a question left on top, terminal
// tabs, terminals marked waiting, settings.conf, keys.conf, a key window
// left pinned): a difference is a "LEAK [suite]" line and a failed check of
// the suite that left it, and the pin and the marks are put back and a
// window left on top is closed (a modal question held the app's keys), so
// one suite's leftovers never fail the suites after it. The two files are the harness's to put back between suites, and a
// terminal tab left open is only noted: "LEFT [suite]" lines, which warn.
// MC_GUI_LEAKS=note prints the lines and neither fails nor puts anything
// back. A "_" suite (a second process) is not looked at. (The
// Mac's harness does the same; HANDOFF.md has the GTK cascades that led to
// it: a terminal's width, the editor's font, a row menu left open.)
//
// Traps worth knowing, some from the Mac's harness and some the port's own:
//   - Before a positive check after input, wait on what the check reads
//     (MCGuiWait). A key or click is only on its way when MCGuiKey returns.
//   - There is no window manager. Every window opens at the top left, a
//     later one covers the others, and nothing gives one the focus but the
//     harness (MCGuiSetKeyWindow; the newest app window by default).
//   - An alert (GtkAlertDialog) is a window of its own: MCGuiTopWindow finds
//     it, MCGuiSetKeyWindow gives it the keyboard, and MCGuiClick presses
//     its buttons (MCGuiFindButton).
//   - A hook that calls the open path directly proves the wiring, not the
//     click (HANDOFF.md): prefer real input where the input is the point.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "AppSeam.h"
#include "EnvLock.h"

using MCGuiDone = std::function<void()>;

// ---- registering suites
void MCGuiRegisterSuite(const char* name, void (*body)());
#define MC_GUI_SUITE(name)                                                     \
    static void mc_gui_suite_##name();                                         \
    __attribute__((constructor)) static void mc_gui_register_##name() {        \
        MCGuiRegisterSuite(#name, mc_gui_suite_##name);                        \
    }                                                                          \
    static void mc_gui_suite_##name()

// ---- steps (queued by the suite, played in order)
void MCGuiStep(const std::string& name, std::function<void()> block);
// Polls `until` every 50 ms; a check fails if it is not true in `timeout`.
void MCGuiWait(const std::string& what, double timeout, std::function<bool()> until);
// A step that ends when `done` is called (a check fails after `timeout`).
void MCGuiAsync(const std::string& name, double timeout, std::function<void(MCGuiDone)> block);
// A fixed wait. Before a positive check use MCGuiWait on what the check
// reads; a pause is for a negative check ("nothing happened in this time")
// or where time itself is the condition (a double-click interval), and says
// which.
void MCGuiPause(double seconds);
// Waits until `value` has not changed for `quiet` seconds (events still
// arriving, a count still growing), failing after `timeout`.
void MCGuiWaitQuiet(const std::string& what, double quiet, double timeout,
                    std::function<long()> value);

// ---- checks
bool MCGuiCheck(bool ok, const std::string& what, const char* file, int line);
#define GCHECK(cond) MCGuiCheck((cond) ? true : false, #cond, __FILE__, __LINE__)
// A check with the actual value printed when it fails.
bool MCGuiCheckEqual(const std::string& got, const std::string& want, const char* a, const char* b,
                     const char* file, int line);
#define GCHECK_EQ_STR(a, b) MCGuiCheckEqual((a), (b), #a, #b, __FILE__, __LINE__)
void MCGuiNote(const char* format, ...) G_GNUC_PRINTF(1, 2);
// GLib's log watched: from MCGuiLogWatchStart on, every warning and
// critical any library logs (GTK's "Gtk-WARNING" lines among them) is kept
// as well as printed, as "<domain>-WARNING: <message>" or "-CRITICAL",
// until MCGuiLogWatchStop returns them. The harness is the program, so it
// sets GLib's one log writer (g_log_set_writer_func), at the first start;
// it hands every message on to GLib's default writer.
void MCGuiLogWatchStart();
std::vector<std::string> MCGuiLogWatchStop();

// ---- timing bounds that hold on a loaded machine
// Bound the CPU time of the thread doing the work (MCGuiThreadCpuMs), not
// wall time: with several agents building at once wall time ran three to ten
// times over, while a regression (a pass over the whole file per keystroke)
// costs CPU and is still caught. MCGuiNowMs is wall time, for notes.
double MCGuiThreadCpuMs();
double MCGuiNowMs();
bool MCGuiTimingSkipped();   // MC_SKIP_TIMING=1 passes every bound
double MCGuiTimingScale();   // MC_TIMING_SCALE, 1 when unset (see above)
double MCGuiWaitScale();     // MC_GUI_WAIT_SCALE, the same
// `ms` (CPU time) under `bound`, the value in the check's text either way.
bool MCGuiCheckTime(double ms, double bound, const std::string& what, const char* file, int line);
#define GCHECK_TIME(ms, bound, what) MCGuiCheckTime((ms), (bound), (what), __FILE__, __LINE__)

// ---- the world
std::string MCGuiScratch();                              // the scratch root
std::string MCGuiProject();                              // the folder the first window shows
std::string MCGuiProjectFile(const std::string& name);   // a path in it
std::string MCGuiBuildDir();                             // where mutible-guitest is
void MCGuiWriteFile(const std::string& path, const std::string& text);   // makes folders
std::string MCGuiReadFile(const std::string& path);      // "" when unreadable
bool MCGuiFileExists(const std::string& path);
void MCGuiSetSettings(const std::string& text);          // writes the file, has the app read it
void MCGuiSetKeys(const std::string& text);              // keys.conf ($MUTIBLE_KEYS), the same
// The text of the editor's buffer (the newest window's).
std::string MCGuiEditorText();
// A picture of a widget as drawn, written as <name>.png into
// $MC_GUI_SNAPSHOTS when that is set, for looking at.
void MCGuiSnapshot(GtkWidget* widget, const std::string& name);
bool MCGuiPixel(GtkWidget* widget, int x, int y, int rgba[4]);

// ---- finding widgets
// Whether a widget is on screen where a click would reach it: mapped, laid
// out with a size, and what GTK picks at its center (not clipped away, not
// covered). A list view maps a new row's widgets the moment the row is
// added and places them at its next layout; a click aimed at one in between
// went to the window's top left corner (the File menu), which made the save
// suite's first click flaky once the tree's listings came from a worker
// (R1). A row scrolled just out of a list stays mapped and placed, and a
// click at it lands on whatever is drawn there.
bool MCGuiOnScreen(GtkWidget* widget);
// The first label under `root` showing exactly `text`, or null. Only
// widgets on screen count, so a list's recycled rows off screen, and rows
// not laid out yet, are not found.
GtkWidget* MCGuiFindLabel(GtkWidget* root, const std::string& text);
// The first button under `root` labelled `text`, or null.
GtkWidget* MCGuiFindButton(GtkWidget* root, const std::string& text);
// The newest mapped toplevel of this process, the app's or an alert's.
GtkWindow* MCGuiTopWindow();
// The newest window's tree row for `path`, its name's label, on screen, or
// null. The row is known by its path (FileTree keeps it on the label), so a
// file of the same name in a folder an earlier suite opened (watch/notes.txt
// beside notes.txt) is never taken for it.
GtkWidget* MCGuiTreeRow(const std::string& path);
// The same in another window's tree (a suite's second window on a folder
// of its own).
GtkWidget* MCGuiTreeRow(FileTree* tree, const std::string& path);
// Queues what a click on a tree row needs after other suites: a full run
// leaves the project folder with a hundred entries and folders open, more
// than the tree shows at once, and its list view makes rows only for what is
// on screen. The row is revealed (FileTree::revealPath: its folders opened,
// scrolled to, selected), again while it is not there yet (a file just
// written arrives from the folder's monitor), and waited for until it stands
// still (a double-click whose second press lands on the next row is two
// single clicks).
void MCGuiBringTreeRow(const std::string& path, double timeout = 10);
// Queues hiding the newest window's browser pane, when an earlier suite left
// it shown (the pane takes the editor's width: a PDF's pages narrower, the
// keyboard maybe in the page), and a wait until it is hidden.
void MCGuiHideBrowserPane();
// Puts every window's Find in Folder back as a new window has it: the query
// empty (first, so that nothing below starts a search), no files to include
// or exclude, Match Case, Whole Word and Regular Expression off, and the
// scope the window's folder. The scope, the files and the options outlive
// the window's closing, so a suite that searched calls it before it ends
// (else the harness does, with a LEFT line); one about to search calls it
// once the window is up, then sets its own scope. A files box it empties
// searches again 350 ms later, with the query in the box by then.
void MCGuiResetFolderSearch();
// Queues taking back what a suite typed at the newest window's shell prompt
// and left there (Run in Terminal and Open in Terminal type a command
// without Return): Ctrl+U sent to the shell, then, unless `typed` is empty,
// a wait until the terminal's last line no longer shows it. A line left
// there joins the next suite's typing: find_bar's "stty raw ..." became
// part of install_hints' "uv tool install ..." and never ran.
void MCGuiClearTerminalLine(const std::string& typed);
// Whether a GtkTextView has laid out every line of the buffer it shows.
// After a buffer comes on show GTK lays it out in idle slices, from the
// top, for seconds when it is big (longer on a loaded machine), and until
// then a line it has not reached has no height: a scroll to a far line
// lands short of it and stays there (GTK keeps the top line still as the
// heights come), and gtk_text_view_scroll_to_mark waits, doing nothing,
// until the layout is complete. The last line is the last to get a height,
// unless something scrolled to the end or asked where it is (GTK lays out
// the lines around a scroll's target at once); asking for its height lays
// nothing out.
bool MCGuiTextLaidOut(GtkWidget* textView);

// ---- input: real X events from XTest (Input.cpp)
// Keys go to the key window: MCGuiSetKeyWindow's, else the newest app window.
// Each call gives it the X focus first, as a window manager would.
void MCGuiSetKeyWindow(GtkWindow* window);   // null: the newest
GtkWindow* MCGuiKeyWindow();
GtkWindow* MCGuiPinnedKeyWindow();   // MCGuiSetKeyWindow's, while still mapped; else null
void MCGuiFocusKeyWindow();
// Another program takes the keyboard, as switching apps with the mouse
// does: a click where no window of the app is, then the X focus to none of
// them, so GTK's is-active goes false (Input.cpp says what an autohide
// popover's grab makes of it). The next key, or MCGuiFocusKeyWindow, gives
// it back.
void MCGuiFocusElsewhere();
// A chord in the registry's notation (src/Commands.h): "ctrl+s",
// "ctrl+shift+p", "alt+shift+f", "f2", "enter", "ctrl+`". Modifiers are held
// around the key as a finger would. A key that needs Shift on the layout
// ("ctrl+>") gets it without being asked.
void MCGuiKey(const std::string& chord);
// Types text as key presses. Characters the layout lacks are put on spare
// key codes first ("é", "→"); "\n" is Return and "\t" Tab.
void MCGuiType(const std::string& utf8);
// Pointer, at a point in `widget`'s coordinates (center by default). A
// widget that is not on screen (MCGuiOnScreen) is a failed check, never a
// click at the window's corner.
void MCGuiMoveTo(GtkWidget* widget, double x = -1, double y = -1);
void MCGuiClick(GtkWidget* widget, double x = -1, double y = -1, int button = 1);
// A click on a widget that may not be laid out yet: a bar or a question
// just put up has its buttons a frame or so later (#96, #97: a click at a
// 0x0 button, or before the bar's notice was mapped, reached nothing). This
// queues a wait until `find` returns a widget on screen (MCGuiOnScreen),
// then a step that finds it again and clicks it. When the wait times out,
// the click step fails naming `what` and why it is not on screen ("no
// widget", "a GtkButton, 0x0", what covers it). Prefer it to MCGuiClick
// for anything that appeared in the step before.
void MCGuiClickWhenReady(const std::string& what, std::function<GtkWidget*()> find,
                         double timeout = 10);
// `count` clicks at one point as a hand makes them: the first now, each next
// 60 ms of the X server's time after the one before, while the app runs on
// (well within GTK's double-click time); the next step waits until they
// are in.
void MCGuiClicks(GtkWidget* widget, double x, double y, int count);
void MCGuiDoubleClick(GtkWidget* widget, double x = -1, double y = -1);   // two of them
// A button held down on `widget`, and let go later, wherever the pointer is
// then: what happens in between (a listing moving rows under it) is the
// check's.
void MCGuiPress(GtkWidget* widget, double x = -1, double y = -1, int button = 1);
void MCGuiRelease(int button = 1);
// A click with modifiers held around it, "ctrl", "shift", "alt" or several
// ("ctrl+shift"), as a hand holds them.
void MCGuiClickWith(const std::string& modifiers, GtkWidget* widget, double x = -1,
                    double y = -1, int button = 1);
// Press at one point of `widget`, release at another, moving between over a
// few turns of the main loop; the next step waits until it is over.
void MCGuiDrag(GtkWidget* widget, double x0, double y0, double x1, double y1);
// A double-click whose second press drags: a click, then the drag pressed
// where it was a hand's moment later (MCGuiClicks).
void MCGuiDoubleClickDrag(GtkWidget* widget, double x0, double y0, double x1, double y1);
// The same with modifiers held throughout ("alt", "shift+alt", "" for
// none) and any button (2 the middle one): a column dragged out with
// Shift+Alt or the middle button (Carets.h).
void MCGuiDragWith(const std::string& modifiers, GtkWidget* widget, double x0, double y0,
                   double x1, double y1, int button = 1);
// The same through several points in turn, pressed at the first and let go
// at the last: a hand that goes to an edge and comes back in one drag.
void MCGuiDragThrough(GtkWidget* widget, const std::vector<std::pair<double, double>>& points);
bool MCGuiInputPending();   // a drag still being played
// Wheel clicks: positive scrolls down, negative up.
void MCGuiScroll(GtkWidget* widget, int clicks);

// ---- processes
// Runs a program (the environment is this process's, plus `env`) in the
// project folder (or `cwd`) without blocking the main loop; `done` gets its
// exit status (128 + the signal if one ended it), stdout, and stderr.
void MCGuiRunProcess(const std::vector<std::string>& argv,
                     const std::map<std::string, std::string>& env,
                     std::function<void(int status, std::string out, std::string err)> done,
                     const std::string& cwd = "");
// Runs this program again as a second app in the same scratch world and on
// the same display, on a private session bus of its own, opening `folder`,
// with only the suite named `suite` (a "_" one); an empty `folder` launches
// it with no path from the home folder, as the dock does. `done` gets the exit status
// and the output. A real SIGKILL or crash in the child is safe to test: it
// is reported in the status and nothing else is hit.
void MCGuiRunChild(const std::string& suite, const std::string& folder,
                   std::function<void(int status, std::string out)> done);
// Another process's windows on this display (Input.cpp, by _NET_WM_PID):
// a program the check started that is not this app, such as the real
// build/mutible the detach suite runs.
struct MCGuiOtherWindow {
    unsigned long id = 0;
    bool viewable = false;   // mapped, and every parent mapped
    std::string title;       // _NET_WM_NAME
};
std::vector<MCGuiOtherWindow> MCGuiWindowsOfProcess(int pid);
