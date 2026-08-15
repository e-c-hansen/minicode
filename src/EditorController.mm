// EditorController.mm — Objective-C++. Bridges the pure-C++ core (syntax
// highlighter, markdown parser) to AppKit views.
#import "EditorController.h"
#import "Terminal.h"
#import "Browser.h"
#import "Search.h"
#import <CoreServices/CoreServices.h>   // FSEvents, for live file-tree updates
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

// When NO, entries beginning with "." are hidden from the tree (default).
static BOOL gShowHidden = NO;

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
        if (!gShowHidden && [entry hasPrefix:@"."]) continue;   // hide dotfiles
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

// Re-scan this directory, reusing existing child objects for paths that still
// exist so the outline keeps its expanded and selected state. Recurses into
// any subdirectory that was already loaded.
- (void)refresh {
    if (!self.isDir || !self.loaded) return;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *entries = [[fm contentsOfDirectoryAtPath:self.path error:nil]
        sortedArrayUsingComparator:^(NSString *a, NSString *b) {
            return [a caseInsensitiveCompare:b];
        }];

    NSMutableDictionary<NSString *, FileItem *> *existing =
        [NSMutableDictionary dictionary];
    for (FileItem *c in self.children) existing[c.path] = c;

    NSMutableArray *dirs = [NSMutableArray array];
    NSMutableArray *files = [NSMutableArray array];
    for (NSString *entry in entries) {
        if (!gShowHidden && [entry hasPrefix:@"."]) continue;
        NSString *full = [self.path stringByAppendingPathComponent:entry];
        BOOL d = NO;
        [fm fileExistsAtPath:full isDirectory:&d];
        FileItem *item = existing[full];
        if (!item || item.isDir != d) {          // new, or type changed
            item = [FileItem new];
            item.path = full; item.isDir = d;
        }
        [(d ? dirs : files) addObject:item];
    }
    self.children = [NSMutableArray array];
    [self.children addObjectsFromArray:dirs];
    [self.children addObjectsFromArray:files];
    for (FileItem *c in self.children)           // recurse into open subdirs
        if (c.isDir && c.loaded) [c refresh];
}
@end

// ---------------------------------------------------------------------------
// ClickOutline: handles a click in mouseDown directly, so opening a file never
// depends on NSOutlineView's internal selection/tracking loop (which was
// swallowing single clicks in this layout).
// ---------------------------------------------------------------------------
@interface ClickOutline : NSOutlineView
@property(nonatomic, copy) void (^onRowClick)(NSInteger row, NSPoint pointInView);
@property(nonatomic, copy) void (^onActivate)(NSInteger row);   // Return / Enter
@end

@implementation ClickOutline
- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:p];
    if (row >= 0 && self.onRowClick) self.onRowClick(row, p);
    [super mouseDown:event];   // keep native selection / expansion visuals
}
- (void)keyDown:(NSEvent *)event {
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length == 1) {
        unichar c = [chars characterAtIndex:0];
        if ((c == NSCarriageReturnCharacter || c == NSEnterCharacter) &&
            self.onActivate) {
            self.onActivate(self.selectedRow);
            return;
        }
    }
    [super keyDown:event];   // arrows, left/right expand-collapse stay native
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
    NSMutableArray<NSString *> *_recent;   // most-recently-opened files, front = newest
    FSEventStreamRef _fsStream;            // watches the open folder for changes
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
@property(nonatomic, strong) NSDate *fileModDate;    // on-disk mtime we last saw
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
@property(nonatomic, assign) BOOL editorCollapsed;   // terminal owns the area
@property(nonatomic, strong) NSSplitView *splitView;
@property(nonatomic, strong) NSScrollView *sidebarScroll;
@property(nonatomic, assign) BOOL sidebarCollapsed;
@property(nonatomic, strong) SearchPanel *searchPanel;
@end

@implementation EditorController

