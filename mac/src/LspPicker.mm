// LspPicker.mm — Objective-C++. See LspPicker.h.
#import "LspPicker.h"
#import "EditorFont.h"
#import "FindWidget.h"

// ----------------------------------------------------------- MCPickerPopup
@interface MCPickerPanel : NSPanel
@end
@implementation MCPickerPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@implementation MCPopupRowView
- (BOOL)isEmphasized { return NO; }
// The row's text keeps the colors its attributed string gives it.
- (NSBackgroundStyle)interiorBackgroundStyle { return NSBackgroundStyleNormal; }
- (void)drawSelectionInRect:(NSRect)dirty {
    (void)dirty;
    [(self.selectionColor ?: NSColor.selectedContentBackgroundColor) setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
}
@end

@interface MCPickerPopup () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation MCPickerPopup {
    MCPickerPanel *_panel;
    NSScrollView *_scroll;
    NSColor *_selection;   // the theme's list selection, as of the last showing
    NSTableView *_table;
    NSIndexSet *_headers;
    NSInteger _maxRows;
    __weak NSWindow *_parent;
}
static const CGFloat kPickerRowHeight = 20;

- (void)build {
    _panel = [[MCPickerPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 400, 100)
                  styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                    backing:NSBackingStoreBuffered defer:YES];
    _panel.hasShadow = YES;
    _panel.releasedWhenClosed = NO;
    _panel.becomesKeyOnlyIfNeeded = YES;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:_panel.contentView.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.wantsLayer = YES;
    scroll.layer.borderWidth = 1;
    _scroll = scroll;

    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"row"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = kPickerRowHeight;
    _table.intercellSpacing = NSMakeSize(0, 0);
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

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_rows.count;
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
    f.attributedStringValue = row >= 0 && (NSUInteger)row < _rows.count
                                  ? _rows[(NSUInteger)row]
                                  : [NSAttributedString new];
    return f;
}

- (NSTableRowView *)tableView:(NSTableView *)tv rowViewForRow:(NSInteger)row {
    (void)row;
    MCPopupRowView *v = [tv makeViewWithIdentifier:@"pickerRow" owner:self];
    if (!v) {
        v = [MCPopupRowView new];
        v.identifier = @"pickerRow";
    }
    v.selectionColor = _selection;
    return v;
}

- (BOOL)tableView:(NSTableView *)tv shouldSelectRow:(NSInteger)row {
    (void)tv;
    return ![_headers containsIndex:(NSUInteger)row];
}

- (void)clicked:(id)sender {
    (void)sender;
    NSInteger row = _table.clickedRow;
    if (row >= 0 && ![_headers containsIndex:(NSUInteger)row] && self.onAccept)
        self.onAccept(row);
}

- (NSRect)frame { return _visible ? _panel.frame : NSZeroRect; }
- (NSView *)contentView { return _panel.contentView; }
- (NSInteger)selectedRow { return _visible ? _table.selectedRow : -1; }

- (NSInteger)selectableFrom:(NSInteger)row step:(NSInteger)step {
    NSInteger n = (NSInteger)_rows.count;
    while (row >= 0 && row < n && [_headers containsIndex:(NSUInteger)row]) row += step;
    return row >= 0 && row < n ? row : -1;
}

- (void)selectRow:(NSInteger)row {
    if (row < 0) return;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
        byExtendingSelection:NO];
    // A heading just above the first row of its file stays in view.
    NSInteger above = row > 0 && [_headers containsIndex:(NSUInteger)row - 1] ? row - 1 : row;
    [_table scrollRowToVisible:above];
    [_table scrollRowToVisible:row];
}

- (void)showRows:(NSArray<NSAttributedString *> *)rows
         headers:(NSIndexSet *)headers
          select:(NSInteger)select
           width:(CGFloat)width
         maxRows:(NSInteger)maxRows
    atScreenRect:(NSRect)caret
          parent:(NSWindow *)parent {
    if (!_panel) [self build];
    // The theme's widget colors, which may have changed since the last time.
    MCFindStyle *style = [MCFindStyle current];
    _panel.backgroundColor = style.widgetBackground;
    _panel.appearance = style.appearance;
    _scroll.layer.borderColor = style.widgetEdge.CGColor;
    _table.backgroundColor = style.widgetBackground;
    _selection = style.selection;
    _rows = [rows copy];
    _headers = [headers copy] ?: [NSIndexSet indexSet];
    _maxRows = MAX(maxRows, 1);
    [_table reloadData];
    NSInteger row = [self selectableFrom:MAX(select, 0) step:1];
    if (row < 0) row = [self selectableFrom:(NSInteger)rows.count - 1 step:-1];

    CGFloat h = MIN((NSInteger)rows.count, _maxRows) * kPickerRowHeight + 2;
    NSRect frame = NSMakeRect(caret.origin.x - 6, caret.origin.y - h - 2, width, h);
    NSScreen *screen = parent.screen ?: [NSScreen mainScreen];
    NSRect vis = screen.visibleFrame;
    if (NSMinY(frame) < NSMinY(vis)) frame.origin.y = NSMaxY(caret) + 2;   // above
    if (NSMaxX(frame) > NSMaxX(vis)) frame.origin.x = MAX(NSMinX(vis), NSMaxX(vis) - width);
    [_panel setFrame:frame display:YES];
    _table.tableColumns.firstObject.width = width - 4;
    if (!_visible || _parent != parent) {
        [_parent removeChildWindow:_panel];
        [parent addChildWindow:_panel ordered:NSWindowAbove];
        _parent = parent;
    }
    [_panel orderFront:nil];
    _visible = YES;
    [self selectRow:row];
}

