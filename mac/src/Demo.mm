// Demo.mm — scripted demo scenes and the window recorder behind `make demos`.
//
// Inert unless MINICODE_DEMO is set. A scene is a list of steps (click a file,
// type, press a shortcut, wait) played in the live app. Clicks and keys are
// posted as real NSEvents, so they travel the same path a person's do: the
// tree's mouseDown, the text input system, the local key monitor for Ctrl+`,
// and the menu's key equivalents. The pointer and the key captions are drawn
// by an overlay view inside the window, because a window capture shows
// neither the real cursor nor the keyboard.
//
// While a scene plays, a background thread runs `screencapture -l` on the
// window about ten times a second, plus any of the app's windows in front of
// it (a popover, the color panel), and logs each frame's time and offsets to
// manifest.txt. tools/makegif.m assembles the GIF from that.
//
// Adding a scene: write a function that calls the builder methods on MCDemo
// (see the scenes at the bottom) and add it to kScenes.
#import "Demo.h"
#import "AgentWindow.h"
#import "AgentsPanel.h"
#import "EditorController.h"
#import "GitPanel.h"
#import "Latex.h"
#import "MarkdownSidePreview.h"
#import "Terminal.h"
#import "TerminalGridView.h"
#import <Quartz/Quartz.h>
#include <initializer_list>
#include <vector>
#include <spawn.h>
#include <sys/wait.h>
#include <mach/mach_time.h>

extern char **environ;

// ------------------------------------------------------------- the overlay

// Pointer and key captions, drawn over the window's content. Never takes a
// click: hitTest returns nil, so events reach the views underneath.
@interface MCDemoOverlay : NSView
@property(nonatomic, assign) NSPoint pointer;       // window coordinates
@property(nonatomic, assign) BOOL pointerVisible;
@property(nonatomic, assign) CGFloat ripple;        // 0 = none, else 0..1
@property(nonatomic, copy) NSString *caption;
@end

@implementation MCDemoOverlay
- (NSView *)hitTest:(NSPoint)p { (void)p; return nil; }
- (BOOL)isOpaque { return NO; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (self.caption.length) [self drawCaption];
    if (self.pointerVisible) [self drawPointer];
}

- (void)drawCaption {
    NSDictionary *a = @{
        NSFontAttributeName: [NSFont systemFontOfSize:17 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName: [NSColor whiteColor],
    };
    NSSize s = [self.caption sizeWithAttributes:a];
    CGFloat padX = 18, padY = 10;
    NSRect box = NSMakeRect(floor(NSMidX(self.bounds) - s.width / 2 - padX), 44,
                            ceil(s.width + 2 * padX), ceil(s.height + 2 * padY));
    NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:box xRadius:10 yRadius:10];
    [[NSColor colorWithSRGBRed:0.06 green:0.06 blue:0.07 alpha:0.88] setFill];
    [bg fill];
    [[NSColor colorWithWhite:1 alpha:0.18] setStroke];
    bg.lineWidth = 1;
    [bg stroke];
    [self.caption drawAtPoint:NSMakePoint(box.origin.x + padX, box.origin.y + padY)
               withAttributes:a];
}

- (void)drawPointer {
    NSPoint p = [self convertPoint:self.pointer fromView:nil];
    if (self.ripple > 0) {
        CGFloat r = 7 + 16 * self.ripple;
        NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:
            NSMakeRect(p.x - r, p.y - r, 2 * r, 2 * r)];
        ring.lineWidth = 3;
        [[NSColor colorWithSRGBRed:0.31 green:0.63 blue:0.97
                             alpha:0.9 * (1 - self.ripple)] setStroke];
        [ring stroke];
    }
    // The classic arrow, tip at p, scaled up a little so it reads in a GIF.
    static const CGFloat pts[][2] = {
        {0, 0}, {0, 17}, {4, 13}, {7, 20}, {10, 19}, {7, 12}, {12, 12},
    };
    const CGFloat k = 1.15;
    NSBezierPath *arrow = [NSBezierPath bezierPath];
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        NSPoint q = NSMakePoint(p.x + pts[i][0] * k, p.y - pts[i][1] * k);
        if (i == 0) [arrow moveToPoint:q]; else [arrow lineToPoint:q];
    }
    [arrow closePath];
    arrow.lineJoinStyle = NSLineJoinStyleRound;
    arrow.lineWidth = 1.3;
    [[NSColor blackColor] setFill];
    [arrow fill];
    [[NSColor whiteColor] setStroke];
    [arrow stroke];
}
@end

// ------------------------------------------------------------ the recorder

static double NowSeconds(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static pid_t SpawnScreencapture(int windowNumber, NSString *path) {
    char flag[32];
    snprintf(flag, sizeof flag, "-l%d", windowNumber);
    const char *argv[] = {"/usr/sbin/screencapture", "-x", "-o", flag,
                          path.fileSystemRepresentation, NULL};
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0)
        return 0;
    return pid;
}

