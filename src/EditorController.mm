// EditorController.mm — Objective-C++. Bridges the pure-C++ core (syntax
// highlighter, markdown parser) to AppKit views.
#import "EditorController.h"
#import "Terminal.h"
#import "Browser.h"
#include "SyntaxHighlighter.h"
#include "MarkdownParser.h"
#include <string>

// A plain container that relays every resize to a layout block, so we can
// dock the editor/browser/terminal manually (predictable, no split-view math).
@interface PanelHost : NSView
@property(nonatomic, copy) void (^onLayout)(void);
@end
@implementation PanelHost
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; if (self.onLayout) self.onLayout(); }
@end

// A thin horizontal drag handle for resizing the terminal dock. Reports the
// dragged Y position (in its superview's coordinates) to a block.
@interface DragBar : NSView
@property(nonatomic, copy) void (^onDrag)(CGFloat yInSuperview);
@end
@implementation DragBar
- (void)mouseDragged:(NSEvent *)event {
    NSPoint p = [self.superview convertPoint:event.locationInWindow fromView:nil];
    if (self.onDrag) self.onDrag(p.y);
}
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:[NSCursor resizeUpDownCursor]];
}
@end

// ---------------------------------------------------------------------------
// FileItem: one node in the file tree. Children load lazily on expansion.
// ---------------------------------------------------------------------------
@interface FileItem : NSObject
@property(nonatomic, copy)   NSString *path;
@property(nonatomic, assign) BOOL isDir;
@property(nonatomic, strong) NSMutableArray<FileItem *> *children;
@property(nonatomic, assign) BOOL loaded;
@end

@implementation FileItem
- (NSString *)name { return self.path.lastPathComponent; }

- (void)loadChildren {
    if (self.loaded || !self.isDir) return;
    self.loaded = YES;
    self.children = [NSMutableArray array];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *entries = [fm contentsOfDirectoryAtPath:self.path error:nil];
    entries = [entries sortedArrayUsingComparator:^(NSString *a, NSString *b) {
        return [a caseInsensitiveCompare:b];
    }];
    NSMutableArray *dirs = [NSMutableArray array];
    NSMutableArray *files = [NSMutableArray array];
    for (NSString *entry in entries) {
        if ([entry hasPrefix:@"."]) continue;               // hide dotfiles
        NSString *full = [self.path stringByAppendingPathComponent:entry];
        BOOL d = NO;
        [fm fileExistsAtPath:full isDirectory:&d];
        FileItem *item = [FileItem new];
        item.path = full; item.isDir = d;
        [(d ? dirs : files) addObject:item];                // dirs first
    }
    [self.children addObjectsFromArray:dirs];
    [self.children addObjectsFromArray:files];
}
@end

// ---------------------------------------------------------------------------
// ClickOutline: handles a click in mouseDown directly, so opening a file never
// depends on NSOutlineView's internal selection/tracking loop (which was
// swallowing single clicks in this layout).
// ---------------------------------------------------------------------------
@interface ClickOutline : NSOutlineView
@property(nonatomic, copy) void (^onRowClick)(NSInteger row, NSPoint pointInView);
@end

@implementation ClickOutline
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    if (row >= 0 && self.onRowClick) self.onRowClick(row, p);
    [super mouseDown:event];   // keep native selection / expansion visuals
}
@end

// ---------------------------------------------------------------------------
// Dark theme colors (VS Code "Dark+" inspired).
// ---------------------------------------------------------------------------
static NSColor *Hex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8)  & 0xFF) / 255.0
                                blue:( rgb        & 0xFF) / 255.0
                               alpha:1.0];
}

static NSColor *ColorForStyle(TokenStyle s) {
    switch (s) {
        case TokenStyle::Keyword:      return Hex(0x569CD6);
        case TokenStyle::Type:         return Hex(0x4EC9B0);
        case TokenStyle::String:       return Hex(0xCE9178);
        case TokenStyle::Comment:      return Hex(0x6A9955);
        case TokenStyle::Number:       return Hex(0xB5CEA8);
        case TokenStyle::Preprocessor: return Hex(0xC586C0);
        case TokenStyle::Function:     return Hex(0xDCDCAA);
        default:                       return Hex(0xD4D4D4);
    }
}