- (void)moveSelection:(NSInteger)delta {
    NSInteger n = (NSInteger)_rows.count;
    if (!_visible || n == 0 || delta == 0) return;
    NSInteger step = delta > 0 ? 1 : -1;
    NSInteger row = _table.selectedRow;
    BOOL page = labs(delta) > 1;
    for (NSInteger moved = 0; moved < labs(delta); moved++) {
        NSInteger next = [self selectableFrom:row + step step:step];
        if (next < 0) {
            if (page) break;   // a page stops at the end
            next = [self selectableFrom:step > 0 ? 0 : n - 1 step:step];   // wraps around
        }
        if (next < 0) break;
        row = next;
    }
    [self selectRow:row];
}

- (void)close {
    if (!_visible) return;
    _visible = NO;
    [_parent removeChildWindow:_panel];
    [_panel orderOut:nil];
    _rows = @[];
    _headers = [NSIndexSet indexSet];
    // The table must hear the rows are gone, or a later layout pass asks for
    // one of them (see MCCompletionPopup close).
    [_table reloadData];
}
@end

// ------------------------------------------------------------ MCRenameBox
// A small panel of its own over the name, which takes the keyboard while it
// shows. A popover would do on screen, but this one also works in a window
// that is never shown (the offscreen tests), and its place is exact.
@interface MCRenamePanel : NSPanel
@end
@implementation MCRenamePanel
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@implementation MCRenameBox {
    MCRenamePanel *_panel;
    NSView *_content;
    __weak NSWindow *_parent;
}

- (NSView *)contentView { return _content; }

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)build {
    _panel = [[MCRenamePanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 200, 20)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered defer:YES];
    _panel.hasShadow = YES;
    _panel.releasedWhenClosed = NO;
    _content = _panel.contentView;
    _content.wantsLayer = YES;
    _content.layer.borderWidth = 1;
    _field = [[NSTextField alloc] initWithFrame:NSMakeRect(2, 1, 196, 18)];
    _field.delegate = self;
    _field.bezeled = NO;
    _field.bordered = NO;
    _field.drawsBackground = YES;
    _field.focusRingType = NSFocusRingTypeNone;
    _field.usesSingleLineMode = YES;
    _field.cell.scrollable = YES;
    _field.cell.wraps = NO;
    _field.toolTip = @"Return to rename, Esc to cancel";
    [_content addSubview:_field];
    // A click anywhere else takes the keyboard away, which cancels.
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(resignedKey:)
                                                 name:NSWindowDidResignKeyNotification
                                               object:_panel];
}

- (void)showWithName:(NSString *)name relativeToRect:(NSRect)rect ofView:(NSView *)view
                font:(NSFont *)font {
    if (!_panel) [self build];
    [self close];
    NSWindow *parent = view.window;
    if (!parent) return;
    // The theme's input colors (VS Code's input.* and focusBorder).
    MCFindStyle *style = [MCFindStyle current];
    _panel.appearance = style.appearance;
    _panel.backgroundColor = style.inputBackground;
    _content.layer.borderColor = style.focusBorder.CGColor;
    _field.backgroundColor = style.inputBackground;
    _field.textColor = style.inputForeground;
    _field.font = font ?: MCMonoFont(MCEditorFontSize(), NSFontWeightRegular);
    _field.stringValue = name ?: @"";
    // Over the name's line, its text where the name's is: the field holds
    // the line's height from just above its bottom edge to its top, and the
    // border stands 2 points above the line, so the line below stays in
    // view. As wide as the name and room to type, at least 180 points.
    NSRect line = [parent convertRectToScreen:[view convertRect:rect toView:nil]];
    const CGFloat lineH = ceil(NSHeight(line));
    const CGFloat nameW = ceil([_field.stringValue sizeWithAttributes:
                                   @{NSFontAttributeName: _field.font}].width);
    const CGFloat w = MAX(nameW + 6 + ceil(_field.font.pointSize) * 6, 180);
    NSRect f = NSMakeRect(NSMinX(line) - 4, NSMinY(line), w, lineH + 2);
    NSRect vis = (parent.screen ?: [NSScreen mainScreen]).visibleFrame;
    if (NSMaxX(f) > NSMaxX(vis)) f.origin.x = MAX(NSMinX(vis), NSMaxX(vis) - w);
    [_panel setFrame:f display:NO];
    _field.frame = NSMakeRect(2, 1, w - 4, lineH - 1);
    [parent addChildWindow:_panel ordered:NSWindowAbove];
    _parent = parent;
    _visible = YES;
    [_panel makeKeyAndOrderFront:nil];
    [_panel makeFirstResponder:_field];
    [_field.currentEditor selectAll:nil];
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv
    doCommandBySelector:(SEL)sel {
    (void)control;
    (void)tv;
    if (sel == @selector(insertNewline:)) {
        NSString *name = [_field.stringValue stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceAndNewlineCharacterSet]];
        void (^commit)(NSString *) = self.onCommit;
        [self close];
        if (name.length && commit) commit(name);
        return YES;
    }
    if (sel == @selector(cancelOperation:)) {
        [self close];
        return YES;
    }
    return NO;
}

- (void)resignedKey:(NSNotification *)note {
    (void)note;
    [self close];
}

- (void)close {
    if (!_visible) return;
    _visible = NO;
    NSWindow *parent = _parent;
    [parent removeChildWindow:_panel];
    [_panel orderOut:nil];
    [parent makeKeyWindow];
    if (self.onClose) self.onClose();
}
@end
