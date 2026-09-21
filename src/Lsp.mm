// Lsp.mm — Objective-C++. See Lsp.h. Server processes (MCLspServer), the
// completion list (MCCompletionPopup), the editor's text view (CodeTextView)
// and the per-window glue (LspSession).
#import "Lsp.h"
#import "AppSettings.h"
#include "LspClient.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

// ------------------------------------------------------------------ helpers
static NSColor *LHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static NSString *NS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

static std::string Std(NSString *s) {
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

static std::u16string U16Of(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}

// The canonical spelling of a path (/tmp -> /private/tmp), so paths from a
// server and from the file tree can be compared. The path itself if it does
// not exist.
static NSString *RealPath(NSString *p) {
    if (!p.length) return p;
    char buf[PATH_MAX];
    if (realpath(p.fileSystemRepresentation, buf)) return [NSString stringWithUTF8String:buf];
    return p.stringByStandardizingPath;
}

static BOOL IsIdentChar(unichar c) {
    return c == '_' || c == '$' ||
           [[NSCharacterSet alphanumericCharacterSet] characterIsMember:c];
}

// ------------------------------------------------------ finding a server
// A Finder-launched app gets a short PATH (/usr/bin:/bin:/usr/sbin:/sbin), so
// the places package managers put language servers are searched as well.
static NSArray<NSString *> *MCSearchDirs(void) {
    NSMutableArray<NSString *> *dirs = [NSMutableArray array];
    NSString *path = NSProcessInfo.processInfo.environment[@"PATH"] ?: @"";
    NSString *home = NSHomeDirectory();
    NSArray *extra = @[@"/opt/homebrew/bin", @"/usr/local/bin",
                       [home stringByAppendingPathComponent:@".cargo/bin"],
                       [home stringByAppendingPathComponent:@"go/bin"],
                       [home stringByAppendingPathComponent:@".local/bin"],
                       @"/usr/bin"];
    for (NSString *d in [[path componentsSeparatedByString:@":"]
                            arrayByAddingObjectsFromArray:extra])
        if (d.length && ![dirs containsObject:d]) [dirs addObject:d];
    return dirs;
}

// The developer tools' own copy of a tool. /usr/bin/clangd is only a shim
// that runs it through xcrun, and on a Mac without the command line tools
// running the shim pops up the "install developer tools" dialog, which an
// editor opening a .c file must never do. So the shim is never run; the
// real binary is, when it exists.
static NSString *MCDeveloperTool(NSString *name) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dev = [fm destinationOfSymbolicLinkAtPath:@"/var/db/xcode_select_link"
                                                  error:nil];
    NSMutableArray *roots = [NSMutableArray array];
    if (dev.length) [roots addObject:dev];
    [roots addObject:@"/Library/Developer/CommandLineTools"];
    for (NSString *root in roots) {
        for (NSString *sub in @[@"usr/bin",
                                @"Toolchains/XcodeDefault.xctoolchain/usr/bin"]) {
            NSString *p = [[root stringByAppendingPathComponent:sub]
                              stringByAppendingPathComponent:name];
            if ([fm isExecutableFileAtPath:p]) return p;
        }
    }
    return nil;
}

static NSString *MCFindProgram(NSString *name) {
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([name containsString:@"/"]) {
        NSString *p = name.stringByExpandingTildeInPath;
        return [fm isExecutableFileAtPath:p] ? p : nil;
    }
    for (NSString *dir in MCSearchDirs()) {
        if ([dir isEqualToString:@"/usr/bin"]) {
            NSString *real = MCDeveloperTool(name);
            if (real) return real;
            continue;
        }
        NSString *p = [dir stringByAppendingPathComponent:name];
        if ([fm isExecutableFileAtPath:p]) return p;
    }
    return nil;
}

// A command line from the settings (or the defaults) -> program path plus
// arguments, or nil when the program is not installed.
static NSArray<NSString *> *MCResolveCommand(const std::string &command) {
    std::vector<std::string> words = Lsp::splitCommand(command);
    if (words.empty()) return nil;
    NSString *prog = MCFindProgram(NS(words[0]));
    if (!prog) return nil;
    NSMutableArray *argv = [NSMutableArray arrayWithObject:prog];
    for (size_t i = 1; i < words.size(); i++) [argv addObject:NS(words[i])];
    return argv;
}

// ------------------------------------------------------------- MCLspServer
// One language server process. The client state machine runs on the main
// thread; bytes are read on a background queue and handed over, and writes
// go through a serial queue so a server that is slow to read never blocks
// typing.
@class MCLspServer;
static NSMutableSet<MCLspServer *> *gServers;   // every running server

@interface MCLspServer : NSObject
@property(nonatomic, readonly, copy) NSString *key;
@property(nonatomic, readonly, copy) NSString *name;   // program name, for the status bar
@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) BOOL stopping;
@property(nonatomic, readonly) pid_t pid;
@property(nonatomic, readonly) BOOL processRunning;   // NSTask's view, any thread
@property(nonatomic, copy) void (^onExit)(MCLspServer *server);
- (instancetype)initWithKey:(NSString *)key argv:(NSArray<NSString *> *)argv
                       root:(NSString *)root;
- (Lsp::Client &)client;
- (void)stop;       // shutdown, exit, then signals if it lingers
- (void)stopNow;    // for app quit: no waiting on the event loop
@end

@implementation MCLspServer {
    NSTask *_task;
    NSFileHandle *_input;
    int _inputFd;
    BOOL _inputClosed;
    dispatch_queue_t _writeQueue;
    std::unique_ptr<Lsp::Client> _client;
    NSFileHandle *_log;
}

