// PalettePanel.mm — see PalettePanel.h. Objective-C++.
//
// The palette is a view laid over the window's content (not a window of its
// own), so the editor window stays key: the commands are validated, and
// later run, against the same responder chain the menu bar would use. The
// overlay covers the whole content view and takes clicks outside the box,
// which close it.
#import "PalettePanel.h"
#import "AppSettings.h"
#import "TreeDetails.h"   // MCAppDefaults
#import "Keymap.h"
#include "CommandPalette.h"

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

static NSString *const kPaletteRecentKey = @"MCCommandPaletteRecent";
static NSString *const kQuickOpenRecentKey = @"MCQuickOpenRecent";
static const CGFloat kPaletteRowHeight = 26;
static const NSInteger kPaletteMaxRows = 12;
static const size_t kQuickOpenMaxFiles = 20000;

static NSColor *PHex(unsigned rgb, CGFloat alpha = 1) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:alpha];
}
static NSColor *PAccent(void) { return PHex(0x4EA1F7); }
static NSColor *PMuted(void) { return PHex(0x9CA3AF); }

static std::u16string PU16(NSString *s) {
    std::u16string out(s.length, u'\0');
    if (s.length) [s getCharacters:(unichar *)out.data() range:NSMakeRange(0, s.length)];
    return out;
}
static NSString *PNS(const std::u16string &s) {
    return [NSString stringWithCharacters:(const unichar *)s.data() length:s.size()];
}

@implementation MCPaletteContext
@end
@implementation MCPalettePick
@end

// ------------------------------------------------------------ the commands
@interface MCPaletteCommand : NSObject
@property(nonatomic, strong) NSMenuItem *item;
@property(nonatomic, copy) NSString *label;   // "View > Toggle Terminal"
@property(nonatomic, copy) NSString *title;   // "Toggle Terminal"
@property(nonatomic, copy) NSString *keys;    // "⇧⌘T"
@end
@implementation MCPaletteCommand
@end

static unsigned PMods(NSEventModifierFlags m) {
    unsigned out = 0;
    if (m & NSEventModifierFlagShift) out |= CommandPalette::Shift;
    if (m & NSEventModifierFlagControl) out |= CommandPalette::Control;
    if (m & NSEventModifierFlagOption) out |= CommandPalette::Option;
    if (m & NSEventModifierFlagCommand) out |= CommandPalette::Command;
    return out;
}

// Every enabled command below menu, validated now (NSMenu update asks each
// item's target, found through the key window's responder chain, as the
// menu bar does before it opens). Separators, hidden items (the second keys
// of Next Tab and the like), alternates and the palette itself are left out.
static void PCollect(NSMenu *menu, NSString *path, NSMutableArray<MCPaletteCommand *> *out) {
    // A menu filled as it opens (the Bookmarks menu) fills now.
    if ([menu.delegate respondsToSelector:@selector(menuNeedsUpdate:)])
        [menu.delegate menuNeedsUpdate:menu];
    [menu update];
    for (NSMenuItem *it in menu.itemArray) {
        if (it.isSeparatorItem || it.isHidden || it.isAlternate) continue;
        if (it.hasSubmenu) {
            NSString *name;
            if (!path) {
                // The app menu has no title of its own (its item's is the
                // placeholder "NSMenuItem").
                name = it.submenu.title.length ? it.submenu.title : @"minicode";
            } else {
                name = [NSString stringWithFormat:@"%@ > %@", path, it.title];
            }
            PCollect(it.submenu, name, out);
            continue;
        }
        if (!path || !it.action || it.action == NSSelectorFromString(@"showCommandPalette:"))
            continue;
        if (!it.isEnabled) continue;
        MCPaletteCommand *c = [MCPaletteCommand new];
        c.item = it;
        c.title = it.title;
        c.label = [NSString stringWithFormat:@"%@ > %@", path, it.title];
        // The command's first key in effect (keys.conf), which may be one
        // no menu can show (Cmd+keypad 0); the item's own for the rest.
        c.keys = MCKeymapGlyphsForMenuItem(it)
                     ?: PNS(CommandPalette::keyLabel(PU16(it.keyEquivalent),
                                                     PMods(it.keyEquivalentModifierMask)));
        [out addObject:c];
    }
}

// The registry's commands that no menu item runs (Keymap.h), when the
// responder chain has someone to take them now.
static void PCollectUnmenued(NSMutableArray<MCPaletteCommand *> *out) {
    for (NSMenuItem *it in MCKeymapCommandsWithoutMenuItems()) {
        id target = [NSApp targetForAction:it.action to:nil from:it];
        if (!target) continue;
        if ([target respondsToSelector:@selector(validateMenuItem:)] && ![target validateMenuItem:it])
            continue;
        MCPaletteCommand *c = [MCPaletteCommand new];
        c.item = it;
        c.title = it.title;
        c.label = [NSString stringWithFormat:@"%@ > %@", it.representedObject, it.title];
        c.keys = MCKeymapGlyphsForMenuItem(it) ?: @"";
        [out addObject:c];
    }
}

static NSArray<NSString *> *PStrings(NSString *key) {
    NSMutableArray *out = [NSMutableArray array];
    for (id s in [MCAppDefaults() arrayForKey:key])
        if ([s isKindOfClass:NSString.class]) [out addObject:s];
    return out;
}

// Most recent first, once each, at most cap.
static void PNote(NSString *key, NSString *value, NSUInteger cap) {
    std::vector<std::string> r;
    for (NSString *s in PStrings(key)) r.push_back(s.UTF8String);
    r = CommandPalette::noteRun(r, value.UTF8String, cap);
    NSMutableArray *a = [NSMutableArray array];
    for (const std::string &s : r) [a addObject:@(s.c_str())];
    [MCAppDefaults() setObject:a forKey:key];
}

