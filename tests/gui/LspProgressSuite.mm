// LspProgressSuite.mm — `make guitest SUITES=lsp_progress`: a server's
// output is framed and parsed off the main thread, and $/progress shows in
// the status bar and the Language Servers popover, against the scripted
// server (build/fakelsp).
//
// The server makes a progress token once initialized and begins work when
// the file opens ("Indexing: 2/5 files 40%"); the status item says so in
// place of "no problems", and the popover's card too. Then, while a 1 ms
// timer on the main thread records its longest gap and the most CPU time
// the main thread spent within one gap, the server writes three
// notifications of about 6 MB of JSON each (ones the client ignores, so
// only their arrival costs anything) and a progress report after them:
// framed and parsed on the server's read queue, they leave the main thread
// alone (well under a millisecond of its CPU between two fires; parsed on
// the main thread they took 50 ms of it). The bound is on that CPU time:
// a gap in wall time on a loaded machine is mostly the thread waiting for
// a core (at load 60 to 100 the gaps ran past 15 ms with nothing to do). A completion list of 5,000 items is
// timed the same way, end to end, and noted: its items are parsed and
// first filtered off the main thread too, and the list's windows are
// built ahead, so what is left is the rows on show (8 to 11 ms on a loaded
// Mac, against 25 to 45 with the parse, the panels' building and the
// documentation's layout all in one pause).
// An edit makes the server end its work, and the status item goes back to
// its counts.
#import "Harness.h"
#import "EditorController.h"
#import "Lsp.h"
#import <QuartzCore/QuartzCore.h>

namespace {
LspSession *Session(EditorController *c) { return [c valueForKey:@"lsp"]; }
NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }

MCLspServerInfo *PythonInfo(EditorController *c) {
    for (MCLspServerInfo *i in [Session(c) serverInfos])
        if ([i.key isEqualToString:@"python"]) return i;
    return nil;
}

void Append(EditorController *c, NSString *text) {
    NSTextView *tv = Text(c);
    MCGuiUndoGroup(tv.undoManager, ^{
        [tv insertText:text replacementRange:NSMakeRange(tv.string.length, 0)];
    });
}
}  // namespace

// The longest time between two fires of a 1 ms timer on the main run loop
// (in every mode), from start to stop.
@interface MCStallMeter : NSObject
@property(nonatomic, readonly) double longest;   // seconds
@property(nonatomic, readonly) double busiest;   // ms of main-thread CPU between two fires
@property(nonatomic, readonly) NSUInteger fires;
- (void)start;
- (void)stop;
@end

@implementation MCStallMeter {
    NSTimer *_timer;
    CFTimeInterval _last;
    double _lastCpu;
}
- (void)start {
    _longest = 0;
    _busiest = 0;
    _fires = 0;
    _last = CACurrentMediaTime();
    _lastCpu = MCGuiThreadCpuMs();
    _timer = [NSTimer timerWithTimeInterval:0.001 target:self selector:@selector(tick:)
                                   userInfo:nil repeats:YES];
    _timer.tolerance = 0;
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}
- (void)tick:(NSTimer *)t {
    (void)t;
    CFTimeInterval now = CACurrentMediaTime();
    const double cpu = MCGuiThreadCpuMs();
    if (now - _last > _longest) _longest = now - _last;
    if (cpu - _lastCpu > _busiest) _busiest = cpu - _lastCpu;
    _last = now;
    _lastCpu = cpu;
    _fires++;
}
- (void)stop {
    [self tick:nil];
    [_timer invalidate];
    _timer = nil;
}
@end