- (instancetype)initWithRootPath:(NSString *)path {
    if ((self = [super init])) {
        _root = [FileItem new];
        _root.path = path; _root.isDir = YES;
        [_root loadChildren];
        _terminalHeight = 220;
        _recent = [NSMutableArray array];
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
    // ARC owns the window through the strong `window` property. NSWindow defaults
    // to releasedWhenClosed=YES, which adds a legacy unbalanced release on close;
    // combined with the ARC release that's a double-free that crashes on close.
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    self.window.minSize = NSMakeSize(640, 400);
    [self.window center];

    // --- split view: sidebar | editor
    NSSplitView *split = [[NSSplitView alloc] initWithFrame:frame];
    self.splitView = split;
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    split.delegate = self;   // pins sidebar width, prevents collapse-to-zero

    // --- sidebar (file tree)
    NSScrollView *treeScroll = [[NSScrollView alloc] init];
    self.sidebarScroll = treeScroll;
    treeScroll.hasVerticalScroller = YES;
    treeScroll.drawsBackground = YES;
    treeScroll.backgroundColor = Hex(0x252526);

    self.outline = [[ClickOutline alloc] init];
    __weak EditorController *weakSelf = self;
    self.outline.onRowClick = ^(NSInteger row, NSPoint pt) {
        [weakSelf handleRowClick:row atPoint:pt];
    };
    self.outline.onActivate = ^(NSInteger row) {
        [weakSelf activateRow:row];
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
    self.outline.menu = [self buildTreeContextMenu];
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
    self.textView.usesFindBar = YES;               // Cmd+F find bar
    self.textView.incrementalSearchingEnabled = YES;
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
    // Custom hotkeys (Ctrl+`, Cmd+B) are handled by a global key monitor in the
    // app delegate, so they work regardless of which pane has focus.
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

    [self startWatching:_root.path];   // live tree updates
}

- (void)dealloc { [self stopWatching]; }

- (NSMenu *)buildTreeContextMenu {
    NSMenu *m = [[NSMenu alloc] init];
    [m addItemWithTitle:@"New File…" action:@selector(newFile:) keyEquivalent:@""];
    [m addItemWithTitle:@"New Folder…" action:@selector(newFolder:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Rename…" action:@selector(renameSelected:) keyEquivalent:@""];
    [m addItemWithTitle:@"Move to Trash" action:@selector(deleteSelected:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Reveal in Finder" action:@selector(revealInFinder:) keyEquivalent:@""];
    [m addItemWithTitle:@"Refresh" action:@selector(refreshTree:) keyEquivalent:@""];
    for (NSMenuItem *it in m.itemArray) it.target = self;
    return m;
}

// -------------------------------------------------------- status bar + hints
- (void)buildStatusBarInContainer:(NSView *)container height:(CGFloat)barH {
    self.statusBar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, container.bounds.size.width, barH)];
    self.statusBar.wantsLayer = YES;
    self.statusBar.layer.backgroundColor = Hex(0x007ACC).CGColor;  // VS Code blue
    self.statusBar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;

    NSTextField *label = [NSTextField labelWithString:@"⇧⌘H  Shortcuts"];
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

// Set the hint text and size the panel to fit it, anchored to the top-right.
- (void)updateHints {
    if (!self.hintsLabel) return;
    NSString *text = [self hintsText];
    self.hintsLabel.stringValue = text;

    NSView *container = self.hintsPanel.superview;
    const CGFloat pw = 380, pad = 16, margin = 18;
    NSRect textRect = [text boundingRectWithSize:NSMakeSize(pw - 2 * pad, 10000)
        options:NSStringDrawingUsesLineFragmentOrigin
     attributes:@{NSFontAttributeName: self.hintsLabel.font}];
    CGFloat ph = ceil(textRect.size.height) + 2 * pad;
    self.hintsPanel.frame = NSMakeRect(
        container.bounds.size.width - pw - margin,
        container.bounds.size.height - ph - margin, pw, ph);
    self.hintsLabel.frame = NSMakeRect(pad, pad, pw - 2 * pad, ph - 2 * pad);
}

// Context-aware shortcut list.
- (NSString *)hintsText {
    NSMutableString *s = [NSMutableString string];
    [s appendString:@"Keyboard Shortcuts\n"];
    [s appendString:@"──────────────────────────\n"];
    [s appendString:@"⌘N    New window     ⌘O   Open folder\n"];
    [s appendString:@"⌘S    Save           ⌘F   Find in file\n"];
    [s appendString:@"⇧⌘F   Find in folder  ⌘Z   Undo   ⇧⌘Z  Redo\n"];
    [s appendString:@"⌘C    Copy           ⌘A   Select all\n"];

    [s appendString:@"\nFiles\n"];
    [s appendString:@"──────────────────────────\n"];
    [s appendString:@"⌃⌘N   New file       ⇧⌘N  New folder\n"];
    [s appendString:@"⌘⌫    Move to trash   ⌘R   Refresh tree\n"];
    [s appendString:@"⇧⌘.   Show hidden files\n"];

    [s appendString:@"\nNavigation & panes\n"];
    [s appendString:@"──────────────────────────\n"];
    [s appendString:@"⌘0    Focus tree      ⌘1   Focus editor\n"];
    [s appendString:@"↑ ↓   Browse tree     ⏎    Open   ⌃⇥  Previous\n"];
    [s appendString:@"⌘B    Toggle sidebar\n"];
    [s appendFormat:@"⇧⌘E   Editor    (%@)\n",
        self.editorCollapsed ? @"collapsed" : @"shown"];
    [s appendFormat:@"⌘T / ⌃`   Terminal  (%@)\n",
        self.terminalVisible ? @"open" : @"hidden"];
    [s appendFormat:@"⇧⌘B   Browser   (%@)\n",
        self.browserVisible ? @"open" : @"hidden"];
    if (self.isMarkdown) {
        [s appendFormat:@"⇧⌘P   Markdown preview  (now: %@)\n",
            self.previewMode ? @"rendered" : @"source"];
    }
    [s appendString:@"\n⇧⌘H   Hide these hints"];
    return s;
}

- (void)toggleHints:(id)sender {
    self.hintsVisible = !self.hintsVisible;
    if (self.hintsVisible) [self updateHints];
    self.hintsPanel.hidden = !self.hintsVisible;
    self.statusLabel.stringValue =
        self.hintsVisible ? @"⇧⌘H  Hide shortcuts" : @"⇧⌘H  Shortcuts";
}

// -------------------------------------------------------- terminal + browser
// Dock the terminal at the bottom; the editor OR the browser fills the top.
- (void)relayoutRightArea {
    NSRect b = self.rightArea.bounds;
    CGFloat W = b.size.width, H = b.size.height;
    BOOL showTerm = self.terminalVisible && self.terminal != nil;

    // Collapsing only means anything when there is a terminal to hand the space
    // to. Requiring showTerm here is also the backstop that keeps an
    // inconsistent state from producing an empty window.
    BOOL collapsed = self.editorCollapsed && showTerm;

    CGFloat termH = 0;
    if (collapsed)     termH = H;
    else if (showTerm) termH = MIN(MAX(self.terminalHeight, 80), MAX(120, H - 120));
    CGFloat topH = MAX(0, H - termH);
    NSRect topRect = NSMakeRect(0, termH, W, topH);

    BOOL showBrowser = self.browserVisible && self.browser != nil;
    self.editorScroll.frame = topRect;
    self.editorScroll.hidden = showBrowser || collapsed;
    if (self.browser) {
        self.browser.frame = topRect;
        self.browser.hidden = !showBrowser || collapsed;
    }
    if (self.terminal) {
        self.terminal.frame = NSMakeRect(0, 0, W, termH);
        self.terminal.hidden = !showTerm;
    }
    if (self.termDivider) {
        self.termDivider.frame = NSMakeRect(0, termH - 3, W, 6);
        // Nothing above the terminal to size against while collapsed, so the
        // drag handle would only be a way to get into a confusing state.
        self.termDivider.hidden = !showTerm || collapsed;
    }
}

// Give the terminal the whole right area. The drag handle deliberately stops at
// 120px of editor, which is right for dragging and wrong for "I want the
// terminal and nothing else"; this is the separate gesture for the latter, the
// same way Cmd+B collapses the sidebar past the split's own 160px minimum.
- (void)toggleEditor:(id)sender {
    if (self.editorCollapsed) {
        self.editorCollapsed = NO;
    } else {
        // Collapsing with nothing below would leave an empty window.
        if (!self.terminalVisible) [self toggleTerminal:nil];
        self.editorCollapsed = YES;
    }
    [self relayoutRightArea];
    if (self.editorCollapsed) [self.terminal focusInput];
    if (self.hintsVisible) [self updateHints];
}

// The single place the editor comes back, so it cannot stay hidden behind
// content that has nowhere to appear.
- (void)restoreEditorArea {
    if (!self.editorCollapsed) return;
    self.editorCollapsed = NO;
    [self relayoutRightArea];
    if (self.hintsVisible) [self updateHints];
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
    // Closing the terminal while the editor is collapsed would leave an empty
    // window, so the editor comes back with it.
    if (!self.terminalVisible) self.editorCollapsed = NO;
    [self relayoutRightArea];
    if (self.terminalVisible) [self.terminal focusInput];
    if (self.hintsVisible) [self updateHints];
}

- (void)toggleBrowser:(id)sender {
    if (!self.browser) {
        self.browser = [[BrowserView alloc]
            initWithHomeURL:@"https://duckduckgo.com"];
        [self.rightArea addSubview:self.browser];
    }
    self.browserVisible = !self.browserVisible;
    self.editorCollapsed = NO;   // the browser lives in the top area too
    [self relayoutRightArea];
    if (self.browserVisible) [self.browser focusURLBar];
    if (self.hintsVisible) [self updateHints];
}

// ---- NSSplitViewDelegate: keep the sidebar a sane, fixed-ish width ----
- (BOOL)splitView:(NSSplitView *)sv shouldAdjustSizeOfSubview:(NSView *)view {
    // On window resize, grow/shrink the editor, not the sidebar.
    return view != sv.subviews.firstObject;
}
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMinCoordinate:(CGFloat)min
               ofSubviewAt:(NSInteger)i {
    return self.sidebarCollapsed ? 0 : 160;   // allow full collapse via Cmd+B
}
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMaxCoordinate:(CGFloat)max
               ofSubviewAt:(NSInteger)i { return 480; }

// Cmd+B: collapse or restore the file-tree sidebar.
- (void)toggleSidebar:(id)sender {
    self.sidebarCollapsed = !self.sidebarCollapsed;
    [self.splitView setPosition:(self.sidebarCollapsed ? 0 : 260)
               ofDividerAtIndex:0];
}

// Cmd+Shift+. : show or hide dotfiles in the tree (like Finder).
- (void)toggleHiddenFiles:(id)sender {
    gShowHidden = !gShowHidden;
    [self refreshTree:nil];
}

// Cmd+Shift+F : project-wide text search.
- (void)openSearch:(id)sender {
    if (!self.searchPanel) {
        __weak EditorController *weakSelf = self;
        self.searchPanel = [[SearchPanel alloc]
            initWithRoot:_root.path
             openHandler:^(NSString *path, NSInteger line) {
                EditorController *s = weakSelf;
                [s openFileAtPath:path];
                [s jumpToLine:line];
                [s.window makeKeyAndOrderFront:nil];
            }];
    }
    // If a folder is selected in the tree, scope the search to it; otherwise
    // leave the scope as it is (the opened folder by default).
    NSInteger row = self.outline.selectedRow;
    if (row >= 0) {
        FileItem *node = [self.outline itemAtRow:row];
        if (node.isDir) [self.searchPanel setScope:node.path];
    }
    [self.searchPanel show];
}

// Move the editor selection to a 1-based line and reveal it.
- (void)jumpToLine:(NSInteger)line {
    if (line < 1) return;
    NSString *s = self.textView.string;
    __block NSUInteger loc = NSNotFound;
    __block NSInteger n = 1;
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length)
                          options:NSStringEnumerationByLines
                       usingBlock:^(NSString *sub, NSRange r, NSRange e, BOOL *stop) {
        (void)sub; (void)e;
        if (n == line) { loc = r.location; *stop = YES; }
        n++;
    }];
    if (loc == NSNotFound) return;
    NSRange lineRange = [s lineRangeForRange:NSMakeRange(loc, 0)];
    [self.textView setSelectedRange:lineRange];
    [self.textView scrollRangeToVisible:lineRange];
    [self.window makeFirstResponder:self.textView];
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

// Keyboard activation (Return in the tree): open a file, or toggle a folder.
// Arrow keys only move the selection; they do not open, so you can browse
// freely without triggering loads or unsaved-changes prompts.
- (void)activateRow:(NSInteger)row {
    if (row < 0) return;
    FileItem *node = [self.outline itemAtRow:row];
    if (!node) return;
    if (node.isDir) {
        if ([self.outline isItemExpanded:node]) [self.outline collapseItem:node];
        else                                    [self.outline expandItem:node];
        return;
    }
    [self openFileAtPath:node.path];
    [self focusEditor:nil];   // opened it to work on it
}

// --------------------------------------------------- keyboard focus + switch
- (void)focusTree:(id)sender {
    if (self.outline.selectedRow < 0 && self.outline.numberOfRows > 0)
        [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
                  byExtendingSelection:NO];
    [self.window makeFirstResponder:self.outline];
}
- (void)focusEditor:(id)sender {
    [self.window makeFirstResponder:self.textView];
}
- (void)switchToPreviousFile:(id)sender {
    // _recent is most-recent-first; index 1 is the file before the current one.
    if (_recent.count < 2) { NSBeep(); return; }
    [self openFileAtPath:_recent[1]];
}

// ------------------------------------------------ live file-tree (FSEvents)
static void FSCallback(ConstFSEventStreamRef stream, void *info, size_t n,
                       void *paths, const FSEventStreamEventFlags flags[],
                       const FSEventStreamEventId ids[]) {
    (void)stream; (void)n; (void)paths; (void)flags; (void)ids;
    EditorController *self = (__bridge EditorController *)info;
    [self refreshTree:nil];
}

- (void)startWatching:(NSString *)path {
    [self stopWatching];
    if (!path) return;
    FSEventStreamContext ctx = {0, (__bridge void *)self, NULL, NULL, NULL};
    CFStringRef cfPath = (__bridge CFStringRef)path;
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&cfPath, 1, NULL);
    _fsStream = FSEventStreamCreate(
        NULL, &FSCallback, &ctx, paths, kFSEventStreamEventIdSinceNow,
        0.3 /* seconds of coalescing */, kFSEventStreamCreateFlagNone);
    CFRelease(paths);
    if (_fsStream) {
        FSEventStreamSetDispatchQueue(_fsStream, dispatch_get_main_queue());
        FSEventStreamStart(_fsStream);
    }
}

- (void)stopWatching {
    if (!_fsStream) return;
    FSEventStreamStop(_fsStream);
    FSEventStreamInvalidate(_fsStream);
    FSEventStreamRelease(_fsStream);
    _fsStream = NULL;
}

// Re-scan the tree, keeping expanded folders and the selection where possible.
- (void)refreshTree:(id)sender {
    NSString *selPath = nil;
    if (self.outline.selectedRow >= 0)
        selPath = [(FileItem *)[self.outline itemAtRow:self.outline.selectedRow] path];

    NSMutableSet<NSString *> *expanded = [NSMutableSet set];
    for (NSInteger r = 0; r < self.outline.numberOfRows; r++) {
        FileItem *it = [self.outline itemAtRow:r];
        if ([self.outline isItemExpanded:it]) [expanded addObject:it.path];
    }

    [_root refresh];
    [self.outline reloadData];
    [self reExpand:_root usingSet:expanded];

    if (selPath) {
        for (NSInteger r = 0; r < self.outline.numberOfRows; r++) {
            if ([[(FileItem *)[self.outline itemAtRow:r] path] isEqual:selPath]) {
                [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:r]
                          byExtendingSelection:NO];
                break;
            }
        }
    }
}

- (void)reExpand:(FileItem *)node usingSet:(NSSet<NSString *> *)expanded {
    for (FileItem *child in node.children) {
        if (child.isDir && [expanded containsObject:child.path]) {
            [self.outline expandItem:child];
            [self reExpand:child usingSet:expanded];
        }
    }
}

// -------------------------------------------------- file operations (tree)
// Where a new file/folder should go: the selected folder, the selected file's
// folder, or the tree root.
- (NSString *)targetDirectory {
    NSInteger row = self.outline.clickedRow >= 0 ? self.outline.clickedRow
                                                 : self.outline.selectedRow;
    if (row < 0) return _root.path;
    FileItem *node = [self.outline itemAtRow:row];
    return node.isDir ? node.path : [node.path stringByDeletingLastPathComponent];
}

- (FileItem *)clickedOrSelectedItem {
    NSInteger row = self.outline.clickedRow >= 0 ? self.outline.clickedRow
                                                 : self.outline.selectedRow;
    return row >= 0 ? [self.outline itemAtRow:row] : nil;
}

// A simple modal name prompt. Returns nil if cancelled or empty.
- (NSString *)promptForName:(NSString *)title default:(NSString *)initial {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    NSTextField *field =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    field.stringValue = initial ?: @"";
    a.accessoryView = field;
    [a.window setInitialFirstResponder:field];
    if ([a runModal] != NSAlertFirstButtonReturn) return nil;
    NSString *name = [field.stringValue stringByTrimmingCharactersInSet:
                      [NSCharacterSet whitespaceCharacterSet]];
    return name.length ? name : nil;
}

- (void)newFile:(id)sender {
    NSString *name = [self promptForName:@"New file name:" default:@""];
    if (!name) return;
    NSString *path = [[self targetDirectory] stringByAppendingPathComponent:name];
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:path]) { [self warn:@"A file with that name already exists."]; return; }
    if (![fm createFileAtPath:path contents:[NSData data] attributes:nil]) {
        [self warn:@"Could not create the file."]; return;
    }
    [self refreshTree:nil];
    [self revealPath:path andOpen:YES];
}

