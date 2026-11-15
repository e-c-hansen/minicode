// MarkdownMedia.mm — see MarkdownMedia.h.
#import "MarkdownMedia.h"
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>

NSNotificationName const MCMarkdownMediaNotification = @"MCMarkdownMediaNotification";
NSString *const MCMarkdownMediaURLKey = @"url";

// ------------------------------------------------------------ the address

NSURL *MCMarkdownMediaURL(NSString *src, NSString *folder, BOOL web) {
    if (src.length == 0) return nil;
    NSString *lower = src.lowercaseString;
    if ([lower hasPrefix:@"https://"]) return web ? [NSURL URLWithString:src] : nil;
    NSString *path;
    if ([lower hasPrefix:@"file://"]) {
        path = [NSURL URLWithString:src].path;
    } else if ([src containsString:@":"] && ![src hasPrefix:@"/"] && ![src hasPrefix:@"~"] &&
               [src rangeOfString:@":"].location < [src rangeOfString:@"/"].location) {
        return nil;   // http:, data: and other schemes
    } else {
        // A query or fragment dropped, percent-encoding undone (%20).
        NSRange cut = [src rangeOfCharacterFromSet:
            [NSCharacterSet characterSetWithCharactersInString:@"?#"]];
        if (cut.location != NSNotFound) src = [src substringToIndex:cut.location];
        path = (src.stringByRemovingPercentEncoding ?: src).stringByExpandingTildeInPath;
        if (!path.absolutePath && folder) path = [folder stringByAppendingPathComponent:path];
    }
    if (!path) return nil;
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] || dir) return nil;
    return [NSURL fileURLWithPath:path];
}

// ------------------------------------------------------------ the info

@implementation MCMediaInfo
- (instancetype)initPlayable:(BOOL)playable size:(NSSize)size poster:(NSImage *)poster
                     problem:(NSString *)problem {
    if ((self = [super init])) {
        _playable = playable;
        _size = size;
        _poster = poster;
        _problem = [problem copy];
    }
    return self;
}
@end

// An asset for `url`: an https one sends no cookies, as web pictures send
// none.
static AVURLAsset *MCMediaAsset(NSURL *url) {
    NSDictionary *options = url.isFileURL ? @{} : @{AVURLAssetHTTPCookiesKey: @[]};
    return [AVURLAsset URLAssetWithURL:url options:options];
}

static NSMutableDictionary<NSString *, MCMediaInfo *> *gInfo;
static NSMutableSet<NSString *> *gUnderWay;

MCMediaInfo *MCMarkdownMediaInfo(NSURL *url) {
    if (!url) return nil;
    if (!gInfo) {
        gInfo = [NSMutableDictionary dictionary];
        gUnderWay = [NSMutableSet set];
    }
    NSString *key = url.absoluteString;
    if (MCMediaInfo *known = gInfo[key]) return known;
    if ([gUnderWay containsObject:key]) return nil;
    [gUnderWay addObject:key];
    AVURLAsset *asset = MCMediaAsset(url);
    [asset loadValuesAsynchronouslyForKeys:@[@"playable", @"tracks"] completionHandler:^{
        // On AVFoundation's queue: the first frame is read here too.
        NSError *error = nil;
        const BOOL loaded = [asset statusOfValueForKey:@"playable" error:&error] ==
                            AVKeyValueStatusLoaded;
        const BOOL playable = loaded && asset.playable;
        NSSize size = NSZeroSize;
        NSImage *poster = nil;
        AVAssetTrack *track = playable ? [asset tracksWithMediaType:AVMediaTypeVideo].firstObject
                                       : nil;
        if (track) {
            const CGRect r = CGRectApplyAffineTransform(
                CGRectMake(0, 0, track.naturalSize.width, track.naturalSize.height),
                track.preferredTransform);
            size = NSMakeSize(fabs(r.size.width), fabs(r.size.height));
            AVAssetImageGenerator *frames = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
            frames.appliesPreferredTrackTransform = YES;
            frames.maximumSize = CGSizeMake(1600, 1600);
            frames.requestedTimeToleranceAfter = CMTimeMake(1, 2);
            CGImageRef frame = [frames copyCGImageAtTime:CMTimeMake(1, 10) actualTime:NULL error:NULL];
            if (frame) {
                poster = [[NSImage alloc] initWithCGImage:frame size:size];
                CGImageRelease(frame);
            }
        }
        NSString *problem = playable ? nil
                                     : (error.localizedDescription ?: @"This Mac cannot play it.");
        MCMediaInfo *info = [[MCMediaInfo alloc] initPlayable:playable size:size poster:poster
                                                      problem:problem];
        dispatch_async(dispatch_get_main_queue(), ^{
            gInfo[key] = info;
            [gUnderWay removeObject:key];
            [[NSNotificationCenter defaultCenter] postNotificationName:MCMarkdownMediaNotification
                                                                object:nil
                                                              userInfo:@{MCMarkdownMediaURLKey: key}];
        });
    }];
    return nil;
}

