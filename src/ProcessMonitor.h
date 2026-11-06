// ProcessMonitor.h — Objective-C++. The Mac side of processes and ports
// per folder (ProcessTable.h has the rules): one look at the process table
// for the whole app, off the main thread, every 10 seconds, and every 2
// while a Processes panel shows; libproc and sysctl supply the table, the
// working folders, command lines, memory, CPU time and listening ports.
//
// It starts when the app finishes launching (no hook needed elsewhere) and
// asks every editor window for its folder and terminals at each look, on the
// main thread (a few microseconds). Stopping is only ever asked for: by the
// panel's Stop, after its question. `minicode procs` is registered here too.
#import <Cocoa/Cocoa.h>
#include "ControlProtocol.h"
#include "ProcessTable.h"
#include <functional>
#include <vector>

@protocol MCControlAppHost;

// Posted on the main thread after each look.
extern NSNotificationName const MCProcessMonitorDidSampleNotification;

@interface MCProcessMonitor : NSObject
+ (instancetype)shared;
// A panel came on show or went: the looks are 2 s apart while any shows.
- (void)panelAppeared;
- (void)panelDisappeared;
@property(nonatomic, readonly) NSTimeInterval interval;
// A look now (as well as the timer's), then `done` on the main thread.
- (void)sampleNow:(void (^)(void))done;
// What one window's panel shows: its terminals (tab order), its orphans,
// and what else runs in `folder` (resolved with realpath). Window 0: the
// folder only.
- (Procs::View)viewForWindow:(NSInteger)window folder:(NSString *)folder;
- (std::vector<Procs::Terminal>)terminals;
// Stop (ProcessTable's planStop): SIGTERM now, SIGKILL 3 s later to what is
// still alive (each checked by its start time first). `done` on the main
// thread with nil, or why nothing was sent.
- (void)stop:(Procs::Key)key done:(void (^)(NSString *refusal))done;

// For the GUI checks: the last look's time off the main thread, the most
// time a look took on the main thread (gathering terminals, taking the
// result; the panels' redraw after it is their own), how many processes the
// machine had, and how many looks so far.
@property(nonatomic, readonly) double lastSampleMilliseconds;
@property(nonatomic, readonly) double lastMainThreadMilliseconds;
@property(nonatomic, readonly) NSUInteger lastListed;
@property(nonatomic, readonly) NSUInteger samples;
@end

// The real path (symlinks and /private resolved, as the kernel reports a
// working folder), or the path as given when it does not exist.
NSString *MCProcessRealPath(NSString *path);

// `minicode procs [--folder D] [--json]` (ControlCommands.mm calls this).
void MCProcessesRegisterCommand(ControlChannel::Dispatcher &dispatcher, id<MCControlAppHost> host);
