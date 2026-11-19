// Harness.mm — see Harness.h.
#import "Harness.h"
#import "EditorController.h"
#import "SessionStore.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#import "AppSettings.h"
#import "ControlServer.h"
#import "TreeDetails.h"   // MCAppDefaults
#import "SessionLink.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#import "Keymap.h"
#include "../../../tests/GuiRetry.h"

// The panel's close without the question (TerminalPanel.mm), for the
// terminals a suite left open: they go between suites, unasked.
@interface MCTerminalPanel (HarnessPutBack)
- (BOOL)removeTerminal:(TerminalView *)terminal;
@end

int mutible_main(int argc, const char *argv[]);   // mac/src/main.mm, renamed

// ------------------------------------------------------------- registry
namespace {
struct Suite { std::string name; void (*body)(void); };
std::vector<Suite> &suites() {
    static std::vector<Suite> s;
    return s;
}
}  // namespace

void MCGuiRegisterSuite(const char *name, void (*body)(void)) {
    suites().push_back({name, body});
}

// ---------------------------------------------------------------- items
typedef NS_ENUM(NSInteger, MCGuiKind) { MCGuiKindStep, MCGuiKindWait, MCGuiKindAsync, MCGuiKindSuite };

@interface MCGuiItem : NSObject
@property(nonatomic) MCGuiKind kind;
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) void (^block)(void);
@property(nonatomic, copy) BOOL (^until)(void);
@property(nonatomic, copy) void (^async)(MCGuiDone);
@property(nonatomic) NSTimeInterval timeout;
@end
@implementation MCGuiItem
@end

static NSMutableArray<MCGuiItem *> *gItems;
static int gPass, gFail, gSlow, gLeaks;
static int gSuitePass, gSuiteFail;
static NSString *gSuite;
static NSString *gStep;
static NSMutableArray<NSNumber *> *gAnswers;
static NSMutableArray<NSString *> *gAlerts;
static NSMutableArray<NSString *> *gSheets;
static NSString *gLastInformative;
static __weak NSWindow *gLastSheetWindow;
static __weak NSWindow *gKeyWindow;
// What failed, by suite, for the first stage's retry (tests/GuiRetry.h);
// gOutside while the end's checks or the run's own limit fail.
static GuiRetry::Results gResults;
static BOOL gOutside;

static void Enqueue(MCGuiItem *item) {
    if (!gItems) gItems = [NSMutableArray array];
    [gItems addObject:item];
}

static NSString *Env(NSString *name) { return NSProcessInfo.processInfo.environment[name]; }

// A factor of at least 1 from the environment (CI sets these; see Harness.h).
static double Factor(NSString *name) {
    const double v = Env(name).doubleValue;
    return v > 1 ? v : 1;
}

double MCGuiWaitScale(void) { return Factor(@"MC_GUI_WAIT_SCALE"); }
double MCGuiTimingScale(void) { return Factor(@"MC_TIMING_SCALE"); }

void MCGuiStep(NSString *name, void (^block)(void)) {
    MCGuiItem *i = [MCGuiItem new];
    i.kind = MCGuiKindStep;
    i.name = name;
    i.block = block;
    Enqueue(i);
}

void MCGuiWait(NSString *what, NSTimeInterval timeout, BOOL (^until)(void)) {
    MCGuiItem *i = [MCGuiItem new];
    i.kind = MCGuiKindWait;
    i.name = what;
    i.until = until;
    i.timeout = timeout * MCGuiWaitScale();
    Enqueue(i);
}

void MCGuiAsync(NSString *name, NSTimeInterval timeout, void (^block)(MCGuiDone)) {
    MCGuiItem *i = [MCGuiItem new];
    i.kind = MCGuiKindAsync;
    i.name = name;
    i.async = block;
    i.timeout = timeout * MCGuiWaitScale();
    Enqueue(i);
}

void MCGuiPause(NSTimeInterval seconds) {
    // Timed from when the pause is reached, not from when the suite queued
    // it (every pause used to end at once in a suite running longer).
    __block NSDate *until = nil;
    MCGuiItem *i = [MCGuiItem new];
    i.kind = MCGuiKindWait;
    i.name = [NSString stringWithFormat:@"a pause of %.1f s", seconds];
    i.until = ^BOOL {
        if (!until) until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        return [until timeIntervalSinceNow] <= 0;
    };
    i.timeout = seconds + 5;
    Enqueue(i);
}

void MCGuiWaitQuiet(NSString *what, NSTimeInterval quiet, NSTimeInterval timeout,
                    NSInteger (^value)(void)) {
    __block NSInteger last = 0;
    __block NSDate *since = nil;
    MCGuiItem *i = [MCGuiItem new];
    i.kind = MCGuiKindWait;
    i.name = what;
    i.until = ^BOOL {
        const NSInteger v = value();
        if (!since || v != last) {
            last = v;
            since = [NSDate date];
            return NO;
        }
        return -since.timeIntervalSinceNow >= quiet;
    };
    i.timeout = timeout * MCGuiWaitScale();
    Enqueue(i);
}

static NSWindow *TestKeyWindow(void);

BOOL MCGuiCheck(BOOL ok, NSString *what, const char *file, int line) {
    if (ok) {
        gPass++;
        gSuitePass++;
        gResults.passed++;
    } else {
        gFail++;
        gSuiteFail++;
        const char *slash = strrchr(file, '/');
        std::printf("  FAIL [%s] %s:%d  %s  (in: %s)\n", gSuite.UTF8String ?: "",
                    slash ? slash + 1 : file, line, what.UTF8String, gStep.UTF8String ?: "");
        char text[1024];
        snprintf(text, sizeof text, "%s:%d  %s  (in: %s)", slash ? slash + 1 : file, line,
                 what.UTF8String, gStep.UTF8String ?: "");
        GuiRetry::noteFailure(gResults, gOutside ? "" : (gSuite.UTF8String ?: ""), text);
        std::fflush(stdout);
        // A picture of the test window as it was (CI keeps them when the job
        // fails), the first few only: a run that goes wrong early can fail
        // every check after.
        NSString *dir = Env(@"MC_GUI_SNAPSHOTS");
        if (dir.length && gFail <= 10 && NSThread.isMainThread) {
            NSString *name = [NSString stringWithFormat:@"fail-%d-%@", gFail, gSuite ?: @"start"];
            @try {
                MCGuiSnapshot(TestKeyWindow().contentView, name);
                std::printf("    a picture of the window: %s/%s.png\n", dir.UTF8String, name.UTF8String);
            } @catch (NSException *e) {
                std::printf("    no picture: %s\n", e.reason.UTF8String ?: "");
            }
            std::fflush(stdout);
        }
    }
    return ok;
}

void MCGuiNote(NSString *format, ...) {
    va_list ap;
    va_start(ap, format);
    NSString *s = [[NSString alloc] initWithFormat:format arguments:ap];
    va_end(ap);
    std::printf("    %s\n", s.UTF8String);
    std::fflush(stdout);
}