static int WaitFor(pid_t pid) {
    if (pid <= 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

@interface MCRecorder : NSObject
@property(nonatomic, assign) BOOL paused;
@end

@implementation MCRecorder {
    NSString *_dir;
    NSWindow *_window;
    double _interval;
    double _t0, _pausedTotal, _pauseStart;
    NSFileHandle *_manifest;
    NSArray *_snapshot;          // what to capture next: main window + extras
    NSTimer *_snapTimer;
    dispatch_semaphore_t _finished;
    volatile BOOL _stop;
    int _frame;
    int _failures;
}

- (instancetype)initWithDirectory:(NSString *)dir window:(NSWindow *)w {
    if ((self = [super init])) {
        _dir = dir;
        _window = w;
        _interval = 0.1;
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        NSString *m = [dir stringByAppendingPathComponent:@"manifest.txt"];
        [[NSData data] writeToFile:m atomically:NO];
        _manifest = [NSFileHandle fileHandleForWritingAtPath:m];
        _finished = dispatch_semaphore_create(0);
    }
    return self;
}

// Milliseconds of recorded time: wall time with the paused stretches cut out,
// so a wait that was not recorded leaves no gap in the GIF.
- (long)timelineMs {
    double paused = _pausedTotal;
    if (self.paused) paused += NowSeconds() - _pauseStart;
    return lround((NowSeconds() - _t0 - paused) * 1000);
}

- (void)setPaused:(BOOL)paused {
    @synchronized(self) {
        if (paused == _paused) return;
        if (paused) _pauseStart = NowSeconds();
        else _pausedTotal += NowSeconds() - _pauseStart;
        _paused = paused;
    }
}

- (void)writeLine:(NSString *)line {
    @synchronized(self) {
        [_manifest writeData:[[line stringByAppendingString:@"\n"]
                                 dataUsingEncoding:NSUTF8StringEncoding]];
    }
}

- (void)mark:(NSString *)what {
    [self writeLine:[NSString stringWithFormat:@"%@ %ld", what, [self timelineMs]]];
}

// Runs on the main thread: which windows to capture, and where they sit
// relative to the main window's top-left corner, in points.
- (void)takeSnapshot {
    NSWindow *w = _window;
    NSRect mf = w.frame;
    NSMutableArray *list = [NSMutableArray arrayWithObject:@[@(w.windowNumber), @0, @0,
                                                              @(mf.size.width),
                                                              @(mf.size.height)]];
    // Front to back; only windows in front of ours matter. A child window
    // (a popover) comes along in the main window's own capture, which then
    // grows to take it in; the rest (the color panel) are captured apart.
    NSArray<NSNumber *> *order = [NSWindow windowNumbersWithOptions:0];
    NSMutableArray *extras = [NSMutableArray array];
    for (NSNumber *n in order) {
        if (n.integerValue == w.windowNumber) break;
        NSWindow *x = [NSApp windowWithWindowNumber:n.integerValue];
        if (!x || !x.isVisible || x.alphaValue <= 0 ||
            !NSIntersectsRect(x.frame, mf)) continue;
        BOOL child = NO;
        for (NSWindow *p = x.parentWindow; p; p = p.parentWindow)
            if (p == w) child = YES;
        NSRect f = x.frame;
        [extras insertObject:@[child ? @(-1) : n, @(f.origin.x - mf.origin.x),
                               @(NSMaxY(mf) - NSMaxY(f)),
                               @(f.size.width), @(f.size.height)] atIndex:0];
    }
    [list addObjectsFromArray:extras];   // back to front after the main window
    @synchronized(self) { _snapshot = list; }
}

- (void)start {
    _t0 = NowSeconds();
    [self takeSnapshot];
    _snapTimer = [NSTimer timerWithTimeInterval:0.05 repeats:YES
                                          block:^(NSTimer *t) {
        (void)t;
        [self takeSnapshot];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_snapTimer forMode:NSRunLoopCommonModes];
    [NSThread detachNewThreadWithBlock:^{ [self loop]; }];
}

- (void)loop {
    double next = NowSeconds();
    while (!_stop) {
        if (self.paused) { usleep(10000); next = NowSeconds(); continue; }
        long t = [self timelineMs];
        NSArray *snap;
        @synchronized(self) { snap = _snapshot; }
        NSMutableString *lines = [NSMutableString string];
        NSString *stem = [NSString stringWithFormat:@"f%06d", ++_frame];
        // All windows at once, so a popover costs no frame rate.
        std::vector<pid_t> pids(snap.count, 0);
        NSMutableArray<NSString *> *names = [NSMutableArray array];
        for (NSUInteger i = 0; i < snap.count; i++) {
            NSString *name = i == 0 ? [stem stringByAppendingString:@".png"]
                : [NSString stringWithFormat:@"%@-%lu.png", stem, (unsigned long)i];
            [names addObject:name];
            if ([snap[i][0] intValue] > 0)
                pids[i] = SpawnScreencapture([snap[i][0] intValue],
                                             [_dir stringByAppendingPathComponent:name]);
        }
        BOOL mainOK = YES;
        for (NSUInteger i = 0; i < snap.count; i++) {
            NSArray *e = snap[i];
            if ([e[0] intValue] < 0) {   // a child window: where it is, no image
                [lines appendFormat:@"child %g %g %g %g\n",
                                    [e[1] doubleValue], [e[2] doubleValue],
                                    [e[3] doubleValue], [e[4] doubleValue]];
                continue;
            }
            if (WaitFor(pids[i]) != 0) {
                if (i == 0) mainOK = NO;
                continue;   // a popover can vanish between snapshot and capture
            }
            if (i == 0)
                [lines appendFormat:@"frame %ld %@ %g %g\n", t, names[i],
                                    [e[3] doubleValue], [e[4] doubleValue]];
            else
                [lines appendFormat:@"over %@ %g %g %g %g\n", names[i],
                                    [e[1] doubleValue], [e[2] doubleValue],
                                    [e[3] doubleValue], [e[4] doubleValue]];
        }
        if (!mainOK) [lines setString:@""];
        if (lines.length) {
            _failures = 0;
            @synchronized(self) {
                [_manifest writeData:[lines dataUsingEncoding:NSUTF8StringEncoding]];
            }
        } else if (++_failures >= 5) {
            fprintf(stderr, "minicode demo: screencapture keeps failing (does the "
                            "terminal have Screen Recording permission?)\n");
            exit(4);
        }
        next += _interval;
        double now = NowSeconds();
        if (next > now) usleep((useconds_t)((next - now) * 1e6));
        else next = now;
    }
    dispatch_semaphore_signal(_finished);
}

- (void)stopThen:(dispatch_block_t)done {
    self.paused = NO;
    [self mark:@"end"];
    _stop = YES;
    [_snapTimer invalidate];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        dispatch_semaphore_wait(self->_finished, DISPATCH_TIME_FOREVER);
        [self->_manifest closeFile];
        dispatch_async(dispatch_get_main_queue(), done);
    });
}
@end

// --------------------------------------------------------------- the player

@class MCDemo;
typedef void (^MCDone)(void);
typedef void (^MCStep)(MCDemo *d, MCDone done);
typedef NSPoint (^MCWhere)(void);

@interface MCDemo : NSObject
@property(nonatomic, strong) EditorController *controller;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, copy) NSString *root;
@property(nonatomic, copy) NSString *skipReason;
- (id)ui:(NSString *)key;
@end

@implementation MCDemo {
    NSMutableArray<MCStep> *_steps;
    MCDemoOverlay *_overlay;
    MCRecorder *_recorder;
    NSInteger _eventNumber;
    NSUInteger _captionToken;
    NSMutableSet<NSNumber *> *_ours;   // timestamps (µs) of the events we posted
    long long _lastStamp;
    id _monitor;
}

- (instancetype)initWithController:(EditorController *)c {
    if ((self = [super init])) {
        _controller = c;
        _window = c.window;
        _root = c.rootPath;
        _steps = [NSMutableArray array];
        _ours = [NSMutableSet set];
    }
    return self;
}

// A timestamp for an event we post, remembered so the input filter lets it
// through. Whole microseconds, one apart: AppKit keeps the timestamp only to
// about a nanosecond on its way through the queue.
- (NSTimeInterval)stamp {
    long long us = MAX(_lastStamp + 1,
                       llround(NSProcessInfo.processInfo.systemUptime * 1e6));
    _lastStamp = us;
    [_ours addObject:@(us)];
    ++_eventNumber;
    return us / 1e6;
}

// The window comes to the front while a scene plays, so whatever the person
// at the keyboard types or clicks would land in it and derail the scene (or
// type into a file). Only the scene's own events get through.
- (void)ignoreRealInput {
    NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged |
        NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged |
        NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
        NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp | NSEventMaskScrollWheel;
    NSMutableSet<NSNumber *> *ours = _ours;
    _monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                     handler:^NSEvent *(NSEvent *e) {
        return [ours containsObject:@(llround(e.timestamp * 1e6))] ? e : nil;
    }];
}

// The controller keeps its views in a class extension; KVC reaches them.
- (id)ui:(NSString *)key { return [self.controller valueForKey:key]; }

- (void)add:(MCStep)step { [_steps addObject:step]; }

static void After(double seconds, dispatch_block_t block) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), block);
}

- (void)next {
    if (_steps.count == 0) { [self finish]; return; }
    MCStep step = _steps.firstObject;
    [_steps removeObjectAtIndex:0];
    // A person would bring the window back; so does the demo, or keystrokes
    // meant for it would go to whatever the user clicked meanwhile.
    if (!NSApp.isActive) {
        [NSApp activateIgnoringOtherApps:YES];
        [self.window makeKeyAndOrderFront:nil];
    }
    __weak MCDemo *weakSelf = self;
    step(self, ^{ [weakSelf next]; });
}

- (void)fail:(NSString *)why {
    fprintf(stderr, "minicode demo: %s\n", why.UTF8String);
    exit(2);
}

