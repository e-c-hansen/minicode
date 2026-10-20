// LspPanel.mm — Objective-C++. See LspPanel.h.
#import "LspPanel.h"
#import "Lsp.h"

static NSColor *PHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static NSColor *StateColor(MCLspState state) {
    switch (state) {
    case MCLspStateRunning: return PHex(0x73C991);
    case MCLspStateStarting:
    case MCLspStateRestarting: return PHex(0xCCA700);
    case MCLspStateFailed:
    case MCLspStateCrashed: return PHex(0xF14C4C);
    default: return PHex(0x9CA3AF);
    }
}

// ~/ for the home folder, so paths fit.
static NSString *Short(NSString *path) {
    NSString *home = NSHomeDirectory();
    if (path.length > home.length && [path hasPrefix:[home stringByAppendingString:@"/"]])
        return [@"~" stringByAppendingString:[path substringFromIndex:home.length]];
    return path;
}

// ------------------------------------------------------- MCLspStatusField
@implementation MCLspStatusField {
    NSString *_text;
    NSColor *_dot;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.bezeled = NO;
        self.bordered = NO;
        self.drawsBackground = NO;
        self.editable = NO;
        self.selectable = NO;
        self.toolTip = @"Language servers: click to see them, restart one, or choose "
                       @"the Python interpreter";
    }
    return self;
}

- (void)render {
    NSFont *font = self.font ?: [NSFont systemFontOfSize:11];
    NSColor *color = self.textColor ?: [NSColor labelColor];
    NSMutableParagraphStyle *p = [NSMutableParagraphStyle new];
    p.alignment = self.alignment;
    p.lineBreakMode = self.lineBreakMode;
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    if (_dot && _text.length)
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"● "
            attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: _dot}]];
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:_text ?: @""
        attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color}]];
    [s addAttribute:NSParagraphStyleAttributeName value:p range:NSMakeRange(0, s.length)];
    [super setAttributedStringValue:s];
}

- (void)setTextColor:(NSColor *)color {
    [super setTextColor:color];
    [self render];
}

- (void)setStatus:(NSString *)text dot:(NSColor *)dot {
    _text = [text copy];
    _dot = dot;
    [self render];
    [self.window invalidateCursorRectsForView:self];
}

- (void)resetCursorRects {
    if (_text.length) [self addCursorRect:self.bounds cursor:[NSCursor pointingHandCursor]];
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    (void)event;
    return YES;
}

- (void)mouseDown:(NSEvent *)event {
    if (_text.length && self.onClick) { self.onClick(); return; }
    [super mouseDown:event];
}
@end

// ------------------------------------------------- the servers popover
static const CGFloat kPanelWidth = 540;

static NSTextField *Label(NSString *text, NSFont *font, NSColor *color, BOOL wraps,
                          BOOL selectable) {
    NSTextField *f = wraps ? [NSTextField wrappingLabelWithString:text ?: @""]
                           : [NSTextField labelWithString:text ?: @""];
    f.font = font;
    f.textColor = color;
    f.selectable = selectable;
    if (wraps) f.preferredMaxLayoutWidth = kPanelWidth - 40;
    else f.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [f setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return f;
}

static NSFont *Mono(void) { return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]; }
static NSFont *Small(void) { return [NSFont systemFontOfSize:11]; }

@interface MCServersController : NSViewController
- (instancetype)initWithSession:(LspSession *)session;
@property(nonatomic, copy) void (^onChooseInterpreter)(void);
@end

@implementation MCServersController {
    __weak LspSession *_session;
}

- (instancetype)initWithSession:(LspSession *)session {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        _session = session;
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sessionChanged:)
                                                     name:MCLspSessionDidChangeNotification
                                                   object:session];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)loadView {
    self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kPanelWidth, 200)];
    [self rebuild];
}

- (void)sessionChanged:(NSNotification *)note {
    (void)note;
    if (self.isViewLoaded) [self rebuild];
}

