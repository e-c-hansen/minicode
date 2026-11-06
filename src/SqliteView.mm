// SqliteView.mm — see SqliteView.h.
#import "SqliteView.h"
#import "AppSettings.h"
#import "Appearance.h"
#include <sqlite3.h>

const NSUInteger MCSqliteMaxRows = 1000;

namespace {
struct Deadline {
    CFAbsoluteTime until;
    bool hit;
};
int StopAtDeadline(void *ctx) {
    Deadline *d = (Deadline *)ctx;
    if (CFAbsoluteTimeGetCurrent() > d->until) {
        d->hit = true;
        return 1;   // interrupts the statement
    }
    return 0;
}
}  // namespace

static NSString *Quoted(NSString *name) {
    return [NSString stringWithFormat:@"\"%@\"",
            [name stringByReplacingOccurrencesOfString:@"\"" withString:@"\"\""]];
}

static NSColor *Muted(void) {
    return [NSColor colorWithSRGBRed:0x9C / 255.0 green:0xA3 / 255.0 blue:0xAF / 255.0 alpha:1];
}

@interface MCSqliteView () <NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate>
@end

@implementation MCSqliteView {
    dispatch_queue_t _queue;
    sqlite3 *_db;                 // used on _queue only (sqlite3_interrupt aside)
    NSString *_path;
    NSArray<NSString *> *_names;
    NSDictionary<NSString *, NSString *> *_sql, *_types;
    NSArray<NSString *> *_columns;
    NSArray<NSArray<NSString *> *> *_rows;
    NSTableView *_objects, *_grid;
    NSScrollView *_objectsScroll, *_gridScroll, *_schemaScroll;
    NSTextView *_schema;
    NSTextField *_query, *_status;
    NSButton *_run;
    uint64_t _generation;
    BOOL _busy;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _queue = dispatch_queue_create("minicode.sqlite", DISPATCH_QUEUE_SERIAL);
        _timeLimit = 5;
        self.wantsLayer = YES;

        _objects = [[NSTableView alloc] initWithFrame:NSZeroRect];
        NSTableColumn *oc = [[NSTableColumn alloc] initWithIdentifier:@"name"];
        oc.title = @"Tables and views";
        oc.resizingMask = NSTableColumnAutoresizingMask;
        [_objects addTableColumn:oc];
        _objects.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
        _objects.dataSource = self;
        _objects.delegate = self;
        _objects.style = NSTableViewStylePlain;
        _objects.backgroundColor = NSColor.clearColor;
        _objectsScroll = [self scrollFor:_objects];

        _grid = [[NSTableView alloc] initWithFrame:NSZeroRect];
        _grid.dataSource = self;
        _grid.delegate = self;
        _grid.style = NSTableViewStylePlain;
        _grid.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
        _grid.gridStyleMask = NSTableViewSolidVerticalGridLineMask | NSTableViewSolidHorizontalGridLineMask;
        _grid.gridColor = [NSColor colorWithWhite:0.2 alpha:1];
        _grid.allowsColumnReordering = NO;
        _grid.rowHeight = 18;
        _gridScroll = [self scrollFor:_grid];
        _gridScroll.hasHorizontalScroller = YES;

        _schema = [[NSTextView alloc] initWithFrame:NSZeroRect];
        _schema.editable = NO;
        _schema.selectable = YES;
        _schema.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        _schema.textContainerInset = NSMakeSize(6, 4);
        _schema.drawsBackground = NO;
        _schemaScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _schemaScroll.documentView = _schema;
        _schemaScroll.hasVerticalScroller = YES;
        _schemaScroll.autohidesScrollers = YES;
        _schemaScroll.drawsBackground = NO;
        _schema.autoresizingMask = NSViewWidthSizable;
        [self addSubview:_schemaScroll];

        _query = [NSTextField textFieldWithString:@""];
        _query.placeholderString = @"SQL to run on the database, read-only (Return runs it)";
        _query.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
        _query.target = self;
        _query.action = @selector(queryEntered:);
        _query.delegate = self;
        [self addSubview:_query];
        _run = [NSButton buttonWithTitle:@"Run" target:self action:@selector(queryEntered:)];
        _run.controlSize = NSControlSizeSmall;
        [self addSubview:_run];
        _status = [NSTextField labelWithString:@""];
        _status.font = [NSFont systemFontOfSize:11];
        _status.textColor = Muted();
        _status.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:_status];
        [self applySettings];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(settingsChanged:)
                                                     name:MCSettingsDidChangeNotification
                                                   object:nil];
        NSMenu *menu = MCAdjustSettingsMenu(@"editor", self);
        _objects.menu = menu;
        _grid.menu = menu;
    }
    return self;
}

