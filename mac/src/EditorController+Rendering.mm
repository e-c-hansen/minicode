// EditorController+Rendering.mm — EditorController: what the editor's slot shows: a file's mode, pictures, PDFs, media, viewers, diffs, LaTeX.
#import "EditorController+Private.h"
#import "MarkdownMedia.h"
#import "EditorFont.h"
#import "PageZoom.h"
#include "FileDetails.h"
#include <objc/runtime.h>
#include "LatexLog.h"

@implementation EditorController (Rendering)

// KVO context for the player item (see observeMediaItem:).
static void *kMediaItemContext = &kMediaItemContext;

// The drawn title bar shows the window's title, which is set in several places.
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                        change:(NSDictionary *)change context:(void *)context {
    if (object == self.window && [keyPath isEqualToString:@"title"])
        self.titleLabel.stringValue = self.window.title ?: @"";
    else if ([self windowTitleObserved:keyPath of:object context:context]) {
        // who has the keyboard changed: EditorController+WindowTitle.mm
    } else if (context == kMediaItemContext) {
        // AVFoundation may report from another thread.
        AVPlayerItem *item = object;
        dispatch_async(dispatch_get_main_queue(), ^{ [self mediaItemChanged:item]; });
    } else
        [super observeValueForKeyPath:keyPath ofObject:object change:change
                              context:context];
}

// A file's diff in the editor's slot, read-only and colored, the way an
// image or a PDF takes it. Opening any file puts the editor back.
// The same file's diff again (after a hunk is staged or discarded) keeps
// its place, so the next hunk is where the eye already is.
- (void)showDiffNamed:(NSString *)name path:(NSString *)path data:(NSData *)diff {
    (void)path;
    const BOOL again = self.isDiff && !self.diffIsCommit && !_activeTab.comparison &&
                       !_activeTab.page && [self.diffName isEqualToString:name];
    const NSPoint place = self.diffScroll.contentView.bounds.origin;
    [self showDiffTitled:name data:diff commit:NO];
    if (!again || place.y <= 0) return;
    NSRect want = self.diffScroll.contentView.bounds;
    want.origin = place;
    [self.diffView.layoutManager ensureLayoutForBoundingRect:NSInsetRect(want, 0, -want.size.height)
                                             inTextContainer:self.diffView.textContainer];
    [self.diffView scrollPoint:place];
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
    for (MCTab *t in [self tabs]) if (t.isDiff && !t.comparison && !t.page) { tab = t; break; }
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
        // The hunk actions are links that keep their own color.
        tv.linkTextAttributes = @{NSCursorAttributeName: [NSCursor pointingHandCursor]};
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
    NSFont *mono = MCMonoFont(12.5, NSFontWeightRegular);
    // The panel's own diff comes with its hunk actions, which the panel runs
    // as the view's delegate.
    NSAttributedString *withActions =
        self.diffIsCommit ? nil : [self.gitPanel diffTextFor:self.diffData font:mono];
    self.diffView.delegate = withActions ? self.gitPanel.diffDelegate : nil;
    [self.diffView.textStorage setAttributedString:
        withActions ?: self.diffIsCommit ? MCGitCommitText(self.diffData, mono)
                                         : MCGitDiffText(self.diffData, mono)];
}

// ------------------------------------------------------------ file rendering
// Forget everything about the previous file's mode. Called before a new file
// is shown, so a stale preview or image, or an editable text view, can never
// survive into a file that shows something else. A text file sets its own
// mode again from its extension, and refreshDisplay makes the view editable.
- (void)resetViewMode {
    [self makeEditorReadOnly];   // a long text on show goes first, while the tab still has it
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
    // The core's table, which the tree's icons read too.
    return FileIcon::isImageExtension(path.pathExtension.lowercaseString.UTF8String ?: "");
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

// Marks a PDF document that still owes its open-at-the-top scroll.
static char kPdfOpensAtTop;

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
    MCPDFAnalyseOnlyScans(self.pdfView, doc);   // no Vision pass over a PDF of text
    self.pdfView.document = doc;
    MCPDFUseOverlayScrollers(self.pdfView);
    if (reload && pageIndex < doc.pageCount) {
        if (!autoScales) self.pdfView.scaleFactor = scale;
        [self.pdfView goToDestination:[[PDFDestination alloc]
            initWithPage:[doc pageAtIndex:pageIndex] atPoint:point]];
    }
    self.isPDF = YES;
    self.currentExt = @"pdf";
    [self setPlainMessage:@""];   // nothing in the text view to save
    [self relayoutRightArea];
    if (!reload) {
        // A new document opens at the top of its first page, now and again
        // after the layout this turn brings (PDFView left a tall page
        // scrolled to its foot). A tab shown again puts back its own place
        // right after this returns, which cancels the second scroll.
        PDFView *v = self.pdfView;
        MCPDFShowFirstPageTop(v);
        objc_setAssociatedObject(doc, &kPdfOpensAtTop, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (v.document == doc && [objc_getAssociatedObject(doc, &kPdfOpensAtTop) boolValue])
                MCPDFShowFirstPageTop(v);
            objc_setAssociatedObject(doc, &kPdfOpensAtTop, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        });
    }
    [self.lsp documentOpened:nil];
    [self updateTitle];
    return YES;
}

