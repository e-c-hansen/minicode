// EditorController.mm — Objective-C++. Bridges the pure-C++ core (syntax
// highlighter, markdown parser) to AppKit views.
#import "EditorController.h"
#import "Terminal.h"
#import "Browser.h"
#import "Latex.h"
#import "Search.h"
#import "AppSettings.h"
#import "Lsp.h"
#import <CoreServices/CoreServices.h>   // FSEvents, for live file-tree updates
#include "SyntaxHighlighter.h"
#include "MarkdownParser.h"
#include "LineComments.h"
#include <memory>
#include <string>

// A plain container that relays every resize to a layout block, so we can
// dock the editor/browser/terminal manually (predictable, no split-view math).
@interface PanelHost : NSView
@property(nonatomic, copy) void (^onLayout)(void);
@end
@implementation PanelHost
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; if (self.onLayout) self.onLayout(); }
@end

// Drag handle for resizing the terminal dock. The view is a generous grab
// area centered on the boundary but draws only a 1px line, which lights up
// while hovered or dragged. Reports the new boundary Y (in its superview's
// coordinates) to a block, keeping the offset where the bar was grabbed so it
// doesn't jump. The host must keep it above neighboring views, or they take
// the clicks.
@interface DragBar : NSView
@property(nonatomic, copy) void (^onDrag)(CGFloat yInSuperview);
@end
@implementation DragBar {
    CGFloat _grabOffset;   // pointer Y minus boundary Y at mouseDown
    BOOL _hot;             // hovered or dragging
    BOOL _dragging;
}
static const CGFloat kDragBarHeight = 12;

- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingCursorUpdate |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:nil]];
}

// Cursor rects lose to overlapping text views; a tracking-area cursor update
// goes to whichever view is on top under the pointer.
- (void)cursorUpdate:(NSEvent *)event { [[NSCursor resizeUpDownCursor] set]; }

- (void)setHot:(BOOL)hot {
    if (hot == _hot) return;
    _hot = hot;
    self.needsDisplay = YES;
}
- (void)mouseEntered:(NSEvent *)event { [self setHot:YES]; }
- (void)mouseExited:(NSEvent *)event { if (!_dragging) [self setHot:NO]; }

- (CGFloat)boundaryY { return NSMidY(self.frame); }

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self.superview convertPoint:event.locationInWindow fromView:nil];
    _grabOffset = p.y - [self boundaryY];
    _dragging = YES;
    [self setHot:YES];
    [[NSCursor resizeUpDownCursor] set];
}
- (void)mouseDragged:(NSEvent *)event {
    NSPoint p = [self.superview convertPoint:event.locationInWindow fromView:nil];
    [[NSCursor resizeUpDownCursor] set];
    if (self.onDrag) self.onDrag(p.y - _grabOffset);
}
- (void)mouseUp:(NSEvent *)event {
    _dragging = NO;
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    [self setHot:NSPointInRect(p, self.bounds)];
}

- (void)drawRect:(NSRect)dirty {
    NSRect b = self.bounds;
    CGFloat h = _hot ? 2 : 1;
    NSRect line = NSMakeRect(0, floor(NSMidY(b) - h / 2), b.size.width, h);
    [(_hot ? [NSColor colorWithSRGBRed:0x4E / 255.0 green:0xA1 / 255.0
                                  blue:0xF7 / 255.0 alpha:1]
           : [NSColor colorWithSRGBRed:0x33 / 255.0 green:0x33 / 255.0
                                  blue:0x33 / 255.0 alpha:1]) set];
    NSRectFill(line);
}
@end

// A scroll view whose panel background is a plain layer color behind the clip
// view, which itself draws nothing. A clip view's own translucent
// backgroundColor comes out opaque; a layer color composites correctly over
// the blur or the desktop. NSScrollView lays out only its own parts, so the
// backdrop is sized in tile.
@interface PanelScrollView : NSScrollView
@property(nonatomic, strong) NSColor *panelColor;
@end
@implementation PanelScrollView {
    NSView *_backdrop;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.drawsBackground = NO;
        _backdrop = [[NSView alloc] initWithFrame:self.bounds];
        _backdrop.wantsLayer = YES;
        [self addSubview:_backdrop positioned:NSWindowBelow relativeTo:self.contentView];
    }
    return self;
}
- (void)tile {
    [super tile];
    _backdrop.frame = self.bounds;
}
- (void)setPanelColor:(NSColor *)color {
    _panelColor = color;
    _backdrop.layer.backgroundColor = color.CGColor;
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

static const CGFloat kStatusBarHeight = 24;

static std::u16string U16(NSString *s) {
    std::u16string u(s.length, u'\0');
    [s getCharacters:(unichar *)u.data() range:NSMakeRange(0, s.length)];
    return u;
}
static NSString *FromU16(const std::u16string &u) {
    return [NSString stringWithCharacters:(const unichar *)u.data() length:u.size()];
}

// Link value marking a clickable color in the settings file.
static NSString *const kColorLink = @"minicode-color";

// Panel, text and syntax colors come from the settings file (AppSettings);
// Hex is for the fixed accents that aren't configurable.
static NSColor *ColorForStyle(TokenStyle s) {
    return [[AppSettings shared] syntax:s];
}

// The editor's text as the incremental highlighter reads it: UTF-16 straight
// out of the text storage, a line at a time, never copied whole.
class NSStringSource : public TextSource<char16_t> {
public:
    explicit NSStringSource(NSString *s) : s_(s) {}
    size_t length() const override { return s_.length; }
    void read(size_t pos, size_t len, char16_t *out) const override {
        [s_ getCharacters:(unichar *)out range:NSMakeRange(pos, len)];
    }
private:
    NSString *s_;
};

// ---------------------------------------------------------------------------
@interface EditorController () <NSTextStorageDelegate> {
    FileItem *_root;
    NSMutableArray<NSString *> *_recent;   // most-recently-opened files, front = newest
    FSEventStreamRef _fsStream;            // watches the open folder for changes
    // Syntax highlighting of the editable source. While _liveHighlight is on,
    // every change to the text storage is re-highlighted as it happens, from
    // the storage delegate; _hl (null for a language without a grammar)
    // tracks line states so only the changed lines are re-lexed.
    std::unique_ptr<IncrementalHighlighter<char16_t>> _hl;
    BOOL _liveHighlight;
    NSUInteger _pendingStart, _pendingEnd; // edited, not yet recolored (NSNotFound: none)
    BOOL _flushScheduled;
    BOOL _highlightingSettingsFile;        // give color values swatches
    NSColor *_styleColors[8];              // by TokenStyle, from the settings
    NSColor *_plainColor;
    NSDictionary *_sourceAttributes;       // font, paragraph style, plain color
}
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) ClickOutline *outline;
@property(nonatomic, strong) CodeTextView *textView;
@property(nonatomic, strong) NSTextField *statusLabel;
@property(nonatomic, copy)   NSString *currentPath;
@property(nonatomic, copy)   NSString *sourceText;   // raw file text (edited)
@property(nonatomic, copy)   NSString *currentExt;
@property(nonatomic, assign) BOOL isMarkdown;
@property(nonatomic, assign) BOOL isLatex;
@property(nonatomic, assign) BOOL previewMode;       // markdown: rendered vs source
@property(nonatomic, assign) BOOL dirty;             // unsaved changes
@property(nonatomic, strong) NSDate *fileModDate;    // on-disk mtime we last saw
@property(nonatomic, strong) NSView *statusBar;
@property(nonatomic, strong) NSView *hintsPanel;
@property(nonatomic, strong) NSTextField *hintsLabel;
@property(nonatomic, assign) BOOL hintsVisible;
@property(nonatomic, strong) PanelHost *rightArea;
@property(nonatomic, strong) PanelScrollView *editorScroll;
@property(nonatomic, strong) TerminalView *terminal;
@property(nonatomic, strong) DragBar *termDivider;
@property(nonatomic, strong) BrowserView *browser;
@property(nonatomic, strong) LatexView *latex;
@property(nonatomic, strong) NSImageView *imageView;   // editor's slot, for images
@property(nonatomic, assign) BOOL isImage;
@property(nonatomic, assign) NSSize imagePixels;       // for the title bar
@property(nonatomic, strong) PDFView *pdfView;         // editor's slot, for PDFs
@property(nonatomic, assign) BOOL isPDF;
@property(nonatomic, assign) BOOL terminalVisible;
@property(nonatomic, assign) BOOL browserVisible;
@property(nonatomic, assign) CGFloat terminalHeight;
@property(nonatomic, strong) NSSplitView *splitView;
@property(nonatomic, strong) PanelScrollView *sidebarScroll;
@property(nonatomic, assign) BOOL sidebarCollapsed;
@property(nonatomic, assign) CGFloat sidebarWidthBeforeCollapse;
@property(nonatomic, assign) BOOL editorHidden;   // Shift+Cmd+E: file view hidden
@property(nonatomic, strong) SearchPanel *searchPanel;
@property(nonatomic, strong) NSVisualEffectView *blurView;   // window.blur
@property(nonatomic, strong) NSView *titlebarView;   // drawn when customTitlebar
@property(nonatomic, strong) NSTextField *titleLabel;
@property(nonatomic, strong) NSTextField *settingsLabel;     // settings errors
@property(nonatomic, assign) BOOL showingMessage;   // welcome/binary text shown
@property(nonatomic, assign) NSUInteger colorLineStart;   // line the color panel edits
@property(nonatomic, copy)   NSString *colorEditPath;
@property(nonatomic, strong) LspSession *lsp;         // language servers
@property(nonatomic, strong) NSTextField *lspLabel;   // their status
@end

