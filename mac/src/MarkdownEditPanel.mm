// MarkdownEditPanel.mm — see MarkdownEditPanel.h.
#import "MarkdownEditPanel.h"
#import "EditorFont.h"

// Return applies, Shift+Return types a newline, Escape cancels.
@interface MCMarkdownEditText : NSTextView
@property(nonatomic, copy) void (^onCommit)(void);
@property(nonatomic, copy) void (^onCancel)(void);
@end

@implementation MCMarkdownEditText
- (void)insertNewline:(id)sender {
    if (NSEvent.modifierFlags & NSEventModifierFlagShift) {
        [super insertNewline:sender];
        return;
    }
    if (self.onCommit) self.onCommit();
}
- (void)cancelOperation:(id)sender {
    if (self.onCancel) self.onCancel();
}
@end

@interface MCMarkdownEditPanel () <NSPopoverDelegate>
@property(nonatomic, readwrite) NSRect anchor;
@property(nonatomic, readwrite) CGFloat boxWidth;
@end

@implementation MCMarkdownEditPanel {
    NSPopover *_popover;
    MCMarkdownEditText *_editor;
    void (^_commit)(NSString *);
    void (^_addItem)(NSString *);
    BOOL _adding;
    // The text the box opened with, and whether its draft has been dealt
    // with (applied, or dropped by Esc or Cancel): a popover closed any
    // other way (a click elsewhere in the window, its window closing)
    // applies what was typed rather than losing it (popoverWillClose:).
    NSString *_original;
    BOOL _settled;
    __weak NSView *_view;
    NSRect _rect;
    CGFloat _width;
    CGFloat _left;
    NSFont *_font;
}

- (BOOL)shown { return _popover.shown; }

- (void)close {
    _settled = YES;
    [_popover performClose:nil];
}

- (BOOL)edited {
    return _editor && !_settled && ![_editor.string isEqualToString:_original ?: @""];
}

- (void)commitEditing {
    if ([self edited]) [self save:nil];
    else [self close];
}

- (void)showText:(NSString *)text
           title:(NSString *)title
          inView:(NSView *)view
            rect:(NSRect)rect
          commit:(void (^)(NSString *))commit
         addItem:(void (^)(NSString *))addItem {
    [self showText:text title:title inView:view rect:rect
         selection:NSMakeRange(NSNotFound, 0) width:0 left:NAN font:nil commit:commit
           addItem:addItem];
}

- (void)showText:(NSString *)text
           title:(NSString *)title
          inView:(NSView *)view
            rect:(NSRect)rect
       selection:(NSRange)selection
           width:(CGFloat)width
            left:(CGFloat)left
            font:(NSFont *)font
          commit:(void (^)(NSString *))commit
         addItem:(void (^)(NSString *))addItem {
    _commit = [commit copy];
    _addItem = [addItem copy];
    _adding = NO;
    _view = view;
    _rect = rect;
    _width = width;
    _left = left;
    _font = font;
    [self presentText:text title:title allowsItem:addItem != nil selection:selection];
}

