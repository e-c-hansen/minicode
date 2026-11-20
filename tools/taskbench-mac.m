// taskbench-mac — drive one editor through one task and measure it.
//
// scripts/taskbench-mac.sh (run through `make taskbench-mac`) writes each
// app's steps for each task to a file and runs `taskbench-mac run` on it.
// The steps are inputs a person makes (key chords, typed text, clicks on
// elements found through the Accessibility API, a file opened as Finder
// opens it) each followed by a result condition polled every 10 ms (a window
// title, an element's text, the keyboard's place, a shell command such as
// `git diff --cached`, a port closing). Measured per step: wall time from the
// input sent to the condition true. Per task: the inputs counted (clicks, key
// chords, keystrokes of typed text, Finder opens), the CPU time of the app's
// whole process tree (user + system, proc_pid_rusage, including children it
// reaped), and its physical footprint, peak and settled (procmem-mac's
// method: the process and every process whose parent or responsible pid is
// in the tree).
//
//   taskbench-mac check                      AX trust and the owner's idle time
//   taskbench-mac idle-wait SECONDS STATE    wait until the owner has been idle
//   taskbench-mac run --exe PATH --steps FILE --state STATE [--out FILE]
//   taskbench-mac quit --exe PATH PID        stop a pid this run started, and its tree
//   taskbench-mac axdump PID [DEPTH]         print an app's windows' AX tree
//   taskbench-mac tree PID                   print a tree's CPU and footprint
//   taskbench-mac media DIR                  write report.pdf, picture.png, clip.mp4
//   taskbench-mac biglog PATH MB NEEDLE      write a log of MB megabytes, NEEDLE near its end
//
// The step language (one step a line, `#` starts a comment line):
//
//   [setup [N]:] [?]INPUT [=> CONDITION] [@TIMEOUT_SECONDS]
//
// INPUT is one of
//   key CHORD [CHORD...]   chords like cmd+shift+p, alt+f5, return (one input each)
//   type TEXT              typed text, one input per character
//   press SPEC             AXPress on an element, else a click at its centre (one click)
//   click SPEC | dclick SPEC | rclick SPEC   a mouse click at its centre (one, two, one)
//   menu A>B>C             menu bar items pressed in turn (one click each)
//   finder CMD             a shell command that opens a file as Finder does (one input)
//   launch CMD             a shell command that starts the app (setup, no inputs)
//   sh CMD                 a shell action, such as starting a server (no inputs)
//   none                   no input: wait for the condition
// `setup N:` counts the step as N setup inputs (N defaults to the input's own
// count) and keeps its time out of the task's wall time. A leading `?` makes
// a press or click optional: if the element never shows, nothing is counted.
// A step whose input starts the app (launch or finder) finds the app's new pid
// by its executable (--exe), never touching a copy that was already running.
//
// SPEC is ROLE=TEXT (exact) or ROLE~TEXT (contains, any case), ROLE may be `*`,
// TEXT is matched against an element's title, value, and description, a
// trailing #N picks the Nth match, and alternatives are joined by ` || `.
//
// CONDITION is alternatives joined by ` || `, each terms joined by ` && `, an
// alternative optionally led by [label] (reported as what matched); a
// leading `?` on the whole condition makes a timeout "unverified", not a
// failure. Terms:
//   window | window~TEXT | window=TEXT   a window / the focused window's title
//   ax SPEC | !ax SPEC     an element exists / does not
//   focus ROLE | focus SPEC   the keyboard's element
//   sh CMD                 a shell command exits 0
//   port-closed N          nothing accepts on 127.0.0.1:N or [::1]:N
//   quiet MS               the tree used under 5% of a core for MS ms ("settled, unverified")
//   view NAME MAGIC        NAME's window is up and settled; then "opens as text" when a
//                          text area starts with MAGIC, "refused" when the app says it
//                          cannot show it, "shown" otherwise, "unverified" without an AX tree
//   none                   true at once
//
// Owner input stops the run: the state file holds the time of the last input
// event this benchmark knows of (the owner's last before the run, then each
// event this tool posts). An event newer than that (HIDIdleTime from
// IOHIDSystem) is someone at the keyboard or mouse, and `run` exits 3 at once.
//
// `run` prints, and writes to --out, a line a step (step, n, kind, inputs,
// input ms, wall ms, slowest poll ms, status, what matched, the step), a
// `pid` line once the app's pid is known, and a last `summary` line of
// key=value fields. The slowest poll is the step's time resolution: a poll
// that walks a large AX tree takes as long as the walk.
//
// Exit codes: 0 done, 1 a step failed, 2 usage or setup error, 3 owner input,
// 4 no Accessibility permission.
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <IOKit/IOKitLib.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ------------------------------------------------------------------ clocks

static double MonoMs(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static double WallMs(void) { return clock_gettime_nsec_np(CLOCK_REALTIME) / 1e6; }

static double gTickNs = 1;   // rusage's CPU times are mach ticks on Apple silicon

static void SleepMs(double ms) {
    if (ms > 0) usleep((useconds_t)(ms * 1000));
}

// ------------------------------------------------------------------ the owner

// Milliseconds since the last keyboard or mouse event, or -1.
static double HidIdleMs(void) {
    static io_service_t svc = 0;
    if (!svc) svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOHIDSystem"));
    if (!svc) return -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("HIDIdleTime"), kCFAllocatorDefault, 0);
    if (!v) return -1;
    int64_t ns = 0;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &ns);
    else if (CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)v) >= 8)
        memcpy(&ns, CFDataGetBytePtr((CFDataRef)v), 8);
    CFRelease(v);
    return ns / 1e6;
}

static NSString *gStatePath;
static double gLastKnownInput = 0;   // wall ms of the last input event we account for
static const double kGuardSlackMs = 300;

static void SaveState(void) {
    if (!gStatePath) return;
    [[NSString stringWithFormat:@"%.0f\n", gLastKnownInput] writeToFile:gStatePath atomically:YES
                                                               encoding:NSUTF8StringEncoding error:nil];
}

static void LoadState(void) {
    NSString *s = [NSString stringWithContentsOfFile:gStatePath encoding:NSUTF8StringEncoding error:nil];
    gLastKnownInput = s.doubleValue;
}

static void NotePosted(void) {
    gLastKnownInput = WallMs();
    SaveState();
}

// YES when an input event arrived that this benchmark did not post.
static BOOL OwnerTouched(void) {
    const double idle = HidIdleMs();
    if (idle < 0) return NO;
    return WallMs() - idle > gLastKnownInput + kGuardSlackMs;
}

// ------------------------------------------------------------------ processes

typedef pid_t (*RespFn)(pid_t);
static RespFn gResponsible;

typedef struct {
    pid_t pid, ppid, rpid;
} ProcLink;

// Every process this user can see, with its parent and responsible pids.
static NSData *ListLinks(void) {
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return [NSData data];
    pid_t *pids = calloc((size_t)n * 2, sizeof(pid_t));
    n = proc_listallpids(pids, (int)(n * 2 * sizeof(pid_t)));
    NSMutableData *out = [NSMutableData dataWithCapacity:(NSUInteger)n * sizeof(ProcLink)];
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        struct proc_bsdshortinfo b;
        if (proc_pidinfo(pids[i], PROC_PIDT_SHORTBSDINFO, 0, &b, sizeof b) != sizeof b) continue;
        ProcLink l = {pids[i], (pid_t)b.pbsi_ppid, gResponsible ? gResponsible(pids[i]) : pids[i]};
        [out appendBytes:&l length:sizeof l];
    }
    free(pids);
    return out;
}

