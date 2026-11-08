// TerminalPanel.mm — see TerminalPanel.h. Objective-C++.
#import "TerminalPanel.h"
#import "AppSettings.h"
#import "TabStrip.h"
#import "EditorPanes.h"
#import "TerminalAttention.h"
#import "TerminalGridView.h"
#include "EditorTabs.h"
#include "TerminalLayout.h"

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
@property(nonatomic, copy) void (^onSplitDown)(void);   // Option+click on ◫
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
    else if (NSPointInRect(p, [self splitRect])) {
        BOOL down = (event.modifierFlags & NSEventModifierFlagOption) != 0;
        if (down && self.onSplitDown) self.onSplitDown();
        else if (self.onSplit) self.onSplit();
    }
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self addRect] owner:@"New terminal (⌃⇧`)" userData:NULL];
    [self addToolTipRect:[self splitRect] owner:@"Split terminal (⌘\\); Option-click to split down"
                    userData:NULL];
}
@end

// One tab: its terminals as columns side by side, each a stack top to
// bottom, and the shares of the space they hold (TerminalLayout.h).
@interface MCTerminalTab : NSObject {
@public
    TerminalLayout::Layout layout;
}
@property(nonatomic, strong) NSMutableArray<NSMutableArray<TerminalView *> *> *columns;
@property(nonatomic, strong) TerminalView *active;   // its active pane
@end

@implementation MCTerminalTab
- (instancetype)initWith:(TerminalView *)t {
    if ((self = [super init])) {
        _columns = [NSMutableArray arrayWithObject:[NSMutableArray arrayWithObject:t]];
        _active = t;
    }
    return self;
}
// Column by column, each top to bottom: the order Option+Cmd+] walks.
- (NSArray<TerminalView *> *)all {
    NSMutableArray *out = [NSMutableArray array];
    for (NSArray *column in _columns) [out addObjectsFromArray:column];
    return out;
}
- (BOOL)find:(TerminalView *)t column:(NSUInteger *)col row:(NSUInteger *)row {
    for (NSUInteger c = 0; c < _columns.count; c++) {
        NSUInteger r = [_columns[c] indexOfObject:t];
        if (r == NSNotFound) continue;
        *col = c;
        *row = r;
        return YES;
    }
    return NO;
}
@end

// A bar between two panes: after column `column` (row -1), or after row
// `row` of that column's stack. Made once and given a place on each layout.
@interface MCTerminalSplitBar : DragBar
@property(nonatomic) NSUInteger column;
@property(nonatomic) NSInteger row;
@end
@implementation MCTerminalSplitBar
@end

// Points between neighbouring panes: the bar's 1px line.
static const CGFloat kPaneGap = 1;
// A split is refused when a pane would be under 20 columns, or a stacked
// one under 5 rows: a 2-column shell is no use, and a split nobody can read
// is worse than a beep. Bars stop there too. A window made smaller later
// shrinks the panes as they are; each pty still follows its pane down to 2
// columns (TerminalView updateWindowSize), so nothing runs off an edge.
static const int kMinSplitColumns = 20;
static const int kMinStackRows = 5;
// A resize from the keyboard moves a bar this many cells, as VS Code's.
static const int kResizeCells = 4;

@implementation MCTerminalPanel {
    NSString *_dir;
    NSMutableArray<MCTerminalTab *> *_tabs;
    NSInteger _selected;
    NSMutableArray<MCTerminalSplitBar *> *_bars;
    NSView *_activeMark;                          // under the active pane, when split
    // Watched for its first responder. Weak: the window owns this view
    // through its content view, and a strong reference here kept a closed
    // window, the panel and every shell in it alive until the app quit.
    __weak NSWindow *_observedWindow;
}

- (instancetype)initWithDirectory:(NSString *)dir {
    return [self initWithDirectory:dir firstIn:nil];
}

- (instancetype)initWithDirectory:(NSString *)dir firstIn:(NSString *)first {
    return [self initWithDirectory:dir firstIn:first environment:nil];
}