@implementation EditorController

- (instancetype)initWithRootPath:(NSString *)path {
    if ((self = [super init])) {
        _root = [FileItem new];
        _root.path = path; _root.isDir = YES;
        [_root loadChildren];
        _terminalHeight = 220;
        _recent = [NSMutableArray array];
        _pendingStart = _pendingEnd = NSNotFound;
    }
    return self;
}

- (NSString *)rootPath { return _root.path; }

// -------------------------------------------------------------- window setup
- (void)showWindow {
    NSRect frame = NSMakeRect(0, 0, 1100, 720);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                       // Content runs under the title bar, so MiniCode can draw
                       // it (see applySettings); layoutContainer keeps the
                       // panels below it.
                       NSWindowStyleMaskFullSizeContentView;
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
    PanelScrollView *treeScroll = [[PanelScrollView alloc] init];
    self.sidebarScroll = treeScroll;
    treeScroll.hasVerticalScroller = YES;
    treeScroll.automaticallyAdjustsContentInsets = NO;   // colored in applySettings

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
    // Clear, so the scroll view's color is the only one (a translucent color
    // drawn twice would look more opaque than configured).
    self.outline.backgroundColor = [NSColor clearColor];
    self.outline.rowSizeStyle = NSTableViewRowSizeStyleMedium;
    self.outline.indentationPerLevel = 14;
    self.outline.floatsGroupRows = NO;
    self.outline.menu = [self buildTreeContextMenu];
    treeScroll.documentView = self.outline;

    // --- editor pane
    PanelScrollView *textScroll = [[PanelScrollView alloc] init];
    self.editorScroll = textScroll;
    textScroll.hasVerticalScroller = YES;
    textScroll.hasHorizontalScroller = YES;
    textScroll.autohidesScrollers = YES;
    textScroll.automaticallyAdjustsContentInsets = NO;

    self.textView = [[CodeTextView alloc] initWithFrame:frame];
    self.textView.editable = YES;
    self.textView.delegate = self;
    self.textView.textStorage.delegate = self;   // incremental highlighting
    self.textView.richText = YES;
    self.textView.allowsUndo = YES;
    self.textView.automaticSpellingCorrectionEnabled = NO;
    self.textView.automaticDashSubstitutionEnabled = NO;
    self.textView.usesFindBar = YES;               // Cmd+F find bar
    // Not the Font panel: MiniCode sets its own fonts, and a text view that
    // uses it also pushes the color under the caret into the shared color
    // panel whenever the selection moves. With the panel picking a color for
    // the settings file (colorPicked:), that push came back as a pick and
    // wrote the text color over the color just chosen.
    self.textView.usesFontPanel = NO;
    // Links are only used for the color swatches in the settings file; keep
    // their own colors and just show a pointing hand.
    self.textView.linkTextAttributes =
        @{NSCursorAttributeName: [NSCursor pointingHandCursor]};
    self.textView.incrementalSearchingEnabled = YES;
    // The backdrop paints the background over the whole scroll view; the text
    // view drawing it too would double a translucent color.
    self.textView.drawsBackground = NO;
    self.textView.textContainerInset = NSMakeSize(8, 8);
    self.textView.automaticQuoteSubstitutionEnabled = NO;
    self.textView.minSize = NSMakeSize(0, 0);
    self.textView.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
    self.textView.verticallyResizable = YES;
    self.textView.horizontallyResizable = YES;
    self.textView.textContainer.widthTracksTextView = YES;
    textScroll.documentView = self.textView;

    // Language servers: completion, go to definition, error underlines.
    self.lsp = [[LspSession alloc] initWithTextView:self.textView root:_root.path];
    self.lsp.onStatus = ^(NSString *text) { weakSelf.lspLabel.stringValue = text; };
    self.lsp.openFile = ^BOOL(NSString *p) {
        EditorController *s = weakSelf;
        if ([p hasPrefix:[s.rootPath stringByAppendingString:@"/"]])
            [s revealPath:p andOpen:YES];
        else
            [s openFileAtPath:p];
        return [s.currentPath isEqualToString:p];
    };

    // Right side hosts the editor plus (lazily) a terminal dock and browser.
    self.rightArea = [[PanelHost alloc] initWithFrame:frame];
    self.rightArea.onLayout = ^{ [weakSelf relayoutRightArea]; };
    textScroll.frame = self.rightArea.bounds;
    [self.rightArea addSubview:textScroll];

    [split addSubview:treeScroll];
    [split addSubview:self.rightArea];

    // Container holds, back to front: the blur, the title bar strip, the split
    // view, the status bar (bottom) and the hints overlay. layoutContainer
    // places them by hand.
    PanelHost *container = [[PanelHost alloc] initWithFrame:frame];
    self.blurView = [[NSVisualEffectView alloc] initWithFrame:frame];
    self.blurView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    self.blurView.state = NSVisualEffectStateActive;   // stays frosted when inactive
    self.blurView.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [container addSubview:self.blurView];
    [self buildTitlebarInContainer:container];
    [container addSubview:split];
    [self buildStatusBarInContainer:container height:kStatusBarHeight];
    [self buildHintsPanelInContainer:container];

    self.window.contentView = container;
    container.onLayout = ^{ [weakSelf layoutContainer]; };
    [self applySettings];
    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(applySettings)
               name:MCSettingsDidChangeNotification object:nil];
    [self.window addObserver:self forKeyPath:@"title"
                     options:NSKeyValueObservingOptionInitial context:NULL];
    [self showWelcome];
    [self.window makeKeyAndOrderFront:nil];
    [self.outline reloadData];

    // Set the divider AFTER the window is on screen and laid out — otherwise
    // autoresizing collapses the sidebar to zero width.
    [self.window layoutIfNeeded];
    [self layoutContainer];
    [split adjustSubviews];
    [split setPosition:260 ofDividerAtIndex:0];
    [self.outline sizeLastColumnToFit];

    [self startWatching:_root.path];   // live tree updates
}

- (void)dealloc {
    [self stopWatching];
    [self.window removeObserver:self forKeyPath:@"title"];
}

// The drawn title bar shows the window's title, which is set in several places.
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    if (object == self.window && [keyPath isEqualToString:@"title"])
        self.titleLabel.stringValue = self.window.title ?: @"";
    else
        [super observeValueForKeyPath:keyPath ofObject:object change:change
                              context:context];
}

// ------------------------------------------------------------ layout + look
// Height of the title bar the content runs under (0 in full screen). Before
// the window is on screen contentLayoutRect is empty, so ask the frame math.
- (CGFloat)titlebarHeight {
    NSWindow *w = self.window;
    NSRect layout = w.contentLayoutRect;
    if (NSIsEmptyRect(layout)) {
        NSRect frame = w.frame;
        return MAX(0, frame.size.height -
                      [NSWindow contentRectForFrameRect:frame
                                              styleMask:w.styleMask &
                                  ~NSWindowStyleMaskFullSizeContentView].size.height);
    }
    return MAX(0, NSMaxY(w.contentView.bounds) - NSMaxY(layout));
}

- (void)layoutContainer {
    NSView *c = self.window.contentView;
    if (!c || !self.splitView) return;
    CGFloat W = c.bounds.size.width, H = c.bounds.size.height;
    CGFloat titleH = [self titlebarHeight];
    self.blurView.frame = c.bounds;
    self.titlebarView.frame = NSMakeRect(0, H - titleH, W, titleH);
    // Traffic lights on the left; keep the title centered on the window.
    self.titleLabel.frame = NSMakeRect(80, floor((titleH - 16) / 2), MAX(0, W - 160), 16);
    self.splitView.frame = NSMakeRect(0, kStatusBarHeight, W,
                                      MAX(0, H - kStatusBarHeight - titleH));
    self.statusBar.frame = NSMakeRect(0, 0, W, kStatusBarHeight);
    if (self.hintsVisible) [self updateHints];
}

- (void)buildTitlebarInContainer:(NSView *)container {
    self.titlebarView = [[NSView alloc] initWithFrame:NSZeroRect];
    self.titlebarView.wantsLayer = YES;
    NSTextField *label = [NSTextField labelWithString:@""];
    label.font = [NSFont titleBarFontOfSize:0];
    label.alignment = NSTextAlignmentCenter;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.titleLabel = label;
    [self.titlebarView addSubview:label];
    [container addSubview:self.titlebarView];
}

