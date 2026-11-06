// EditorController.mm — Objective-C++. Bridges the pure-C++ core (syntax
// highlighter, markdown parser) to AppKit views.
#import "EditorController+Private.h"

// A task list item's box, drawn rather than the ☐ and ☑ characters, whose
// look depends on the font. Checked, it is filled with the link color and
// carries a check mark.
@interface MCTaskBoxCell : NSTextAttachmentCell
@property(nonatomic) BOOL checked;
@property(nonatomic, strong) NSColor *ink, *fill;
@end

@implementation MCTaskBoxCell
- (NSSize)cellSize { return NSMakeSize(14, 14); }
- (NSPoint)cellBaselineOffset { return NSMakePoint(0, -2); }
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    NSRect box = NSInsetRect(frame, 0.75, 0.75);
    NSBezierPath *outline = [NSBezierPath bezierPathWithRoundedRect:box xRadius:3 yRadius:3];
    if (self.checked) {
        [self.fill setFill];
        [outline fill];
        // The text view is flipped: y grows downwards.
        NSBezierPath *tick = [NSBezierPath bezierPath];
        [tick moveToPoint:NSMakePoint(NSMinX(box) + 3.0, NSMidY(box) + 0.2)];
        [tick lineToPoint:NSMakePoint(NSMinX(box) + 5.6, NSMaxY(box) - 3.2)];
        [tick lineToPoint:NSMakePoint(NSMaxX(box) - 2.8, NSMinY(box) + 3.2)];
        tick.lineWidth = 1.8;
        tick.lineCapStyle = NSLineCapStyleRound;
        tick.lineJoinStyle = NSLineJoinStyleRound;
        [[NSColor whiteColor] setStroke];
        [tick stroke];
    } else {
        outline.lineWidth = 1.3;
        [[self.ink colorWithAlphaComponent:0.75] setStroke];
        [outline stroke];
    }
}
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view
       characterIndex:(NSUInteger)charIndex layoutManager:(NSLayoutManager *)lm {
    [self drawWithFrame:frame inView:view];
}
@end

// The cell is handed out by an override, since one set in init reads back
// nil (see MarkdownImage).
@interface MCTaskBox : NSTextAttachment
@property(nonatomic, strong) MCTaskBoxCell *box;
@end

@implementation MCTaskBox
- (id<NSTextAttachmentCell>)attachmentCell { return self.box; }
@end

@implementation EditorController

- (instancetype)initWithRootPath:(NSString *)path {
    if ((self = [super init])) {
        _root = [FileItem new];
        _root.path = path; _root.isDir = YES;
        [_root loadChildren];
        _terminalHeight = 220;
        _browserFraction = 0.5;
        _tabs = [NSMutableArray array];
        _noTabUndo = [NSUndoManager new];
        _pendingStart = _pendingEnd = NSNotFound;
        [self setUpTreeDetails];
        _agents = [[MCAgentWindow alloc] initWithHost:self];
    }
    return self;
}

- (NSString *)rootPath { return _root.path; }

// KVO context for the player item (see observeMediaItem:).
static void *kMediaItemContext = &kMediaItemContext;

- (void)dealloc {
    [self stopMedia];
    [self stopWatching];
    [self.window removeObserver:self forKeyPath:@"title"];
}

// The drawn title bar shows the window's title, which is set in several places.
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    if (object == self.window && [keyPath isEqualToString:@"title"])
        self.titleLabel.stringValue = self.window.title ?: @"";
    else if (context == kMediaItemContext) {
        // AVFoundation may report from another thread.
        AVPlayerItem *item = object;
        dispatch_async(dispatch_get_main_queue(), ^{ [self mediaItemChanged:item]; });
    } else
        [super observeValueForKeyPath:keyPath ofObject:object change:change
                              context:context];
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

// keys.conf, as Settings… opens settings.conf; the first time it is written
// with every command and its keys, commented out (Commands::defaultFileText).
- (void)openKeyboardShortcuts:(id)sender {
    (void)sender;
    MCKeymap *keys = [MCKeymap shared];
    if (![keys ensureFileExists]) {
        [self warn:[NSString stringWithFormat:@"Could not create %@.", keys.path]];
        return;
    }
    [self openFileAtPath:keys.path];
    [self focusEditor:nil];
}

- (TerminalView *)terminal { return self.terminalPanel.activeTerminal; }

- (NSArray<NSString *> *)recentTabPaths {
    NSArray<MCTab *> *byUse = [_tabs sortedArrayUsingComparator:^NSComparisonResult(MCTab *a, MCTab *b) {
        return a.lastActive == b.lastActive ? NSOrderedSame
             : a.lastActive > b.lastActive  ? NSOrderedAscending : NSOrderedDescending;
    }];
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (MCTab *t in byUse)
        if (t.currentPath) [paths addObject:t.currentPath];
    return paths;
}

// A file's diff in the editor's slot, read-only and colored, the way an
// image or a PDF takes it. Opening any file puts the editor back.
- (void)showDiffNamed:(NSString *)name path:(NSString *)path data:(NSData *)diff {
    (void)path;
    [self showDiffTitled:name data:diff commit:NO];
}

// A commit from the graph, the same way: its header and message, then its
// diff. The title is "<short hash> <subject>".
- (void)showCommitTitled:(NSString *)title data:(NSData *)show {
    [self showDiffTitled:title data:show commit:YES];
}

- (void)showDiffTitled:(NSString *)name data:(NSData *)diff commit:(BOOL)commit {
    [self revealEditor];
    // Diffs and commits share one tab, which each new one replaces, so
    // stepping through the changed files does not leave a tab for each.
    MCTab *tab = nil;
    for (MCTab *t in _tabs) if (t.isDiff && !t.comparison && !t.page) { tab = t; break; }
    if (!tab) {
        tab = [MCTab new];
        tab.loaded = YES;
        tab.isDiff = YES;
        [self insertTab:tab];
    }
    [self activateTab:tab];
    if (!self.diffScroll) {
        PanelScrollView *scroll = [[PanelScrollView alloc] init];
        scroll.hasVerticalScroller = YES;
        scroll.autohidesScrollers = YES;
        scroll.automaticallyAdjustsContentInsets = NO;
        scroll.panelColor = [[AppSettings shared] background:Surface::Editor];
        NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
        tv.editable = NO;
        tv.selectable = YES;
        tv.richText = YES;
        tv.drawsBackground = NO;
        tv.usesFontPanel = NO;
        tv.usesFindBar = YES;
        tv.incrementalSearchingEnabled = YES;
        tv.textContainerInset = NSMakeSize(8, 8);
        tv.minSize = NSMakeSize(0, 0);
        tv.maxSize = NSMakeSize(FLT_MAX, FLT_MAX);
        tv.verticallyResizable = YES;
        tv.horizontallyResizable = NO;   // long lines wrap, as in the editor
        tv.autoresizingMask = NSViewWidthSizable;
        tv.textContainer.widthTracksTextView = YES;
        scroll.documentView = tv;
        [self.rightArea addSubview:scroll positioned:NSWindowBelow
                        relativeTo:self.termDivider];
        self.diffScroll = scroll;
        self.diffView = tv;
    }
    self.currentPath = nil;   // nothing here is a file to save or reload
    [self resetViewMode];
    self.isDiff = YES;
    self.diffName = name;
    self.diffData = diff;
    self.diffIsCommit = commit;
    [self setPlainMessage:@""];   // the hidden text view holds nothing to save
    [self renderDiff];
    [self.diffView scrollPoint:NSZeroPoint];
    [self relayoutRightArea];
    [self.lsp documentOpened:nil];
    [self updateTitle];
}

- (void)renderDiff {
    if (_activeTab.page) return;   // a page draws itself
    if (_activeTab.comparison) {
        [self compareViewMade].comparison = _activeTab.comparison;
        return;
    }
    NSFont *mono = [NSFont monospacedSystemFontOfSize:12.5 weight:NSFontWeightRegular];
    [self.diffView.textStorage setAttributedString:
        self.diffIsCommit ? MCGitCommitText(self.diffData, mono)
                          : MCGitDiffText(self.diffData, mono)];
}

// ---------------------------------------------------------- a tab's menu
// A right click on a tab: closing it and its neighbours, and the file
// commands the tree offers for its file. Each item carries its tab.
- (NSMenu *)menuForTabAtIndex:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_tabs.count) return nil;
    MCTab *t = _tabs[(NSUInteger)i];
    NSMenu *m = [[NSMenu alloc] init];
    m.autoenablesItems = NO;
    NSMenuItem *(^add)(NSString *, SEL, BOOL) = ^NSMenuItem *(NSString *title, SEL a, BOOL on) {
        NSMenuItem *it = [m addItemWithTitle:title action:a keyEquivalent:@""];
        it.target = self;
        it.representedObject = t;
        it.enabled = on;
        return it;
    };
    const NSInteger count = (NSInteger)_tabs.count;
    add(@"Close", @selector(closeTabFromMenu:), YES);
    add(@"Close Other Tabs", @selector(closeOtherTabsFromMenu:), count > 1);
    add(@"Close Tabs to the Right", @selector(closeTabsToTheRightFromMenu:), i < count - 1);
    add(@"Close All Tabs", @selector(closeAllTabsFromMenu:), YES);
    NSString *path = t.currentPath;
    if (path) {
        const BOOL inRoot = [self pathFromRoot:path] != nil;
        const BOOL onDisk = [[NSFileManager defaultManager] fileExistsAtPath:path];
        [m addItem:[NSMenuItem separatorItem]];
        add(@"Copy Path", @selector(copyTabPath:), YES);
        add(@"Copy Relative Path", @selector(copyTabRelativePath:), inRoot);
        add(@"Reveal in Finder", @selector(revealTabInFinder:), onDisk);
        add(@"Reveal in File Tree", @selector(revealTabInTree:), inRoot && onDisk);
        [m addItem:[NSMenuItem separatorItem]];
        add(@"Rename…", @selector(renameTabFile:), onDisk);
        add(@"Move to Trash", @selector(trashTabFile:), onDisk);
    }
    return m;
}

- (MCTab *)tabOfMenuItem:(id)sender {
    id t = [sender respondsToSelector:@selector(representedObject)]
               ? [sender representedObject] : nil;
    return [t isKindOfClass:MCTab.class] && [_tabs containsObject:t] ? t : nil;
}

- (void)closeTabFromMenu:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t) [self closeTab:t prompt:YES];
}

// Closes `tabs` in turn; Cancel at any unsaved one stops there.
- (void)closeTabsInList:(NSArray<MCTab *> *)tabs {
    for (MCTab *each in tabs)
        if (![self closeTab:each prompt:YES]) return;
}

- (void)closeOtherTabsFromMenu:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (!t) return;
    NSMutableArray *others = [_tabs mutableCopy];
    [others removeObject:t];
    [self closeTabsInList:others];
}

- (void)closeTabsToTheRightFromMenu:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (!t) return;
    const NSUInteger i = [_tabs indexOfObject:t];
    [self closeTabsInList:[_tabs subarrayWithRange:NSMakeRange(i + 1, _tabs.count - i - 1)]];
}

- (void)closeAllTabsFromMenu:(id)sender {
    [self closeTabsInList:[_tabs copy]];
}

- (void)copyTabPath:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self copyText:t.currentPath];
}

- (void)copyTabRelativePath:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    NSString *rel = t.currentPath ? [self pathFromRoot:t.currentPath] : nil;
    if (rel) [self copyText:rel];
}

- (void)revealTabInFinder:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self revealPathInFinder:t.currentPath];
}

// Selects the tab's file in the tree, bringing the tree back if the sidebar
// is collapsed or shows Source Control.
- (void)revealTabInTree:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    NSString *rel = t.currentPath ? [self pathFromRoot:t.currentPath] : nil;
    if (!rel) return;
    if (self.gitPanelVisible) [self toggleSourceControl:nil];
    if (self.agentsVisible) [self toggleAgents:nil];
    if (self.sidebarCollapsed) [self toggleSidebar:nil];
    // Spelled from the tree's root, which revealPath: walks down from.
    [self revealPath:[rel isEqualToString:@"."] ? _root.path
                                                : [_root.path stringByAppendingPathComponent:rel]
             andOpen:NO];
    [self.window makeFirstResponder:self.outline];
}

// The same commands in the main menu (File), for the tab on show, so the
// menu bar and the command palette have them too.
- (NSMenuItem *)menuItemForActiveTab {
    NSMenuItem *it = [[NSMenuItem alloc] init];
    it.representedObject = _activeTab;
    return it;
}
- (void)closeOtherTabs:(id)sender {
    if (_activeTab) [self closeOtherTabsFromMenu:[self menuItemForActiveTab]];
}
- (void)closeTabsToTheRight:(id)sender {
    if (_activeTab) [self closeTabsToTheRightFromMenu:[self menuItemForActiveTab]];
}
- (void)closeAllTabs:(id)sender { [self closeAllTabsFromMenu:sender]; }
- (void)copyActiveFilePath:(id)sender { [self copyTabPath:[self menuItemForActiveTab]]; }
- (void)copyActiveFileRelativePath:(id)sender {
    [self copyTabRelativePath:[self menuItemForActiveTab]];
}
- (void)revealActiveFileInFinder:(id)sender {
    [self revealTabInFinder:[self menuItemForActiveTab]];
}
- (void)revealActiveFileInTree:(id)sender {
    [self revealTabInTree:[self menuItemForActiveTab]];
}

// What those items can do for the tab on show, as its right click menu
// decides it; NO for an action that is not one of them.
- (BOOL)validateActiveTabAction:(SEL)a enabled:(BOOL *)enabled {
    MCTab *t = _activeTab;
    const NSInteger count = (NSInteger)_tabs.count;
    const NSInteger i = t ? (NSInteger)[_tabs indexOfObject:t] : -1;
    NSString *path = t.currentPath;
    const BOOL inRoot = path && [self pathFromRoot:path] != nil;
    const BOOL onDisk = path && [[NSFileManager defaultManager] fileExistsAtPath:path];
    if (a == @selector(reopenClosedTab:)) *enabled = [self canReopenClosedTab];
    else if (a == @selector(closeOtherTabs:)) *enabled = t && count > 1;
    else if (a == @selector(closeTabsToTheRight:)) *enabled = t && i < count - 1;
    else if (a == @selector(closeAllTabs:)) *enabled = count > 0;
    else if (a == @selector(copyActiveFilePath:)) *enabled = path != nil;
    else if (a == @selector(copyActiveFileRelativePath:)) *enabled = inRoot;
    else if (a == @selector(revealActiveFileInFinder:)) *enabled = onDisk;
    else if (a == @selector(revealActiveFileInTree:)) *enabled = inRoot && onDisk;
    else return NO;
    return YES;
}

- (void)renameTabFile:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self renamePath:t.currentPath];
}

- (void)trashTabFile:(id)sender {
    MCTab *t = [self tabOfMenuItem:sender];
    if (t.currentPath) [self trashPath:t.currentPath];
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
    _haveSourcePosition = NO;
    [_awaitedWebImages removeAllObjects];
    [_awaitedMath removeAllObjects];
    self.sourceText = nil;
    self.savedText = nil;
    self.dirty = NO;
    self.textView.editable = NO;
    self.imageView.image = nil;
    self.pdfView.document = nil;
    [self stopMedia];   // the sound must stop when anything else is shown
    self.viewerKind = MCViewerNone;
    [_viewers clear];
    self.isDiff = NO;
    self.diffName = nil;
    self.diffData = nil;
    self.diffIsCommit = NO;
    _activeTab.comparison = nil;
    [self.diffView.textStorage setAttributedString:[NSAttributedString new]];
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

// Video and audio that AVFoundation plays on this Mac. WebM, Matroska, Ogg
// video, WMV and FLV are not, so they are left to the "Cannot display"
// message. .ts and .mts are TypeScript here, not MPEG transport streams.
+ (BOOL)isMediaPath:(NSString *)path {
    static NSSet<NSString *> *exts;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        exts = [NSSet setWithArray:@[
            @"mp4", @"m4v", @"mov", @"3gp", @"avi",                 // video
            @"mp3", @"wav", @"m4a", @"aac", @"aif", @"aiff",       // audio
            @"flac", @"ogg", @"opus"]];
    });
    return [exts containsObject:path.pathExtension.lowercaseString];
}

// Watch one player item for its status, size and duration; nil stops.
- (void)observeMediaItem:(AVPlayerItem *)item {
    if (self.mediaItem == item) return;
    for (NSString *key in @[@"status", @"presentationSize", @"duration"])
        [self.mediaItem removeObserver:self forKeyPath:key context:kMediaItemContext];
    self.mediaItem = item;
    for (NSString *key in @[@"status", @"presentationSize", @"duration"])
        [item addObserver:self forKeyPath:key options:0 context:kMediaItemContext];
}

// Pause and let go of the player, so nothing keeps playing once another file,
// folder or message takes the slot, or the window closes.
- (void)stopMedia {
    [self.playerView.player pause];
    [self observeMediaItem:nil];
    self.playerView.player = nil;
    self.isMedia = NO;
}

// Size and duration arrive once the item is ready. A file AVFoundation turns
// down only at this point gets a message in place of the player.
- (void)mediaItemChanged:(AVPlayerItem *)item {
    if (!self.isMedia || item != self.mediaItem) return;   // stale
    if (item.status == AVPlayerItemStatusFailed) {
        NSString *name = self.currentPath.lastPathComponent ?: @"";
        NSString *why = item.error.localizedDescription ?: @"";
        [self stopMedia];
        [self setPlainMessage:[NSString stringWithFormat:
            @"Cannot play “%@”.\n\n(%@)", name, why]];
        [self relayoutRightArea];
        [self updateTitle];
        return;
    }
    if (item.status != AVPlayerItemStatusReadyToPlay) return;
    CMTime d = item.duration;
    self.mediaSeconds = CMTIME_IS_NUMERIC(d) ? CMTimeGetSeconds(d) : -1;
    self.mediaPixels = item.presentationSize;
    [self updateTitle];
}

// Show a video or audio file in the editor's slot with AVKit's player: the
// first frame, paused, with the usual transport bar and full screen button.
// A reload (the file changed on disk) keeps the time and whether it was
// playing. Returns NO if AVFoundation can't play it, and the caller falls
// through to the "Cannot display" message.
- (BOOL)showMediaAtPath:(NSString *)path {
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path]
                                            options:nil];
    if (!asset.playable) return NO;
    if (!self.playerView) {
        AVPlayerView *v = [[AVPlayerView alloc] initWithFrame:NSZeroRect];
        v.controlsStyle = AVPlayerViewControlsStyleInline;
        v.showsFullScreenToggleButton = YES;
        v.videoGravity = AVLayerVideoGravityResizeAspect;
        v.wantsLayer = YES;
        v.layer.backgroundColor =
            [[AppSettings shared] background:Surface::Editor].CGColor;
        [self.rightArea addSubview:v positioned:NSWindowBelow
                        relativeTo:self.termDivider];
        self.playerView = v;
    }
    AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
    AVPlayer *player = self.playerView.player;
    if (self.isMedia && player) {
        CMTime at = player.currentTime;
        BOOL playing = player.rate != 0;
        [player replaceCurrentItemWithPlayerItem:item];
        [player seekToTime:at toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
        if (playing) [player play];
    } else {
        player = [AVPlayer playerWithPlayerItem:item];
        player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
        self.playerView.player = player;
        self.mediaPixels = NSZeroSize;
        self.mediaSeconds = -1;
    }
    [self observeMediaItem:item];
    self.isMedia = YES;
    self.currentExt = path.pathExtension.lowercaseString;
    [self setPlainMessage:@""];   // nothing in the text view to save
    [self relayoutRightArea];
    [self.lsp documentOpened:nil];
    [self updateTitle];
    [self mediaItemChanged:item];   // a reload may be ready already
    return YES;
}

- (MCFileViewers *)viewers {
    if (!_viewers) {
        _viewers = [[MCFileViewers alloc] initWithContainer:self.rightArea below:self.termDivider];
        __weak EditorController *weakSelf = self;
        _viewers.onTextEdited = ^(NSString *text, NSString *name) {
            [weakSelf viewerEditedText:text actionName:name];
        };
        _viewers.undoManagerProvider = ^NSUndoManager * {
            EditorController *me = weakSelf;
            return me ? me->_activeTab.undoManager : nil;
        };
    }
    return _viewers;
}

// An edit made in a viewer over the text (a cell of a CSV table): the
// file's new text becomes the tab's, dirty and undoable like typing.
- (void)viewerEditedText:(NSString *)text actionName:(NSString *)name {
    [self setViewerText:text actionName:name reshow:NO];
}

- (void)setViewerText:(NSString *)text actionName:(NSString *)name reshow:(BOOL)reshow {
    NSString *old = self.sourceText ?: @"";
    if (!text || [text isEqualToString:old]) return;
    NSUndoManager *undo = _activeTab.undoManager;
    __weak EditorController *weakSelf = self;
    [undo registerUndoWithTarget:self handler:^(EditorController *target) {
        (void)target;
        [weakSelf setViewerText:old actionName:name reshow:YES];
    }];
    [undo setActionName:name];
    self.sourceText = text;
    [self noteEdited];
    [self updateTitle];
    if (reshow) [self refreshDisplay];   // an undo: the viewer (or the text) shows it
}

// Show the active tab's file in a viewer that holds no text (Quick Look,
// say): nothing in the text view to save, as for an image. NO when the
// viewer cannot show it.
- (BOOL)showViewer:(MCViewerKind)kind {
    if (![self.viewers showWithoutText:kind path:self.currentPath]) return NO;
    self.viewerKind = kind;
    self.currentExt = self.currentPath.pathExtension.lowercaseString;
    [self setPlainMessage:@""];
    [self relayoutRightArea];
    [self.lsp documentOpened:nil];
    [self updateTitle];
    return YES;
}

- (BOOL)canTogglePlayback { return self.isMedia && self.playerView.player != nil; }

// Shift+Cmd+Space: play or pause whatever the player holds, from any pane.
// At the end it starts again from the beginning.
- (void)togglePlayback:(id)sender {
    if (![self canTogglePlayback]) { NSBeep(); return; }
    AVPlayer *p = self.playerView.player;
    if (p.rate != 0) { [p pause]; return; }
    CMTime end = p.currentItem.duration;
    if (CMTIME_IS_NUMERIC(end) && CMTimeCompare(p.currentTime, end) >= 0)
        [p seekToTime:kCMTimeZero];
    [p play];
}

// Show a file: its tab when it has one, else a new tab just right of the
// one on show. The file in the tab that was on show stays as it was,
// unsaved changes and all.
- (void)openFileAtPath:(NSString *)path {
    [self revealEditor];
    if (!path) return;
    if ([path isEqualToString:self.currentPath]) return;  // avoid double-render
    MCTab *tab = [self tabForPath:path];
    if (!tab) {
        tab = [MCTab new];
        tab.currentPath = path;
        [self insertTab:tab];
    }
    [self activateTab:tab];
}

