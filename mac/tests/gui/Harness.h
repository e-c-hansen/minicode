// Harness.h — offscreen GUI checks for the macOS app (`make guitest`).
//
// build/guitest is the whole app (every src/*.mm and .cpp, with main.mm's
// main renamed mutible_main) plus the suites in mac/tests/gui/*.mm. It never
// puts a window on screen or activates the app, so it runs while someone
// works at the Mac, and in the macOS CI job:
//
//   - It first re-executes itself with a scratch world under /tmp/mcgui-*:
//     HOME and CFFIXED_USER_HOME, ZDOTDIR (an empty .zshrc and its own
//     history), TMPDIR (short, for Unix socket paths), MUTIBLE_SETTINGS
//     (language servers off), MUTIBLE_DEFAULTS_SUITE, MUTIBLE_BACKUP_DIR,
//     MUTIBLE_PASTEBOARD, and git's identity with no global config. The
//     folder is deleted at the end (MC_GUI_KEEP=1 keeps it).
//   - Window ordering (makeKeyAndOrderFront:, orderFront:,
//     orderWindow:relativeTo:, ...), activation, the activation policy
//     (always Prohibited), child windows, NSAlert's runModal (answered from
//     MCGuiAnswerAlerts, else the first button), the general pasteboard (a
//     private one) and NSWorkspace's openURL: are swizzled. keyWindow,
//     mainWindow and isKeyWindow report the test window (the newest editor
//     window, or MCGuiSetKeyWindow), since an offscreen window is never key.
//   - mutible_main runs with the scratch project folder as its argument, so
//     applicationDidFinishLaunching: opens the window as it would for a user.
//
// A suite is a function registered with MC_GUI_SUITE that queues steps. The
// runner plays them one at a time from the main run loop, which keeps
// spinning between steps (pty reads, dispatches to the main queue and
// timers all run), and prints each failed check with its file and line.
// The exit status is non-zero when any check failed or a wait timed out.
// At the end the runner also checks that no window of this process was ever
// found on screen (it looks after every step).
//
//   MC_GUI_SUITE(example) {
//       MCGuiStep(@"opens a file", ^{
//           GCHECK([MCGuiController() showFile:MCGuiProjectFile(@"a.txt")]);
//       });
//       MCGuiWait(@"the shell starts", 10, ^BOOL { return term.atPrompt; });
//       MCGuiAsync(@"the CLI answers", 20, ^(MCGuiDone done) {
//           MCGuiRunProcess(@[cli, @"state"], nil, ^(int s, NSString *out, NSString *err) {
//               GCHECK(s == 0);
//               done();
//           });
//       });
//   }
//
// Between suites the runner compares the world with what the suite found
// (editor windows, sheets, terminals, settings.conf, keys.conf, the zooms,
// Show Hidden Files, the tree's columns, a key window left pinned, alert
// answers left unused): a difference is a "LEAK [suite]" line and a failed
// check of the suite that left it, and what the harness can put back (the
// pin, the answers) it puts back, so one suite's leftovers never fail the
// suites after it. settings.conf and keys.conf are the harness's to put
// back between suites, and a terminal left open is only noted: those are
// "LEFT [suite]" lines, which warn. MC_GUI_LEAKS=note prints the lines and
// neither fails nor puts anything back. A "_" suite (a second process) is
// not looked at.
//
// Environment for slower machines (CI's job sets them; see
// .github/workflows/ci.yml): MC_GUI_WAIT_SCALE multiplies every wait's and
// async step's timeout (a wait polls, so more room only delays a failure);
// MC_TIMING_SCALE multiplies every GCHECK_TIME bound, and a time over the
// bound but within the scaled one passes with a "SLOW [suite]" line, so a
// bound being outgrown shows before it fails. MC_GUI_SNAPSHOTS=dir also
// gets a picture of the test window at each of the first ten failures.
//
// `make guitest` runs every suite; `make guitest SUITES="control"` some.
// MC_GUI_SKIP_SUITES="a b" leaves suites out (CI runs the list as two
// processes at once, one naming its half, the other skipping it); a name
// in either list that no suite has fails the run. MC_GUI_SHUFFLE=<seed>
// plays the chosen suites in an order drawn from the seed, the same on
// every machine (the soak workflow's rounds; tests/GuiRetry.h).
// MC_GUI_RETRIES=<n> (CI sets 3): the first stage, which now waits for the
// run rather than replacing itself with it, reads what failed and runs
// each failed suite once more, alone, as a fresh `guitest <suite>` in a new
// scratch world (MC_GUI_RETRY_OF names it there), its output marked
// "    retry| "; passing then, it is a "FLAKY [suite]" line and the run
// passes. More than n failed suites, a crash, the run's limit, or a
// failure outside any suite retries nothing (docs/ci.md, "Retries").
// A suite whose name starts with "_" runs only when named: it is a part
// another suite runs in a second process (MCGuiRunChild), as BackupsSuite
// does to SIGKILL an app and start another on what it left.
// Traps worth knowing: everything done in one step lands in one undo group
// (wrap a simulated edit in MCGuiUndoGroup); a key event for the app's key
// monitor must go through [NSApp sendEvent:] with the window's number
// (MCGuiSendKeyToApp), while menu keys work best as CGEvent-made events
// through the window and then the main menu (MCGuiPerformMenuKey).
// A suite changes an environment variable only with MCSetEnv(name, value
// or nil) from Environment.h, never setenv or unsetenv: workers start git,
// servers and tools from the app's snapshot of the environment, which
// MCSetEnv refreshes, and a raw setenv under a thread copying environ
// corrupted the heap on Linux (core check env:raw-access).
#import <Cocoa/Cocoa.h>
#import "Environment.h"