// ------------------------------------------------------------------ a row
// What the list shows: a label (its matched characters marked), the keys
// at the right, and what Return does.
@interface MCPaletteItem : NSObject
@property(nonatomic, copy) NSString *label;
@property(nonatomic, strong) NSAttributedString *text;
@property(nonatomic, copy) NSString *keys;
@property(nonatomic, copy) void (^run)(void);   // nil: nothing to run
@property(nonatomic, copy) void (^preview)(void);   // when the list comes to it
@property(nonatomic) BOOL heading;   // a kind's heading in "@:": never selected
@end
@implementation MCPaletteItem
@end

@interface MCPaletteRow : NSView
@property(nonatomic, strong) NSAttributedString *text;
@property(nonatomic, copy) NSString *keys;
@property(nonatomic, strong) NSColor *keyColor;
@property(nonatomic) BOOL selected;
@end

@implementation MCPaletteRow
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    if (self.selected) {
        [[PAccent() colorWithAlphaComponent:0.28] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(b, 2, 1) xRadius:4 yRadius:4] fill];
    }
    CGFloat keysW = 0;
    if (self.keys.length) {
        NSDictionary *ka = @{
            NSFontAttributeName: [NSFont systemFontOfSize:12],
            NSForegroundColorAttributeName: self.keyColor ?: PMuted(),
        };
        NSSize ks = [self.keys sizeWithAttributes:ka];
        keysW = ceil(ks.width) + 16;
        [self.keys drawAtPoint:NSMakePoint(NSMaxX(b) - ks.width - 12,
                                           floor((b.size.height - ks.height) / 2))
                withAttributes:ka];
    }
    NSRect tr = NSMakeRect(12, 0, MAX(0, b.size.width - 24 - keysW), b.size.height);
    NSSize ts = [self.text size];
    tr.origin.y = floor((b.size.height - ts.height) / 2);
    tr.size.height = ts.height;
    [self.text drawWithRect:tr
                    options:NSStringDrawingUsesLineFragmentOrigin |
                            NSStringDrawingTruncatesLastVisibleLine];
}
@end

// A list that never takes the keyboard: the field keeps it while rows are
// clicked.
@interface MCPaletteTable : NSTableView
@end
@implementation MCPaletteTable
- (BOOL)acceptsFirstResponder { return NO; }
@end

// --------------------------------------------------------------- the overlay
@interface MCPaletteOverlay : NSView <NSTextFieldDelegate, NSTableViewDataSource,
                                      NSTableViewDelegate>
@property(nonatomic, strong) NSArray<MCPaletteCommand *> *commands;
@property(nonatomic, strong) MCPaletteContext *context;
@property(nonatomic, strong) NSArray<NSString *> *files;   // relative; nil until listed
@property(nonatomic) BOOL filesTruncated;
@property(nonatomic, strong) NSArray<MCPaletteItem *> *items;
@property(nonatomic, weak) NSResponder *restore;
@property(nonatomic) NSRange restoreSelection;   // in a text field's field editor
@property(nonatomic, strong) NSView *box;
@property(nonatomic, strong) NSView *fieldBack;
@property(nonatomic, strong) NSTextField *field;
@property(nonatomic, strong) NSScrollView *scroll;
@property(nonatomic, strong) MCPaletteTable *table;
@property(nonatomic, strong) NSTextField *emptyLabel;
@property(nonatomic) NSInteger selected;
@property(nonatomic) BOOL closing;
@property(nonatomic, strong) NSColor *textColor, *mutedColor;
// Go to Symbol: the file's symbols once they arrive, the one at the caret,
// whether one was shown in the editor, and whether a row was run.
@property(nonatomic) BOOL askedSymbols;
@property(nonatomic, strong) NSArray<MCSymbolItem *> *symbols;
@property(nonatomic) NSInteger caretSymbol;
@property(nonatomic, copy) NSString *symbolSource;
@property(nonatomic) BOOL previewed;
@property(nonatomic) BOOL ran;
// Go to Symbol in Workspace: the last answer, the query it was for and the
// one asked now.
@property(nonatomic, strong) NSArray<MCSymbolItem *> *workspaceItems;
@property(nonatomic, copy) NSString *workspaceAsked;
@property(nonatomic) BOOL workspaceComplete;
@property(nonatomic, copy) NSString *workspaceNote;
@end

@implementation MCPaletteOverlay {
    std::vector<CommandPalette::Entry> _entries;
    std::vector<std::u16string> _fileKeys;
    std::shared_ptr<std::atomic<bool>> _cancelListing;
    std::vector<Lsp::Symbol> _symbolKeys, _workspaceKeys;   // names and kinds, for ranking
    NSInteger _preselect;   // the row "@" selects first
}

