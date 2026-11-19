// TerminalPtySuite.mm — `make guitest SUITES=terminal_pty`: a terminal's
// pty (MCPty in Terminal.mm) is torn down whole when the terminal closes.
//   - An idle shell: it is reaped (kill(pid, 0) fails with ESRCH; never
//     waitpid, which would take the app's own reap away), the master is
//     closed, both I/O sources are cancelled and finished, and the exited
//     handler ran.
//   - A shell that ignores the hang-up (`trap '' HUP; exec sleep`) is
//     killed 2 s after it, so it is gone within 2 s and a margin, and not
//     before the kill (it really ignored SIGHUP).
//   - 1 MB written into a program that does not read: the pty fills, the
//     rest waits on the I/O queue (the write source armed), the main thread
//     spends almost nothing on it and stays responsive, and closing still
//     tears everything down, the program included.
//   - Closing while `yes` floods the terminal: no crash, the shell and yes
//     gone, no zombie left.
//   - The shell's own end (terminal.close-on-exit, TerminalExit.h): `exit`
//     typed at the prompt closes the terminal, torn down the same way, and
//     as the last one takes the dock with it; Ctrl+D at an empty prompt
//     does too; with the setting off the terminal stays, saying "Process
//     exited (code 3)", and Return starts a new shell; a shell killed with
//     SIGKILL never closes its terminal: "The shell ended (signal 9)",
//     then Return starts a new one.
// With editor.session-host on (CI's second pass) the session's host owns
// the pty (Shared::Pty on its thread, by MCPty's rules): the same cases,
// with the host's master, its write buffer and its reaping looked at
// through MCSessionInspectTerminal in place of MCPty's state.
#import "Harness.h"
#import "AppSettings.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalPanel.h"
#import "TerminalGridView.h"
#import "SessionLink.h"
#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace {

bool PidGone(pid_t pid) { return pid > 0 && kill(pid, 0) == -1 && errno == ESRCH; }

bool Zombie(pid_t pid) {
    struct proc_bsdinfo info = {};
    return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) == (int)sizeof info &&
           info.pbi_status == SZOMB;
}

// What a descriptor was, so a closed one that the number now names again
// (another file opened since) is not taken for it.
struct FdIdentity { dev_t dev = 0, rdev = 0; ino_t ino = 0; };

FdIdentity IdentityOf(int fd) {
    struct stat st = {};
    FdIdentity id;
    if (fstat(fd, &st) == 0) { id.dev = st.st_dev; id.rdev = st.st_rdev; id.ino = st.st_ino; }
    return id;
}

bool FdClosed(int fd, FdIdentity was) {
    struct stat st = {};
    if (fstat(fd, &st) != 0) return errno == EBADF;
    return st.st_dev != was.dev || st.st_rdev != was.rdev || st.st_ino != was.ino;
}

bool ShellNameIs(TerminalView *t, const char *name) {
    char buf[PROC_PIDPATHINFO_MAXSIZE] = {0};
    return t.shellPid > 0 && proc_name(t.shellPid, buf, sizeof buf) > 0 && strcmp(buf, name) == 0;
}

// The pty's I/O-queue state, read on that queue; the host's write buffer
// when the host owns the pty (`pty` nil).
BOOL WriteArmed(id pty, unsigned long long hostTerm) {
    if (hostTerm) return MCSessionInspectTerminal(hostTerm).writeWaiting;
    __block BOOL armed = NO;
    dispatch_queue_t q = [pty valueForKey:@"queue"];
    dispatch_sync(q, ^{ armed = [[pty valueForKey:@"writeArmed"] boolValue]; });
    return armed;
}

}  // namespace

