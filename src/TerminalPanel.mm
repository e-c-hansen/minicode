// TerminalPanel.mm — see TerminalPanel.h. Objective-C++.
#import "TerminalPanel.h"
#import "AppSettings.h"
#import "TabStrip.h"
#import "TerminalGridView.h"

@class MCTerminalStripButtons;

@interface MCTerminalPanel ()
@property(nonatomic, strong) MCTabStrip *strip;
@property(nonatomic, strong) MCTerminalStripButtons *buttons;
@end

static NSColor *PHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static const CGFloat kButtonWidth = 26;   // + and ◫ at the strip's right end

// The + and ◫ buttons beside the tabs, on the strip's own color. MCTabStrip
// (the editor's) draws the tabs themselves, so both rows of tabs look alike.
@interface MCTerminalStripButtons : NSView
@property(nonatomic, strong) NSColor *stripColor;
@property(nonatomic, copy) void (^onAdd)(void);
@property(nonatomic, copy) void (^onSplit)(void);
@end

@implementation MCTerminalStripButtons
- (BOOL)isFlipped { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

- (NSRect)addRect {
    return NSMakeRect(0, 0, kButtonWidth, self.bounds.size.height);
}
- (NSRect)splitRect {
    return NSMakeRect(kButtonWidth, 0, kButtonWidth, self.bounds.size.height);
}

- (void)drawGlyph:(NSString *)glyph inRect:(NSRect)r size:(CGFloat)size {
    NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:size],
                        NSForegroundColorAttributeName: PHex(0x9CA3AF)};
    NSSize sz = [glyph sizeWithAttributes:a];
    [glyph drawAtPoint:NSMakePoint(floor(NSMidX(r) - sz.width / 2),
                                   floor(NSMidY(r) - sz.height / 2))
        withAttributes:a];
}

- (void)drawRect:(NSRect)dirty {
    // Filled once, like MCTabStrip, so a see-through color is not doubled.
    [(self.stripColor ?: PHex(0x252526)) set];
    NSRectFill(self.bounds);
    [self drawGlyph:@"+" inRect:[self addRect] size:15];
    [self drawGlyph:@"◫" inRect:[self splitRect] size:13];
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (NSPointInRect(p, [self addRect]) && self.onAdd) self.onAdd();
    else if (NSPointInRect(p, [self splitRect]) && self.onSplit) self.onSplit();
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self addRect] owner:@"New terminal (⌃⇧`)" userData:NULL];
    [self addToolTipRect:[self splitRect] owner:@"Split terminal (⌘\\)" userData:NULL];
}
@end

@implementation MCTerminalPanel {
    NSString *_dir;
    NSMutableArray<NSMutableArray<TerminalView *> *> *_tabs;
    NSMutableArray<TerminalView *> *_tabActive;   // per tab: its active pane
    NSInteger _selected;
    NSMutableArray<NSView *> *_separators;
    NSView *_activeMark;                          // under the active pane, when split
    // Watched for its first responder. Weak: the window owns this view
    // through its content view, and a strong reference here kept a closed
    // window, the panel and every shell in it alive until the app quit.
    __weak NSWindow *_observedWindow;
}

- (instancetype)initWithDirectory:(NSString *)dir {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 400, 220)])) {
        _dir = dir.length ? dir : NSHomeDirectory();
        _tabs = [NSMutableArray array];
        _tabActive = [NSMutableArray array];
        _separators = [NSMutableArray array];
        _selected = -1;
        __weak MCTerminalPanel *weakSelf = self;
        self.strip = [[MCTabStrip alloc] initWithFrame:NSZeroRect];
        self.strip.surface = @"terminal-tabs";
        self.strip.onSelect = ^(NSInteger i) {
            [weakSelf selectTab:(NSUInteger)i];
            [weakSelf focusActive];
        };
        self.strip.onClose = ^(NSInteger i) { [weakSelf closeTab:(NSUInteger)i]; };
        [self addSubview:self.strip];
        self.buttons = [[MCTerminalStripButtons alloc] initWithFrame:NSZeroRect];
        self.buttons.onAdd = ^{ [weakSelf addTab]; };
        self.buttons.onSplit = ^{ [weakSelf splitActive]; };
        [self addSubview:self.buttons];
        _activeMark = [[NSView alloc] initWithFrame:NSZeroRect];
        _activeMark.wantsLayer = YES;
        _activeMark.layer.backgroundColor = PHex(0x4EA1F7).CGColor;
        _activeMark.hidden = YES;
        [self addSubview:_activeMark];
        [self applySettings];
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(applySettings)
                   name:MCSettingsDidChangeNotification object:nil];
        [self addTab];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self stopObservingWindow];
    for (TerminalView *t in self.terminals) [t close];
}

