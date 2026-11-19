// CsvTable.mm — see CsvTable.h.
#import "CsvTable.h"
#import "EditorFont.h"
#import "AppSettings.h"
#import "Appearance.h"
#include "Csv.h"
#include <algorithm>
#include <string>

static const NSUInteger kSampleRows = 200;   // rows measured for column widths
static NSString *const kNumberColumn = @"#";
// The files whose first row the reader said is, or is not, a header: path
// to YES or NO, in the user's defaults, the oldest dropped past kMaxChoices.
static NSString *const kHeaderChoices = @"MCCsvFirstRowIsHeader";
static NSString *const kHeaderChoiceOrder = @"MCCsvFirstRowIsHeaderOrder";
static const NSUInteger kMaxChoices = 500;

static NSNumber *StoredHeaderChoice(NSString *path) {
    if (!path.length) return nil;
    id v = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kHeaderChoices][path];
    return [v isKindOfClass:NSNumber.class] ? v : nil;
}

static void StoreHeaderChoice(NSString *path, BOOL header) {
    if (!path.length) return;
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary *choices = [([d dictionaryForKey:kHeaderChoices] ?: @{}) mutableCopy];
    NSMutableArray *order = [([d arrayForKey:kHeaderChoiceOrder] ?: @[]) mutableCopy];
    [order removeObject:path];
    [order addObject:path];
    choices[path] = @(header);
    while (order.count > kMaxChoices) {
        [choices removeObjectForKey:order.firstObject];
        [order removeObjectAtIndex:0];
    }
    [d setObject:choices forKey:kHeaderChoices];
    [d setObject:order forKey:kHeaderChoiceOrder];
}

static NSString *Str(const std::string &s) {
    NSString *n = [[NSString alloc] initWithBytes:s.data() length:s.size()
                                         encoding:NSUTF8StringEncoding];
    return n ?: [[NSString alloc] initWithBytes:s.data() length:s.size()
                                       encoding:NSISOLatin1StringEncoding];
}

static std::string Std(NSString *s) {
    const char *c = s.UTF8String;
    return c ? c : "";
}

// Spreadsheet column names: A..Z, AA..
static NSString *ColumnName(NSUInteger i) {
    NSMutableString *s = [NSMutableString string];
    NSUInteger n = i + 1;
    while (n > 0) {
        NSUInteger r = (n - 1) % 26;
        [s insertString:[NSString stringWithFormat:@"%c", (char)('A' + r)] atIndex:0];
        n = (n - 1) / 26;
    }
    return s;
}

@class MCCsvTableView;

// The table: Return edits the row's cell in the column last clicked, and
// Undo and Redo go to the tab's undo manager (a table has none of its own).
@interface MCCsvGrid : NSTableView
@property(nonatomic, weak) MCCsvTableView *owner;
@property(nonatomic, assign) NSInteger activeColumn;
@end

@interface MCCsvTableView () <NSTableViewDataSource, NSTableViewDelegate, NSMenuItemValidation>
- (NSUndoManager *)tabUndo;
- (NSMenu *)menuForRow:(NSInteger)row column:(NSInteger)column;
@end

