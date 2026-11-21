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
#import "FileTypeIcons.h"
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

// The most lines a search shows, as VS Code's (search.maxResults).
static const std::size_t kMaxResults = 20000;
static NSString *const kNoReplaceOverSsh =
    @"Replace in Folder does not work in a window over SSH yet: its files are on the other machine. "
    @"Replace in each file's tab, or in the terminal there.";

static NSString *MCGrouped(unsigned long n) {
    return [NSNumberFormatter localizedStringFromNumber:@(n)
                                            numberStyle:NSNumberFormatterDecimalStyle];
}

// The line under the results naming the files a search left out, the
// files themselves in its tooltip; nil when none were. One file is named
// in the line itself ("app.log was not searched: it is not UTF-8"; since
// 2026-11-18 text that is not UTF-8 is read as Windows-1252, so only
// UTF-16 Replace in Folder could not take back says so), more
// are counted ("12 files not searched: 3 with a line over 64 MB, 9 not
// UTF-8"). A large text file is searched (FolderSearch reads it in
// pieces), so only a line past the limit leaves one out for its size.
static NSString *MCSkippedNote(const FolderSearchResult &r, NSString **tooltip) {
    const unsigned long large = r.skippedTooLarge, other = r.skippedNotUtf8;
    if (!large && !other) return nil;
    const unsigned long mb =
        (unsigned long)(FolderSearchOptions().maxStreamedLine / (1024 * 1024));
    NSString *lineReason = [NSString stringWithFormat:@"a line longer than %lu MB", mb];
    NSMutableString *tip = [NSMutableString string];
    for (const FolderSearchSkip &k : r.skipped)
        [tip appendFormat:@"%@%@ (%@)", tip.length ? @"\n" : @"", SNS(k.relativePath),
                          k.tooLarge ? lineReason : @"not UTF-8"];
    if (large + other > r.skipped.size())
        [tip appendFormat:@"\nand %@ more", MCGrouped(large + other - r.skipped.size())];
    *tooltip = tip;
    if (large + other == 1 && r.skipped.size() == 1)
        return [NSString stringWithFormat:@"%@ was not searched: %@",
                                          SNS(r.skipped[0].relativePath),
                                          large ? [@"it has " stringByAppendingString:lineReason]
                                                : @"it is not UTF-8"];
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    if (large)
        [parts addObject:[NSString stringWithFormat:@"%@ with a line over %lu MB",
                                                    MCGrouped(large), mb]];
    if (other) [parts addObject:[NSString stringWithFormat:@"%@ not UTF-8", MCGrouped(other)]];
    return [NSString stringWithFormat:@"%@ files not searched: %@", MCGrouped(large + other),
                                      [parts componentsJoinedByString:@", "]];
}

// Text that is not UTF-8 and holds no NUL: what the search reads as
// Windows-1252 (FolderSearch::decodeWindows1252) and Replace in Folder,
// which writes UTF-8 or UTF-16 only, leaves alone.
static BOOL MCReadsAsWindows1252(const std::string &path) {
    std::string bytes;
    FolderSearch::FileStamp st;
    if (FolderSearch::readFileStable(path, FolderSearchOptions().maxFileSize, &bytes, &st) !=
        FolderSearch::ReadStatus::Ok)
        return NO;
    return FolderSearch::readsAsWindows1252(bytes);
}

// Mostly UTF-8, with a few stray bytes (FolderSearch::readsAsUtf8WithStrayBytes),
// which Replace in Folder leaves alone too.
static BOOL MCReadsAsUtf8WithStrayBytes(const std::string &path) {
    std::string bytes;
    FolderSearch::FileStamp st;
    if (FolderSearch::readFileStable(path, FolderSearchOptions().maxFileSize, &bytes, &st) !=
        FolderSearch::ReadStatus::Ok)
        return NO;
    return FolderSearch::readsAsUtf8WithStrayBytes(bytes);
}

// Replace in Folder rewrites a file whole in memory, so it leaves a file
// over FolderSearch's whole-read size alone (the search still finds its
// matches): "1 MB".
static NSString *MCReplaceSizeLimit(void) {
    return [NSString stringWithFormat:@"%lu MB",
                     (unsigned long)(FolderSearchOptions().maxFileSize / (1024 * 1024))];
}