// The strip in the editor strip's colors, except that the active tab is
// the terminal's own color, so it runs into the terminal below it.
- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.strip.stripColor = [cfg background:Surface::Sidebar];
    self.strip.activeColor = [cfg background:Surface::Terminal];
    self.strip.activeText = [cfg text:Surface::Terminal];
    self.strip.inactiveText = [[cfg text:Surface::Sidebar] colorWithAlphaComponent:0.7];
    self.strip.needsDisplay = YES;
    self.buttons.stripColor = self.strip.stripColor;
    self.buttons.needsDisplay = YES;
}

// Titles and the selected tab, after anything that changes them.
- (void)reloadStrip {
    NSMutableArray<MCTabStripItem *> *items = [NSMutableArray array];
    NSArray<NSString *> *titles = [self tabTitles];
    for (NSUInteger i = 0; i < titles.count; i++) {
        MCTabStripItem *item = [MCTabStripItem new];
        item.title = titles[i];
        item.toolTip = [self tabToolTip:i];
        [items addObject:item];
    }
    self.strip.items = items;
    self.strip.activeIndex = _selected;
    self.strip.needsDisplay = YES;
}

// ------------------------------------------------------------ the terminals
- (TerminalView *)makeTerminal {
    TerminalView *t = [[TerminalView alloc] initWithDirectory:_dir];
    t.projectRoot = self.projectRoot;
    __weak MCTerminalPanel *weakSelf = self;
    t.onOpenLink = ^(MCTermLink *link) {
        MCTerminalPanel *s = weakSelf;
        if (s.onOpenLink) s.onOpenLink(link);
    };
    t.onTitleChange = ^{ [weakSelf reloadStrip]; };
    [self addSubview:t positioned:NSWindowBelow relativeTo:_activeMark];
    return t;
}

- (NSArray<TerminalView *> *)terminals {
    NSMutableArray *all = [NSMutableArray array];
    for (NSArray *tab in _tabs) [all addObjectsFromArray:tab];
    return all;
}
- (NSUInteger)tabCount { return _tabs.count; }
- (NSInteger)selectedTab { return _selected; }
- (NSArray<TerminalView *> *)terminalsInTab:(NSUInteger)tab {
    return tab < _tabs.count ? [_tabs[tab] copy] : @[];
}

- (void)setProjectRoot:(NSString *)root {
    _projectRoot = [root copy];
    for (TerminalView *t in self.terminals) t.projectRoot = root;
}

- (void)setDirectory:(NSString *)dir {
    if (dir.length) _dir = dir;
    for (TerminalView *t in self.terminals) [t setDirectory:dir];
}

// The pane the keyboard is in, in the shown tab, if any.
- (TerminalView *)focusedTerminal {
    if (_selected < 0) return nil;
    for (TerminalView *t in _tabs[_selected])
        if ([t hasFocus]) return t;
    return nil;
}

- (TerminalView *)activeTerminal {
    if (_selected < 0) return nil;
    TerminalView *focused = [self focusedTerminal];
    if (focused && focused != _tabActive[_selected]) {
        _tabActive[_selected] = focused;
        [self layoutPanes];
    }
    return _tabActive[_selected];
}

- (BOOL)hasFocus { return [self focusedTerminal] != nil; }

- (void)focusActive {
    [self.activeTerminal focusInput];
}

- (TerminalView *)addTab {
    TerminalView *t = [self makeTerminal];
    [_tabs addObject:[NSMutableArray arrayWithObject:t]];
    [_tabActive addObject:t];
    [self selectTab:_tabs.count - 1];
    [t focusInput];
    return t;
}

// A split is refused when each pane of the shown tab would be under this many
// columns: a 2-column shell is no use, and a split nobody can read is worse
// than a beep. A window made narrower later narrows the panes as they are;
// each pty still follows its pane down to 2 columns (TerminalView
// updateWindowSize), so nothing runs off a pane's edge.
static const int kMinSplitColumns = 20;

- (BOOL)roomForPanes:(NSUInteger)n {
    CGFloat W = self.bounds.size.width;
    if (W <= 0 || n == 0) return YES;   // not laid out yet: nothing to judge by
    CGFloat each = floor((W - (n - 1)) / n);
    CGFloat cols = floor((each - 2 * [TerminalGridView padding]) /
                         [TerminalGridView cellSize].width);
    return cols >= kMinSplitColumns;
}

- (TerminalView *)splitActive {
    if (_selected < 0) return [self addTab];
    NSMutableArray<TerminalView *> *tab = _tabs[_selected];
    if (![self roomForPanes:tab.count + 1]) {
        NSBeep();
        if (self.onNote)
            self.onNote([NSString stringWithFormat:@"No room to split: each terminal would be "
                                                    "narrower than %d columns",
                                                    kMinSplitColumns]);
        return nil;
    }
    TerminalView *t = [self makeTerminal];
    NSUInteger at = [tab indexOfObject:self.activeTerminal];
    [tab insertObject:t atIndex:at == NSNotFound ? tab.count : at + 1];
    _tabActive[_selected] = t;
    [self layoutPanes];
    [t focusInput];
    return t;
}

