// ProblemsPanel.mm — Objective-C++. See ProblemsPanel.h.
#import "ProblemsPanel.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "Lsp.h"   // MCProblem

static NSColor *PHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

// The squiggles' colors (Lsp.mm), so a row and its underline agree.
static NSColor *SeverityColor(NSInteger severity) {
    switch (severity) {
    case 1: return PHex(0xF14C4C);
    case 2: return PHex(0xCCA700);
    case 3: return PHex(0x3794FF);
    default: return PHex(0x9CA3AF);
    }
}

static NSString *SeverityLetter(NSInteger severity) {
    switch (severity) {
    case 1: return @"E";
    case 2: return @"W";
    case 3: return @"I";
    default: return @"H";
    }
}

static NSString *SeverityName(NSInteger severity) {
    switch (severity) {
    case 1: return @"Error";
    case 2: return @"Warning";
    case 3: return @"Info";
    default: return @"Hint";
    }
}

static NSString *Plural(NSInteger n, NSString *one, NSString *many) {
    return [NSString stringWithFormat:@"%ld %@", (long)n, n == 1 ? one : many];
}

static NSString *HeadingCounts(MCProblem *p) {
    NSMutableArray *parts = [NSMutableArray array];
    if (p.errors) [parts addObject:Plural(p.errors, @"error", @"errors")];
    if (p.warnings) [parts addObject:Plural(p.warnings, @"warning", @"warnings")];
    if (p.others) [parts addObject:Plural(p.others, @"other", @"others")];
    return [parts componentsJoinedByString:@", "];
}

// "pyright(reportUndefinedVariable)", "clang", or "".
static NSString *SourceText(MCProblem *p) {
    if (p.source.length && p.code.length)
        return [NSString stringWithFormat:@"%@(%@)", p.source, p.code];
    return p.source.length ? p.source : p.code ?: @"";
}

// A severity mark, drawn: a red disc with a cross, a yellow triangle with
// "!", a blue disc with "i", a gray ring for a hint.
static void DrawSeverity(NSInteger severity, NSRect r) {
    NSColor *c = SeverityColor(severity);
    NSBezierPath *path;
    if (severity == 2) {
        path = [NSBezierPath bezierPath];
        [path moveToPoint:NSMakePoint(NSMidX(r), NSMaxY(r))];
        [path lineToPoint:NSMakePoint(NSMaxX(r), NSMinY(r) + 0.5)];
        [path lineToPoint:NSMakePoint(NSMinX(r), NSMinY(r) + 0.5)];
        [path closePath];
    } else {
        path = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(r, 0.5, 0.5)];
    }
    if (severity == 4) {
        [c setStroke];
        path.lineWidth = 1.2;
        [path stroke];
        return;
    }
    [c setFill];
    [path fill];
    NSColor *ink = severity == 2 ? PHex(0x1E1E1E) : [NSColor whiteColor];
    if (severity == 1) {
        NSBezierPath *x = [NSBezierPath bezierPath];
        NSRect in = NSInsetRect(r, r.size.width * 0.32, r.size.height * 0.32);
        [x moveToPoint:NSMakePoint(NSMinX(in), NSMinY(in))];
        [x lineToPoint:NSMakePoint(NSMaxX(in), NSMaxY(in))];
        [x moveToPoint:NSMakePoint(NSMinX(in), NSMaxY(in))];
        [x lineToPoint:NSMakePoint(NSMaxX(in), NSMinY(in))];
        x.lineWidth = 1.4;
        x.lineCapStyle = NSLineCapStyleRound;
        [ink setStroke];
        [x stroke];
        return;
    }
    NSString *glyph = severity == 2 ? @"!" : @"i";
    NSDictionary *a = @{NSFontAttributeName: [NSFont boldSystemFontOfSize:r.size.height * 0.7],
                        NSForegroundColorAttributeName: ink};
    NSSize sz = [glyph sizeWithAttributes:a];
    CGFloat dy = severity == 2 ? -1 : 0;
    [glyph drawAtPoint:NSMakePoint(NSMidX(r) - sz.width / 2,
                                   NSMidY(r) - sz.height / 2 + dy)
        withAttributes:a];
}

// ---------------------------------------------------------------- one row
@interface MCProblemCell : NSView
@property(nonatomic, strong) MCProblem *problem;
@property(nonatomic, copy) NSString *folder;   // a heading's, relative to the root
@property(nonatomic, strong) NSColor *text;
@end

