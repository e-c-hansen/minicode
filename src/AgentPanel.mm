// AgentPanel.mm — see AgentPanel.h.
#import "AgentPanel.h"
#import "AppSettings.h"
#import "Lsp.h"   // MCSearchDirs
#include "ChangeRequest.h"
#include "InlineAgent.h"
#include "LineDiff.h"

#include <signal.h>
#include <unistd.h>

static std::string Std(NSString *s) { return s.UTF8String ?: ""; }
static NSString *NS(const std::string &s) { return @(s.c_str()) ?: @""; }

static NSColor *AHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1];
}

// The accent of the rest of the window (inline hex, see the UI map).
static const unsigned kAccent = 0x4EA1F7;
static const unsigned kErrorRed = 0xF14C4C;
static const CGFloat kPad = 10;
static const CGFloat kHeadH = 18;
static const CGFloat kFootH = 22;
static const CGFloat kMaxBoxLines = 6;
static const CGFloat kMaxMessageH = 220;
static const NSTimeInterval kRunLimit = 300;   // five minutes, then it is stopped

@implementation MCAgentContext
@end

// ------------------------------------------------------------ running Claude
@implementation MCAgentRun {
    NSTask *_task;
    BOOL _cancelled;
    void (^_completion)(NSString *, NSString *);
}

+ (instancetype)runPrompt:(NSString *)prompt inFolder:(NSString *)folder
               completion:(void (^)(NSString *, NSString *))completion {
    MCAgentRun *r = [MCAgentRun new];
    r->_completion = [completion copy];
    [r startWithPrompt:prompt folder:folder];
    return r;
}

- (void)finishOutput:(NSString *)output error:(NSString *)error {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_cancelled || !self->_completion) return;
        void (^done)(NSString *, NSString *) = self->_completion;
        self->_completion = nil;
        done(output, error);
    });
}

- (void)startWithPrompt:(NSString *)prompt folder:(NSString *)folder {
    const std::string command = [AppSettings shared].settings.agentCommand();
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/zsh"];
    // A login shell, so the PATH the user's .zprofile sets finds claude; exec,
    // so the task is Claude itself and stopping it stops Claude.
    task.arguments = @[@"-l", @"-c",
                       [NSString stringWithFormat:@"exec %@ %s", NS(command),
                                                  InlineAgent::kPrintFlags]];
    BOOL isDir = NO;
    if (!folder.length || ![[NSFileManager defaultManager] fileExistsAtPath:folder
                                                                isDirectory:&isDir] || !isDir)
        folder = NSHomeDirectory();
    task.currentDirectoryURL = [NSURL fileURLWithPath:folder];
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    NSString *path = env[@"PATH"] ?: @"/usr/bin:/bin:/usr/sbin:/sbin";
    env[@"PATH"] = [[MCSearchDirs() componentsJoinedByString:@":"]
                       stringByAppendingFormat:@":%@", path];
    env[@"NO_COLOR"] = @"1";
    task.environment = env;
    NSPipe *in = [NSPipe pipe], *out = [NSPipe pipe], *err = [NSPipe pipe];
    task.standardInput = in;
    task.standardOutput = out;
    task.standardError = err;
    _task = task;

    dispatch_group_t group = dispatch_group_create();
    __block NSData *outData = nil, *errData = nil;
    __block int status = -1;
    __block BOOL timedOut = NO;
    dispatch_group_enter(group);   // the process
    task.terminationHandler = ^(NSTask *t) {
        status = t.terminationStatus;
        dispatch_group_leave(group);
    };
    NSError *launchError = nil;
    if (![task launchAndReturnError:&launchError]) {
        [self finishOutput:nil error:[NSString stringWithFormat:@"Could not start the agent: %@",
                                                                launchError.localizedDescription]];
        return;
    }
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_group_async(group, q, ^{ outData = [out.fileHandleForReading readDataToEndOfFile]; });
    dispatch_group_async(group, q, ^{ errData = [err.fileHandleForReading readDataToEndOfFile]; });
    // The prompt may be larger than a pipe holds, so it is written off the
    // main thread while Claude reads it.
    NSData *input = [prompt dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];
    dispatch_async(q, ^{
        signal(SIGPIPE, SIG_IGN);
        @try {
            [in.fileHandleForWriting writeData:input];
        } @catch (NSException *e) {
            (void)e;   // Claude exited before reading it all; its error says why
        }
        [in.fileHandleForWriting closeFile];
    });
    __weak NSTask *weakTask = task;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kRunLimit * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        NSTask *t = weakTask;
        if (!t.running) return;
        timedOut = YES;
        [t terminate];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            if (weakTask.running) kill(weakTask.processIdentifier, SIGKILL);
        });
    });
    dispatch_group_notify(group, q, ^{
        NSString *o = [[NSString alloc] initWithData:outData ?: [NSData data]
                                            encoding:NSUTF8StringEncoding] ?: @"";
        NSString *e = [[NSString alloc] initWithData:errData ?: [NSData data]
                                            encoding:NSUTF8StringEncoding] ?: @"";
        if (timedOut) {
            [self finishOutput:nil error:@"Claude Code did not answer within five minutes, "
                                         @"so it was stopped."];
            return;
        }
        if (status == 127 || [e containsString:@"command not found"]) {
            [self finishOutput:nil error:[NSString stringWithFormat:
                @"Claude Code (%@) was not found. Install it from claude.com/claude-code, "
                @"or name the command in the settings file's agent.command.", NS(command)]];
            return;
        }
        if (status != 0) {
            NSString *why = [e stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if (!why.length)
                why = [o stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            NSArray<NSString *> *lines = [why componentsSeparatedByString:@"\n"];
            if (lines.count > 6)
                why = [[lines subarrayWithRange:NSMakeRange(lines.count - 6, 6)]
                          componentsJoinedByString:@"\n"];
            [self finishOutput:nil error:why.length ? why
                : [NSString stringWithFormat:@"Claude Code stopped with status %d.", status]];
            return;
        }
        [self finishOutput:o error:nil];
    });
}

- (void)cancel {
    _cancelled = YES;
    _completion = nil;
    NSTask *t = _task;
    if (t.running) {
        [t terminate];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            if (t.running) kill(t.processIdentifier, SIGKILL);
        });
    }
}
@end