- (void)newFolder:(id)sender {
    NSString *name = [self promptForName:@"New folder name:" default:@""];
    if (!name) return;
    NSString *path = [[self targetDirectory] stringByAppendingPathComponent:name];
    NSError *err = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:path
            withIntermediateDirectories:NO attributes:nil error:&err]) {
        [self warn:err.localizedDescription]; return;
    }
    [self refreshTree:nil];
    [self revealPath:path andOpen:NO];
}

- (void)renameSelected:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    if (!node) { NSBeep(); return; }
    NSString *name = [self promptForName:@"Rename to:"
                                 default:node.path.lastPathComponent];
    if (!name) return;
    NSString *dst = [[node.path stringByDeletingLastPathComponent]
                        stringByAppendingPathComponent:name];
    NSError *err = nil;
    if (![[NSFileManager defaultManager] moveItemAtPath:node.path
                                                 toPath:dst error:&err]) {
        [self warn:err.localizedDescription]; return;
    }
    if ([self.currentPath isEqual:node.path]) self.currentPath = dst;  // keep editor in sync
    [self refreshTree:nil];
    [self revealPath:dst andOpen:NO];
}

- (void)deleteSelected:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    if (!node) { NSBeep(); return; }
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Move “%@” to the Trash?",
                     node.path.lastPathComponent];
    [a addButtonWithTitle:@"Move to Trash"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    NSError *err = nil;
    NSURL *url = [NSURL fileURLWithPath:node.path];
    if (![[NSFileManager defaultManager] trashItemAtURL:url
                                       resultingItemURL:nil error:&err]) {
        [self warn:err.localizedDescription]; return;
    }
    if ([self.currentPath isEqual:node.path]) {   // the open file went away
        self.currentPath = nil; self.dirty = NO;
        [self showWelcome]; [self updateTitle];
    }
    [self refreshTree:nil];
}

