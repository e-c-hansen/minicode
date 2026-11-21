// taskbench-mac — drive one editor through one task and measure it.
//
// scripts/taskbench-mac.sh (run through `make taskbench-mac`) writes each
// app's steps for each task to a file and runs `taskbench-mac run` on it.
// The steps are inputs a person makes (key chords, typed text, clicks on
// elements found through the Accessibility API, a file opened as Finder
// opens it) each followed by a result condition polled every 10 ms (a window
// title, an element's text, the keyboard's place, a shell command such as
// `git diff --cached`, a port closing). Measured per step: wall time from the
// last event the input posted to the moment its result happened, which for
// most conditions is when the poll found it, but for a `quiet` condition is
// when the tree's CPU activity stopped and for a screen read when the first
// frame showing it was on screen, so the benchmark's own waiting (the quiet
// window, Vision's reading) is never in a step's wall time; each step says
// how its end was observed (ax, file, git, port, cpu-quiet, screen). Per
// task: the inputs counted (clicks, key chords, keystrokes of typed text,
// Finder opens), the CPU time of the app's whole process tree (user +
// system, proc_pid_rusage, including children it reaped), and its physical
// footprint, peak and settled (procmem-mac's method: the process and every
// process whose parent or responsible pid is in the tree). A thread samples
// the tree every 10 ms from the launch through the settle, so the peak is
// over all of it and never below the settled footprint.
//
//   taskbench-mac check                      AX trust and the owner's idle time
//   taskbench-mac idle-wait SECONDS STATE    wait until the owner has been idle
//   taskbench-mac run --exe PATH --steps FILE --state STATE [--out FILE] [--pid PID]
//   taskbench-mac quit --exe PATH PID        stop a pid this run started, and its tree
//   taskbench-mac axdump PID [DEPTH] [manual] print an app's windows' AX tree
//   taskbench-mac screentext PID             print the text Vision reads in an app's front window
//   taskbench-mac tree PID                   print a tree's CPU and footprint
//   taskbench-mac windows PID                print an app's on-screen windows
//   taskbench-mac ocr PNG                    print what Vision reads in a picture
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
//   sclick TEXT            a click on TEXT where it shows in the front window, read from
//                          the screen by Vision (for an app with no AX tree; one click)
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
//   text TEXT              a text area or static text in the focused window holds TEXT
//   quiet MS               the app's own process has been quiet for MS ms ("settled,
//                          unverified"): its median CPU under 20% of a core and no
//                          sample busy (1 ms over that level a 10 ms sample, a lone
//                          blink under 2 ms over aside); the step ends at the last busy
//                          sample (QuietAt). Its own process, so a language server
//                          starting beside it does not hold up its UI's answer
//   settle MS              the same of the whole tree (after a launch)
//   screen TEXT | !screen TEXT   the app's front window shows TEXT / text without it,
//                          read from a ScreenCaptureKit stream of its windows by Vision
//                          (for an app with no AX tree; spaces ignored, case kept); the
//                          step ends at the first frame that showed it
//   luma>X | luma<X        the front window's mean brightness (0 to 1), from the stream
//   view NAME MAGIC        NAME's window is up and settled; then "opens as text" when a
//                          text area starts with MAGIC, "refused" when the app says it
//                          cannot show it, "shown" otherwise; without an AX tree the
//                          same is read from the screen, and "unverified" if no text is
//   none                   true at once
//
// Owner input stops the run: the state file holds the time of the last input
// event this benchmark knows of (the owner's last before the run, then each
// event this tool posts). An event newer than that (HIDIdleTime from
// IOHIDSystem) is someone at the keyboard or mouse, and `run` exits 3 at once.
//
// `run` prints, and writes to --out, a line a step (step, n, kind, inputs,
// input ms, wall ms, slowest poll ms, resolution ms, how the end was
// observed, status, what matched, the step), a `pid` line once the app's pid
// is known, and a last `summary` line of key=value fields. The resolution is
// the slowest poll for a condition read by polling (a poll that walks a large
// AX tree takes as long as the walk), the CPU sample's length for a quiet
// one, and the gap since the frame before for a screen read (34 ms at most).
// TASKBENCH_TRACE=DIR writes each step's CPU samples (cpu.txt) and every
// frame Vision reads there.
//
// Exit codes: 0 done, 1 a step failed, 2 usage or setup error, 3 owner input,
// 4 no Accessibility permission.
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <Vision/Vision.h>
#include <os/lock.h>
#include <pthread.h>
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

static double gLastPostMono = 0;   // CLOCK_UPTIME_RAW ms of the last event this tool posted

static void NotePosted(void) {
    gLastKnownInput = WallMs();
    gLastPostMono = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
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
    double ownMs;       // the app's own process (its UI), without its helpers and servers
    double footprintMB; // physical footprint of the live tree
    int count;
} TreeSample;

// What exited members had used when last seen (launchd reaps a helper whose
// parent is launchd, so its time would otherwise vanish with it). Touched
// only by whichever thread samples (the sampler, or `tree` on its own).
static NSMutableDictionary<NSNumber *, NSNumber *> *gOrphanCpu;
static double gGoneCpuMs = 0;

// The tree's members, found in C (the sampler runs this every 10 ms, so no
// sets of objects): a pid bitmap grown to a fixed point over the links.
enum { kMaxPid = 1 << 20 };
static int TreeMembers(pid_t root, pid_t *out, pid_t *outPpid, int cap) {
    static uint8_t *in;
    static ProcLink *links;
    static int linksCap;
    if (!in) in = calloc(kMaxPid / 8, 1);
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return 0;
    if (n * 2 > linksCap) {
        linksCap = n * 2;
        links = realloc(links, (size_t)linksCap * sizeof(ProcLink));
    }
    pid_t *pids = calloc((size_t)linksCap, sizeof(pid_t));
    n = proc_listallpids(pids, (int)(linksCap * sizeof(pid_t)));
    int m = 0;
    BOOL alive = NO;
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0 || pids[i] >= kMaxPid) continue;
        struct proc_bsdshortinfo b;
        if (proc_pidinfo(pids[i], PROC_PIDT_SHORTBSDINFO, 0, &b, sizeof b) != sizeof b) continue;
        links[m++] = (ProcLink){pids[i], (pid_t)b.pbsi_ppid, gResponsible ? gResponsible(pids[i]) : pids[i]};
        if (pids[i] == root) alive = YES;
    }
    free(pids);
    if (!alive) return 0;
#define IN(p) ((p) > 0 && (p) < kMaxPid && (in[(p) >> 3] & (1 << ((p) & 7))))
#define SET(p) (in[(p) >> 3] |= (uint8_t)(1 << ((p) & 7)))
    SET(root);
    BOOL changed;
    do {
        changed = NO;
        for (int i = 0; i < m; i++)
            if (!IN(links[i].pid) && (IN(links[i].ppid) || IN(links[i].rpid))) {
                SET(links[i].pid);
                changed = YES;
            }
    } while (changed);
    int k = 0;
    for (int i = 0; i < m; i++)
        if (IN(links[i].pid) && k < cap) {
            out[k] = links[i].pid;
            outPpid[k] = links[i].ppid;
            k++;
        }
    for (int i = 0; i < m; i++) in[links[i].pid >> 3] = 0;   // cleared for the next call
#undef IN
#undef SET
    return k;
}