// Apply the settings file: window transparency and blur, the title bar, and
// each panel's background and text colors. Runs at startup and whenever the
// file changes.
- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    const Settings &st = cfg.settings;
    NSWindow *w = self.window;

    BOOL opaque = st.windowIsOpaque();
    w.opaque = opaque;
    w.backgroundColor = opaque ? [NSColor windowBackgroundColor]
                               : [NSColor clearColor];
    self.blurView.hidden = !st.blur();
    self.blurView.material = cfg.material;

    BOOL custom = st.customTitlebar();
    w.titlebarAppearsTransparent = custom;
    w.titleVisibility = custom ? NSWindowTitleHidden : NSWindowTitleVisible;
    w.titlebarSeparatorStyle = custom ? NSTitlebarSeparatorStyleNone
                                      : NSTitlebarSeparatorStyleAutomatic;
    self.titlebarView.hidden = !custom;
    self.titlebarView.layer.backgroundColor = [cfg background:Surface::Titlebar].CGColor;
    self.titleLabel.textColor = [cfg text:Surface::Titlebar];

    self.sidebarScroll.panelColor = [cfg background:Surface::Sidebar];
    NSColor *treeText = [cfg text:Surface::Sidebar];
    [self.outline enumerateAvailableRowViewsUsingBlock:^(NSTableRowView *row,
                                                         NSInteger r) {
        (void)r;
        for (NSInteger i = 0; i < row.numberOfColumns; i++) {
            NSTableCellView *cell = [row viewAtColumn:i];
            if ([cell isKindOfClass:[NSTableCellView class]])
                cell.textField.textColor = treeText;
        }
    }];

    self.editorScroll.panelColor = [cfg background:Surface::Editor];
    [self.latex applySettings];
    self.imageView.layer.backgroundColor = [cfg background:Surface::Editor].CGColor;
    self.pdfView.backgroundColor = [cfg background:Surface::Editor];
    self.textView.insertionPointColor = [cfg text:Surface::Editor];
    [self recolorEditor];

    self.statusBar.layer.backgroundColor = [cfg background:Surface::Statusbar].CGColor;
    self.statusLabel.textColor = [cfg text:Surface::Statusbar];
    self.settingsLabel.textColor = [cfg text:Surface::Statusbar];
    self.lspLabel.textColor = [cfg text:Surface::Statusbar];
    NSArray<NSString *> *errors = cfg.errors;
    self.settingsLabel.stringValue = errors.count == 0 ? @"" :
        [NSString stringWithFormat:@"Settings %@%@", errors.firstObject,
            errors.count > 1
                ? [NSString stringWithFormat:@" (and %lu more)",
                   (unsigned long)errors.count - 1] : @""];
    self.settingsLabel.toolTip = errors.count
        ? [NSString stringWithFormat:@"%@\n\n%@", cfg.path,
           [errors componentsJoinedByString:@"\n"]] : nil;

    [w invalidateShadow];
    [self layoutContainer];
}

// Redraw the open document in the current colors, keeping the scroll
// position and selection.
- (void)recolorEditor {
    if (self.showingMessage || !self.currentPath) return;
    if (self.isMarkdown && self.previewMode) {
        NSPoint origin = self.editorScroll.contentView.bounds.origin;
        [self renderMarkdown:self.sourceText];
        [self.editorScroll.contentView scrollToPoint:origin];
        [self.editorScroll reflectScrolledClipView:self.editorScroll.contentView];
    } else {
        NSMutableDictionary *typing = [self.textView.typingAttributes mutableCopy];
        typing[NSForegroundColorAttributeName] =
            [[AppSettings shared] text:Surface::Editor];
        self.textView.typingAttributes = typing;
        [self applyHighlighting];
    }
}

// Cmd+, : open the settings file in this window, creating it first.
- (void)openSettings:(id)sender {
    AppSettings *cfg = [AppSettings shared];
    if (![cfg ensureFileExists]) {
        [self warn:[NSString stringWithFormat:@"Could not create %@.", cfg.path]];
        return;
    }
    [self openFileAtPath:cfg.path];
    [self focusEditor:nil];
}

- (NSMenu *)buildTreeContextMenu {
    NSMenu *m = [[NSMenu alloc] init];
    [m addItemWithTitle:@"New File…" action:@selector(newFile:) keyEquivalent:@""];
    [m addItemWithTitle:@"New Folder…" action:@selector(newFolder:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Rename…" action:@selector(renameSelected:) keyEquivalent:@""];
    [m addItemWithTitle:@"Move to Trash" action:@selector(deleteSelected:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Reveal in Finder" action:@selector(revealInFinder:) keyEquivalent:@""];
    [m addItemWithTitle:@"Copy Path" action:@selector(copyPath:) keyEquivalent:@""];
    [m addItemWithTitle:@"Refresh" action:@selector(refreshTree:) keyEquivalent:@""];
    for (NSMenuItem *it in m.itemArray) it.target = self;
    return m;
}

// -------------------------------------------------------- status bar + hints
- (void)buildStatusBarInContainer:(NSView *)container height:(CGFloat)barH {
    self.statusBar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, container.bounds.size.width, barH)];
    self.statusBar.wantsLayer = YES;   // colors set in applySettings
    self.statusBar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;

    NSTextField *label = [NSTextField labelWithString:@"⇧⌘H  Shortcuts"];
    label.font = [NSFont systemFontOfSize:11];
    label.backgroundColor = [NSColor clearColor];
    label.frame = NSMakeRect(0, 3, container.bounds.size.width - 12, 16);
    label.alignment = NSTextAlignmentRight;
    label.autoresizingMask = NSViewWidthSizable;
    self.statusLabel = label;
    [self.statusBar addSubview:label];

    // Left side: the first problem in the settings file, if any.
    NSTextField *errors = [NSTextField labelWithString:@""];
    errors.font = [NSFont systemFontOfSize:11];
    errors.frame = NSMakeRect(12, 3, container.bounds.size.width * 0.6, 16);
    errors.autoresizingMask = NSViewWidthSizable;
    errors.lineBreakMode = NSLineBreakByTruncatingTail;
    self.settingsLabel = errors;
    [self.statusBar addSubview:errors];

    // Middle: the language server for the open file and its problem count,
    // right-aligned just left of the shortcuts hint.
    NSTextField *lsp = [NSTextField labelWithString:@""];
    lsp.font = [NSFont systemFontOfSize:11];
    lsp.alignment = NSTextAlignmentRight;
    lsp.lineBreakMode = NSLineBreakByTruncatingHead;
    lsp.frame = NSMakeRect(container.bounds.size.width - 150 - 340, 3, 340, 16);
    lsp.autoresizingMask = NSViewMinXMargin;
    self.lspLabel = lsp;
    [self.statusBar addSubview:lsp];
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
    NSAttributedString *text = [self hintsText];
    self.hintsLabel.attributedStringValue = text;

    NSView *container = self.hintsPanel.superview;
    const CGFloat pw = kHintsWidth, pad = 16, margin = 18;
    NSRect textRect = [text boundingRectWithSize:NSMakeSize(pw - 2 * pad, 10000)
        options:NSStringDrawingUsesLineFragmentOrigin];
    CGFloat ph = ceil(textRect.size.height) + 2 * pad;
    self.hintsPanel.frame = NSMakeRect(
        container.bounds.size.width - pw - margin,
        container.bounds.size.height - [self titlebarHeight] - ph - margin, pw, ph);
    self.hintsLabel.frame = NSMakeRect(pad, pad, pw - 2 * pad, ph - 2 * pad);
}

// The hints are laid out on tab stops, never with spaces: the key glyphs
// (⌘ ⇧ ⌃ ⌫ ⏎) have different widths even in a monospaced font, so padding with
// spaces leaves every column ragged. A row holds one or two shortcuts:
//   key | label | key | label       (two shortcuts)
//   key | label | state             (a pane, with whether it is open)
static const CGFloat kHintsWidth = 420;
static const CGFloat kHintsLabel1 = 52, kHintsKey2 = 208, kHintsLabel2 = 260;

- (void)appendHintsHeading:(NSString *)title to:(NSMutableAttributedString *)s {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.paragraphSpacingBefore = s.length ? 10 : 0;
    ps.paragraphSpacing = 4;
    [s appendAttributedString:[[NSAttributedString alloc]
        initWithString:[title.uppercaseString stringByAppendingString:@"\n"]
            attributes:@{
                NSFontAttributeName: [NSFont systemFontOfSize:10
                                                       weight:NSFontWeightSemibold],
                NSForegroundColorAttributeName: Hex(0x9CA3AF),
                NSKernAttributeName: @0.6,
                NSParagraphStyleAttributeName: ps,
            }]];
}

// One row. `key2`/`label2` may be nil; `state` (muted) takes the second
// column instead, for panes.
- (void)appendHintsRow:(NSMutableAttributedString *)s
                   key:(NSString *)key1 label:(NSString *)label1
                  key2:(NSString *)key2 label2:(NSString *)label2
                 state:(NSString *)state {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.tabStops = @[
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                        location:kHintsLabel1 options:@{}],
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                        location:kHintsKey2 options:@{}],
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                        location:kHintsLabel2 options:@{}],
    ];
    ps.lineSpacing = 3;
    NSDictionary *keyAttr = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12
                                                         weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: Hex(0x4EA1F7),
        NSParagraphStyleAttributeName: ps,
    };
    NSDictionary *labelAttr = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12],
        NSForegroundColorAttributeName: Hex(0xE0E0E0),
        NSParagraphStyleAttributeName: ps,
    };
    NSDictionary *stateAttr = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12],
        NSForegroundColorAttributeName: Hex(0x9CA3AF),
        NSParagraphStyleAttributeName: ps,
    };
    void (^add)(NSString *, NSDictionary *) = ^(NSString *t, NSDictionary *a) {
        [s appendAttributedString:[[NSAttributedString alloc] initWithString:t
                                                                  attributes:a]];
    };
    add(key1, keyAttr);
    add([@"\t" stringByAppendingString:label1], labelAttr);
    if (key2) {
        add([@"\t" stringByAppendingString:key2], keyAttr);
        add([@"\t" stringByAppendingString:label2 ?: @""], labelAttr);
    } else if (state) {
        add([@"\t" stringByAppendingString:state], stateAttr);
    }
    add(@"\n", labelAttr);
}