- (instancetype)initWithKey:(NSString *)key argv:(NSArray<NSString *> *)argv
                       root:(NSString *)root {
    if (!(self = [super init])) return nil;
    _key = [key copy];
    _name = [argv.firstObject.lastPathComponent copy];
    _task = [[NSTask alloc] init];
    _task.executableURL = [NSURL fileURLWithPath:argv.firstObject];
    _task.arguments = [argv subarrayWithRange:NSMakeRange(1, argv.count - 1)];
    BOOL isDir = NO;
    if (root && [[NSFileManager defaultManager] fileExistsAtPath:root isDirectory:&isDir] && isDir)
        _task.currentDirectoryURL = [NSURL fileURLWithPath:root];

    // Servers start helpers of their own (node, go, cargo), which need the
    // same wider PATH the server was found on.
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    env[@"PATH"] = [MCSearchDirs() componentsJoinedByString:@":"];
    _task.environment = env;

    NSPipe *toServer = [NSPipe pipe], *fromServer = [NSPipe pipe];
    _task.standardInput = toServer;
    _task.standardOutput = fromServer;
    // stderr must go somewhere that never fills up: clangd logs every
    // request there, and a full pipe would stall the server mid-reply.
    NSString *logPath = NSProcessInfo.processInfo.environment[@"MINICODE_LSP_LOG"];
    if (logPath.length) {
        // O_APPEND, so several servers (and their stderr) share one file
        // without writing over each other.
        int fd = open(logPath.fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) _log = [[NSFileHandle alloc] initWithFileDescriptor:fd closeOnDealloc:YES];
    }
    _task.standardError = _log ?: [NSFileHandle fileHandleWithNullDevice];

    __weak MCLspServer *weakSelf = self;
    fromServer.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *data = h.availableData;
        if (data.length == 0) { h.readabilityHandler = nil; return; }
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf received:data]; });
    };
    _task.terminationHandler = ^(NSTask *t) {
        (void)t;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf ended]; });
    };

    NSError *err = nil;
    if (![_task launchAndReturnError:&err]) {
        NSLog(@"MiniCode: could not start %@: %@", _name, err.localizedDescription);
        return nil;
    }
    _running = YES;
    _pid = _task.processIdentifier;
    _input = toServer.fileHandleForWriting;
    _inputFd = _input.fileDescriptor;
    // A server that dies mid-write must not take the editor with it: with
    // NOSIGPIPE the write fails with EPIPE instead of raising SIGPIPE.
    fcntl(_inputFd, F_SETNOSIGPIPE, 1);
    _writeQueue = dispatch_queue_create("minicode.lsp.write", DISPATCH_QUEUE_SERIAL);
    _client = std::make_unique<Lsp::Client>([weakSelf](const std::string &bytes) {
        [weakSelf writeBytes:bytes];
    });
    if (!gServers) gServers = [NSMutableSet set];
    [gServers addObject:self];
    return self;
}

- (Lsp::Client &)client { return *_client; }
- (BOOL)processRunning { return _task.isRunning; }

- (void)logLine:(NSString *)prefix bytes:(const std::string &)bytes {
    if (!_log) return;
    NSMutableData *d = [NSMutableData data];
    [d appendData:[prefix dataUsingEncoding:NSUTF8StringEncoding]];
    [d appendBytes:bytes.data() length:bytes.size()];
    [d appendBytes:"\n" length:1];
    [_log writeData:d];
}

- (void)writeBytes:(const std::string &)bytes {
    if (_inputClosed) return;
    [self logLine:@"--> " bytes:bytes];
    NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    int fd = _inputFd;
    NSFileHandle *keep = _input;   // keeps the descriptor open until written
    dispatch_async(_writeQueue, ^{
        (void)keep;
        const char *p = (const char *)data.bytes;
        size_t left = data.length;
        while (left > 0) {
            ssize_t n = write(fd, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                return;   // EPIPE: the server is gone; its exit is handled elsewhere
            }
            p += n;
            left -= (size_t)n;
        }
    });
}

// EOF on the server's stdin, after anything already queued. Servers exit on
// it, which is the backstop if they ignore `exit`.
- (void)closeInput {
    if (_inputClosed) return;
    _inputClosed = YES;
    NSFileHandle *h = _input;
    dispatch_async(_writeQueue, ^{ [h closeFile]; });
}

- (void)received:(NSData *)data {
    if (_log) [self logLine:@"<-- " bytes:std::string((const char *)data.bytes, data.length)];
    _client->receive((const char *)data.bytes, data.length);
}

- (void)ended {
    if (!_running) return;
    _running = NO;
    [self closeInput];
    [gServers removeObject:self];
    if (self.onExit) self.onExit(self);
}

- (void)stop {
    if (!_running || _stopping) return;
    _stopping = YES;
    __weak MCLspServer *weakSelf = self;
    _client->shutdown([weakSelf] { [weakSelf closeInput]; });
    // A server that doesn't answer shutdown within two seconds is told to
    // go, then made to.
    pid_t pid = _pid;
    MCLspServer *strong = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        if (!strong.running) return;
        [strong closeInput];
        kill(pid, SIGTERM);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            if (strong.running) kill(pid, SIGKILL);
        });
    });
}

- (void)stopNow {
    if (!_running) return;
    _stopping = YES;
    _client->exitNow();
    [self closeInput];
    dispatch_sync(_writeQueue, ^{});   // flushed and closed
}
@end

void MCLspTerminateAllServers(void) {
    NSArray<MCLspServer *> *servers = gServers.allObjects;
    if (!servers.count) return;
    for (MCLspServer *s in servers) [s stopNow];
    // The app is about to exit, so wait here rather than on the event loop:
    // half a second for a clean exit, then signals. NSTask updates isRunning
    // off the main thread, so polling it works while main is blocked.
    auto alive = [](MCLspServer *s) { return (bool)s.processRunning; };
    for (int i = 0; i < 50; i++) {
        BOOL any = NO;
        for (MCLspServer *s in servers) if (alive(s)) any = YES;
        if (!any) return;
        usleep(10000);
    }
    for (MCLspServer *s in servers) if (alive(s)) kill(s.pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        BOOL any = NO;
        for (MCLspServer *s in servers) if (alive(s)) any = YES;
        if (!any) return;
        usleep(10000);
    }
    for (MCLspServer *s in servers) if (alive(s)) kill(s.pid, SIGKILL);
}