// One match: file path, 1-based line number, the line's text, and where the
// match starts in the raw line (bytes), for replacing just it.
@interface SearchHit : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, assign) NSInteger line;
@property(nonatomic, copy) NSString *text;
@property(nonatomic, assign) NSUInteger byteColumn;
@property(nonatomic, assign) NSUInteger count;   // matches on the line
// The lines just above and below (FolderSearchMatch::before and after),
// drawn muted around it with their numbers, as Zed and VS Code show a hit
// in its excerpt (C3, 2026-11-18: each hit was a bare line).
@property(nonatomic, copy) NSArray<NSString *> *before, *after;
// Digits in the largest line number its file's rows show, so the numbers
// line up down the file.
@property(nonatomic, assign) NSUInteger digits;
@end
@implementation SearchHit
@end

// Lines of context above and below each hit (FolderSearchOptions::
// contextLines), and the height a line of a hit's row takes.
static const std::size_t kContextLines = 1;
static const CGFloat kExcerptLine = 18;

static NSArray<NSString *> *MCLines(const std::vector<std::string> &lines) {
    NSMutableArray<NSString *> *a = [NSMutableArray arrayWithCapacity:lines.size()];
    for (const std::string &l : lines) [a addObject:SNS(l)];
    return a;
}

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
// Cmd+G and Shift+Cmd+G anywhere in the window: the next or previous result.
@property(nonatomic, copy) void (^onStep)(BOOL forward);
// Option+Cmd+Return anywhere in the window: Replace All, as VS Code's
// search view has it (the Edit menu's Replace All is the editor's).
@property(nonatomic, copy) void (^onReplaceAll)(void);
@property(nonatomic, strong) NSColor *background;
@end
@implementation MCSearchContent
- (BOOL)isFlipped { return YES; }
- (void)layout {
    [super layout];
    if (self.onLayout) self.onLayout();
}
// The window offers key equivalents to its views before the menu, so the
// keys stay here whatever in the window has the keyboard: Shift+Cmd+G is
// Find Previous in this window and Toggle Source Control's menu key
// everywhere else (Commands.cpp).
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    const NSEventModifierFlags mods =
        e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                           NSEventModifierFlagOption | NSEventModifierFlagControl);
    if (self.onStep && e.keyCode == 5 /* kVK_ANSI_G */ &&
        (mods == NSEventModifierFlagCommand ||
         mods == (NSEventModifierFlagCommand | NSEventModifierFlagShift))) {
        self.onStep(!(mods & NSEventModifierFlagShift));
        return YES;
    }
    if (self.onReplaceAll && (e.keyCode == 36 || e.keyCode == 76) /* Return, keypad Enter */ &&
        mods == (NSEventModifierFlagCommand | NSEventModifierFlagOption)) {
        self.onReplaceAll();
        return YES;
    }
    return [super performKeyEquivalent:e];
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
// A line's row: its number, left of the text, and the lines around it,
// each with its number (kContextLines above, then as many below; those
// not used hidden).
@property(nonatomic, strong) NSTextField *number;
@property(nonatomic, strong) NSArray<NSTextField *> *contextNumbers, *contextTexts;
@property(nonatomic) NSUInteger beforeCount, afterCount;
@property(nonatomic) CGFloat gutter;   // the numbers' width
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
// Top down, for a line's row of several lines.
- (BOOL)isFlipped { return YES; }
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
    // A line's row: the line itself kExcerptLine high after its lines
    // above, the actions beside it.
    const CGFloat hitY = self.number ? 2 + self.beforeCount * kExcerptLine
                                     : round((b.size.height - kExcerptLine) / 2);
    if (self.showActions) {
        for (MCFindButton *a in self.actions.reverseObjectEnumerator) {
            right -= 20;
            a.frame = NSMakeRect(right, self.number ? hitY - 1 : round((b.size.height - 20) / 2),
                                 20, 20);
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
    if (self.number) {
        // The numbers right-aligned in their gutter; the lines around in
        // the full width (the actions sit beside the line itself).
        const CGFloat tx = self.gutter + 8, full = NSMaxX(b) - 4;
        self.number.frame = NSMakeRect(0, hitY + 1, self.gutter, kExcerptLine - 1);
        for (NSUInteger i = 0; i < self.contextTexts.count; i++) {
            const BOOL above = i < kContextLines;
            const NSUInteger k = above ? i : i - kContextLines;
            const BOOL used = above ? k < self.beforeCount : k < self.afterCount;
            self.contextNumbers[i].hidden = self.contextTexts[i].hidden = !used;
            if (!used) continue;
            const CGFloat y = above ? 2 + k * kExcerptLine : hitY + (k + 1) * kExcerptLine;
            self.contextNumbers[i].frame = NSMakeRect(0, y + 1, self.gutter, kExcerptLine - 1);
            self.contextTexts[i].frame = NSMakeRect(tx, y, MAX(0, full - tx), kExcerptLine - 1);
        }
        self.name.frame = NSMakeRect(tx, hitY, MAX(0, right - tx), kExcerptLine - 1);
        return;
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
    uint64_t _hostReq;              // over SSH: the host's search in flight (hostSearch)
    MCFindStyle *_style;
    MCSearchContent *_content;
    MCFindInput *_queryInput, *_replaceInput, *_includeInput, *_excludeInput;
    MCFindButton *_case, *_word, *_regex, *_preserve;
    MCFindButton *_toggleReplace, *_replaceAll, *_more;
    NSTextField *_includeLabel, *_excludeLabel;
    NSScrollView *_scroll;
    NSTextField *_skippedNote;      // the files a search left out, under the results
    BOOL _replaceShown, _detailsShown;
    BOOL _replacing;
    NSString *_afterNote;           // what a replace did, shown with the next results
    BOOL _afterNoteError;
    // The rows dismissed since the last search, which Replace All and
    // Replace in This File leave alone (FolderReplaceExclusions).
    FolderReplaceExclusions _dismissed;
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
    if (_hostReq && _cancelHostSearch) _cancelHostSearch(_hostReq);
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
    _content.onStep = ^(BOOL forward) { [weakSelf stepResult:forward]; };
    if (!_todos) _content.onReplaceAll = ^{ [weakSelf replaceAllFromKey]; };
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
        _replaceAll = [MCFindButton button:MCFindIconReplaceAll tip:@"Replace All (⌥⌘⏎)"
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

    _skippedNote = [NSTextField labelWithString:@""];
    _skippedNote.font = [NSFont systemFontOfSize:12];
    _skippedNote.lineBreakMode = NSLineBreakByTruncatingTail;
    _skippedNote.hidden = YES;
    [_content addSubview:_skippedNote];

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
    // The skipped files' line, under the results.
    const CGFloat note = _skippedNote.hidden ? 0 : 16 + 8;
    _skippedNote.frame = NSMakeRect(22, MAX(y, H - 16 - 4), W - 44, 16);
    _scroll.frame = NSMakeRect(0, y, W, MAX(0, H - y - note));
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

- (void)showReplace { [self showReplaceFocusingReplace:YES]; }

- (void)showReplaceFocusingReplace:(BOOL)focusReplace {
    if (_replaceInput) [self setReplaceShown:YES];
    [self show];   // the keyboard in the query, its text selected
    if (_replaceInput && focusReplace) [self.window makeFirstResponder:_replaceInput.field];
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
        return !_replacing && !self.hostSearch && [self hitForAction] != nil;
    else if (a == @selector(replaceSelectedFile:))
        return !_replacing && !self.hostSearch && [self.outline itemAtRow:[self rowForAction]] != nil;
    else if (a == @selector(dismissSelected:))
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
    o.maxMatches = kMaxResults;
    if (!_todos) o.contextLines = kContextLines;
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

// Return in any field searches now, the replace field's too, as in VS
// Code's search view: Replace All is its button or Option+Cmd+Return, and
// asks first. (Return there was Replace All until 1.6.4, and text typed
// into the window by mistake, then Return, rewrote files.)
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    (void)control;
    if (sel == @selector(insertNewline:)) {
        [self runSearch:nil];
        return YES;
    }
    return NO;
}

// Option+Cmd+Return: Replace All while the replace row shows (its field
// is what replaces; hidden, it is not shown text to write).
- (void)replaceAllFromKey {
    if (!_replaceShown) {
        NSBeep();
        return;
    }
    [self replaceAll:nil];
}

- (BOOL)busy { return _replacing || _cancel != nullptr || _hostReq != 0; }

- (void)setStatus:(NSString *)text error:(BOOL)error {
    self.status.stringValue = text ?: @"";
    self.status.tag = error;
    self.status.toolTip = text.length > 80 ? text : nil;
    [self showStatusColor];
}
- (void)showStatusColor {
    self.status.textColor = self.status.tag ? _style.errorForeground : _style.muted;
    _skippedNote.textColor = _style.muted;
}

- (NSTextField *)skippedNote { return _skippedNote; }

- (void)setSkippedNote:(NSString *)text tooltip:(NSString *)tooltip {
    const BOOL hidden = text.length == 0;
    _skippedNote.stringValue = text ?: @"";
    _skippedNote.toolTip = tooltip;
    if (_skippedNote.hidden != hidden) {
        _skippedNote.hidden = hidden;
        _content.needsLayout = YES;
    }
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
    for (SearchFile *f in _files) {
        NSInteger last = 0;
        for (SearchHit *h in f.hits) last = MAX(last, h.line + (NSInteger)h.after.count);
        const NSUInteger digits = [NSString stringWithFormat:@"%ld", (long)last].length;
        for (SearchHit *h in f.hits) h.digits = digits;
    }
    [self.outline reloadData];
    [self.outline expandItem:nil expandChildren:YES];
    _replaceAll.enabled = _hits.count > 0 && !_todos && !self.hostSearch;
    if (self.hostSearch) _replaceAll.toolTip = kNoReplaceOverSsh;
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

// A line's row is as tall as its lines: the line and those around it.
- (CGFloat)outlineView:(NSOutlineView *)ov heightOfRowByItem:(id)item {
    (void)ov;
    if (![item isKindOfClass:[SearchHit class]]) return 22;
    SearchHit *h = item;
    return 4 + kExcerptLine * (CGFloat)(1 + h.before.count + h.after.count);
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
    } else {
        auto label = ^NSTextField *(BOOL number) {
            NSTextField *t = [NSTextField labelWithString:@""];
            t.font = number ? [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular]
                            : [NSFont systemFontOfSize:12];
            t.alignment = number ? NSTextAlignmentRight : NSTextAlignmentLeft;
            t.lineBreakMode = number ? NSLineBreakByClipping : NSLineBreakByTruncatingTail;
            [cell addSubview:t];
            return t;
        };
        cell.number = label(YES);
        NSMutableArray<NSTextField *> *numbers = [NSMutableArray array], *texts = [NSMutableArray array];
        for (std::size_t i = 0; i < 2 * kContextLines; i++) {
            [numbers addObject:label(YES)];
            [texts addObject:label(NO)];
        }
        cell.contextNumbers = numbers;
        cell.contextTexts = texts;
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
        // Its type's icon, as the tree shows it (FileTypeIcons.h).
        const FileIcon::Type type = MCFileTypeOf(f.path, NO);
        cell.imageView.image = MCFileTypeImage(type);
        cell.imageView.contentTintColor =
            MCFileTypeTint(type, MCFileTypeTextIsLight(_style.widgetForeground));
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
    // Its number, and the lines around it, muted, with theirs.
    static CGFloat digitWidth = 0;
    if (!digitWidth)
        digitWidth = [@"0" sizeWithAttributes:@{NSFontAttributeName: cell.number.font}].width;
    cell.gutter = ceil(digitWidth * MAX((NSUInteger)2, h.digits)) + 2;
    cell.number.stringValue = [NSString stringWithFormat:@"%ld", (long)h.line];
    cell.number.textColor = _style.muted;
    cell.beforeCount = MIN(h.before.count, kContextLines);
    cell.afterCount = MIN(h.after.count, kContextLines);
    for (NSUInteger i = 0; i < cell.contextTexts.count; i++) {
        const BOOL above = i < kContextLines;
        const NSUInteger k = above ? i : i - kContextLines;
        NSArray<NSString *> *lines = above ? h.before : h.after;
        if (k >= lines.count) continue;
        const NSInteger n = above ? h.line - (NSInteger)h.before.count + (NSInteger)k
                                  : h.line + 1 + (NSInteger)k;
        cell.contextNumbers[i].stringValue = [NSString stringWithFormat:@"%ld", (long)n];
        cell.contextNumbers[i].textColor = _style.muted;
        cell.contextTexts[i].stringValue = lines[k];
        cell.contextTexts[i].textColor = _style.muted;
    }
    cell.needsLayout = YES;
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
    if (_hostReq && self.cancelHostSearch) self.cancelHostSearch(_hostReq);
    _hostReq = 0;
    NSString *query = self.field.stringValue;
    const FolderSearchOptions options = [self options];
    [self setResults:@[]];
    [self setSkippedNote:nil tooltip:nil];
    _dismissed.clear();   // a new search shows every row again, as VS Code's does
    if (!FolderSearch::isSearchable(SStd(query), options)) {
        NSString *trimmed = [query stringByTrimmingCharactersInSet:
                             [NSCharacterSet whitespaceCharacterSet]];
        [self setStatus:trimmed.length ? @"Type at least 2 characters" : @"" error:NO];
        return;
    }

    [self setStatus:@"Searching…" error:NO];
    NSUInteger gen = _searchGeneration;
    if (self.hostSearch) {
        [self runHostSearch:query options:options generation:gen];
        return;
    }
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
            h.before = MCLines(m.before);
            h.after = MCLines(m.after);
            [found addObject:h];
            [files addObject:h.path];
        }
        NSString *error = r.error.empty() ? nil : SNS(r.error);
        const BOOL truncated = r.truncated, tooSlow = r.tooSlow;
        const unsigned long total = (unsigned long)r.totalMatches;
        NSString *skipTip = nil;
        NSString *skipped = MCSkippedNote(r, &skipTip);
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s || s->_searchGeneration != gen) return;   // superseded
            s->_cancel = nullptr;
            [s setResults:found];
            [s setSkippedNote:skipped tooltip:skipTip];
            [s showFound:found.count files:files.count total:total truncated:truncated
                 tooSlow:tooSlow stopped:nil error:error note:note noteError:noteError];
        });
    });
}

