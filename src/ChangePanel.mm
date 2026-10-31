// ChangePanel.mm — see ChangePanel.h.
#import "ChangePanel.h"
#import "AppSettings.h"
#import "Terminal.h"
#import "TreeDetails.h"   // MCAppDefaults
#include "ChangeRequest.h"

#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

static std::string Std(NSString *s) { return s.UTF8String ?: ""; }
static NSString *NS(const std::string &s) { return @(s.c_str()) ?: @""; }

static NSColor *CHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1];
}

// ------------------------------------------------------------ the bar
const CGFloat kMCChangeBarHeight = 32;

@implementation MCChangeBar {
    NSTextField *_label;
    NSTextField *_field;
    NSButton *_action, *_close;
    NSView *_line;
    NSColor *_textColor;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    _label = [NSTextField labelWithString:@""];
    _label.font = [NSFont systemFontOfSize:12];
    _label.lineBreakMode = NSLineBreakByTruncatingTail;
    _field = [NSTextField textFieldWithString:@""];
    _field.font = [NSFont systemFontOfSize:12];
    _field.placeholderString = @"Describe a change to minicode, then press Return";
    _field.bezelStyle = NSTextFieldRoundedBezel;
    _field.focusRingType = NSFocusRingTypeNone;
    _field.delegate = self;
    _field.cell.scrollable = YES;
    _field.cell.wraps = NO;
    _action = [NSButton buttonWithTitle:@"Send" target:self action:@selector(act:)];
    _action.controlSize = NSControlSizeSmall;
    _action.font = [NSFont systemFontOfSize:11];
    _action.bezelStyle = NSBezelStyleRounded;
    _close = [NSButton buttonWithTitle:@"×" target:self action:@selector(close:)];
    _close.bordered = NO;
    _line = [NSView new];
    _line.wantsLayer = YES;
    _line.layer.backgroundColor = CHex(0x333333).CGColor;
    for (NSView *v in @[_label, _field, _action, _close, _line]) [self addSubview:v];
    [self setBackground:CHex(0x252526) text:CHex(0xCCCCCC)];
    self.mode = MCChangeBarPrompt;
    return self;
}

- (NSTextField *)field { return _field; }
- (NSButton *)actionButton { return _action; }

- (void)setMode:(MCChangeBarMode)mode {
    _mode = mode;
    _field.hidden = mode != MCChangeBarPrompt;
    _action.hidden = mode == MCChangeBarNote;
    _action.title = mode == MCChangeBarPrompt ? @"Send"
                  : mode == MCChangeBarWorking ? @"Show Terminal" : @"Restart";
    if (mode == MCChangeBarPrompt) {
        _label.stringValue = @"Change minicode:";
        _close.toolTip = @"Close (Esc)";
    } else {
        _label.stringValue = _message ?: @"";
        _close.toolTip = @"Close";
    }
    _label.toolTip = mode == MCChangeBarPrompt ? nil : _message;
    self.needsLayout = YES;
}

- (void)setMessage:(NSString *)message {
    _message = [message copy];
    self.mode = _mode;   // the label again
}

- (void)setBackground:(NSColor *)background text:(NSColor *)text {
    self.layer.backgroundColor = background.CGColor;
    _textColor = text;
    _label.textColor = text;
    _close.attributedTitle = [[NSAttributedString alloc] initWithString:@"×"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:15],
                     NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.7]}];
}

- (void)layout {
    [super layout];
    const NSRect b = self.bounds;
    const CGFloat H = NSHeight(b);
    CGFloat x = NSMaxX(b) - 8;
    [_close sizeToFit];
    NSSize c = _close.frame.size;
    x -= c.width;
    _close.frame = NSMakeRect(x, round((H - c.height) / 2), c.width, c.height);
    if (!_action.hidden) {
        [_action sizeToFit];
        NSSize a = _action.frame.size;
        x -= a.width + 8;
        _action.frame = NSMakeRect(x, round((H - a.height) / 2), a.width, a.height);
    }
    const CGFloat lh = 16;
    if (_mode == MCChangeBarPrompt) {
        [_label sizeToFit];
        const CGFloat lw = ceil(_label.frame.size.width);
        _label.frame = NSMakeRect(12, round((H - lh) / 2), lw, lh);
        const CGFloat fx = 12 + lw + 8, fh = 22;
        _field.frame = NSMakeRect(fx, round((H - fh) / 2), MAX(40, x - 8 - fx), fh);
    } else {
        _label.frame = NSMakeRect(12, round((H - lh) / 2), MAX(0, x - 20), lh);
    }
    _line.frame = NSMakeRect(0, 0, NSWidth(b), 1);
}

