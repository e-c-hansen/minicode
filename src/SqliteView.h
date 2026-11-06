// SqliteView.h — a SQLite database in the editor's slot, read-only: its
// tables and views down the left with the selected one's schema, the
// first 1,000 rows of a table, and a query box. The database is opened
// with SQLITE_OPEN_READONLY and a mode=ro URI (and query_only on), so
// nothing typed in the box can change it; a statement that would write is
// refused before it runs, and so is one that would reach past the viewer
// (BEGIN, which would hold a lock other writers wait on, SAVEPOINT, ATTACH,
// a pragma that sets something). A query has a time limit (a progress
// handler stops it). All of the database's work, opening it included,
// happens on one background queue. A WAL database whose writer has closed
// (no -wal file beside it) is opened as immutable, since a read-only
// connection cannot make the files WAL needs. A reopen of the same file
// comes back to the table or query that was on show.
#import <Cocoa/Cocoa.h>

@interface MCSqliteView : NSView
// Opens the database in the background (busy until it is open, its first
// table's rows asked for). One SQLite cannot open says so in the status
// line. YES: the tab is the database's either way.
- (BOOL)showDatabase:(NSString *)path;
- (void)close;

@property(nonatomic, readonly, copy) NSString *path;
@property(nonatomic, readonly) NSArray<NSString *> *objectNames;   // tables and views
@property(nonatomic, readonly, copy) NSString *schemaText;         // the selected one's SQL
@property(nonatomic, readonly) NSArray<NSString *> *columns;       // of the rows on show
@property(nonatomic, readonly) NSArray<NSArray<NSString *> *> *rows;   // NULL as @"NULL"
- (BOOL)isNullAtRow:(NSUInteger)row column:(NSUInteger)column;     // SQL NULL, not the text
@property(nonatomic, readonly, copy) NSString *statusText;
@property(nonatomic, readonly) BOOL busy;                          // opening, or a query running
@property(nonatomic, readonly) NSTextField *queryField;
@property(nonatomic, readonly) NSTableView *objectsTable, *rowsTable;
// How long a query may run, in seconds (5 unless changed).
@property(nonatomic, assign) NSTimeInterval timeLimit;

// The first 1,000 rows of a table or view, as a click in the list does.
- (void)showObject:(NSString *)name;
// Runs SQL typed in the box, as Return there does.
- (void)runQuery:(NSString *)sql;
@end

// The most rows shown of a table or a query.
extern const NSUInteger MCSqliteMaxRows;
