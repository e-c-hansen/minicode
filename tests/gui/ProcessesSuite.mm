// ProcessesSuite.mm — processes and ports per folder (spec feature 5):
//   make guitest SUITES=processes
// A web server started in a terminal shows under that terminal with its
// port, in the panel and in `minicode procs`; with `disown` and the terminal
// closed it moves to the orphans, and Stop ends it without asking. A sleep
// in a second terminal: Reveal Origin shows its terminal, and Stop asks
// first (Cancel keeps it). A server started outside minicode in the folder
// is "not started here". The time a look takes is measured with at least
// 200 processes on the machine. Every process here is one this suite
// started, and only those are signalled.
#import "Harness.h"
#import "ControlCommands.h"
#import "EditorController.h"
#import "Lsp.h"
#import "ProcessMonitor.h"
#import "ProcessesPanel.h"
#import "Terminal.h"
#import "TerminalPanel.h"

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

@interface EditorController (ProcessesSuite)
- (MCProcessesPanel *)processesPanel;
@end

@interface MCProcsState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *termA, *termB;
@property(nonatomic, strong) MCProcessesPanel *panel;
@property(nonatomic, copy) NSString *server;   // python3, or nc
@property(nonatomic, copy) NSString *root;     // the window's folder
@property(nonatomic) int port, port2;
@property(nonatomic) int serverPid, outsidePid, sleepPid;
@property(nonatomic, strong) NSMutableArray<NSNumber *> *started;   // every pid to clean up
@property(nonatomic, copy) NSString *openedURL;
@property(nonatomic) double worstSample, worstMain, worstSampleCpu, worstMainCpu;
@property(nonatomic) BOOL extraStarted;   // the sleeps outside the folder are up
@property(nonatomic) NSUInteger listed;
@property(nonatomic) NSUInteger alerts, windows;
@end
@implementation MCProcsState
@end

namespace {

NSString *CLI() { return [MCGuiBuildDir() stringByAppendingPathComponent:@"minicode-cli"]; }
NSString *Pid() { return [NSString stringWithFormat:@"%d", getpid()]; }

int FreePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t len = sizeof a;
    int port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

NSUInteger TabOf(MCTerminalPanel *tp, TerminalView *t) {
    for (NSUInteger i = 0; i < tp.tabCount; i++)
        if ([[tp terminalsInTab:i] containsObject:t]) return i;
    return NSNotFound;
}

BOOL Alive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

// A command that serves `port` on 127.0.0.1 until it is stopped, quiet.
NSString *ServeCommand(MCProcsState *s, int port) {
    if ([s.server.lastPathComponent hasPrefix:@"python"])
        // http.server, less its reverse DNS lookup (see serve.py).
        return [NSString stringWithFormat:@"'%@' '%@' %d", s.server,
                [[MCGuiBuildDir() stringByDeletingLastPathComponent]
                    stringByAppendingPathComponent:@"tests/gui/serve.py"], port];
    return [NSString stringWithFormat:@"/usr/bin/nc -lk 127.0.0.1 %d", port];
}

// The rows of the panel from the first group row whose description starts
// with `group` up to the next group.
NSArray<NSString *> *RowsIn(MCProcessesPanel *panel, NSString *group) {
    NSMutableArray *out = [NSMutableArray array];
    BOOL in = NO;
    for (NSString *r in [panel rowDescriptions]) {
        if ([r hasPrefix:@"G "]) {
            in = [r hasPrefix:group];
            continue;
        }
        if (in) [out addObject:r];
    }
    return out;
}

BOOL GroupHasPort(MCProcessesPanel *panel, NSString *group, int port) {
    NSString *want = [NSString stringWithFormat:@":%d", port];
    for (NSString *r in RowsIn(panel, group))
        if ([r containsString:want] && ![r hasSuffix:@" exited"]) return YES;
    return NO;
}

// The pid of the live process listening on `port` in the monitor's view.
int PidWithPort(MCProcsState *s, int port) {
    Procs::View v = [[MCProcessMonitor shared] viewForWindow:MCControlWindowID(s.c)
                                                      folder:MCProcessRealPath(s.c.rootPath)];
    std::function<int(const std::vector<Procs::Node> &)> find = [&](const std::vector<Procs::Node> &ns) {
        for (const Procs::Node &n : ns) {
            if (n.proc.alive)
                for (const Procs::Listen &l : n.proc.ports)
                    if (l.port == port) return n.proc.key.pid;
            if (int p = find(n.children)) return p;
        }
        return 0;
    };
    for (const Procs::Group &g : v.groups)
        if (int p = find(g.roots)) return p;
    return 0;
}

// Live processes named `name` that terminal `term` started.
std::vector<int> PidsNamed(MCProcsState *s, TerminalView *term, const std::string &name) {
    std::vector<int> out;
    Procs::View v = [[MCProcessMonitor shared] viewForWindow:MCControlWindowID(s.c)
                                                      folder:MCProcessRealPath(s.c.rootPath)];
    std::function<void(const std::vector<Procs::Node> &)> walk = [&](const std::vector<Procs::Node> &ns) {
        for (const Procs::Node &n : ns) {
            if (n.proc.alive && Procs::displayName(n.proc) == name) out.push_back(n.proc.key.pid);
            walk(n.children);
        }
    };
    for (const Procs::Group &g : v.groups)
        if (g.kind == Procs::GroupKind::Terminal && g.terminal == MCControlTerminalID(term)) walk(g.roots);
    return out;
}

// The command line, in the suite's window unless the arguments name a folder.
void CLIStep(MCProcsState *st, NSString *name, NSArray<NSString *> *args,
             void (^check)(int, NSString *, NSString *)) {
    MCGuiAsync(name, 30, ^(MCGuiDone done) {
        NSMutableArray *argv = [NSMutableArray arrayWithObject:CLI()];
        [argv addObjectsFromArray:args];
        if (![args containsObject:@"--folder"])
            [argv addObjectsFromArray:@[@"--window", [NSString stringWithFormat:@"%ld",
                                                      (long)MCControlWindowID(st.c)]]];
        [argv addObjectsFromArray:@[@"--pid", Pid()]];
        MCGuiRunProcess(argv, nil, ^(int status, NSString *out, NSString *err) {
            check(status, out, err);
            done();
        });
    });
}

NSDictionary *JSONOf(NSString *text) {
    id j = [NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding]
                                           options:0 error:nil];
    return [j isKindOfClass:NSDictionary.class] ? j : nil;
}

