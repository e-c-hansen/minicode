// AgentsPanel.mm — see AgentsPanel.h.
#import "AgentsPanel.h"
#import "AgentModel.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "TreeDetails.h"
#import "TerminalAttention.h"
#import "Terminal.h"
#import "ClaudeHooksOffer.h"
#import "UsageView.h"

#include "AgentView.h"
#include "Settings.h"

static NSColor *AHex(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1];
}
static NSString *ANS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}
static std::string AU8(NSString *s) { return s.UTF8String ? std::string(s.UTF8String) : ""; }

static NSString *KeyOf(id item) {
    if ([item isKindOfClass:MCAgentTask.class]) return [@"task:" stringByAppendingString:((MCAgentTask *)item).path];
    if ([item isKindOfClass:MCAgentWork.class]) return ((MCAgentWork *)item).transcript;
    if ([item isKindOfClass:MCAgentFileGroup.class]) {
        MCAgentFileGroup *g = item;
        return [NSString stringWithFormat:@"%@#%ld", g.transcript, (long)g.place];
    }
    if ([item isKindOfClass:MCAgentFile.class]) {
        MCAgentFile *f = item;
        return [NSString stringWithFormat:@"%@#%ld#%@", f.transcript, (long)f.place, f.path];
    }
    return @"";
}

// The row's second line, from the core's words and the tree's ages.
static NSString *DetailOf(MCAgentWork *s, NSDate *now) {
    NSTimeInterval t = MAX(s.lastActivity, s.written);
    NSString *age = t > 0 ? MCCompactAge([NSDate dateWithTimeIntervalSince1970:t], now) : @"";
    return ANS(AgentView::detailLine([s runningAt:now], AU8(age), AU8(s.whereText),
                                     (int)s.filesTouched, (int)s.bashCommands));
}

// A task's second line: its branch and how long ago it started.
static NSString *TaskDetailOf(MCAgentTask *t, NSDate *now) {
    NSString *age = MCCompactAge([NSDate dateWithTimeIntervalSince1970:t.started], now);
    return [NSString stringWithFormat:@"starting · %@%@", t.name,
            age.length ? [@" · " stringByAppendingString:age] : @""];
}

// ---------------------------------------------------------------- one row
@interface MCAgentRowView : NSView
@property(nonatomic, strong) id item;
@property(nonatomic, strong) NSColor *textColor;
@property(nonatomic, strong) NSDate *now;
@end