- (void)revealInFinder:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    NSString *p = node ? node.path : _root.path;
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:
        @[[NSURL fileURLWithPath:p]]];
}

// Expand ancestor folders down to path, select it, and optionally open it.
- (void)revealPath:(NSString *)path andOpen:(BOOL)open {
    NSArray<NSString *> *rootParts = _root.path.pathComponents;
    NSArray<NSString *> *parts = path.pathComponents;
    FileItem *node = _root;
    NSString *acc = _root.path;
    for (NSUInteger i = rootParts.count; i < parts.count; i++) {
        acc = [acc stringByAppendingPathComponent:parts[i]];
        [node loadChildren];
        FileItem *next = nil;
        for (FileItem *c in node.children)
            if ([c.path isEqual:acc]) { next = c; break; }
        if (!next) break;
        if (next.isDir && i < parts.count - 1) [self.outline expandItem:next];
        node = next;
    }
    NSInteger row = [self.outline rowForItem:node];
    if (row >= 0) {
        [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                  byExtendingSelection:NO];
        [self.outline scrollRowToVisible:row];
    }
    if (open && !node.isDir) [self openFileAtPath:node.path];
}

- (void)warn:(NSString *)msg {
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = msg;
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}


// ------------------------------------------------------------ file rendering
- (void)openFileAtPath:(NSString *)path {
    // Before the early return: clicking the already-open file while the editor
    // is collapsed is a request to see it again, not a no-op.
    [self restoreEditorArea];
    if ([path isEqualToString:self.currentPath]) return;  // avoid double-render
    if (![self confirmProceedPastUnsavedChanges]) {
        [self reselectCurrentFileInTree];   // undo the tree's selection move
        return;
    }
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
    [self recordModDate];
    [_recent removeObject:path];
    [_recent insertObject:path atIndex:0];   // newest first
    [self refreshDisplay];
    [self updateTitle];
}

