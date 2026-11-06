// Search.mm — Find in Folder, Replace in Folder and the TODO list (see
// Search.h). Objective-C++. A window with a scope, a query and its options,
// a replacement, and a results list; choosing a result calls back to open
// that file at the matching line. The searching is FolderSearch's, off the
// main thread, a newer search cancelling the one before.
#import "Search.h"
#import "AppSettings.h"
#include "FolderSearch.h"
#include "TextFind.h"
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

static NSColor *SHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8)  & 0xFF) / 255.0
                                blue:( rgb        & 0xFF) / 255.0
                               alpha:1.0];
}

static std::string SStd(NSString *s) { return s.UTF8String ?: ""; }
static NSString *SNS(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
static std::u16string SU16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}
static NSString *SFromU16(const std::u16string &u) {
    return [NSString stringWithCharacters:(const unichar *)u.data() length:u.size()];
}

// One match: file path, 1-based line number, the line's text, and where the
// match starts in the raw line (bytes), for replacing just it.
@interface SearchHit : NSObject
@property(nonatomic, copy) NSString *path;
@property(nonatomic, assign) NSInteger line;
@property(nonatomic, copy) NSString *text;
@property(nonatomic, assign) NSUInteger byteColumn;
@end
@implementation SearchHit
@end

@interface SearchPanel () <NSTableViewDataSource, NSTableViewDelegate,
                           NSTextFieldDelegate, NSWindowDelegate> {
    NSString *_root;
    void (^_open)(NSString *, NSInteger);
    NSMutableArray<SearchHit *> *_hits;
    NSUInteger _searchGeneration;   // drops a stale in-flight search's results
    NSString *_scope;               // directory currently being searched
    BOOL _todos;                    // the TODO list, not a text search
    NSArray<SearchHit *> *_allTodos;   // the last scan, before the filter
    std::shared_ptr<std::atomic<bool>> _cancel;   // the search or scan in flight
    NSButton *_case, *_word, *_regex;
    NSButton *_replaceMatch, *_replaceFile, *_replaceAll;
    BOOL _replacing;
    NSString *_afterNote;           // what a replace did, shown with the next results
    BOOL _afterNoteError;
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *scopeField;
@property(nonatomic, strong) NSTextField *field;
@property(nonatomic, strong) NSTextField *replaceField;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSTableView *table;
@end

@implementation SearchPanel

- (instancetype)initWithRoot:(NSString *)root
                 openHandler:(void (^)(NSString *, NSInteger))handler {
    if ((self = [super init])) {
        [self setUpWithRoot:root openHandler:handler];
    }
    return self;
}

- (instancetype)initTodosWithRoot:(NSString *)root
                      openHandler:(void (^)(NSString *, NSInteger))handler {
    if ((self = [super init])) {
        _todos = YES;
        [self setUpWithRoot:root openHandler:handler];
    }
    return self;
}

- (void)setUpWithRoot:(NSString *)root openHandler:(void (^)(NSString *, NSInteger))handler {
    _root = root;
    _scope = root;
    _open = [handler copy];
    _hits = [NSMutableArray array];
    [self build];
}

- (void)dealloc {
    if (_cancel) _cancel->store(true);
}

// VS Code's toggles: plain when off, on a tinted square with an accent
// border when on.
- (NSButton *)toggle:(NSString *)title tip:(NSString *)tip {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:@selector(optionToggled:)];
    [b setButtonType:NSButtonTypePushOnPushOff];
    b.bordered = NO;
    b.wantsLayer = YES;
    b.layer.cornerRadius = 3;
    b.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    b.toolTip = tip;
    b.refusesFirstResponder = YES;
    b.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    return b;
}