@implementation MCAgentRowView
- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = self.bounds;
    NSColor *text = self.textColor ?: NSColor.textColor;
    NSColor *muted = AHex(0x9D9D9D);
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    const NSStringDrawingOptions opts =
        NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine;
    if ([self.item isKindOfClass:MCAgentTask.class]) {
        MCAgentTask *t = self.item;
        // A ring, not a dot: started, its session not seen yet.
        NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(2.5, 8.5, 6, 6)];
        ring.lineWidth = 1.2;
        [AHex(0x4EA1F7) setStroke];
        [ring stroke];
        NSString *title = [t.prompt componentsSeparatedByCharactersInSet:
                               NSCharacterSet.newlineCharacterSet].firstObject;
        if (!title.length) title = t.name ?: @"";
        [title drawWithRect:NSMakeRect(14, 2, MAX(0, b.size.width - 16), 17)
                    options:opts
                 attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5
                                                                    weight:NSFontWeightMedium],
                              NSForegroundColorAttributeName: text,
                              NSParagraphStyleAttributeName: ps}];
        [TaskDetailOf(t, self.now ?: [NSDate date])
            drawWithRect:NSMakeRect(14, 19, MAX(0, b.size.width - 16), 15)
                 options:opts
              attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10.5],
                           NSForegroundColorAttributeName: muted,
                           NSParagraphStyleAttributeName: ps}];
        return;
    }
    if ([self.item isKindOfClass:MCAgentWork.class]) {
        MCAgentWork *s = self.item;
        const BOOL running = [s runningAt:self.now ?: [NSDate date]];
        NSColor *dot = running ? AHex(0x73C991) : AHex(0x858585);
        [dot setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(2, 8, 7, 7)] fill];
        [s.name drawWithRect:NSMakeRect(14, 2, MAX(0, b.size.width - 16), 17)
                     options:opts
                  attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12.5
                                                                     weight:NSFontWeightMedium],
                               NSForegroundColorAttributeName: text,
                               NSParagraphStyleAttributeName: ps}];
        [DetailOf(s, self.now ?: [NSDate date])
            drawWithRect:NSMakeRect(14, 19, MAX(0, b.size.width - 16), 15)
                 options:opts
              attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10.5],
                           NSForegroundColorAttributeName: muted,
                           NSParagraphStyleAttributeName: ps}];
        return;
    }
    if ([self.item isKindOfClass:MCAgentFileGroup.class]) {
        MCAgentFileGroup *g = self.item;
        NSString *title = [NSString stringWithFormat:@"%@  %lu", g.title.uppercaseString,
                           (unsigned long)g.files.count];
        [title drawWithRect:NSMakeRect(2, 4, MAX(0, b.size.width - 4), 14)
                    options:opts
                 attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:9.5
                                                                    weight:NSFontWeightSemibold],
                              NSForegroundColorAttributeName: muted,
                              NSKernAttributeName: @0.5,
                              NSParagraphStyleAttributeName: ps}];
        return;
    }
    if ([self.item isKindOfClass:MCAgentFile.class]) {
        MCAgentFile *f = self.item;
        NSMutableDictionary *name = [@{NSFontAttributeName: [NSFont systemFontOfSize:12],
                                       NSForegroundColorAttributeName: f.exists ? text : muted,
                                       NSParagraphStyleAttributeName: ps} mutableCopy];
        if (!f.exists) {
            name[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
            name[NSStrikethroughColorAttributeName] = muted;
        }
        // The file's name stays in sight: a name wider than the row gives
        // way in its middle (keeping its extension), and the folder after
        // it takes what is left, giving way in its middle too.
        NSMutableParagraphStyle *middle = [NSMutableParagraphStyle new];
        middle.lineBreakMode = NSLineBreakByTruncatingMiddle;
        name[NSParagraphStyleAttributeName] = middle;
        NSString *fileName = f.path.lastPathComponent ?: @"";
        const CGFloat avail = MAX(0, b.size.width - 4);
        const CGFloat nameW = MIN(avail, ceil([fileName sizeWithAttributes:name].width) + 1);
        [fileName drawWithRect:NSMakeRect(2, 3, nameW, 16) options:opts attributes:name];
        NSString *dir = f.folderText;
        const CGFloat gap = 8, dirX = 2 + nameW + gap;
        if (dir.length && dirX + 20 < b.size.width - 2)
            [dir drawWithRect:NSMakeRect(dirX, 4, b.size.width - 2 - dirX, 15)
                      options:opts
                   attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10.5],
                                NSForegroundColorAttributeName: muted,
                                NSParagraphStyleAttributeName: middle}];
    }
}
@end

// ------------------------------------------------------------------ outline
@interface MCAgentsOutline : NSOutlineView
@property(nonatomic, copy) void (^onActivate)(NSInteger row);
@property(nonatomic, copy) void (^onToggle)(NSInteger row);
@property(nonatomic, copy) void (^onClick)(NSInteger row);
@property(nonatomic, copy) void (^onTab)(void);
@property(nonatomic, copy) NSMenu *(^menuFor)(NSInteger row);
@end

@implementation MCAgentsOutline
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
    [super keyDown:event];
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    BOOL onTriangle = row >= 0 && NSPointInRect(p, [self frameOfOutlineCellAtRow:row]);
    [super mouseDown:event];
    if (row >= 0 && !onTriangle && event.clickCount == 1 && self.onClick) self.onClick(row);
}
- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    if (row < 0) row = self.selectedRow;
    return self.menuFor ? self.menuFor(row) : nil;
}
@end

// -------------------------------------------------------------------- panel
@interface MCAgentsPanel () <NSOutlineViewDataSource, NSOutlineViewDelegate>
@end

