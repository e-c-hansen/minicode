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

// Pixels, not points, as the image viewer does: a 144 dpi screenshot would
// otherwise show at half its size. A drawing (SVG, PDF) keeps its own size.
static NSImage *MCPixelSized(NSImage *picture) {
    NSImageRep *rep = picture.representations.firstObject;
    if (rep.pixelsWide > 0 && rep.pixelsHigh > 0)
        picture.size = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
    return picture;
}

// Bytes from outside (the web, a data: URI) as a picture, or nil when
// NSImage cannot decode them. A picture of more than 50 million pixels is
// refused too: a few kilobytes of PNG can claim a size that takes gigabytes
// to draw.
static NSImage *MCPictureFromData(NSData *data) {
    if (data.length == 0) return nil;
    NSImage *picture = [[NSImage alloc] initWithData:data];
    NSImageRep *rep = picture.representations.firstObject;
    if (!rep || picture.size.width <= 0 || picture.size.height <= 0) return nil;
    if ((double)rep.pixelsWide * (double)rep.pixelsHigh > 50e6) return nil;
    return MCPixelSized(picture);
}

static const NSUInteger kMaxWebImageBytes = 20u * 1024 * 1024;

// data:image/png;base64,... Only base64 pictures; anything else is alt text.
static NSImage *MCDataURIImage(NSString *src) {
    NSRange comma = [src rangeOfString:@","];
    if (comma.location == NSNotFound || comma.location > 200) return nil;
    NSString *head = [src substringToIndex:comma.location].lowercaseString;
    if (![head hasPrefix:@"data:image/"] || ![head hasSuffix:@";base64"]) return nil;
    if (src.length - NSMaxRange(comma) > kMaxWebImageBytes / 3 * 4 + 4) return nil;
    NSData *data = [[NSData alloc]
        initWithBase64EncodedString:[src substringFromIndex:NSMaxRange(comma)]
                            options:NSDataBase64DecodingIgnoreUnknownCharacters];
    return MCPictureFromData(data);
}

NSImage *MCMarkdownLoadImage(NSString *src, NSString *folder) {
    if (src.length == 0) return nil;
    NSString *path;
    if ([src hasPrefix:@"file://"]) {
        path = [NSURL URLWithString:src].path;
    } else if ([src.lowercaseString hasPrefix:@"data:"]) {
        return MCDataURIImage(src);
    } else if ([src containsString:@"://"]) {
        return nil;   // the web is MCMarkdownWebImage's
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
    return picture ? MCPixelSized(picture) : nil;
}

// ------------------------------------------------------------ web pictures

NSNotificationName const MCMarkdownWebImageNotification = @"MCMarkdownWebImageNotification";
NSString *const MCMarkdownWebImageURLKey = @"url";
NSString *const MCMarkdownWebImageOKKey = @"ok";
NSString *const MCMarkdownWebImageErrorKey = @"error";

static const NSTimeInterval kFailureMemory = 30;   // seconds before a retry

BOOL MCMarkdownIsWebImage(NSString *src) {
    return src.length > 8 &&
           [src compare:@"https://" options:NSCaseInsensitiveSearch
                  range:NSMakeRange(0, 8)] == NSOrderedSame;
}

// One URL session for the app. Its delegate calls come on a serial queue of
// their own, which alone touches the bytes being received; the bookkeeping of
// what is under way or failed is the main thread's. The cache (NSCache is
// thread-safe) holds the bytes, not an NSImage, so every render gets its own
// picture: an animated cell sets the frame to draw on the picture it holds.
@interface MCWebImageLoader : NSObject <NSURLSessionDataDelegate>
+ (instancetype)shared;
- (NSImage *)pictureFor:(NSString *)src pending:(BOOL *)pending;
- (BOOL)isReady:(NSString *)src;
@end

@implementation MCWebImageLoader {
    NSURLSession *_session;
    NSCache<NSString *, NSData *> *_cache;
    NSMutableSet<NSString *> *_inFlight;                          // main thread
    NSMutableDictionary<NSString *, NSDate *> *_failed;           // main thread
    NSMutableDictionary<NSNumber *, NSMutableData *> *_received;  // delegate queue
    NSMutableDictionary<NSNumber *, NSString *> *_refused;        // delegate queue
}

+ (instancetype)shared {
    static MCWebImageLoader *loader;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ loader = [MCWebImageLoader new]; });
    return loader;
}

- (instancetype)init {
    if ((self = [super init])) {
        _cache = [NSCache new];
        _cache.name = @"MiniCode web images";
        _cache.totalCostLimit = 64 * 1024 * 1024;
        _inFlight = [NSMutableSet set];
        _failed = [NSMutableDictionary dictionary];
        _received = [NSMutableDictionary dictionary];
        _refused = [NSMutableDictionary dictionary];
    }
    return self;
}