// Read the active tab's file and show it: the first time the tab is shown,
// and again when it is reloaded.
- (void)loadActiveTab {
    NSString *path = self.currentPath;
    if (!path) return;
    // A buffer brought back from a backup (restoreSession) goes over the
    // disk's text once that is read; what it was based on is compared then.
    MCTab *restoring = _activeTab.restoredBackup && _activeTab.dirty ? _activeTab : nil;
    NSString *restored = restoring.sourceText;
    NSDate *restoredBase = restoring.fileModDate;
    const uint64_t restoredInode = restoring.fileID;
    _activeTab.restoredBackup = NO;
    _activeTab.loaded = YES;
    _activeTab.gone = NO;
    _activeTab.pendingSort = nil;
    [self forgetHistoryOf:_activeTab];   // the text is read afresh
    [self resetViewMode];
    // Images are routed by extension before any attempt to read them as text.
    if ([EditorController isImagePath:path] && [self showImageAtPath:path]) {
        [self recordModDate];
        return;
    }
    if ([path.pathExtension.lowercaseString isEqualToString:@"pdf"] &&
        [self showPDFAtPath:path]) {
        [self recordModDate];
        return;
    }
    if ([EditorController isMediaPath:path] && [self showMediaAtPath:path]) {
        [self recordModDate];
        return;
    }
    // Binary by its content (or a document only Quick Look shows) is never
    // opened as text; a backup, being text, still is.
    const MCViewerKind viewer = restored ? MCViewerNone : MCViewerKindForFile(path);
    if (MCViewerKindWithoutText(viewer) && [self showViewer:viewer]) {
        [self recordModDate];
        return;
    }
    NSError *err = nil;
    NSString *content = [NSString stringWithContentsOfFile:path
                                                  encoding:NSUTF8StringEncoding
                                                     error:&err];
    // The file is gone, or no longer reads as text: the backup is all there is.
    if (!content && restored) content = @"";
    // Not UTF-8: Quick Look shows what it can (text in another encoding,
    // or the file's icon).
    if (!content && [[NSFileManager defaultManager] fileExistsAtPath:path] &&
        [self showViewer:MCViewerQuickLook]) {
        [self recordModDate];
        return;
    }
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
    // Markdown and LaTeX both open in their preview, unless the file is
    // empty: a blank preview of a new file looks like nothing opened.
    BOOL blank = [content stringByTrimmingCharactersInSet:
                     NSCharacterSet.whitespaceAndNewlineCharacterSet].length == 0;
    // A CSV opens as its table, the text a Shift+Cmd+V away; a JSON file
    // gets an outline beside its text.
    if (MCViewerKindOverText(viewer) || MCViewerKindBesideText(viewer) ||
        MCViewerKindRendersMarkdown(viewer))
        self.viewerKind = viewer;
    self.previewMode = (self.isMarkdown || self.isLatex || MCViewerKindOverText(viewer) ||
                        MCViewerKindRendersMarkdown(viewer)) && !blank;
    self.savedText = content;
    self.dirty = NO;
    [self recordModDate];
    if (restored) [self showRestoredText:restored base:restoredBase inode:restoredInode];
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
// What the tab knows of its file on disk: when it was last changed, and its
// inode, so a rename made outside can be followed.
- (void)recordDiskStateOf:(MCTab *)t {
    NSDictionary *attrs = t.currentPath
        ? [[NSFileManager defaultManager] attributesOfItemAtPath:t.currentPath error:nil] : nil;
    t.fileModDate = attrs.fileModificationDate;
    t.fileID = [attrs[NSFileSystemFileNumber] unsignedLongLongValue];
    if (t == _activeTab) [self updateSavedStatus];
}
- (void)recordModDate { [self recordDiskStateOf:_activeTab]; }

// The tab's text was replaced from disk. Its undo steps were made for the
// old text, and taken now they would edit the new text at the old places
// (or throw past its end), so they go, and so do the LaTeX preview's.
- (void)forgetHistoryOf:(MCTab *)t {
    [t.undoManager removeAllActions];
    t.folds = nil;
    [t.latex forgetEdits];
}

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

// ====================================================================== tabs
// The document properties are the active tab's (see MCTab): with no tab
// open they read as nil, NO and zero, and setting them does nothing.
#define MC_TAB_PROPERTY(type, name, setter)                     \
    - (type)name { return _activeTab.name; }                    \
    - (void)setter:(type)value { _activeTab.name = value; }
MC_TAB_PROPERTY(NSString *, currentPath, setCurrentPath)
MC_TAB_PROPERTY(NSString *, sourceText, setSourceText)
MC_TAB_PROPERTY(NSString *, savedText, setSavedText)
MC_TAB_PROPERTY(NSString *, currentExt, setCurrentExt)
MC_TAB_PROPERTY(BOOL, isMarkdown, setIsMarkdown)
MC_TAB_PROPERTY(BOOL, isLatex, setIsLatex)
MC_TAB_PROPERTY(BOOL, previewMode, setPreviewMode)
MC_TAB_PROPERTY(BOOL, dirty, setDirty)
MC_TAB_PROPERTY(NSDate *, fileModDate, setFileModDate)
MC_TAB_PROPERTY(LatexView *, latex, setLatex)
MC_TAB_PROPERTY(BOOL, isImage, setIsImage)
MC_TAB_PROPERTY(NSSize, imagePixels, setImagePixels)
MC_TAB_PROPERTY(BOOL, isPDF, setIsPDF)
MC_TAB_PROPERTY(BOOL, isMedia, setIsMedia)
MC_TAB_PROPERTY(MCViewerKind, viewerKind, setViewerKind)
MC_TAB_PROPERTY(NSSize, mediaPixels, setMediaPixels)
MC_TAB_PROPERTY(double, mediaSeconds, setMediaSeconds)
MC_TAB_PROPERTY(BOOL, showingMessage, setShowingMessage)
MC_TAB_PROPERTY(BOOL, isDiff, setIsDiff)
MC_TAB_PROPERTY(NSString *, diffName, setDiffName)
MC_TAB_PROPERTY(NSData *, diffData, setDiffData)
MC_TAB_PROPERTY(BOOL, diffIsCommit, setDiffIsCommit)
#undef MC_TAB_PROPERTY

// Typing, and edits made from the Markdown preview, are undone per tab.
- (NSUndoManager *)undoManagerForTextView:(NSTextView *)view {
    return _activeTab ? _activeTab.undoManager : _noTabUndo;
}

- (BOOL)hasUnsavedChanges {
    for (MCTab *t in _tabs) if (t.dirty) return YES;
    return NO;
}

- (void)buildTabStrip {
    MCTabStrip *strip = [[MCTabStrip alloc] initWithFrame:NSZeroRect];
    strip.hidden = YES;
    strip.surface = @"tabs";
    __weak EditorController *weakSelf = self;
    strip.onSelect = ^(NSInteger i) { [weakSelf selectTabAtIndex:i]; };
    strip.onClose = ^(NSInteger i) { [weakSelf closeTabAtIndex:i]; };
    strip.menuForTab = ^NSMenu *(NSInteger i) { return [weakSelf menuForTabAtIndex:i]; };
    strip.onDoubleClick = ^(NSInteger i) { [weakSelf keepAgentPreviewTabAtIndex:i]; };
    [self.rightArea addSubview:strip];
    self.tabStrip = strip;
    // With no file open the slot is empty but for one muted line.
    NSTextField *label = [NSTextField labelWithString:@"No file open. Choose one in the file tree."];
    label.font = [NSFont systemFontOfSize:13];
    label.alignment = NSTextAlignmentCenter;
    label.hidden = YES;
    [self.rightArea addSubview:label];
    self.emptyLabel = label;
}

- (void)applyTabSettings {
    AppSettings *cfg = [AppSettings shared];
    self.tabStrip.stripColor = [cfg background:Surface::Sidebar];
    self.tabStrip.activeColor = [cfg background:Surface::Editor];
    self.tabStrip.activeText = [cfg text:Surface::Editor];
    self.tabStrip.inactiveText = [[cfg text:Surface::Sidebar] colorWithAlphaComponent:0.7];
    self.tabStrip.needsDisplay = YES;
    self.emptyLabel.textColor = Hex(0x9CA3AF);
}

// Called by relayoutRightArea with the editor's slot: the strip takes its
// top while any tab is open, and the empty line sits in it while none is.
// Returns what is left for the editor.
- (NSRect)layoutTabStripIn:(NSRect)slot editorShown:(BOOL)shown {
    const BOOL strip = shown && _tabs.count > 0;
    const BOOL empty = shown && _tabs.count == 0;
    self.tabStrip.hidden = !strip;
    self.emptyLabel.hidden = !empty;
    if (empty)
        self.emptyLabel.frame = NSMakeRect(NSMinX(slot) + 16, NSMidY(slot) + 10,
                                           MAX(0, NSWidth(slot) - 32), 20);
    if (!strip) return slot;
    const CGFloat h = MIN(kMCTabStripHeight, NSHeight(slot));
    self.tabStrip.frame = NSMakeRect(NSMinX(slot), NSMaxY(slot) - h, NSWidth(slot), h);
    return NSMakeRect(NSMinX(slot), NSMinY(slot), NSWidth(slot), NSHeight(slot) - h);
}

// The strip again from the tabs: titles, dots for unsaved changes, the
// active one. The editor's slot is laid out again when the strip comes or goes.
- (void)reloadTabStrip {
    std::vector<std::string> paths;
    for (MCTab *t in _tabs) paths.push_back(t.currentPath ? Utf8String(t.currentPath) : "");
    std::vector<std::string> names = EditorTabs::labels(paths);
    NSMutableArray<MCTabStripItem *> *items = [NSMutableArray array];
    for (NSUInteger i = 0; i < _tabs.count; i++) {
        MCTab *t = _tabs[i];
        MCTabStripItem *item = [MCTabStripItem new];
        if (t.page)
            item.title = t.page.pageTitle ?: @"";
        else if (t.comparison)
            item.title = t.comparison.title ?: @"Diff";
        else if (t.isDiff)
            item.title = t.diffIsCommit ? (t.diffName ?: @"Commit")
                                        : [NSString stringWithFormat:@"%@ (diff)", t.diffName ?: @""];
        else
            item.title = [NSString stringWithUTF8String:names[i].c_str()] ?: t.currentPath.lastPathComponent;
        if (t.gone) item.title = [item.title stringByAppendingString:@" (deleted)"];
        item.toolTip = t.currentPath ?: item.title;
        item.dirty = t.dirty;
        item.preview = t.agentPreview;
        [items addObject:item];
    }
    const BOOL wasShown = !self.tabStrip.hidden;
    self.tabStrip.items = items;
    self.tabStrip.activeIndex = _activeTab ? (NSInteger)[_tabs indexOfObject:_activeTab] : -1;
    if (wasShown != (_tabs.count > 0)) [self relayoutRightArea];
}

- (MCTab *)tabForPath:(NSString *)path {
    for (MCTab *t in _tabs) if ([t.currentPath isEqualToString:path]) return t;
    // The same file spelled another way (/tmp and /private/tmp).
    NSString *real = path.stringByResolvingSymlinksInPath;
    for (MCTab *t in _tabs)
        if (t.currentPath && [t.currentPath.stringByResolvingSymlinksInPath isEqualToString:real])
            return t;
    return nil;
}

- (void)insertTab:(MCTab *)tab {
    NSInteger active = _activeTab ? (NSInteger)[_tabs indexOfObject:_activeTab] : -1;
    [_tabs insertObject:tab
                atIndex:(NSUInteger)EditorTabs::insertionIndex((int)active, (int)_tabs.count)];
}

// Put a tab on show. The tab that was on show keeps its text, unsaved
// changes, undo stack and place; a tab shown for the first time reads its
// file, and one shown again is checked against the disk.
- (void)activateTab:(MCTab *)tab {
    if (!tab) return;
    [self revealEditor];
    tab.lastActive = ++_tabStamp;
    if (tab == _activeTab) { [self reloadTabStrip]; return; }
    [self.navigator tabWillSwitch];   // Go Back returns to the tab left
    [_notes activeTabWillChange];
    [self stashActiveTab];
    if (_activeTab) [self autoSaveOnFocusChange];   // the tab that leaves (focus-change)
    _activeTab = tab;
    if (!tab.loaded) {
        [self loadActiveTab];
    } else {
        [self showActiveTab];
        [self checkExternalChange];
    }
    [self reselectCurrentFileInTree];
    [self reloadTabStrip];
    [self saveSession];
    [self.navigator tabDidSwitch];
    [_notes activeTabDidChange];
}

// The active tab is about to leave the screen: remember where its view
// was, and let go of what the shared views hold for it (a playing video
// stops).
- (void)stashActiveTab {
    MCTab *t = _activeTab;
    if (!t) return;
    [self saveBeforeLeavingFormatSave];   // Cmd+S is never lost to a tab switch
    NSTextView *tv = self.textView;
    [self.markdownEditPanel close];
    // A color pick saves the settings file a moment later (colorPicked:);
    // that save belongs to this tab, so it happens now, not in the next one.
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(saveCurrentFile:)
                                               object:nil];
    if (t.dirty && [t.currentPath isEqualToString:self.colorEditPath] && [self isSettingsFile])
        [self saveCurrentFile:nil];
    [tv breakUndoCoalescing];   // typing in the next tab is a new undo step
    [self flushHighlighting];
    if (tv.editable && !t.showingMessage) t.sourceText = tv.string;
    if (tv.editable && !t.showingMessage) t.folds = [self.textView.folds snapshot];
    t.selection = tv.selectedRange;
    t.topCharacter = 0;
    t.topOffset = 0;
    const NSUInteger n = tv.string.length;
    if (n > 0 && !t.showingMessage && !(t.isLatex && t.previewMode)) {
        const NSRect visible = tv.visibleRect;
        NSUInteger top = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)];
        if (top >= n) top = n - 1;
        const NSRect r = [self textRectForCharacter:top];
        t.topCharacter = top;
        t.topOffset = NSIsEmptyRect(r) ? 0 : NSMinY(r) - NSMinY(visible);
    }
    t.haveView = YES;
    if (t.isDiff && self.diffScroll) t.diffScroll = self.diffScroll.contentView.bounds.origin;
    if (t.isDiff && t.comparison && self.compareView) t.diffScroll = self.compareView.scrollOrigin;
    t.sourceSelection = _sourceSelection;
    t.sourceCaretOffset = _sourceCaretOffset;
    t.haveSourcePosition = _haveSourcePosition;
    t.previewEntryScroll = _previewEntryScroll;
    if (t.isPDF && self.pdfView.document) {
        PDFDestination *at = self.pdfView.currentDestination;
        t.pdfPage = [self.pdfView.document indexForPage:at.page];
        t.pdfPoint = at.point;
        t.pdfScale = self.pdfView.scaleFactor;
        t.pdfAutoScales = self.pdfView.autoScales;
    }
    if (t.isMedia && self.playerView.player)
        t.mediaTime = CMTimeGetSeconds(self.playerView.player.currentTime);
    // The shared views let go; the tab's own flags stay as they are.
    [_viewers clear];
    self.imageView.image = nil;
    self.pdfView.document = nil;
    [self.playerView.player pause];
    [self observeMediaItem:nil];
    self.playerView.player = nil;
    [self.diffView.textStorage setAttributedString:[NSAttributedString new]];
    [_awaitedWebImages removeAllObjects];
    [_awaitedMath removeAllObjects];
    t.latex.hidden = YES;
    _liveHighlight = NO;
}

// Show the active tab again from what it kept.
- (void)showActiveTab {
    [_agent documentChanged];   // an edit bar for the tab that left closes
    MCTab *t = _activeTab;
    NSString *path = t.currentPath;
    _sourceSelection = t.sourceSelection;
    _sourceCaretOffset = t.sourceCaretOffset;
    _haveSourcePosition = t.haveSourcePosition;
    _previewEntryScroll = t.previewEntryScroll;
    self.textView.editable = NO;
    if (MCViewerKindWithoutText(t.viewerKind)) {
        if (![self showViewer:t.viewerKind]) [self loadActiveTab];
        return;
    }
    if (t.isImage) {
        if (![self showImageAtPath:path]) [self loadActiveTab];
        return;
    }
    if (t.isPDF) {
        if (![self showPDFAtPath:path]) { [self loadActiveTab]; return; }
        PDFDocument *doc = self.pdfView.document;
        if (t.haveView && t.pdfPage < doc.pageCount) {
            if (!t.pdfAutoScales) self.pdfView.scaleFactor = t.pdfScale;
            [self.pdfView goToDestination:[[PDFDestination alloc]
                initWithPage:[doc pageAtIndex:t.pdfPage] atPoint:t.pdfPoint]];
        }
        return;
    }
    if (t.isMedia) {
        const double at = t.mediaTime;
        if (![self showMediaAtPath:path]) { [self loadActiveTab]; return; }
        if (at > 0)
            [self.playerView.player seekToTime:CMTimeMakeWithSeconds(at, 600)
                               toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
        return;
    }
    if (t.isDiff) {
        [self setPlainMessage:@""];
        [self renderDiff];
        [self relayoutRightArea];
        if (t.comparison) self.compareView.scrollOrigin = t.diffScroll;
        else if (!t.page) [self.diffView scrollPoint:t.diffScroll];
        [self.lsp documentOpened:nil];
        [self updateTitle];
        return;
    }
    if (t.showingMessage) {
        [self setPlainMessage:t.message ?: @""];
        [self relayoutRightArea];
        [self.lsp documentOpened:nil];
        [self updateTitle];
        return;
    }
    [self refreshDisplay];
    [self restoreTextPlaceOf:t];
    // Sorted as it was saved in the background (saveTab:): the sort goes in
    // now, as one undo step, which leaves the buffer as the file is.
    if (t.pendingSort) {
        NSString *sorted = t.pendingSort;
        t.pendingSort = nil;
        if (self.textView.editable) {
            [self replaceSourceWithText:sorted actionName:@"Sort keep-sorted Blocks"];
        } else {
            t.sourceText = sorted;
            [self forgetHistoryOf:t];
            [self refreshDisplay];
        }
    }
    [self.lsp documentOpened:self.previewMode ? nil : path];
    [self updateTitle];
}

// The selection and the scroll a tab kept (stashActiveTab), put back in the
// text view once it shows the tab's text.
- (void)restoreTextPlaceOf:(MCTab *)t {
    if ((t.isLatex && t.previewMode) || !t.haveView) return;
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    if (t.folds.count && tv.editable) [self.textView.folds restore:t.folds];
    NSRange sel = t.selection;
    if (sel.location > n) sel = NSMakeRange(n, 0);
    if (NSMaxRange(sel) > n) sel.length = n - sel.location;
    tv.selectedRange = sel;
    if (n > 0) {
        const NSRect r = [self textRectForCharacter:MIN(t.topCharacter, n - 1)];
        // Laid out a screen past it too, or the scroll stops where the
        // layout does, with that character at the bottom of the pane.
        if (!tv.textLayoutManager && !NSIsEmptyRect(r))
            [tv.layoutManager ensureLayoutForBoundingRect:
                NSMakeRect(0, 0, CGFLOAT_MAX,
                           NSMaxY(r) + 2 * NSHeight(self.editorScroll.contentView.bounds))
                                          inTextContainer:tv.textContainer];
        if (!NSIsEmptyRect(r)) [self scrollEditorToY:NSMinY(r) - t.topOffset];
    }
    if (t.isMarkdown && t.previewMode)
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
}

// ------------------------------------------------- closed tabs, reopened
// Option+Shift+Cmd+T (VS Code's Shift+Cmd+T is minicode's Toggle Terminal):
// the last tab closed in this window comes back where it was in the strip,
// with its selection, scroll and preview as it left them. A file already
// open again is just shown; one no longer on disk is passed over.
static const NSUInteger kMaxClosedTabs = 50;

- (void)rememberClosedTab:(MCTab *)t index:(NSUInteger)index {
    if (!t.currentPath || t.isDiff) return;
    if (!_closedTabs) _closedTabs = [NSMutableArray array];
    NSMutableDictionary *c = [@{@"path": t.currentPath, @"index": @(index),
                                @"preview": @(t.previewMode)} mutableCopy];
    if (t.haveView) {
        c[@"selection"] = [NSValue valueWithRange:t.selection];
        c[@"top"] = @(t.topCharacter);
        c[@"topOffset"] = @(t.topOffset);
    }
    [_closedTabs addObject:c];
    if (_closedTabs.count > kMaxClosedTabs) [_closedTabs removeObjectAtIndex:0];
}

- (BOOL)canReopenClosedTab { return _closedTabs.count > 0; }

- (void)reopenClosedTab:(id)sender {
    NSFileManager *fm = [NSFileManager defaultManager];
    while (_closedTabs.count) {
        NSDictionary *c = _closedTabs.lastObject;
        [_closedTabs removeLastObject];
        NSString *path = c[@"path"];
        if (MCTab *open = [self tabForPath:path]) {
            [self activateTab:open];
            [self focusActiveDocument];
            return;
        }
        BOOL dir = NO;
        if (![fm fileExistsAtPath:path isDirectory:&dir] || dir) continue;
        MCTab *t = [MCTab new];
        t.currentPath = path;
        [_tabs insertObject:t atIndex:MIN([c[@"index"] unsignedIntegerValue], _tabs.count)];
        [self activateTab:t];   // reads the file
        if (_activeTab != t) return;
        const BOOL preview = [c[@"preview"] boolValue];
        if (self.canTogglePreview && self.previewMode != preview) {
            if (self.textView.editable) self.sourceText = self.textView.string;
            self.previewMode = preview;
            [self refreshDisplay];
            [self.lsp documentOpened:preview ? nil : path];
            [self updateTitle];
        }
        if (c[@"selection"]) {
            t.haveView = YES;
            t.selection = [c[@"selection"] rangeValue];
            t.topCharacter = [c[@"top"] unsignedIntegerValue];
            t.topOffset = [c[@"topOffset"] doubleValue];
            [self restoreTextPlaceOf:t];
        }
        [self focusActiveDocument];
        return;
    }
    NSBeep();
}

// The editor's slot with no tab open: nothing to edit, one muted line.
- (void)showEmptyState {
    [_notes activeTabWillChange];
    _activeTab = nil;
    [self resetViewMode];
    [self setPlainMessage:@""];
    self.textView.editable = NO;
    [self.lsp documentOpened:nil];
    [self reloadTabStrip];
    [self relayoutRightArea];
    [self updateTitle];
    [_notes activeTabDidChange];
}

- (void)focusActiveDocument {
    if ([self latexIsShowing]) [self.window makeFirstResponder:self.latex];
    else if (self.isMedia && self.playerView) [self.window makeFirstResponder:self.playerView];
    else if (self.isPDF && self.pdfView) [self.window makeFirstResponder:self.pdfView];
    else if (self.isDiff && _activeTab.page)
        [self.window makeFirstResponder:_activeTab.page.pageFocusView];
    else if (self.isDiff && _activeTab.comparison && self.compareView)
        [self.window makeFirstResponder:self.compareView.leftView];
    else if (self.isDiff && self.diffView) [self.window makeFirstResponder:self.diffView];
    else if (self.isImage) [self.window makeFirstResponder:nil];   // nothing in it takes keys
    else if (self.viewerKind != MCViewerNone && _viewers.focusView)
        [self.window makeFirstResponder:_viewers.focusView];
    else [self.window makeFirstResponder:self.textView];
}

- (void)selectTabAtIndex:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_tabs.count) return;
    [self activateTab:_tabs[(NSUInteger)i]];
    [self focusActiveDocument];
}

- (void)closeTabAtIndex:(NSInteger)i {
    if (i < 0 || i >= (NSInteger)_tabs.count) return;
    [self closeTab:_tabs[(NSUInteger)i] prompt:YES];
}

// Ctrl+Tab and Shift+Cmd+], and the other way.
- (void)selectNextTab:(id)sender { [self stepTab:1]; }
- (void)selectPreviousTab:(id)sender { [self stepTab:-1]; }
- (void)stepTab:(int)delta {
    if (_tabs.count == 0) { NSBeep(); return; }
    NSInteger active = _activeTab ? (NSInteger)[_tabs indexOfObject:_activeTab] : -1;
    [self selectTabAtIndex:EditorTabs::step((int)active, (int)_tabs.count, delta)];
}

// Shift+Cmd+1 to 9 and Ctrl+1 to 9; the menu item's tag is the number.
- (void)selectTabNumber:(id)sender {
    const int i = EditorTabs::indexForNumber((int)[sender tag], (int)_tabs.count);
    if (i < 0) { NSBeep(); return; }
    [self selectTabAtIndex:i];
}