static TreeSample SampleTree(pid_t root) {
    TreeSample s = {0, 0, 0, 0};
    if (root <= 0) return s;
    enum { kCap = 4096 };
    static pid_t members[kCap], ppids[kCap];
    const int n = TreeMembers(root, members, ppids, kCap);
    if (!gOrphanCpu) gOrphanCpu = [NSMutableDictionary dictionary];
    NSMutableSet *seen = [NSMutableSet set];
    for (int i = 0; i < n; i++) {
        struct rusage_info_v4 ru;
        if (proc_pid_rusage(members[i], RUSAGE_INFO_V4, (rusage_info_t *)&ru) != 0) continue;
        const double cpu = (double)(ru.ri_user_time + ru.ri_system_time + ru.ri_child_user_time +
                                    ru.ri_child_system_time) * gTickNs / 1e6;
        s.cpuMs += cpu;
        if (members[i] == root) s.ownMs = cpu;
        s.footprintMB += ru.ri_phys_footprint / 1048576.0;
        s.count++;
        if (members[i] != root && ppids[i] == 1) {
            gOrphanCpu[@(members[i])] = @(cpu);
            [seen addObject:@(members[i])];
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

// YES when a running process's executable is the app's. An app that still
// carries the quarantine attribute and was not moved by Finder is run by
// Gatekeeper from a read-only copy under .../AppTranslocation/<uuid>/d/
// (VS Code 1.107, unzipped into /Applications, was), so its path is not the
// one under /Applications: then the bundle's name and the path inside it
// must match.
static BOOL ExeMatches(NSString *path, NSString *exe) {
    if (!path) return NO;
    if ([path isEqualToString:exe]) return YES;
    if ([path rangeOfString:@"/AppTranslocation/"].location == NSNotFound) return NO;
    NSRange inner = [exe rangeOfString:@".app/Contents/MacOS/" options:NSBackwardsSearch];
    if (inner.location == NSNotFound) return NO;
    NSRange slash = [exe rangeOfString:@"/" options:NSBackwardsSearch range:NSMakeRange(0, inner.location)];
    if (slash.location == NSNotFound) return NO;
    return [path hasSuffix:[exe substringFromIndex:slash.location]];
}

static NSSet<NSNumber *> *PidsRunning(NSString *exe) {
    NSMutableSet *out = [NSMutableSet set];
    NSData *links = ListLinks();
    const ProcLink *l = links.bytes;
    for (NSUInteger i = 0; i < links.length / sizeof(ProcLink); i++)
        if (ExeMatches(ExePath(l[i].pid), exe)) [out addObject:@(l[i].pid)];
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
    // Each element once: an Electron app turning its accessibility on listed
    // its application element among its windows and as its own child, a
    // cycle that filled every walk to kMaxNodes.
    NSMutableSet *visited = [NSMutableSet set];
    for (id r in roots) [queue addObject:@[ r, @0 ]];
    int seen = 0;
    for (NSUInteger i = 0; i < queue.count && seen < maxNodes; i++) {
        AXUIElementRef e = (__bridge AXUIElementRef)queue[i][0];
        int depth = [queue[i][1] intValue];
        if ([visited containsObject:queue[i][0]]) continue;
        [visited addObject:queue[i][0]];
        Node n = NodeOf(e);
        seen++;
        if (!visit(e, n, depth)) break;
        if (depth > 80 ||
            (depth > 0 && ([n.role isEqualToString:@"AXApplication"] || [n.role isEqualToString:@"AXMenuBar"])))
            continue;
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

static BOOL ElementCentre(id el, CGPoint *out);

// The element's centre once its frame has held still for 60 ms (2 s at
// most), as a person waits for a list to stop moving before clicking. A
// click at the frame read the moment a VS Code row appeared landed on the
// row that took its place while the view filled in (Commit, not app.py).
static BOOL StableCentre(id el, CGPoint *out) {
    CGPoint a, b;
    if (!ElementCentre(el, &a)) return NO;
    const double until = MonoMs() + 2000;
    for (;;) {
        SleepMs(30);
        if (!ElementCentre(el, &b)) return NO;
        if (CGPointEqualToPoint(a, b)) {
            SleepMs(30);
            if (!ElementCentre(el, &b)) return NO;
            if (CGPointEqualToPoint(a, b) || MonoMs() > until) break;
        }
        if (MonoMs() > until) break;
        a = b;
    }
    *out = b;
    return YES;
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

// Chromium (VS Code) builds its accessibility tree only when asked. The run
// does not ask through AXManualAccessibility: set on a starting app the
// request was lost, set later it took seconds to build and, while it built,
// the app listed itself among its windows (a cycle a walk never left) and
// failed AX calls; and VS Code 1.107 never reads it back as true. The script
// starts Electron apps with Chromium's --force-renderer-accessibility
// instead, so the tree is there from the first window, at the same cost in
// every task. axdump, which may look at any running app, still sets it.
static void Adopt(pid_t pid) {
    gPid = pid;
    if (gApp) CFRelease(gApp);
    gApp = AXUIElementCreateApplication(pid);
}

static void LookForNewPid(void) {
    if (gPid || !gBefore) return;
    pid_t best = 0;
    for (NSNumber *p in PidsRunning(gExe))
        if (![gBefore containsObject:p] && p.intValue > best) best = p.intValue;
    if (best) Adopt(best);
}

// The owner of the frontmost normal-level window on screen, or 0.
static pid_t TopWindowPid(void) {
    NSArray *wins = CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    for (NSDictionary *w in wins)
        if ([w[(__bridge id)kCGWindowLayer] intValue] == 0) return [w[(__bridge id)kCGWindowOwnerPID] intValue];
    return 0;
}

// Asked of the system through AX each time: NSWorkspace's answer is updated
// by notifications this tool, with no run loop, never receives. The system
// answers AX's question by asking the frontmost app, and an Electron app
// busy building its accessibility tree (VS Code for a few seconds after
// AXManualAccessibility) fails it with kAXErrorCannotComplete: then the
// owner of the frontmost window on screen answers instead.
static BOOL Frontmost(void) {
    static AXUIElementRef sys;
    if (!sys) sys = AXUIElementCreateSystemWide();
    CFTypeRef app = NULL;
    pid_t pid = 0;
    if (AXUIElementCopyAttributeValue(sys, kAXFocusedApplicationAttribute, &app) == kAXErrorSuccess && app) {
        AXUIElementGetPid((AXUIElementRef)app, &pid);
        CFRelease(app);
        return pid == gPid;
    }
    return gPid && TopWindowPid() == gPid;
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

// A thread samples the app's tree every 10 ms from the moment its pid is
// known (the launch) until the run ends (after the settle), whatever the
// main thread is doing: the CPU and the footprint, with their times. So the
// peak is over every sample from launch through settle (never below the
// settled footprint, which is the last sample), and a `quiet` step can say
// when the tree's activity stopped, not only when the benchmark noticed.

typedef struct {
    double t, cpu, mb, own;   // own: the app process's CPU (a `quiet` term's)
} Pt;

enum { kRing = 1 << 15 };   // 32768 samples at 10 ms: over five minutes
static Pt gRing[kRing];
static long gRingN = 0;   // samples taken; sample i is gRing[i % kRing]
static os_unfair_lock gMeasureLock = OS_UNFAIR_LOCK_INIT;
static double gPeakMB = 0, gPeakAt = 0;
static const double kSampleMs = 10;
static double gSampleGapMaxMs = 0;   // the longest gap between two samples: the CPU's resolution

static void *SamplerMain(void *unused) {
    (void)unused;
    double last = 0;
    for (;;) {
        @autoreleasepool {
            const double tick = MonoMs();
            const pid_t pid = gPid;
            if (pid > 0) {
                TreeSample s = SampleTree(pid);
                const double t = MonoMs();
                os_unfair_lock_lock(&gMeasureLock);
                if (s.count > 0) {
                    gRing[gRingN % kRing] = (Pt){t, s.cpuMs, s.footprintMB, s.ownMs};
                    gRingN++;
                    if (s.footprintMB > gPeakMB) {
                        gPeakMB = s.footprintMB;
                        gPeakAt = t;
                    }
                    if (last > 0 && t - last > gSampleGapMaxMs) gSampleGapMaxMs = t - last;
                    last = t;
                }
                os_unfair_lock_unlock(&gMeasureLock);
            }
            SleepMs(MAX(1, kSampleMs - (MonoMs() - tick)));
        }
    }
    return NULL;
}

static void StartSampler(void) {
    static pthread_t th;
    if (th) return;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_create(&th, &a, SamplerMain, NULL);
}

// The newest sample (t 0 when there is none yet).
static Pt Latest(void) {
    os_unfair_lock_lock(&gMeasureLock);
    Pt p = gRingN ? gRing[(gRingN - 1) % kRing] : (Pt){0, 0, 0, 0};
    os_unfair_lock_unlock(&gMeasureLock);
    return p;
}

// Waits for a sample taken after now, so a reading at a step's edge is of
// that moment (20 ms at most).
static Pt Fresh(void) {
    const double now = MonoMs();
    Pt p = Latest();
    for (int i = 0; i < 20 && gPid && p.t < now; i++) {
        SleepMs(2);
        p = Latest();
    }
    return p;
}

// Samples since `from` (with the one before it, as the CPU's starting point).
static NSData *SamplesSince(double from) {
    os_unfair_lock_lock(&gMeasureLock);
    long first = gRingN - 1;
    while (first > 0 && first > gRingN - kRing + 1 && gRing[first % kRing].t >= from) first--;
    NSMutableData *d = [NSMutableData dataWithCapacity:(NSUInteger)(gRingN - first) * sizeof(Pt)];
    for (long i = MAX(first, 0); i < gRingN; i++) [d appendBytes:&gRing[i % kRing] length:sizeof(Pt)];
    os_unfair_lock_unlock(&gMeasureLock);
    return d;
}

// Quiet and busy are judged against the tree's own settled level, since an
// app at rest is not always at zero (Zed redraws about every 12 ms, about
// 1 ms of CPU each, while a picker is open). Each sample's CPU is put per
// 10 ms. Over a window, the level is the median sample. A sample is busy
// when it is over the level by 1 ms or more; a lone busy sample under 2 ms
// over (a caret's blink) does not count. A window is quiet when its level
// is under 2 ms (20% of a core) and nothing in it counts as busy.
static const double kLevelMaxMs = 2.0, kBusyOverMs = 1.0, kBlinkOverMs = 2.0;

typedef struct {
    double t, c, dt;   // the sample's time, its CPU per 10 ms, its length
} Use;

static int CompareDouble(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static BOOL CountsBusy(const Use *u, long n, long i, double level) {
    if (u[i].c <= level + kBusyOverMs) return NO;
    if (u[i].c > level + kBlinkOverMs) return YES;
    return (i > 0 && u[i - 1].c > level + kBusyOverMs) || (i + 1 < n && u[i + 1].c > level + kBusyOverMs);
}

// YES when the tree has been quiet for the last `ms`, counting only samples
// since `from` (the step's input), so a step's quiet never comes from
// before the app saw it. *endAt (when given) is when the tree's activity
// stopped: the time of the last sample since `from` that counts as busy
// against the quiet window's level, or `from` when none does; *resMs that
// sample's length.
static double gQuietFrom = 0;

// `own`: the app's own process (a step's `quiet`: its UI answering, not a
// language server starting beside it), else the whole tree (`settle`, and
// the settle before the footprint is read).
static BOOL QuietAt(double ms, double from, BOOL own, double *endAt, double *resMs) {
    if (!gPid) return NO;
    NSData *d = SamplesSince(from);
    const Pt *p = d.bytes;
    const long np = (long)(d.length / sizeof(Pt));
    if (np < 3) return NO;
    NSMutableData *ud = [NSMutableData dataWithLength:(NSUInteger)(np - 1) * sizeof(Use)];
    Use *u = ud.mutableBytes;
    long n = 0;
    for (long i = 1; i < np; i++) {
        const double dt = p[i].t - p[i - 1].t;
        if (dt <= 0) continue;
        const double used = own ? p[i].own - p[i - 1].own : p[i].cpu - p[i - 1].cpu;
        u[n++] = (Use){p[i].t, used * 10 / dt, dt};
    }
    if (n < 2 || u[n - 1].t - MAX(from, p[0].t) < ms) return NO;
    long w0 = n - 1;   // the window: samples in the last `ms`
    while (w0 > 0 && u[n - 1].t - u[w0 - 1].t <= ms && u[w0 - 1].t - u[w0 - 1].dt >= from) w0--;
    const long wn = n - w0;
    double *sorted = malloc((size_t)wn * sizeof(double));
    for (long i = 0; i < wn; i++) sorted[i] = u[w0 + i].c;
    qsort(sorted, (size_t)wn, sizeof(double), CompareDouble);
    const double level = sorted[wn / 2];
    free(sorted);
    if (level >= kLevelMaxMs) return NO;
    for (long i = w0; i < n; i++)
        if (CountsBusy(u, n, i, level)) return NO;
    if (endAt) {
        *endAt = from;
        if (resMs) *resMs = kSampleMs;
        for (long k = w0 - 1; k >= 0; k--) {
            if (u[k].t <= from) break;
            if (CountsBusy(u, n, k, level)) {
                *endAt = u[k].t;
                if (resMs) *resMs = u[k].dt;
                break;
            }
        }
    }
    return YES;
}

static BOOL Settled(double ms) { return QuietAt(ms, gQuietFrom, NO, NULL, NULL); }

// ------------------------------------------------------------------ conditions

static NSRegularExpression *gRefused;

// ------------------------------------------------------------------ the screen
//
// An app that shows no AX tree (Zed) is read from its pixels. A
// ScreenCaptureKit stream of the app's windows delivers a frame each time
// what they show changes (30 a second at most), stamped with the time it
// was shown; Vision reads the text in the front window's part of a frame.
// A `screen` term asks the newest frame; once one holds, the frames between
// the last that did not and that one are halved down to the first that
// does, and the step ends at that frame's time: when the result was on
// screen, not when the reading finished, so the reading (300 to 1,000 ms a
// frame) is never in a step's wall time. The stream is started before the
// input of a step that reads the screen, so the frame before the input is
// there to compare with. It needs Screen Recording permission for the
// terminal; without it nothing is read, which reads as unverified.

@interface TBFrame : NSObject {
  @public
    double t;
    CGImageRef img;
    CGRect display;   // the display's bounds in global points, top left origin
    double scale;     // image pixels per point
    NSMutableDictionary *cache;
}
@end
@implementation TBFrame
- (void)dealloc {
    if (img) CGImageRelease(img);
}
@end

static NSMutableArray<TBFrame *> *gFrames;
static os_unfair_lock gFrameLock = OS_UNFAIR_LOCK_INIT;
// Frames older than the newest at or before this are dropped as they come.
static double gFramesKeepFrom = INFINITY;
static const NSUInteger kMaxFrames = 24;   // past it, middle frames go first

static void KeepFramesFrom(double t) {
    os_unfair_lock_lock(&gFrameLock);
    gFramesKeepFrom = t;
    while (gFrames.count >= 2 && gFrames[1]->t <= gFramesKeepFrom) [gFrames removeObjectAtIndex:0];
    os_unfair_lock_unlock(&gFrameLock);
}

@interface TBCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
  @public
    SCStream *stream;
    pid_t pid;
    CGRect display;
    double scale;
    volatile BOOL failed;
}
@end
@implementation TBCapture
- (void)stream:(SCStream *)s didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type {
    (void)s;
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sb)) return;
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (!atts || CFArrayGetCount(atts) < 1) return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(atts, 0);
    if ([info[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) return;
    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb) return;
    NSNumber *shown = info[SCStreamFrameInfoDisplayTime];   // mach time, as CLOCK_UPTIME_RAW counts
    const double t = shown ? shown.unsignedLongLongValue * gTickNs / 1e6 : MonoMs();
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb);
    CFDataRef data = CFDataCreate(NULL, CVPixelBufferGetBaseAddress(pb), (CFIndex)(bpr * h));
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (!data) return;
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(w, h, 8, 32, bpr, cs,
                                   kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaPremultipliedFirst, prov,
                                   NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(prov);
    CFRelease(data);
    if (!img) return;
    TBFrame *f = [TBFrame new];
    f->t = t;
    f->img = img;
    f->display = display;
    f->scale = scale;
    f->cache = [NSMutableDictionary dictionary];
    os_unfair_lock_lock(&gFrameLock);
    if (!gFrames) gFrames = [NSMutableArray array];
    [gFrames addObject:f];
    while (gFrames.count >= 2 && gFrames[1]->t <= gFramesKeepFrom) [gFrames removeObjectAtIndex:0];
    while (gFrames.count > kMaxFrames) [gFrames removeObjectAtIndex:gFrames.count / 2];
    os_unfair_lock_unlock(&gFrameLock);
}
- (void)stream:(SCStream *)s didStopWithError:(NSError *)error {
    (void)s;
    (void)error;
    failed = YES;
}
@end

static TBCapture *gCapture;
static double gCaptureTriedAt = -1e9;

// The app's front normal window: its bounds in global points (top left
// origin), or CGRectNull.
static CGRect FrontWindowBounds(pid_t pid, CGWindowID *idOut) {
    NSArray *wins = CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    for (NSDictionary *w in wins)
        if ([w[(__bridge id)kCGWindowOwnerPID] intValue] == pid && [w[(__bridge id)kCGWindowLayer] intValue] == 0) {
            CGRect r;
            if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(__bridge id)kCGWindowBounds], &r))
                continue;
            if (r.size.width < 50 || r.size.height < 50) continue;
            if (idOut) *idOut = [w[(__bridge id)kCGWindowNumber] unsignedIntValue];
            return r;
        }
    return CGRectNull;
}

static CGWindowID FrontWindowOf(pid_t pid) {
    CGWindowID wid = 0;
    FrontWindowBounds(pid, &wid);
    return wid;
}

static void StopCapture(void) {
    if (!gCapture) return;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [gCapture->stream stopCaptureWithCompletionHandler:^(NSError *e) {
        (void)e;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    gCapture = nil;
    os_unfair_lock_lock(&gFrameLock);
    [gFrames removeAllObjects];
    os_unfair_lock_unlock(&gFrameLock);
}

// A stream of the app's windows on the display its front window is on,
// started once and kept for the run. NO until the app has a window to show.
static BOOL EnsureCapture(pid_t pid) {
    if (pid <= 0) return NO;
    if (gCapture && gCapture->pid == pid && !gCapture->failed) return YES;
    if (MonoMs() - gCaptureTriedAt < 200) return NO;
    gCaptureTriedAt = MonoMs();
    StopCapture();
    if (!CGPreflightScreenCaptureAccess()) return NO;
    const CGRect wb = FrontWindowBounds(pid, NULL);
    if (CGRectIsNull(wb)) return NO;
    __block SCShareableContent *content = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO
                                                 completionHandler:^(SCShareableContent *c, NSError *e) {
                                                     (void)e;
                                                     content = c;
                                                     dispatch_semaphore_signal(sem);
                                                 }];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC))) return NO;
    SCRunningApplication *app = nil;
    for (SCRunningApplication *a in content.applications)
        if (a.processID == pid) app = a;
    if (!app) return NO;
    SCDisplay *disp = content.displays.firstObject;
    for (SCDisplay *d in content.displays)
        if (CGRectContainsPoint(CGDisplayBounds(d.displayID), CGPointMake(CGRectGetMidX(wb), CGRectGetMidY(wb))))
            disp = d;
    if (!disp) return NO;
    TBCapture *c = [TBCapture new];
    c->pid = pid;
    c->display = CGDisplayBounds(disp.displayID);
    // Pixels per point: the display's own (2 on a Retina panel), so Vision
    // reads the small text of a find bar's count; TASKBENCH_SCREEN_SCALE
    // overrides it.
    c->scale = getenv("TASKBENCH_SCREEN_SCALE")
                   ? atof(getenv("TASKBENCH_SCREEN_SCALE"))
                   : (double)CGDisplayPixelsWide(disp.displayID) / MAX(1.0, c->display.size.width);
    {
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(disp.displayID);
        if (mode && !getenv("TASKBENCH_SCREEN_SCALE"))
            c->scale = (double)CGDisplayModeGetPixelWidth(mode) / MAX(1.0, c->display.size.width);
        if (mode) CGDisplayModeRelease(mode);
    }
    if (c->scale < 1) c->scale = 1;
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:disp includingApplications:@[ app ]
                                                      exceptingWindows:@[]];
    SCStreamConfiguration *cfg = [SCStreamConfiguration new];
    cfg.width = (size_t)(c->display.size.width * c->scale);
    cfg.height = (size_t)(c->display.size.height * c->scale);
    cfg.minimumFrameInterval = CMTimeMake(1, 30);
    cfg.pixelFormat = kCVPixelFormatType_32BGRA;
    cfg.showsCursor = NO;
    cfg.queueDepth = 5;
    c->stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:c];
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("taskbench.frames", DISPATCH_QUEUE_SERIAL);
    if (![c->stream addStreamOutput:c type:SCStreamOutputTypeScreen sampleHandlerQueue:q error:nil]) return NO;
    __block NSError *startErr = nil;
    dispatch_semaphore_t s2 = dispatch_semaphore_create(0);
    [c->stream startCaptureWithCompletionHandler:^(NSError *e) {
        startErr = e;
        dispatch_semaphore_signal(s2);
    }];
    if (dispatch_semaphore_wait(s2, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) || startErr) return NO;
    gCapture = c;
    return YES;
}