@class EditorController;

typedef void (^MCGuiDone)(void);

// ---- registering suites
void MCGuiRegisterSuite(const char *name, void (*body)(void));
#define MC_GUI_SUITE(name)                                                     \
    static void mc_gui_suite_##name(void);                                     \
    __attribute__((constructor)) static void mc_gui_register_##name(void) {    \
        MCGuiRegisterSuite(#name, mc_gui_suite_##name);                        \
    }                                                                          \
    static void mc_gui_suite_##name(void)

// ---- steps (queued by the suite, played in order)
void MCGuiStep(NSString *name, void (^block)(void));
// Polls `until` every 50 ms; a check fails if it is not true in `timeout`.
void MCGuiWait(NSString *what, NSTimeInterval timeout, BOOL (^until)(void));
// A step that ends when `done` is called (a check fails after `timeout`).
void MCGuiAsync(NSString *name, NSTimeInterval timeout, void (^block)(MCGuiDone done));
// A fixed wait. Before a positive check use MCGuiWait on what the check
// reads; a pause is for a negative check ("nothing happened in this time")
// or where time itself is the condition (a double-click interval), and
// says which.
void MCGuiPause(NSTimeInterval seconds);
// Waits until `value` has not changed for `quiet` seconds (events still
// arriving, a count still growing), failing after `timeout`.
void MCGuiWaitQuiet(NSString *what, NSTimeInterval quiet, NSTimeInterval timeout,
                    NSInteger (^value)(void));