// The root and every process whose parent or responsible pid is in the set.
static NSArray<NSNumber *> *TreeOf(pid_t root, NSData *links, NSMutableDictionary *ppidOut) {
    const ProcLink *l = links.bytes;
    const NSUInteger n = links.length / sizeof(ProcLink);
    NSMutableSet *in = [NSMutableSet setWithObject:@(root)];
    BOOL alive = NO;
    for (NSUInteger i = 0; i < n; i++)
        if (l[i].pid == root) alive = YES;
    if (!alive) return @[];
    BOOL changed;
    do {
        changed = NO;
        for (NSUInteger i = 0; i < n; i++)
            if (![in containsObject:@(l[i].pid)] &&
                ([in containsObject:@(l[i].ppid)] || [in containsObject:@(l[i].rpid)])) {
                [in addObject:@(l[i].pid)];
                changed = YES;
            }
    } while (changed);
    if (ppidOut)
        for (NSUInteger i = 0; i < n; i++)
            if ([in containsObject:@(l[i].pid)]) ppidOut[@(l[i].pid)] = @(l[i].ppid);
    return in.allObjects;
}

typedef struct {
    double cpuMs;       // user + system of the live tree, with what its members reaped
    double footprintMB; // physical footprint of the live tree
    int count;
} TreeSample;

// What exited members had used when last seen (launchd reaps a helper whose
// parent is launchd, so its time would otherwise vanish with it).
static NSMutableDictionary<NSNumber *, NSNumber *> *gOrphanCpu;
static double gGoneCpuMs = 0;

static TreeSample SampleTree(pid_t root) {
    TreeSample s = {0, 0, 0};
    if (root <= 0) return s;
    NSMutableDictionary *ppids = [NSMutableDictionary dictionary];
    NSArray *members = TreeOf(root, ListLinks(), ppids);
    if (!gOrphanCpu) gOrphanCpu = [NSMutableDictionary dictionary];
    NSMutableSet *seen = [NSMutableSet set];
    for (NSNumber *p in members) {
        struct rusage_info_v4 ru;
        if (proc_pid_rusage(p.intValue, RUSAGE_INFO_V4, (rusage_info_t *)&ru) != 0) continue;
        const double cpu = (double)(ru.ri_user_time + ru.ri_system_time + ru.ri_child_user_time +
                                    ru.ri_child_system_time) * gTickNs / 1e6;
        s.cpuMs += cpu;
        s.footprintMB += ru.ri_phys_footprint / 1048576.0;
        s.count++;
        if (p.intValue != root && [ppids[p] intValue] == 1) {
            gOrphanCpu[p] = @(cpu);
            [seen addObject:p];
        }
    }
    for (NSNumber *p in gOrphanCpu.allKeys)
        if (![seen containsObject:p]) {
            gGoneCpuMs += gOrphanCpu[p].doubleValue;
            [gOrphanCpu removeObjectForKey:p];
        }
    s.cpuMs += gGoneCpuMs;
    return s;
}

static NSString *ExePath(pid_t pid) {
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, buf, sizeof buf) <= 0) return nil;
    return @(buf);
}

static NSSet<NSNumber *> *PidsRunning(NSString *exe) {
    NSMutableSet *out = [NSMutableSet set];
    NSData *links = ListLinks();
    const ProcLink *l = links.bytes;
    for (NSUInteger i = 0; i < links.length / sizeof(ProcLink); i++)
        if ([ExePath(l[i].pid) isEqualToString:exe]) [out addObject:@(l[i].pid)];
    return out;
}

// ------------------------------------------------------------------ accessibility

typedef struct {
    NSString *role;
    NSString *text;
    BOOL exact;
    int nth;   // 1-based
} Spec;

static NSString *AXStr(AXUIElementRef e, CFStringRef attr) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(e, attr, &v) != kAXErrorSuccess || !v) return nil;
    NSString *s = CFGetTypeID(v) == CFStringGetTypeID() ? [(__bridge NSString *)v copy] : nil;
    CFRelease(v);
    return s;
}

static id AXObj(AXUIElementRef e, CFStringRef attr) {
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(e, attr, &v) != kAXErrorSuccess || !v) return nil;
    return CFBridgingRelease(v);
}

// The first characters of a text area, without copying all of it (a huge
// log's whole text through AXValue would cost the app the measurement).
static NSString *AXPrefix(AXUIElementRef e, CFIndex n) {
    CFRange r = CFRangeMake(0, n);
    NSNumber *count = AXObj(e, kAXNumberOfCharactersAttribute);
    if (count && count.longValue < n) r.length = count.longValue;
    AXValueRef range = AXValueCreate(kAXValueTypeCFRange, &r);
    CFTypeRef v = NULL;
    AXError err = AXUIElementCopyParameterizedAttributeValue(e, kAXStringForRangeParameterizedAttribute, range, &v);
    CFRelease(range);
    if (err != kAXErrorSuccess || !v) return nil;
    NSString *s = CFGetTypeID(v) == CFStringGetTypeID() ? [(__bridge NSString *)v copy] : nil;
    CFRelease(v);
    return s;
}

typedef struct {
    NSString *role, *title, *value, *desc;
    NSArray *kids;
} Node;

// Roles whose value is a short string worth reading. A text area's value is
// never read whole (a huge log's text through AXValue would cost the app the
// measurement); AXPrefix reads its start when a condition needs it.
static BOOL ShortValueRole(NSString *role) {
    static NSSet *roles;
    if (!roles)
        roles = [NSSet setWithArray:@[ @"AXStaticText", @"AXTextField", @"AXButton", @"AXCell", @"AXLink",
                                       @"AXHeading", @"AXRow", @"AXMenuItem", @"AXPopUpButton", @"AXCheckBox",
                                       @"AXRadioButton", @"AXTab", @"AXComboBox", @"AXSearchField" ]];
    return role && [roles containsObject:role];
}

// One round trip for role, title, description and children; one more for
// the value of a role that has a short one.
static Node NodeOf(AXUIElementRef e) {
    Node n = {nil, nil, nil, nil, nil};
    NSArray *attrs = @[ (__bridge id)kAXRoleAttribute, (__bridge id)kAXTitleAttribute,
                        (__bridge id)kAXDescriptionAttribute, (__bridge id)kAXChildrenAttribute ];
    CFArrayRef vals = NULL;
    if (AXUIElementCopyMultipleAttributeValues(e, (__bridge CFArrayRef)attrs, 0, &vals) == kAXErrorSuccess && vals) {
        NSArray *a = CFBridgingRelease(vals);
        id r = a.count > 0 ? a[0] : nil, t = a.count > 1 ? a[1] : nil, d = a.count > 2 ? a[2] : nil,
           k = a.count > 3 ? a[3] : nil;
        if ([r isKindOfClass:NSString.class]) n.role = r;
        if ([t isKindOfClass:NSString.class]) n.title = t;
        if ([d isKindOfClass:NSString.class]) n.desc = d;
        if ([k isKindOfClass:NSArray.class]) n.kids = k;
    }
    if (ShortValueRole(n.role)) {
        id v = AXObj(e, kAXValueAttribute);
        if ([v isKindOfClass:NSString.class]) n.value = [(NSString *)v length] > 2000 ? [v substringToIndex:2000] : v;
    }
    return n;
}

static BOOL TextMatches(NSString *have, NSString *want, BOOL exact) {
    if (!have.length) return NO;
    if (exact) return [have isEqualToString:want];
    return [have rangeOfString:want options:NSCaseInsensitiveSearch].location != NSNotFound;
}

static BOOL NodeMatches(Node n, Spec s) {
    if (s.role.length && ![s.role isEqualToString:@"*"] && ![n.role isEqualToString:s.role]) return NO;
    if (!s.text.length) return YES;
    return TextMatches(n.title, s.text, s.exact) || TextMatches(n.value, s.text, s.exact) ||
           TextMatches(n.desc, s.text, s.exact);
}

static NSArray *Windows(AXUIElementRef app) {
    NSArray *w = AXObj(app, kAXWindowsAttribute);
    return [w isKindOfClass:NSArray.class] ? w : @[];
}