@implementation MCAgentsPanel {
    NSTextField *_heading, *_summary, *_notice;
    NSButton *_hooks;
    MCUsageView *_usage;
    NSScrollView *_scroll;
    MCAgentsOutline *_outline;
    NSArray<MCAgentWork *> *_sessions;
    NSArray<MCAgentTask *> *_tasks;   // started here, no session yet
    NSMutableSet<NSString *> *_expanded;
    NSColor *_text;
    NSDate *_now;
    NSTimer *_timer;
    id _observer;
}

- (instancetype)initWithModel:(MCAgentModel *)model {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 260, 400)])) {
        _model = model;
        _sessions = @[];
        _tasks = @[];
        _expanded = [NSMutableSet set];
        _now = [NSDate date];
        self.wantsLayer = YES;
        [self build];
        [self applySettings];
        __weak MCAgentsPanel *weakSelf = self;
        _observer = [NSNotificationCenter.defaultCenter
            addObserverForName:MCAgentModelDidChangeNotification object:model queue:nil
                    usingBlock:^(NSNotification *n) { (void)n; [weakSelf reload]; }];
        [self reload];
    }
    return self;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    if (_observer) [NSNotificationCenter.defaultCenter removeObserver:_observer];
    [_timer invalidate];
}

- (BOOL)isFlipped { return YES; }

- (void)build {
    __weak MCAgentsPanel *weakSelf = self;
    _heading = [NSTextField labelWithString:@""];
    _heading.attributedStringValue = [[NSAttributedString alloc]
        initWithString:@"AGENTS"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: AHex(0x9D9D9D),
                         NSKernAttributeName: @0.6}];
    [self addSubview:_heading];
    _summary = [NSTextField labelWithString:@""];
    _summary.font = [NSFont systemFontOfSize:11];
    _summary.textColor = AHex(0x9D9D9D);
    _summary.alignment = NSTextAlignmentRight;
    _summary.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:_summary];
    _hooks = [NSButton buttonWithTitle:@"" target:self action:@selector(offerHooks:)];
    _hooks.bordered = NO;
    _hooks.alignment = NSTextAlignmentLeft;
    _hooks.toolTip = @"Adds a hook to Claude Code's settings, after showing you what it adds, so "
                      "a terminal's tab gets an orange dot while its session waits for you.";
    [self setHooksTitle];
    [self addSubview:_hooks];
    _usage = [[MCUsageView alloc] initWithFrame:NSMakeRect(0, 0, 240, 80)];
    _usage.onHeightChange = ^{ [weakSelf layoutPanel]; };
    [self addSubview:_usage];
        _notice = [NSTextField wrappingLabelWithString:@""];
    _notice.font = [NSFont systemFontOfSize:12];
    _notice.textColor = AHex(0x9D9D9D);
    _notice.selectable = YES;
    [self addSubview:_notice];

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.drawsBackground = NO;
    _scroll.automaticallyAdjustsContentInsets = NO;
    _outline = [[MCAgentsOutline alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"agents"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_outline addTableColumn:col];
    _outline.outlineTableColumn = col;
    _outline.headerView = nil;
    _outline.backgroundColor = NSColor.clearColor;
    _outline.intercellSpacing = NSMakeSize(0, 0);
    _outline.indentationPerLevel = 12;
    _outline.style = NSTableViewStylePlain;
    _outline.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    // Expanding a session added file rows that widened the column past the
    // pane, so every title's "…" was drawn off screen; the column is kept
    // at the list's width (layoutPanel), as the Processes panel's is.
    _outline.autoresizesOutlineColumn = NO;
    _outline.dataSource = self;
    _outline.delegate = self;
    _outline.onActivate = ^(NSInteger row) { [weakSelf activateRow:row]; };
    _outline.onToggle = ^(NSInteger row) { [weakSelf toggleRow:row]; };
    _outline.onClick = ^(NSInteger row) { [weakSelf clickRow:row]; };
    _outline.onTab = ^{
        MCAgentsPanel *s = weakSelf;
        if (s.onTab) s.onTab();
    };
    _outline.menuFor = ^NSMenu *(NSInteger row) { return [weakSelf menuForRow:row]; };
    _scroll.documentView = _outline;
    [self addSubview:_scroll];
    // A scroll bar that comes or goes (rows added, legacy scrollers) changes
    // the width the list shows.
    _scroll.contentView.postsFrameChangedNotifications = YES;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(clipResized:)
                                               name:NSViewFrameDidChangeNotification
                                             object:_scroll.contentView];
}

