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
#include "CommandPalette.h"

#include <string>
#include <vector>

static NSString *const kPaletteRecentKey = @"MCCommandPaletteRecent";
static const CGFloat kPaletteRowHeight = 26;
static const NSInteger kPaletteMaxRows = 12;

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
        c.keys = PNS(CommandPalette::keyLabel(PU16(it.keyEquivalent),
                                              PMods(it.keyEquivalentModifierMask)));
        [out addObject:c];
    }
}

static std::vector<std::string> PRecent(void) {
    std::vector<std::string> out;
    for (id s in [MCAppDefaults() arrayForKey:kPaletteRecentKey])
        if ([s isKindOfClass:NSString.class]) out.push_back([s UTF8String]);
    return out;
}

static void PNoteRun(NSString *label) {
    std::vector<std::string> r = CommandPalette::noteRun(PRecent(), label.UTF8String);
    NSMutableArray *a = [NSMutableArray array];
    for (const std::string &s : r) [a addObject:@(s.c_str())];
    [MCAppDefaults() setObject:a forKey:kPaletteRecentKey];
}

// ------------------------------------------------------------------ a row
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
@end

@implementation MCPaletteOverlay {
    std::vector<CommandPalette::Entry> _entries;
    std::vector<CommandPalette::Match> _matches;
}

- (instancetype)initWithFrame:(NSRect)frame commands:(NSArray<MCPaletteCommand *> *)commands {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.commands = commands;
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
        initWithString:@"Type the name of a command"
            attributes:@{NSForegroundColorAttributeName: self.mutedColor,
                         NSFontAttributeName: f.font}];
    f.delegate = self;
    self.field = f;
    [self.fieldBack addSubview:f];

    MCPaletteTable *t = [[MCPaletteTable alloc] initWithFrame:NSZeroRect];
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"command"];
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

    self.emptyLabel = [NSTextField labelWithString:@"No matching commands"];
    self.emptyLabel.textColor = self.mutedColor;
    self.emptyLabel.font = [NSFont systemFontOfSize:13];
    self.emptyLabel.hidden = YES;
    [self.box addSubview:self.emptyLabel];

    [self refilter];
    return self;
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
    const NSInteger rows = MAX(1, MIN((NSInteger)_matches.size(), kPaletteMaxRows));
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

- (void)refilter {
    _matches = CommandPalette::rank(_entries, PU16(self.field.stringValue ?: @""), PRecent());
    self.selected = _matches.empty() ? -1 : 0;
    [self.table reloadData];
    self.emptyLabel.hidden = !_matches.empty();
    [self relayout];
    if (self.selected >= 0) [self.table scrollRowToVisible:0];
}

- (NSArray<NSString *> *)labels {
    NSMutableArray *a = [NSMutableArray array];
    for (const auto &m : _matches) [a addObject:self.commands[m.index].label];
    return a;
}

- (void)select:(NSInteger)row {
    const NSInteger n = (NSInteger)_matches.size();
    if (n == 0) return;
    row = ((row % n) + n) % n;   // wraps, as VS Code's list does
    NSInteger old = self.selected;
    self.selected = row;
    NSMutableIndexSet *rows = [NSMutableIndexSet indexSetWithIndex:row];
    if (old >= 0 && old < n) [rows addIndex:old];
    [self.table reloadDataForRowIndexes:rows columnIndexes:[NSIndexSet indexSetWithIndex:0]];
    [self.table scrollRowToVisible:row];
}

