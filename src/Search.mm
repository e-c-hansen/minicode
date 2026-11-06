// Search.mm — Find in Folder, Replace in Folder and the TODO list (see
// Search.h). Objective-C++. A window laid out as VS Code's search view
// (searchView.ts, searchWidget.ts and searchview.css), out of FindWidget.h's
// pieces: the query with its toggles inside, the replace row under it with
// its chevron down the left edge and Replace All beside it, "files to
// include" and "files to exclude" behind the "..." button, a line saying
// what was found, and the results as a tree of files, each with its match
// count, holding its lines. Choosing a line calls back to open that file at
// it. The searching is FolderSearch's, off the main thread, a newer search
// cancelling the one before.
#import "Search.h"
#import "AppSettings.h"
#import "FindWidget.h"
#include "FolderSearch.h"
#include "TextFind.h"
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

static std::string SStd(NSString *s) { return s.UTF8String ?: ""; }
static NSString *SNS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
static std::u16string SU16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}
static NSString *SFromU16(const std::u16string &u) {
    return [NSString stringWithCharacters:(const unichar *)u.data() length:u.size()];
}

// One match: file path, 1-based line number, the line's text, and where the
// match starts in the raw line (bytes), for replacing just it.
@interface SearchHit : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, assign) NSInteger line;
@property(nonatomic, copy) NSString *text;
@property(nonatomic, assign) NSUInteger byteColumn;
@property(nonatomic, assign) NSUInteger count;   // matches on the line
@end
@implementation SearchHit
@end

// A file in the results tree: its lines, and how many matches they hold.
@interface SearchFile : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, strong) NSMutableArray<SearchHit *> *hits;
@property(nonatomic, assign) NSUInteger count;
@end
@implementation SearchFile
@end

// ============================================================ the views
// The content view, laid out by hand (flipped, top down).
@interface MCSearchContent : NSView
@property(nonatomic, copy) void (^onLayout)(void);
@property(nonatomic, strong) NSColor *background;
@end
@implementation MCSearchContent
- (BOOL)isFlipped { return YES; }
- (void)layout {
    [super layout];
    if (self.onLayout) self.onLayout();
}
- (void)drawRect:(NSRect)dirty {
    [(self.background ?: NSColor.windowBackgroundColor) setFill];
    NSRectFill(dirty);
}
@end

// VS Code's count badge: the number on a pill.
@interface MCSearchBadge : NSView
@property(nonatomic, copy) NSString *text;
@property(nonatomic, strong) MCFindStyle *style;
@end
@implementation MCSearchBadge
- (NSDictionary *)attrs {
    return @{NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular],
             NSForegroundColorAttributeName: self.style.badgeForeground ?: NSColor.labelColor};
}
- (CGFloat)widthNeeded { return MAX(18, ceil([self.text sizeWithAttributes:[self attrs]].width) + 12); }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    const CGFloat h = 18;
    NSRect pill = NSMakeRect(NSMaxX(b) - [self widthNeeded], round(NSMidY(b) - h / 2),
                             [self widthNeeded], h);
    [self.style.badgeBackground setFill];
    [[NSBezierPath bezierPathWithRoundedRect:pill xRadius:h / 2 yRadius:h / 2] fill];
    NSSize sz = [self.text sizeWithAttributes:[self attrs]];
    [self.text drawAtPoint:NSMakePoint(NSMidX(pill) - sz.width / 2, NSMidY(pill) - sz.height / 2)
            withAttributes:[self attrs]];
}
@end

// A row: VS Code's list colors, and the row's actions shown while the
// pointer is over it or it is selected.
@interface MCSearchRowView : NSTableRowView
@property(nonatomic, strong) MCFindStyle *style;
@property(nonatomic) BOOL hover;
@end

@interface MCSearchCell : NSTableCellView
@property(nonatomic, strong) NSTextField *name, *detail;
@property(nonatomic, strong) MCSearchBadge *badge;
@property(nonatomic, strong) NSArray<MCFindButton *> *actions;
@property(nonatomic) BOOL showActions;
@end

@implementation MCSearchRowView {
    NSTrackingArea *_tracking;
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
}
- (void)updateActions {
    for (NSView *v in self.subviews)
        if ([v isKindOfClass:[MCSearchCell class]])
            ((MCSearchCell *)v).showActions = self.hover || self.selected;
}
- (void)mouseEntered:(NSEvent *)e { (void)e; self.hover = YES; [self updateActions]; self.needsDisplay = YES; }
- (void)mouseExited:(NSEvent *)e { (void)e; self.hover = NO; [self updateActions]; self.needsDisplay = YES; }
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    [self updateActions];
}
- (void)didAddSubview:(NSView *)v {
    [super didAddSubview:v];
    [self updateActions];
}
- (void)drawBackgroundInRect:(NSRect)dirty {
    (void)dirty;
    if (self.hover && !self.selected) {
        [[self.style.toolbarHover colorWithAlphaComponent:0.5] setFill];
        NSRectFill(self.bounds);
    }
}
- (void)drawSelectionInRect:(NSRect)dirty {
    (void)dirty;
    [self.style.selection setFill];
    NSRectFill(self.bounds);
}
@end

@implementation MCSearchCell
// VS Code hides the count while a row shows its actions.
- (void)setShowActions:(BOOL)show {
    _showActions = show;
    for (MCFindButton *b in self.actions) b.hidden = !show;
    self.badge.hidden = show || !self.badge.text.length;
    self.needsLayout = YES;
}
- (void)layout {
    [super layout];
    const NSRect b = self.bounds;
    CGFloat right = NSMaxX(b) - 4;
    if (self.showActions) {
        for (MCFindButton *a in self.actions.reverseObjectEnumerator) {
            right -= 20;
            a.frame = NSMakeRect(right, round((b.size.height - 20) / 2), 20, 20);
            right -= 2;
        }
    } else if (!self.badge.hidden) {
        const CGFloat w = [self.badge widthNeeded];
        right -= 10;   // .monaco-count-badge margin-right 12, and 2 for the scroll bar
        self.badge.frame = NSMakeRect(right - w, 0, w, b.size.height);
        right -= w + 6;
    }
    CGFloat x = 0;
    if (self.imageView) {
        self.imageView.frame = NSMakeRect(0, round((b.size.height - 16) / 2), 16, 16);
        x = 22;
    }
    const CGFloat h = 17;
    const CGFloat y = round((b.size.height - h) / 2);
    [self.name sizeToFit];
    CGFloat nameW = MIN(NSWidth(self.name.frame), MAX(0, right - x));
    if (!self.detail) nameW = MAX(0, right - x);
    self.name.frame = NSMakeRect(x, y, nameW, h);
    if (self.detail) {
        const CGFloat dx = x + nameW + 6;
        self.detail.frame = NSMakeRect(dx, y + 1, MAX(0, right - dx), h - 1);
    }
}
@end