- (instancetype)initWithDirectory:(NSString *)dir firstIn:(NSString *)first
                      environment:(NSDictionary<NSString *, NSString *> *(^)(NSString *))environment {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 400, 220)])) {
        _environment = [environment copy];
        _dir = dir.length ? dir : NSHomeDirectory();
        _tabs = [NSMutableArray array];
        _bars = [NSMutableArray array];
        _selected = -1;
        __weak MCTerminalPanel *weakSelf = self;
        self.strip = [[MCTabStrip alloc] initWithFrame:NSZeroRect];
        self.strip.surface = @"terminal-tabs";
        self.strip.onSelect = ^(NSInteger i) {
            [weakSelf selectTab:(NSUInteger)i];
            [weakSelf focusActive];
        };
        self.strip.onClose = ^(NSInteger i) { [weakSelf closeTab:(NSUInteger)i]; };
        self.strip.onMove = ^(NSInteger from, NSInteger to) {
            [weakSelf moveTab:(NSUInteger)from to:(NSUInteger)to];
        };
        self.strip.menuForTab = ^NSMenu *(NSInteger i) { return [weakSelf menuForTab:(NSUInteger)i]; };
        [self addSubview:self.strip];
        self.buttons = [[MCTerminalStripButtons alloc] initWithFrame:NSZeroRect];
        self.buttons.onAdd = ^{ [weakSelf addTab]; };
        self.buttons.onSplit = ^{ [weakSelf splitActive]; };
        self.buttons.onSplitDown = ^{ [weakSelf splitActiveDown]; };
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
        // A terminal marked waiting or done (`minicode notify`): its tab's dot.
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(reloadStrip)
                   name:MCTerminalAttentionDidChangeNotification object:nil];
        [self addTabInDirectory:first];
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
        item.attention = [self tabAttention:i];
        [items addObject:item];
    }
    self.strip.items = items;
    self.strip.activeIndex = _selected;
    self.strip.needsDisplay = YES;
}

// ------------------------------------------------------------ the terminals
- (TerminalView *)makeTerminal { return [self makeTerminalIn:nil]; }
- (TerminalView *)makeTerminalIn:(NSString *)dir {
    NSString *start = dir.length ? dir : _dir;
    TerminalView *t = [[TerminalView alloc] initWithDirectory:start
                                                  environment:_environment ? _environment(start) : nil];
    t.projectRoot = self.projectRoot;
    t.extraLinkRoots = self.extraLinkRoots;
    t.sessionLinkRoots = self.sessionLinkRoots;
    __weak MCTerminalPanel *weakSelf = self;
    t.onOpenLink = ^(MCTermLink *link) {
        MCTerminalPanel *s = weakSelf;
        if (s.onOpenLink) s.onOpenLink(link);
    };
    t.onTitleChange = ^{ [weakSelf reloadStrip]; };
    // At the back, so the bars between panes stay above the terminals and
    // take the clicks in their grab areas.
    [self addSubview:t positioned:NSWindowBelow relativeTo:nil];
    return t;
}

- (NSArray<TerminalView *> *)terminals {
    NSMutableArray *all = [NSMutableArray array];
    for (MCTerminalTab *tab in _tabs) [all addObjectsFromArray:[tab all]];
    return all;
}
- (NSUInteger)tabCount { return _tabs.count; }
- (NSInteger)selectedTab { return _selected; }
- (NSArray<TerminalView *> *)terminalsInTab:(NSUInteger)tab {
    return tab < _tabs.count ? [_tabs[tab] all] : @[];
}
- (NSArray<NSArray<TerminalView *> *> *)columnsInTab:(NSUInteger)tab {
    if (tab >= _tabs.count) return @[];
    NSMutableArray *out = [NSMutableArray array];
    for (NSArray *column in _tabs[tab].columns) [out addObject:[column copy]];
    return out;
}

- (void)setProjectRoot:(NSString *)root {
    _projectRoot = [root copy];
    for (TerminalView *t in self.terminals) t.projectRoot = root;
}

- (void)setExtraLinkRoots:(NSArray<NSString *> *(^)(void))roots {
    _extraLinkRoots = [roots copy];
    for (TerminalView *t in self.terminals) t.extraLinkRoots = _extraLinkRoots;
}

- (void)setSessionLinkRoots:(NSArray<NSString *> *(^)(TerminalView *))roots {
    _sessionLinkRoots = [roots copy];
    for (TerminalView *t in self.terminals) t.sessionLinkRoots = _sessionLinkRoots;
}

- (void)setDirectory:(NSString *)dir {
    if (dir.length) _dir = dir;
    for (TerminalView *t in self.terminals) [t setDirectory:dir];
}

