// FileViewers.mm — see FileViewers.h.
#import "FileViewers.h"
#import "QuickLook.h"
#import "CsvTable.h"
#import "JsonOutlineView.h"
#import "SqliteView.h"
#include "FileSniff.h"
#include "JsonOutline.h"
#include "Notebook.h"
#include <string>

const unsigned long long MCCsvEditableBytes = 50ull * 1000 * 1000;
const NSUInteger MCCsvLargeRows = 100000;

static std::string Utf8(NSString *s) {
    const char *c = s.fileSystemRepresentation;
    return c ? c : "";
}

MCViewerKind MCViewerKindForFile(NSString *path) {
    if (!path) return MCViewerNone;
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    if (!attrs) return MCViewerNone;
    if ([attrs.fileType isEqualToString:NSFileTypeDirectory])
        return MCViewerQuickLook;   // a package (.pages, .rtfd) opened as a file
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!fh) return MCViewerNone;   // unreadable: the text path says why
    NSData *head = [fh readDataUpToLength:FileSniff::kHeadBytes error:nil];
    [fh closeFile];
    std::string bytes((const char *)head.bytes, head.length);
    const BOOL large = attrs.fileSize > MCCsvEditableBytes;
    switch (FileSniff::classify(Utf8(path), bytes)) {
    case FileSniff::Kind::QuickLook: return MCViewerQuickLook;
    case FileSniff::Kind::Csv: return large ? MCViewerCsvLarge : MCViewerCsv;
    case FileSniff::Kind::Tsv: return large ? MCViewerCsvLarge : MCViewerTsv;
    case FileSniff::Kind::Json: return MCViewerJson;
    case FileSniff::Kind::Notebook: return MCViewerNotebook;
    case FileSniff::Kind::Sqlite: return MCViewerSqlite;
    default: return MCViewerNone;
    }
}

BOOL MCViewerKindWithoutText(MCViewerKind kind) {
    return kind == MCViewerQuickLook || kind == MCViewerCsvLarge || kind == MCViewerSqlite;
}

BOOL MCViewerKindOverText(MCViewerKind kind) {
    return kind == MCViewerCsv || kind == MCViewerTsv;
}

BOOL MCViewerKindBesideText(MCViewerKind kind) {
    return kind == MCViewerJson;
}

BOOL MCViewerKindRendersMarkdown(MCViewerKind kind) {
    return kind == MCViewerNotebook;
}

