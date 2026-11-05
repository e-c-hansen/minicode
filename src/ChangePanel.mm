// ChangePanel.mm — see ChangePanel.h.
#import "ChangePanel.h"
#import "AppSettings.h"
#import "Lsp.h"   // MCFindProgram
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
// Every mode of the bar is about changing minicode itself, which nobody does
// in passing, so it looks the part: an amber stripe along its top, an amber
// tint over the sidebar color, and a DEV pill. The amber is inline hex like
// the other accents (see the UI map in CLAUDE.md), not a setting.
static const unsigned kDevAmber = 0xE8A33D;
static const CGFloat kStripe = 2;      // the amber line along the top
static const CGFloat kRowH = 32;       // a one-line bar's row, under the stripe
static const CGFloat kHeadH = 18;      // the prompt's heading row
static const CGFloat kMaxLines = 10;   // the box grows to this many, then scrolls

// The DEV pill: a rounded amber box with dark letters.
@interface MCDevPill : NSView
@end
@implementation MCDevPill
- (NSDictionary *)attrs {
    return @{NSFontAttributeName: [NSFont systemFontOfSize:9.5 weight:NSFontWeightHeavy],
             NSForegroundColorAttributeName: CHex(0x1E1E1E),
             NSKernAttributeName: @0.6};
}
- (NSSize)intrinsicContentSize {
    NSSize s = [@"DEV" sizeWithAttributes:[self attrs]];
    return NSMakeSize(ceil(s.width) + 10, 15);
}
- (void)drawRect:(NSRect)r {
    (void)r;
    [CHex(kDevAmber) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:3 yRadius:3] fill];
    NSDictionary *a = [self attrs];
    NSSize s = [@"DEV" sizeWithAttributes:a];
    [@"DEV" drawAtPoint:NSMakePoint(round((NSWidth(self.bounds) - s.width) / 2),
                                    round((NSHeight(self.bounds) - s.height) / 2))
         withAttributes:a];
}
@end

// The request box: a text view that wraps, with a placeholder.
@interface MCChangeTextView : NSTextView
@property(nonatomic, copy) NSString *placeholder;
@property(nonatomic, strong) NSColor *placeholderColor;
@end
@implementation MCChangeTextView
- (void)drawRect:(NSRect)r {
    [super drawRect:r];
    if (self.string.length || !self.placeholder.length) return;
    const NSPoint o = self.textContainerOrigin;
    [self.placeholder drawAtPoint:NSMakePoint(o.x + self.textContainer.lineFragmentPadding, o.y)
                   withAttributes:@{NSFontAttributeName: self.font,
                                    NSForegroundColorAttributeName:
                                        self.placeholderColor ?: NSColor.grayColor}];
}
@end

@implementation MCChangeBar {
    MCDevPill *_pill;
    NSTextField *_label, *_hintLabel;
    NSScrollView *_box;
    MCChangeTextView *_text;
    NSButton *_action, *_close;
    NSView *_stripe, *_line;
    NSColor *_textColor;
}

