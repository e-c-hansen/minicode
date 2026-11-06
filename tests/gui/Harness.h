// Harness.h — offscreen GUI checks for the macOS app (`make guitest`).
//
// build/guitest is the whole app (every src/*.mm and .cpp, with main.mm's
// main renamed minicode_main) plus the suites in tests/gui/*.mm. It never
// puts a window on screen or activates the app, so it runs while someone
// works at the Mac, and in the macOS CI job:
//
//   - It first re-executes itself with a scratch world under /tmp/mcgui-*:
//     HOME and CFFIXED_USER_HOME, ZDOTDIR (an empty .zshrc and its own
//     history), TMPDIR (short, for Unix socket paths), MINICODE_SETTINGS
//     (language servers off), MINICODE_DEFAULTS_SUITE, MINICODE_BACKUP_DIR,
//     MINICODE_PASTEBOARD, and git's identity with no global config. The
//     folder is deleted at the end (MC_GUI_KEEP=1 keeps it).
//   - Window ordering (makeKeyAndOrderFront:, orderFront:,
//     orderWindow:relativeTo:, ...), activation, the activation policy
//     (always Prohibited), child windows, NSAlert's runModal (answered from
//     MCGuiAnswerAlerts, else the first button), the general pasteboard (a
//     private one) and NSWorkspace's openURL: are swizzled. keyWindow,
//     mainWindow and isKeyWindow report the test window (the newest editor
//     window, or MCGuiSetKeyWindow), since an offscreen window is never key.
//   - minicode_main runs with the scratch project folder as its argument, so
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
// `make guitest` runs every suite; `make guitest SUITES="control"` some.
// Traps worth knowing: everything done in one step lands in one undo group
// (wrap a simulated edit in MCGuiUndoGroup); a key event for the app's key
// monitor must go through [NSApp sendEvent:] with the window's number
// (MCGuiSendKeyToApp), while menu keys work best as CGEvent-made events
// through the window and then the main menu (MCGuiPerformMenuKey).
#import <Cocoa/Cocoa.h>

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
void MCGuiPause(NSTimeInterval seconds);

// ---- checks
BOOL MCGuiCheck(BOOL ok, NSString *what, const char *file, int line);
#define GCHECK(cond) MCGuiCheck((cond) ? YES : NO, @#cond, __FILE__, __LINE__)
// A check with the actual value printed when it fails.
#define GCHECK_EQ_STR(a, b)                                                    \
    MCGuiCheck([(a) ?: @"(nil)" isEqualToString:(b)],                         \
               [NSString stringWithFormat:@"%s == %s (got \"%@\")", #a, #b, (a)], \
               __FILE__, __LINE__)
void MCGuiNote(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);

// ---- the world
NSString *MCGuiScratch(void);               // the scratch root
NSString *MCGuiProject(void);               // the folder the first window shows
NSString *MCGuiProjectFile(NSString *name); // a path in it
NSString *MCGuiBuildDir(void);              // build/, where guitest and minicode-cli are
EditorController *MCGuiController(void);    // the newest editor window's controller
NSArray<EditorController *> *MCGuiControllers(void);
void MCGuiSetKeyWindow(NSWindow *window);   // what keyWindow reports (nil: the newest)
// Answers for the next alerts (NSAlertFirstButtonReturn and so on), in order.
void MCGuiAnswerAlerts(NSArray<NSNumber *> *answers);
NSArray<NSString *> *MCGuiAlertsShown(void); // their message texts, so far
// Writes a file (making folders), UTF-8.
void MCGuiWriteFile(NSString *path, NSString *text);
// A picture of a view as drawn (cacheDisplayInRect:), written as
// <name>.png into $MC_GUI_SNAPSHOTS when that is set, for looking at.
void MCGuiSnapshot(NSView *view, NSString *name);

// Writes the settings file (MINICODE_SETTINGS) and has the app read it now.
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
// Runs `block` as one undo group of its own, as a key press would be.
void MCGuiUndoGroup(NSUndoManager *undo, void (^block)(void));

// ---- processes
// Runs a program (the environment is this process's, plus `env`) without
// blocking the main thread; `done` gets its exit status, stdout and stderr
// on the main thread.
void MCGuiRunProcess(NSArray<NSString *> *argv, NSDictionary<NSString *, NSString *> *env,
                     void (^done)(int status, NSString *out, NSString *err));