// ------------------------------------------------------------ builder API

- (void)pause:(double)seconds {
    [self add:^(MCDemo *d, MCDone done) { (void)d; After(seconds, done); }];
}

- (void)run:(void (^)(MCDemo *d))block {
    [self add:^(MCDemo *d, MCDone done) { block(d); done(); }];
}

- (void)poster {
    [self add:^(MCDemo *d, MCDone done) { [d->_recorder mark:@"poster"]; done(); }];
}

- (void)showCaption:(NSString *)text for:(double)seconds {
    NSUInteger token = ++_captionToken;
    _overlay.caption = text;
    _overlay.needsDisplay = YES;
    After(seconds, ^{
        if (token != self->_captionToken) return;
        self->_overlay.caption = nil;
        self->_overlay.needsDisplay = YES;
    });
}

- (void)caption:(NSString *)text {
    [self add:^(MCDemo *d, MCDone done) { [d showCaption:text for:2.2]; done(); }];
}

// Wait until cond holds. With `recorded` NO the wait is cut from the GIF
// (after a short lead-in, so a spinner still shows that something happened).
- (void)waitFor:(BOOL (^)(MCDemo *d))cond timeout:(double)timeout
       recorded:(BOOL)recorded {
    [self add:^(MCDemo *d, MCDone done) {
        [d poll:cond since:NowSeconds() timeout:timeout recorded:recorded done:done];
    }];
}

- (void)poll:(BOOL (^)(MCDemo *d))cond since:(double)start timeout:(double)timeout
    recorded:(BOOL)recorded done:(MCDone)done {
    if (cond(self)) { _recorder.paused = NO; done(); return; }
    double waited = NowSeconds() - start;
    if (waited > timeout) [self fail:@"a scene step timed out"];
    if (!recorded && waited > 0.6) _recorder.paused = YES;
    After(0.05, ^{
        [self poll:cond since:start timeout:timeout recorded:recorded done:done];
    });
}

// ---- pointer

- (void)animatePointerTo:(NSPoint)target then:(MCDone)done {
    NSPoint from = _overlay.pointer;
    BOOL wasVisible = _overlay.pointerVisible;
    _overlay.pointerVisible = YES;
    if (!wasVisible) {
        // Enter from below the target rather than popping up on it.
        from = NSMakePoint(target.x + 60, target.y - 80);
    }
    double dist = hypot(target.x - from.x, target.y - from.y);
    int steps = (int)MIN(MAX(dist / 18, 8), 26);
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / steps;
        double e = t < 0.5 ? 2 * t * t : 1 - pow(-2 * t + 2, 2) / 2;   // ease in-out
        NSPoint p = NSMakePoint(from.x + (target.x - from.x) * e,
                                from.y + (target.y - from.y) * e);
        After(i * 0.018, ^{
            self->_overlay.pointer = p;
            self->_overlay.needsDisplay = YES;
            if (i == steps) After(0.08, done);
        });
    }
}

- (void)moveTo:(MCWhere)where {
    [self add:^(MCDemo *d, MCDone done) { [d animatePointerTo:where() then:done]; }];
}

- (void)hidePointer {
    [self add:^(MCDemo *d, MCDone done) {
        d->_overlay.pointerVisible = NO;
        d->_overlay.needsDisplay = YES;
        done();
    }];
}

- (void)rippleAt:(NSPoint)p {
    for (int i = 1; i <= 10; i++) {
        After(i * 0.03, ^{
            self->_overlay.ripple = i == 10 ? 0 : i / 10.0;
            self->_overlay.needsDisplay = YES;
        });
    }
}

- (void)postMouse:(NSEventType)type at:(NSPoint)p clicks:(NSInteger)clicks {
    [self postMouse:type at:p clicks:clicks flags:0];
}

- (void)postMouse:(NSEventType)type at:(NSPoint)p clicks:(NSInteger)clicks
            flags:(NSEventModifierFlags)flags {
    NSEvent *e = [NSEvent mouseEventWithType:type
                                    location:p
                               modifierFlags:flags
                                   timestamp:[self stamp]
                                windowNumber:self.window.windowNumber
                                     context:nil
                                 eventNumber:_eventNumber
                                  clickCount:clicks
                                    pressure:type == NSEventTypeLeftMouseDown ? 1 : 0];
    [NSApp postEvent:e atStart:NO];
}

- (void)clickAt:(MCWhere)where count:(NSInteger)count {
    [self add:^(MCDemo *d, MCDone done) {
        NSPoint p = where();
        [d animatePointerTo:p then:^{
            [d rippleAt:p];
            for (NSInteger n = 1; n <= count; n++) {
                [d postMouse:NSEventTypeLeftMouseDown at:p clicks:n];
                [d postMouse:NSEventTypeLeftMouseUp at:p clicks:n];
            }
            After(0.25, done);
        }];
    }];
}

- (void)clickAt:(MCWhere)where { [self clickAt:where count:1]; }

// Command+click. The mouse event carries the flag, which is what the
// terminal's mouseDown: checks.
- (void)cmdClickAt:(MCWhere)where {
    [self add:^(MCDemo *d, MCDone done) {
        NSPoint p = where();
        [d animatePointerTo:p then:^{
            [d rippleAt:p];
            [d postMouse:NSEventTypeLeftMouseDown at:p clicks:1 flags:NSEventModifierFlagCommand];
            [d postMouse:NSEventTypeLeftMouseUp at:p clicks:1 flags:NSEventModifierFlagCommand];
            After(0.25, done);
        }];
    }];
}
- (void)doubleClickAt:(MCWhere)where { [self clickAt:where count:2]; }

// ---- the file tree

- (NSInteger)rowForPath:(NSString *)path {
    NSOutlineView *o = [self ui:@"outline"];
    for (NSInteger r = 0; r < o.numberOfRows; r++)
        if ([[[o itemAtRow:r] valueForKey:@"path"] isEqualToString:path]) return r;
    return -1;
}

- (NSPoint)pointForRow:(NSInteger)row {
    NSOutlineView *o = [self ui:@"outline"];
    NSRect cell = [o frameOfCellAtColumn:0 row:row];
    NSPoint p = NSMakePoint(cell.origin.x + 44, NSMidY(cell));
    return [o convertPoint:p toView:nil];
}

// Click through the tree to a file, opening each folder on the way.
- (void)clickFile:(NSString *)relative {
    NSArray<NSString *> *parts = relative.pathComponents;
    NSString *path = self.root;
    for (NSUInteger i = 0; i < parts.count; i++) {
        path = [path stringByAppendingPathComponent:parts[i]];
        NSString *p = path;
        BOOL folder = i + 1 < parts.count;
        [self add:^(MCDemo *d, MCDone done) {
            NSInteger row = [d rowForPath:p];
            if (row < 0) [d fail:[NSString stringWithFormat:@"%@ is not in the tree", p]];
            NSOutlineView *o = [d ui:@"outline"];
            if (folder && [o isItemExpanded:[o itemAtRow:row]]) { done(); return; }
            NSPoint pt = [d pointForRow:row];
            [d animatePointerTo:pt then:^{
                [d rippleAt:pt];
                [d postMouse:NSEventTypeLeftMouseDown at:pt clicks:1];
                [d postMouse:NSEventTypeLeftMouseUp at:pt clicks:1];
                After(folder ? 0.45 : 0.3, done);
            }];
        }];
    }
}