// The group of a `procs --json` answer holding `pid`, or nil.
NSDictionary *GroupWithPid(NSDictionary *j, int pid) {
    for (NSDictionary *g in j[@"groups"])
        for (NSDictionary *p in g[@"processes"])
            if ([p[@"pid"] intValue] == pid) return g;
    return nil;
}

void Sample(NSString *name) {
    MCGuiAsync(name, 10, ^(MCGuiDone done) { [[MCProcessMonitor shared] sampleNow:done]; });
}

}  // namespace

MC_GUI_SUITE(processes) {
    MCProcsState *s = [MCProcsState new];
    s.started = [NSMutableArray array];

    MCGuiStep(@"a terminal, two free ports, and a server to run", ^{
        s.c = MCGuiController();
        s.root = s.c.rootPath;
        MCGuiNote(@"the window's folder: %@", s.root);
        s.windows = MCGuiControllers().count;
        s.port = FreePort();
        s.port2 = FreePort();
        while (s.port2 == s.port) s.port2 = FreePort();
        GCHECK(s.port > 0 && s.port2 > 0);
        s.server = MCFindProgram(@"python3") ?: @"/usr/bin/nc";
        MCGuiNote(@"serving with %@ on %d and %d", s.server, s.port, s.port2);
        // A terminal of its own (other suites may have left theirs).
        s.termA = [s.c newTerminalForAgentIn:s.root beside:NO];
        GCHECK(s.termA != nil);
        GCHECK(s.termA.shellPid == 0 || s.termA.shellPid > 1);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.termA.atPrompt && !s.termA.busy; });
    MCGuiStep(@"a server in the background, in the folder", ^{
        GCHECK(s.termA.shellPid > 1);
        [s.termA runCommand:[NSString stringWithFormat:@"cd '%@' && %@ </dev/null >/dev/null 2>&1 &",
                             s.root, ServeCommand(s, s.port)]];
    });
    MCGuiWait(@"the shell is back at its prompt", 10, ^BOOL { return s.termA.atPrompt; });

    // ------------------------------------------------------------ the panel
    MCGuiStep(@"View > Toggle Processes shows the panel in the sidebar's place", ^{
        NSMenuItem *item = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if (i.action == @selector(toggleProcesses:)) item = i;
        GCHECK(item != nil && [item.menu.title isEqualToString:@"View"]);
        [NSApp sendAction:@selector(toggleProcesses:) to:nil from:item];
        s.panel = [s.c processesPanel];
        GCHECK(s.panel.window == s.c.window);
        GCHECK(s.panel.superview != nil);
        GCHECK([MCProcessMonitor shared].interval == 2);   // looks 2 s apart while it shows
        GCHECK([s.panel.helpText containsString:@"can be missed"]);
    });
    MCGuiWait(@"the server under its terminal, with its port", 15, ^BOOL {
        return GroupHasPort(s.panel, @"G terminal", s.port);
    });
    MCGuiStep(@"its row, the group and the heading", ^{
        s.serverPid = PidWithPort(s, s.port);
        GCHECK(s.serverPid > 1);
        if (s.serverPid > 1) [s.started addObject:@(s.serverPid)];
        NSArray *rows = [s.panel rowDescriptions];
        MCGuiNote(@"%@", [rows componentsJoinedByString:@"\n"]);
        GCHECK([rows.firstObject hasPrefix:@"G terminal "]);
        NSString *want = [NSString stringWithFormat:@"P 0 %d ", s.serverPid];
        BOOL found = NO;
        for (NSString *r in rows) found |= [r hasPrefix:want];
        GCHECK(found);
        GCHECK([s.panel.summaryText containsString:@" in "] &&
               [s.panel.summaryText containsString:@" process"]);
        GCHECK([MCProcessMonitor shared].lastListed > 10);
    });

    // ------------------------------------------------------ minicode procs
    CLIStep(s, @"minicode procs --json lists it under the terminal", @[@"procs", @"--json"],
            ^(int status, NSString *out, NSString *err) {
        GCHECK(status == 0);
        NSDictionary *j = JSONOf(out);
        GCHECK(j != nil);
        NSDictionary *g = GroupWithPid(j, s.serverPid);
        GCHECK([g[@"kind"] isEqualToString:@"terminal"]);
        GCHECK([g[@"terminal"] integerValue] == MCControlTerminalID(s.termA));
        NSDictionary *p = nil;
        for (NSDictionary *q in g[@"processes"])
            if ([q[@"pid"] intValue] == s.serverPid) p = q;
        GCHECK([p[@"ports"][0][@"port"] intValue] == s.port);
        GCHECK([p[@"ports"][0][@"address"] isEqualToString:@"127.0.0.1"]);
        GCHECK([p[@"memory"] doubleValue] > 100000);
        GCHECK([p[@"cwd"] isEqualToString:MCProcessRealPath(s.root)]);
        NSString *portText = [NSString stringWithFormat:@"%d", s.port];
        GCHECK([p[@"command"] containsString:portText]);
        GCHECK([j[@"folder"] isEqualToString:MCProcessRealPath(s.root)]);
        GCHECK([j[@"window"] integerValue] == MCControlWindowID(s.c));
        if (!g) MCGuiNote(@"procs: %@ %@", out, err);
    });
    CLIStep(s, @"minicode procs prints a table", @[@"procs"], ^(int status, NSString *out, NSString *) {
        GCHECK(status == 0);
        GCHECK([out containsString:@" process"] &&
               [out containsString:[@" in " stringByAppendingString:MCProcessRealPath(s.root)]]);
        NSString *term = [NSString stringWithFormat:@"(terminal %ld)", (long)MCControlTerminalID(s.termA)];
        NSString *portText = [NSString stringWithFormat:@"%d", s.port];
        GCHECK([out containsString:term]);
        GCHECK([out containsString:portText]);
        GCHECK([out containsString:@"PID"] && [out containsString:@"COMMAND"]);
    });
    CLIStep(s, @"--folder of a folder no window shows: nothing, and no window opens",
            @[@"procs", @"--folder", @"/"], ^(int status, NSString *out, NSString *) {
        GCHECK(status == 0);
        GCHECK(MCGuiControllers().count == s.windows);
        (void)out;
    });
    CLIStep(s, @"--folder that is not a folder", @[@"procs", @"--folder", @"/no/such/dir"],
            ^(int status, NSString *, NSString *err) {
        GCHECK(status == 1 && [err containsString:@"not a folder"]);
    });

    // ------------------------------------------ a second terminal, and speed
    MCGuiStep(@"a second terminal", ^{
        s.termB = [s.c newTerminalForAgentIn:s.root beside:NO];
        GCHECK(s.termB != nil && s.termB != s.termA);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.termB.atPrompt && !s.termB.busy; });
    MCGuiStep(@"a sleep and forty more", ^{
        [s.termB runCommand:@"sleep 400 & for i in {1..40}; do sleep 401 & done"];
    });
    MCGuiWait(@"41 sleeps under the second terminal", 15, ^BOOL {
        return PidsNamed(s, s.termB, "sleep").size() == 41;
    });
    MCGuiStep(@"the machine has 200 processes", ^{
        for (int p : PidsNamed(s, s.termB, "sleep")) [s.started addObject:@(p)];
        // The first sleep is the one started on its own.
        Procs::View v = [[MCProcessMonitor shared] viewForWindow:MCControlWindowID(s.c)
                                                          folder:MCProcessRealPath(s.root)];
        int64_t first = INT64_MAX;
        for (const Procs::Group &g : v.groups)
            if (g.terminal == MCControlTerminalID(s.termB))
                for (const Procs::Node &n : g.roots)
                    if (n.proc.key.start < first && n.proc.command == "sleep 400") {
                        first = n.proc.key.start;
                        s.sleepPid = n.proc.key.pid;
                    }
        GCHECK(s.sleepPid > 1);
        const NSUInteger listed = [MCProcessMonitor shared].lastListed;
        s.extraStarted = listed >= 220;
        if (listed < 220) {
            // More, outside the folder, so the table has at least 200.
            NSString *script = [NSString stringWithFormat:
                @"cd / && for i in $(seq %lu); do sleep 120 </dev/null >/dev/null 2>&1 & echo $!; done",
                (unsigned long)(220 - listed)];
            MCGuiRunProcess(@[@"/bin/sh", @"-c", script], nil, ^(int st, NSString *out, NSString *) {
                (void)st;
                for (NSString *l in [out componentsSeparatedByString:@"\n"])
                    if (l.intValue > 1) [s.started addObject:@(l.intValue)];
                s.extraStarted = YES;
            });
        }
    });
    MCGuiWait(@"the sleeps outside the folder are started", 30, ^BOOL { return s.extraStarted; });
    for (int i = 0; i < 5; i++) {
        Sample([NSString stringWithFormat:@"a look (%d of 5)", i + 1]);
        MCGuiStep(@"how long it took", ^{
            MCProcessMonitor *m = [MCProcessMonitor shared];
            s.worstSample = MAX(s.worstSample, m.lastSampleMilliseconds);
            s.worstMain = MAX(s.worstMain, m.lastMainThreadMilliseconds);
            s.worstSampleCpu = MAX(s.worstSampleCpu, m.lastSampleCpuMilliseconds);
            s.worstMainCpu = MAX(s.worstMainCpu, m.lastMainThreadCpuMilliseconds);
            s.listed = MAX(s.listed, m.lastListed);
        });
    }
    MCGuiStep(@"the panel's own redraw", ^{
        const double t0 = MCGuiNowMs(), c0 = MCGuiThreadCpuMs();
        for (int i = 0; i < 10; i++) [s.panel reload];
        const double ms = (MCGuiThreadCpuMs() - c0) / 10;
        MCGuiNote(@"the panel's redraw after a look: %.3f ms of CPU (%.3f ms wall)", ms,
                  (MCGuiNowMs() - t0) / 10);
        GCHECK_TIME(ms, 20, @"the panel's redraw");
    });
    MCGuiStep(@"under 20 ms off the main thread, and little on it", ^{
        MCGuiNote(@"a look at %lu processes: %.2f ms of CPU off the main thread (%.2f ms wall), "
                  @"%.3f ms on it (%.3f ms wall)", (unsigned long)s.listed, s.worstSampleCpu,
                  s.worstSample, s.worstMainCpu, s.worstMain);
        GCHECK(s.listed >= 200);
        // CPU time of the threads doing it: in wall time a loaded machine
        // (and CI's runner once) took the look past 20 ms.
        GCHECK_TIME(s.worstSampleCpu, 20, @"a look off the main thread");
        GCHECK_TIME(s.worstMainCpu, 5, @"a look's part on the main thread");
    });
    MCGuiStep(@"a picture of the panel", ^{
        [s.panel expandAll];
        MCGuiSnapshot(s.panel, @"processes-panel");
        // The rows stay inside the pane (the outline column once grew as
        // rows nested, and the memory figures went past the edge).
        NSOutlineView *o = s.panel.outline;
        GCHECK(NSWidth(o.frame) <= NSWidth(o.enclosingScrollView.frame));
        NSView *cell = [o viewAtColumn:0 row:1 makeIfNecessary:YES];
        GCHECK(NSMaxX(cell.frame) <= NSWidth(o.enclosingScrollView.frame));
    });
    MCGuiStep(@"the forty go (this suite started them)", ^{
        for (int p : PidsNamed(s, s.termB, "sleep"))
            if (p != s.sleepPid) kill(p, SIGTERM);
    });
    MCGuiWait(@"only the first sleep is left", 10, ^BOOL {
        return PidsNamed(s, s.termB, "sleep").size() == 1;
    });

    // ------------------------------------------- Reveal Origin, Stop asking
    MCGuiStep(@"the keyboard in the first terminal", ^{
        MCTerminalPanel *tp = [s.c valueForKey:@"terminalPanel"];
        [tp selectTab:TabOf(tp, s.termA)];
        [s.termA focusInput];
        GCHECK(tp.activeTerminal == s.termA);
    });
    MCGuiStep(@"Reveal Origin on the sleep shows the second terminal", ^{
        [s.panel expandAll];
        NSInteger row = [s.panel rowForPid:s.sleepPid];
        GCHECK(row >= 0);
        NSMenu *menu = [s.panel menuForRow:row];
        GCHECK([menu itemWithTitle:@"Stop…"] != nil && [menu itemWithTitle:@"Reveal Origin"].enabled);
        GCHECK([menu itemWithTitle:@"Copy Process ID"] != nil);
        [s.panel revealOriginOfRow:row];
        MCTerminalPanel *tp = [s.c valueForKey:@"terminalPanel"];
        GCHECK(tp.activeTerminal == s.termB);
        GCHECK(tp.selectedTab == (NSInteger)TabOf(tp, s.termB));
    });
    MCGuiStep(@"Stop asks first, and Cancel keeps it", ^{
        s.alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertSecondButtonReturn)]);
        [s.panel stopRow:[s.panel rowForPid:s.sleepPid]];
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
        GCHECK_EQ_STR(MCGuiAlertsShown().lastObject, @"Stop sleep?");
    });
    // A pause before a negative check: the sleep must still be alive after
    // Cancel, and no condition says "nothing will happen".
    MCGuiPause(0.5);
    MCGuiStep(@"still running; Stop again, answered Stop", ^{
        GCHECK(Alive(s.sleepPid));
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [s.panel stopRow:[s.panel rowForPid:s.sleepPid]];
    });
    MCGuiWait(@"the sleep ends", 5, ^BOOL { return !Alive(s.sleepPid); });
    MCGuiWait(@"its row says it exited", 10, ^BOOL {
        NSString *want = [NSString stringWithFormat:@" %d sleep ", s.sleepPid];
        for (NSString *r in [s.panel rowDescriptions])
            if ([r containsString:want] && [r hasSuffix:@" exited"]) return YES;
        return NO;
    });
    MCGuiStep(@"the second terminal's shell lives on", ^{
        GCHECK(Alive(s.termB.shellPid));
        GCHECK(Alive(s.serverPid));
    });

    // ------------------------------------------------------------ Open port
    MCGuiStep(@"Return on the server's row opens its port", ^{
        void (^real)(NSString *) = s.panel.onOpenURL;
        GCHECK(real != nil);
        __weak MCProcsState *weakS = s;
        s.panel.onOpenURL = ^(NSString *url) { weakS.openedURL = url; };   // no page is loaded here
        [s.panel activateRow:[s.panel rowForPid:s.serverPid]];
        GCHECK_EQ_STR(s.openedURL, ([NSString stringWithFormat:@"http://localhost:%d", s.port]));
        NSMenu *menu = [s.panel menuForRow:[s.panel rowForPid:s.serverPid]];
        NSString *title = [NSString stringWithFormat:@"Open localhost:%d", s.port];
        GCHECK([menu itemWithTitle:title] != nil);
        s.panel.onOpenURL = real;
    });

    // ----------------------------------------------- disown, close: orphans
    MCGuiStep(@"disown, in the first terminal", ^{ [s.termA runCommand:@"disown"]; });
    MCGuiWait(@"done", 10, ^BOOL { return s.termA.atPrompt && !s.termA.busy; });
    MCGuiStep(@"closing the terminal (it asks: the server still runs)", ^{
        s.alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        MCTerminalPanel *tp = [s.c valueForKey:@"terminalPanel"];
        GCHECK([tp closeTerminal:s.termA]);
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);   // "Close the terminal?"
        GCHECK(![tp.terminals containsObject:s.termA]);
    });
    MCGuiWait(@"the server moves to the orphans", 15, ^BOOL {
        return GroupHasPort(s.panel, @"G orphans", s.port);
    });
    MCGuiStep(@"it lives, and says where it came from", ^{
        GCHECK(Alive(s.serverPid));
        NSMenu *menu = [s.panel menuForRow:[s.panel rowForPid:s.serverPid]];
        GCHECK([menu itemWithTitle:@"Stop"] != nil);   // no question for an orphan
        GCHECK(![menu itemWithTitle:@"Reveal Origin"].enabled);
    });
    CLIStep(s, @"minicode procs has it as an orphan", @[@"procs", @"--json"], ^(int st, NSString *out, NSString *) {
        GCHECK(st == 0);
        GCHECK([GroupWithPid(JSONOf(out), s.serverPid)[@"kind"] isEqualToString:@"orphans"]);
    });
    MCGuiStep(@"Stop on the orphan, unasked", ^{
        s.alerts = MCGuiAlertsShown().count;
        [s.panel stopRow:[s.panel rowForPid:s.serverPid]];
        GCHECK(MCGuiAlertsShown().count == s.alerts);
    });
    MCGuiWait(@"the server ends", 6, ^BOOL { return !Alive(s.serverPid); });

    // --------------------------------------------------- not started here
    MCGuiAsync(@"a server started outside minicode, in the folder", 10, ^(MCGuiDone done) {
        NSString *web = [s.root stringByAppendingPathComponent:@"web"];
        [NSFileManager.defaultManager createDirectoryAtPath:web withIntermediateDirectories:YES
                                                attributes:nil error:nil];
        // Its shell exits at once, so launchd is its parent, as for one
        // started in Terminal.app.
        NSString *script = [NSString stringWithFormat:@"cd '%@' || exit 1; %@ </dev/null >/dev/null 2>&1 & echo $!",
                            web, ServeCommand(s, s.port2)];
        MCGuiRunProcess(@[@"/bin/sh", @"-c", script], nil, ^(int st, NSString *out, NSString *) {
            GCHECK(st == 0);
            s.outsidePid = out.intValue;
            if (s.outsidePid > 1) [s.started addObject:@(s.outsidePid)];
            done();
        });
    });
    MCGuiWait(@"it shows under not started here, with its port", 15, ^BOOL {
        return GroupHasPort(s.panel, @"G outside", s.port2);
    });
    CLIStep(s, @"and in minicode procs", @[@"procs", @"--json"], ^(int st, NSString *out, NSString *) {
        GCHECK(st == 0);
        NSDictionary *g = GroupWithPid(JSONOf(out), s.outsidePid);
        GCHECK([g[@"kind"] isEqualToString:@"outside"] && [g[@"title"] isEqualToString:@"Not started here"]);
        GCHECK([JSONOf(out)[@"help"] containsString:@"can be missed"]);
    });
    MCGuiStep(@"Stop asks for it too; answered Stop", ^{
        NSInteger row = [s.panel rowForPid:s.outsidePid];
        GCHECK(row >= 0);
        GCHECK(![[s.panel menuForRow:row] itemWithTitle:@"Reveal Origin"].enabled);
        s.alerts = MCGuiAlertsShown().count;
        MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
        [s.panel stopRow:row];
        GCHECK(MCGuiAlertsShown().count == s.alerts + 1);
    });
    MCGuiWait(@"it ends", 6, ^BOOL { return !Alive(s.outsidePid); });

    // --------------------------------------------------------------- after
    MCGuiStep(@"the panel goes, the tree comes back", ^{
        [s.c toggleProcesses:nil];
        GCHECK(s.panel.window == nil);
        GCHECK([MCProcessMonitor shared].interval == 10);
        for (NSNumber *p in s.started)
            if (Alive(p.intValue)) kill(p.intValue, SIGKILL);   // only what this suite started
        MCTerminalPanel *tp = [s.c valueForKey:@"terminalPanel"];
        // An answer only for a question that will come: one left queued
        // answered the next suite's (terminal_paste's Cancel became Paste).
        if (s.termB && [tp.terminals containsObject:s.termB]) {
            if (s.termB.runningCommands.count) MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
            [tp closeTerminal:s.termB];
        }
    });
}