// Context-aware shortcut list.
- (NSAttributedString *)hintsText {
    NSMutableAttributedString *s = [NSMutableAttributedString new];
    NSString *(^openOr)(BOOL) = ^NSString *(BOOL open) {
        return open ? @"open" : @"hidden";
    };

    [self appendHintsHeading:@"Editing" to:s];
    [self appendHintsRow:s key:@"⌘N" label:@"New window"
                    key2:@"⌘O" label2:@"Open folder" state:nil];
    [self appendHintsRow:s key:@"⌘S" label:@"Save"
                    key2:@"⌘F" label2:@"Find in file" state:nil];
    [self appendHintsRow:s key:@"⌘Z" label:@"Undo"
                    key2:@"⇧⌘Z" label2:@"Redo" state:nil];
    [self appendHintsRow:s key:@"⌘C" label:@"Copy"
                    key2:@"⌘A" label2:@"Select all" state:nil];
    [self appendHintsRow:s key:@"⌘/" label:@"Toggle comment"
                    key2:@"⇧⌘F" label2:@"Find in folder" state:nil];
    [self appendHintsRow:s key:@"⌘," label:@"Settings"
                    key2:@"⌃Space" label2:@"Complete" state:nil];
    [self appendHintsRow:s key:@"F12" label:@"Go to definition"
                    key2:@"⌘I" label2:@"Hover info" state:nil];

    [self appendHintsHeading:@"Files" to:s];
    [self appendHintsRow:s key:@"⌃⌘N" label:@"New file"
                    key2:@"⇧⌘N" label2:@"New folder" state:nil];
    [self appendHintsRow:s key:@"⌘⌫" label:@"Move to trash"
                    key2:@"⌘R" label2:@"Refresh tree" state:nil];
    [self appendHintsRow:s key:@"⇧⌘." label:@"Show hidden files"
                    key2:nil label2:nil state:nil];

    [self appendHintsHeading:@"Navigation" to:s];
    [self appendHintsRow:s key:@"⌘0" label:@"Focus tree"
                    key2:@"⌘1" label2:@"Focus editor" state:nil];
    [self appendHintsRow:s key:@"↑ ↓" label:@"Browse tree"
                    key2:@"⏎" label2:@"Open file" state:nil];
    [self appendHintsRow:s key:@"⌃⇥" label:@"Previous file"
                    key2:@"⌘B" label2:@"Toggle sidebar" state:nil];

    [self appendHintsHeading:@"Panes" to:s];
    [self appendHintsRow:s key:@"⇧⌘E" label:@"Editor"
                    key2:nil label2:nil state:openOr(!self.editorHidden)];
    [self appendHintsRow:s key:@"⇧⌘T" label:@"Terminal  (also ⌃`)"
                    key2:nil label2:nil state:openOr(self.terminalVisible)];
    [self appendHintsRow:s key:@"⇧⌘B" label:@"Browser"
                    key2:nil label2:nil state:openOr(self.browserVisible)];
    if (self.isMarkdown) {
        [self appendHintsRow:s key:@"⇧⌘P" label:@"Markdown preview"
                        key2:nil label2:nil
                       state:self.previewMode ? @"rendered" : @"source"];
    } else if (self.isLatex) {
        [self appendHintsRow:s key:@"⇧⌘P" label:@"LaTeX preview"
                        key2:nil label2:nil
                       state:self.previewMode ? @"typeset" : @"source"];
        [self appendHintsRow:s key:@"⇧⌘S" label:@"Export PDF"
                        key2:nil label2:nil state:nil];
    }

    // A short gap, not a whole empty heading, before the way out.
    [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
        attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:6]}]];
    [self appendHintsRow:s key:@"⇧⌘H" label:@"Hide these hints"
                    key2:nil label2:nil state:nil];
    // No trailing newline, or the panel sizes itself one empty line too tall.
    if ([s.string hasSuffix:@"\n"])
        [s deleteCharactersInRange:NSMakeRange(s.length - 1, 1)];
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
// The top slot holds the browser when it's open, else the file editor unless
// that is hidden. The terminal docks below it, or fills the pane when the top
// slot is empty.
static const CGFloat kMinTopSlotHeight = 40;   // smallest editor while dragging
static const CGFloat kTopSnapDistance  = 16;   // bar this close to the top hides it

- (void)relayoutRightArea {
    NSRect b = self.rightArea.bounds;
    CGFloat W = b.size.width, H = b.size.height;
    BOOL showTerm = self.terminalVisible && self.terminal != nil;
    BOOL showBrowser = self.browserVisible && self.browser != nil;
    BOOL showEditor = !showBrowser && !self.editorHidden;
    BOOL topShown = showBrowser || showEditor;

    CGFloat termH = 0;
    if (showTerm)
        termH = topShown ? MIN(MAX(self.terminalHeight, 80),
                               MAX(80, H - kMinTopSlotHeight))
                         : H;
    NSRect topRect = NSMakeRect(0, termH, W, MAX(0, H - termH));

    // The LaTeX preview sits in the editor's slot, in place of the text view.
    BOOL showLatex = showEditor && self.latex != nil && self.isLatex &&
                     self.previewMode;
    // So does an image, in place of the text view.
    BOOL showImage = showEditor && self.imageView != nil && self.isImage;
    BOOL showPDF = showEditor && self.pdfView != nil && self.isPDF;
    self.editorScroll.frame = topRect;
    self.editorScroll.hidden = !showEditor || showLatex || showImage || showPDF;
    if (self.pdfView) {
        self.pdfView.frame = topRect;
        self.pdfView.hidden = !showPDF;
    }
    if (self.imageView) {
        self.imageView.frame = topRect;
        self.imageView.hidden = !showImage;
    }
    if (self.latex) {
        self.latex.frame = topRect;
        self.latex.hidden = !showLatex;
    }
    if (self.browser) {
        self.browser.frame = topRect;
        self.browser.hidden = !showBrowser;
    }
    if (self.terminal) {
        self.terminal.frame = NSMakeRect(0, 0, W, termH);
        self.terminal.hidden = !showTerm;
    }
    if (self.termDivider) {
        // With the top slot empty the bar sits at the top edge, still inside
        // the pane, so it can be dragged back down.
        CGFloat barY = topShown ? termH - kDragBarHeight / 2
                                : H - kDragBarHeight;
        self.termDivider.frame = NSMakeRect(0, barY, W, kDragBarHeight);
        self.termDivider.hidden = !showTerm;
        // Views added later (the browser) would otherwise sit on top of the
        // bar and swallow the clicks meant for it.
        if (self.rightArea.subviews.lastObject != self.termDivider)
            [self.rightArea addSubview:self.termDivider
                            positioned:NSWindowAbove relativeTo:nil];
    }
}

// Dragging the terminal bar. Near the top it hides whatever is above the
// terminal (editor, and the browser if open); dragging down again brings the
// editor back.
- (void)terminalBarDraggedTo:(CGFloat)y {
    CGFloat H = self.rightArea.bounds.size.height;
    BOOL topShown = (self.browserVisible && self.browser) || !self.editorHidden;
    if (y >= H - kTopSnapDistance) {
        if (topShown) {
            self.editorHidden = YES;
            self.browserVisible = NO;
            [self.window makeFirstResponder:self.terminal];
            [self.terminal focusInput];
        }
    } else {
        if (!topShown) self.editorHidden = NO;
        self.terminalHeight = MIN(MAX(y, 80), MAX(80, H - kMinTopSlotHeight));
    }
    [self relayoutRightArea];
    if (self.hintsVisible) [self updateHints];
}

- (void)toggleTerminal:(id)sender {
    if (!self.terminal) {
        self.terminal = [[TerminalView alloc] initWithDirectory:_root.path];
        [self.rightArea addSubview:self.terminal];

        self.termDivider = [[DragBar alloc] initWithFrame:NSZeroRect];
        __weak EditorController *weakSelf = self;
        self.termDivider.onDrag = ^(CGFloat y) {
            [weakSelf terminalBarDraggedTo:y];
        };
        [self.rightArea addSubview:self.termDivider];
    }
    self.terminalVisible = !self.terminalVisible;
    if (self.terminalVisible) [self showRightArea];
    [self relayoutRightArea];
    if (self.terminalVisible) [self.terminal focusInput];
    else [self collapseRightAreaIfEmpty];
    if (self.hintsVisible) [self updateHints];
}