// YES when the tab may go: no unsaved changes, or the user saved them or
// let them go.
- (BOOL)confirmCloseTab:(MCTab *)t {
    if (!t.dirty) return YES;
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Save changes to “%@”?",
                     t.currentPath.lastPathComponent ?: @"this file"];
    a.informativeText = @"Your changes will be lost if you don't save them.";
    [a addButtonWithTitle:@"Save"];         // 1000
    [a addButtonWithTitle:@"Don't Save"];   // 1001
    [a addButtonWithTitle:@"Cancel"];       // 1002
    NSModalResponse r = [a runModal];
    if (r == NSAlertFirstButtonReturn) return [self saveTab:t];
    return r == NSAlertSecondButtonReturn;
}

// Before the window closes, the app quits or another folder takes over:
// one question for every tab with unsaved changes.
- (BOOL)confirmCloseAllTabs {
    [self saveBeforeLeavingFormatSave];   // a Cmd+S still waiting for its formatter
    NSMutableArray<MCTab *> *dirty = [NSMutableArray array];
    for (MCTab *t in _tabs) if (t.dirty) [dirty addObject:t];
    if (dirty.count == 0) return YES;
    if (dirty.count == 1) {
        [self activateTab:dirty[0]];   // so the file asked about is the one on show
        return [self confirmCloseTab:dirty[0]];
    }
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (MCTab *t in dirty) [names addObject:t.currentPath.lastPathComponent ?: @"?"];
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Save changes to %lu files?",
                     (unsigned long)dirty.count];
    a.informativeText = [NSString stringWithFormat:
        @"%@\n\nYour changes will be lost if you don't save them.",
        [names componentsJoinedByString:@"\n"]];
    [a addButtonWithTitle:@"Save All"];     // 1000
    [a addButtonWithTitle:@"Don't Save"];   // 1001
    [a addButtonWithTitle:@"Cancel"];       // 1002
    NSModalResponse r = [a runModal];
    if (r == NSAlertSecondButtonReturn) return YES;
    if (r != NSAlertFirstButtonReturn) return NO;
    // Every question about the disk first (Keep Mine, Reload, Cancel), and
    // only then any writing, so Cancel at any of them leaves every file and
    // every tab as it was.
    NSMutableArray<MCTab *> *write = [NSMutableArray array], *reload = [NSMutableArray array];
    for (MCTab *t in dirty) {
        switch ([self diskDecisionForTab:t]) {
        case MCDiskWrite: [write addObject:t]; break;
        case MCDiskReload: [reload addObject:t]; break;
        case MCDiskCancel: return NO;
        }
    }
    for (MCTab *t in reload) [self reloadTabFromDisk:t];
    _approvedWrites = [NSSet setWithArray:write];
    BOOL ok = YES;
    for (MCTab *t in write)
        if (![self saveTab:t]) { ok = NO; break; }
    _approvedWrites = nil;
    return ok;
}

// Write a tab's text to its file, YES when nothing is left unsaved. The
// active tab saves as a save MiniCode makes itself does (saveCurrentFile:
// with no sender). A tab in the background saves where it is by the same
// rules: the disk is checked first (Keep Mine, Reload, Cancel), and with
// editor.keep-sorted-on-save its blocks are sorted unless it shows a
// preview; the sort reaches its buffer as an undoable edit when the tab is
// next shown. format.on-save stays with Cmd+S on the tab on show, so a tab
// is never formatted unseen.
- (BOOL)saveTab:(MCTab *)t {
    if (t == _activeTab) {
        [self saveCurrentFile:nil];
        return !self.dirty;
    }
    if (!t.dirty || !t.currentPath) return YES;
    if (t.isImage || t.isPDF || t.isMedia || t.isDiff || t.showingMessage) return YES;
    if (![self mayWriteTab:t]) return !t.dirty;   // Reload leaves nothing to save
    NSString *text = t.sourceText ?: @"";
    NSString *sortProblem = nil;
    const BOOL preview = (t.isMarkdown || t.isLatex) && t.previewMode;
    if (!preview && [AppSettings shared].settings.keepSortedOnSave())
        text = MCKeepSortedText(text, &sortProblem) ?: text;
    NSError *err = nil;
    if (![text writeToFile:t.currentPath atomically:YES
                  encoding:NSUTF8StringEncoding error:&err]) {
        [[NSAlert alertWithError:err] runModal];
        return NO;
    }
    if (![text isEqualToString:t.sourceText ?: @""]) t.pendingSort = text;
    t.savedText = text;
    t.gone = NO;
    t.dirty = NO;
    [self recordDiskStateOf:t];
    [self updateTitle];
    if (sortProblem)
        [self flashStatus:[NSString stringWithFormat:@"%@: %@",
                           t.currentPath.lastPathComponent, sortProblem]];
    return YES;
}

// Close a tab, asking about unsaved changes first when `prompt` is set. NO
// when the user kept it. The tab shown next is the one used most recently.
- (BOOL)closeTab:(MCTab *)t prompt:(BOOL)prompt {
    const NSUInteger index = [_tabs indexOfObject:t];
    if (index == NSNotFound) return YES;
    if (t == _activeTab) [self saveBeforeLeavingFormatSave];   // saved, so not asked about
    if (prompt && ![self confirmCloseTab:t]) return NO;
    [self discardBackupOf:t];   // saved, or let go
    std::vector<uint64_t> stamps;
    for (MCTab *each in _tabs) stamps.push_back(each.lastActive);
    const NSInteger active = _activeTab ? (NSInteger)[_tabs indexOfObject:_activeTab] : -1;
    const int next = EditorTabs::activeAfterClose(stamps, (int)active, (int)index);
    const BOOL wasActive = t == _activeTab;
    if (wasActive) {
        [self stashActiveTab];
        _activeTab = nil;
    }
    [self rememberClosedTab:t index:index];
    [t.latex removeFromSuperview];
    [self closePageOf:t];
    [_tabs removeObjectAtIndex:index];
    if (wasActive && next >= 0) {
        [self activateTab:_tabs[(NSUInteger)next]];
    } else {
        if (wasActive) [self showEmptyState];
        else [self.lsp tabsChanged];   // its file is closed on its server
        [self reloadTabStrip];
        [self updateTitle];
        [self saveSession];
    }
    return YES;
}

// Every tab goes, unasked (the caller asked), and nothing is remembered of
// it: the folder's tabs stay remembered for when it is opened again.
- (void)dropAllTabs {
    [self stashActiveTab];
    _activeTab = nil;
    for (MCTab *t in _tabs) {
        [t.latex removeFromSuperview];
        [self closePageOf:t];
        [self discardBackupOf:t];
    }
    [_tabs removeAllObjects];
    [self showEmptyState];
}

// Cmd+W: what has the keyboard closes. In a terminal that terminal closes
// (its tab, or its pane of a split; the panel hides with the last one; a
// command still running in it is asked about), the browser panel hides, and
// anywhere else the tab on show closes. With no tab left a new press closes
// the window, so the key is never dead. A held key's repeats do nothing: one
// press closes one thing, where a held Cmd+W once took three terminals (one
// running a command) and then both editor tabs.
- (void)closeFocused:(id)sender {
    NSEvent *e = NSApp.currentEvent;
    if (e.type == NSEventTypeKeyDown && e.isARepeat) return;
    switch ([self focusedCloseTarget]) {
    case 1:
        [self closeFocusedTerminal];
        return;
    case 2:
        [self toggleBrowser:nil];
        [self focusAfterClosingPanel];
        return;
    case 3:
        [self closeTab:_activeTab prompt:YES];
        return;
    case 4:
        [self toggleProblems:nil];
        return;
    default:
        [self.window performClose:sender];
    }
}

// What Cmd+W would close: 1 the terminal, 2 the browser, 3 the active tab,
// 4 the Problems list, 0 the window.
- (int)focusedCloseTarget {
    NSResponder *fr = self.window.firstResponder;
    NSView *v = [fr isKindOfClass:NSView.class] ? (NSView *)fr : nil;
    if (v && self.problemsPanel && self.problemsVisible &&
        [v isDescendantOf:self.problemsPanel]) return 4;
    if (v && self.terminalPanel && self.terminalVisible &&
        [v isDescendantOf:self.terminalPanel]) return 1;
    if (v && self.browser && self.browserVisible && [v isDescendantOf:self.browser]) return 2;
    return _activeTab ? 3 : 0;
}

- (void)focusAfterClosingPanel {
    const BOOL browser = self.browserVisible && self.browser;
    if (self.rightAreaCollapsed) [self.window makeFirstResponder:self.outline];
    else if (!browser && !self.editorHidden) [self focusActiveDocument];
    else if (browser) [self.browser focusURLBar];
    else if (self.terminalVisible && self.terminal) [self.terminal focusInput];
    else [self.window makeFirstResponder:self.outline];
}

// ----------------------------------------------- the tabs between runs
// Each folder's open files and which was on show, in the user defaults by
// the folder's real path; the most recently used 50 folders are kept. Off
// for the demos and the memory benchmark, which must start the same way
// every time.
static NSString *const kTabSessionsKey = @"MCTabSessions";
static const NSUInteger kMaxTabSessions = 50;
// The largest file shown at launch, the GTK port's limit for opening one.
static const unsigned long long kMaxRestoredFileBytes = 16ull * 1024 * 1024;

- (NSUserDefaults *)sessionDefaults {
    // Tests point this at a scratch suite, away from the user's own.
    NSString *suite = NSProcessInfo.processInfo.environment[@"MINICODE_DEFAULTS_SUITE"];
    return suite.length ? [[NSUserDefaults alloc] initWithSuiteName:suite]
                        : [NSUserDefaults standardUserDefaults];
}

- (BOOL)sessionsEnabled {
    NSDictionary *env = NSProcessInfo.processInfo.environment;
    return !env[@"MINICODE_DEMO"] && !env[@"MINICODE_BENCH"];
}

- (NSString *)sessionKey { return _root.path.stringByResolvingSymlinksInPath; }

// The files this window would reopen, in tab order.
- (NSArray<NSString *> *)sessionFilePaths {
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    for (MCTab *t in _tabs)
        if (t.currentPath && !t.agentPreview) [files addObject:t.currentPath];
    return files;
}

- (void)saveSession {
    if (![self sessionsEnabled] || !_root.path) return;
    // The file on show, or with a diff on show the file shown last.
    MCTab *shown = _activeTab.currentPath ? _activeTab : nil;
    for (MCTab *t in _tabs)
        if (!shown && t.currentPath) shown = t;
        else if (shown != _activeTab && t.currentPath && t.lastActive > shown.lastActive)
            shown = t;
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    NSInteger active = 0;
    for (MCTab *t in _tabs) {
        if (!t.currentPath) continue;   // a diff is not reopened
        if (t == shown) active = (NSInteger)files.count;
        [files addObject:t.currentPath];
    }
    NSString *key = [self sessionKey];
    // Windows open on the same folder share its session: this window's
    // files first, then the other windows' not already there, so whichever
    // saves last, quitting with two windows on a folder reopens all their
    // files. A window that closes while another stays open drops out of the
    // next save, so what is remembered is what was open when the folder's
    // last window went.
    id app = NSApp.delegate;
    NSArray *windows = [app respondsToSelector:@selector(controllers)]
                           ? [app valueForKey:@"controllers"] : nil;
    for (EditorController *other in windows) {
        if (other == self || ![other isKindOfClass:[EditorController class]] ||
            other->_closing || ![[other sessionKey] isEqualToString:key])
            continue;
        if (_closing) {   // the folder stays open in another window: it speaks for it
            [other saveSession];
            return;
        }
        for (NSString *f in [other sessionFilePaths])
            if (![files containsObject:f]) [files addObject:f];
    }
    NSUserDefaults *d = [self sessionDefaults];
    NSMutableDictionary *all = [[d dictionaryForKey:kTabSessionsKey] mutableCopy]
                               ?: [NSMutableDictionary dictionary];
    // The browser panel: where it was, and whether it showed.
    NSDictionary *browser = [self browserSessionState];
    if (files.count == 0 && !browser) {
        [all removeObjectForKey:key];
    } else {
        NSMutableDictionary *entry = [@{@"files": files, @"active": @(active),
                                        @"used": @([NSDate date].timeIntervalSince1970)}
                                      mutableCopy];
        if (browser) entry[@"browser"] = browser;
        all[key] = entry;
        while (all.count > kMaxTabSessions) {
            NSString *oldest = nil;
            double when = DBL_MAX;
            for (NSString *k in all) {
                double used = [all[k][@"used"] doubleValue];
                if (used < when) { when = used; oldest = k; }
            }
            if (!oldest) break;
            [all removeObjectForKey:oldest];
        }
    }
    [d setObject:all forKey:kTabSessionsKey];
}

// The browser panel's part of the session: its page (BrowserView
// savedState), whether it showed, and its share of the width. nil with no
// browser, or one that never loaded a page.
- (NSDictionary *)browserSessionState {
    if (!self.browser) return nil;
    NSDictionary *page = [self.browser savedState];
    if (!page) return nil;
    return @{@"page": page, @"visible": @(self.browserVisible),
             @"fraction": @(self.browserFraction)};
}

// A new window's browser comes back as it was: made now if the folder's
// session had one, shown if it showed, without taking the keyboard.
- (void)restoreBrowserFromSession {
    if (![self sessionsEnabled]) return;
    NSDictionary *s = [[self sessionDefaults] dictionaryForKey:kTabSessionsKey][[self sessionKey]];
    NSDictionary *b = [s isKindOfClass:NSDictionary.class] ? s[@"browser"] : nil;
    if (![b isKindOfClass:NSDictionary.class] || self.browser) return;
    [self makeBrowserWithHome:@""];
    [self.browser restoreState:b[@"page"]];
    const double fraction = [b[@"fraction"] doubleValue];
    if (fraction > 0.05 && fraction < 0.95) self.browserFraction = fraction;
    if ([b[@"visible"] boolValue]) {
        self.browserVisible = YES;
        [self showRightArea];
    }
    [self relayoutRightArea];
}

// Open the tabs this folder had last time; only the one on show is read
// now, the others when they are first shown.
- (void)restoreSession {
    if (![self sessionsEnabled]) return;
    NSDictionary *s = [[self sessionDefaults] dictionaryForKey:kTabSessionsKey][[self sessionKey]];
    EditorTabs::Session saved;
    if ([s isKindOfClass:NSDictionary.class]) {
        for (id f in s[@"files"])
            if ([f isKindOfClass:NSString.class]) saved.files.push_back(Utf8String(f));
        saved.active = [s[@"active"] intValue];
    }
    // Unsaved buffers kept from a crash, a force quit or a restart: a file of
    // the session with a backup comes back even if it is gone from disk, and
    // a backup of a file the session lost comes back after the session's.
    std::map<std::string, std::string> backupFor;
    std::vector<std::string> extra;
    if (MCBackupStore *store = [MCBackupStore shared]) {
        [store cleanUpOnce];
        Backups::Restore plan = [store planForRoot:[self sessionKey] sessionFiles:saved.files];
        for (const Backups::Restore::Item &item : plan.items) {
            backupFor[item.path] = item.id;
            if (!item.inSession) extra.push_back(item.path);
        }
    }
    NSFileManager *fm = [NSFileManager defaultManager];
    EditorTabs::Session r = EditorTabs::restorable(saved, [fm, &backupFor](const std::string &f) {
        BOOL dir = NO;
        return backupFor.count(f) ||
               ([fm fileExistsAtPath:[NSString stringWithUTF8String:f.c_str()] isDirectory:&dir] &&
                !dir);
    });
    for (const std::string &f : extra) r.files.push_back(f);
    if (r.files.empty()) return;
    if (r.active < 0) r.active = 0;
    for (const std::string &f : r.files) {
        MCTab *t = [MCTab new];
        t.currentPath = [NSString stringWithUTF8String:f.c_str()];
        auto b = backupFor.find(f);
        if (b != backupFor.end() && ![self takeBackup:@(b->second.c_str()) intoTab:t] &&
            ![fm fileExistsAtPath:t.currentPath])
            continue;   // nothing to show: the backup does not read and the file is gone
        [_tabs addObject:t];
    }
    if (_tabs.count == 0) return;
    if (r.active >= (int)_tabs.count) r.active = (int)_tabs.count - 1;
    // The tab on show is read now, while the window opens. One holding a
    // file too big for that (it held up every launch) or no plain file at
    // all waits for a click instead, and the nearest other tab is shown.
    auto quick = [fm](NSString *path) {
        NSDictionary *a = [fm attributesOfItemAtPath:path error:nil];
        return [a.fileType isEqualToString:NSFileTypeRegular] &&
               a.fileSize <= kMaxRestoredFileBytes;
    };
    NSInteger active = -1;
    for (NSInteger d = 0; active < 0 && d < (NSInteger)_tabs.count; d++)
        for (NSInteger i : {(NSInteger)r.active - d, (NSInteger)r.active + d})
            if (active < 0 && i >= 0 && i < (NSInteger)_tabs.count && quick(_tabs[(NSUInteger)i].currentPath))
                active = i;
    if (active < 0) {
        [self reloadTabStrip];
        [self updateTitle];
        return;
    }
    [self activateTab:_tabs[(NSUInteger)active]];
}

// ------------------------------------------------- backups and hot exit
// A tab whose buffer differs from disk has it written to the backup store
// (BackupStore) a second after typing stops, and at least every five seconds
// while it goes on; the backup goes as soon as the tab is clean again or is
// closed. Quitting still asks about unsaved tabs, and Don't Save deletes
// their backups; a crash, a force quit or a power loss leaves them, and the
// folder's next window brings them back as unsaved tabs. Request a Change's
// restart writes them at once and asks nothing about them.
static const NSTimeInterval kBackupDelay = 1.0, kBackupMaxWait = 5.0;

// Only text read from a file has anything to keep.
- (BOOL)tabTakesBackup:(MCTab *)t {
    return t.currentPath && t.sourceText && !t.isImage && !t.isPDF && !t.isMedia &&
           !t.isDiff && !t.showingMessage;
}

- (BOOL)tabNeedsBackup:(MCTab *)t {
    return t.dirty && [self tabTakesBackup:t] &&
           (t.backedUpText != t.sourceText || ![t.backedUpPath isEqualToString:t.currentPath]);
}

// After every edit and every change of a dirty flag: a clean tab's backup
// goes now, and a changed buffer is written when typing pauses. Costs a
// pointer comparison per tab, so it can run on every keystroke.
- (void)scheduleBackups {
    if (_closing || ![MCBackupStore shared]) return;
    BOOL pending = NO;
    for (MCTab *t in _tabs) {
        if (t.dirty && [self tabTakesBackup:t]) pending = pending || [self tabNeedsBackup:t];
        else if (t.hasBackup) [self discardBackupOf:t];
    }
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(writeBackups)
                                               object:nil];
    if (!pending) {
        _backupFirstPending = nil;
        return;
    }
    NSDate *now = [NSDate date];
    if (!_backupFirstPending) _backupFirstPending = now;
    const NSTimeInterval waited = [now timeIntervalSinceDate:_backupFirstPending];
    [self performSelector:@selector(writeBackups) withObject:nil
               afterDelay:MAX(0, MIN(kBackupDelay, kBackupMaxWait - waited))];
}

// Every changed buffer to the store's queue; the main thread only hands
// over the (immutable) text.
- (void)writeBackups {
    _backupFirstPending = nil;
    MCBackupStore *store = [MCBackupStore shared];
    if (_closing || !store) return;
    for (MCTab *t in _tabs) {
        if (![self tabNeedsBackup:t]) continue;
        if (!t.backupID) t.backupID = [store newID];
        [store writeID:t.backupID path:t.currentPath root:[self sessionKey]
              diskDate:t.fileModDate inode:t.fileID text:t.sourceText];
        t.backedUpText = t.sourceText;
        t.backedUpPath = t.currentPath;
        t.hasBackup = YES;
    }
}

- (void)discardBackupOf:(MCTab *)t {
    if (t.backupID && t.hasBackup) [[MCBackupStore shared] removeID:t.backupID];
    t.hasBackup = NO;
    t.backedUpText = nil;
    t.backedUpPath = nil;
}

// Request a Change's restart: every unsaved buffer written now, so the new
// build brings them back. NO when backups are off or one could not be
// written, and the usual question is asked instead.
- (BOOL)keepUnsavedTabsForRestart {
    MCBackupStore *store = [MCBackupStore shared];
    if (!store || ![self sessionsEnabled]) return NO;
    [self saveBeforeLeavingFormatSave];
    if (self.textView.editable && _activeTab && !_activeTab.showingMessage)
        self.sourceText = self.textView.string;
    for (MCTab *t in _tabs) {
        if (!t.dirty || ![self tabTakesBackup:t]) continue;
        if (!t.backupID) t.backupID = [store newID];
        if (![store writeNowID:t.backupID path:t.currentPath root:[self sessionKey]
                      diskDate:t.fileModDate inode:t.fileID text:t.sourceText])
            return NO;
        t.backedUpText = t.sourceText;
        t.backedUpPath = t.currentPath;
        t.hasBackup = YES;
    }
    [self saveSession];
    return YES;
}

// Quitting: the questions were asked (applicationShouldTerminate), so what
// is still unsaved was let go with Don't Save, and its backups go. A restart
// keeps them (keepUnsavedTabsForRestart wrote them).
- (void)backupsAppWillTerminate:(NSNotification *)note {
    MCBackupStore *store = [MCBackupStore shared];
    if (!store) return;
    id app = NSApp.delegate;
    SEL restartApp = NSSelectorFromString(@"restartApp");
    const BOOL restarting = [app respondsToSelector:restartApp] && [app valueForKey:@"restartApp"];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(writeBackups)
                                               object:nil];
    if (!restarting)
        for (MCTab *t in _tabs) [self discardBackupOf:t];
    [store flush];
}

// A tab restored from the session takes its backup: the text, unsaved, and
// what the disk file was when the backup was taken (fileModDate and fileID,
// so a save checks the disk against that). The disk's own text is read when
// the tab is first shown.
- (BOOL)takeBackup:(NSString *)bid intoTab:(MCTab *)t {
    Backups::Entry e;
    NSString *text = nil;
    if (![[MCBackupStore shared] loadID:bid entry:&e text:&text]) return NO;
    t.backupID = bid;
    t.hasBackup = YES;        // written again soon under this process's name
    t.restoredBackup = YES;
    t.sourceText = text;
    t.dirty = YES;
    t.fileModDate = e.diskExisted ? [NSDate dateWithTimeIntervalSince1970:e.diskMtime] : nil;
    t.fileID = e.diskInode;
    NSDictionary *a = [[NSFileManager defaultManager] attributesOfItemAtPath:t.currentPath error:nil];
    t.gone = e.diskExisted && !a;
    // Unchanged since: the disk's own date, exactly, so a save in the
    // background does not take a rounding for a change made outside.
    if (e.diskExisted && a &&
        fabs(a.fileModificationDate.timeIntervalSince1970 - e.diskMtime) < 0.001 &&
        (!e.diskInode || [a[NSFileSystemFileNumber] unsignedLongLongValue] == e.diskInode))
        t.fileModDate = a.fileModificationDate;
    return YES;
}

// The active tab was just read from disk (loadActiveTab) and had a backup:
// the backup's text goes in as the unsaved buffer, with nothing to undo, and
// the disk's text stays savedText. A file changed since the backup keeps the
// backup's date, so the next check (window focus, Cmd+S) asks Keep Mine or
// Reload; a file gone is a gone tab holding the text.
- (void)showRestoredText:(NSString *)text base:(NSDate *)base inode:(uint64_t)inode {
    MCTab *t = _activeTab;
    NSString *name = t.currentPath.lastPathComponent;
    NSString *note = [NSString stringWithFormat:@"Restored unsaved changes to %@", name];
    self.sourceText = text;
    if (base && !t.fileModDate) {
        t.gone = YES;
        t.fileModDate = base;
        t.fileID = inode;
        note = [note stringByAppendingString:@"; the file is no longer on disk"];
    } else if (base && (fabs([t.fileModDate timeIntervalSinceDate:base]) > 0.001 ||
                        (inode && t.fileID && inode != t.fileID))) {
        t.fileModDate = base;
        t.fileID = inode;
        note = [note stringByAppendingString:@"; the file has changed on disk since"];
    }
    self.dirty = t.gone || ![text isEqualToString:self.savedText ?: @""];
    if (self.dirty) [self showStatusNote:note];
}