// Started, and its first frame in, before a step's input (2 s at most).
static void CaptureBeforeInput(void) {
    const double until = MonoMs() + 2000;
    while (MonoMs() < until) {
        if (EnsureCapture(gPid)) {
            os_unfair_lock_lock(&gFrameLock);
            const NSUInteger n = gFrames.count;
            os_unfair_lock_unlock(&gFrameLock);
            if (n) return;
        }
        SleepMs(20);
    }
}

static int RunShell(NSString *cmd);

// Vision's accurate level, else its fast one: the accurate level read
// nothing at all in a frame of Zed's image viewer (a large picture beside the
// tabs and tree), where the fast level read every line.
static NSArray<VNRecognizedTextObservation *> *Recognise(CGImageRef img) {
    if (!img) return @[];
    for (int fast = 0; fast < 2; fast++) {
        VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
        req.recognitionLevel = fast ? VNRequestTextRecognitionLevelFast : VNRequestTextRecognitionLevelAccurate;
        req.usesLanguageCorrection = NO;
        VNImageRequestHandler *h = [[VNImageRequestHandler alloc] initWithCGImage:img options:@{}];
        if ([h performRequests:@[ req ] error:nil] && req.results.count) return req.results;
    }
    return @[];
}

static NSString *TextOf(NSArray<VNRecognizedTextObservation *> *obs) {
    NSMutableArray *lines = [NSMutableArray array];
    for (VNRecognizedTextObservation *o in obs) {
        VNRecognizedText *best = [o topCandidates:1].firstObject;
        if (best.string.length) [lines addObject:best.string];
    }
    return [lines componentsJoinedByString:@"\n"];
}