// The results: Return opens the selected line.
@interface MCSearchOutline : NSOutlineView
@property(nonatomic, copy) void (^onReturn)(void);
@end
@implementation MCSearchOutline
- (void)keyDown:(NSEvent *)e {
    if ((e.keyCode == 36 || e.keyCode == 76) && self.onReturn) {
        self.onReturn();
        return;
    }
    [super keyDown:e];
}
@end

// ============================================================ the panel
@interface SearchPanel () <NSOutlineViewDataSource, NSOutlineViewDelegate,
                           NSTextFieldDelegate, NSWindowDelegate> {
    NSString *_root;
    void (^_open)(NSString *, NSInteger);
    NSMutableArray<SearchHit *> *_hits;
    NSMutableArray<SearchFile *> *_files;
    NSUInteger _searchGeneration;   // drops a stale in-flight search's results
    NSString *_scope;               // the folder searched: the window's, or one outside it
    BOOL _todos;                    // the TODO list, not a text search
    NSArray<SearchHit *> *_allTodos;   // the last scan, before the filter
    std::shared_ptr<std::atomic<bool>> _cancel;   // the search or scan in flight
    MCFindStyle *_style;
    MCSearchContent *_content;
    MCFindInput *_queryInput, *_replaceInput, *_includeInput, *_excludeInput;
    MCFindButton *_case, *_word, *_regex, *_preserve;
    MCFindButton *_toggleReplace, *_replaceAll, *_more;
    NSTextField *_includeLabel, *_excludeLabel;
    NSScrollView *_scroll;
    BOOL _replaceShown, _detailsShown;
    BOOL _replacing;
    NSString *_afterNote;           // what a replace did, shown with the next results
    BOOL _afterNoteError;
    id _settingsObserver;
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSOutlineView *outline;
@end

@implementation SearchPanel

- (instancetype)initWithRoot:(NSString *)root
                 openHandler:(void (^)(NSString *, NSInteger))handler {
    if ((self = [super init])) {
        [self setUpWithRoot:root openHandler:handler];
    }
    return self;
}

- (instancetype)initTodosWithRoot:(NSString *)root
                      openHandler:(void (^)(NSString *, NSInteger))handler {
    if ((self = [super init])) {
        _todos = YES;
        [self setUpWithRoot:root openHandler:handler];
    }
    return self;
}

- (void)setUpWithRoot:(NSString *)root openHandler:(void (^)(NSString *, NSInteger))handler {
    _root = root;
    _scope = root;
    _open = [handler copy];
    _hits = [NSMutableArray array];
    _files = [NSMutableArray array];
    _style = [MCFindStyle current];
    [self build];
    __weak SearchPanel *weakSelf = self;
    _settingsObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:MCSettingsDidChangeNotification object:nil queue:nil
                usingBlock:^(NSNotification *n) { (void)n; [weakSelf applyStyle]; }];
}

- (void)dealloc {
    if (_cancel) _cancel->store(true);
    if (_settingsObserver) [[NSNotificationCenter defaultCenter] removeObserver:_settingsObserver];
}

// ------------------------------------------------------------------ build
- (MCFindInput *)input:(NSString *)placeholder toggles:(NSArray<MCFindButton *> *)toggles {
    MCFindInput *in = [[MCFindInput alloc] initWithPlaceholder:placeholder toggles:toggles];
    in.field.delegate = self;
    [_content addSubview:in];
    return in;
}

- (NSTextField *)label:(NSString *)text {
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont systemFontOfSize:11];
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    [_content addSubview:l];
    return l;
}

- (MCFindButton *)option:(MCFindIcon)icon tip:(NSString *)tip {
    return [MCFindButton toggle:icon tip:tip target:self action:@selector(optionToggled:)];
}

- (void)build {
    NSRect frame = NSMakeRect(0, 0, 680, 520);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = _todos ? @"TODOs" : @"Search";
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;   // the Find Options items reach the panel
    self.window.minSize = NSMakeSize(320, 260);
    [self.window center];

    _content = [[MCSearchContent alloc] initWithFrame:frame];
    __weak SearchPanel *weakSelf = self;
    _content.onLayout = ^{ [weakSelf layoutContent]; };
    self.window.contentView = _content;

    // The title bar's actions, as the view's title bar has them in VS Code.
    NSView *actions = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 3 * 24 + 8, 28)];
    MCFindButton *refresh = [MCFindButton button:MCFindIconRefresh tip:@"Refresh"
                                          target:self action:@selector(refresh:)];
    MCFindButton *clear = [MCFindButton button:MCFindIconClear tip:@"Clear Search Results"
                                        target:self action:@selector(clearResults:)];
    MCFindButton *collapse = [MCFindButton button:MCFindIconCollapseAll tip:@"Collapse All"
                                           target:self action:@selector(collapseAll:)];
    CGFloat ax = 0;
    for (MCFindButton *b in @[refresh, clear, collapse]) {
        b.frame = NSMakeRect(ax, 3, 22, 22);
        [actions addSubview:b];
        ax += 24;
    }
    NSTitlebarAccessoryViewController *acc = [[NSTitlebarAccessoryViewController alloc] init];
    acc.view = actions;
    acc.layoutAttribute = NSLayoutAttributeRight;
    [self.window addTitlebarAccessoryViewController:acc];

    if (!_todos) {
        _case = [self option:MCFindIconCase tip:@"Match Case (⌥⌘C)"];
        _word = [self option:MCFindIconWord tip:@"Match Whole Word (⌥⌘W)"];
        _regex = [self option:MCFindIconRegex tip:@"Use Regular Expression (⌥⌘R)"];
        _preserve = [self option:MCFindIconPreserveCase tip:@"Preserve Case (⌥⌘P)"];
        _toggleReplace = [MCFindButton button:MCFindIconChevronRight tip:@"Toggle Replace"
                                       target:self action:@selector(toggleReplace:)];
        _toggleReplace.radius = 4;
        [_content addSubview:_toggleReplace];
    }
    _queryInput = [self input:_todos ? @"Filter TODOs (TODO, FIXME, HACK, XXX, BUG and - [ ] tasks)"
                                     : @"Search"
                      toggles:_todos ? @[] : @[_case, _word, _regex]];
    if (!_todos) {
        _replaceInput = [self input:@"Replace" toggles:@[_preserve]];
        _replaceAll = [MCFindButton button:MCFindIconReplaceAll tip:@"Replace All (⏎ in the field)"
                                    target:self action:@selector(replaceAll:)];
        [_content addSubview:_replaceAll];
    }
    _more = [MCFindButton button:MCFindIconMore tip:@"Toggle Search Details"
                          target:self action:@selector(toggleDetails:)];
    _more.frame = NSMakeRect(0, 0, 25, 16);
    _more.radius = 3;
    [_content addSubview:_more];
    _includeLabel = [self label:@"files to include"];
    _includeInput = [self input:@"e.g. *.ts, src/**/include" toggles:@[]];
    _excludeLabel = [self label:@"files to exclude"];
    _excludeInput = [self input:@"e.g. *.ts, src/**/exclude" toggles:@[]];
    for (MCFindInput *in in @[_queryInput, _includeInput, _excludeInput]) in.fontSize = 13;

    self.status = [NSTextField labelWithString:@""];
    self.status.font = [NSFont systemFontOfSize:12];
    self.status.lineBreakMode = NSLineBreakByTruncatingTail;
    [_content addSubview:self.status];

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.hasVerticalScroller = YES;
    _scroll.borderType = NSNoBorder;
    _scroll.drawsBackground = NO;
    MCSearchOutline *outline = [[MCSearchOutline alloc] init];
    outline.onReturn = ^{ [weakSelf openSelected:nil]; };
    self.outline = outline;
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"hit"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [self.outline addTableColumn:col];
    self.outline.outlineTableColumn = col;
    self.outline.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    self.outline.headerView = nil;
    self.outline.rowHeight = 22;
    self.outline.indentationPerLevel = 14;
    self.outline.intercellSpacing = NSMakeSize(0, 0);
    self.outline.backgroundColor = NSColor.clearColor;
    self.outline.focusRingType = NSFocusRingTypeNone;
    if (@available(macOS 11.0, *)) self.outline.style = NSTableViewStyleFullWidth;
    self.outline.dataSource = self;
    self.outline.delegate = self;
    self.outline.doubleAction = @selector(openSelected:);
    self.outline.action = @selector(rowClicked:);
    self.outline.target = self;
    if (!_todos) {
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Result"];
        [menu addItemWithTitle:@"Open" action:@selector(openSelected:) keyEquivalent:@""].target = self;
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItemWithTitle:@"Replace This Match" action:@selector(replaceSelectedMatch:)
                 keyEquivalent:@""].target = self;
        [menu addItemWithTitle:@"Replace in This File" action:@selector(replaceSelectedFile:)
                 keyEquivalent:@""].target = self;
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItemWithTitle:@"Dismiss" action:@selector(dismissSelected:) keyEquivalent:@""].target = self;
        self.outline.menu = menu;
    }
    _scroll.documentView = self.outline;
    [_content addSubview:_scroll];

    // Tab goes query, replace, include, exclude, the results.
    NSMutableArray<NSView *> *loop = [NSMutableArray arrayWithObject:_queryInput.field];
    if (_replaceInput) [loop addObject:_replaceInput.field];
    [loop addObjectsFromArray:@[_includeInput.field, _excludeInput.field, self.outline]];
    for (NSUInteger i = 0; i < loop.count; i++) loop[i].nextKeyView = loop[(i + 1) % loop.count];

    [self setReplaceShown:NO];
    [self setDetailsShown:NO];
    [self applyStyle];
}