NSUInteger MCLspRunningServerCount(void) { return gServers.count; }

// -------------------------------------------------------- CodeTextView
@implementation MCDiagnosticMark
@end

@implementation CodeTextView {
    BOOL _tooltipsDirty;
    NSArray<NSString *> *_tooltipMessages;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(storageEdited:)
                   name:NSTextStorageDidProcessEditingNotification
                 object:self.textStorage];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)setDiagnostics:(NSArray<MCDiagnosticMark *> *)diagnostics {
    _diagnostics = [diagnostics copy];
    _tooltipsDirty = YES;
    self.needsDisplay = YES;
}

// Keep the marks on their text while it is edited, until the server sends
// fresh ones: shift those after the edit, stretch those it touches.
- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || _diagnostics.count == 0) return;
    NSRange er = ts.editedRange;
    NSInteger delta = ts.changeInLength;
    NSInteger s = (NSInteger)er.location;
    NSInteger oldEnd = (NSInteger)NSMaxRange(er) - delta;
    NSInteger newEnd = (NSInteger)NSMaxRange(er);
    NSInteger length = (NSInteger)ts.length;
    NSMutableArray *kept = [NSMutableArray array];
    for (MCDiagnosticMark *m in _diagnostics) {
        NSInteger a = (NSInteger)m.range.location, b = (NSInteger)NSMaxRange(m.range);
        if (b <= s) {
            // before the edit: unchanged
        } else if (a >= oldEnd) {
            a += delta; b += delta;
        } else {
            a = MIN(a, s);
            b = MAX(newEnd, b + delta);
        }
        a = MAX(0, MIN(a, length));
        b = MAX(a, MIN(b, length));
        MCDiagnosticMark *n = [MCDiagnosticMark new];
        n.range = NSMakeRange((NSUInteger)a, (NSUInteger)(b - a));
        n.severity = m.severity;
        n.message = m.message;
        [kept addObject:n];
    }
    _diagnostics = kept;
    _tooltipsDirty = YES;
    self.needsDisplay = YES;
}

- (NSString *)diagnosticMessageAtIndex:(NSUInteger)index {
    NSMutableArray *msgs = [NSMutableArray array];
    for (MCDiagnosticMark *m in _diagnostics)
        if (NSLocationInRange(index, m.range) ||
            (m.range.length == 0 && index == m.range.location))
            [msgs addObject:m.message];
    return msgs.count ? [msgs componentsJoinedByString:@"\n"] : nil;
}

// The characters on screen, so drawing never forces layout of a whole file.
- (NSRange)visibleCharacterRange {
    NSTextLayoutManager *tlm = self.textLayoutManager;
    if (tlm) {
        NSTextRange *vr = tlm.textViewportLayoutController.viewportRange;
        if (!vr) return NSMakeRange(0, self.string.length);
        NSTextContentManager *cm = tlm.textContentManager;
        NSInteger a = [cm offsetFromLocation:cm.documentRange.location toLocation:vr.location];
        NSInteger b = [cm offsetFromLocation:cm.documentRange.location
                                  toLocation:vr.endLocation];
        if (a < 0 || b < a) return NSMakeRange(0, self.string.length);
        return NSMakeRange((NSUInteger)a, (NSUInteger)(b - a));
    }
    NSLayoutManager *lm = self.layoutManager;
    NSRange glyphs = [lm glyphRangeForBoundingRect:self.visibleRect
                                   inTextContainer:self.textContainer];
    return [lm characterRangeForGlyphRange:glyphs actualGlyphRange:NULL];
}

// Line fragments covering a character range, in view coordinates. TextKit 2
// (the default) draws no underline from rendering attributes, so the marks
// are drawn by hand from these rects; TextKit 1 is handled in case something
// ever switches the view over.
- (void)enumerateRectsForRange:(NSRange)r block:(void (^)(NSRect rect))block {
    NSPoint origin = self.textContainerOrigin;
    NSTextLayoutManager *tlm = self.textLayoutManager;
    if (tlm) {
        NSTextContentManager *cm = tlm.textContentManager;
        id<NSTextLocation> start = [cm locationFromLocation:cm.documentRange.location
                                                 withOffset:(NSInteger)r.location];
        id<NSTextLocation> end = start ? [cm locationFromLocation:start
                                                       withOffset:(NSInteger)r.length]
                                       : nil;
        if (!start || !end) return;
        NSTextRange *tr = [[NSTextRange alloc] initWithLocation:start endLocation:end];
        if (!tr) return;
        [tlm enumerateTextSegmentsInRange:tr
                                     type:NSTextLayoutManagerSegmentTypeStandard
                                  options:NSTextLayoutManagerSegmentOptionsRangeNotRequired
                               usingBlock:^BOOL(NSTextRange *sr, CGRect frame,
                                                CGFloat baseline, NSTextContainer *c) {
            (void)sr; (void)baseline; (void)c;
            block(NSOffsetRect(frame, origin.x, origin.y));
            return YES;
        }];
        return;
    }
    NSLayoutManager *lm = self.layoutManager;
    NSRange gr = [lm glyphRangeForCharacterRange:r actualCharacterRange:NULL];
    [lm enumerateEnclosingRectsForGlyphRange:gr
                    withinSelectedGlyphRange:NSMakeRange(NSNotFound, 0)
                             inTextContainer:self.textContainer
                                  usingBlock:^(NSRect rect, BOOL *stop) {
        (void)stop;
        block(NSOffsetRect(rect, origin.x, origin.y));
    }];
}