// ------------------------------------------------------------ the bar
@interface MCAgentTextView : NSTextView
@property(nonatomic, copy) NSString *placeholder;
@property(nonatomic, strong) NSColor *placeholderColor;
@property(nonatomic, copy) void (^onCommandReturn)(void);
@end
@implementation MCAgentTextView
- (void)drawRect:(NSRect)r {
    [super drawRect:r];
    if (self.string.length || !self.placeholder.length) return;
    const NSPoint o = self.textContainerOrigin;
    [self.placeholder drawAtPoint:NSMakePoint(o.x + self.textContainer.lineFragmentPadding, o.y)
                   withAttributes:@{NSFontAttributeName: self.font,
                                    NSForegroundColorAttributeName:
                                        self.placeholderColor ?: NSColor.grayColor}];
}
// Cmd+Return hands the request to Claude Code in a terminal. It arrives as a
// key equivalent first, before any menu item can take it.
- (BOOL)isCommandReturn:(NSEvent *)e {
    const NSEventModifierFlags f = e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    return (e.keyCode == 36 || e.keyCode == 76) && (f & NSEventModifierFlagCommand) &&
           !(f & (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagOption));
}
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    if (self.window.firstResponder == self && [self isCommandReturn:e] && self.onCommandReturn) {
        self.onCommandReturn();
        return YES;
    }
    return [super performKeyEquivalent:e];
}
- (void)keyDown:(NSEvent *)e {
    if ([self isCommandReturn:e] && self.onCommandReturn) { self.onCommandReturn(); return; }
    [super keyDown:e];
}
@end

// A view that draws the bar's rounded body and border.
@implementation MCAgentBar {
    NSTextField *_headLabel, *_hintLabel, *_keysLabel;
    NSScrollView *_box, *_messageScroll;
    MCAgentTextView *_text;
    NSTextView *_messageView;
    NSButton *_close, *_primary, *_secondary;
    NSProgressIndicator *_spinner;
    NSColor *_textColor, *_fieldColor;
}

- (BOOL)isFlipped { return YES; }