- (void)clipResized:(NSNotification *)n {
    (void)n;
    [self fitColumn];
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    return MCAdjustSettingsMenu(@"tree", self);
}

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Sidebar].CGColor;
    _text = [cfg text:Surface::Sidebar];
    [_usage applyTextColor:_text];
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
    _summary.frame = NSMakeRect(pad + 60, 10, MAX(0, W - 2 * pad - 60), 16);
    const CGFloat usageTop = _hooks.hidden ? 32 : 52;
    _hooks.frame = NSMakeRect(pad - 2, 30, MAX(0, W - 2 * pad + 2), 18);
    const CGFloat usageW = MAX(40, W - 2 * pad);
    const CGFloat usageH = [_usage heightForWidth:usageW];
    _usage.frame = NSMakeRect(pad, usageTop + 4, usageW, usageH);
    const CGFloat top = usageTop + 4 + usageH + 6;
    _notice.preferredMaxLayoutWidth = MAX(40, W - 2 * pad);
    _notice.frame = NSMakeRect(pad, top + 2, MAX(40, W - 2 * pad), 80);
    _scroll.frame = NSMakeRect(0, top, W, MAX(0, H - top));
    [self fitColumn];
}

// One column exactly as wide as what the list shows, so a row's text ends
// in "…" at the pane's edge, expanded or not.
- (void)fitColumn {
    NSTableColumn *col = _outline.tableColumns.firstObject;
    const CGFloat colW = MAX(60, _scroll.contentSize.width);
    if (fabs(col.width - colW) > 0.5) col.width = colW;
}

- (void)setHooksTitle {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    _hooks.attributedTitle = [[NSAttributedString alloc]
        initWithString:@"Show which session is waiting on you…"
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11.5],
                         NSForegroundColorAttributeName: AHex(0x4EA1F7),
                         NSParagraphStyleAttributeName: ps}];
}

- (NSButton *)hooksButton { return _hooks; }
- (MCUsageView *)usageView { return _usage; }

// The line shows only while the hooks are missing; the file is small and
// read on each reload (the model's changes and the 30 s timer).
- (void)updateHooksLine {
    const BOOL hide = MCClaudeHooksInstalled();
    if (_hooks.hidden == hide) return;
    _hooks.hidden = hide;
    [self layoutPanel];
}

- (void)offerHooks:(id)sender {
    (void)sender;
    __weak MCAgentsPanel *weakSelf = self;
    MCInstallClaudeHooks(self.window, ^(BOOL installed) {
        (void)installed;
        [weakSelf updateHooksLine];
    });
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [_timer invalidate];
    _timer = nil;
    if (!self.window) return;
    [self layoutPanel];
    [self.model refresh];
    __weak MCAgentsPanel *weakSelf = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:30 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf reload];
    }];
}