// Made on the first fetch: no cookies, no credentials, no URL cache (so
// nothing reaches the disk), and a short User-Agent.
- (NSURLSession *)session {
    if (_session) return _session;
    NSURLSessionConfiguration *c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    c.HTTPCookieStorage = nil;
    c.HTTPShouldSetCookies = NO;
    c.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    c.URLCredentialStorage = nil;
    c.URLCache = nil;
    c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    c.timeoutIntervalForRequest = 15;    // this long without a byte ends it
    c.timeoutIntervalForResource = 30;   // and so does this long in all
    NSString *version = NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
    c.HTTPAdditionalHeaders = @{
        @"User-Agent": version.length ? [@"MiniCode/" stringByAppendingString:version]
                                      : @"MiniCode",
        @"Accept": @"image/*,*/*;q=0.8",
    };
    NSOperationQueue *queue = [NSOperationQueue new];
    queue.maxConcurrentOperationCount = 1;
    queue.name = @"MiniCode web images";
    _session = [NSURLSession sessionWithConfiguration:c delegate:self delegateQueue:queue];
    return _session;
}

- (BOOL)isReady:(NSString *)src {
    return [_cache objectForKey:src] != nil;
}

- (NSImage *)pictureFor:(NSString *)src pending:(BOOL *)pending {
    *pending = NO;
    NSData *data = [_cache objectForKey:src];
    if (data) {
        NSImage *picture = MCPictureFromData(data);
        if (picture) return picture;
    }
    if ([_inFlight containsObject:src]) {
        *pending = YES;
        return nil;
    }
    NSDate *failed = _failed[src];
    if (failed && -failed.timeIntervalSinceNow < kFailureMemory) return nil;
    NSURL *url = [NSURL URLWithString:src];
    if (!url || [url.scheme caseInsensitiveCompare:@"https"] != NSOrderedSame ||
        url.host.length == 0)
        return nil;
    NSURLSessionDataTask *task = [self.session dataTaskWithURL:url];
    task.taskDescription = src;
    [_inFlight addObject:src];
    [task resume];
    *pending = YES;
    return nil;
}

// An error status or a promised length past the cap ends the fetch before
// the body comes.
- (void)URLSession:(NSURLSession *)session
              dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session;
    NSNumber *key = @(task.taskIdentifier);
    NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class]
                           ? ((NSHTTPURLResponse *)response).statusCode : 200;
    NSString *refuse = nil;
    if (status < 200 || status > 299)
        refuse = [NSString stringWithFormat:@"HTTP status %ld", (long)status];
    else if (response.expectedContentLength > (long long)kMaxWebImageBytes)
        refuse = @"too big";
    if (refuse) {
        _refused[key] = refuse;
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    const long long expected = response.expectedContentLength;
    _received[key] = [NSMutableData dataWithCapacity:expected > 0 ? (NSUInteger)expected : 0];
    completionHandler(NSURLSessionResponseAllow);
}

// A server that sends no length, or lies about it, is stopped at the cap.
- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)task
    didReceiveData:(NSData *)data {
    (void)session;
    NSNumber *key = @(task.taskIdentifier);
    NSMutableData *buffer = _received[key];
    if (!buffer) return;
    if (buffer.length + data.length > kMaxWebImageBytes) {
        [_received removeObjectForKey:key];
        _refused[key] = @"too big";
        [task cancel];
        return;
    }
    [buffer appendData:data];
}

// A redirect is followed only to another https address: following one to
// plain http would fetch what an http address in the file may not.
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
    (void)session; (void)task; (void)response;
    const BOOL https = [request.URL.scheme caseInsensitiveCompare:@"https"] == NSOrderedSame;
    completionHandler(https ? request : nil);
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
    (void)session;
    NSNumber *key = @(task.taskIdentifier);
    NSData *data = _received[key];
    NSString *why = _refused[key];
    [_received removeObjectForKey:key];
    [_refused removeObjectForKey:key];
    NSString *src = task.taskDescription;
    if (!why && error) why = error.localizedDescription ?: @"failed";
    if (!why && !MCPictureFromData(data)) why = @"not an image";
    if (!why) [_cache setObject:[data copy] forKey:src cost:data.length];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self finished:src why:why];
    });
}

- (void)finished:(NSString *)src why:(NSString *)why {
    [_inFlight removeObject:src];
    if (why) {
        if (_failed.count > 500) {   // forget the old ones now and then
            for (NSString *old in _failed.allKeys)
                if (-_failed[old].timeIntervalSinceNow >= kFailureMemory)
                    [_failed removeObjectForKey:old];
        }
        _failed[src] = [NSDate date];
    } else {
        [_failed removeObjectForKey:src];
    }
    NSMutableDictionary *info = [NSMutableDictionary dictionary];
    info[MCMarkdownWebImageURLKey] = src;
    info[MCMarkdownWebImageOKKey] = @(why == nil);
    if (why) info[MCMarkdownWebImageErrorKey] = why;
    [[NSNotificationCenter defaultCenter] postNotificationName:MCMarkdownWebImageNotification
                                                        object:nil
                                                      userInfo:info];
}

@end

NSImage *MCMarkdownWebImage(NSString *src, BOOL *pending) {
    BOOL ignored;
    if (!pending) pending = &ignored;
    *pending = NO;
    if (!MCMarkdownIsWebImage(src)) return nil;
    return [[MCWebImageLoader shared] pictureFor:src pending:pending];
}

BOOL MCMarkdownWebImageIsReady(NSString *src) {
    return MCMarkdownIsWebImage(src) && [[MCWebImageLoader shared] isReady:src];
}