- (void)showToggleStates {
    NSColor *accent = SHex(0x4EA1F7);
    for (NSButton *b in @[_case, _word, _regex]) {
        BOOL on = b.state == NSControlStateValueOn;
        b.layer.backgroundColor = on ? [accent colorWithAlphaComponent:0.3].CGColor : nil;
        b.layer.borderColor = accent.CGColor;
        b.layer.borderWidth = on ? 1 : 0;
        NSMutableDictionary *attrs = [@{NSFontAttributeName: b.font,
                                        NSForegroundColorAttributeName: [NSColor labelColor]}
                                      mutableCopy];
        if (b == _word) attrs[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
        b.attributedTitle = [[NSAttributedString alloc] initWithString:b.title attributes:attrs];
    }
}

- (NSButton *)button:(NSString *)title action:(SEL)action tip:(NSString *)tip {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    b.bezelStyle = NSBezelStyleRounded;
    b.controlSize = NSControlSizeSmall;
    b.font = [NSFont systemFontOfSize:11];
    b.toolTip = tip;
    b.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    return b;
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
    self.window.title = _todos ? @"TODOs" : @"Search";
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;   // the Find Options items reach the panel
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

    // --- query field, and in a search its options ---
    const CGFloat togglesW = _todos ? 0 : 3 * 26 + 4;
    self.field = [[NSTextField alloc]
        initWithFrame:NSMakeRect(12, H - 70, W - 24 - togglesW, 24)];
    self.field.placeholderString = _todos
        ? @"Filter TODOs (TODO, FIXME, HACK, XXX, BUG comments and open - [ ] tasks)…"
        : @"Search text in this folder…";
    self.field.delegate = self;
    self.field.target = self;
    self.field.action = @selector(runSearch:);
    self.field.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.field];

    CGFloat y = H - 70;
    if (!_todos) {
        _case = [self toggle:@"Aa" tip:@"Match Case (⌥⌘C)"];
        _word = [self toggle:@"ab" tip:@"Match Whole Word (⌥⌘W)"];
        _regex = [self toggle:@".*" tip:@"Use Regular Expression (⌥⌘R)"];
        CGFloat x = W - 12 - 3 * 26;
        for (NSButton *b in @[_case, _word, _regex]) {
            b.frame = NSMakeRect(x, H - 68, 24, 20);
            [content addSubview:b];
            x += 26;
        }
        [self showToggleStates];

        // --- replace row: [ replacement ] [Replace Match] [Replace in File] [Replace All]
        y = H - 100;
        const CGFloat bw1 = 104, bw2 = 104, bw3 = 86;
        self.replaceField = [[NSTextField alloc]
            initWithFrame:NSMakeRect(12, y, W - 24 - bw1 - bw2 - bw3 - 12, 24)];
        self.replaceField.placeholderString = @"Replace with… ($1 for a group in a regex)";
        self.replaceField.delegate = self;
        self.replaceField.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
        [content addSubview:self.replaceField];
        self.field.nextKeyView = self.replaceField;
        _replaceMatch = [self button:@"Replace Match" action:@selector(replaceSelectedMatch:)
                                 tip:@"Replace the selected result's match"];
        _replaceFile = [self button:@"Replace in File" action:@selector(replaceSelectedFile:)
                                tip:@"Replace every match in the selected result's file"];
        _replaceAll = [self button:@"Replace All" action:@selector(replaceAll:)
                               tip:@"Replace every match in the folder (Return in the field)"];
        CGFloat bx = W - 12 - bw3;
        _replaceAll.frame = NSMakeRect(bx, y, bw3, 24);
        bx -= bw2 + 4;
        _replaceFile.frame = NSMakeRect(bx, y, bw2, 24);
        bx -= bw1 + 4;
        _replaceMatch.frame = NSMakeRect(bx, y, bw1, 24);
        for (NSButton *b in @[_replaceMatch, _replaceFile, _replaceAll]) [content addSubview:b];
    }

    self.status = [NSTextField labelWithString:@""];
    self.status.frame = NSMakeRect(12, y - 20, W - 24, 16);
    self.status.textColor = SHex(0x9CA3AF);
    self.status.font = [NSFont systemFontOfSize:11];
    self.status.lineBreakMode = NSLineBreakByTruncatingTail;
    self.status.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:self.status];

    [self updateScopeField];

    NSScrollView *scroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12, 12, W - 24, y - 40)];
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
    if (!_todos) {
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Result"];
        [menu addItemWithTitle:@"Open" action:@selector(openSelected:) keyEquivalent:@""].target = self;
        [menu addItem:[NSMenuItem separatorItem]];
        [menu addItemWithTitle:@"Replace This Match" action:@selector(replaceSelectedMatch:)
                 keyEquivalent:@""].target = self;
        [menu addItemWithTitle:@"Replace in This File" action:@selector(replaceSelectedFile:)
                 keyEquivalent:@""].target = self;
        self.table.menu = menu;
    }
    scroll.documentView = self.table;
    [content addSubview:scroll];
}