- (void)pdfKeepsPlaceGiven {
    PDFDocument *doc = self.pdfView.document;
    if (doc) objc_setAssociatedObject(doc, &kPdfOpensAtTop, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

// Video and audio that AVFoundation plays on this Mac. WebM, Matroska, Ogg
// video, WMV and FLV are not, so they are left to the "Cannot display"
// message. .ts and .mts are TypeScript here, not MPEG transport streams.
+ (BOOL)isMediaPath:(NSString *)path {
    // The core's tables, which the tree's icons read too.
    const std::string ext = path.pathExtension.lowercaseString.UTF8String ?: "";
    return FileIcon::isVideoExtension(ext) || FileIcon::isAudioExtension(ext);
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

// AVPlayerView looks for text, objects, and people in a paused frame
// (allowsVideoFrameAnalysis, VisionKit's Live Text, on by default from
// macOS 13), and a file here opens paused on its first frame, so every
// video shown started that analysis at once, in the app and in
// mediaanalysisd, after the frame was up (the taskbench's MP4).
// It is off until `player` first plays, and on from then, so a frame
// paused while watching can still be read. nil stops watching.
- (void)analyseFramesOncePlayed:(AVPlayer *)player {
    if (@available(macOS 13.0, *)) {
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc removeObserver:self name:AVPlayerRateDidChangeNotification object:nil];
        if (!player) return;
        self.playerView.allowsVideoFrameAnalysis = NO;
        [nc addObserver:self selector:@selector(mediaRateChanged:)
                   name:AVPlayerRateDidChangeNotification object:player];
    }
}

- (void)mediaRateChanged:(NSNotification *)note {
    AVPlayer *player = note.object;
    // AVFoundation may post from another thread.
    __weak EditorController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        EditorController *me = weakSelf;
        if (!me || player != me.playerView.player || player.rate == 0) return;
        if (@available(macOS 13.0, *)) {
            me.playerView.allowsVideoFrameAnalysis = YES;
            [[NSNotificationCenter defaultCenter] removeObserver:me
                                                            name:AVPlayerRateDidChangeNotification
                                                          object:player];
        }
    });
}