- (void)selectTab:(NSUInteger)tab {
    if (tab >= _tabs.count) return;
    BOOL hadFocus = [self hasFocus];
    _selected = (NSInteger)tab;
    [self layoutPanes];
    if (hadFocus) [self focusActive];
}

- (void)moveActive:(NSInteger)delta {
    NSArray<TerminalView *> *all = self.terminals;
    if (all.count == 0) return;
    NSInteger i = (NSInteger)[all indexOfObject:self.activeTerminal];
    if (i == NSNotFound) i = 0;
    i = ((i + delta) % (NSInteger)all.count + (NSInteger)all.count) % (NSInteger)all.count;
    TerminalView *next = all[i];
    for (NSUInteger tab = 0; tab < _tabs.count; tab++) {
        if (![_tabs[tab] containsObject:next]) continue;
        _tabActive[tab] = next;
        _selected = (NSInteger)tab;
    }
    [self layoutPanes];
    [next focusInput];
}

// ------------------------------------------------------------------ closing
NSArray<NSString *> *MCRunningCommands(NSArray<TerminalView *> *terminals) {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (TerminalView *t in terminals) [out addObjectsFromArray:t.runningCommands];
    return out;
}

BOOL MCConfirmClosingRunning(NSArray<NSString *> *commands, NSString *question,
                             NSString *button) {
    if (commands.count == 0) return YES;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = question;
    if (commands.count == 1)
        a.informativeText = [NSString stringWithFormat:@"“%@” is still running.",
                             commands.firstObject];
    else
        a.informativeText = [NSString stringWithFormat:@"These are still running:\n%@",
                             [commands componentsJoinedByString:@"\n"]];
    [a addButtonWithTitle:button];      // 1000, Return
    [a addButtonWithTitle:@"Cancel"];   // 1001, Esc
    a.buttons[1].keyEquivalent = @"\033";
    return [a runModal] == NSAlertFirstButtonReturn;
}

- (BOOL)confirmClosing:(NSArray<TerminalView *> *)terminals {
    NSArray<NSString *> *running = MCRunningCommands(terminals);
    return MCConfirmClosingRunning(running, terminals.count > 1 ? @"Close these terminals?"
                                                                : @"Close the terminal?",
                                   @"Close");
}

// A command queued for a terminal that closes before its prompt never runs;
// the user agreed to that at the question, and the status bar says so.
- (void)noteDroppedCommands:(NSArray<TerminalView *> *)terminals {
    for (TerminalView *t in terminals) {
        NSString *c = t.pendingCommand;
        if (c && self.onNote)
            self.onNote([NSString stringWithFormat:@"“%@” did not run: its terminal was closed", c]);
    }
}

- (BOOL)closeTerminal:(TerminalView *)terminal {
    for (NSUInteger tab = 0; tab < _tabs.count; tab++) {
        NSMutableArray<TerminalView *> *panes = _tabs[tab];
        NSUInteger i = [panes indexOfObject:terminal];
        if (i == NSNotFound) continue;
        if (![self confirmClosing:@[terminal]]) return NO;
        BOOL hadFocus = [terminal hasFocus];
        [self noteDroppedCommands:@[terminal]];
        [terminal close];
        [terminal removeFromSuperview];
        [panes removeObjectAtIndex:i];
        if (panes.count == 0) {
            [self removeTab:tab];
        } else if (_tabActive[tab] == terminal) {
            _tabActive[tab] = panes[i > 0 ? i - 1 : 0];
        }
        [self finishClose:hadFocus];
        return YES;
    }
    return YES;
}

- (BOOL)closeTab:(NSUInteger)tab {
    if (tab >= _tabs.count) return YES;
    if (![self confirmClosing:_tabs[tab]]) return NO;
    [self noteDroppedCommands:_tabs[tab]];
    BOOL hadFocus = NO;
    for (TerminalView *t in _tabs[tab]) {
        hadFocus = hadFocus || [t hasFocus];
        [t close];
        [t removeFromSuperview];
    }
    [self removeTab:tab];
    [self finishClose:hadFocus];
    return YES;
}

- (void)closeAll {
    for (TerminalView *t in self.terminals) {
        [t close];
        [t removeFromSuperview];
    }
    [_tabs removeAllObjects];
    [_tabActive removeAllObjects];
    _selected = -1;
    [self stopObservingWindow];
    [self layoutPanes];
}

