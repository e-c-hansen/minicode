// Search.mm — recursive, project-wide text search. Objective-C++.
// A floating window with a query field and a results list; selecting a result
// calls back to open that file at the matching line.
#import "Search.h"

static NSColor *SHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8)  & 0xFF) / 255.0
                                blue:( rgb        & 0xFF) / 255.0
                               alpha:1.0];
}

// One match: file path, 1-based line number, and the line's text.
@interface SearchHit : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, assign) NSInteger line;
@property(nonatomic, copy) NSString *text;
@end
@implementation SearchHit
@end

@interface SearchPanel () <NSTableViewDataSource, NSTableViewDelegate,
                           NSTextFieldDelegate> {
    NSString *_root;
    void (^_open)(NSString *, NSInteger);
    NSMutableArray<SearchHit *> *_hits;
    NSUInteger _searchGeneration;   // cancels a stale in-flight search
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *field;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSTableView *table;
@end

@implementation SearchPanel

- (instancetype)initWithRoot:(NSString *)root
                 openHandler:(void (^)(NSString *, NSInteger))handler {
    if ((self = [super init])) {
        _root = root;
        _open = [handler copy];
        _hits = [NSMutableArray array];
        [self build];
    }
    return self;
}

- (void)build {
    NSRect frame = NSMakeRect(0, 0, 680, 460);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = [NSString stringWithFormat:@"Search in %@",
                         _root.lastPathComponent];
    self.window.releasedWhenClosed = NO;
    [self.window center];

    NSView *content = self.window.contentView;

    self.field = [[NSTextField alloc]
        initWithFrame:NSMakeRect(12, frame.size.height - 40, frame.size.width - 24, 24)];
    self.field.placeholderString = @"Search text across the folder, then press Return";
    self.field.delegate = self;
    self.field.target = self;
    self.field.action = @selector(runSearch:);
    self.field.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.field];

    self.status = [NSTextField labelWithString:@""];
    self.status.frame = NSMakeRect(12, frame.size.height - 62, frame.size.width - 24, 16);
    self.status.textColor = SHex(0x9CA3AF);
    self.status.font = [NSFont systemFontOfSize:11];
    self.status.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.status];

    NSScrollView *scroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12, 12, frame.size.width - 24, frame.size.height - 82)];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.borderType = NSBezelBorder;

    self.table = [[NSTableView alloc] init];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"hit"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [self.table addTableColumn:col];
    self.table.headerView = nil;
    self.table.rowHeight = 20;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.doubleAction = @selector(openSelected:);
    self.table.target = self;
    scroll.documentView = self.table;
    [content addSubview:scroll];
}

- (void)show {
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.field];
}

// ------------------------------------------------------------------ search
- (void)runSearch:(id)sender {
    NSString *query = [self.field.stringValue stringByTrimmingCharactersInSet:
                       [NSCharacterSet whitespaceCharacterSet]];
    [_hits removeAllObjects];
    [self.table reloadData];
    if (query.length == 0) { self.status.stringValue = @""; return; }

    self.status.stringValue = @"Searching…";
    NSUInteger gen = ++_searchGeneration;
    NSString *root = _root;
    __weak SearchPanel *weakSelf = self;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<SearchHit *> *found = [SearchPanel searchRoot:root query:query
                                                    generation:gen
                                                    isCurrent:^BOOL {
            SearchPanel *s = weakSelf;
            return s && s->_searchGeneration == gen;
        }];
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s || s->_searchGeneration != gen) return;   // superseded
            [s->_hits addObjectsFromArray:found];
            [s.table reloadData];
            NSUInteger files = 0;
            NSMutableSet *seen = [NSMutableSet set];
            for (SearchHit *h in found) if (![seen containsObject:h.path]) {
                [seen addObject:h.path]; files++;
            }
            s.status.stringValue = [NSString stringWithFormat:
                @"%lu matches in %lu files", (unsigned long)found.count,
                (unsigned long)files];
        });
    });
}