double MCGuiThreadCpuMs(void) {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

double MCGuiNowMs(void) {
    timespec ts{};
    clock_gettime(CLOCK_UPTIME_RAW, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

BOOL MCGuiTimingSkipped(void) {
    NSString *s = NSProcessInfo.processInfo.environment[@"MC_SKIP_TIMING"];
    return s.length && ![s isEqualToString:@"0"];
}

// Every timing check's share of its bound, for the end of the run's list of
// those closest to failing.
namespace {
struct Timed {
    double ratio;
    std::string line;
};
std::vector<Timed> gTimed;
}  // namespace

static void PrintNearestBounds(void) {
    if (gTimed.empty()) return;
    std::sort(gTimed.begin(), gTimed.end(), [](const Timed &a, const Timed &b) { return a.ratio > b.ratio; });
    std::printf("Timing checks closest to their bounds (CPU time over the bound set on a Mac):\n");
    for (size_t i = 0; i < gTimed.size() && i < 10; i++) std::printf("%s\n", gTimed[i].line.c_str());
}

BOOL MCGuiCheckTime(double ms, double bound, NSString *what, const char *file, int line) {
    if (bound > 0) {
        const char *slash = strrchr(file, '/');
        char buf[512];
        snprintf(buf, sizeof buf, "  NEAR [%s] %s:%d  %s: %.0f%% (%.2f of %.0f ms)", gSuite.UTF8String ?: "",
                 slash ? slash + 1 : file, line, what.UTF8String, 100 * ms / bound, ms, bound);
        gTimed.push_back({ms / bound, buf});
    }
    const double scale = MCGuiTimingScale();
    if (!MCGuiTimingSkipped() && ms >= bound && ms < bound * scale) {
        // Over the bound set on a Mac, within the room this runner is given:
        // a pass, said loudly, so a bound that is being outgrown shows in
        // the job's summary before it fails.
        gSlow++;
        const char *slash = strrchr(file, '/');
        std::printf("  SLOW [%s] %s:%d  %s: %.2f ms of CPU, over its bound of %.0f "
                    "(passed within this runner's x%.2g)\n",
                    gSuite.UTF8String ?: "", slash ? slash + 1 : file, line, what.UTF8String, ms,
                    bound, scale);
        std::fflush(stdout);
    }
    NSString *text = scale > 1
        ? [NSString stringWithFormat:@"%@ under %.0f ms of CPU, x%.2g on this runner (%.2f)", what,
                                     bound, scale, ms]
        : [NSString stringWithFormat:@"%@ under %.0f ms of CPU (%.2f)", what, bound, ms];
    return MCGuiCheck(MCGuiTimingSkipped() || ms < bound * scale, text, file, line);
}

// ---------------------------------------------------------------- world
NSString *MCGuiScratch(void) { return Env(@"MC_GUI_SCRATCH"); }
NSString *MCGuiProject(void) { return [MCGuiScratch() stringByAppendingPathComponent:@"project"]; }
NSString *MCGuiProjectFile(NSString *name) { return [MCGuiProject() stringByAppendingPathComponent:name]; }

NSString *MCGuiBuildDir(void) {
    char buf[PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return @"build";
    char real[PATH_MAX];
    if (!realpath(buf, real)) return @"build";
    return [NSString stringWithUTF8String:real].stringByDeletingLastPathComponent;
}

NSArray<EditorController *> *MCGuiControllers(void) {
    id delegate = NSApp.delegate;
    NSArray *list = [delegate valueForKey:@"controllers"];
    return [list isKindOfClass:NSArray.class] ? list : @[];
}

EditorController *MCGuiController(void) { return MCGuiControllers().lastObject; }

static NSWindow *TestKeyWindow(void) {
    NSWindow *w = gKeyWindow;
    return w ?: MCGuiController().window;
}

void MCGuiSetKeyWindow(NSWindow *window) { gKeyWindow = window; }

void MCGuiAnswerAlerts(NSArray<NSNumber *> *answers) {
    if (!gAnswers) gAnswers = [NSMutableArray array];
    [gAnswers addObjectsFromArray:answers];
}
NSArray<NSString *> *MCGuiAlertsShown(void) { return [gAlerts copy] ?: @[]; }
NSArray<NSString *> *MCGuiSheetsShown(void) { return [gSheets copy] ?: @[]; }
NSString *MCGuiLastAlertInformative(void) { return gLastInformative ?: @""; }
NSWindow *MCGuiLastSheetWindow(void) { return gLastSheetWindow; }

void MCGuiWriteFile(NSString *path, NSString *text) {
    [[NSFileManager defaultManager] createDirectoryAtPath:path.stringByDeletingLastPathComponent
                              withIntermediateDirectories:YES attributes:nil error:nil];
    [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

void MCGuiSetSettings(NSString *text) {
    MCGuiWriteFile(Env(@"MUTIBLE_SETTINGS"), text);
    [[AppSettings shared] reload];
}

static NSString *FakeLspDir(void) { return [MCGuiScratch() stringByAppendingPathComponent:@"fakelsp"]; }

NSString *MCGuiFakeLsp(NSString *name, id script, NSString *program, NSString *words) {
    NSString *dir = FakeLspDir();
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *scriptPath = [dir stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"json"]];
    NSString *logPath = [dir stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"log"]];
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:script options:0 error:&error];
    if (!json) MCGuiCheck(NO, [NSString stringWithFormat:@"the %@ script is JSON: %@", name, error],
                          __FILE__, __LINE__);
    // Written beside and renamed, so the server never reads half a script.
    NSString *tmp = [scriptPath stringByAppendingString:@".tmp"];
    [json writeToFile:tmp atomically:NO];
    rename(tmp.fileSystemRepresentation, scriptPath.fileSystemRepresentation);
    NSString *exe = [MCGuiBuildDir() stringByAppendingPathComponent:@"fakelsp"];
    if (![fm isExecutableFileAtPath:exe])
        MCGuiCheck(NO, @"build/fakelsp is built (make fakelsp)", __FILE__, __LINE__);
    if (program.length) {
        NSString *bin = [dir stringByAppendingPathComponent:[@"bin-" stringByAppendingString:name]];
        [fm createDirectoryAtPath:bin withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *link = [bin stringByAppendingPathComponent:program];
        [fm removeItemAtPath:link error:nil];
        [fm createSymbolicLinkAtPath:link withDestinationPath:exe error:nil];
        exe = link;
    }
    return [NSString stringWithFormat:@"%@%@%@ --script %@ --log %@", exe, words.length ? @" " : @"",
                                      words ?: @"", scriptPath, logPath];
}

NSArray<NSDictionary *> *MCGuiFakeLspReceived(NSString *name, NSString *method) {
    NSString *log = [FakeLspDir() stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"log"]];
    NSString *text = [NSString stringWithContentsOfFile:log encoding:NSUTF8StringEncoding error:nil];
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        if (!line.length) continue;
        id m = [NSJSONSerialization JSONObjectWithData:[line dataUsingEncoding:NSUTF8StringEncoding]
                                               options:0 error:nil];
        if (![m isKindOfClass:NSDictionary.class]) continue;
        if (method && ![m[@"method"] isEqual:method]) continue;
        [out addObject:m];
    }
    return out;
}

void MCGuiFakeLspClearLog(NSString *name) {
    NSString *log = [FakeLspDir() stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"log"]];
    truncate(log.fileSystemRepresentation, 0);
}

void MCGuiSnapshot(NSView *view, NSString *name) {
    NSString *dir = Env(@"MC_GUI_SNAPSHOTS");
    if (!dir.length || !view || NSIsEmptyRect(view.bounds)) return;
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:[dir stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"png"]]
          atomically:YES];
}

