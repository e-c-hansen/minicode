// MarkdownEditPanel.mm — see MarkdownEditPanel.h.
#import "MarkdownEditPanel.h"
#import "EditorFont.h"

// Return commits, Shift+Return types a newline, Escape cancels.
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
@end

@implementation MCMarkdownEditPanel {
    NSPopover *_popover;
    MCMarkdownEditText *_editor;
    void (^_commit)(NSString *);
    void (^_addItem)(NSString *);
    BOOL _adding;
    __weak NSView *_view;
    NSRect _rect;
    CGFloat _width;
    NSFont *_font;
}

- (BOOL)shown { return _popover.shown; }

- (void)close {
    [_popover performClose:nil];
}

- (void)showText:(NSString *)text
           title:(NSString *)title
          inView:(NSView *)view
            rect:(NSRect)rect
          commit:(void (^)(NSString *))commit
         addItem:(void (^)(NSString *))addItem {
    [self showText:text title:title inView:view rect:rect
         selection:NSMakeRange(NSNotFound, 0) width:0 font:nil commit:commit addItem:addItem];
}

- (void)showText:(NSString *)text
           title:(NSString *)title
          inView:(NSView *)view
            rect:(NSRect)rect
       selection:(NSRange)selection
           width:(CGFloat)width
            font:(NSFont *)font
          commit:(void (^)(NSString *))commit
         addItem:(void (^)(NSString *))addItem {
    _commit = [commit copy];
    _addItem = [addItem copy];
    _adding = NO;
    _view = view;
    _rect = rect;
    _width = width;
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

    NSButton *save = [NSButton buttonWithTitle:@"Save" target:self action:@selector(save:)];
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
    [_popover performClose:nil];
    _popover = [[NSPopover alloc] init];
    _popover.contentViewController = vc;
    _popover.behavior = NSPopoverBehaviorTransient;
    _popover.delegate = self;
    NSView *view = _view;
    if (!view) return;
    [_popover showRelativeToRect:_rect ofView:view preferredEdge:NSRectEdgeMaxY];
    [editor.window makeFirstResponder:editor];
    // The word that was double-clicked, as a double-click selects one
    // anywhere else; never the whole block, which one key would replace.
    if (selection.location != NSNotFound && NSMaxRange(selection) <= editor.string.length)
        [editor setSelectedRange:selection];
    else
        [editor setSelectedRange:NSMakeRange(editor.string.length, 0)];
    [editor scrollRangeToVisible:editor.selectedRange];
}

- (void)startAddItem:(id)sender {
    _adding = YES;
    [self presentText:@"" title:@"New list entry" allowsItem:NO
            selection:NSMakeRange(NSNotFound, 0)];
}

- (void)save:(id)sender {
    NSString *text = [_editor.string copy] ?: @"";
    const BOOL adding = _adding;
    void (^commit)(NSString *) = _commit;
    void (^addItem)(NSString *) = _addItem;
    [self close];
    if (adding) {
        if (text.length && addItem) addItem(text);
    } else if (commit) {
        commit(text);
    }
}

- (void)popoverDidClose:(NSNotification *)note {
    // A popover replaced by the "Add item" one closes too; keep the editor
    // that is showing.
    if (note.object == _popover) _editor = nil;
}

@end