// The line under the query once a search is done: its counts, or why it
// stopped.
- (void)showFound:(NSUInteger)rows
            files:(NSUInteger)files
            total:(unsigned long)total
        truncated:(BOOL)truncated
          tooSlow:(BOOL)tooSlow
          stopped:(NSString *)stopped
            error:(NSString *)error
             note:(NSString *)note
        noteError:(BOOL)noteError {
    if (error) {
        [self setStatus:error error:YES];
        return;
    }
    // A pattern std::regex took too long over: what came before it.
    NSString *slow = @"This pattern takes too long; the search stopped";
    if (tooSlow && !rows) {
        [self setStatus:slow error:YES];
        return;
    }
    // Every match, as Replace All counts them, not the rows (lines).
    NSString *counts = [NSString stringWithFormat:@"%@ %@ in %@ %@%@",
        MCGrouped(total), total == 1 ? @"match" : @"matches",
        MCGrouped(files), files == 1 ? @"file" : @"files",
        tooSlow ? [@". " stringByAppendingString:slow]
        : truncated ? [NSString stringWithFormat:@". Stopped at %@ results",
                                                 MCGrouped(kMaxResults)]
        : stopped ? [@". " stringByAppendingString:stopped]
                    : @""];
    if (tooSlow) {
        [self setStatus:counts error:YES];
        return;
    }
    if (note) {
        [self setStatus:[NSString stringWithFormat:@"%@. Now %@", note,
                          rows ? counts : @"no matches"]
               error:noteError];
        return;
    }
    [self setStatus:rows ? counts : @"No results found. Review your settings for "
                                     "configured exclusions and check your "
                                     "files to include."
           error:NO];
}