// A frame's part that shows the app's front window, and that window's
// bounds in global points.
static CGImageRef CropToWindow(TBFrame *f, CGRect *boundsOut) {
    CGRect wb = FrontWindowBounds(gPid, NULL);
    if (CGRectIsNull(wb)) wb = f->display;
    if (boundsOut) *boundsOut = wb;
    CGRect px = CGRectMake((wb.origin.x - f->display.origin.x) * f->scale, (wb.origin.y - f->display.origin.y) * f->scale,
                           wb.size.width * f->scale, wb.size.height * f->scale);
    px = CGRectIntersection(px, CGRectMake(0, 0, CGImageGetWidth(f->img), CGImageGetHeight(f->img)));
    if (CGRectIsEmpty(px)) return NULL;
    return CGImageCreateWithImageInRect(f->img, px);
}

static NSArray<VNRecognizedTextObservation *> *FrameObservations(TBFrame *f, CGRect *boundsOut) {
    CGRect wb;
    CGImageRef crop = CropToWindow(f, &wb);
    if (boundsOut) *boundsOut = wb;
    if (!crop) return @[];
    NSArray *obs = f->cache[@"obs"];
    if (!obs) {
        obs = Recognise(crop);
        f->cache[@"obs"] = obs;
        // TASKBENCH_TRACE=DIR: every frame read, as a picture named for its time.
        if (getenv("TASKBENCH_TRACE")) {
            NSString *png = [NSString stringWithFormat:@"%s/frame-%.0f.png", getenv("TASKBENCH_TRACE"), f->t];
            CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:png],
                                                                      CFSTR("public.png"), 1, NULL);
            if (d) {
                CGImageDestinationAddImage(d, crop, NULL);
                CGImageDestinationFinalize(d);
                CFRelease(d);
            }
        }
    }
    CGImageRelease(crop);
    return obs;
}