// ------------------------------------------------------------- auto-save
// editor.auto-save (Settings.h). after-delay writes every tab with unsaved
// changes once typing has paused for editor.auto-save-delay, as VS Code's
// files.autoSave afterDelay; focus-change writes them when the editor loses
// the keyboard, the window stops being key, or another tab is shown. An
// auto-save is a plain write: no formatter (format.on-save stays with
// Cmd+S, as VS Code leaves it with afterDelay), no keep-sorted (it would
// rewrite text under the caret), and never a question. A file changed on
// disk since it was read is skipped and named in the status bar, and Cmd+S
// asks Keep Mine or Reload as before; a file gone from disk, a tab never
// read yet, and a tab whose Cmd+S waits for its formatter are left alone.
// Claude Code editing the same files is the case this is for: buffers are
// written within a second, so they rarely differ from the disk when it
// writes, and clean tabs follow its writes quietly (checkTabsOnDisk).

- (Settings::AutoSave)autoSaveMode {
    return [AppSettings shared].settings.autoSave();
}

// Not a tab with an inline agent edit waiting for Keep or Discard (the
// agent edits only the tab on show): it is written once the bar closes.
- (BOOL)tabTakesAutoSave:(MCTab *)t {
    return t.dirty && t.loaded && !t.gone && [self tabTakesBackup:t] &&
           t != _formatSaveTab && !(t == _activeTab && _agent.editPending);
}

// After every edit and every change of a dirty flag, beside scheduleBackups.
- (void)scheduleAutoSave {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(autoSaveAfterDelay)
                                               object:nil];
    if (_closing || [self autoSaveMode] != Settings::AutoSave::AfterDelay) return;
    BOOL any = NO;
    for (MCTab *t in _tabs) any = any || [self tabTakesAutoSave:t];
    if (!any) return;
    const int ms = [AppSettings shared].settings.autoSaveDelay();
    [self performSelector:@selector(autoSaveAfterDelay) withObject:nil afterDelay:ms / 1000.0];
}
- (void)autoSaveAfterDelay { [self autoSaveNow]; }

// focus-change: the editor lost the keyboard, the window lost key, or the
// tab on show is about to change. Saved on the next turn of the run loop,
// not inside the responder change (a selector of its own, which the
// debounce above never cancels).
- (void)autoSaveOnFocusChange {
    if (_closing || [self autoSaveMode] != Settings::AutoSave::FocusChange) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(autoSaveNow)
                                               object:nil];
    [self performSelector:@selector(autoSaveNow) withObject:nil afterDelay:0];
}

// Every tab that takes an auto-save, written now, unasked.
- (void)autoSaveNow {
    if (_closing || [self autoSaveMode] == Settings::AutoSave::Off) return;
    if (_activeTab && self.textView.editable && !_activeTab.showingMessage)
        self.sourceText = self.textView.string;
    NSMutableArray<NSString *> *changed = [NSMutableArray array];
    for (MCTab *t in [_tabs copy]) {
        if (![self tabTakesAutoSave:t]) continue;
        NSDate *disk = [self diskModDateFor:t.currentPath];
        if (!disk && t.fileModDate) {   // deleted or moved: marked, not written again
            [self tabWentMissing:t];
            continue;
        }
        // Changed (or made) on disk since it was read: never written over
        // unasked. Cmd+S asks.
        if (t.fileModDate ? ![disk isEqualToDate:t.fileModDate] : disk != nil) {
            [changed addObject:t.currentPath.lastPathComponent ?: @"?"];
            continue;
        }
        [self writeTabForAutoSave:t];
    }
    if (changed.count)
        [self showStatusNote:[NSString stringWithFormat:
            @"Not auto-saved: %@ changed on disk. ⌘S asks what to do.",
            [changed componentsJoinedByString:@", "]]];
}

- (BOOL)writeTabForAutoSave:(MCTab *)t {
    const BOOL shown = t == _activeTab;
    NSString *text = shown && self.textView.editable && !t.showingMessage
                         ? [self.textView.string copy] : (t.sourceText ?: @"");
    NSError *err = nil;
    if (![text writeToFile:t.currentPath atomically:YES encoding:NSUTF8StringEncoding
                     error:&err]) {
        [self showStatusNote:[NSString stringWithFormat:@"Could not auto-save %@: %@",
                              t.currentPath.lastPathComponent, err.localizedDescription]];
        return NO;
    }
    t.sourceText = text;
    t.savedText = text;
    t.dirty = NO;
    [self recordDiskStateOf:t];
    if (shown) [self.lsp documentSaved];
    [self updateTitle];
    return YES;
}

// A tab with nothing unsaved whose file changed on disk takes the new text
// with no question. The tab on show keeps its caret and the line at the top
// of the pane (the change goes in as the lines that differ, as Format
// Document's does), and loses its undo steps, which were for the old text.
- (void)reloadCleanTab:(MCTab *)t {
    NSString *fresh = [NSString stringWithContentsOfFile:t.currentPath
                                                encoding:NSUTF8StringEncoding error:nil];
    if (!fresh) return;
    if ([fresh isEqualToString:t.savedText ?: @""] && !t.gone) {   // touched, not changed
        [self recordDiskStateOf:t];
        return;
    }
    if (t != _activeTab || !self.textView.editable || t.showingMessage) {
        [self takeDiskText:fresh forTab:t];
        return;
    }
    t.savedText = fresh;   // so the edit below reads as clean
    NSUndoManager *um = self.textView.undoManager;
    [um disableUndoRegistration];
    [self replaceSourceWithText:fresh actionName:@"Reload"];
    [um enableUndoRegistration];
    self.sourceText = self.textView.string;
    t.dirty = NO;
    t.gone = NO;
    t.pendingSort = nil;
    [self recordDiskStateOf:t];
    [self forgetHistoryOf:t];
    [self updateTitle];
}

// Called when the window regains focus: reconcile with on-disk changes.
- (void)checkExternalChange {
    if (!self.currentPath || !self.fileModDate) return;
    NSDate *disk = [self diskModDateFor:self.currentPath];
    if (!disk) {                             // moved or deleted
        [self tabWentMissing:_activeTab];
        return;
    }
    if (_activeTab.gone) [self tabCameBack:_activeTab];
    if ([disk isEqualToDate:self.fileModDate]) return;   // unchanged

    self.fileModDate = disk;
    if (self.isImage) { [self showImageAtPath:self.currentPath]; return; }
    if (self.isPDF) { [self showPDFAtPath:self.currentPath]; return; }
    if (self.isMedia) { [self showMediaAtPath:self.currentPath]; return; }
    if (MCViewerKindWithoutText(self.viewerKind)) { [self showViewer:self.viewerKind]; return; }
    if (!self.dirty) {                       // no local edits: reload quietly
        [self reloadCleanTab:_activeTab];
        return;
    }
    NSAlert *a = [[NSAlert alloc] init];     // local edits AND disk changed
    a.messageText = [NSString stringWithFormat:
        @"“%@” changed on disk.", self.currentPath.lastPathComponent];
    a.informativeText = @"You have unsaved changes here. Keep your version, "
                         "or reload the file from disk and lose them?";
    [a addButtonWithTitle:@"Keep Mine"];     // 1000
    [a addButtonWithTitle:@"Reload"];        // 1001
    NSString *fresh = [NSString stringWithContentsOfFile:self.currentPath
                                                encoding:NSUTF8StringEncoding
                                                   error:nil];
    if ([a runModal] == NSAlertSecondButtonReturn) {
        if (fresh) [self takeDiskText:fresh forTab:_activeTab];
    } else if (fresh) {
        // Kept: what counts as unchanged from now on is the file's new text.
        self.savedText = fresh;
        [self noteEdited];
    }
}

// Put the file's text from disk in a tab, as if it had just been opened:
// clean, with nothing to undo. A background tab shows it when shown.
- (void)takeDiskText:(NSString *)fresh forTab:(MCTab *)t {
    if (!t) return;
    t.sourceText = fresh;
    t.savedText = fresh;
    t.dirty = NO;
    t.gone = NO;
    t.pendingSort = nil;
    [self recordDiskStateOf:t];
    [self forgetHistoryOf:t];
    if (t == _activeTab) [self refreshDisplay];
    else [self.lsp tabsChanged];   // the server gets the disk's text for it too
    [self updateTitle];
}

// ------------------------------------------- files moved or deleted outside
// A tab's file is no longer at its path. A rename within its folder (mv, git
// mv) is followed when a file there has the same inode and modification
// date, which a rename keeps and nothing else produces; otherwise the tab is
// marked: "(deleted)" in its title, unsaved, and Cmd+S asks first.
- (void)tabWentMissing:(MCTab *)t {
    if (!t.currentPath || !t.fileModDate || t.gone) return;
    if ([[NSFileManager defaultManager] fileExistsAtPath:t.currentPath]) return;
    NSString *moved = [self renamedFileFor:t];
    if (moved) {
        [self pathMovedFrom:t.currentPath to:moved];
        if (t == _activeTab) [self reselectCurrentFileInTree];
        return;
    }
    t.gone = YES;
    // Only text has anything to save; a picture or a video is just marked.
    if (!t.isImage && !t.isPDF && !t.isMedia && !t.isDiff && !t.showingMessage && t.loaded)
        t.dirty = YES;
    [self updateTitle];
    [self saveSession];
}

- (void)tabCameBack:(MCTab *)t {
    if (!t.gone) return;
    t.gone = NO;
    NSString *now = t.sourceText ?: @"", *saved = t.savedText ?: @"";
    t.dirty = t.loaded && ![now isEqualToString:saved];
    [self updateTitle];
}

- (NSString *)renamedFileFor:(MCTab *)t {
    if (!t.fileID) return nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dir = t.currentPath.stringByDeletingLastPathComponent;
    for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
        NSString *p = [dir stringByAppendingPathComponent:name];
        NSDictionary *a = [fm attributesOfItemAtPath:p error:nil];
        if (![a.fileType isEqualToString:NSFileTypeRegular]) continue;
        if ([a[NSFileSystemFileNumber] unsignedLongLongValue] != t.fileID) continue;
        if (![a.fileModificationDate isEqualToDate:t.fileModDate]) continue;
        if ([self tabForPath:p]) return nil;   // already open in another tab
        return p;
    }
    return nil;
}

// After a change in the folder (FSEvents): every tab whose file went away
// or came back, and every text tab with nothing unsaved whose file changed
// (Claude Code in the terminal, git checkout), which takes the new text
// quietly, keeping its place. A tab with unsaved changes is left as it is
// until it is saved (Cmd+S asks, an auto-save skips it), shown again, or
// the window comes forward.
- (void)checkTabsOnDisk {
    for (MCTab *t in [_tabs copy]) {
        if (!t.currentPath || !t.fileModDate) continue;
        NSDate *disk = [self diskModDateFor:t.currentPath];
        if (!disk) [self tabWentMissing:t];
        else if (t.gone) {
            if (t == _activeTab) [self checkExternalChange];
            else [self tabCameBack:t];
        } else if (!t.dirty && t.loaded && [self tabTakesBackup:t] &&
                   ![disk isEqualToDate:t.fileModDate]) {
            [self reloadCleanTab:t];
        }
    }
}

// Before writing a tab's file: NO when the save must not go ahead. A file
// changed on disk since it was read asks Keep Mine (write over it), Reload
// (take the disk's text, nothing is written) or Cancel; a file no longer
// there asks before it is written again. Save All asks the same, file by
// file, so nothing outside is overwritten unasked, but asks all of them
// before it writes any (confirmCloseAllTabs).
- (BOOL)mayWriteTab:(MCTab *)t {
    if ([_approvedWrites containsObject:t]) return YES;   // Save All asked already
    MCDiskDecision d = [self diskDecisionForTab:t];
    if (d == MCDiskReload) [self reloadTabFromDisk:t];
    return d == MCDiskWrite;
}

- (void)reloadTabFromDisk:(MCTab *)t {
    NSString *fresh = [NSString stringWithContentsOfFile:t.currentPath
                                                encoding:NSUTF8StringEncoding error:nil];
    if (fresh) [self takeDiskText:fresh forTab:t];
}

// The question mayWriteTab: asks, without acting on the answer.
- (MCDiskDecision)diskDecisionForTab:(MCTab *)t {
    if (!t.fileModDate) return MCDiskWrite;   // never on disk as far as we know
    NSDate *disk = [self diskModDateFor:t.currentPath];
    if (disk && [disk isEqualToDate:t.fileModDate] && !t.gone) return MCDiskWrite;
    NSString *name = t.currentPath.lastPathComponent;
    NSAlert *a = [[NSAlert alloc] init];
    if (!disk) {
        a.messageText = [NSString stringWithFormat:@"“%@” is no longer on disk.", name];
        a.informativeText = @"It was moved or deleted after it was opened here. "
                             "Save your version at its old place?";
        [a addButtonWithTitle:@"Save"];      // 1000
        [a addButtonWithTitle:@"Cancel"];    // 1001
        return [a runModal] == NSAlertFirstButtonReturn ? MCDiskWrite : MCDiskCancel;
    }
    if (t.gone) {   // it came back: compare with what is there now
        [self tabCameBack:t];
        if ([disk isEqualToDate:t.fileModDate]) return MCDiskWrite;
    }
    a.messageText = [NSString stringWithFormat:@"“%@” changed on disk.", name];
    a.informativeText = @"It changed after it was opened here. Keep your version "
                         "and write over it, or reload the file from disk and lose "
                         "your changes?";
    [a addButtonWithTitle:@"Keep Mine"];     // 1000
    [a addButtonWithTitle:@"Reload"];        // 1001
    [a addButtonWithTitle:@"Cancel"];        // 1002
    NSModalResponse r = [a runModal];
    return r == NSAlertFirstButtonReturn ? MCDiskWrite
         : r == NSAlertSecondButtonReturn ? MCDiskReload : MCDiskCancel;
}

// -------------------------------------------------------- NSWindowDelegate
// Commands still running in this window's terminals, for the question asked
// before the window closes or the app quits.
- (NSArray<NSString *> *)runningTerminalCommands {
    return self.terminalPanel ? MCRunningCommands(self.terminalPanel.terminals) : @[];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if (!MCConfirmClosingRunning([self runningTerminalCommands], @"Close the window?",
                                 @"Close"))
        return NO;
    return [self confirmCloseAllTabs];
}
- (void)windowWillClose:(NSNotification *)note {
    [_agent close];
    // Asked already (windowShouldClose:): what is still unsaved was let go.
    for (MCTab *t in _tabs) [self discardBackupOf:t];
    _closing = YES;
    // The shells go with the window (hung up and reaped), and the dock with
    // them, so nothing of the window outlives it.
    if (self.terminalPanel) {
        MCTerminalPanel *panel = self.terminalPanel;
        self.terminalPanel = nil;
        [panel closeAll];
        [panel removeFromSuperview];
    }
    [self saveSession];        // reopened with the folder next time
    [_sizer cancel];           // no counting for a closed window
    [_detailsTimer invalidate];
    _detailsTimer = nil;
    _awaitedWebImages = nil;   // pictures still on their way land in the cache only
    _awaitedMath = nil;        // and so do formulas
    [self stopMedia];      // no sound from a closed window
    _formatGeneration++;   // a formatter still running finds nothing to apply
    [self.lsp shutdown];   // never leave a server running for a closed window
    [_change stop];        // no build watched for a closed window
    [_agents stop];        // no transcripts watched for a closed window
}
- (void)windowDidBecomeKey:(NSNotification *)note {
    [self checkExternalChange];
    if (self.gitPanelVisible) [self.gitPanel refresh];
    [_agents windowBecameKey];
}
// Another window or another app (editor.auto-save = focus-change).
- (void)windowDidResignKey:(NSNotification *)note {
    [self autoSaveOnFocusChange];
}

- (BOOL)canTogglePreview {
    return self.isMarkdown || self.isLatex || MCViewerKindOverText(self.viewerKind) ||
           MCViewerKindBesideText(self.viewerKind) || MCViewerKindRendersMarkdown(self.viewerKind);
}

// The rendered Markdown preview is up: a Markdown file's, or a notebook's,
// which is Markdown made from it (read-only: double-click editing, task
// boxes and the like stay with Markdown files).
- (BOOL)showsRenderedMarkdown {
    return (self.isMarkdown || MCViewerKindRendersMarkdown(self.viewerKind)) && self.previewMode;
}

// The Markdown that preview renders.
- (NSString *)markdownToRender {
    if (MCViewerKindRendersMarkdown(self.viewerKind)) return MCNotebookMarkdown(self.sourceText ?: @"");
    return self.sourceText;
}

// Decide what to show for the current file/mode.
- (void)refreshDisplay {
    self.showingMessage = NO;
    if (self.isLatex && self.previewMode) {
        [self showLatexPreview];
        return;
    }
    // A viewer over the text (a CSV table) holds the slot in preview mode;
    // the hidden text view holds nothing, and saving writes the tab's text.
    if (self.previewMode && MCViewerKindOverText(self.viewerKind)) {
        if (self.latex) self.latex.hidden = YES;
        self.textView.editable = NO;
        _liveHighlight = NO;
        [self.textView.textStorage setAttributedString:[NSAttributedString new]];
        [self.viewers showText:self.sourceText ?: @"" kind:self.viewerKind path:self.currentPath];
        [self relayoutRightArea];
        return;
    }
    if (!MCViewerKindWithoutText(self.viewerKind)) [_viewers clear];
    // Always relay out: whatever held the editor's slot before (a PDF, an
    // image, the LaTeX preview) has to give it back to the text view.
    if (self.latex) self.latex.hidden = YES;
    [self relayoutRightArea];
    if ([self showsRenderedMarkdown]) {
        self.textView.editable = NO;
        [self renderMarkdown:[self markdownToRender]];
    } else {
        [self displaySourceEditable];
        // The JSON outline, beside the text it follows.
        if (MCViewerKindBesideText(self.viewerKind)) {
            [self.viewers showBesideTextView:self.textView kind:self.viewerKind];
            [self relayoutRightArea];
        }
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
    BOOL enabled = NO;
    if (item.action == @selector(navigateBack:)) return [self.navigator canGoBack];
    if (item.action == @selector(navigateForward:)) return [self.navigator canGoForward];
    if ([self validateActiveTabAction:item.action enabled:&enabled]) return enabled;
    if (item.action == @selector(sortTreeBy:)) {
        item.state = item.tag == (NSInteger)_treeSort.key ? NSControlStateValueOn
                                                          : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(setTreeSortDirection:)) {
        item.state = _treeSort.ascending == (item.tag == 1) ? NSControlStateValueOn
                                                             : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFoldersFirst:)) {
        item.state = _treeSort.foldersFirst ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleTreeColumn:)) {
        item.state = MCTreeColumnShown((MCTreeColumns)item.tag) ? NSControlStateValueOn
                                                                 : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(closeFocused:)) {
        static NSString *const titles[] = {@"Close Window", @"Close Terminal",
                                           @"Close Browser", @"Close Tab",
                                           @"Close Problems"};
        item.title = titles[[self focusedCloseTarget]];
        return YES;
    }
    if (item.action == @selector(selectTabNumber:))
        return EditorTabs::indexForNumber((int)item.tag, (int)_tabs.count) >= 0;
    if (item.action == @selector(selectNextTab:) || item.action == @selector(selectPreviousTab:))
        return _tabs.count > 0;
    if (item.action == @selector(goToBracket:)) return [self bracketMatchingApplies];
    // Off without a tour, so Option+] and Option+[ type their characters.
    if (item.action == @selector(nextTourStop:) || item.action == @selector(previousTourStop:))
        return self.tourActive;
    if (item.action == @selector(exportPDF:)) return self.canExportPDF;
    if (item.action == @selector(togglePlayback:)) return self.canTogglePlayback;
    if (item.action == @selector(toggleProblems:)) {
        item.state = self.problemsVisible ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFindMatchCase:)) {
        item.state = _editorFind.matchCase ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFindWholeWord:)) {
        item.state = _editorFind.wholeWord ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(toggleFindRegex:)) {
        item.state = _editorFind.regex ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (item.action == @selector(performTextFinderAction:))
        return !self.editorScroll.hidden && [_editorFind validateFindAction:item.tag];
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
    [self noteEdited];
}

// The buffer changed: it has unsaved changes exactly when it differs from
// the text on disk, so an edit taken back (typed and deleted, undone, a
// preview edit reverted) leaves none. Lengths are compared first, so most
// keystrokes cost nothing more; equal lengths compare the characters, a
// fraction of a millisecond for a large file.
- (void)noteEdited {
    [self scheduleBackups];
    [self scheduleAutoSave];
    NSString *now = self.sourceText ?: @"", *saved = self.savedText ?: @"";
    // A file deleted outside has nothing on disk to match.
    const BOOL dirty = _activeTab.gone || now.length != saved.length ||
                       ![now isEqualToString:saved];
    if (dirty && _activeTab.agentPreview) {
        _activeTab.agentPreview = NO;   // typed in: the user's tab now
        [self reloadTabStrip];
        [self saveSession];
    }
    if (dirty == self.dirty) return;
    self.dirty = dirty;
    [self updateTitle];
}

// Show the raw text as an editable, monospaced, syntax-highlighted document.
- (void)displaySourceEditable {
    NSFont *mono = MCEditorFont();   // editor.font-size and the zoom
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
    // Regions by the file's name; an untitled buffer by its extension.
    self.textView.folds.fileName = self.currentPath ?:
        [@"untitled." stringByAppendingString:self.currentExt ?: @"txt"];
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
    if (_sourceAttributes && !_highlightingSettingsFile && range.length <= 65536) {
        [self restyleChangedIn:range tokens:tokens inStorage:storage];
        return;
    }
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

// The attributes a token of `style` gets: the source attributes with its
// color (index 8 is plain text), made once per font and color scheme.
- (NSDictionary *)attributesForStyle:(int)style {
    if (_styleAttributesBase != _sourceAttributes) {
        NSMutableArray *all = [NSMutableArray arrayWithCapacity:9];
        for (int i = 0; i < 8; i++) {
            NSMutableDictionary *d = [_sourceAttributes mutableCopy];
            d[NSForegroundColorAttributeName] = _styleColors[i];
            [all addObject:[d copy]];
        }
        [all addObject:_sourceAttributes];
        _styleAttributes = all;
        _styleAttributesBase = _sourceAttributes;
    }
    return _styleAttributes[(NSUInteger)style];
}

// restyleRange: for the few lines an edit touches: the same attributes, but
// only pieces whose attributes differ are set. Setting the whole range
// first merged its attribute runs and adding the colors split them again,
// and each merge or split moved the storage's run array along by a
// memmove of everything after it (440,000 runs in a 100,000-line file), a
// good part of what was left of a keystroke there.
- (void)restyleChangedIn:(NSRange)range tokens:(const std::vector<Token> &)tokens
               inStorage:(NSTextStorage *)storage {
    const NSUInteger limit = NSMaxRange(range);
    auto paint = ^(NSUInteger start, NSUInteger end, int style) {
        if (end <= start) return;
        NSDictionary *want = [self attributesForStyle:style];
        NSRange run;
        NSDictionary *have = [storage attributesAtIndex:start effectiveRange:&run];
        if (run.location <= start && NSMaxRange(run) >= end &&
            (have == want || [have isEqualToDictionary:want]))
            return;
        [storage setAttributes:want range:NSMakeRange(start, end - start)];
    };
    NSUInteger at = range.location;
    for (size_t i = 0; i < tokens.size();) {
        size_t start = tokens[i].start, end = start + tokens[i].length;
        TokenStyle st = tokens[i].style;
        size_t j = i + 1;
        while (j < tokens.size() && tokens[j].style == st && tokens[j].start == end) {
            end = tokens[j].start + tokens[j].length;
            ++j;
        }
        i = j;
        if (start < at || end > limit || end <= start) continue;
        paint(at, start, 8);
        paint(start, end, (int)st & 7);
        at = end;
    }
    paint(at, limit, 8);
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
        std::u16string wide = U16(line);
        if (!Settings::findColor(wide, span)) {
            // An opacity is a link too, opening a slider; a dotted underline
            // says it can be clicked without looking like a swatch.
            ValueSpan os;
            double value;
            if (Settings::findOpacity(wide, os, value))
                [storage addAttributes:@{
                    NSLinkAttributeName: kOpacityLink,
                    NSUnderlineStyleAttributeName:
                        @(NSUnderlineStyleSingle | NSUnderlinePatternDot),
                    NSUnderlineColorAttributeName: Hex(0x4EA1F7),
                } range:NSMakeRange(lr.location + os.start, os.length)];
            return;
        }
        [storage addAttributes:@{
            NSBackgroundColorAttributeName: MCColor(span.color),
            NSForegroundColorAttributeName: ContrastColor(span.color),
            NSLinkAttributeName: kColorLink,
        } range:NSMakeRange(lr.location + span.start, span.length)];
    }];
}

