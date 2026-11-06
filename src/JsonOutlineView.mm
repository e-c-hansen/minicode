// JsonOutlineView.mm — see JsonOutlineView.h.
#import "JsonOutlineView.h"
#import "AppSettings.h"
#import "Appearance.h"
#include "JsonOutline.h"
#include <memory>
#include <string>
#include <vector>

using JsonOutline::Kind;
using JsonOutline::Node;
using JsonOutline::Outline;

static NSString *Str(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}

static NSColor *Muted(void) {
    return [NSColor colorWithSRGBRed:0x9C / 255.0 green:0xA3 / 255.0 blue:0xAF / 255.0 alpha:1];
}

@interface MCJsonOutlinePane () <NSOutlineViewDataSource, NSOutlineViewDelegate>
@end

@implementation MCJsonOutlinePane {
    NSScrollView *_scroll;
    NSOutlineView *_outline;
    NSTextField *_status;
    __weak NSTextView *_textView;
    std::shared_ptr<Outline> _tree;
    uint64_t _generation;
    BOOL _parsing, _syncing;
    BOOL _stale;   // the text changed since _tree was read: its offsets are off
    dispatch_queue_t _queue;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        _queue = dispatch_queue_create("minicode.json-outline", DISPATCH_QUEUE_SERIAL);
        _status = [NSTextField labelWithString:@""];
        _status.font = [NSFont systemFontOfSize:11];
        _status.textColor = Muted();
        _status.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:_status];

        _outline = [[NSOutlineView alloc] initWithFrame:NSZeroRect];
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"node"];
        col.resizingMask = NSTableColumnAutoresizingMask;
        [_outline addTableColumn:col];
        _outline.outlineTableColumn = col;
        _outline.headerView = nil;
        _outline.dataSource = self;
        _outline.delegate = self;
        _outline.rowSizeStyle = NSTableViewRowSizeStyleSmall;
        _outline.indentationPerLevel = 12;
        _outline.style = NSTableViewStylePlain;
        _outline.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
        _outline.backgroundColor = NSColor.clearColor;
        _outline.target = self;
        _outline.doubleAction = @selector(doubleClicked:);
        _outline.menu = MCAdjustSettingsMenu(@"editor", _outline);

        _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _scroll.documentView = _outline;
        _scroll.hasVerticalScroller = YES;
        _scroll.autohidesScrollers = YES;
        _scroll.drawsBackground = NO;
        _scroll.borderType = NSNoBorder;
        [self addSubview:_scroll];
        [self applySettings];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(settingsChanged:)
                                                     name:MCSettingsDidChangeNotification
                                                   object:nil];
    }
    return self;
}

- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }

- (void)settingsChanged:(NSNotification *)n {
    [self applySettings];
    [_outline reloadData];
}

- (void)applySettings {
    self.layer.backgroundColor = [[AppSettings shared] background:Surface::Sidebar].CGColor;
    self.layer.borderColor = [NSColor colorWithWhite:0.2 alpha:1].CGColor;
    self.layer.borderWidth = 0;
}

- (BOOL)isFlipped { return YES; }

- (void)resizeSubviewsWithOldSize:(NSSize)old {
    const NSRect b = self.bounds;
    _status.frame = NSMakeRect(10, 4, MAX(0, NSWidth(b) - 16), 16);
    _scroll.frame = NSMakeRect(1, 24, MAX(0, NSWidth(b) - 1), MAX(0, NSHeight(b) - 24));
}

- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithWhite:0.2 alpha:1] setFill];
    NSRectFill(NSMakeRect(0, 0, 1, NSHeight(self.bounds)));   // the line by the text
}

- (NSOutlineView *)outline { return _outline; }
- (NSString *)statusText { return _status.stringValue; }
- (BOOL)parsing { return _parsing; }
- (BOOL)stale { return _stale; }

// ------------------------------------------------------------ the text
- (void)attachToTextView:(NSTextView *)textView {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    if (_textView) {
        [nc removeObserver:self name:NSTextStorageDidProcessEditingNotification
                    object:_textView.textStorage];
        [nc removeObserver:self name:NSTextViewDidChangeSelectionNotification object:_textView];
    }
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(parseNow) object:nil];
    _textView = textView;
    _tree.reset();
    _generation++;
    _parsing = NO;
    _stale = NO;
    _outline.alphaValue = 1;
    _status.stringValue = @"";
    [_outline reloadData];
    if (!textView) return;
    [nc addObserver:self selector:@selector(textChanged:)
               name:NSTextStorageDidProcessEditingNotification object:textView.textStorage];
    [nc addObserver:self selector:@selector(caretMoved:)
               name:NSTextViewDidChangeSelectionNotification object:textView];
    [self parseNow];
}