- (void)focusList {
    if (_outline.selectedRow < 0 && _outline.numberOfRows > 0)
        [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [self.window makeFirstResponder:_outline];
}

- (NSOutlineView *)outline { return _outline; }
- (NSString *)noticeText { return _notice.hidden ? @"" : _notice.stringValue; }
- (NSString *)summaryText { return _summary.stringValue ?: @""; }

// ----------------------------------------------------------------- rows
- (void)reload {
    _now = [NSDate date];
    NSString *selected = _outline.selectedRow >= 0 ? KeyOf([_outline itemAtRow:_outline.selectedRow]) : nil;
    _sessions = self.model.sessions ?: @[];
    _tasks = [self.model pendingTasks] ?: @[];
    [_outline reloadData];
    [self restoreExpansion];
    if (selected) {
        for (NSInteger r = 0; r < _outline.numberOfRows; r++)
            if ([KeyOf([_outline itemAtRow:r]) isEqualToString:selected]) {
                [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r]
                      byExtendingSelection:NO];
                break;
            }
    }
    NSUInteger running = 0, total = 0;
    NSMutableArray<MCAgentWork *> *all = [_sessions mutableCopy];
    while (all.count) {
        MCAgentWork *s = all.lastObject;
        [all removeLastObject];
        total++;
        if ([s runningAt:_now]) running++;
        [all addObjectsFromArray:s.subagents];
    }
    NSString *starting = _tasks.count
        ? [NSString stringWithFormat:@"%lu starting · ", (unsigned long)_tasks.count] : @"";
    _summary.stringValue = total == 0 ? (_tasks.count ? [starting substringToIndex:starting.length - 3] : @"")
        : [starting stringByAppendingString:[NSString stringWithFormat:@"%@%lu %@", running ? [NSString stringWithFormat:@"%lu running · ",
                                                            (unsigned long)running] : @"",
           (unsigned long)total, total == 1 ? @"session" : @"sessions"]];
    _notice.stringValue = ANS(AgentView::emptyText(self.model.projectsMissing, self.model.loaded,
                                                   AU8([self.model.projectsDir
                                                       stringByAbbreviatingWithTildeInPath])));
    _notice.hidden = _sessions.count + _tasks.count > 0;
    _scroll.hidden = _sessions.count + _tasks.count == 0;
    [self updateHooksLine];
    [_usage refresh];
}

- (void)restoreExpansion {
    // Expanding a row adds rows below it, so walk while the count grows.
    for (NSInteger r = 0; r < _outline.numberOfRows; r++) {
        id item = [_outline itemAtRow:r];
        if ([_expanded containsObject:KeyOf(item)] && ![_outline isItemExpanded:item])
            [_outline expandItem:item];
    }
}

- (NSArray *)childrenOf:(id)item {
    if (!item) return [(NSArray *)_tasks arrayByAddingObjectsFromArray:_sessions];
    if ([item isKindOfClass:MCAgentWork.class]) {
        MCAgentWork *s = item;
        return [(NSArray *)s.groups arrayByAddingObjectsFromArray:(NSArray *)s.subagents];
    }
    if ([item isKindOfClass:MCAgentFileGroup.class]) return ((MCAgentFileGroup *)item).files;
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
    return [item isKindOfClass:MCAgentWork.class] || [item isKindOfClass:MCAgentTask.class] ? 38 : 21;
}
- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item {
    (void)col;
    MCAgentRowView *v = [ov makeViewWithIdentifier:@"agent" owner:self];
    if (!v) {
        v = [[MCAgentRowView alloc] initWithFrame:NSZeroRect];
        v.identifier = @"agent";
    }
    v.item = item;
    v.textColor = _text;
    v.now = _now;
    if ([item isKindOfClass:MCAgentFile.class]) {
        MCAgentFile *f = item;
        v.toolTip = [NSString stringWithFormat:@"%@\n%ld %@, %ld %@%@%@\nClick: what the session changed", f.path, (long)f.writes,
                     f.writes == 1 ? @"write" : @"writes", (long)f.edits,
                     f.edits == 1 ? @"edit" : @"edits",
                     f.failed ? [NSString stringWithFormat:@", %ld refused", (long)f.failed] : @"",
                     f.exists ? @"" : @"\nNo longer on disk"];
    } else if ([item isKindOfClass:MCAgentTask.class]) {
        MCAgentTask *t = item;
        v.toolTip = [NSString stringWithFormat:@"%@\nWorktree: %@ (branch %@)\nClaude Code's session shows "
                                               @"here once it starts\nClick: its terminal",
                     t.prompt ?: @"", [t.path stringByAbbreviatingWithTildeInPath], t.name];
    } else if ([item isKindOfClass:MCAgentWork.class]) {
        MCAgentWork *s = item;
        NSMutableArray *tip = [NSMutableArray arrayWithObject:s.name ?: @""];
        if (s.cwd) [tip addObject:[s.cwd stringByAbbreviatingWithTildeInPath]];
        if (s.worktreePath) [tip addObject:[@"Worktree: " stringByAppendingString:
                                               [s.worktreePath stringByAbbreviatingWithTildeInPath]]];
        if (s.versions.count)
            [tip addObject:[@"Claude Code " stringByAppendingString:
                               [s.versions componentsJoinedByString:@", "]]];
        if (s.badLines)
            [tip addObject:[NSString stringWithFormat:@"%ld unreadable lines skipped", (long)s.badLines]];
        [tip addObject:@"Return or click: the commands it ran (Evidence)"];
        v.toolTip = [tip componentsJoinedByString:@"\n"];
    } else {
        v.toolTip = nil;
    }
    [v setNeedsDisplay:YES];
    return v;
}
- (void)outlineViewItemDidExpand:(NSNotification *)n {
    [_expanded addObject:KeyOf(n.userInfo[@"NSObject"])];
    [self fitColumn];
}
- (void)outlineViewItemDidCollapse:(NSNotification *)n {
    [_expanded removeObject:KeyOf(n.userInfo[@"NSObject"])];
}