- (NSButton *)button:(NSString *)title action:(SEL)action key:(NSString *)key {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    b.controlSize = NSControlSizeSmall;
    b.font = Small();
    b.identifier = key;
    return b;
}

- (NSView *)cardFor:(MCLspServerInfo *)info {
    NSStackView *card = [NSStackView new];
    card.orientation = NSUserInterfaceLayoutOrientationVertical;
    card.alignment = NSLayoutAttributeLeading;
    card.spacing = 3;

    // Language, state, and the buttons.
    NSMutableAttributedString *head = [[NSMutableAttributedString alloc]
        initWithString:info.language
            attributes:@{NSFontAttributeName: [NSFont boldSystemFontOfSize:13],
                         NSForegroundColorAttributeName: PHex(0xE0E0E0)}];
    [head appendAttributedString:[[NSAttributedString alloc] initWithString:@"   ● "
        attributes:@{NSFontAttributeName: Small(),
                     NSForegroundColorAttributeName: StateColor(info.state)}]];
    NSString *state = info.stateText;
    if (info.serverName.length && info.state != MCLspStateNotInstalled)
        state = [NSString stringWithFormat:@"%@ %@", info.serverName, state];
    if (info.pid) state = [state stringByAppendingFormat:@", PID %d", info.pid];
    if (info.current) state = [state stringByAppendingString:@"  (this file)"];
    [head appendAttributedString:[[NSAttributedString alloc] initWithString:state
        attributes:@{NSFontAttributeName: Small(),
                     NSForegroundColorAttributeName: PHex(0xC8C8C8)}]];
    NSTextField *title = [NSTextField labelWithAttributedString:head];
    title.lineBreakMode = NSLineBreakByTruncatingTail;
    // The title keeps its width; the spacer gives way.
    [title setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh - 1
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [title setContentHuggingPriority:NSLayoutPriorityDefaultHigh
                      forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSView *spacer = [NSView new];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSMutableArray *row = [NSMutableArray arrayWithObjects:title, spacer, nil];
    switch (info.state) {
    case MCLspStateRunning:
    case MCLspStateStarting:
    case MCLspStateRestarting:
        [row addObject:[self button:@"Restart" action:@selector(restart:) key:info.key]];
        [row addObject:[self button:@"Stop" action:@selector(stop:) key:info.key]];
        break;
    case MCLspStateNotInstalled:
        [row addObject:[self button:@"Look Again" action:@selector(start:) key:info.key]];
        break;
    case MCLspStateOff:
        break;
    default:
        if (info.program)
            [row addObject:[self button:@"Start" action:@selector(start:) key:info.key]];
        break;
    }
    NSStackView *header = [NSStackView stackViewWithViews:row];
    header.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    header.distribution = NSStackViewDistributionFill;
    header.spacing = 6;
    [card addArrangedSubview:header];
    [header.widthAnchor constraintEqualToConstant:kPanelWidth - 24].active = YES;

    NSColor *muted = PHex(0x9CA3AF);
    if (info.state == MCLspStateOff) {
        [card addArrangedSubview:Label([NSString stringWithFormat:
            @"Switched off in the settings file (lsp.%@ = off, or lsp.enabled = false).",
            info.key], Small(), muted, YES, NO)];
        return card;
    }
    // What runs.
    NSString *command = info.program
        ? [NSString stringWithFormat:@"%@", Short(info.program)]
        : [NSString stringWithFormat:@"Looked for: %@", info.command];
    if (info.program && info.command.length) {
        NSArray *words = [info.command componentsSeparatedByString:@" "];
        if (words.count > 1)
            command = [command stringByAppendingFormat:@" %@",
                          [[words subarrayWithRange:NSMakeRange(1, words.count - 1)]
                              componentsJoinedByString:@" "]];
    }
    [card addArrangedSubview:Label(command, Mono(), muted, YES, YES)];
    if (info.installHint.length)
        [card addArrangedSubview:Label([@"Install: " stringByAppendingString:info.installHint],
                                       Mono(), PHex(0xD4D4D4), YES, YES)];
    if ([info.key isEqualToString:@"python"]) {
        NSString *interp = info.interpreter
            ? [NSString stringWithFormat:@"Interpreter: %@ (%@)", Short(info.interpreter),
                                         info.interpreterSource]
            : [NSString stringWithFormat:@"Interpreter: the server's own choice (%@)",
                                         info.interpreterSource ?: @"automatic"];
        NSTextField *l = Label(interp, Small(), PHex(0xC8C8C8), NO, YES);
        NSView *gap = [NSView new];
        [gap setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSStackView *line = [NSStackView stackViewWithViews:@[
            l, gap, [self button:@"Change…" action:@selector(changeInterpreter:) key:info.key]]];
        line.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        line.distribution = NSStackViewDistributionFill;
        line.spacing = 6;
        [line.widthAnchor constraintEqualToConstant:kPanelWidth - 24].active = YES;
        [card addArrangedSubview:line];
    }
    if (info.message.length) {
        BOOL bad = info.state == MCLspStateFailed || info.state == MCLspStateCrashed ||
                   info.state == MCLspStateRestarting;
        [card addArrangedSubview:Label(info.message, Small(), bad ? PHex(0xF48771) : muted,
                                       YES, YES)];
    }
    return card;
}

- (void)rebuild {
    LspSession *session = _session;
    for (NSView *v in [self.view.subviews copy]) [v removeFromSuperview];
    NSStackView *stack = [NSStackView new];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 10;
    stack.edgeInsets = NSEdgeInsetsMake(12, 12, 12, 12);
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    NSString *folder = session.root.lastPathComponent ?: @"";
    [stack addArrangedSubview:Label([NSString stringWithFormat:@"Language servers for %@", folder],
                                    [NSFont boldSystemFontOfSize:12], PHex(0xE0E0E0), NO, NO)];
    NSArray<MCLspServerInfo *> *infos = session.serverInfos;
    for (NSUInteger i = 0; i < infos.count; i++) {
        if (i > 0) {
            NSBox *line = [NSBox new];
            line.boxType = NSBoxSeparator;
            [stack addArrangedSubview:line];
            [line.widthAnchor constraintEqualToConstant:kPanelWidth - 24].active = YES;
        }
        [stack addArrangedSubview:[self cardFor:infos[i]]];
    }
    NSTextField *note = Label(@"Commands come from the lsp lines in the settings file, and "
                              @"python.interpreter sets the default Python. MiniCode never "
                              @"installs a server itself.",
                              Small(), PHex(0x9CA3AF), YES, NO);
    NSButton *settings = [NSButton buttonWithTitle:@"Open Settings" target:self
                                            action:@selector(openSettings:)];
    settings.controlSize = NSControlSizeSmall;
    settings.font = Small();
    [stack addArrangedSubview:note];
    [stack addArrangedSubview:settings];

    [self.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [stack.widthAnchor constraintEqualToConstant:kPanelWidth],
    ]];
    NSSize size = stack.fittingSize;
    size.width = kPanelWidth;
    [self.view setFrameSize:size];
    self.preferredContentSize = size;
}

- (void)restart:(NSButton *)sender { [_session restartServer:sender.identifier]; }
- (void)stop:(NSButton *)sender { [_session stopServer:sender.identifier]; }
- (void)start:(NSButton *)sender { [_session startServer:sender.identifier]; }
- (void)changeInterpreter:(id)sender {
    (void)sender;
    if (self.onChooseInterpreter) self.onChooseInterpreter();
}
- (void)openSettings:(id)sender {
    [NSApp sendAction:@selector(openSettings:) to:nil from:sender];
}
@end

static NSPopover *gServersPopover;

NSView *MCLanguageServersView(LspSession *session) {
    static MCServersController *keep;
    keep = [[MCServersController alloc] initWithSession:session];
    NSView *v = keep.view;
    v.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    return v;
}

void MCShowLanguageServers(LspSession *session, NSView *view) {
    if (gServersPopover.shown) {
        [gServersPopover close];
        return;
    }
    MCServersController *vc = [[MCServersController alloc] initWithSession:session];
    __weak NSWindow *window = view.window;
    // Weak: the popover lives on in gServersPopover after it closes, and a
    // strong session here kept a closed window's session (and its text view)
    // alive.
    __weak LspSession *weakSession = session;
    vc.onChooseInterpreter = ^{
        [gServersPopover close];
        LspSession *s = weakSession;
        if (s) MCChoosePythonInterpreter(s, window);
    };
    gServersPopover = [NSPopover new];
    gServersPopover.behavior = NSPopoverBehaviorTransient;
    gServersPopover.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    gServersPopover.contentViewController = vc;
    [gServersPopover showRelativeToRect:view.bounds ofView:view preferredEdge:NSRectEdgeMaxY];
}

// ------------------------------------------------- the interpreter picker
@interface MCInterpreterPicker () <NSTableViewDataSource, NSTableViewDelegate>
@end

static MCInterpreterPicker *gPicker;   // kept while its sheet is up

@implementation MCInterpreterPicker {
    __weak LspSession *_session;
    NSArray<MCPythonCandidate *> *_candidates;
    NSString *_automaticLine;
    NSTableView *_table;
}

- (instancetype)initWithSession:(LspSession *)session {
    if (!(self = [super init])) return nil;
    _session = session;
    _candidates = session.pythonCandidates;
    NSString *source = nil;
    NSString *auto_ = [session defaultPythonInterpreterWithSource:&source];
    _automaticLine = auto_ ? [NSString stringWithFormat:@"%@ (%@)", Short(auto_), source]
                           : [NSString stringWithFormat:@"the language server's own choice (%@)",
                                                        source];
    [self build];
    NSString *chosen = session.chosenPythonInterpreter.stringByExpandingTildeInPath;
    NSInteger row = 0;
    for (NSUInteger i = 0; i < _candidates.count; i++)
        if ([_candidates[i].path isEqualToString:chosen]) row = (NSInteger)i + 1;
    [self selectRow:row];
    return self;
}

- (void)build {
    const CGFloat W = 640, H = 420;
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered defer:YES];
    _panel.releasedWhenClosed = NO;
    NSView *c = _panel.contentView;

    NSString *folder = _session.root.lastPathComponent ?: @"this folder";
    NSTextField *title = [NSTextField labelWithString:
        [NSString stringWithFormat:@"Python interpreter for %@", folder]];
    title.font = [NSFont boldSystemFontOfSize:13];
    title.frame = NSMakeRect(20, H - 38, W - 40, 20);
    [c addSubview:title];
    NSTextField *about = [NSTextField wrappingLabelWithString:
        @"The Python language server analyses your code with this interpreter, so it finds "
        @"the packages installed there. The choice is kept for this folder; "
        @"python.interpreter in the settings file is the default for folders without one."];
    about.font = [NSFont systemFontOfSize:11];
    about.textColor = [NSColor secondaryLabelColor];
    about.frame = NSMakeRect(20, H - 82, W - 40, 40);
    [c addSubview:about];

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 56, W - 40, H - 148)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"python"];
    col.width = W - 44;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = 36;
    _table.style = NSTableViewStylePlain;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(commit);
    scroll.documentView = _table;
    [c addSubview:scroll];

    NSButton *other = [NSButton buttonWithTitle:@"Other…" target:self action:@selector(other:)];
    other.frame = NSMakeRect(16, 14, 100, 30);
    [c addSubview:other];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    cancel.keyEquivalent = @"\e";
    cancel.frame = NSMakeRect(W - 216, 14, 100, 30);
    [c addSubview:cancel];
    NSButton *select = [NSButton buttonWithTitle:@"Select" target:self action:@selector(commit)];
    select.keyEquivalent = @"\r";
    select.frame = NSMakeRect(W - 116, 14, 100, 30);
    [c addSubview:select];
    _panel.initialFirstResponder = _table;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_candidates.count + 1;
}