// The on-disk modification time of the current file, or nil.
- (NSDate *)diskModDateFor:(NSString *)path {
    if (!path) return nil;
    NSDictionary *attrs = [[NSFileManager defaultManager]
        attributesOfItemAtPath:path error:nil];
    return attrs.fileModificationDate;
}
- (void)recordModDate { self.fileModDate = [self diskModDateFor:self.currentPath]; }

- (void)reselectCurrentFileInTree {
    for (NSInteger row = 0; row < self.outline.numberOfRows; row++) {
        FileItem *node = [self.outline itemAtRow:row];
        if ([node.path isEqualToString:self.currentPath]) {
            [self.outline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                      byExtendingSelection:NO];
            return;
        }
    }
}

// Returns YES if it is safe to replace the current buffer. Prompts on dirty.
- (BOOL)confirmProceedPastUnsavedChanges {
    if (!self.dirty) return YES;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Save changes to “%@”?",
                     self.currentPath.lastPathComponent ?: @"this file"];
    a.informativeText = @"Your changes will be lost if you don't save them.";
    [a addButtonWithTitle:@"Save"];         // 1000
    [a addButtonWithTitle:@"Don't Save"];   // 1001
    [a addButtonWithTitle:@"Cancel"];       // 1002
    NSModalResponse r = [a runModal];
    if (r == NSAlertFirstButtonReturn) { [self saveCurrentFile:nil]; return !self.dirty; }
    if (r == NSAlertSecondButtonReturn) return YES;   // discard
    return NO;                                          // cancel
}

