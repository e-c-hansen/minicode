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

// Strip ANSI escape sequences and stray control characters so a matched line
// from a file that contains terminal color codes renders as plain text.
static NSString *CleanLine(NSString *s) {
    static NSRegularExpression *ansi;
    if (!ansi)
        ansi = [NSRegularExpression
            regularExpressionWithPattern:@"\x1b[@-_][0-?]*[ -/]*[@-~]"
                                 options:0 error:nil];
    s = [ansi stringByReplacingMatchesInString:s options:0
                                         range:NSMakeRange(0, s.length)
                                  withTemplate:@""];
    // Drop any remaining control characters (lone ESC, bells, etc.).
    NSCharacterSet *ctrl = [NSCharacterSet controlCharacterSet];
    if ([s rangeOfCharacterFromSet:ctrl].location != NSNotFound)
        s = [[s componentsSeparatedByCharactersInSet:ctrl]
                componentsJoinedByString:@""];
    return s;
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
    NSString *_scope;               // directory currently being searched
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *scopeField;
@property(nonatomic, strong) NSTextField *field;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSTableView *table;
@end

@implementation SearchPanel

- (instancetype)initWithRoot:(NSString *)root
                 openHandler:(void (^)(NSString *, NSInteger))handler {
    if ((self = [super init])) {
        _root = root;
        _scope = root;
        _open = [handler copy];
        _hits = [NSMutableArray array];
        [self build];
    }
    return self;
}

- (void)build {
    NSRect frame = NSMakeRect(0, 0, 680, 460);
    CGFloat W = frame.size.width, H = frame.size.height;
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"Search";
    self.window.releasedWhenClosed = NO;
    [self.window center];

    NSView *content = self.window.contentView;

    // --- scope row: "Folder:" [ editable path ] [ Choose… ] ---
    NSTextField *label = [NSTextField labelWithString:@"Folder:"];
    label.frame = NSMakeRect(12, H - 37, 48, 18);
    label.textColor = SHex(0x9CA3AF);
    label.font = [NSFont systemFontOfSize:11];
    label.autoresizingMask = NSViewMinYMargin;
    [content addSubview:label];

    self.scopeField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(62, H - 39, W - 62 - 12 - 92, 22)];
    self.scopeField.font = [NSFont systemFontOfSize:11];
    self.scopeField.delegate = self;
    self.scopeField.target = self;
    self.scopeField.action = @selector(scopeEntered:);
    self.scopeField.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.scopeField];

    NSButton *choose = [NSButton buttonWithTitle:@"Choose…"
                                          target:self action:@selector(chooseScope:)];
    choose.frame = NSMakeRect(W - 12 - 90, H - 41, 90, 26);
    choose.bezelStyle = NSBezelStyleRounded;
    choose.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [content addSubview:choose];

    // --- query field ---
    self.field = [[NSTextField alloc]
        initWithFrame:NSMakeRect(12, H - 70, W - 24, 24)];
    self.field.placeholderString = @"Search text in this folder…";
    self.field.delegate = self;
    self.field.target = self;
    self.field.action = @selector(runSearch:);
    self.field.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.field];

    self.status = [NSTextField labelWithString:@""];
    self.status.frame = NSMakeRect(12, H - 90, W - 24, 16);
    self.status.textColor = SHex(0x9CA3AF);
    self.status.font = [NSFont systemFontOfSize:11];
    self.status.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.status];

    [self updateScopeField];

    NSScrollView *scroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12, 12, W - 24, H - 110)];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.borderType = NSBezelBorder;

    self.table = [[NSTableView alloc] init];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"hit"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    col.minWidth = 240;
    [self.table addTableColumn:col];
    self.table.columnAutoresizingStyle =
        NSTableViewLastColumnOnlyAutoresizingStyle;   // fill the width
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
    [self.table sizeLastColumnToFit];
    [self.window makeFirstResponder:self.field];
}

// ------------------------------------------------------------------- scope
- (void)updateScopeField {
    self.scopeField.stringValue = [_scope stringByAbbreviatingWithTildeInPath];
}

// Point the search at a directory and re-run the current query.
- (void)setScope:(NSString *)dir {
    if (dir.length) _scope = dir;
    [self updateScopeField];
    [self runSearch:nil];
}

// The user typed a path into the scope field.
- (void)scopeEntered:(id)sender {
    NSString *entered = [self.scopeField.stringValue stringByTrimmingCharactersInSet:
                         [NSCharacterSet whitespaceCharacterSet]];
    NSString *path;
    if ([entered hasPrefix:@"~"]) path = [entered stringByExpandingTildeInPath];
    else if ([entered hasPrefix:@"/"]) path = entered;
    else path = [_root stringByAppendingPathComponent:entered];  // relative to root
    path = [path stringByStandardizingPath];
    BOOL isDir = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir]
        && isDir) {
        _scope = path;
        [self updateScopeField];
        [self runSearch:nil];
    } else {
        self.status.stringValue = @"Not a folder";
        [self updateScopeField];   // revert to the valid scope
    }
}

// Pick a folder with a standard open panel.
- (void)chooseScope:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    panel.directoryURL = [NSURL fileURLWithPath:_scope];
    if ([panel runModal] == NSModalResponseOK) {
        _scope = panel.URLs.firstObject.path;
        [self updateScopeField];
        [self runSearch:nil];
    }
}

// Search as the user types, but only after a longer pause and for queries of a
// few characters, so we don't kick off a full folder scan on every keypress.
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != self.field) return;   // scope field handled separately
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(runSearchNow)
                                               object:nil];
    [self performSelector:@selector(runSearchNow) withObject:nil afterDelay:0.35];
}
- (void)runSearchNow { [self runSearch:nil]; }

// ------------------------------------------------------------------ search
- (void)runSearch:(id)sender {
    // Cancel any in-flight search immediately (bumps the generation).
    ++_searchGeneration;
    NSString *query = [self.field.stringValue stringByTrimmingCharactersInSet:
                       [NSCharacterSet whitespaceCharacterSet]];
    [_hits removeAllObjects];
    [self.table reloadData];
    if (query.length < 2) {   // avoid scanning on 0–1 characters
        self.status.stringValue = query.length ? @"Type at least 2 characters" : @"";
        return;
    }

    self.status.stringValue = @"Searching…";
    NSUInteger gen = _searchGeneration;
    NSString *root = _scope;   // search only within the chosen folder
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
            if (!isCurrent()) return hits;   // a newer search superseded us
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
                    NSString *clean = CleanLine([line stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceCharacterSet]]);
                    h.text = clean.length > 200 ? [clean substringToIndex:200] : clean;
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
        MIN(h.path.length, _scope.length + 1)];
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