- (void)toggleBrowser:(id)sender {
    if (!self.browser) {
        self.browser = [[BrowserView alloc]
            initWithHomeURL:@"https://duckduckgo.com"];
        [self.rightArea addSubview:self.browser];
    }
    self.browserVisible = !self.browserVisible;
    if (self.browserVisible) [self showRightArea];
    [self relayoutRightArea];
    if (self.browserVisible) [self.browser focusURLBar];
    else [self collapseRightAreaIfEmpty];
    if (self.hintsVisible) [self updateHints];
}

// ---- NSSplitViewDelegate: sidebar | right pane ----
// The right pane (rightArea: editor, terminal, browser) shrinks down to this
// width as the divider is dragged right; NSSplitView collapses it once the
// pointer is past the midpoint of what's left, like the GTK port's paned.
static const CGFloat kMinRightAreaWidth = 150;
// The thin divider is drawn 1px wide; this much on each side also grabs it.
// NSSplitView's default is 2px, which is easy to miss.
static const CGFloat kDividerGrabSlop = 5;

- (BOOL)rightAreaCollapsed {
    return [self.splitView isSubviewCollapsed:self.rightArea];
}
- (BOOL)splitView:(NSSplitView *)sv shouldAdjustSizeOfSubview:(NSView *)view {
    // On window resize, grow/shrink the right pane, not the sidebar, unless
    // the right pane is collapsed; then the sidebar takes the whole window.
    if (self.rightAreaCollapsed) return view != self.rightArea;
    return view != sv.subviews.firstObject;
}
- (NSRect)splitView:(NSSplitView *)sv effectiveRect:(NSRect)proposed
          forDrawnRect:(NSRect)drawn ofDividerAtIndex:(NSInteger)i {
    return NSInsetRect(drawn, -kDividerGrabSlop, 0);
}
- (BOOL)splitView:(NSSplitView *)sv canCollapseSubview:(NSView *)view {
    return view == self.rightArea;
}
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMinCoordinate:(CGFloat)min
               ofSubviewAt:(NSInteger)i {
    return self.sidebarCollapsed ? 0 : 160;   // allow full collapse via Cmd+B
}
- (CGFloat)splitView:(NSSplitView *)sv
    constrainMaxCoordinate:(CGFloat)max
               ofSubviewAt:(NSInteger)i {
    return MAX(160, sv.bounds.size.width - kMinRightAreaWidth);
}

// Shift+Cmd+E: hide or restore the file editor/preview only. The terminal and
// browser stay put and take over its space.
- (void)toggleEditor:(id)sender {
    if (self.editorHidden || self.rightAreaCollapsed) {
        [self revealEditor];
        [self.window makeFirstResponder:self.textView];
        return;
    }
    self.editorHidden = YES;
    [self relayoutRightArea];
    if (self.terminalVisible && self.terminal) [self.terminal focusInput];
    else if (!(self.browserVisible && self.browser))
        [self.window makeFirstResponder:self.outline];
    [self collapseRightAreaIfEmpty];
    if (self.hintsVisible) [self updateHints];
}

// Show the file editor again: unhide it and uncollapse the right pane. Called
// by anything that puts a file in front of the user, so opening a file never
// appears to do nothing.
- (void)revealEditor {
    BOOL wasHidden = self.editorHidden;
    self.editorHidden = NO;
    [self showRightArea];
    if (wasHidden) [self relayoutRightArea];
    if (self.hintsVisible) [self updateHints];
}

// Uncollapse the right pane if it was dragged or toggled shut.
- (void)showRightArea {
    if (!self.rightAreaCollapsed) return;
    CGFloat W = self.splitView.bounds.size.width;
    CGFloat width = self.sidebarWidthBeforeCollapse;
    if (width < 160) width = 260;
    width = MIN(width, MAX(160, W - kMinRightAreaWidth));
    [self.splitView setPosition:width ofDividerAtIndex:0];
}

// With the editor hidden and no terminal or browser, the right pane would be
// an empty panel; collapse it so the file tree fills the window.
- (void)collapseRightAreaIfEmpty {
    BOOL empty = self.editorHidden && !(self.terminalVisible && self.terminal) &&
                 !(self.browserVisible && self.browser);
    if (!empty || self.rightAreaCollapsed) return;
    NSView *sidebar = self.splitView.subviews.firstObject;
    self.sidebarWidthBeforeCollapse = sidebar.frame.size.width;
    self.sidebarCollapsed = NO;   // never leave the window with nothing in it
    // With canCollapseSubview:, moving the divider to the far edge collapses.
    [self.splitView setPosition:self.splitView.bounds.size.width
               ofDividerAtIndex:0];
    [self.window makeFirstResponder:self.outline];
}

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
    cell.textField.textColor = [[AppSettings shared] text:Surface::Sidebar];
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
    [self revealEditor];
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
    if ([self.currentPath isEqual:node.path]) {   // keep editor in sync
        self.currentPath = dst;
        [self.lsp documentOpened:dst];
    }
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
        self.currentPath = nil;
        [self resetViewMode];
        [self.lsp documentOpened:nil];
        [self showWelcome]; [self relayoutRightArea]; [self updateTitle];
    }
    [self refreshTree:nil];
}

- (void)revealInFinder:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    NSString *p = node ? node.path : _root.path;
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:
        @[[NSURL fileURLWithPath:p]]];
}

// Puts an absolute path on the clipboard as plain text: the clicked item's,
// else the selected one's, else the open folder's (as Reveal in Finder picks).
- (void)copyPath:(id)sender {
    FileItem *node = [self clickedOrSelectedItem];
    NSString *p = node ? node.path : _root.path;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:p forType:NSPasteboardTypeString];
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
// Forget everything about the previous file's mode. Called before a new file
// is shown, so a stale preview or image, or an editable text view, can never
// survive into a file that shows something else. A text file sets its own
// mode again from its extension, and refreshDisplay makes the view editable.
- (void)resetViewMode {
    self.isMarkdown = NO;
    self.isLatex = NO;
    self.isImage = NO;
    self.isPDF = NO;
    self.previewMode = NO;
    self.sourceText = nil;
    self.dirty = NO;
    self.textView.editable = NO;
    self.imageView.image = nil;
    self.pdfView.document = nil;
}

// Formats the system can decode that are worth showing as a picture. SVG is
// left out because it is source people edit, and PDF has its own viewer
// (showPDFAtPath:), since a picture of it would be only its first page.
+ (BOOL)isImagePath:(NSString *)path {
    static NSSet<NSString *> *exts;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        exts = [NSSet setWithArray:@[@"png", @"jpg", @"jpeg", @"gif", @"tif",
            @"tiff", @"bmp", @"heic", @"heif", @"webp", @"ico", @"icns"]];
    });
    return [exts containsObject:path.pathExtension.lowercaseString];
}

// Show an image in the editor's slot. Returns NO if it can't be decoded, and
// the caller falls through to the usual "Cannot display" message.
- (BOOL)showImageAtPath:(NSString *)path {
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:path];
    if (!img || !img.isValid) return NO;
    if (!self.imageView) {
        NSImageView *v = [[NSImageView alloc] initWithFrame:NSZeroRect];
        // Fit the pane, but never blow a small image up past its own size.
        v.imageScaling = NSImageScaleProportionallyDown;
        v.imageAlignment = NSImageAlignCenter;
        v.imageFrameStyle = NSImageFrameNone;
        v.animates = YES;             // animated GIFs play
        v.editable = NO;
        v.wantsLayer = YES;
        v.layer.backgroundColor =
            [[AppSettings shared] background:Surface::Editor].CGColor;
        [self.rightArea addSubview:v positioned:NSWindowBelow
                        relativeTo:self.termDivider];
        self.imageView = v;
    }
    // Pixel size from the largest bitmap, since NSImage.size is in points and
    // a 144 dpi screenshot would otherwise report half its real size.
    NSInteger pw = 0, ph = 0;
    for (NSImageRep *rep in img.representations) {
        if (rep.pixelsWide > pw) { pw = rep.pixelsWide; ph = rep.pixelsHigh; }
    }
    self.imagePixels = pw > 0 ? NSMakeSize(pw, ph) : img.size;
    // Draw at one image pixel per screen point, so "never scale up" means
    // what it says on a Retina screen too.
    if (pw > 0) img.size = NSMakeSize(pw, ph);
    self.imageView.image = img;
    self.isImage = YES;
    self.currentExt = path.pathExtension.lowercaseString;
    // The hidden text view holds nothing, and a message is never saved.
    [self setPlainMessage:@""];
    [self relayoutRightArea];
    [self.lsp documentOpened:nil];
    [self updateTitle];
    return YES;
}