- (NSAttributedString *)rowText:(NSInteger)row {
    NSString *name, *about, *path;
    if (row == 0) {
        name = @"Automatic";
        about = @"no choice of this folder's own";
        path = [@"Uses " stringByAppendingString:_automaticLine];
    } else {
        MCPythonCandidate *c = _candidates[(NSUInteger)row - 1];
        name = c.name;
        about = c.version.length ? [NSString stringWithFormat:@"%@, %@", c.version, c.kind] : c.kind;
        path = Short(c.path);
    }
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc] initWithString:name
        attributes:@{NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                     NSForegroundColorAttributeName: [NSColor labelColor]}];
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:[NSString stringWithFormat:@"   %@\n", about]
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                         NSForegroundColorAttributeName: [NSColor secondaryLabelColor]}]];
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:path
        attributes:@{NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10.5
                                                                       weight:NSFontWeightRegular],
                     NSForegroundColorAttributeName: [NSColor secondaryLabelColor]}]];
    // Each line stays one line: a long path loses its middle, never wraps
    // at a slash onto a third line the row has no room for.
    NSMutableParagraphStyle *p = [NSMutableParagraphStyle new];
    p.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [s addAttribute:NSParagraphStyleAttributeName value:p range:NSMakeRange(0, s.length)];
    return s;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
    NSTextField *f = [tv makeViewWithIdentifier:@"row" owner:self];
    if (!f) {
        f = [NSTextField labelWithString:@""];
        f.identifier = @"row";
        f.lineBreakMode = NSLineBreakByTruncatingMiddle;
        f.maximumNumberOfLines = 2;
    }
    f.attributedStringValue = [self rowText:row];
    return f;
}