- (BOOL)isFlipped { return YES; }

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    _pill = [[MCDevPill alloc] initWithFrame:NSZeroRect];
    _pill.toolTip = @"You are changing minicode itself: Claude Code edits its source and rebuilds it";
    _label = [NSTextField labelWithString:@""];
    _label.font = [NSFont systemFontOfSize:12];
    _label.lineBreakMode = NSLineBreakByTruncatingTail;
    _hintLabel = [NSTextField labelWithString:@""];
    _hintLabel.font = [NSFont systemFontOfSize:11];
    _hintLabel.lineBreakMode = NSLineBreakByTruncatingTail;

    // TextKit 1 throughout, built by hand, so asking for the layout manager
    // (to measure the text) never switches the view over mid-run.
    NSTextStorage *storage = [NSTextStorage new];
    NSLayoutManager *lm = [NSLayoutManager new];
    [storage addLayoutManager:lm];
    NSTextContainer *tc = [[NSTextContainer alloc] initWithSize:NSMakeSize(200, CGFLOAT_MAX)];
    tc.widthTracksTextView = YES;
    [lm addTextContainer:tc];
    _text = [[MCChangeTextView alloc] initWithFrame:NSMakeRect(0, 0, 200, 24) textContainer:tc];
    _text.font = [NSFont systemFontOfSize:13];
    _text.richText = NO;
    _text.importsGraphics = NO;
    _text.allowsUndo = YES;
    _text.usesFontPanel = NO;
    // A request often names code: keep quotes and dashes as typed.
    _text.automaticQuoteSubstitutionEnabled = NO;
    _text.automaticDashSubstitutionEnabled = NO;
    _text.automaticTextReplacementEnabled = NO;
    _text.textContainerInset = NSMakeSize(4, 4);
    _text.verticallyResizable = YES;
    _text.horizontallyResizable = NO;
    _text.autoresizingMask = NSViewWidthSizable;
    _text.minSize = NSMakeSize(0, 0);
    _text.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _text.placeholder = @"Describe a change to minicode. Return sends, Shift Return adds a line, Esc closes.";
    _text.delegate = self;
    _box = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
    _box.hasVerticalScroller = YES;
    _box.autohidesScrollers = YES;
    _box.borderType = NSNoBorder;
    _box.documentView = _text;
    _box.wantsLayer = YES;
    _box.layer.cornerRadius = 5;
    _box.layer.borderWidth = 1;
    _box.layer.masksToBounds = YES;

    _action = [NSButton buttonWithTitle:@"Send" target:self action:@selector(act:)];
    _action.controlSize = NSControlSizeSmall;
    _action.font = [NSFont systemFontOfSize:11];
    _action.bezelStyle = NSBezelStyleRounded;
    _close = [NSButton buttonWithTitle:@"×" target:self action:@selector(close:)];
    _close.bordered = NO;
    _stripe = [NSView new];
    _stripe.wantsLayer = YES;
    _stripe.layer.backgroundColor = CHex(kDevAmber).CGColor;
    _line = [NSView new];
    _line.wantsLayer = YES;
    for (NSView *v in @[_pill, _label, _hintLabel, _box, _action, _close, _stripe, _line])
        [self addSubview:v];
    [self setBackground:CHex(0x252526) text:CHex(0xCCCCCC) field:CHex(0x1E1E1E)];
    self.mode = MCChangeBarPrompt;
    return self;
}

- (NSTextView *)textView { return _text; }
- (NSButton *)actionButton { return _action; }
- (NSString *)request { return _text.string.copy; }

- (void)clearRequest {
    _text.string = @"";
    [self textDidChange:[NSNotification notificationWithName:NSTextDidChangeNotification
                                                      object:_text]];
}

- (void)setMode:(MCChangeBarMode)mode {
    _mode = mode;
    const BOOL prompt = mode == MCChangeBarPrompt;
    _box.hidden = !prompt;
    _hintLabel.hidden = !prompt;
    _action.hidden = mode == MCChangeBarNote;
    _action.title = prompt ? @"Send"
                  : mode == MCChangeBarWorking ? @"Show Terminal" : @"Restart";
    _action.toolTip = prompt ? @"Send the request to Claude Code (Return)" : nil;
    if (prompt) {
        _label.stringValue = @"Change minicode";
        _close.toolTip = @"Close (Esc)";
    } else {
        _label.stringValue = _message ?: @"";
        _close.toolTip = @"Close";
    }
    _label.toolTip = prompt ? nil : _message;
    [self applyLabelColors];
    self.needsLayout = YES;
}

- (void)setMessage:(NSString *)message {
    _message = [message copy];
    self.mode = _mode;   // the label again
}

- (void)setHint:(NSString *)hint {
    _hint = [hint copy];
    _hintLabel.stringValue = _hint ?: @"";
    _hintLabel.toolTip = _hint;
    self.needsLayout = YES;
}