// ---- checks
BOOL MCGuiCheck(BOOL ok, NSString *what, const char *file, int line);
#define GCHECK(cond) MCGuiCheck((cond) ? YES : NO, @#cond, __FILE__, __LINE__)
// A check with the actual value printed when it fails.
#define GCHECK_EQ_STR(a, b)                                                    \
    MCGuiCheck([(a) ?: @"(nil)" isEqualToString:(b)],                         \
               [NSString stringWithFormat:@"%s == %s (got \"%@\")", #a, #b, (a)], \
               __FILE__, __LINE__)
void MCGuiNote(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);

// ---- timing bounds that hold on a loaded machine
// Bound the CPU time of the thread doing the work (MCGuiThreadCpuMs), not
// wall time: with several agents building at once (load 40 to 100) wall
// time ran three to ten times over, while a regression (a pass over the
// whole file per keystroke) costs CPU and is still caught. MCGuiNowMs is
// wall time, for notes. A pause measured by wall time before a positive
// check is a wait in disguise: wait on the condition instead (MCGuiWait).
double MCGuiThreadCpuMs(void);
double MCGuiNowMs(void);
// MC_SKIP_TIMING=1 passes every timing check (the numbers are still noted).
BOOL MCGuiTimingSkipped(void);
// MC_TIMING_SCALE and MC_GUI_WAIT_SCALE, 1 when unset (see above).
double MCGuiTimingScale(void);
double MCGuiWaitScale(void);
// `ms` (CPU time) under `bound`, the value in the check's text either way.
BOOL MCGuiCheckTime(double ms, double bound, NSString *what, const char *file, int line);
#define GCHECK_TIME(ms, bound, what) MCGuiCheckTime((ms), (bound), (what), __FILE__, __LINE__)

// ---- the world
NSString *MCGuiScratch(void);               // the scratch root
NSString *MCGuiProject(void);               // the folder the first window shows
NSString *MCGuiProjectFile(NSString *name); // a path in it
NSString *MCGuiBuildDir(void);              // build/, where guitest and mutible-cli are
EditorController *MCGuiController(void);    // the newest editor window's controller
NSArray<EditorController *> *MCGuiControllers(void);
void MCGuiSetKeyWindow(NSWindow *window);   // what keyWindow reports (nil: the newest)
// The last window the app asked to make key since MCGuiSetKeyWindow (the
// harness keeps keyWindow as set; this says what the app wanted), or nil.
NSWindow *MCGuiWindowMadeKey(void);
// Answers for the next alerts (NSAlertFirstButtonReturn and so on), in order.
void MCGuiAnswerAlerts(NSArray<NSNumber *> *answers);
NSArray<NSString *> *MCGuiAlertsShown(void); // their message texts, so far
NSString *MCGuiLastAlertInformative(void);   // the last one's informative text
// Those of them begun as sheets (beginSheetModalForWindow:, answered as
// runModal is, the handler a turn later), with the window each was for.
NSArray<NSString *> *MCGuiSheetsShown(void);
// Every URL handed to NSWorkspace's openURL: (swizzled: nothing opens), in
// order, so a check can say a page did not open another app.
NSArray<NSURL *> *MCGuiOpenedURLs(void);
NSWindow *MCGuiLastSheetWindow(void);
// Writes a file (making folders), UTF-8.
void MCGuiWriteFile(NSString *path, NSString *text);
// A picture of a view as drawn (cacheDisplayInRect:), written as
// <name>.png into $MC_GUI_SNAPSHOTS when that is set, for looking at.
void MCGuiSnapshot(NSView *view, NSString *name);

// Writes the settings file (MUTIBLE_SETTINGS) and has the app read it now.
void MCGuiSetSettings(NSString *text);

// ---- a scripted language server (build/fakelsp; tests/fakelsp/fakelsp.cpp
// describes the script). Each name has its own script and log under
// <scratch>/fakelsp. MCGuiFakeLsp writes the script (a JSON-able tree) and
// returns the command line to put in the settings (lsp.python = ...);
// `program`, when given, is the name the server runs under (a link to
// fakelsp named "ruff", so the app takes it for Ruff), and `words` are
// added to the command line. Writing a name's script again changes what
// the running server does from its next message on.
NSString *MCGuiFakeLsp(NSString *name, id script, NSString *program, NSString *words);
// Every message the server named `name` received, in order, parsed; with a
// method, only those.
NSArray<NSDictionary *> *MCGuiFakeLspReceived(NSString *name, NSString *method);
// Empties a server's log.
void MCGuiFakeLspClearLog(NSString *name);

// ---- input
// A key event made the way the hardware's are (CGEvent), offered to the
// window's key equivalents, then the main menu; YES when one took it. The
// characters may be nil (the US layout's for the key code); given, they
// are checked against what the layout types and set on the event only when
// they differ.
BOOL MCGuiPerformMenuKey(NSWindow *window, unsigned short keyCode, NSEventModifierFlags flags,
                         NSString *chars, NSString *charsIgnoringModifiers);
// A key event for the window, through [NSApp sendEvent:] (local key monitors
// see it first).
void MCGuiSendKeyToApp(NSWindow *window, unsigned short keyCode, NSEventModifierFlags flags,
                       NSString *chars, NSString *charsIgnoringModifiers);
// A key press the way the app takes one: the key monitor first
// (MCKeymapHandleKeyDown), then the window's views' key equivalents, then
// the main menu, with a CGEvent-made event. YES when one took it.
BOOL MCGuiPressKey(NSWindow *window, unsigned short keyCode, NSEventModifierFlags flags);
// Runs `block` as one undo group of its own, as a key press would be.
void MCGuiUndoGroup(NSUndoManager *undo, void (^block)(void));

// ---- text geometry
// Where the text of the line holding character `index` is drawn, in the
// text view: its line fragment's used rect, laid out first, which leaves
// out room kept above the line (an agent's note sits there).
NSRect MCGuiLineTextRect(NSTextView *tv, NSUInteger index);
// How many line fragments' text a rect in the text view covers (by their
// used rects, so room above a line is not text).
NSUInteger MCGuiLinesUnder(NSTextView *tv, NSRect rect);

// ---- processes
// Runs a program (the environment is this process's, plus `env`) without
// blocking the main thread; `done` gets its exit status, stdout and stderr
// on the main thread.
void MCGuiRunProcess(NSArray<NSString *> *argv, NSDictionary<NSString *, NSString *> *env,
                     void (^done)(int status, NSString *out, NSString *err));
// Runs this program again as a second app in the same scratch world (home,
// settings, backups, temporary folder), opening `folder`, with only the
// suite named `suite` (a "_" one). Children share a defaults suite of their
// own, so one sees the tab sessions an earlier one left. `done` gets the
// exit status (a signal's number when one killed it) and the output. A nil
// `folder` opens nothing, as a launch from the Dock does (WindowRestoreSuite).
void MCGuiRunChild(NSString *suite, NSString *folder, void (^done)(int status, NSString *out));