- (NSArray<NSString *> *)rowTitles {
    NSMutableArray *out = [NSMutableArray array];
    for (NSInteger i = 0; i <= (NSInteger)_candidates.count; i++)
        [out addObject:[self rowText:i].string];
    return out;
}

- (NSInteger)selectedRow { return _table.selectedRow; }

- (void)selectRow:(NSInteger)row {
    if (row < 0 || row > (NSInteger)_candidates.count) return;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];
}

- (void)close {
    if (_panel.sheetParent) [_panel.sheetParent endSheet:_panel];
    else [_panel orderOut:nil];
    if (gPicker == self) gPicker = nil;
}

- (void)commit {
    NSInteger row = _table.selectedRow;
    if (row < 0) return;
    [_session choosePythonInterpreter:row == 0 ? nil : _candidates[(NSUInteger)row - 1].path];
    [self close];
}

- (void)cancel:(id)sender {
    (void)sender;
    [self close];
}

- (void)other:(id)sender {
    (void)sender;
    NSOpenPanel *open = [NSOpenPanel openPanel];
    open.message = @"Choose a Python interpreter (an environment's bin/python)";
    open.canChooseFiles = YES;
    open.canChooseDirectories = NO;
    open.showsHiddenFiles = YES;
    open.treatsFilePackagesAsDirectories = YES;
    open.resolvesAliases = NO;   // keep .venv/bin/python, not the Python it links to
    __weak MCInterpreterPicker *weakSelf = self;
    [open beginSheetModalForWindow:_panel completionHandler:^(NSModalResponse r) {
        MCInterpreterPicker *s = weakSelf;
        if (r != NSModalResponseOK || !s) return;
        // An environment's bin/python is often a symlink to the Python it
        // was made from; keep the environment's own path.
        NSString *path = open.URL.path;
        if (![[NSFileManager defaultManager] isExecutableFileAtPath:path]) {
            NSBeep();
            return;
        }
        [s->_session choosePythonInterpreter:path];
        [s close];
    }];
}
@end

void MCChoosePythonInterpreter(LspSession *session, NSWindow *window) {
    if (gPicker) return;   // one at a time
    gPicker = [[MCInterpreterPicker alloc] initWithSession:session];
    if (window) [window beginSheet:gPicker.panel completionHandler:nil];
    else [gPicker.panel makeKeyAndOrderFront:nil];
}