- (void)applyLabelColors {
    if (!_textColor) return;
    const BOOL prompt = _mode == MCChangeBarPrompt;
    _label.textColor = prompt ? CHex(kDevAmber) : _textColor;
    _label.font = prompt ? [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold]
                         : [NSFont systemFontOfSize:12];
}

- (void)setBackground:(NSColor *)background text:(NSColor *)text field:(NSColor *)field {
    NSColor *amber = CHex(kDevAmber);
    NSColor *bg = [background colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: background;
    // Tinted toward amber, keeping the sidebar's opacity.
    NSColor *tint = [[bg blendedColorWithFraction:0.13 ofColor:amber]
                        colorWithAlphaComponent:bg.alphaComponent];
    self.layer.backgroundColor = (tint ?: bg).CGColor;
    _textColor = text;
    [self applyLabelColors];
    _hintLabel.textColor = [text colorWithAlphaComponent:0.6];
    _line.layer.backgroundColor = [amber colorWithAlphaComponent:0.35].CGColor;
    _text.backgroundColor = field;
    _text.drawsBackground = YES;
    _box.backgroundColor = field;
    _box.layer.borderColor = [amber colorWithAlphaComponent:0.75].CGColor;
    _text.textColor = text;
    _text.insertionPointColor = text;
    _text.placeholderColor = [text colorWithAlphaComponent:0.4];
    _text.typingAttributes = @{NSFontAttributeName: _text.font,
                               NSForegroundColorAttributeName: text};
    _close.attributedTitle = [[NSAttributedString alloc] initWithString:@"×"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:15],
                     NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.7]}];
    _text.needsDisplay = YES;
}

// The box's height for a box `width` wide: its text laid out at that width,
// at least one line and at most kMaxLines, plus the insets.
- (CGFloat)boxHeightForWidth:(CGFloat)width {
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
    const NSSize inset = _text.textContainerInset;
    tc.lineFragmentPadding = _text.textContainer.lineFragmentPadding;
    tc.size = NSMakeSize(MAX(20, width - 2 * inset.width), CGFLOAT_MAX);
    [storage setAttributedString:_text.textStorage];
    [storage addAttribute:NSFontAttributeName value:_text.font
                    range:NSMakeRange(0, storage.length)];
    [lm ensureLayoutForTextContainer:tc];
    const CGFloat line = [lm defaultLineHeightForFont:_text.font];
    const CGFloat h = MIN(MAX(ceil([lm usedRectForTextContainer:tc].size.height), line),
                          line * kMaxLines);
    return h + 2 * inset.height;
}

- (CGFloat)boxWidthForWidth:(CGFloat)W {
    [_action sizeToFit];
    return MAX(60, W - 12 - 12 - 8 - NSWidth(_action.frame));
}

- (CGFloat)heightForWidth:(CGFloat)W {
    if (_mode != MCChangeBarPrompt) return kStripe + kRowH;
    return kStripe + 6 + kHeadH + 4 + [self boxHeightForWidth:[self boxWidthForWidth:W]] + 8;
}