// ------------------------------------------------------- what a suite leaves
// A suite starts from what the one before left, so what one leaves behind
// fails later suites that never touched it (2026-11-14: daily_work left the
// key window pinned to its own window, and git_gutter, git_panel,
// git_actions, git_verbs and git_sync failed on CI). The runner looks at the
// world as each suite starts and again as it ends, and a difference is a
// LEAK line naming the suite, a failed check of that suite, and put back
// where the harness can put it back, so the suites after it start clean.
// MC_GUI_LEAKS=note prints the lines without failing or putting anything
// back.
static NSString *KeysPath(void) {
    NSString *env = Env(@"MUTIBLE_KEYS");
    return env.length ? env.stringByExpandingTildeInPath
                      : [NSHomeDirectory() stringByAppendingPathComponent:@".config/mutible/keys.conf"];
}

static NSString *FileText(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

static NSString *Short(NSString *s) {
    if (!s) return @"(none)";
    NSString *one = [[s stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"]
        stringByReplacingOccurrencesOfString:@"\t" withString:@"\\t"];
    return one.length > 120 ? [[one substringToIndex:120] stringByAppendingString:@"..."] : one;
}

namespace {
struct World {
    NSInteger windows = 0, terminals = 0, sheets = 0;
    NSString *settings, *keys;
    double editorZoom = 0, terminalZoom = 0;
    BOOL showHidden = NO;
    NSInteger treeColumns = 0;
    NSString *browser;   // each window's browser pane, and its agent page
    NSHashTable *terminalViews;   // the terminals themselves, held weakly
};
World gWorldAtStart;
}  // namespace

static World LookAtWorld(void) {
    World w;
    NSArray<EditorController *> *cs = MCGuiControllers();
    w.windows = (NSInteger)cs.count;
    NSMutableArray<NSString *> *browsers = [NSMutableArray array];
    w.terminalViews = [NSHashTable weakObjectsHashTable];
    for (EditorController *c in cs) {
        if (c.window.attachedSheet) w.sheets++;
        @try {
            const BOOL shown = [[c valueForKey:@"browserVisible"] boolValue];
            id b = [c valueForKey:@"browser"];
            const BOOL agent = b && [[b valueForKey:@"hasAgentPage"] boolValue];
            const BOOL agentShown = agent && [[b valueForKey:@"agentPageShown"] boolValue];
            if (shown || agent)
                [browsers addObject:[NSString stringWithFormat:@"%@: browser %@%@",
                                     c.rootPath.lastPathComponent ?: @"?", shown ? @"shown" : @"hidden",
                                     agentShown ? @", the agent page on show"
                                     : agent ? @", an agent page behind" : @""]];
        } @catch (NSException *) {
        }
        @try {
            id panel = [c valueForKey:@"terminalPanel"];
            NSArray *list = [panel valueForKey:@"terminals"];
            if ([list isKindOfClass:NSArray.class]) {
                w.terminals += (NSInteger)list.count;
                for (id t in list) [w.terminalViews addObject:t];
            }
        } @catch (NSException *) {
        }
    }
    w.browser = browsers.count ? [browsers componentsJoinedByString:@"; "] : @"none shown";
    w.settings = FileText(Env(@"MUTIBLE_SETTINGS"));
    w.keys = FileText(KeysPath());
    NSUserDefaults *d = MCAppDefaults();
    w.editorZoom = [d doubleForKey:@"MCEditorZoom"];
    // Read without making it: MCTerminalZoom() writes the key when missing.
    w.terminalZoom = [d objectForKey:@"MCTerminalZoom"] ? [d doubleForKey:@"MCTerminalZoom"]
                                                        : w.editorZoom;
    w.showHidden = [d boolForKey:@"MCShowHiddenFiles"];
    w.treeColumns = [d integerForKey:@"MCTreeColumns"];
    return w;
}

static BOOL LeaksFail(void) { return ![Env(@"MC_GUI_LEAKS") isEqualToString:@"note"]; }
static NSUInteger gAlertsAtStart;

// A LEAK fails the suite (what a later suite trips over: a pin, answers, a
// window, a zoom); a LEFT line only warns (a file the harness puts back
// itself, a terminal left open).
static void Leak(NSString *suite, NSString *what, BOOL fails = YES) {
    if (fails) gLeaks++;
    std::printf("  %s [%s] %s\n", fails ? "LEAK" : "LEFT", suite.UTF8String ?: "", what.UTF8String);
    std::fflush(stdout);
    if (fails && LeaksFail()) {
        gFail++;
        gSuiteFail++;
        GuiRetry::noteFailure(gResults, suite.UTF8String ?: "",
                              std::string("LEAK ") + (what.UTF8String ?: ""));
    }
}

// With the session's host on (editor.session-host, MUTIBLE_SESSION_HOST),
// every tab's text as the host has it must be the tab's own, the editor's
// storage for the tab on show: the mirrors never drift, whatever a suite
// did to them. GTK's harness checks the same (checkSessionMirrors).
static void CheckSessionMirrors(NSString *suite) {
    for (EditorController *c in MCGuiControllers()) {
        NSArray<MCTab *> *tabs = nil;
        @try {
            tabs = [c valueForKey:@"tabs"];
        } @catch (NSException *) {
        }
        if (![tabs isKindOfClass:NSArray.class]) continue;
        NSString *why = MCSessionCheckTabs(tabs);
        if (!why.length) continue;
        std::printf("  FAIL [%s] the session's host and the window it renders differ: %s\n",
                    suite.UTF8String ?: "", why.UTF8String);
        std::fflush(stdout);
        gFail++;
        gSuiteFail++;
        GuiRetry::noteFailure(gResults, suite.UTF8String ?: "",
                              std::string("the session's host and the window it renders differ: ") +
                                  (why.UTF8String ?: ""));
    }
}

// Every terminal not open when the suite started, closed unasked (a shell
// still running a command is hung up, as a window's close does).
static void CloseTerminalsLeft(const World &was) {
    for (EditorController *c in MCGuiControllers()) {
        MCTerminalPanel *panel = nil;
        @try {
            panel = [c valueForKey:@"terminalPanel"];
        } @catch (NSException *) {
        }
        if (![panel isKindOfClass:MCTerminalPanel.class]) continue;
        for (TerminalView *t in [panel.terminals copy])
            if (![was.terminalViews containsObject:t]) [panel removeTerminal:t];
    }
}

static void CheckWhatSuiteLeft(NSString *suite) {
    // A "_" suite runs alone in a process of its own, which it may end.
    if (!suite || [suite hasPrefix:@"_"]) return;
    CheckSessionMirrors(suite);
    // What windows that closed left on the session's hosts (their documents,
    // terminals and records) is ended, so one suite's never comes back in
    // the next suite's window on the same folder (GTK's endDetached).
    MCSession::endDetached();
    const BOOL putBack = LeaksFail();
    if (gKeyWindow) {
        Leak(suite, @"left the key window pinned (MCGuiSetKeyWindow(nil) at its end)");
        if (putBack) gKeyWindow = nil;
    }
    if (gAnswers.count) {
        NSArray *shown = gAlerts.count > gAlertsAtStart
            ? [gAlerts subarrayWithRange:NSMakeRange(gAlertsAtStart, gAlerts.count - gAlertsAtStart)]
            : @[];
        Leak(suite, [NSString stringWithFormat:@"left %lu alert answer(s) unused, which would answer "
                                               @"a later suite's questions (questions it saw: \"%@\")",
                                               (unsigned long)gAnswers.count,
                                               [shown componentsJoinedByString:@"\", \""]]);
        if (putBack) [gAnswers removeAllObjects];
    }
    World now = LookAtWorld(), was = gWorldAtStart;
    if (now.windows != was.windows)
        Leak(suite, [NSString stringWithFormat:@"left %ld editor window(s) where it found %ld",
                                               (long)now.windows, (long)was.windows]);
    if (now.sheets > was.sheets)
        Leak(suite, [NSString stringWithFormat:@"left %ld sheet(s) open on its windows", (long)now.sheets]);
    if (now.terminals > was.terminals) {
        Leak(suite, [NSString stringWithFormat:@"left %ld terminal(s) open where it found %ld, closed",
                                               (long)now.terminals, (long)was.terminals], NO);
        // Closed, as the two files are put back: the next suite finds the
        // terminals this one found (2026-11-18: run_file, terminal_vt,
        // daily_work, palette and keymap each left some, and control,
        // counting its own from none, failed in every soak round).
        if (putBack) CloseTerminalsLeft(was);
    }
    // The two files are the harness's to put back between suites, so a suite
    // that changes them needs no step of its own to undo it; said, not failed.
    if (!(now.settings == was.settings || [now.settings isEqualToString:was.settings])) {
        Leak(suite, [NSString stringWithFormat:@"left settings.conf changed, put back: \"%@\" (was \"%@\")",
                                               Short(now.settings), Short(was.settings)], NO);
        if (putBack && was.settings) MCGuiSetSettings(was.settings);
    }
    if (!(now.keys == was.keys || [now.keys isEqualToString:was.keys])) {
        Leak(suite, [NSString stringWithFormat:@"left keys.conf changed, put back: \"%@\" (was \"%@\")",
                                               Short(now.keys), Short(was.keys)], NO);
        // The app watches the file and reads it again itself.
        if (putBack) {
            if (was.keys) MCGuiWriteFile(KeysPath(), was.keys);
            else [[NSFileManager defaultManager] removeItemAtPath:KeysPath() error:nil];
        }
    }
    if (now.editorZoom != was.editorZoom || now.terminalZoom != was.terminalZoom)
        Leak(suite, [NSString stringWithFormat:@"left the zoom changed: editor %+g, terminals %+g (was "
                                               @"%+g, %+g)", now.editorZoom, now.terminalZoom,
                                               was.editorZoom, was.terminalZoom]);
    if (now.showHidden != was.showHidden)
        Leak(suite, [NSString stringWithFormat:@"left Show Hidden Files %@", now.showHidden ? @"on" : @"off"]);
    // A browser pane left showing narrows the editor of the window the next
    // suite works in (2026-11-17: control's agent page wrapped
    // markdown_side_sync's lines). A warning for now: how many suites leave
    // one was not known when it was added, and none is put back.
    if (![now.browser isEqualToString:was.browser])
        Leak(suite, [NSString stringWithFormat:@"left the browser pane changed: %@ (was %@)", now.browser,
                                               was.browser], NO);
    if (now.treeColumns != was.treeColumns)
        Leak(suite, [NSString stringWithFormat:@"left the tree's columns changed (%ld, was %ld)",
                                               (long)now.treeColumns, (long)was.treeColumns]);
}

// Windows of this process on screen now; there must never be any.
static int gOnScreen;
static void CountOnScreen(void) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list) return;
    for (NSDictionary *w in (__bridge NSArray *)list)
        if ([w[(__bridge NSString *)kCGWindowOwnerPID] intValue] == getpid()) gOnScreen++;
    CFRelease(list);
}