static NSColor *SeverityColor(NSInteger severity) {
    switch (severity) {
    case 1: return LHex(0xF14C4C);   // error
    case 2: return LHex(0xCCA700);   // warning
    case 3: return LHex(0x3794FF);   // information
    default: return LHex(0x9CA3AF);  // hint
    }
}

- (void)drawRect:(NSRect)dirty {
    [super drawRect:dirty];
    if (_diagnostics.count == 0) return;
    NSRange visible = [self visibleCharacterRange];
    // Errors last, so they draw over a warning on the same text.
    NSArray *ordered = [_diagnostics sortedArrayUsingComparator:
        ^NSComparisonResult(MCDiagnosticMark *a, MCDiagnosticMark *b) {
            return a.severity > b.severity ? NSOrderedAscending
                 : a.severity < b.severity ? NSOrderedDescending : NSOrderedSame;
        }];
    for (MCDiagnosticMark *m in ordered) {
        NSRange r = m.range;
        if (NSMaxRange(r) < visible.location || r.location > NSMaxRange(visible)) continue;
        NSColor *color = SeverityColor(m.severity);
        [self enumerateRectsForRange:r block:^(NSRect rect) {
            if (!NSIntersectsRect(NSInsetRect(rect, 0, -3), dirty)) return;
            CGFloat width = MAX(rect.size.width, 6);
            // A squiggle along the bottom of the line fragment (the view is
            // flipped, so the bottom is maxY).
            CGFloat y = NSMaxY(rect) - 2, amp = 1.5, step = 2;
            NSBezierPath *path = [NSBezierPath bezierPath];
            path.lineWidth = 1;
            [path moveToPoint:NSMakePoint(rect.origin.x, y)];
            BOOL up = YES;
            for (CGFloat x = rect.origin.x + step; x <= rect.origin.x + width + 0.1; x += step) {
                [path lineToPoint:NSMakePoint(x, y + (up ? -amp : amp))];
                up = !up;
            }
            [color set];
            [path stroke];
        }];
    }
    if (_tooltipsDirty) {
        _tooltipsDirty = NO;
        dispatch_async(dispatch_get_main_queue(), ^{ [self rebuildTooltips]; });
    }
}

// One tooltip rect per visible diagnostic, rebuilt after anything moves them.
- (void)rebuildTooltips {
    [self removeAllToolTips];
    NSMutableArray<NSString *> *messages = [NSMutableArray array];
    NSRange visible = [self visibleCharacterRange];
    for (MCDiagnosticMark *m in _diagnostics) {
        NSRange r = m.range;
        if (NSMaxRange(r) < visible.location || r.location > NSMaxRange(visible)) continue;
        NSUInteger index = messages.count;
        [messages addObject:m.message ?: @""];
        [self enumerateRectsForRange:r block:^(NSRect rect) {
            rect.size.width = MAX(rect.size.width, 6);
            [self addToolTipRect:rect owner:self userData:(void *)index];
        }];
    }
    _tooltipMessages = messages;
}

- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag
             point:(NSPoint)point userData:(void *)data {
    (void)view; (void)tag;
    // Every diagnostic under the pointer, not just the one this rect is for.
    NSUInteger i = [self characterIndexForInsertionAtPoint:point];
    NSString *all = [self diagnosticMessageAtIndex:i];
    if (!all && i > 0) all = [self diagnosticMessageAtIndex:i - 1];
    if (all) return all;
    NSUInteger index = (NSUInteger)data;
    return index < _tooltipMessages.count ? _tooltipMessages[index] : @"";
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    _tooltipsDirty = YES;
}

- (void)mouseDown:(NSEvent *)event {
    NSEventModifierFlags mods = event.modifierFlags &
        NSEventModifierFlagDeviceIndependentFlagsMask;
    if (mods == NSEventModifierFlagCommand && self.onCommandClick) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        NSUInteger i = [self characterIndexForInsertionAtPoint:p];
        if (self.onCommandClick(i)) return;
    }
    [super mouseDown:event];
}
@end

// ------------------------------------------------------ MCCompletionPopup
// A borderless child window with a one-column table. It never becomes key:
// the text view keeps focus and forwards arrows, Return, Tab and Esc.
@interface MCCompletionPanel : NSPanel
@end
@implementation MCCompletionPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface MCCompletionPopup : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, copy) void (^onAccept)(NSInteger row);
@property(nonatomic, readonly) BOOL visible;
@property(nonatomic, readonly) NSArray<NSString *> *labels;
- (void)showLabels:(NSArray<NSString *> *)labels details:(NSArray<NSString *> *)details
         atScreenRect:(NSRect)caret parent:(NSWindow *)parent;
- (void)close;
- (void)moveSelection:(NSInteger)delta;
- (NSInteger)selectedRow;
@end

@implementation MCCompletionPopup {
    MCCompletionPanel *_panel;
    NSTableView *_table;
    NSArray<NSString *> *_details;
    __weak NSWindow *_parent;
}
static const CGFloat kRowHeight = 20, kPopupWidth = 420;
static const NSInteger kMaxRows = 10;

- (void)build {
    _panel = [[MCCompletionPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, kPopupWidth, 100)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered defer:YES];
    _panel.hasShadow = YES;
    _panel.releasedWhenClosed = NO;
    _panel.backgroundColor = LHex(0x252526);
    _panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _panel.becomesKeyOnlyIfNeeded = YES;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:_panel.contentView.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.wantsLayer = YES;
    scroll.layer.borderWidth = 1;
    scroll.layer.borderColor = LHex(0x454545).CGColor;

    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"item"];
    col.width = kPopupWidth - 4;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = kRowHeight;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.backgroundColor = LHex(0x252526);
    _table.style = NSTableViewStylePlain;
    _table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.action = @selector(clicked:);
    _table.refusesFirstResponder = YES;
    scroll.documentView = _table;
    [_panel.contentView addSubview:scroll];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_labels.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
    NSTextField *f = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!f) {
        f = [NSTextField labelWithString:@""];
        f.identifier = @"cell";
        f.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    NSFont *mono = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:[@" " stringByAppendingString:_labels[(NSUInteger)row]]
            attributes:@{NSFontAttributeName: mono,
                         NSForegroundColorAttributeName: LHex(0xD4D4D4)}];
    NSString *detail = (NSUInteger)row < _details.count ? _details[(NSUInteger)row] : @"";
    if (detail.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"   " stringByAppendingString:detail]
                attributes:@{NSFontAttributeName: mono,
                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
    f.attributedStringValue = s;
    return f;
}