- (instancetype)initWithFrame:(NSRect)frame commands:(NSArray<MCPaletteCommand *> *)commands
                      context:(MCPaletteContext *)context {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.commands = commands;
    self.context = context;
    for (MCPaletteCommand *c in commands) {
        CommandPalette::Entry e;
        e.label = PU16(c.label);
        e.title = PU16(c.title);
        e.id = c.label.UTF8String;
        _entries.push_back(e);
    }

    AppSettings *cfg = [AppSettings shared];
    NSColor *boxColor = [[cfg background:Surface::Sidebar] colorWithAlphaComponent:1];
    NSColor *fieldColor = [[cfg background:Surface::Editor] colorWithAlphaComponent:1];
    self.textColor = [cfg text:Surface::Sidebar];
    self.mutedColor = PMuted();

    self.box = [[NSView alloc] initWithFrame:NSZeroRect];
    self.box.wantsLayer = YES;
    self.box.layer.backgroundColor = boxColor.CGColor;
    self.box.layer.cornerRadius = 7;
    self.box.layer.borderWidth = 1;
    self.box.layer.borderColor = PHex(0x454545).CGColor;
    self.box.layer.shadowColor = NSColor.blackColor.CGColor;
    self.box.layer.shadowOpacity = 0.45;
    self.box.layer.shadowRadius = 14;
    self.box.layer.shadowOffset = CGSizeMake(0, -4);
    [self addSubview:self.box];

    self.fieldBack = [[NSView alloc] initWithFrame:NSZeroRect];
    self.fieldBack.wantsLayer = YES;
    self.fieldBack.layer.backgroundColor = fieldColor.CGColor;
    self.fieldBack.layer.cornerRadius = 4;
    self.fieldBack.layer.borderWidth = 1;
    self.fieldBack.layer.borderColor = PAccent().CGColor;
    [self.box addSubview:self.fieldBack];

    NSTextField *f = [[NSTextField alloc] initWithFrame:NSZeroRect];
    f.bezeled = NO;
    f.bordered = NO;
    f.drawsBackground = NO;
    f.focusRingType = NSFocusRingTypeNone;
    f.font = [NSFont systemFontOfSize:14];
    f.textColor = [cfg text:Surface::Editor];
    f.usesSingleLineMode = YES;
    f.cell.scrollable = YES;
    f.placeholderAttributedString = [[NSAttributedString alloc]
        initWithString:context.pick.placeholder ?:
                           @"Search files by name (> commands, : a line, @ a symbol, # in the workspace)"
            attributes:@{NSForegroundColorAttributeName: self.mutedColor,
                         NSFontAttributeName: f.font}];
    f.delegate = self;
    self.field = f;
    [self.fieldBack addSubview:f];

    MCPaletteTable *t = [[MCPaletteTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"item"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [t addTableColumn:col];
    t.headerView = nil;
    t.rowHeight = kPaletteRowHeight;
    t.intercellSpacing = NSZeroSize;
    t.backgroundColor = NSColor.clearColor;
    t.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;
    t.focusRingType = NSFocusRingTypeNone;
    t.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    if (@available(macOS 11.0, *)) t.style = NSTableViewStylePlain;
    t.dataSource = self;
    t.delegate = self;
    t.target = self;
    t.action = @selector(rowClicked:);
    self.table = t;

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    sv.drawsBackground = NO;
    sv.hasVerticalScroller = YES;
    sv.autohidesScrollers = YES;
    sv.scrollerStyle = NSScrollerStyleOverlay;
    sv.borderType = NSNoBorder;
    sv.documentView = t;
    sv.contentView.drawsBackground = NO;
    self.scroll = sv;
    [self.box addSubview:sv];

    self.emptyLabel = [NSTextField labelWithString:@""];
    self.emptyLabel.textColor = self.mutedColor;
    self.emptyLabel.font = [NSFont systemFontOfSize:13];
    self.emptyLabel.hidden = YES;
    [self.box addSubview:self.emptyLabel];
    return self;
}

// The folder's files, listed on a background queue; the list fills in when
// they arrive (or never, if the palette closed first, which stops the walk).
- (void)listFiles {
    NSString *root = self.context.root;
    if (!root.length || self.context.pick) {
        self.files = @[];
        return;
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    _cancelListing = cancel;
    __weak MCPaletteOverlay *weakSelf = self;
    const std::string rootUtf8 = root.fileSystemRepresentation;
    const bool useIgnore = [AppSettings shared].settings.searchUseIgnoreFiles();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        bool truncated = false;
        std::vector<std::string> found = CommandPalette::listFiles(
            rootUtf8, kQuickOpenMaxFiles, cancel.get(), &truncated, useIgnore);
        if (cancel->load()) return;
        NSMutableArray<NSString *> *files = [NSMutableArray arrayWithCapacity:found.size()];
        for (const std::string &rel : found) {
            NSString *s = [NSString stringWithUTF8String:rel.c_str()];
            if (s) [files addObject:s];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            MCPaletteOverlay *me = weakSelf;
            if (!me || me.closing) return;
            me.filesTruncated = truncated;
            [me setListedFiles:files];
        });
    });
}

- (void)setListedFiles:(NSArray<NSString *> *)files {
    self.files = files;
    _fileKeys.clear();
    _fileKeys.reserve(files.count);
    for (NSString *f in files) _fileKeys.push_back(PU16(f));
    if (CommandPalette::parse(PU16(self.field.stringValue ?: @"")).mode ==
        CommandPalette::Mode::Files)
        [self refilter];
}

- (void)setFrameSize:(NSSize)s {
    [super setFrameSize:s];
    [self relayout];
}

// Top centre, below the title bar, as wide as VS Code's (600 points at
// most), as tall as its rows up to twelve.
- (void)relayout {
    NSRect b = self.bounds;
    NSWindow *w = self.window;
    CGFloat titleH = 28;
    if (w && !NSIsEmptyRect(w.contentLayoutRect)) {
        CGFloat t = NSMaxY(b) - NSMaxY(w.contentLayoutRect);
        if (t >= 0 && t < 100) titleH = t;
    }
    const CGFloat boxW = floor(MIN(600, MAX(200, b.size.width - 40)));
    const NSInteger rows = MAX(1, MIN((NSInteger)self.items.count, kPaletteMaxRows));
    const CGFloat pad = 6, fieldH = 30, listH = rows * kPaletteRowHeight;
    const CGFloat boxH = pad + fieldH + pad + listH + pad;
    const CGFloat top = NSMaxY(b) - titleH - 8;
    self.box.frame = NSMakeRect(floor((b.size.width - boxW) / 2), floor(top - boxH), boxW, boxH);
    self.fieldBack.frame = NSMakeRect(pad, boxH - pad - fieldH, boxW - 2 * pad, fieldH);
    NSSize fs = [self.field.cell cellSizeForBounds:NSMakeRect(0, 0, 1000, 1000)];
    self.field.frame = NSMakeRect(8, floor((fieldH - fs.height) / 2),
                                  boxW - 2 * pad - 16, ceil(fs.height));
    self.scroll.frame = NSMakeRect(pad - 2, pad, boxW - 2 * pad + 4, listH);
    self.table.tableColumns.firstObject.width = self.scroll.contentSize.width;
    [self.emptyLabel sizeToFit];
    NSSize es = self.emptyLabel.frame.size;
    es.width = MIN(es.width, boxW - 2 * pad - 24);
    self.emptyLabel.frame = NSMakeRect(pad + 12, pad + floor((listH - es.height) / 2),
                                       es.width, es.height);
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) [self relayout];
}