// Page moves stop at the ends instead of wrapping.
- (void)page:(NSInteger)delta {
    const NSInteger n = (NSInteger)_matches.size();
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
    return (NSInteger)_matches.size();
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col
                  row:(NSInteger)row {
    (void)col;
    MCPaletteRow *v = [tv makeViewWithIdentifier:@"MCPaletteRow" owner:self];
    if (!v) {
        v = [[MCPaletteRow alloc] initWithFrame:NSMakeRect(0, 0, 100, kPaletteRowHeight)];
        v.identifier = @"MCPaletteRow";
    }
    if (row < 0 || row >= (NSInteger)_matches.size()) return v;
    const CommandPalette::Match &m = _matches[(size_t)row];
    MCPaletteCommand *c = self.commands[m.index];
    v.text = [self attributedLabel:c match:m];
    v.keys = c.keys;
    v.keyColor = self.mutedColor;
    v.selected = row == self.selected;
    v.needsDisplay = YES;
    return v;
}

// The menu's part muted, the item's own title in the text color, the
// matched characters in the accent color and semibold.
- (NSAttributedString *)attributedLabel:(MCPaletteCommand *)c
                                  match:(const CommandPalette::Match &)m {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSFont *font = [NSFont systemFontOfSize:13];
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:c.label
            attributes:@{NSFontAttributeName: font,
                         NSForegroundColorAttributeName: self.textColor,
                         NSParagraphStyleAttributeName: ps}];
    const NSUInteger titleAt = c.label.length - c.title.length;
    if (titleAt > 0)
        [s addAttribute:NSForegroundColorAttributeName value:self.mutedColor
                  range:NSMakeRange(0, titleAt)];
    NSFont *bold = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
    for (size_t p : m.positions) {
        if (p >= c.label.length) continue;
        NSRange r = NSMakeRange(p, 1);
        [s addAttributes:@{NSFontAttributeName: bold,
                           NSForegroundColorAttributeName: PAccent()}
                   range:r];
    }
    return s;
}

- (void)rowClicked:(id)sender {
    (void)sender;
    NSInteger row = self.table.clickedRow;
    if (row < 0) return;
    [self runRow:row];
}

// ------------------------------------------------------------- run, close
- (void)runRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_matches.size()) { NSBeep(); return; }
    MCPaletteCommand *c = self.commands[_matches[(size_t)row].index];
    PNoteRun(c.label);
    NSMenuItem *item = c.item;
    // The keyboard goes back first, so the command acts on what had it
    // (Close Tab, Toggle Comment, Find...), as from the menu bar. It runs on
    // the next turn of the run loop, out of the field's own key handling.
    [self closeRestoringFocus:YES];
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp sendAction:item.action to:item.target from:item];
    });
}

- (void)closeRestoringFocus:(BOOL)restore {
    if (self.closing) return;
    self.closing = YES;
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
@end

// ------------------------------------------------------------------ public
NSView *MCCommandPaletteIn(NSWindow *window) {
    for (NSView *v in window.contentView.subviews)
        if ([v isKindOfClass:MCPaletteOverlay.class] && !((MCPaletteOverlay *)v).closing)
            return v;
    return nil;
}

void MCShowCommandPalette(NSWindow *window) {
    NSView *content = window.contentView;
    if (!content) return;
    MCPaletteOverlay *open = (MCPaletteOverlay *)MCCommandPaletteIn(window);
    if (open) {
        [window makeFirstResponder:open.field];
        [open.field selectText:nil];
        return;
    }
    // Read before the keyboard moves, so validation sees what had it.
    NSMutableArray<MCPaletteCommand *> *commands = [NSMutableArray array];
    PCollect(NSApp.mainMenu, nil, commands);

    MCPaletteOverlay *p = [[MCPaletteOverlay alloc] initWithFrame:content.bounds
                                                         commands:commands];
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
    [p relayout];
    [window makeFirstResponder:p.field];
}

NSTextField *MCCommandPaletteField(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] ? ((MCPaletteOverlay *)palette).field
                                                          : nil;
}

NSArray<NSString *> *MCCommandPaletteLabels(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] ? [(MCPaletteOverlay *)palette labels]
                                                          : @[];
}

NSInteger MCCommandPaletteSelection(NSView *palette) {
    return [palette isKindOfClass:MCPaletteOverlay.class] ? ((MCPaletteOverlay *)palette).selected
                                                          : -1;
}