- (void)act:(id)sender {
    (void)sender;
    if (_mode == MCChangeBarPrompt) {
        if (self.onSend) self.onSend(_field.stringValue);
    } else if (self.onAction) {
        self.onAction();
    }
}
- (void)close:(id)sender { (void)sender; if (self.onClose) self.onClose(); }

// Return sends, Esc closes.
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)control; (void)tv;
    if (sel == @selector(insertNewline:)) { [self act:nil]; return YES; }
    if (sel == @selector(cancelOperation:)) { [self close:nil]; return YES; }
    return NO;
}
@end

// ------------------------------------------------------------ the source
static NSString *const kChangeSourceKey = @"MCChangeSource";

static BOOL MCLooksLikeSource(NSString *dir) {
    NSFileManager *fm = [NSFileManager defaultManager];
    return ChangeRequest::looksLikeSource(Std(dir), [fm](const std::string &p) {
        return (bool)[fm fileExistsAtPath:NS(p)];
    });
}

NSString *MCChangeSourceFolder(NSString **problem) {
    std::string set = [AppSettings shared].settings.developSource();
    if (!set.empty()) {
        NSString *dir = NS(set).stringByExpandingTildeInPath.stringByStandardizingPath;
        if (MCLooksLikeSource(dir)) return dir;
        if (problem)
            *problem = [NSString stringWithFormat:
                @"develop.source in the settings is %@, which is not minicode's source folder.",
                NS(set)];
        return nil;
    }
    NSString *built = NS(ChangeRequest::checkoutOfBundle(Std(NSBundle.mainBundle.bundlePath)));
    if (built.length && MCLooksLikeSource(built)) return built;
    NSString *chosen = [MCAppDefaults() stringForKey:kChangeSourceKey];
    if (chosen.length && MCLooksLikeSource(chosen)) return chosen;
    return nil;
}

void MCSetChangeSourceFolder(NSString *path) {
    [MCAppDefaults() setObject:path forKey:kChangeSourceKey];
}

// ------------------------------------------------------------ restarting
static NSString *const kRestartKey = @"MCRestart";

void MCSaveRestartRecord(NSDictionary *record) {
    NSMutableDictionary *r = [record mutableCopy];
    r[@"at"] = @([NSDate date].timeIntervalSince1970);
    [MCAppDefaults() setObject:r forKey:kRestartKey];
    [MCAppDefaults() synchronize];
}

NSDictionary *MCTakeRestartRecord(void) {
    NSUserDefaults *d = MCAppDefaults();
    NSDictionary *r = [d dictionaryForKey:kRestartKey];
    if (!r) return nil;
    [d removeObjectForKey:kRestartKey];
    [d synchronize];
    const double age = [NSDate date].timeIntervalSince1970 - [r[@"at"] doubleValue];
    return age >= 0 && age < 120 ? r : nil;
}