static NSString *FrameText(TBFrame *f) {
    NSString *s = f->cache[@"text"];
    if (!s) {
        s = TextOf(FrameObservations(f, NULL));
        f->cache[@"text"] = s;
    }
    return s;
}

// Mean luminance (0 dark to 1 light) of the front window's part.
static double FrameLuma(TBFrame *f) {
    NSNumber *cached = f->cache[@"luma"];
    if (cached) return cached.doubleValue;
    CGImageRef crop = CropToWindow(f, NULL);
    if (!crop) return -1;
    enum { S = 48 };
    uint8_t px[S * S * 4];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px, S, S, 8, S * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, S, S), crop);
    double sum = 0;
    for (int i = 0; i < S * S; i++) sum += (0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2]) / 255;
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(crop);
    const double l = sum / (S * S);
    f->cache[@"luma"] = @(l);
    return l;
}

// The newest frame, or nil.
static TBFrame *NewestFrame(void) {
    os_unfair_lock_lock(&gFrameLock);
    TBFrame *f = gFrames.lastObject;
    os_unfair_lock_unlock(&gFrameLock);
    return f;
}

// The text Vision reads in the app's front window, a line each, or nil:
// from the stream's newest frame when it runs, else a capture of the
// window (screencapture), as `screentext` and a first read before a stream
// can start need.
static NSString *ScreenText(pid_t pid) {
    if (gCapture && gCapture->pid == pid && !gCapture->failed) {
        TBFrame *f = NewestFrame();
        NSString *t = f ? FrameText(f) : nil;
        if (t.length) return t;
    }
    NSString *text = nil;
    CGWindowID wid = FrontWindowOf(pid);
    NSString *png = [NSTemporaryDirectory() stringByAppendingPathComponent:
                                                [NSString stringWithFormat:@"taskbench-screen-%d.png", getpid()]];
    const BOOL captured =
        wid && RunShell([NSString stringWithFormat:@"/usr/sbin/screencapture -x -o -l%u '%@'", wid, png]) == 0;
    if (captured) {
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:png], NULL);
        CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
        if (img) {
            text = TextOf(Recognise(img));
            CGImageRelease(img);
        }
        if (src) CFRelease(src);
    }
    [NSFileManager.defaultManager removeItemAtPath:png error:nil];
    return text;
}