// ---------------------------------------------------------------- actions
- (void)activateRow:(NSInteger)row {
    if (row < 0) return;
    id item = [_outline itemAtRow:row];
    if ([item isKindOfClass:MCAgentTask.class]) {
        MCAgentTask *t = item;
        TerminalView *term = t.terminal;
        if (term && term.window) MCRevealTerminal(term);
        else if (self.onCompareWorktree) self.onCompareWorktree(t.path);
    } else if ([item isKindOfClass:MCAgentWork.class]) {
        MCAgentWork *s = item;
        if (self.onShowEvidence) self.onShowEvidence(s.transcript, s.name);
    } else if ([item isKindOfClass:MCAgentFile.class]) {
        MCAgentFile *f = item;
        if (!f.exists) { NSBeep(); return; }
        MCAgentWork *s = [self sessionOfFile:item];
        if (self.onShowFileChanges && s) self.onShowFileChanges(f.path, s.transcript, s.name);
        else if (self.onOpenFile) self.onOpenFile(f.path);
    } else {
        [self toggleRow:row];
    }
}

// The session a file row is under (file, then its group, then the session).
- (MCAgentWork *)sessionOfFile:(id)file {
    for (id up = [_outline parentForItem:file]; up; up = [_outline parentForItem:up])
        if ([up isKindOfClass:MCAgentWork.class]) return up;
    return nil;
}

- (void)toggleRow:(NSInteger)row {
    if (row < 0) return;
    id item = [_outline itemAtRow:row];
    if (![self childrenOf:item].count) return;
    if ([_outline isItemExpanded:item]) [_outline collapseItem:item];
    else [_outline expandItem:item];
}

- (void)clickRow:(NSInteger)row {
    id item = [_outline itemAtRow:row];
    if ([item isKindOfClass:MCAgentFileGroup.class]) [self toggleRow:row];
    else [self activateRow:row];
}

- (NSArray<NSString *> *)rowDescriptions {
    NSMutableArray *out = [NSMutableArray array];
    for (NSInteger r = 0; r < _outline.numberOfRows; r++) {
        id item = [_outline itemAtRow:r];
        if ([item isKindOfClass:MCAgentTask.class]) {
            MCAgentTask *t = item;
            [out addObject:[NSString stringWithFormat:@"T %@ | %@", t.name, TaskDetailOf(t, _now)]];
        } else if ([item isKindOfClass:MCAgentWork.class]) {
            MCAgentWork *s = item;
            [out addObject:[NSString stringWithFormat:@"S %ld %@ %@ | %@", (long)s.depth,
                            [s runningAt:_now] ? @"running" : @"finished", s.name,
                            DetailOf(s, _now)]];
        } else if ([item isKindOfClass:MCAgentFileGroup.class]) {
            [out addObject:[@"G " stringByAppendingString:((MCAgentFileGroup *)item).title]];
        } else if ([item isKindOfClass:MCAgentFile.class]) {
            MCAgentFile *f = item;
            [out addObject:[NSString stringWithFormat:@"F %@%@", f.path, f.exists ? @"" : @" (missing)"]];
        }
    }
    return out;
}

