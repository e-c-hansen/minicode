// LspPopups.mm — Objective-C++. See LspPopups.h.
#import "LspShared.h"
#import "EditorFont.h"
#import "LspPopups.h"

// ------------------------------------------------------ MCCompletionPopup
// A borderless child window with a one-column table, and a second one beside
// it for the selected item's documentation. Neither becomes key: the text
// view keeps focus and forwards arrows, Return, Tab and Esc.
@interface MCCompletionPanel : NSPanel
@end
@implementation MCCompletionPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

static MCCompletionPanel *MakePopupPanel(NSRect frame) {
    MCCompletionPanel *p = [[MCCompletionPanel alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered defer:YES];
    p.hasShadow = YES;
    p.releasedWhenClosed = NO;
    p.backgroundColor = LHex(0x252526);
    p.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    p.becomesKeyOnlyIfNeeded = YES;
    return p;
}

@implementation MCCompletionPopup {
    MCCompletionPanel *_panel, *_docPanel;
    NSTableView *_table;
    NSTextView *_docView;
    NSScrollView *_docScroll;
    NSArray<NSString *> *_details;
    __weak NSWindow *_parent;
    BOOL _docVisible;
}
// Sizes at the editor's 13 points, scaled with its font (editor.font-size
// and the zoom), as the list's text is.
static CGFloat Zoomed(CGFloat size) { return round(size * MCEditorScale()); }
static CGFloat RowHeight(void) { return Zoomed(20); }
static CGFloat PopupWidth(void) { return Zoomed(420); }
static CGFloat DocWidth(void) { return Zoomed(400); }
static CGFloat DocMaxHeight(void) { return Zoomed(320); }
static const NSInteger kMaxRows = 10;

- (void)build {
    _panel = MakePopupPanel(NSMakeRect(0, 0, PopupWidth(), 100));
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:_panel.contentView.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.wantsLayer = YES;
    scroll.layer.borderWidth = 1;
    scroll.layer.borderColor = LHex(0x454545).CGColor;

    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"item"];
    col.width = PopupWidth() - 4;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = RowHeight();
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.backgroundColor = LHex(0x252526);
    _table.style = NSTableViewStylePlain;
    _table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.action = @selector(clicked:);
    _table.refusesFirstResponder = YES;
    scroll.documentView = _table;
    [_panel.contentView addSubview:scroll];
}

- (void)buildDoc {
    _docPanel = MakePopupPanel(NSMakeRect(0, 0, DocWidth(), 100));
    _docScroll = [[NSScrollView alloc] initWithFrame:_docPanel.contentView.bounds];
    _docScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _docScroll.hasVerticalScroller = YES;
    _docScroll.autohidesScrollers = YES;
    _docScroll.drawsBackground = NO;
    _docScroll.wantsLayer = YES;
    _docScroll.layer.borderWidth = 1;
    _docScroll.layer.borderColor = LHex(0x454545).CGColor;
    _docView = [[NSTextView alloc] initWithFrame:_docScroll.bounds];
    _docView.editable = NO;
    _docView.selectable = NO;
    _docView.drawsBackground = NO;
    _docView.textContainerInset = NSMakeSize(6, 6);
    _docView.verticallyResizable = YES;
    _docView.horizontallyResizable = NO;
    _docView.autoresizingMask = NSViewWidthSizable;
    _docView.textContainer.widthTracksTextView = YES;
    _docScroll.documentView = _docView;
    [_docPanel.contentView addSubview:_docScroll];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_labels.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
    NSTextField *f = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!f) {
        f = [NSTextField labelWithString:@""];
        f.identifier = @"cell";
        f.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    if (row < 0 || (NSUInteger)row >= _labels.count) {
        f.stringValue = @"";
        return f;
    }
    NSFont *mono = MCMonoFont(MCEditorFontSize() - 1, NSFontWeightRegular);
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:[@" " stringByAppendingString:_labels[(NSUInteger)row]]
            attributes:@{NSFontAttributeName: mono,
                         NSForegroundColorAttributeName: LHex(0xD4D4D4)}];
    NSString *detail = (NSUInteger)row < _details.count ? _details[(NSUInteger)row] : @"";
    if (detail.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"   " stringByAppendingString:detail]
                attributes:@{NSFontAttributeName: mono,
                             NSForegroundColorAttributeName: LHex(0x9CA3AF)}]];
    f.attributedStringValue = s;
    return f;
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
    (void)note;
    if (_visible && self.onSelectionChanged) self.onSelectionChanged();
}

- (void)clicked:(id)sender {
    (void)sender;
    NSInteger row = _table.clickedRow;
    if (row >= 0 && self.onAccept) self.onAccept(row);
}

- (NSRect)frame { return _panel.frame; }