// Directories we never descend into (heavy / uninteresting).
+ (BOOL)skipDir:(NSString *)name {
    static NSSet *skip;
    if (!skip) skip = [NSSet setWithArray:@[@".git", @"node_modules", @"build",
        @".build", @"DerivedData", @"dist", @".venv", @"venv", @"__pycache__"]];
    return [skip containsObject:name];
}

+ (NSArray<SearchHit *> *)searchRoot:(NSString *)root
                              query:(NSString *)query
                         generation:(NSUInteger)gen
                          isCurrent:(BOOL (^)(void))isCurrent {
    NSMutableArray<SearchHit *> *hits = [NSMutableArray array];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<NSString *> *stack = [NSMutableArray arrayWithObject:root];
    const NSUInteger kMaxHits = 2000;
    const unsigned long long kMaxFileSize = 1024 * 1024;   // 1 MB

    while (stack.count && hits.count < kMaxHits) {
        if (!isCurrent()) break;
        NSString *dir = stack.lastObject;
        [stack removeLastObject];
        NSArray *entries = [fm contentsOfDirectoryAtPath:dir error:nil];
        for (NSString *entry in entries) {
            if ([entry hasPrefix:@"."]) continue;
            NSString *full = [dir stringByAppendingPathComponent:entry];
            BOOL isDir = NO;
            [fm fileExistsAtPath:full isDirectory:&isDir];
            if (isDir) {
                if (![self skipDir:entry]) [stack addObject:full];
                continue;
            }
            NSDictionary *attrs = [fm attributesOfItemAtPath:full error:nil];
            if (attrs.fileSize > kMaxFileSize) continue;
            NSString *content = [NSString stringWithContentsOfFile:full
                                    encoding:NSUTF8StringEncoding error:nil];
            if (!content) continue;   // binary / non-UTF8
            __block NSInteger lineNo = 0;
            [content enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
                lineNo++;
                NSRange m = [line rangeOfString:query
                                        options:NSCaseInsensitiveSearch];
                if (m.location != NSNotFound) {
                    SearchHit *h = [SearchHit new];
                    h.path = full;
                    h.line = lineNo;
                    h.text = [line stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceCharacterSet]];
                    if (h.text.length > 200) h.text = [h.text substringToIndex:200];
                    [hits addObject:h];
                    if (hits.count >= kMaxHits) *stop = YES;
                }
            }];
        }
    }
    return hits;
}

// ------------------------------------------------------------- table + open
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return _hits.count; }

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    NSTextField *cell = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!cell) {
        cell = [NSTextField labelWithString:@""];
        cell.identifier = @"cell";
        cell.font = [NSFont monospacedSystemFontOfSize:11.5
                                                weight:NSFontWeightRegular];
    }
    SearchHit *h = _hits[row];
    NSString *rel = [h.path substringFromIndex:
        MIN(h.path.length, _root.length + 1)];
    NSMutableAttributedString *a = [[NSMutableAttributedString alloc] init];
    NSString *loc = [NSString stringWithFormat:@"%@:%ld  ", rel, (long)h.line];
    [a appendAttributedString:[[NSAttributedString alloc] initWithString:loc
        attributes:@{NSForegroundColorAttributeName: SHex(0x7FB0E0)}]];
    [a appendAttributedString:[[NSAttributedString alloc] initWithString:h.text
        attributes:@{NSForegroundColorAttributeName: SHex(0xD4D4D4)}]];
    cell.attributedStringValue = a;
    return cell;
}

- (void)openSelected:(id)sender {
    NSInteger row = self.table.clickedRow >= 0 ? self.table.clickedRow
                                               : self.table.selectedRow;
    if (row < 0 || row >= (NSInteger)_hits.count) return;
    SearchHit *h = _hits[row];
    if (_open) _open(h.path, h.line);
}

@end