// ------------------------------------------------------------ the players

static NSHashTable<MCMarkdownMedia *> *MCPlaying(void) {
    static NSHashTable *table;
    if (!table) table = [NSHashTable weakObjectsHashTable];
    return table;
}

void MCMarkdownMediaStopAll(NSWindow *window) {
    for (MCMarkdownMedia *m in MCPlaying().allObjects)
        if (!window || m.playerView.window == window || !m.playerView.window) [m stop];
}

@interface MCMediaCell : NSTextAttachmentCell
@property(nonatomic, weak) MCMarkdownMedia *owner;
@property(nonatomic, strong) MCMediaInfo *info;
@property(nonatomic) BOOL video;
@property(nonatomic, copy) NSString *name;
@property(nonatomic) NSSize wanted;
@property(nonatomic) CGFloat widthPercent;
@property(nonatomic, weak) NSTextContainer *container;   // where it was laid out
@end

@implementation MCMarkdownMedia {
    MCMediaCell *_cell;
    id _scrollObserver;
}

// Setting attachmentCell in init does not stick (it reads back nil), so the
// cell is handed out here, as MCMarkdownImage does.
- (id<NSTextAttachmentCell>)attachmentCell {
    return _cell;
}

- (instancetype)initWithURL:(NSURL *)url
                      video:(BOOL)video
                       name:(NSString *)name
                       size:(NSSize)size
               widthPercent:(CGFloat)widthPercent {
    if ((self = [super initWithData:nil ofType:nil])) {
        _url = url;
        _video = video;
        _cell = [MCMediaCell new];
        _cell.owner = self;
        _cell.video = video;
        _cell.name = name.length ? name : url.lastPathComponent;
        _cell.wanted = size;
        _cell.widthPercent = widthPercent;
        _cell.info = MCMarkdownMediaInfo(url);
        if (!_cell.info)
            [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(infoArrived:)
                                                         name:MCMarkdownMediaNotification
                                                       object:nil];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    if (_scrollObserver) [[NSNotificationCenter defaultCenter] removeObserver:_scrollObserver];
    [_playerView.player pause];
    [_playerView removeFromSuperview];
}

// Its size and first frame are known: laid out and drawn again where it is.
- (void)infoArrived:(NSNotification *)note {
    if (![note.userInfo[MCMarkdownMediaURLKey] isEqual:_url.absoluteString]) return;
    [[NSNotificationCenter defaultCenter] removeObserver:self name:MCMarkdownMediaNotification
                                                  object:nil];
    _cell.info = MCMarkdownMediaInfo(_url);
    NSLayoutManager *lm = _cell.container.layoutManager;
    NSTextStorage *st = lm.textStorage;
    if (!st) return;
    __block NSUInteger at = NSNotFound;
    [st enumerateAttribute:NSAttachmentAttributeName inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(id v, NSRange r, BOOL *stop) {
        if (v == self) { at = r.location; *stop = YES; }
    }];
    if (at == NSNotFound) return;
    [lm invalidateLayoutForCharacterRange:NSMakeRange(at, 1) actualCharacterRange:NULL];
    [lm invalidateDisplayForCharacterRange:NSMakeRange(at, st.length - at)];
}

- (void)playInView:(NSView *)view frame:(NSRect)frame {
    if (_cell.info && !_cell.info.playable) return;
    if (!_playerView) {
        AVPlayerView *pv = [[AVPlayerView alloc] initWithFrame:frame];
        pv.controlsStyle = AVPlayerViewControlsStyleInline;
        pv.player = [AVPlayer playerWithPlayerItem:[AVPlayerItem playerItemWithAsset:MCMediaAsset(_url)]];
        _playerView = pv;
        [MCPlaying() addObject:self];
        // Scrolled out of view: paused.
        NSClipView *clip = view.enclosingScrollView.contentView;
        if (clip) {
            clip.postsBoundsChangedNotifications = YES;
            __weak MCMarkdownMedia *weakSelf = self;
            __weak NSView *weakView = view;
            _scrollObserver = [[NSNotificationCenter defaultCenter]
                addObserverForName:NSViewBoundsDidChangeNotification object:clip queue:nil
                        usingBlock:^(NSNotification *n) {
                (void)n;
                MCMarkdownMedia *me = weakSelf;
                NSView *v = weakView;
                if (me.playerView && v && !NSIntersectsRect(v.visibleRect, me.playerView.frame))
                    [me.playerView.player pause];
            }];
        }
    }
    _playerView.frame = frame;
    if (_playerView.superview != view) [view addSubview:_playerView];
    [_playerView.player play];
}

- (void)stop {
    if (_scrollObserver) [[NSNotificationCenter defaultCenter] removeObserver:_scrollObserver];
    _scrollObserver = nil;
    [_playerView.player pause];
    _playerView.player = nil;
    [_playerView removeFromSuperview];
    _playerView = nil;
    [MCPlaying() removeObject:self];
}

@end

@implementation MCMediaCell

static const CGFloat kAudioHeight = 36, kAudioWidth = 380;

- (NSSize)cellSize {
    return self.video ? NSMakeSize(480, 270) : NSMakeSize(kAudioWidth, kAudioHeight);
}

- (NSPoint)cellBaselineOffset {
    return NSMakePoint(0, self.video ? 0 : -10);
}

// A video at its frames' size (or as asked), scaled down whole to what is
// left of the line; audio a bar; one that cannot play a bar saying so.
- (NSRect)cellFrameForTextContainer:(NSTextContainer *)textContainer
               proposedLineFragment:(NSRect)lineFrag
                      glyphPosition:(NSPoint)position
                     characterIndex:(NSUInteger)charIndex {
    self.container = textContainer;
    const CGFloat line = MAX(16, lineFrag.size.width - 2 * textContainer.lineFragmentPadding);
    const CGFloat room = MAX(16, line - position.x);
    if (!self.video || (self.info && !self.info.playable))
        return NSMakeRect(0, -10, MIN(room, kAudioWidth), kAudioHeight);
    NSSize own = self.info.size.width > 0 ? self.info.size : NSMakeSize(480, 270);
    CGFloat w = own.width, h = own.height;
    const CGFloat askW = self.widthPercent > 0 ? line * MIN(self.widthPercent, 100.0) / 100
                                               : self.wanted.width;
    if (askW > 0 && self.wanted.height > 0) { w = askW; h = self.wanted.height; }
    else if (askW > 0) { h = h * askW / w; w = askW; }
    else if (self.wanted.height > 0) { w = w * self.wanted.height / h; h = self.wanted.height; }
    if (w > room) { h = h * room / w; w = room; }
    return NSMakeRect(0, 0, round(MAX(w, 1)), round(MAX(h, 1)));
}

- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    MCMarkdownMedia *owner = self.owner;
    if (owner.playerView) {   // the player is over it: keep it there
        if (!NSEqualRects(owner.playerView.frame, frame)) owner.playerView.frame = frame;
        return;
    }
    const BOOL failed = self.info && !self.info.playable;
    NSDictionary *label = @{
        NSFontAttributeName: [NSFont systemFontOfSize:12],
        NSForegroundColorAttributeName: [NSColor colorWithWhite:0.85 alpha:1],
    };
    if (!self.video || failed) {
        NSBezierPath *bar = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(frame, 0.5, 0.5)
                                                            xRadius:8 yRadius:8];
        [[NSColor colorWithWhite:0.5 alpha:0.18] setFill];
        [bar fill];
        [[NSColor colorWithWhite:0.5 alpha:0.4] setStroke];
        [bar stroke];
        CGFloat x = NSMinX(frame) + 12;
        if (!failed) {
            [self drawTriangleIn:NSMakeRect(x, NSMidY(frame) - 7, 12, 14) color:label[NSForegroundColorAttributeName]];
            x += 22;
        }
        NSString *text = failed ? [NSString stringWithFormat:@"%@: cannot play", self.name]
                                : self.name;
        const NSSize ts = [text sizeWithAttributes:label];
        [text drawInRect:NSMakeRect(x, NSMidY(frame) - ts.height / 2, NSMaxX(frame) - x - 8, ts.height)
          withAttributes:label];
        return;
    }
    [[NSColor colorWithWhite:0.08 alpha:1] setFill];
    NSRectFill(frame);
    if (NSImage *poster = owner.poster ?: self.info.poster) {
        [poster drawInRect:frame fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                  fraction:1 respectFlipped:YES
                     hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    }
    // The play button: a dark disc with a white triangle.
    const CGFloat d = MIN(56, MIN(NSWidth(frame), NSHeight(frame)) * 0.5);
    const NSRect disc = NSMakeRect(NSMidX(frame) - d / 2, NSMidY(frame) - d / 2, d, d);
    [[NSColor colorWithWhite:0 alpha:0.55] setFill];
    [[NSBezierPath bezierPathWithOvalInRect:disc] fill];
    [self drawTriangleIn:NSInsetRect(disc, d * 0.32, d * 0.28) color:NSColor.whiteColor];
}