- (NSScrollView *)scrollFor:(NSTableView *)table {
    NSScrollView *s = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    s.documentView = table;
    s.hasVerticalScroller = YES;
    s.autohidesScrollers = YES;
    s.borderType = NSNoBorder;
    s.drawsBackground = NO;
    [self addSubview:s];
    return s;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    sqlite3 *db = _db;
    _db = nullptr;
    if (db) {
        sqlite3_interrupt(db);
        dispatch_async(_queue, ^{ sqlite3_close_v2(db); });
    }
}

- (void)settingsChanged:(NSNotification *)n {
    [self applySettings];
    [_grid reloadData];
    [_objects reloadData];
}

- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    _grid.backgroundColor = [cfg background:Surface::Editor];
    _schema.textColor = [cfg text:Surface::Editor];
}

- (BOOL)isFlipped { return YES; }

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    const NSRect b = self.bounds;
    const CGFloat W = NSWidth(b), H = NSHeight(b);
    const CGFloat left = MIN(240, MAX(140, round(W * 0.22)));
    const CGFloat schemaH = MIN(140, MAX(60, round(H * 0.25)));
    _objectsScroll.frame = NSMakeRect(0, 0, left, MAX(0, H - schemaH));
    _schemaScroll.frame = NSMakeRect(0, H - schemaH, left, schemaH);
    _schema.frame = NSMakeRect(0, 0, left, MAX(schemaH, _schema.frame.size.height));
    const CGFloat x = left + 8, w = MAX(0, W - x - 8);
    _run.frame = NSMakeRect(W - 8 - 56, 8, 56, 24);
    _query.frame = NSMakeRect(x, 8, MAX(0, w - 64), 24);
    _status.frame = NSMakeRect(x, 38, w, 16);
    _gridScroll.frame = NSMakeRect(left + 1, 60, MAX(0, W - left - 1), MAX(0, H - 60));
}

- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithWhite:0.2 alpha:1] setFill];
    NSRectFill(NSMakeRect(_objectsScroll.frame.size.width, 0, 1, NSHeight(self.bounds)));
}

- (NSString *)path { return _path; }
- (NSArray<NSString *> *)objectNames { return _names ?: @[]; }
- (NSString *)schemaText { return _schema.string; }
- (NSArray<NSString *> *)columns { return _columns ?: @[]; }
- (NSArray<NSArray<NSString *> *> *)rows { return _rows ?: @[]; }
- (NSString *)statusText { return _status.stringValue; }
- (BOOL)busy { return _busy; }
- (NSTextField *)queryField { return _query; }
- (NSTableView *)objectsTable { return _objects; }
- (NSTableView *)rowsTable { return _grid; }