- (void)textChanged:(NSNotification *)n {
    if (!([(NSTextStorage *)n.object editedMask] & NSTextStorageEditedCharacters)) return;
    _stale = YES;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(parseNow) object:nil];
    [self performSelector:@selector(parseNow) withObject:nil afterDelay:0.3];
}

- (void)parseNow {
    NSTextView *tv = _textView;
    if (!tv) return;
    NSString *text = [tv.string copy];   // the storage's string is live
    const uint64_t gen = ++_generation;
    _parsing = YES;
    const bool jsonc = _allowsComments;
    __weak MCJsonOutlinePane *weakSelf = self;
    dispatch_async(_queue, ^{
        const char *c = text.UTF8String;
        auto tree = std::make_shared<Outline>(JsonOutline::parse(c ? c : "", 2000000, jsonc));
        dispatch_async(dispatch_get_main_queue(), ^{
            MCJsonOutlinePane *me = weakSelf;
            if (me) [me apply:tree generation:gen];
        });
    });
}

// The path of labels down to a node, to find it again after a parse.
- (NSString *)pathOf:(size_t)i in:(const Outline &)o {
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (long k = (long)i; k >= 0; k = o.nodes[(size_t)k].parent)
        [parts insertObject:Str(JsonOutline::label(o.nodes[(size_t)k])) atIndex:0];
    return [parts componentsJoinedByString:@"\x1f"];
}

- (void)apply:(std::shared_ptr<Outline>)tree generation:(uint64_t)gen {
    if (gen != _generation) return;   // a newer parse is on its way
    _parsing = NO;
    NSNumberFormatter *f = [NSNumberFormatter new];
    f.numberStyle = NSNumberFormatterDecimalStyle;
    if (!tree->ok) {
        _status.stringValue = [NSString stringWithFormat:@"Not valid JSON: %@ at line %zu, column %zu",
                                                         Str(tree->error), tree->errorLine,
                                                         tree->errorColumn];
        _status.textColor = [NSColor systemOrangeColor];
        if (_tree && !_tree->nodes.empty()) {
            // The last good outline stays, dimmed: its places in the text
            // are off by whatever changed since, so it selects nothing.
            _stale = YES;
            _outline.alphaValue = 0.5;
            return;
        }
    } else {
        _status.stringValue = [NSString stringWithFormat:@"%@ %@",
                                  [f stringFromNumber:@(tree->nodes.size())],
                                  tree->nodes.size() == 1 ? @"value" : @"values"];
        _status.textColor = Muted();
    }
    // What was open stays open.
    NSMutableSet<NSString *> *open = [NSMutableSet set];
    if (_tree) {
        for (NSInteger r = 0; r < _outline.numberOfRows; r++) {
            id item = [_outline itemAtRow:r];
            if ([_outline isItemExpanded:item])
                [open addObject:[self pathOf:[item unsignedIntegerValue] in:*_tree]];
        }
    }
    const BOOL first = !_tree;
    _tree = tree;
    _stale = NO;
    _outline.alphaValue = 1;
    _syncing = YES;
    [_outline reloadData];
    if (!_tree->nodes.empty()) {
        if (first || open.count == 0) {
            [_outline expandItem:@0];
        } else {
            // Down from the root, only into what was open.
            std::vector<size_t> todo{0};
            while (!todo.empty()) {
                const size_t i = todo.back();
                todo.pop_back();
                if (![open containsObject:[self pathOf:i in:*_tree]]) continue;
                [_outline expandItem:@(i)];
                for (size_t c : _tree->nodes[i].children)
                    if (!_tree->nodes[c].children.empty()) todo.push_back(c);
            }
        }
    }
    _syncing = NO;
    [self caretMoved:nil];
}

// ------------------------------------------------------------ data source
- (const Node *)node:(id)item {
    if (!_tree || ![item isKindOfClass:NSNumber.class]) return nullptr;
    const size_t i = [item unsignedIntegerValue];
    return i < _tree->nodes.size() ? &_tree->nodes[i] : nullptr;
}

- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    if (!_tree || _tree->nodes.empty()) return 0;
    if (!item) return 1;
    const Node *n = [self node:item];
    return n ? (NSInteger)n->children.size() : 0;
}

- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)index ofItem:(id)item {
    if (!item) return @0;
    const Node *n = [self node:item];
    return @(n->children[(size_t)index]);
}

- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item {
    const Node *n = [self node:item];
    return n && !n->children.empty();
}

- (NSAttributedString *)textFor:(const Node &)n {
    AppSettings *cfg = [AppSettings shared];
    NSFont *font = [NSFont monospacedSystemFontOfSize:11.5 weight:NSFontWeightRegular];
    NSMutableAttributedString *s = [[NSMutableAttributedString alloc]
        initWithString:Str(JsonOutline::label(n))
            attributes:@{NSFontAttributeName: font,
                         NSForegroundColorAttributeName: [cfg text:Surface::Sidebar]}];
    NSColor *valueColor = Muted();
    switch (n.kind) {
    case Kind::String: valueColor = [cfg syntax:TokenStyle::String]; break;
    case Kind::Number: valueColor = [cfg syntax:TokenStyle::Number]; break;
    case Kind::Bool:
    case Kind::Null: valueColor = [cfg syntax:TokenStyle::Keyword]; break;
    default: break;
    }
    std::string summary = JsonOutline::summary(n);
    for (char &ch : summary)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:[@"  " stringByAppendingString:Str(summary)]
            attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: valueColor}]];
    return s;
}

- (NSView *)outlineView:(NSOutlineView *)ov viewForTableColumn:(NSTableColumn *)col item:(id)item {
    NSTableCellView *cell = [ov makeViewWithIdentifier:@"node" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = @"node";
        NSTextField *tf = [NSTextField labelWithString:@""];
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
        tf.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:tf];
        cell.textField = tf;
        [NSLayoutConstraint activateConstraints:@[
            [tf.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:2],
            [tf.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-2],
            [tf.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    const Node *n = [self node:item];
    cell.textField.attributedStringValue = n ? [self textFor:*n] : [NSAttributedString new];
    return cell;
}

- (NSString *)rowText:(NSInteger)row {
    const Node *n = [self node:[_outline itemAtRow:row]];
    if (!n) return @"";
    return [NSString stringWithFormat:@"%@ %@", Str(JsonOutline::label(*n)),
                                      Str(JsonOutline::summary(*n))];
}

// ------------------------------------------------------------ linking
- (NSRange)rangeOf:(const Node &)n {
    NSTextView *tv = _textView;
    const NSUInteger len = tv.string.length;
    NSUInteger start = MIN((NSUInteger)n.memberStart16, len);
    NSUInteger end = MIN(MAX((NSUInteger)n.end16, start), len);
    return NSMakeRange(start, end - start);
}

- (void)selectRow:(NSInteger)row {
    [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
}

- (void)outlineViewSelectionDidChange:(NSNotification *)n {
    if (_syncing || _stale) return;
    const Node *node = [self node:[_outline itemAtRow:_outline.selectedRow]];
    NSTextView *tv = _textView;
    if (!node || !tv) return;
    _syncing = YES;
    NSRange r = [self rangeOf:*node];
    tv.selectedRange = r;
    [tv scrollRangeToVisible:r];
    _syncing = NO;
}

// The caret moved in the text: the node it is in is selected, its
// folders opened, the text left alone.
- (void)caretMoved:(NSNotification *)n {
    NSTextView *tv = _textView;
    if (_syncing || _stale || !_tree || !tv) return;
    const long at = JsonOutline::nodeAt16(*_tree, tv.selectedRange.location);
    if (at < 0) return;
    _syncing = YES;
    NSMutableArray<NSNumber *> *chain = [NSMutableArray array];
    for (long k = _tree->nodes[(size_t)at].parent; k >= 0; k = _tree->nodes[(size_t)k].parent)
        [chain insertObject:@(k) atIndex:0];
    for (NSNumber *k in chain) [_outline expandItem:k];
    const NSInteger row = [_outline rowForItem:@(at)];
    if (row >= 0) {
        [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
        [_outline scrollRowToVisible:row];
    }
    _syncing = NO;
}

// A double click takes the keyboard to the node's text.
- (void)doubleClicked:(id)sender {
    NSTextView *tv = _textView;
    if (tv) [tv.window makeFirstResponder:tv];
}
@end