// ---------------------------------------------------------------- input
static CGEventFlags CGFlags(NSEventModifierFlags f) {
    CGEventFlags g = 0;
    if (f & NSEventModifierFlagCommand) g |= kCGEventFlagMaskCommand;
    if (f & NSEventModifierFlagShift) g |= kCGEventFlagMaskShift;
    if (f & NSEventModifierFlagOption) g |= kCGEventFlagMaskAlternate;
    if (f & NSEventModifierFlagControl) g |= kCGEventFlagMaskControl;
    return g;
}

// The key as the keyboard layout types it, as hardware sends it. A unicode
// string set on the CGEvent (CGEventKeyboardSetUnicodeString) made the menu
// refuse the event even when the characters read back the same ("=" for
// Cmd+=, "o" for Shift+Cmd+O), so the string is set only when the layout
// gives other characters than the ones asked for.
static NSEvent *MenuKeyEvent(unsigned short keyCode, NSEventModifierFlags flags, NSString *chars,
                             BOOL setString) {
    CGEventRef cg = CGEventCreateKeyboardEvent(NULL, keyCode, true);
    CGEventSetFlags(cg, CGFlags(flags));
    if (setString && chars.length) {
        unichar buf[8];
        NSUInteger n = MIN(chars.length, (NSUInteger)8);
        [chars getCharacters:buf range:NSMakeRange(0, n)];
        CGEventKeyboardSetUnicodeString(cg, n, buf);
    }
    NSEvent *e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    return e;
}

