// ProcessesPanel.mm — see ProcessesPanel.h.
#import "ProcessesPanel.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "ProcessMonitor.h"

#include "ProcessTable.h"
#include "Settings.h"

static NSColor *PHex(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1];
}
static NSString *PNS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

// ------------------------------------------------------------------ items
@interface MCProcItem : NSObject
@property(nonatomic) int pid, ppid, pgid;
@property(nonatomic) int64_t start;
@property(nonatomic, copy) NSString *name, *command, *cwd, *terminalTitle, *session;
@property(nonatomic) uint64_t memory;
@property(nonatomic) double cpu;
@property(nonatomic, copy) NSArray<NSNumber *> *ports;   // each once, in order
@property(nonatomic) BOOL alive, outside, orphan;
@property(nonatomic) NSInteger terminal;
@property(nonatomic) NSInteger depth;
@property(nonatomic, copy) NSArray<MCProcItem *> *children;
@property(nonatomic, readonly) NSString *key;
@end

@implementation MCProcItem
- (NSString *)key { return [NSString stringWithFormat:@"p:%d:%lld", self.pid, (long long)self.start]; }
@end

@interface MCProcGroupItem : NSObject
@property(nonatomic) Procs::GroupKind kind;
@property(nonatomic) NSInteger terminal;
@property(nonatomic, copy) NSString *title;
@property(nonatomic) uint64_t memory;
@property(nonatomic) int count;
@property(nonatomic, copy) NSArray<MCProcItem *> *children;
@property(nonatomic, readonly) NSString *key;
@end

@implementation MCProcGroupItem
- (NSString *)key { return [NSString stringWithFormat:@"g:%d:%ld", (int)self.kind, (long)self.terminal]; }
@end

static NSString *KindName(Procs::GroupKind k) {
    switch (k) {
    case Procs::GroupKind::Terminal: return @"terminal";
    case Procs::GroupKind::Orphans: return @"orphans";
    case Procs::GroupKind::Outside: return @"outside";
    }
    return @"terminal";
}

static NSString *PortsText(NSArray<NSNumber *> *ports) {
    NSMutableArray *words = [NSMutableArray array];
    for (NSNumber *p in ports) [words addObject:[NSString stringWithFormat:@":%@", p]];
    return [words componentsJoinedByString:@" "];
}

static MCProcItem *MakeItem(const Procs::Node &n, Procs::GroupKind kind, NSInteger depth) {
    const Procs::Proc &p = n.proc;
    MCProcItem *it = [MCProcItem new];
    it.pid = p.key.pid;
    it.start = p.key.start;
    it.ppid = p.ppid;
    it.pgid = p.pgid;
    it.name = PNS(Procs::displayName(p));
    it.command = PNS(p.command);
    it.cwd = PNS(p.cwd);
    it.terminalTitle = PNS(p.terminalTitle);
    it.session = PNS(p.session);
    it.memory = p.memory;
    it.cpu = p.cpu;
    NSMutableArray<NSNumber *> *ports = [NSMutableArray array];
    for (const Procs::Listen &l : p.ports)
        if (![ports containsObject:@(l.port)]) [ports addObject:@(l.port)];
    it.ports = ports;
    it.alive = p.alive;
    it.outside = p.origin == Procs::Origin::Outside;
    it.orphan = kind == Procs::GroupKind::Orphans;
    it.terminal = p.terminal;
    it.depth = depth;
    NSMutableArray *kids = [NSMutableArray array];
    for (const Procs::Node &c : n.children) [kids addObject:MakeItem(c, kind, depth + 1)];
    it.children = kids;
    return it;
}

// --------------------------------------------------------------- row views
@interface MCProcRowView : NSView
@property(nonatomic, strong) id item;
@property(nonatomic, strong) NSColor *textColor;
@end