@implementation MCCsvGrid
- (void)keyDown:(NSEvent *)event {
    NSString *c = event.charactersIgnoringModifiers;
    const NSEventModifierFlags mods = event.modifierFlags &
        (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl);
    if (mods == 0 && c.length == 1 &&
        ([c characterAtIndex:0] == NSCarriageReturnCharacter ||
         [c characterAtIndex:0] == NSEnterCharacter) &&
        self.selectedRow >= 0 && self.owner.editable && self.numberOfColumns > 1) {
        NSInteger col = MAX(1, MIN(self.activeColumn, self.numberOfColumns - 1));
        [self editColumn:col row:self.selectedRow withEvent:nil select:YES];
        return;
    }
    [super keyDown:event];
}
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger col = [self columnAtPoint:p];
    if (col > 0) self.activeColumn = col;
    [super mouseDown:event];
}
- (NSMenu *)menuForEvent:(NSEvent *)event {
    [super menuForEvent:event];   // sets clickedRow and clickedColumn
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    return [self.owner menuForRow:[self rowAtPoint:p] column:[self columnAtPoint:p]];
}
- (void)undo:(id)sender { [[self.owner tabUndo] undo]; }
- (void)redo:(id)sender { [[self.owner tabUndo] redo]; }
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(undo:)) {
        NSUndoManager *u = [self.owner tabUndo];
        item.title = u.undoMenuItemTitle ?: @"Undo";
        return u.canUndo;
    }
    if (item.action == @selector(redo:)) {
        NSUndoManager *u = [self.owner tabUndo];
        item.title = u.redoMenuItemTitle ?: @"Redo";
        return u.canRedo;
    }
    // NSTableView validates through validateUserInterfaceItem: and has no
    // validateMenuItem: of its own: asking super for that one threw, and
    // AppKit dropped the key, so no menu key worked while the grid had the
    // keyboard (round 9: Shift+Cmd+P did nothing there).
    return [super validateUserInterfaceItem:item];
}
@end

@implementation MCCsvTableView {
    NSScrollView *_scroll;
    MCCsvGrid *_table;
    NSTextField *_info;
    Csv::Document _doc;
    NSString *_path;
    NSString *_lastText;       // the text shown, to know an echo of our own edit
    BOOL _editable;
    BOOL _header;              // the first row shows as the columns' names
    unsigned long long _fileBytes;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        _info = [NSTextField labelWithString:@""];
        _info.font = [NSFont systemFontOfSize:11];
        _info.textColor = [NSColor colorWithSRGBRed:0x9C / 255.0 green:0xA3 / 255.0
                                               blue:0xAF / 255.0 alpha:1];
        _info.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:_info];

        _table = [[MCCsvGrid alloc] initWithFrame:NSZeroRect];
        _table.owner = self;
        _table.activeColumn = 1;
        _table.dataSource = self;
        _table.delegate = self;
        _table.allowsColumnReordering = NO;
        _table.allowsMultipleSelection = NO;
        _table.usesAlternatingRowBackgroundColors = NO;
        _table.gridStyleMask = NSTableViewSolidVerticalGridLineMask |
                               NSTableViewSolidHorizontalGridLineMask;
        _table.gridColor = [NSColor colorWithWhite:0.2 alpha:1];
        _table.rowHeight = 18;
        _table.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
        _table.style = NSTableViewStylePlain;
        _table.intercellSpacing = NSMakeSize(6, 2);

        _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _scroll.documentView = _table;
        _scroll.hasVerticalScroller = YES;
        _scroll.hasHorizontalScroller = YES;
        _scroll.autohidesScrollers = YES;
        _scroll.borderType = NSNoBorder;
        [self addSubview:_scroll];
        [self applySettings];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(settingsChanged:)
                                                     name:MCSettingsDidChangeNotification
                                                   object:nil];
    }
    return self;
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

- (void)settingsChanged:(NSNotification *)n { [self applySettings]; [_table reloadData]; }

- (void)applySettings {
    NSColor *bg = [[AppSettings shared] background:Surface::Editor];
    self.layer.backgroundColor = bg.CGColor;
    _table.backgroundColor = bg;
    _scroll.backgroundColor = bg;
}

- (BOOL)isFlipped { return YES; }

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    const NSRect b = self.bounds;
    const CGFloat infoH = 22;
    _info.frame = NSMakeRect(10, 3, MAX(0, NSWidth(b) - 20), infoH - 6);
    _scroll.frame = NSMakeRect(0, infoH, NSWidth(b), MAX(0, NSHeight(b) - infoH));
}

- (NSTableView *)table { return _table; }
- (BOOL)editable { return _editable; }
- (BOOL)firstRowIsHeader { return _header; }