// A click outside the box closes the palette (and goes no further).
- (void)mouseDown:(NSEvent *)e {
    (void)e;
    [self closeRestoringFocus:YES];
}
- (void)rightMouseDown:(NSEvent *)e { [self mouseDown:e]; }

// ------------------------------------------------------------- the items
- (void)refilter {
    using namespace CommandPalette;
    Parsed p = parse(PU16(self.field.stringValue ?: @""));
    if (self.context.pick) p.mode = Mode::Files;   // a pick list has no prefixes
    NSMutableArray<MCPaletteItem *> *items = [NSMutableArray array];
    NSString *empty = @"";
    __weak MCPaletteOverlay *weakSelf = self;
    if (self.context.pick) {
        empty = [self pickItems:items];
    } else if (p.mode == Mode::Commands) {
        std::vector<std::string> recent;
        for (NSString *s in PStrings(kPaletteRecentKey)) recent.push_back(s.UTF8String);
        for (const Match &m : rank(_entries, p.query, recent)) {
            MCPaletteCommand *c = self.commands[m.index];
            MCPaletteItem *it = [MCPaletteItem new];
            it.label = c.label;
            it.text = [self attributed:c.label title:c.title match:m];
            it.keys = c.keys;
            NSMenuItem *menuItem = c.item;
            NSString *label = c.label;
            it.run = ^{
                PNote(kPaletteRecentKey, label, 20);
                [weakSelf closeRestoringFocus:YES];
                // On the next turn of the run loop, out of the field's own
                // key handling, with the keyboard back where it was, so the
                // command acts on what had it, as from the menu bar.
                dispatch_async(dispatch_get_main_queue(), ^{
                    [NSApp sendAction:menuItem.action to:menuItem.target from:menuItem];
                });
            };
            [items addObject:it];
        }
        if (!items.count) empty = @"No matching commands";
    } else if (p.mode == Mode::Line) {
        void (^go)(NSInteger, NSInteger) = self.context.goToLine;
        MCPaletteItem *it = [MCPaletteItem new];
        if (!go) {
            it.label = @"Open a file to go to one of its lines";
        } else if (!p.lineValid) {
            it.label = @"Type a line number, and a column after a colon if you like";
        } else if (p.line <= 0) {
            it.label = @"Type a line number to go to";
        } else {
            it.label = p.column > 0
                ? [NSString stringWithFormat:@"Go to line %d, column %d", p.line, p.column]
                : [NSString stringWithFormat:@"Go to line %d", p.line];
            const NSInteger line = p.line, column = p.column;
            it.run = ^{
                [weakSelf closeRestoringFocus:YES];
                dispatch_async(dispatch_get_main_queue(), ^{ go(line, column); });
            };
        }
        it.text = [[NSAttributedString alloc]
            initWithString:it.label
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13],
                             NSForegroundColorAttributeName:
                                 it.run ? self.textColor : self.mutedColor}];
        [items addObject:it];
    } else if (p.mode == Mode::Symbols) {
        empty = [self symbolItems:items query:p.query byKind:p.byKind];
    } else if (p.mode == Mode::WorkspaceSymbols) {
        empty = [self workspaceItems:items query:p.query];
    } else if (!self.files) {
        empty = @"Listing the folder's files…";
    } else {
        NSString *root = self.context.root;
        NSString *prefix = [root hasSuffix:@"/"] ? root : [root stringByAppendingString:@"/"];
        std::vector<std::u16string> recent;
        NSMutableArray<NSString *> *recentAbs = [NSMutableArray array];
        [recentAbs addObjectsFromArray:self.context.recentFiles ?: @[]];
        [recentAbs addObjectsFromArray:PStrings(kQuickOpenRecentKey)];
        // Compared with symlinks resolved, as /tmp and /private/tmp are one.
        NSString *resolvedRoot = root.stringByResolvingSymlinksInPath;
        NSString *resolvedPrefix = [resolvedRoot hasSuffix:@"/"]
            ? resolvedRoot : [resolvedRoot stringByAppendingString:@"/"];
        for (NSString *a in recentAbs) {
            if ([a hasPrefix:prefix]) {
                recent.push_back(PU16([a substringFromIndex:prefix.length]));
                continue;
            }
            NSString *r = a.stringByResolvingSymlinksInPath;
            if ([r hasPrefix:resolvedPrefix])
                recent.push_back(PU16([r substringFromIndex:resolvedPrefix.length]));
        }
        std::set<std::u16string> recentSet(recent.begin(), recent.end());
        for (const Match &m : rankFiles(_fileKeys, p.query, recent)) {
            NSString *rel = self.files[m.index];
            MCPaletteItem *it = [MCPaletteItem new];
            it.label = rel;
            it.text = [self attributed:rel title:rel.lastPathComponent match:m];
            if (recentSet.count(_fileKeys[m.index])) it.keys = @"recently opened";
            NSString *abs = [prefix stringByAppendingString:rel];
            void (^open)(NSString *) = self.context.openFile;
            it.run = ^{
                PNote(kQuickOpenRecentKey, abs, 50);
                [weakSelf closeRestoringFocus:YES];
                if (open) dispatch_async(dispatch_get_main_queue(), ^{ open(abs); });
            };
            [items addObject:it];
        }
        if (!items.count)
            empty = self.files.count ? @"No matching files" : @"No files in this folder";
    }
    // Out of "@", the editor goes back to where it was.
    if (p.mode != Mode::Symbols && self.previewed) {
        self.previewed = NO;
        if (self.context.endPreview) self.context.endPreview();
    }
    if (p.mode != Mode::WorkspaceSymbols && self.workspaceAsked) {
        self.workspaceAsked = nil;
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(askWorkspace:)
                                                   object:nil];
        if (self.context.cancelWorkspaceSymbols) self.context.cancelWorkspaceSymbols();
    }
    NSInteger first = -1;
    for (NSUInteger i = 0; i < items.count && first < 0; i++)
        if (!items[i].heading) first = (NSInteger)i;
    if (p.mode == Mode::Symbols && _preselect >= 0 && _preselect < (NSInteger)items.count)
        first = _preselect;
    self.items = items;
    self.selected = first;
    [self.table reloadData];
    self.emptyLabel.stringValue = empty;
    self.emptyLabel.hidden = items.count > 0;
    [self relayout];
    if (self.selected >= 0) [self.table scrollRowToVisible:self.selected];
    // Typing in "@" shows the best match in the editor, as VS Code does.
    if (p.mode == Mode::Symbols && !p.query.empty() && self.selected >= 0 &&
        self.items[(NSUInteger)self.selected].preview)
        self.items[(NSUInteger)self.selected].preview();
}