@implementation MCProcRowView
- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = self.bounds;
    NSColor *text = self.textColor ?: NSColor.textColor;
    NSColor *muted = PHex(0x9CA3AF);
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSMutableParagraphStyle *right = [NSMutableParagraphStyle new];
    right.alignment = NSTextAlignmentRight;
    const NSStringDrawingOptions opts =
        NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine;
    if ([self.item isKindOfClass:MCProcGroupItem.class]) {
        MCProcGroupItem *g = self.item;
        NSString *mem = PNS(Procs::formatBytes(g.memory));
        NSDictionary *memAttrs = @{NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:10.5
                                                                                  weight:NSFontWeightRegular],
                                   NSForegroundColorAttributeName: muted,
                                   NSParagraphStyleAttributeName: right};
        const CGFloat memW = ceil([mem sizeWithAttributes:memAttrs].width) + 4;
        [mem drawWithRect:NSMakeRect(b.size.width - memW - 6, 5, memW, 14) options:opts
               attributes:memAttrs];
        NSMutableAttributedString *t = [[NSMutableAttributedString alloc]
            initWithString:g.title ?: @""
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11.5 weight:NSFontWeightSemibold],
                             NSForegroundColorAttributeName: text,
                             NSParagraphStyleAttributeName: ps}];
        if (g.kind == Procs::GroupKind::Terminal)
            [t appendAttributedString:[[NSAttributedString alloc]
                initWithString:[NSString stringWithFormat:@"  terminal %ld", (long)g.terminal]
                    attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10.5],
                                 NSForegroundColorAttributeName: muted,
                                 NSParagraphStyleAttributeName: ps}]];
        [t drawWithRect:NSMakeRect(2, 4, MAX(0, b.size.width - memW - 14), 16) options:opts];
        return;
    }
    MCProcItem *p = self.item;
    NSColor *dot = !p.alive ? PHex(0x6B7280) : p.orphan ? PHex(0xE8A33D) : PHex(0x73C991);
    [dot setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(2, 8, 7, 7)] fill];
    NSString *mem = p.alive ? PNS(Procs::formatBytes(p.memory)) : @"";
    NSDictionary *memAttrs = @{NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:11
                                                                              weight:NSFontWeightRegular],
                               NSForegroundColorAttributeName: text,
                               NSParagraphStyleAttributeName: right};
    const CGFloat memW = ceil([mem sizeWithAttributes:memAttrs].width) + 4;
    [mem drawWithRect:NSMakeRect(b.size.width - memW - 6, 3, memW, 16) options:opts attributes:memAttrs];
    [p.name drawWithRect:NSMakeRect(14, 2, MAX(0, b.size.width - memW - 24), 17)
                 options:opts
              attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5 weight:NSFontWeightMedium],
                           NSForegroundColorAttributeName: p.alive ? text : muted,
                           NSParagraphStyleAttributeName: ps}];
    NSDictionary *small = @{NSFontAttributeName: [NSFont systemFontOfSize:10.5],
                            NSForegroundColorAttributeName: muted,
                            NSParagraphStyleAttributeName: ps};
    NSMutableAttributedString *line = [[NSMutableAttributedString alloc] init];
    if (!p.alive) {
        [line appendAttributedString:[[NSAttributedString alloc] initWithString:@"exited" attributes:small]];
    } else {
        [line appendAttributedString:[[NSAttributedString alloc]
            initWithString:[NSString stringWithFormat:@"CPU %@", PNS(Procs::formatCpu(p.cpu))]
                attributes:small]];
        if (p.ports.count) {
            NSMutableDictionary *accent = [small mutableCopy];
            accent[NSForegroundColorAttributeName] = PHex(0x4EA1F7);
            [line appendAttributedString:[[NSAttributedString alloc] initWithString:@"  " attributes:small]];
            [line appendAttributedString:[[NSAttributedString alloc] initWithString:PortsText(p.ports)
                                                                         attributes:accent]];
        }
    }
    [line appendAttributedString:[[NSAttributedString alloc]
        initWithString:[NSString stringWithFormat:@"  pid %d", p.pid] attributes:small]];
    if (p.orphan && p.terminal)
        [line appendAttributedString:[[NSAttributedString alloc]
            initWithString:[NSString stringWithFormat:@"  from terminal %ld", (long)p.terminal]
                attributes:small]];
    [line drawWithRect:NSMakeRect(14, 19, MAX(0, b.size.width - 20), 15) options:opts];
}
@end