static NSTextView *MCAgentMakeTextView(NSRect frame, Class cls) {
    // TextKit 1, built by hand, so measuring never switches the view over.
    NSTextStorage *storage = [NSTextStorage new];
    NSLayoutManager *lm = [NSLayoutManager new];
    [storage addLayoutManager:lm];
    NSTextContainer *tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(frame.size.width, CGFLOAT_MAX)];
    tc.widthTracksTextView = YES;
    [lm addTextContainer:tc];
    NSTextView *tv = [[cls alloc] initWithFrame:frame textContainer:tc];
    tv.richText = NO;
    tv.importsGraphics = NO;
    tv.usesFontPanel = NO;
    tv.automaticQuoteSubstitutionEnabled = NO;
    tv.automaticDashSubstitutionEnabled = NO;
    tv.automaticTextReplacementEnabled = NO;
    tv.verticallyResizable = YES;
    tv.horizontallyResizable = NO;
    tv.autoresizingMask = NSViewWidthSizable;
    tv.minSize = NSMakeSize(0, 0);
    tv.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    return tv;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    self.layer.cornerRadius = 7;
    self.layer.borderWidth = 1;
    self.shadow = [NSShadow new];
    self.layer.shadowOpacity = 0.35;
    self.layer.shadowRadius = 8;
    self.layer.shadowOffset = CGSizeMake(0, -2);

    _headLabel = [NSTextField labelWithString:@"Agent"];
    _headLabel.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
    _hintLabel = [NSTextField labelWithString:@""];
    _hintLabel.font = [NSFont systemFontOfSize:11];
    _hintLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _keysLabel = [NSTextField labelWithString:@""];
    _keysLabel.font = [NSFont systemFontOfSize:10.5];
    _keysLabel.lineBreakMode = NSLineBreakByTruncatingTail;

    _text = (MCAgentTextView *)MCAgentMakeTextView(NSMakeRect(0, 0, 300, 24), [MCAgentTextView class]);
    _text.font = [NSFont systemFontOfSize:13];
    _text.allowsUndo = YES;
    _text.textContainerInset = NSMakeSize(4, 4);
    _text.delegate = self;
    __weak MCAgentBar *weakSelf = self;
    _text.onCommandReturn = ^{
        MCAgentBar *b = weakSelf;
        if (b && b.onTask) b.onTask(b->_text.string.copy);
    };
    _box = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
    _box.hasVerticalScroller = YES;
    _box.autohidesScrollers = YES;
    _box.borderType = NSNoBorder;
    _box.documentView = _text;
    _box.wantsLayer = YES;
    _box.layer.cornerRadius = 5;
    _box.layer.borderWidth = 1;
    _box.layer.masksToBounds = YES;

    _messageView = MCAgentMakeTextView(NSMakeRect(0, 0, 300, 20), [NSTextView class]);
    _messageView.editable = NO;
    _messageView.selectable = YES;
    _messageView.drawsBackground = NO;
    _messageView.textContainerInset = NSMakeSize(0, 0);
    _messageView.textContainer.lineFragmentPadding = 2;
    _messageScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 300, 20)];
    _messageScroll.hasVerticalScroller = YES;
    _messageScroll.autohidesScrollers = YES;
    _messageScroll.borderType = NSNoBorder;
    _messageScroll.drawsBackground = NO;
    _messageScroll.documentView = _messageView;

    _close = [NSButton buttonWithTitle:@"×" target:self action:@selector(closeClicked:)];
    _close.bordered = NO;
    _close.toolTip = @"Close (Esc)";
    _primary = [NSButton buttonWithTitle:@"Ask" target:self action:@selector(primaryClicked:)];
    _secondary = [NSButton buttonWithTitle:@"Discard" target:self action:@selector(secondaryClicked:)];
    for (NSButton *b in @[_primary, _secondary]) {
        b.controlSize = NSControlSizeSmall;
        b.font = [NSFont systemFontOfSize:11];
        b.bezelStyle = NSBezelStyleRounded;
    }
    _spinner = [NSProgressIndicator new];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.controlSize = NSControlSizeSmall;
    _spinner.displayedWhenStopped = NO;
    for (NSView *v in @[_headLabel, _hintLabel, _keysLabel, _box, _messageScroll, _close,
                        _primary, _secondary, _spinner])
        [self addSubview:v];
    _heading = @"Agent";
    [self setBackground:AHex(0x252526) text:AHex(0xCCCCCC) field:AHex(0x1E1E1E)];
    self.mode = MCAgentBarPrompt;
    return self;
}

- (NSTextView *)textView { return _text; }

- (void)clearRequest {
    _text.string = @"";
    [self textDidChange:[NSNotification notificationWithName:NSTextDidChangeNotification
                                                      object:_text]];
}

- (void)setHeading:(NSString *)heading {
    _heading = [heading copy];
    _headLabel.stringValue = heading ?: @"";
    self.needsLayout = YES;
}

- (void)setHint:(NSString *)hint {
    _hint = [hint copy];
    _hintLabel.stringValue = hint ?: @"";
    _hintLabel.toolTip = hint;
    self.needsLayout = YES;
}

- (void)setMessage:(NSString *)message {
    _message = [message copy];
    [self applyMessage];
}

- (void)setMessageIsError:(BOOL)messageIsError {
    _messageIsError = messageIsError;
    [self applyMessage];
}

- (void)applyMessage {
    NSColor *color = _messageIsError ? AHex(kErrorRed) : (_textColor ?: NSColor.textColor);
    NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:12],
                        NSForegroundColorAttributeName: color};
    [_messageView.textStorage setAttributedString:
        [[NSAttributedString alloc] initWithString:_message ?: @"" attributes:a]];
    _messageScroll.hidden = !_message.length;
    self.needsLayout = YES;
}