// ---------------------------------------------------------------------------
@interface EditorController () {
    FileItem *_root;
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) ClickOutline *outline;
@property(nonatomic, strong) NSTextView *textView;
@property(nonatomic, strong) NSTextField *statusLabel;
@property(nonatomic, copy)   NSString *currentPath;
@property(nonatomic, copy)   NSString *sourceText;   // raw file text (edited)
@property(nonatomic, copy)   NSString *currentExt;
@property(nonatomic, assign) BOOL isMarkdown;
@property(nonatomic, assign) BOOL previewMode;       // markdown: rendered vs source
@property(nonatomic, assign) BOOL dirty;             // unsaved changes
@property(nonatomic, strong) NSView *statusBar;
@property(nonatomic, strong) NSView *hintsPanel;
@property(nonatomic, strong) NSTextField *hintsLabel;
@property(nonatomic, assign) BOOL hintsVisible;
@property(nonatomic, strong) PanelHost *rightArea;
@property(nonatomic, strong) NSScrollView *editorScroll;
@property(nonatomic, strong) TerminalView *terminal;
@property(nonatomic, strong) DragBar *termDivider;
@property(nonatomic, strong) BrowserView *browser;
@property(nonatomic, assign) BOOL terminalVisible;
@property(nonatomic, assign) BOOL browserVisible;
@property(nonatomic, assign) CGFloat terminalHeight;
@end

@implementation EditorController

- (instancetype)initWithRootPath:(NSString *)path {
    if ((self = [super init])) {
        _root = [FileItem new];
        _root.path = path; _root.isDir = YES;
        [_root loadChildren];
        _terminalHeight = 220;
    }
    return self;
}

- (NSString *)rootPath { return _root.path; }

// -------------------------------------------------------------- window setup
- (void)showWindow {
    NSRect frame = NSMakeRect(0, 0, 1100, 720);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"MiniCode";
    self.window.delegate = self;
    self.window.minSize = NSMakeSize(640, 400);
    [self.window center];

    // --- split view: sidebar | editor
    NSSplitView *split = [[NSSplitView alloc] initWithFrame:frame];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    split.delegate = self;   // pins sidebar width, prevents collapse-to-zero

    // --- sidebar (file tree)
    NSScrollView *treeScroll = [[NSScrollView alloc] init];
    treeScroll.hasVerticalScroller = YES;
    treeScroll.drawsBackground = YES;
    treeScroll.backgroundColor = Hex(0x252526);

    self.outline = [[ClickOutline alloc] init];
    __weak EditorController *weakSelf = self;
    self.outline.onRowClick = ^(NSInteger row, NSPoint pt) {
        [weakSelf handleRowClick:row atPoint:pt];
    };
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"file"];
    col.editable = NO;
    col.minWidth = 80;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [self.outline addTableColumn:col];
    self.outline.outlineTableColumn = col;
    self.outline.columnAutoresizingStyle =
        NSTableViewLastColumnOnlyAutoresizingStyle;
    self.outline.headerView = nil;
    self.outline.dataSource = self;
    self.outline.delegate = self;
    self.outline.backgroundColor = Hex(0x252526);
    self.outline.rowSizeStyle = NSTableViewRowSizeStyleMedium;
    self.outline.indentationPerLevel = 14;
    self.outline.floatsGroupRows = NO;
    treeScroll.documentView = self.outline;

    // --- editor pane
    NSScrollView *textScroll = [[NSScrollView alloc] init];
    self.editorScroll = textScroll;
    textScroll.hasVerticalScroller = YES;
    textScroll.hasHorizontalScroller = YES;
    textScroll.autohidesScrollers = YES;

    self.textView = [[NSTextView alloc] initWithFrame:frame];
    self.textView.editable = YES;
    self.textView.delegate = self;
    self.textView.richText = YES;
    self.textView.allowsUndo = YES;
    self.textView.automaticSpellingCorrectionEnabled = NO;
    self.textView.automaticDashSubstitutionEnabled = NO;
    self.textView.backgroundColor = Hex(0x1E1E1E);
    self.textView.textColor = Hex(0xD4D4D4);
    self.textView.insertionPointColor = Hex(0xD4D4D4);
    self.textView.textContainerInset = NSMakeSize(8, 8);
    self.textView.automaticQuoteSubstitutionEnabled = NO;
    self.textView.minSize = NSMakeSize(0, 0);
    self.textView.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    self.textView.verticallyResizable = YES;
    self.textView.horizontallyResizable = YES;
    self.textView.textContainer.widthTracksTextView = YES;
    textScroll.documentView = self.textView;

    // Right side hosts the editor plus (lazily) a terminal dock and browser.
    self.rightArea = [[PanelHost alloc] initWithFrame:frame];
    self.rightArea.onLayout = ^{ [weakSelf relayoutRightArea]; };
    textScroll.frame = self.rightArea.bounds;
    [self.rightArea addSubview:textScroll];

    [split addSubview:treeScroll];
    [split addSubview:self.rightArea];

    // Container holds: split view (top) + status bar (bottom) + hints overlay.
    NSView *container = [[NSView alloc] initWithFrame:frame];
    const CGFloat barH = 24;
    split.frame = NSMakeRect(0, barH, frame.size.width, frame.size.height - barH);
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [container addSubview:split];
    [self buildStatusBarInContainer:container height:barH];
    [self buildHintsPanelInContainer:container];

    self.window.contentView = container;
    [self showWelcome];
    [self.window makeKeyAndOrderFront:nil];
    [self.outline reloadData];

    // Set the divider AFTER the window is on screen and laid out — otherwise
    // autoresizing collapses the sidebar to zero width.
    [self.window layoutIfNeeded];
    [split adjustSubviews];
    [split setPosition:260 ofDividerAtIndex:0];
    [self.outline sizeLastColumnToFit];
}