- (void)setFirstRowIsHeader:(BOOL)on {
    StoreHeaderChoice(_path, on);
    if (on == _header) return;
    [self commitEditing];
    // The same record stays selected and the rows where they were.
    const NSInteger selected = _table.selectedRow >= 0 ? _table.selectedRow + [self headerRows] : -1;
    _header = on;
    [self rebuild];
    const NSInteger row = selected - (NSInteger)[self headerRows];
    if (row >= 0 && row < _table.numberOfRows)
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
            byExtendingSelection:NO];
}

// The rows above the grid's first: 1 when the first row is the header.
- (NSUInteger)headerRows { return _header && _doc.rows() > 0 ? 1 : 0; }

// What a newly shown file's first row is: as the reader said for it, else
// the core's guess.
- (void)chooseHeaderFor:(NSString *)path {
    NSNumber *stored = StoredHeaderChoice(path);
    _header = stored ? stored.boolValue : Csv::looksLikeHeader(_doc);
}
- (NSUInteger)rowCount { return _doc.rows(); }
- (NSUInteger)columnCount { return _doc.columns(); }
- (BOOL)truncated { return _doc.truncated(); }
- (NSString *)infoText { return _info.stringValue; }
- (NSUndoManager *)tabUndo { return self.undoManagerProvider ? self.undoManagerProvider() : nil; }

- (NSString *)cellAtRow:(NSUInteger)row column:(NSUInteger)column {
    return Str(_doc.cell(row, column));
}

// ------------------------------------------------------------ showing
- (void)showText:(NSString *)text path:(NSString *)path tsv:(BOOL)tsv {
    const BOOL same = [path isEqualToString:_path] && _editable;
    if (same && text == _lastText) return;   // our own edit coming back
    // NSTableView would commit an open cell editor during reloadData into
    // the new document, at the old row and column: another file's cell, or
    // whatever record a reload from disk put there.
    [self dropEditing];
    const NSPoint scroll = _scroll.contentView.bounds.origin;
    const NSInteger selected = _table.selectedRow;
    const std::string bytes = Std(text ?: @"");
    const char delim = tsv ? '\t' : Csv::detectDelimiter(bytes);
    _doc = Csv::Document::parse(bytes, delim);
    if (!same) [self chooseHeaderFor:path];
    _path = [path copy];
    _lastText = text;
    _editable = YES;
    _fileBytes = bytes.size();
    [self rebuild];
    if (same) {
        if (selected >= 0 && selected < _table.numberOfRows)
            [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)selected]
                byExtendingSelection:NO];
        [_scroll.contentView scrollToPoint:scroll];
        [_scroll reflectScrolledClipView:_scroll.contentView];
    } else {
        [_table scrollRowToVisible:0];
    }
}

- (BOOL)showLargeFile:(NSString *)path tsv:(BOOL)tsv maxRows:(NSUInteger)maxRows {
    [self dropEditing];
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!fh) return NO;
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    _fileBytes = attrs.fileSize;
    // Enough for the rows asked for in all but files of very long rows.
    const NSUInteger want = 96u << 20;
    NSData *data = [fh readDataUpToLength:want error:nil];
    [fh closeFile];
    if (!data) return NO;
    const std::string bytes((const char *)data.bytes, data.length);
    const char delim = tsv ? '\t' : Csv::detectDelimiter(bytes);
    _doc = Csv::Document::parse(bytes, delim, maxRows, data.length >= _fileBytes);
    [self chooseHeaderFor:path];
    _path = [path copy];
    _lastText = nil;
    _editable = NO;
    [self rebuild];
    [_table scrollRowToVisible:0];
    return YES;
}

- (void)clear {
    [self dropEditing];   // before the document goes, never after
    _doc = Csv::Document();
    _path = nil;
    _lastText = nil;
    [self rebuild];
}