- (void)clicked:(id)sender {
    (void)sender;
    NSInteger row = _table.clickedRow;
    if (row >= 0 && self.onAccept) self.onAccept(row);
}

- (void)showLabels:(NSArray<NSString *> *)labels details:(NSArray<NSString *> *)details
         atScreenRect:(NSRect)caret parent:(NSWindow *)parent {
    if (!_panel) [self build];
    NSString *previous = [self selectedRow] >= 0 && [self selectedRow] < (NSInteger)_labels.count
                             ? _labels[(NSUInteger)[self selectedRow]] : nil;
    _labels = [labels copy];
    _details = [details copy];
    [_table reloadData];
    // Keep the highlighted item while the list narrows, else the first.
    NSUInteger keep = previous ? [_labels indexOfObject:previous] : NSNotFound;
    NSInteger row = keep == NSNotFound ? 0 : (NSInteger)keep;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];

    CGFloat h = MIN((NSInteger)labels.count, kMaxRows) * kRowHeight + 2;
    NSRect frame = NSMakeRect(caret.origin.x - 6, caret.origin.y - h - 2, kPopupWidth, h);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    if (NSMinY(frame) < NSMinY(vis)) frame.origin.y = NSMaxY(caret) + 2;   // above
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - kPopupWidth;
    [_panel setFrame:frame display:YES];
    if (!_visible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
}

- (void)close {
    if (!_visible) return;
    _visible = NO;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
    _labels = @[];
}

- (NSInteger)selectedRow { return _table ? _table.selectedRow : -1; }

- (void)moveSelection:(NSInteger)delta {
    NSInteger n = (NSInteger)_labels.count;
    if (n == 0) return;
    NSInteger row = ([self selectedRow] + delta + n) % n;   // wraps around
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];
}
@end

// ------------------------------------------------------------ LspSession
@implementation LspSession {
    __weak CodeTextView *_tv;
    NSString *_root;
    NSMutableDictionary<NSString *, id> *_servers;   // key -> MCLspServer, or a note
    NSString *_path;          // the file in the view, even with no server
    NSString *_uri;           // its URI, when a server has it open
    MCLspServer *_server;     // the server for it, or nil
    NSString *_note;          // why there is no server, for the status bar
    NSString *_transient;     // a brief message ("No definition found")
    std::map<std::string, std::vector<Lsp::Diagnostic>> _diags;
    NSString *_settingsSignature;

    MCCompletionPopup *_popup;
    std::vector<Lsp::CompletionItem> _items, _shown;
    NSUInteger _anchor;       // where the word being completed starts
    BOOL _completing;         // a request is out or the list is showing
    int _completionGen, _definitionGen, _hoverGen;
    NSPopover *_hover;
}

- (instancetype)initWithTextView:(CodeTextView *)textView root:(NSString *)root {
    if (!(self = [super init])) return nil;
    _tv = textView;
    _root = [root copy];
    _servers = [NSMutableDictionary dictionary];
    _settingsSignature = [self currentSettingsSignature];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(storageEdited:)
               name:NSTextStorageDidProcessEditingNotification object:textView.textStorage];
    [nc addObserver:self selector:@selector(selectionChanged:)
               name:NSTextViewDidChangeSelectionNotification object:textView];
    [nc addObserver:self selector:@selector(settingsChanged:)
               name:MCSettingsDidChangeNotification object:nil];
    __weak LspSession *weakSelf = self;
    textView.onCommandClick = ^BOOL(NSUInteger index) {
        LspSession *s = weakSelf;
        if (!s || !s->_server) return NO;
        [s->_tv setSelectedRange:NSMakeRange(index, 0)];
        [s definitionAtIndex:index];
        return YES;
    };
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self shutdown];
}

// ---- status
- (void)updateStatus {
    NSString *s = @"";
    if (_transient) {
        s = _transient;
    } else if (_server) {
        if (_server.client.state() != Lsp::Client::State::Ready) {
            s = [NSString stringWithFormat:@"%@ starting…", _server.name];
        } else {
            int errors = 0, warnings = 0;
            auto it = _diags.find(Std(_uri));
            if (it != _diags.end())
                for (const Lsp::Diagnostic &d : it->second) {
                    if (d.severity == Lsp::Severity::Error) errors++;
                    else if (d.severity == Lsp::Severity::Warning) warnings++;
                }
            NSMutableArray *parts = [NSMutableArray array];
            if (errors) [parts addObject:[NSString stringWithFormat:@"%d error%@", errors,
                                          errors == 1 ? @"" : @"s"]];
            if (warnings) [parts addObject:[NSString stringWithFormat:@"%d warning%@",
                                            warnings, warnings == 1 ? @"" : @"s"]];
            s = [NSString stringWithFormat:@"%@: %@", _server.name,
                 parts.count ? [parts componentsJoinedByString:@", "] : @"no problems"];
        }
    } else if (_note) {
        s = _note;
    }
    _statusText = s;
    if (self.onStatus) self.onStatus(s);
}

- (void)flash:(NSString *)message {
    _transient = [message copy];
    [self updateStatus];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(endFlash)
                                               object:nil];
    [self performSelector:@selector(endFlash) withObject:nil afterDelay:3];
}
- (void)endFlash {
    _transient = nil;
    [self updateStatus];
}