NSRect MCGuiLineTextRect(NSTextView *tv, NSUInteger index) {
    NSLayoutManager *lm = tv.layoutManager;
    const NSUInteger n = tv.string.length;
    if (n == 0) return NSZeroRect;
    index = MIN(index, n - 1);
    [lm ensureLayoutForCharacterRange:lm.allowsNonContiguousLayout ? NSMakeRange(index, 1)
                                                                   : NSMakeRange(0, index + 1)];
    const NSRect r = [lm lineFragmentUsedRectForGlyphAtIndex:[lm glyphIndexForCharacterAtIndex:index]
                                              effectiveRange:nil];
    const NSPoint o = tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

NSUInteger MCGuiLinesUnder(NSTextView *tv, NSRect rect) {
    NSLayoutManager *lm = tv.layoutManager;
    const NSPoint o = tv.textContainerOrigin;
    const NSRect in = NSOffsetRect(rect, -o.x, -o.y);
    // A little above and below, so a line just past an edge is looked at.
    const NSRect around = NSInsetRect(in, 0, -40);
    [lm ensureLayoutForBoundingRect:around inTextContainer:tv.textContainer];
    const NSRange glyphs = [lm glyphRangeForBoundingRect:around inTextContainer:tv.textContainer];
    __block NSUInteger covered = 0;
    [lm enumerateLineFragmentsForGlyphRange:glyphs
                                 usingBlock:^(NSRect, NSRect used, NSTextContainer *, NSRange, BOOL *) {
        if (NSMinY(used) < NSMaxY(in) - 0.5 && NSMaxY(used) > NSMinY(in) + 0.5) covered++;
    }];
    return covered;
}

BOOL MCGuiPerformMenuKey(NSWindow *window, unsigned short keyCode, NSEventModifierFlags flags,
                         NSString *chars, NSString *charsIgnoringModifiers) {
    NSEvent *e = MenuKeyEvent(keyCode, flags, chars, NO);
    const BOOL asked = (chars.length && ![e.characters isEqualToString:chars]) ||
                       (charsIgnoringModifiers.length &&
                        ![e.charactersIgnoringModifiers isEqualToString:charsIgnoringModifiers] &&
                        ![e.charactersIgnoringModifiers.lowercaseString
                            isEqualToString:charsIgnoringModifiers.lowercaseString]);
    if (asked) e = MenuKeyEvent(keyCode, flags, chars.length ? chars : charsIgnoringModifiers, YES);
    if ([window performKeyEquivalent:e]) return YES;
    return [NSApp.mainMenu performKeyEquivalent:e];
}

void MCGuiSendKeyToApp(NSWindow *window, unsigned short keyCode, NSEventModifierFlags flags,
                       NSString *chars, NSString *charsIgnoringModifiers) {
    NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                             modifierFlags:flags
                                 timestamp:NSProcessInfo.processInfo.systemUptime
                              windowNumber:window.windowNumber context:nil
                                characters:chars charactersIgnoringModifiers:charsIgnoringModifiers
                                 isARepeat:NO keyCode:keyCode];
    [NSApp sendEvent:e];
}

BOOL MCGuiPressKey(NSWindow *window, unsigned short keyCode, NSEventModifierFlags flags) {
    NSEvent *made = MenuKeyEvent(keyCode, flags, nil, NO);
    // The same press, addressed to the window, for the monitor's question
    // of what has the keyboard.
    NSEvent *e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                             modifierFlags:made.modifierFlags timestamp:0
                              windowNumber:window.windowNumber context:nil
                                characters:made.characters
               charactersIgnoringModifiers:made.charactersIgnoringModifiers
                                 isARepeat:NO keyCode:keyCode];
    if (MCKeymapHandleKeyDown(e)) return YES;
    return MCGuiPerformMenuKey(window, keyCode, flags, nil, nil);
}

void MCGuiUndoGroup(NSUndoManager *undo, void (^block)(void)) {
    BOOL byEvent = undo.groupsByEvent;
    undo.groupsByEvent = NO;
    [undo beginUndoGrouping];
    block();
    [undo endUndoGrouping];
    undo.groupsByEvent = byEvent;
}

// ------------------------------------------------------------ processes
void MCGuiRunProcess(NSArray<NSString *> *argv, NSDictionary<NSString *, NSString *> *env,
                     void (^done)(int status, NSString *out, NSString *err)) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:argv.firstObject];
    task.arguments = [argv subarrayWithRange:NSMakeRange(1, argv.count - 1)];
    NSMutableDictionary *e = [NSProcessInfo.processInfo.environment mutableCopy];
    [e addEntriesFromDictionary:env ?: @{}];
    task.environment = e;
    task.currentDirectoryURL = [NSURL fileURLWithPath:MCGuiProject()];
    NSPipe *outPipe = [NSPipe pipe], *errPipe = [NSPipe pipe];
    task.standardOutput = outPipe;
    task.standardError = errPipe;
    task.standardInput = [NSFileHandle fileHandleWithNullDevice];
    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        done(-1, @"", error.localizedDescription ?: @"could not start");
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        __block NSData *errData = nil;
        dispatch_group_t g = dispatch_group_create();
        dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            errData = [errPipe.fileHandleForReading readDataToEndOfFile];
        });
        NSData *outData = [outPipe.fileHandleForReading readDataToEndOfFile];
        dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
        [task waitUntilExit];
        int status = task.terminationStatus;
        NSString *out = [[NSString alloc] initWithData:outData encoding:NSUTF8StringEncoding] ?: @"";
        NSString *err = [[NSString alloc] initWithData:errData encoding:NSUTF8StringEncoding] ?: @"";
        dispatch_async(dispatch_get_main_queue(), ^{
            @try { done(status, out, err); }
            @catch (NSException *e) {
                MCGuiCheck(NO, [NSString stringWithFormat:@"exception: %@ %@", e.name, e.reason],
                           __FILE__, __LINE__);
            }
        });
    });
}

void MCGuiRunChild(NSString *suite, NSString *folder, void (^done)(int status, NSString *out)) {
    NSString *defaults = [Env(@"MUTIBLE_DEFAULTS_SUITE") stringByAppendingString:@".child"];
    NSString *program = [MCGuiBuildDir() stringByAppendingPathComponent:@"guitest"];
    MCGuiRunProcess(folder.length ? @[program, folder] : @[program],
                    @{@"MC_GUI_CHILD": @"1", @"MC_GUI_SUITES": suite, @"MC_GUI_KEEP": @"1",
                      @"MUTIBLE_DEFAULTS_SUITE": defaults, @"MC_GUI_RESULTS": @"",
                      @"MC_GUI_SHUFFLE": @""},
                    ^(int status, NSString *out, NSString *err) {
                        done(status, [out stringByAppendingString:err ?: @""]);
                    });
}

// --------------------------------------------------------------- runner
@interface MCGuiRunner : NSObject
+ (instancetype)shared;
- (void)start;
- (void)finish;
@end

// The words of a list of suite names (MC_GUI_SUITES, MC_GUI_SKIP_SUITES).
static NSArray<NSString *> *SuiteNames(NSString *list) {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    NSCharacterSet *blanks = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    for (NSString *w in [list ?: @"" componentsSeparatedByCharactersInSet:blanks])
        if (w.length) [out addObject:w];
    return out;
}

@implementation MCGuiRunner {
    NSUInteger _index;
    MCGuiItem *_waiting;
    NSDate *_deadline;
    BOOL _asyncDone;
    NSDate *_started;
}