- (void)dropEditing {
    if (_table.editedRow >= 0 || _table.currentEditor) [_table abortEditing];
}

- (void)commitEditing {
    if (_table.editedRow < 0 && !_table.currentEditor) return;
    // Ending the field editor's turn as first responder commits it through
    // tableView:setObjectValue:..., into this document.
    NSWindow *w = _table.window;
    if (!w || ![w makeFirstResponder:_table]) [_table abortEditing];
}

- (void)updateInfo {
    NSNumberFormatter *f = [NSNumberFormatter new];
    f.numberStyle = NSNumberFormatterDecimalStyle;
    NSString *rows = [f stringFromNumber:@(_doc.rows())];
    NSString *cols = [f stringFromNumber:@(_doc.columns())];
    const char d = _doc.delimiter();
    NSString *by = d == '\t' ? @"tab" : d == ';' ? @"semicolon" : d == '|' ? @"pipe" : @"comma";
    if (!_editable) {
        NSString *size = [NSByteCountFormatter stringFromByteCount:(long long)_fileBytes
                                                         countStyle:NSByteCountFormatterCountStyleFile];
        _info.stringValue = _doc.truncated()
            ? [NSString stringWithFormat:@"Read-only, too large to edit here: the first %@ rows of %@, %@ columns, %@ separated",
                                         rows, size, cols, by]
            : [NSString stringWithFormat:@"Read-only, too large to edit here: %@ rows of %@, %@ columns, %@ separated",
                                         rows, size, cols, by];
        return;
    }
    NSString *under = @"";
    if ([self headerRows]) {
        rows = [f stringFromNumber:@(_doc.rows() - 1)];
        under = @" under a header";
    }
    _info.stringValue = [NSString stringWithFormat:
        @"%@ %@%@, %@ %@, %@ separated    ⇧⌘V shows the text",
        rows, [rows isEqualToString:@"1"] ? @"row" : @"rows", under, cols,
        _doc.columns() == 1 ? @"column" : @"columns", by];
}

// Columns to match the document: a row number column, then one per field.
- (void)rebuild {
    const NSUInteger want = _doc.columns() + 1;
    NSFont *font = MCMonoFont(12, NSFontWeightRegular);
    const CGFloat charW = [@"0" sizeWithAttributes:@{NSFontAttributeName: font}].width;
    while ((NSUInteger)_table.numberOfColumns > want)
        [_table removeTableColumn:_table.tableColumns.lastObject];
    for (NSUInteger i = (NSUInteger)_table.numberOfColumns; i < want; i++) {
        NSTableColumn *col = [[NSTableColumn alloc]
            initWithIdentifier:i == 0 ? kNumberColumn : [NSString stringWithFormat:@"%lu", (unsigned long)(i - 1)]];
        NSTextFieldCell *cell = [[NSTextFieldCell alloc] initTextCell:@""];
        cell.font = font;
        cell.lineBreakMode = NSLineBreakByTruncatingTail;
        cell.usesSingleLineMode = YES;
        cell.editable = YES;
        col.dataCell = cell;
        [_table addTableColumn:col];
    }
    for (NSUInteger i = 0; i < want; i++) {
        NSTableColumn *col = _table.tableColumns[i];
        if (i == 0) {
            col.title = @"";
            col.headerToolTip = nil;
            col.editable = NO;
            NSString *most = [NSString stringWithFormat:@"%lu", (unsigned long)_doc.rows()];
            col.width = MAX(28, charW * most.length + 12);
            ((NSTextFieldCell *)col.dataCell).alignment = NSTextAlignmentRight;
            continue;
        }
        // The header's field names the column, bold; A, B, C without one.
        NSString *name = [self headerRows] ? Str(_doc.cell(0, i - 1)) : @"";
        name = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        col.title = name.length ? name : ColumnName(i - 1);
        col.headerCell.font = name.length ? [NSFont systemFontOfSize:NSFont.smallSystemFontSize
                                                               weight:NSFontWeightBold]
                                          : [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        col.headerToolTip = name.length ? [NSString stringWithFormat:@"Column %@", ColumnName(i - 1)]
                                        : nil;
        col.editable = _editable;
        // As wide as the widest of the first rows, within reason.
        size_t widest = 3;
        for (size_t r = 0; r < MIN((size_t)kSampleRows, _doc.rows()); r++) {
            std::string v = _doc.cell(r, i - 1);
            size_t chars = 0;
            for (unsigned char ch : v) chars += (ch & 0xC0) != 0x80;
            widest = std::max(widest, chars);
        }
        col.width = MIN(MAX(charW * widest + 14, 48), 360);
        col.minWidth = 24;
    }
    [self updateInfo];
    [_table reloadData];
}

// ------------------------------------------------------------ data source
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    return (NSInteger)(_doc.rows() - [self headerRows]);
}