- (void)setMode:(MCAgentBarMode)mode {
    _mode = mode;
    const BOOL working = mode == MCAgentBarWorking;
    _text.editable = !working;
    _text.textColor = working ? [_textColor colorWithAlphaComponent:0.5] : _textColor;
    if (working) [_spinner startAnimation:nil];
    else [_spinner stopAnimation:nil];
    _secondary.hidden = mode != MCAgentBarReview;
    _primary.hidden = NO;
    switch (mode) {
    case MCAgentBarPrompt:
        _primary.title = @"Ask";
        _primary.toolTip = @"Ask the agent (Return)";
        _text.placeholder = @"Ask for a change, or ask a question";
        _keysLabel.stringValue = @"Return asks · ⌘Return hands it to Claude Code in a terminal · Esc closes";
        break;
    case MCAgentBarWorking:
        _primary.title = @"Stop";
        _primary.toolTip = @"Stop the agent (Esc)";
        _keysLabel.stringValue = @"Esc stops";
        break;
    case MCAgentBarReview:
        _primary.title = @"Keep";
        _primary.toolTip = @"Keep the change (Return)";
        _secondary.title = @"Discard";
        _secondary.toolTip = @"Put the text back as it was (Esc)";
        _text.placeholder = @"Ask for more, or press Return to keep the change";
        _keysLabel.stringValue = @"Return keeps · Esc discards · ⌘Return hands it to Claude Code";
        break;
    case MCAgentBarAnswer:
        _primary.title = @"Close";
        _primary.toolTip = @"Close (Esc)";
        _text.placeholder = @"Ask again, or ask something more";
        _keysLabel.stringValue = @"Return asks · ⌘Return hands it to Claude Code in a terminal · Esc closes";
        break;
    }
    _text.needsDisplay = YES;
    self.needsLayout = YES;
}

- (void)setBackground:(NSColor *)background text:(NSColor *)text field:(NSColor *)field {
    // Opaque: the bar sits over text, which would show through.
    NSColor *bg = [[background colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: background
                      colorWithAlphaComponent:1];
    self.layer.backgroundColor = bg.CGColor;
    self.layer.borderColor = [AHex(kAccent) colorWithAlphaComponent:0.8].CGColor;
    _textColor = text;
    _fieldColor = [field colorWithAlphaComponent:1];
    _headLabel.textColor = AHex(kAccent);
    _hintLabel.textColor = [text colorWithAlphaComponent:0.6];
    _keysLabel.textColor = [text colorWithAlphaComponent:0.5];
    _text.backgroundColor = _fieldColor;
    _text.drawsBackground = YES;
    _box.backgroundColor = _fieldColor;
    _box.layer.borderColor = [AHex(kAccent) colorWithAlphaComponent:0.55].CGColor;
    _text.textColor = _mode == MCAgentBarWorking ? [text colorWithAlphaComponent:0.5] : text;
    _text.insertionPointColor = text;
    _text.placeholderColor = [text colorWithAlphaComponent:0.4];
    _text.typingAttributes = @{NSFontAttributeName: _text.font,
                               NSForegroundColorAttributeName: text};
    _close.attributedTitle = [[NSAttributedString alloc] initWithString:@"×"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:15],
                     NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.7]}];
    [self applyMessage];
    _text.needsDisplay = YES;
}

// The height of `storage`'s text laid out `width` wide.
static CGFloat MCAgentTextHeight(NSAttributedString *text, NSFont *font, CGFloat width,
                                 CGFloat padding) {
    static NSTextStorage *storage;
    static NSLayoutManager *lm;
    static NSTextContainer *tc;
    if (!storage) {
        storage = [NSTextStorage new];
        lm = [NSLayoutManager new];
        [storage addLayoutManager:lm];
        tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(100, CGFLOAT_MAX)];
        [lm addTextContainer:tc];
    }
    tc.lineFragmentPadding = padding;
    tc.size = NSMakeSize(MAX(20, width), CGFLOAT_MAX);
    [storage setAttributedString:text];
    if (font) [storage addAttribute:NSFontAttributeName value:font range:NSMakeRange(0, storage.length)];
    [lm ensureLayoutForTextContainer:tc];
    return ceil([lm usedRectForTextContainer:tc].size.height);
}

- (CGFloat)boxHeightForWidth:(CGFloat)width {
    const NSSize inset = _text.textContainerInset;
    const CGFloat line = [_text.layoutManager defaultLineHeightForFont:_text.font];
    const CGFloat h = MIN(MAX(MCAgentTextHeight(_text.textStorage, _text.font,
                                                width - 2 * inset.width,
                                                _text.textContainer.lineFragmentPadding),
                              line),
                          line * kMaxBoxLines);
    return h + 2 * inset.height;
}

- (CGFloat)messageHeightForWidth:(CGFloat)width {
    if (!_message.length) return 0;
    return MIN(MCAgentTextHeight(_messageView.textStorage, nil, width - 16, 2) + 2, kMaxMessageH);
}

- (CGFloat)heightForWidth:(CGFloat)W {
    const CGFloat inner = W - 2 * kPad;
    CGFloat h = kPad + kHeadH + 6 + [self boxHeightForWidth:inner];
    const CGFloat m = [self messageHeightForWidth:inner];
    if (m > 0) h += 8 + m;
    return h + 8 + kFootH + kPad - 2;
}

