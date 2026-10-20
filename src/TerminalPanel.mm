// TerminalPanel.mm — see TerminalPanel.h. Objective-C++.
#import "TerminalPanel.h"
#import "AppSettings.h"

static NSColor *PHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static const CGFloat kStripHeight = 24;
static const CGFloat kButtonWidth = 24;   // + and ◫ at the strip's right end
static const CGFloat kTabMaxWidth = 200;
static const CGFloat kTabMinWidth = 70;
static const CGFloat kTabPad = 10;
static const CGFloat kCloseWidth = 14;

@class MCTerminalTabStrip;

@interface MCTerminalPanel ()
@property(nonatomic, strong) MCTerminalTabStrip *strip;
- (NSArray<NSString *> *)tabTitles;
- (NSString *)tabToolTip:(NSUInteger)tab;
@end

// The row of tabs, drawn by hand on the panel's own background. A click
// selects a tab, × closes one, + adds a tab and ◫ splits the shown one.
@interface MCTerminalTabStrip : NSView
@property(nonatomic, weak) MCTerminalPanel *panel;
@end

@implementation MCTerminalTabStrip

- (BOOL)isFlipped { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

- (NSDictionary *)attributes:(BOOL)selected {
    NSColor *color = selected ? [[AppSettings shared] text:Surface::Terminal]
                              : PHex(0x9CA3AF);
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingMiddle;
    return @{NSFontAttributeName: [NSFont systemFontOfSize:11],
             NSForegroundColorAttributeName: color,
             NSParagraphStyleAttributeName: ps};
}

// Each tab's rectangle, left to right. Tabs share what is left of the strip
// once the buttons have their room, each between kTabMinWidth and
// kTabMaxWidth wide.
- (NSArray<NSValue *> *)tabRects {
    NSArray<NSString *> *titles = [self.panel tabTitles];
    CGFloat avail = self.bounds.size.width - 2 * kButtonWidth - 8;
    NSMutableArray<NSNumber *> *widths = [NSMutableArray array];
    CGFloat total = 0;
    for (NSString *t in titles) {
        CGFloat w = ceil([t sizeWithAttributes:[self attributes:YES]].width) +
                    2 * kTabPad + kCloseWidth;
        w = MIN(MAX(w, kTabMinWidth), kTabMaxWidth);
        [widths addObject:@(w)];
        total += w;
    }
    CGFloat squeeze = total > avail && titles.count
        ? MAX(kTabMinWidth / 2, avail / titles.count) : 0;
    NSMutableArray<NSValue *> *rects = [NSMutableArray array];
    CGFloat x = 4;
    for (NSNumber *w in widths) {
        CGFloat width = squeeze ? squeeze : w.doubleValue;
        [rects addObject:[NSValue valueWithRect:
            NSMakeRect(x, 0, width, self.bounds.size.height)]];
        x += width;
    }
    return rects;
}

- (NSRect)closeRectIn:(NSRect)tab {
    return NSMakeRect(NSMaxX(tab) - kTabPad / 2 - kCloseWidth, 0, kCloseWidth,
                      tab.size.height);
}
- (NSRect)addRect {
    return NSMakeRect(self.bounds.size.width - 2 * kButtonWidth - 4, 0,
                      kButtonWidth, self.bounds.size.height);
}
- (NSRect)splitRect {
    return NSMakeRect(self.bounds.size.width - kButtonWidth - 4, 0,
                      kButtonWidth, self.bounds.size.height);
}

- (void)drawGlyph:(NSString *)glyph inRect:(NSRect)r size:(CGFloat)size {
    NSDictionary *a = @{NSFontAttributeName: [NSFont systemFontOfSize:size],
                        NSForegroundColorAttributeName: PHex(0x9CA3AF)};
    NSSize s = [glyph sizeWithAttributes:a];
    [glyph drawAtPoint:NSMakePoint(NSMidX(r) - s.width / 2, NSMidY(r) - s.height / 2)
        withAttributes:a];
}

- (void)drawRect:(NSRect)dirty {
    NSArray<NSString *> *titles = [self.panel tabTitles];
    NSArray<NSValue *> *rects = [self tabRects];
    NSInteger selected = self.panel.selectedTab;
    for (NSUInteger i = 0; i < titles.count && i < rects.count; i++) {
        NSRect r = rects[i].rectValue;
        BOOL sel = (NSInteger)i == selected;
        NSRect text = NSMakeRect(NSMinX(r) + kTabPad, 0,
                                 r.size.width - 2 * kTabPad - kCloseWidth, r.size.height);
        NSDictionary *a = [self attributes:sel];
        CGFloat h = [titles[i] sizeWithAttributes:a].height;
        [titles[i] drawInRect:NSMakeRect(text.origin.x, floor((r.size.height - h) / 2),
                                         MAX(0, text.size.width), h)
               withAttributes:a];
        [self drawGlyph:@"×" inRect:[self closeRectIn:r] size:12];
        if (sel) {
            [PHex(0x4EA1F7) setFill];
            NSRectFill(NSMakeRect(NSMinX(r) + 4, r.size.height - 2, r.size.width - 8, 2));
        }
    }
    [self drawGlyph:@"+" inRect:[self addRect] size:14];
    [self drawGlyph:@"◫" inRect:[self splitRect] size:12];
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    MCTerminalPanel *panel = self.panel;
    if (NSPointInRect(p, [self addRect])) { [panel addTab]; return; }
    if (NSPointInRect(p, [self splitRect])) { [panel splitActive]; return; }
    NSArray<NSValue *> *rects = [self tabRects];
    for (NSUInteger i = 0; i < rects.count; i++) {
        NSRect r = rects[i].rectValue;
        if (!NSPointInRect(p, r)) continue;
        if (NSPointInRect(p, [self closeRectIn:r])) [panel closeTab:i];
        else { [panel selectTab:i]; [panel focusActive]; }
        return;
    }
}

- (void)updateToolTips {
    [self removeAllToolTips];
    NSArray<NSValue *> *rects = [self tabRects];
    for (NSUInteger i = 0; i < rects.count; i++)
        [self addToolTipRect:rects[i].rectValue owner:self userData:(void *)(uintptr_t)i];
    [self addToolTipRect:[self addRect] owner:self userData:(void *)(uintptr_t)1000];
    [self addToolTipRect:[self splitRect] owner:self userData:(void *)(uintptr_t)1001];
}

- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag
             point:(NSPoint)point userData:(void *)data {
    uintptr_t i = (uintptr_t)data;
    if (i == 1000) return @"New terminal (⌃⇧`)";
    if (i == 1001) return @"Split terminal (⌘\\)";
    return [self.panel tabToolTip:i];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateToolTips];
}

