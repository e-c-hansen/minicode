// Terminal.mm — lightweight command runner. Objective-C++.
// Deliberately NOT a full terminal: no pty, no cursor addressing. Each command
// runs in its own zsh; `cd` and `clear` are handled locally so the panel feels
// like a shell for quick tasks.
#import "Terminal.h"

static NSColor *THex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8)  & 0xFF) / 255.0
                                blue:( rgb        & 0xFF) / 255.0
                               alpha:1.0];
}

// Read-only output view. A plain click focuses the input line; a drag still
// selects text for copying.
@interface TerminalOutputView : NSTextView
@property(nonatomic, copy) void (^onPlainClick)(void);
@end
@implementation TerminalOutputView
- (void)mouseDown:(NSEvent *)event {
    [super mouseDown:event];   // runs selection tracking until mouseUp
    if (self.selectedRange.length == 0 && self.onPlainClick) self.onPlainClick();
}
@end

@interface TerminalView () <NSTextFieldDelegate> {
    NSString *_cwd;
    NSTask   *_running;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyIdx;
    NSDictionary<NSString *, NSString *> *_shellEnv;  // captured login env
}
@property(nonatomic, strong) NSScrollView *outScroll;
@property(nonatomic, strong) TerminalOutputView *output;
@property(nonatomic, strong) NSTextField  *prompt;
@property(nonatomic, strong) NSTextField  *input;
@end

@implementation TerminalView

- (instancetype)initWithDirectory:(NSString *)dir {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 400, 220)])) {
        _cwd = dir.length ? dir : NSHomeDirectory();
        _history = [NSMutableArray array];
        _historyIdx = 0;
        // Seed with our own env immediately; upgrade to the full login env async.
        _shellEnv = [[NSProcessInfo processInfo] environment];
        self.wantsLayer = YES;
        self.layer.backgroundColor = THex(0x181818).CGColor;
        [self buildViews];
        [self appendLine:@"MiniCode terminal — quick command runner. "
                          "Type `clear` to reset."
                   color:THex(0x6A9955)];
        [self updatePrompt];
        [self captureLoginEnvironment];
    }
    return self;
}

- (void)buildViews {
    // Output (read-only, monospaced, selectable).
    self.outScroll = [[NSScrollView alloc] init];
    self.outScroll.hasVerticalScroller = YES;
    self.outScroll.drawsBackground = NO;
    self.output = [[TerminalOutputView alloc] initWithFrame:self.bounds];
    __weak TerminalView *weakSelf = self;
    self.output.onPlainClick = ^{ [weakSelf focusInput]; };
    self.output.editable = NO;
    self.output.selectable = YES;
    self.output.richText = YES;
    self.output.drawsBackground = NO;
    self.output.textContainerInset = NSMakeSize(6, 6);
    self.output.verticallyResizable = YES;
    self.output.horizontallyResizable = NO;
    self.output.textContainer.widthTracksTextView = YES;
    self.output.autoresizingMask = NSViewWidthSizable;
    self.outScroll.documentView = self.output;
    [self addSubview:self.outScroll];

    // Prompt label + input field.
    self.prompt = [NSTextField labelWithString:@"❯"];
    self.prompt.font = [NSFont monospacedSystemFontOfSize:12
                                                   weight:NSFontWeightBold];
    self.prompt.textColor = THex(0x4EC9B0);
    self.prompt.backgroundColor = [NSColor clearColor];
    [self addSubview:self.prompt];

    self.input = [[NSTextField alloc] init];
    self.input.font = [NSFont monospacedSystemFontOfSize:12
                                                  weight:NSFontWeightRegular];
    self.input.textColor = THex(0xEDEDED);
    self.input.backgroundColor = THex(0x232323);
    self.input.bezeled = NO;
    self.input.focusRingType = NSFocusRingTypeNone;
    self.input.bordered = NO;
    self.input.drawsBackground = YES;
    self.input.delegate = self;
    self.input.target = self;
    self.input.action = @selector(inputEntered:);
    self.input.placeholderString = @"Enter a command…";
    [self addSubview:self.input];
}

// Manual layout — reliable regardless of how the host sizes us.
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self layoutChildren];
}

- (void)layoutChildren {
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    const CGFloat inH = 26, promptW = 20;
    self.outScroll.frame = NSMakeRect(0, inH, W, MAX(0, H - inH));
    self.prompt.frame = NSMakeRect(6, 4, promptW, 18);
    self.input.frame  = NSMakeRect(6 + promptW, 2, MAX(0, W - promptW - 12), 22);
}

- (void)focusInput { [self.window makeFirstResponder:self.input]; }

// Clicks on the terminal's own background (padding/gaps) also focus input.
- (void)mouseDown:(NSEvent *)event {
    [super mouseDown:event];
    [self focusInput];
}

- (void)setDirectory:(NSString *)dir {
    if (dir.length) { _cwd = dir; [self updatePrompt]; }
}

- (void)updatePrompt {
    self.prompt.toolTip = _cwd;
}

// ---------------------------------------------------------------- input
- (void)inputEntered:(id)sender {
    NSString *cmd = self.input.stringValue;
    self.input.stringValue = @"";
    if (cmd.length) {
        [_history addObject:cmd];
        _historyIdx = _history.count;
    }
    [self runCommand:cmd];
}

// Up/Down arrow -> command history.
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv
    doCommandBySelector:(SEL)sel {
    if (sel == @selector(moveUp:)) {
        if (_historyIdx > 0) {
            _historyIdx--;
            self.input.stringValue = _history[_historyIdx];
        }
        return YES;
    }
    if (sel == @selector(moveDown:)) {
        if (_historyIdx < (NSInteger)_history.count - 1) {
            _historyIdx++;
            self.input.stringValue = _history[_historyIdx];
        } else {
            _historyIdx = _history.count;
            self.input.stringValue = @"";
        }
        return YES;
    }
    return NO;
}

