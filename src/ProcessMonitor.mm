// ProcessMonitor.mm — see ProcessMonitor.h.
#import "ProcessMonitor.h"
#import "ControlCommands.h"
#import "ControlServer.h"
#import "EditorController.h"
#import "Terminal.h"

#include <arpa/inet.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <memory>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

NSNotificationName const MCProcessMonitorDidSampleNotification = @"MCProcessMonitorDidSample";

NSString *MCProcessRealPath(NSString *path) {
    if (!path.length) return path ?: @"";
    char buf[PATH_MAX];
    if (realpath(path.fileSystemRepresentation, buf)) return @(buf) ?: path;
    return path;
}

// ------------------------------------------------------------ the source
namespace {

// libproc and sysctl. Used on the monitor's queue only.
class MacSource : public Procs::Source {
public:
    MacSource() { mach_timebase_info(&timebase_); }

    std::vector<Procs::Basic> list() override {
        std::vector<Procs::Basic> out;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
        std::vector<struct kinfo_proc> procs;
        for (int attempt = 0; attempt < 4; attempt++) {
            size_t size = 0;
            if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0) return out;
            size += size / 8 + 16 * sizeof(struct kinfo_proc);   // room for a few new ones
            procs.resize(size / sizeof(struct kinfo_proc));
            if (sysctl(mib, 4, procs.data(), &size, nullptr, 0) == 0) {
                procs.resize(size / sizeof(struct kinfo_proc));
                break;
            }
            if (errno != ENOMEM) return out;
            procs.clear();
        }
        out.reserve(procs.size());
        for (const struct kinfo_proc &k : procs) {
            Procs::Basic b;
            b.pid = k.kp_proc.p_pid;
            b.ppid = k.kp_eproc.e_ppid;
            b.pgid = k.kp_eproc.e_pgid;
            b.uid = k.kp_eproc.e_ucred.cr_uid;
            b.start = (int64_t)k.kp_proc.p_starttime.tv_sec * 1000000 + k.kp_proc.p_starttime.tv_usec;
            b.name = std::string(k.kp_proc.p_comm, strnlen(k.kp_proc.p_comm, sizeof k.kp_proc.p_comm));
            out.push_back(std::move(b));
        }
        return out;
    }

    bool cwd(int pid, std::string &out) override {
        struct proc_vnodepathinfo vpi;
        if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) != (int)sizeof vpi)
            return false;
        out = vpi.pvi_cdir.vip_path;
        return !out.empty();
    }

    bool commandLine(int pid, std::string &out) override {
        int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
        size_t size = 0;
        if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int)) return false;
        std::vector<char> buf(size);
        if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return false;
        int argc = 0;
        memcpy(&argc, buf.data(), sizeof argc);
        size_t i = sizeof argc;
        while (i < size && buf[i]) i++;    // the executable's path
        while (i < size && !buf[i]) i++;   // padding
        out.clear();
        for (int a = 0; a < argc && i < size && out.size() < 4096; a++) {
            size_t start = i;
            while (i < size && buf[i]) i++;
            if (!out.empty()) out += ' ';
            out.append(buf.data() + start, i - start);
            i++;
        }
        // Control characters would upset a row and the command line's table.
        for (char &c : out)
            if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
        return !out.empty();
    }

    bool detail(int pid, Procs::Detail &out) override {
        struct rusage_info_v2 ri;
        if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) != 0) return false;
        out.memory = ri.ri_phys_footprint;
        // Mach time units (not nanoseconds on Apple silicon).
        const uint64_t ticks = ri.ri_user_time + ri.ri_system_time;
        out.cpuNanos = timebase_.denom ? ticks * timebase_.numer / timebase_.denom : ticks;
        out.ports = listening(pid);
        return true;
    }

    // TCP sockets in LISTEN, as `lsof -iTCP -sTCP:LISTEN` finds them.
    static std::vector<Procs::Listen> listening(int pid) {
        std::vector<Procs::Listen> out;
        int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (bytes <= 0) return out;
        std::vector<struct proc_fdinfo> fds((size_t)bytes / sizeof(struct proc_fdinfo) + 16);
        bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(),
                             (int)(fds.size() * sizeof(struct proc_fdinfo)));
        if (bytes <= 0) return out;
        const size_t n = (size_t)bytes / sizeof(struct proc_fdinfo);
        for (size_t i = 0; i < n; i++) {
            if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            struct socket_fdinfo si;
            if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) !=
                (int)sizeof si)
                continue;
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            const struct tcp_sockinfo &tcp = si.psi.soi_proto.pri_tcp;
            if (tcp.tcpsi_state != TSI_S_LISTEN) continue;
            const struct in_sockinfo &in = tcp.tcpsi_ini;
            Procs::Listen l;
            l.port = ntohs((uint16_t)in.insi_lport);
            char addr[INET6_ADDRSTRLEN] = "";
            if (in.insi_vflag & INI_IPV4) {
                struct in_addr a = in.insi_laddr.ina_46.i46a_addr4;
                if (a.s_addr == INADDR_ANY) l.address = "*";
                else if (inet_ntop(AF_INET, &a, addr, sizeof addr)) l.address = addr;
            } else if (in.insi_vflag & INI_IPV6) {
                struct in6_addr a = in.insi_laddr.ina_6;
                if (IN6_IS_ADDR_UNSPECIFIED(&a)) l.address = "*";
                else if (inet_ntop(AF_INET6, &a, addr, sizeof addr)) l.address = addr;
            }
            if (l.port > 0) out.push_back(l);
        }
        return out;
    }