// Over SSH: the host searches its folder (SessionSearch.h's "search", the
// Mac's options) and its rows come in batches as it goes, each shown as it
// arrives; the last says what it counted and whether it stopped.
- (void)runHostSearch:(NSString *)query options:(const FolderSearchOptions &)o generation:(NSUInteger)gen {
    Json ask = Json::object();
    ask.set("query", Json(SStd(query)));
    ask.set("caseSensitive", Json(o.caseSensitive));
    ask.set("wholeWord", Json(o.wholeWord));
    ask.set("regex", Json(o.regex));
    ask.set("include", Json(o.include));
    ask.set("exclude", Json(o.exclude));
    ask.set("hidden", Json(true));   // dotfiles searched, as the Mac's own search does
    NSString *root = _root;
    NSMutableArray<SearchHit *> *found = [NSMutableArray array];
    __weak SearchPanel *weakSelf = self;
    _hostReq = self.hostSearch(ask, ^(const std::vector<Json> &answers, BOOL done) {
        SearchPanel *s = weakSelf;
        if (!s || s->_searchGeneration != gen) return;   // superseded
        const Json *last = nullptr;
        for (const Json &a : answers) {
            for (const Json &row : a["rows"].elements()) {
                NSString *rel = SNS(row["path"].asString());
                if (!rel.length) continue;
                SearchHit *h = [SearchHit new];
                h.path = [root stringByAppendingPathComponent:rel];
                h.line = (NSInteger)row["line"].asInt();
                h.text = SNS(row["text"].asString());
                h.count = (NSUInteger)std::max<int64_t>(1, row["count"].asInt(1));
                NSMutableArray<NSString *> *before = [NSMutableArray array], *after = [NSMutableArray array];
                for (const Json &l : row["before"].elements()) [before addObject:SNS(l.asString())];
                for (const Json &l : row["after"].elements()) [after addObject:SNS(l.asString())];
                h.before = before;
                h.after = after;
                [found addObject:h];
            }
            if (a["done"].asBool()) last = &a;
        }
        if (!answers.empty()) [s setResults:found];
        NSMutableSet<NSString *> *files = [NSMutableSet set];
        for (SearchHit *h in found) [files addObject:h.path];
        if (!done) {
            [s setStatus:[NSString stringWithFormat:@"Searching… %@ in %@ %@", MCGrouped(found.count),
                                                    MCGrouped(files.count), files.count == 1 ? @"file" : @"files"]
                   error:NO];
            return;
        }
        s->_hostReq = 0;
        if (!last) {
            [s showFound:found.count files:files.count total:found.count truncated:NO tooSlow:NO stopped:nil
                   error:@"The connection to the other machine was lost before the search ended" note:nil
               noteError:NO];
            return;
        }
        const std::string error = (*last)["error"].asString();
        unsigned long total = (unsigned long)(*last)["totalMatches"].asInt(0);
        if (!total) for (SearchHit *h in found) total += h.count;
        [s showFound:found.count files:files.count total:total truncated:(*last)["truncated"].asBool()
             tooSlow:(*last)["tooSlow"].asBool()
             stopped:(*last)["timedOut"].asBool() ? @"The other machine stopped searching after 30 seconds" : nil
               error:error.empty() ? nil : SNS(error) note:nil noteError:NO];
    });
    if (!_hostReq) [self setStatus:@"The other machine cannot be asked: Reconnect, then search again" error:YES];
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
    [self setSkippedNote:nil tooltip:nil];
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
        NSString *skipTip = nil;
        NSString *skipped = MCSkippedNote(r, &skipTip);
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s || s->_searchGeneration != gen) return;   // superseded
            s->_cancel = nullptr;
            s->_allTodos = found;
            [s filterTodos];
            [s setSkippedNote:skipped tooltip:skipTip];
            if (truncated)
                s.status.stringValue = [s.status.stringValue
                    stringByAppendingString:[NSString stringWithFormat:@". Stopped at %@ results",
                                                                       MCGrouped(kMaxResults)]];
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

