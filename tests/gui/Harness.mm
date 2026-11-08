// Harness.mm — see Harness.h.
#import "Harness.h"
#import "EditorController.h"
#include <cstdio>
#include <ctime>
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

int minicode_main(int argc, const char *argv[]);   // src/main.mm, renamed

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
static int gPass, gFail;
static int gSuitePass, gSuiteFail;
static NSString *gSuite;
static NSString *gStep;
static NSMutableArray<NSNumber *> *gAnswers;
static NSMutableArray<NSString *> *gAlerts;
static __weak NSWindow *gKeyWindow;

static void Enqueue(MCGuiItem *item) {
    if (!gItems) gItems = [NSMutableArray array];
    [gItems addObject:item];
}

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
    i.timeout = timeout;
    Enqueue(i);
}

void MCGuiAsync(NSString *name, NSTimeInterval timeout, void (^block)(MCGuiDone)) {
    MCGuiItem *i = [MCGuiItem new];
    i.kind = MCGuiKindAsync;
    i.name = name;
    i.async = block;
    i.timeout = timeout;
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
    i.timeout = timeout;
    Enqueue(i);
}

BOOL MCGuiCheck(BOOL ok, NSString *what, const char *file, int line) {
    if (ok) {
        gPass++;
        gSuitePass++;
    } else {
        gFail++;
        gSuiteFail++;
        const char *slash = strrchr(file, '/');
        std::printf("  FAIL [%s] %s:%d  %s  (in: %s)\n", gSuite.UTF8String ?: "",
                    slash ? slash + 1 : file, line, what.UTF8String, gStep.UTF8String ?: "");
        std::fflush(stdout);
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

BOOL MCGuiCheckTime(double ms, double bound, NSString *what, const char *file, int line) {
    NSString *text = [NSString stringWithFormat:@"%@ under %.0f ms of CPU (%.2f)", what, bound, ms];
    return MCGuiCheck(MCGuiTimingSkipped() || ms < bound, text, file, line);
}

// ---------------------------------------------------------------- world
static NSString *Env(NSString *name) { return NSProcessInfo.processInfo.environment[name]; }
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

void MCGuiWriteFile(NSString *path, NSString *text) {
    [[NSFileManager defaultManager] createDirectoryAtPath:path.stringByDeletingLastPathComponent
                              withIntermediateDirectories:YES attributes:nil error:nil];
    [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

void MCGuiSetSettings(NSString *text) {
    MCGuiWriteFile(Env(@"MINICODE_SETTINGS"), text);
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
    NSString *defaults = [Env(@"MINICODE_DEFAULTS_SUITE") stringByAppendingString:@".child"];
    MCGuiRunProcess(@[[MCGuiBuildDir() stringByAppendingPathComponent:@"guitest"], folder],
                    @{@"MC_GUI_CHILD": @"1", @"MC_GUI_SUITES": suite, @"MC_GUI_KEEP": @"1",
                      @"MINICODE_DEFAULTS_SUITE": defaults},
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
    NSArray<NSString *> *wanted = only.length
        ? [only componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet] : @[];
    int ran = 0;
    for (const Suite &s : suites()) {
        NSString *name = [NSString stringWithUTF8String:s.name.c_str()];
        if (wanted.count && ![wanted containsObject:name]) continue;
        // A suite named with a leading underscore is a part another suite
        // runs in a process of its own (BackupsSuite): only when named.
        if (!wanted.count && [name hasPrefix:@"_"]) continue;
        MCGuiItem *marker = [MCGuiItem new];
        marker.kind = MCGuiKindSuite;
        marker.name = name;
        Enqueue(marker);
        s.body();
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
        [self endSuite];
        [self finish];
        return;
    }
    CountOnScreen();
    MCGuiItem *item = gItems[_index++];
    gStep = item.name;
    switch (item.kind) {
    case MCGuiKindSuite:
        [self endSuite];
        gSuite = item.name;
        gSuitePass = gSuiteFail = 0;
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
    MCGuiCheck(gOnScreen == 0, @"no window of this process was ever on screen", __FILE__, __LINE__);
    std::printf("\nGUI checks: %d passed, %d failed (%.1f s)\n", gPass, gFail,
                -[_started timeIntervalSinceNow]);
    std::fflush(stdout);
    // The windows go (their shells hung up and reaped), then the world.
    for (EditorController *c in [MCGuiControllers() copy]) [c.window close];
    MCControlServerStop();
    NSString *suite = Env(@"MINICODE_DEFAULTS_SUITE");
    if (suite.length) [[NSUserDefaults standardUserDefaults] removePersistentDomainForName:suite];
    NSString *scratch = MCGuiScratch();
    if (scratch.length && !Env(@"MC_GUI_KEEP").length && [scratch hasPrefix:@"/tmp/mcgui-"])
        [[NSFileManager defaultManager] removeItemAtPath:scratch error:nil];
    else if (scratch.length)
        std::printf("Scratch kept in %s\n", scratch.UTF8String);
    std::fflush(stdout);
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
        if (gAnswers.count) {
            NSModalResponse r = gAnswers.firstObject.integerValue;
            [gAnswers removeObjectAtIndex:0];
            return r;
        }
        return NSAlertFirstButtonReturn;
    });
    Replace(NSAlert.class, @selector(beginSheetModalForWindow:completionHandler:),
            ^(NSAlert *a, NSWindow *, void (^handler)(NSModalResponse)) {
        NSModalResponse r = [a runModal];
        if (handler) dispatch_async(dispatch_get_main_queue(), ^{ handler(r); });
    });
    ReplaceClass(NSPasteboard.class, @selector(generalPasteboard), ^NSPasteboard *(id) {
        return [NSPasteboard pasteboardWithName:Env(@"MINICODE_PASTEBOARD") ?: @"org.minicode.guitest"];
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
            // Re-run this program in the scratch world, so nothing the app
            // reads at startup (home, defaults, settings) is the user's.
            NSString *root = MakeWorld();
            NSString *suite = [NSString stringWithFormat:@"org.minicode.guitest.%d", getpid()];
            NSDictionary *env = @{
                @"MC_GUI_CHILD": @"1",
                @"MC_GUI_SCRATCH": root,
                @"HOME": [root stringByAppendingPathComponent:@"home"],
                @"CFFIXED_USER_HOME": [root stringByAppendingPathComponent:@"home"],
                @"ZDOTDIR": [root stringByAppendingPathComponent:@"zdot"],
                @"TMPDIR": [[root stringByAppendingPathComponent:@"t"] stringByAppendingString:@"/"],
                @"MINICODE_SETTINGS": [root stringByAppendingPathComponent:@"settings.conf"],
                @"MINICODE_DEFAULTS_SUITE": suite,
                @"MINICODE_BACKUP_DIR": [root stringByAppendingPathComponent:@"backups"],
                @"MINICODE_PASTEBOARD": [suite stringByAppendingString:@".pasteboard"],
                @"MINICODE_MATH_CACHE": [root stringByAppendingPathComponent:@"math"],
                @"GIT_CONFIG_GLOBAL": @"/dev/null",
                @"GIT_CONFIG_NOSYSTEM": @"1",
                @"GIT_AUTHOR_NAME": @"GUI Check", @"GIT_AUTHOR_EMAIL": @"gui@example.invalid",
                @"GIT_COMMITTER_NAME": @"GUI Check",
                @"GIT_COMMITTER_EMAIL": @"gui@example.invalid",
            };
            for (NSString *k in env) setenv(k.UTF8String, [env[k] UTF8String], 1);
            NSMutableString *wanted = [NSMutableString string];
            for (int i = 1; i < argc; i++) [wanted appendFormat:@"%s%s", i > 1 ? " " : "", argv[i]];
            setenv("MC_GUI_SUITES", wanted.UTF8String, 1);
            // A socket left by a process that is gone, as kill -9 leaves one:
            // the app must remove it when it starts.
            pid_t dead = fork();
            if (dead == 0) _exit(0);
            waitpid(dead, nullptr, 0);
            NSString *folder = [root stringByAppendingPathComponent:@"t/MiniCode"];
            mkdir(folder.fileSystemRepresentation, 0700);
            NSString *stale = [folder stringByAppendingFormat:@"/%d.sock", dead];
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            strlcpy(addr.sun_path, stale.fileSystemRepresentation, sizeof addr.sun_path);
            bind(fd, (struct sockaddr *)&addr, sizeof addr);
            close(fd);
            setenv("MC_GUI_STALE_SOCKET", stale.UTF8String, 1);
            char self[PATH_MAX];
            uint32_t size = sizeof self;
            _NSGetExecutablePath(self, &size);
            const char *child[] = {self, [root stringByAppendingPathComponent:@"project"].UTF8String,
                                   nullptr};
            execv(self, (char *const *)child);
            perror("execv");
            return 2;
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
            MCGuiCheck(NO, [NSString stringWithFormat:@"the whole run took over %.0f s", total],
                       __FILE__, __LINE__);
            [[MCGuiRunner shared] finish];
        });
    }
    return minicode_main(argc, argv);
}