+ (instancetype)shared {
    static MCGuiRunner *r;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ r = [MCGuiRunner new]; });
    return r;
}

- (void)start {
    _started = [NSDate date];
    NSString *only = Env(@"MC_GUI_SUITES");
    NSArray<NSString *> *wanted = SuiteNames(only);
    // MC_GUI_SKIP_SUITES leaves suites out: CI runs the list as two
    // processes at once, one naming its half and the other skipping it, so
    // a new suite lands in the second without anyone listing it.
    NSArray<NSString *> *skipped = SuiteNames(Env(@"MC_GUI_SKIP_SUITES"));
    // A name in either list that no suite has is a failure, not a silent
    // skip: a suite renamed after CI's list named it would otherwise run in
    // neither half.
    NSMutableSet<NSString *> *known = [NSMutableSet set];
    for (const Suite &s : suites()) [known addObject:[NSString stringWithUTF8String:s.name.c_str()]];
    for (NSString *w in [wanted arrayByAddingObjectsFromArray:skipped])
        if (![known containsObject:w])
            MCGuiCheck(NO, [NSString stringWithFormat:@"no suite is named \"%@\"", w], __FILE__, __LINE__);
    std::vector<const Suite *> chosen;
    for (const Suite &s : suites()) {
        NSString *name = [NSString stringWithUTF8String:s.name.c_str()];
        if (wanted.count && ![wanted containsObject:name]) continue;
        if ([skipped containsObject:name]) continue;
        // A suite named with a leading underscore is a part another suite
        // runs in a process of its own (BackupsSuite): only when named.
        if (!wanted.count && [name hasPrefix:@"_"]) continue;
        chosen.push_back(&s);
    }
    // MC_GUI_SHUFFLE=<seed>: the chosen suites in the order the seed gives
    // (tests/GuiRetry.h), the same on every machine, so the soak
    // workflow's rounds can be replayed. Otherwise the order they are
    // linked in.
    NSString *shuffle = Env(@"MC_GUI_SHUFFLE");
    if (shuffle.length) {
        uint64_t seed = 0;
        if (!GuiRetry::parseSeed(shuffle.UTF8String, &seed)) {
            MCGuiCheck(NO, [NSString stringWithFormat:@"MC_GUI_SHUFFLE is \"%@\", not a number", shuffle],
                       __FILE__, __LINE__);
        } else {
            std::vector<const Suite *> order;
            for (size_t i : GuiRetry::shuffledOrder(chosen.size(), seed)) order.push_back(chosen[i]);
            chosen = order;
            std::printf("Suite order shuffled with MC_GUI_SHUFFLE=%llu (the same seed and suites "
                        "replay it)\n", (unsigned long long)seed);
            std::fflush(stdout);
        }
    }
    int ran = 0;
    for (const Suite *s : chosen) {
        MCGuiItem *marker = [MCGuiItem new];
        marker.kind = MCGuiKindSuite;
        marker.name = [NSString stringWithUTF8String:s->name.c_str()];
        Enqueue(marker);
        s->body();
        ran++;
    }
    if (!ran) {
        std::printf("No suite matched \"%s\".\n", only.UTF8String ?: "");
        [self finish];
        return;
    }
    [self schedule];
}

- (void)schedule {
    [self performSelector:@selector(next) withObject:nil afterDelay:0.01
                  inModes:@[NSRunLoopCommonModes]];
}

- (void)endSuite {
    if (gSuite) std::printf("%s: %d passed, %d failed\n", gSuite.UTF8String, gSuitePass, gSuiteFail);
    std::fflush(stdout);
}

- (void)next {
    if (_waiting) {
        BOOL finished = NO, timedOut = [_deadline timeIntervalSinceNow] < 0;
        if (_waiting.kind == MCGuiKindWait) {
            @try { finished = _waiting.until(); }
            @catch (NSException *e) {
                MCGuiCheck(NO, [NSString stringWithFormat:@"exception: %@", e.reason], __FILE__, __LINE__);
                finished = YES;
            }
        } else {
            finished = _asyncDone;
        }
        if (!finished && !timedOut) {
            [self performSelector:@selector(next) withObject:nil afterDelay:0.05
                          inModes:@[NSRunLoopCommonModes]];
            return;
        }
        if (!finished)
            MCGuiCheck(NO, [NSString stringWithFormat:@"timed out after %.0f s: %@",
                            _waiting.timeout, _waiting.name], __FILE__, __LINE__);
        _waiting = nil;
    }
    if (_index >= gItems.count) {
        gStep = @"what the suite left";
        CheckWhatSuiteLeft(gSuite);
        [self endSuite];
        [self finish];
        return;
    }
    CountOnScreen();
    MCGuiItem *item = gItems[_index++];
    gStep = item.name;
    switch (item.kind) {
    case MCGuiKindSuite:
        gStep = @"what the suite left";
        CheckWhatSuiteLeft(gSuite);
        gStep = item.name;
        [self endSuite];
        gSuite = item.name;
        gSuitePass = gSuiteFail = 0;
        gWorldAtStart = LookAtWorld();
        gAlertsAtStart = gAlerts.count;
        std::printf("Suite %s\n", item.name.UTF8String);
        std::fflush(stdout);
        break;
    case MCGuiKindStep:
        if (Env(@"MC_GUI_VERBOSE").length) MCGuiNote(@"%@", item.name);
        @try { item.block(); }
        @catch (NSException *e) {
            MCGuiCheck(NO, [NSString stringWithFormat:@"exception: %@ %@", e.name, e.reason],
                       __FILE__, __LINE__);
        }
        break;
    case MCGuiKindWait:
        _waiting = item;
        _deadline = [NSDate dateWithTimeIntervalSinceNow:item.timeout];
        break;
    case MCGuiKindAsync: {
        _waiting = item;
        _deadline = [NSDate dateWithTimeIntervalSinceNow:item.timeout];
        _asyncDone = NO;
        __weak MCGuiRunner *weakSelf = self;
        MCGuiItem *mine = item;
        @try {
            item.async(^{
                MCGuiRunner *s = weakSelf;
                if (s && s->_waiting == mine) s->_asyncDone = YES;
            });
        } @catch (NSException *e) {
            MCGuiCheck(NO, [NSString stringWithFormat:@"exception: %@", e.reason], __FILE__, __LINE__);
            _asyncDone = YES;
        }
        break;
    }
    }
    [self schedule];
}