// ---------------------------------------------------------------- running
- (void)runCommand:(NSString *)cmd {
    NSString *abbrev = [_cwd stringByAbbreviatingWithTildeInPath];
    [self appendLine:[NSString stringWithFormat:@"%@ ❯ %@", abbrev, cmd]
               color:THex(0x569CD6)];

    NSString *trimmed = [cmd stringByTrimmingCharactersInSet:
                         [NSCharacterSet whitespaceCharacterSet]];
    if (trimmed.length == 0) return;
    if ([trimmed isEqualToString:@"clear"]) {
        self.output.string = @"";
        return;
    }
    if ([trimmed isEqualToString:@"cd"] || [trimmed hasPrefix:@"cd "]) {
        [self changeDirectory:trimmed];
        return;
    }

    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/zsh"];
    task.arguments = @[@"-c", cmd];            // non-login: no per-command startup tax
    task.environment = _shellEnv;              // full login env, captured once
    task.currentDirectoryURL = [NSURL fileURLWithPath:_cwd];
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;
    _running = task;
    self.input.enabled = NO;

    __weak TerminalView *weakSelf = self;
    NSFileHandle *rh = pipe.fileHandleForReading;
    rh.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData;
        if (!d.length) return;
        NSString *chunk = [[NSString alloc] initWithData:d
                                                encoding:NSUTF8StringEncoding];
        if (!chunk) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf appendANSI:chunk];
        });
    };
    task.terminationHandler = ^(NSTask *finished) {
        (void)finished;
        dispatch_async(dispatch_get_main_queue(), ^{
            TerminalView *strong = weakSelf;
            if (!strong) return;
            rh.readabilityHandler = nil;
            strong.input.enabled = YES;
            strong->_running = nil;
            [strong focusInput];
        });
    };

    NSError *err = nil;
    if (![task launchAndReturnError:&err]) {
        [self appendLine:err.localizedDescription color:THex(0xF48771)];
        self.input.enabled = YES;
        _running = nil;
    }
}

- (void)changeDirectory:(NSString *)cmd {
    NSString *arg = @"";
    if ([cmd hasPrefix:@"cd "])
        arg = [[cmd substringFromIndex:3] stringByTrimmingCharactersInSet:
               [NSCharacterSet whitespaceCharacterSet]];
    NSString *target;
    if (arg.length == 0 || [arg isEqualToString:@"~"]) {
        target = NSHomeDirectory();
    } else if ([arg hasPrefix:@"/"]) {
        target = arg;
    } else if ([arg hasPrefix:@"~"]) {
        target = [arg stringByExpandingTildeInPath];
    } else {
        target = [_cwd stringByAppendingPathComponent:arg];
    }
    target = [target stringByStandardizingPath];
    BOOL isDir = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:target
                                             isDirectory:&isDir] && isDir) {
        _cwd = target;
        [self updatePrompt];
    } else {
        [self appendLine:[NSString stringWithFormat:@"cd: no such directory: %@",
                          arg] color:THex(0xF48771)];
    }
}

// One-time capture of the login-shell environment (PATH, etc.) on a background
// queue, so subsequent commands can use plain `zsh -c` and stay instant.
- (void)captureLoginEnvironment {
    __weak TerminalView *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSTask *t = [[NSTask alloc] init];
        t.executableURL = [NSURL fileURLWithPath:@"/bin/zsh"];
        t.arguments = @[@"-lc", @"env"];
        NSPipe *p = [NSPipe pipe];
        t.standardOutput = p;
        t.standardError = [NSPipe pipe];
        if (![t launchAndReturnError:nil]) return;
        NSData *d = [p.fileHandleForReading readDataToEndOfFile];
        [t waitUntilExit];
        NSString *s = [[NSString alloc] initWithData:d
                                            encoding:NSUTF8StringEncoding];
        NSMutableDictionary *env = [NSMutableDictionary dictionary];
        for (NSString *line in [s componentsSeparatedByString:@"\n"]) {
            NSRange eq = [line rangeOfString:@"="];
            if (eq.location != NSNotFound && eq.location > 0) {
                env[[line substringToIndex:eq.location]] =
                    [line substringFromIndex:eq.location + 1];
            }
        }
        if (env.count == 0) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            TerminalView *strong = weakSelf;
            if (strong) strong->_shellEnv = env;
        });
    });
}

// ---------------------------------------------------------------- output
// Strip ANSI escape sequences; keep the plain text. (Color SGR could be parsed
// here later — for now we favor clean, readable output.)
- (void)appendANSI:(NSString *)text {
    static NSRegularExpression *re = nil;
    if (!re)
        re = [NSRegularExpression
              regularExpressionWithPattern:@"\x1b\\[[0-9;?]*[ -/]*[@-~]"
                                   options:0 error:nil];
    NSString *clean = [re stringByReplacingMatchesInString:text options:0
                            range:NSMakeRange(0, text.length) withTemplate:@""];
    [self append:clean color:THex(0xD4D4D4)];
}

- (void)appendLine:(NSString *)line color:(NSColor *)color {
    [self append:[line stringByAppendingString:@"\n"] color:color];
}

- (void)append:(NSString *)text color:(NSColor *)color {
    NSDictionary *attrs = @{
        NSFontAttributeName:
            [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: color,
    };
    [self.output.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:text attributes:attrs]];
    [self.output scrollRangeToVisible:
        NSMakeRange(self.output.textStorage.length, 0)];
}

@end