- (void)applyStyle {
    _style = [MCFindStyle current];
    self.window.appearance = _style.appearance;
    _content.background = _style.sideBarBackground;
    _content.needsDisplay = YES;
    for (MCFindInput *in in @[_queryInput, _includeInput, _excludeInput])
        in.style = _style;
    if (_replaceInput) _replaceInput.style = _style;
    _more.style = _style;
    _toggleReplace.style = _style;
    _replaceAll.style = _style;
    for (NSView *v in self.window.titlebarAccessoryViewControllers.firstObject.view.subviews)
        if ([v isKindOfClass:[MCFindButton class]]) ((MCFindButton *)v).style = _style;
    _includeLabel.textColor = _excludeLabel.textColor = _style.widgetForeground;
    [self showStatusColor];
    [self.outline reloadData];
    [self.outline expandItem:nil expandChildren:YES];
}

// searchview.css: the widgets 6 from the top, 2 from the left and 12 from
// the right; the chevron 16 wide down the query and replace rows; the
// inputs from 18 in, 26 high (25 for the details' boxes), 6 between rows;
// Replace All 4 after the replace input.
- (void)layoutContent {
    const CGFloat W = _content.bounds.size.width, H = _content.bounds.size.height;
    const CGFloat left = 2, right = 12, inset = left + 18, inH = 26;
    CGFloat y = 6;
    const CGFloat rowsTop = y;
    _queryInput.frame = NSMakeRect(inset, y, W - inset - right, inH);
    y += inH;
    if (_replaceInput && _replaceShown) {
        y += 6;
        _replaceInput.frame = NSMakeRect(inset, y, W - inset - right - 4 - 24, inH);
        _replaceAll.frame = NSMakeRect(W - right - 24, y + 1, 24, 24);
        y += inH;
    }
    _toggleReplace.frame = NSMakeRect(left, rowsTop, 16, y - rowsTop);
    // .query-details: the "..." at its right, then the two boxes.
    y += 4;
    _more.frame = NSMakeRect(W - right - 25 + 2, y, 25, 16);
    y += 16;
    if (_detailsShown) {
        _includeLabel.frame = NSMakeRect(inset, y, W - inset - right, 15);
        y += 17;
        _includeInput.frame = NSMakeRect(inset, y, W - inset - right, 25);
        y += 25;
        _excludeLabel.frame = NSMakeRect(inset, y + 2, W - inset - right, 15);
        y += 19;
        _excludeInput.frame = NSMakeRect(inset, y, W - inset - right, 25);
        y += 25 + 4;
    }
    // .message: padding 0 22 8.
    self.status.frame = NSMakeRect(22, y, W - 44, 16);
    y += 16 + 8;
    _scroll.frame = NSMakeRect(0, y, W, MAX(0, H - y));
}

- (void)setReplaceShown:(BOOL)shown {
    _replaceShown = shown && _replaceInput != nil;
    _replaceInput.hidden = _replaceAll.hidden = !_replaceShown;
    _toggleReplace.icon = _replaceShown ? MCFindIconChevronDown : MCFindIconChevronRight;
    _content.needsLayout = YES;
    [self.outline reloadData];
    [self.outline expandItem:nil expandChildren:YES];
}

- (void)setDetailsShown:(BOOL)shown {
    _detailsShown = shown;
    _includeLabel.hidden = _includeInput.hidden = !shown;
    _excludeLabel.hidden = _excludeInput.hidden = !shown;
    _content.needsLayout = YES;
}

- (void)toggleReplace:(id)sender {
    (void)sender;
    [self setReplaceShown:!_replaceShown];
    if (_replaceShown) [self.window makeFirstResponder:_replaceInput.field];
}
- (void)toggleDetails:(id)sender {
    (void)sender;
    [self setDetailsShown:!_detailsShown];
    if (_detailsShown) [self.window makeFirstResponder:_includeInput.field];
}

- (void)show {
    if (_todos) [self runSearch:nil];   // the list is scanned afresh each time
    [self.window makeKeyAndOrderFront:nil];
    [_content layoutSubtreeIfNeeded];
    [self.outline sizeLastColumnToFit];
    [self.window makeFirstResponder:_queryInput.field];
}

- (void)showReplace {
    if (_replaceInput) [self setReplaceShown:YES];
    [self show];
    if (_replaceInput) [self.window makeFirstResponder:_replaceInput.field];
}