NSString *MCNotebookMarkdown(NSString *json) {
    const char *c = json.UTF8String;
    const std::string md = Notebook::markdownForFile(c ? c : "");
    return [[NSString alloc] initWithBytes:md.data() length:md.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

static BOOL IsTsvPath(NSString *path) {
    NSString *ext = path.pathExtension.lowercaseString;
    return [ext isEqualToString:@"tsv"] || [ext isEqualToString:@"tab"];
}

@implementation MCFileViewers {
    __weak NSView *_container;
    __weak NSView *_sibling;
    MCViewerKind _kind;
    MCQuickLookSlot *_quickLook;
    MCCsvTableView *_csv;
    MCJsonOutlinePane *_json;
    MCSqliteView *_sqlite;
}

- (MCSqliteView *)sqlite { return _sqlite; }

- (instancetype)initWithContainer:(NSView *)container below:(NSView *)sibling {
    if ((self = [super init])) {
        _container = container;
        _sibling = sibling;
        _besideTextShown = YES;
    }
    return self;
}

- (MCJsonOutlinePane *)json { return _json; }

- (void)showBesideTextView:(NSTextView *)textView kind:(MCViewerKind)kind path:(NSString *)path {
    if (kind != _kind) [self clear];
    if (kind != MCViewerJson) return;
    if (!_json) {
        _json = [[MCJsonOutlinePane alloc] initWithFrame:NSZeroRect];
        [self addView:_json];
    }
    _json.allowsComments = path.length && JsonOutline::allowsComments(path.UTF8String ?: "");
    [_json attachToTextView:textView];
    _kind = kind;
}

- (MCViewerKind)shownKind { return _kind; }
- (MCQuickLookSlot *)quickLook { return _quickLook; }
- (MCCsvTableView *)csv { return _csv; }

- (void)addView:(NSView *)v {
    v.hidden = YES;
    if (_sibling && _sibling.superview == _container)
        [_container addSubview:v positioned:NSWindowBelow relativeTo:_sibling];
    else
        [_container addSubview:v];
}

- (MCCsvTableView *)makeCsv {
    if (!_csv) {
        _csv = [[MCCsvTableView alloc] initWithFrame:NSZeroRect];
        __weak MCFileViewers *weakSelf = self;
        _csv.onEdit = ^(NSString *text, NSString *name) {
            MCFileViewers *me = weakSelf;
            if (me.onTextEdited) me.onTextEdited(text, name);
        };
        _csv.undoManagerProvider = ^NSUndoManager * {
            MCFileViewers *me = weakSelf;
            return me.undoManagerProvider ? me.undoManagerProvider() : nil;
        };
        [self addView:_csv];
    }
    return _csv;
}

- (BOOL)showWithoutText:(MCViewerKind)kind path:(NSString *)path {
    [self clear];
    if (kind == MCViewerQuickLook) {
        if (!_quickLook) {
            _quickLook = [[MCQuickLookSlot alloc] initWithFrame:NSZeroRect];
            [self addView:_quickLook];
        }
        [_quickLook showPath:path];
        _kind = kind;
        return YES;
    }
    if (kind == MCViewerSqlite) {
        if (!_sqlite) {
            _sqlite = [[MCSqliteView alloc] initWithFrame:NSZeroRect];
            [self addView:_sqlite];
        }
        if (![_sqlite showDatabase:path]) return NO;
        _kind = kind;
        return YES;
    }
    if (kind == MCViewerCsvLarge) {
        if (![[self makeCsv] showLargeFile:path tsv:IsTsvPath(path) maxRows:MCCsvLargeRows])
            return NO;
        _kind = kind;
        return YES;
    }
    return NO;
}

- (void)showText:(NSString *)text kind:(MCViewerKind)kind path:(NSString *)path {
    if (kind != _kind) [self clear];
    if (kind == MCViewerCsv || kind == MCViewerTsv) {
        [[self makeCsv] showText:text path:path tsv:kind == MCViewerTsv];
        _kind = kind;
    }
}

- (void)commitEditing {
    if (_kind == MCViewerCsv || _kind == MCViewerTsv) [_csv commitEditing];
}

- (void)clear {
    [_quickLook clear];
    if (_kind == MCViewerCsv || _kind == MCViewerTsv || _kind == MCViewerCsvLarge) [_csv clear];
    if (_kind == MCViewerJson) [_json attachToTextView:nil];
    if (_kind == MCViewerSqlite) [_sqlite close];
    _kind = MCViewerNone;
}

- (NSRect)layoutInRect:(NSRect)rect editorShown:(BOOL)shown coversText:(BOOL *)coversText {
    BOOL covers = NO;
    if (_quickLook) {
        const BOOL on = shown && _kind == MCViewerQuickLook;
        _quickLook.frame = rect;
        _quickLook.hidden = !on;
        covers = covers || on;
    }
    if (_csv) {
        const BOOL on = shown && (_kind == MCViewerCsv || _kind == MCViewerTsv ||
                                  _kind == MCViewerCsvLarge);
        _csv.frame = rect;
        _csv.hidden = !on;
        covers = covers || on;
    }
    if (_sqlite) {
        const BOOL on = shown && _kind == MCViewerSqlite;
        _sqlite.frame = rect;
        _sqlite.hidden = !on;
        covers = covers || on;
    }
    NSRect text = rect;
    if (_json) {
        // At the right of the text, a third of the slot within reason, and
        // only when the text keeps room to be read.
        const BOOL on = shown && _kind == MCViewerJson && _besideTextShown &&
                        NSWidth(rect) >= 480;
        const CGFloat w = on ? round(MIN(MAX(NSWidth(rect) * 0.3, 220), 420)) : 0;
        _json.frame = NSMakeRect(NSMaxX(rect) - w, NSMinY(rect), w, NSHeight(rect));
        _json.hidden = !on;
        text.size.width -= w;
    }
    if (coversText) *coversText = covers;
    return text;
}

- (NSView *)focusView {
    switch (_kind) {
    case MCViewerQuickLook: return _quickLook.preview;
    case MCViewerCsv:
    case MCViewerTsv:
    case MCViewerCsvLarge: return _csv.table;
    case MCViewerSqlite: return _sqlite.objectsTable;
    default: return nil;
    }
}
@end