// The pane the keyboard is in, in the shown tab, if any.
- (TerminalView *)focusedTerminal {
    if (_selected < 0) return nil;
    for (TerminalView *t in [_tabs[_selected] all])
        if ([t hasFocus]) return t;
    return nil;
}

- (TerminalView *)activeTerminal {
    if (_selected < 0) return nil;
    MCTerminalTab *tab = _tabs[_selected];
    TerminalView *focused = [self focusedTerminal];
    if (focused && focused != tab.active) {
        tab.active = focused;
        [self layoutPanes];
    }
    return tab.active;
}

- (BOOL)hasFocus { return [self focusedTerminal] != nil; }

- (void)focusActive {
    [self.activeTerminal focusInput];
}

- (TerminalView *)addTab { return [self addTabInDirectory:nil]; }
- (TerminalView *)addTabInDirectory:(NSString *)dir {
    TerminalView *t = [self makeTerminalIn:dir];
    [_tabs addObject:[[MCTerminalTab alloc] initWith:t]];
    [self selectTab:_tabs.count - 1];
    [t focusInput];
    return t;
}

// ------------------------------------------------------------------- sizes
// The space the panes share: the panel below its strip.
- (NSSize)paneArea {
    NSRect b = self.bounds;
    return NSMakeSize(b.size.width, MAX(0, b.size.height - kMCTabStripHeight));
}
+ (CGFloat)minPaneWidth {
    return kMinSplitColumns * [TerminalGridView cellSize].width + 2 * [TerminalGridView padding];
}
+ (CGFloat)minPaneHeight {
    return kMinStackRows * [TerminalGridView cellSize].height + 2 * [TerminalGridView padding];
}

// ------------------------------------------------------------------ splits
- (TerminalView *)splitActive { return [self splitActiveInDirectory:nil]; }
- (TerminalView *)splitActiveInDirectory:(NSString *)dir {
    return [self split:dir down:NO];
}
- (TerminalView *)splitActiveDown { return [self splitActiveDownInDirectory:nil]; }
- (TerminalView *)splitActiveDownInDirectory:(NSString *)dir {
    return [self split:dir down:YES];
}

// A new terminal beside the active one (a column to its right), or below it
// in its column's stack: the active pane's share halved, or every share
// evened when halving leaves too little, or a beep when even that would.
- (TerminalView *)split:(NSString *)dir down:(BOOL)down {
    if (_selected < 0) return [self addTabInDirectory:dir];
    MCTerminalTab *tab = _tabs[_selected];
    NSUInteger c = tab.columns.count - 1, r = 0;
    if (![tab find:self.activeTerminal column:&c row:&r]) r = tab.columns[c].count - 1;
    NSSize area = [self paneArea];
    TerminalLayout::Fractions fr = down ? tab->layout.rows[c] : tab->layout.columns;
    if (TerminalLayout::split(fr, down ? r : c, down ? area.height : area.width, kPaneGap,
                              down ? [MCTerminalPanel minPaneHeight] : [MCTerminalPanel minPaneWidth])
        == TerminalLayout::Split::Refused) {
        NSBeep();
        if (self.onNote)
            self.onNote(down ? [NSString stringWithFormat:@"No room to split down: each terminal "
                                                           "would be shorter than %d rows",
                                                           kMinStackRows]
                             : [NSString stringWithFormat:@"No room to split: each terminal would be "
                                                           "narrower than %d columns",
                                                           kMinSplitColumns]);
        return nil;
    }
    TerminalView *t = [self makeTerminalIn:dir];
    if (down) {
        tab->layout.rows[c] = fr;
        [tab.columns[c] insertObject:t atIndex:r + 1];
    } else {
        tab->layout.columns = fr;
        tab->layout.rows.insert(tab->layout.rows.begin() + (long)c + 1, TerminalLayout::Fractions{1.0});
        [tab.columns insertObject:[NSMutableArray arrayWithObject:t] atIndex:c + 1];
    }
    tab.active = t;
    [self layoutPanes];
    [t focusInput];
    return t;
}