- (void)seedQuery:(NSString *)query {
    if (!query.length || _todos) return;
    _queryInput.field.stringValue = query;
    [self runSearch:nil];
}

// ----------------------------------------------------------------- options
- (NSTextField *)field { return _queryInput.field; }
- (NSTextField *)replaceField { return _replaceInput.field; }
- (NSTextField *)includeField { return _includeInput.field; }
- (NSTextField *)excludeField { return _excludeInput.field; }
- (NSTableView *)table { return self.outline; }
- (BOOL)replaceShown { return _replaceShown; }
- (BOOL)detailsShown { return _detailsShown; }

- (BOOL)matchCase { return _case.on; }
- (BOOL)wholeWord { return _word.on; }
- (BOOL)regex { return _regex.on; }
- (BOOL)preserveCase { return _preserve.on; }
- (void)setOption:(MCFindButton *)b on:(BOOL)on {
    b.on = on;
    [self optionToggled:b];
}
- (void)setMatchCase:(BOOL)v { [self setOption:_case on:v]; }
- (void)setWholeWord:(BOOL)v { [self setOption:_word on:v]; }
- (void)setRegex:(BOOL)v { [self setOption:_regex on:v]; }
- (void)setPreserveCase:(BOOL)v { [self setOption:_preserve on:v]; }

- (void)optionToggled:(id)sender {
    if (sender == _preserve) {   // only the replacement changes: the previews
        [self.outline reloadData];
        [self.outline expandItem:nil expandChildren:YES];
        return;
    }
    [self runSearch:nil];
}

// Option+Cmd+C, W, R and P (Edit > Find Options), through the window delegate.
- (void)toggleFindMatchCase:(id)sender { if (!_todos) self.matchCase = !self.matchCase; }
- (void)toggleFindWholeWord:(id)sender { if (!_todos) self.wholeWord = !self.wholeWord; }
- (void)toggleFindRegex:(id)sender { if (!_todos) self.regex = !self.regex; }
- (void)toggleFindPreserveCase:(id)sender { if (!_todos) self.preserveCase = !self.preserveCase; }
- (void)toggleFindInSelection:(id)sender { NSBeep(); }

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    SEL a = item.action;
    BOOL on = NO;
    if (a == @selector(toggleFindMatchCase:)) on = self.matchCase;
    else if (a == @selector(toggleFindWholeWord:)) on = self.wholeWord;
    else if (a == @selector(toggleFindRegex:)) on = self.regex;
    else if (a == @selector(toggleFindPreserveCase:)) on = self.preserveCase;
    else if (a == @selector(toggleFindInSelection:)) return NO;
    else if (a == @selector(replaceSelectedMatch:))
        return !_replacing && [self hitForAction] != nil;
    else if (a == @selector(replaceSelectedFile:) || a == @selector(dismissSelected:))
        return !_replacing && [self.outline itemAtRow:[self rowForAction]] != nil;
    else return YES;
    item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    return !_todos;
}

- (FolderSearchOptions)options {
    FolderSearchOptions o;
    o.caseSensitive = self.matchCase;
    o.wholeWord = self.wholeWord;
    o.regex = self.regex;
    o.preserveCase = self.preserveCase;
    o.include = SStd(_includeInput.field.stringValue);
    o.exclude = SStd(_excludeInput.field.stringValue);
    // search.use-ignore-files: the folder's .gitignore files, from the
    // window's folder down, even when the scope is a folder inside it.
    o.useIgnoreFiles = [AppSettings shared].settings.searchUseIgnoreFiles();
    if (_root.length) o.ignoreTop = _root.fileSystemRepresentation;
    return o;
}

// ------------------------------------------------------------------- scope
// VS Code's Find in Folder from the explorer: a folder inside the window's
// is put in "files to include" as ./path; the window's own clears it; a
// folder outside it becomes what is searched.
- (void)setScope:(NSString *)dir {
    if (dir.length) {
        // Compared with symlinks resolved: /tmp is /private/tmp.
        NSString *real = dir.stringByResolvingSymlinksInPath.stringByStandardizingPath;
        NSString *realRoot = _root.stringByResolvingSymlinksInPath.stringByStandardizingPath;
        NSString *root = [realRoot hasSuffix:@"/"] ? realRoot : [realRoot stringByAppendingString:@"/"];
        if ([real isEqualToString:realRoot]) {
            _scope = _root;
            _includeInput.field.stringValue = @"";
        } else if ([real hasPrefix:root]) {
            _scope = _root;
            _includeInput.field.stringValue =
                [@"./" stringByAppendingString:[real substringFromIndex:root.length]];
            [self setDetailsShown:YES];
        } else {
            _scope = dir;
            _includeInput.field.stringValue = @"";
        }
        self.window.title = [NSString stringWithFormat:@"%@%@", _todos ? @"TODOs" : @"Search",
            [_scope isEqualToString:_root] ? @""
                : [NSString stringWithFormat:@" — %@", [_scope stringByAbbreviatingWithTildeInPath]]];
    }
    [self runSearch:nil];
}

// Search as the user types, but only after a pause, so we don't kick off a
// full folder scan on every keypress.
- (void)controlTextDidChange:(NSNotification *)note {
    id o = note.object;
    if (o == _replaceInput.field) {   // the previews follow the replacement
        [self.outline reloadData];
        [self.outline expandItem:nil expandChildren:YES];
        return;
    }
    if (o != _queryInput.field && o != _includeInput.field && o != _excludeInput.field) return;
    if (_todos && o == _queryInput.field) { [self filterTodos]; return; }
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(runSearchNow)
                                               object:nil];
    [self performSelector:@selector(runSearchNow) withObject:nil afterDelay:0.35];
}
- (void)runSearchNow { [self runSearch:nil]; }

- (void)controlTextDidBeginEditing:(NSNotification *)note { [self redrawInputs:note]; }
- (void)controlTextDidEndEditing:(NSNotification *)note { [self redrawInputs:note]; }
- (void)redrawInputs:(NSNotification *)note {
    (void)note;
    for (MCFindInput *in in @[_queryInput, _includeInput, _excludeInput]) in.needsDisplay = YES;
    _replaceInput.needsDisplay = YES;
}

// Return in the query searches now; in the replace field it is Replace All
// (it asks first).
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    if (sel == @selector(insertNewline:)) {
        if (control == _replaceInput.field) [self replaceAll:nil];
        else [self runSearch:nil];
        return YES;
    }
    return NO;
}

- (BOOL)busy { return _replacing || _cancel != nullptr; }

- (void)setStatus:(NSString *)text error:(BOOL)error {
    self.status.stringValue = text ?: @"";
    self.status.tag = error;
    self.status.toolTip = text.length > 80 ? text : nil;
    [self showStatusColor];
}
- (void)showStatusColor {
    self.status.textColor = self.status.tag ? _style.errorForeground : _style.muted;
}