@interface MCPtyState : NSObject
@property(nonatomic, strong) EditorController *c;
@property(nonatomic, strong) TerminalView *term;
@property(nonatomic, strong) id pty;       // the terminal's MCPty, kept past the close
@property(nonatomic) unsigned long long hostTerm;   // or its terminal on the session's host
@property(nonatomic) pid_t pid;            // the shell
@property(nonatomic) pid_t child;          // its foreground program, when one runs
@property(nonatomic) int fd;               // the pty master
@property(nonatomic) FdIdentity fdWas;
@property(nonatomic) double closedAt;      // MCGuiNowMs at the close
@end
@implementation MCPtyState
@end

// The longest stretch of main-thread CPU between two fires of a 1 ms timer.
@interface MCPtyStallMeter : NSObject
@property(nonatomic, readonly) double busiest;
@property(nonatomic, readonly) NSUInteger fires;
- (void)start;
- (void)stop;
@end
@implementation MCPtyStallMeter {
    NSTimer *_timer;
    double _lastCpu;
}
- (void)start {
    _busiest = 0;
    _fires = 0;
    _lastCpu = MCGuiThreadCpuMs();
    _timer = [NSTimer timerWithTimeInterval:0.001 target:self selector:@selector(tick:)
                                   userInfo:nil repeats:YES];
    _timer.tolerance = 0;
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}
- (void)tick:(NSTimer *)t {
    (void)t;
    const double cpu = MCGuiThreadCpuMs();
    if (cpu - _lastCpu > _busiest) _busiest = cpu - _lastCpu;
    _lastCpu = cpu;
    _fires++;
}
- (void)stop {
    [self tick:nil];
    [_timer invalidate];
    _timer = nil;
}
@end

namespace {

MCTerminalPanel *Panel(MCPtyState *s) { return [s.c valueForKey:@"terminalPanel"]; }

// A new terminal at its prompt in the project folder.
void OpenTerminal(MCPtyState *s, NSString *what) {
    MCGuiStep([NSString stringWithFormat:@"a new terminal (%@)", what], ^{
        s.c = MCGuiController();
        [s.c newTerminal:nil];
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(s.term != nil);
        s.child = 0;
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return s.term.atPrompt && !s.term.busy; });
}

// Notes the shell, the master and the pty before a close.
void Remember(MCPtyState *s) {
    s.pid = s.term.shellPid;
    s.hostTerm = s.term.onSessionHost ? s.term.sessionTerminal : 0;
    GCHECK(s.term.onSessionHost == MCSessionHostEnabled());
    s.fd = s.hostTerm ? MCSessionInspectTerminal(s.hostTerm).fd : [[s.term valueForKey:@"master"] intValue];
    s.fdWas = IdentityOf(s.fd);
    s.pty = s.hostTerm ? nil : [s.term valueForKey:@"pty"];
    GCHECK(s.pid > 0 && s.fd >= 0 && (s.hostTerm || s.pty != nil));
    if (s.hostTerm) GCHECK(MCSessionInspectTerminal(s.hostTerm).pid == s.pid);
}

// Closes it through the panel, answering Close if it asks (a command runs).
void Close(MCPtyState *s, BOOL asks) {
    NSUInteger before = MCGuiAlertsShown().count;
    if (asks) MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
    s.closedAt = MCGuiNowMs();
    GCHECK([Panel(s) closeTerminal:s.term]);
    GCHECK(MCGuiAlertsShown().count == before + (asks ? 1 : 0));
    s.term = nil;
}

// Everything the pty had is gone: the shell (and its program) reaped, the
// master closed, both sources cancelled and finished, the exited handler
// run. Waited for, then checked one by one so a failure names the part.
void WaitTornDown(MCPtyState *s, NSTimeInterval timeout) {
    MCGuiWait(@"the shell reaped and the master closed", timeout, ^BOOL {
        if (s.hostTerm) {
            const MCSessionTerminalState h = MCSessionInspectTerminal(s.hostTerm);
            if (h.fd >= 0 || h.dying) return NO;
        } else if ([s.pty valueForKey:@"exited"] != nil) {
            return NO;
        }
        return PidGone(s.pid) && (s.child <= 0 || PidGone(s.child)) && FdClosed(s.fd, s.fdWas);
    });
    MCGuiStep(@"each part of the teardown", ^{
        GCHECK(PidGone(s.pid));
        if (!PidGone(s.pid)) MCGuiNote(@"shell %d %s", (int)s.pid, Zombie(s.pid) ? "a zombie" : "alive");
        if (s.child > 0) {
            GCHECK(PidGone(s.child));
            if (!PidGone(s.child))
                MCGuiNote(@"child %d %s", (int)s.child, Zombie(s.child) ? "a zombie" : "alive");
        }
        GCHECK(FdClosed(s.fd, s.fdWas));
        if (s.hostTerm) {
            // The host forgot the terminal and reaped its shell.
            const MCSessionTerminalState h = MCSessionInspectTerminal(s.hostTerm);
            GCHECK(h.fd == -1 && h.pid == 0);
            GCHECK(h.dying == 0);
            s.hostTerm = 0;
            return;
        }
        GCHECK([[s.pty valueForKey:@"sources"] intValue] == 0);
        dispatch_source_t rd = [s.pty valueForKey:@"readSource"];
        dispatch_source_t wr = [s.pty valueForKey:@"writeSource"];
        GCHECK(rd && dispatch_source_testcancel(rd) != 0);
        GCHECK(wr && dispatch_source_testcancel(wr) != 0);
        GCHECK([s.pty valueForKey:@"released"] == nil);   // the main thread let go of the fd
        GCHECK([s.pty valueForKey:@"exited"] == nil);     // and heard of the reap
        s.pty = nil;
    });
}

}  // namespace