// Called when the window regains focus: reconcile with on-disk changes.
- (void)checkExternalChange {
    if (!self.currentPath) return;
    NSDate *disk = [self diskModDateFor:self.currentPath];
    if (!disk || !self.fileModDate) return;
    if ([disk isEqualToDate:self.fileModDate]) return;   // unchanged

    self.fileModDate = disk;
    if (!self.dirty) {                       // no local edits: reload quietly
        NSString *fresh = [NSString stringWithContentsOfFile:self.currentPath
                                                    encoding:NSUTF8StringEncoding
                                                       error:nil];
        if (fresh) { self.sourceText = fresh; [self refreshDisplay]; }
        return;
    }
    NSAlert *a = [[NSAlert alloc] init];     // local edits AND disk changed
    a.messageText = [NSString stringWithFormat:
        @"“%@” changed on disk.", self.currentPath.lastPathComponent];
    a.informativeText = @"You have unsaved changes here. Keep your version, "
                         "or reload the file from disk and lose them?";
    [a addButtonWithTitle:@"Keep Mine"];     // 1000
    [a addButtonWithTitle:@"Reload"];        // 1001
    if ([a runModal] == NSAlertSecondButtonReturn) {
        NSString *fresh = [NSString stringWithContentsOfFile:self.currentPath
                                                    encoding:NSUTF8StringEncoding
                                                       error:nil];
        if (fresh) {
            self.sourceText = fresh; self.dirty = NO;
            [self refreshDisplay]; [self updateTitle];
        }
    }
}