// --------------------------------------------------------- the results tree
- (void)setResults:(NSArray<SearchHit *> *)hits {
    [_hits setArray:hits];
    [_files removeAllObjects];
    NSMutableDictionary<NSString *, SearchFile *> *byPath = [NSMutableDictionary dictionary];
    for (SearchHit *h in _hits) {
        SearchFile *f = byPath[h.path];
        if (!f) {
            f = [SearchFile new];
            f.path = h.path;
            f.hits = [NSMutableArray array];
            byPath[h.path] = f;
            [_files addObject:f];
        }
        [f.hits addObject:h];
        f.count += MAX((NSUInteger)1, h.count);
    }
    [self.outline reloadData];
    [self.outline expandItem:nil expandChildren:YES];
    _replaceAll.enabled = _hits.count > 0 && !_todos;
}

- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    (void)ov;
    if (!item) return (NSInteger)_files.count;
    return [item isKindOfClass:[SearchFile class]] ? (NSInteger)((SearchFile *)item).hits.count : 0;
}
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)index ofItem:(id)item {
    (void)ov;
    if (!item) return _files[(NSUInteger)index];
    return ((SearchFile *)item).hits[(NSUInteger)index];
}
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item {
    (void)ov;
    return [item isKindOfClass:[SearchFile class]];
}

- (NSTableRowView *)outlineView:(NSOutlineView *)ov rowViewForItem:(id)item {
    (void)ov;
    (void)item;
    MCSearchRowView *r = [MCSearchRowView new];
    r.style = _style;
    return r;
}

- (NSString *)relativePath:(NSString *)path {
    NSString *base = [_scope hasSuffix:@"/"] ? _scope : [_scope stringByAppendingString:@"/"];
    return [path hasPrefix:base] ? [path substringFromIndex:base.length] : path;
}

- (MCSearchCell *)cellFor:(NSOutlineView *)ov kind:(NSString *)kind {
    MCSearchCell *cell = [ov makeViewWithIdentifier:kind owner:self];
    if (cell) return cell;
    cell = [[MCSearchCell alloc] initWithFrame:NSMakeRect(0, 0, 300, 22)];
    cell.identifier = kind;
    cell.name = [NSTextField labelWithString:@""];
    cell.name.lineBreakMode = NSLineBreakByTruncatingTail;
    [cell addSubview:cell.name];
    cell.textField = cell.name;
    cell.badge = [MCSearchBadge new];
    [cell addSubview:cell.badge];
    const BOOL file = [kind isEqualToString:@"file"];
    if (file) {
        NSImageView *icon = [NSImageView new];
        [cell addSubview:icon];
        cell.imageView = icon;
        cell.detail = [NSTextField labelWithString:@""];
        cell.detail.lineBreakMode = NSLineBreakByTruncatingHead;
        cell.detail.font = [NSFont systemFontOfSize:12];
        [cell addSubview:cell.detail];
    }
    NSMutableArray *actions = [NSMutableArray array];
    if (!_todos) {
        MCFindButton *rep = [MCFindButton button:file ? MCFindIconReplaceAll : MCFindIconReplace
                                             tip:file ? @"Replace All in This File" : @"Replace"
                                          target:self action:@selector(rowReplace:)];
        [actions addObject:rep];
    }
    [actions addObject:[MCFindButton button:MCFindIconClose tip:@"Dismiss"
                                     target:self action:@selector(rowDismiss:)]];
    for (MCFindButton *b in actions) {
        b.hidden = YES;
        [cell addSubview:b];
    }
    cell.actions = actions;
    return cell;
}

- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item {
    (void)col;
    if ([item isKindOfClass:[SearchFile class]]) {
        SearchFile *f = item;
        MCSearchCell *cell = [self cellFor:ov kind:@"file"];
        NSString *rel = [self relativePath:f.path];
        cell.name.stringValue = rel.lastPathComponent;
        cell.name.font = [NSFont systemFontOfSize:13];
        cell.name.textColor = _style.widgetForeground;
        NSString *dir = rel.stringByDeletingLastPathComponent;
        cell.detail.stringValue = dir.length ? dir : @"";
        cell.detail.textColor = _style.muted;
        cell.imageView.image = [[NSWorkspace sharedWorkspace] iconForFile:f.path];
        cell.badge.style = _style;
        cell.badge.text = [NSString stringWithFormat:@"%lu", (unsigned long)f.count];
        cell.badge.hidden = NO;
        cell.toolTip = f.path;
        for (MCFindButton *b in cell.actions) b.style = _style;
        [cell setShowActions:cell.showActions];
        return cell;
    }
    SearchHit *h = item;
    MCSearchCell *cell = [self cellFor:ov kind:@"line"];
    cell.name.attributedStringValue = [self previewFor:h];
    cell.badge.text = @"";
    cell.badge.hidden = YES;
    cell.toolTip = [NSString stringWithFormat:@"%@:%ld", [self relativePath:h.path], (long)h.line];
    for (MCFindButton *b in cell.actions) b.style = _style;
    [cell setShowActions:cell.showActions];
    return cell;
}

