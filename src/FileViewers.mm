// FileViewers.mm — see FileViewers.h.
#import "FileViewers.h"
#import "QuickLook.h"
#import "CsvTable.h"
#include "FileSniff.h"
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
    default: return MCViewerNone;
    }
}

BOOL MCViewerKindWithoutText(MCViewerKind kind) {
    return kind == MCViewerQuickLook || kind == MCViewerCsvLarge;
}

BOOL MCViewerKindOverText(MCViewerKind kind) {
    return kind == MCViewerCsv || kind == MCViewerTsv;
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
}

- (instancetype)initWithContainer:(NSView *)container below:(NSView *)sibling {
    if ((self = [super init])) {
        _container = container;
        _sibling = sibling;
    }
    return self;
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

- (void)clear {
    [_quickLook clear];
    if (_kind == MCViewerCsv || _kind == MCViewerTsv || _kind == MCViewerCsvLarge) [_csv clear];
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
    if (coversText) *coversText = covers;
    return rect;
}

- (NSView *)focusView {
    switch (_kind) {
    case MCViewerQuickLook: return _quickLook.preview;
    case MCViewerCsv:
    case MCViewerTsv:
    case MCViewerCsvLarge: return _csv.table;
    default: return nil;
    }
}
@end