- (BOOL)textView:(NSTextView *)tv clickedOnLink:(id)link atIndex:(NSUInteger)i {
    if (self.isMarkdown && self.previewMode && [link isKindOfClass:NSString.class] &&
        [link hasPrefix:kTaskLinkPrefix]) {
        [self toggleTaskOnSourceLine:[[link substringFromIndex:kTaskLinkPrefix.length] intValue]];
        return YES;
    }
    if (self.isMarkdown && self.previewMode && [link isEqual:kMathDownloadLink]) {
        [self downloadTectonicForMath];
        return YES;
    }
    if ([self showsRenderedMarkdown] && [link isKindOfClass:NSString.class]) {
        [self openMarkdownLink:link];
        return YES;
    }
    if ([link isEqual:kOpacityLink]) {
        [self pickOpacityAtIndex:i];
        return YES;
    }
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

// The rectangle of a character range in the text view's coordinates.
- (NSRect)editorRectForRange:(NSRange)range {
    NSTextView *tv = self.textView;
    NSLayoutManager *lm = tv.layoutManager;   // the editor is TextKit 1 (see Lsp.mm)
    [lm ensureLayoutForCharacterRange:NSMakeRange(0, NSMaxRange(range))];
    NSRange glyphs = [lm glyphRangeForCharacterRange:range actualCharacterRange:NULL];
    NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
    return NSOffsetRect(r, tv.textContainerOrigin.x, tv.textContainerOrigin.y);
}

// A click on an opacity value in the settings file: a slider in a popover.
// Like a color pick, each change rewrites the line (uncommenting it) and the
// file is saved shortly after, so the panel changes as the slider moves.
- (void)pickOpacityAtIndex:(NSUInteger)i {
    NSString *str = self.textView.string;
    if (i > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(i, 0)];
    ValueSpan span;
    double value;
    if (!Settings::findOpacity(U16([str substringWithRange:
            NSMakeRange(start, contentsEnd - start)]), span, value)) return;
    self.opacityLineStart = start;
    self.colorEditPath = self.currentPath;
    [self.textView breakUndoCoalescing];
    NSRange valueRange = NSMakeRange(start + span.start, span.length);
    [self.textView setSelectedRange:NSMakeRange(valueRange.location, 0)];
    __weak EditorController *weakSelf = self;
    [MCOpacityPopover showForValue:value
                    relativeToRect:[self editorRectForRange:valueRange]
                            ofView:self.textView
                          onChange:^(double v) { [weakSelf opacityPicked:v]; }];
}

- (void)opacityPicked:(double)value {
    NSTextView *tv = self.textView;
    if (!self.colorEditPath || ![self.currentPath isEqualToString:self.colorEditPath] ||
        !tv.editable)
        return;
    NSString *str = tv.string;
    if (self.opacityLineStart > str.length) return;
    NSUInteger start, contentsEnd;
    [str getLineStart:&start end:NULL contentsEnd:&contentsEnd
             forRange:NSMakeRange(self.opacityLineStart, 0)];
    NSRange lineRange = NSMakeRange(start, contentsEnd - start);
    std::u16string line = U16([str substringWithRange:lineRange]);
    std::u16string updated = Settings::setOpacity(line, value);
    if (updated == line) return;
    NSString *replacement = FromU16(updated);
    if (![tv shouldChangeTextInRange:lineRange replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:lineRange withString:replacement];
    [tv didChangeText];
    [tv.undoManager setActionName:@"Change Opacity"];
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(saveCurrentFile:)
                                               object:nil];
    [self performSelector:@selector(saveCurrentFile:) withObject:nil afterDelay:0.15];
}

// ------------------------------------------------- Adjust Settings (right click)
// The editor's context menu: AppKit's items, then Adjust Settings for the
// editor, or for the Markdown preview while it shows.
- (NSMenu *)textView:(NSTextView *)tv menu:(NSMenu *)menu forEvent:(NSEvent *)event
             atIndex:(NSUInteger)index {
    (void)event; (void)index;
    if (tv == self.textView)
        MCAddAdjustSettingsItem(menu, (self.isMarkdown && self.previewMode) ? @"markdown"
                                                                            : @"editor", tv);
    return menu;
}

// View > Adjust Settings > (surface): the id is the item's representedObject.
- (void)adjustAppearance:(id)sender {
    id surface = [sender respondsToSelector:@selector(representedObject)]
        ? [sender representedObject] : nil;
    if ([surface isKindOfClass:[NSString class]]) [self showSettingsForSurface:surface];
}

// Open the settings file with a surface's keys in view: the first key's line
// in the middle of the pane with its value selected, and every key's line
// tinted for a few seconds.
- (void)showSettingsForSurface:(NSString *)surface {
    const AppearanceSurface *s = Settings::appearanceSurface(surface.UTF8String ?: "");
    if (!s) return;
    [self openSettings:nil];
    if (![self isSettingsFile] || (self.isMarkdown && self.previewMode)) return;

    NSTextView *tv = self.textView;
    // A file written before some of these keys existed has no line for
    // them: their commented-out defaults are added at its end first, so
    // the swatches and sliders are there to use.
    const std::string missing = Settings::missingSurfaceLines(Utf8String(tv.string),
                                                              surface.UTF8String ?: "");
    if (!missing.empty() && tv.editable) [self appendSettingsLines:missing title:s->title];
    NSString *text = tv.string;
    std::string utf8 = text.UTF8String ?: "";
    // Start of each line, so a line number from the core maps to a range.
    NSMutableArray<NSNumber *> *starts = [NSMutableArray arrayWithObject:@0];
    for (NSUInteger i = 0; i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') [starts addObject:@(i + 1)];
    NSMutableArray<NSValue *> *lines = [NSMutableArray array];
    for (const char *key : s->keys) {
        int n = Settings::findKeyLine(utf8, key);
        if (n < 0 || n >= (int)starts.count) continue;
        NSRange r = [text lineRangeForRange:NSMakeRange(starts[n].unsignedIntegerValue, 0)];
        while (r.length && [text characterAtIndex:NSMaxRange(r) - 1] == '\n') r.length--;
        NSValue *v = [NSValue valueWithRange:r];
        if (![lines containsObject:v]) [lines addObject:v];
    }
    if (!lines.count) {
        [self flashStatus:[NSString stringWithFormat:@"No %s settings in this file.", s->title]];
        return;
    }

    NSRange first = lines.firstObject.rangeValue;
    ValueSpan value;
    NSRange selection = NSMakeRange(first.location, 0);
    if (Settings::findValue(U16([text substringWithRange:first]), value))
        selection = NSMakeRange(first.location + value.start, value.length);
    [tv setSelectedRange:selection];
    [self centerEditorOnRange:first];
    // Once more after the pane has had its layout pass.
    dispatch_async(dispatch_get_main_queue(), ^{
        if ([self isSettingsFile] && NSMaxRange(first) <= tv.string.length)
            [self centerEditorOnRange:first];
    });
    [self tintKeyLines:lines];
}

// Add the default lines of keys the settings file lacks at its end, under a
// line saying where they came from, as one undo step. A file with no unsaved
// changes is saved at once (the lines are all commented out, so nothing
// changes but the file), one with changes is left for the user to save.
- (void)appendSettingsLines:(const std::string &)lines title:(const char *)title {
    NSTextView *tv = self.textView;
    NSString *text = tv.string;
    const BOOL wasClean = !self.dirty;
    NSMutableString *add = [NSMutableString string];
    if (text.length && ![text hasSuffix:@"\n"]) [add appendString:@"\n"];
    if (text.length) [add appendString:@"\n"];
    [add appendFormat:@"# Added by Adjust Settings: the %s keys this file did not have yet.\n",
                      title];
    [add appendString:[NSString stringWithUTF8String:lines.c_str()] ?: @""];
    const NSRange end = NSMakeRange(text.length, 0);
    [tv breakUndoCoalescing];
    if (![tv shouldChangeTextInRange:end replacementString:add]) return;
    [tv.textStorage replaceCharactersInRange:end withString:add];
    [tv didChangeText];
    [tv breakUndoCoalescing];
    [tv.undoManager setActionName:@"Adjust Settings"];
    if (wasClean) [self saveCurrentFile:nil];
}

- (void)centerEditorOnRange:(NSRange)range {
    NSRect r = [self editorRectForRange:range];
    NSClipView *clip = self.editorScroll.contentView;
    CGFloat visible = clip.bounds.size.height;
    CGFloat maxY = MAX(0, NSHeight(self.textView.frame) - visible);
    CGFloat y = MIN(MAX(0, NSMidY(r) - visible / 2), maxY);
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [self.editorScroll reflectScrolledClipView:clip];
}

// A quiet accent tint behind the lines, held for a moment and then faded
// out. Temporary attributes, so the file and its undo are untouched; a color
// swatch keeps its own color.
- (void)tintKeyLines:(NSArray<NSValue *> *)lines {
    NSLayoutManager *lm = self.textView.layoutManager;
    NSUInteger generation = ++self.keyFlashGeneration;
    void (^paint)(CGFloat) = ^(CGFloat alpha) {
        if (generation != self.keyFlashGeneration) return;
        NSString *text = self.textView.string;
        NSUInteger length = text.length;
        [lm removeTemporaryAttribute:NSBackgroundColorAttributeName
                   forCharacterRange:NSMakeRange(0, length)];
        if (alpha <= 0 || ![self isSettingsFile]) return;
        NSColor *tint = [Hex(0x4EA1F7) colorWithAlphaComponent:alpha];
        for (NSValue *v in lines) {
            NSRange r = v.rangeValue;
            if (NSMaxRange(r) > length) continue;
            NSRange skip = NSMakeRange(NSMaxRange(r), 0);
            ColorSpan swatch;
            if (Settings::findColor(U16([text substringWithRange:r]), swatch))
                skip = NSMakeRange(r.location + swatch.start, swatch.length);
            NSRange before = NSMakeRange(r.location, skip.location - r.location);
            NSRange after = NSMakeRange(NSMaxRange(skip), NSMaxRange(r) - NSMaxRange(skip));
            for (NSValue *part in @[[NSValue valueWithRange:before],
                                    [NSValue valueWithRange:after]])
                if (part.rangeValue.length)
                    [lm addTemporaryAttribute:NSBackgroundColorAttributeName value:tint
                            forCharacterRange:part.rangeValue];
        }
    };
    paint(0.24);
    const double steps[][2] = {{2.4, 0.16}, {2.7, 0.09}, {3.0, 0.04}, {3.3, 0}};
    for (const auto &step : steps) {
        CGFloat alpha = step[1];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(step[0] * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ paint(alpha); });
    }
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

// A click on a task's box in the preview: tick or clear it in the source,
// which is then dirty until Cmd+S, as after any other preview edit.
- (void)toggleTaskOnSourceLine:(int)line {
    const std::string src = Utf8String(self.sourceText ?: @"");
    const std::string edited = MarkdownTasks::toggleBox(src, line);
    if (edited == src) { NSBeep(); return; }
    [self applyMarkdownSource:[NSString stringWithUTF8String:edited.c_str()]];
    [self.textView.undoManager setActionName:@"Toggle Task"];
}

// Option+Cmd+L in Markdown source: make the lines the selection touches tasks, or
// tick or clear them.
- (void)toggleTask:(id)sender {
    NSTextView *tv = self.textView;
    if (!self.isMarkdown || self.window.firstResponder != tv || !tv.editable) {
        NSBeep();
        return;
    }
    NSRange sel = tv.selectedRange;
    MarkdownTasks::Edit e = MarkdownTasks::toggle(U16(tv.string), sel.location, NSMaxRange(sel));
    if (!e.changed) { NSBeep(); return; }
    NSRange range = NSMakeRange(e.replaceStart, e.replaceLength);
    NSString *replacement = FromU16(e.replacement);
    [tv breakUndoCoalescing];
    if (![tv shouldChangeTextInRange:range replacementString:replacement]) return;
    [tv.textStorage replaceCharactersInRange:range withString:replacement];
    [tv didChangeText];
    [tv breakUndoCoalescing];
    [tv.undoManager setActionName:@"Toggle Task"];
    [tv setSelectedRange:NSMakeRange(e.selStart, e.selEnd - e.selStart)];
}

// ------------------------------------------------------------ format document
// Shift+Option+F: run the file's formatter (FormatRunner.mm) on the buffer and
// put its output in as one undo step. Only the source view of a file can be
// formatted, never a preview, a message, a picture or a diff.
- (BOOL)canFormatSource {
    return self.currentPath && self.textView.editable && !self.showingMessage;
}

- (void)formatDocument:(id)sender {
    if (![self canFormatSource]) { NSBeep(); return; }
    [self formatSourceQuietly:NO then:nil];
}

// Start formatting. `then` runs once the result is in (applied, or refused)
// if the same file is still open; quietly leaves out the notes that only
// answer a key press (no formatter for this kind of file, already formatted).
- (void)formatSourceQuietly:(BOOL)quiet then:(void (^)(void))then {
    NSTextView *tv = self.textView;
    NSString *original = [tv.string copy];
    NSString *path = self.currentPath;
    MCTab *tab = _activeTab;
    // Format Document while Cmd+S waits for format.on-save: this run takes
    // over the save, since the one it replaces will be dropped.
    if (!then && _formatSaveTab && _formatSaveTab == tab) {
        __weak EditorController *weakSelf = self;
        then = ^{ [weakSelf finishFormatSave]; };
    }
    // A selection formats its lines, where the formatter can do that.
    NSInteger first = 0, last = 0;
    NSRange sel = tv.selectedRange;
    if (sel.length > 0) {
        NSUInteger end = NSMaxRange(sel);
        if ([original characterAtIndex:end - 1] == '\n' && end - 1 > sel.location) end--;
        auto lineOf = ^NSInteger(NSUInteger index) {
            NSInteger line = 1;
            for (NSUInteger i = 0; i < index; i++)
                if ([original characterAtIndex:i] == '\n') line++;
            return line;
        };
        first = lineOf(sel.location);
        last = lineOf(end);
    }
    NSUInteger generation = ++_formatGeneration;
    __weak EditorController *weakSelf = self;
    __weak MCTab *weakTab = tab;
    MCFormatText(original, path, self.rootPath, first, last, 10, ^(MCFormatResult *r) {
        EditorController *s = weakSelf;
        // Another format started, or the window closed, or another tab is
        // on show (which saved this one as it left, if a save waited).
        if (!s || generation != s->_formatGeneration || !weakTab || weakTab != s->_activeTab)
            return;
        if (r.error) {
            if (!(quiet && r.unavailable))
                [s showFormatNote:r.error error:YES install:r.installPrograms];
        } else if (!s.textView.editable || ![s.textView.string isEqualToString:original]) {
            // Typed into meanwhile, or a preview took over: the output is
            // for text that is no longer there.
        } else if (![s replaceSourceWithText:r.text actionName:@"Format Document"]) {
            if (!quiet)
                [s showFormatNote:[NSString stringWithFormat:@"Already formatted (%@, %@).",
                                                             r.tool, r.style]
                            error:NO];
        }
        if (then) then();
    });
}

// format.on-save: NO when this file has no formatter, so the save goes on.
- (BOOL)formatBeforeSaving {
    if (![self canFormatSource] || Formatter::languageFor(Utf8String(self.currentPath)).empty())
        return NO;
    __weak EditorController *weakSelf = self;
    _formatSaveTab = _activeTab;
    [self formatSourceQuietly:YES then:^{ [weakSelf finishFormatSave]; }];
    return YES;
}

// The formatter is done (or its result was dropped): the save Cmd+S asked
// for happens now, if it still waits.
- (void)finishFormatSave {
    MCTab *t = _formatSaveTab;
    _formatSaveTab = nil;
    if (t && t == _activeTab) [self saveCurrentFile:nil];
}

// The tab whose save waits for the formatter is leaving the screen (another
// tab, or it closes): the formatter's result is dropped and the tab saved
// now, unformatted, while it is still the one on show.
- (void)saveBeforeLeavingFormatSave {
    if (!_formatSaveTab || _formatSaveTab != _activeTab) return;
    _formatSaveTab = nil;
    _formatGeneration++;
    [self saveCurrentFile:nil];
}

// Put `text` in place of the source as one undo step, touching only the
// lines that differ, so the highlighter and the language server see an
// ordinary edit and the caret and the scroll position stay with their text.
// NO when the text is the same.
//
// The hunks go in with one shouldChangeTextInRanges (one undo step) and one
// didChangeText. Each hunk is still its own storage edit, so the
// highlighter, the squiggles and the language server follow each one as
// they follow typing. Each hunk used to get a didChangeText too, and
// NSTextView lays out from the edit onwards in every one: 298 hunks spread
// over a 100,000-line file took 58 s, against 2 s now (most of it the
// layout of the lines between the first hunk and the last, which a
// contiguous layout has to redo).
- (BOOL)replaceSourceWithText:(NSString *)text actionName:(NSString *)name {
    CodeTextView *tv = self.textView;
    const std::u16string a = U16(tv.string), b = U16(text);
    std::vector<LineDiff::Hunk> hunks = LineDiff::diff(a, b);
    if (hunks.empty()) return NO;

    // Where the view is: the character at the top of the pane, and how far
    // above or below the top its line sits.
    NSRect visible = tv.visibleRect;
    NSUInteger top = [tv characterIndexForInsertionAtPoint:
        NSMakePoint(NSMinX(visible) + tv.textContainerOrigin.x + 1, NSMinY(visible) + 1)];
    __block CGFloat topY = NAN;
    NSUInteger length = a.size();
    if (top <= length)
        [tv enumerateRectsForRange:NSMakeRange(top, top < length ? 1 : 0) block:^(NSRect r) {
            if (isnan(topY)) topY = NSMinY(r);
        }];
    NSRange sel = tv.selectedRange;

    NSMutableArray<NSValue *> *ranges = [NSMutableArray arrayWithCapacity:hunks.size()];
    NSMutableArray<NSString *> *strings = [NSMutableArray arrayWithCapacity:hunks.size()];
    for (const LineDiff::Hunk &h : hunks) {
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(h.oldStart, h.oldLength)]];
        [strings addObject:FromU16(b.substr(h.newStart, h.newLength))];
    }
    NSUndoManager *um = tv.undoManager;
    [tv breakUndoCoalescing];
    [um beginUndoGrouping];
    const BOOL ok = [tv shouldChangeTextInRanges:ranges replacementStrings:strings];
    if (ok) {
        NSTextStorage *storage = tv.textStorage;
        for (NSInteger i = (NSInteger)ranges.count - 1; i >= 0; i--)   // last first
            [storage replaceCharactersInRange:ranges[(NSUInteger)i].rangeValue
                                   withString:strings[(NSUInteger)i]];
        [tv didChangeText];
    }
    [um setActionName:name];
    [um endUndoGrouping];
    [tv breakUndoCoalescing];
    if (!ok) return NO;

    size_t start = LineDiff::mapOffset(a, b, hunks, sel.location);
    size_t end = sel.length ? LineDiff::mapOffset(a, b, hunks, NSMaxRange(sel)) : start;
    if (end < start) end = start;
    [tv setSelectedRange:NSMakeRange(start, end - start)];
    if (!isnan(topY)) {
        NSUInteger newTop = LineDiff::mapOffset(a, b, hunks, top);
        NSUInteger newLength = tv.string.length;
        __block CGFloat y = NAN;
        [tv enumerateRectsForRange:NSMakeRange(newTop, newTop < newLength ? 1 : 0)
                             block:^(NSRect r) { if (isnan(y)) y = NSMinY(r); }];
        if (!isnan(y))
            [tv scrollPoint:NSMakePoint(NSMinX(visible), MAX(0, y - (topY - NSMinY(visible))))];
    }
    return YES;
}

// A note from Format Document by the caret: the formatter's complaint, what
// to install, or that there was nothing to change (which closes itself).
- (void)showFormatNote:(NSString *)message error:(BOOL)error {
    [self showFormatNote:message error:error install:nil];
}

// With `install` (formatters, best first), the note is how to install one:
// the message, then the commands with Copy and Run in Terminal.
- (void)showFormatNote:(NSString *)message error:(BOOL)error
               install:(NSArray<NSString *> *)install {
    self.lastFormatNote = message;
    [_formatNote close];
    _formatNote = nil;
    CodeTextView *tv = self.textView;
    if (!tv.window || tv.isHiddenOrHasHiddenAncestor) { NSBeep(); return; }

    MCInstallView *installView = nil;
    if (install.count) {
        __weak EditorController *weakSelf = self;
        installView = [[MCInstallView alloc]
            initWithFormatters:install message:message width:500
                 runInTerminal:^(NSString *command) { [weakSelf typeCommandInTerminal:command]; }];
        if (!installView.command) installView = nil;
    }

    NSTextField *label = [NSTextField wrappingLabelWithString:message];
    label.font = error ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]
                       : [NSFont systemFontOfSize:12];
    label.selectable = YES;
    const CGFloat maxWidth = 560;
    label.preferredMaxLayoutWidth = maxWidth;
    NSSize fit = [label sizeThatFits:NSMakeSize(maxWidth, CGFLOAT_MAX)];
    fit = NSMakeSize(ceil(MIN(fit.width, maxWidth)), ceil(fit.height));
    NSView *content = [[NSView alloc]
        initWithFrame:NSMakeRect(0, 0, fit.width + 24, fit.height + 20)];
    label.frame = NSMakeRect(12, 10, fit.width, fit.height);
    [content addSubview:label];
    NSViewController *vc = [NSViewController new];
    vc.view = installView ?: content;
    vc.preferredContentSize = vc.view.frame.size;
    NSPopover *note = [NSPopover new];
    note.contentViewController = vc;
    note.behavior = NSPopoverBehaviorTransient;
    if (installView) {
        note.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        __weak NSPopover *weakNote = note;
        installView.onDone = ^{ [weakNote close]; };
    }

    // Under the caret's line when it is on screen, else at the top of the pane.
    NSRect visible = tv.visibleRect;
    NSUInteger caret = tv.selectedRange.location, length = tv.string.length;
    __block NSRect anchor = NSZeroRect;
    [tv enumerateRectsForRange:NSMakeRange(caret, caret < length ? 1 : 0) block:^(NSRect r) {
        if (NSIsEmptyRect(anchor)) anchor = r;
    }];
    if (NSIsEmptyRect(anchor) || !NSIntersectsRect(anchor, visible))
        anchor = NSMakeRect(NSMidX(visible), NSMinY(visible), 1, 1);
    anchor.size.width = MAX(anchor.size.width, 1);
    anchor.size.height = MAX(anchor.size.height, 1);
    [note showRelativeToRect:anchor ofView:tv preferredEdge:NSRectEdgeMaxY];
    _formatNote = note;
    if (!error) {
        __weak NSPopover *weakNote = note;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{ [weakNote close]; });
    }
}