- (void)layout {
    [super layout];
    const CGFloat W = NSWidth(self.bounds), inner = W - 2 * kPad;
    [_close sizeToFit];
    const NSSize c = _close.frame.size;
    _close.frame = NSMakeRect(W - kPad + 2 - c.width, kPad + round((kHeadH - c.height) / 2),
                              c.width, c.height);
    [_headLabel sizeToFit];
    const CGFloat hw = ceil(NSWidth(_headLabel.frame));
    _headLabel.frame = NSMakeRect(kPad, kPad + 1, hw, 16);
    _spinner.frame = NSMakeRect(kPad + hw + 6, kPad + 1, 16, 16);
    const CGFloat hx = kPad + hw + (_mode == MCAgentBarWorking ? 28 : 10);
    _hintLabel.frame = NSMakeRect(hx, kPad + 2, MAX(0, NSMinX(_close.frame) - 8 - hx), 15);

    CGFloat y = kPad + kHeadH + 6;
    const CGFloat boxH = [self boxHeightForWidth:inner];
    _box.frame = NSMakeRect(kPad, y, inner, boxH);
    y += boxH;
    const CGFloat m = [self messageHeightForWidth:inner];
    if (m > 0) {
        y += 8;
        _messageScroll.frame = NSMakeRect(kPad, y, inner, m);
        y += m;
    }
    y += 8;
    CGFloat x = W - kPad;
    for (NSButton *b in @[_primary, _secondary]) {
        if (b.hidden) continue;
        [b sizeToFit];
        const NSSize s = b.frame.size;
        x -= s.width;
        b.frame = NSMakeRect(x, y + round((kFootH - s.height) / 2), s.width, s.height);
        x -= 6;
    }
    _keysLabel.frame = NSMakeRect(kPad, y + 4, MAX(0, x - 6 - kPad), 14);
}

- (void)primaryClicked:(id)sender {
    (void)sender;
    switch (_mode) {
    case MCAgentBarPrompt:
        if (self.onSend) self.onSend(_text.string.copy);
        break;
    case MCAgentBarWorking:
        if (self.onStop) self.onStop();
        break;
    case MCAgentBarReview:
        if (self.onKeep) self.onKeep();
        break;
    case MCAgentBarAnswer:
        if (self.onClose) self.onClose();
        break;
    }
}
- (void)secondaryClicked:(id)sender {
    (void)sender;
    if (self.onDiscard) self.onDiscard();
}
- (void)closeClicked:(id)sender {
    (void)sender;
    if (_mode == MCAgentBarWorking && self.onStop) self.onStop();
    if (self.onClose) self.onClose();
}

- (void)textDidChange:(NSNotification *)note {
    (void)note;
    _text.needsDisplay = YES;   // the placeholder
    if (!self.superview) return;
    if (fabs([self heightForWidth:NSWidth(self.bounds)] - NSHeight(self.bounds)) > 0.5) {
        if (self.onResize) self.onResize();
    } else {
        self.needsLayout = YES;
    }
}

// Return asks (or, with nothing typed, keeps a change or closes an answer);
// Shift or Option+Return adds a line; Esc stops, discards or closes.
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    if (sel == @selector(insertNewline:)) {
        const NSEventModifierFlags f = NSApp.currentEvent.modifierFlags;
        if (!(f & (NSEventModifierFlagShift | NSEventModifierFlagOption))) {
            NSString *typed = [tv.string stringByTrimmingCharactersInSet:
                                   NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if (_mode == MCAgentBarWorking) return YES;
            if (typed.length) {
                if (self.onSend) self.onSend(tv.string.copy);
            } else if (_mode == MCAgentBarReview) {
                if (self.onKeep) self.onKeep();
            } else if (_mode == MCAgentBarAnswer) {
                if (self.onClose) self.onClose();
            } else {
                NSBeep();
            }
            return YES;
        }
        sel = @selector(insertLineBreak:);
    }
    if (sel == @selector(insertLineBreak:) || sel == @selector(insertNewlineIgnoringFieldEditor:)) {
        [tv insertText:@"\n" replacementRange:tv.selectedRange];
        return YES;
    }
    if (sel == @selector(cancelOperation:) || sel == @selector(complete:)) {
        if (_mode == MCAgentBarWorking) {
            if (self.onStop) self.onStop();
        } else if (_mode == MCAgentBarReview) {
            if (self.onDiscard) self.onDiscard();
        } else if (self.onClose) {
            self.onClose();
        }
        return YES;
    }
    if (sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) return YES;
    return NO;
}
@end

// ------------------------------------------------------------ the session
@implementation MCAgentSession {
    __weak id<MCAgentHost> _host;
    MCAgentBar *_bar;
    MCAgentContext *_ctx;
    MCAgentRun *_run;
    NSTimer *_tick;
    NSDate *_started;
    NSMutableArray<NSString *> *_earlier;
    NSString *_original;   // the text before this bar's first edit
    NSString *_applied;    // the text after its last
    NSString *_asked;      // the request on its way
    id _scrollObserver;
    NSUInteger _taskCount;
}

