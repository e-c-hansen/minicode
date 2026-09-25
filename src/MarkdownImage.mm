// MarkdownImage.mm — see MarkdownImage.h.
#import "MarkdownImage.h"

@interface MCAnimatedImageCell : NSTextAttachmentCell
- (instancetype)initWithPicture:(NSImage *)picture;
@end

static CGRect MCFitPicture(NSImage *picture, CGFloat width) {
    const NSSize s = picture.size;
    if (s.width <= 0 || s.height <= 0) return CGRectZero;
    const CGFloat w = MIN(s.width, MAX(width, 16.0));
    return CGRectMake(0, 0, w, round(s.height * w / s.width));
}

@implementation MCMarkdownImage {
    MCAnimatedImageCell *_cell;
}

// Setting attachmentCell in init does not stick (it reads back nil), so the
// cell is handed out here.
- (id<NSTextAttachmentCell>)attachmentCell {
    return _cell;
}

- (instancetype)initWithPicture:(NSImage *)picture {
    if ((self = [super initWithData:nil ofType:nil])) {
        _picture = picture;
        _cell = [[MCAnimatedImageCell alloc] initWithPicture:picture];
    }
    return self;
}

@end

// TextKit 1, which is what the editor really runs: CodeTextView overrides
// drawRect: (for the squiggles), and AppKit gives any NSTextView subclass that
// does a TextKit 1 layout manager. A cell has no view to animate, so it steps
// through the frames itself: it draws the current one, and a timer moves to
// the next and asks the text view to redraw just the picture. The timer runs
// only while the picture is on screen; drawing it again starts it.
@implementation MCAnimatedImageCell {
    NSBitmapImageRep *_rep;       // nil unless the picture is animated
    NSInteger _frames, _frame;
    NSTimer *_timer;
    __weak NSView *_view;
    NSRect _drawn;                // where it was last drawn, in _view
}

- (instancetype)initWithPicture:(NSImage *)picture {
    if ((self = [super initImageCell:picture])) {
        for (NSImageRep *rep in picture.representations) {
            if (![rep isKindOfClass:NSBitmapImageRep.class]) continue;
            NSInteger n = [[(NSBitmapImageRep *)rep valueForProperty:NSImageFrameCount]
                              integerValue];
            if (n > 1) { _rep = (NSBitmapImageRep *)rep; _frames = n; }
            break;
        }
        // A cached copy of the image would keep showing the first frame.
        if (_rep) picture.cacheMode = NSImageCacheNever;
    }
    return self;
}

- (void)dealloc {
    [_timer invalidate];
}

- (NSSize)cellSize {
    return self.image.size;
}

- (NSRect)cellFrameForTextContainer:(NSTextContainer *)textContainer
               proposedLineFragment:(NSRect)lineFrag
                      glyphPosition:(NSPoint)position
                     characterIndex:(NSUInteger)charIndex {
    const CGFloat room = lineFrag.size.width - 2 * textContainer.lineFragmentPadding;
    return MCFitPicture(self.image, room - position.x);
}

- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    if (_rep) [_rep setProperty:NSImageCurrentFrame withValue:@(_frame)];
    [self.image drawInRect:frame
                  fromRect:NSZeroRect
                 operation:NSCompositingOperationSourceOver
                  fraction:1
            respectFlipped:YES
                     hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    _view = view;
    _drawn = frame;
    if (_rep && !_timer) [self scheduleNextFrame];
}

- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view
       characterIndex:(NSUInteger)charIndex
        layoutManager:(NSLayoutManager *)layoutManager {
    [self drawWithFrame:frame inView:view];
}

- (void)scheduleNextFrame {
    // GIFs that ask for no delay (or a tiny one) play at 10 fps, as browsers do.
    double delay = [[_rep valueForProperty:NSImageCurrentFrameDuration] doubleValue];
    if (delay < 0.02) delay = 0.1;
    __weak MCAnimatedImageCell *weakSelf = self;
    _timer = [NSTimer timerWithTimeInterval:delay repeats:NO block:^(NSTimer *t) {
        (void)t;
        [weakSelf advance];
    }];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)advance {
    _timer = nil;
    NSView *view = _view;
    NSWindow *window = view.window;
    // Off screen, hidden or scrolled away: stop until it is drawn again.
    if (!window || !(window.occlusionState & NSWindowOcclusionStateVisible) ||
        view.hiddenOrHasHiddenAncestor || !NSIntersectsRect(view.visibleRect, _drawn))
        return;
    _frame = (_frame + 1) % _frames;
    [view setNeedsDisplayInRect:_drawn];
}

@end

NSImage *MCMarkdownLoadImage(NSString *src, NSString *folder) {
    if (src.length == 0) return nil;
    NSString *path;
    if ([src hasPrefix:@"file://"]) {
        path = [NSURL URLWithString:src].path;
    } else if ([src containsString:@"://"] || [src hasPrefix:@"data:"]) {
        return nil;   // no network fetches from a preview
    } else {
        // Drop a query or fragment, then undo percent-encoding (%20).
        NSRange cut = [src rangeOfCharacterFromSet:
            [NSCharacterSet characterSetWithCharactersInString:@"?#"]];
        if (cut.location != NSNotFound) src = [src substringToIndex:cut.location];
        path = src.stringByRemovingPercentEncoding ?: src;
        path = path.stringByExpandingTildeInPath;
        if (!path.absolutePath && folder)
            path = [folder stringByAppendingPathComponent:path];
    }
    if (!path) return nil;
    NSDictionary *info = [[NSFileManager defaultManager] attributesOfItemAtPath:path
                                                                          error:nil];
    // Regular files only, and nothing absurd: a preview is not an image viewer.
    if (![info.fileType isEqualToString:NSFileTypeRegular] ||
        info.fileSize > 64ull * 1024 * 1024)
        return nil;
    NSImage *picture = [[NSImage alloc] initWithContentsOfFile:path];
    if (!picture) return nil;
    // Pixels, not points, as the image viewer does: a 144 dpi screenshot would
    // otherwise show at half its size.
    NSImageRep *rep = picture.representations.firstObject;
    if (rep.pixelsWide > 0 && rep.pixelsHigh > 0)
        picture.size = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
    return picture;
}
