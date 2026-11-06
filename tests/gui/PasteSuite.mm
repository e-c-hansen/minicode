// PasteSuite.mm — `make guitest SUITES=paste`: a paste far bigger than
// the pty's input queue into a program that is not reading. write() on the
// pty used to block the main thread until the program read it all, and for
// good if that program was itself blocked writing output the app had
// stopped reading. Now the master is non-blocking and the rest waits in the
// terminal's own queue, in order: the paste returns at once, the app keeps
// running, and the program gets every byte once it reads.
#import "Harness.h"
#import "EditorController.h"
#import "Terminal.h"
#import "TerminalGridView.h"

namespace {
TerminalGridView *Grid(TerminalView *t) { return [t valueForKey:@"grid"]; }
double Now() { return NSProcessInfo.processInfo.systemUptime; }
}  // namespace

MC_GUI_SUITE(paste) {
    __block EditorController *c = nil;
    __block TerminalView *term = nil;
    __block double pastedAt = 0, longestGap = 0, lastTick = 0;
    __block NSTimer *ticker = nil;
    static const int kLines = 4000;   // 4000 lines of 61 bytes: 244 KB
    MCGuiStep(@"a terminal", ^{
        c = MCGuiController();
        [c focusTerminal:nil];
        term = [c valueForKey:@"terminal"];
        GCHECK(term != nil);
    });
    MCGuiWait(@"its prompt", 20, ^BOOL { return term.atPrompt && !term.busy; });
    // A program that reads nothing for two seconds, then counts lines.
    MCGuiStep(@"sleep 2, then wc -l", ^{ [term runCommand:@"sleep 2; wc -l"]; });
    MCGuiWait(@"it runs", 10, ^BOOL { return term.busy; });
    MCGuiStep(@"paste 244 KB into it, then Ctrl+D", ^{
        NSMutableString *text = [NSMutableString string];
        for (int i = 0; i < kLines; i++)
            [text appendFormat:@"%05d abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz.\n", i];
        GCHECK(text.length == (NSUInteger)kLines * 61);
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:text forType:NSPasteboardTypeString];
        // A timer on the main thread: the longest gap between its ticks is
        // how long the main thread was held up.
        lastTick = Now();
        ticker = [NSTimer scheduledTimerWithTimeInterval:0.01 repeats:YES block:^(NSTimer *) {
            const double t = Now();
            longestGap = MAX(longestGap, t - lastTick);
            lastTick = t;
        }];
        [[NSRunLoop currentRunLoop] addTimer:ticker forMode:NSRunLoopCommonModes];
        const double t0 = Now();
        [NSApp sendAction:@selector(paste:) to:Grid(term) from:nil];
        pastedAt = Now();
        const double took = pastedAt - t0;
        MCGuiNote(@"the paste returned in %.1f ms", took * 1000);
        MCGuiCheck(took < 0.25, [NSString stringWithFormat:
                       @"a 244 KB paste into a program not reading returns at once (%.3f s)", took],
                   __FILE__, __LINE__);
        const char eot = 0x04;   // after the paste, in order
        Grid(term).onInput([NSData dataWithBytes:&eot length:1]);
    });
    MCGuiStep(@"the app is not held up while the program sleeps", ^{
        GCHECK(Now() - pastedAt < 1.5);   // this step ran before the sleep ended
        GCHECK(term.busy);
    });
    MCGuiWait(@"wc counts every pasted line", 60, ^BOOL {
        return term.atPrompt && !term.busy &&
               [[term recentText:4] rangeOfString:[NSString stringWithFormat:@"%d", kLines]].location !=
                   NSNotFound;
    });
    MCGuiStep(@"the main thread was never held up long", ^{
        [ticker invalidate];
        MCGuiNote(@"longest main-thread gap while the paste drained: %.0f ms", longestGap * 1000);
        MCGuiCheck(longestGap < 0.5, [NSString stringWithFormat:
                       @"no main-thread gap of half a second (%.3f s)", longestGap], __FILE__, __LINE__);
        NSString *tail = [term recentText:3];
        NSString *count = [NSString stringWithFormat:@"%d", kLines];
        GCHECK([tail containsString:count]);
        if (![tail containsString:count]) MCGuiNote(@"%@", tail);
    });
}