// VS Code's line preview: the text before the match (cut to its last 26
// characters when long, after an ellipsis), the match highlighted, and the
// rest; with a replacement typed, each match struck through on red with
// what replaces it on green after it.
- (NSAttributedString *)previewFor:(SearchHit *)h {
    NSFont *font = [NSFont systemFontOfSize:13];
    NSDictionary *plain = @{NSFontAttributeName: font, NSForegroundColorAttributeName: _style.widgetForeground};
    const std::string line = SStd(h.text);
    std::vector<TextFind::Match> ms;
    const FolderSearchOptions o = [self options];
    const TextFind::Query q = _todos ? TextFind::Query() : FolderSearch::makeQuery(SStd(self.field.stringValue), o);
    if (q.usable()) ms = TextFind::findAllInLine(line, q);
    NSMutableAttributedString *a = [[NSMutableAttributedString alloc] init];
    const BOOL replacing = _replaceShown && !_todos;
    NSString *with = _replaceInput.field.stringValue ?: @"";
    size_t at = 0;
    for (size_t k = 0; k < ms.size(); ++k) {
        const TextFind::Match &m = ms[k];
        std::string before = line.substr(at, m.start - at);
        NSString *pre = SNS(before);
        if (k == 0 && pre.length > 40)
            pre = [@"…" stringByAppendingString:[pre substringFromIndex:pre.length - 26]];
        [a appendAttributedString:[[NSAttributedString alloc] initWithString:pre attributes:plain]];
        NSString *matched = SNS(line.substr(m.start, m.length));
        if (replacing && (with.length || o.regex)) {
            std::string out;
            NSString *rep = with;
            if (TextFind::replaceOneInText(line, q, SStd(with), 1, m.start, &out))
                rep = SNS(out.substr(m.start, out.size() - line.size() + m.length));
            NSMutableDictionary *gone = [plain mutableCopy];
            gone[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
            gone[NSBackgroundColorAttributeName] = _style.removedText;
            [a appendAttributedString:[[NSAttributedString alloc] initWithString:matched attributes:gone]];
            NSMutableDictionary *added = [plain mutableCopy];
            added[NSBackgroundColorAttributeName] = _style.insertedText;
            [a appendAttributedString:[[NSAttributedString alloc] initWithString:rep attributes:added]];
        } else {
            NSMutableDictionary *hit = [plain mutableCopy];
            hit[NSBackgroundColorAttributeName] = _style.matchHighlight;
            [a appendAttributedString:[[NSAttributedString alloc] initWithString:matched attributes:hit]];
        }
        at = m.start + m.length;
    }
    [a appendAttributedString:[[NSAttributedString alloc]
                                  initWithString:SNS(line.substr(MIN(at, line.size())))
                                      attributes:plain]];
    return a;
}

- (void)outlineViewSelectionDidChange:(NSNotification *)note {
    (void)note;
    [self.outline enumerateAvailableRowViewsUsingBlock:^(NSTableRowView *rv, NSInteger row) {
        (void)row;
        if ([rv isKindOfClass:[MCSearchRowView class]]) [(MCSearchRowView *)rv updateActions];
    }];
}

// ------------------------------------------------------------------ search
- (void)runSearch:(id)sender {
    if (_todos) { [self scanTodos]; return; }
    // A replace's note goes with the results it asked for, and no others.
    NSString *note = sender == self ? _afterNote : nil;
    const BOOL noteError = _afterNoteError;
    _afterNote = nil;
    // Cancel any in-flight search immediately (bumps the generation).
    ++_searchGeneration;
    if (_cancel) _cancel->store(true);
    _cancel = nullptr;
    NSString *query = self.field.stringValue;
    const FolderSearchOptions options = [self options];
    [self setResults:@[]];
    if (!FolderSearch::isSearchable(SStd(query), options)) {
        NSString *trimmed = [query stringByTrimmingCharactersInSet:
                             [NSCharacterSet whitespaceCharacterSet]];
        [self setStatus:trimmed.length ? @"Type at least 2 characters" : @"" error:NO];
        return;
    }

    [self setStatus:@"Searching…" error:NO];
    NSUInteger gen = _searchGeneration;
    const std::string root = _scope.fileSystemRepresentation;
    const std::string q = SStd(query);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        FolderSearchResult r = FolderSearch::search(root, q, cancel.get(), options);
        if (r.cancelled) return;
        NSMutableArray<SearchHit *> *found = [NSMutableArray array];
        NSMutableSet *files = [NSMutableSet set];
        for (const FolderSearchMatch &m : r.matches) {
            SearchHit *h = [SearchHit new];
            h.path = SNS(m.path);
            h.line = m.line;
            h.text = SNS(m.text);
            h.byteColumn = m.byteColumn;
            h.count = m.count;
            [found addObject:h];
            [files addObject:h.path];
        }
        NSString *error = r.error.empty() ? nil : SNS(r.error);
        const BOOL truncated = r.truncated;
        const unsigned long total = (unsigned long)r.totalMatches;
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s || s->_searchGeneration != gen) return;   // superseded
            s->_cancel = nullptr;
            [s setResults:found];
            if (error) {
                [s setStatus:error error:YES];
                return;
            }
            // Every match, as Replace All counts them, not the rows (lines).
            NSString *counts = [NSString stringWithFormat:@"%lu %@ in %lu %@%@",
                total, total == 1 ? @"match" : @"matches",
                (unsigned long)files.count, files.count == 1 ? @"file" : @"files",
                truncated ? @" (stopped at 2000)" : @""];
            if (note) {
                [s setStatus:[NSString stringWithFormat:@"%@. Now %@", note,
                                  found.count ? counts : @"no matches"]
                       error:noteError];
                return;
            }
            [s setStatus:found.count ? counts : @"No results found. Review your settings for "
                                                 "configured exclusions and check your "
                                                 "files to include."
                   error:NO];
        });
    });
}

- (void)refresh:(id)sender { (void)sender; [self runSearch:nil]; }

- (void)clearResults:(id)sender {
    (void)sender;
    ++_searchGeneration;
    if (_cancel) _cancel->store(true);
    _cancel = nullptr;
    self.field.stringValue = @"";
    if (_replaceInput) _replaceInput.field.stringValue = @"";
    [self setResults:@[]];
    [self setStatus:@"" error:NO];
    [self.window makeFirstResponder:self.field];
}

- (void)collapseAll:(id)sender {
    (void)sender;
    BOOL anyOpen = NO;
    for (SearchFile *f in _files) anyOpen = anyOpen || [self.outline isItemExpanded:f];
    if (anyOpen) [self.outline collapseItem:nil collapseChildren:YES];
    else [self.outline expandItem:nil expandChildren:YES];
}

// ------------------------------------------------------------------- TODOs
// Scan the folder for TODOs off the main thread; a newer scan cancels this one.
- (void)scanTodos {
    ++_searchGeneration;
    if (_cancel) _cancel->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    _allTodos = @[];
    [self setResults:@[]];
    [self setStatus:@"Looking for TODOs…" error:NO];
    NSUInteger gen = _searchGeneration;
    const std::string root = _scope.fileSystemRepresentation;
    const FolderSearchOptions options = [self options];
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        FolderSearchResult r = FolderSearch::findTodos(root, cancel.get(), options);
        NSMutableArray<SearchHit *> *found = [NSMutableArray array];
        for (const FolderSearchMatch &m : r.matches) {
            SearchHit *h = [SearchHit new];
            h.path = SNS(m.path);
            h.line = m.line;
            h.text = SNS(m.text);
            h.byteColumn = m.byteColumn;
            h.count = 1;
            [found addObject:h];
        }
        const BOOL truncated = r.truncated;
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s || s->_searchGeneration != gen) return;   // superseded
            s->_cancel = nullptr;
            s->_allTodos = found;
            [s filterTodos];
            if (truncated)
                s.status.stringValue = [s.status.stringValue
                    stringByAppendingString:@" (stopped at 2000)"];
        });
    });
}

// Show the scanned TODOs whose text or path holds what the field says.
- (void)filterTodos {
    NSString *q = [self.field.stringValue stringByTrimmingCharactersInSet:
                   [NSCharacterSet whitespaceCharacterSet]];
    NSMutableArray<SearchHit *> *shown = [NSMutableArray array];
    NSMutableSet *files = [NSMutableSet set];
    for (SearchHit *h in _allTodos) {
        if (q.length && [h.text rangeOfString:q options:NSCaseInsensitiveSearch].location == NSNotFound &&
            [h.path rangeOfString:q options:NSCaseInsensitiveSearch].location == NSNotFound)
            continue;
        [shown addObject:h];
        [files addObject:h.path];
    }
    [self setResults:shown];
    if (_allTodos.count == 0)
        [self setStatus:@"No TODOs in this folder" error:NO];
    else
        [self setStatus:[NSString stringWithFormat:@"%lu %@ in %lu %@%@",
            (unsigned long)shown.count, shown.count == 1 ? @"TODO" : @"TODOs",
            (unsigned long)files.count, files.count == 1 ? @"file" : @"files",
            q.length ? [NSString stringWithFormat:@" (of %lu)",
                        (unsigned long)_allTodos.count] : @""]
                  error:NO];
}