- (void)showLabels:(NSArray<NSString *> *)labels details:(NSArray<NSString *> *)details
               select:(NSInteger)select atScreenRect:(NSRect)caret parent:(NSWindow *)parent {
    if (!_panel) [self build];
    _labels = [labels copy];
    _details = [details copy];
    // The zoom may have changed since the list was last shown.
    _table.rowHeight = RowHeight();
    _table.tableColumns.firstObject.width = PopupWidth() - 4;
    BOOL wasVisible = _visible;
    _visible = NO;   // no selection callbacks while the rows are replaced
    [_table reloadData];
    NSInteger row = select >= 0 && select < (NSInteger)labels.count ? select : 0;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];

    CGFloat h = MIN((NSInteger)labels.count, kMaxRows) * RowHeight() + 2;
    NSRect frame = NSMakeRect(caret.origin.x - 6, caret.origin.y - h - 2, PopupWidth(), h);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    if (NSMinY(frame) < NSMinY(vis)) frame.origin.y = NSMaxY(caret) + 2;   // above
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - PopupWidth();
    [_panel setFrame:frame display:YES];
    if (!wasVisible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
    if (_docVisible) [self placeDoc];
    if (self.onSelectionChanged) self.onSelectionChanged();
}

- (void)placeDoc {
    NSRect list = _panel.frame;
    NSScreen *screen = _parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    NSRect f = _docPanel.frame;
    f.origin.x = NSMaxX(list) + 2;
    if (NSMaxX(f) > NSMaxX(vis)) f.origin.x = NSMinX(list) - f.size.width - 2;   // left of it
    f.origin.y = NSMaxY(list) - f.size.height;   // tops level
    if (NSMinY(f) < NSMinY(vis)) f.origin.y = NSMinY(vis);
    [_docPanel setFrame:f display:YES];
}

- (void)showDoc:(NSAttributedString *)doc {
    if (!doc.length || !_visible) { [self hideDoc]; return; }
    if (!_docPanel) [self buildDoc];
    [_docPanel setContentSize:NSMakeSize(DocWidth(), DocMaxHeight())];
    [_docView.textStorage setAttributedString:doc];
    NSLayoutManager *lm = _docView.layoutManager;
    [lm ensureLayoutForTextContainer:_docView.textContainer];
    CGFloat h = ceil([lm usedRectForTextContainer:_docView.textContainer].size.height) +
                2 * _docView.textContainerInset.height + 2;
    h = MIN(MAX(h, RowHeight() + 2), DocMaxHeight());
    NSRect f = _docPanel.frame;
    f.size = NSMakeSize(DocWidth(), h);
    [_docPanel setFrame:f display:NO];
    [self placeDoc];
    [_docView scrollRangeToVisible:NSMakeRange(0, 0)];
    if (!_docVisible) {
        [_parent addChildWindow:_docPanel ordered:NSWindowAbove];
        [_docPanel orderFront:nil];
        _docVisible = YES;
    }
}

- (void)hideDoc {
    if (!_docVisible) return;
    _docVisible = NO;
    [_parent removeChildWindow:_docPanel];
    [_docPanel orderOut:nil];
}

- (NSString *)docText { return _docVisible ? _docView.string : nil; }

- (void)close {
    [self hideDoc];
    if (!_visible) return;
    _visible = NO;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
    _labels = @[];
    _details = @[];
    // The table must hear the rows are gone, or a later layout pass asks
    // for one of them.
    [_table reloadData];
}

- (NSInteger)selectedRow { return _table ? _table.selectedRow : -1; }

- (void)moveSelection:(NSInteger)delta {
    NSInteger n = (NSInteger)_labels.count;
    if (n == 0) return;
    NSInteger row = ([self selectedRow] + delta + n) % n;   // wraps around
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    [_table scrollRowToVisible:row];
}
@end

// ----------------------------------------------------------- MCHoverPopup
// Hover info: what the server says about the symbol under the pointer (or
// the caret, for Cmd+I), with any diagnostic there first. Above its line, as
// in VS Code, below when there is no room. The pointer may move into it to
// scroll a long docstring; it says when the pointer leaves.
@interface MCHoverView : NSView
@property(nonatomic, copy) void (^onExit)(void);
@end
@implementation MCHoverView {
    NSTrackingArea *_area;
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_area) [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:self userInfo:nil];
    [self addTrackingArea:_area];
}
- (void)mouseExited:(NSEvent *)event {
    (void)event;
    if (self.onExit) self.onExit();
}
@end

@implementation MCHoverPopup {
    MCCompletionPanel *_panel;
    NSScrollView *_scroll;
    NSTextView *_view;
    __weak NSWindow *_parent;
}
static const CGFloat kHoverMinWidth = 60;
#define kHoverMaxWidth Zoomed(520)
#define kHoverMaxHeight Zoomed(320)