// The grid's rows are the file's under the header; the number column
// gives the file's row, so a record keeps its number either way.
- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    const size_t r = (size_t)row + [self headerRows];
    if ([col.identifier isEqualToString:kNumberColumn]) return @(r + 1).stringValue;
    return Str(_doc.cell(r, (size_t)col.identifier.integerValue));
}

- (void)tableView:(NSTableView *)tv setObjectValue:(id)value forTableColumn:(NSTableColumn *)col
              row:(NSInteger)row {
    if ([col.identifier isEqualToString:kNumberColumn] || row < 0) return;
    NSString *s = [value isKindOfClass:NSString.class] ? value : [value description];
    [self setCell:s ?: @"" row:(NSUInteger)row + [self headerRows]
           column:(NSUInteger)col.identifier.integerValue];
}

- (BOOL)tableView:(NSTableView *)tv shouldEditTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    return _editable && ![col.identifier isEqualToString:kNumberColumn];
}

- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)col
              row:(NSInteger)row {
    NSTextFieldCell *c = cell;
    AppSettings *cfg = [AppSettings shared];
    const BOOL number = [col.identifier isEqualToString:kNumberColumn];
    c.textColor = number ? [NSColor colorWithWhite:0.55 alpha:1] : [cfg text:Surface::Editor];
    // A first row not shown as the header is still bold, as a spreadsheet's.
    c.font = row == 0 && !number && ![self headerRows]
        ? MCMonoFont(12, NSFontWeightSemibold)
        : MCMonoFont(12, NSFontWeightRegular);
}

// ------------------------------------------------------------ edits
- (BOOL)apply:(BOOL (^)(Csv::Document &d))edit name:(NSString *)name {
    if (!_editable) { NSBeep(); return NO; }
    if (!edit(_doc)) return NO;
    const std::string out = _doc.serialize();
    NSString *text = Str(out);
    _lastText = text;
    [self rebuild];
    if (self.onEdit) self.onEdit(text, name);
    return YES;
}

- (BOOL)setCell:(NSString *)value row:(NSUInteger)row column:(NSUInteger)column {
    const std::string v = Std(value);
    return [self apply:^BOOL(Csv::Document &d) { return d.setCell(row, column, v); }
                  name:@"Edit Cell"];
}
- (BOOL)insertRowAt:(NSUInteger)row {
    return [self apply:^BOOL(Csv::Document &d) { return d.insertRow(row); } name:@"Insert Row"];
}
- (BOOL)deleteRowAt:(NSUInteger)row {
    return [self apply:^BOOL(Csv::Document &d) { return d.deleteRow(row); } name:@"Delete Row"];
}
- (BOOL)insertColumnAt:(NSUInteger)column {
    return [self apply:^BOOL(Csv::Document &d) { return d.insertColumn(column); }
                  name:@"Insert Column"];
}
- (BOOL)deleteColumnAt:(NSUInteger)column {
    return [self apply:^BOOL(Csv::Document &d) { return d.deleteColumn(column); }
                  name:@"Delete Column"];
}