- (void)show {
    if (_todos) [self runSearch:nil];   // the list is scanned afresh each time
    [self.window makeKeyAndOrderFront:nil];
    [self.table sizeLastColumnToFit];
    [self.window makeFirstResponder:self.field];
}

- (void)showReplace {
    [self show];
    if (self.replaceField) [self.window makeFirstResponder:self.replaceField];
}

// ----------------------------------------------------------------- options
- (BOOL)matchCase { return _case.state == NSControlStateValueOn; }
- (BOOL)wholeWord { return _word.state == NSControlStateValueOn; }
- (BOOL)regex { return _regex.state == NSControlStateValueOn; }
- (void)setOption:(NSButton *)b on:(BOOL)on {
    b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    [self optionToggled:b];
}
- (void)setMatchCase:(BOOL)v { [self setOption:_case on:v]; }
- (void)setWholeWord:(BOOL)v { [self setOption:_word on:v]; }
- (void)setRegex:(BOOL)v { [self setOption:_regex on:v]; }

- (void)optionToggled:(id)sender {
    (void)sender;
    [self showToggleStates];
    [self runSearch:nil];
}

// Option+Cmd+C, W and R (Edit > Find Options), through the window delegate.
- (void)toggleFindMatchCase:(id)sender { if (!_todos) self.matchCase = !self.matchCase; }
- (void)toggleFindWholeWord:(id)sender { if (!_todos) self.wholeWord = !self.wholeWord; }
- (void)toggleFindRegex:(id)sender { if (!_todos) self.regex = !self.regex; }

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    SEL a = item.action;
    BOOL on = NO;
    if (a == @selector(toggleFindMatchCase:)) on = self.matchCase;
    else if (a == @selector(toggleFindWholeWord:)) on = self.wholeWord;
    else if (a == @selector(toggleFindRegex:)) on = self.regex;
    else if (a == @selector(replaceSelectedMatch:) || a == @selector(replaceSelectedFile:))
        return !_replacing && [self rowForAction] >= 0;
    else return YES;
    item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    return !_todos;
}

- (FolderSearchOptions)options {
    FolderSearchOptions o;
    o.caseSensitive = self.matchCase;
    o.wholeWord = self.wholeWord;
    o.regex = self.regex;
    // search.use-ignore-files: the folder's .gitignore files, from the
    // window's folder down, even when the scope is a folder inside it.
    o.useIgnoreFiles = [AppSettings shared].settings.searchUseIgnoreFiles();
    if (_root.length) o.ignoreTop = _root.fileSystemRepresentation;
    return o;
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

// Search as the user types, but only after a pause, so we don't kick off a
// full folder scan on every keypress.
- (void)controlTextDidChange:(NSNotification *)note {
    if (note.object != self.field) return;   // scope and replacement handled separately
    if (_todos) { [self filterTodos]; return; }
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(runSearchNow)
                                               object:nil];
    [self performSelector:@selector(runSearchNow) withObject:nil afterDelay:0.35];
}
- (void)runSearchNow { [self runSearch:nil]; }

// Return in the replace field is Replace All (it asks first).
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    if (control == self.replaceField && sel == @selector(insertNewline:)) {
        [self replaceAll:nil];
        return YES;
    }
    return NO;
}

- (BOOL)busy { return _replacing || _cancel != nullptr; }

- (void)setStatus:(NSString *)text error:(BOOL)error {
    self.status.stringValue = text ?: @"";
    self.status.textColor = error ? SHex(0xF14C4C) : SHex(0x9CA3AF);
    self.status.toolTip = text.length > 80 ? text : nil;
}