- (instancetype)initWithHost:(id<MCAgentHost>)host {
    if ((self = [super init])) {
        _host = host;
        _earlier = [NSMutableArray array];
    }
    return self;
}

- (void)dealloc {
    [_run cancel];
    [_tick invalidate];
    if (_scrollObserver) [[NSNotificationCenter defaultCenter] removeObserver:_scrollObserver];
}

- (MCAgentContext *)context { return _ctx; }

- (MCAgentBar *)bar {
    if (_bar) return _bar;
    _bar = [[MCAgentBar alloc] initWithFrame:NSMakeRect(0, 0, 600, 120)];
    _bar.hidden = YES;
    __weak MCAgentSession *weakSelf = self;
    _bar.onSend = ^(NSString *r) { [weakSelf send:r]; };
    _bar.onTask = ^(NSString *r) { [weakSelf handOver:r]; };
    _bar.onKeep = ^{ [weakSelf keep]; };
    _bar.onDiscard = ^{ [weakSelf discard]; };
    _bar.onStop = ^{ [weakSelf stop]; };
    _bar.onClose = ^{ [weakSelf close]; };
    _bar.onResize = ^{ [weakSelf layout]; };
    [self applyColors];
    return _bar;
}

- (void)applyColors {
    if (!_bar) return;
    AppSettings *cfg = [AppSettings shared];
    [_bar setBackground:[cfg background:Surface::Sidebar]
                   text:[cfg text:Surface::Sidebar]
                  field:[cfg background:Surface::Editor]];
}

- (void)beginWithContext:(MCAgentContext *)context {
    id<MCAgentHost> host = _host;
    if (!host || !context) return;
    if (_shown && _ctx && context.owner && context.owner == _ctx.owner &&
        context.target == _ctx.target) {
        [host.window makeFirstResponder:_bar.textView];
        return;
    }
    if (_shown) [self finish:NO];
    _ctx = context;
    [_earlier removeAllObjects];
    _original = _applied = nil;
    MCAgentBar *bar = self.bar;
    NSView *parent = [host agentBarHost];
    if (bar.superview != parent) [parent addSubview:bar positioned:NSWindowAbove relativeTo:nil];
    bar.heading = @"Agent";
    bar.hint = context.hint;
    bar.message = nil;
    bar.messageIsError = NO;
    bar.mode = MCAgentBarPrompt;
    [bar clearRequest];
    _shown = YES;
    bar.hidden = NO;
    if (context.followScroll) {
        context.followScroll.postsBoundsChangedNotifications = YES;
        __weak MCAgentSession *weakSelf = self;
        _scrollObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSViewBoundsDidChangeNotification object:context.followScroll
                         queue:nil usingBlock:^(NSNotification *n) { (void)n; [weakSelf layout]; }];
    }
    [host agentBarDidChange];
    [host.window makeFirstResponder:bar.textView];
}

- (void)layout {
    if (!_shown || !_ctx) return;
    MCAgentBar *bar = _bar;
    NSView *parent = bar.superview;
    if (!parent) return;
    const NSRect area = _ctx.area ? _ctx.area() : parent.bounds;
    NSRect anchor = _ctx.anchor ? _ctx.anchor() : NSMakeRect(NSMinX(area), NSMaxY(area) - 8, 0, 0);
    const CGFloat w = MAX(120, MIN(720, NSWidth(area) - 16));
    const CGFloat h = MIN([bar heightForWidth:w], MAX(60, NSHeight(area) - 8));
    // The host is not flipped: y goes up. Under the anchor's line first.
    const CGFloat below = NSMinY(anchor) - 4 - h, above = NSMaxY(anchor) + 4;
    const BOOL fitsBelow = below >= NSMinY(area) + 4, fitsAbove = above + h <= NSMaxY(area) - 4;
    CGFloat y = _ctx.anchorAbove ? (fitsAbove || !fitsBelow ? above : below)
                                 : (fitsBelow || !fitsAbove ? below : above);
    y = MAX(NSMinY(area) + 4, MIN(y, NSMaxY(area) - 4 - h));
    CGFloat x = MAX(NSMinX(area) + 8, MIN(NSMinX(anchor), NSMaxX(area) - 8 - w));
    bar.frame = NSMakeRect(round(x), round(y), w, h);
    if (parent.subviews.lastObject != bar)
        [parent addSubview:bar positioned:NSWindowAbove relativeTo:nil];
    bar.needsLayout = YES;
}

- (void)startTicking {
    _started = [NSDate date];
    [_tick invalidate];
    __weak MCAgentSession *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) {
        MCAgentSession *s = weakSelf;
        if (!s || !s->_run) { [t invalidate]; return; }
        [s showWorking];
    }];
    [self showWorking];
}