// Show a PDF in the editor's slot with PDFKit, the same way an image is shown.
// A reload (the file changed on disk, say tectonic run in the terminal) keeps
// the page and the zoom. Returns NO if PDFKit can't read it, and the caller
// falls through to the "Cannot display" message.
- (BOOL)showPDFAtPath:(NSString *)path {
    PDFDocument *doc = [[PDFDocument alloc] initWithURL:[NSURL fileURLWithPath:path]];
    if (!doc) return NO;
    if (!self.pdfView) {
        PDFView *v = [[PDFView alloc] initWithFrame:NSZeroRect];
        v.autoScales = YES;
        v.displayMode = kPDFDisplaySinglePageContinuous;
        v.backgroundColor = [[AppSettings shared] background:Surface::Editor];
        [self.rightArea addSubview:v positioned:NSWindowBelow
                        relativeTo:self.termDivider];
        self.pdfView = v;
    }
    BOOL reload = self.isPDF && self.pdfView.document != nil;
    NSUInteger pageIndex = 0;
    NSPoint point = NSZeroPoint;
    CGFloat scale = self.pdfView.scaleFactor;
    BOOL autoScales = self.pdfView.autoScales;
    if (reload) {
        PDFDestination *was = self.pdfView.currentDestination;
        pageIndex = [self.pdfView.document indexForPage:was.page];
        point = was.point;
    }
    self.pdfView.document = doc;
    if (reload && pageIndex < doc.pageCount) {
        if (!autoScales) self.pdfView.scaleFactor = scale;
        [self.pdfView goToDestination:[[PDFDestination alloc]
            initWithPage:[doc pageAtIndex:pageIndex] atPoint:point]];
    }
    self.isPDF = YES;
    self.currentExt = @"pdf";
    [self setPlainMessage:@""];   // nothing in the text view to save
    [self relayoutRightArea];
    [self.lsp documentOpened:nil];
    [self updateTitle];
    return YES;
}

- (void)openFileAtPath:(NSString *)path {
    [self revealEditor];
    if ([path isEqualToString:self.currentPath]) return;  // avoid double-render
    if (![self confirmProceedPastUnsavedChanges]) {
        [self reselectCurrentFileInTree];   // undo the tree's selection move
        return;
    }
    self.currentPath = path;
    [self resetViewMode];
    // Images are routed by extension before any attempt to read them as text.
    if ([EditorController isImagePath:path] && [self showImageAtPath:path]) {
        [self recordModDate];
        [_recent removeObject:path];
        [_recent insertObject:path atIndex:0];
        return;
    }
    if ([path.pathExtension.lowercaseString isEqualToString:@"pdf"] &&
        [self showPDFAtPath:path]) {
        [self recordModDate];
        [_recent removeObject:path];
        [_recent insertObject:path atIndex:0];
        return;
    }
    NSError *err = nil;
    NSString *content = [NSString stringWithContentsOfFile:path
                                                  encoding:NSUTF8StringEncoding
                                                     error:&err];
    if (!content) {
        // resetViewMode above cleared the previous file's mode, so no stale
        // preview stays on screen and the text view is read-only, which keeps
        // Cmd+S from writing this message over the binary file.
        [self setPlainMessage:[NSString stringWithFormat:
            @"Cannot display “%@”.\n\n(Binary file or unsupported encoding.)",
            path.lastPathComponent]];
        [self relayoutRightArea];
        [self.lsp documentOpened:nil];
        [self updateTitle];
        [self setStatus:path];
        return;
    }
    NSString *ext = path.pathExtension.lowercaseString;
    self.sourceText = content;
    self.currentExt = ext;
    self.isMarkdown = [ext isEqualToString:@"md"] ||
                      [ext isEqualToString:@"markdown"];
    self.isLatex = [ext isEqualToString:@"tex"] || [ext isEqualToString:@"ltx"] ||
                   [ext isEqualToString:@"latex"];
    // Markdown and LaTeX both open in their preview.
    self.previewMode = self.isMarkdown || self.isLatex;
    self.dirty = NO;
    [self recordModDate];
    [_recent removeObject:path];
    [_recent insertObject:path atIndex:0];   // newest first
    [self refreshDisplay];
    // Previews show rendered text, which no language server should see.
    [self.lsp documentOpened:self.previewMode ? nil : path];
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
    if (self.isImage) { [self showImageAtPath:self.currentPath]; return; }
    if (self.isPDF) { [self showPDFAtPath:self.currentPath]; return; }
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
- (void)windowWillClose:(NSNotification *)note {
    [self.lsp shutdown];   // never leave a server running for a closed window
}
- (void)windowDidBecomeKey:(NSNotification *)note {
    [self checkExternalChange];
}

- (BOOL)canTogglePreview { return self.isMarkdown || self.isLatex; }

// Decide what to show for the current file/mode.
- (void)refreshDisplay {
    self.showingMessage = NO;
    if (self.isLatex && self.previewMode) {
        [self showLatexPreview];
        return;
    }
    // Always relay out: whatever held the editor's slot before (a PDF, an
    // image, the LaTeX preview) has to give it back to the text view.
    if (self.latex) self.latex.hidden = YES;
    [self relayoutRightArea];
    if (self.isMarkdown && self.previewMode) {
        self.textView.editable = NO;
        [self renderMarkdown:self.sourceText];
    } else {
        [self displaySourceEditable];
    }
}

// Typeset the current buffer and show the PDF where the editor sits. The
// preview keeps its own copy of the source; edits made in it come back through
// onSourceEdited and land in the buffer exactly as typing would.
- (void)showLatexPreview {
    self.textView.editable = NO;
    if (!self.latex) {
        self.latex = [[LatexView alloc] initWithPath:self.currentPath
                                              source:self.sourceText ?: @""];
        __weak EditorController *weakSelf = self;
        self.latex.onSourceEdited = ^(NSString *newSource) {
            [weakSelf latexDidEditSource:newSource];
        };
        [self.rightArea addSubview:self.latex];
    } else {
        [self.latex setPath:self.currentPath source:self.sourceText ?: @""];
    }
    [self relayoutRightArea];
    [self.window makeFirstResponder:self.latex];
}

// Shift+Cmd+S: save the typeset PDF somewhere of the user's choosing. The
// file is what tectonic wrote for the buffer as it is now, unsaved edits
// included; if the preview is behind (or was never shown, or the source view
// is up) the buffer is typeset first.
- (BOOL)canExportPDF { return self.isLatex && self.currentPath != nil; }

- (void)exportPDF:(id)sender {
    if (!self.canExportPDF) { NSBeep(); return; }
    if (!self.latex) {
        // Opened straight into the source view: make the preview, hidden.
        self.latex = [[LatexView alloc] initWithPath:self.currentPath
                                              source:self.sourceText ?: @""];
        __weak EditorController *weakSelf = self;
        self.latex.onSourceEdited = ^(NSString *newSource) {
            [weakSelf latexDidEditSource:newSource];
        };
        [self.rightArea addSubview:self.latex];
        [self relayoutRightArea];
    }
    NSString *texPath = self.currentPath;
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.directoryURL = [NSURL fileURLWithPath:
        texPath.stringByDeletingLastPathComponent];
    panel.nameFieldStringValue = [texPath.lastPathComponent
        .stringByDeletingPathExtension stringByAppendingPathExtension:@"pdf"];
    panel.canCreateDirectories = YES;
    NSString *source = self.sourceText ?: @"";
    __weak EditorController *weakSelf = self;
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSURL *dest = panel.URL;
        EditorController *me = weakSelf;
        if (!me) return;
        [me.latex pdfForPath:texPath source:source
                  completion:^(NSData *pdf, NSString *error) {
            NSError *err = nil;
            if (pdf && [pdf writeToURL:dest options:NSDataWritingAtomic error:&err])
                return;
            [weakSelf warn:error ?: [NSString stringWithFormat:
                @"Could not save “%@”: %@", dest.lastPathComponent,
                err.localizedDescription]];
        }];
    }];
}

// Cmd+Z with the preview open steps back through the edits made in it. The
// text view handles undo itself whenever it has focus, so this only runs when
// the responder chain got as far as the window's delegate.
- (BOOL)latexIsShowing {
    return self.latex != nil && !self.latex.isHidden;
}
- (void)undo:(id)sender {
    if ([self latexIsShowing]) [self.latex undoEdit];
    else NSBeep();
}
- (void)redo:(id)sender {
    if ([self latexIsShowing]) [self.latex redoEdit];
    else NSBeep();
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(exportPDF:)) return self.canExportPDF;
    if (item.action == @selector(undo:))
        return [self latexIsShowing] && [self.latex canUndoEdit];
    if (item.action == @selector(redo:))
        return [self latexIsShowing] && [self.latex canRedoEdit];
    return YES;
}

// An edit made in the PDF preview leaves the buffer dirty, like any typing.
- (void)latexDidEditSource:(NSString *)newSource {
    if ([newSource isEqualToString:self.sourceText]) return;
    self.sourceText = newSource;
    self.dirty = YES;
    [self updateTitle];
}