// ------------------------------------------------------------------ outline
@interface MCProcOutline : NSOutlineView
@property(nonatomic, copy) void (^onActivate)(NSInteger row);
@property(nonatomic, copy) void (^onToggle)(NSInteger row);
@property(nonatomic, copy) void (^onStop)(NSInteger row);
@property(nonatomic, copy) void (^onTab)(void);
@property(nonatomic, copy) NSMenu *(^menuFor)(NSInteger row);
@end

@implementation MCProcOutline
- (void)keyDown:(NSEvent *)event {
    NSEventModifierFlags m = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
    NSString *ch = event.charactersIgnoringModifiers;
    unichar c = ch.length ? [ch characterAtIndex:0] : 0;
    if (!m) {
        if ((c == '\r' || c == 3) && self.onActivate) { self.onActivate(self.selectedRow); return; }
        if (c == ' ' && self.onToggle) { self.onToggle(self.selectedRow); return; }
        if ((c == '\t' || c == 25) && self.onTab) { self.onTab(); return; }
    }
    if (m == NSEventModifierFlagCommand && (c == 0x7f || c == NSDeleteFunctionKey) && self.onStop) {
        self.onStop(self.selectedRow);
        return;
    }
    [super keyDown:event];
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    [super mouseDown:event];
    if (row >= 0 && event.clickCount == 2 && self.onActivate) self.onActivate(row);
}
- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    if (row < 0) row = self.selectedRow;
    return self.menuFor ? self.menuFor(row) : nil;
}
@end

// -------------------------------------------------------------------- panel
@interface MCProcessesPanel () <NSOutlineViewDataSource, NSOutlineViewDelegate>
@end

@implementation MCProcessesPanel {
    NSTextField *_heading, *_summary, *_notice, *_help;
    NSScrollView *_scroll;
    MCProcOutline *_outline;
    NSArray<MCProcGroupItem *> *_groups;
    NSMutableSet<NSString *> *_collapsedGroups, *_expandedProcs;
    NSColor *_text;
    id _observer, _settingsObserver;
    BOOL _appeared;
}

- (instancetype)init {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 260, 400)])) {
        _groups = @[];
        _collapsedGroups = [NSMutableSet set];
        _expandedProcs = [NSMutableSet set];
        self.wantsLayer = YES;
        [self build];
        [self applySettings];
        __weak MCProcessesPanel *weakSelf = self;
        _observer = [NSNotificationCenter.defaultCenter
            addObserverForName:MCProcessMonitorDidSampleNotification object:nil queue:nil
                    usingBlock:^(NSNotification *n) {
                        (void)n;
                        MCProcessesPanel *s = weakSelf;
                        if (s.window) [s reload];
                    }];
        // Colors follow the settings file, as the other sidebar panels' do.
        _settingsObserver = [NSNotificationCenter.defaultCenter
            addObserverForName:MCSettingsDidChangeNotification object:nil queue:nil
                    usingBlock:^(NSNotification *n) {
                        (void)n;
                        [weakSelf applySettings];
                    }];
    }
    return self;
}

- (void)dealloc {
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
    if (_settingsObserver) [NSNotificationCenter.defaultCenter removeObserver:_settingsObserver];
    if (_appeared) [[MCProcessMonitor shared] panelDisappeared];
}

- (BOOL)isFlipped { return YES; }