@end

@implementation MCTerminalPanel {
    NSString *_dir;
    NSMutableArray<NSMutableArray<TerminalView *> *> *_tabs;
    NSMutableArray<TerminalView *> *_tabActive;   // per tab: its active pane
    NSInteger _selected;
    NSMutableArray<NSView *> *_separators;
    NSView *_activeMark;                          // under the active pane, when split
    NSWindow *_observedWindow;
}

- (instancetype)initWithDirectory:(NSString *)dir {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 400, 220)])) {
        _dir = dir.length ? dir : NSHomeDirectory();
        _tabs = [NSMutableArray array];
        _tabActive = [NSMutableArray array];
        _separators = [NSMutableArray array];
        _selected = -1;
        self.strip = [[MCTerminalTabStrip alloc] initWithFrame:NSZeroRect];
        self.strip.panel = self;
        self.strip.wantsLayer = YES;
        [self addSubview:self.strip];
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
    [_observedWindow removeObserver:self forKeyPath:@"firstResponder"];
    for (TerminalView *t in self.terminals) [t close];
}

// The strip stands on the terminal's own background; the terminals paint
// theirs, so nothing is painted twice on a see-through panel.
- (void)applySettings {
    self.strip.layer.backgroundColor =
        [[AppSettings shared] background:Surface::Terminal].CGColor;
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
    t.onTitleChange = ^{
        MCTerminalPanel *s = weakSelf;
        s.strip.needsDisplay = YES;
        [s.strip updateToolTips];
    };
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

- (TerminalView *)splitActive {
    if (_selected < 0) return [self addTab];
    NSMutableArray<TerminalView *> *tab = _tabs[_selected];
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

- (void)closeTerminal:(TerminalView *)terminal {
    for (NSUInteger tab = 0; tab < _tabs.count; tab++) {
        NSMutableArray<TerminalView *> *panes = _tabs[tab];
        NSUInteger i = [panes indexOfObject:terminal];
        if (i == NSNotFound) continue;
        BOOL hadFocus = [terminal hasFocus];
        [terminal close];
        [terminal removeFromSuperview];
        [panes removeObjectAtIndex:i];
        if (panes.count == 0) {
            [self removeTab:tab];
        } else if (_tabActive[tab] == terminal) {
            _tabActive[tab] = panes[i > 0 ? i - 1 : 0];
        }
        [self finishClose:hadFocus];
        return;
    }
}

- (void)closeTab:(NSUInteger)tab {
    if (tab >= _tabs.count) return;
    BOOL hadFocus = NO;
    for (TerminalView *t in _tabs[tab]) {
        hadFocus = hadFocus || [t hasFocus];
        [t close];
        [t removeFromSuperview];
    }
    [self removeTab:tab];
    [self finishClose:hadFocus];
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
    CGFloat W = b.size.width, H = MAX(0, b.size.height - kStripHeight);
    self.strip.frame = NSMakeRect(0, H, W, MIN(kStripHeight, b.size.height));
    self.strip.needsDisplay = YES;
    [self.strip updateToolTips];

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
    [_observedWindow removeObserver:self forKeyPath:@"firstResponder"];
    _observedWindow = window;
    [window addObserver:self forKeyPath:@"firstResponder" options:0 context:NULL];
    [super viewWillMoveToWindow:window];
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