// ---- servers
- (NSString *)currentSettingsSignature {
    const Settings &st = [AppSettings shared].settings;
    NSMutableString *sig = [NSMutableString stringWithFormat:@"%d", st.lspEnabled()];
    for (const std::string &k : Settings::lspServers())
        [sig appendFormat:@"|%s", st.lspCommand(k).c_str()];
    return sig;
}

- (MCLspServer *)serverForKey:(const std::string &)key {
    NSString *k = NS(key);
    id existing = _servers[k];
    if ([existing isKindOfClass:[MCLspServer class]]) return existing;
    if ([existing isKindOfClass:[NSString class]]) {
        _note = ((NSString *)existing).length ? existing : nil;
        return nil;
    }
    const Settings &st = [AppSettings shared].settings;
    if (st.lspOff(key)) {
        _servers[k] = @"";   // switched off on purpose: say nothing
        return nil;
    }
    std::vector<std::string> commands;
    if (!st.lspCommand(key).empty()) commands.push_back(st.lspCommand(key));
    else commands = Lsp::defaultCommands(key);
    MCLspServer *server = nil;
    for (const std::string &cmd : commands) {
        NSArray *argv = MCResolveCommand(cmd);
        if (!argv) continue;
        server = [[MCLspServer alloc] initWithKey:k argv:argv root:_root];
        if (server) break;
    }
    if (!server) {
        NSString *note = [NSString stringWithFormat:@"No %s language server found",
                          Lsp::serverDisplayName(key).c_str()];
        _servers[k] = note;
        _note = note;
        return nil;
    }
    __weak LspSession *weakSelf = self;
    __weak MCLspServer *weakServer = server;
    Lsp::Client &c = server.client;
    c.onReady = [weakSelf] { [weakSelf updateStatus]; };
    c.onDiagnostics = [weakSelf](const std::string &uri,
                                 const std::vector<Lsp::Diagnostic> &list) {
        [weakSelf diagnosticsArrived:uri list:list];
    };
    c.onProtocolError = [weakServer](const std::string &problem) {
        NSLog(@"MiniCode: %@: %s", weakServer.name, problem.c_str());
    };
    server.onExit = ^(MCLspServer *s) { [weakSelf serverExited:s]; };
    c.initialize(Std(_root), (int)getpid());
    _servers[k] = server;
    return server;
}

- (void)serverExited:(MCLspServer *)server {
    if (_servers[server.key] == server)
        _servers[server.key] = server.stopping ? nil
            : [NSString stringWithFormat:@"%@ stopped", server.name];
    if (_server == server) {
        _server = nil;
        _uri = nil;
        _note = server.stopping ? nil : _servers[server.key];
        _tv.diagnostics = @[];
        [self closeCompletion];
        [self updateStatus];
    }
}

- (void)stopAllServers {
    for (id s in _servers.allValues)
        if ([s isKindOfClass:[MCLspServer class]]) [(MCLspServer *)s stop];
    [_servers removeAllObjects];
    _diags.clear();
}

- (void)settingsChanged:(NSNotification *)note {
    (void)note;
    NSString *sig = [self currentSettingsSignature];
    if ([sig isEqualToString:_settingsSignature]) return;
    _settingsSignature = sig;
    NSString *path = _path;
    [self closeDocument];
    [self stopAllServers];
    [self documentOpened:path];
}

// ---- documents
- (void)closeDocument {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    [self closeCompletion];
    [_hover close];
    if (_server && _uri) {
        _server.client.didClose(Std(_uri));
        _diags.erase(Std(_uri));
    }
    _server = nil;
    _uri = nil;
    _path = nil;
    _note = nil;
    _tv.diagnostics = @[];
}

- (void)documentOpened:(NSString *)path {
    [self closeDocument];
    _path = [path copy];
    Lsp::Language lang;
    if (!path || ![AppSettings shared].settings.lspEnabled() ||
        !Lsp::languageForExtension(Std(path.pathExtension.lowercaseString), lang)) {
        [self updateStatus];
        return;
    }
    MCLspServer *server = [self serverForKey:lang.server];
    if (!server) { [self updateStatus]; return; }
    _server = server;
    _uri = NS(Lsp::uriFromPath(Std(RealPath(path))));
    server.client.didOpen(Std(_uri), lang.languageId, Std(_tv.string));
    [self updateStatus];
}

- (void)documentSaved {
    if (!_server || !_uri) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    _server.client.didSave(Std(_uri), Std(_tv.string));
}

- (void)setRoot:(NSString *)root {
    [self closeDocument];
    [self stopAllServers];
    _root = [root copy];
    [self updateStatus];
}

- (void)shutdown {
    [self closeDocument];
    [self stopAllServers];
}

// Full-document sync, a moment after typing pauses. Anything that asks the
// server about the text flushes first.
- (void)flushChanges {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    if (_server && _uri) _server.client.didChange(Std(_uri), Std(_tv.string));
}

- (void)storageEdited:(NSNotification *)note {
    NSTextStorage *ts = note.object;
    if (!(ts.editedMask & NSTextStorageEditedCharacters) || !_server || !_uri) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(flushChanges)
                                               object:nil];
    [self performSelector:@selector(flushChanges) withObject:nil afterDelay:0.3];

    // One character typed: is it the end of ".", "->" or "::"?
    NSRange er = ts.editedRange;
    if (ts.changeInLength == 1 && er.length == 1) {
        NSString *s = ts.string;
        unichar c = [s characterAtIndex:er.location];
        unichar prev = er.location > 0 ? [s characterAtIndex:er.location - 1] : 0;
        NSString *trigger = nil;
        if (c == '.') trigger = @".";
        else if (c == '>' && prev == '-') trigger = @">";
        else if (c == ':' && prev == ':') trigger = @":";
        if (trigger) {
            std::vector<std::string> triggers = _server.client.completionTriggers();
            bool allowed = triggers.empty() ||
                std::find(triggers.begin(), triggers.end(), Std(trigger)) != triggers.end();
            if (allowed) {
                // After the text view has finished the edit and moved the caret.
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self requestCompletion:trigger];
                });
                return;
            }
        }
    }
    if (_completing)
        dispatch_async(dispatch_get_main_queue(), ^{ [self refilterCompletion]; });
}