@implementation MCProblemCell
- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    MCProblem *p = self.problem;
    if (!p) return;
    NSColor *text = self.text ?: [NSColor labelColor];
    NSColor *muted = [text colorWithAlphaComponent:0.55];
    NSFont *font = [NSFont systemFontOfSize:12];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    const CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    CGFloat y = floor((H - 15) / 2);
    if (p.heading) {
        NSMutableAttributedString *s = [NSMutableAttributedString new];
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:p.path.lastPathComponent ?: @""
                attributes:@{NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                             NSForegroundColorAttributeName: text}]];
        if (self.folder.length)
            [s appendAttributedString:[[NSAttributedString alloc]
                initWithString:[@"  " stringByAppendingString:self.folder]
                    attributes:@{NSFontAttributeName: font,
                                 NSForegroundColorAttributeName: muted}]];
        NSString *counts = HeadingCounts(p);
        NSDictionary *ca = @{NSFontAttributeName: [NSFont systemFontOfSize:11],
                             NSForegroundColorAttributeName: muted};
        CGFloat cw = ceil([counts sizeWithAttributes:ca].width);
        [counts drawAtPoint:NSMakePoint(W - 12 - cw, y + 1) withAttributes:ca];
        [s addAttribute:NSParagraphStyleAttributeName value:ps range:NSMakeRange(0, s.length)];
        [s drawWithRect:NSMakeRect(10, y, MAX(0, W - 34 - cw), 16)
                options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine];
        return;
    }
    // [mark] message   source(code)  [Ln 3, Col 5]
    DrawSeverity(p.severity, NSMakeRect(26, floor((H - 12) / 2), 12, 12));
    NSString *where = [NSString stringWithFormat:@"[Ln %ld, Col %ld]",
                       (long)p.line + 1, (long)p.column + 1];
    NSString *source = SourceText(p);
    NSDictionary *ma = @{NSFontAttributeName: font, NSForegroundColorAttributeName: muted};
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:p.message ?: @""
            attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: text}]];
    NSString *tail = source.length ? [NSString stringWithFormat:@"  %@  %@", source, where]
                                   : [@"  " stringByAppendingString:where];
    // The message gives way first: the place stays readable.
    CGFloat tw = MIN(ceil([tail sizeWithAttributes:ma].width), W * 0.45);
    [s addAttribute:NSParagraphStyleAttributeName value:ps range:NSMakeRange(0, s.length)];
    CGFloat x = 46, mw = MAX(0, W - x - 10 - tw);
    CGFloat used = MIN(ceil(s.size.width), mw);
    [s drawWithRect:NSMakeRect(x, y, mw, 16)
            options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine];
    NSMutableAttributedString *t = [[NSMutableAttributedString alloc] initWithString:tail
                                                                          attributes:ma];
    [t addAttribute:NSParagraphStyleAttributeName value:ps range:NSMakeRange(0, t.length)];
    [t drawWithRect:NSMakeRect(x + used, y, tw, 16)
            options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine];
}
@end

// ------------------------------------------------------------- the table
@interface MCProblemsTable : NSTableView
@property(nonatomic, copy) void (^onActivate)(NSInteger row);
@property(nonatomic, copy) void (^onEscape)(void);
@property(nonatomic, copy) BOOL (^selectable)(NSInteger row);
@property(nonatomic, copy) NSMenu *(^menuAt)(NSInteger row);
- (BOOL)moveBy:(NSInteger)step;
- (void)revealRow:(NSInteger)row;
@end