- (void)build {
    __weak MCProcessesPanel *weakSelf = self;
    _heading = [NSTextField labelWithString:@""];
    _heading.attributedStringValue = [[NSAttributedString alloc]
        initWithString:@"PROCESSES"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: PHex(0x9CA3AF),
                         NSKernAttributeName: @0.6}];
    [self addSubview:_heading];
    _summary = [NSTextField labelWithString:@""];
    _summary.font = [NSFont systemFontOfSize:11];
    _summary.textColor = PHex(0x9CA3AF);
    _summary.alignment = NSTextAlignmentRight;
    _summary.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:_summary];
    _notice = [NSTextField wrappingLabelWithString:
        @"Nothing is running from this window's terminals, or in this folder."];
    _notice.font = [NSFont systemFontOfSize:12];
    _notice.textColor = PHex(0x9CA3AF);
    [self addSubview:_notice];
    _help = [NSTextField wrappingLabelWithString:PNS(Procs::helpText())];
    _help.font = [NSFont systemFontOfSize:10.5];
    _help.textColor = PHex(0x9CA3AF);
    [self addSubview:_help];

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.drawsBackground = NO;
    _scroll.automaticallyAdjustsContentInsets = NO;
    _outline = [[MCProcOutline alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"processes"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_outline addTableColumn:col];
    _outline.outlineTableColumn = col;
    _outline.headerView = nil;
    _outline.backgroundColor = NSColor.clearColor;
    _outline.intercellSpacing = NSMakeSize(0, 0);
    _outline.indentationPerLevel = 12;
    _outline.autoresizesOutlineColumn = NO;   // it widened past the pane as rows nested
    _outline.style = NSTableViewStylePlain;
    _outline.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _outline.dataSource = self;
    _outline.delegate = self;
    _outline.onActivate = ^(NSInteger row) { [weakSelf activateRow:row]; };
    _outline.onToggle = ^(NSInteger row) { [weakSelf toggleRow:row]; };
    _outline.onStop = ^(NSInteger row) { [weakSelf stopRow:row]; };
    _outline.onTab = ^{
        MCProcessesPanel *s = weakSelf;
        if (s.onTab) s.onTab();
    };
    _outline.menuFor = ^NSMenu *(NSInteger row) { return [weakSelf menuForRow:row]; };
    _scroll.documentView = _outline;
    [self addSubview:_scroll];
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    return MCAdjustSettingsMenu(@"tree", self);
}

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Sidebar].CGColor;
    _text = [cfg text:Surface::Sidebar];
    [_outline reloadData];
    [self restoreExpansion];
}

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    (void)old;
    [self layoutPanel];
}

- (void)layoutPanel {
    const CGFloat W = self.bounds.size.width, H = self.bounds.size.height, pad = 10;
    _heading.frame = NSMakeRect(pad, 10, 80, 16);
    _summary.frame = NSMakeRect(pad + 76, 10, MAX(0, W - 2 * pad - 76), 16);
    const CGFloat textW = MAX(40, W - 2 * pad);
    _help.preferredMaxLayoutWidth = textW;
    const CGFloat helpH = ceil([_help.cell cellSizeForBounds:NSMakeRect(0, 0, textW, 10000)].height) + 2;
    _help.frame = NSMakeRect(pad, MAX(32, H - helpH - 8), textW, helpH);
    _notice.preferredMaxLayoutWidth = textW;
    _notice.frame = NSMakeRect(pad, 34, textW, 60);
    _scroll.frame = NSMakeRect(0, 32, W, MAX(0, H - 32 - helpH - 16));
    // One column as wide as the list, so the memory figures stay in sight.
    NSTableColumn *col = _outline.tableColumns.firstObject;
    const CGFloat colW = MAX(60, _scroll.contentSize.width);
    if (fabs(col.width - colW) > 0.5) col.width = colW;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window && !_appeared) {
        _appeared = YES;
        [[MCProcessMonitor shared] panelAppeared];   // looks 2 s apart, one now
        [self layoutPanel];
        [self reload];
    } else if (!self.window && _appeared) {
        _appeared = NO;
        [[MCProcessMonitor shared] panelDisappeared];
    }
}