// ----------------------------------------------------------------- replace
- (NSInteger)rowForAction {
    NSInteger row = self.outline.clickedRow >= 0 ? self.outline.clickedRow : self.outline.selectedRow;
    return row >= 0 && row < self.outline.numberOfRows ? row : -1;
}

// The line a row's action is for (nil on a file row), and the file.
- (SearchHit *)hitForAction {
    id item = [self.outline itemAtRow:[self rowForAction]];
    return [item isKindOfClass:[SearchHit class]] ? item : nil;
}
- (NSString *)pathForAction {
    id item = [self.outline itemAtRow:[self rowForAction]];
    return [item isKindOfClass:[SearchFile class]] ? ((SearchFile *)item).path
           : [item isKindOfClass:[SearchHit class]] ? ((SearchHit *)item).path : nil;
}

- (void)replaceSelectedMatch:(id)sender {
    (void)sender;
    SearchHit *h = [self hitForAction];
    if (h) [self replaceMatchAtRow:(NSInteger)[_hits indexOfObjectIdenticalTo:h]];
}
- (void)replaceSelectedFile:(id)sender {
    (void)sender;
    NSString *path = [self pathForAction];
    if (!path || ![self readyToReplace]) return;
    _replacing = YES;
    [self replaceInPaths:@[path] hit:nil held:@[]];
}

// A row's own buttons act on that row.
- (NSInteger)rowOfButton:(NSView *)b { return [self.outline rowForView:b]; }
- (void)rowReplace:(id)sender {
    const NSInteger row = [self rowOfButton:sender];
    id item = [self.outline itemAtRow:row];
    if ([item isKindOfClass:[SearchHit class]]) {
        [self replaceMatchAtRow:(NSInteger)[_hits indexOfObjectIdenticalTo:item]];
    } else if ([item isKindOfClass:[SearchFile class]] && [self readyToReplace]) {
        _replacing = YES;
        [self replaceInPaths:@[((SearchFile *)item).path] hit:nil held:@[]];
    }
}
- (void)rowDismiss:(id)sender { [self dismissItem:[self.outline itemAtRow:[self rowOfButton:sender]]]; }
- (void)dismissSelected:(id)sender {
    (void)sender;
    [self dismissItem:[self.outline itemAtRow:[self rowForAction]]];
}

// VS Code's Dismiss: the row leaves the results (a file with its lines).
- (void)dismissItem:(id)item {
    NSMutableArray<SearchHit *> *left = [_hits mutableCopy];
    if ([item isKindOfClass:[SearchFile class]])
        [left removeObjectsInArray:((SearchFile *)item).hits];
    else if (item)
        [left removeObjectIdenticalTo:item];
    else
        return;
    [self setResults:left];
}

// A click on a file's row folds or unfolds it, as in VS Code; a line opens
// on a double click, or Return.
- (void)rowClicked:(id)sender {
    (void)sender;
    id item = [self.outline itemAtRow:self.outline.clickedRow];
    if (![item isKindOfClass:[SearchFile class]] || NSApp.currentEvent.clickCount > 1) return;
    if ([self.outline isItemExpanded:item]) [self.outline collapseItem:item];
    else [self.outline expandItem:item];
}

// The query, replacement and options as they are now, or NO (with a beep and
// a word in the status line) when there is nothing to replace with.
- (BOOL)readyToReplace {
    if (_todos || _replacing) {
        NSBeep();
        return NO;
    }
    const FolderSearchOptions o = [self options];
    if (!FolderSearch::isSearchable(SStd(self.field.stringValue), o)) {
        NSBeep();
        [self setStatus:@"Type what to find first" error:NO];
        return NO;
    }
    TextFind::Query q = FolderSearch::makeQuery(SStd(self.field.stringValue), o);
    if (!q.usable()) {
        NSBeep();
        [self setStatus:SNS(q.error()) error:YES];
        return NO;
    }
    return YES;
}

- (void)replaceAll:(id)sender {
    if (![self readyToReplace]) return;
    _replacing = YES;
    [self setStatus:@"Counting matches…" error:NO];
    const std::string root = _scope.fileSystemRepresentation;
    const std::string q = SStd(self.field.stringValue);
    const FolderSearchOptions o = [self options];
    NSString *with = self.replaceField.stringValue;
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        FolderReplacePlan plan = FolderSearch::planReplace(root, q, nullptr, o);
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        NSMutableArray<NSNumber *> *counts = [NSMutableArray array];
        NSMutableSet<NSString *> *outside = [NSMutableSet set];
        for (const FolderReplaceFile &f : plan.files) {
            [paths addObject:SNS(f.path)];
            [counts addObject:@(f.matches)];
            if (f.outside) [outside addObject:SNS(f.path)];
        }
        NSString *error = plan.error.empty() ? nil : SNS(plan.error);
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s) return;
            // Files whose tabs hold changes the disk lacks are left out of
            // the count, and named.
            NSMutableArray<NSString *> *todo = [NSMutableArray array], *held = [NSMutableArray array];
            NSMutableArray<NSString *> *away = [NSMutableArray array];
            size_t matches = 0;
            for (NSUInteger i = 0; i < paths.count; i++) {
                if (s.openState && s.openState(paths[i]) == MCOpenFileConflict) {
                    [held addObject:paths[i].lastPathComponent];
                    continue;
                }
                [todo addObject:paths[i]];
                matches += counts[i].unsignedLongValue;
                if ([outside containsObject:paths[i]])
                    [away addObject:[paths[i] substringFromIndex:MIN(paths[i].length,
                                                                     s->_scope.length + 1)]];
            }
            if (error || todo.count == 0) {
                s->_replacing = NO;
                NSString *why = error ?: held.count
                    ? [NSString stringWithFormat:@"Nothing replaced: the matches are in tabs with "
                                                  "unsaved changes (%@)",
                                                 [held componentsJoinedByString:@", "]]
                    : @"Nothing to replace";
                [s setStatus:why error:error != nil];
                return;
            }
            NSAlert *alert = [NSAlert new];
            alert.messageText = [NSString stringWithFormat:@"Replace %zu %@ in %lu %@?", matches,
                                 matches == 1 ? @"match" : @"matches", (unsigned long)todo.count,
                                 todo.count == 1 ? @"file" : @"files"];
            NSMutableString *info = [NSMutableString stringWithFormat:
                @"Each becomes “%@”%@. Files open in a tab are changed there, unsaved, and can be "
                 "undone there; the others are written to disk.", with,
                o.preserveCase ? @", in the case of what it replaces" : @""];
            if (away.count)
                [info appendFormat:@" Through symbolic links, these are files outside the folder, "
                                    "and they are changed too: %@.",
                                   [away componentsJoinedByString:@", "]];
            if (held.count)
                [info appendFormat:@" Left alone, since their tabs have unsaved changes: %@.",
                                   [held componentsJoinedByString:@", "]];
            alert.informativeText = info;
            [alert addButtonWithTitle:@"Replace"];
            [alert addButtonWithTitle:@"Cancel"];
            if ([alert runModal] != NSAlertFirstButtonReturn) {
                s->_replacing = NO;
                [s setStatus:@"Nothing replaced" error:NO];
                return;
            }
            [s replaceInPaths:todo hit:nil held:held];
        });
    });
}