@implementation MCProblemsTable
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    [super mouseDown:event];
    if (row >= 0 && event.clickCount == 1 && self.onActivate &&
        (!self.selectable || self.selectable(row)))
        self.onActivate(row);
}
- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSInteger row = [self rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0 && (!self.selectable || self.selectable(row)))
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    return self.menuAt ? self.menuAt(row) : nil;
}
- (BOOL)moveBy:(NSInteger)step {
    NSInteger n = self.numberOfRows, r = self.selectedRow;
    if (r < 0) r = step > 0 ? -1 : n;
    for (r += step; r >= 0 && r < n; r += step) {
        if (self.selectable && !self.selectable(r)) continue;
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:r] byExtendingSelection:NO];
        [self revealRow:r];
        return YES;
    }
    return NO;
}
// A row, and the file heading just above it, in view.
- (void)revealRow:(NSInteger)r {
    if (r > 0 && self.selectable && !self.selectable(r - 1)) [self scrollRowToVisible:r - 1];
    [self scrollRowToVisible:r];
}
// Page Up and Down: a screenful of rows, landing on a problem.
- (void)pageBy:(NSInteger)direction {
    NSInteger per = MAX(1, (NSInteger)(self.enclosingScrollView.contentView.bounds.size.height /
                                       MAX(1, self.rowHeight + self.intercellSpacing.height)) - 1);
    for (NSInteger i = 0; i < per; i++)
        if (![self moveBy:direction]) break;
}
- (void)keyDown:(NSEvent *)event {
    NSEventModifierFlags m = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
    NSString *ch = event.charactersIgnoringModifiers;
    unichar c = ch.length ? [ch characterAtIndex:0] : 0;
    if (!m) {
        if ((c == '\r' || c == 3) && self.onActivate) {
            if (self.selectedRow >= 0) self.onActivate(self.selectedRow);
            return;
        }
        if (c == 27 && self.onEscape) { self.onEscape(); return; }
        if (c == NSUpArrowFunctionKey || c == NSDownArrowFunctionKey) {
            [self moveBy:c == NSUpArrowFunctionKey ? -1 : 1];
            return;
        }
        if (c == NSPageUpFunctionKey || c == NSPageDownFunctionKey) {
            [self pageBy:c == NSPageUpFunctionKey ? -1 : 1];
            return;
        }
        if (c == NSHomeFunctionKey || c == NSEndFunctionKey) {
            BOOL home = c == NSHomeFunctionKey;
            [self deselectAll:nil];
            [self moveBy:home ? 1 : -1];
            return;
        }
    }
    [super keyDown:event];
}
- (void)cancelOperation:(id)sender {
    (void)sender;
    if (self.onEscape) self.onEscape();
}
@end

// ------------------------------------------------------------- the panel
@interface MCProblemsPanel () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation MCProblemsPanel {
    NSView *_header;
    NSTextField *_title, *_counts, *_empty;
    NSButton *_close;
    NSScrollView *_scroll;
    MCProblemsTable *_table;
    NSArray<MCProblem *> *_rows;
    NSString *_root;
    NSColor *_text;
}