// ------------------------------------------------------------------ search
- (void)runSearch:(id)sender {
    if (_todos) { [self scanTodos]; return; }
    // A replace's note goes with the results it asked for, and no others.
    NSString *note = sender == self ? _afterNote : nil;
    const BOOL noteError = _afterNoteError;
    _afterNote = nil;
    // Cancel any in-flight search immediately (bumps the generation).
    ++_searchGeneration;
    if (_cancel) _cancel->store(true);
    _cancel = nullptr;
    NSString *query = self.field.stringValue;
    const FolderSearchOptions options = [self options];
    [_hits removeAllObjects];
    [self.table reloadData];
    if (!FolderSearch::isSearchable(SStd(query), options)) {
        NSString *trimmed = [query stringByTrimmingCharactersInSet:
                             [NSCharacterSet whitespaceCharacterSet]];
        [self setStatus:trimmed.length ? @"Type at least 2 characters" : @"" error:NO];
        return;
    }

    [self setStatus:@"Searching…" error:NO];
    NSUInteger gen = _searchGeneration;
    const std::string root = _scope.fileSystemRepresentation;
    const std::string q = SStd(query);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        FolderSearchResult r = FolderSearch::search(root, q, cancel.get(), options);
        if (r.cancelled) return;
        NSMutableArray<SearchHit *> *found = [NSMutableArray array];
        NSMutableSet *files = [NSMutableSet set];
        for (const FolderSearchMatch &m : r.matches) {
            SearchHit *h = [SearchHit new];
            h.path = SNS(m.path);
            h.line = m.line;
            h.text = SNS(m.text);
            h.byteColumn = m.byteColumn;
            [found addObject:h];
            [files addObject:h.path];
        }
        NSString *error = r.error.empty() ? nil : SNS(r.error);
        const BOOL truncated = r.truncated;
        const unsigned long total = (unsigned long)r.totalMatches;
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s || s->_searchGeneration != gen) return;   // superseded
            s->_cancel = nullptr;
            [s->_hits addObjectsFromArray:found];
            [s.table reloadData];
            if (error) {
                [s setStatus:error error:YES];
                return;
            }
            // Every match, as Replace All counts them, not the rows (lines).
            NSString *counts = [NSString stringWithFormat:@"%lu %@ in %lu %@%@",
                total, total == 1 ? @"match" : @"matches",
                (unsigned long)files.count, files.count == 1 ? @"file" : @"files",
                truncated ? @" (stopped at 2000)" : @""];
            if (note) {
                [s setStatus:[NSString stringWithFormat:@"%@. Now %@", note,
                                  found.count ? counts : @"no matches"]
                       error:noteError];
                return;
            }
            [s setStatus:counts error:NO];
        });
    });
}

// ------------------------------------------------------------------- TODOs
// Scan the folder for TODOs off the main thread; a newer scan cancels this one.
- (void)scanTodos {
    ++_searchGeneration;
    if (_cancel) _cancel->store(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancel = cancel;
    _allTodos = @[];
    [_hits removeAllObjects];
    [self.table reloadData];
    self.status.stringValue = @"Looking for TODOs…";
    NSUInteger gen = _searchGeneration;
    const std::string root = _scope.fileSystemRepresentation;
    const FolderSearchOptions todoOptions = [self options];
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        FolderSearchResult r = FolderSearch::findTodos(root, cancel.get(), todoOptions);
        NSMutableArray<SearchHit *> *found = [NSMutableArray array];
        for (const FolderSearchMatch &m : r.matches) {
            SearchHit *h = [SearchHit new];
            h.path = SNS(m.path);
            h.line = m.line;
            h.text = SNS(m.text);
            h.byteColumn = m.byteColumn;
            [found addObject:h];
        }
        const BOOL truncated = r.truncated;
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s || s->_searchGeneration != gen) return;   // superseded
            s->_cancel = nullptr;
            s->_allTodos = found;
            [s filterTodos];
            if (truncated)
                s.status.stringValue = [s.status.stringValue
                    stringByAppendingString:@" (stopped at 2000)"];
        });
    });
}