- (void)build {
    _panel = MakePopupPanel(NSMakeRect(0, 0, kHoverMaxWidth, 100));
    MCHoverView *box = [[MCHoverView alloc] initWithFrame:_panel.contentView.bounds];
    __weak MCHoverPopup *weakSelf = self;
    box.onExit = ^{
        MCHoverPopup *p = weakSelf;
        if (p && p.onExit) p.onExit();
    };
    _panel.contentView = box;
    _scroll = [[NSScrollView alloc] initWithFrame:box.bounds];
    _scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.drawsBackground = NO;
    _scroll.wantsLayer = YES;
    _scroll.layer.borderWidth = 1;
    _scroll.layer.borderColor = LHex(0x454545).CGColor;
    _view = [[NSTextView alloc] initWithFrame:_scroll.bounds];
    _view.editable = NO;
    _view.selectable = NO;
    _view.drawsBackground = NO;
    _view.textContainerInset = NSMakeSize(8, 6);
    _view.verticallyResizable = YES;
    _view.horizontallyResizable = NO;
    _view.autoresizingMask = NSViewWidthSizable;
    _view.textContainer.widthTracksTextView = YES;
    _scroll.documentView = _view;
    [box addSubview:_scroll];
}

- (NSRect)frame { return _visible ? _panel.frame : NSZeroRect; }

- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)line parent:(NSWindow *)parent {
    if (!_panel) [self build];
    // Laid out at the widest first, then narrowed to the longest line, so a
    // one-line answer gets a small box.
    [_panel setContentSize:NSMakeSize(kHoverMaxWidth, kHoverMaxHeight)];
    [_view.textStorage setAttributedString:text];
    NSLayoutManager *lm = _view.layoutManager;
    [lm ensureLayoutForTextContainer:_view.textContainer];
    NSSize used = [lm usedRectForTextContainer:_view.textContainer].size;
    NSSize inset = _view.textContainerInset;
    CGFloat pad = _view.textContainer.lineFragmentPadding;
    CGFloat w = MIN(MAX(ceil(used.width) + 2 * (inset.width + pad) + 2, kHoverMinWidth),
                    kHoverMaxWidth);
    if (w < kHoverMaxWidth) {
        [_panel setContentSize:NSMakeSize(w, kHoverMaxHeight)];
        [lm ensureLayoutForTextContainer:_view.textContainer];
        used = [lm usedRectForTextContainer:_view.textContainer].size;
    }
    CGFloat h = MIN(ceil(used.height) + 2 * inset.height + 2, kHoverMaxHeight);
    NSRect frame = NSMakeRect(line.origin.x - 8, NSMaxY(line) + 2, w, h);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    if (NSMaxY(frame) > NSMaxY(vis)) frame.origin.y = line.origin.y - h - 2;   // below
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - w;
    if (NSMinX(frame) < NSMinX(vis)) frame.origin.x = NSMinX(vis);
    [_panel setFrame:frame display:YES];
    [_view scrollRangeToVisible:NSMakeRange(0, 0)];
    if (!_visible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
}

- (void)close {
    if (!_visible) return;
    _visible = NO;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
}
@end

@implementation MCSignaturePopup {
    MCCompletionPanel *_panel;
    NSTextField *_label;
    __weak NSWindow *_parent;
}
static const CGFloat kSignatureMaxWidth = 600;

- (void)show:(NSAttributedString *)text atScreenRect:(NSRect)caret parent:(NSWindow *)parent {
    if (!_panel) {
        _panel = MakePopupPanel(NSMakeRect(0, 0, 200, 24));
        NSView *box = _panel.contentView;
        box.wantsLayer = YES;
        box.layer.borderWidth = 1;
        box.layer.borderColor = LHex(0x454545).CGColor;
        _label = [NSTextField wrappingLabelWithString:@""];
        _label.selectable = NO;
        [box addSubview:_label];
    }
    _label.attributedStringValue = text;
    NSSize fit = [_label sizeThatFits:NSMakeSize(kSignatureMaxWidth, CGFLOAT_MAX)];
    fit.width = MIN(ceil(fit.width), kSignatureMaxWidth);
    fit.height = MIN(ceil(fit.height), 200);
    NSSize size = NSMakeSize(fit.width + 16, fit.height + 10);
    _label.frame = NSMakeRect(8, 5, fit.width, fit.height);
    NSRect frame = NSMakeRect(caret.origin.x - 8, NSMaxY(caret) + 2, size.width, size.height);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    // Below the line when there is no room above it.
    if (NSMaxY(frame) > NSMaxY(vis)) frame.origin.y = caret.origin.y - size.height - 2;
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = NSMaxX(vis) - size.width;
    if (NSMinX(frame) < NSMinX(vis)) frame.origin.x = NSMinX(vis);
    [_panel setFrame:frame display:YES];
    if (!_visible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
    _text = [text.string copy];
}

- (void)close {
    if (!_visible) return;
    _visible = NO;
    _text = nil;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
}
@end