// Show the raw text as an editable, monospaced, syntax-highlighted document.
- (void)displaySourceEditable {
    NSFont *mono = [NSFont monospacedSystemFontOfSize:13
                                               weight:NSFontWeightRegular];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = 2.0;
    NSDictionary *base = @{
        NSFontAttributeName: mono,
        NSForegroundColorAttributeName: [[AppSettings shared] text:Surface::Editor],
        NSParagraphStyleAttributeName: ps,
    };
    NSString *content = self.sourceText ?: @"";
    NSMutableAttributedString *attr =
        [[NSMutableAttributedString alloc] initWithString:content attributes:base];
    _liveHighlight = NO;                        // applyHighlighting starts it again
    _sourceAttributes = base;
    [self.textView.textStorage setAttributedString:attr];
    self.textView.typingAttributes = base;      // typed text stays monospaced
    self.textView.editable = YES;
    [self applyHighlighting];
    [self.textView scrollToBeginningOfDocument:nil];
}

// Recolor the whole text storage in place (preserves cursor/selection), and
// start highlighting edits as they happen. Runs when a file is shown as source
// and when the settings change colors; typing goes through
// textStorage:willProcessEditing:, which re-lexes only the lines it changes.
- (void)applyHighlighting {
    std::string cext = self.currentExt.UTF8String ? self.currentExt.UTF8String : "";
    NSTextStorage *storage = self.textView.textStorage;
    AppSettings *cfg = [AppSettings shared];
    _plainColor = [cfg text:Surface::Editor];
    for (int i = 0; i < 8; ++i) _styleColors[i] = ColorForStyle((TokenStyle)i);
    _highlightingSettingsFile = [self isSettingsFile];
    if (_sourceAttributes) {
        NSMutableDictionary *base = [_sourceAttributes mutableCopy];
        base[NSForegroundColorAttributeName] = _plainColor;
        _sourceAttributes = base;
    }

    std::vector<Token> tokens;
    if (SyntaxHighlighter::supports(cext)) {
        _hl.reset(new IncrementalHighlighter<char16_t>(cext));
        _hl->reset(NSStringSource(storage.string), &tokens);
    } else {
        _hl.reset();
    }
    [storage beginEditing];
    [self restyleRange:NSMakeRange(0, storage.length) tokens:tokens inStorage:storage];
    [storage endEditing];
    _pendingStart = _pendingEnd = NSNotFound;   // all of it is fresh now
    _liveHighlight = YES;
}

// Paint `range` (whole lines) from scratch: the plain source attributes, then
// the tokens, then any color swatches. Adjacent tokens of one style are
// painted together (a comment spanning lines comes as one piece per line).
//
// The reset is one setAttributes: call, not addAttribute: of the plain color.
// Over a range holding many attribute runs, addAttribute: is dramatically
// slower (typing "/*" at the top of a 50,000-line file took 3.4 s that way,
// against milliseconds here).
- (void)restyleRange:(NSRange)range tokens:(const std::vector<Token> &)tokens
           inStorage:(NSTextStorage *)storage {
    if (range.length == 0) return;
    if (_sourceAttributes) {
        [storage setAttributes:_sourceAttributes range:range];
    } else {
        [storage addAttribute:NSForegroundColorAttributeName value:_plainColor range:range];
        [storage removeAttribute:NSBackgroundColorAttributeName range:range];
        [storage removeAttribute:NSLinkAttributeName range:range];
    }
    const NSUInteger limit = NSMaxRange(range);
    for (size_t i = 0; i < tokens.size();) {
        size_t start = tokens[i].start, end = start + tokens[i].length;
        TokenStyle st = tokens[i].style;
        size_t j = i + 1;
        while (j < tokens.size() && tokens[j].style == st && tokens[j].start == end) {
            end = tokens[j].start + tokens[j].length;
            ++j;
        }
        i = j;
        if (start < range.location || end > limit || end <= start) continue;
        [storage addAttribute:NSForegroundColorAttributeName
                        value:_styleColors[(int)st & 7]
                        range:NSMakeRange(start, end - start)];
    }
    if (_highlightingSettingsFile) [self decorateColorsIn:storage range:range];
}

// The storage's characters changed (typing, paste, undo, a Cmd+/ toggle, a
// color pick). Only the bookkeeping happens here: the highlighter's line
// table follows every edit, and the lines to recolor are remembered. The
// colors are applied in flushHighlighting, once the edit is complete.
// Changing attributes from inside this callback widens the storage's edited
// range, and NSTextView then moves the insertion point to the end of that
// range: typing "ab" in the middle of a line came out as "a" there and "b" on
// the next line.
- (void)textStorage:(NSTextStorage *)storage
    willProcessEditing:(NSTextStorageEditActions)actions
                 range:(NSRange)edited
        changeInLength:(NSInteger)delta {
    if (!_liveHighlight || !(actions & NSTextStorageEditedCharacters)) return;
    if (storage != self.textView.textStorage) return;
    NSUInteger newLen = edited.length;
    NSUInteger oldLen = (NSUInteger)((NSInteger)edited.length - delta);
    NSRange range;
    if (_hl) {
        std::vector<Token> unused;   // recomputed for the final text at flush time
        auto r = _hl->edit(NSStringSource(storage.string), edited.location, oldLen,
                           newLen, unused);
        range = NSMakeRange(r.start, r.end - r.start);
    } else {
        range = [storage.string lineRangeForRange:edited];
    }

    // Carry an earlier, still pending range across this edit, then add this
    // edit's own.
    if (_pendingStart != NSNotFound) {
        NSUInteger editEnd = edited.location + oldLen;
        if (_pendingStart >= editEnd) {
            _pendingStart = _pendingStart + newLen - oldLen;
            _pendingEnd = _pendingEnd + newLen - oldLen;
        } else if (_pendingEnd > edited.location) {
            _pendingStart = MIN(_pendingStart, edited.location);
            _pendingEnd = MAX(_pendingEnd, editEnd) + newLen - oldLen;
        }
        _pendingStart = MIN(_pendingStart, range.location);
        _pendingEnd = MAX(_pendingEnd, NSMaxRange(range));
    } else {
        _pendingStart = range.location;
        _pendingEnd = NSMaxRange(range);
    }
    // textDidChange: flushes right after the edit; this catches a change
    // made without didChangeText.
    if (!_flushScheduled) {
        _flushScheduled = YES;
        [self performSelector:@selector(flushHighlightingLater) withObject:nil afterDelay:0];
    }
}

- (void)flushHighlightingLater {
    _flushScheduled = NO;
    [self flushHighlighting];
}

// Recolor the lines edits have touched since the last flush.
- (void)flushHighlighting {
    if (_pendingStart == NSNotFound) return;
    NSTextStorage *storage = self.textView.textStorage;
    NSUInteger start = MIN(_pendingStart, storage.length);
    NSUInteger end = MIN(MAX(_pendingEnd, start), storage.length);
    _pendingStart = _pendingEnd = NSNotFound;
    if (!_liveHighlight) return;
    std::vector<Token> tokens;
    NSRange range;
    if (_hl) {
        size_t first = _hl->lineOf(start);
        size_t last = _hl->lineOf(end > start ? end - 1 : start);
        _hl->lineTokens(NSStringSource(storage.string), first, last + 1, tokens);
        size_t to = last + 1 < _hl->lineCount() ? _hl->lineStart(last + 1) : _hl->length();
        range = NSMakeRange(_hl->lineStart(first), to - _hl->lineStart(first));
    } else {
        range = [storage.string lineRangeForRange:NSMakeRange(start, end - start)];
    }
    [storage beginEditing];
    [self restyleRange:range tokens:tokens inStorage:storage];
    [storage endEditing];
}

// ------------------------------------------------ settings file: color swatches
- (BOOL)isSettingsFile {
    if (!self.currentPath) return NO;
    NSString *mine = self.currentPath.stringByResolvingSymlinksInPath;
    return [mine isEqualToString:
        [AppSettings shared].path.stringByResolvingSymlinksInPath];
}

static NSColor *ContrastColor(const Rgba &c) {
    return MCColor([AppSettings shared].settings.contrastText(c));
}

// Show every color value in `range` as a swatch of itself, clickable to pick a
// new one.
- (void)decorateColorsIn:(NSTextStorage *)storage range:(NSRange)range {
    NSString *str = storage.string;
    [str enumerateSubstringsInRange:[str lineRangeForRange:range]
                            options:NSStringEnumerationByLines
                         usingBlock:^(NSString *line, NSRange lr, NSRange er, BOOL *stop) {
        (void)er; (void)stop;
        ColorSpan span;
        if (!Settings::findColor(U16(line), span)) return;
        [storage addAttributes:@{
            NSBackgroundColorAttributeName: MCColor(span.color),
            NSForegroundColorAttributeName: ContrastColor(span.color),
            NSLinkAttributeName: kColorLink,
        } range:NSMakeRange(lr.location + span.start, span.length)];
    }];
}

- (BOOL)textView:(NSTextView *)tv clickedOnLink:(id)link atIndex:(NSUInteger)i {
    if (![link isEqual:kColorLink]) return NO;
    [self pickColorAtIndex:i];
    return YES;
}