// ---- text

// Window point of the text `needle` in the editor (the first match after
// `anchor`, when given). With atEnd, the point just past its last character.
- (NSPoint)pointForText:(NSString *)needle after:(NSString *)anchor atEnd:(BOOL)atEnd {
    return [self pointForText:needle inView:[self ui:@"textView"] after:anchor atEnd:atEnd];
}

// The same in another text view: the side preview's page, a diff.
- (NSPoint)pointForText:(NSString *)needle inView:(NSTextView *)tv after:(NSString *)anchor
                  atEnd:(BOOL)atEnd {
    if (!tv) [self fail:@"no text view to find text in"];
    NSString *s = tv.string;
    NSRange from = NSMakeRange(0, s.length);
    if (anchor.length) {
        NSRange a = [s rangeOfString:anchor];
        if (a.location == NSNotFound) [self fail:[NSString stringWithFormat:@"no “%@” in the editor", anchor]];
        from = NSMakeRange(a.location, s.length - a.location);
    }
    NSRange r = [s rangeOfString:needle options:0 range:from];
    if (r.location == NSNotFound) [self fail:[NSString stringWithFormat:@"no “%@” in the editor", needle]];
    // Through the text input API rather than tv.layoutManager: touching the
    // layout manager switches the view to TextKit 1 for good, and the editor
    // is meant to run on TextKit 2 (the LSP squiggles are drawn that way).
    NSRect screen = [tv firstRectForCharacterRange:r actualRange:NULL];
    NSRect rect = [tv.window convertRectFromScreen:screen];
    return atEnd ? NSMakePoint(NSMaxX(rect) + 1, NSMidY(rect))
                 : NSMakePoint(NSMidX(rect), NSMidY(rect));
}

// Scroll the editor so the line holding `text` is near the top, smoothly.
- (void)scrollEditorTo:(NSString *)text {
    [self add:^(MCDemo *d, MCDone done) {
        NSTextView *tv = [d ui:@"textView"];
        NSRange r = [tv.string rangeOfString:text];
        if (r.location == NSNotFound) [d fail:@"scroll target missing"];
        NSLayoutManager *lm = tv.layoutManager;
        NSRect rect = [lm boundingRectForGlyphRange:
            [lm glyphRangeForCharacterRange:r actualCharacterRange:NULL]
                                    inTextContainer:tv.textContainer];
        NSScrollView *scroll = [d ui:@"editorScroll"];
        NSClipView *clip = scroll.contentView;
        CGFloat maxY = MAX(0, NSHeight(tv.frame) - NSHeight(clip.bounds));
        CGFloat y0 = clip.bounds.origin.y;
        CGFloat y1 = MIN(maxY, MAX(0, rect.origin.y - 6));
        // The line numbers are an inset of the clip view, so its left edge
        // is not x = 0; keep x where it is.
        const CGFloat x = clip.bounds.origin.x;
        const int steps = 18;
        for (int i = 1; i <= steps; i++) {
            double t = (double)i / steps;
            double e = t < 0.5 ? 2 * t * t : 1 - pow(-2 * t + 2, 2) / 2;
            After(i * 0.025, ^{
                [clip scrollToPoint:NSMakePoint(x, y0 + (y1 - y0) * e)];
                [scroll reflectScrolledClipView:clip];
                if (i == steps) After(0.1, done);
            });
        }
    }];
}

// US keyboard key codes, so the text input system sees what a real key sends.
static BOOL KeyFor(unichar c, unsigned short *code, BOOL *shift) {
    static const char *plain = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.";
    static const char *shifted = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\0LJ\"K:|<?NM>";
    if (c == ' ') { *code = 49; *shift = NO; return YES; }
    if (c == '`') { *code = 50; *shift = NO; return YES; }
    if (c == '~') { *code = 50; *shift = YES; return YES; }
    for (unsigned short i = 0; i < 48; i++) {
        if (plain[i] && (unichar)plain[i] == c) { *code = i; *shift = NO; return YES; }
        if (shifted[i] && (unichar)shifted[i] == c) { *code = i; *shift = YES; return YES; }
    }
    return NO;
}

// The modifier keys going down or up, as a keyboard reports them. Without
// the release, AppKit went on taking Shift as held after a typed ")", and
// the Command+P after it matched Shift+Command+P.
- (void)postFlags:(NSEventModifierFlags)flags {
    NSEvent *e = [NSEvent keyEventWithType:NSEventTypeFlagsChanged
                                  location:NSZeroPoint
                             modifierFlags:flags
                                 timestamp:[self stamp]
                              windowNumber:(NSApp.keyWindow ?: self.window).windowNumber
                                   context:nil
                                characters:@""
               charactersIgnoringModifiers:@""
                                 isARepeat:NO
                                   keyCode:flags & NSEventModifierFlagCommand ? 55 : 56];
    [NSApp postEvent:e atStart:NO];
}

- (void)postKey:(NSString *)chars ignoring:(NSString *)ign code:(unsigned short)code
          flags:(NSEventModifierFlags)flags {
    const NSEventModifierFlags held = flags & (NSEventModifierFlagShift |
        NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagCommand);
    if (held) [self postFlags:held];
    for (NSEventType t : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        NSEvent *e = [NSEvent keyEventWithType:t
                                      location:NSZeroPoint
                                 modifierFlags:flags
                                     timestamp:[self stamp]
                                  windowNumber:(NSApp.keyWindow ?: self.window).windowNumber
                                       context:nil
                                    characters:chars
                   charactersIgnoringModifiers:ign
                                     isARepeat:NO
                                       keyCode:code];
        [NSApp postEvent:e atStart:NO];
    }
    if (held) [self postFlags:0];
}

// Type text one key at a time. "\n" presses Return.
- (void)type:(NSString *)text {
    [self add:^(MCDemo *d, MCDone done) {
        NSMutableArray<NSString *> *keys = [NSMutableArray array];
        [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                                 options:NSStringEnumerationByComposedCharacterSequences
                              usingBlock:^(NSString *k, NSRange r, NSRange e, BOOL *stop) {
            (void)r; (void)e; (void)stop;
            [keys addObject:k];
        }];
        [d typeKeys:keys from:0 done:done];
    }];
}

- (void)typeKeys:(NSArray<NSString *> *)keys from:(NSUInteger)i done:(MCDone)done {
    if (i >= keys.count) { After(0.15, done); return; }
    NSString *k = keys[i];
    unichar c = [k characterAtIndex:0];
    double delay = 0.055;
    if (c == '\n') {
        [self postKey:@"\r" ignoring:@"\r" code:36 flags:0];
        delay = 0.25;
    } else {
        unsigned short code = 0; BOOL shift = NO;
        KeyFor(c, &code, &shift);
        [self postKey:k ignoring:k code:code flags:shift ? NSEventModifierFlagShift : 0];
        if (c == ' ') delay = 0.08;
    }
    After(delay, ^{ [self typeKeys:keys from:i + 1 done:done]; });
}