static id FocusedWindow(AXUIElementRef app) {
    id w = AXObj(app, kAXFocusedWindowAttribute);
    if (!w) w = AXObj(app, kAXMainWindowAttribute);
    if (!w) w = Windows(app).firstObject;
    return w;
}

// Breadth first through the given roots. The visitor returns NO to stop.
static int Walk(NSArray *roots, int maxNodes, BOOL (^visit)(AXUIElementRef e, Node n, int depth)) {
    NSMutableArray *queue = [NSMutableArray array];
    for (id r in roots) [queue addObject:@[ r, @0 ]];
    int seen = 0;
    for (NSUInteger i = 0; i < queue.count && seen < maxNodes; i++) {
        AXUIElementRef e = (__bridge AXUIElementRef)queue[i][0];
        int depth = [queue[i][1] intValue];
        Node n = NodeOf(e);
        seen++;
        if (!visit(e, n, depth)) break;
        if (depth > 80) continue;
        for (id k in n.kids) [queue addObject:@[ k, @(depth + 1) ]];
        if (i > 4096) {   // keep the queue short
            [queue removeObjectsInRange:NSMakeRange(0, i + 1)];
            i = (NSUInteger)-1;
        }
    }
    return seen;
}

static const int kMaxNodes = 40000;

static Spec ParseSpec(NSString *s) {
    Spec sp = {@"*", @"", NO, 1};
    s = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSRange hash = [s rangeOfString:@"#" options:NSBackwardsSearch];
    if (hash.location != NSNotFound) {
        NSString *num = [s substringFromIndex:hash.location + 1];
        if (num.length && [num rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location == NSNotFound) {
            sp.nth = MAX(1, num.intValue);
            s = [s substringToIndex:hash.location];
        }
    }
    NSRange eq = [s rangeOfString:@"="], ti = [s rangeOfString:@"~"];
    NSRange op = eq.location < ti.location ? eq : ti;
    if (op.location == NSNotFound) {
        sp.role = s;
        return sp;
    }
    sp.role = [s substringToIndex:op.location];
    sp.text = [s substringFromIndex:op.location + 1];
    sp.exact = op.location == eq.location;
    if (!sp.role.length) sp.role = @"*";
    return sp;
}

// The element a spec (alternatives joined by " || ") names, searched in the
// focused window first, then every window.
static id FindElement(AXUIElementRef app, NSString *specs) {
    for (NSString *one in [specs componentsSeparatedByString:@" || "]) {
        Spec s = ParseSpec(one);
        id fw = FocusedWindow(app);
        NSMutableArray *roots = [NSMutableArray array];
        if (fw) [roots addObject:fw];
        for (id w in Windows(app))
            if (!fw || !CFEqual((__bridge CFTypeRef)w, (__bridge CFTypeRef)fw)) [roots addObject:w];
        __block id found = nil;
        __block int matches = 0;
        Walk(roots, kMaxNodes, ^BOOL(AXUIElementRef e, Node n, int depth) {
            (void)depth;
            if (NodeMatches(n, s) && ++matches == s.nth) {
                found = (__bridge id)e;
                return NO;
            }
            return YES;
        });
        if (found) return found;
    }
    return nil;
}

static BOOL ElementCentre(id el, CGPoint *out) {
    id pv = AXObj((__bridge AXUIElementRef)el, kAXPositionAttribute);
    id sv = AXObj((__bridge AXUIElementRef)el, kAXSizeAttribute);
    CGPoint p;
    CGSize sz;
    if (!pv || !sv || !AXValueGetValue((__bridge AXValueRef)pv, kAXValueTypeCGPoint, &p) ||
        !AXValueGetValue((__bridge AXValueRef)sv, kAXValueTypeCGSize, &sz))
        return NO;
    *out = CGPointMake(p.x + sz.width / 2, p.y + sz.height / 2);
    return YES;
}

// ------------------------------------------------------------------ input

static CGEventSourceRef gSource;

static const struct {
    const char *name;
    CGKeyCode code;
} kKeys[] = {
    {"a", 0},       {"s", 1},         {"d", 2},          {"f", 3},       {"h", 4},        {"g", 5},
    {"z", 6},       {"x", 7},         {"c", 8},          {"v", 9},       {"b", 11},       {"q", 12},
    {"w", 13},      {"e", 14},        {"r", 15},         {"y", 16},      {"t", 17},       {"1", 18},
    {"2", 19},      {"3", 20},        {"4", 21},         {"6", 22},      {"5", 23},       {"=", 24},
    {"9", 25},      {"7", 26},        {"-", 27},         {"8", 28},      {"0", 29},       {"]", 30},
    {"o", 31},      {"u", 32},        {"[", 33},         {"i", 34},      {"p", 35},       {"l", 37},
    {"j", 38},      {"'", 39},        {"k", 40},         {";", 41},      {"\\", 42},      {",", 43},
    {"/", 44},      {"n", 45},        {"m", 46},         {".", 47},      {"`", 50},       {"return", 36},
    {"enter", 36},  {"tab", 48},      {"space", 49},     {"delete", 51}, {"backspace", 51}, {"esc", 53},
    {"escape", 53}, {"fwddelete", 117}, {"home", 115},   {"end", 119},   {"pageup", 116}, {"pagedown", 121},
    {"left", 123},  {"right", 124},   {"down", 125},     {"up", 126},    {"f1", 122},     {"f2", 120},
    {"f3", 99},     {"f4", 118},      {"f5", 96},        {"f6", 97},     {"f7", 98},      {"f8", 100},
    {"f9", 101},    {"f10", 109},     {"f11", 103},      {"f12", 111},
};

static int KeyCode(NSString *name) {
    const char *c = name.lowercaseString.UTF8String;
    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
        if (!strcmp(kKeys[i].name, c)) return kKeys[i].code;
    return -1;
}

static const struct {
    CGEventFlags flag;
    CGKeyCode code;
} kMods[] = {{kCGEventFlagMaskControl, 59}, {kCGEventFlagMaskAlternate, 58},
             {kCGEventFlagMaskShift, 56},   {kCGEventFlagMaskCommand, 55}};

static void Post(CGEventRef e) {
    CGEventSetIntegerValueField(e, kCGEventSourceUserData, 0x7a5b);   // ours
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

// One key with modifiers held around it; `ch` (when not 0) is the text it types.
static void PostKey(CGKeyCode code, CGEventFlags flags, UniChar ch) {
    CGEventFlags cur = 0;
    for (int i = 0; i < 4; i++)
        if (flags & kMods[i].flag) {
            cur |= kMods[i].flag;
            CGEventRef m = CGEventCreateKeyboardEvent(gSource, kMods[i].code, true);
            CGEventSetFlags(m, cur);
            Post(m);
        }
    CGEventRef d = CGEventCreateKeyboardEvent(gSource, code, true);
    CGEventSetFlags(d, cur);
    if (ch) CGEventKeyboardSetUnicodeString(d, 1, &ch);
    Post(d);
    CGEventRef u = CGEventCreateKeyboardEvent(gSource, code, false);
    CGEventSetFlags(u, cur);
    if (ch) CGEventKeyboardSetUnicodeString(u, 1, &ch);
    Post(u);
    for (int i = 3; i >= 0; i--)
        if (flags & kMods[i].flag) {
            cur &= ~kMods[i].flag;
            CGEventRef m = CGEventCreateKeyboardEvent(gSource, kMods[i].code, false);
            CGEventSetFlags(m, cur);
            Post(m);
        }
    NotePosted();
}

static BOOL PostChord(NSString *chord) {
    CGEventFlags flags = 0;
    NSArray *parts = [chord componentsSeparatedByString:@"+"];
    NSString *key = parts.lastObject;
    for (NSUInteger i = 0; i + 1 < parts.count; i++) {
        NSString *m = [parts[i] lowercaseString];
        if ([m isEqualToString:@"cmd"] || [m isEqualToString:@"command"]) flags |= kCGEventFlagMaskCommand;
        else if ([m isEqualToString:@"shift"]) flags |= kCGEventFlagMaskShift;
        else if ([m isEqualToString:@"alt"] || [m isEqualToString:@"opt"] || [m isEqualToString:@"option"])
            flags |= kCGEventFlagMaskAlternate;
        else if ([m isEqualToString:@"ctrl"] || [m isEqualToString:@"control"]) flags |= kCGEventFlagMaskControl;
        else return NO;
    }
    int code = KeyCode(key);
    if (code < 0) return NO;
    PostKey((CGKeyCode)code, flags, 0);
    return YES;
}

static const char *kShifted = "!@#$%^&*()_+{}|:\"<>?~";
static const char *kUnshifted = "1234567890-=[]\\;',./`";

static double gKeyGapMs = 6;   // between typed keystrokes

static BOOL Frontmost(void);
static NSString *const kNotInFront = @"error: the app would not come to the front, so nothing was sent";

static NSUInteger TypeText(NSString *text) {
    NSUInteger n = 0;
    for (NSUInteger i = 0; i < text.length; i++) {
        UniChar ch = [text characterAtIndex:i];
        CGEventFlags flags = 0;
        int code = 0;
        char c = ch < 128 ? (char)ch : 0;
        if (c >= 'A' && c <= 'Z') {
            flags = kCGEventFlagMaskShift;
            code = KeyCode([NSString stringWithFormat:@"%c", c + 32]);
        } else if (c == ' ') {
            code = 49;
        } else if (c && strchr(kShifted, c)) {
            flags = kCGEventFlagMaskShift;
            code = KeyCode([NSString stringWithFormat:@"%c", kUnshifted[strchr(kShifted, c) - kShifted]]);
        } else if (c) {
            code = KeyCode([NSString stringWithFormat:@"%c", c]);
        }
        if (code < 0) code = 0;
        if (!Frontmost()) break;   // something else came forward: stop typing into it
        PostKey((CGKeyCode)code, flags, ch);
        n++;
        SleepMs(gKeyGapMs);
    }
    return n;
}

static void PostClick(CGPoint p, CGMouseButton button, int clicks) {
    CGEventType down = button == kCGMouseButtonRight ? kCGEventRightMouseDown : kCGEventLeftMouseDown;
    CGEventType up = button == kCGMouseButtonRight ? kCGEventRightMouseUp : kCGEventLeftMouseUp;
    Post(CGEventCreateMouseEvent(gSource, kCGEventMouseMoved, p, button));
    SleepMs(15);
    for (int i = 1; i <= clicks; i++) {
        CGEventRef d = CGEventCreateMouseEvent(gSource, down, p, button);
        CGEventSetIntegerValueField(d, kCGMouseEventClickState, i);
        Post(d);
        CGEventRef u = CGEventCreateMouseEvent(gSource, up, p, button);
        CGEventSetIntegerValueField(u, kCGMouseEventClickState, i);
        Post(u);
        if (i < clicks) SleepMs(40);
    }
    NotePosted();
}

// ------------------------------------------------------------------ the app

static pid_t gPid = 0;
static NSString *gExe;
static AXUIElementRef gApp;
static NSSet *gBefore;   // pids of gExe running before the app was started

static void Adopt(pid_t pid) {
    gPid = pid;
    if (gApp) CFRelease(gApp);
    gApp = AXUIElementCreateApplication(pid);
    // Chromium (VS Code) builds its accessibility tree only when asked.
    AXUIElementSetAttributeValue(gApp, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
}

static void LookForNewPid(void) {
    if (gPid || !gBefore) return;
    pid_t best = 0;
    for (NSNumber *p in PidsRunning(gExe))
        if (![gBefore containsObject:p] && p.intValue > best) best = p.intValue;
    if (best) Adopt(best);
}

// Asked of the system through AX each time: NSWorkspace's answer is updated
// by notifications this tool, with no run loop, never receives.
static BOOL Frontmost(void) {
    static AXUIElementRef sys;
    if (!sys) sys = AXUIElementCreateSystemWide();
    id app = AXObj(sys, kAXFocusedApplicationAttribute);
    pid_t pid = 0;
    return app && AXUIElementGetPid((__bridge AXUIElementRef)app, &pid) == kAXErrorSuccess && pid == gPid;
}

// Bring the app forward before input; the time is not counted. NO when it
// would not come forward: then nothing is posted, since keys and clicks go
// to whatever app is in front.
static BOOL Activate(void) {
    if (!gPid) return NO;
    if (Frontmost()) return YES;
    AXUIElementSetAttributeValue(gApp, kAXFrontmostAttribute, kCFBooleanTrue);
    id w = FocusedWindow(gApp);
    if (w) AXUIElementPerformAction((__bridge AXUIElementRef)w, kAXRaiseAction);
    [[NSRunningApplication runningApplicationWithProcessIdentifier:gPid] activateWithOptions:0];
    const double until = MonoMs() + 3000;
    while (!Frontmost() && MonoMs() < until) SleepMs(10);
    return Frontmost();
}

// posix_spawn and waitpid: NSTask's waitUntilExit took about 70 ms a call,
// which made a polled shell condition far coarser than 10 ms.
static int RunShell(NSString *cmd) {
    extern char **environ;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    const char *argv[] = {"/bin/sh", "-c", cmd.UTF8String, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, "/bin/sh", &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return 127;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static BOOL PortOpen(int port) {
    BOOL open = NO;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a4 = {.sin_len = sizeof a4, .sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, "127.0.0.1", &a4.sin_addr);
    if (s >= 0 && connect(s, (struct sockaddr *)&a4, sizeof a4) == 0) open = YES;
    if (s >= 0) close(s);
    if (open) return YES;
    s = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a6 = {.sin6_len = sizeof a6, .sin6_family = AF_INET6, .sin6_port = htons(port)};
    a6.sin6_addr = in6addr_loopback;
    if (s >= 0 && connect(s, (struct sockaddr *)&a6, sizeof a6) == 0) open = YES;
    if (s >= 0) close(s);
    return open;
}

// ------------------------------------------------------------------ measuring

static double gPeakMB = 0, gLastSampleAt = 0;
static BOOL gMeasuring = NO;
static TreeSample gLast;
static NSMutableArray<NSArray<NSNumber *> *> *gCpuRing;   // [t, cpu]

static void Sample(void) {
    gLast = SampleTree(gPid);
    gLastSampleAt = MonoMs();
    if (gMeasuring && gLast.footprintMB > gPeakMB) gPeakMB = gLast.footprintMB;
    if (!gCpuRing) gCpuRing = [NSMutableArray array];
    [gCpuRing addObject:@[ @(gLastSampleAt), @(gLast.cpuMs) ]];
    while (gCpuRing.count > 2 && gLastSampleAt - gCpuRing[1][0].doubleValue > 60000) [gCpuRing removeObjectAtIndex:0];
}

// YES when the tree has used under 5% of a core over the last `ms`.
// Only samples since the step's input count, so a step's quiet never comes
// from before the app saw the input.
static double gQuietFrom = 0;

static BOOL Quiet(double ms) {
    if (!gPid || gCpuRing.count < 2) return NO;
    NSArray *last = gCpuRing.lastObject;
    for (NSInteger i = (NSInteger)gCpuRing.count - 2; i >= 0; i--) {
        if (gCpuRing[i][0].doubleValue < gQuietFrom) break;
        double dt = [last[0] doubleValue] - gCpuRing[i][0].doubleValue;
        if (dt >= ms) return ([last[1] doubleValue] - gCpuRing[i][1].doubleValue) < 0.05 * dt;
    }
    return NO;
}

// ------------------------------------------------------------------ conditions

static NSRegularExpression *gRefused;

// "unverified" when the window shows no accessibility tree to read.
static NSString *ClassifyView(NSString *magic) {
    id w = FocusedWindow(gApp);
    if (!w) return nil;
    __block BOOL refused = NO, text = NO;
    __block int nodes = 0, readable = 0;
    Walk(@[ w ], kMaxNodes, ^BOOL(AXUIElementRef e, Node n, int depth) {
        (void)depth;
        nodes++;
        for (NSString *s in @[ n.title ?: @"", n.value ?: @"", n.desc ?: @"" ])
            if (s.length) {
                readable++;
                if ([gRefused firstMatchInString:s options:0 range:NSMakeRange(0, s.length)]) refused = YES;
            }
        if ([n.role isEqualToString:@"AXTextArea"]) {
            readable++;
            NSString *pre = AXPrefix(e, 4096);
            if (pre && magic.length && [pre rangeOfString:magic].location != NSNotFound) text = YES;
        }
        return !(refused || text);
    });
    if (refused) return @"refused";
    if (text) return @"opens as text";
    if (nodes < 15 || readable < 3) return @"unverified (no accessibility tree)";
    return @"shown";
}

static NSString *gViewResult;

// One term. `state` keeps a view term's timing between polls.
static BOOL Term(NSString *t, NSMutableDictionary *state) {
    t = [t stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if ([t isEqualToString:@"none"]) return YES;
    if ([t hasPrefix:@"sh "]) return RunShell([t substringFromIndex:3]) == 0;
    if ([t hasPrefix:@"port-closed "]) return !PortOpen([t substringFromIndex:12].intValue);
    if ([t hasPrefix:@"quiet "]) {
        if (!gPid) return NO;
        return Quiet([t substringFromIndex:6].doubleValue);
    }
    if (!gPid) return NO;
    if ([t isEqualToString:@"window"]) return Windows(gApp).count > 0;
    if ([t hasPrefix:@"window~"] || [t hasPrefix:@"window="]) {
        NSString *want = [t substringFromIndex:7];
        NSString *title = AXStr((__bridge AXUIElementRef)FocusedWindow(gApp), kAXTitleAttribute);
        return TextMatches(title, want, [t characterAtIndex:6] == '=');
    }
    if ([t hasPrefix:@"ax "]) return FindElement(gApp, [t substringFromIndex:3]) != nil;
    if ([t hasPrefix:@"!ax "]) return FindElement(gApp, [t substringFromIndex:4]) == nil;
    if ([t hasPrefix:@"focus "]) {
        id f = AXObj(gApp, kAXFocusedUIElementAttribute);
        if (!f) return NO;
        Spec s = ParseSpec([t substringFromIndex:6]);
        return NodeMatches(NodeOf((__bridge AXUIElementRef)f), s);
    }
    if ([t hasPrefix:@"view "]) {
        NSArray *a = [[t substringFromIndex:5] componentsSeparatedByString:@" "];
        NSString *name = a.firstObject, *magic = a.count > 1 ? [[a subarrayWithRange:NSMakeRange(1, a.count - 1)] componentsJoinedByString:@" "] : @"";
        NSString *title = AXStr((__bridge AXUIElementRef)FocusedWindow(gApp), kAXTitleAttribute);
        if (!TextMatches(title, name, NO)) return NO;
        if (!state[@"seen"]) state[@"seen"] = @(MonoMs());
        // Settled: quiet for 300 ms, or 3 s after its window came up.
        if (!Quiet(300) && MonoMs() - [state[@"seen"] doubleValue] < 3000) return NO;
        gViewResult = ClassifyView(magic);
        return gViewResult != nil;
    }
    fprintf(stderr, "taskbench: unknown condition term \"%s\"\n", t.UTF8String);
    return NO;
}

// The label of the alternative that holds, or nil.
static NSString *Condition(NSString *cond, NSMutableDictionary *state) {
    NSArray *alts = [cond componentsSeparatedByString:@" || "];
    for (NSUInteger i = 0; i < alts.count; i++) {
        NSString *alt = [alts[i] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSString *label = nil;
        if ([alt hasPrefix:@"["]) {
            NSRange close = [alt rangeOfString:@"]"];
            if (close.location != NSNotFound) {
                label = [alt substringWithRange:NSMakeRange(1, close.location - 1)];
                alt = [[alt substringFromIndex:close.location + 1]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            }
        }
        NSMutableDictionary *st = state[@(i)] ?: (state[@(i)] = [NSMutableDictionary dictionary]);
        BOOL all = YES;
        gViewResult = nil;
        for (NSString *term in [alt componentsSeparatedByString:@" && "])
            if (!Term(term, st)) {
                all = NO;
                break;
            }
        if (!all) continue;
        if (gViewResult) return label ? [NSString stringWithFormat:@"%@: %@", label, gViewResult] : gViewResult;
        if (label) return label;
        if ([alt hasPrefix:@"quiet "]) return @"settled (unverified)";
        return alts.count > 1 ? [@"ok: " stringByAppendingString:alt] : @"ok";
    }
    return nil;
}

// ------------------------------------------------------------------ steps

typedef struct {
    int clicks, chords, keys, opens;
} Counts;

static int CountTotal(Counts c) { return c.clicks + c.chords + c.keys + c.opens; }

static BOOL gAborted = NO;
static double gPollMaxMs = 0;   // the last condition's slowest poll: its time resolution

// Poll until the condition holds or the deadline. Returns the matched label,
// "" when it timed out, nil when the owner touched the keyboard or mouse.
static NSString *Await(NSString *cond, double timeoutMs) {
    NSMutableDictionary *state = [NSMutableDictionary dictionary];
    gQuietFrom = MonoMs();
    const double deadline = MonoMs() + timeoutMs;
    const BOOL quietTerms = [cond rangeOfString:@"quiet "].location != NSNotFound ||
                            [cond rangeOfString:@"view "].location != NSNotFound;
    gPollMaxMs = 0;
    for (;;) {
        const double tick = MonoMs();
        if (OwnerTouched()) {
            gAborted = YES;
            return nil;
        }
        LookForNewPid();
        if (gPid && (quietTerms || tick - gLastSampleAt >= 100)) Sample();
        NSString *m = Condition(cond, state);
        const double took = MonoMs() - tick;
        if (took > gPollMaxMs) gPollMaxMs = took;
        if (m) return m;
        if (MonoMs() > deadline) return @"";
        // Every 10 ms; a poll that walked a large AX tree took longer, and
        // the app gets a third of that again before the next, so the
        // benchmark's questions do not keep it busy.
        SleepMs(MAX(10 - took, took / 3));
    }
}

// Find an element for a press or click, waiting up to `ms` for it.
static id AwaitElement(NSString *spec, double ms) {
    const double deadline = MonoMs() + ms;
    for (;;) {
        if (OwnerTouched()) {
            gAborted = YES;
            return nil;
        }
        LookForNewPid();
        if (gPid) {
            id el = FindElement(gApp, spec);
            if (el) return el;
        }
        if (MonoMs() > deadline) return nil;
        SleepMs(10);
    }
}

static FILE *gOut;

static void Emit(NSString *line) {
    printf("%s\n", line.UTF8String);
    if (gOut) {
        fprintf(gOut, "%s\n", line.UTF8String);
        fflush(gOut);
    }
    fflush(stdout);
}

static NSString *Clean(NSString *s) {
    return [[s stringByReplacingOccurrencesOfString:@"\t" withString:@" "]
        stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
}

static int Run(NSString *exe, NSString *stepsPath, NSString *outPath) {
    gExe = exe;
    NSString *text = [NSString stringWithContentsOfFile:stepsPath encoding:NSUTF8StringEncoding error:nil];
    if (!text) {
        fprintf(stderr, "taskbench: cannot read %s\n", stepsPath.UTF8String);
        return 2;
    }
    if (outPath) gOut = fopen(outPath.fileSystemRepresentation, "w");
    gRefused = [NSRegularExpression
        regularExpressionWithPattern:@"not displayed in the text editor|binary files are not supported|"
                                     @"could not open|cannot open|can't open|unsupported (file|format)|"
                                     @"no editor (is )?available"
                             options:NSRegularExpressionCaseInsensitive error:nil];
    gSource = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (OwnerTouched()) {
        Emit(@"aborted\tinput from the keyboard or mouse before the run");
        return 3;
    }

    Counts task = {0, 0, 0, 0};
    int setupInputs = 0;
    double taskWall = 0, setupWall = 0, launchMs = 0;
    double cpuStart = -1, cpuEnd = 0;
    BOOL failed = NO, started = NO, viewUnverified = NO, lastUnverified = NO, lastSettled = NO;
    NSMutableArray *matched = [NSMutableArray array];
    int n = 0;
    NSRegularExpression *timeoutRe = [NSRegularExpression regularExpressionWithPattern:@"\\s@([0-9.]+)$" options:0 error:nil];

    for (NSString *raw in [text componentsSeparatedByString:@"\n"]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (!line.length || [line hasPrefix:@"#"]) continue;
        n++;
        double timeoutMs = 15000;
        NSTextCheckingResult *tm = [timeoutRe firstMatchInString:line options:0 range:NSMakeRange(0, line.length)];
        if (tm) {
            timeoutMs = [[line substringWithRange:[tm rangeAtIndex:1]] doubleValue] * 1000;
            line = [line substringToIndex:tm.range.location];
        }
        BOOL setup = NO;
        int setupCount = -1;
        if ([line hasPrefix:@"setup"]) {
            NSRange colon = [line rangeOfString:@":"];
            if (colon.location != NSNotFound) {
                NSString *num = [[line substringWithRange:NSMakeRange(5, colon.location - 5)]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                if (num.length) setupCount = num.intValue;
                setup = YES;
                line = [[line substringFromIndex:colon.location + 1]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            }
        }
        NSString *input = line, *cond = @"none";
        NSRange arrow = [line rangeOfString:@" => "];
        if (arrow.location != NSNotFound) {
            input = [line substringToIndex:arrow.location];
            cond = [line substringFromIndex:arrow.location + 4];
        }
        BOOL softCond = [cond hasPrefix:@"?"];
        if (softCond) cond = [cond substringFromIndex:1];
        BOOL optional = [input hasPrefix:@"?"];
        if (optional) input = [input substringFromIndex:1];
        NSRange sp = [input rangeOfString:@" "];
        NSString *verb = sp.location == NSNotFound ? input : [input substringToIndex:sp.location];
        NSString *arg = sp.location == NSNotFound ? @"" : [input substringFromIndex:sp.location + 1];

        // The task's measurement starts with its first counted step.
        if (!setup && ![verb isEqualToString:@"launch"] && !started) {
            started = YES;
            if (gPid) Sample();
            cpuStart = gPid ? gLast.cpuMs : 0;
            gPeakMB = gPid ? gLast.footprintMB : 0;
            gMeasuring = YES;
        }

        Counts c = {0, 0, 0, 0};
        NSString *status = @"ok";
        const double t0 = MonoMs();
        if ([verb isEqualToString:@"launch"] || [verb isEqualToString:@"finder"]) {
            if (!gPid) gBefore = PidsRunning(exe);
            if (RunShell(arg) != 0) status = @"error: the command failed";
            if ([verb isEqualToString:@"finder"]) c.opens = 1;
        } else if ([verb isEqualToString:@"sh"]) {
            if (RunShell(arg) != 0) status = @"error: the command failed";
        } else if ([verb isEqualToString:@"key"]) {
            LookForNewPid();
            for (NSString *chord in [arg componentsSeparatedByString:@" "]) {
                if (!chord.length) continue;
                if (!Activate()) {
                    status = kNotInFront;
                    break;
                }
                if (!PostChord(chord)) status = [NSString stringWithFormat:@"error: unknown key %@", chord];
                c.chords++;
                SleepMs(gKeyGapMs);
            }
        } else if ([verb isEqualToString:@"type"]) {
            LookForNewPid();
            if (!Activate()) status = kNotInFront;
            else {
                c.keys = (int)TypeText(arg);
                if ((NSUInteger)c.keys < arg.length) status = kNotInFront;
            }
        } else if ([verb isEqualToString:@"press"] || [verb isEqualToString:@"click"] ||
                   [verb isEqualToString:@"dclick"] || [verb isEqualToString:@"rclick"]) {
            id el = AwaitElement(arg, optional ? 2000 : timeoutMs);
            if (gAborted) break;
            if (!el) {
                status = optional ? @"skipped: not shown" : @"error: element not found";
            } else if (![verb isEqualToString:@"press"] && !Activate()) {
                status = kNotInFront;
            } else {
                BOOL done = NO;
                if ([verb isEqualToString:@"press"])
                    done = AXUIElementPerformAction((__bridge AXUIElementRef)el, kAXPressAction) == kAXErrorSuccess;
                if (done) NotePosted();
                CGPoint p;
                if (!done && !Activate()) {
                    status = kNotInFront;
                    done = YES;
                } else if (!done && ElementCentre(el, &p)) {
                    PostClick(p, [verb isEqualToString:@"rclick"] ? kCGMouseButtonRight : kCGMouseButtonLeft,
                              [verb isEqualToString:@"dclick"] ? 2 : 1);
                    done = YES;
                }
                if (!done) status = @"error: element has no press action or frame";
                if ([status isEqualToString:@"ok"]) c.clicks = [verb isEqualToString:@"dclick"] ? 2 : 1;
            }
        } else if ([verb isEqualToString:@"menu"]) {
            id parent = gApp ? AXObj(gApp, kAXMenuBarAttribute) : nil;
            for (NSString *title in [arg componentsSeparatedByString:@">"]) {
                __block id item = nil;
                NSString *want = [title stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                if (parent)
                    Walk(@[ parent ], 2000, ^BOOL(AXUIElementRef e, Node nd, int depth) {
                        const BOOL isItem = [nd.role isEqualToString:@"AXMenuItem"] ||
                                            [nd.role isEqualToString:@"AXMenuBarItem"];
                        if (depth > 0 && depth <= 2 && isItem && TextMatches(nd.title, want, YES)) {
                            item = (__bridge id)e;
                            return NO;
                        }
                        return YES;
                    });
                if (!item) {
                    status = [NSString stringWithFormat:@"error: no menu item %@", want];
                    break;
                }
                AXUIElementPerformAction((__bridge AXUIElementRef)item, kAXPressAction);
                NotePosted();
                c.clicks++;
                parent = item;
            }
        } else if (![verb isEqualToString:@"none"]) {
            status = [NSString stringWithFormat:@"error: unknown input %@", verb];
        }
        const double inputMs = MonoMs() - t0;
        const double t1 = MonoMs();
        NSString *m = @"";
        if (gAborted) break;
        // A step read from the app's CPU settling, or a soft condition that
        // never held, is unverified. The task is unverified when its last
        // step is, or when a view could not be read; a step on the way that
        // only waited for the app to settle leaves the result verified.
        BOOL stepUnverified = NO, settledOnly = NO;
        if ([status isEqualToString:@"ok"]) {
            m = Await(cond, timeoutMs);
            if (!m) break;   // the owner
            if (!m.length) {
                status = softCond ? @"unverified" : @"timeout";
                stepUnverified = softCond;
            } else if ([m hasPrefix:@"settled"]) {
                stepUnverified = settledOnly = YES;
            } else if ([m rangeOfString:@"unverified"].location != NSNotFound) {
                stepUnverified = YES;
                if (!setup) viewUnverified = YES;
            }
        }
        const double wall = MonoMs() - t1;
        const BOOL skipped = [status hasPrefix:@"skipped"];
        if (skipped) c = (Counts){0, 0, 0, 0};
        if ([verb isEqualToString:@"launch"]) {
            launchMs += inputMs + wall;
        } else if (setup) {
            setupInputs += setupCount >= 0 && !skipped ? setupCount : CountTotal(c);
            setupWall += inputMs + wall;
        } else {
            task.clicks += c.clicks;
            task.chords += c.chords;
            task.keys += c.keys;
            task.opens += c.opens;
            taskWall += wall;
            if (m.length && ![m hasPrefix:@"ok"] && !settledOnly) [matched addObject:Clean(m)];
            if ([status isEqualToString:@"unverified"])
                [matched addObject:[NSString stringWithFormat:@"step %d not seen", n]];
            lastUnverified = stepUnverified;
            lastSettled = settledOnly;
        }
        if (gPid) Sample();
        if (!setup && ![verb isEqualToString:@"launch"]) cpuEnd = gLast.cpuMs;
        Emit([NSString stringWithFormat:@"step\t%d\t%@\t%d\t%.0f\t%.0f\t%.0f\t%@\t%@\t%@", n,
                                        setup ? @"setup" : [verb isEqualToString:@"launch"] ? @"launch" : @"task",
                                        CountTotal(c), inputMs, wall, gPollMaxMs, status, Clean(m), Clean(line)]);
        gPollMaxMs = 0;
        if (gPid) Emit([NSString stringWithFormat:@"pid\t%d", gPid]);
        if (![status isEqualToString:@"ok"] && ![status isEqualToString:@"unverified"] && !skipped) {
            failed = YES;
            break;
        }
    }
    if (gAborted) {
        if (gPid) Emit([NSString stringWithFormat:@"pid\t%d", gPid]);
        Emit(@"aborted\tinput from the keyboard or mouse during the run");
        return 3;
    }
    gMeasuring = NO;
    // Settled: once the tree has been quiet for a second (30 s at most).
    double settled = 0;
    if (gPid) {
        gQuietFrom = MonoMs();
        const double until = MonoMs() + 30000;
        while (MonoMs() < until) {
            if (OwnerTouched()) {
                Emit([NSString stringWithFormat:@"pid\t%d", gPid]);
                Emit(@"aborted\tinput from the keyboard or mouse during the run");
                return 3;
            }
            Sample();
            if (Quiet(1000)) break;
            SleepMs(50);
        }
        settled = gLast.footprintMB;
    }
    const double cpu = cpuStart >= 0 ? cpuEnd - cpuStart : 0;
    const BOOL unverified = viewUnverified || lastUnverified;
    if (lastSettled) [matched addObject:@"the result was read from the app's CPU settling"];
    Emit([NSString stringWithFormat:
                       @"summary\tstatus=%@\tpid=%d\twall_ms=%.0f\tinputs=%d\tclicks=%d\tchords=%d\tkeys=%d\topens=%d\t"
                       @"setup_inputs=%d\tsetup_ms=%.0f\tlaunch_ms=%.0f\tcpu_ms=%.0f\tpeak_mb=%.0f\tsettled_mb=%.0f\t"
                       @"matched=%@",
                       failed ? @"failed" : unverified ? @"unverified" : @"ok", gPid, taskWall, CountTotal(task),
                       task.clicks, task.chords, task.keys, task.opens, setupInputs, setupWall, launchMs, cpu,
                       gPeakMB, settled, [matched componentsJoinedByString:@"; "]]);
    if (gOut) fclose(gOut);
    return failed ? 1 : 0;
}

// ------------------------------------------------------------------ quit

static int Quit(NSString *exe, pid_t pid) {
    NSString *path = ExePath(pid);
    if (!path) return 0;   // already gone
    if (![path isEqualToString:exe]) {
        fprintf(stderr, "taskbench: pid %d is %s, not %s; left alone\n", pid, path.UTF8String, exe.UTF8String);
        return 2;
    }
    NSArray *members = TreeOf(pid, ListLinks(), nil);
    kill(pid, SIGTERM);
    double until = MonoMs() + 8000;
    while (ExePath(pid) && MonoMs() < until) SleepMs(50);
    for (NSNumber *p in members)
        if (p.intValue != pid) kill(p.intValue, SIGTERM);
    until = MonoMs() + 3000;
    for (;;) {
        BOOL any = NO;
        for (NSNumber *p in members)
            if (kill(p.intValue, 0) == 0) any = YES;
        if (!any || MonoMs() > until) break;
        SleepMs(50);
    }
    for (NSNumber *p in members)
        if (kill(p.intValue, 0) == 0) kill(p.intValue, SIGKILL);
    return 0;
}

// ------------------------------------------------------------------ axdump

static int AxDump(pid_t pid, int maxDepth) {
    Adopt(pid);
    SleepMs(300);   // Chromium fills its tree after AXManualAccessibility
    NSArray *wins = Windows(gApp);
    printf("%lu window(s); focused: %s\n", (unsigned long)wins.count,
           (AXStr((__bridge AXUIElementRef)FocusedWindow(gApp), kAXTitleAttribute) ?: @"(none)").UTF8String);
    id f = AXObj(gApp, kAXFocusedUIElementAttribute);
    if (f) {
        Node n = NodeOf((__bridge AXUIElementRef)f);
        printf("keyboard: %s \"%s\"\n", (n.role ?: @"?").UTF8String, (n.title ?: n.desc ?: @"").UTF8String);
    }
    Walk(wins, kMaxNodes, ^BOOL(AXUIElementRef e, Node n, int depth) {
        if (depth > maxDepth) return YES;
        NSMutableString *s = [NSMutableString string];
        for (int i = 0; i < depth; i++) [s appendString:@"  "];
        [s appendString:n.role ?: @"?"];
        if (n.title.length) [s appendFormat:@" title=\"%@\"", Clean(n.title)];
        if (n.value.length) [s appendFormat:@" value=\"%@\"", Clean(n.value.length > 80 ? [n.value substringToIndex:80] : n.value)];
        if (n.desc.length) [s appendFormat:@" desc=\"%@\"", Clean(n.desc)];
        if ([n.role isEqualToString:@"AXTextArea"]) {
            NSString *pre = AXPrefix(e, 60);
            if (pre) [s appendFormat:@" text=\"%@\"", Clean(pre)];
        }
        printf("%s\n", s.UTF8String);
        return YES;
    });
    return 0;
}

// ------------------------------------------------------------------ fixtures

static BOOL WritePNG(NSString *path) {
    const size_t w = 480, h = 320;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    for (size_t y = 0; y < h; y += 8)
        for (size_t x = 0; x < w; x += 8) {
            CGContextSetRGBFillColor(ctx, x / (double)w, y / (double)h, 0.6, 1);
            CGContextFillRect(ctx, CGRectMake(x, y, 8, 8));
        }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                              CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    BOOL ok = CGImageDestinationFinalize(d);
    CFRelease(d);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return ok;
}

static BOOL WritePDF(NSString *path) {
    CGRect box = CGRectMake(0, 0, 612, 792);
    CGContextRef ctx = CGPDFContextCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], &box, NULL);
    if (!ctx) return NO;
    for (int page = 1; page <= 2; page++) {
        CGPDFContextBeginPage(ctx, NULL);
        NSGraphicsContext *g = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = g;
        NSString *s = [NSString stringWithFormat:@"taskbench report, page %d\n\nQuarterly numbers for the fixture.", page];
        [s drawInRect:NSMakeRect(72, 72, 468, 648)
            withAttributes:@{NSFontAttributeName : [NSFont systemFontOfSize:18]}];
        [NSGraphicsContext restoreGraphicsState];
        CGPDFContextEndPage(ctx);
    }
    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    return YES;
}

static BOOL WriteMP4(NSString *path) {
    [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    NSError *err = nil;
    AVAssetWriter *w = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path] fileType:AVFileTypeMPEG4 error:&err];
    if (!w) return NO;
    const int W = 320, H = 240, fps = 15, frames = 30;
    AVAssetWriterInput *in = [AVAssetWriterInput
        assetWriterInputWithMediaType:AVMediaTypeVideo
                       outputSettings:@{AVVideoCodecKey : AVVideoCodecTypeH264, AVVideoWidthKey : @(W),
                                        AVVideoHeightKey : @(H)}];
    AVAssetWriterInputPixelBufferAdaptor *ad = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
                                   sourcePixelBufferAttributes:@{
                                       (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                                       (id)kCVPixelBufferWidthKey : @(W), (id)kCVPixelBufferHeightKey : @(H)}];
    [w addInput:in];
    if (![w startWriting]) return NO;
    [w startSessionAtSourceTime:kCMTimeZero];
    for (int i = 0; i < frames; i++) {
        while (!in.readyForMoreMediaData) SleepMs(2);
        CVPixelBufferRef pb = NULL;
        CVPixelBufferPoolCreatePixelBuffer(NULL, ad.pixelBufferPool, &pb);
        if (!pb) return NO;
        CVPixelBufferLockBaseAddress(pb, 0);
        uint8_t *base = CVPixelBufferGetBaseAddress(pb);
        size_t row = CVPixelBufferGetBytesPerRow(pb);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t *p = base + y * row + x * 4;
                p[0] = (uint8_t)(x * 255 / W);
                p[1] = (uint8_t)((y + i * 8) % 256);
                p[2] = (uint8_t)(i * 255 / frames);
                p[3] = 255;
            }
        CVPixelBufferUnlockBaseAddress(pb, 0);
        [ad appendPixelBuffer:pb withPresentationTime:CMTimeMake(i, fps)];
        CVPixelBufferRelease(pb);
    }
    [in markAsFinished];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [w finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return w.status == AVAssetWriterStatusCompleted;
}

static int BigLog(NSString *path, long mb, NSString *needle) {
    FILE *f = fopen(path.fileSystemRepresentation, "w");
    if (!f) return 2;
    const long long target = mb * 1048576LL;
    long long written = 0, line = 0;
    BOOL placed = NO;
    static const char *levels[] = {"INFO", "INFO", "INFO", "DEBUG", "WARN"};
    char buf[256];
    while (written < target) {
        int n;
        if (!placed && written > target - target / 1000) {
            n = snprintf(buf, sizeof buf, "2026-11-20 23:59:58.%03lld ERROR worker-7 %s payment failed for order %lld\n",
                         line % 1000, needle.UTF8String, line);
            placed = YES;
        } else {
            n = snprintf(buf, sizeof buf,
                         "2026-11-20 %02lld:%02lld:%02lld.%03lld %s worker-%lld request id=%08llx path=/api/items/%lld "
                         "status=200 ms=%lld\n",
                         (line / 3600000) % 24, (line / 60000) % 60, (line / 1000) % 60, line % 1000,
                         levels[line % 5], line % 16, (unsigned long long)(line * 2654435761ULL) & 0xffffffffULL,
                         line % 9973, line % 250);
        }
        fwrite(buf, 1, (size_t)n, f);
        written += n;
        line++;
    }
    fclose(f);
    printf("%lld lines, %lld bytes\n", line, written);
    return placed ? 0 : 1;
}

// ------------------------------------------------------------------ main

static int Usage(void) {
    fprintf(stderr, "usage: taskbench-mac check | idle-wait SECONDS STATE | run --exe PATH --steps FILE --state STATE "
                    "[--out FILE] | quit --exe PATH PID | axdump PID [DEPTH] | tree PID | media DIR | "
                    "biglog PATH MB NEEDLE\n");
    return 2;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gTickNs = (double)tb.numer / tb.denom;
        gResponsible = (RespFn)dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid");
        AXUIElementSetMessagingTimeout(AXUIElementCreateSystemWide(), 1.5);
        if (getenv("TASKBENCH_KEY_MS")) gKeyGapMs = atof(getenv("TASKBENCH_KEY_MS"));
        if (argc < 2) return Usage();
        NSString *cmd = @(argv[1]);

        if ([cmd isEqualToString:@"check"]) {
            const BOOL trusted = AXIsProcessTrusted();
            printf("accessibility: %s\n", trusted ? "trusted" : "NOT trusted");
            const double idle = HidIdleMs();
            printf("owner idle: %.1f s\n", idle / 1000);
            if (!trusted)
                printf("  Give the app running this script (Terminal, iTerm, ...) Accessibility permission:\n"
                       "  System Settings > Privacy & Security > Accessibility, then run it again.\n");
            return trusted ? 0 : 4;
        }
        if ([cmd isEqualToString:@"idle-wait"] && argc == 4) {
            const double want = atof(argv[2]) * 1000;
            gStatePath = @(argv[3]);
            BOOL said = NO;
            for (;;) {
                const double idle = HidIdleMs();
                if (idle < 0) {
                    fprintf(stderr, "taskbench: cannot read HIDIdleTime\n");
                    return 2;
                }
                if (idle >= want) {
                    gLastKnownInput = WallMs() - idle;
                    SaveState();
                    return 0;
                }
                if (!said) {
                    printf("Waiting until the keyboard and mouse have been idle for %.0f s...\n", want / 1000);
                    fflush(stdout);
                    said = YES;
                }
                SleepMs(250);
            }
        }
        if ([cmd isEqualToString:@"run"]) {
            NSString *exe = nil, *steps = nil, *out = nil;
            for (int i = 2; i + 1 < argc; i += 2) {
                NSString *k = @(argv[i]), *v = @(argv[i + 1]);
                if ([k isEqualToString:@"--exe"]) exe = v;
                else if ([k isEqualToString:@"--steps"]) steps = v;
                else if ([k isEqualToString:@"--state"]) gStatePath = v;
                else if ([k isEqualToString:@"--out"]) out = v;
                else return Usage();
            }
            if (!exe || !steps || !gStatePath) return Usage();
            if (!AXIsProcessTrusted()) {
                fprintf(stderr, "taskbench: this terminal has no Accessibility permission (taskbench-mac check)\n");
                return 4;
            }
            LoadState();
            return Run(exe, steps, out);
        }
        if ([cmd isEqualToString:@"quit"] && argc == 5 && !strcmp(argv[2], "--exe"))
            return Quit(@(argv[3]), (pid_t)atoi(argv[4]));
        if ([cmd isEqualToString:@"axdump"] && argc >= 3) {
            if (!AXIsProcessTrusted()) {
                fprintf(stderr, "taskbench: this terminal has no Accessibility permission\n");
                return 4;
            }
            return AxDump((pid_t)atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 40);
        }
        if ([cmd isEqualToString:@"tree"] && argc == 3) {
            TreeSample s = SampleTree((pid_t)atoi(argv[2]));
            printf("processes=%d cpu_ms=%.0f footprint_mb=%.0f\n", s.count, s.cpuMs, s.footprintMB);
            return s.count ? 0 : 1;
        }
        if ([cmd isEqualToString:@"media"] && argc == 3) {
            NSString *dir = @(argv[2]);
            [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            BOOL ok = WritePDF([dir stringByAppendingPathComponent:@"report.pdf"]);
            ok = WritePNG([dir stringByAppendingPathComponent:@"picture.png"]) && ok;
            ok = WriteMP4([dir stringByAppendingPathComponent:@"clip.mp4"]) && ok;
            return ok ? 0 : 1;
        }
        if ([cmd isEqualToString:@"biglog"] && argc == 5) return BigLog(@(argv[2]), atol(argv[3]), @(argv[4]));
        return Usage();
    }
}