// -------------------------------------------------------- status bar + hints
- (void)buildStatusBarInContainer:(NSView *)container height:(CGFloat)barH {
    self.statusBar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, container.bounds.size.width, barH)];
    self.statusBar.wantsLayer = YES;
    self.statusBar.layer.backgroundColor = Hex(0x007ACC).CGColor;  // VS Code blue
    self.statusBar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;

    NSTextField *label = [NSTextField labelWithString:@"⌃H  Shortcuts"];
    label.textColor = [NSColor whiteColor];
    label.font = [NSFont systemFontOfSize:11];
    label.backgroundColor = [NSColor clearColor];
    label.frame = NSMakeRect(0, 3, container.bounds.size.width - 12, 16);
    label.alignment = NSTextAlignmentRight;
    label.autoresizingMask = NSViewWidthSizable;
    self.statusLabel = label;
    [self.statusBar addSubview:label];
    [container addSubview:self.statusBar];
}

- (void)buildHintsPanelInContainer:(NSView *)container {
    const CGFloat pw = 360, ph = 240, margin = 18;
    NSRect r = NSMakeRect(container.bounds.size.width - pw - margin,
                          container.bounds.size.height - ph - margin, pw, ph);
    self.hintsPanel = [[NSView alloc] initWithFrame:r];
    self.hintsPanel.wantsLayer = YES;
    self.hintsPanel.layer.backgroundColor =
        [NSColor colorWithSRGBRed:0.11 green:0.11 blue:0.12 alpha:0.97].CGColor;
    self.hintsPanel.layer.cornerRadius = 8;
    self.hintsPanel.layer.borderWidth = 1;
    self.hintsPanel.layer.borderColor = Hex(0x3C3C3C).CGColor;
    self.hintsPanel.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    self.hintsPanel.hidden = YES;

    NSTextField *lbl = [NSTextField labelWithString:@""];
    lbl.frame = NSMakeRect(16, 14, pw - 32, ph - 28);
    lbl.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    lbl.textColor = Hex(0xE0E0E0);
    lbl.backgroundColor = [NSColor clearColor];
    lbl.maximumNumberOfLines = 0;
    lbl.lineBreakMode = NSLineBreakByWordWrapping;
    [self.hintsPanel addSubview:lbl];
    self.hintsLabel = lbl;
    [container addSubview:self.hintsPanel];
}

