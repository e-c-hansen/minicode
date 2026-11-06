// CsvTable.h — a CSV or TSV file as a table in the editor's slot (Csv.h
// does the reading and writing). Cells are edited in place (double click,
// or Return on a row); the right click menu adds and deletes rows and
// columns. Every edit hands the whole file's new text to `onEdit`, so the
// tab's text is what changes, and dirty, backups, auto-save and saving work
// as for text; the text written back changes only what was edited.
//
// A file too large to edit (over 50 MB) is shown read-only, its first
// 100,000 rows only, read straight from the disk.
#import <Cocoa/Cocoa.h>

@interface MCCsvTableView : NSView
@property(nonatomic, readonly) NSTableView *table;
// The file's whole new text after an edit, and the edit's name for Undo.
@property(nonatomic, copy) void (^onEdit)(NSString *text, NSString *actionName);
// The undo manager edits are undone with (the tab's).
@property(nonatomic, copy) NSUndoManager * (^undoManagerProvider)(void);

// The tab's text. `tsv` picks tabs; otherwise the delimiter is guessed.
// Shown again with the same file, the scroll and selection stay. A cell
// still being typed into is dropped first (the rows under it are about to
// change), as it is by showLargeFile: and clear; whoever changes what the
// table shows on purpose (another tab, the text) calls commitEditing first.
- (void)showText:(NSString *)text path:(NSString *)path tsv:(BOOL)tsv;
// A large file, read-only: the first `maxRows` rows read from disk.
- (BOOL)showLargeFile:(NSString *)path tsv:(BOOL)tsv maxRows:(NSUInteger)maxRows;
- (void)clear;
// A cell being typed into is put into the document now, as a click
// elsewhere would, so it lands in the file it was typed in.
- (void)commitEditing;

@property(nonatomic, readonly) BOOL editable;
@property(nonatomic, readonly) NSUInteger rowCount, columnCount;
@property(nonatomic, readonly) BOOL truncated;
@property(nonatomic, readonly) NSString *infoText;   // the line over the table
- (NSString *)cellAtRow:(NSUInteger)row column:(NSUInteger)column;

// Edits, as the menu and the cell editor make them (rows and columns from 0).
- (BOOL)setCell:(NSString *)value row:(NSUInteger)row column:(NSUInteger)column;
- (BOOL)insertRowAt:(NSUInteger)row;
- (BOOL)deleteRowAt:(NSUInteger)row;
- (BOOL)insertColumnAt:(NSUInteger)column;
- (BOOL)deleteColumnAt:(NSUInteger)column;
// The right click menu for a row and a table column (0 is the row numbers,
// 1 the file's first column); -1 for none.
- (NSMenu *)menuForRow:(NSInteger)row column:(NSInteger)column;
@end