// Text compared as the screen gives it: no spaces, one kind of quote. Case
// counts: "PNG" is a PNG shown as text, "picture.png" only its tab.
static NSString *Flat(NSString *s) {
    NSMutableString *m = [s mutableCopy];
    for (NSString *q in @[ @"“", @"”", @"„", @"''" ]) [m replaceOccurrencesOfString:q withString:@"\"" options:0 range:NSMakeRange(0, m.length)];
    NSArray *parts = [m componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return [parts componentsJoinedByString:@""];
}

static BOOL ScreenHas(NSString *screen, NSString *want) {
    return screen.length && want.length && [Flat(screen) rangeOfString:Flat(want)].location != NSNotFound;
}

// Where TEXT shows in the app's front window (the newest frame), in global
// points, for a click on what a person reads there.
static BOOL ScreenPoint(NSString *want, CGPoint *out) {
    if (!EnsureCapture(gPid)) return NO;
    TBFrame *f = NewestFrame();
    if (!f) return NO;
    CGRect wb;
    for (VNRecognizedTextObservation *o in FrameObservations(f, &wb)) {
        VNRecognizedText *best = [o topCandidates:1].firstObject;
        if (!ScreenHas(best.string, want)) continue;
        CGRect box = o.boundingBox;   // normalised, origin at the bottom left
        NSRange r = [best.string rangeOfString:want options:NSCaseInsensitiveSearch];
        if (r.location != NSNotFound) {
            VNRectangleObservation *sub = [best boundingBoxForRange:r error:nil];
            if (sub) box = sub.boundingBox;
        }
        *out = CGPointMake(wb.origin.x + (box.origin.x + box.size.width / 2) * wb.size.width,
                           wb.origin.y + (1 - (box.origin.y + box.size.height / 2)) * wb.size.height);
        return YES;
    }
    return NO;
}

static BOOL FrameHolds(TBFrame *f, NSString *term) {
    NSNumber *c = f->cache[term];
    if (c) return c.boolValue;
    BOOL yes = NO;
    if ([term hasPrefix:@"screen "]) yes = ScreenHas(FrameText(f), [term substringFromIndex:7]);
    else if ([term hasPrefix:@"!screen "]) {
        NSString *s = FrameText(f);
        yes = s.length && !ScreenHas(s, [term substringFromIndex:8]);   // nothing read is not "gone"
    } else if ([term hasPrefix:@"luma>"]) {
        const double l = FrameLuma(f);
        yes = l >= 0 && l > [term substringFromIndex:5].doubleValue;
    } else if ([term hasPrefix:@"luma<"]) {
        const double l = FrameLuma(f);
        yes = l >= 0 && l < [term substringFromIndex:5].doubleValue;
    }
    f->cache[term] = @(yes);
    return yes;
}

// A screen term: YES once a frame since the step's input holds; *at is the
// first such frame's time (never before the input), *res the gap since the
// frame before it (a new frame comes within a thirtieth of a second of a
// change, so at most 34 ms).
static BOOL ScreenTerm(NSString *term, NSMutableDictionary *state, double *at, double *res) {
    if (!EnsureCapture(gPid)) return NO;
    os_unfair_lock_lock(&gFrameLock);
    NSArray<TBFrame *> *frames = [gFrames copy];
    os_unfair_lock_unlock(&gFrameLock);
    if (!frames.count) return NO;
    const double no = state[@"no"] ? [state[@"no"] doubleValue] : -INFINITY;
    TBFrame *newest = frames.lastObject;
    if (newest->t <= no) return NO;
    if (!FrameHolds(newest, term)) {
        state[@"no"] = @(newest->t);
        KeepFramesFrom(MAX(newest->t, gQuietFrom));
        return NO;
    }
    NSMutableArray<TBFrame *> *cand = [NSMutableArray array];
    for (TBFrame *f in frames)
        if (f->t > no) [cand addObject:f];
    NSInteger lo = 0, hi = (NSInteger)cand.count - 1;   // cand[hi] holds
    while (lo < hi) {
        const NSInteger mid = (lo + hi) / 2;
        if (FrameHolds(cand[mid], term)) hi = mid;
        else lo = mid + 1;
    }
    TBFrame *first = cand[hi];
    const NSUInteger idx = [frames indexOfObjectIdenticalTo:first];
    const double prev = idx > 0 ? frames[idx - 1]->t : first->t - 34;
    *at = MAX(first->t, gQuietFrom);
    *res = MIN(first->t - prev, 34.0);
    return YES;
}

static NSString *Excerpt(NSString *s) {
    s = [[s componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@" / "];
    return s.length > 70 ? [[s substringToIndex:70] stringByAppendingString:@"..."] : s;
}

// What the screen shows for a view the AX tree cannot read.
static NSString *ClassifyScreen(NSString *magic) {
    CaptureBeforeInput();   // running, with a frame in
    NSString *screen = ScreenText(gPid);
    if (getenv("TASKBENCH_TRACE"))
        fprintf(stderr, "classify: %lu characters read, newest frame %.0f\n", (unsigned long)screen.length,
                NewestFrame() ? NewestFrame()->t : -1);
    if (!screen.length) return @"unverified (no accessibility tree, no text on screen)";
    // The line that decided it, as the report's evidence.
    for (NSString *line in [screen componentsSeparatedByString:@"\n"])
        if ([gRefused firstMatchInString:line options:0 range:NSMakeRange(0, line.length)])
            return [NSString stringWithFormat:@"refused (on screen: \"%@\")", Excerpt(line)];
    for (NSString *line in [screen componentsSeparatedByString:@"\n"])
        if (ScreenHas(line, magic)) return [NSString stringWithFormat:@"opens as text (on screen: \"%@\")", Excerpt(line)];
    if (ScreenHas(screen, magic)) return @"opens as text (read from the screen)";
    return @"shown (read from the screen)";
}

// "unverified" when the window shows no accessibility tree to read and the
// screen gives no text either.
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
    if (nodes < 15 || readable < 3) return ClassifyScreen(magic);
    return @"shown";
}

// Any text area (its first 20,000 characters) or static text in the focused
// window holds TEXT: a file's content as an editor exposes it.
static BOOL AXHasText(NSString *want) {
    id w = FocusedWindow(gApp);
    if (!w) return NO;
    __block BOOL found = NO;
    Walk(@[ w ], kMaxNodes, ^BOOL(AXUIElementRef e, Node n, int depth) {
        (void)depth;
        if ([n.role isEqualToString:@"AXTextArea"]) {
            NSString *pre = AXPrefix(e, 20000);
            if (pre && [pre rangeOfString:want].location != NSNotFound) found = YES;
        } else if ([n.role isEqualToString:@"AXStaticText"]) {
            if ((n.value && [n.value rangeOfString:want].location != NSNotFound) ||
                (n.title && [n.title rangeOfString:want].location != NSNotFound))
                found = YES;
        }
        return !found;
    });
    return found;
}

static NSString *gViewResult;

// How a step's end was observed, and when: set by each term that holds.
// The time is when the result happened, as far as the term can tell: an AX
// read, a shell check, a port, the moment the check found it (so the poll's
// length is its resolution); a `quiet` term, the last busy CPU sample; a
// screen term, the first frame that showed it; a view, the later of its
// title showing and its CPU settling.
static double gTermAt, gTermRes;
static NSString *gTermKind;

static BOOL Term(NSString *t, NSMutableDictionary *state) {
    t = [t stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    gTermAt = -1;
    gTermRes = -1;
    gTermKind = @"ax";
    BOOL yes = NO;
    if ([t isEqualToString:@"none"]) {
        gTermKind = @"none";
        yes = YES;
    } else if ([t hasPrefix:@"sh "]) {
        NSString *cmd = [t substringFromIndex:3];
        gTermKind = [cmd rangeOfString:@"git"].location != NSNotFound ? @"git" : @"file";
        yes = RunShell(cmd) == 0;
    } else if ([t hasPrefix:@"port-closed "]) {
        gTermKind = @"port";
        yes = !PortOpen([t substringFromIndex:12].intValue);
    } else if ([t hasPrefix:@"quiet "] || [t hasPrefix:@"settle "]) {
        gTermKind = @"cpu-quiet";
        double end = 0, res = 0;
        const BOOL own = [t hasPrefix:@"quiet "];
        yes = gPid && QuietAt([t substringFromIndex:own ? 6 : 7].doubleValue, gQuietFrom, own, &end, &res);
        if (yes) {
            gTermAt = end;
            gTermRes = res;
        }
    } else if (!gPid) {
        return NO;
    } else if ([t isEqualToString:@"window"]) {
        yes = Windows(gApp).count > 0;
    } else if ([t hasPrefix:@"window~"] || [t hasPrefix:@"window="]) {
        NSString *want = [t substringFromIndex:7];
        NSString *title = AXStr((__bridge AXUIElementRef)FocusedWindow(gApp), kAXTitleAttribute);
        yes = TextMatches(title, want, [t characterAtIndex:6] == '=');
    } else if ([t hasPrefix:@"screen "] || [t hasPrefix:@"!screen "] || [t hasPrefix:@"luma"]) {
        gTermKind = @"screen";
        NSMutableDictionary *st = state[t] ?: (state[t] = [NSMutableDictionary dictionary]);
        double at = 0, res = 0;
        yes = ScreenTerm(t, st, &at, &res);
        if (yes) {
            gTermAt = at;
            gTermRes = res;
        }
    } else if ([t hasPrefix:@"ax "]) {
        yes = FindElement(gApp, [t substringFromIndex:3]) != nil;
    } else if ([t hasPrefix:@"!ax "]) {
        yes = FindElement(gApp, [t substringFromIndex:4]) == nil;
    } else if ([t hasPrefix:@"text "]) {
        yes = AXHasText([t substringFromIndex:5]);
    } else if ([t hasPrefix:@"focus "]) {
        id f = AXObj(gApp, kAXFocusedUIElementAttribute);
        if (!f) return NO;
        Spec s = ParseSpec([t substringFromIndex:6]);
        yes = NodeMatches(NodeOf((__bridge AXUIElementRef)f), s);
    } else if ([t hasPrefix:@"view "]) {
        NSArray *a = [[t substringFromIndex:5] componentsSeparatedByString:@" "];
        NSString *name = a.firstObject, *magic = a.count > 1 ? [[a subarrayWithRange:NSMakeRange(1, a.count - 1)] componentsJoinedByString:@" "] : @"";
        NSString *title = AXStr((__bridge AXUIElementRef)FocusedWindow(gApp), kAXTitleAttribute);
        if (!TextMatches(title, name, NO)) return NO;
        if (!state[@"seen"]) state[@"seen"] = @(MonoMs());
        const double seen = [state[@"seen"] doubleValue];
        // Settled: quiet for 300 ms, or 3 s after its window came up.
        double end = 0, res = 0;
        const BOOL quiet = QuietAt(300, gQuietFrom, NO, &end, &res);   // the tree: helpers render views
        if (!quiet && MonoMs() - seen < 3000) return NO;
        gViewResult = ClassifyView(magic);
        yes = gViewResult != nil;
        if (yes) {
            if (!quiet) {
                gTermAt = MonoMs();   // never settled: the 3 s are the app's
                gTermKind = @"cpu-quiet";
            } else if (end > seen) {
                gTermAt = end;
                gTermRes = res;
                gTermKind = @"cpu-quiet";
            } else {
                gTermAt = seen;
            }
        }
    } else {
        fprintf(stderr, "taskbench: unknown condition term \"%s\"\n", t.UTF8String);
        return NO;
    }
    if (yes && gTermAt < 0) gTermAt = MonoMs();
    return yes;
}

// When the alternative that held happened (the latest of its terms), how
// that was observed, and its resolution (-1: the poll's).
static double gCondAt, gCondRes;
static NSString *gCondKind;

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
        double at = -1, res = -1;
        NSString *kind = @"none";
        for (NSString *term in [alt componentsSeparatedByString:@" && "]) {
            if (!Term(term, st)) {
                all = NO;
                break;
            }
            if (gTermAt >= at) {
                at = gTermAt;
                res = gTermRes;
                kind = gTermKind;
            }
        }
        if (!all) continue;
        gCondAt = at;
        gCondRes = res;
        gCondKind = kind;
        if (gViewResult) return label ? [NSString stringWithFormat:@"%@: %@", label, gViewResult] : gViewResult;
        if (label) return label;
        if ([alt hasPrefix:@"quiet "] || [alt hasPrefix:@"settle "]) return @"settled (unverified)";
        return alts.count > 1 ? [@"ok: " stringByAppendingString:alt] : @"ok";
    }
    return nil;
}