// ------------------------------------------------------------ the menu
- (NSMenu *)menuForRow:(NSInteger)row column:(NSInteger)column {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Table"];
    // The items carry the file's rows.
    const NSInteger shown = row >= 0 ? row : _table.selectedRow;
    const NSInteger r = shown >= 0 ? shown + (NSInteger)[self headerRows] : -1;
    const NSInteger c = column > 0 ? column - 1 : _table.activeColumn - 1;
    if (_editable) {
        if (r >= 0) {
            [self add:@"Edit Cell" to:menu action:@selector(menuEditCell:) row:r column:c];
            [menu addItem:[NSMenuItem separatorItem]];
        }
        [self add:@"Insert Row Above" to:menu action:@selector(menuInsertRowAbove:) row:r column:c];
        [self add:@"Insert Row Below" to:menu action:@selector(menuInsertRowBelow:) row:r column:c];
        if (r >= 0) [self add:@"Delete Row" to:menu action:@selector(menuDeleteRow:) row:r column:c];
        [menu addItem:[NSMenuItem separatorItem]];
        [self add:@"Insert Column Left" to:menu action:@selector(menuInsertColumnLeft:) row:r column:c];
        [self add:@"Insert Column Right" to:menu action:@selector(menuInsertColumnRight:) row:r column:c];
        if (c >= 0 && (NSUInteger)c < _doc.columns())
            [self add:@"Delete Column" to:menu action:@selector(menuDeleteColumn:) row:r column:c];
    }
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *header = [[NSMenuItem alloc] initWithTitle:@"First Row Is Header"
                                                    action:@selector(menuToggleHeader:)
                                             keyEquivalent:@""];
    header.target = self;
    header.state = _header ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:header];
    MCAddAdjustSettingsItem(menu, @"editor", self);
    return menu;
}

- (void)add:(NSString *)title to:(NSMenu *)menu action:(SEL)action row:(NSInteger)row
     column:(NSInteger)column {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    item.representedObject = @[@(row), @(column)];
    [menu addItem:item];
}

static NSInteger RowOf(NSMenuItem *i) { return [[i.representedObject firstObject] integerValue]; }
static NSInteger ColumnOf(NSMenuItem *i) { return [[i.representedObject lastObject] integerValue]; }

- (void)menuEditCell:(NSMenuItem *)i {
    const NSInteger c = MAX(0, ColumnOf(i)) + 1;
    const NSInteger row = RowOf(i) - (NSInteger)[self headerRows];
    if (c < _table.numberOfColumns && row >= 0 && row < _table.numberOfRows)
        [_table editColumn:c row:row withEvent:nil select:YES];
}
- (void)menuToggleHeader:(NSMenuItem *)i { self.firstRowIsHeader = !_header; }
- (void)menuInsertRowAbove:(NSMenuItem *)i { [self insertRowAt:(NSUInteger)MAX(0, RowOf(i))]; }
- (void)menuInsertRowBelow:(NSMenuItem *)i {
    [self insertRowAt:RowOf(i) >= 0 ? (NSUInteger)RowOf(i) + 1 : _doc.rows()];
}
- (void)menuDeleteRow:(NSMenuItem *)i { [self deleteRowAt:(NSUInteger)RowOf(i)]; }
- (void)menuInsertColumnLeft:(NSMenuItem *)i { [self insertColumnAt:(NSUInteger)MAX(0, ColumnOf(i))]; }
- (void)menuInsertColumnRight:(NSMenuItem *)i {
    [self insertColumnAt:ColumnOf(i) >= 0 ? (NSUInteger)ColumnOf(i) + 1 : _doc.columns()];
}
- (void)menuDeleteColumn:(NSMenuItem *)i { [self deleteColumnAt:(NSUInteger)ColumnOf(i)]; }

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    return _editable || item.action == nil || item.action == @selector(menuToggleHeader:);
}
@end