// Press a shortcut such as "shift+cmd+p", "ctrl+`", "cmd+s" or "return", and
// show `caption` (if any) while it takes effect.
- (void)key:(NSString *)combo caption:(NSString *)caption {
    [self add:^(MCDemo *d, MCDone done) {
        NSEventModifierFlags flags = 0;
        NSArray<NSString *> *parts = [combo componentsSeparatedByString:@"+"];
        for (NSUInteger i = 0; i + 1 < parts.count; i++) {
            NSString *m = parts[i];
            if ([m isEqualToString:@"cmd"])   flags |= NSEventModifierFlagCommand;
            if ([m isEqualToString:@"shift"]) flags |= NSEventModifierFlagShift;
            if ([m isEqualToString:@"ctrl"])  flags |= NSEventModifierFlagControl;
            if ([m isEqualToString:@"opt"])   flags |= NSEventModifierFlagOption;
        }
        NSString *k = parts.lastObject;
        unsigned short code = 0; BOOL shift = NO;
        NSString *chars = k;
        // Function and arrow keys carry the character AppKit gives them and
        // the flags a real keyboard sets (an empty string would make the
        // menu's performKeyEquivalent: throw).
        static const struct { const char *name; unichar c; unsigned short code; } special[] = {
            {"f12", NSF12FunctionKey, 111},
            {"left", NSLeftArrowFunctionKey, 123}, {"right", NSRightArrowFunctionKey, 124},
            {"down", NSDownArrowFunctionKey, 125}, {"up", NSUpArrowFunctionKey, 126},
        };
        if ([k isEqualToString:@"return"]) { chars = @"\r"; code = 36; }
        else if ([k isEqualToString:@"esc"]) { chars = @"\x1b"; code = 53; }
        else if ([k isEqualToString:@"space"]) { chars = @" "; code = 49; }
        else if (k.length > 1) {
            BOOL found = NO;
            for (const auto &sp : special) {
                if (![k isEqualToString:@(sp.name)]) continue;
                chars = [NSString stringWithCharacters:&sp.c length:1];
                code = sp.code;
                flags |= NSEventModifierFlagFunction;
                if (sp.code != 111) flags |= NSEventModifierFlagNumericPad;
                found = YES;
            }
            if (!found) [d fail:[NSString stringWithFormat:@"unknown key %@", k]];
        }
        else KeyFor([k characterAtIndex:0], &code, &shift);
        if (caption.length) [d showCaption:caption for:1.8];
        [d postKey:chars ignoring:chars code:code flags:flags];
        After(0.2, done);
    }];
}

// ---- running

- (void)setUpWindow {
    NSWindow *w = self.window;
    NSSize size = NSMakeSize(960, 600);
    NSString *env = NSProcessInfo.processInfo.environment[@"MINICODE_DEMO_SIZE"];
    NSArray *wh = [env componentsSeparatedByString:@"x"];
    if (wh.count == 2 && [wh[0] doubleValue] > 0 && [wh[1] doubleValue] > 0)
        size = NSMakeSize([wh[0] doubleValue], [wh[1] doubleValue]);
    NSRect vis = (w.screen ?: NSScreen.mainScreen).visibleFrame;
    [w setFrame:NSMakeRect(vis.origin.x + 40, NSMaxY(vis) - 40 - size.height,
                           size.width, size.height)
        display:YES];
    [w layoutIfNeeded];
    [(NSSplitView *)[self ui:@"splitView"] setPosition:210 ofDividerAtIndex:0];

    NSView *content = w.contentView;
    _overlay = [[MCDemoOverlay alloc] initWithFrame:content.bounds];
    _overlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _overlay.pointer = NSMakePoint(NSMidX(content.bounds), NSMidY(content.bounds));
    [content addSubview:_overlay positioned:NSWindowAbove relativeTo:nil];
}

- (void)startRecordingTo:(NSString *)dir {
    // macOS brings back the color panel an earlier scene left open (the
    // settings scene's), whatever HOME says; no scene starts with it.
    if ([NSColorPanel sharedColorPanelExists]) [[NSColorPanel sharedColorPanel] orderOut:nil];
    [self ignoreRealInput];
    _recorder = [[MCRecorder alloc] initWithDirectory:dir window:self.window];
    [_recorder start];
    [self next];
}

// Straight out once the frames are written: a quit would ask about a file
// a scene left unsaved and wait for an answer nobody gives.
- (void)finish {
    [_recorder stopThen:^{ exit(0); }];
}
@end

// ----------------------------------------------------------------- scenes
//
// Each scene runs in a window 960x600 points, sidebar 210 wide, rooted at a
// scratch copy of the repo's demo/ folder (made into a small git repo by
// scripts/demos.sh). Keep them short: a GIF is watched, not read.

static void SceneTour(MCDemo *d) {
    [d pause:0.8];
    [d clickFile:@"README.md"];
    [d pause:1.8];
    [d key:@"shift+cmd+v" caption:@"⇧⌘V  Markdown source"];
    [d pause:1.8];
    [d key:@"shift+cmd+v" caption:@"⇧⌘V  Back to the preview"];
    [d pause:1.4];
    [d clickFile:@"hello.py"];
    [d hidePointer];
    [d pause:1.0];
    [d poster];
    [d clickAt:^NSPoint { return [d pointForText:@"c.area():.2f}\")" after:nil atEnd:YES]; }];
    [d hidePointer];
    // Return keeps the line's indentation, as it does in the editor.
    [d type:@"\nprint(\"Diameter:\", 2 * c.radius)"];
    [d pause:0.5];
    [d key:@"cmd+s" caption:@"⌘S  Save"];
    [d pause:1.2];
    // Not Cmd+P: on the CI runner the posted key opened the command
    // palette rather than Go to File, which no hand has seen.
    [d clickFile:@"sample.cpp"];
    [d pause:1.8];
}

static void SceneTerminal(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"hello.py"];
    [d pause:0.8];
    [d hidePointer];
    // Room for the log, so git shows it without its pager.
    [d run:^(MCDemo *m) { [m.controller setValue:@330 forKey:@"terminalHeight"]; }];
    [d key:@"ctrl+`" caption:@"⌃`  Terminal"];
    // Wait for zsh's first prompt; nobody needs to watch a shell start.
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"terminal"] valueForKey:@"atPrompt"] boolValue];
    } timeout:15 recorded:NO];
    [d pause:0.6];
    [d type:@"ls -G\n"];
    [d pause:1.2];
    [d type:@"git log --oneline\n"];
    [d pause:1.6];
    [d type:@"python3 hello.py\n"];
    [d pause:1.4];
    [d poster];
    [d key:@"cmd+\\" caption:@"⌘\\  Split the terminal"];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"terminal"] valueForKey:@"atPrompt"] boolValue];
    } timeout:15 recorded:NO];
    [d pause:0.4];
    [d type:@"git status --short\n"];
    [d pause:1.8];
}

// The active terminal's cell grid (terminal.mode = grid, the default).
static TerminalGridView *TermGrid(MCDemo *m) {
    TerminalGridView *g = [[m ui:@"terminal"] valueForKey:@"grid"];
    if (!g.screen) [m fail:@"the terminal has no grid"];
    return g;
}

// Window point of `needle` on the terminal's screen, the lowest match, a
// third of the way into it. The lines the scenes look for are ASCII up to
// the match, so a byte is a column.
static NSPoint TermPoint(MCDemo *m, NSString *needle) {
    TerminalGridView *g = TermGrid(m);
    TerminalScreen *s = g.screen;
    const std::string n = needle.UTF8String;
    for (int row = s->rows() - 1; row >= 0; row--) {
        const size_t at = s->lineText(row).rfind(n);
        if (at == std::string::npos) continue;
        const NSSize cell = [TerminalGridView cellSize];
        const CGFloat pad = [TerminalGridView padding];
        NSPoint p = NSMakePoint(pad + (at + n.size() / 3.0) * cell.width,
                                pad + (row + 0.5) * cell.height);
        return [g convertPoint:p toView:nil];
    }
    [m fail:[NSString stringWithFormat:@"no “%@” in the terminal", needle]];
    return NSZeroPoint;
}