// Context-aware shortcut list.
- (NSString *)hintsText {
    NSMutableString *s = [NSMutableString string];
    [s appendString:@"Keyboard Shortcuts\n"];
    [s appendString:@"──────────────────────────\n"];
    [s appendString:@"⌘O    Open Folder\n"];
    [s appendString:@"⌘S    Save\n"];
    [s appendString:@"⌘Z    Undo      ⇧⌘Z  Redo\n"];
    [s appendString:@"⌘C    Copy      ⌘A   Select All\n"];
    [s appendFormat:@"⌃`    Terminal  (%@)\n",
        self.terminalVisible ? @"open" : @"hidden"];
    [s appendFormat:@"⇧⌘B   Browser   (%@)\n",
        self.browserVisible ? @"open" : @"hidden"];
    if (self.isMarkdown) {
        [s appendString:@"\nMarkdown\n"];
        [s appendString:@"──────────────────────────\n"];
        [s appendFormat:@"⇧⌘P   Toggle Preview  (now: %@)\n",
            self.previewMode ? @"rendered" : @"source"];
    }
    [s appendString:@"\n⌃H    Hide these hints"];
    return s;
}

- (void)toggleHints:(id)sender {
    self.hintsVisible = !self.hintsVisible;
    if (self.hintsVisible) self.hintsLabel.stringValue = [self hintsText];
    self.hintsPanel.hidden = !self.hintsVisible;
    self.statusLabel.stringValue =
        self.hintsVisible ? @"⌃H  Hide shortcuts" : @"⌃H  Shortcuts";
}

// -------------------------------------------------------- terminal + browser
// Dock the terminal at the bottom; the editor OR the browser fills the top.
- (void)relayoutRightArea {
    NSRect b = self.rightArea.bounds;
    CGFloat W = b.size.width, H = b.size.height;
    BOOL showTerm = self.terminalVisible && self.terminal != nil;

    CGFloat termH = 0;
    if (showTerm) termH = MIN(MAX(self.terminalHeight, 80), MAX(120, H - 120));
    CGFloat topH = MAX(0, H - termH);
    NSRect topRect = NSMakeRect(0, termH, W, topH);

    BOOL showBrowser = self.browserVisible && self.browser != nil;
    self.editorScroll.frame = topRect;
    self.editorScroll.hidden = showBrowser;
    if (self.browser) {
        self.browser.frame = topRect;
        self.browser.hidden = !showBrowser;
    }
    if (self.terminal) {
        self.terminal.frame = NSMakeRect(0, 0, W, termH);
        self.terminal.hidden = !showTerm;
    }
    if (self.termDivider) {
        self.termDivider.frame = NSMakeRect(0, termH - 3, W, 6);
        self.termDivider.hidden = !showTerm;
    }
}

- (void)toggleTerminal:(id)sender {
    if (!self.terminal) {
        self.terminal = [[TerminalView alloc] initWithDirectory:_root.path];
        [self.rightArea addSubview:self.terminal];

        self.termDivider = [[DragBar alloc] initWithFrame:NSZeroRect];
        self.termDivider.wantsLayer = YES;
        self.termDivider.layer.backgroundColor = Hex(0x333333).CGColor;
        __weak EditorController *weakSelf = self;
        self.termDivider.onDrag = ^(CGFloat y) {
            EditorController *s = weakSelf;
            CGFloat H = s.rightArea.bounds.size.height;
            s.terminalHeight = MIN(MAX(y, 80), MAX(120, H - 120));
            [s relayoutRightArea];
        };
        [self.rightArea addSubview:self.termDivider];
    }
    self.terminalVisible = !self.terminalVisible;
    [self relayoutRightArea];
    if (self.terminalVisible) [self.terminal focusInput];
    if (self.hintsVisible) self.hintsLabel.stringValue = [self hintsText];
}

- (void)toggleBrowser:(id)sender {
    if (!self.browser) {
        self.browser = [[BrowserView alloc]
            initWithHomeURL:@"https://duckduckgo.com"];
        [self.rightArea addSubview:self.browser];
    }
    self.browserVisible = !self.browserVisible;
    [self relayoutRightArea];
    if (self.browserVisible) [self.browser focusURLBar];
    if (self.hintsVisible) self.hintsLabel.stringValue = [self hintsText];
}

// ---- NSSplitViewDelegate: keep the sidebar a sane, fixed-ish width ----
- (BOOL)splitView:(NSSplitView *)sv shouldAdjustSizeOfSubview:(NSView *)view {
    // On window resize, grow/shrink the editor, not the sidebar.
    return view != sv.subviews.firstObject;
}
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMinCoordinate:(CGFloat)min
               ofSubviewAt:(NSInteger)i { return 160; }
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMaxCoordinate:(CGFloat)max
               ofSubviewAt:(NSInteger)i { return 480; }