- (void)focusList {
    if (_outline.selectedRow < 0 && _outline.numberOfRows > 0)
        [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [self.window makeFirstResponder:_outline];
}

- (NSOutlineView *)outline { return _outline; }
- (NSString *)summaryText { return _summary.stringValue ?: @""; }
- (NSString *)helpText { return _help.stringValue ?: @""; }

// ----------------------------------------------------------------- rows
- (void)reload {
    NSString *selected = _outline.selectedRow >= 0
        ? [[_outline itemAtRow:_outline.selectedRow] key] : nil;
    const NSInteger window = self.windowID ? self.windowID() : 0;
    NSString *folder = MCProcessRealPath(self.folder ? self.folder() : @"");
    Procs::View v = [[MCProcessMonitor shared] viewForWindow:window folder:folder];
    NSMutableArray *groups = [NSMutableArray array];
    for (const Procs::Group &g : v.groups) {
        MCProcGroupItem *gi = [MCProcGroupItem new];
        gi.kind = g.kind;
        gi.terminal = g.terminal;
        gi.title = PNS(g.title);
        gi.memory = g.memory;
        gi.count = g.count;
        NSMutableArray *kids = [NSMutableArray array];
        for (const Procs::Node &n : g.roots) [kids addObject:MakeItem(n, g.kind, 0)];
        gi.children = kids;
        [groups addObject:gi];
    }
    _groups = groups;
    [_outline reloadData];
    [self restoreExpansion];
    if (selected) {
        for (NSInteger r = 0; r < _outline.numberOfRows; r++)
            if ([[[_outline itemAtRow:r] key] isEqualToString:selected]) {
                [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r]
                      byExtendingSelection:NO];
                break;
            }
    }
    _summary.stringValue = v.count ? PNS(Procs::summary(v.memory, v.count)) : @"";
    _notice.hidden = _groups.count > 0;
    _scroll.hidden = _groups.count == 0;
}

- (void)restoreExpansion {
    for (NSInteger r = 0; r < _outline.numberOfRows; r++) {
        id item = [_outline itemAtRow:r];
        BOOL want = [item isKindOfClass:MCProcGroupItem.class]
            ? ![_collapsedGroups containsObject:[item key]]
            : [_expandedProcs containsObject:[item key]];
        if (want && ![_outline isItemExpanded:item] && [self childrenOf:item].count)
            [_outline expandItem:item];
    }
}

- (void)expandAll {
    for (NSInteger r = 0; r < _outline.numberOfRows; r++) {
        id item = [_outline itemAtRow:r];
        if ([self childrenOf:item].count && ![_outline isItemExpanded:item]) [_outline expandItem:item];
    }
}

- (NSArray *)childrenOf:(id)item {
    if (!item) return _groups;
    if ([item isKindOfClass:MCProcGroupItem.class]) return ((MCProcGroupItem *)item).children;
    if ([item isKindOfClass:MCProcItem.class]) return ((MCProcItem *)item).children;
    return @[];
}

- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    (void)ov;
    return (NSInteger)[self childrenOf:item].count;
}
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)i ofItem:(id)item {
    (void)ov;
    return [self childrenOf:item][(NSUInteger)i];
}
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item {
    (void)ov;
    return [self childrenOf:item].count > 0;
}
- (CGFloat)outlineView:(NSOutlineView *)ov heightOfRowByItem:(id)item {
    (void)ov;
    return [item isKindOfClass:MCProcItem.class] ? 36 : 24;
}
- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item {
    (void)col;
    MCProcRowView *v = [ov makeViewWithIdentifier:@"proc" owner:self];
    if (!v) {
        v = [[MCProcRowView alloc] initWithFrame:NSZeroRect];
        v.identifier = @"proc";
    }
    v.item = item;
    v.textColor = _text;
    if ([item isKindOfClass:MCProcItem.class]) {
        MCProcItem *p = item;
        NSMutableArray *tip = [NSMutableArray arrayWithObject:p.command ?: @""];
        if (p.cwd.length) [tip addObject:[p.cwd stringByAbbreviatingWithTildeInPath]];
        [tip addObject:[NSString stringWithFormat:@"Process %d, group %d, parent %d", p.pid, p.pgid, p.ppid]];
        if (p.outside) [tip addObject:@"Not started in minicode"];
        else if (p.orphan)
            [tip addObject:[NSString stringWithFormat:@"Its terminal (%ld) was closed", (long)p.terminal]];
        if (p.session.length)
            [tip addObject:[NSString stringWithFormat:@"Started by Claude Code session %@", p.session]];
        if (!p.alive) [tip addObject:@"Exited"];
        v.toolTip = [tip componentsJoinedByString:@"\n"];
    } else {
        MCProcGroupItem *g = item;
        v.toolTip = [NSString stringWithFormat:@"%@ in %d %@", PNS(Procs::formatBytes(g.memory)),
                     g.count, g.count == 1 ? @"process" : @"processes"];
    }
    [v setNeedsDisplay:YES];
    return v;
}
- (void)outlineViewItemDidExpand:(NSNotification *)n {
    id item = n.userInfo[@"NSObject"];
    if ([item isKindOfClass:MCProcGroupItem.class]) [_collapsedGroups removeObject:[item key]];
    else [_expandedProcs addObject:[item key]];
}
- (void)outlineViewItemDidCollapse:(NSNotification *)n {
    id item = n.userInfo[@"NSObject"];
    if ([item isKindOfClass:MCProcGroupItem.class]) [_collapsedGroups addObject:[item key]];
    else [_expandedProcs removeObject:[item key]];
}

- (NSArray<NSString *> *)rowDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    for (NSInteger r = 0; r < _outline.numberOfRows; r++) {
        id item = [_outline itemAtRow:r];
        if ([item isKindOfClass:MCProcGroupItem.class]) {
            MCProcGroupItem *g = item;
            [out addObject:[NSString stringWithFormat:@"G %@ %@ | %@", KindName(g.kind), g.title,
                            PNS(Procs::formatBytes(g.memory))]];
        } else {
            MCProcItem *p = item;
            [out addObject:[NSString stringWithFormat:@"P %ld %d %@ | %@ %@ %@%@", (long)p.depth, p.pid,
                            p.name, PNS(Procs::formatBytes(p.memory)), PNS(Procs::formatCpu(p.cpu)),
                            PortsText(p.ports), p.alive ? @"" : @" exited"]];
        }
    }
    return out;
}

- (NSInteger)rowForPid:(int)pid {
    for (NSInteger r = 0; r < _outline.numberOfRows; r++) {
        id item = [_outline itemAtRow:r];
        if ([item isKindOfClass:MCProcItem.class] && ((MCProcItem *)item).pid == pid &&
            ((MCProcItem *)item).alive)
            return r;
    }
    return -1;
}

// ---------------------------------------------------------------- actions
- (MCProcItem *)procAtRow:(NSInteger)row {
    if (row < 0 || row >= _outline.numberOfRows) return nil;
    id item = [_outline itemAtRow:row];
    return [item isKindOfClass:MCProcItem.class] ? item : nil;
}

- (void)toggleRow:(NSInteger)row {
    if (row < 0 || row >= _outline.numberOfRows) return;
    id item = [_outline itemAtRow:row];
    if (![self childrenOf:item].count) return;
    if ([_outline isItemExpanded:item]) [_outline collapseItem:item];
    else [_outline expandItem:item];
}

- (void)activateRow:(NSInteger)row {
    MCProcItem *p = [self procAtRow:row];
    if (!p) { [self toggleRow:row]; return; }
    if (p.alive && p.ports.count) [self openPortOfRow:row];
    else [self revealOriginOfRow:row];
}

- (void)openPort:(NSNumber *)port {
    if (self.onOpenURL) self.onOpenURL([NSString stringWithFormat:@"http://localhost:%@", port]);
}

- (void)openPortOfRow:(NSInteger)row {
    MCProcItem *p = [self procAtRow:row];
    if (!p.alive || !p.ports.count) { NSBeep(); return; }
    [self openPort:p.ports.firstObject];
}