- (void)layout {
    [super layout];
    const NSRect b = self.bounds;
    const CGFloat W = NSWidth(b), H = NSHeight(b);
    _stripe.frame = NSMakeRect(0, 0, W, kStripe);
    _line.frame = NSMakeRect(0, H - 1, W, 1);
    [_close sizeToFit];
    const NSSize c = _close.frame.size;
    const NSSize p = _pill.intrinsicContentSize;
    const CGFloat lh = 16;
    if (_mode == MCChangeBarPrompt) {
        // Heading: DEV, "Change minicode", the hint, ×. Under it the box,
        // with Send beside its last line.
        const CGFloat y0 = kStripe + 6;
        _close.frame = NSMakeRect(W - 8 - c.width, y0 + round((kHeadH - c.height) / 2),
                                  c.width, c.height);
        _pill.frame = NSMakeRect(12, y0 + round((kHeadH - p.height) / 2), p.width, p.height);
        [_label sizeToFit];
        const CGFloat lx = NSMaxX(_pill.frame) + 7, lw = ceil(NSWidth(_label.frame));
        _label.frame = NSMakeRect(lx, y0 + round((kHeadH - lh) / 2), lw, lh);
        const CGFloat hx = lx + lw + 10;
        _hintLabel.frame = NSMakeRect(hx, y0 + round((kHeadH - 15) / 2),
                                      MAX(0, NSMinX(_close.frame) - 8 - hx), 15);
        const CGFloat boxY = y0 + kHeadH + 4, boxW = [self boxWidthForWidth:W];
        const CGFloat boxH = MAX(0, MIN([self boxHeightForWidth:boxW], H - boxY - 8));
        _box.frame = NSMakeRect(12, boxY, boxW, boxH);
        const NSSize a = _action.frame.size;
        const CGFloat oneLine = [_text.layoutManager defaultLineHeightForFont:_text.font] +
                                2 * _text.textContainerInset.height;
        _action.frame = NSMakeRect(NSMaxX(_box.frame) + 8,
                                   boxY + boxH - oneLine + round((oneLine - a.height) / 2),
                                   a.width, a.height);
    } else {
        const CGFloat y0 = kStripe;
        _close.frame = NSMakeRect(W - 8 - c.width, y0 + round((kRowH - c.height) / 2),
                                  c.width, c.height);
        CGFloat x = NSMinX(_close.frame);
        if (!_action.hidden) {
            [_action sizeToFit];
            const NSSize a = _action.frame.size;
            x -= a.width + 8;
            _action.frame = NSMakeRect(x, y0 + round((kRowH - a.height) / 2), a.width, a.height);
        }
        _pill.frame = NSMakeRect(12, y0 + round((kRowH - p.height) / 2), p.width, p.height);
        const CGFloat lx = NSMaxX(_pill.frame) + 8;
        _label.frame = NSMakeRect(lx, y0 + round((kRowH - lh) / 2), MAX(0, x - 8 - lx), lh);
    }
}

- (void)act:(id)sender {
    (void)sender;
    if (_mode == MCChangeBarPrompt) {
        if (self.onSend) self.onSend(self.request);
    } else if (self.onAction) {
        self.onAction();
    }
}
- (void)close:(id)sender { (void)sender; if (self.onClose) self.onClose(); }

// The box grew or shrank by a line: the window lays the pane out again.
- (void)textDidChange:(NSNotification *)note {
    (void)note;
    _text.needsDisplay = YES;   // the placeholder
    if (_mode != MCChangeBarPrompt || !self.superview) return;
    if (fabs([self heightForWidth:NSWidth(self.bounds)] - NSHeight(self.bounds)) > 0.5) {
        if (self.onResize) self.onResize();
    } else {
        self.needsLayout = YES;
    }
}

// Return sends; Shift+Return or Option+Return starts a new line; Esc closes
// (a text view sends complete: for it); Tab moves on to Send.
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    if (sel == @selector(insertNewline:)) {
        const NSEventModifierFlags f = NSApp.currentEvent.modifierFlags;
        if (!(f & (NSEventModifierFlagShift | NSEventModifierFlagOption))) {
            [self act:nil];
            return YES;
        }
        sel = @selector(insertLineBreak:);
    }
    if (sel == @selector(insertLineBreak:) || sel == @selector(insertNewlineIgnoringFieldEditor:)) {
        [tv insertText:@"\n" replacementRange:tv.selectedRange];
        return YES;
    }
    if (sel == @selector(cancelOperation:) || sel == @selector(complete:)) {
        [self close:nil];
        return YES;
    }
    if (sel == @selector(insertTab:)) { [tv.window selectNextKeyView:nil]; return YES; }
    if (sel == @selector(insertBacktab:)) { [tv.window selectPreviousKeyView:nil]; return YES; }
    return NO;
}
@end