- (void)drawTriangleIn:(NSRect)r color:(NSColor *)color {
    NSBezierPath *t = [NSBezierPath bezierPath];
    [t moveToPoint:NSMakePoint(NSMinX(r) + NSWidth(r) * 0.1, NSMinY(r))];
    [t lineToPoint:NSMakePoint(NSMaxX(r), NSMidY(r))];
    [t lineToPoint:NSMakePoint(NSMinX(r) + NSWidth(r) * 0.1, NSMaxY(r))];
    [t closePath];
    [color setFill];
    [t fill];
}

- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view
       characterIndex:(NSUInteger)charIndex
        layoutManager:(NSLayoutManager *)layoutManager {
    [self drawWithFrame:frame inView:view];
}

// A click plays it.
- (BOOL)wantsToTrackMouse {
    return YES;
}

- (BOOL)wantsToTrackMouseForEvent:(NSEvent *)event inRect:(NSRect)cellFrame
                           ofView:(NSView *)controlView atCharacterIndex:(NSUInteger)charIndex {
    return !(self.info && !self.info.playable);
}

- (BOOL)trackMouse:(NSEvent *)event inRect:(NSRect)cellFrame ofView:(NSView *)controlView
  atCharacterIndex:(NSUInteger)charIndex untilMouseUp:(BOOL)flag {
    [self.owner playInView:controlView frame:cellFrame];
    return YES;
}

- (BOOL)trackMouse:(NSEvent *)event inRect:(NSRect)cellFrame ofView:(NSView *)controlView
      untilMouseUp:(BOOL)flag {
    [self.owner playInView:controlView frame:cellFrame];
    return YES;
}

@end