// ------------------------------------------------------------- pick lists
// The caller's rows that match what is typed, best first, then the row made
// from the typed text, if any. Returns what to say when there are none.
- (NSString *)pickItems:(NSMutableArray<MCPaletteItem *> *)items {
    MCPalettePick *pick = self.context.pick;
    NSString *text = self.field.stringValue ?: @"";
    std::vector<CommandPalette::Entry> entries;
    for (NSString *label in pick.labels) {
        CommandPalette::Entry e;
        e.label = e.title = PU16(label);
        e.id = label.UTF8String ?: "";
        entries.push_back(e);
    }
    std::u16string query = PU16([text stringByTrimmingCharactersInSet:
                                          NSCharacterSet.whitespaceCharacterSet]);
    __weak MCPaletteOverlay *weakSelf = self;
    for (const CommandPalette::Match &m : CommandPalette::rank(entries, query, {})) {
        const NSInteger index = (NSInteger)m.index;
        MCPaletteItem *it = [MCPaletteItem new];
        it.label = pick.labels[(NSUInteger)index];
        it.text = [self attributed:it.label title:it.label match:m];
        if ((NSUInteger)index < pick.details.count) it.keys = pick.details[(NSUInteger)index];
        void (^choose)(NSInteger) = pick.choose;
        it.run = ^{
            [weakSelf closeRestoringFocus:YES];
            if (choose) dispatch_async(dispatch_get_main_queue(), ^{ choose(index); });
        };
        [items addObject:it];
    }
    NSString *typed = pick.typedRow ? pick.typedRow(text) : nil;
    if (typed.length) {
        MCPaletteItem *it = [MCPaletteItem new];
        it.label = typed;
        it.text = [[NSAttributedString alloc]
            initWithString:[@"+ " stringByAppendingString:typed]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13],
                             NSForegroundColorAttributeName: self.textColor}];
        void (^chooseTyped)(NSString *) = pick.chooseTyped;
        NSString *given = [text copy];
        it.run = ^{
            [weakSelf closeRestoringFocus:YES];
            if (chooseTyped) dispatch_async(dispatch_get_main_queue(), ^{ chooseTyped(given); });
        };
        [items addObject:it];
    }
    return pick.emptyText ?: @"Nothing matches";
}

// ------------------------------------------------------------- symbols
// A symbol's row: its kind's mark in the kind's color, the name with the
// matched characters marked, and what contains it, muted.
- (NSAttributedString *)symbolText:(MCSymbolItem *)sym match:(const CommandPalette::Match &)m
                             extra:(NSString *)extra {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    // The mark in a box of fixed width, so the names line up.
    ps.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:20
                                                     options:@{}]];
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    const unsigned rgb = Lsp::symbolKindColor((int)sym.kind);
    NSString *glyph = PNS(Lsp::symbolKindGlyph((int)sym.kind));
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:[glyph stringByAppendingString:@"\t"]
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13
                                                                weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: PHex(rgb),
                         NSParagraphStyleAttributeName: ps}]];
    const NSUInteger nameAt = s.length;
    NSString *name = sym.name ?: @"";
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:name
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:13],
                         NSForegroundColorAttributeName: self.textColor,
                         NSParagraphStyleAttributeName: ps}]];
    NSFont *bold = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    for (size_t pos : m.positions) {
        if (pos >= name.length) continue;
        [s addAttributes:@{NSFontAttributeName: bold, NSForegroundColorAttributeName: PAccent()}
                   range:NSMakeRange(nameAt + pos, 1)];
    }
    if (extra.length)
        [s appendAttributedString:[[NSAttributedString alloc]
            initWithString:[@"   " stringByAppendingString:extra]
                attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:12],
                             NSForegroundColorAttributeName: self.mutedColor,
                             NSParagraphStyleAttributeName: ps}]];
    return s;
}