// ------------------------------------------------------------ the source
static BOOL MCLooksLikeSource(NSString *dir) {
    NSFileManager *fm = [NSFileManager defaultManager];
    return ChangeRequest::looksLikeSource(Std(dir), [fm](const std::string &p) {
        return (bool)[fm fileExistsAtPath:NS(p)];
    });
}

NSString *MCChangeCloneFolder(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:
            @"Library/Application Support/MiniCode/source"];
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
    NSString *clone = MCChangeCloneFolder();
    if (MCLooksLikeSource(clone)) return clone;
    return nil;
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
        _bar.onResize = ^{ [weakSelf barResized]; };
    }
    return _bar;
}

- (BOOL)barShown { return _shown; }

- (void)barResized {
    if (_shown) [_host changeBarDidChange];
}

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
    if (fr != _bar.textView) _before = fr;
    [self showBar:MCChangeBarPrompt message:nil];
    _bar.hint = [self hintText];
    [w makeFirstResponder:_bar.textView];
}

// The line beside the heading: where the change will be made.
- (NSString *)hintText {
    NSString *problem = nil;
    NSString *source = MCChangeSourceFolder(&problem);
    NSString *where = source
        ? [NSString stringWithFormat:@"Claude Code edits %@ and rebuilds it.",
              source.stringByAbbreviatingWithTildeInPath]
        : problem ? @"develop.source in the settings is not minicode's source folder."
                  : [NSString stringWithFormat:@"The source is cloned into %@ first.",
                        MCChangeCloneFolder().stringByAbbreviatingWithTildeInPath];
    return where;
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
        // An installed copy has no source: the first request clones the
        // release that is running into Application Support, in the
        // terminal, before Claude starts.
        NSString *clone = MCChangeCloneFolder();
        NSFileManager *fm = [NSFileManager defaultManager];
        if ([fm fileExistsAtPath:clone]) {
            [self showBar:MCChangeBarNote message:[NSString stringWithFormat:
                @"%@ is in the way of minicode's source: move it away and try again.",
                clone.stringByAbbreviatingWithTildeInPath]];
            return;
        }
        if (!MCFindProgram(@"git")) {
            [self showBar:MCChangeBarNote message:
                @"Building minicode needs Apple's command line tools. Install them with "
                @"xcode-select --install in the terminal, then try again."];
            return;
        }
        [fm createDirectoryAtPath:clone.stringByDeletingLastPathComponent
      withIntermediateDirectories:YES attributes:nil error:nil];
        [self send:trimmed source:clone clone:YES];
        return;
    }
    [self send:trimmed source:source clone:NO];
}

- (void)send:(NSString *)request source:(NSString *)source clone:(BOOL)clone {
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
    NSString *line = clone
        ? NS(ChangeRequest::cloneThenStartCommand(c.version, command, Std(source), Std(file)))
        : NS(ChangeRequest::startCommand(command, Std(source), Std(file)));
    _source = [source copy];
    _request = [request copy];
    [_bar clearRequest];
    TerminalView *t = _terminal;
    if (t && [_host terminalIsOpen:t] && !t.busy) {
        [_host runCommand:line inTerminal:t focus:YES];
    } else {
        _terminal = [_host changeTerminalRunning:line focus:YES];
    }
    [self watchFrom:MCFileTime(NS(ChangeRequest::buildStamp(Std(source))))];
    NSString *shortReq = request.length > 80
        ? [[request substringToIndex:80] stringByAppendingString:@"…"] : request;
    [self showBar:MCChangeBarWorking message:[NSString stringWithFormat:clone
        ? @"Fetching minicode's source, then Claude starts on “%@” in the terminal. "
          @"minicode offers a restart once it is rebuilt."
        : @"Claude is working on “%@” in the terminal. minicode offers a restart once it is rebuilt.",
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