static const CGFloat kHeaderHeight = 24;

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES;
    _rows = @[];

    _header = [[NSView alloc] initWithFrame:NSZeroRect];
    _header.wantsLayer = YES;
    _header.menu = MCAdjustSettingsMenu(@"terminal", _header);
    [self addSubview:_header];
    _title = [NSTextField labelWithString:@"PROBLEMS"];
    _title.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    [_header addSubview:_title];
    _counts = [NSTextField labelWithString:@""];
    _counts.font = [NSFont systemFontOfSize:11];
    _counts.lineBreakMode = NSLineBreakByTruncatingTail;
    [_header addSubview:_counts];
    _close = [NSButton buttonWithTitle:@"×" target:self action:@selector(closeClicked:)];
    _close.bordered = NO;
    _close.toolTip = @"Close Problems (⇧⌘M)";
    [_header addSubview:_close];

    _table = [[MCProblemsTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"problem"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = 22;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.backgroundColor = [NSColor clearColor];
    _table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    _table.focusRingType = NSFocusRingTypeNone;
    _table.dataSource = self;
    _table.delegate = self;
    if (@available(macOS 11.0, *)) _table.style = NSTableViewStylePlain;
    __weak MCProblemsPanel *weakSelf = self;
    _table.selectable = ^BOOL(NSInteger row) { return [weakSelf problemAt:row] != nil; };
    _table.onActivate = ^(NSInteger row) {
        MCProblemsPanel *s = weakSelf;
        MCProblem *p = [s problemAt:row];
        if (p && s.onOpen) s.onOpen(p);
    };
    _table.onEscape = ^{
        MCProblemsPanel *s = weakSelf;
        if (s.onEscape) s.onEscape();
    };
    _table.menuAt = ^NSMenu *(NSInteger row) { return [weakSelf menuForRow:row]; };

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.documentView = _table;
    _scroll.hasVerticalScroller = YES;
    _scroll.drawsBackground = NO;
    _scroll.autohidesScrollers = YES;
    _scroll.borderType = NSNoBorder;
    // Under a full-size content view the scroll view would inset itself for
    // the title bar, hiding the first rows.
    _scroll.automaticallyAdjustsContentInsets = NO;
    _scroll.contentInsets = NSEdgeInsetsZero;
    [self addSubview:_scroll];

    _empty = [NSTextField labelWithString:@"No problems have been reported in the open files."];
    _empty.font = [NSFont systemFontOfSize:12];
    _empty.alignment = NSTextAlignmentLeft;
    [self addSubview:_empty];

    [self applySettings];
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)layout {
    [super layout];
    [self placeSubviews];
}

// Done at once on a resize, not at the next layout pass, so the list is
// its real size before anything scrolls it (a row made visible in a list
// still zero high scrolled the first heading out of sight).
- (void)placeSubviews {
    const CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    _header.frame = NSMakeRect(0, 0, W, kHeaderHeight);
    // The header is not flipped: y from its bottom.
    [_title sizeToFit];
    _title.frame = NSMakeRect(12, 4, _title.frame.size.width, 16);
    CGFloat cx = NSMaxX(_title.frame) + 12;
    _counts.frame = NSMakeRect(cx, 4, MAX(0, W - cx - 36), 16);
    _close.frame = NSMakeRect(W - 30, 1, 24, 22);
    _scroll.frame = NSMakeRect(0, kHeaderHeight, W, MAX(0, H - kHeaderHeight));
    [_table sizeLastColumnToFit];
    _empty.frame = NSMakeRect(46, kHeaderHeight + 8, MAX(0, W - 58), 18);
}

- (void)setFrameSize:(NSSize)size {
    BOOL grew = size.height > self.frame.size.height;
    [super setFrameSize:size];
    [self placeSubviews];
    if (grew && _table.selectedRow >= 0) [_table revealRow:_table.selectedRow];
}

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Terminal].CGColor;
    _header.layer.backgroundColor = [cfg background:Surface::Sidebar].CGColor;
    _text = [cfg text:Surface::Terminal];
    NSColor *headerText = [cfg text:Surface::Sidebar];
    _title.textColor = headerText;
    _counts.textColor = [headerText colorWithAlphaComponent:0.7];
    _close.attributedTitle = [[NSAttributedString alloc] initWithString:@"×"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:15],
                     NSForegroundColorAttributeName: [headerText colorWithAlphaComponent:0.8]}];
    _empty.textColor = [_text colorWithAlphaComponent:0.55];
    [_table reloadData];
}

- (void)closeClicked:(id)sender {
    (void)sender;
    if (self.onClose) self.onClose();
}

// ---- rows
- (MCProblem *)problemAt:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_rows.count) return nil;
    MCProblem *p = _rows[(NSUInteger)row];
    return p.heading ? nil : p;
}

static BOOL SameProblem(MCProblem *a, MCProblem *b) {
    return a && b && [a.path isEqualToString:b.path] && a.line == b.line &&
           a.column == b.column && a.severity == b.severity &&
           [a.message isEqualToString:b.message];
}

- (void)setRows:(NSArray<MCProblem *> *)rows counts:(NSString *)counts root:(NSString *)root {
    MCProblem *was = [self problemAt:_table.selectedRow];
    NSInteger wasRow = _table.selectedRow;
    _rows = [rows copy] ?: @[];
    _root = [root copy];
    _counts.stringValue = rows.count ? counts ?: @"" : @"";
    _empty.hidden = rows.count > 0;
    [_table reloadData];
    NSInteger select = -1;
    if (was)
        for (NSUInteger i = 0; i < _rows.count; i++)
            if (SameProblem(_rows[i], was)) { select = (NSInteger)i; break; }
    if (select < 0 && wasRow >= 0) {
        // Gone: the problem now at its place, or the one before.
        for (NSInteger r = MIN(wasRow, (NSInteger)_rows.count - 1); r >= 0 && select < 0; r--)
            if ([self problemAt:r]) select = r;
        for (NSInteger r = wasRow; r < (NSInteger)_rows.count && select < 0; r++)
            if ([self problemAt:r]) select = r;
    }
    if (select >= 0) {
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)select]
            byExtendingSelection:NO];
        [_table revealRow:select];
    } else
        [_table deselectAll:nil];
}

- (void)focusList {
    [self.window makeFirstResponder:_table];
    if (_table.selectedRow < 0) [_table moveBy:1];
    else [_table revealRow:_table.selectedRow];
}

- (BOOL)hasFocus {
    NSResponder *fr = self.window.firstResponder;
    return fr == _table || ([fr isKindOfClass:NSView.class] &&
                            [(NSView *)fr isDescendantOf:self]);
}