// ------------------------------------------------------------ the menu
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
    id item = row >= 0 && row < _outline.numberOfRows ? [_outline itemAtRow:row] : nil;
    __weak MCAgentsPanel *weakSelf = self;
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([item isKindOfClass:MCAgentTask.class]) {
        MCAgentTask *t = item;
        __weak TerminalView *weakTerm = t.terminal;
        if (t.terminal.window) [self item:@"Show Terminal" in:m run:^{ MCRevealTerminal(weakTerm); }];
        const BOOL there = [fm fileExistsAtPath:t.path];
        [self item:@"Compare with Main…" in:m run:^{
            MCAgentsPanel *p = weakSelf;
            if (p.onCompareWorktree) p.onCompareWorktree(t.path);
        }].enabled = there;
        [self item:@"Open Worktree in New Window" in:m run:^{
            MCAgentsPanel *p = weakSelf;
            if (p.onOpenFolderInNewWindow) p.onOpenFolderInNewWindow(t.path);
        }].enabled = there;
        [self item:@"Copy Worktree Path" in:m run:^{
            [NSPasteboard.generalPasteboard clearContents];
            [NSPasteboard.generalPasteboard setString:t.path forType:NSPasteboardTypeString];
        }];
    } else if ([item isKindOfClass:MCAgentWork.class]) {
        MCAgentWork *s = item;
        [self item:@"Show Evidence" in:m run:^{ [weakSelf activateRow:row]; }];
        // The terminal its session runs in, as its hooks told us (R7.4).
        TerminalView *term = [[MCTerminalAttention shared] terminalForSession:s.sessionId
                                                                   transcript:s.transcript];
        __weak TerminalView *weakTerm = term;
        if (term)
            [self item:@"Show Terminal" in:m run:^{ MCRevealTerminal(weakTerm); }];
        if (s.worktreePath.length && [fm fileExistsAtPath:s.worktreePath])
            [self item:@"Compare with Main…" in:m run:^{
                MCAgentsPanel *p = weakSelf;
                if (p.onCompareWorktree) p.onCompareWorktree(s.worktreePath);
            }];
        if (s.worktreePath.length && [fm fileExistsAtPath:s.worktreePath])
            [self item:@"Open Worktree in New Window" in:m run:^{
                MCAgentsPanel *p = weakSelf;
                if (p.onOpenFolderInNewWindow) p.onOpenFolderInNewWindow(s.worktreePath);
            }];
        if (s.scratchpad.length && [NSFileManager.defaultManager fileExistsAtPath:s.scratchpad])
            [self item:@"Reveal Scratchpad in Finder" in:m run:^{
                [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:
                    @[[NSURL fileURLWithPath:s.scratchpad]]];
            }];
        [self item:@"Reveal Transcript in Finder" in:m run:^{
            [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:
                @[[NSURL fileURLWithPath:s.transcript]]];
        }];
        [self item:@"Copy Transcript Path" in:m run:^{
            [NSPasteboard.generalPasteboard clearContents];
            [NSPasteboard.generalPasteboard setString:s.transcript forType:NSPasteboardTypeString];
        }];
    } else if ([item isKindOfClass:MCAgentFile.class]) {
        MCAgentFile *f = item;
        [self item:@"Show What the Session Changed" in:m run:^{ [weakSelf activateRow:row]; }].enabled =
            f.exists;
        [self item:@"Open" in:m run:^{
            MCAgentsPanel *p = weakSelf;
            if (p.onOpenFile) p.onOpenFile(f.path);
        }].enabled = f.exists;
        [self item:@"Reveal in Finder" in:m run:^{
            [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:f.path]]];
        }].enabled = f.exists;
        [self item:@"Copy Path" in:m run:^{
            [NSPasteboard.generalPasteboard clearContents];
            [NSPasteboard.generalPasteboard setString:f.path forType:NSPasteboardTypeString];
        }];
    }
    MCAddAdjustSettingsItem(m, @"tree", _outline);
    return m;
}

@end