- (void)finish {
    CountOnScreen();
    gStep = @"the end";
    gOutside = YES;
    MCGuiCheck(gOnScreen == 0, @"no window of this process was ever on screen", __FILE__, __LINE__);
    std::printf("\nGUI checks: %d passed, %d failed (%.1f s)\n", gPass, gFail,
                -[_started timeIntervalSinceNow]);
    if (gSlow || gLeaks)
        std::printf("Also: %d timing check(s) over their bound within this runner's room, "
                    "%d leak(s) between suites\n", gSlow, gLeaks);
    PrintNearestBounds();
    std::fflush(stdout);
    // The windows go (their shells hung up and reaped), then the world.
    for (EditorController *c in [MCGuiControllers() copy]) [c.window close];
    // What closing them wrote down (the last window's record, kept) is in
    // sessions.json before the process goes, as quitting flushes it: a
    // child app's parent reads the file next.
    MCSessionsFlush();
    MCControlServerStop();
    NSString *suite = Env(@"MUTIBLE_DEFAULTS_SUITE");
    if (suite.length) [[NSUserDefaults standardUserDefaults] removePersistentDomainForName:suite];
    NSString *scratch = MCGuiScratch();
    if (scratch.length && !Env(@"MC_GUI_KEEP").length && [scratch hasPrefix:@"/tmp/mcgui-"])
        [[NSFileManager defaultManager] removeItemAtPath:scratch error:nil];
    else if (scratch.length)
        std::printf("Scratch kept in %s\n", scratch.UTF8String);
    std::fflush(stdout);
    // What failed, for the first stage, which may retry a failed suite
    // (MC_GUI_RETRIES; "_" children are given none to write).
    gResults.complete = YES;
    if (Env(@"MC_GUI_RESULTS").length) GuiRetry::writeResults(Env(@"MC_GUI_RESULTS").UTF8String, gResults);
    _exit(gFail ? 1 : 0);
}
@end

// -------------------------------------------------------------- swizzles
static void Replace(Class c, SEL sel, id block) {
    Method m = class_getInstanceMethod(c, sel);
    IMP imp = imp_implementationWithBlock(block);
    if (m && class_addMethod(c, sel, imp, method_getTypeEncoding(m))) return;   // a subclass's own
    if (m) method_setImplementation(m, imp);
}

static void ReplaceClass(Class c, SEL sel, id block) {
    Method m = class_getClassMethod(c, sel);
    if (m) method_setImplementation(m, imp_implementationWithBlock(block));
}

// An exception AppKit cannot recover from (one thrown in drawRect: or a
// layout pass) ends the process through +[NSApplication _crashOnException:]
// with only "Trace/BPT trap" to show for it. Say which exception, in which
// suite and step, and where it was thrown, before letting it crash.
static void ReportFatalException(NSException *e) {
    std::fprintf(stdout, "\nFATAL exception in [%s] (step: %s): %s: %s\n%s\n",
                 gSuite.UTF8String ?: "?", gStep.UTF8String ?: "?",
                 e.name.UTF8String ?: "?", e.reason.UTF8String ?: "",
                 [e.callStackSymbols componentsJoinedByString:@"\n"].UTF8String ?: "");
    std::fflush(stdout);
}

static void InstallExceptionReporting(void) {
    NSSetUncaughtExceptionHandler([](NSException *e) { ReportFatalException(e); });
    Class meta = object_getClass(NSApplication.class);
    SEL sel = NSSelectorFromString(@"_crashOnException:");
    Method m = class_getInstanceMethod(meta, sel);
    if (!m) return;
    typedef void (*Crash)(id, SEL, NSException *);
    Crash original = (Crash)method_getImplementation(m);
    method_setImplementation(m, imp_implementationWithBlock(^(id cls, NSException *e) {
        ReportFatalException(e);
        original(cls, sel, e);
    }));
}

static void InstallSwizzles(void) {
    InstallExceptionReporting();
    Class W = NSWindow.class;
    Replace(W, @selector(makeKeyAndOrderFront:), ^(id, id) {});
    Replace(W, @selector(orderFront:), ^(id, id) {});
    Replace(W, @selector(orderFrontRegardless), ^(id) {});
    Replace(W, @selector(orderWindow:relativeTo:), ^(id, NSWindowOrderingMode, NSInteger) {});
    Replace(W, @selector(makeKeyWindow), ^(id) {});
    Replace(W, @selector(makeMainWindow), ^(id) {});
    Replace(W, @selector(addChildWindow:ordered:), ^(id, NSWindow *, NSWindowOrderingMode) {});
    Replace(W, @selector(isKeyWindow), ^BOOL(NSWindow *w) { return w == TestKeyWindow(); });
    Replace(W, @selector(isMainWindow), ^BOOL(NSWindow *w) { return w == TestKeyWindow(); });
    Class A = NSApplication.class;
    Replace(A, @selector(keyWindow), ^NSWindow *(id) { return TestKeyWindow(); });
    Replace(A, @selector(mainWindow), ^NSWindow *(id) { return TestKeyWindow(); });
    Replace(A, @selector(activateIgnoringOtherApps:), ^(id, BOOL) {});
    Replace(A, @selector(activate), ^(id) {});
    {
        Method m = class_getInstanceMethod(A, @selector(setActivationPolicy:));
        typedef BOOL (*SetPolicy)(id, SEL, NSApplicationActivationPolicy);
        SetPolicy original = (SetPolicy)method_getImplementation(m);
        method_setImplementation(m, imp_implementationWithBlock(^BOOL(id app, NSApplicationActivationPolicy) {
            return original(app, @selector(setActivationPolicy:),
                            NSApplicationActivationPolicyProhibited);
        }));
    }
    Replace(NSAlert.class, @selector(runModal), ^NSModalResponse(NSAlert *a) {
        if (!gAlerts) gAlerts = [NSMutableArray array];
        [gAlerts addObject:a.messageText ?: @""];
        gLastInformative = [a.informativeText copy];
        if (gAnswers.count) {
            NSModalResponse r = gAnswers.firstObject.integerValue;
            [gAnswers removeObjectAtIndex:0];
            return r;
        }
        return NSAlertFirstButtonReturn;
    });
    Replace(NSAlert.class, @selector(beginSheetModalForWindow:completionHandler:),
            ^(NSAlert *a, NSWindow *window, void (^handler)(NSModalResponse)) {
        if (!gSheets) gSheets = [NSMutableArray array];
        [gSheets addObject:a.messageText ?: @""];
        gLastSheetWindow = window;
        NSModalResponse r = [a runModal];
        if (handler) dispatch_async(dispatch_get_main_queue(), ^{ handler(r); });
    });
    ReplaceClass(NSPasteboard.class, @selector(generalPasteboard), ^NSPasteboard *(id) {
        return [NSPasteboard pasteboardWithName:Env(@"MUTIBLE_PASTEBOARD") ?: @"ai.ible.mutible.guitest"];
    });
    Replace(NSWorkspace.class, @selector(openURL:), ^BOOL(id, NSURL *) { return YES; });
}