// Pause and let go of the player, so nothing keeps playing once another file,
// folder or message takes the slot, or the window closes.
- (void)stopMedia {
    MCMarkdownMediaStopAll(self.window);   // a Markdown page's players too
    [self analyseFramesOncePlayed:nil];
    [self.playerView.player pause];
    [self observeMediaItem:nil];
    // The transport bar goes too: left on the hidden view, it kept the app
    // busy at the display's rate for half a second after another file
    // showed.
    self.playerView.controlsStyle = AVPlayerViewControlsStyleNone;
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
    // The transport bar, now that the item is ready (showMediaAtPath:).
    if (self.playerView.controlsStyle != AVPlayerViewControlsStyleInline)
        self.playerView.controlsStyle = AVPlayerViewControlsStyleInline;
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
        [self analyseFramesOncePlayed:player];   // before the view shows its first frame
        // No transport bar until the item is ready (mediaItemChanged:):
        // AVKit's bar (SwiftUI) given an item still loading animated into
        // its ready state for about 600 ms at a tenth of a core, even for
        // a short local file, and kept the app busy that long; one made
        // once the item is ready starts in that state.
        self.playerView.controlsStyle = AVPlayerViewControlsStyleNone;
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
        // An HTML file's page reads the window's folder; its links open as
        // a rendered Markdown page's do (a file in its tab, a web address
        // in the browser pane).
        _viewers.htmlFolder = ^NSString * { return weakSelf.rootPath; };
        _viewers.onOpenLink = ^(NSURL *url) {
            [weakSelf openMarkdownLink:url.absoluteString ?: @""];
        };
        _viewers.onHtmlRefused = ^(NSURL *url) {
            [weakSelf htmlPageRefused:url];
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
    if (!text || ![self registerViewerUndoFrom:old to:text actionName:name]) return;
    self.sourceText = text;
    [self noteEdited];
    [self updateTitle];
    if (reshow) [self refreshDisplay];   // an undo: the viewer (or the text) shows it
}

// The two texts differ only between a common start and a common end:
// the range in each that differs. Linear, with no copies of either.
static void MCTextSplice(NSString *a, NSString *b, NSRange *inA, NSRange *inB) {
    const CFIndex na = (CFIndex)a.length, nb = (CFIndex)b.length;
    CFStringInlineBuffer ba, bb;
    CFStringInitInlineBuffer((__bridge CFStringRef)a, &ba, CFRangeMake(0, na));
    CFStringInitInlineBuffer((__bridge CFStringRef)b, &bb, CFRangeMake(0, nb));
    CFIndex lead = 0;
    while (lead < na && lead < nb &&
           CFStringGetCharacterFromInlineBuffer(&ba, lead) == CFStringGetCharacterFromInlineBuffer(&bb, lead))
        lead++;
    CFIndex trail = 0;
    while (trail < na - lead && trail < nb - lead &&
           CFStringGetCharacterFromInlineBuffer(&ba, na - 1 - trail) ==
               CFStringGetCharacterFromInlineBuffer(&bb, nb - 1 - trail))
        trail++;
    // Never split a surrogate pair at either edge.
    while (lead > 0 && CFStringIsSurrogateLowCharacter(CFStringGetCharacterFromInlineBuffer(&ba, lead)))
        lead--;
    while (trail > 0 &&
           CFStringIsSurrogateHighCharacter(CFStringGetCharacterFromInlineBuffer(&ba, na - trail - 1)))
        trail--;
    *inA = NSMakeRange((NSUInteger)lead, (NSUInteger)(na - lead - trail));
    *inB = NSMakeRange((NSUInteger)lead, (NSUInteger)(nb - lead - trail));
}

// An undo step from `now` back to `old` on the tab's undo manager, NO when
// they are the same text. The step keeps only the part of `old` that
// differs (a cell edit in a 40 MB CSV keeps a few bytes, not 40 MB), and
// puts it back into the tab's text, which is `now` when it is taken.
- (BOOL)registerViewerUndoFrom:(NSString *)old to:(NSString *)now actionName:(NSString *)name {
    NSRange inOld, inNow;
    MCTextSplice(old, now, &inOld, &inNow);
    if (inOld.length == 0 && inNow.length == 0) return NO;
    NSString *piece = [old substringWithRange:inOld];
    const NSUInteger expected = now.length;
    NSUndoManager *undo = _activeTab.undoManager;
    __weak EditorController *weakSelf = self;
    [undo registerUndoWithTarget:self handler:^(EditorController *target) {
        (void)target;
        EditorController *me = weakSelf;
        NSString *cur = me.sourceText ?: @"";
        if (!me || cur.length != expected) { NSBeep(); return; }   // not the text it was made for
        [me setViewerText:[cur stringByReplacingCharactersInRange:inNow withString:piece]
               actionName:name reshow:YES];
    }];
    [undo setActionName:name];
    return YES;
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

// Show a file: its tab when it has one, else a new kept tab just right of
// the one on show (openFileAtPath:from:, EditorController+Tabs.mm, which
// also opens files in the preview tab).
- (void)openFileAtPath:(NSString *)path {
    [self openFileAtPath:path from:EditorTabs::OpenFrom::Other];
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
    // A document the session's host held (a closed window's, stage 6): its
    // text and undo history are the host's, kept; the file's text counts as
    // saved, so the tab is unsaved where the two differ.
    NSString *held = _activeTab.heldFromHost ? (_activeTab.sourceText ?: @"") : nil;
    _activeTab.heldFromHost = NO;
    _activeTab.restoredBackup = NO;
    _activeTab.loaded = YES;
    _activeTab.gone = NO;
    _activeTab.pendingSort = nil;
    if (!held) [self forgetHistoryOf:_activeTab];   // the text is read afresh
    [self resetViewMode];
    // A window over SSH (stage 8): the file is on the other machine, read
    // through its host as text; the viewers that read the disk themselves
    // (pictures, PDFs, players, Quick Look) are not offered for it yet.
    const BOOL remote = [self sessionIsRemote];
    // Images are routed by extension before any attempt to read them as text.
    if (!remote && [EditorController isImagePath:path] && [self showImageAtPath:path]) {
        [self recordModDate];
        return;
    }
    if (!remote && [path.pathExtension.lowercaseString isEqualToString:@"pdf"] &&
        [self showPDFAtPath:path]) {
        [self recordModDate];
        return;
    }
    if (!remote && [EditorController isMediaPath:path] && [self showMediaAtPath:path]) {
        [self recordModDate];
        return;
    }
    // Binary by its content (or a document only Quick Look shows) is never
    // opened as text; a backup, being text, still is, and in the viewer its
    // kind of file has over its text (a CSV's table, a JSON outline).
    MCViewerKind viewer = remote ? MCViewerNone : MCViewerKindForFile(path);
    if ((restored || held) && MCViewerKindWithoutText(viewer)) viewer = MCViewerNone;
    if (MCViewerKindWithoutText(viewer) && [self showViewer:viewer]) {
        [self recordModDate];
        return;
    }
    // Stat'ed before it is read, so a file still being written when it is
    // read never counts as matching the disk (FolderSearch::readFileStable).
    MCDiskText *disk = remote ? [self sessionRemoteTextOf:_activeTab] : MCReadDiskText(path, ULLONG_MAX);
    NSString *content = disk.text;
    // Over SSH, a file not there yet (the address named a new one, or New
    // File made its tab): empty, untitled at its path, and made there by
    // its first save, through the host (+Remote).
    if (!content && remote && !restored && !held && [self remotePathIsNew:path]) {
        content = @"";
        _activeTab.untitled = YES;
    }
    // The file is gone, or no longer reads as text: the backup (or the
    // host's text) is all there is.
    if (!content && (restored || held)) content = @"";
    // Not UTF-8, nor UTF-16 that decodes exactly: Quick Look shows what
    // it can (text in another encoding, or the file's icon).
    if (!content && !remote && [[NSFileManager defaultManager] fileExistsAtPath:path] &&
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
        return;
    }
    NSString *ext = path.pathExtension.lowercaseString;
    self.sourceText = held ?: content;   // the host's own text: nothing to tell it
    self.currentExt = ext;
    self.isMarkdown = [ext isEqualToString:@"md"] ||
                      [ext isEqualToString:@"markdown"];
    self.isLatex = [ext isEqualToString:@"tex"] || [ext isEqualToString:@"ltx"] ||
                   [ext isEqualToString:@"latex"];
    // Markdown and LaTeX both open in their preview, unless the file is
    // empty: a blank preview of a new file looks like nothing opened.
    BOOL blank = [(held ?: content) stringByTrimmingCharactersInSet:
                     NSCharacterSet.whitespaceAndNewlineCharacterSet].length == 0;
    // A CSV opens as its table, the text a Shift+Cmd+V away; a JSON file
    // gets an outline beside its text.
    if (MCViewerKindOverText(viewer) || MCViewerKindBesideText(viewer) ||
        MCViewerKindRendersMarkdown(viewer))
        self.viewerKind = viewer;
    self.previewMode = (self.isMarkdown || self.isLatex || MCViewerKindOverText(viewer) ||
                        MCViewerKindRendersMarkdown(viewer)) && !blank;
    self.savedText = content;
    self.dirty = held && (!disk.text || ![held isEqualToString:content]);
    // A held buffer counts unsaved against the file as it is now, on the
    // host too (stage 7: the window's dot follows the host's count).
    if (held && disk.text) [self sessionTab:_activeTab savedAs:content];
    // A held file gone from disk says so; an untitled buffer (MCTab
    // untitled) never had one.
    if (held && !disk.text && !_activeTab.untitled && ![[NSFileManager defaultManager] fileExistsAtPath:path])
        _activeTab.gone = YES;
    // How the file is written, as read; a restored buffer whose file is
    // gone keeps what its backup said (takeBackup:intoTab:).
    if (disk.text) {
        _activeTab.bom = disk.bom;
        _activeTab.encoding = disk.encoding;
    }
    [self recordStamp:disk.stamp ofTab:_activeTab];
    if (restored) {
        [self showRestoredText:restored base:restoredBase inode:restoredInode];
        // The session's host counts the backup's text unsaved too (against
        // the file's, "" for a file not on disk), so a headless host keeps
        // it and backs it up if the window leaves it there.
        if (self.dirty) [self sessionRestoredBackupOf:_activeTab saved:self.savedText ?: @""];
    }
    [self noteLargeFileText:self.sourceText];   // large-file mode, before it shows
    [self refreshDisplay];
    // Previews show rendered text, which no language server should see, nor
    // does a file in large-file mode.
    // Nor does a file on another machine: its servers are stage 9's.
    [self.lsp documentOpened:self.previewMode || remote ? nil : [self languageServerPath:path]];
    [self updateTitle];
    [self applySessionPlaceOf:_activeTab];   // brought back with its window: where it was
}

// The on-disk modification time of a file (a symlink's target's), or nil.
- (NSDate *)diskModDateFor:(NSString *)path {
    return MCStampOfFile(path).modDate;
}
// What the tab knows of its file on disk: when it was last changed, its
// inode, so a rename made outside can be followed, and its size. Symlinks
// are followed (attributesOfItemAtPath: did not, so a tab on a link never
// saw its file change).
- (void)recordDiskStateOf:(MCTab *)t {
    [self recordStamp:MCStampOfFile(t.currentPath) ofTab:t];
}
- (void)recordStamp:(MCDiskStamp *)s ofTab:(MCTab *)t {
    if (!t) return;
    // A window over SSH keeps no stamps: its files are on the other machine,
    // and a stamp of this Mac's file at that path would be another file's.
    // With none, the disk checks pass it by (checkExternalChange, the
    // questions before a save).
    if ([self sessionIsRemote]) s = nil;
    t.fileModDate = s.modDate;
    t.fileID = s.fileID;
    t.fileSize = s ? (int64_t)s.size : -1;
    t.tooLargeToFollow = NO;
    if (t == _activeTab) [self updateSavedStatus];
    // The file as now on disk is the tab's: an HTML page on show follows it.
    if (t == _activeTab && self.previewMode) [_viewers htmlFileMayHaveChanged];
}
- (void)recordModDate { [self recordDiskStateOf:_activeTab]; }

// Whether the file is not as the tab last saw it: another date, inode or
// size, or gone. Unknowns (no inode, no size, from a backup) are not
// compared.
- (BOOL)tab:(MCTab *)t changedOnDisk:(MCDiskStamp *)s {
    if (!s || !t.fileModDate) return s != nil || t.fileModDate != nil;
    if (![s.modDate isEqualToDate:t.fileModDate]) return YES;
    if (t.fileID && s.fileID != t.fileID) return YES;
    return t.fileSize >= 0 && (int64_t)s.size != t.fileSize;
}

// The tab's text was replaced from disk. Its undo steps were made for the
// old text, and taken now they would edit the new text at the old places
// (or throw past its end), so they go, and so do the LaTeX preview's.
- (void)forgetHistoryOf:(MCTab *)t {
    [t.undoManager removeAllActions];
    [self sessionForgetHistoryOf:t];   // the session host's too
    t.textSessionStart = nil;
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

// Decide what to show for the current file/mode.
- (void)refreshDisplay {
    self.showingMessage = NO;
    self.textView.buildMarks = nil;   // the LaTeX preview's, for the tab that had them
    if (self.isLatex && self.previewMode) {
        [self showLatexPreview];
        return;
    }
    // A viewer over the text (a CSV table) holds the slot in preview mode;
    // the hidden text view holds nothing, and saving writes the tab's text.
    if (self.previewMode && MCViewerKindOverText(self.viewerKind)) {
        if (self.latex) self.latex.hidden = YES;
        [self makeEditorReadOnly];
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
        [self makeEditorReadOnly];   // a long source goes first; the page's layout is contiguous
        [self renderMarkdown:[self markdownToRender]];
    } else {
        [self displaySourceEditable];
        [self showLatexProblemMarks];
        [self refreshSidePreview];   // a Markdown tab's page beside it, if it wants one
        [self refreshPageSide];      // a LaTeX or HTML tab's
        // A CSV's text on show: where its typing started (togglePreview:).
        if (MCViewerKindOverText(self.viewerKind) && !_activeTab.textSessionStart)
            _activeTab.textSessionStart = self.sourceText ?: @"";
        // The JSON outline, beside the text it follows.
        if (MCViewerKindBesideText(self.viewerKind)) {
            [self.viewers showBesideTextView:self.textView kind:self.viewerKind
                                          path:self.currentPath];
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
        [self makeLatexPreview];
    } else {
        [self.latex setPath:self.currentPath source:self.sourceText ?: @""];
        [self.latex refreshStatus];   // an \input file saved or edited meanwhile
    }
    [self relayoutRightArea];
    [self.window makeFirstResponder:self.latex];
}

// The active tab's preview, made: edits made in it come back as typing,
// and its problems go to the gutter and, clicked, to their lines.
- (void)makeLatexPreview {
    LatexView *view = [[LatexView alloc] initWithPath:self.currentPath
                                               source:self.sourceText ?: @""];
    self.latex = view;
    MCTab *tab = _activeTab;
    __weak EditorController *weakSelf = self;
    __weak MCTab *weakTab = tab;
    view.onSourceEdited = ^(NSString *newSource) {
        [weakSelf latexDidEditSource:newSource];
    };
    view.onProblemsChanged = ^{
        EditorController *me = weakSelf;
        if (me && me->_activeTab == weakTab && weakTab) [me showLatexProblemMarks];
    };
    view.onOpenProblem = ^(NSString *path, NSInteger line) {
        [weakSelf openLatexProblemAt:path line:line];
    };
    // Command-click on the page: the source line behind it.
    view.onShowSource = ^(NSString *path, NSInteger line, NSInteger column) {
        [weakSelf latexShowSource:path line:line column:column];
    };
    // Beside the source, the page typesets on save: a double-click on a
    // page older than the buffer typesets the buffer first.
    view.bufferText = ^NSString * {
        EditorController *me = weakSelf;
        return weakTab ? [me pageSideBufferFor:weakTab] : nil;
    };
    // The tab's text wherever it is now, for the edit box's Apply: an
    // agent's rewrite read in while the box was open is kept, never
    // spliced at the old offsets or put back to the old text.
    view.sourceNow = ^NSString * {
        EditorController *me = weakSelf;
        MCTab *t = weakTab;
        if (!me || !t) return nil;
        if (t == me->_activeTab && me.textView.editable && !t.showingMessage)
            return [me.textView.string copy];
        return t.sourceText;
    };
    [self wireLatexInputsOf:view];
    [self wireSessionTypesetOf:view];   // its pages on the window's page in a browser
    [self.rightArea addSubview:view];
}

// A problem's row in the failure panel: its file at its line, the source
// shown (goToLine: leaves the preview).
- (void)openLatexProblemAt:(NSString *)path line:(NSInteger)line {
    if (!path.length) return;
    if (![path isEqualToString:self.currentPath]) {
        [self openFileAtPath:path];
        if (![self.currentPath isEqualToString:path]) return;   // refused
    }
    [self goToLine:(int)line column:0];
}

// The last typeset's problems as dots in the gutter, while the source on
// show is the text that failed (they then follow its edits).
- (void)showLatexProblemMarks {
    if (!self.isLatex || self.previewMode || !self.latex) {
        self.textView.buildMarks = nil;
        return;
    }
    NSString *text = self.textView.string;
    NSArray<MCLatexProblem *> *problems = [self.latex problemsForPath:self.currentPath
                                                                source:text];
    NSMutableArray<MCDiagnosticMark *> *marks = [NSMutableArray array];
    for (MCLatexProblem *p in problems) {
        const NSUInteger start = [self characterAtStartOfLine:p.line - 1 inString:text];
        if ([self lineOfCharacter:start inString:text] != p.line - 1) continue;   // past the end
        const NSRange line = [text lineRangeForRange:NSMakeRange(start, 0)];
        MCDiagnosticMark *m = [MCDiagnosticMark new];
        m.range = NSMakeRange(start, NSMaxRange(line) - start);
        m.severity = p.severity;
        m.message = p.message;
        [marks addObject:m];
    }
    self.textView.buildMarks = marks;
}

// Option+Cmd+J, LaTeX Workshop's SyncTeX from Cursor: the preview, at what
// the caret's line put on the page.
- (BOOL)canShowLineInPDF {
    return self.isLatex && self.currentPath != nil && !self.showingMessage;
}

- (void)showLineInPDF:(id)sender {
    (void)sender;
    if (!self.canShowLineInPDF) { NSBeep(); return; }
    NSString *text = self.textView.string;
    const NSInteger line = [self lineOfCharacter:self.textView.selectedRange.location
                                        inString:text] + 1;
    // A part of a larger document (\input, \include) shows the line in its
    // main document's preview, in that document's tab.
    NSString *part = self.currentPath;
    NSString *main = self.previewMode ? nil : [self latexMainDocumentOf:part text:text];
    if (main) {
        if (![self showFile:main]) { NSBeep(); return; }
    }
    // The page beside the source shows it there, the source keeping the
    // keyboard.
    if (!main && [self pageSideShowsLatex]) {
        [self.latex revealSourceLine:line];
        return;
    }
    if (!self.previewMode) [self togglePreview:nil];
    else [self revealEditor];
    if (!self.latex) return;
    [self.window makeFirstResponder:self.latex];
    if (main) [self.latex revealSourceLine:line inFile:part];
    else [self.latex revealSourceLine:line];
}

// Inverse search, Command-click on the page (or Show in Source): the
// source at the clicked word's line, lit for a moment (goToLine:, which
// leaves the full preview, and which Go Back undoes). `path` nil is the
// document itself; an \input file opens in its tab.
- (void)latexShowSource:(NSString *)path line:(NSInteger)line column:(NSInteger)column {
    if (line <= 0) return;
    if (path.length && ![path isEqualToString:self.currentPath]) {
        if (![self showFile:path]) return;
        if (![self.currentPath isEqualToString:path]) return;   // refused
    }
    [self goToLine:(int)line column:(int)MAX(0, column)];
}

// The document a .tex file is part of, for forward search: the one a
// "% !TEX root = main.tex" comment names (LaTeX Workshop's convention),
// else, for a file that is no document of its own, the open preview whose
// last typeset read it. nil when the file is its own document.
- (NSString *)latexMainDocumentOf:(NSString *)path text:(NSString *)text {
    if (!path.length) return nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    const std::string written = LatexLog::rootComment(text.UTF8String ?: "");
    if (!written.empty()) {
        NSString *root = @(LatexLog::resolveRoot(path.UTF8String, written).c_str());
        BOOL dir = NO;
        if (![fm fileExistsAtPath:root isDirectory:&dir] || dir) {
            NSString *withTex = [root stringByAppendingPathExtension:@"tex"];
            if ([fm fileExistsAtPath:withTex isDirectory:&dir] && !dir) root = withTex;
        }
        char a[PATH_MAX], b[PATH_MAX];
        const BOOL same = realpath(root.fileSystemRepresentation, a) &&
                          realpath(path.fileSystemRepresentation, b) && strcmp(a, b) == 0;
        if (!same && [fm fileExistsAtPath:root isDirectory:&dir] && !dir) return root;
    }
    if ([text rangeOfString:@"\\documentclass"].location != NSNotFound ||
        [text rangeOfString:@"\\begin{document}"].location != NSNotFound)
        return nil;
    for (MCTab *t in [self tabs])
        if (t != _activeTab && t.latex && t.currentPath && [t.latex typesetReadFile:path])
            return t.currentPath;
    return nil;
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
        [self makeLatexPreview];
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
    return self.latex != nil && !self.latex.isHidden && self.previewMode;
}

// An edit made in the PDF preview leaves the buffer dirty, like any typing.
// Made on the page beside the source, it goes into the source on show as
// one undo step there.
- (void)latexDidEditSource:(NSString *)newSource {
    if ([self pageSideShowsLatex] && self.textView.editable) {
        if (![newSource isEqualToString:self.textView.string])
            [self putBackText:newSource inTab:_activeTab actionName:@"Edit in Preview"];
        return;
    }
    if ([newSource isEqualToString:self.sourceText]) return;
    self.sourceText = newSource;
    [self noteEdited];
}

- (void)setPlainMessage:(NSString *)msg {
    NSDictionary *a = @{
        NSFontAttributeName: [NSFont systemFontOfSize:14],
        NSForegroundColorAttributeName: Hex(0x9D9D9D),
    };
    _liveHighlight = NO;
    [self.textView.textStorage setAttributedString:
        [[NSAttributedString alloc] initWithString:msg attributes:a]];
    [self applyMarkdownMeasure];   // a page's centered inset goes with it
    self.showingMessage = YES;
    _activeTab.message = msg;   // shown again when the tab comes back
}

@end