// ------------------------------------------- line numbers, brackets and zoom
// The gutter's colors and mode, the bracket box's color, and the font (into
// the source attributes; recolorEditor then sets it in the text).
- (void)applyGutterSettings {
    const Settings &st = [AppSettings shared].settings;
    MCLineGutter *g = self.gutter;
    const Settings::LineNumbers mode = st.lineNumbers();
    g.mode = mode == Settings::LineNumbers::Off      ? MCLineNumbersOff
           : mode == Settings::LineNumbers::Relative ? MCLineNumbersRelative
                                                     : MCLineNumbersOn;
    g.backgroundColor = MCColor(st.gutterBackground());
    g.numberColor = MCColor(st.lineNumberColor());
    g.activeNumberColor = MCColor(st.lineNumberActiveColor());
    self.textView.bracketMatchColor = MCColor(st.bracketMatchColor());
    [self setEditorFont:MCEditorFont()];
}

// The font for the source from now on: the attributes the highlighter
// restyles with, what typing gets, and the gutter's numbers. The text on
// show changes when it is next restyled.
- (void)setEditorFont:(NSFont *)font {
    self.gutter.font = font;
    if (_sourceAttributes && ![_sourceAttributes[NSFontAttributeName] isEqual:font]) {
        NSMutableDictionary *base = [_sourceAttributes mutableCopy];
        base[NSFontAttributeName] = font;
        _sourceAttributes = base;
    }
    if (self.textView.editable && _liveHighlight) {
        NSMutableDictionary *typing = [self.textView.typingAttributes mutableCopy];
        typing[NSFontAttributeName] = font;
        self.textView.typingAttributes = typing;
    }
}

// Zoom In, Zoom Out or Reset Zoom, from any window: the source on show
// takes the new size at once, keeping the line at the top of the pane.
- (void)editorZoomChanged:(NSNotification *)note {
    (void)note;
    NSFont *font = MCEditorFont();
    const BOOL showing = _liveHighlight && self.textView.editable;
    const BOOL changed = showing && ![_sourceAttributes[NSFontAttributeName] isEqual:font];
    if (!changed) {
        [self setEditorFont:font];
        // A rendered Markdown page follows the zoom too, keeping its place.
        if (self.isMarkdown && self.previewMode && !self.textView.editable)
            [self renderMarkdownKeepingPlace];
        return;
    }
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    NSFont *old = _sourceAttributes[NSFontAttributeName];
    const CGFloat scale = old.pointSize > 0 ? font.pointSize / old.pointSize : 1;
    NSUInteger top = 0;
    CGFloat offset = 0;
    if (n > 0) {
        const NSRect visible = tv.visibleRect;
        top = MIN([tv characterIndexForInsertionAtPoint:
                      NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)], n - 1);
        const NSRect r = [self textRectForCharacter:top];
        offset = NSIsEmptyRect(r) ? 0 : NSMinY(r) - NSMinY(visible);
    }
    [self setEditorFont:font];
    [self applyHighlighting];   // every run in the new font
    if (n > 0) {
        const NSRect r = [self textRectForCharacter:top];
        if (!NSIsEmptyRect(r)) {
            [tv.layoutManager ensureLayoutForBoundingRect:
                NSMakeRect(0, 0, CGFLOAT_MAX,
                           NSMaxY(r) + 2 * NSHeight(self.editorScroll.contentView.bounds))
                                          inTextContainer:tv.textContainer];
            [self scrollEditorToY:NSMinY(r) - offset * scale];
        }
    }
}

// The stretches of [lo, hi) that are strings or comments, from the
// highlighter's tokens of those lines, offset from lo (none without a
// grammar, so every bracket counts).
- (std::vector<Brackets::Span>)nonCodeSpansFrom:(NSUInteger)lo to:(NSUInteger)hi {
    std::vector<Brackets::Span> spans;
    if (!_hl || hi <= lo) return spans;
    NSTextStorage *storage = self.textView.textStorage;
    if (_hl->length() != storage.length) return spans;
    std::vector<Token> tokens;
    _hl->lineTokens(NSStringSource(storage.string), _hl->lineOf(lo), _hl->lineOf(hi - 1) + 1,
                    tokens);
    for (const Token &t : tokens) {
        if (t.style != TokenStyle::String && t.style != TokenStyle::Comment) continue;
        const size_t a = MAX((size_t)lo, t.start), b = MIN((size_t)hi, t.start + t.length);
        if (b <= a) continue;
        if (!spans.empty() && spans.back().end >= a - lo)
            spans.back().end = MAX(spans.back().end, b - lo);
        else
            spans.push_back({a - lo, b - lo});
    }
    return spans;
}

// Bracket matching runs on a window of the text around the caret: what the
// search may reach, and no more.
- (NSRange)bracketWindowAround:(NSUInteger)caret {
    const NSUInteger n = self.textView.textStorage.length;
    const NSUInteger reach = Brackets::kDefaultScan + 1;
    const NSUInteger lo = caret > reach ? caret - reach : 0;
    const NSUInteger hi = MIN(n, caret + reach);
    return NSMakeRange(lo, hi - lo);
}

- (BOOL)bracketMatchingApplies {
    CodeTextView *tv = self.textView;
    return _liveHighlight && tv.editable && !tv.multiCaretActive && !self.showingMessage;
}

- (void)textViewDidChangeSelection:(NSNotification *)note {
    if (note.object != self.textView || _bracketMatchQueued) return;
    // Once the edit or the selection settles (typing changes both).
    _bracketMatchQueued = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_bracketMatchQueued = NO;
        [self updateBracketMatch];
    });
}

// The bracket beside the caret and its partner get a box, as in VS Code.
- (void)updateBracketMatch {
    CodeTextView *tv = self.textView;
    const NSRange sel = tv.selectedRange;
    NSString *s = tv.textStorage.string;
    NSArray<NSValue *> *boxes = @[];
    if ([self bracketMatchingApplies] && sel.length == 0 && sel.location <= s.length) {
        const NSUInteger caret = sel.location;
        const BOOL near = (caret > 0 && Brackets::isBracket([s characterAtIndex:caret - 1])) ||
                          (caret < s.length && Brackets::isBracket([s characterAtIndex:caret]));
        if (near) {
            const NSRange w = [self bracketWindowAround:caret];
            std::vector<unichar> buf(w.length);
            [s getCharacters:buf.data() range:w];
            const Brackets::Pair p = Brackets::match(
                reinterpret_cast<const char16_t *>(buf.data()), buf.size(), caret - w.location,
                [self nonCodeSpansFrom:w.location to:NSMaxRange(w)]);
            if (p.found())
                boxes = @[[NSValue valueWithRange:NSMakeRange(w.location + p.open, 1)],
                          [NSValue valueWithRange:NSMakeRange(w.location + p.close, 1)]];
        }
    }
    tv.bracketRanges = boxes;
}

// Shift+Cmd+\ (VS Code's Go to Bracket): to the partner of the bracket by
// the caret, else to the closer of the pair around it, else to the next
// bracket.
- (void)goToBracket:(id)sender {
    (void)sender;
    CodeTextView *tv = self.textView;
    if (![self bracketMatchingApplies]) { NSBeep(); return; }
    const NSUInteger caret = NSMaxRange(tv.selectedRange);
    NSString *s = tv.textStorage.string;
    const NSRange w = [self bracketWindowAround:caret];
    std::vector<unichar> buf(w.length);
    [s getCharacters:buf.data() range:w];
    const size_t to = Brackets::jumpTarget(reinterpret_cast<const char16_t *>(buf.data()),
                                           buf.size(), caret - w.location,
                                           [self nonCodeSpansFrom:w.location to:NSMaxRange(w)]);
    if (to == Brackets::npos) { NSBeep(); return; }
    tv.selectedRange = NSMakeRange(w.location + to, 0);
    [tv scrollRangeToVisible:tv.selectedRange];
}

// -------------------------------------------------------------- editing hooks
// Arrows, Return, Tab and Esc drive the completion list while it is open;
// Option+Esc (complete:) asks the language server.
- (BOOL)textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    if (tv == self.textView && [_notes handleCommand:sel]) return YES;
    if ([self.lsp handleCommand:sel]) return YES;
    // Esc with nothing else to close closes the find bar, as in VS Code.
    if (tv == self.textView && sel == @selector(cancelOperation:) &&
        !self.textView.multiCaretActive && [_editorFind closeFromEditor])
        return YES;
    // Return, Tab, Shift+Tab and Backspace follow the language's indentation
    // (AutoIndent), and Return in a Markdown list continues the list.
    if (tv == self.textView && MCIndentCommand(tv, sel, self.currentPath, self.isMarkdown))
        return YES;
    return NO;
}

// A typed closer, or Python's ':' after else and the like, may move its line
// back; MCIndentDidChange (from textDidChange:) does it once the text is in.
- (BOOL)textView:(NSTextView *)tv shouldChangeTextInRange:(NSRange)range
    replacementString:(NSString *)replacement {
    // Several carets type the same character everywhere, and a selection
    // wrapped in brackets keeps its place; no line moves.
    if (tv == self.textView && replacement && !self.textView.multiCaretActive &&
        !self.textView.multiCarets.applying)
        MCIndentWillChange(tv, range, replacement);
    return YES;
}

// F9: sort the selected lines, or the keep-sorted block around the caret.
- (void)sortLines:(id)sender {
    if (self.window.firstResponder != self.textView) {
        NSBeep();
        return;
    }
    [self flashStatus:MCSortLines(self.textView)];
}

// Shift+F9: sort every keep-sorted block in the file.
- (void)fixKeepSorted:(id)sender {
    if (self.window.firstResponder != self.textView) {
        NSBeep();
        return;
    }
    [self flashStatus:MCFixKeepSorted(self.textView)];
}

// Ctrl+Space, F12, Option+Cmd+I. The session beeps and says why when the file has
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
// F2 is VS Code's key for both: the symbol in the editor, the selected file
// in the tree. The tree takes F2 itself (ClickOutline keyDown:), and a menu
// key that reaches here from the tree goes the same way.
- (void)renameSymbol:(id)sender {
    if (self.window.firstResponder == self.outline) {
        [self renameSelected:sender];
        return;
    }
    if ([self lspActionAllowed]) [self.lsp renameSymbol];
}
- (void)findReferences:(id)sender {
    if ([self lspActionAllowed]) [self.lsp findReferences];
}

- (void)showQuickFixes:(id)sender {
    if ([self lspActionAllowed]) [self.lsp showQuickFixes];
}
- (void)organizeImports:(id)sender {
    if ([self lspActionAllowed]) [self.lsp organizeImports];
}
// F8 / Shift+F8: the next or previous problem in the file on show.
- (void)nextProblem:(id)sender {
    if ([self lspActionAllowed]) [self.lsp goToProblem:YES];
}
- (void)previousProblem:(id)sender {
    if ([self lspActionAllowed]) [self.lsp goToProblem:NO];
}

// Shift+Cmd+M, as in VS Code: shows the Problems list with the keyboard in
// it; pressed in it, hides it again; pressed elsewhere while it shows,
// gives it the keyboard.
- (void)toggleProblems:(id)sender {
    if (self.problemsVisible && !self.problemsPanel.hasFocus &&
        [sender isKindOfClass:NSMenuItem.class]) {
        [self.problemsPanel focusList];
        return;
    }
    if (self.problemsVisible) {
        BOOL hadFocus = self.problemsPanel.hasFocus;
        self.problemsVisible = NO;
        [self relayoutRightArea];
        [self collapseRightAreaIfEmpty];
        if (hadFocus) [self focusAfterClosingPanel];
    } else {
        if (!self.problemsPanel) {
            MCProblemsPanel *panel = [[MCProblemsPanel alloc] initWithFrame:NSZeroRect];
            __weak EditorController *weakSelf = self;
            panel.onOpen = ^(MCProblem *p) { [weakSelf.lsp openProblem:p]; };
            panel.onEscape = ^{ [weakSelf focusEditor:nil]; };
            panel.onClose = ^{
                EditorController *s = weakSelf;
                if (s.problemsVisible) [s toggleProblems:nil];
            };
            panel.hidden = YES;
            [self.rightArea addSubview:panel];
            self.problemsPanel = panel;
        }
        self.problemsVisible = YES;
        [self updateProblems];
        [self showRightArea];
        [self relayoutRightArea];
        [self.problemsPanel focusList];
    }
    if (self.hintsVisible) [self updateHints];
}

// The servers' diagnostics changed: the counts in the status bar, and the
// list when it shows.
- (void)updateProblems {
    [self updateProblemCounts];
    if (self.problemsVisible)
        [self.problemsPanel setRows:[self.lsp problemRows] counts:self.lsp.problemCountsText
                               root:_root.path];
}

// The status bar's counts alone (cheap: no text is read), on every change
// of the language server's status too.
- (void)updateProblemCounts {
    LspSession *lsp = self.lsp;
    BOOL known = lsp.problemsKnown;
    self.problemsLabel.hidden = !known;
    if (known) {
        NSInteger errors = lsp.problemErrors, warnings = lsp.problemWarnings;
        NSColor *dot = errors ? [NSColor colorWithSRGBRed:0xF1 / 255.0 green:0x4C / 255.0
                                                     blue:0x4C / 255.0 alpha:1]
                     : warnings ? [NSColor colorWithSRGBRed:0xCC / 255.0 green:0xA7 / 255.0
                                                       blue:0 alpha:1]
                     : nil;
        [self.problemsLabel setStatus:lsp.problemCountsText dot:dot];
    }
    [self layoutStatusRight];
}
- (void)showLanguageServers:(id)sender {
    __weak EditorController *weakSelf = self;
    MCShowLanguageServers(self.lsp, self.lspLabel, ^(NSString *command) {
        [weakSelf typeCommandInTerminal:command];
    });
}

// ------------------------------------------------- installing a server
// The bar over the editor, when the open file's language server is wanted
// but missing. It is offered once per language per launch (across windows)
// and never for a language whose bar said Don't Show Again; it stays over
// that language's files in this window until closed or the server turns up.
- (void)updateInstallBar {
    NSString *missing = self.lsp.missingServer;
    NSString *language = self.lsp.currentLanguage;
    // The open file's server was missing a moment ago and runs now.
    if (_missingBefore && !missing && [language isEqualToString:_missingBefore] &&
        self.lsp.hasServer) {
        [_installPopover close];
        [self showStatusNote:[NSString stringWithFormat:@"Found the %@ language server",
                             @(Lsp::serverDisplayName(Utf8String(language)).c_str())]];
    }
    _missingBefore = [missing copy];
    if (_installBarKey && !missing && [language isEqualToString:_installBarKey]) {
        _installBarKey = nil;   // found, or switched off: the bar has done its job
    } else if (missing && ![missing isEqualToString:_installBarKey] &&
               !MCInstallBarMuted(missing) && MCInstallBarTakeOffer(missing)) {
        _installBarKey = [missing copy];   // a language not offered yet takes the bar
    }
    BOOL show = _installBarKey && [_installBarKey isEqualToString:missing];
    if (show && !self.installBar) {
        MCInstallBar *bar = [[MCInstallBar alloc] initWithFrame:NSZeroRect];
        __weak EditorController *weakSelf = self;
        bar.onInstall = ^(NSButton *sender) { [weakSelf showInstallHelpFrom:sender]; };
        bar.onClose = ^{ [weakSelf closeInstallBarMuting:NO]; };
        bar.onMute = ^{ [weakSelf closeInstallBarMuting:YES]; };
        self.installBar = bar;
        [self applyInstallBarColors];
        bar.hidden = YES;   // shown, and laid out, just below
        [self.rightArea addSubview:bar];
    }
    if (show) self.installBar.serverKey = _installBarKey;
    if (self.installBar && self.installBar.hidden == show) {
        self.installBar.hidden = !show;
        [self relayoutRightArea];
    }
}

// The bar takes the top of the editor's slot, under the tab strip.
- (NSRect)layoutInstallBarIn:(NSRect)slot editorShown:(BOOL)shown {
    MCInstallBar *bar = self.installBar;
    if (!bar) return slot;
    BOOL show = shown && !bar.hidden && _installBarKey &&
                [_installBarKey isEqualToString:self.lsp.missingServer];
    if (!show) {
        bar.frame = NSZeroRect;
        return slot;
    }
    const CGFloat h = MIN(kMCInstallBarHeight, NSHeight(slot));
    bar.frame = NSMakeRect(NSMinX(slot), NSMaxY(slot) - h, NSWidth(slot), h);
    return NSMakeRect(NSMinX(slot), NSMinY(slot), NSWidth(slot), NSHeight(slot) - h);
}

- (void)applyInstallBarColors {
    AppSettings *cfg = [AppSettings shared];
    [self.installBar setBackground:[cfg background:Surface::Sidebar]
                              text:[cfg text:Surface::Sidebar]];
}

- (void)closeInstallBarMuting:(BOOL)mute {
    if (mute && _installBarKey) MCInstallBarSetMuted(_installBarKey, YES);
    _installBarKey = nil;
    self.installBar.hidden = YES;
    [self relayoutRightArea];
}

// The install popover for the open file's missing server, by `anchor` (the
// bar's Install… button or the status bar item).
- (void)showInstallHelpFrom:(NSView *)anchor {
    NSString *key = self.lsp.missingServer;
    if (!key) return;
    [_installPopover close];
    std::string setting = [AppSettings shared].settings.lspCommand(Utf8String(key));
    NSString *message = setting.empty() ? nil
        : [NSString stringWithFormat:@"lsp.%@ in the settings runs %@, which was not found. "
                                     @"The server minicode suggests is below.",
                                     key, @(setting.c_str())];
    __weak EditorController *weakSelf = self;
    MCInstallView *view = [[MCInstallView alloc]
        initWithServer:key message:message width:500
         runInTerminal:^(NSString *command) { [weakSelf typeCommandInTerminal:command]; }];
    if (!view.command) return;
    NSRectEdge edge = anchor == self.lspLabel ? NSRectEdgeMaxY : NSRectEdgeMinY;
    _installPopover = MCShowInstallPopover(view, anchor, anchor.bounds, edge);
}

// Run in Terminal: the command goes into a terminal's input line, and the
// keyboard with it, for the user to read and press Return. Never run here.
// A busy terminal is left alone and a new one opened, as Run File does.
- (void)typeCommandInTerminal:(NSString *)command {
    BOOL fresh = self.terminalPanel == nil;
    [self showTerminalPanel];
    TerminalView *term = self.terminalPanel.activeTerminal;
    if (!fresh && term.busy) term = [self.terminalPanel addTab];
    if (![term typeCommand:command]) {
        term = [self.terminalPanel addTab];
        [term typeCommand:command];
    }
    [self showStatusNote:@"Press Return in the terminal to run the command"];
}
- (void)restartLanguageServer:(id)sender {
    [self.lsp restartCurrentServer];
}
- (void)selectPythonInterpreter:(id)sender {
    MCChoosePythonInterpreter(self.lsp, self.window);
}

- (void)textDidChange:(NSNotification *)note {
    // A copy of the storage's string. With AppKit's storage (an
    // NSBigMutableString) it shared the characters and took about a
    // microsecond; MCSourceStorage keeps a CFString, so it is a copy of the
    // characters, 0.13 ms for a 2.4 MB file (measured), still a small part
    // of a keystroke. backedUpText's pointer compare relies on a new object
    // per change.
    self.sourceText = self.textView.string;
    [self noteEdited];
    // The title's task count follows typing, a moment after it stops.
    if (self.isMarkdown) {
        [NSObject cancelPreviousPerformRequestsWithTarget:self
                                                 selector:@selector(updateTitle)
                                                   object:nil];
        [self performSelector:@selector(updateTitle) withObject:nil afterDelay:0.3];
    }
    [self flushHighlighting];   // recolor the lines this change touched
    MCIndentDidChange(self.textView, self.currentPath);
}

- (void)saveCurrentFile:(id)sender {
    // A message (welcome, binary file) is not the file's contents.
    if (!self.currentPath || self.showingMessage || self.isImage || self.isPDF ||
        self.isMedia) return;
    // format.on-save: Cmd+S (a sender) formats first and saves when the
    // formatter is done; saves asked for by MiniCode itself (nil) never wait.
    if (sender && [AppSettings shared].settings.formatOnSave() && [self formatBeforeSaving])
        return;
    // Never over a change made outside, or a file deleted outside, unasked.
    if (![self mayWriteTab:_activeTab]) return;
    // In markdown preview mode the text view holds rendered text, not source;
    // save the tracked source instead.
    // editor.keep-sorted-on-save: the blocks are sorted first, as one undo step.
    NSString *sortProblem = nil;
    if (self.textView.editable && [AppSettings shared].settings.keepSortedOnSave())
        sortProblem = MCKeepSortedBeforeSave(self.textView);
    NSString *text = self.textView.editable ? self.textView.string
                                            : (self.sourceText ?: @"");
    NSError *err = nil;
    BOOL ok = [text writeToFile:self.currentPath atomically:YES
                       encoding:NSUTF8StringEncoding error:&err];
    if (ok) {
        self.sourceText = text;
        self.savedText = text;
        _activeTab.gone = NO;
        self.dirty = NO;
        [self recordModDate];   // so our own save doesn't look like an external change
        [self.lsp documentSaved];
        [self updateTitle];
        [self flashStatus:sortProblem];
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
    if (MCViewerKindBesideText(self.viewerKind)) {   // the JSON outline: shown or not
        self.viewers.besideTextShown = !self.viewers.besideTextShown;
        [self relayoutRightArea];
        return;
    }
    [self revealEditor];
    NSTextView *tv = self.textView;
    const BOOL toPreview = !self.previewMode;
    NSInteger sourceLine = -1;       // the caret's line, for the preview to show
    NSInteger previewLine = -1;      // the line at the top of a scrolled preview
    if (toPreview) {
        _sourceSelection = tv.selectedRange;
        NSRect caret = [self textRectForCharacter:_sourceSelection.location];
        _sourceCaretOffset = NSMinY(caret) - NSMinY(tv.visibleRect);
        _haveSourcePosition = !NSIsEmptyRect(caret);
        sourceLine = [self lineOfCharacter:_sourceSelection.location inString:tv.string];
    } else if (self.isMarkdown &&
               !NSEqualPoints(self.editorScroll.contentView.bounds.origin, _previewEntryScroll)) {
        NSUInteger top = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMidX(tv.visibleRect), NSMinY(tv.visibleRect) + 4)];
        previewLine = [self markdownSourceLineNear:top];
    }
    if (tv.editable) self.sourceText = tv.string;  // keep edits
    self.previewMode = toPreview;
    [self refreshDisplay];
    [self updateTitle];

    if (toPreview && self.isMarkdown) {
        // Show the part of the page the caret was in, a third of the way down.
        NSUInteger target = [self markdownCharacterForSourceLine:sourceLine];
        NSRect r = [self textRectForCharacter:target];
        if (!NSIsEmptyRect(r))
            [self scrollEditorToY:NSMinY(r) - NSHeight(tv.visibleRect) / 3];
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
    } else if (!toPreview && previewLine >= 0) {
        // The preview was scrolled: open the source where it was looking.
        NSUInteger at = [self characterAtStartOfLine:previewLine inString:tv.string];
        tv.selectedRange = NSMakeRange(at, 0);
        NSRect r = [self textRectForCharacter:at];
        if (!NSIsEmptyRect(r)) [self scrollEditorToY:NSMinY(r) - 8];
    } else if (!toPreview && _haveSourcePosition) {
        const NSUInteger n = tv.string.length;
        NSRange sel = _sourceSelection;
        if (sel.location > n) sel = NSMakeRange(n, 0);
        if (NSMaxRange(sel) > n) sel.length = n - sel.location;
        tv.selectedRange = sel;
        NSRect r = [self textRectForCharacter:sel.location];
        if (!NSIsEmptyRect(r)) [self scrollEditorToY:NSMinY(r) - _sourceCaretOffset];
    }
    if (!self.previewMode) [self.window makeFirstResponder:tv];
    else if (MCViewerKindOverText(self.viewerKind) && _viewers.focusView)
        [self.window makeFirstResponder:_viewers.focusView];
}

