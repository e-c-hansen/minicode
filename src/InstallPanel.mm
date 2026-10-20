// InstallPanel.mm — Objective-C++. See InstallPanel.h.
#import "InstallPanel.h"
#import "InstallHints.h"
#import "Lsp.h"           // MCFindProgram
#import "LspClient.h"     // Lsp::serverDisplayName
#import "TreeDetails.h"   // MCAppDefaults
#import <objc/runtime.h>

#include <functional>
#include <string>
#include <vector>

static NSColor *IHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}
static NSString *NS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
static std::string Std(NSString *s) { return s.UTF8String ? s.UTF8String : ""; }

NSPasteboard *MCInstallPasteboard(void) {
    NSString *name = NSProcessInfo.processInfo.environment[@"MINICODE_PASTEBOARD"];
    return name.length ? [NSPasteboard pasteboardWithName:name] : [NSPasteboard generalPasteboard];
}

BOOL MCInstallerAvailable(NSString *program) {
    NSString *fake = NSProcessInfo.processInfo.environment[@"MINICODE_INSTALL_HAVE"];
    if (fake) return [[fake componentsSeparatedByString:@","] containsObject:program];
    return MCFindProgram(program) != nil;
}

static std::function<bool(const std::string &)> Have() {
    return [](const std::string &p) { return (bool)MCInstallerAvailable(NS(p)); };
}

// ------------------------------------------------------------- pieces
static NSFont *Mono(void) { return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]; }
static NSFont *Small(void) { return [NSFont systemFontOfSize:11]; }

static NSTextField *Text(NSString *s, NSFont *font, NSColor *color, CGFloat width) {
    NSTextField *f = [NSTextField wrappingLabelWithString:s ?: @""];
    f.font = font;
    f.textColor = color;
    f.selectable = YES;
    f.preferredMaxLayoutWidth = width;
    return f;
}

// "basedpyright's website", or a bare address, as a link.
static NSTextField *Link(NSString *before, NSString *title, NSString *url, CGFloat width) {
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:before ?: @""
            attributes:@{NSFontAttributeName: Small(), NSForegroundColorAttributeName: IHex(0x9CA3AF)}];
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:title
        attributes:@{NSFontAttributeName: Small(), NSLinkAttributeName: [NSURL URLWithString:url],
                     NSForegroundColorAttributeName: IHex(0x4EA1F7)}]];
    NSTextField *f = [NSTextField labelWithAttributedString:s];
    f.allowsEditingTextAttributes = YES;   // makes the link clickable
    f.selectable = YES;
    f.preferredMaxLayoutWidth = width;
    f.lineBreakMode = NSLineBreakByWordWrapping;
    return f;
}

// A button that remembers its command.
@interface MCCommandButton : NSButton
@property(nonatomic, copy) NSString *command;
@end
@implementation MCCommandButton
@end

// A command in a dark box, selectable, with Copy and maybe Run in Terminal.
@interface MCCommandRow : NSStackView
@end
@implementation MCCommandRow
@end

static NSView *CommandBox(NSString *command, CGFloat width) {
    NSView *box = [NSView new];
    box.wantsLayer = YES;
    box.layer.backgroundColor = IHex(0x1A1A1A).CGColor;
    box.layer.borderColor = IHex(0x3C3C3C).CGColor;
    box.layer.borderWidth = 1;
    box.layer.cornerRadius = 4;
    box.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *f = Text(command, Mono(), IHex(0xD4D4D4), width - 14);
    f.translatesAutoresizingMaskIntoConstraints = NO;
    f.identifier = @"command";
    [box addSubview:f];
    [NSLayoutConstraint activateConstraints:@[
        [f.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:7],
        [f.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-7],
        [f.topAnchor constraintEqualToAnchor:box.topAnchor constant:4],
        [f.bottomAnchor constraintEqualToAnchor:box.bottomAnchor constant:-4],
        [box.widthAnchor constraintEqualToConstant:width],
    ]];
    return box;
}

@interface MCInstallActions : NSObject
@property(nonatomic, copy) void (^run)(NSString *command);
@property(nonatomic, copy) void (^after)(void);
@end

@implementation MCInstallActions
- (void)copy:(MCCommandButton *)sender {
    NSPasteboard *pb = MCInstallPasteboard();
    [pb clearContents];
    [pb setString:sender.command forType:NSPasteboardTypeString];
    sender.title = @"Copied";
    __weak MCCommandButton *b = sender;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ b.title = @"Copy"; });
}
- (void)runInTerminal:(MCCommandButton *)sender {
    if (self.run) self.run(sender.command);
    if (self.after) self.after();
}
@end