// A bar dragged: the two panes either side of it share their space anew.
- (void)splitBar:(MCTerminalSplitBar *)bar draggedTo:(CGFloat)p {
    if (_selected < 0) return;
    MCTerminalTab *tab = _tabs[_selected];
    NSSize area = [self paneArea];
    BOOL moved = NO;
    if (bar.row < 0)
        moved = TerminalLayout::dragBoundary(tab->layout.columns, bar.column, p, area.width,
                                             kPaneGap, [MCTerminalPanel minPaneWidth]);
    else if (bar.column < tab->layout.rows.size())
        // The panel is not flipped; the layout counts from the top.
        moved = TerminalLayout::dragBoundary(tab->layout.rows[bar.column], (size_t)bar.row,
                                             area.height - p, area.height, kPaneGap,
                                             [MCTerminalPanel minPaneHeight]);
    if (moved) [self layoutPanes];
}

- (BOOL)resizeActivePane:(MCPaneDirection)direction {
    if (_selected < 0) return NO;
    MCTerminalTab *tab = _tabs[_selected];
    NSUInteger c = 0, r = 0;
    if (![tab find:self.activeTerminal column:&c row:&r]) return NO;
    NSSize area = [self paneArea], cell = [TerminalGridView cellSize];
    BOOL across = direction == MCPaneLeft || direction == MCPaneRight;
    BOOL towardEnd = direction == MCPaneRight || direction == MCPaneDown;
    BOOL moved = NO;
    if (across) {
        moved = TerminalLayout::nudge(tab->layout.columns, c, towardEnd, kResizeCells * cell.width,
                                      area.width, kPaneGap, [MCTerminalPanel minPaneWidth]);
    } else if (tab->layout.rows[c].size() > 1) {
        moved = TerminalLayout::nudge(tab->layout.rows[c], r, towardEnd, kResizeCells * cell.height,
                                      area.height, kPaneGap, [MCTerminalPanel minPaneHeight]);
    } else if (self.onResizeDock) {
        // A pane alone in its column: Up and Down resize the dock itself,
        // as VS Code resizes the panel across its terminals' direction.
        return self.onResizeDock((towardEnd ? -1 : 1) * kResizeCells * cell.height);
    }
    if (moved) [self layoutPanes];
    return moved;
}

- (void)resizeTerminalPane:(id)sender {
    NSInteger tag = [sender respondsToSelector:@selector(tag)] ? [sender tag] : 0;
    if (tag < MCPaneLeft || tag > MCPaneDown) return;
    if (![self resizeActivePane:(MCPaneDirection)tag]) NSBeep();
}

- (void)selectTab:(NSUInteger)tab {
    if (tab >= _tabs.count) return;
    BOOL hadFocus = [self hasFocus];
    _selected = (NSInteger)tab;
    [self layoutPanes];
    if (hadFocus) [self focusActive];
}

- (void)selectTerminal:(TerminalView *)terminal {
    for (NSUInteger i = 0; i < _tabs.count; i++) {
        if (![[_tabs[i] all] containsObject:terminal]) continue;
        _tabs[i].active = terminal;
        _selected = (NSInteger)i;
        [self layoutPanes];
        return;
    }
}

- (void)moveActive:(NSInteger)delta {
    NSArray<TerminalView *> *all = self.terminals;
    if (all.count == 0) return;
    NSInteger i = (NSInteger)[all indexOfObject:self.activeTerminal];
    if (i == NSNotFound) i = 0;
    i = ((i + delta) % (NSInteger)all.count + (NSInteger)all.count) % (NSInteger)all.count;
    TerminalView *next = all[i];
    [self selectTerminal:next];
    [next focusInput];
}

// ----------------------------------------------------------- tab order
- (BOOL)moveTab:(NSUInteger)from to:(NSUInteger)to {
    if (from >= _tabs.count || to >= _tabs.count || from == to) return NO;
    MCTerminalTab *shown = _selected >= 0 ? _tabs[_selected] : nil;
    MCTerminalTab *moving = _tabs[from];
    [_tabs removeObjectAtIndex:from];
    [_tabs insertObject:moving atIndex:to];
    _selected = shown ? (NSInteger)[_tabs indexOfObject:shown] : -1;
    [self layoutPanes];
    return YES;
}

- (BOOL)moveSelectedTab:(NSInteger)delta {
    if (![self canMoveSelectedTab:delta]) return NO;
    return [self moveTab:(NSUInteger)_selected to:(NSUInteger)(_selected + delta)];
}

- (BOOL)canMoveSelectedTab:(NSInteger)delta {
    NSInteger to = _selected + delta;
    return _selected >= 0 && to >= 0 && to < (NSInteger)_tabs.count;
}