// ---------------------------------------------------------------- setup
// The scratch world, made before the real run (see Harness.h).
static NSString *MakeWorld(void) {
    char tmpl[] = "/tmp/mcgui-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
    NSString *root = [NSString stringWithUTF8String:tmpl];
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSString *d in @[@"home", @"zdot", @"t", @"backups", @"project/src"])
        [fm createDirectoryAtPath:[root stringByAppendingPathComponent:d]
      withIntermediateDirectories:YES attributes:nil error:nil];
    MCGuiWriteFile([root stringByAppendingPathComponent:@"zdot/.zshrc"],
                   @"HISTFILE=$ZDOTDIR/.zsh_history\nPS1='%% '\n");
    MCGuiWriteFile([root stringByAppendingPathComponent:@"settings.conf"],
                   @"lsp.enabled = false\n");
    NSMutableString *big = [NSMutableString string];
    for (int i = 1; i <= 400; i++) [big appendFormat:@"line %d of big.txt\n", i];
    MCGuiWriteFile([root stringByAppendingPathComponent:@"project/big.txt"], big);
    MCGuiWriteFile([root stringByAppendingPathComponent:@"project/README.md"],
                   @"# Project\n\nA scratch project for the GUI checks.\n");
    return root;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (!getenv("MC_GUI_CHILD")) {
            // The environment as started, for a retry's fresh run (below).
            std::vector<std::string> startEnv;
            for (char **e = *_NSGetEnviron(); e && *e; e++) startEnv.push_back(*e);
            // Re-run this program in the scratch world, so nothing the app
            // reads at startup (home, defaults, settings) is the user's.
            NSString *root = MakeWorld();
            NSString *suite = [NSString stringWithFormat:@"ai.ible.mutible.guitest.%d", getpid()];
            NSDictionary *env = @{
                @"MC_GUI_CHILD": @"1",
                @"MC_GUI_SCRATCH": root,
                @"HOME": [root stringByAppendingPathComponent:@"home"],
                @"CFFIXED_USER_HOME": [root stringByAppendingPathComponent:@"home"],
                @"ZDOTDIR": [root stringByAppendingPathComponent:@"zdot"],
                @"TMPDIR": [[root stringByAppendingPathComponent:@"t"] stringByAppendingString:@"/"],
                @"MUTIBLE_SETTINGS": [root stringByAppendingPathComponent:@"settings.conf"],
                @"MUTIBLE_DEFAULTS_SUITE": suite,
                @"MUTIBLE_BACKUP_DIR": [root stringByAppendingPathComponent:@"backups"],
                @"MUTIBLE_PASTEBOARD": [suite stringByAppendingString:@".pasteboard"],
                @"MUTIBLE_MATH_CACHE": [root stringByAppendingPathComponent:@"math"],
                // Claude Code's settings and lock files, never ~/.claude's.
                @"CLAUDE_CONFIG_DIR": [root stringByAppendingPathComponent:@"claude"],
                @"GIT_CONFIG_GLOBAL": @"/dev/null",
                @"GIT_CONFIG_NOSYSTEM": @"1",
                @"GIT_AUTHOR_NAME": @"GUI Check", @"GIT_AUTHOR_EMAIL": @"gui@example.invalid",
                @"GIT_COMMITTER_NAME": @"GUI Check",
                @"GIT_COMMITTER_EMAIL": @"gui@example.invalid",
            };
            for (NSString *k in env) MCSetEnv(k.UTF8String, [env[k] UTF8String]);
            NSMutableString *wanted = [NSMutableString string];
            for (int i = 1; i < argc; i++) [wanted appendFormat:@"%s%s", i > 1 ? " " : "", argv[i]];
            MCSetEnv("MC_GUI_SUITES", wanted.UTF8String);
            // A socket left by a process that is gone, as kill -9 leaves one:
            // the app must remove it when it starts.
            pid_t dead = fork();
            if (dead == 0) _exit(0);
            waitpid(dead, nullptr, 0);
            NSString *folder = [root stringByAppendingPathComponent:@"t/Mutible"];
            mkdir(folder.fileSystemRepresentation, 0700);
            NSString *stale = [folder stringByAppendingFormat:@"/%d.sock", dead];
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            strlcpy(addr.sun_path, stale.fileSystemRepresentation, sizeof addr.sun_path);
            bind(fd, (struct sockaddr *)&addr, sizeof addr);
            close(fd);
            MCSetEnv("MC_GUI_STALE_SOCKET", stale.UTF8String);
            char self[PATH_MAX];
            uint32_t size = sizeof self;
            _NSGetExecutablePath(self, &size);
            // What failed goes beside the scratch folder, which the run
            // removes as it ends.
            NSString *results = [root stringByAppendingString:@".results"];
            MCSetEnv("MC_GUI_RESULTS", results.fileSystemRepresentation);
            const char *child[] = {self, [root stringByAppendingPathComponent:@"project"].UTF8String,
                                   nullptr};
            // The run is a child, so that this process can retry a suite
            // that failed (MC_GUI_RETRIES, tests/GuiRetry.h) once it ends.
            std::fflush(stdout);
            const pid_t run = fork();
            if (run == 0) {
                execv(self, (char *const *)child);
                perror("execv");
                _exit(2);
            }
            int status = 2;
            if (run > 0) {
                int st = 0;
                while (waitpid(run, &st, 0) < 0 && errno == EINTR) {
                }
                status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
                if (WIFSIGNALED(st)) {
                    std::printf("\nguitest: the run ended with signal %d\n", WTERMSIG(st));
                    std::fflush(stdout);
                }
            }
            const char *retries = getenv("MC_GUI_RETRIES");
            status = GuiRetry::retryFailedSuites(status, results.fileSystemRepresentation,
                                                 retries ? atoi(retries) : 0, self, startEnv, stdout);
            unlink(results.fileSystemRepresentation);
            return status == 0 ? 0 : 1;
        }
        InstallSwizzles();
        // MC_GUI_SCROLLBARS=always: scroll bars that always take room, as
        // on a Mac without a trackpad and on CI's runners.
        if ([Env(@"MC_GUI_SCROLLBARS") isEqualToString:@"always"])
            [[NSUserDefaults standardUserDefaults]
                setVolatileDomain:@{@"AppleShowScrollBars": @"Always"} forName:NSArgumentDomain];
        // The checks start once the app has opened its window, and the whole
        // run has 25 minutes (MC_GUI_TOTAL_SECONDS), a backstop only: every
        // wait has its own limit. Ten minutes was hit by full runs on a
        // machine loaded by several builds (the run was about 5 minutes
        // unloaded on 2026-11-08 and grows with every suite).
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidFinishLaunchingNotification object:nil
                         queue:nil usingBlock:^(NSNotification *) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)),
                           dispatch_get_main_queue(), ^{ [[MCGuiRunner shared] start]; });
        }];
        const double total = Env(@"MC_GUI_TOTAL_SECONDS").doubleValue > 0
                                 ? Env(@"MC_GUI_TOTAL_SECONDS").doubleValue : 1500;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(total * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            gOutside = YES;
            MCGuiCheck(NO, [NSString stringWithFormat:@"the whole run took over %.0f s", total],
                       __FILE__, __LINE__);
            [[MCGuiRunner shared] finish];
        });
    }
    return mutible_main(argc, argv);
}