BOOL MCLaunchAfterExit(NSString *appPath) {
    // The shell outlives this process (its own session), waits for it to
    // go, then opens the app; `open -n` so it never just reactivates a
    // MiniCode still on its way out.
    NSString *pid = [NSString stringWithFormat:@"%d", getpid()];
    const char *script =
        "while kill -0 \"$1\" 2>/dev/null; do sleep 0.1; done; exec /usr/bin/open -n \"$2\"";
    const char *argv[] = {"/bin/sh", "-c", script, "minicode-restart", pid.UTF8String,
                          appPath.fileSystemRepresentation, NULL};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t child = 0;
    const int rc = posix_spawn(&child, "/bin/sh", &fa, &attr, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    return rc == 0;
}

// ------------------------------------------------------------ the session
static double MCFileTime(NSString *path) {
    struct stat st;
    if (stat(path.fileSystemRepresentation, &st) != 0) return 0;
    return (double)st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9;
}

@implementation MCChangeSession {
    __weak id<MCChangeHost> _host;
    MCChangeBar *_bar;
    BOOL _shown;
    NSTimer *_timer;
    double _baseline;    // the build's stamp when watching began, or after a restart was declined
    double _seen;        // the stamp at the last look, to see it settle
    NSString *_request;
    __weak NSResponder *_before;   // what had the keyboard before the prompt
}

- (instancetype)initWithHost:(id<MCChangeHost>)host {
    if ((self = [super init])) _host = host;
    return self;
}

- (MCChangeBar *)bar {
    if (!_bar) {
        _bar = [[MCChangeBar alloc] initWithFrame:NSZeroRect];
        __weak MCChangeSession *weakSelf = self;
        _bar.onSend = ^(NSString *r) { [weakSelf send:r]; };
        _bar.onAction = ^{ [weakSelf barAction]; };
        _bar.onClose = ^{ [weakSelf closeBar]; };
    }
    return _bar;
}

- (BOOL)barShown { return _shown; }

- (void)showBar:(MCChangeBarMode)mode message:(NSString *)message {
    MCChangeBar *bar = self.bar;
    bar.message = message;
    bar.mode = mode;
    if (!_shown) {
        _shown = YES;
        [_host changeBarDidChange];
    }
    [bar layout];
}

- (void)closeBar {
    if (!_shown) return;
    const BOOL prompt = _bar.mode == MCChangeBarPrompt;
    if (_bar.mode == MCChangeBarRebuilt) _baseline = _seen;   // not this build; the next one
    _shown = NO;
    [_host changeBarDidChange];
    if (prompt) {
        NSWindow *w = _host.window;
        NSResponder *back = _before;
        if (back && w) [w makeFirstResponder:back];
    }
}

- (BOOL)claudeRunning {
    TerminalView *t = _terminal;
    return t && [_host terminalIsOpen:t] && t.busy;
}

- (void)begin {
    if ([self claudeRunning]) {
        [_host focusChangeTerminal:_terminal];
        [_host showStatusNote:@"Claude is still on the last request: type in its terminal"];
        return;
    }
    NSWindow *w = _host.window;
    NSResponder *fr = w.firstResponder;
    if (fr != _bar.field.currentEditor && fr != _bar.field) _before = fr;
    [self showBar:MCChangeBarPrompt message:nil];
    [w makeFirstResponder:_bar.field];
}

- (void)send:(NSString *)request {
    NSString *trimmed = [request stringByTrimmingCharactersInSet:
                         NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!trimmed.length) { NSBeep(); return; }
    NSString *problem = nil;
    NSString *source = MCChangeSourceFolder(&problem);
    if (!source && problem) {
        [self showBar:MCChangeBarNote message:problem];
        return;
    }
    if (!source) {
        [self chooseSourceThen:^(NSString *dir) { [self send:trimmed source:dir]; }];
        return;
    }
    [self send:trimmed source:source];
}

// The open panel, as a sheet, until a folder holding minicode's source is
// chosen or the user cancels.
- (void)chooseSourceThen:(void (^)(NSString *dir))then {
    NSWindow *w = _host.window;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.allowsMultipleSelection = NO;
    p.prompt = @"Use This Folder";
    p.message = @"Choose minicode's source folder, a clone of github.com/e-c-hansen/minicode. "
                @"Claude Code makes the change there and rebuilds it.";
    __weak MCChangeSession *weakSelf = self;
    [p beginSheetModalForWindow:w completionHandler:^(NSModalResponse r) {
        MCChangeSession *s = weakSelf;
        if (!s || r != NSModalResponseOK || !p.URL) return;
        NSString *dir = p.URL.path;
        if (!MCLooksLikeSource(dir)) {
            [s showBar:MCChangeBarNote message:[NSString stringWithFormat:
                @"%@ is not minicode's source folder (it has no src/EditorController.mm).",
                dir.stringByAbbreviatingWithTildeInPath]];
            return;
        }
        MCSetChangeSourceFolder(dir);
        then(dir);
    }];
}

- (void)send:(NSString *)request source:(NSString *)source {
    ChangeRequest::Context c;
    c.request = Std(request);
    c.version = Std(NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"] ?: @"");
    c.appPath = Std(NSBundle.mainBundle.bundlePath);
    const std::string prompt = ChangeRequest::prompt(c);
    const std::string command = [AppSettings shared].settings.developCommand();
    // The prompt in a file the command reads, so the typed line stays short.
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"MiniCode-change"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    static NSUInteger serial;
    NSString *file = [dir stringByAppendingPathComponent:[NSString stringWithFormat:
        @"request-%d-%lu.txt", getpid(), (unsigned long)++serial]];
    if (![NS(prompt) writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
        [self showBar:MCChangeBarNote message:@"The request could not be written to a file in "
                                              @"the temporary folder."];
        return;
    }
    NSString *line = NS(ChangeRequest::startCommand(command, Std(source), Std(file)));
    _source = [source copy];
    _request = [request copy];
    _bar.field.stringValue = @"";
    TerminalView *t = _terminal;
    if (t && [_host terminalIsOpen:t] && !t.busy) {
        [_host runCommand:line inTerminal:t focus:YES];
    } else {
        _terminal = [_host changeTerminalRunning:line focus:YES];
    }
    [self watchFrom:MCFileTime(NS(ChangeRequest::buildStamp(Std(source))))];
    NSString *shortReq = request.length > 80
        ? [[request substringToIndex:80] stringByAppendingString:@"…"] : request;
    [self showBar:MCChangeBarWorking message:[NSString stringWithFormat:
        @"Claude is working on “%@” in the terminal. minicode offers a restart once it is rebuilt.",
        shortReq]];
}

- (void)resumeWithSource:(NSString *)source command:(NSString *)command {
    _source = [source copy];
    NSString *line = NS(ChangeRequest::resumeCommand(
        command.length ? Std(command) : [AppSettings shared].settings.developCommand(),
        Std(source)));
    _terminal = [_host changeTerminalRunning:line focus:NO];
    [self watchFrom:MCFileTime(NS(ChangeRequest::buildStamp(Std(source))))];
    [self showBar:MCChangeBarNote message:
        @"Restarted into the new build. The conversation with Claude goes on in the terminal."];
}

- (void)watchFrom:(double)stamp {
    _baseline = stamp;
    _seen = stamp;
    if (_timer) return;
    __weak MCChangeSession *weakSelf = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf checkBuild];
    }];
    _timer.tolerance = 0.5;
}