// ------------------------------------------------------------------ rename
// Asks for a name, the current one filled in. nil when cancelled; an empty
// name gives the terminal its own title back.
static NSString *MCAskTerminalName(TerminalView *t) {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Rename Terminal";
    a.informativeText = @"An empty name gives the terminal back its own title.";
    [a addButtonWithTitle:@"Rename"];
    [a addButtonWithTitle:@"Cancel"];
    a.buttons[1].keyEquivalent = @"\033";
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = t.userTitle ?: t.title ?: @"";
    a.accessoryView = field;
    [a.window setInitialFirstResponder:field];
    if ([a runModal] != NSAlertFirstButtonReturn) return nil;
    return field.stringValue ?: @"";
}

- (BOOL)renameTerminal:(TerminalView *)terminal {
    // Also the menu's action of that name, which reaches the panel first
    // when the keyboard is in one of its terminals: the active one, then.
    if (![terminal isKindOfClass:TerminalView.class]) terminal = self.activeTerminal;
    if (!terminal || ![self.terminals containsObject:terminal]) return NO;
    NSString *name = self.namePrompt ? self.namePrompt(terminal) : MCAskTerminalName(terminal);
    if (!name) return NO;
    terminal.userTitle = name;   // the strip hears through onTitleChange
    return YES;
}

// ------------------------------------------------------- the tab's menu
- (NSMenu *)menuForTab:(NSUInteger)i {
    if (i >= _tabs.count) return nil;
    NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
    NSArray *rows = @[@[@"Rename Terminal…", NSStringFromSelector(@selector(tabMenuRename:))],
                      @[@"Move Terminal Left", NSStringFromSelector(@selector(tabMenuMoveLeft:))],
                      @[@"Move Terminal Right", NSStringFromSelector(@selector(tabMenuMoveRight:))],
                      @[],
                      @[@"Split Terminal", NSStringFromSelector(@selector(tabMenuSplit:))],
                      @[@"Split Terminal Down", NSStringFromSelector(@selector(tabMenuSplitDown:))],
                      @[],
                      @[[_tabs[i] all].count > 1 ? @"Close Terminals" : @"Close Terminal",
                        NSStringFromSelector(@selector(tabMenuClose:))]];
    for (NSArray *row in rows) {
        if (row.count == 0) { [m addItem:[NSMenuItem separatorItem]]; continue; }
        NSMenuItem *item = [m addItemWithTitle:row[0] action:NSSelectorFromString(row[1])
                                 keyEquivalent:@""];
        item.target = self;
        item.tag = (NSInteger)i;
    }
    return m;
}

- (void)tabMenuRename:(NSMenuItem *)item {
    if ((NSUInteger)item.tag < _tabs.count) [self renameTerminal:_tabs[(NSUInteger)item.tag].active];
}
- (void)tabMenuMoveLeft:(NSMenuItem *)item {
    if (item.tag > 0) [self moveTab:(NSUInteger)item.tag to:(NSUInteger)(item.tag - 1)];
}
- (void)tabMenuMoveRight:(NSMenuItem *)item {
    [self moveTab:(NSUInteger)item.tag to:(NSUInteger)(item.tag + 1)];
}
- (void)tabMenuSplit:(NSMenuItem *)item {
    [self selectTab:(NSUInteger)item.tag];
    [self splitActive];
}
- (void)tabMenuSplitDown:(NSMenuItem *)item {
    [self selectTab:(NSUInteger)item.tag];
    [self splitActiveDown];
}
- (void)tabMenuClose:(NSMenuItem *)item { [self closeTab:(NSUInteger)item.tag]; }

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(resizeTerminalPane:)) return [self hasFocus];
    if (item.action == @selector(tabMenuMoveLeft:)) return item.tag > 0;
    if (item.action == @selector(tabMenuMoveRight:)) return item.tag + 1 < (NSInteger)_tabs.count;
    return YES;
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
    // Also Close Terminal's action (Ctrl+Shift+W), which reaches the panel
    // before the window's controller when the keyboard is in a terminal,
    // and closed nothing: the active one, as the controller's would.
    if (![terminal isKindOfClass:TerminalView.class]) terminal = self.activeTerminal;
    for (NSUInteger i = 0; i < _tabs.count; i++) {
        MCTerminalTab *tab = _tabs[i];
        NSUInteger c = 0, r = 0;
        if (![tab find:terminal column:&c row:&r]) continue;
        if (![self confirmClosing:@[terminal]]) return NO;
        BOOL hadFocus = [terminal hasFocus];
        NSUInteger flat = [[tab all] indexOfObject:terminal];
        [self noteDroppedCommands:@[terminal]];
        [terminal close];
        [terminal removeFromSuperview];
        // Its space goes to its neighbour (TerminalLayout::removePane).
        [tab.columns[c] removeObjectAtIndex:r];
        if (tab.columns[c].count == 0) [tab.columns removeObjectAtIndex:c];
        TerminalLayout::removePane(tab->layout, c, r);
        NSArray<TerminalView *> *left = [tab all];
        if (left.count == 0) {
            [self removeTab:i];
        } else if (tab.active == terminal) {
            tab.active = left[flat > 0 ? flat - 1 : 0];
        }
        [self finishClose:hadFocus];
        return YES;
    }
    return YES;
}