// Show the scanned TODOs whose text or path holds what the field says.
- (void)filterTodos {
    NSString *q = [self.field.stringValue stringByTrimmingCharactersInSet:
                   [NSCharacterSet whitespaceCharacterSet]];
    [_hits removeAllObjects];
    NSMutableSet *files = [NSMutableSet set];
    for (SearchHit *h in _allTodos) {
        if (q.length && [h.text rangeOfString:q options:NSCaseInsensitiveSearch].location == NSNotFound &&
            [h.path rangeOfString:q options:NSCaseInsensitiveSearch].location == NSNotFound)
            continue;
        [_hits addObject:h];
        [files addObject:h.path];
    }
    [self.table reloadData];
    if (_allTodos.count == 0)
        self.status.stringValue = @"No TODOs in this folder";
    else
        self.status.stringValue = [NSString stringWithFormat:@"%lu %@ in %lu %@%@",
            (unsigned long)_hits.count, _hits.count == 1 ? @"TODO" : @"TODOs",
            (unsigned long)files.count, files.count == 1 ? @"file" : @"files",
            q.length ? [NSString stringWithFormat:@" (of %lu)",
                        (unsigned long)_allTodos.count] : @""];
}

// ----------------------------------------------------------------- replace
- (NSInteger)rowForAction {
    NSInteger row = self.table.clickedRow >= 0 ? self.table.clickedRow : self.table.selectedRow;
    return row >= 0 && row < (NSInteger)_hits.count ? row : -1;
}

- (void)replaceSelectedMatch:(id)sender { [self replaceMatchAtRow:[self rowForAction]]; }
- (void)replaceSelectedFile:(id)sender { [self replaceFileAtRow:[self rowForAction]]; }

// The query, replacement and options as they are now, or NO (with a beep and
// a word in the status line) when there is nothing to replace with.
- (BOOL)readyToReplace {
    if (_todos || _replacing) {
        NSBeep();
        return NO;
    }
    const FolderSearchOptions o = [self options];
    if (!FolderSearch::isSearchable(SStd(self.field.stringValue), o)) {
        NSBeep();
        [self setStatus:@"Type what to find first" error:NO];
        return NO;
    }
    TextFind::Query q = FolderSearch::makeQuery(SStd(self.field.stringValue), o);
    if (!q.usable()) {
        NSBeep();
        [self setStatus:SNS(q.error()) error:YES];
        return NO;
    }
    return YES;
}

- (void)replaceAll:(id)sender {
    if (![self readyToReplace]) return;
    _replacing = YES;
    [self setStatus:@"Counting matches…" error:NO];
    const std::string root = _scope.fileSystemRepresentation;
    const std::string q = SStd(self.field.stringValue);
    const FolderSearchOptions o = [self options];
    NSString *with = self.replaceField.stringValue;
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        FolderReplacePlan plan = FolderSearch::planReplace(root, q, nullptr, o);
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        NSMutableArray<NSNumber *> *counts = [NSMutableArray array];
        NSMutableSet<NSString *> *outside = [NSMutableSet set];
        for (const FolderReplaceFile &f : plan.files) {
            [paths addObject:SNS(f.path)];
            [counts addObject:@(f.matches)];
            if (f.outside) [outside addObject:SNS(f.path)];
        }
        NSString *error = plan.error.empty() ? nil : SNS(plan.error);
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s) return;
            // Files whose tabs hold changes the disk lacks are left out of
            // the count, and named.
            NSMutableArray<NSString *> *todo = [NSMutableArray array], *held = [NSMutableArray array];
            NSMutableArray<NSString *> *away = [NSMutableArray array];
            size_t matches = 0;
            for (NSUInteger i = 0; i < paths.count; i++) {
                if (s.openState && s.openState(paths[i]) == MCOpenFileConflict) {
                    [held addObject:paths[i].lastPathComponent];
                    continue;
                }
                [todo addObject:paths[i]];
                matches += counts[i].unsignedLongValue;
                if ([outside containsObject:paths[i]])
                    [away addObject:[paths[i] substringFromIndex:MIN(paths[i].length,
                                                                     s->_scope.length + 1)]];
            }
            if (error || todo.count == 0) {
                s->_replacing = NO;
                NSString *why = error ?: held.count
                    ? [NSString stringWithFormat:@"Nothing replaced: the matches are in tabs with "
                                                  "unsaved changes (%@)",
                                                 [held componentsJoinedByString:@", "]]
                    : @"Nothing to replace";
                [s setStatus:why error:error != nil];
                return;
            }
            NSAlert *alert = [NSAlert new];
            alert.messageText = [NSString stringWithFormat:@"Replace %zu %@ in %lu %@?", matches,
                                 matches == 1 ? @"match" : @"matches", (unsigned long)todo.count,
                                 todo.count == 1 ? @"file" : @"files"];
            NSMutableString *info = [NSMutableString stringWithFormat:
                @"Each becomes “%@”. Files open in a tab are changed there, unsaved, and can be "
                 "undone there; the others are written to disk.", with];
            if (away.count)
                [info appendFormat:@" Through symbolic links, these are files outside the folder, "
                                    "and they are changed too: %@.",
                                   [away componentsJoinedByString:@", "]];
            if (held.count)
                [info appendFormat:@" Left alone, since their tabs have unsaved changes: %@.",
                                   [held componentsJoinedByString:@", "]];
            alert.informativeText = info;
            [alert addButtonWithTitle:@"Replace"];
            [alert addButtonWithTitle:@"Cancel"];
            if ([alert runModal] != NSAlertFirstButtonReturn) {
                s->_replacing = NO;
                [s setStatus:@"Nothing replaced" error:NO];
                return;
            }
            [s replaceInPaths:todo hit:nil held:held];
        });
    });
}