- (void)note:(NSString *)text {
    if (self.onNote) self.onNote(text);
}

- (void)revealOriginOfRow:(NSInteger)row {
    MCProcItem *p = [self procAtRow:row];
    if (!p) { NSBeep(); return; }
    if (p.outside) {
        [self note:[NSString stringWithFormat:@"%@ was not started in minicode", p.name]];
        return;
    }
    if (!(self.onRevealTerminal && self.onRevealTerminal(p.terminal))) {
        NSBeep();
        [self note:[NSString stringWithFormat:@"The terminal that started %@ was closed", p.name]];
    }
}

- (void)stopRow:(NSInteger)row {
    MCProcItem *p = [self procAtRow:row];
    if (!p || !p.alive) { NSBeep(); return; }
    if (!p.orphan) {
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"Stop %@?", p.name];
        NSString *where = p.outside
            ? @"was not started in minicode"
            : [NSString stringWithFormat:@"was started in terminal %ld (%@)", (long)p.terminal,
               p.terminalTitle.length ? p.terminalTitle : @"its shell"];
        a.informativeText = [NSString stringWithFormat:
            @"“%@” (process %d) %@. It is sent SIGTERM, and SIGKILL 3 seconds later if it is "
            @"still running; what it started stops with it.", p.command, p.pid, where];
        [a addButtonWithTitle:@"Stop"];
        [a addButtonWithTitle:@"Cancel"];
        if ([a runModal] != NSAlertFirstButtonReturn) return;
    }
    __weak MCProcessesPanel *weakSelf = self;
    NSString *name = p.name;
    const int pid = p.pid;
    [[MCProcessMonitor shared] stop:Procs::Key{p.pid, p.start} done:^(NSString *refusal) {
        MCProcessesPanel *s = weakSelf;
        if (refusal) {
            NSBeep();
            [s note:[NSString stringWithFormat:@"%@ was not stopped: %@", name, refusal]];
        } else {
            [s note:[NSString stringWithFormat:@"Stopping %@ (process %d)", name, pid]];
        }
    }];
}

- (NSMenuItem *)item:(NSString *)title in:(NSMenu *)m run:(void (^)(void))block {
    NSMenuItem *it = [m addItemWithTitle:title action:@selector(runMenuBlock:) keyEquivalent:@""];
    it.target = self;
    it.representedObject = [block copy];
    return it;
}
- (void)runMenuBlock:(NSMenuItem *)sender {
    void (^block)(void) = sender.representedObject;
    if (block) block();
}

- (NSMenu *)menuForRow:(NSInteger)row {
    NSMenu *m = [NSMenu new];
    m.autoenablesItems = NO;
    MCProcItem *p = [self procAtRow:row];
    __weak MCProcessesPanel *weakSelf = self;
    if (p) {
        [self item:p.orphan ? @"Stop" : @"Stop…" in:m run:^{ [weakSelf stopRow:row]; }].enabled = p.alive;
        for (NSNumber *port in p.alive ? p.ports : @[])
            [self item:[NSString stringWithFormat:@"Open localhost:%@", port] in:m run:^{
                [weakSelf openPort:port];
            }];
        [self item:@"Reveal Origin" in:m run:^{ [weakSelf revealOriginOfRow:row]; }].enabled =
            !p.outside && !p.orphan;
        [m addItem:[NSMenuItem separatorItem]];
        NSString *command = p.command ?: @"";
        NSString *pid = [NSString stringWithFormat:@"%d", p.pid];
        [self item:@"Copy Command Line" in:m run:^{
            [NSPasteboard.generalPasteboard clearContents];
            [NSPasteboard.generalPasteboard setString:command forType:NSPasteboardTypeString];
        }];
        [self item:@"Copy Process ID" in:m run:^{
            [NSPasteboard.generalPasteboard clearContents];
            [NSPasteboard.generalPasteboard setString:pid forType:NSPasteboardTypeString];
        }];
    }
    MCAddAdjustSettingsItem(m, @"tree", _outline);
    return m;
}

@end