// A point inside the terminal, for a click that gives it the keyboard back.
static NSPoint TermInside(MCDemo *m) {
    TerminalGridView *g = TermGrid(m);
    return [g convertPoint:NSMakePoint(NSMidX(g.bounds), NSMaxY(g.bounds) - 30) toView:nil];
}

// Command+click what the terminal prints: a grep result opens at its line,
// a URL in the browser panel.
static void SceneLinks(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"sample.cpp"];
    [d hidePointer];
    [d run:^(MCDemo *m) { [m.controller setValue:@300 forKey:@"terminalHeight"]; }];
    [d key:@"ctrl+`" caption:@"⌃`  Terminal"];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"terminal"] valueForKey:@"atPrompt"] boolValue];
    } timeout:15 recorded:NO];
    [d pause:0.4];
    [d type:@"grep -Hn \"def area\" *.py\n"];
    [d pause:1.0];
    [d moveTo:^NSPoint { return TermPoint(d, @"hello.py:9"); }];
    [d caption:@"⌘-click a file:line in the output"];
    [d pause:0.9];
    [d cmdClickAt:^NSPoint { return TermPoint(d, @"hello.py:9"); }];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"textView"] string] hasPrefix:@"# A sample Python file"];
    } timeout:10 recorded:YES];
    [d hidePointer];
    [d poster];
    [d pause:2.0];
    // Opening the file moved focus to the editor; a click in the terminal
    // brings it back, as it would for a person.
    [d clickAt:^NSPoint { return TermInside(d); }];
    [d hidePointer];
    [d type:@"echo \"docs: https://docs.python.org/3/library/math.html\"\n"];
    [d pause:0.8];
    [d moveTo:^NSPoint { return TermPoint(d, @"docs.python.org"); }];
    [d caption:@"URLs open in the browser panel"];
    [d pause:0.8];
    [d cmdClickAt:^NSPoint { return TermPoint(d, @"docs.python.org"); }];
    [d hidePointer];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[m.controller valueForKey:@"browserVisible"] boolValue];
    } timeout:10 recorded:YES];
    [d pause:3.0];
}

// Pick a new color for `key` the way a person does: click its swatch, then
// drag through the color panel, here in `steps` even steps to `to`.
static void PickColor(MCDemo *d, NSString *key, NSString *from, uint32_t to, int steps) {
    NSString *line = [NSString stringWithFormat:@"# %@ = ", key];
    [d clickAt:^NSPoint { return [d pointForText:from after:line atEnd:NO]; }];
    [d run:^(MCDemo *m) {
        NSColorPanel *panel = [NSColorPanel sharedColorPanel];
        NSRect wf = m.window.frame;
        [panel setFrameTopLeftPoint:NSMakePoint(NSMaxX(wf) - panel.frame.size.width - 24,
                                                NSMaxY(wf) - 64)];
    }];
    [d pause:0.5];
    unsigned int start = 0;
    [[NSScanner scannerWithString:[from substringFromIndex:1]] scanHexInt:&start];
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / steps;
        auto mix = [&](int shift) {
            double a = (start >> shift) & 0xFF, b = (to >> shift) & 0xFF;
            return (a + (b - a) * t) / 255.0;
        };
        NSColor *c = [NSColor colorWithSRGBRed:mix(16) green:mix(8) blue:mix(0) alpha:1];
        [d run:^(MCDemo *m) {
            NSColorPanel *panel = [NSColorPanel sharedColorPanel];
            panel.color = c;
            // Setting the color may or may not send the panel's action (the
            // editor's colorPicked:); send it once more to be sure. A repeat
            // of the same color changes nothing.
            [NSApp sendAction:NSSelectorFromString(@"colorPicked:")
                           to:m.controller from:panel];
        }];
        [d pause:0.22];
    }
    [d pause:0.6];
}

// Run a menu command by name from the command palette.
static void Palette(MCDemo *d, NSString *typed, NSString *caption) {
    [d key:@"shift+cmd+p" caption:caption];
    [d pause:0.5];
    [d type:typed];
    [d pause:0.7];
    [d key:@"return" caption:nil];
}

static void SceneSettings(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"sample.cpp"];
    [d pause:1.0];
    [d hidePointer];
    // Themes first: once the file sets colors of its own, picking a theme
    // asks whether to comment them out.
    Palette(d, @"theme light modern", @"⇧⌘P  Pick a theme by name");
    [d pause:1.8];
    Palette(d, @"theme solarized dark", nil);
    [d pause:1.8];
    Palette(d, @"theme dark+", nil);
    [d pause:1.0];
    [d key:@"cmd+," caption:@"⌘,  Settings"];
    [d pause:1.0];
    [d scrollEditorTo:@"# The file tree."];
    [d pause:0.6];
    // GitHub Dark's surfaces, and a quiet status bar: colors someone might
    // keep, rather than a showcase of loud ones.
    PickColor(d, @"sidebar.background", @"#181818", 0x1F2428, 7);
    PickColor(d, @"editor.background", @"#1F1F1F", 0x24292E, 7);
    PickColor(d, @"statusbar.background", @"#181818", 0x3A3F46, 7);
    [d run:^(MCDemo *m) { (void)m; [[NSColorPanel sharedColorPanel] orderOut:nil]; }];
    [d hidePointer];
    [d pause:0.6];
    [d poster];
    [d pause:1.4];
}

static void SceneLatex(MCDemo *d) {
    if (!MCTectonicPath()) {
        d.skipReason = @"tectonic is not installed";
        return;
    }
    __block PDFDocument *before = nil;
    auto pdfView = [](MCDemo *m) -> PDFView * {
        return [[m ui:@"latex"] valueForKey:@"pdf"];
    };
    [d pause:0.6];
    [d clickFile:@"paper/notes.tex"];
    [d waitFor:^BOOL(MCDemo *m) { return [pdfView(m) document] != nil; }
       timeout:60 recorded:NO];
    [d pause:1.2];
    [d poster];
    [d caption:@"Double-click the PDF to edit the LaTeX behind it"];
    [d doubleClickAt:^NSPoint {
        PDFView *pdf = pdfView(d);
        PDFSelection *sel = [pdf.document findString:@"Ask for a review" withOptions:0].firstObject;
        if (!sel) [d fail:@"the text to click is not in the PDF"];
        PDFPage *page = sel.pages.firstObject;
        NSRect r = [sel boundsForPage:page];
        // On "review", the last word.
        NSPoint onPage = NSMakePoint(NSMaxX(r) - r.size.width * 0.15, NSMidY(r));
        NSPoint inView = [pdf convertPoint:onPage fromPage:page];
        return [pdf convertPoint:inView toView:nil];
    }];
    [d hidePointer];
    [d pause:1.2];
    [d run:^(MCDemo *m) { before = [pdfView(m) document]; }];
    [d type:@"Ask two friends for a review"];
    [d pause:0.5];
    [d key:@"return" caption:@"⏎  Save the edit"];
    [d waitFor:^BOOL(MCDemo *m) { return [pdfView(m) document] != before; }
       timeout:60 recorded:YES];
    [d pause:1.8];
    [d key:@"shift+cmd+v" caption:@"⇧⌘V  The source, edited in place"];
    [d run:^(MCDemo *m) {
        NSTextView *tv = [m ui:@"textView"];
        NSRange r = [tv.string rangeOfString:@"Ask two friends for a review"];
        if (r.location != NSNotFound) [tv setSelectedRange:r];
    }];
    [d pause:2.4];
}