static NSString *POneLine(NSString *s) {
    if (!s.length) return s;
    NSArray *parts = [s componentsSeparatedByCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSMutableArray *kept = [NSMutableArray array];
    for (NSString *p in parts) if (p.length) [kept addObject:p];
    return [kept componentsJoinedByString:@" "];
}

// Go to Symbol in File: the rows for "@" (or "@:", grouped by kind), the
// file's symbols asked for the first time. Returns what to say when there
// are none.
- (NSString *)symbolItems:(NSMutableArray<MCPaletteItem *> *)items
                    query:(const std::u16string &)query
                   byKind:(BOOL)byKind {
    _preselect = -1;
    if (!self.context.documentSymbols) return @"Open a file to go to one of its symbols";
    if (!self.askedSymbols) {
        self.askedSymbols = YES;
        __weak MCPaletteOverlay *weakSelf = self;
        self.context.documentSymbols(^(NSArray<MCSymbolItem *> *got, NSInteger caretIndex,
                                       NSString *source) {
            // Never in the middle of a refilter, even when it answers at once.
            dispatch_async(dispatch_get_main_queue(), ^{
                MCPaletteOverlay *me = weakSelf;
                if (!me || me.closing) return;
                [me setDocumentSymbols:got ?: @[] caret:caretIndex source:source];
            });
        });
    }
    if (!self.symbols) return @"Listing the file's symbols…";
    int pre = -1;
    std::vector<CommandPalette::SymbolRow> rows = CommandPalette::symbolRows(
        _symbolKeys, query, byKind, (int)self.caretSymbol, &pre);
    __weak MCPaletteOverlay *weakSelf = self;
    void (^preview)(MCSymbolItem *) = self.context.previewSymbol;
    void (^go)(MCSymbolItem *) = self.context.goToSymbol;
    for (const CommandPalette::SymbolRow &r : rows) {
        MCPaletteItem *it = [MCPaletteItem new];
        if (r.heading) {
            it.heading = YES;
            it.label = PNS(r.text);
            it.text = [[NSAttributedString alloc]
                initWithString:it.label
                    attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11
                                                                        weight:NSFontWeightSemibold],
                                 NSForegroundColorAttributeName: self.mutedColor}];
            [items addObject:it];
            continue;
        }
        MCSymbolItem *sym = self.symbols[r.symbol];
        NSString *extra = sym.container.length ? sym.container : POneLine(sym.detail);
        it.label = sym.name;
        it.text = [self symbolText:sym match:r.match extra:extra];
        it.keys = [NSString stringWithFormat:@"%ld", (long)sym.line + 1];
        it.preview = ^{
            MCPaletteOverlay *me = weakSelf;
            if (!me || !preview) return;
            me.previewed = YES;
            preview(sym);
        };
        it.run = ^{
            MCPaletteOverlay *me = weakSelf;
            me.ran = YES;
            [me closeRestoringFocus:YES];
            if (go) dispatch_async(dispatch_get_main_queue(), ^{ go(sym); });
        };
        [items addObject:it];
    }
    _preselect = pre;
    if (items.count) return @"";
    if (!self.symbols.count)
        return [self.symbolSource isEqualToString:@"outline"]
            ? @"No symbols found in this file" : @"The language server lists no symbols here";
    return @"No matching symbols";
}

- (void)setDocumentSymbols:(NSArray<MCSymbolItem *> *)got caret:(NSInteger)caret
                    source:(NSString *)source {
    self.symbols = got;
    self.caretSymbol = caret;
    self.symbolSource = source;
    _symbolKeys.clear();
    for (MCSymbolItem *it in got) {
        Lsp::Symbol k;
        k.name = it.name.UTF8String ?: "";
        k.kind = (int)it.kind;
        _symbolKeys.push_back(k);
    }
    if (CommandPalette::parse(PU16(self.field.stringValue ?: @"")).mode ==
        CommandPalette::Mode::Symbols)
        [self refilter];
}

// Go to Symbol in Workspace: the last answer ranked against what is typed
// now, while the servers are asked again (a moment after typing stops).
- (NSString *)workspaceItems:(NSMutableArray<MCPaletteItem *> *)items
                       query:(const std::u16string &)query {
    if (!self.context.workspaceSymbols) return @"No language server is running in this window";
    NSString *q = PNS(query);
    if (!q.length) {
        self.workspaceAsked = nil;
        self.workspaceItems = nil;
        _workspaceKeys.clear();
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(askWorkspace:)
                                                   object:nil];
        if (self.context.cancelWorkspaceSymbols) self.context.cancelWorkspaceSymbols();
        return @"Type the name of a symbol to look for it in the workspace";
    }
    if (![q isEqualToString:self.workspaceAsked]) {
        self.workspaceAsked = q;
        self.workspaceComplete = NO;
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(askWorkspace:)
                                                   object:nil];
        [self performSelector:@selector(askWorkspace:) withObject:q afterDelay:0.15];
    }
    if (self.workspaceItems.count) {
        int pre = -1;
        std::vector<CommandPalette::SymbolRow> rows =
            CommandPalette::symbolRows(_workspaceKeys, query, false, -1, &pre, 300);
        NSString *root = self.context.root ?: @"";
        NSString *prefix = [root hasSuffix:@"/"] ? root : [root stringByAppendingString:@"/"];
        __weak MCPaletteOverlay *weakSelf = self;
        void (^open)(MCSymbolItem *) = self.context.openWorkspaceSymbol;
        for (const CommandPalette::SymbolRow &r : rows) {
            MCSymbolItem *sym = self.workspaceItems[r.symbol];
            MCPaletteItem *it = [MCPaletteItem new];
            it.label = sym.name;
            it.text = [self symbolText:sym match:r.match extra:sym.container];
            NSString *path = sym.path ?: @"";
            if (root.length && [path hasPrefix:prefix]) path = [path substringFromIndex:prefix.length];
            it.keys = [NSString stringWithFormat:@"%@:%ld", path, (long)sym.line + 1];
            it.run = ^{
                MCPaletteOverlay *me = weakSelf;
                me.ran = YES;
                [me closeRestoringFocus:YES];
                if (open) dispatch_async(dispatch_get_main_queue(), ^{ open(sym); });
            };
            [items addObject:it];
        }
    }
    if (items.count) return @"";
    if (!self.workspaceComplete) return @"Searching the workspace's symbols…";
    return self.workspaceNote ?: @"No matching symbols";
}