- (void)presentText:(NSString *)text
              title:(NSString *)title
         allowsItem:(BOOL)allowsItem
          selection:(NSRange)selection {
    // As wide as asked (the paragraph on the page), within reason, and as
    // tall as the text is there, from two lines to a screenful.
    const CGFloat width = MIN(1400, MAX(460, _width > 0 ? _width + 24 : 460));
    NSFont *font = _font ?: MCMonoFont(MCZoomed(12), NSFontWeightRegular);
    NSTextStorage *measure = [[NSTextStorage alloc]
        initWithString:text.length ? text : @"M"
            attributes:@{NSFontAttributeName: font}];
    NSLayoutManager *lm = [NSLayoutManager new];
    NSTextContainer *tc = [[NSTextContainer alloc]
        initWithContainerSize:NSMakeSize(width - 24 - 4 - 16, CGFLOAT_MAX)];   // insets, scroller
    [lm addTextContainer:tc];
    [measure addLayoutManager:lm];
    [lm ensureLayoutForTextContainer:tc];
    const CGFloat line = ceil([lm defaultLineHeightForFont:font]);
    const CGFloat needed = ceil(NSHeight([lm usedRectForTextContainer:tc])) + 8 + line;
    const CGFloat editorHeight = MIN(420, MAX(2 * line + 8, needed));
    const CGFloat height = editorHeight + 76;
    NSView *content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];

    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont systemFontOfSize:11];
    label.textColor = [NSColor secondaryLabelColor];
    label.frame = NSMakeRect(12, height - 26, width - 24, 14);
    [content addSubview:label];

    NSScrollView *scroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12, 44, width - 24, editorHeight)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    MCMarkdownEditText *editor = [[MCMarkdownEditText alloc]
        initWithFrame:NSMakeRect(0, 0, width - 24, editorHeight)];
    editor.font = font;
    editor.string = text ?: @"";
    editor.richText = NO;
    editor.automaticQuoteSubstitutionEnabled = NO;
    editor.automaticDashSubstitutionEnabled = NO;
    editor.automaticTextReplacementEnabled = NO;
    editor.allowsUndo = YES;
    editor.textContainerInset = NSMakeSize(2, 4);
    editor.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = editor;
    [content addSubview:scroll];
    _editor = editor;

    __weak MCMarkdownEditPanel *weakSelf = self;
    editor.onCommit = ^{ [weakSelf save:nil]; };
    editor.onCancel = ^{ [weakSelf close]; };

    // Apply, not Save: the edit goes into the document, which Cmd+S (or
    // auto-save) then writes, as typing in the source would.
    NSButton *save = [NSButton buttonWithTitle:@"Apply" target:self action:@selector(save:)];
    save.bezelStyle = NSBezelStyleRounded;
    save.keyEquivalent = @"\r";
    save.frame = NSMakeRect(width - 92, 10, 80, 26);
    [content addSubview:save];

    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(close)];
    cancel.bezelStyle = NSBezelStyleRounded;
    cancel.frame = NSMakeRect(width - 176, 10, 80, 26);
    [content addSubview:cancel];

    if (allowsItem) {
        NSButton *add = [NSButton buttonWithTitle:@"Add item" target:self
                                           action:@selector(startAddItem:)];
        add.bezelStyle = NSBezelStyleRounded;
        add.font = [NSFont systemFontOfSize:11];
        add.frame = NSMakeRect(12, 10, 96, 26);
        add.toolTip = @"Add a new entry to this list, below this one";
        [content addSubview:add];
    }

    NSViewController *vc = [[NSViewController alloc] init];
    vc.view = content;
    _settled = YES;   // the box this one replaces: its draft was dealt with
    [_popover performClose:nil];
    _popover = [[NSPopover alloc] init];
    _popover.contentViewController = vc;
    // Semi-transient: a click elsewhere in the window closes it, but
    // switching to another app and back does not (transient did, and what
    // was typed went with it: D2 on 1.6.3).
    _popover.behavior = NSPopoverBehaviorSemitransient;
    _popover.delegate = self;
    _original = [text copy] ?: @"";
    _settled = NO;
    NSView *view = _view;
    if (!view) return;
    _anchor = [self anchorForWidth:width inView:view];
    _boxWidth = width;
    [_popover showRelativeToRect:_anchor ofView:view preferredEdge:NSRectEdgeMaxY];
    [editor.window makeFirstResponder:editor];
    // The word that was double-clicked, as a double-click selects one
    // anywhere else; never the whole block, which one key would replace.
    if (selection.location != NSNotFound && NSMaxRange(selection) <= editor.string.length)
        [editor setSelectedRange:selection];
    else
        [editor setSelectedRange:NSMakeRange(editor.string.length, 0)];
    [editor scrollRangeToVisible:editor.selectedRange];
}

// Where the popover points. A popover is centered on what it points at, so
// with a left edge asked for the point moves along rect's line to where a
// box `width` wide starts with its text at `_left` (the editor's text is
// kTextInset in from the box's edge), and the box stays within the view's
// part on show, its left edge first when it is wider than that.
static const CGFloat kTextInset = 12 + 1 + 2 + 5;   // margin, bezel, inset, padding
static const CGFloat kEdgeGap = 4;

- (NSRect)anchorForWidth:(CGFloat)width inView:(NSView *)view {
    if (isnan(_left)) return _rect;
    const NSRect shown = view.visibleRect;
    CGFloat boxLeft = _left - kTextInset;
    boxLeft = MIN(boxLeft, NSMaxX(shown) - kEdgeGap - width);
    boxLeft = MAX(boxLeft, NSMinX(shown) + kEdgeGap);
    const CGFloat mid = round(boxLeft + width / 2);
    return NSMakeRect(mid - 1, NSMinY(_rect), 2, MAX(1, NSHeight(_rect)));
}

- (void)startAddItem:(id)sender {
    _adding = YES;
    [self presentText:@"" title:@"New list entry" allowsItem:NO
            selection:NSMakeRange(NSNotFound, 0)];
}

- (void)save:(id)sender {
    NSString *text = [_editor.string copy] ?: @"";
    [self close];
    [self apply:text];
}

- (void)apply:(NSString *)text {
    if (_adding) {
        if (text.length && _addItem) _addItem(text);
    } else if (_commit) {
        _commit(text);
    }
}

// AppKit closing the box on its own (a click elsewhere in the window, the
// window closing): what was typed is applied, one undoable edit, never
// dropped unseen. Esc, Cancel, Apply and a box replacing this one have
// settled it first.
- (void)popoverWillClose:(NSNotification *)note {
    if (note.object != _popover || _settled) return;
    const BOOL edited = [self edited];
    NSString *text = [_editor.string copy] ?: @"";
    _settled = YES;
    if (edited) [self apply:text];
}

- (void)popoverDidClose:(NSNotification *)note {
    // A popover replaced by the "Add item" one closes too; keep the editor
    // that is showing.
    if (note.object == _popover) _editor = nil;
}

@end