// The language server's status text ("clangd: 1 error"), as the status bar has it.
static NSString *LspStatus(MCDemo *m) {
    return [[m ui:@"lsp"] valueForKey:@"statusText"] ?: @"";
}

static void SceneLsp(MCDemo *d) {
    [d pause:0.4];
    [d clickFile:@"vec/main.cpp"];
    [d hidePointer];
    // clangd parses the file before it has anything to say.
    [d waitFor:^BOOL(MCDemo *m) {
        if ([LspStatus(m) hasPrefix:@"No "])
            [m fail:@"the lsp scene needs clangd (the Xcode command line tools)"];
        return [LspStatus(m) hasSuffix:@"no problems"];
    } timeout:40 recorded:NO];
    [d pause:0.5];
    [d clickAt:^NSPoint { return [d pointForText:@"scaled(2);" after:nil atEnd:YES]; }];
    [d hidePointer];
    // Return indents the line itself, so no spaces (I1: four more made eight).
    [d type:@"\ndouble d = p."];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"lsp"] valueForKey:@"completionVisible"] boolValue];
    } timeout:10 recorded:YES];
    [d pause:0.8];
    [d type:@"le"];
    [d pause:0.5];
    [d key:@"return" caption:@"⏎  Complete"];
    [d pause:0.3];
    [d type:@";"];
    [d pause:0.6];
    // A mistake, which clangd reports as you type.
    [d type:@"\nint n = q + 1;"];
    [d waitFor:^BOOL(MCDemo *m) { return [LspStatus(m) hasSuffix:@"1 error"]; }
       timeout:15 recorded:YES];
    [d poster];
    [d pause:1.2];
    [d clickAt:^NSPoint { return [d pointForText:@"q + 1" after:@"int n" atEnd:NO]; }];
    [d hidePointer];
    [d pause:0.1];
    [d key:@"opt+cmd+i" caption:@"⌥⌘I  What is wrong here"];
    [d pause:2.4];
    [d run:^(MCDemo *m) { [[[m ui:@"lsp"] valueForKey:@"hover"] close]; }];
    [d key:@"ctrl+shift+k" caption:@"⌃⇧K  Delete the line"];
    [d waitFor:^BOOL(MCDemo *m) { return [LspStatus(m) hasSuffix:@"no problems"]; }
       timeout:15 recorded:YES];
    [d pause:0.5];
    [d clickAt:^NSPoint { return [d pointForText:@"scaled" after:nil atEnd:NO]; }];
    [d hidePointer];
    [d key:@"opt+cmd+i" caption:@"⌥⌘I  Hover info"];
    [d pause:2.0];
    [d run:^(MCDemo *m) { [[[m ui:@"lsp"] valueForKey:@"hover"] close]; }];
    // Saved first, or leaving the file asks about the unsaved edit.
    [d key:@"cmd+s" caption:@"⌘S  Save"];
    [d pause:0.4];
    [d key:@"f12" caption:@"F12  Go to definition"];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"textView"] string] hasPrefix:@"#pragma once"];
    } timeout:10 recorded:YES];
    [d pause:1.6];
}

static void SceneVim(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"README.md"];
    [d hidePointer];
    // A taller panel than the default, as if the divider had been dragged up.
    [d run:^(MCDemo *m) { [m.controller setValue:@370 forKey:@"terminalHeight"]; }];
    [d key:@"ctrl+`" caption:@"⌃`  Terminal"];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"terminal"] valueForKey:@"atPrompt"] boolValue];
    } timeout:15 recorded:NO];
    [d pause:0.3];
    auto grid = ^BOOL(MCDemo *m) {
        return [[[m ui:@"terminal"] valueForKey:@"gridMode"] boolValue];
    };
    auto log = ^BOOL(MCDemo *m) {
        return ![[[m ui:@"terminal"] valueForKey:@"gridMode"] boolValue] &&
               [[[m ui:@"terminal"] valueForKey:@"atPrompt"] boolValue];
    };
    [d type:@"vim todo.md\n"];
    [d waitFor:grid timeout:10 recorded:YES];
    [d pause:0.5];
    [d type:@"i# Today\n\n- Try the LSP demo\n- Read the git log"];
    [d pause:0.4];
    [d poster];
    [d key:@"esc" caption:nil];
    [d type:@":wq"];
    [d pause:0.3];
    [d key:@"return" caption:nil];
    [d waitFor:log timeout:10 recorded:YES];
    [d pause:0.4];
    [d type:@"cat todo.md\n"];
    [d pause:1.2];
    // git's pager runs with LESS=FRX, so it keeps the main screen: wait for
    // it to hold the terminal rather than for the alternate screen.
    [d type:@"git log\n"];
    [d waitFor:^BOOL(MCDemo *m) { return [[[m ui:@"terminal"] valueForKey:@"busy"] boolValue]; }
       timeout:10 recorded:YES];
    [d pause:1.4];
    [d key:@"space" caption:@"Space  Next page"];
    [d pause:1.2];
    [d key:@"q" caption:@"q  Quit the pager"];
    [d waitFor:log timeout:10 recorded:YES];
    [d pause:1.2];
}

// Images and PDFs open where the editor sits.
static void SceneFiles(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"images/icon.png"];
    [d caption:@"Images open in the editor pane, never enlarged"];
    [d hidePointer];
    [d pause:2.4];
    [d poster];
    [d clickFile:@"paper/notes.pdf"];
    [d caption:@"PDFs too, with the page count in the title"];
    [d hidePointer];
    [d pause:2.6];
    [d clickFile:@"data/cities.csv"];
    [d caption:@"A CSV file opens as a table you can edit"];
    [d hidePointer];
    [d pause:2.4];
    [d key:@"shift+cmd+v" caption:@"⇧⌘V  The text behind it"];
    [d pause:1.8];
}

// ---- source control

static MCGitPanel *GitPanel(MCDemo *m) { return [m ui:@"gitPanel"]; }

// The panel's row whose description ("  M hello.py", a header) holds `text`.
static NSInteger GitRow(MCDemo *m, NSString *text) {
    NSArray<NSString *> *rows = [GitPanel(m) rowDescriptions];
    for (NSUInteger i = 0; i < rows.count; i++)
        if ([rows[i] containsString:text]) return (NSInteger)i;
    return -1;
}

static NSPoint GitRowPoint(MCDemo *m, NSString *text) {
    NSInteger row = GitRow(m, text);
    if (row < 0) [m fail:[NSString stringWithFormat:@"no %@ in the source control panel", text]];
    NSTableView *list = GitPanel(m).list;
    NSRect r = [list rectOfRow:row];
    return [list convertPoint:NSMakePoint(NSMinX(r) + 70, NSMidY(r)) toView:nil];
}