// VS Code's Dismiss: the row leaves the results (a file with its lines),
// and Replace All and Replace in This File leave what it held alone until
// the next search.
- (void)dismissItem:(id)item {
    if (_replacing) return;
    NSMutableArray<SearchHit *> *left = [_hits mutableCopy];
    if ([item isKindOfClass:[SearchFile class]]) {
        SearchFile *f = item;
        [left removeObjectsInArray:f.hits];
        _dismissed.dismissFile(SStd(f.path));
    } else if ([item isKindOfClass:[SearchHit class]]) {
        SearchHit *h = item;
        [left removeObjectIdenticalTo:h];
        if (!_todos) _dismissed.dismissLine(SStd(h.path), (int)h.line, SStd(h.text));
    } else {
        return;
    }
    [self setResults:left];
    if (!_todos) [self showDismissedStatus];
}

// The status line after a dismiss: what is left to replace.
- (void)showDismissedStatus {
    NSUInteger matches = 0;
    for (SearchFile *f in _files) matches += f.count;
    [self setStatus:[NSString stringWithFormat:@"%@ %@ in %@ %@ (dismissed rows are left alone)",
                                               MCGrouped(matches), matches == 1 ? @"match" : @"matches",
                                               MCGrouped(_files.count),
                                               _files.count == 1 ? @"file" : @"files"]
              error:NO];
}