// Open the system color panel on the color at character index i. Picks
// rewrite that line (uncommenting it if needed) and save, so the change
// shows up live.
- (void)pickColorAtIndex:(NSUInteger)i {
    NSString *str = self.textView.string;
    if (i > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(i, 0)];
    ColorSpan span;
    if (!Settings::findColor(U16([str substringWithRange:
            NSMakeRange(start, contentsEnd - start)]), span)) return;
    self.colorLineStart = start;
    self.colorEditPath = self.currentPath;
    // Picks from this panel session undo together, apart from earlier typing.
    [self.textView breakUndoCoalescing];
    // Caret on the color, so nothing is selected when the panel also sends
    // changeColor: to the text view.
    [self.textView setSelectedRange:NSMakeRange(start + span.start, 0)];

    NSColorPanel *panel = [NSColorPanel sharedColorPanel];
    panel.showsAlpha = YES;
    panel.continuous = YES;
    panel.target = nil;   // setting the starting color must not count as a pick
    panel.action = NULL;
    panel.color = MCColor(span.color);
    panel.target = self;
    panel.action = @selector(colorPicked:);
    [panel orderFront:nil];
}

- (void)colorPicked:(NSColorPanel *)panel {
    NSTextView *tv = self.textView;
    if (!self.colorEditPath || ![self.currentPath isEqualToString:self.colorEditPath] ||
        !tv.editable)
        return;
    NSColor *c = [panel.color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!c) return;
    auto byte = [](CGFloat v) { return (uint8_t)lround(MIN(MAX(v, 0.0), 1.0) * 255); };
    Rgba rgba;
    rgba.r = byte(c.redComponent);
    rgba.g = byte(c.greenComponent);
    rgba.b = byte(c.blueComponent);
    rgba.a = MIN(MAX(c.alphaComponent, 0.0), 1.0);

    NSString *str = tv.string;
    if (self.colorLineStart > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(self.colorLineStart, 0)];
    NSRange lineRange = NSMakeRange(start, contentsEnd - start);
    std::u16string line = U16([str substringWithRange:lineRange]);
    ColorSpan span;
    if (!Settings::findColor(line, span)) return;   // the line was edited away
    std::u16string updated = Settings::setColor(line, rgba);
    if (updated == line) return;
    NSString *replacement = FromU16(updated);
    if (![tv shouldChangeTextInRange:lineRange replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:lineRange withString:replacement];
    [tv didChangeText];
    [tv.undoManager setActionName:@"Change Color"];
    ColorSpan now;
    if (Settings::findColor(updated, now))
        [tv setSelectedRange:NSMakeRange(start + now.start, 0)];

    // Save shortly after the last change, so dragging around the color wheel
    // doesn't write the file dozens of times a second.
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(saveCurrentFile:)
                                               object:nil];
    [self performSelector:@selector(saveCurrentFile:) withObject:nil afterDelay:0.15];
}

// Cmd+/ : comment or uncomment the lines the selection touches.
- (void)toggleComment:(id)sender {
    NSTextView *tv = self.textView;
    std::string marker = self.currentPath
        ? LineComments::markerFor(self.currentPath.UTF8String) : "";
    if (self.window.firstResponder != tv || !tv.editable || marker.empty()) {
        NSBeep();
        return;
    }
    NSRange sel = tv.selectedRange;
    LineComments::Result r = LineComments::toggle(U16(tv.string), sel.location,
                                                  NSMaxRange(sel), marker);
    if (!r.changed) return;
    NSRange range = NSMakeRange(r.replaceStart, r.replaceLength);
    NSString *replacement = FromU16(r.replacement);
    // Each toggle is its own undo step, not merged with typing or the last
    // toggle.
    [tv breakUndoCoalescing];
    if (![tv shouldChangeTextInRange:range replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:range withString:replacement];
    [tv didChangeText];
    [tv breakUndoCoalescing];
    [tv.undoManager setActionName:@"Toggle Comment"];
    [tv setSelectedRange:NSMakeRange(r.selStart, r.selEnd - r.selStart)];
}

// -------------------------------------------------------------- editing hooks
// Arrows, Return, Tab and Esc drive the completion list while it is open;
// Option+Esc (complete:) asks the language server.
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    return [self.lsp handleCommand:sel];
}

// Ctrl+Space, F12, Cmd+I. The session beeps and says why when the file has
// no language server.
- (BOOL)lspActionAllowed {
    if (self.window.firstResponder == self.textView && self.textView.editable) return YES;
    NSBeep();
    return NO;
}
- (void)triggerCompletion:(id)sender {
    if ([self lspActionAllowed]) [self.lsp triggerCompletion];
}
- (void)goToDefinition:(id)sender {
    if ([self lspActionAllowed]) [self.lsp goToDefinition];
}
- (void)showHoverInfo:(id)sender {
    if ([self lspActionAllowed]) [self.lsp showHoverInfo];
}

- (void)textDidChange:(NSNotification *)note {
    self.sourceText = self.textView.string;
    if (!self.dirty) { self.dirty = YES; [self updateTitle]; }
    [self flushHighlighting];   // recolor the lines this change touched
}

- (void)saveCurrentFile:(id)sender {
    // A message (welcome, binary file) is not the file's contents.
    if (!self.currentPath || self.showingMessage || self.isImage || self.isPDF) return;
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
        [self.lsp documentSaved];
        [self updateTitle];
    } else {
        NSAlert *a = [NSAlert alertWithError:err];
        [a runModal];
    }
}

- (void)togglePreview:(id)sender {
    if (!self.canTogglePreview) {
        NSBeep();
        return;
    }
    [self revealEditor];
    if (self.textView.editable) self.sourceText = self.textView.string;  // keep edits
    self.previewMode = !self.previewMode;
    [self refreshDisplay];
    [self updateTitle];
}

- (void)updateTitle {
    NSString *name = self.currentPath.lastPathComponent ?: @"MiniCode";
    NSString *flag = self.dirty ? @"● " : @"";
    NSString *mode = (self.canTogglePreview && self.previewMode) ? @"  [Preview]" : @"";
    if (self.isImage)
        mode = [NSString stringWithFormat:@"  %.0f × %.0f",
                self.imagePixels.width, self.imagePixels.height];
    if (self.isPDF) {
        NSUInteger n = self.pdfView.document.pageCount;
        mode = n == 1 ? @"  1 page"
                      : [NSString stringWithFormat:@"  %lu pages", (unsigned long)n];
    }
    self.window.title = [NSString stringWithFormat:@"%@%@ — MiniCode%@",
                         flag, name, mode];
    self.window.documentEdited = self.dirty;
    if (self.hintsVisible) [self updateHints];
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

        AppSettings *cfg = [AppSettings shared];
        NSFont *font = body;
        NSColor *color = [cfg text:Surface::Editor];
        NSMutableDictionary *a = [NSMutableDictionary dictionary];

        if (r.heading > 0) {
            CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
            font = [NSFont boldSystemFontOfSize:sizes[r.heading]];
            color = [cfg markdown:MarkdownColor::Heading];
            ps.paragraphSpacing = 8.0;
            ps.paragraphSpacingBefore = r.heading <= 2 ? 18.0 : 12.0;  // gap above
        }
        if (r.codeBlock || r.code) {
            font = mono;
            color = [cfg markdown:MarkdownColor::Code];
            a[NSBackgroundColorAttributeName] =
                MCColor(cfg.settings.markdownCodeBackground());
        }
        if (r.table) {
            font = mono;   // monospace keeps the padded columns aligned
            if (!r.code) color = r.bold ? [cfg markdown:MarkdownColor::Heading]
                                        : [cfg text:Surface::Editor];
        }
        if (r.quote) {
            color = [cfg markdown:MarkdownColor::Quote];
            ps.headIndent = 16; ps.firstLineHeadIndent = 16;
        }
        if (r.rule) {
            // Draw a rule as a full line of box-drawing chars.
            s = @"────────────────────────────────";
            color = Hex(0x555555);
        }
        if (r.link) { color = [cfg markdown:MarkdownColor::Link]; a[NSUnderlineStyleAttributeName] =
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
    _liveHighlight = NO;   // rendered Markdown is not source
    [self.textView.textStorage setAttributedString:out];
    [self.textView scrollToBeginningOfDocument:nil];
}

- (void)setPlainMessage:(NSString *)msg {
    NSDictionary *a = @{
        NSFontAttributeName: [NSFont systemFontOfSize:14],
        NSForegroundColorAttributeName: Hex(0x9CA3AF),
    };
    _liveHighlight = NO;
    [self.textView.textStorage setAttributedString:
        [[NSAttributedString alloc] initWithString:msg attributes:a]];
    self.showingMessage = YES;
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
        [self resetViewMode];     // no stale preview or image over the welcome
        [self.lsp setRoot:dir];   // servers belong to the old folder
        [self.outline reloadData];
        [self showWelcome];
        [self relayoutRightArea];
        [self.terminal setDirectory:dir];   // keep terminal cwd in sync
        [self startWatching:dir];           // watch the new folder
        self.window.title = [NSString stringWithFormat:@"MiniCode — %@",
                             dir.lastPathComponent];
    }
}

@end
