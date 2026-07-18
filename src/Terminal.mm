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

// Unique marker printed after each command so we know when its output ends,
// and can read back the exit status and the working directory.
static NSString *const kSentinel = @"__MC_DONE_a1b2c3d4__";

@interface TerminalView () <NSTextFieldDelegate> {
    NSString *_cwd;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyIdx;

    NSTask *_shell;              // one long-lived login shell
    NSFileHandle *_writeHandle;  // its stdin
    NSMutableString *_pending;   // output not yet scanned for the sentinel
    BOOL _running;               // a command is in flight
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
        _pending = [NSMutableString string];
        self.wantsLayer = YES;
        self.layer.backgroundColor = THex(0x181818).CGColor;
        [self buildViews];
        [self appendLine:@"MiniCode terminal — a persistent zsh session. "
                          "Type `clear` to reset."
                   color:THex(0x6A9955)];
        [self updatePrompt];
        [self startShell];
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
    if (!dir.length) return;
    _cwd = dir;
    [self updatePrompt];
    if (_writeHandle && !_running)   // move the live shell too
        [self sendRaw:[NSString stringWithFormat:@"cd %@\n", [self quote:dir]]];
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

// ------------------------------------------------------- persistent shell
- (void)dealloc {
    _shell.terminationHandler = nil;   // don't auto-restart on teardown
    [_shell terminate];
}

- (void)startShell {
    _shell = [[NSTask alloc] init];
    _shell.executableURL = [NSURL fileURLWithPath:@"/bin/zsh"];
    _shell.arguments = @[@"-l"];   // login shell, reads commands from stdin
    NSPipe *inPipe = [NSPipe pipe];
    NSPipe *outPipe = [NSPipe pipe];
    _shell.standardInput = inPipe;
    _shell.standardOutput = outPipe;
    _shell.standardError = outPipe;   // merge stderr into stdout for natural order
    _writeHandle = inPipe.fileHandleForWriting;

    __weak TerminalView *weakSelf = self;
    outPipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData;
        if (!d.length) return;
        NSString *chunk = [[NSString alloc] initWithData:d
                                                encoding:NSUTF8StringEncoding];
        if (!chunk) return;
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf ingest:chunk]; });
    };
    _shell.terminationHandler = ^(NSTask *t) {
        (void)t;
        dispatch_async(dispatch_get_main_queue(), ^{
            TerminalView *s = weakSelf;
            if (!s) return;
            [s appendLine:@"[shell exited — restarting]" color:THex(0x6A9955)];
            [s startShell];
            s.input.enabled = YES;
            s->_running = NO;
        });
    };

    NSError *err = nil;
    if (![_shell launchAndReturnError:&err]) {
        [self appendLine:[@"Could not start shell: "
            stringByAppendingString:err.localizedDescription] color:THex(0xF48771)];
        return;
    }
    // Move the shell to the starting directory (echoes no output).
    [self sendRaw:[NSString stringWithFormat:@"cd %@\n", [self quote:_cwd]]];
}

- (NSString *)quote:(NSString *)s {   // single-quote for the shell
    return [NSString stringWithFormat:@"'%@'",
            [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

- (void)sendRaw:(NSString *)text {
    [_writeHandle writeData:[text dataUsingEncoding:NSUTF8StringEncoding]];
}

// ---------------------------------------------------------------- running
- (void)runCommand:(NSString *)cmd {
    NSString *abbrev = [_cwd stringByAbbreviatingWithTildeInPath];
    [self appendLine:[NSString stringWithFormat:@"%@ ❯ %@", abbrev, cmd]
               color:THex(0x569CD6)];

    NSString *trimmed = [cmd stringByTrimmingCharactersInSet:
                         [NSCharacterSet whitespaceCharacterSet]];
    if ([trimmed isEqualToString:@"clear"]) {   // handled locally
        self.output.string = @"";
        return;
    }
    if (!_writeHandle) return;

    _running = YES;
    self.input.enabled = NO;

    // Run the command with stdin from /dev/null so it can't swallow the control
    // stream, then print the sentinel with the exit status and the new cwd.
    // The braces keep `cd` and `export` in the shell itself (not a subshell).
    [self sendRaw:[NSString stringWithFormat:@"{ %@ ; } </dev/null\n", cmd]];
    [self sendRaw:[NSString stringWithFormat:
        @"printf '%%s%%d\\t%%s\\n' '%@' \"$?\" \"$PWD\"\n", kSentinel]];
}

// Scan incoming shell output for the sentinel; display everything before it.
- (void)ingest:(NSString *)chunk {
    [_pending appendString:chunk];

    NSRange mark = [_pending rangeOfString:kSentinel];
    if (mark.location == NSNotFound) {
        // No sentinel yet: flush all but a tail that might be a partial marker.
        NSUInteger hold = kSentinel.length;
        if (_pending.length > hold) {
            NSString *safe = [_pending substringToIndex:_pending.length - hold];
            [self appendOutput:safe];
            [_pending deleteCharactersInRange:
                NSMakeRange(0, _pending.length - hold)];
        }
        return;
    }

    // Everything before the marker is command output.
    [self appendOutput:[_pending substringToIndex:mark.location]];

    // After the marker: "<rc>\t<pwd>\n". Parse, then keep any remainder.
    NSString *rest = [_pending substringFromIndex:NSMaxRange(mark)];
    NSRange nl = [rest rangeOfString:@"\n"];
    if (nl.location == NSNotFound) return;   // wait for the full status line
    NSString *status = [rest substringToIndex:nl.location];
    [_pending setString:[rest substringFromIndex:NSMaxRange(nl)]];

    NSArray<NSString *> *parts = [status componentsSeparatedByString:@"\t"];
    if (parts.count >= 2 && [parts[1] length]) {
        _cwd = parts[1];
        [self updatePrompt];
    }
    [self commandFinished];

    // A second command's output could already be buffered; process it.
    if ([_pending rangeOfString:kSentinel].location != NSNotFound)
        [self ingest:@""];
}

- (void)commandFinished {
    // Ensure the next prompt starts on its own line.
    NSString *s = self.output.string;
    if (s.length && ![s hasSuffix:@"\n"]) [self appendOutput:@"\n"];
    _running = NO;
    self.input.enabled = YES;
    [self focusInput];
}

// ---------------------------------------------------------------- output
// Strip ANSI escape sequences; keep the plain text. (Color SGR could be parsed
// here later — for now we favor clean, readable output.)
- (void)appendOutput:(NSString *)text {
    if (!text.length) return;
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