static BOOL NeedsScreen(NSString *cond) {
    return [cond rangeOfString:@"screen "].location != NSNotFound || [cond rangeOfString:@"luma"].location != NSNotFound;
}

// ------------------------------------------------------------------ steps

typedef struct {
    int clicks, chords, keys, opens;
} Counts;

static int CountTotal(Counts c) { return c.clicks + c.chords + c.keys + c.opens; }

static BOOL gAborted = NO;
static double gPollMaxMs = 0;   // the last condition's slowest poll

// Poll until the condition holds or the deadline. Returns the matched label,
// "" when it timed out, nil when the owner touched the keyboard or mouse.
// gCondAt, gCondKind and gCondRes say when and how the result was seen.
static NSString *Await(NSString *cond, double timeoutMs, double from) {
    NSMutableDictionary *state = [NSMutableDictionary dictionary];
    gQuietFrom = from;
    const BOOL screen = NeedsScreen(cond);
    if (screen) KeepFramesFrom(gQuietFrom);   // the frame before the input stays, to compare with
    double deadline = MonoMs() + timeoutMs;
    gPollMaxMs = 0;
    NSString *result = nil;
    for (;;) {
        const double tick = MonoMs();
        if (OwnerTouched()) {
            gAborted = YES;
            result = nil;
            break;
        }
        LookForNewPid();
        if (screen && gPid) EnsureCapture(gPid);   // a cold start's window: as soon as it shows
        NSString *m = Condition(cond, state);
        const double took = MonoMs() - tick;
        if (took > gPollMaxMs) gPollMaxMs = took;
        if (m) {
            result = m;
            break;
        }
        // A read that took the benchmark over a second (the first screen
        // read of a newly built tool took 32 s) is not the app's time to
        // answer: the timeout moves on by as much.
        if (took > 1000) deadline += took;
        if (MonoMs() > deadline) {
            result = @"";
            break;
        }
        // Every 10 ms; a poll that walked a large AX tree took longer, and
        // the app gets a third of that again before the next, so the
        // benchmark's questions do not keep it busy.
        SleepMs(MAX(10 - took, took / 3));
    }
    if (screen) KeepFramesFrom(INFINITY);
    return result;
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

// Find TEXT on screen for a click, waiting up to `ms` for it.
static BOOL AwaitScreenPoint(NSString *text, double ms, CGPoint *out) {
    const double deadline = MonoMs() + ms;
    for (;;) {
        if (OwnerTouched()) {
            gAborted = YES;
            return NO;
        }
        LookForNewPid();
        if (gPid && ScreenPoint(text, out)) return YES;
        if (MonoMs() > deadline) return NO;
        SleepMs(50);
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
    StartSampler();
    // Vision's first recognition in a newly built tool took 32 s: done once
    // here, on a picture of a word, before any step reads the screen.
    if ([text rangeOfString:@"screen "].location != NSNotFound || [text rangeOfString:@"sclick "].location != NSNotFound ||
        [text rangeOfString:@"view "].location != NSNotFound) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(NULL, 400, 100, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
        CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, 400, 100));
        NSGraphicsContext *g = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = g;
        [@"taskbench" drawAtPoint:NSMakePoint(20, 30) withAttributes:@{NSFontAttributeName : [NSFont systemFontOfSize:40]}];
        [NSGraphicsContext restoreGraphicsState];
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        Recognise(img);
        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
    }
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
            cpuStart = gPid ? Fresh().cpu : 0;
        }

        // A step read from the screen has the stream running, with a frame of
        // the moment before its input, before the input is sent.
        if (gPid && (NeedsScreen(cond) || [verb isEqualToString:@"sclick"])) CaptureBeforeInput();

        Counts c = {0, 0, 0, 0};
        double settleMs = 0;   // waiting for a click's target to hold still: the app's time, so in wall
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
                // AXPress returns once the app has handled it, which can take
                // the app hundreds of ms: the step's wall starts when it is sent.
                const double pressAt = MonoMs();
                if ([verb isEqualToString:@"press"])
                    done = AXUIElementPerformAction((__bridge AXUIElementRef)el, kAXPressAction) == kAXErrorSuccess;
                if (done) {
                    NotePosted();
                    gLastPostMono = pressAt;
                }
                CGPoint p;
                if (!done && !Activate()) {
                    status = kNotInFront;
                    done = YES;
                } else if (!done && ({
                               const double s0 = MonoMs();
                               const BOOL ok = StableCentre(el, &p);
                               settleMs = MonoMs() - s0;
                               ok;
                           })) {
                    PostClick(p, [verb isEqualToString:@"rclick"] ? kCGMouseButtonRight : kCGMouseButtonLeft,
                              [verb isEqualToString:@"dclick"] ? 2 : 1);
                    done = YES;
                }
                if (!done) status = @"error: element has no press action or frame";
                if ([status isEqualToString:@"ok"]) c.clicks = [verb isEqualToString:@"dclick"] ? 2 : 1;
            }
        } else if ([verb isEqualToString:@"sclick"]) {
            // A click on text read from the screen (an app with no AX tree).
            // Finding it is the benchmark's reading, so it is input time.
            CGPoint p;
            if (!AwaitScreenPoint(arg, optional ? 2000 : timeoutMs, &p)) {
                if (gAborted) break;
                status = optional ? @"skipped: not shown" : @"error: text not found on screen";
            } else if (!Activate()) {
                status = kNotInFront;
            } else {
                PostClick(p, kCGMouseButtonLeft, 1);
                c.clicks = 1;
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
                const double pressAt = MonoMs();
                AXUIElementPerformAction((__bridge AXUIElementRef)item, kAXPressAction);
                NotePosted();
                gLastPostMono = pressAt;
                c.clicks++;
                parent = item;
            }
        } else if (![verb isEqualToString:@"none"]) {
            status = [NSString stringWithFormat:@"error: unknown input %@", verb];
        }
        // The step's wall starts with the last event the input posted, not
        // after the pause the tool takes between keys (the app may already be
        // answering in it).
        const double t1 = gLastPostMono > t0 ? gLastPostMono : MonoMs();
        const double inputMs = MAX(0, t1 - t0 - settleMs);
        NSString *m = @"";
        gCondAt = -1;
        gCondRes = -1;
        gCondKind = @"none";
        if (gAborted) break;
        // A step read from the app's CPU settling, or a soft condition that
        // never held, is unverified. The task is unverified when its last
        // step is, or when a view could not be read; a step on the way that
        // only waited for the app to settle leaves the result verified.
        BOOL stepUnverified = NO, settledOnly = NO;
        if ([status isEqualToString:@"ok"]) {
            m = Await(cond, timeoutMs, t1);
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
        // The step ends when its result happened (gCondAt), not when the
        // benchmark confirmed it: a quiet step at its last busy CPU sample,
        // a screen read at the first frame that showed it.
        const double endAt = gCondAt >= 0 ? gCondAt : MonoMs();
        const double wall = MAX(0, endAt - t1) + settleMs;
        const double res = gCondRes >= 0 ? gCondRes : gPollMaxMs;
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
        if (gPid && !setup && ![verb isEqualToString:@"launch"]) cpuEnd = Fresh().cpu;
        // TASKBENCH_TRACE=DIR: the step's CPU samples (ms since its input,
        // CPU ms in the sample, footprint MB), to check where a quiet step ended.
        if (getenv("TASKBENCH_TRACE") && gPid) {
            NSData *d = SamplesSince(t1);
            const Pt *p = d.bytes;
            NSMutableString *tr = [NSMutableString stringWithFormat:@"# step %d end %.0f: %@\n", n, endAt - t1, line];
            for (NSUInteger i = 1; i < d.length / sizeof(Pt); i++)
                [tr appendFormat:@"%.0f %.2f %.2f %.0f\n", p[i].t - t1, p[i].cpu - p[i - 1].cpu,
                                 p[i].own - p[i - 1].own, p[i].mb];
            NSString *path = [NSString stringWithFormat:@"%s/cpu.txt", getenv("TASKBENCH_TRACE")];
            NSFileHandle *fh = [NSFileHandle fileHandleForWritingAtPath:path];
            if (!fh) {
                [@"" writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil];
                fh = [NSFileHandle fileHandleForWritingAtPath:path];
            }
            [fh seekToEndOfFile];
            [fh writeData:[tr dataUsingEncoding:NSUTF8StringEncoding]];
            [fh closeFile];
        }
        Emit([NSString stringWithFormat:@"step\t%d\t%@\t%d\t%.0f\t%.0f\t%.0f\t%.0f\t%@\t%@\t%@\t%@", n,
                                        setup ? @"setup" : [verb isEqualToString:@"launch"] ? @"launch" : @"task",
                                        CountTotal(c), inputMs, wall, gPollMaxMs, res,
                                        m.length ? gCondKind : @"-", status, Clean(m), Clean(line)]);
        gPollMaxMs = 0;
        if (gPid) Emit([NSString stringWithFormat:@"pid\t%d", gPid]);
        if (![status isEqualToString:@"ok"] && ![status isEqualToString:@"unverified"] && !skipped) {
            failed = YES;
            break;
        }
    }
    // A launch whose condition timed out may have started the app all the
    // same: its pid is still reported, so the script stops it.
    if (!gPid) {
        LookForNewPid();
        if (gPid) Emit([NSString stringWithFormat:@"pid\t%d", gPid]);
    }
    if (gAborted) {
        if (gPid) Emit([NSString stringWithFormat:@"pid\t%d", gPid]);
        Emit(@"aborted\tinput from the keyboard or mouse during the run");
        return 3;
    }
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
            if (Settled(1000)) break;
            SleepMs(50);
        }
        settled = Latest().mb;
    }
    os_unfair_lock_lock(&gMeasureLock);
    const double peak = gPeakMB;
    os_unfair_lock_unlock(&gMeasureLock);
    StopCapture();
    const double cpu = cpuStart >= 0 ? cpuEnd - cpuStart : 0;
    const BOOL unverified = viewUnverified || lastUnverified;
    if (lastSettled) [matched addObject:@"the result was read from the app's CPU settling"];
    Emit([NSString stringWithFormat:
                       @"summary\tstatus=%@\tpid=%d\twall_ms=%.0f\tinputs=%d\tclicks=%d\tchords=%d\tkeys=%d\topens=%d\t"
                       @"setup_inputs=%d\tsetup_ms=%.0f\tlaunch_ms=%.0f\tcpu_ms=%.0f\tpeak_mb=%.0f\tsettled_mb=%.0f\t"
                       @"matched=%@",
                       failed ? @"failed" : unverified ? @"unverified" : @"ok", gPid, taskWall, CountTotal(task),
                       task.clicks, task.chords, task.keys, task.opens, setupInputs, setupWall, launchMs, cpu,
                       peak, settled, [matched componentsJoinedByString:@"; "]]);
    if (gOut) fclose(gOut);
    return failed ? 1 : 0;
}