- (void)selectionChanged:(NSNotification *)note {
    (void)note;
    if (_completing)
        dispatch_async(dispatch_get_main_queue(), ^{ [self refilterCompletion]; });
}

// ---- diagnostics
- (BOOL)uri:(const std::string &)a isSameFileAs:(NSString *)b {
    if (!b) return NO;
    if (NS(a) && [NS(a) isEqualToString:b]) return YES;
    NSString *pa = NS(Lsp::pathFromUri(a)), *pb = NS(Lsp::pathFromUri(Std(b)));
    return pa.length && [RealPath(pa) isEqualToString:RealPath(pb)];
}

- (void)diagnosticsArrived:(const std::string &)uri
                      list:(const std::vector<Lsp::Diagnostic> &)list {
    if (![self uri:uri isSameFileAs:_uri]) {
        _diags[uri] = list;   // another file; shown if it is opened while cached
        return;
    }
    _diags[Std(_uri)] = list;
    std::u16string text = U16Of(_tv.string);
    NSMutableArray *marks = [NSMutableArray array];
    for (const Lsp::Diagnostic &d : list) {
        size_t a = Lsp::offsetForPosition(text, d.range.start);
        size_t b = Lsp::offsetForPosition(text, d.range.end);
        if (b <= a) {
            // An empty range: mark the word there, or at least one character.
            b = a;
            while (b < text.size() && IsIdentChar(text[b])) b++;
            if (b == a && a < text.size() && text[a] != u'\n') b = a + 1;
            else if (b == a && a > 0) a--;
        }
        MCDiagnosticMark *m = [MCDiagnosticMark new];
        m.range = NSMakeRange(a, b - a);
        m.severity = (NSInteger)d.severity;
        NSString *kind = d.severity == Lsp::Severity::Error ? @"Error"
                       : d.severity == Lsp::Severity::Warning ? @"Warning"
                       : d.severity == Lsp::Severity::Information ? @"Info" : @"Hint";
        m.message = [NSString stringWithFormat:@"%@: %@", kind, NS(d.message)];
        [marks addObject:m];
    }
    _tv.diagnostics = marks;
    [self updateStatus];
}

// ---- completion
- (NSUInteger)cursor {
    NSRange sel = _tv.selectedRange;
    return NSMaxRange(sel);
}

- (void)triggerCompletion {
    if (!_server) { NSBeep(); [self flash:_note ?: @"No language server for this file"]; return; }
    [self requestCompletion:nil];
}

- (void)requestCompletion:(NSString *)trigger {
    if (!_server || !_uri) return;
    [self flushChanges];
    NSString *s = _tv.string;
    NSUInteger cursor = [self cursor];
    NSUInteger anchor = cursor;
    while (anchor > 0 && IsIdentChar([s characterAtIndex:anchor - 1])) anchor--;
    _anchor = anchor;
    _completing = YES;
    int gen = ++_completionGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(s), cursor);
    _server.client.completion(Std(uri), pos,
        [weakSelf, gen, uri](const Json &result, const Json &error) {
            (void)error;
            [weakSelf completionArrived:result gen:gen uri:uri];
        }, trigger ? Std(trigger) : "");
}

- (void)completionArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri {
    if (gen != _completionGen || ![uri isEqualToString:_uri] || !_completing) return;
    _items = Lsp::parseCompletion(result);
    [self refilterCompletion];
}

- (void)refilterCompletion {
    if (!_completing) return;
    NSString *s = _tv.string;
    NSUInteger cursor = [self cursor];
    if (_tv.selectedRange.length > 0 || cursor < _anchor || cursor > s.length ||
        _tv.window.firstResponder != _tv) {
        [self closeCompletion];
        return;
    }
    for (NSUInteger i = _anchor; i < cursor; i++)
        if (!IsIdentChar([s characterAtIndex:i])) { [self closeCompletion]; return; }
    if (_items.empty()) return;   // still waiting for the server
    NSString *prefix = [s substringWithRange:NSMakeRange(_anchor, cursor - _anchor)];
    _shown = Lsp::filterCompletions(_items, Std(prefix));
    // Nothing to offer, or only exactly what is already typed.
    if (_shown.empty() ||
        (_shown.size() == 1 && _shown[0].textToInsert() == Std(prefix))) {
        [self closeCompletion];
        return;
    }
    if (!_popup) {
        _popup = [MCCompletionPopup new];
        __weak LspSession *weakSelf = self;
        _popup.onAccept = ^(NSInteger row) { [weakSelf acceptCompletion:row]; };
    }
    NSMutableArray *labels = [NSMutableArray array], *details = [NSMutableArray array];
    size_t limit = std::min<size_t>(_shown.size(), 200);
    for (size_t i = 0; i < limit; i++) {
        [labels addObject:NS(_shown[i].label)];
        [details addObject:NS(_shown[i].detail)];
    }
    NSRect caret = [_tv firstRectForCharacterRange:NSMakeRange(_anchor, 0) actualRange:NULL];
    [_popup showLabels:labels details:details atScreenRect:caret parent:_tv.window];
}

- (void)closeCompletion {
    _completing = NO;
    _items.clear();
    _shown.clear();
    [_popup close];
}

