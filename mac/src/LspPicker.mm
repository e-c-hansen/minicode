// LspPicker.mm — Objective-C++. See LspPicker.h.
#import "LspPicker.h"
#import "EditorFont.h"

static NSColor *PHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

// ----------------------------------------------------------- MCPickerPopup
@interface MCPickerPanel : NSPanel
@end
@implementation MCPickerPanel
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

// The selected row in VS Code's list selection blue, drawn the same whether
// or not its window has the keyboard (it never does: the editor keeps it).
@interface MCPickerRowView : NSTableRowView
@end
@implementation MCPickerRowView
- (BOOL)isEmphasized { return YES; }
- (void)drawSelectionInRect:(NSRect)dirty {
    (void)dirty;
    [PHex(0x04395E) setFill];
    NSRectFill(self.bounds);
}
@end

@interface MCPickerPopup () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation MCPickerPopup {
    MCPickerPanel *_panel;
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
    _panel.backgroundColor = PHex(0x252526);
    _panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _panel.becomesKeyOnlyIfNeeded = YES;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:_panel.contentView.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.drawsBackground = NO;
    scroll.wantsLayer = YES;
    scroll.layer.borderWidth = 1;
    scroll.layer.borderColor = PHex(0x454545).CGColor;

    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"row"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];
    _table.headerView = nil;
    _table.rowHeight = kPickerRowHeight;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.backgroundColor = PHex(0x252526);
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
    MCPickerRowView *v = [tv makeViewWithIdentifier:@"pickerRow" owner:self];
    if (!v) {
        v = [MCPickerRowView new];
        v.identifier = @"pickerRow";
    }
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
// A small panel of its own under the name, which takes the keyboard while it
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
    const CGFloat w = 300, h = 56;
    _panel = [[MCRenamePanel alloc]
        initWithContentRect:NSMakeRect(0, 0, w, h)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered defer:YES];
    _panel.hasShadow = YES;
    _panel.releasedWhenClosed = NO;
    _panel.backgroundColor = PHex(0x252526);
    _panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _content = _panel.contentView;
    _content.wantsLayer = YES;
    _content.layer.borderWidth = 1;
    _content.layer.borderColor = PHex(0x4EA1F7).CGColor;
    _field = [[NSTextField alloc] initWithFrame:NSMakeRect(8, 25, w - 16, 23)];
    _field.font = MCMonoFont(12.5, NSFontWeightRegular);
    _field.delegate = self;
    _field.usesSingleLineMode = YES;
    _field.cell.scrollable = YES;
    _field.cell.wraps = NO;
    [_content addSubview:_field];
    NSTextField *hint = [NSTextField labelWithString:@"Return to rename, Esc to cancel"];
    hint.font = [NSFont systemFontOfSize:11];
    hint.textColor = PHex(0x9CA3AF);
    hint.frame = NSMakeRect(9, 5, w - 18, 15);
    [_content addSubview:hint];
    // A click anywhere else takes the keyboard away, which cancels.
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(resignedKey:)
                                                 name:NSWindowDidResignKeyNotification
                                               object:_panel];
}

- (void)showWithName:(NSString *)name relativeToRect:(NSRect)rect ofView:(NSView *)view {
    if (!_panel) [self build];
    [self close];
    NSWindow *parent = view.window;
    if (!parent) return;
    _field.stringValue = name ?: @"";
    // Under the name's line, lined up with the name.
    NSRect screen = [parent convertRectToScreen:[view convertRect:rect toView:nil]];
    NSRect f = _panel.frame;
    f.origin = NSMakePoint(NSMinX(screen) - 9, NSMinY(screen) - NSHeight(f) - 2);
    NSRect vis = (parent.screen ?: [NSScreen mainScreen]).visibleFrame;
    if (NSMinY(f) < NSMinY(vis)) f.origin.y = NSMaxY(screen) + 2;   // above it
    if (NSMaxX(f) > NSMaxX(vis)) f.origin.x = NSMaxX(vis) - NSWidth(f);
    [_panel setFrame:f display:YES];
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