- (void)checkBuild {
    if (!_source) return;
    const double now = MCFileTime(NS(ChangeRequest::buildStamp(Std(_source))));
    const double before = _seen;
    _seen = now;
    // Newer than the baseline and unchanged since the last look: codesign,
    // the last step of `make`, has finished.
    if (now > _baseline && now == before && !(_shown && _bar.mode == MCChangeBarRebuilt)) {
        NSString *app = NS(ChangeRequest::builtApp(Std(_source)));
        [self showBar:MCChangeBarRebuilt
              message:@"The new build of minicode is ready. Restart to try the change."];
        _bar.actionButton.toolTip = [NSString stringWithFormat:
            @"Quit and open %@, with every window's folder and the conversation with Claude",
            app.stringByAbbreviatingWithTildeInPath];
    }
    // Nothing more to wait for once Claude's terminal is gone.
    TerminalView *t = _terminal;
    if ((!t || ![_host terminalIsOpen:t]) && !(_shown && _bar.mode == MCChangeBarRebuilt)) {
        [_timer invalidate];
        _timer = nil;
        if (_shown && _bar.mode == MCChangeBarWorking) [self closeBar];
    }
}

- (void)barAction {
    if (_bar.mode == MCChangeBarWorking) {
        TerminalView *t = _terminal;
        if (t && [_host terminalIsOpen:t]) [_host focusChangeTerminal:t];
        else NSBeep();
    } else if (_bar.mode == MCChangeBarRebuilt) {
        [self restart];
    }
}

- (void)restart {
    if (!_source) return;
    NSString *app = NS(ChangeRequest::builtApp(Std(_source)));
    NSString *command = NS([AppSettings shared].settings.developCommand());
    [_host restartIntoApp:app source:_source command:command];
}

- (void)stop {
    [_timer invalidate];
    _timer = nil;
}

- (void)dealloc { [_timer invalidate]; }
@end