- (BOOL)closeTab:(NSUInteger)i {
    if (i >= _tabs.count) return YES;
    NSArray<TerminalView *> *panes = [_tabs[i] all];
    if (![self confirmClosing:panes]) return NO;
    [self noteDroppedCommands:panes];
    BOOL hadFocus = NO;
    for (TerminalView *t in panes) {
        hadFocus = hadFocus || [t hasFocus];
        [t close];
        [t removeFromSuperview];
    }
    [self removeTab:i];
    [self finishClose:hadFocus];
    return YES;
}

- (void)closeAll {
    for (TerminalView *t in self.terminals) {
        [t close];
        [t removeFromSuperview];
    }
    [_tabs removeAllObjects];
    _selected = -1;
    [self stopObservingWindow];
    [self layoutPanes];
}

// Drops an emptied tab; the one to its left is shown when it was the shown one.
- (void)removeTab:(NSUInteger)tab {
    [_tabs removeObjectAtIndex:tab];
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

// The strip on top; below it the shown tab's columns, each a stack, sized
// by the tab's shares (TerminalLayout::frames), a drag bar on each boundary.
// Hidden tabs keep the same frames, so their programs keep the size they had.
- (void)layoutPanes {
    NSRect b = self.bounds;
    CGFloat W = b.size.width, H = MAX(0, b.size.height - kMCTabStripHeight);
    CGFloat stripH = MIN(kMCTabStripHeight, b.size.height);
    CGFloat buttonsW = MIN(2 * kButtonWidth, W);
    self.strip.frame = NSMakeRect(0, H, W - buttonsW, stripH);
    self.buttons.frame = NSMakeRect(W - buttonsW, H, buttonsW, stripH);
    [self reloadStrip];

    for (NSUInteger i = 0; i < _tabs.count; i++) {
        MCTerminalTab *tab = _tabs[i];
        std::vector<std::vector<TerminalLayout::Rect>> f =
            TerminalLayout::frames(tab->layout, W, H, kPaneGap);
        for (NSUInteger c = 0; c < tab.columns.count && c < f.size(); c++) {
            NSArray<TerminalView *> *column = tab.columns[c];
            for (NSUInteger r = 0; r < column.count && r < f[c].size(); r++) {
                const TerminalLayout::Rect &p = f[c][r];
                // The layout counts from the top; the panel is not flipped.
                column[r].frame = NSMakeRect(p.x, H - p.y - p.h, MAX(0, p.w), MAX(0, p.h));
                column[r].hidden = (NSInteger)i != _selected;
            }
        }
    }
    [self layoutBarsIn:NSMakeSize(W, H)];

    MCTerminalTab *shown = _selected >= 0 ? _tabs[_selected] : nil;
    TerminalView *active = shown.active;
    _activeMark.hidden = !(active && [shown all].count > 1);
    if (active) {
        NSRect fr = active.frame;
        // Along the bottom, under the input line, so it never runs into the
        // selected tab's underline.
        _activeMark.frame = NSMakeRect(NSMinX(fr), NSMinY(fr), fr.size.width, 2);
    }
}

// The bar with this place, made the first time one is needed. Each knows
// its place, so the block it reports to never changes, even mid-drag.
- (MCTerminalSplitBar *)barAt:(NSUInteger)index column:(NSUInteger)column row:(NSInteger)row {
    if (index == _bars.count) {
        MCTerminalSplitBar *bar = [[MCTerminalSplitBar alloc] initWithFrame:NSZeroRect];
        __weak MCTerminalPanel *weakSelf = self;
        __weak MCTerminalSplitBar *weakBar = bar;
        bar.onDrag = ^(CGFloat p) { [weakSelf splitBar:weakBar draggedTo:p]; };
        [self addSubview:bar positioned:NSWindowBelow relativeTo:_activeMark];
        [_bars addObject:bar];
    }
    MCTerminalSplitBar *bar = _bars[index];
    bar.column = column;
    bar.row = row;
    bar.vertical = row < 0;
    bar.hidden = NO;
    return bar;
}

// A vertical bar on each boundary between columns, and a horizontal one on
// each boundary within a column's stack, centred on the 1px gap.
- (void)layoutBarsIn:(NSSize)area {
    NSUInteger used = 0;
    MCTerminalTab *tab = _selected >= 0 ? _tabs[_selected] : nil;
    if (tab) {
        const TerminalLayout::Layout &l = tab->layout;
        const std::vector<double> widths = TerminalLayout::lengths(l.columns, area.width, kPaneGap);
        for (NSUInteger c = 0; c < l.columns.size(); c++) {
            if (c + 1 < l.columns.size()) {
                MCTerminalSplitBar *bar = [self barAt:used++ column:c row:-1];
                CGFloat x = TerminalLayout::boundaryAt(l.columns, c, area.width, kPaneGap);
                bar.frame = NSMakeRect(x - kDragBarHeight / 2, 0, kDragBarHeight, area.height);
            }
            if (c >= l.rows.size()) continue;
            CGFloat x0 = TerminalLayout::startOf(l.columns, c, area.width, kPaneGap);
            for (NSUInteger r = 0; r + 1 < l.rows[c].size(); r++) {
                MCTerminalSplitBar *bar = [self barAt:used++ column:c row:(NSInteger)r];
                CGFloat y = area.height -
                            TerminalLayout::boundaryAt(l.rows[c], r, area.height, kPaneGap);
                bar.frame = NSMakeRect(x0, y - kDragBarHeight / 2, widths[c], kDragBarHeight);
            }
        }
    }
    for (NSUInteger i = used; i < _bars.count; i++) _bars[i].hidden = YES;
}

- (NSArray<DragBar *> *)splitBars {
    NSMutableArray *out = [NSMutableArray array];
    for (MCTerminalSplitBar *bar in _bars)
        if (!bar.hidden) [out addObject:bar];
    return out;
}

// ------------------------------------------------------------------ titles
- (NSArray<NSString *> *)tabTitles {
    NSMutableArray *titles = [NSMutableArray array];
    for (MCTerminalTab *tab in _tabs) {
        NSMutableArray *parts = [NSMutableArray array];
        for (TerminalView *t in [tab all]) [parts addObject:t.title ?: @""];
        [titles addObject:[parts componentsJoinedByString:@" │ "]];
    }
    return titles;
}

// The tab's mark: waiting when any of its panes waits, else done when any
// is done.
- (MCTabAttention)tabAttention:(NSUInteger)tab {
    if (tab >= _tabs.count) return MCTabAttentionNone;
    MCTerminalAttention *att = [MCTerminalAttention shared];
    MCTabAttention mark = MCTabAttentionNone;
    for (TerminalView *t in [_tabs[tab] all]) {
        MCAttentionMark m = [att markForTerminal:t];
        if (m == MCAttentionWaiting) return MCTabAttentionWaiting;
        if (m == MCAttentionDone) mark = MCTabAttentionDone;
    }
    return mark;
}

// A waiting terminal's message comes first, so the tooltip says what it
// waits for ("Claude needs your permission to use Bash").
- (NSString *)tabToolTip:(NSUInteger)tab {
    if (tab >= _tabs.count) return nil;
    NSMutableArray *lines = [NSMutableArray array];
    MCTerminalAttention *att = [MCTerminalAttention shared];
    NSArray<TerminalView *> *panes = [_tabs[tab] all];
    for (TerminalView *t in panes) {
        NSString *m = [att markForTerminal:t] == MCAttentionWaiting ? [att messageForTerminal:t] : nil;
        if (m.length) [lines addObject:m];
        else if ([att markForTerminal:t] == MCAttentionWaiting) [lines addObject:@"Waiting for you"];
        else if ([att markForTerminal:t] == MCAttentionDone) [lines addObject:@"Done"];
    }
    for (TerminalView *t in panes)
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
