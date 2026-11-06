// SqliteView.mm — see SqliteView.h.
#import "SqliteView.h"
#import "EditorFont.h"
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

// sqlite3_stmt_readonly lets through what changes no bytes but still
// reaches past the viewer: BEGIN (a read transaction then holds a lock
// other writers wait on, and the rows on show freeze), SAVEPOINT, ATTACH
// and DETACH, and pragmas that set something (query_only = OFF). Those are
// refused here; pragmas that only read, with or without a table name, run.
int Authorize(void *, int action, const char *a, const char *b, const char *, const char *) {
    switch (action) {
    case SQLITE_TRANSACTION:
    case SQLITE_SAVEPOINT:
    case SQLITE_ATTACH:
    case SQLITE_DETACH:
        return SQLITE_DENY;
    case SQLITE_PRAGMA: {
        if (!b) return SQLITE_OK;   // asks for a setting
        static const char *const reads[] = {
            "table_info", "table_xinfo", "index_list", "index_info", "index_xinfo",
            "foreign_key_list", "foreign_key_check", "integrity_check", "quick_check",
            "table_list"};
        for (const char *r : reads)
            if (a && sqlite3_stricmp(a, r) == 0) return SQLITE_OK;
        return SQLITE_DENY;
    }
    default:
        return SQLITE_OK;
    }
}

// The database's header says WAL (bytes 18 and 19 are 2).
bool IsWalFile(NSString *path) {
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    NSData *head = [fh readDataUpToLength:20 error:nil];
    [fh closeFile];
    if (head.length < 20) return false;
    const unsigned char *b = (const unsigned char *)head.bytes;
    return b[18] == 2 && b[19] == 2;
}

struct Opened {
    sqlite3 *db = nullptr;
    NSString *problem = nil;
    NSArray<NSString *> *names = nil;
    NSDictionary<NSString *, NSString *> *sql = nil, *types = nil;
};

// One read-only open with `query` in the URI, and the list of tables and
// views; on the database's queue. A failure leaves db null and says why;
// *cantOpen when SQLite could not open the file at all.
Opened OpenWith(NSString *path, NSString *query, bool *cantOpen) {
    Opened o;
    *cantOpen = false;
    NSURLComponents *uri = [NSURLComponents componentsWithURL:[NSURL fileURLWithPath:path]
                                      resolvingAgainstBaseURL:NO];
    uri.query = query;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(uri.string.UTF8String, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                        nullptr) != SQLITE_OK) {
        *cantOpen = !db || sqlite3_errcode(db) == SQLITE_CANTOPEN;
        o.problem = db ? @(sqlite3_errmsg(db)) : @"cannot open";
        sqlite3_close_v2(db);
        return o;
    }
    sqlite3_exec(db, "PRAGMA query_only = ON", nullptr, nullptr, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_set_authorizer(db, Authorize, nullptr);
    NSMutableArray *n = [NSMutableArray array];
    NSMutableDictionary *s = [NSMutableDictionary dictionary], *t = [NSMutableDictionary dictionary];
    sqlite3_stmt *st = nullptr;
    const char *list = "SELECT name, type, sql FROM sqlite_master "
                       "WHERE type IN ('table', 'view') ORDER BY type = 'view', name";
    int rc = sqlite3_prepare_v2(db, list, -1, &st, nullptr);
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            const char *type = (const char *)sqlite3_column_text(st, 1);
            const char *text = (const char *)sqlite3_column_text(st, 2);
            if (!name) continue;
            NSString *key = @(name);
            [n addObject:key];
            t[key] = type ? @(type) : @"table";
            s[key] = text ? @(text) : @"";
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    if (rc != SQLITE_OK) {
        *cantOpen = (sqlite3_extended_errcode(db) & 0xFF) == SQLITE_CANTOPEN;
        o.problem = @(sqlite3_errmsg(db));   // not a database after all, or no -wal to make
    }
    sqlite3_finalize(st);
    if (o.problem) {
        sqlite3_close_v2(db);
        return o;
    }
    o.db = db;
    o.names = n;
    o.sql = s;
    o.types = t;
    return o;
}