// Where character `i` of the editor is drawn, in the text view's coordinates;
// the end of the text counts as the last character. Empty when it cannot be
// known. The editor runs TextKit 1 (see CLAUDE.md), so this asks the layout
// manager, and it never touches one under TextKit 2, where asking would
// switch the view over.
- (NSRect)textRectForCharacter:(NSUInteger)i {
    NSTextView *tv = self.textView;
    const NSUInteger n = tv.string.length;
    if (tv.textLayoutManager || n == 0) return NSZeroRect;
    NSLayoutManager *lm = tv.layoutManager;
    if (i >= n) i = n - 1;
    [lm ensureLayoutForCharacterRange:NSMakeRange(0, i + 1)];
    NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(i, 1)
                                actualCharacterRange:NULL];
    NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
    if (NSIsEmptyRect(r)) r.size = NSMakeSize(1, 1);   // a bare newline
    NSPoint o = tv.textContainerOrigin;
    return NSOffsetRect(r, o.x, o.y);
}

- (void)scrollEditorToY:(CGFloat)y {
    NSClipView *clip = self.editorScroll.contentView;
    const CGFloat most = NSHeight(self.textView.frame) - NSHeight(clip.bounds);
    y = MAX(0, MIN(y, MAX(0, most)));
    [clip scrollToPoint:NSMakePoint(clip.bounds.origin.x, y)];
    [self.editorScroll reflectScrolledClipView:clip];
}

- (NSInteger)lineOfCharacter:(NSUInteger)i inString:(NSString *)s {
    NSInteger line = 0;
    const NSUInteger end = MIN(i, s.length);
    for (NSUInteger k = 0; k < end; k++)
        if ([s characterAtIndex:k] == '\n') line++;
    return line;
}

- (NSUInteger)characterAtStartOfLine:(NSInteger)line inString:(NSString *)s {
    NSUInteger k = 0;
    for (NSInteger l = 0; l < line && k < s.length; k++)
        if ([s characterAtIndex:k] == '\n') l++;
    return k;
}

// The first rendered character that came from source line `line` or later
// (the end of the page when none did).
- (NSUInteger)markdownCharacterForSourceLine:(NSInteger)line {
    NSTextStorage *st = self.textView.textStorage;
    __block NSUInteger found = st.length;
    if (line <= 0) return 0;
    [st enumerateAttribute:kMarkdownSourceLine inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(NSNumber *v, NSRange r, BOOL *stop) {
        if (v && v.integerValue >= line) { found = r.location; *stop = YES; }
    }];
    return found;
}

// The source line behind rendered character `i`, or the nearest one after it.
- (NSInteger)markdownSourceLineNear:(NSUInteger)i {
    NSTextStorage *st = self.textView.textStorage;
    if (i >= st.length) i = st.length ? st.length - 1 : 0;
    __block NSInteger line = -1;
    if (st.length == 0) return line;
    [st enumerateAttribute:kMarkdownSourceLine inRange:NSMakeRange(i, st.length - i) options:0
                usingBlock:^(NSNumber *v, NSRange r, BOOL *stop) {
        (void)r;
        if (v) { line = v.integerValue; *stop = YES; }
    }];
    return line;
}

// ---------------------------------------------- editing from the preview

// A double-click on rendered Markdown opens the block behind it (a
// paragraph, heading, list item, quote, code block or table cell) in a
// popover holding its Markdown; saving splices just those bytes back into
// the buffer, which is then dirty until Cmd+S, as typing leaves it.
- (BOOL)editMarkdownAtCharacter:(NSUInteger)index point:(NSPoint)point {
    if (!self.isMarkdown || !self.previewMode) return NO;
    NSTextStorage *st = self.textView.textStorage;
    if (st.length == 0) return NO;
    // The insertion point can fall just after the character clicked.
    if (index >= st.length) index = st.length - 1;
    if (index > 0 && !NSPointInRect(point, [self textRectForCharacter:index]) &&
        NSPointInRect(point, [self textRectForCharacter:index - 1]))
        index--;
    // A box was ticked by the first click; the second does nothing more.
    id link = [st attribute:NSLinkAttributeName atIndex:index effectiveRange:NULL];
    if ([link isKindOfClass:NSString.class] && [link hasPrefix:kTaskLinkPrefix]) return YES;
    NSNumber *line = [st attribute:kMarkdownSourceLine atIndex:index effectiveRange:NULL];
    if (!line) return NO;
    NSArray<NSNumber *> *cell = [st attribute:kMarkdownTableCell atIndex:index
                               effectiveRange:NULL];
    int sourceLine = line.intValue, column = -1;
    if (cell.count == 2) {
        const int row = cell[0].intValue;
        sourceLine += row + (row > 0 ? 1 : 0);   // the separator row follows the header
        column = cell[1].intValue;
    }
    const std::string src = Utf8String(self.sourceText ?: @"");
    MarkdownEdit::Block block = MarkdownEdit::blockAt(src, sourceLine, column);
    if (block.kind == MarkdownEdit::Block::None) return NO;

    static NSString *const titles[] = {@"", @"Paragraph", @"Heading", @"List item",
                                       @"Quote", @"Code block", @"Table cell", @"Table row",
                                       @"Formula"};
    NSString *title = [titles[block.kind] stringByAppendingString:
        @" (Markdown; Return saves, Shift+Return for a new line)"];
    NSString *text = [NSString stringWithUTF8String:
        src.substr(block.start, block.end - block.start).c_str()] ?: @"";

    NSRect anchor = [self textRectForCharacter:index];
    if (NSIsEmptyRect(anchor)) anchor = NSMakeRect(point.x, point.y, 1, 1);
    if (!self.markdownEditPanel) self.markdownEditPanel = [MCMarkdownEditPanel new];
    __weak EditorController *weakSelf = self;
    NSString *before = self.sourceText;
    void (^addItem)(NSString *) = nil;
    if (block.kind == MarkdownEdit::Block::ListItem) {
        addItem = ^(NSString *newText) {
            EditorController *me = weakSelf;
            if (!me || ![me.sourceText isEqualToString:before]) return;
            std::string edited = MarkdownEdit::addItem(src, block, Utf8String(newText));
            [me applyMarkdownSource:[NSString stringWithUTF8String:edited.c_str()]];
        };
    }
    [self.markdownEditPanel showText:text title:title inView:self.textView rect:anchor
        commit:^(NSString *newText) {
            EditorController *me = weakSelf;
            if (!me || ![me.sourceText isEqualToString:before]) return;   // file changed meanwhile
            std::string edited = MarkdownEdit::replace(src, block, Utf8String(newText));
            if (edited != src) [me applyMarkdownSource:[NSString stringWithUTF8String:edited.c_str()]];
        }
        addItem:addItem];
    return YES;
}

// A new buffer from a preview edit: dirty, undoable with Cmd+Z, and shown
// again without moving the page.
- (void)applyMarkdownSource:(NSString *)source {
    if (!source || [source isEqualToString:self.sourceText]) return;
    NSString *old = self.sourceText ?: @"";
    NSUndoManager *undo = self.textView.undoManager;
    __weak EditorController *weakSelf = self;
    [undo registerUndoWithTarget:self handler:^(EditorController *target) {
        (void)target;
        [weakSelf applyMarkdownSource:old];
    }];
    [undo setActionName:@"Preview Edit"];
    self.sourceText = source;
    [self noteEdited];
    [self updateTitle];
    if (self.isMarkdown && self.previewMode) {
        NSPoint origin = self.editorScroll.contentView.bounds.origin;
        [self renderMarkdown:source];
        [self scrollEditorToY:origin.y];
        _previewEntryScroll = self.editorScroll.contentView.bounds.origin;
    } else if (self.isMarkdown) {
        [self refreshDisplay];
    }
}

// A click on a link in rendered Markdown. "#section" scrolls to that heading;
// a web address opens in the browser panel (another scheme, such as mailto,
// goes to the system); anything else is a path relative to the Markdown
// file, opened as the terminal opens paths, with "#section" or GitHub's
// "#L12" honoured when the target is a Markdown or source file.
- (void)openMarkdownLink:(NSString *)href {
    if ([href hasPrefix:@"#"]) {
        if (![self scrollToMarkdownAnchor:[href substringFromIndex:1]]) NSBeep();
        return;
    }
    NSURL *url = [NSURL URLWithString:href];
    NSString *scheme = url.scheme.lowercaseString;
    if (scheme.length && ![scheme isEqualToString:@"file"]) {
        if ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) {
            MCTermLink *link = [MCTermLink new];
            link.isURL = YES;
            link.target = href;
            [self openTerminalLink:link];
        } else if (url) {
            [[NSWorkspace sharedWorkspace] openURL:url];
        }
        return;
    }
    NSString *path = href, *fragment = nil;
    if ([scheme isEqualToString:@"file"]) path = url.path;
    NSRange hash = [path rangeOfString:@"#"];
    if (hash.location != NSNotFound) {
        fragment = [path substringFromIndex:hash.location + 1];
        path = [path substringToIndex:hash.location];
    }
    path = (path.stringByRemovingPercentEncoding ?: path).stringByExpandingTildeInPath;
    if (!path.absolutePath)
        path = [self.currentPath.stringByDeletingLastPathComponent
                   stringByAppendingPathComponent:path];
    path = path.stringByStandardizingPath;
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir]) {
        NSBeep();
        return;
    }
    MCTermLink *link = [MCTermLink new];
    link.target = path;
    link.isDirectory = dir;
    if (fragment.length > 1 && [fragment characterAtIndex:0] == 'L')
        link.line = [fragment substringFromIndex:1].intValue;   // GitHub's #L12
    [self openTerminalLink:link];
    if (fragment.length && link.line <= 0 && self.isMarkdown && self.previewMode &&
        [self.currentPath isEqualToString:path])
        [self scrollToMarkdownAnchor:fragment];
}

- (BOOL)scrollToMarkdownAnchor:(NSString *)anchor {
    NSString *key = (anchor.stringByRemovingPercentEncoding ?: anchor).lowercaseString;
    NSNumber *at = _markdownAnchors[key];
    if (!at) return NO;
    NSRect r = [self textRectForCharacter:at.unsignedIntegerValue];
    if (NSIsEmptyRect(r)) {
        [self.textView scrollRangeToVisible:NSMakeRange(at.unsignedIntegerValue, 0)];
        return YES;
    }
    [self scrollEditorToY:NSMinY(r) - 8];
    return YES;
}

- (void)renderMarkdown:(NSString *)content {
    std::string md = content.UTF8String ? content.UTF8String : "";
    std::vector<MdRun> runs = MarkdownParser::parse(md);
    _awaitedWebImages = [NSMutableSet set];   // markdownRun: fills it
    _webImageMarks = 0;
    _awaitedMath = [NSMutableSet set];        // and these
    _mathWanted = [NSMutableArray array];
    _mathMarks = 0;
    _mathOn = [self typesetsMath];            // asked once, not per formula
    _mathRunProblem = nil;                    // markdownRun: notes a failed run

    NSMutableAttributedString *out = [[NSMutableAttributedString alloc] init];
    NSMutableDictionary<NSString *, NSNumber *> *anchors = [NSMutableDictionary dictionary];
    // A paragraph's look comes from its first run, and every run in it gets
    // that paragraph style; a quote's or a code block's paragraphs share one
    // text block, which draws the bar or the box around all of them.
    NSParagraphStyle *para = nil;
    NSTextBlock *quoteBlock = nil, *codeBox = nil;
    BOOL lineStart = YES;
    MCMarkdownCodeBlocks *codeColors = [MCMarkdownCodeBlocks new];   // ```python and the like
    for (size_t i = 0; i < runs.size();) {
        const MdRun &r = runs[i];
        if (r.tableId > 0) {
            size_t end = i;
            while (end < runs.size() && runs[end].tableId == r.tableId) end++;
            [self appendMarkdownTable:runs from:i to:end into:out];
            i = end;
            lineStart = YES;
            quoteBlock = codeBox = nil;
            continue;
        }
        if (lineStart) {
            if (r.heading > 0) {
                // A heading's text may be several runs (bold, code, links).
                std::string title;
                for (size_t k = i; k < runs.size() && runs[k].heading > 0 &&
                                   runs[k].line == r.line; k++)
                    title += runs[k].text;
                NSString *slug = MCMarkdownAnchor([NSString stringWithUTF8String:title.c_str()]);
                // GitHub numbers repeats: intro, intro-1, intro-2.
                NSString *key = slug;
                for (int n = 1; anchors[key]; n++)
                    key = [NSString stringWithFormat:@"%@-%d", slug, n];
                if (key.length) anchors[key] = @(out.length);
            }
            para = [self markdownParagraphFor:r quote:&quoteBlock code:&codeBox];
        }
        NSAttributedString *piece = [self markdownRun:r style:para inCell:NO];
        if (piece.length) {
            if (r.codeBlock) [codeColors noteRun:NSMakeRange(out.length, piece.length) language:r.lang];
            [out appendAttributedString:piece];
            lineStart = [piece.string hasSuffix:@"\n"];
        }
        i++;
    }
    [codeColors colorPage:out];
    // A page whose math cannot be typeset says why in a line at its top.
    BOOL hasMath = NO;
    for (const MdRun &r : runs) hasMath = hasMath || r.math;
    NSAttributedString *note = !hasMath ? nil
                             : self.mathNoteWanted ? [self mathNote]
                             : _mathRunProblem ? [self mathNoteForProblem:_mathRunProblem]
                             // Formulas still out with tectonic: say so, or the TeX
                             // standing in looks like math that will never be typeset.
                             : _awaitedMath.count ? [self mathNoteTypesetting:_awaitedMath.count]
                             : nil;
    if (note) {
        [out insertAttributedString:note atIndex:0];
        for (NSString *key in anchors.allKeys)
            anchors[key] = @(anchors[key].unsignedIntegerValue + note.length);
    }
    _markdownAnchors = anchors;
    _liveHighlight = NO;   // rendered Markdown is not source
    [self.textView.textStorage setAttributedString:out];
    [self.textView scrollToBeginningOfDocument:nil];
    // Formulas not typeset yet go to tectonic in one run; the page renders
    // again when they are back (markdownMathArrived:).
    if (_mathWanted.count) MCMathTypeset(_mathWanted);
    _mathWanted = nil;
}

// ------------------------------------------------------ math in the preview

// Whether formulas are typeset: the setting, and tectonic to do it.
- (BOOL)typesetsMath {
    return [AppSettings shared].settings.math() && MCTectonicPath() != nil;
}

// The line at the top of a page with math is wanted when typesetting is
// on but tectonic is not there.
- (BOOL)mathNoteWanted {
    return [AppSettings shared].settings.math() && MCTectonicPath() == nil;
}

// The line at the top of a page, muted, with a link at its end if any.
- (NSAttributedString *)mathNoteText:(NSString *)text link:(NSString *)link {
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.paragraphSpacing = 10;
    NSDictionary *muted = @{
        NSFontAttributeName: [NSFont systemFontOfSize:MCZoomed(12)],
        NSForegroundColorAttributeName: Hex(0x9CA3AF),
        NSParagraphStyleAttributeName: ps,
        kMarkdownMathNote: @YES,
    };
    NSMutableAttributedString *note = [[NSMutableAttributedString alloc] init];
    [note appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:muted]];
    if (link) {
        NSMutableDictionary *a = [muted mutableCopy];
        a[NSLinkAttributeName] = kMathDownloadLink;
        a[NSForegroundColorAttributeName] = [[AppSettings shared] markdown:MarkdownColor::Link];
        [note appendAttributedString:[[NSAttributedString alloc] initWithString:link attributes:a]];
    }
    [note appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:muted]];
    return note;
}

// A whole run failed (tectonic could not fetch its files, say): why, and
// that it is tried again.
- (NSAttributedString *)mathNoteForProblem:(NSString *)problem {
    NSString *why = [problem stringByTrimmingCharactersInSet:
                                 NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if ([why hasSuffix:@"."]) why = [why substringToIndex:why.length - 1];
    return [self mathNoteText:[NSString stringWithFormat:
        @"The math on this page shows as TeX: tectonic could not typeset it (%@). "
        @"It is tried again when the page is next shown, a minute from now or later.", why]
                         link:nil];
}

// tectonic keeps the LaTeX files it downloads in its own cache. Before its
// first run there is none, and that run fetches about 42 MB first.
static BOOL MCTectonicCacheIsEmpty(void) {
    NSString *dir = NSProcessInfo.processInfo.environment[@"TECTONIC_CACHE_DIR"];
    if (!dir.length)
        dir = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)
                   .firstObject stringByAppendingPathComponent:@"TectonicProject.Tectonic"];
    NSArray *inside = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil];
    return inside.count == 0;
}

// "Typesetting 12 formulas…" while they are out with tectonic, with a word
// on the wait when this is tectonic's first run.
- (NSAttributedString *)mathNoteTypesetting:(NSUInteger)count {
    NSString *text = count == 1 ? @"Typesetting 1 formula…"
                                : [NSString stringWithFormat:@"Typesetting %lu formulas…",
                                                             (unsigned long)count];
    if (MCTectonicCacheIsEmpty())
        text = [text stringByAppendingString:
            @" tectonic's first run downloads its LaTeX files first, which takes about a minute."];
    return [self mathNoteText:text link:nil];
}

// "Math shows as TeX..." with a link that downloads tectonic, or how that
// download is going.
- (NSAttributedString *)mathNote {
    NSString *text, *link = nil;
    if (MCTectonicDownloading()) {
        text = @"Downloading tectonic, which typesets the math on this page…";
    } else if (_tectonicProblem) {
        text = [NSString stringWithFormat:@"tectonic could not be downloaded (%@), so the math "
                                          @"shows as TeX. ", _tectonicProblem];
        link = @"Try again";
    } else {
        text = @"The math on this page shows as TeX: MiniCode typesets it with tectonic, "
               @"which is not installed. ";
        link = @"Download tectonic";
    }
    return [self mathNoteText:text link:link];
}

- (void)downloadTectonicForMath {
    if (MCTectonicDownloading()) return;
    _tectonicProblem = nil;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    MCDownloadTectonic(^(NSString *problem) {
        EditorController *me = weakSelf;
        if (!me) return;
        me->_tectonicProblem = [problem copy];
        if (me.isMarkdown && me.previewMode && [path isEqualToString:me.currentPath])
            [me renderMarkdownKeepingPlace];
    });
    [self renderMarkdownKeepingPlace];   // "Downloading..."
}

// How long the line about tectonic at the top of the page is, or 0.
- (NSUInteger)mathNoteLength {
    NSTextStorage *st = self.textView.textStorage;
    NSRange r = NSMakeRange(0, 0);
    if (st.length && [st attribute:kMarkdownMathNote atIndex:0 longestEffectiveRange:&r
                                inRange:NSMakeRange(0, st.length)])
        return r.length;
    return 0;
}

// The typeset formula for a math run, or nil when it is to show as TeX:
// typesetting is off, it is still being typeset, or it failed.
- (MCMathFormula *)typesetFormulaFor:(const MdRun &)r {
    if (!r.math || !_mathOn) return nil;
    NSString *tex = [NSString stringWithUTF8String:r.text.c_str()];
    if (!tex) return nil;
    MCMathFormula *f = MCMathLookup(tex, r.math == 2);
    return f && !f.failed ? f : nil;
}

// Screen points per PDF point of a formula set beside text of `fontSize`.
// TeX sets the formulas at 10 pt. Computer Modern's lowercase letters are
// smaller for their size than the system font's and its capitals about the
// same, so its em is made a little larger than the text's: halfway, near
// enough, between matching the capitals and matching the lowercase.
static CGFloat MCMathScaleFor(CGFloat fontSize) {
    static const CGFloat kEmToText = 1.12;             // math em / text size
    static const CGFloat kTenPointsInPdf = 10 * 72.0 / 72.27;
    return fontSize * kEmToText / kTenPointsInPdf;
}

// Some formulas of the page on show are back from tectonic: render it again
// with them in, once for all that arrive together, keeping the place.
- (void)markdownMathArrived:(NSNotification *)note {
    NSSet<NSString *> *keys = note.userInfo[MCMathKeysKey];
    if (!_awaitedMath || ![_awaitedMath intersectsSet:keys]) return;
    [_awaitedMath minusSet:keys];
    if (_mathRenderQueued) return;
    _mathRenderQueued = YES;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [weakSelf showArrivedMathForPath:path];
    });
}

- (void)showArrivedMathForPath:(NSString *)path {
    _mathRenderQueued = NO;
    if (!_awaitedMath || ![self showsRenderedMarkdown] || !path ||
        ![path isEqualToString:self.currentPath])
        return;
    [self renderMarkdownKeepingPlace];
}

// A web picture finished. When the page on show waits for it, the page is
// rendered again with the picture in, and pictures finishing close together
// share one render. A failure keeps the alt text and needs nothing.
- (void)markdownWebImageArrived:(NSNotification *)note {
    NSString *src = note.userInfo[MCMarkdownWebImageURLKey];
    if (!src || ![_awaitedWebImages containsObject:src]) return;
    [_awaitedWebImages removeObject:src];
    if (![note.userInfo[MCMarkdownWebImageOKKey] boolValue] || _webImageRenderQueued) return;
    _webImageRenderQueued = YES;
    NSString *path = self.currentPath;
    __weak EditorController *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        [weakSelf showArrivedWebImagesForPath:path];
    });
}

- (void)showArrivedWebImagesForPath:(NSString *)path {
    _webImageRenderQueued = NO;
    // Not after the window closed, another file was opened, or the source
    // took the preview's place (the preview renders anew when it is back).
    if (!_awaitedWebImages || ![self showsRenderedMarkdown] || !path ||
        ![path isEqualToString:self.currentPath])
        return;
    [self renderMarkdownKeepingPlace];
}