- (void)showWorking {
    const int secs = (int)(-[_started timeIntervalSinceNow]);
    NSString *what = _ctx.target == MCAgentTargetCommand ? @"Writing a command" : @"Asking Claude Code";
    _bar.message = secs < 1 ? [what stringByAppendingString:@"…"]
                            : [NSString stringWithFormat:@"%@… %d s", what, secs];
    _bar.messageIsError = NO;
    [self layout];
}

// UTF-16 offsets of `text` to UTF-8 bytes.
static size_t MCAgentBytes(NSString *text, NSUInteger utf16) {
    utf16 = MIN(utf16, text.length);
    // Never between the halves of a surrogate pair.
    if (utf16 > 0 && utf16 < text.length && CFStringIsSurrogateLowCharacter([text characterAtIndex:utf16]))
        utf16--;
    return [[text substringToIndex:utf16] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

- (void)send:(NSString *)request {
    NSString *typed = [request stringByTrimmingCharactersInSet:
                           NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!_ctx || !typed.length || _run) return;
    if (_ctx.target == MCAgentTargetTask) { [self handOver:request]; return; }
    NSString *prompt = nil;
    if (_ctx.target == MCAgentTargetEdit) {
        NSString *text = _ctx.currentText ? _ctx.currentText() : nil;
        if (!text) { [self close]; return; }
        const NSRange sel = _ctx.currentSelection ? _ctx.currentSelection() : _ctx.selection;
        InlineAgent::EditRequest r;
        r.request = Std(typed);
        r.path = Std(_ctx.path);
        r.text = Std(text);
        r.selStart = MCAgentBytes(text, sel.location);
        r.selEnd = MCAgentBytes(text, NSMaxRange(sel));
        for (NSString *e in _earlier) r.earlier.push_back(Std(e));
        prompt = NS(InlineAgent::editPrompt(r));
    } else {
        InlineAgent::CommandRequest r;
        r.request = Std(typed);
        r.directory = Std(_ctx.directory ?: _ctx.root);
        r.screen = Std(_ctx.screen);
        for (NSString *e in _earlier) r.earlier.push_back(Std(e));
        prompt = NS(InlineAgent::commandPrompt(r));
    }
    _asked = typed;
    _bar.mode = MCAgentBarWorking;
    [self startTicking];
    __weak MCAgentSession *weakSelf = self;
    _run = [MCAgentRun runPrompt:prompt inFolder:_ctx.root completion:^(NSString *out, NSString *err) {
        [weakSelf finishedWithOutput:out error:err];
    }];
}

- (void)showAnswer:(NSString *)text error:(BOOL)error {
    _bar.mode = _original ? MCAgentBarReview : MCAgentBarAnswer;
    _bar.message = text;
    _bar.messageIsError = error;
    [self layout];
}

- (void)finishedWithOutput:(NSString *)output error:(NSString *)error {
    _run = nil;
    [_tick invalidate];
    _tick = nil;
    if (!_shown) return;
    if (error) {
        [self showAnswer:error error:YES];
        return;
    }
    if (_ctx.target == MCAgentTargetCommand) {
        std::string note;
        std::string command = InlineAgent::commandFrom(Std(output), &note);
        if (command.empty()) {
            [self showAnswer:note.empty() ? @"Claude Code gave no command." : NS(note) error:NO];
            return;
        }
        NSString *cmd = NS(command);
        if (_ctx.typeCommand && _ctx.typeCommand(cmd)) {
            [_bar clearRequest];
            [self finish:YES keepFocus:YES];
            [_host showStatusNote:@"The agent's command is at the prompt: press Return to run it"];
        } else {
            [self showAnswer:[NSString stringWithFormat:
                                  @"The terminal is busy, so the command was not typed:\n\n%@", cmd]
                       error:NO];
        }
        return;
    }
    // An edit, or an answer.
    InlineAgent::Reply reply = InlineAgent::parseReply(Std(output));
    if (reply.edits.empty()) {
        [self showAnswer:reply.answer.empty() ? @"Claude Code gave no answer." : NS(reply.answer)
                   error:NO];
        [_bar clearRequest];
        return;
    }
    NSString *now = [_ctx.currentText ? _ctx.currentText() : nil copy];
    if (!now) { [self close]; return; }
    const NSRange sel = _ctx.currentSelection ? _ctx.currentSelection() : _ctx.selection;
    InlineAgent::Applied a = InlineAgent::applyEdits(Std(now), reply.edits,
                                                     MCAgentBytes(now, sel.location));
    if (!a.ok) { [self showAnswer:NS(a.error) error:YES]; return; }
    NSString *next = NS(a.text);
    if ([next isEqualToString:now]) {
        [self showAnswer:@"Claude's edit leaves the file as it is." error:NO];
        [_bar clearRequest];
        return;
    }
    const std::u16string before = [self u16:now], after = [self u16:next];
    size_t changed = 0;
    for (const LineDiff::Hunk &h : LineDiff::diff(before, after))
        changed += MAX(h.oldLines, h.newLines);
    NSString *firstOriginal = _original ?: now;
    if (!_ctx.applyText || !_ctx.applyText(next, @"Inline Agent")) {
        [self showAnswer:@"The file could not be changed here." error:YES];
        return;
    }
    _original = firstOriginal;
    _applied = [_ctx.currentText ? _ctx.currentText() : next copy];
    [_earlier addObject:_asked ?: @""];
    if (_ctx.tint) _ctx.tint(_original);
    [_bar clearRequest];
    _bar.mode = MCAgentBarReview;
    _bar.message = [NSString stringWithFormat:@"Changed %zu line%@.", changed, changed == 1 ? @"" : @"s"];
    _bar.messageIsError = NO;
    [self layout];
}

- (std::u16string)u16:(NSString *)s {
    std::u16string out(s.length, u'\0');
    [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}

- (void)stop {
    if (!_run) return;
    [_run cancel];
    _run = nil;
    [_tick invalidate];
    _tick = nil;
    [self showAnswer:@"Stopped." error:NO];
    // The request comes back, to change and ask again.
    if (_asked.length && !_bar.textView.string.length) {
        _bar.textView.string = _asked;
        [_bar textDidChange:[NSNotification notificationWithName:NSTextDidChangeNotification
                                                          object:_bar.textView]];
    }
}

- (void)handOver:(NSString *)request {
    NSString *typed = [request stringByTrimmingCharactersInSet:
                           NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!typed.length) typed = _asked;
    if (!_ctx || !typed.length) { NSBeep(); return; }
    InlineAgent::TaskRequest t;
    t.request = Std(typed);
    t.where = Std(_ctx.where);
    t.path = Std(_ctx.path);
    t.selection = Std(_ctx.selectionText);
    t.firstLine = _ctx.firstLine;
    t.lastLine = _ctx.lastLine;
    t.screen = Std(_ctx.screen);
    const std::string prompt = InlineAgent::taskPrompt(t);
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"MiniCode-agent"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                               attributes:@{NSFilePosixPermissions: @0700}
                                                    error:nil];
    NSString *file = [dir stringByAppendingPathComponent:
        [NSString stringWithFormat:@"task-%d-%lu.txt", getpid(), (unsigned long)++_taskCount]];
    if (![NS(prompt) writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
        [self showAnswer:@"Could not write the request for Claude Code." error:YES];
        return;
    }
    NSString *root = _ctx.root.length ? _ctx.root : NSHomeDirectory();
    const std::string command = ChangeRequest::startCommand(
        [AppSettings shared].settings.agentCommand(), Std(root), Std(file));
    id<MCAgentHost> host = _host;
    [self finish:YES keepFocus:YES];
    [host agentRunInTerminal:NS(command)];
}

- (void)keep {
    [self finish:YES];
}

- (void)discard {
    if (_original && _ctx.currentText && _ctx.applyText) {
        NSString *now = _ctx.currentText();
        if (now && [now isEqualToString:_applied]) {
            _ctx.applyText(_original, @"Discard Agent Edit");
        } else if (now) {
            [_host showStatusNote:@"The file changed after the agent's edit, so it was left; "
                                  @"Cmd+Z takes it back"];
        }
    }
    [self finish:YES];
}

- (void)close {
    [self finish:YES];
}

- (void)documentChanged {
    if (_shown && _ctx.target == MCAgentTargetEdit && !(_ctx.currentText && _ctx.currentText()))
        [self finish:NO];
}

- (void)finish:(BOOL)restoreFocus {
    [self finish:restoreFocus keepFocus:NO];
}

// Closes the bar, keeping whatever edit is in the text. With restoreFocus,
// the keyboard goes back where it was, unless keepFocus says whoever closed
// the bar has put it somewhere already.
- (void)finish:(BOOL)restoreFocus keepFocus:(BOOL)keepFocus {
    if (!_shown) return;
    [_run cancel];
    _run = nil;
    [_tick invalidate];
    _tick = nil;
    if (_ctx.tint) _ctx.tint(nil);
    if (_scrollObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:_scrollObserver];
        _scrollObserver = nil;
    }
    _shown = NO;
    id<MCAgentHost> host = _host;
    NSWindow *w = host.window;
    const BOOL barHadKeyboard = [w.firstResponder isKindOfClass:[NSView class]] &&
                                [(NSView *)w.firstResponder isDescendantOf:_bar];
    _bar.hidden = YES;
    NSResponder *back = _ctx.returnFocus;
    _ctx = nil;
    _original = _applied = nil;
    [host agentBarDidChange];
    if (restoreFocus && !keepFocus && barHadKeyboard) {
        if ([back isKindOfClass:[NSView class]] && [(NSView *)back window] == w &&
            !((NSView *)back).isHiddenOrHasHiddenAncestor)
            [w makeFirstResponder:back];
        else
            [w makeFirstResponder:nil];
    } else if (barHadKeyboard && !keepFocus) {
        [w makeFirstResponder:nil];
    }
}
@end