- (void)askWorkspace:(NSString *)query {
    if (self.closing || ![query isEqualToString:self.workspaceAsked]) return;
    __weak MCPaletteOverlay *weakSelf = self;
    self.context.workspaceSymbols(query, ^(NSArray<MCSymbolItem *> *got, BOOL complete,
                                           NSString *note) {
        MCPaletteOverlay *me = weakSelf;
        if (!me || me.closing || ![query isEqualToString:me.workspaceAsked]) return;
        // A partial answer with nothing in it leaves the last list up, so a
        // slow server never empties what another has filled.
        if (got.count || complete) {
            me.workspaceItems = got;
            me->_workspaceKeys.clear();
            for (MCSymbolItem *it in got) {
                Lsp::Symbol k;
                k.name = it.name.UTF8String ?: "";
                k.kind = (int)it.kind;
                me->_workspaceKeys.push_back(k);
            }
        }
        me.workspaceComplete = complete;
        me.workspaceNote = note;
        if (CommandPalette::parse(PU16(me.field.stringValue ?: @"")).mode ==
            CommandPalette::Mode::WorkspaceSymbols)
            [me refilter];
    });
}


// The part before the title muted (a command's menu, a file's folder), the
// title in the text color, the matched characters in the accent color and
// semibold.
- (NSAttributedString *)attributed:(NSString *)label title:(NSString *)title
                             match:(const CommandPalette::Match &)m {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingHead;
    NSFont *font = [NSFont systemFontOfSize:13];
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:label
            attributes:@{NSFontAttributeName: font,
                         NSForegroundColorAttributeName: self.textColor,
                         NSParagraphStyleAttributeName: ps}];
    const NSUInteger titleAt = label.length >= title.length ? label.length - title.length : 0;
    if (titleAt > 0)
        [s addAttribute:NSForegroundColorAttributeName value:self.mutedColor
                  range:NSMakeRange(0, titleAt)];
    NSFont *bold = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    for (size_t p : m.positions) {
        if (p >= label.length) continue;
        [s addAttributes:@{NSFontAttributeName: bold,
                           NSForegroundColorAttributeName: PAccent()}
                   range:NSMakeRange(p, 1)];
    }
    return s;
}

- (NSArray<NSString *> *)labels {
    NSMutableArray *a = [NSMutableArray array];
    for (MCPaletteItem *it in self.items) [a addObject:it.label];
    return a;
}

- (NSArray<NSString *> *)keyLabels {
    NSMutableArray *a = [NSMutableArray array];
    for (MCPaletteItem *it in self.items) [a addObject:it.keys ?: @""];
    return a;
}

- (void)select:(NSInteger)row {
    const NSInteger n = (NSInteger)self.items.count;
    if (n == 0) return;
    const NSInteger step = row < self.selected ? -1 : 1;
    row = ((row % n) + n) % n;   // wraps, as VS Code's list does
    // Over the headings of "@:", in the direction moved.
    for (NSInteger k = 0; k < n && self.items[(NSUInteger)row].heading; k++)
        row = ((row + step) % n + n) % n;
    if (self.items[(NSUInteger)row].heading) return;
    NSInteger old = self.selected;
    self.selected = row;
    NSMutableIndexSet *rows = [NSMutableIndexSet indexSetWithIndex:row];
    if (old >= 0 && old < n) [rows addIndex:old];
    [self.table reloadDataForRowIndexes:rows columnIndexes:[NSIndexSet indexSetWithIndex:0]];
    [self.table scrollRowToVisible:row];
    if (row != old && self.items[(NSUInteger)row].preview) self.items[(NSUInteger)row].preview();
}

// Page moves stop at the ends instead of wrapping.
- (void)page:(NSInteger)delta {
    const NSInteger n = (NSInteger)self.items.count;
    if (n == 0) return;
    [self select:MAX(0, MIN(n - 1, self.selected + delta))];
}

// ------------------------------------------------------------- the field
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv
    doCommandBySelector:(SEL)sel {
    (void)control; (void)tv;
    if (sel == @selector(moveUp:)) { [self select:self.selected - 1]; return YES; }
    if (sel == @selector(moveDown:)) { [self select:self.selected + 1]; return YES; }
    if (sel == @selector(scrollPageUp:) || sel == @selector(pageUp:)) {
        [self page:-(kPaletteMaxRows - 1)];
        return YES;
    }
    if (sel == @selector(scrollPageDown:) || sel == @selector(pageDown:)) {
        [self page:kPaletteMaxRows - 1];
        return YES;
    }
    if (sel == @selector(insertNewline:) || sel == @selector(insertLineBreak:)) {
        [self runRow:self.selected];
        return YES;
    }
    if (sel == @selector(cancelOperation:) || sel == @selector(complete:)) {
        [self closeRestoringFocus:YES];
        return YES;
    }
    // Tab would end editing and leave the palette open without the keyboard.
    if (sel == @selector(insertTab:) || sel == @selector(insertBacktab:)) return YES;
    return NO;
}

- (void)controlTextDidChange:(NSNotification *)n {
    (void)n;
    [self refilter];
}