- (BOOL)splitView:(NSSplitView *)sv canCollapseSubview:(NSView *)view {
    return NO;   // sidebar can't be dragged shut to nothing
}

// ------------------------------------------------------- NSOutlineView source
- (NSInteger)outlineView:(NSOutlineView *)ov numberOfChildrenOfItem:(id)item {
    FileItem *node = item ?: _root;
    [node loadChildren];
    return node.children.count;
}
- (id)outlineView:(NSOutlineView *)ov child:(NSInteger)i ofItem:(id)item {
    FileItem *node = item ?: _root;
    [node loadChildren];
    return node.children[i];
}
- (BOOL)outlineView:(NSOutlineView *)ov isItemExpandable:(id)item {
    return ((FileItem *)item).isDir;
}

- (NSView *)outlineView:(NSOutlineView *)ov
     viewForTableColumn:(NSTableColumn *)col
                   item:(id)item {
    FileItem *node = item;
    NSTableCellView *cell = [ov makeViewWithIdentifier:@"cell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = @"cell";
        NSTextField *tf = [NSTextField labelWithString:@""];
        tf.translatesAutoresizingMaskIntoConstraints = NO;
        NSImageView *iv = [[NSImageView alloc] init];
        iv.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:iv];
        [cell addSubview:tf];
        cell.textField = tf;
        cell.imageView = iv;
        [NSLayoutConstraint activateConstraints:@[
            [iv.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:2],
            [iv.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [iv.widthAnchor constraintEqualToConstant:16],
            [iv.heightAnchor constraintEqualToConstant:16],
            [tf.leadingAnchor constraintEqualToAnchor:iv.trailingAnchor constant:5],
            [tf.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-2],
            [tf.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    cell.textField.stringValue = [node name];
    cell.textField.textColor = Hex(0xCCCCCC);
    cell.textField.font = [NSFont systemFontOfSize:12.5];
    NSString *sym = node.isDir ? @"folder.fill" : @"doc.text";
    NSImage *img = [NSImage imageWithSystemSymbolName:sym
                             accessibilityDescription:nil];
    cell.imageView.image = img;
    cell.imageView.contentTintColor = node.isDir ? Hex(0xC09553) : Hex(0x8A99A8);
    return cell;
}

// Primary open path: driven from ClickOutline's mouseDown, so it always fires.
- (void)handleRowClick:(NSInteger)row atPoint:(NSPoint)pt {
    if (row < 0) return;
    FileItem *node = [self.outline itemAtRow:row];
    if (!node) return;
    if (node.isDir) {
        // Clicking the disclosure triangle: let the native handler toggle it.
        // Clicking the folder name: toggle it ourselves (VS Code behavior).
        NSRect triangle = [self.outline frameOfOutlineCellAtRow:row];
        if (NSPointInRect(pt, triangle)) return;
        if ([self.outline isItemExpanded:node]) [self.outline collapseItem:node];
        else                                    [self.outline expandItem:node];
        return;
    }
    [self openFileAtPath:node.path];
}

// Secondary path: keyboard arrow-key navigation also opens the highlighted file.
- (void)outlineViewSelectionDidChange:(NSNotification *)note {
    NSInteger row = self.outline.selectedRow;
    if (row < 0) return;
    FileItem *node = [self.outline itemAtRow:row];
    if (node.isDir) return;
    [self openFileAtPath:node.path];
}


// ------------------------------------------------------------ file rendering
- (void)openFileAtPath:(NSString *)path {
    if ([path isEqualToString:self.currentPath]) return;  // avoid double-render
    self.currentPath = path;
    NSError *err = nil;
    NSString *content = [NSString stringWithContentsOfFile:path
                                                  encoding:NSUTF8StringEncoding
                                                     error:&err];
    if (!content) {
        [self setPlainMessage:[NSString stringWithFormat:
            @"Cannot display “%@”.\n\n(Binary file or unsupported encoding.)",
            path.lastPathComponent]];
        [self setStatus:path];
        return;
    }
    NSString *ext = path.pathExtension.lowercaseString;
    self.sourceText = content;
    self.currentExt = ext;
    self.isMarkdown = [ext isEqualToString:@"md"] ||
                      [ext isEqualToString:@"markdown"];
    self.previewMode = self.isMarkdown;   // markdown opens rendered by default
    self.dirty = NO;
    [self refreshDisplay];
    [self updateTitle];
}

- (BOOL)canTogglePreview { return self.isMarkdown; }

// Decide what to show for the current file/mode.
- (void)refreshDisplay {
    if (self.isMarkdown && self.previewMode) {
        self.textView.editable = NO;
        [self renderMarkdown:self.sourceText];
    } else {
        [self displaySourceEditable];
    }
}

// Show the raw text as an editable, monospaced, syntax-highlighted document.
- (void)displaySourceEditable {
    NSFont *mono = [NSFont monospacedSystemFontOfSize:13
                                               weight:NSFontWeightRegular];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = 2.0;
    NSDictionary *base = @{
        NSFontAttributeName: mono,
        NSForegroundColorAttributeName: Hex(0xD4D4D4),
        NSParagraphStyleAttributeName: ps,
    };
    NSString *content = self.sourceText ?: @"";
    NSMutableAttributedString *attr =
        [[NSMutableAttributedString alloc] initWithString:content attributes:base];
    [self.textView.textStorage setAttributedString:attr];
    self.textView.typingAttributes = base;      // typed text stays monospaced
    self.textView.editable = YES;
    [self applyHighlighting];
    [self.textView scrollToBeginningOfDocument:nil];
}

// Recolor the current text storage in place (preserves cursor/selection).
- (void)applyHighlighting {
    std::string cext = self.currentExt.UTF8String ? self.currentExt.UTF8String : "";
    if (!SyntaxHighlighter::supports(cext)) return;

    NSTextStorage *storage = self.textView.textStorage;
    std::string text = storage.string.UTF8String ? storage.string.UTF8String : "";
    std::vector<Token> tokens = SyntaxHighlighter::highlight(text, cext);

    [storage beginEditing];
    NSRange full = NSMakeRange(0, storage.length);
    [storage addAttribute:NSForegroundColorAttributeName
                    value:Hex(0xD4D4D4) range:full];   // reset baseline
    for (const Token &t : tokens) {
        NSUInteger a = [self utf16IndexForByte:t.start inUTF8:text];
        NSUInteger b = [self utf16IndexForByte:t.start + t.length inUTF8:text];
        if (b <= a || b > storage.length) continue;
        [storage addAttribute:NSForegroundColorAttributeName
                        value:ColorForStyle(t.style)
                        range:NSMakeRange(a, b - a)];
    }
    [storage endEditing];
}

// -------------------------------------------------------------- editing hooks
- (void)textDidChange:(NSNotification *)note {
    self.sourceText = self.textView.string;
    if (!self.dirty) { self.dirty = YES; [self updateTitle]; }
    if (!(self.isMarkdown && self.previewMode)) [self applyHighlighting];
}

- (void)saveCurrentFile:(id)sender {
    if (!self.currentPath) return;
    // In markdown preview mode the text view holds rendered text, not source;
    // save the tracked source instead.
    NSString *text = self.textView.editable ? self.textView.string
                                            : (self.sourceText ?: @"");
    NSError *err = nil;
    BOOL ok = [text writeToFile:self.currentPath atomically:YES
                       encoding:NSUTF8StringEncoding error:&err];
    if (ok) {
        self.sourceText = text;
        self.dirty = NO;
        [self updateTitle];
    } else {
        NSAlert *a = [NSAlert alertWithError:err];
        [a runModal];
    }
}

- (void)togglePreview:(id)sender {
    if (!self.isMarkdown) {
        NSBeep();
        return;
    }
    if (self.textView.editable) self.sourceText = self.textView.string;  // keep edits
    self.previewMode = !self.previewMode;
    [self refreshDisplay];
    [self updateTitle];
}

- (void)updateTitle {
    NSString *name = self.currentPath.lastPathComponent ?: @"MiniCode";
    NSString *flag = self.dirty ? @"● " : @"";
    NSString *mode = (self.isMarkdown && self.previewMode) ? @"  [Preview]" : @"";
    self.window.title = [NSString stringWithFormat:@"%@%@ — MiniCode%@",
                         flag, name, mode];
    self.window.documentEdited = self.dirty;
    if (self.hintsVisible) self.hintsLabel.stringValue = [self hintsText];
}

// UTF-8 byte offset -> UTF-16 code-unit offset. ASCII-fast, correct for UTF-8.
- (NSUInteger)utf16IndexForByte:(size_t)byteOffset inUTF8:(const std::string &)s {
    NSUInteger u16 = 0;
    size_t i = 0;
    while (i < byteOffset && i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t adv; NSUInteger units;
        if (c < 0x80)      { adv = 1; units = 1; }
        else if (c < 0xE0) { adv = 2; units = 1; }
        else if (c < 0xF0) { adv = 3; units = 1; }
        else               { adv = 4; units = 2; } // surrogate pair
        i += adv; u16 += units;
    }
    return u16;
}

- (void)renderMarkdown:(NSString *)content {
    std::string md = content.UTF8String ? content.UTF8String : "";
    std::vector<MdRun> runs = MarkdownParser::parse(md);

    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    NSFont *body = [NSFont systemFontOfSize:15];
    NSFont *mono = [NSFont monospacedSystemFontOfSize:13
                                               weight:NSFontWeightRegular];

    for (const MdRun &r : runs) {
        NSString *s = [NSString stringWithUTF8String:r.text.c_str()];
        if (!s) continue;

        NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
        ps.lineSpacing = 3.0; ps.paragraphSpacing = 4.0;

        NSFont *font = body;
        NSColor *color = Hex(0xD4D4D4);
        NSMutableDictionary *a = [NSMutableDictionary dictionary];

        if (r.heading > 0) {
            CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
            font = [NSFont boldSystemFontOfSize:sizes[r.heading]];
            color = Hex(0xFFFFFF);
            ps.paragraphSpacing = 8.0;
        }
        if (r.codeBlock || r.code) {
            font = mono;
            color = Hex(0xCE9178);
            a[NSBackgroundColorAttributeName] = Hex(0x2A2A2A);
        }
        if (r.quote) {
            color = Hex(0x9CA3AF);
            ps.headIndent = 16; ps.firstLineHeadIndent = 16;
        }
        if (r.rule) {
            // Draw a rule as a full line of box-drawing chars.
            s = @"────────────────────────────────";
            color = Hex(0x555555);
        }
        if (r.link) { color = Hex(0x4EA1F7); a[NSUnderlineStyleAttributeName] =
            @(NSUnderlineStyleSingle); }

        NSFontManager *fm = [NSFontManager sharedFontManager];
        if (r.bold) font = [fm convertFont:font toHaveTrait:NSBoldFontMask];
        if (r.italic) font = [fm convertFont:font toHaveTrait:NSItalicFontMask];

        a[NSFontAttributeName] = font;
        a[NSForegroundColorAttributeName] = color;
        a[NSParagraphStyleAttributeName] = ps;

        [out appendAttributedString:
            [[NSAttributedString alloc] initWithString:s attributes:a]];
    }
    [self.textView.textStorage setAttributedString:out];
    [self.textView scrollToBeginningOfDocument:nil];
}

- (void)setPlainMessage:(NSString *)msg {
    NSDictionary *a = @{
        NSFontAttributeName: [NSFont systemFontOfSize:14],
        NSForegroundColorAttributeName: Hex(0x9CA3AF),
    };
    [self.textView.textStorage setAttributedString:
        [[NSAttributedString alloc] initWithString:msg attributes:a]];
}

- (void)showWelcome {
    [self setPlainMessage:
        @"\n  MiniCode — a native C++ editor for macOS\n\n"
         "  • Select a file in the sidebar to view it\n"
         "  • Source files are syntax-highlighted by type\n"
         "  • Markdown (.md) files render formatted\n\n"
         "  Cmd+O to open a different folder."];
}

- (void)setStatus:(NSString *)path {
    self.window.title = [NSString stringWithFormat:@"MiniCode — %@",
                         path.lastPathComponent];
}

// ------------------------------------------------------------- open folder
- (void)openFolder:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK) {
        NSString *dir = panel.URLs.firstObject.path;
        _root = [FileItem new];
        _root.path = dir; _root.isDir = YES;
        [_root loadChildren];
        self.currentPath = nil;
        [self.outline reloadData];
        [self showWelcome];
        [self.terminal setDirectory:dir];   // keep terminal cwd in sync
        self.window.title = [NSString stringWithFormat:@"MiniCode — %@",
                             dir.lastPathComponent];
    }
}

@end