- (void)replaceFileAtRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_hits.count || ![self readyToReplace]) return;
    _replacing = YES;
    [self replaceInPaths:@[_hits[(NSUInteger)row].path] hit:nil held:@[]];
}

- (void)replaceMatchAtRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_hits.count || ![self readyToReplace]) return;
    _replacing = YES;
    SearchHit *h = _hits[(NSUInteger)row];
    [self replaceInPaths:@[h.path] hit:h held:@[]];
}

// Replace every match in these files, or only `hit`'s. Tabs first (on the
// main thread, through the editor), then the other files on disk, off it.
// `held` names files already left out for their unsaved tabs.
- (void)replaceInPaths:(NSArray<NSString *> *)paths hit:(SearchHit *)hit
                  held:(NSArray<NSString *> *)held {
    const FolderSearchOptions o = [self options];
    const std::string q = SStd(self.field.stringValue);
    NSString *with = self.replaceField.stringValue;
    const TextFind::Query query = FolderSearch::makeQuery(q, o);
    const std::u16string with16 = SU16(with);

    NSMutableArray<NSString *> *tabPaths = [NSMutableArray array], *diskPaths = [NSMutableArray array];
    NSMutableArray<NSString *> *skipped = [held mutableCopy], *problems = [NSMutableArray array];
    NSMutableArray<NSArray<NSValue *> *> *tabRanges = [NSMutableArray array];
    NSMutableArray<NSArray<NSString *> *> *tabStrings = [NSMutableArray array];
    size_t tabMatches = 0;
    // A row's match: only while its line still reads as it did.
    const std::string expected = hit ? SStd(hit.text) : std::string();
    // One file under two names (a symlink, a hard link) is replaced once.
    std::set<std::pair<uint64_t, uint64_t>> seen;
    for (NSString *path in paths) {
        const FolderSearch::FileStamp st = FolderSearch::stampOf(path.fileSystemRepresentation);
        if (st.exists && !seen.insert({st.device, st.inode}).second) continue;
        MCOpenFileState state = self.openState ? self.openState(path) : MCOpenFileNone;
        if (state == MCOpenFileConflict) {
            [skipped addObject:path.lastPathComponent];
            continue;
        }
        NSString *text = state == MCOpenFileHere && self.tabText ? self.tabText(path) : nil;
        if (!text) {
            [diskPaths addObject:path];
            continue;
        }
        const std::u16string u = SU16(text);
        std::vector<TextFind::Edit16> edits;
        if (hit) {
            TextFind::Edit16 e;
            std::string now;
            if (FolderSearch::lineAt(SStd(text), (int)hit.line, &now) &&
                FolderSearch::displayText(now, o.maxLineChars) == expected &&
                TextFind::replaceOneAt(u, query, with16, (int)hit.line, hit.byteColumn, &e))
                edits.push_back(e);
        } else {
            edits = TextFind::replaceAll(u, query, with16);
        }
        if (edits.empty()) continue;
        NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
        NSMutableArray<NSString *> *strings = [NSMutableArray array];
        for (const TextFind::Edit16 &e : edits) {
            [ranges addObject:[NSValue valueWithRange:NSMakeRange(e.start, e.length)]];
            [strings addObject:SFromU16(e.text)];
        }
        [tabPaths addObject:path];
        [tabRanges addObject:ranges];
        [tabStrings addObject:strings];
        tabMatches += edits.size();
    }
    size_t tabFiles = 0;
    if (tabPaths.count) {
        NSString *failure = nil;
        NSInteger n = self.editTabs ? self.editTabs(tabPaths, tabRanges, tabStrings, &failure) : -1;
        if (n < 0) {
            [problems addObject:failure ?: @"the open tabs could not be changed"];
            tabMatches = 0;
        } else {
            tabFiles = (size_t)n;
        }
    }

    const int line = hit ? (int)hit.line : 0;
    const size_t byteColumn = hit ? hit.byteColumn : 0;
    const std::string with8 = SStd(with);
    const BOOL one = hit != nil;
    __weak SearchPanel *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        size_t files = 0, matches = 0;
        NSMutableArray<NSString *> *failed = [NSMutableArray array];
        for (NSString *path in diskPaths) {
            const std::string p = path.fileSystemRepresentation;
            std::string content, out, error;
            if (!FolderSearch::readTextFile(p, FolderSearchOptions().maxFileSize, &content)) {
                [failed addObject:[NSString stringWithFormat:@"%@ could not be read as text",
                                                             path.lastPathComponent]];
                continue;
            }
            size_t n = 0;
            if (one) {
                if (FolderSearch::replaceMatchInText(content, q, with8, o, line, byteColumn, &out,
                                                     &expected))
                    n = 1;
            } else {
                out = FolderSearch::replaceInText(content, q, with8, o, &n);
            }
            if (n == 0 || out == content) continue;
            if (!FolderSearch::writeFileAtomically(p, out, &error)) {
                [failed addObject:[NSString stringWithFormat:@"%@: %s", path.lastPathComponent,
                                                             error.c_str()]];
                continue;
            }
            files++;
            matches += n;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            SearchPanel *s = weakSelf;
            if (!s) return;
            s->_replacing = NO;
            [problems addObjectsFromArray:failed];
            const size_t total = matches + tabMatches, nfiles = files + tabFiles;
            NSMutableString *msg = [NSMutableString string];
            if (total == 0 && !skipped.count && !problems.count)
                [msg appendString:one ? @"That match is no longer there" : @"Nothing to replace"];
            else
                [msg appendFormat:@"Replaced %zu %@ in %zu %@", total,
                                  total == 1 ? @"match" : @"matches", nfiles,
                                  nfiles == 1 ? @"file" : @"files"];
            if (tabFiles)
                [msg appendFormat:@" (%zu open in tabs, unsaved)", tabFiles];
            if (skipped.count)
                [msg appendFormat:@". Left alone, with unsaved changes: %@",
                                  [skipped componentsJoinedByString:@", "]];
            if (problems.count)
                [msg appendFormat:@". %@", [problems componentsJoinedByString:@"; "]];
            // The results as they are now, after this note.
            s->_afterNote = msg;
            s->_afterNoteError = problems.count > 0;
            [s runSearch:s];
            if (!s.busy) [s setStatus:msg error:s->_afterNoteError];
        });
    });
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
    NSInteger row = [self rowForAction];
    if (row < 0) return;
    SearchHit *h = _hits[row];
    if (_open) _open(h.path, h.line);
}

@end