// The keyboard went elsewhere (Cmd+1 from the menu, say): the palette goes,
// and the keyboard stays where it went.
- (void)controlTextDidEndEditing:(NSNotification *)n {
    (void)n;
    if (self.closing) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.closing && self.window &&
            self.window.firstResponder != self.field.currentEditor)
            [self closeRestoringFocus:NO];
    });
}

// ------------------------------------------------------------- the list
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)self.items.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
    MCPaletteRow *v = [tv makeViewWithIdentifier:@"MCPaletteRow" owner:self];
    if (!v) {
        v = [[MCPaletteRow alloc] initWithFrame:NSMakeRect(0, 0, 100, kPaletteRowHeight)];
        v.identifier = @"MCPaletteRow";
    }
    if (row < 0 || row >= (NSInteger)self.items.count) return v;
    MCPaletteItem *it = self.items[(NSUInteger)row];
    v.text = it.text;
    v.keys = it.keys;
    v.keyColor = self.mutedColor;
    v.selected = row == self.selected;
    v.needsDisplay = YES;
    return v;
}

- (void)rowClicked:(id)sender {
    (void)sender;
    NSInteger row = self.table.clickedRow;
    if (row < 0) return;
    [self runRow:row];
}

// ------------------------------------------------------------- run, close
- (void)runRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)self.items.count || !self.items[(NSUInteger)row].run) {
        NSBeep();
        return;
    }
    self.items[(NSUInteger)row].run();
}

- (void)closeRestoringFocus:(BOOL)restore {
    if (self.closing) return;
    self.closing = YES;
    if (_cancelListing) _cancelListing->store(true);
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(askWorkspace:)
                                               object:nil];
    if (self.workspaceAsked && self.context.cancelWorkspaceSymbols)
        self.context.cancelWorkspaceSymbols();
    // Closed without going to a symbol: the editor goes back to where it was.
    if (self.previewed && !self.ran && self.context.endPreview) self.context.endPreview();
    NSWindow *w = self.window;
    if (restore && w) {
        NSResponder *r = self.restore;
        BOOL ok = r == w;
        if ([r isKindOfClass:NSView.class]) {
            NSView *v = (NSView *)r;
            ok = v.window == w && !v.isHiddenOrHasHiddenAncestor;
        }
        [w makeFirstResponder:ok ? r : nil];
        // A text field selects all its text as it takes the keyboard; put
        // the caret back where it was (the terminal's input line, say).
        if (ok && [r isKindOfClass:NSTextField.class] &&
            self.restoreSelection.location != NSNotFound) {
            NSText *ed = ((NSTextField *)r).currentEditor;
            if (NSMaxRange(self.restoreSelection) <= ed.string.length)
                ed.selectedRange = self.restoreSelection;
        }
    }
    [self removeFromSuperview];
}

// New text in the field, the caret after it, and the list to match.
- (void)takeText:(NSString *)text {
    self.field.stringValue = text ?: @"";
    [self refilter];
    NSText *ed = self.field.currentEditor;
    if (ed) ed.selectedRange = NSMakeRange(ed.string.length, 0);
}
@end

// ------------------------------------------------------------------ public
NSView *MCCommandPaletteIn(NSWindow *window) {
    for (NSView *v in window.contentView.subviews)
        if ([v isKindOfClass:MCPaletteOverlay.class] && !((MCPaletteOverlay *)v).closing)
            return v;
    return nil;
}

void MCShowPalette(NSWindow *window, NSString *text, MCPaletteContext *context) {
    NSView *content = window.contentView;
    if (!content) return;
    MCPaletteOverlay *open = (MCPaletteOverlay *)MCCommandPaletteIn(window);
    // A pick list is its own palette: one never takes over the other.
    if (open && (open.context.pick || context.pick)) {
        [open closeRestoringFocus:YES];
        open = nil;
    }
    if (open) {
        [window makeFirstResponder:open.field];
        [open takeText:text];
        return;
    }
    // Read before the keyboard moves, so validation sees what had it.
    NSMutableArray<MCPaletteCommand *> *commands = [NSMutableArray array];
    PCollect(NSApp.mainMenu, nil, commands);
    PCollectUnmenued(commands);

    MCPaletteOverlay *p = [[MCPaletteOverlay alloc] initWithFrame:content.bounds
                                                         commands:commands
                                                          context:context ?: [MCPaletteContext new]];
    // Where the keyboard goes back to: a text field rather than its field
    // editor, which the field gets again (with the caret where it was).
    NSResponder *fr = window.firstResponder;
    p.restoreSelection = NSMakeRange(NSNotFound, 0);
    if ([fr isKindOfClass:NSTextView.class] && ((NSTextView *)fr).isFieldEditor &&
        [((NSTextView *)fr).delegate isKindOfClass:NSTextField.class]) {
        p.restoreSelection = ((NSTextView *)fr).selectedRange;
        fr = (NSResponder *)((NSTextView *)fr).delegate;
    }
    p.restore = fr;
    [content addSubview:p positioned:NSWindowAbove relativeTo:nil];
    [window makeFirstResponder:p.field];
    [p takeText:text];
    [p listFiles];
}

NSTextField *MCCommandPaletteField(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] ? ((MCPaletteOverlay *)palette).field
                                                          : nil;
}

NSArray<NSString *> *MCCommandPaletteLabels(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] ? [(MCPaletteOverlay *)palette labels]
                                                          : @[];
}

NSArray<NSString *> *MCCommandPaletteKeys(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] ? [(MCPaletteOverlay *)palette keyLabels]
                                                          : @[];
}

NSInteger MCCommandPaletteSelection(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] ? ((MCPaletteOverlay *)palette).selected
                                                          : -1;
}

BOOL MCCommandPaletteFilesListed(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] &&
           ((MCPaletteOverlay *)palette).files != nil;
}