MC_GUI_SUITE(terminal_pty) {
    MCPtyState *s = [MCPtyState new];
    MCGuiStep(@"settings", ^{ MCGuiSetSettings(@"lsp.enabled = false\n"); });

    // ---- an idle shell
    OpenTerminal(s, @"idle");
    MCGuiStep(@"close it at its prompt", ^{
        Remember(s);
        Close(s, NO);
    });
    WaitTornDown(s, 10);

    // ---- a shell that ignores the hang-up
    OpenTerminal(s, @"ignoring SIGHUP");
    MCGuiStep(@"trap '' HUP, then exec sleep (which keeps ignoring it)", ^{
        [s.term runCommand:@"trap '' HUP; exec sleep 600"];
    });
    MCGuiWait(@"the shell is sleep", 10, ^BOOL { return ShellNameIs(s.term, "sleep"); });
    MCGuiStep(@"close it", ^{
        Remember(s);
        Close(s, YES);
    });
    MCGuiWait(@"killed 2 s after the hang-up, within a margin", 8, ^BOOL { return PidGone(s.pid); });
    MCGuiStep(@"not before the kill: it did ignore SIGHUP", ^{
        const double took = MCGuiNowMs() - s.closedAt;
        MCGuiNote(@"a shell ignoring SIGHUP was gone %.0f ms after the close", took);
        // A lower bound on wall time, which load only lengthens: under it,
        // the hang-up ended the shell and the kill was never needed.
        GCHECK(took > 1500);
    });
    WaitTornDown(s, 5);

    // ---- 1 MB into a program that does not read
    __block MCPtyStallMeter *meter = [MCPtyStallMeter new];
    OpenTerminal(s, @"a program that does not read");
    MCGuiStep(@"sleep 600", ^{ [s.term runCommand:@"sleep 600"]; });
    MCGuiWait(@"sleep holds the terminal", 10, ^BOOL {
        if (!s.term.busy) return NO;
        const pid_t fg = s.term.foregroundJob;   // tcgetpgrp, or the host's look at it
        char name[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (fg <= 0 || proc_name(fg, name, sizeof name) <= 0 || strcmp(name, "sleep") != 0) return NO;
        s.child = fg;
        return YES;
    });
    MCGuiStep(@"1 MB of lines sent: queued, not written on the main thread", ^{
        Remember(s);
        std::string line(63, 'y');
        line += '\r';   // Return, as typed: whole lines, which the pty queues
        std::string mb;
        for (int i = 0; i < (1 << 20) / 64; i++) mb += line;
        NSData *bytes = [NSData dataWithBytes:mb.data() length:mb.size()];
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        const double c0 = MCGuiThreadCpuMs();
        grid.onInput(bytes);   // what typing and pasting hand the pty
        // MUTIBLE_HOST=loopback (CI's pass with the host) encodes every
        // message and decodes it again on the sending thread: base64 and
        // JSON both ways, 25 to 30 ms for this megabyte on an M3.
        GCHECK_TIME(MCGuiThreadCpuMs() - c0, s.hostTerm && MCSessionLoopback() ? 120 : 50,
                    @"handing 1 MB to the pty's queue");
        [meter start];
    });
    MCGuiWait(@"the pty is full and the rest waits on its queue", 10, ^BOOL {
        return WriteArmed(s.pty, s.hostTerm);
    });
    // Time is the condition: the main thread's CPU between timer fires over
    // a second with 1 MB waiting.
    MCGuiPause(1.0);
    MCGuiStep(@"the main thread stayed responsive", ^{
        [meter stop];
        MCGuiNote(@"1 MB waiting: at most %.2f ms of main-thread CPU between fires, %lu fires",
                  meter.busiest, (unsigned long)meter.fires);
        GCHECK(meter.fires > 50);
        GCHECK_TIME(meter.busiest, 25, @"the main thread's busiest gap with 1 MB waiting");
        GCHECK(WriteArmed(s.pty, s.hostTerm));   // still waiting: sleep reads nothing
        Close(s, YES);
    });
    WaitTornDown(s, 10);

    // ---- closing while output floods in
    OpenTerminal(s, @"yes");
    MCGuiStep(@"yes", ^{ [s.term runCommand:@"yes"]; });
    MCGuiWait(@"yes floods the terminal", 10, ^BOOL {
        if (!s.term.busy || ![[s.term recentText:4] containsString:@"y\ny\ny"]) return NO;
        const pid_t fg = s.term.foregroundJob;
        char name[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (fg <= 0 || proc_name(fg, name, sizeof name) <= 0 || strcmp(name, "yes") != 0) return NO;
        s.child = fg;
        return YES;
    });
    MCGuiStep(@"close it mid-flood", ^{
        Remember(s);
        Close(s, YES);
    });
    WaitTornDown(s, 10);
    MCGuiStep(@"no zombie left", ^{
        GCHECK(!Zombie(s.pid));
        GCHECK(!Zombie(s.child));
    });

    // ---- `exit` closes the terminal, and the last one the dock
    OpenTerminal(s, @"exit");
    MCGuiStep(@"it is the only terminal", ^{
        // Other suites' terminals go (this one's are closed already), so
        // closing this one is closing the last.
        for (TerminalView *t in Panel(s).terminals) {
            if (t == s.term) continue;
            const BOOL asks = t.runningCommands.count > 0;
            if (asks) MCGuiAnswerAlerts(@[@(NSAlertFirstButtonReturn)]);
            [Panel(s) closeTerminal:t];
        }
        GCHECK(Panel(s).terminals.count == 1 && Panel(s).terminals.firstObject == s.term);
    });
    MCGuiStep(@"exit, typed", ^{
        Remember(s);
        s.child = 0;
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        grid.onInput([@"exit\r" dataUsingEncoding:NSUTF8StringEncoding]);
    });
    MCGuiWait(@"the terminal closes, and the dock with it", 10, ^BOOL {
        return Panel(s) == nil && ![[s.c valueForKey:@"terminalVisible"] boolValue];
    });
    MCGuiStep(@"nothing was asked, and it is closed for good", ^{
        GCHECK([[s.term valueForKey:@"closed"] boolValue]);
        GCHECK(s.term.superview == nil);
        s.term = nil;
    });
    WaitTornDown(s, 10);

    // ---- Ctrl+D at an empty prompt
    OpenTerminal(s, @"Ctrl+D, one of two");
    MCGuiStep(@"a second terminal beside it", ^{
        s.pid = s.term.shellPid;
        [s.c newTerminal:nil];
    });
    MCGuiWait(@"its prompt", 20, ^BOOL {
        TerminalView *t = [s.c valueForKey:@"terminal"];
        return t != s.term && t.atPrompt && !t.busy;
    });
    MCGuiStep(@"Ctrl+D in the second", ^{
        s.term = [s.c valueForKey:@"terminal"];
        GCHECK(Panel(s).terminals.count == 2);
        Remember(s);
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        grid.onInput([NSData dataWithBytes:"\x04" length:1]);
    });
    MCGuiWait(@"it closes; the first stays", 10, ^BOOL {
        return Panel(s) && ![Panel(s).terminals containsObject:s.term] && Panel(s).terminals.count == 1;
    });
    MCGuiStep(@"the first is the one left", ^{
        s.term = Panel(s).terminals.firstObject;
        GCHECK(s.term.shellPid > 0 && s.term.atPrompt);
    });
    WaitTornDown(s, 10);
    MCGuiStep(@"close the first", ^{
        Remember(s);
        Close(s, NO);
    });
    WaitTornDown(s, 10);

    // ---- terminal.close-on-exit = off: "Process exited", Return for a new shell
    MCGuiStep(@"terminal.close-on-exit = off", ^{
        MCGuiSetSettings(@"lsp.enabled = false\nterminal.close-on-exit = off\n");
        GCHECK(![AppSettings shared].settings.terminalCloseOnExit());
    });
    OpenTerminal(s, @"close-on-exit off");
    MCGuiStep(@"exit 3", ^{
        Remember(s);
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        grid.onInput([@"exit 3\r" dataUsingEncoding:NSUTF8StringEncoding]);
    });
    MCGuiWait(@"it says so, and stays", 10, ^BOOL {
        return s.term.shellPid == 0 &&
               [[s.term recentText:6] containsString:@"Process exited (code 3). Press Return to start a new shell."];
    });
    MCGuiStep(@"still in the dock", ^{ GCHECK([Panel(s).terminals containsObject:s.term]); });
    {
        // WaitTornDown forgets the terminal's pty; the terminal stays.
        WaitTornDown(s, 10);
    }
    MCGuiStep(@"Return", ^{
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        grid.onInput([NSData dataWithBytes:"\r" length:1]);
    });
    MCGuiWait(@"a new shell's prompt", 20, ^BOOL {
        return s.term.shellPid > 0 && s.term.shellPid != s.pid && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"close it; the setting back on", ^{
        Remember(s);
        Close(s, NO);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        GCHECK([AppSettings shared].settings.terminalCloseOnExit());
    });
    WaitTornDown(s, 10);

    // ---- a shell killed: the crash is said, never a closed tab
    OpenTerminal(s, @"SIGKILL");
    MCGuiStep(@"SIGKILL to the shell (this suite's)", ^{
        Remember(s);
        GCHECK(kill(s.pid, SIGKILL) == 0);
    });
    MCGuiWait(@"the crash line, the terminal kept", 10, ^BOOL {
        return s.term.shellPid == 0 &&
               [[s.term recentText:6] containsString:@"The shell ended (signal 9). Press Return to start a new one."];
    });
    MCGuiStep(@"still in the dock", ^{ GCHECK([Panel(s).terminals containsObject:s.term]); });
    WaitTornDown(s, 10);
    MCGuiStep(@"Return", ^{
        TerminalGridView *grid = [s.term valueForKey:@"grid"];
        grid.onInput([NSData dataWithBytes:"\r" length:1]);
    });
    MCGuiWait(@"a new shell's prompt", 20, ^BOOL {
        return s.term.shellPid > 0 && s.term.shellPid != s.pid && s.term.atPrompt && !s.term.busy;
    });
    MCGuiStep(@"close it", ^{
        Remember(s);
        Close(s, NO);
    });
    WaitTornDown(s, 10);
}