- (NSString *)folderOf:(NSString *)path {
    NSString *dir = path.stringByDeletingLastPathComponent;
    if (_root.length && [dir hasPrefix:[_root stringByAppendingString:@"/"]])
        return [dir substringFromIndex:_root.length + 1];
    if (_root.length && [dir isEqualToString:_root]) return @"";
    return [dir stringByAbbreviatingWithTildeInPath];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    (void)tableView;
    return (NSInteger)_rows.count;
}

- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column
                  row:(NSInteger)row {
    (void)column;
    MCProblemCell *cell = [tableView makeViewWithIdentifier:@"problem" owner:self];
    if (!cell) {
        cell = [[MCProblemCell alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"problem";
    }
    MCProblem *p = _rows[(NSUInteger)row];
    cell.problem = p;
    cell.folder = p.heading ? [self folderOf:p.path] : nil;
    cell.text = _text;
    cell.toolTip = p.heading ? p.path
        : [NSString stringWithFormat:@"%@: %@%@\n%@:%ld:%ld", SeverityName(p.severity),
           p.fullMessage ?: p.message ?: @"",
           SourceText(p).length ? [NSString stringWithFormat:@" (%@)", SourceText(p)] : @"",
           p.path.lastPathComponent, (long)p.line + 1, (long)p.column + 1];
    cell.needsDisplay = YES;
    return cell;
}

- (BOOL)tableView:(NSTableView *)tableView shouldSelectRow:(NSInteger)row {
    (void)tableView;
    return [self problemAt:row] != nil;
}

- (CGFloat)tableView:(NSTableView *)tableView heightOfRow:(NSInteger)row {
    (void)tableView;
    return row < (NSInteger)_rows.count && _rows[(NSUInteger)row].heading ? 24 : 22;
}

// ---- the right click
- (NSMenu *)menuForRow:(NSInteger)row {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    MCProblem *p = [self problemAt:row];
    if (p) {
        NSMenuItem *open = [menu addItemWithTitle:@"Go to Problem" action:@selector(openClicked:)
                                    keyEquivalent:@""];
        open.target = self;
        open.representedObject = p;
        NSMenuItem *copy = [menu addItemWithTitle:@"Copy" action:@selector(copyClicked:)
                                    keyEquivalent:@""];
        copy.target = self;
        copy.representedObject = p;
        NSMenuItem *msg = [menu addItemWithTitle:@"Copy Message"
                                          action:@selector(copyMessageClicked:)
                                   keyEquivalent:@""];
        msg.target = self;
        msg.representedObject = p;
    }
    MCAddAdjustSettingsItem(menu, @"terminal", _table);
    return menu;
}

- (void)openClicked:(NSMenuItem *)item {
    if (self.onOpen) self.onOpen(item.representedObject);
}

// As a compiler prints it: path:line:column: severity: message (source).
- (void)copyClicked:(NSMenuItem *)item {
    MCProblem *p = item.representedObject;
    NSString *s = [NSString stringWithFormat:@"%@:%ld:%ld: %@: %@%@", p.path, (long)p.line + 1,
                   (long)p.column + 1, SeverityName(p.severity).lowercaseString,
                   p.fullMessage ?: p.message ?: @"",
                   SourceText(p).length ? [NSString stringWithFormat:@" [%@]", SourceText(p)] : @""];
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:s forType:NSPasteboardTypeString];
}

- (void)copyMessageClicked:(NSMenuItem *)item {
    MCProblem *p = item.representedObject;
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:p.fullMessage ?: p.message ?: @""
                                      forType:NSPasteboardTypeString];
}

// ---- test hooks
- (NSArray<NSString *> *)rowTexts {
    NSMutableArray *out = [NSMutableArray array];
    for (MCProblem *p in _rows) {
        if (p.heading)
            [out addObject:[NSString stringWithFormat:@"# %@ — %@", p.path.lastPathComponent,
                            HeadingCounts(p)]];
        else
            [out addObject:[NSString stringWithFormat:@"%@ %ld:%ld %@ [%@]",
                            SeverityLetter(p.severity), (long)p.line + 1, (long)p.column + 1,
                            p.message, SourceText(p)]];
    }
    return out;
}
- (NSInteger)selectedRow { return _table.selectedRow; }
- (NSString *)countsText { return _counts.stringValue; }
- (NSTableView *)table { return _table; }
@end
