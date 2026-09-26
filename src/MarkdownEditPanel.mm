// MarkdownEditPanel.mm — see MarkdownEditPanel.h.
#import "MarkdownEditPanel.h"

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
    _commit = [commit copy];
    _addItem = [addItem copy];
    _adding = NO;
    _view = view;
    _rect = rect;
    [self presentText:text title:title allowsItem:addItem != nil];
}

- (void)presentText:(NSString *)text title:(NSString *)title allowsItem:(BOOL)allowsItem {
    // Taller for text that already runs over several lines.
    NSUInteger lines = 1;
    for (NSUInteger i = 0; i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') lines++;
    const CGFloat editorHeight = MIN(260, MAX(74, 18.0 * lines + 20));
    const CGFloat width = 460, height = editorHeight + 76;
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
    editor.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
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
    [editor setSelectedRange:NSMakeRange(0, editor.string.length)];
}

- (void)startAddItem:(id)sender {
    _adding = YES;
    [self presentText:@"" title:@"New list entry" allowsItem:NO];
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