// Render the preview again, keeping the text at the top of the pane where it
// was. Each web picture that has arrived takes the place of its alt text, and
// each typeset formula the place of its TeX, one character for many, and
// moves what follows by the height it adds, so the text to keep is found
// again by its character, counted past those replacements (and past the
// line about tectonic, when it is going). A page at the very top stays at
// the top, as a browser's does while its pictures load.
- (void)renderMarkdownKeepingPlace {
    NSTextView *tv = self.textView;
    NSTextStorage *st = tv.textStorage;
    NSClipView *clip = self.editorScroll.contentView;
    const NSPoint origin = clip.bounds.origin;
    const BOOL unscrolled = NSEqualPoints(origin, _previewEntryScroll);
    NSUInteger anchor = NSNotFound;
    CGFloat offset = 0;
    if (origin.y > 0 && st.length > 0) {
        const NSRect visible = tv.visibleRect;
        NSUInteger i = [tv characterIndexForInsertionAtPoint:
            NSMakePoint(NSMinX(visible) + 20, NSMinY(visible) + 1)];
        if (i >= st.length) i = st.length - 1;
        const NSRect r = [self textRectForCharacter:i];
        if (!NSIsEmptyRect(r)) {
            offset = NSMinY(r) - NSMinY(visible);
            // Stretches before the anchor that change length: where they
            // are, and how many characters they will be.
            NSMutableArray<NSArray<NSNumber *> *> *changes = [NSMutableArray array];
            const NSRange before = NSMakeRange(0, i + 1);
            [st enumerateAttribute:kMarkdownWebImage inRange:before options:0
                        usingBlock:^(NSArray *mark, NSRange range, BOOL *stop) {
                (void)stop;
                if (mark && MCMarkdownWebImageIsReady(mark.firstObject))
                    [changes addObject:@[@(range.location), @(range.length), @1]];
            }];
            if ([self typesetsMath])
                [st enumerateAttribute:kMarkdownMath inRange:before options:0
                            usingBlock:^(NSArray *mark, NSRange range, BOOL *stop) {
                    (void)stop;
                    if (mark && MCMathIsReady(mark.firstObject))
                        [changes addObject:@[@(range.location), @(range.length), @1]];
                }];
            [changes sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) {
                return [a[0] compare:b[0]];
            }];
            NSUInteger shift = 0, inside = NSNotFound;
            for (NSArray<NSNumber *> *c in changes) {
                const NSRange range = NSMakeRange(c[0].unsignedIntegerValue,
                                                  c[1].unsignedIntegerValue);
                if (NSLocationInRange(i, range)) {
                    inside = range.location - shift;
                    break;
                }
                shift += range.length - c[2].unsignedIntegerValue;
            }
            anchor = inside != NSNotFound ? inside : i - shift;
        }
    }
    const NSUInteger noteBefore = [self mathNoteLength];
    [self renderMarkdown:[self markdownToRender]];
    // The line about tectonic at the top may have come or gone.
    const NSUInteger noteAfter = [self mathNoteLength];
    if (anchor != NSNotFound) {
        if (anchor >= noteBefore) anchor = anchor - noteBefore + noteAfter;
        else anchor = 0;
    }
    if (anchor != NSNotFound && st.length > 0) {
        if (!tv.textLayoutManager)   // TextKit 1, as the editor is: the full height
            [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
        const NSRect r = [self textRectForCharacter:MIN(anchor, st.length - 1)];
        [self scrollEditorToY:NSIsEmptyRect(r) ? origin.y : NSMinY(r) - offset];
    }
    if (unscrolled) _previewEntryScroll = clip.bounds.origin;
}

// GitHub's anchor for a heading: lowercase, punctuation dropped (letters of
// any script, digits, hyphens and underscores stay), spaces as hyphens.
static NSString *MCMarkdownAnchor(NSString *title) {
    NSString *t = [title stringByTrimmingCharactersInSet:
                            NSCharacterSet.whitespaceAndNewlineCharacterSet].lowercaseString;
    NSMutableString *slug = [NSMutableString string];
    [t enumerateSubstringsInRange:NSMakeRange(0, t.length)
                          options:NSStringEnumerationByComposedCharacterSequences
                       usingBlock:^(NSString *ch, NSRange r, NSRange er, BOOL *stop) {
        (void)r; (void)er; (void)stop;
        unichar c = [ch characterAtIndex:0];
        if (c == ' ') [slug appendString:@"-"];
        else if (c == '-' || c == '_' ||
                 [NSCharacterSet.alphanumericCharacterSet characterIsMember:c])
            [slug appendString:ch];
    }];
    return slug;
}

// The preview's vertical rhythm, in points at no zoom: the empty line
// between two blocks (the parser's `gap` runs), and the indent of each list
// level.
static const CGFloat kMarkdownGap = 12;
static const CGFloat kMarkdownListStep = 24;

// A text block as wide as the text container.
static NSTextBlock *MCFullWidthBlock(void) {
    NSTextBlock *b = [NSTextBlock new];
    [b setValue:100 type:NSTextBlockPercentageValueType forDimension:NSTextBlockWidth];
    return b;
}

// The paragraph style for a paragraph of rendered Markdown, from its first
// run. A block quote draws a bar down its left side and a code block a box;
// `quote` and `code` carry that text block from one paragraph to the next,
// so the paragraphs of one quote or code block (and a quote's inner gaps)
// share it and it is drawn once around them all.
- (NSParagraphStyle *)markdownParagraphFor:(const MdRun &)r
                                     quote:(NSTextBlock * __strong *)quote
                                      code:(NSTextBlock * __strong *)code {
    // Display math on a line of its own: centred when it is typeset, in a
    // code block's box while it shows as TeX.
    if (r.math == 2 && r.heading == 0 && !r.table && ![self typesetFormulaFor:r]) {
        MdRun box = r;
        box.math = 0;
        box.codeBlock = true;
        return [self markdownParagraphFor:box quote:quote code:code];
    }
    AppSettings *cfg = [AppSettings shared];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = 3.0;
    if (r.math == 2 && r.heading == 0 && !r.table) {
        ps.alignment = NSTextAlignmentCenter;
        ps.lineSpacing = 0;
        ps.paragraphSpacingBefore = 4;
        ps.paragraphSpacing = 4;
    }
    NSMutableArray<NSTextBlock *> *blocks = [NSMutableArray array];
    if (r.quote) {
        if (!*quote) {
            NSTextBlock *b = MCFullWidthBlock();
            [b setWidth:3 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
                   edge:NSRectEdgeMinX];
            [b setBorderColor:Hex(0x4B5563) forEdge:NSRectEdgeMinX];
            [b setWidth:13 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinX];
            *quote = b;
        }
        [blocks addObject:*quote];
    } else {
        *quote = nil;
    }
    if (r.codeBlock) {
        if (!*code) {
            NSTextBlock *b = MCFullWidthBlock();
            b.backgroundColor = MCColor(cfg.settings.markdownCodeBackground());
            [b setWidth:12 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinX];
            [b setWidth:12 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxX];
            [b setWidth:9 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinY];
            [b setWidth:9 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxY];
            *code = b;
        }
        [blocks addObject:*code];
        ps.lineSpacing = 2.0;
    } else {
        *code = nil;
    }
    if (r.gap) {
        ps.minimumLineHeight = ps.maximumLineHeight = MCZoomed(kMarkdownGap);
        ps.lineSpacing = 0;
    } else if (r.heading > 0) {
        ps.paragraphSpacingBefore = r.heading <= 2 ? 10.0 : 4.0;
        ps.paragraphSpacing = 2.0;
        if (r.heading <= 2) {   // a line under the two biggest, as GitHub has
            NSTextBlock *b = MCFullWidthBlock();
            [b setWidth:1 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
                   edge:NSRectEdgeMaxY];
            [b setBorderColor:Hex(0x3F3F46) forEdge:NSRectEdgeMaxY];
            [b setWidth:6 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxY];
            [blocks addObject:b];
        }
    } else if (r.rule) {
        NSTextBlock *b = MCFullWidthBlock();
        [b setWidth:2 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
               edge:NSRectEdgeMaxY];
        [b setBorderColor:Hex(0x4B5563) forEdge:NSRectEdgeMaxY];
        [blocks addObject:b];
        ps.minimumLineHeight = ps.maximumLineHeight = 4;
        ps.lineSpacing = 0;
    }
    if (r.listDepth > 0) {
        // The text of every line sits at the level's indent; a marker is
        // right-aligned just before it, so numbers of any width line up.
        const CGFloat step = MCZoomed(kMarkdownListStep);
        const CGFloat text = r.listDepth * step;
        ps.headIndent = text;
        ps.firstLineHeadIndent = r.marker ? text - step : text;
        ps.tabStops = @[
            [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentRight
                                            location:text - 7 options:@{}],
            [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                            location:text options:@{}],
        ];
        ps.paragraphSpacing = 3.0;
    }
    ps.textBlocks = blocks;
    return ps;
}

// One run of rendered Markdown, set in the paragraph style `ps`. Inside a
// table cell (`inCell`) the text is set in the body font, since a real
// table lines its columns up without monospace.
- (NSAttributedString *)markdownRun:(const MdRun &)r
                              style:(NSParagraphStyle *)ps
                             inCell:(BOOL)inCell {
    NSString *s = [NSString stringWithUTF8String:r.text.c_str()];
    if (!s) return nil;

    // Sizes at no zoom; Zoom In and Out scale the preview with the editor.
    NSFont *body = [NSFont systemFontOfSize:MCZoomed(15)];
    NSFont *mono = [NSFont monospacedSystemFontOfSize:MCZoomed(13)
                                               weight:NSFontWeightRegular];
    AppSettings *cfg = [AppSettings shared];
    NSFont *font = body;
    NSColor *color = [cfg text:Surface::Editor];
    NSMutableDictionary *a = [NSMutableDictionary dictionary];

    if (r.heading > 0) {
        CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
        font = [NSFont boldSystemFontOfSize:MCZoomed(sizes[r.heading])];
        color = [cfg markdown:MarkdownColor::Heading];
    }
    // Display math on a line of its own, shown as TeX, sits in a code box.
    const BOOL boxedMath = r.math == 2 && r.heading == 0 && !r.table;
    if (r.codeBlock || r.code) {
        font = mono;
        color = [cfg markdown:MarkdownColor::Code];
        if (!r.codeBlock && !boxedMath)   // a code block's box is its paragraphs' text block
            a[NSBackgroundColorAttributeName] =
                MCColor(cfg.settings.markdownCodeBackground());
    }
    if (r.table && !inCell) {
        font = mono;   // monospace keeps the padded columns aligned
        if (!r.code) color = r.bold ? [cfg markdown:MarkdownColor::Heading]
                                    : [cfg text:Surface::Editor];
    }
    if (r.table && inCell && r.bold && !r.code)
        color = [cfg markdown:MarkdownColor::Heading];
    if (r.quote && !r.code) color = [cfg markdown:MarkdownColor::Quote];
    if (r.marker) {
        // "  • " from the parser: the tabs of the paragraph style place it.
        NSString *m = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        s = [NSString stringWithFormat:@"\t%@\t", m];
    }
    // A line break inside a paragraph, not the start of a new one.
    if (r.hardBreak) s = @" ";
    if (r.rule) font = [NSFont systemFontOfSize:2];
    if (r.strike)
        a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    if (r.link) { color = [cfg markdown:MarkdownColor::Link]; a[NSUnderlineStyleAttributeName] =
        @(NSUnderlineStyleSingle); }
    // A link's target as written; textView:clickedOnLink: resolves it.
    NSString *href = r.link && !r.url.empty()
                         ? [NSString stringWithUTF8String:r.url.c_str()] : nil;
    if (href.length) a[NSLinkAttributeName] = href;
    if (r.line >= 0) a[kMarkdownSourceLine] = @(r.line);
    // A checked task's text: muted and struck through.
    if (r.task == 2 && !r.marker) {
        color = [cfg markdown:MarkdownColor::Done];
        a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    }
    // A task's marker is a box to click, placed by the tabs as a bullet is.
    if (r.marker && r.task && r.line >= 0) {
        MCTaskBox *att = [MCTaskBox new];
        att.box = [MCTaskBoxCell new];
        att.box.checked = r.task == 2;
        att.box.ink = [cfg text:Surface::Editor];
        att.box.fill = [cfg markdown:MarkdownColor::Link];
        NSMutableDictionary *ta = [a mutableCopy];
        ta[NSFontAttributeName] = font;
        ta[NSForegroundColorAttributeName] = color;
        ta[NSParagraphStyleAttributeName] = ps;
        NSMutableAttributedString *m =
            [[NSMutableAttributedString alloc] initWithString:@"\t" attributes:ta];
        NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
            initWithAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
        [pic addAttributes:ta range:NSMakeRange(0, pic.length)];
        [pic addAttribute:NSLinkAttributeName
                    value:[kTaskLinkPrefix stringByAppendingFormat:@"%d", r.line]
                    range:NSMakeRange(0, pic.length)];
        [pic addAttribute:NSToolTipAttributeName
                    value:r.task == 2 ? @"Done: click to clear" : @"Click to tick"
                    range:NSMakeRange(0, pic.length)];
        [m appendAttributedString:pic];
        [m appendAttributedString:[[NSAttributedString alloc] initWithString:@"\t"
                                                                  attributes:ta]];
        return m;
    }
    if (!ps) ps = [NSParagraphStyle defaultParagraphStyle];
    if (r.math && cfg.settings.math()) {
        // Typeset, a picture on the baseline in the color the text around
        // it has. Otherwise its TeX in the code style: while it is being
        // typeset (it is asked for once the page is built), or with TeX's
        // complaint on hover when it failed.
        const BOOL display = r.math == 2;
        MCMathFormula *f = _mathOn ? MCMathLookup(s, display) : nil;
        if (f && !f.failed) {
            NSColor *ink = [cfg text:Surface::Editor];
            NSFont *around = body;
            if (r.heading > 0) {
                const CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
                around = [NSFont boldSystemFontOfSize:MCZoomed(sizes[r.heading])];
                ink = [cfg markdown:MarkdownColor::Heading];
            }
            if (r.table && r.bold) ink = [cfg markdown:MarkdownColor::Heading];
            if (r.quote) ink = [cfg markdown:MarkdownColor::Quote];
            if (r.link) ink = [cfg markdown:MarkdownColor::Link];
            MCMathAttachment *att = [[MCMathAttachment alloc]
                initWithFormula:f scale:MCMathScaleFor(around.pointSize) color:ink font:around];
            NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
                initWithAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
            NSMutableDictionary *pa = [NSMutableDictionary dictionary];
            pa[NSParagraphStyleAttributeName] = ps;
            pa[NSFontAttributeName] = around;
            if (href.length) pa[NSLinkAttributeName] = href;
            if (r.line >= 0) pa[kMarkdownSourceLine] = @(r.line);
            [pic addAttributes:pa range:NSMakeRange(0, pic.length)];
            return pic;
        }
        if (f.failed) {
            if (f.transient) _mathRunProblem = f.error;
            a[NSToolTipAttributeName] = f.error.length ? f.error : @"TeX could not typeset this.";
            // A dotted red line under it says there is something to hover.
            a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle | NSUnderlinePatternDot);
            a[NSUnderlineColorAttributeName] = Hex(0xF14C4C);
        } else if (_mathOn) {
            NSString *key = MCMathKey(s, display);
            [_awaitedMath addObject:key];
            [_mathWanted addObject:@[s, @(display)]];
            a[kMarkdownMath] = @[key, @(++_mathMarks)];
        }
    }
    if (r.image) {
        NSString *src = [NSString stringWithUTF8String:r.src.c_str()];
        NSImage *picture = MCMarkdownLoadImage(src,
            self.currentPath.stringByDeletingLastPathComponent);
        // An https picture comes from the cache, or is fetched in the
        // background while its alt text stands in; the page is rendered
        // again when it arrives (markdownWebImageArrived:).
        BOOL waiting = NO;
        if (!picture && MCMarkdownIsWebImage(src) && cfg.settings.webImages())
            picture = MCMarkdownWebImage(src, &waiting);
        if (waiting) {
            [_awaitedWebImages addObject:src];
            a[kMarkdownWebImage] = @[src, @(++_webImageMarks)];
        }
        if (picture) {
            MCMarkdownImage *att = [[MCMarkdownImage alloc] initWithPicture:picture];
            NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
                initWithAttributedString:
                    [NSAttributedString attributedStringWithAttachment:att]];
            [pic addAttribute:NSParagraphStyleAttributeName value:ps
                        range:NSMakeRange(0, pic.length)];
            if (href.length)
                [pic addAttribute:NSLinkAttributeName value:href range:NSMakeRange(0, pic.length)];
            if (r.line >= 0)
                [pic addAttribute:kMarkdownSourceLine value:@(r.line)
                            range:NSMakeRange(0, pic.length)];
            return pic;
        }
        // Missing, still on its way, or a web picture that may not or could
        // not be fetched: its alt text, muted.
        if (!r.link) color = Hex(0x9CA3AF);
        if (s.length == 0) s = src;
    }

    NSFontManager *fm = [NSFontManager sharedFontManager];
    if (r.bold) font = [fm convertFont:font toHaveTrait:NSBoldFontMask];
    if (r.italic) font = [fm convertFont:font toHaveTrait:NSItalicFontMask];

    a[NSFontAttributeName] = font;
    a[NSForegroundColorAttributeName] = color;
    a[NSParagraphStyleAttributeName] = ps;
    return [[NSAttributedString alloc] initWithString:s attributes:a];
}

// How wide a table cell's content must be (its widest word or formula, which
// cannot wrap) and would like to be (all of it on one line).
static void MCMeasureCell(NSAttributedString *s, CGFloat *least, CGFloat *most) {
    *least = *most = 0;
    if (s.length == 0) return;
    *most = ceil([s boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX) options:0].size.width);
    NSString *str = s.string;
    NSCharacterSet *space = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    const NSUInteger n = str.length;
    for (NSUInteger i = 0; i < n;) {
        while (i < n && [space characterIsMember:[str characterAtIndex:i]]) i++;
        NSUInteger j = i;
        while (j < n && ![space characterIsMember:[str characterAtIndex:j]]) j++;
        if (j > i) {
            NSAttributedString *word = [s attributedSubstringFromRange:NSMakeRange(i, j - i)];
            *least = MAX(*least, ceil([word boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)
                                                        options:0].size.width));
        }
        i = j;
    }
}

// Each column's share of a table `available` points wide, as percentages,
// laid out the way a browser lays out a table: every column gets its widest
// piece that cannot wrap, and the room left over goes to the columns by how
// much wider their text would like to be. `chrome` is a cell's padding and
// border. Empty when the pane is too narrow to bother.
static std::vector<CGFloat> MCTableColumnWidths(NSArray<NSAttributedString *> *cells, int rows,
                                                int cols, CGFloat available, CGFloat chrome) {
    std::vector<CGFloat> lo((size_t)cols, 0), hi((size_t)cols, 0);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++) {
            CGFloat least = 0, most = 0;
            MCMeasureCell(cells[(NSUInteger)(row * cols + col)], &least, &most);
            // A point to spare, for the rounding of percentages.
            lo[(size_t)col] = MAX(lo[(size_t)col], least + 1);
            hi[(size_t)col] = MAX(hi[(size_t)col], most + 1);
        }
    const CGFloat room = available - cols * chrome;
    if (room <= 40) return {};
    CGFloat sumLo = 0, sumHi = 0;
    for (int c = 0; c < cols; c++) {
        sumLo += lo[(size_t)c];
        sumHi += hi[(size_t)c];
    }
    std::vector<CGFloat> pct((size_t)cols);
    for (int c = 0; c < cols; c++) {
        const size_t k = (size_t)c;
        CGFloat w;
        if (sumHi <= room)         // everything fits on one line: share out the rest
            w = sumHi > 0 ? hi[k] + (room - sumHi) * hi[k] / sumHi : room / cols;
        else if (sumLo >= room)    // not even the widest pieces fit: scale them all
            w = lo[k] * room / sumLo;
        else
            w = lo[k] + (room - sumLo) * (hi[k] - lo[k]) / (sumHi - sumLo);
        pct[k] = w / available * 100;
    }
    return pct;
}

// A Markdown table as an NSTextTable: every cell is a paragraph of its own
// that wraps inside its column, so a table wider than the pane stays a
// table. Columns are sized to their content by AppKit, the header row is
// shaded, and each column keeps the alignment its separator row gave it.
// The parser's padding and rules (tableCol -1) are for monospace ports and
// are skipped here.
//
// AppKit's sizing reads text and knows nothing of an attachment's width, so
// a table holding typeset formulas gets its column widths from
// MCTableColumnWidths instead: formulas cannot wrap, and AppKit would make
// the columns equal and leave them to be scaled down.
- (void)appendMarkdownTable:(const std::vector<MdRun> &)runs
                       from:(size_t)first
                         to:(size_t)end
                       into:(NSMutableAttributedString *)out {
    const int cols = runs[first].tableCols;
    int rows = 0;
    for (size_t k = first; k < end; k++) rows = std::max(rows, runs[k].tableRow + 1);
    if (cols <= 0 || rows <= 0) return;

    AppSettings *cfg = [AppSettings shared];
    NSDictionary *plain = @{NSFontAttributeName: [NSFont systemFontOfSize:MCZoomed(15)],
                            NSForegroundColorAttributeName: [cfg text:Surface::Editor]};
    if (out.length && ![out.string hasSuffix:@"\n"])
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                    attributes:plain]];

    NSTextTable *table = [[NSTextTable alloc] init];
    table.numberOfColumns = (NSUInteger)cols;
    table.layoutAlgorithm = NSTextTableAutomaticLayoutAlgorithm;
    table.collapsesBorders = YES;
    table.hidesEmptyCells = NO;
    NSColor *border = Hex(0x444444);
    NSColor *headerBackground = MCColor(cfg.settings.markdownCodeBackground());
    const NSTextAlignment alignments[3] = {NSTextAlignmentLeft, NSTextAlignmentCenter,
                                           NSTextAlignmentRight};

    // The cells' contents first, so the columns can be measured.
    NSMutableArray<NSMutableAttributedString *> *contents = [NSMutableArray array];
    std::vector<int> aligns((size_t)(rows * cols), 0);
    BOOL typesetMath = NO;
    NSParagraphStyle *interim = [NSParagraphStyle defaultParagraphStyle];
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
            for (size_t k = first; k < end; k++) {
                const MdRun &r = runs[k];
                if (r.tableRow != row || r.tableCol != col) continue;
                aligns[(size_t)(row * cols + col)] = r.tableAlign;
                NSAttributedString *piece = [self markdownRun:r style:interim inCell:YES];
                if (!piece) continue;
                [text appendAttributedString:piece];
                if (r.math && piece.length == 1 &&
                    [[piece attribute:NSAttachmentAttributeName atIndex:0 effectiveRange:NULL]
                        isKindOfClass:MCMathAttachment.class])
                    typesetMath = YES;
            }
            [contents addObject:text];
        }
    }
    // Each cell pads its text by 8 points a side and has a 1 point border.
    static const CGFloat kCellChrome = 17;
    std::vector<CGFloat> widths;
    if (typesetMath)
        widths = MCTableColumnWidths(contents, rows, cols,
                                     self.textView.textContainer.size.width -
                                         2 * self.textView.textContainer.lineFragmentPadding,
                                     kCellChrome);

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            NSTextTableBlock *cell =
                [[NSTextTableBlock alloc] initWithTable:table startingRow:row rowSpan:1
                                         startingColumn:col columnSpan:1];
            [cell setWidth:1 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder];
            [cell setBorderColor:border];
            [cell setWidth:8 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMinX];
            [cell setWidth:8 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMaxX];
            [cell setWidth:4 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMinY];
            [cell setWidth:4 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMaxY];
            if (row == 0) cell.backgroundColor = headerBackground;
            if (!widths.empty())
                [cell setValue:widths[(size_t)col] type:NSTextBlockPercentageValueType
                  forDimension:NSTextBlockWidth];

            NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
            ps.textBlocks = @[cell];
            ps.lineSpacing = 2.0;
            const int align = aligns[(size_t)(row * cols + col)];
            ps.alignment = alignments[std::min(std::max(align, 0), 2)];
            NSMutableAttributedString *text = contents[(NSUInteger)(row * cols + col)];
            NSMutableDictionary *end = [plain mutableCopy];
            end[NSParagraphStyleAttributeName] = ps;
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                         attributes:end]];
            [text addAttribute:NSParagraphStyleAttributeName value:ps
                         range:NSMakeRange(0, text.length)];
            [text addAttribute:kMarkdownSourceLine value:@(runs[first].line)
                         range:NSMakeRange(0, text.length)];
            [text addAttribute:kMarkdownTableCell value:@[@(row), @(col)]
                         range:NSMakeRange(0, text.length)];
            [out appendAttributedString:text];
        }
    }
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
    _activeTab.message = msg;   // shown again when the tab comes back
}

- (void)setStatus:(NSString *)path {
    self.window.title = [NSString stringWithFormat:@"MiniCode — %@",
                         path.lastPathComponent];
}

@end