// For tests: the dismissed rows.
- (NSUInteger)dismissedCount {
    std::size_t n = _dismissed.files.size();
    for (const auto &kv : _dismissed.lines) n += kv.second.size();
    return n;
}

// Cmd+G and Shift+Cmd+G: the next or previous line in the results, in the
// tree's order and wrapping, selected and shown in the editor's preview tab
// as a single click shows it, the keyboard left where it is.
- (void)stepResult:(BOOL)forward {
    NSMutableArray<SearchHit *> *order = [NSMutableArray array];
    for (SearchFile *f in _files) [order addObjectsFromArray:f.hits];
    if (!order.count) {
        NSBeep();
        return;
    }
    const NSInteger n = (NSInteger)order.count;
    id item = [self.outline itemAtRow:self.outline.selectedRow];
    NSInteger i;
    if ([item isKindOfClass:[SearchHit class]]) {
        const NSUInteger at = [order indexOfObjectIdenticalTo:item];
        i = at == NSNotFound ? (forward ? 0 : n - 1) : (NSInteger)at + (forward ? 1 : -1);
    } else if ([item isKindOfClass:[SearchFile class]] && ((SearchFile *)item).hits.count) {
        const NSUInteger first =
            [order indexOfObjectIdenticalTo:((SearchFile *)item).hits.firstObject];
        i = first == NSNotFound ? 0 : (NSInteger)first - (forward ? 0 : 1);
    } else {
        i = forward ? 0 : n - 1;
    }
    i = (i % n + n) % n;
    SearchHit *h = order[(NSUInteger)i];
    for (SearchFile *f in _files)
        if ([f.hits indexOfObjectIdenticalTo:h] != NSNotFound) {
            [self.outline expandItem:f];
            break;
        }
    const NSInteger row = [self.outline rowForItem:h];
    if (row < 0) return;
    [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
              byExtendingSelection:NO];
    [self.outline scrollRowToVisible:row];
    if (self.previewHandler) self.previewHandler(h.path, h.line);
}