// Opens the database read-only. A WAL database whose last writer closed
// has no -wal or -shm beside it, and a read-only connection cannot make
// them, so it would not open; with no -wal file there is nothing outside
// the main file to miss, and it is opened as immutable instead.
Opened OpenDatabase(NSString *path) {
    bool cantOpen = false;
    Opened o = OpenWith(path, @"mode=ro", &cantOpen);
    if (!o.db && cantOpen && IsWalFile(path) &&
        ![[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingString:@"-wal"]]) {
        bool again = false;
        Opened i = OpenWith(path, @"mode=ro&immutable=1", &again);
        if (i.db) return i;
    }
    return o;
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
    NSArray<NSArray *> *_rows;              // NSNull for SQL NULL
    NSTableView *_objects, *_grid;
    NSScrollView *_objectsScroll, *_gridScroll, *_schemaScroll;
    NSTextView *_schema;
    NSTextField *_query, *_status;
    NSButton *_run;
    uint64_t _generation;
    BOOL _busy, _restoring;
    NSString *_object;        // the table or view on show, nil after a query
    NSString *_ranQuery;      // the query on show, nil for a table
    // Per database, what was on show, so a reopen (the file changed on
    // disk, or the tab shown again) comes back to it.
    NSMutableDictionary<NSString *, NSDictionary *> *_remembered;
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
        _schema.font = MCMonoFont(11, NSFontWeightRegular);
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
        _query.font = MCMonoFont(12, NSFontWeightRegular);
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
- (NSArray<NSArray<NSString *> *> *)rows {
    NSMutableArray *out = [NSMutableArray arrayWithCapacity:_rows.count];
    for (NSArray *r in _rows) {
        NSMutableArray *row = [NSMutableArray arrayWithCapacity:r.count];
        for (id v in r) [row addObject:v == NSNull.null ? @"NULL" : v];
        [out addObject:row];
    }
    return out;
}
- (BOOL)isNullAtRow:(NSUInteger)row column:(NSUInteger)column {
    return row < _rows.count && column < _rows[row].count && _rows[row][column] == (id)NSNull.null;
}
- (NSString *)statusText { return _status.stringValue; }
- (BOOL)busy { return _busy; }
- (NSTextField *)queryField { return _query; }
- (NSTableView *)objectsTable { return _objects; }
- (NSTableView *)rowsTable { return _grid; }

// ------------------------------------------------------------ opening
- (BOOL)showDatabase:(NSString *)path {
    [self close];   // the same file again (changed on disk) is read afresh
    _path = [path copy];
    NSDictionary *place = _remembered[path];
    _query.stringValue = place[@"query"] ?: @"";   // another database's query is not this one's
    const uint64_t gen = ++_generation;
    _busy = YES;
    _status.stringValue = @"Opening…";
    _status.textColor = Muted();
    // Opened on the queue, never on the main thread: a database on a slow
    // volume, or a query still giving way on the queue, holds up nothing.
    __weak MCSqliteView *weakSelf = self;
    NSString *file = [path copy];
    dispatch_queue_t queue = _queue;
    dispatch_async(queue, ^{
        Opened o = OpenDatabase(file);
        dispatch_async(dispatch_get_main_queue(), ^{
            MCSqliteView *me = weakSelf;
            if (!me || gen != me->_generation) {
                sqlite3 *db = o.db;
                if (db) dispatch_async(queue, ^{ sqlite3_close_v2(db); });
                return;
            }
            [me opened:o place:place];
        });
    });
    return YES;
}

// The open's result, on the main thread. A database SQLite cannot open is
// said so in the tab, rather than left to Quick Look's blank icon.
- (void)opened:(const Opened &)o place:(NSDictionary *)place {
    _busy = NO;
    if (!o.db) {
        _status.stringValue = [NSString stringWithFormat:@"This database cannot be opened: %@",
                                                         o.problem ?: @"unknown error"];
        _status.textColor = [NSColor systemOrangeColor];
        return;
    }
    _db = o.db;
    _names = o.names;
    _sql = o.sql;
    _types = o.types;
    [_objects reloadData];
    _status.stringValue = _names.count
        ? [NSString stringWithFormat:@"%lu %@, read-only", (unsigned long)_names.count,
                                     _names.count == 1 ? @"table" : @"tables and views"]
        : @"No tables, read-only";
    // Back to what was on show, if it is still there; else the first table.
    NSString *object = place[@"object"];
    NSUInteger row = object ? [_names indexOfObject:object] : NSNotFound;
    if (row == NSNotFound && _names.count) row = 0;
    NSString *ran = place[@"ran"];
    if (row != NSNotFound) {
        _restoring = YES;   // the selection change below shows nothing by itself
        [_objects selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
        _restoring = NO;
        [_objects scrollRowToVisible:(NSInteger)row];
        if (ran) {
            _schema.string = _sql[_names[row]] ?: @"";
            _object = _names[row];
        } else {
            [self showObject:_names[row]];
        }
    }
    if (ran) [self runQuery:ran];
}

// What is on show, kept for when this database is opened again.
- (void)rememberPlace {
    if (!_path) return;
    if (!_remembered) _remembered = [NSMutableDictionary dictionary];
    if (_remembered.count >= 50 && !_remembered[_path]) [_remembered removeAllObjects];
    NSMutableDictionary *p = [NSMutableDictionary dictionary];
    if (_object) p[@"object"] = _object;
    if (_ranQuery) p[@"ran"] = _ranQuery;
    if (_query.stringValue.length) p[@"query"] = _query.stringValue;
    _remembered[_path] = p;
}

- (void)close {
    // Still opening (no _db yet): what was asked to come back stays asked.
    if (_db) [self rememberPlace];
    _generation++;
    _busy = NO;
    _object = nil;
    _ranQuery = nil;
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
    _object = name;
    _ranQuery = nil;
    [self run:[NSString stringWithFormat:@"SELECT * FROM %@", Quoted(name)] fromList:YES];
}

- (void)queryEntered:(id)sender {
    NSString *sql = [_query.stringValue stringByTrimmingCharactersInSet:
                         NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (sql.length) [self runQuery:sql];
}

- (void)runQuery:(NSString *)sql {
    _ranQuery = [sql copy];
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
        NSString *why = sqlite3_errcode(db) == SQLITE_AUTH
            ? @"Not run: the database is open read-only, and this statement would start a "
              @"transaction, attach a database, or change a setting."
            : st ? @(sqlite3_errmsg(db))
                 : (sqlite3_errcode(db) == SQLITE_OK ? @"nothing to run" : @(sqlite3_errmsg(db)));
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
    NSMutableArray<NSArray *> *rows = [NSMutableArray array];
    BOOL truncated = NO;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (rows.count == MCSqliteMaxRows) { truncated = YES; break; }
        NSMutableArray *row = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
        for (int i = 0; i < n; i++) {
            switch (sqlite3_column_type(st, i)) {
            case SQLITE_NULL: [row addObject:NSNull.null]; break;   // not the text "NULL"
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
    // Nothing the authorizer lets through opens a transaction, but should
    // one be open, it is ended: a read transaction left open locks out
    // other writers and freezes what the viewer shows.
    if (!sqlite3_get_autocommit(db)) {
        sqlite3_set_authorizer(db, nullptr, nullptr);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_set_authorizer(db, Authorize, nullptr);
    }
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
    NSFont *font = MCMonoFont(12, NSFontWeightRegular);
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
        for (NSUInteger r = 0; r < MIN(_rows.count, (NSUInteger)200); r++) {
            id v = _rows[r][i];
            widest = MAX(widest, v == NSNull.null ? (NSUInteger)4 : [v length]);
        }
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
    NSArray *r = _rows[(NSUInteger)row];
    const NSUInteger i = (NSUInteger)col.identifier.integerValue;
    if (i >= r.count) return @"";
    return r[i] == NSNull.null ? @"NULL" : r[i];
}

- (void)tableView:(NSTableView *)tv willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)col
              row:(NSInteger)row {
    NSTextFieldCell *c = cell;
    if (tv == _objects) {
        c.textColor = [[AppSettings shared] text:Surface::Editor];
        return;
    }
    const BOOL null = [self isNullAtRow:(NSUInteger)row column:(NSUInteger)col.identifier.integerValue];
    c.textColor = null ? Muted() : [[AppSettings shared] text:Surface::Editor];
    // NULL in italics as well as muted, so the text "NULL" never passes for it.
    NSFont *font = c.font ?: MCMonoFont(12, NSFontWeightRegular);
    NSFontManager *fm = [NSFontManager sharedFontManager];
    c.font = null ? [fm convertFont:font toHaveTrait:NSItalicFontMask]
                  : [fm convertFont:font toNotHaveTrait:NSItalicFontMask];
}

- (void)tableViewSelectionDidChange:(NSNotification *)n {
    if (n.object != _objects || _restoring) return;
    const NSInteger row = _objects.selectedRow;
    if (row >= 0 && row < (NSInteger)_names.count) [self showObject:_names[(NSUInteger)row]];
}
@end