// Drops an emptied tab; the one to its left is shown when it was the shown one.
- (void)removeTab:(NSUInteger)tab {
    [_tabs removeObjectAtIndex:tab];
    [_tabActive removeObjectAtIndex:tab];
    if (_selected > (NSInteger)tab || _selected >= (NSInteger)_tabs.count)
        _selected--;
    if (_tabs.count == 0) _selected = -1;
}

- (void)finishClose:(BOOL)hadFocus {
    if (_tabs.count == 0) {
        [self layoutPanes];
        if (self.onEmpty) self.onEmpty();
        return;
    }
    [self layoutPanes];
    if (hadFocus) [self focusActive];
}

// ------------------------------------------------------------------ layout
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self layoutPanes];
}

// The strip on top; the shown tab's panes below it in equal widths, with a
// 1px line between them. Hidden tabs keep the same frames, so their programs
// keep the size they had.
- (void)layoutPanes {
    NSRect b = self.bounds;
    CGFloat W = b.size.width, H = MAX(0, b.size.height - kMCTabStripHeight);
    CGFloat stripH = MIN(kMCTabStripHeight, b.size.height);
    CGFloat buttonsW = MIN(2 * kButtonWidth, W);
    self.strip.frame = NSMakeRect(0, H, W - buttonsW, stripH);
    self.buttons.frame = NSMakeRect(W - buttonsW, H, buttonsW, stripH);
    [self reloadStrip];

    NSUInteger shownCount = _selected >= 0 ? _tabs[_selected].count : 0;
    while (_separators.count < MAX(shownCount, 1u) - 1) {
        NSView *line = [[NSView alloc] initWithFrame:NSZeroRect];
        line.wantsLayer = YES;
        line.layer.backgroundColor = PHex(0x333333).CGColor;
        [self addSubview:line];
        [_separators addObject:line];
    }
    for (NSView *line in _separators) line.hidden = YES;

    for (NSUInteger tab = 0; tab < _tabs.count; tab++) {
        NSArray<TerminalView *> *panes = _tabs[tab];
        NSUInteger n = panes.count;
        CGFloat each = floor((W - (n - 1)) / n);
        CGFloat x = 0;
        for (NSUInteger i = 0; i < n; i++) {
            CGFloat w = i + 1 == n ? W - x : each;   // the last takes the rest
            panes[i].frame = NSMakeRect(x, 0, MAX(0, w), H);
            panes[i].hidden = (NSInteger)tab != _selected;
            if ((NSInteger)tab == _selected && i + 1 < n) {
                NSView *line = _separators[i];
                line.frame = NSMakeRect(x + w, 0, 1, H);
                line.hidden = NO;
            }
            x += w + 1;
        }
    }

    TerminalView *active = _selected >= 0 ? _tabActive[_selected] : nil;
    _activeMark.hidden = !(active && shownCount > 1);
    if (active) {
        NSRect f = active.frame;
        // Along the bottom, under the input line, so it never runs into the
        // selected tab's underline.
        _activeMark.frame = NSMakeRect(NSMinX(f), NSMinY(f), f.size.width, 2);
    }
}

// ------------------------------------------------------------------ titles
- (NSArray<NSString *> *)tabTitles {
    NSMutableArray *titles = [NSMutableArray array];
    for (NSUInteger tab = 0; tab < _tabs.count; tab++) {
        NSMutableArray *parts = [NSMutableArray array];
        for (TerminalView *t in _tabs[tab]) [parts addObject:t.title ?: @""];
        [titles addObject:[parts componentsJoinedByString:@" │ "]];
    }
    return titles;
}

- (NSString *)tabToolTip:(NSUInteger)tab {
    if (tab >= _tabs.count) return nil;
    NSMutableArray *lines = [NSMutableArray array];
    for (TerminalView *t in _tabs[tab])
        [lines addObject:[NSString stringWithFormat:@"%@ (%@)", t.title,
                          [t.directory stringByAbbreviatingWithTildeInPath]]];
    return [lines componentsJoinedByString:@"\n"];
}

// ------------------------------------------------------------ focus marking
// The active pane follows the keyboard: a click in a pane of the shown tab
// makes it the one Cmd+W, Run File and the next/previous keys act on.
- (void)viewWillMoveToWindow:(NSWindow *)window {
    [self stopObservingWindow];
    if (window) {
        _observedWindow = window;
        [window addObserver:self forKeyPath:@"firstResponder" options:0 context:NULL];
    }
    [super viewWillMoveToWindow:window];
}

- (void)stopObservingWindow {
    NSWindow *w = _observedWindow;
    if (!w) return;
    [w removeObserver:self forKeyPath:@"firstResponder"];
    _observedWindow = nil;
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    if (object == _observedWindow && [keyPath isEqualToString:@"firstResponder"]) {
        (void)self.activeTerminal;   // picks up the focused pane
        return;
    }
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

@end