private:
    mach_timebase_info_data_t timebase_{};
};

// A process's start time now, or 0 when it is gone: the check before a
// signal, so a pid used again is never signalled.
int64_t StartTimeOf(int pid) {
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi) return 0;
    return (int64_t)bi.pbi_start_tvsec * 1000000 + (int64_t)bi.pbi_start_tvusec;
}

double Millis(uint64_t from, uint64_t to) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)(to - from) * tb.numer / tb.denom / 1e6;
}

// The calling thread's CPU time, in ms.
double ThreadCpuMillis() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

}  // namespace

// ------------------------------------------------------------ the monitor
@implementation MCProcessMonitor {
    dispatch_queue_t _queue;
    // On _queue only.
    Procs::Tracker _tracker;
    std::unique_ptr<MacSource> _source;
    // On the main thread.
    std::vector<Procs::Proc> _procs;
    std::vector<Procs::Terminal> _terms;
    NSTimer *_timer;
    NSInteger _visible;
    BOOL _inFlight;
}

+ (void)load {
    // Track from launch, so a job that later loses its terminal was seen
    // while it still had one.
    [NSNotificationCenter.defaultCenter
        addObserverForName:NSApplicationDidFinishLaunchingNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) {
                    (void)n;
                    [MCProcessMonitor shared];
                }];
}

+ (instancetype)shared {
    static MCProcessMonitor *monitor;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ monitor = [MCProcessMonitor new]; });
    return monitor;
}

- (instancetype)init {
    if ((self = [super init])) {
        _queue = dispatch_queue_create("minicode.processes", DISPATCH_QUEUE_SERIAL);
        _source = std::make_unique<MacSource>();
        [self reschedule];
        [self tick];
    }
    return self;
}

- (NSTimeInterval)interval { return _visible > 0 ? 2 : 10; }

- (void)reschedule {
    [_timer invalidate];
    __weak MCProcessMonitor *weakSelf = self;
    _timer = [NSTimer timerWithTimeInterval:self.interval repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf tick];
    }];
    _timer.tolerance = self.interval / 10;
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)panelAppeared {
    if (++_visible == 1) {
        [self reschedule];
        [self tick];
    }
}

- (void)panelDisappeared {
    if (_visible > 0 && --_visible == 0) [self reschedule];
}

- (void)tick {
    if (_inFlight) return;   // the last look is still going: skip this one
    [self sampleNow:nil];
}

// Every editor window's terminals and folder, on the main thread.
- (void)gatherTerminals:(std::vector<Procs::Terminal> &)terms folders:(std::vector<std::string> &)folders {
    id<MCControlAppHost> host = (id<MCControlAppHost>)NSApp.delegate;
    if (![host respondsToSelector:@selector(controlControllers)]) return;
    for (EditorController *c in [host controlControllers]) {
        const int window = (int)MCControlWindowID(c);
        if (c.rootPath.length) folders.push_back(MCProcessRealPath(c.rootPath).UTF8String ?: "");
        for (TerminalView *t in [c controlTerminals]) {
            Procs::Terminal pt;
            pt.id = (int)MCControlTerminalID(t);
            pt.title = t.title.UTF8String ?: "";
            pt.shellPid = t.shellPid;
            pt.window = window;
            terms.push_back(pt);
        }
    }
}

- (void)sampleNow:(void (^)(void))done {
    const uint64_t m0 = mach_absolute_time();
    const double mc0 = ThreadCpuMillis();
    auto terms = std::make_shared<std::vector<Procs::Terminal>>();
    auto folders = std::make_shared<std::vector<std::string>>();
    [self gatherTerminals:*terms folders:*folders];
    const double gatherMs = Millis(m0, mach_absolute_time());
    const double gatherCpu = ThreadCpuMillis() - mc0;
    _inFlight = YES;
    __weak MCProcessMonitor *weakSelf = self;
    dispatch_async(_queue, ^{
        MCProcessMonitor *s = weakSelf;
        if (!s) return;
        const uint64_t t0 = mach_absolute_time();
        const double c0 = ThreadCpuMillis();
        Procs::SampleStats st = s->_tracker.sample(*s->_source, NSProcessInfo.processInfo.systemUptime,
                                                    *terms, *folders, getpid(), getuid());
        auto snap = std::make_shared<std::vector<Procs::Proc>>(s->_tracker.snapshot());
        const double ms = Millis(t0, mach_absolute_time());
        const double cpu = ThreadCpuMillis() - c0;
        dispatch_async(dispatch_get_main_queue(), ^{
            MCProcessMonitor *m = weakSelf;
            if (!m) return;
            const uint64_t a = mach_absolute_time();
            const double ac = ThreadCpuMillis();
            m->_procs = std::move(*snap);
            m->_terms = *terms;
            m->_inFlight = NO;
            m->_lastSampleMilliseconds = ms;
            m->_lastSampleCpuMilliseconds = cpu;
            m->_lastListed = st.listed;
            m->_samples++;
            m->_lastMainThreadMilliseconds = gatherMs + Millis(a, mach_absolute_time());
            m->_lastMainThreadCpuMilliseconds = gatherCpu + ThreadCpuMillis() - ac;
            // Panels on show redraw from this (ProcessesPanel reload).
            [NSNotificationCenter.defaultCenter postNotificationName:MCProcessMonitorDidSampleNotification
                                                              object:m];
            if (done) done();
        });
    });
}