static MCCommandButton *Button(NSString *title, NSString *command, MCInstallActions *target,
                               SEL action) {
    MCCommandButton *b = [MCCommandButton buttonWithTitle:title target:target action:action];
    b.command = command;
    b.controlSize = NSControlSizeSmall;
    b.font = Small();
    [b setContentHuggingPriority:NSLayoutPriorityRequired
                  forOrientation:NSLayoutConstraintOrientationHorizontal];
    [b setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return b;
}

static const CGFloat kCopyWidth = 62, kRunWidth = 116;

static NSView *Row(NSString *command, CGFloat width, BOOL withRun, MCInstallActions *actions) {
    // The same width with or without Run in Terminal, so the Copy buttons line up.
    CGFloat boxWidth = width - kCopyWidth - kRunWidth;
    NSMutableArray *views = [NSMutableArray arrayWithObject:CommandBox(command, boxWidth)];
    [views addObject:Button(@"Copy", command, actions, @selector(copy:))];
    if (withRun) [views addObject:Button(@"Run in Terminal", command, actions,
                                         @selector(runInTerminal:))];
    MCCommandRow *row = [MCCommandRow stackViewWithViews:views];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeCenterY;
    row.spacing = 6;
    return row;
}

static NSStackView *Column(void) {
    NSStackView *s = [NSStackView new];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

// Every string in the views under `v`, in order.
static void CollectText(NSView *v, NSMutableArray<NSString *> *out) {
    if ([v isKindOfClass:[NSTextField class]]) [out addObject:((NSTextField *)v).stringValue];
    else if ([v isKindOfClass:[NSButton class]]) [out addObject:((NSButton *)v).title];
    NSArray *subs = [v isKindOfClass:[NSStackView class]] ? ((NSStackView *)v).arrangedSubviews
                                                          : v.subviews;
    for (NSView *s in subs) CollectText(s, out);
}

// ----------------------------------------------------------- the view
@implementation MCInstallView {
    MCInstallActions *_actions;
}

- (instancetype)initWithTool:(const Install::Tool *)tool
                       extra:(const std::vector<const Install::Tool *> &)extra
                       title:(NSString *)title
                     message:(NSString *)message
                       width:(CGFloat)width
               runInTerminal:(void (^)(NSString *))run {
    if (!(self = [super initWithFrame:NSMakeRect(0, 0, width, 100)])) return nil;
    _actions = [MCInstallActions new];
    _actions.run = run;
    __weak MCInstallView *weakSelf = self;
    _actions.after = ^{
        MCInstallView *v = weakSelf;
        if (v.onDone) v.onDone();
    };
    const CGFloat inner = width - 28;
    NSStackView *stack = Column();
    stack.spacing = 8;
    stack.edgeInsets = NSEdgeInsetsMake(14, 14, 14, 14);
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    [stack addArrangedSubview:Text(title, [NSFont boldSystemFontOfSize:13], IHex(0xE0E0E0), inner)];
    if (message.length)
        [stack addArrangedSubview:Text(message, Small(), IHex(0xC8C8C8), inner)];
    if (tool) {
        Install::Suggestion s = Install::suggest(*tool, Have(), extra);
        [stack addArrangedSubview:Text(NS(tool->gives), Small(), IHex(0xC8C8C8), inner)];
        NSString *command = NS(s.command);
        if (!s.before.empty()) {
            // The installer is missing too, and Homebrew can get it: one line
            // that does both.
            command = [NSString stringWithFormat:@"%@ && %@", NS(s.before), NS(s.command)];
            [stack addArrangedSubview:Text([NSString stringWithFormat:
                @"It installs with %@, which this Mac does not have yet, so this gets %@ first:",
                NS(s.missing), NS(s.missing)], Small(), IHex(0xC8C8C8), inner)];
        } else if (!s.beforeUrl.empty()) {
            [stack addArrangedSubview:Link([NSString stringWithFormat:
                @"It installs with %@, which this Mac does not have yet: ", NS(s.missing)],
                [NSString stringWithFormat:@"how to get %@", NS(s.missing)], NS(s.beforeUrl),
                inner)];
        } else {
            [stack addArrangedSubview:Text(@"Install it with:", Small(), IHex(0xC8C8C8), inner)];
        }
        _command = [command copy];
        [stack addArrangedSubview:Row(command, inner, run != nil, _actions)];
        NSMutableArray *alts = [NSMutableArray array];
        for (const std::string &a : s.alternatives) [alts addObject:NS(a)];
        _alternatives = [alts copy];
        if (alts.count) {
            [stack addArrangedSubview:Text(alts.count == 1 ? @"Or:" : @"Or one of these:",
                                           Small(), IHex(0x9CA3AF), inner)];
            for (NSString *a in alts) [stack addArrangedSubview:Row(a, inner, NO, _actions)];
        }
        [stack addArrangedSubview:Text(
            @"Run in Terminal types the command into the terminal; press Return there to run "
            @"it. minicode finds the program by itself once it is installed, with no restart.",
            Small(), IHex(0x9CA3AF), inner)];
        [stack addArrangedSubview:Link(@"", [NSString stringWithFormat:@"%@'s website",
                                                                       NS(tool->name)],
                                       NS(tool->homepage), inner)];
    }
    [self addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.topAnchor],
        [stack.widthAnchor constraintEqualToConstant:width],
    ]];
    NSSize size = stack.fittingSize;
    size.width = width;
    [self setFrameSize:size];
    [NSLayoutConstraint activateConstraints:@[
        [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

- (instancetype)initWithServer:(NSString *)key message:(NSString *)message
                         width:(CGFloat)width
                 runInTerminal:(void (^)(NSString *))run {
    NSString *title = [NSString stringWithFormat:@"No %@ language server",
                                                 NS(Lsp::serverDisplayName(Std(key)))];
    return [self initWithTool:Install::server(Std(key)) extra:{} title:title message:message
                        width:width runInTerminal:run];
}

- (instancetype)initWithFormatters:(NSArray<NSString *> *)programs message:(NSString *)message
                             width:(CGFloat)width
                     runInTerminal:(void (^)(NSString *))run {
    const Install::Tool *first = nullptr;
    std::vector<const Install::Tool *> extra;
    for (NSString *p in programs) {
        const Install::Tool *t = Install::formatter(Std(p));
        if (!t) continue;
        if (!first) first = t;
        else extra.push_back(t);
    }
    NSString *title = first ? [NSString stringWithFormat:@"%@ is not installed", NS(first->name)]
                            : @"No formatter found";
    return [self initWithTool:first extra:extra title:title message:message width:width
                runInTerminal:run];
}

- (BOOL)isFlipped { return YES; }

- (void)copyCommand:(NSString *)command {
    MCCommandButton *b = [MCCommandButton new];
    b.command = command;
    [_actions copy:b];
}

- (void)runCommand:(NSString *)command {
    MCCommandButton *b = [MCCommandButton new];
    b.command = command;
    [_actions runInTerminal:b];
}

- (NSString *)shownText {
    NSMutableArray *all = [NSMutableArray array];
    CollectText(self, all);
    return [all componentsJoinedByString:@"\n"];
}
@end

// Rows only, for the Language Servers popover. The actions object lives as
// long as the rows do.
NSView *MCInstallCommandRows(NSString *serverKey, CGFloat width, void (^run)(NSString *)) {
    const Install::Tool *tool = Install::server(Std(serverKey));
    if (!tool) return nil;
    Install::Suggestion s = Install::suggest(*tool, Have());
    MCInstallActions *actions = [MCInstallActions new];
    actions.run = run;
    NSStackView *col = Column();
    col.spacing = 4;
    NSString *command = s.before.empty()
        ? NS(s.command) : [NSString stringWithFormat:@"%@ && %@", NS(s.before), NS(s.command)];
    if (!s.beforeUrl.empty())
        [col addArrangedSubview:Link([NSString stringWithFormat:@"Needs %@: ", NS(s.missing)],
                                     [NSString stringWithFormat:@"how to get %@", NS(s.missing)],
                                     NS(s.beforeUrl), width)];
    [col addArrangedSubview:Row(command, width, run != nil, actions)];
    for (const std::string &a : s.alternatives)
        [col addArrangedSubview:Row(NS(a), width, NO, actions)];
    objc_setAssociatedObject(col, @selector(MCInstallActions), actions,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return col;
}

NSPopover *MCShowInstallPopover(MCInstallView *view, NSView *anchor, NSRect rect,
                                NSRectEdge edge) {
    NSViewController *vc = [NSViewController new];
    vc.view = view;
    vc.preferredContentSize = view.frame.size;
    NSPopover *p = [NSPopover new];
    p.behavior = NSPopoverBehaviorTransient;
    p.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    p.contentViewController = vc;
    __weak NSPopover *weak = p;
    void (^done)(void) = view.onDone;
    view.onDone = ^{
        if (done) done();
        [weak close];
    };
    [p showRelativeToRect:rect ofView:anchor preferredEdge:edge];
    return p;
}

// ------------------------------------------------------------ the bar
const CGFloat kMCInstallBarHeight = 30;

@implementation MCInstallBar {
    NSTextField *_label;
    NSButton *_install, *_mute, *_close;
    NSView *_line;
    NSColor *_textColor;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    _label = [NSTextField labelWithString:@""];
    _label.font = [NSFont systemFontOfSize:12];
    _label.lineBreakMode = NSLineBreakByTruncatingTail;
    _install = [NSButton buttonWithTitle:@"Install…" target:self action:@selector(install:)];
    _install.controlSize = NSControlSizeSmall;
    _install.font = Small();
    _install.bezelStyle = NSBezelStyleRounded;
    _install.keyEquivalent = @"";
    _mute = [NSButton buttonWithTitle:@"Don't Show Again" target:self action:@selector(mute:)];
    _mute.controlSize = NSControlSizeSmall;
    _mute.font = Small();
    _mute.bordered = NO;
    _close = [NSButton buttonWithTitle:@"×" target:self action:@selector(close:)];
    _close.bordered = NO;
    _close.font = [NSFont systemFontOfSize:15];
    _close.toolTip = @"Close (it stays closed until minicode starts again)";
    _line = [NSView new];
    _line.wantsLayer = YES;
    _line.layer.backgroundColor = IHex(0x333333).CGColor;
    for (NSView *v in @[_label, _install, _mute, _close, _line]) [self addSubview:v];
    [self setBackground:IHex(0x252526) text:IHex(0xCCCCCC)];
    return self;
}

- (NSButton *)installButton { return _install; }
- (NSString *)text { return _label.stringValue; }

- (void)setServerKey:(NSString *)key {
    _serverKey = [key copy];
    NSString *language = NS(Lsp::serverDisplayName(Std(key)));
    _label.stringValue = [NSString stringWithFormat:
        @"No %@ language server is installed, so there are no completions or error checks.",
        language];
    _label.toolTip = _label.stringValue;
    [self layout];
}

- (void)setBackground:(NSColor *)background text:(NSColor *)text {
    self.layer.backgroundColor = background.CGColor;
    _textColor = text;
    _label.textColor = text;
    NSDictionary *attrs = @{NSFontAttributeName: Small(),
                            NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.7]};
    _mute.attributedTitle = [[NSAttributedString alloc] initWithString:@"Don't Show Again"
                                                            attributes:attrs];
    _close.attributedTitle = [[NSAttributedString alloc] initWithString:@"×"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:15],
                     NSForegroundColorAttributeName: [text colorWithAlphaComponent:0.7]}];
}

- (void)layout {
    [super layout];
    NSRect b = self.bounds;
    CGFloat x = NSMaxX(b) - 8;
    [_close sizeToFit];
    NSSize c = _close.frame.size;
    x -= c.width;
    _close.frame = NSMakeRect(x, round((NSHeight(b) - c.height) / 2), c.width, c.height);
    [_mute sizeToFit];
    NSSize m = _mute.frame.size;
    x -= m.width + 8;
    _mute.frame = NSMakeRect(x, round((NSHeight(b) - m.height) / 2), m.width, m.height);
    [_install sizeToFit];
    NSSize i = _install.frame.size;
    x -= i.width + 8;
    _install.frame = NSMakeRect(x, round((NSHeight(b) - i.height) / 2), i.width, i.height);
    CGFloat lh = 16;
    _label.frame = NSMakeRect(12, round((NSHeight(b) - lh) / 2), MAX(0, x - 20), lh);
    _line.frame = NSMakeRect(0, 0, NSWidth(b), 1);
}

- (void)install:(NSButton *)sender { if (self.onInstall) self.onInstall(sender); }
- (void)mute:(id)sender { if (self.onMute) self.onMute(); }
- (void)close:(id)sender { if (self.onClose) self.onClose(); }
@end

// ------------------------------------------------------ remembering
static NSString *const kMutedKey = @"MCInstallHintsMuted";

BOOL MCInstallBarMuted(NSString *key) {
    return [[MCAppDefaults() arrayForKey:kMutedKey] containsObject:key];
}

void MCInstallBarSetMuted(NSString *key, BOOL muted) {
    NSMutableArray *all = [[MCAppDefaults() arrayForKey:kMutedKey] mutableCopy]
                              ?: [NSMutableArray array];
    [all removeObject:key];
    if (muted) [all addObject:key];
    [MCAppDefaults() setObject:all forKey:kMutedKey];
}

BOOL MCInstallBarTakeOffer(NSString *key) {
    static NSMutableSet<NSString *> *offered;
    if (!offered) offered = [NSMutableSet set];
    if ([offered containsObject:key]) return NO;
    [offered addObject:key];
    return YES;
}