- (void)acceptCompletion:(NSInteger)row {
    if (row < 0 || (size_t)row >= _shown.size()) { [self closeCompletion]; return; }
    Lsp::CompletionItem item = _shown[(size_t)row];
    NSUInteger cursor = [self cursor];
    NSUInteger start = _anchor;
    if (item.hasEdit) {
        // The edit's range was computed on the text as it was when asked;
        // everything before the word is unchanged since, so its start holds.
        size_t editStart = Lsp::offsetForPosition(U16Of(_tv.string), item.editRange.start);
        if (editStart <= cursor) start = editStart;
    }
    [self closeCompletion];
    NSString *insert = NS(item.textToInsert());
    NSRange range = NSMakeRange(start, cursor - start);
    if (![_tv shouldChangeTextInRange:range replacementString:insert]) return;
    [_tv.textStorage replaceCharactersInRange:range withString:insert];
    [_tv didChangeText];
    [_tv setSelectedRange:NSMakeRange(start + insert.length, 0)];
    [_tv scrollRangeToVisible:_tv.selectedRange];
}

- (BOOL)handleCommand:(SEL)sel {
    if (sel == @selector(complete:)) {
        if (!_server) return NO;   // the text view's own word completion
        [self requestCompletion:nil];
        return YES;
    }
    if (!_popup.visible) return NO;
    if (sel == @selector(moveUp:)) { [_popup moveSelection:-1]; return YES; }
    if (sel == @selector(moveDown:)) { [_popup moveSelection:1]; return YES; }
    if (sel == @selector(insertNewline:) || sel == @selector(insertTab:)) {
        [self acceptCompletion:_popup.selectedRow];
        return YES;
    }
    if (sel == @selector(cancelOperation:)) { [self closeCompletion]; return YES; }
    return NO;
}

- (BOOL)completionVisible { return _popup.visible; }
- (NSArray<NSString *> *)completionLabels { return _popup.visible ? _popup.labels : @[]; }

// ---- go to definition
- (void)goToDefinition {
    if (!_server) { NSBeep(); [self flash:_note ?: @"No language server for this file"]; return; }
    [self definitionAtIndex:_tv.selectedRange.location];
}

- (void)definitionAtIndex:(NSUInteger)index {
    if (!_server || !_uri) return;
    [self flushChanges];
    int gen = ++_definitionGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    _server.client.definition(Std(uri), pos, [weakSelf, gen, uri](const Json &r, const Json &e) {
        (void)e;
        [weakSelf definitionArrived:r gen:gen uri:uri];
    });
}

- (void)definitionArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri {
    if (gen != _definitionGen || ![uri isEqualToString:_uri]) return;
    std::vector<Lsp::Location> locs = Lsp::parseLocations(result);
    if (locs.empty()) { NSBeep(); [self flash:@"No definition found"]; return; }
    Lsp::Location loc = locs[0];
    NSString *target = NS(Lsp::pathFromUri(loc.uri));
    if (!target.length) return;
    if (![self uri:loc.uri isSameFileAs:_uri]) {
        // Open it under the tree's own spelling of the path when it is inside
        // the folder, so the tree can select it.
        NSString *real = RealPath(target), *rootReal = RealPath(_root);
        NSString *path = target;
        if (rootReal.length && [real hasPrefix:[rootReal stringByAppendingString:@"/"]])
            path = [_root stringByAppendingPathComponent:
                        [real substringFromIndex:rootReal.length + 1]];
        if (!self.openFile || !self.openFile(path)) return;
    }
    std::u16string text = U16Of(_tv.string);
    size_t a = Lsp::offsetForPosition(text, loc.range.start);
    size_t b = Lsp::offsetForPosition(text, loc.range.end);
    NSRange r = NSMakeRange(a, b >= a ? b - a : 0);
    [_tv.window makeFirstResponder:_tv];
    [_tv setSelectedRange:r];
    [_tv scrollRangeToVisible:r];
    [_tv showFindIndicatorForRange:r];
}

// ---- hover
- (void)showHoverInfo {
    if (!_server || !_uri) { NSBeep(); [self flash:_note ?: @"No language server for this file"]; return; }
    [self flushChanges];
    NSUInteger index = _tv.selectedRange.location;
    int gen = ++_hoverGen;
    __weak LspSession *weakSelf = self;
    NSString *uri = _uri;
    Lsp::Position pos = Lsp::positionForOffset(U16Of(_tv.string), index);
    _server.client.hover(Std(uri), pos, [weakSelf, gen, uri, index](const Json &r, const Json &e) {
        (void)e;
        [weakSelf hoverArrived:r gen:gen uri:uri index:index];
    });
}

- (void)hoverArrived:(const Json &)result gen:(int)gen uri:(NSString *)uri
               index:(NSUInteger)index {
    if (gen != _hoverGen || ![uri isEqualToString:_uri]) return;
    NSString *text = NS(Lsp::parseHover(result));
    NSString *diag = [_tv diagnosticMessageAtIndex:index];
    if (diag) text = text.length ? [NSString stringWithFormat:@"%@\n\n%@", diag, text] : diag;
    _lastHoverText = text;
    if (!text.length) { [self flash:@"No information here"]; return; }

    NSTextField *label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    label.textColor = LHex(0xD4D4D4);
    label.selectable = YES;
    NSSize fit = [label sizeThatFits:NSMakeSize(520, CGFLOAT_MAX)];
    fit.width = MIN(ceil(fit.width), 520);
    fit.height = MIN(ceil(fit.height), 360);
    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, fit.width + 20, fit.height + 16)];
    label.frame = NSMakeRect(10, 8, fit.width, fit.height);
    [box addSubview:label];
    NSViewController *vc = [NSViewController new];
    vc.view = box;

    [_hover close];
    _hover = [NSPopover new];
    _hover.behavior = NSPopoverBehaviorTransient;
    _hover.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _hover.contentViewController = vc;
    NSRect screen = [_tv firstRectForCharacterRange:NSMakeRange(index, 0) actualRange:NULL];
    NSRect inWindow = [_tv.window convertRectFromScreen:screen];
    NSRect inView = [_tv convertRect:inWindow fromView:nil];
    inView.size.width = MAX(inView.size.width, 1);
    [_hover showRelativeToRect:inView ofView:_tv preferredEdge:NSRectEdgeMaxY];
}
@end