// ------------------------------------------------------------------ quit

static int Quit(NSString *exe, pid_t pid) {
    NSString *path = ExePath(pid);
    if (!path) return 0;   // already gone
    if (!ExeMatches(path, exe)) {
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

static int AxDump(pid_t pid, int maxDepth, BOOL manual) {
    Adopt(pid);
    // An Electron app started without --force-renderer-accessibility builds
    // its tree only when asked, and takes a few seconds to (`manual`).
    if (manual) {
        AXUIElementSetAttributeValue(gApp, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
        SleepMs(3000);
    }
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
                    "[--out FILE] [--pid PID] | quit --exe PATH PID | axdump PID [DEPTH] [manual] | screentext PID | tree PID | media DIR | "
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
            printf("screen recording: %s\n", CGPreflightScreenCaptureAccess()
                                                 ? "allowed"
                                                 : "not allowed (Zed's results, read from the screen, read as unverified)");
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
            pid_t attach = 0;
            for (int i = 2; i + 1 < argc; i += 2) {
                NSString *k = @(argv[i]), *v = @(argv[i + 1]);
                if ([k isEqualToString:@"--exe"]) exe = v;
                else if ([k isEqualToString:@"--steps"]) steps = v;
                else if ([k isEqualToString:@"--state"]) gStatePath = v;
                else if ([k isEqualToString:@"--out"]) out = v;
                else if ([k isEqualToString:@"--pid"]) attach = (pid_t)v.intValue;
                else return Usage();
            }
            if (!exe || !steps || !gStatePath) return Usage();
            if (!AXIsProcessTrusted()) {
                fprintf(stderr, "taskbench: this terminal has no Accessibility permission (taskbench-mac check)\n");
                return 4;
            }
            // --pid: drive a copy already running (one a kept run left, while
            // working out a step); it must be the --exe's.
            if (attach) {
                if (!ExeMatches(ExePath(attach), exe)) {
                    fprintf(stderr, "taskbench: pid %d is not %s\n", attach, exe.UTF8String);
                    return 2;
                }
                Adopt(attach);
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
            return AxDump((pid_t)atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 40, argc > 4 && !strcmp(argv[4], "manual"));
        }
        if ([cmd isEqualToString:@"screentext"] && argc == 3) {
            NSString *s = ScreenText((pid_t)atoi(argv[2]));
            printf("%s\n", (s ?: @"(nothing read)").UTF8String);
            return s.length ? 0 : 1;
        }
        if ([cmd isEqualToString:@"ocr"] && argc == 3) {   // what Vision reads in a picture
            CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[2])], NULL);
            CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
            VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
            req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
            req.usesLanguageCorrection = NO;
            NSError *err = nil;
            VNImageRequestHandler *h = [[VNImageRequestHandler alloc] initWithCGImage:img options:@{}];
            BOOL ok = img && [h performRequests:@[ req ] error:&err];
            printf("%s\n%s\n", ok ? "ok" : "failed", (err.description ?: TextOf(req.results ?: @[])).UTF8String);
            return ok ? 0 : 1;
        }
        if ([cmd isEqualToString:@"capturetest"] && argc == 3) {   // how long the stream takes to start
            gPid = atoi(argv[2]);
            double t0 = MonoMs();
            BOOL ok = EnsureCapture(gPid);
            double t1 = MonoMs();
            CaptureBeforeInput();
            double t2 = MonoMs();
            TBFrame *f = NewestFrame();
            NSString *txt = f ? FrameText(f) : nil;
            printf("start %s %.0f ms, first frame %.0f ms, read %.0f ms, %lu characters\n", ok ? "ok" : "failed", t1 - t0,
                   t2 - t1, MonoMs() - t2, (unsigned long)txt.length);
            StopCapture();
            return 0;
        }
        if ([cmd isEqualToString:@"windows"] && argc == 3) {
            NSArray *wins = CFBridgingRelease(CGWindowListCopyWindowInfo(
                kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
            for (NSDictionary *w in wins)
                if ([w[(__bridge id)kCGWindowOwnerPID] intValue] == atoi(argv[2]))
                    printf("%u layer=%d %s \"%s\"\n", [w[(__bridge id)kCGWindowNumber] unsignedIntValue],
                           [w[(__bridge id)kCGWindowLayer] intValue], [w[(__bridge id)kCGWindowBounds] description].UTF8String,
                           [w[(__bridge id)kCGWindowName] ?: @"" UTF8String]);
            return 0;
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