MC_GUI_SUITE(lsp_progress) {
    __block EditorController *c = nil;
    __block MCStallMeter *meter = [MCStallMeter new];
    __block double idle = 0, idleCpu = 0;
    NSString *file = MCGuiProjectFile(@"progress/app.py");

    // 5,000 items, as a big library's completions would come.
    NSMutableArray *items = [NSMutableArray array];
    for (int k = 0; k < 5000; k++)
        [items addObject:@{@"label": [NSString stringWithFormat:@"name_%04d_of_a_module", k],
                           @"kind": @6,
                           @"detail": [NSString stringWithFormat:@"module.sub.name_%04d: int", k],
                           @"sortText": [NSString stringWithFormat:@"%05d", k],
                           @"documentation": @{@"kind": @"markdown", @"value":
                               [NSString stringWithFormat:@"The %d-th name, documented at "
                                   @"some length so the list is the size a real one is.", k]},
                           @"data": @{@"id": @(k), @"module": @"module.sub"}}];
    // About 6 MB of JSON (a large semantic-tokens or symbols answer), sent
    // three times over.
    NSMutableArray *bulk = [NSMutableArray array];
    for (int k = 0; k < 40000; k++)
        [bulk addObject:@{@"name": [NSString stringWithFormat:@"event_%d", k], @"n": @(k),
                          @"tags": @[@"alpha", @"beta", @"gamma"],
                          @"text": @"a measurement the client has no use for, padded out a little"}];
    NSDictionary *telemetry = @{@"method": @"telemetry/event", @"params": @{@"events": bulk}};
    NSDictionary *create = @{@"method": @"window/workDoneProgress/create",
                             @"params": @{@"token": @"index"}};
    NSDictionary *begin = @{@"method": @"$/progress", @"params": @{@"token": @"index",
        @"value": @{@"kind": @"begin", @"title": @"Indexing", @"message": @"2/5 files",
                    @"percentage": @40}}};
    NSDictionary *report = @{@"method": @"$/progress", @"params": @{@"token": @"index",
        @"value": @{@"kind": @"report", @"message": @"5/5 files", @"percentage": @100}}};
    NSDictionary *end = @{@"method": @"$/progress", @"params": @{@"token": @"index",
        @"value": @{@"kind": @"end", @"message": @"done"}}};

    MCGuiStep(@"a server that indexes once the file opens", ^{
        c = MCGuiController();
        MCGuiWriteFile(file, @"import os\n\nx = na\n");
        NSString *cmd = MCGuiFakeLsp(@"progress", @{@"name": @"fakelsp", @"rules": @[
            @{@"method": @"initialized", @"request": @[create]},
            @{@"method": @"textDocument/didOpen", @"notify": @[begin]},
            @{@"method": @"textDocument/completion", @"result":
                @{@"isIncomplete": @NO, @"items": items}},
            @{@"method": @"textDocument/didChange", @"when": @{@"textContains": @"# done"},
              @"notify": @[end]},
            @{@"method": @"textDocument/didChange", @"when": @{@"textContains": @"# flood"},
              @"times": @1,
              @"notify": @[telemetry, telemetry, telemetry, report]}]},
                                     nil, nil);
        MCGuiSetSettings([NSString stringWithFormat:
            @"lsp.enabled = true\nlsp.python = %@\nlsp.python.also = off\n", cmd]);
        GCHECK([c showFile:file]);
    });
    MCGuiWait(@"the status item says what the server is doing", 20, ^BOOL {
        return [Session(c).statusText containsString:@"Indexing"];
    });
    MCGuiStep(@"in place of the counts, and on the popover's card", ^{
        GCHECK_EQ_STR(Session(c).statusText, @"fakelsp: Indexing: 2/5 files 40%");
        GCHECK_EQ_STR(PythonInfo(c).progress, @"Indexing: 2/5 files 40%");
        // The token was made with a request, which was answered null.
        BOOL answered = NO;
        for (NSDictionary *m in MCGuiFakeLspReceived(@"progress", nil))
            if (!m[@"method"] && [m[@"id"] isEqual:@1000] && m[@"result"] == [NSNull null] &&
                !m[@"error"])
                answered = YES;
        GCHECK(answered);
        // And the client said it takes progress.
        NSDictionary *init = MCGuiFakeLspReceived(@"progress", @"initialize").firstObject;
        GCHECK([init[@"params"][@"capabilities"][@"window"][@"workDoneProgress"] isEqual:@YES]);
        [meter start];
    });
    // The idle baseline: a few hundred fires, however long a loaded
    // machine takes to give them.
    MCGuiWait(@"the meter's idle baseline", 20, ^BOOL { return meter.fires >= 300; });
    MCGuiStep(@"the meter, idle; then the server writes 18 MB", ^{
        [meter stop];
        idle = meter.longest;
        idleCpu = meter.busiest;
        MCGuiNote(@"idle: longest main-thread gap %.1f ms (at most %.2f ms of its CPU) over %lu fires",
                  idle * 1000, idleCpu, (unsigned long)meter.fires);
        [c.window makeFirstResponder:Text(c)];
        Append(c, @"# flood\n");   // sent 0.3 s later, with the timer running
        [meter start];
    });
    MCGuiWait(@"the report after them arrives", 20, ^BOOL {
        return [Session(c).statusText containsString:@"5/5 files"];
    });
    MCGuiStep(@"the main thread never stalled for long while they arrived", ^{
        [meter stop];
        MCGuiNote(@"three 6 MB notifications: longest main-thread gap %.1f ms (at most %.2f ms "
                  @"of its CPU) over %lu fires", meter.longest * 1000, meter.busiest,
                  (unsigned long)meter.fires);
        // Under 10 ms of the main thread's CPU between two fires, the idle
        // baseline's on top: a parse there took 50 ms of it.
        GCHECK_TIME(meter.busiest, 10 + idleCpu, @"the main thread's busiest gap while they arrive");
        GCHECK_EQ_STR(Session(c).statusText, @"fakelsp: Indexing: 5/5 files 100%");
        NSTextView *tv = Text(c);
        tv.selectedRange = NSMakeRange(NSMaxRange([tv.string rangeOfString:@"x = na"]), 0);
        [meter start];
        [Session(c) triggerCompletion];
    });
    MCGuiWait(@"5,000 completions arrive and show", 20, ^BOOL {
        return Session(c).completionVisible && Session(c).completionLabels.count > 0;
    });
    MCGuiStep(@"the list, end to end", ^{
        [meter stop];
        MCGuiNote(@"5,000 completions shown: longest main-thread gap %.1f ms (%.2f ms of its CPU) "
                  @"over %lu fires", meter.longest * 1000, meter.busiest, (unsigned long)meter.fires);
        GCHECK([Session(c).completionLabels containsObject:@"name_0042_of_a_module"]);
        // Showing the rows is what is left on the main thread. Printed, not
        // bounded: CI's runner took 70 ms for the rows and the first
        // showing before they were moved and built ahead, and has not been
        // measured since; the parse bound above is the gate.
        [Session(c) handleCommand:@selector(cancelOperation:)];
    });
    MCGuiWait(@"the list closes", 5, ^BOOL { return !Session(c).completionVisible; });
    MCGuiStep(@"an edit, after which the server ends its work", ^{ Append(c, @"# done\n"); });
    MCGuiWait(@"the status item goes back to the counts", 10, ^BOOL {
        return ![Session(c).statusText containsString:@"Indexing"] &&
               [Session(c).statusText hasPrefix:@"fakelsp: "];
    });
    MCGuiStep(@"and the popover's card says nothing of it", ^{
        GCHECK_EQ_STR(Session(c).statusText, @"fakelsp: no problems");
        GCHECK(PythonInfo(c).progress == nil);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [c closeAllTabs:nil];
    });
    MCGuiWait(@"the server is gone", 10, ^BOOL { return !Session(c).hasServer; });
}