// ------------------------------------------------------------ opening
- (BOOL)showDatabase:(NSString *)path {
    [self close];   // the same file again (changed on disk) is read afresh
    __block sqlite3 *db = nullptr;
    __block NSString *problem = nil;
    __block NSArray *names = nil;
    __block NSDictionary *sql = nil, *types = nil;
    // mode=ro in the URI and the read-only flag: SQLite will not write the
    // file, and will not create it either.
    NSURLComponents *uri = [NSURLComponents componentsWithURL:[NSURL fileURLWithPath:path]
                                      resolvingAgainstBaseURL:NO];
    uri.query = @"mode=ro";
    NSString *spec = uri.string;
    dispatch_sync(_queue, ^{
        if (sqlite3_open_v2(spec.UTF8String, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) !=
            SQLITE_OK) {
            problem = db ? @(sqlite3_errmsg(db)) : @"cannot open";
            sqlite3_close_v2(db);
            db = nullptr;
            return;
        }
        sqlite3_exec(db, "PRAGMA query_only = ON", nullptr, nullptr, nullptr);
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        NSMutableArray *n = [NSMutableArray array];
        NSMutableDictionary *s = [NSMutableDictionary dictionary], *t = [NSMutableDictionary dictionary];
        sqlite3_stmt *st = nullptr;
        const char *list = "SELECT name, type, sql FROM sqlite_master "
                           "WHERE type IN ('table', 'view') ORDER BY type = 'view', name";
        if (sqlite3_prepare_v2(db, list, -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *name = (const char *)sqlite3_column_text(st, 0);
                const char *type = (const char *)sqlite3_column_text(st, 1);
                const char *text = (const char *)sqlite3_column_text(st, 2);
                if (!name) continue;
                NSString *key = @(name);
                [n addObject:key];
                t[key] = type ? @(type) : @"table";
                s[key] = text ? @(text) : @"";
            }
        } else {
            problem = @(sqlite3_errmsg(db));   // not a database after all
        }
        sqlite3_finalize(st);
        if (problem) {
            sqlite3_close_v2(db);
            db = nullptr;
            return;
        }
        names = n;
        sql = s;
        types = t;
    });
    if (!db) {
        _status.stringValue = problem ?: @"";
        return NO;
    }
    _db = db;
    _path = [path copy];
    _names = names;
    _sql = sql;
    _types = types;
    [_objects reloadData];
    _status.stringValue = names.count
        ? [NSString stringWithFormat:@"%lu %@, read-only", (unsigned long)names.count,
                                     names.count == 1 ? @"table" : @"tables and views"]
        : @"No tables, read-only";
    if (names.count) {
        [_objects selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [self showObject:names.firstObject];
    }
    return YES;
}

- (void)close {
    _generation++;
    _busy = NO;
    sqlite3 *db = _db;
    _db = nullptr;
    if (db) {
        sqlite3_interrupt(db);   // a long query gives way
        dispatch_async(_queue, ^{ sqlite3_close_v2(db); });
    }
    _path = nil;
    _names = nil;
    _columns = nil;
    _rows = nil;
    _schema.string = @"";
    _status.stringValue = @"";
    [_objects reloadData];
    [self rebuildColumns];
}

// ------------------------------------------------------------ queries
- (void)showObject:(NSString *)name {
    if (!name || !_sql[name]) return;
    _schema.string = _sql[name];
    [self run:[NSString stringWithFormat:@"SELECT * FROM %@", Quoted(name)] fromList:YES];
}

- (void)queryEntered:(id)sender {
    NSString *sql = [_query.stringValue stringByTrimmingCharactersInSet:
                         NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (sql.length) [self runQuery:sql];
}

- (void)runQuery:(NSString *)sql {
    [self run:sql fromList:NO];
}

- (void)run:(NSString *)sql fromList:(BOOL)fromList {
    sqlite3 *db = _db;
    if (!db) return;
    const uint64_t gen = ++_generation;
    _busy = YES;
    _status.stringValue = @"Running…";
    const NSTimeInterval limit = _timeLimit;
    __weak MCSqliteView *weakSelf = self;
    dispatch_async(_queue, ^{
        NSDictionary *result = [MCSqliteView execute:sql on:db limit:limit];
        dispatch_async(dispatch_get_main_queue(), ^{
            MCSqliteView *me = weakSelf;
            if (!me || gen != me->_generation || me->_db != db) return;
            [me apply:result fromList:fromList];
        });
    });
}

// One statement on the read-only connection, on the queue: its columns,
// the first MCSqliteMaxRows rows as text, and whether there were more.
+ (NSDictionary *)execute:(NSString *)sql on:(sqlite3 *)db limit:(NSTimeInterval)limit {
    const CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
    sqlite3_stmt *st = nullptr;
    const char *tail = nullptr;
    if (sqlite3_prepare_v2(db, sql.UTF8String, -1, &st, &tail) != SQLITE_OK || !st) {
        NSString *why = st ? @(sqlite3_errmsg(db)) : (sqlite3_errcode(db) == SQLITE_OK
                                                        ? @"nothing to run" : @(sqlite3_errmsg(db)));
        sqlite3_finalize(st);
        return @{@"error": why};
    }
    if (!sqlite3_stmt_readonly(st)) {
        sqlite3_finalize(st);
        return @{@"error": @"Not run: the database is open read-only, and this statement would change it."};
    }
    BOOL more = NO;
    while (tail && *tail) {
        const char c = *tail++;
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r' && c != ';') { more = YES; break; }
    }
    Deadline deadline{start + limit, false};
    sqlite3_progress_handler(db, 1000, StopAtDeadline, &deadline);
    const int n = sqlite3_column_count(st);
    NSMutableArray<NSString *> *columns = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        const char *name = sqlite3_column_name(st, i);
        [columns addObject:name ? @(name) : @""];
    }
    NSMutableArray<NSArray<NSString *> *> *rows = [NSMutableArray array];
    BOOL truncated = NO;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (rows.count == MCSqliteMaxRows) { truncated = YES; break; }
        NSMutableArray<NSString *> *row = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
        for (int i = 0; i < n; i++) {
            switch (sqlite3_column_type(st, i)) {
            case SQLITE_NULL: [row addObject:@"NULL"]; break;
            case SQLITE_BLOB:
                [row addObject:[NSString stringWithFormat:@"<%d bytes>", sqlite3_column_bytes(st, i)]];
                break;
            default: {
                const unsigned char *t = sqlite3_column_text(st, i);
                const int len = sqlite3_column_bytes(st, i);
                NSString *s = t ? [[NSString alloc] initWithBytes:t length:(NSUInteger)len
                                                         encoding:NSUTF8StringEncoding] : nil;
                [row addObject:s ?: @"?"];
            }
            }
        }
        [rows addObject:row];
    }
    NSString *error = nil;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        error = deadline.hit ? [NSString stringWithFormat:@"Stopped: the query ran longer than %g seconds.", limit]
                             : @(sqlite3_errmsg(db));
    sqlite3_progress_handler(db, 0, nullptr, nullptr);
    sqlite3_finalize(st);
    NSMutableDictionary *out = [@{@"columns": columns, @"rows": rows, @"truncated": @(truncated),
                                  @"seconds": @(CFAbsoluteTimeGetCurrent() - start),
                                  @"more": @(more)} mutableCopy];
    if (error) out[@"error"] = error;
    return out;
}