- (std::vector<Procs::Terminal>)terminals { return _terms; }

- (Procs::View)viewForWindow:(NSInteger)window folder:(NSString *)folder {
    return Procs::buildView(_procs, _terms, (int)window, folder.UTF8String ?: "");
}

- (void)stop:(Procs::Key)key done:(void (^)(NSString *))done {
    std::vector<int> shells;
    for (const Procs::Terminal &t : _terms)
        if (t.shellPid > 0) shells.push_back(t.shellPid);
    // Shells of terminals opened since the last look, too.
    std::vector<Procs::Terminal> now;
    std::vector<std::string> folders;
    [self gatherTerminals:now folders:folders];
    for (const Procs::Terminal &t : now)
        if (t.shellPid > 0) shells.push_back(t.shellPid);
    __weak MCProcessMonitor *weakSelf = self;
    dispatch_async(_queue, ^{
        MCProcessMonitor *s = weakSelf;
        if (!s) return;
        Procs::StopPlan plan = Procs::planStop(s->_source->list(), key, getpid(), getpgrp(), shells);
        if (plan.ok) {
            // Each target is still the process it was (its start time).
            bool all = true;
            for (const Procs::Key &k : plan.targets)
                if (StartTimeOf(k.pid) != k.start) all = false;
            if (plan.group && all && StartTimeOf(key.pid) == key.start) {
                kill(-plan.pgid, SIGTERM);
            } else {
                for (const Procs::Key &k : plan.targets)
                    if (StartTimeOf(k.pid) == k.start) kill(k.pid, SIGTERM);
            }
            const std::vector<Procs::Key> targets = plan.targets;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), s->_queue, ^{
                for (const Procs::Key &k : targets)
                    if (StartTimeOf(k.pid) == k.start) kill(k.pid, SIGKILL);
                dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf sampleNow:nil]; });
            });
        }
        NSString *refusal = plan.ok ? nil : @(plan.refusal.c_str());
        dispatch_async(dispatch_get_main_queue(), ^{
            if (done) done(refusal);
            [weakSelf sampleNow:nil];
        });
    });
}

@end

// ------------------------------------------------------- minicode procs
void MCProcessesRegisterCommand(ControlChannel::Dispatcher &d, id<MCControlAppHost> hostIn) {
    __weak id<MCControlAppHost> weakHost = hostIn;
    d.on("procs", [weakHost](const ControlChannel::Request &req,
                             ControlChannel::Dispatcher::Respond respond) {
        id<MCControlAppHost> host = weakHost;
        const Json &a = req.args;
        EditorController *c = nil;
        NSString *folder = nil;
        if (a.has("window")) {
            const NSInteger want = (NSInteger)a["window"].asInt();
            for (EditorController *e in [host controlControllers])
                if (MCControlWindowID(e) == want) c = e;
            if (!c) {
                respond(ControlChannel::Result::failure("there is no window " +
                                                        std::to_string(want)));
                return;
            }
        } else if (a.has("folder")) {
            NSString *dir = [NSString stringWithUTF8String:a["folder"].asString().c_str()] ?: @"";
            BOOL isDir = NO;
            if (![NSFileManager.defaultManager fileExistsAtPath:dir isDirectory:&isDir] || !isDir) {
                respond(ControlChannel::Result::failure("not a folder: " + a["folder"].asString()));
                return;
            }
            folder = MCProcessRealPath(dir);
            // The window showing it, when one does; a folder no window shows
            // is answered for anyway (no window is opened for a read).
            for (EditorController *e in [host controlControllers])
                if ([MCProcessRealPath(e.rootPath) isEqualToString:folder]) { c = e; break; }
        } else {
            c = [host controlFrontController];
            if (!c) {
                respond(ControlChannel::Result::failure("no window is open; give --folder"));
                return;
            }
        }
        if (c) folder = MCProcessRealPath(c.rootPath);
        const NSInteger window = c ? MCControlWindowID(c) : 0;
        NSString *f = folder;
        MCProcessMonitor *monitor = [MCProcessMonitor shared];
        [monitor sampleNow:^{
            Procs::View v = [monitor viewForWindow:window folder:f];
            respond(ControlChannel::Result::success(Procs::toJson(v, f.UTF8String ?: "", (int)window)));
        }];
    });
}