// A click on a file's row folds or unfolds it, as in VS Code; a line opens
// on a double click, or Return.
- (void)rowClicked:(id)sender {
    (void)sender;
    id item = [self.outline itemAtRow:self.outline.clickedRow];
    if (NSApp.currentEvent.clickCount > 1) return;   // the double-click's own action
    if ([item isKindOfClass:[SearchHit class]]) {
        SearchHit *h = item;
        if (self.previewHandler) self.previewHandler(h.path, h.line);
        return;
    }
    if (![item isKindOfClass:[SearchFile class]]) return;
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
    // Over SSH the files are the other machine's, and this window does not
    // replace in them yet (docs/notes/session-host.md).
    if (self.hostSearch) {
        NSBeep();
        [self setStatus:kNoReplaceOverSsh error:YES];
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
    const FolderReplaceExclusions dismissed = _dismissed;
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        FolderReplacePlan plan = FolderSearch::planReplace(root, q, nullptr, o);
        // The rows dismissed from the results stay as they are.
        FolderSearch::excludeDismissed(plan, q, o, dismissed);
        const size_t dismissedMatches = plan.dismissed;
        NSMutableArray<NSString *> *changed = [NSMutableArray array];
        for (const std::string &rel : plan.changedSinceSearch) [changed addObject:SNS(rel)];
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        NSMutableArray<NSNumber *> *counts = [NSMutableArray array];
        NSMutableSet<NSString *> *outside = [NSMutableSet set];
        NSMutableArray<NSString *> *large = [NSMutableArray array];
        for (const std::string &rel : plan.tooLarge) [large addObject:SNS(rel)];
        if (plan.tooLargeCount > plan.tooLarge.size())
            [large addObject:[NSString stringWithFormat:@"and %@ more",
                                       MCGrouped(plan.tooLargeCount - plan.tooLarge.size())]];
        // Files read as Windows-1252: searched, but written as UTF-8 or
        // UTF-16 only, so left alone and named.
        NSMutableArray<NSString *> *legacy = [NSMutableArray array];
        for (const std::string &rel : plan.otherEncoding) [legacy addObject:SNS(rel)];
        if (plan.otherEncodingCount > plan.otherEncoding.size())
            [legacy addObject:[NSString stringWithFormat:@"and %@ more",
                                        MCGrouped(plan.otherEncodingCount - plan.otherEncoding.size())]];
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
                    : large.count
                    ? [NSString stringWithFormat:@"Nothing replaced: the matches are in files "
                                                  "larger than %@, which Replace in Folder "
                                                  "leaves alone (%@)",
                                                 MCReplaceSizeLimit(),
                                                 [large componentsJoinedByString:@", "]]
                    : legacy.count
                    ? [NSString stringWithFormat:@"Nothing replaced: the matches are in files "
                                                  "that are not UTF-8 (read as Windows-1252), "
                                                  "which Replace in Folder leaves alone (%@)",
                                                 [legacy componentsJoinedByString:@", "]]
                    : changed.count
                    ? [NSString stringWithFormat:@"Nothing replaced: files with dismissed rows "
                                                  "changed after the search (%@); search again",
                                                 [changed componentsJoinedByString:@", "]]
                    : dismissedMatches
                    ? @"Nothing replaced: every match left is in a dismissed row"
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
            if (large.count)
                [info appendFormat:@" Left alone, since they are larger than %@: %@.",
                                   MCReplaceSizeLimit(), [large componentsJoinedByString:@", "]];
            if (legacy.count)
                [info appendFormat:@" Left alone, since they are not UTF-8 (read as "
                                    "Windows-1252): %@.",
                                   [legacy componentsJoinedByString:@", "]];
            if (dismissedMatches)
                [info appendFormat:@" %zu %@ in dismissed rows %@ left alone.", dismissedMatches,
                                   dismissedMatches == 1 ? @"match" : @"matches",
                                   dismissedMatches == 1 ? @"is" : @"are"];
            if (changed.count)
                [info appendFormat:@" Left alone, since they changed after the search and "
                                    "have dismissed rows: %@.",
                                   [changed componentsJoinedByString:@", "]];
            alert.informativeText = info;
            [alert addButtonWithTitle:@"Replace"];
            [alert addButtonWithTitle:@"Cancel"];
            // Only a key pressed once the question shows answers it: keys
            // typed while the matches were counted (text meant for another
            // window, a second Return) are dropped, not taken as Replace.
            [NSApp discardEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp beforeEvent:nil];
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
    // Every match of a file, but those in rows dismissed from the results.
    const FolderReplaceExclusions dismissed = hit ? FolderReplaceExclusions() : _dismissed;
    for (NSString *path in paths) {
        const FolderSearch::FileStamp st = FolderSearch::stampOf(path.fileSystemRepresentation);
        if (st.exists && !seen.insert({st.device, st.inode}).second) continue;
        if (dismissed.excludesFile(SStd(path))) continue;
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
            std::set<int> keep;
            if (!FolderSearch::dismissedLines(SStd(text), SStd(path), dismissed, o.maxLineChars,
                                              &keep)) {
                [problems addObject:[NSString stringWithFormat:
                            @"%@ changed after the search and has dismissed rows: left alone",
                            path.lastPathComponent]];
                continue;
            }
            edits = TextFind::replaceAll(u, query, with16);
            TextFind::dropEditsOnLines(u, &edits, keep);
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
            // A UTF-16 file is replaced in its decoded text and written
            // back as UTF-16 with its byte order mark.
            FolderSearch::TextEncoding encoding = FolderSearch::TextEncoding::Utf8;
            if (!FolderSearch::readTextFile(p, FolderSearchOptions().maxFileSize, &content,
                                            &encoding)) {
                const FolderSearch::FileStamp st = FolderSearch::stampOf(p);
                if (st.exists && st.size > FolderSearchOptions().maxFileSize)
                    [failed addObject:[NSString stringWithFormat:
                                @"%@ is larger than %@, which Replace in Folder leaves alone",
                                path.lastPathComponent, MCReplaceSizeLimit()]];
                else if (MCReadsAsWindows1252(p))
                    [failed addObject:[NSString stringWithFormat:
                                @"%@ is not UTF-8 (read as Windows-1252), which Replace in "
                                 "Folder leaves alone", path.lastPathComponent]];
                else if (MCReadsAsUtf8WithStrayBytes(p))
                    [failed addObject:[NSString stringWithFormat:
                                @"%@ has bytes that are not UTF-8, which Replace in Folder "
                                 "leaves alone", path.lastPathComponent]];
                else
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
                std::set<int> keep;
                if (!FolderSearch::dismissedLines(content, SStd(path), dismissed, o.maxLineChars,
                                                  &keep)) {
                    [failed addObject:[NSString stringWithFormat:
                                @"%@ changed after the search and has dismissed rows: left alone",
                                path.lastPathComponent]];
                    continue;
                }
                out = FolderSearch::replaceInText(content, q, with8, o, &n, &keep);
            }
            if (n == 0 || out == content) continue;
            if (!FolderSearch::writeFileAtomically(p, FolderSearch::encodeText(out, encoding),
                                                   &error)) {
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