- (void)apply:(NSDictionary *)result fromList:(BOOL)fromList {
    _busy = NO;
    if (result[@"error"] && !result[@"columns"]) {
        _status.stringValue = result[@"error"];
        _status.textColor = [NSColor systemOrangeColor];
        return;
    }
    _columns = result[@"columns"];
    _rows = result[@"rows"];
    [self rebuildColumns];
    NSNumberFormatter *f = [NSNumberFormatter new];
    f.numberStyle = NSNumberFormatterDecimalStyle;
    NSString *count = [f stringFromNumber:@(_rows.count)];
    NSString *text = [result[@"truncated"] boolValue]
        ? [NSString stringWithFormat:@"The first %@ rows", count]
        : [NSString stringWithFormat:@"%@ %@", count, _rows.count == 1 ? @"row" : @"rows"];
    if (!fromList) text = [text stringByAppendingFormat:@", %.2f s", [result[@"seconds"] doubleValue]];
    if ([result[@"more"] boolValue]) text = [text stringByAppendingString:@" (only the first statement ran)"];
    if (result[@"error"]) text = result[@"error"];
    _status.stringValue = text;
    _status.textColor = result[@"error"] ? [NSColor systemOrangeColor] : Muted();
}

- (void)rebuildColumns {
    while (_grid.numberOfColumns) [_grid removeTableColumn:_grid.tableColumns.lastObject];
    NSFont *font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    const CGFloat charW = [@"0" sizeWithAttributes:@{NSFontAttributeName: font}].width;
    for (NSUInteger i = 0; i < _columns.count; i++) {
        NSTableColumn *col = [[NSTableColumn alloc]
            initWithIdentifier:[NSString stringWithFormat:@"%lu", (unsigned long)i]];
        col.title = _columns[i];
        NSTextFieldCell *cell = [[NSTextFieldCell alloc] initTextCell:@""];
        cell.font = font;
        cell.lineBreakMode = NSLineBreakByTruncatingTail;
        cell.usesSingleLineMode = YES;
        col.dataCell = cell;
        col.editable = NO;
        NSUInteger widest = MAX(_columns[i].length, (NSUInteger)3);
        for (NSUInteger r = 0; r < MIN(_rows.count, (NSUInteger)200); r++)
            widest = MAX(widest, _rows[r][i].length);
        col.width = MIN(MAX(charW * widest + 14, 48), 360);
        [_grid addTableColumn:col];
    }
    [_grid reloadData];
}

// ------------------------------------------------------------ tables
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    return tv == _objects ? (NSInteger)_names.count : (NSInteger)_rows.count;
}

- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    if (tv == _objects) {
        NSString *name = _names[(NSUInteger)row];
        return [_types[name] isEqualToString:@"view"] ? [name stringByAppendingString:@"  (view)"] : name;
    }
    NSArray<NSString *> *r = _rows[(NSUInteger)row];
    const NSUInteger i = (NSUInteger)col.identifier.integerValue;
    return i < r.count ? r[i] : @"";
}

- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)col
              row:(NSInteger)row {
    NSTextFieldCell *c = cell;
    if (tv == _objects) {
        c.textColor = [[AppSettings shared] text:Surface::Editor];
        return;
    }
    NSArray<NSString *> *r = _rows[(NSUInteger)row];
    const NSUInteger i = (NSUInteger)col.identifier.integerValue;
    const BOOL null = i < r.count && [r[i] isEqualToString:@"NULL"];
    c.textColor = null ? Muted() : [[AppSettings shared] text:Surface::Editor];
}

- (void)tableViewSelectionDidChange:(NSNotification *)n {
    if (n.object != _objects) return;
    const NSInteger row = _objects.selectedRow;
    if (row >= 0 && row < (NSInteger)_names.count) [self showObject:_names[(NSUInteger)row]];
}
@end