// Edit a file, see its change bar, stage the hunk from the diff, commit,
// and see the commit at the top of the graph.
static void SceneGit(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"hello.py"];
    [d clickAt:^NSPoint { return [d pointForText:@"# A sample Python file" after:nil atEnd:YES]; }];
    [d hidePointer];
    [d type:@", with a circle"];
    [d pause:0.4];
    [d clickAt:^NSPoint { return [d pointForText:@"Circle(3.5)" after:nil atEnd:NO]; }];
    [d hidePointer];
    [d key:@"cmd+s" caption:@"Change bars mark the lines that changed"];
    [d pause:1.8];
    [d key:@"ctrl+shift+g" caption:@"⌃⇧G  Source control"];
    [d waitFor:^BOOL(MCDemo *m) { return GitRow(m, @"W M hello.py") >= 0; }
       timeout:10 recorded:YES];
    [d pause:1.2];
    [d clickAt:^NSPoint { return GitRowPoint(d, @"W M hello.py"); }];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"diffView"] string] containsString:@"Stage Hunk"];
    } timeout:10 recorded:YES];
    [d poster];
    [d pause:1.2];
    [d caption:@"Stage one hunk from the diff"];
    [d clickAt:^NSPoint {
        return [d pointForText:@"Stage Hunk" inView:[d ui:@"diffView"] after:nil atEnd:NO];
    }];
    [d hidePointer];
    [d waitFor:^BOOL(MCDemo *m) { return GitRow(m, @"S M hello.py") >= 0; }
       timeout:10 recorded:YES];
    [d pause:1.4];
    [d clickAt:^NSPoint {
        NSTextView *box = GitPanel(d).messageView;
        return [box convertPoint:NSMakePoint(30, NSMidY(box.bounds)) toView:nil];
    }];
    [d hidePointer];
    [d type:@"Say what the sample shows"];
    [d pause:0.4];
    [d key:@"cmd+return" caption:@"⌘⏎  Commit"];
    [d waitFor:^BOOL(MCDemo *m) { return GitRow(m, @"hello.py") < 0; } timeout:10 recorded:YES];
    [d pause:2.4];
}

// ---- Markdown

static void SceneMarkdown(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"README.md"];
    [d hidePointer];
    [d pause:1.6];
    [d key:@"shift+cmd+v" caption:@"⇧⌘V  The source"];
    [d pause:1.0];
    [d key:@"ctrl+cmd+v" caption:@"⌃⌘V  The page beside it"];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"sidePreview"] textView].string containsString:@"Field notes"];
    } timeout:10 recorded:YES];
    [d pause:1.0];
    [d clickAt:^NSPoint {
        return [d pointForText:@"Add a perimeter to the circle" after:nil atEnd:YES];
    }];
    [d hidePointer];
    [d type:@"\nTry the page beside the source"];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"sidePreview"] textView].string containsString:@"Try the page"];
    } timeout:10 recorded:YES];
    [d poster];
    [d pause:1.2];
    [d caption:@"Double-click the page to edit its Markdown"];
    [d doubleClickAt:^NSPoint {
        return [d pointForText:@"Double-click a block"
                        inView:[[d ui:@"sidePreview"] textView] after:nil atEnd:NO];
    }];
    [d hidePointer];
    [d pause:1.0];
    [d type:@"> Edits made on the page go back into the source."];
    [d pause:0.4];
    [d key:@"return" caption:nil];
    [d waitFor:^BOOL(MCDemo *m) {
        return [[[m ui:@"textView"] string] containsString:@"go back into the source"];
    } timeout:10 recorded:YES];
    [d pause:2.4];
}

// ---- agents

static MCAgentsPanel *AgentsPanel(MCDemo *m) { return [[m ui:@"agents"] panel]; }

static NSInteger AgentRow(MCDemo *m, NSString *text) {
    NSArray<NSString *> *rows = [AgentsPanel(m) rowDescriptions];
    for (NSUInteger i = 0; i < rows.count; i++)
        if ([rows[i] containsString:text]) return (NSInteger)i;
    return -1;
}

// The Agents panel over transcripts scripts/demos.sh writes
// (tools/demo-agents): the sessions, the files one wrote, its evidence.
static void SceneAgents(MCDemo *d) {
    [d pause:0.6];
    [d clickFile:@"hello.py"];
    [d hidePointer];
    [d pause:0.8];
    [d key:@"shift+cmd+a" caption:@"⇧⌘A  What agents did in this folder"];
    [d pause:0.6];
    // On the runner the key once never reached the menu (the Services
    // menu's own Shift+Command+A, perhaps); the menu's action then.
    [d run:^(MCDemo *m) {
        if (![[m.controller valueForKey:@"agentsVisible"] boolValue])
            [NSApp sendAction:NSSelectorFromString(@"toggleAgents:") to:nil from:nil];
    }];
    [d waitFor:^BOOL(MCDemo *m) {
        return AgentsPanel(m) && AgentRow(m, @"perimeter") >= 0;
    } timeout:15 recorded:YES];
    [d pause:1.4];
    [d run:^(MCDemo *m) { [AgentsPanel(m) toggleRow:AgentRow(m, @"perimeter")]; }];
    [d pause:1.6];
    [d clickAt:^NSPoint {
        NSOutlineView *o = AgentsPanel(d).outline;
        NSRect r = [o rectOfRow:AgentRow(d, @"perimeter")];
        return [o convertPoint:NSMakePoint(NSMinX(r) + 60, NSMidY(r)) toView:nil];
    }];
    [d caption:@"Its last message beside its last test run"];
    [d hidePointer];
    [d poster];
    [d pause:3.2];
}

typedef void (*MCSceneBuilder)(MCDemo *d);
static const struct { const char *name; MCSceneBuilder build; } kScenes[] = {
    {"tour", SceneTour},
    {"terminal", SceneTerminal},
    {"settings", SceneSettings},
    {"latex", SceneLatex},
    {"lsp", SceneLsp},
    {"vim", SceneVim},
    {"files", SceneFiles},
    {"links", SceneLinks},
    {"git", SceneGit},
    {"markdown", SceneMarkdown},
    {"agents", SceneAgents},
};

// ------------------------------------------------------------ entry points

BOOL MCDemoListScenes(void) {
    const char *scene = getenv("MINICODE_DEMO");
    if (!scene || strcmp(scene, "list") != 0) return NO;
    for (const auto &s : kScenes) printf("%s\n", s.name);
    return YES;
}

void MCDemoStart(EditorController *controller) {
    const char *name = getenv("MINICODE_DEMO");
    if (!name || !*name) return;
    MCSceneBuilder build = nullptr;
    for (const auto &s : kScenes)
        if (strcmp(s.name, name) == 0) build = s.build;
    if (!build) {
        fprintf(stderr, "minicode demo: no scene called %s\n", name);
        exit(2);
    }
    const char *frames = getenv("MINICODE_DEMO_FRAMES");
    if (!frames || !*frames) {
        fprintf(stderr, "minicode demo: set MINICODE_DEMO_FRAMES to a folder\n");
        exit(2);
    }
    static MCDemo *demo;   // lives until the app quits
    demo = [[MCDemo alloc] initWithController:controller];
    build(demo);
    if (demo.skipReason) {
        fprintf(stderr, "minicode demo: skipping %s: %s\n", name,
                demo.skipReason.UTF8String);
        exit(3);
    }
    // Nothing should take this long; if it does, don't leave a window behind.
    After(180, ^{
        fprintf(stderr, "minicode demo: %s did not finish in time\n", name);
        exit(2);
    });
    [demo setUpWindow];
    NSString *dir = [NSString stringWithUTF8String:frames];
    // Let the resized window settle before the first frame.
    After(0.6, ^{ [demo startRecordingTo:dir]; });
}