// -------------------------------------------------------- NSWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    return [self confirmProceedPastUnsavedChanges];
}
- (void)windowDidBecomeKey:(NSNotification *)note {
    [self checkExternalChange];
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
    if (!(self.isMarkdown && self.previewMode)) {
        // Coalesce rapid keystrokes: re-highlight ~120ms after typing pauses
        // instead of re-lexing the whole file on every keypress.
        [NSObject cancelPreviousPerformRequestsWithTarget:self
                                                 selector:@selector(applyHighlighting)
                                                   object:nil];
        [self performSelector:@selector(applyHighlighting)
                   withObject:nil
                   afterDelay:0.12];
    }
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
        [self recordModDate];   // so our own save doesn't look like an external change
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
    if (self.hintsVisible) [self updateHints];
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
            ps.paragraphSpacingBefore = r.heading <= 2 ? 18.0 : 12.0;  // gap above
        }
        if (r.codeBlock || r.code) {
            font = mono;
            color = Hex(0xCE9178);
            a[NSBackgroundColorAttributeName] = Hex(0x2A2A2A);
        }
        if (r.table) {
            font = mono;   // monospace keeps the padded columns aligned
            color = r.bold ? Hex(0xFFFFFF) : Hex(0xD4D4D4);
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
    if (![self confirmProceedPastUnsavedChanges]) return;
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
        self.dirty = NO;
        [self.outline reloadData];
        [self showWelcome];
        [self.terminal setDirectory:dir];   // keep terminal cwd in sync
        [self startWatching:dir];           // watch the new folder
        self.window.title = [NSString stringWithFormat:@"MiniCode — %@",
                             dir.lastPathComponent];
    }
}

@end