- (void)replaceFileAtRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_hits.count || ![self readyToReplace]) return;
    _replacing = YES;
    [self replaceInPaths:@[_hits[(NSUInteger)row].path] hit:nil held:@[]];
}

- (void)replaceMatchAtRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_hits.count || ![self readyToReplace]) return;
    _replacing = YES;
    SearchHit *h = _hits[(NSUInteger)row];
    [self replaceInPaths:@[h.path] hit:h held:@[]];
}

// Replace every match in these files, or only `hit`'s. Tabs first (on the
// main thread, through the editor), then the other files on disk, off it.
// `held` names files already left out for their unsaved tabs.
- (void)replaceInPaths:(NSArray<NSString *> *)paths hit:(SearchHit *)hit
                  held:(NSArray<NSString *> *)held {
    const FolderSearchOptions o = [self options];
    const std::string q = SStd(self.field.stringValue);
    NSString *with = self.replaceField.stringValue;
    const TextFind::Query query = FolderSearch::makeQuery(q, o);
    const std::u16string with16 = SU16(with);

    NSMutableArray<NSString *> *tabPaths = [NSMutableArray array], *diskPaths = [NSMutableArray array];
    NSMutableArray<NSString *> *skipped = [held mutableCopy], *problems = [NSMutableArray array];
    NSMutableArray<NSArray<NSValue *> *> *tabRanges = [NSMutableArray array];
    NSMutableArray<NSArray<NSString *> *> *tabStrings = [NSMutableArray array];
    size_t tabMatches = 0;
    // A row's match: only while its line still reads as it did.
    const std::string expected = hit ? SStd(hit.text) : std::string();
    // One file under two names (a symlink, a hard link) is replaced once.
    std::set<std::pair<uint64_t, uint64_t>> seen;
    for (NSString *path in paths) {
        const FolderSearch::FileStamp st = FolderSearch::stampOf(path.fileSystemRepresentation);
        if (st.exists && !seen.insert({st.device, st.inode}).second) continue;
        MCOpenFileState state = self.openState ? self.openState(path) : MCOpenFileNone;
        if (state == MCOpenFileConflict) {
            [skipped addObject:path.lastPathComponent];
            continue;
        }
        NSString *text = state == MCOpenFileHere && self.tabText ? self.tabText(path) : nil;
        if (!text) {
            [diskPaths addObject:path];
            continue;
        }
        const std::u16string u = SU16(text);
        std::vector<TextFind::Edit16> edits;
        if (hit) {
            TextFind::Edit16 e;
            std::string now;
            if (FolderSearch::lineAt(SStd(text), (int)hit.line, &now) &&
                FolderSearch::displayText(now, o.maxLineChars) == expected &&
                TextFind::replaceOneAt(u, query, with16, (int)hit.line, hit.byteColumn, &e))
                edits.push_back(e);
        } else {
            edits = TextFind::replaceAll(u, query, with16);
        }
        if (edits.empty()) continue;
        NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
        NSMutableArray<NSString *> *strings = [NSMutableArray array];
        for (const TextFind::Edit16 &e : edits) {
            [ranges addObject:[NSValue valueWithRange:NSMakeRange(e.start, e.length)]];
            [strings addObject:SFromU16(e.text)];
        }
        [tabPaths addObject:path];
        [tabRanges addObject:ranges];
        [tabStrings addObject:strings];
        tabMatches += edits.size();
    }
    size_t tabFiles = 0;
    if (tabPaths.count) {
        NSString *failure = nil;
        NSInteger n = self.editTabs ? self.editTabs(tabPaths, tabRanges, tabStrings, &failure) : -1;
        if (n < 0) {
            [problems addObject:failure ?: @"the open tabs could not be changed"];
            tabMatches = 0;
        } else {
            tabFiles = (size_t)n;
        }
    }

    const int line = hit ? (int)hit.line : 0;
    const size_t byteColumn = hit ? hit.byteColumn : 0;
    const std::string with8 = SStd(with);
    const BOOL one = hit != nil;
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        size_t files = 0, matches = 0;
        NSMutableArray<NSString *> *failed = [NSMutableArray array];
        for (NSString *path in diskPaths) {
            const std::string p = path.fileSystemRepresentation;
            std::string content, out, error;
            if (!FolderSearch::readTextFile(p, FolderSearchOptions().maxFileSize, &content)) {
                [failed addObject:[NSString stringWithFormat:@"%@ could not be read as text",
                                                             path.lastPathComponent]];
                continue;
            }
            size_t n = 0;
            if (one) {
                if (FolderSearch::replaceMatchInText(content, q, with8, o, line, byteColumn, &out,
                                                     &expected))
                    n = 1;
            } else {
                out = FolderSearch::replaceInText(content, q, with8, o, &n);
            }
            if (n == 0 || out == content) continue;
            if (!FolderSearch::writeFileAtomically(p, out, &error)) {
                [failed addObject:[NSString stringWithFormat:@"%@: %s", path.lastPathComponent,
                                                             error.c_str()]];
                continue;
            }
            files++;
            matches += n;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s) return;
            s->_replacing = NO;
            [problems addObjectsFromArray:failed];
            const size_t total = matches + tabMatches, nfiles = files + tabFiles;
            NSMutableString *msg = [NSMutableString string];
            if (total == 0 && !skipped.count && !problems.count)
                [msg appendString:one ? @"That match is no longer there" : @"Nothing to replace"];
            else
                [msg appendFormat:@"Replaced %zu %@ in %zu %@", total,
                                  total == 1 ? @"match" : @"matches", nfiles,
                                  nfiles == 1 ? @"file" : @"files"];
            if (tabFiles)
                [msg appendFormat:@" (%zu open in tabs, unsaved)", tabFiles];
            if (skipped.count)
                [msg appendFormat:@". Left alone, with unsaved changes: %@",
                                  [skipped componentsJoinedByString:@", "]];
            if (problems.count)
                [msg appendFormat:@". %@", [problems componentsJoinedByString:@"; "]];
            // The results as they are now, after this note.
            s->_afterNote = msg;
            s->_afterNoteError = problems.count > 0;
            [s runSearch:s];
            if (!s.busy) [s setStatus:msg error:s->_afterNoteError];
        });
    });
}

// ------------------------------------------------------------- open
- (void)openSelected:(id)sender {
    (void)sender;
    id item = [self.outline itemAtRow:[self rowForAction]];
    if ([item isKindOfClass:[SearchFile class]]) item = ((SearchFile *)item).hits.firstObject;
    SearchHit *h = item;
    if (h && _open) _open(h.path, h.line);
}

@end
