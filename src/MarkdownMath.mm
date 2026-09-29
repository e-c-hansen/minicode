// MarkdownMath.mm — see MarkdownMath.h.
//
// A run, start to end:
//   1. Pages ask for formulas (MCMathTypeset). Requests are collected for a
//      moment and handed to one background run; while it goes, new ones
//      wait for the next.
//   2. The run refuses what MathTex::check refuses, writes MathTex's
//      document to a scratch folder and runs tectonic once, with
//      -Z continue-on-errors so a bad formula does not stop the rest.
//   3. The log says, per formula, whether TeX finished it, what it
//      complained about and its box. The PDF is kept whole, as tectonic
//      wrote it (its formulas share the fonts, so the cheatsheet's 287 take
//      about 160 KB, where a PDF per formula took 7.5 MB), with an index
//      beside it: each good formula's key, page and depth below the
//      baseline.
//   4. If tectonic stopped without a PDF, the formula it was in fails and
//      the others go round again. If it hangs (15 s with no word once the
//      formulas have begun, or 90 s in all), it is killed and the formula
//      it was in fails. If it failed before any formula (not installed
//      properly, or the network is needed for fonts and is not there), the
//      run's formulas fail for a minute and are asked for again after that.
//   5. Results go into memory on the main thread and a notification names
//      them; the preview renders again with the pictures in.
#import "MarkdownMath.h"
#import "Latex.h"
#import <CommonCrypto/CommonDigest.h>
#include "MathTex.h"
#include <string>
#include <vector>

NSNotificationName const MCMathTypesetNotification = @"MCMathTypesetNotification";
NSString *const MCMathKeysKey = @"keys";
NSString *const MCMathErrorKey = @"error";

// A whole run that failed is tried again after this long.
static const NSTimeInterval kRetryAfter = 60;
// Killing a run: silence once TeX is in the formulas, or in all.
static const NSTimeInterval kStallLimit = 15, kRunLimit = 90;
// The disk cache is cut back to kCacheTrimTo once it passes kCacheLimit.
static const unsigned long long kCacheLimit = 50ull << 20, kCacheTrimTo = 40ull << 20;
// PDF points per TeX point.
static const double kBpPerPt = 72.0 / 72.27;
// The first line of a batch's index.
static NSString *const kIndexHeader = @"minicode-math-index 1";

// ------------------------------------------------------------------ formula

// A batch's PDF, whole, as tectonic wrote it. The bytes are held in memory,
// so the file can be trimmed from the cache while its formulas are on show.
@interface MCMathPages : NSObject
- (instancetype)initWithData:(NSData *)data;
@property(nonatomic, readonly) CGPDFDocumentRef doc;
@end

@implementation MCMathPages {
    NSData *_data;
}

- (instancetype)initWithData:(NSData *)data {
    if ((self = [super init])) {
        _data = data;
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        _doc = provider ? CGPDFDocumentCreateWithProvider(provider) : NULL;
        CGDataProviderRelease(provider);
        if (!_doc) return nil;
    }
    return self;
}

- (void)dealloc {
    CGPDFDocumentRelease(_doc);
}

@end

@implementation MCMathFormula {
    MCMathPages *_pages;
    size_t _page;
    NSDate *_retryAfter;     // a failed run's formula: asked for again after this
}

- (instancetype)initWithPages:(MCMathPages *)pages page:(size_t)page descent:(CGFloat)descent {
    CGPDFPageRef p = pages ? CGPDFDocumentGetPage(pages.doc, page) : NULL;
    if (!p) return nil;
    if ((self = [super init])) {
        _pages = pages;
        _page = page;
        _size = CGPDFPageGetBoxRect(p, kCGPDFMediaBox).size;
        _descent = descent;
        _margin = MathTex::kPadPt * kBpPerPt;
    }
    return self;
}

- (instancetype)initWithError:(NSString *)error retryAfter:(NSDate *)retry {
    if ((self = [super init])) {
        _failed = YES;
        _error = [error copy];
        _retryAfter = retry;
    }
    return self;
}

- (BOOL)stale {
    return _retryAfter && _retryAfter.timeIntervalSinceNow <= 0;
}

- (void)drawInRect:(NSRect)rect color:(NSColor *)color flipped:(BOOL)flipped {
    CGPDFPageRef page = _pages ? CGPDFDocumentGetPage(_pages.doc, _page) : NULL;
    if (!page || rect.size.width <= 0 || rect.size.height <= 0) return;
    const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    if (!ctx || box.size.width <= 0 || box.size.height <= 0) return;
    CGContextSaveGState(ctx);
    // TeX draws in black. In a layer of its own, the marks are painted over
    // in the text color (source-in keeps only where there is ink), and the
    // layer then goes onto the page as usual.
    CGContextBeginTransparencyLayerWithRect(ctx, rect, NULL);
    if (flipped) {
        CGContextTranslateCTM(ctx, NSMinX(rect), NSMaxY(rect));
        CGContextScaleCTM(ctx, rect.size.width / box.size.width, -rect.size.height / box.size.height);
    } else {
        CGContextTranslateCTM(ctx, NSMinX(rect), NSMinY(rect));
        CGContextScaleCTM(ctx, rect.size.width / box.size.width, rect.size.height / box.size.height);
    }
    CGContextTranslateCTM(ctx, -box.origin.x, -box.origin.y);
    CGContextDrawPDFPage(ctx, page);
    CGContextSetBlendMode(ctx, kCGBlendModeSourceIn);
    CGContextSetFillColorWithColor(ctx, color.CGColor);
    CGContextFillRect(ctx, box);
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

@end

// -------------------------------------------------------------------- cache

NSString *MCMathKey(NSString *tex, BOOL display) {
    NSString *text = [NSString stringWithFormat:@"v%d\n%c\n%@", MathTex::kVersion,
                                                display ? 'D' : 'I', tex ?: @""];
    NSData *bytes = [text dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, digest);
    NSMutableString *hex = [NSMutableString stringWithCapacity:2 * CC_SHA256_DIGEST_LENGTH];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [hex appendFormat:@"%02x", digest[i]];
    return hex;
}

NSString *MCMathCacheDirectory(void) {
    NSString *over = NSProcessInfo.processInfo.environment[@"MINICODE_MATH_CACHE"];
    if (over.length) return over;
    NSString *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory,
                                                           NSUserDomainMask, YES).firstObject;
    return [caches stringByAppendingPathComponent:@"MiniCode/math"];
}

static NSString *MCMathBatchPath(NSString *batch, NSString *extension) {
    return [MCMathCacheDirectory() stringByAppendingPathComponent:
        [batch stringByAppendingPathExtension:extension]];
}

// Formulas by key, main thread only. What NSCache lets go of comes back
// from the disk.
static NSCache<NSString *, MCMathFormula *> *MCMathMemory(void) {
    static NSCache *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [NSCache new];
        cache.countLimit = 20000;
    });
    return cache;
}

// The disk cache's index, main thread only: key -> @[batch, page, descent].
// Read from every batch's .idx the first time it is needed; a batch written
// later adds its own entries. Batches are named so that newer ones sort
// later, and a newer entry for a key wins.
static NSMutableDictionary<NSString *, NSArray *> *gIndex;

static void MCMathReadIndex(NSString *batch, NSString *text,
                            NSMutableDictionary<NSString *, NSArray *> *into) {
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    if (lines.count == 0 || ![lines[0] isEqualToString:kIndexHeader]) return;
    for (NSUInteger i = 1; i < lines.count; i++) {
        NSArray<NSString *> *f = [lines[i] componentsSeparatedByString:@" "];
        if (f.count != 3 || f[0].length != 64) continue;
        into[f[0]] = @[batch, @(f[1].integerValue), @(f[2].doubleValue)];
    }
}

static NSMutableDictionary<NSString *, NSArray *> *MCMathIndex(void) {
    if (gIndex) return gIndex;
    gIndex = [NSMutableDictionary dictionary];
    NSString *dir = MCMathCacheDirectory();
    NSArray<NSString *> *names = [[[NSFileManager defaultManager]
        contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *name in names) {
        if (![name.pathExtension isEqualToString:@"idx"]) continue;
        NSString *text = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:name]
                                                   encoding:NSUTF8StringEncoding error:nil];
        if (text) MCMathReadIndex(name.stringByDeletingPathExtension, text, gIndex);
    }
    return gIndex;
}

// A batch's PDF, from memory or the disk, or nil when it is gone.
static MCMathPages *MCMathOpenBatch(NSString *batch) {
    static NSCache<NSString *, MCMathPages *> *open;
    static NSMutableSet<NSString *> *touched;
    if (!open) {
        open = [NSCache new];
        open.countLimit = 64;
        touched = [NSMutableSet set];
    }
    MCMathPages *pages = [open objectForKey:batch];
    if (pages) return pages;
    NSString *path = MCMathBatchPath(batch, @"pdf");
    NSData *data = [NSData dataWithContentsOfFile:path];
    pages = data ? [[MCMathPages alloc] initWithData:data] : nil;
    if (!pages) return nil;
    [open setObject:pages forKey:batch];
    // Used today: the trim, oldest first, leaves it alone for a while. Only
    // rewritten once a day, so a page opened again costs no writes.
    if (![touched containsObject:batch]) {
        [touched addObject:batch];
        NSFileManager *fm = [NSFileManager defaultManager];
        NSDate *when = [fm attributesOfItemAtPath:path error:nil].fileModificationDate;
        if (-[when timeIntervalSinceNow] > 86400)
            [fm setAttributes:@{NSFileModificationDate: [NSDate date]} ofItemAtPath:path error:nil];
    }
    return pages;
}

MCMathFormula *MCMathLookup(NSString *tex, BOOL display) {
    NSString *key = MCMathKey(tex, display);
    NSCache *memory = MCMathMemory();
    MCMathFormula *f = [memory objectForKey:key];
    if (f && [f stale]) {
        [memory removeObjectForKey:key];
        f = nil;
    }
    if (f) return f;
    NSMutableDictionary<NSString *, NSArray *> *index = MCMathIndex();
    NSArray *entry = index[key];
    if (!entry) return nil;
    MCMathPages *pages = MCMathOpenBatch(entry[0]);
    f = pages ? [[MCMathFormula alloc] initWithPages:pages page:[entry[1] unsignedIntegerValue]
                                             descent:[entry[2] doubleValue]]
              : nil;
    if (!f) {   // trimmed away, or not readable: typeset it again
        [index removeObjectForKey:key];
        return nil;
    }
    [memory setObject:f forKey:key];
    return f;
}

BOOL MCMathIsReady(NSString *key) {
    MCMathFormula *f = [MCMathMemory() objectForKey:key];
    return f && !f.failed;
}

// Oldest batches first until the folder is under kCacheTrimTo, once it is
// over kCacheLimit. Background.
static void MCMathTrimCache(void) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSURL *dir = [NSURL fileURLWithPath:MCMathCacheDirectory()];
    NSArray<NSURL *> *files = [fm contentsOfDirectoryAtURL:dir
        includingPropertiesForKeys:@[NSURLFileSizeKey, NSURLContentModificationDateKey]
                           options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
    unsigned long long total = 0;
    NSMutableArray<NSURL *> *pdfs = [NSMutableArray array];
    for (NSURL *u in files) {
        NSNumber *size = nil;
        [u getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        total += size.unsignedLongLongValue;
        if ([u.pathExtension isEqualToString:@"pdf"]) [pdfs addObject:u];
    }
    if (total <= kCacheLimit) return;
    [pdfs sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        NSDate *da = nil, *db = nil;
        [a getResourceValue:&da forKey:NSURLContentModificationDateKey error:nil];
        [b getResourceValue:&db forKey:NSURLContentModificationDateKey error:nil];
        return [da ?: NSDate.distantPast compare:db ?: NSDate.distantPast];
    }];
    for (NSURL *pdf in pdfs) {
        if (total <= kCacheTrimTo) break;
        for (NSURL *u in @[pdf, [pdf.URLByDeletingPathExtension URLByAppendingPathExtension:@"idx"]]) {
            NSNumber *size = nil;
            [u getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
            if ([fm removeItemAtURL:u error:nil]) total -= size.unsignedLongLongValue;
        }
    }
}

// ---------------------------------------------------------------------- run

// One formula of a run.
struct MCMathItem {
    NSString *key;
    std::string tex;
    bool display;
};

// A formula typeset in a run: where the index will find it.
struct MCMathPlace {
    NSString *key;
    NSString *batch;
    size_t page;
    double descent;
};

// What one tectonic run made of its formulas.
struct MCMathRun {
    NSMutableDictionary<NSString *, MCMathFormula *> *done;   // typeset or failed
    std::vector<MCMathPlace> placed;                          // the typeset ones
    std::vector<size_t> again;                                // go round again
    NSString *wholeError;                                     // nothing reached TeX
};

static NSString *MCStr(const std::string &s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

// The last "MC:BEGIN:k" in what TeX printed: the formula it was in.
static long MCMathLastBegun(NSData *output) {
    NSString *s = [[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding] ?:
                  [[NSString alloc] initWithData:output encoding:NSISOLatin1StringEncoding];
    NSRange r = [s rangeOfString:@"MC:BEGIN:" options:NSBackwardsSearch];
    if (r.location == NSNotFound) return -1;
    return [s substringFromIndex:NSMaxRange(r)].integerValue;
}

// A name for a batch that sorts after every earlier one.
static NSString *MCMathBatchName(void) {
    static unsigned counter;
    return [NSString stringWithFormat:@"%015.0f-%04u-%08x",
            NSDate.date.timeIntervalSince1970 * 1000, ++counter % 10000, arc4random()];
}

// Runs tectonic once on `items[which]`, in the background.
static MCMathRun MCMathRunOnce(NSString *tool, const std::vector<MCMathItem> &items,
                               const std::vector<size_t> &which) {
    MCMathRun run;
    run.done = [NSMutableDictionary dictionary];
    std::vector<MathTex::Formula> formulas;
    for (size_t i : which) formulas.push_back({items[i].tex, items[i].display});

    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dir = [[NSTemporaryDirectory() stringByAppendingPathComponent:@"MiniCode-math"]
        stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *tex = [dir stringByAppendingPathComponent:@"math.tex"];
    NSString *source = MCStr(MathTex::document(formulas));
    NSError *err = nil;
    if (![source writeToFile:tex atomically:NO encoding:NSUTF8StringEncoding error:&err]) {
        [fm removeItemAtPath:dir error:nil];
        run.wholeError = err.localizedDescription ?: @"The formulas could not be written out.";
        return run;
    }

    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:tool];
    // --print streams TeX's own output, so a run that hangs still says
    // which formula it hung in.
    task.arguments = @[@"-Z", @"continue-on-errors", @"--untrusted", @"--print", @"--keep-logs",
                       @"--chatter", @"minimal", @"--color", @"never", @"--outdir", dir, tex];
    task.currentDirectoryURL = [NSURL fileURLWithPath:dir];
    task.standardInput = [NSFileHandle fileHandleWithNullDevice];
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;

    NSMutableData *output = [NSMutableData data];
    __block NSDate *lastWord = [NSDate date];
    __block BOOL inFormulas = NO, killed = NO;
    NSDate *started = [NSDate date];
    NSObject *lock = [NSObject new];
    if (![task launchAndReturnError:&err]) {
        [fm removeItemAtPath:dir error:nil];
        run.wholeError = err.localizedDescription ?: @"tectonic did not start.";
        return run;
    }
    // A watchdog: a formula that loops for ever must not hold up the rest.
    dispatch_source_t watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(watch, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler(watch, ^{
        BOOL stop;
        @synchronized(lock) {
            stop = !killed && (-started.timeIntervalSinceNow > kRunLimit ||
                               (inFormulas && -lastWord.timeIntervalSinceNow > kStallLimit));
            if (stop) killed = YES;
        }
        if (stop && task.running) [task terminate];
    });
    dispatch_resume(watch);
    NSData *needle = [@"MC:BEGIN:" dataUsingEncoding:NSUTF8StringEncoding];
    NSFileHandle *reader = pipe.fileHandleForReading;
    for (;;) {
        NSData *chunk = [reader availableData];
        if (chunk.length == 0) break;
        @synchronized(lock) {
            [output appendData:chunk];
            lastWord = [NSDate date];
            if (!inFormulas) {
                const NSUInteger from = output.length > chunk.length + 16
                                            ? output.length - chunk.length - 16 : 0;
                inFormulas = [output rangeOfData:needle options:0
                                           range:NSMakeRange(from, output.length - from)]
                                 .location != NSNotFound;
            }
        }
    }
    [task waitUntilExit];
    dispatch_source_cancel(watch);
    BOOL wasKilled;
    @synchronized(lock) { wasKilled = killed; }

    NSData *logData = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"math.log"]];
    NSData *pdfData = wasKilled
        ? nil : [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"math.pdf"]];
    [fm removeItemAtPath:dir error:nil];
    const std::string printed((const char *)output.bytes, output.length);
    const std::string log = logData.length
        ? std::string((const char *)logData.bytes, logData.length) : printed;

    if (wasKilled) {
        // Whatever it was in fails; the rest go round again.
        const long culprit = MCMathLastBegun(output);
        if (culprit < 0 || (size_t)culprit >= which.size()) {
            run.wholeError = @"tectonic did not finish in time.";
            return run;
        }
        for (size_t k = 0; k < which.size(); k++) {
            if ((long)k == culprit)
                run.done[items[which[k]].key] = [[MCMathFormula alloc]
                    initWithError:@"TeX did not finish this formula." retryAfter:nil];
            else
                run.again.push_back(which[k]);
        }
        return run;
    }

    const std::vector<MathTex::Box> boxes = MathTex::readLog(log, which.size());
    MCMathPages *pages = pdfData.length ? [[MCMathPages alloc] initWithData:pdfData] : nil;
    bool anyReached = false;
    for (const MathTex::Box &b : boxes) anyReached = anyReached || b.reached;
    if (!anyReached) {
        NSString *why = MCStr(MathTex::firstError(printed));
        run.wholeError = why.length ? why : @"tectonic did not typeset the formulas.";
        return run;
    }
    NSString *batch = MCMathBatchName();
    NSMutableString *index = [NSMutableString stringWithFormat:@"%@\n", kIndexHeader];
    const size_t pageCount = pages ? CGPDFDocumentGetNumberOfPages(pages.doc) : 0;
    for (size_t k = 0; k < which.size(); k++) {
        const MathTex::Box &b = boxes[k];
        const MCMathItem &item = items[which[k]];
        if (!b.error.empty()) {
            run.done[item.key] = [[MCMathFormula alloc] initWithError:MCStr(b.error) retryAfter:nil];
            continue;
        }
        if (!pages) {
            // tectonic stopped with no PDF: in this formula if TeX began it
            // and never finished, else it is only waiting its turn again.
            if (b.reached && !b.measured) {
                NSString *why = MCStr(MathTex::firstError(printed));
                run.done[item.key] = [[MCMathFormula alloc]
                    initWithError:why.length ? why : @"TeX stopped in this formula." retryAfter:nil];
            } else {
                run.again.push_back(which[k]);
            }
            continue;
        }
        if (!b.reached) {   // TeX never got to it (something ended the run early)
            run.again.push_back(which[k]);
            continue;
        }
        // The page must be the size the log gave the box, or it is not this
        // formula's page.
        CGPDFPageRef page = b.measured && b.page >= 1 && (size_t)b.page <= pageCount
                                ? CGPDFDocumentGetPage(pages.doc, (size_t)b.page) : NULL;
        const CGRect box = page ? CGPDFPageGetBoxRect(page, kCGPDFMediaBox) : CGRectZero;
        const double pad = MathTex::kPadPt;
        const double wantW = (b.width + 2 * pad) * kBpPerPt;
        const double wantH = (b.height + b.depth + 2 * pad) * kBpPerPt;
        if (!page || fabs(box.size.width - wantW) > 0.1 || fabs(box.size.height - wantH) > 0.1 ||
            b.width <= 0) {
            run.done[item.key] = [[MCMathFormula alloc]
                initWithError:(b.width <= 0 && page ? @"The formula came out empty."
                                                    : @"The formula could not be measured.")
                   retryAfter:nil];
            continue;
        }
        const double descent = (b.depth + pad) * kBpPerPt;
        MCMathFormula *f = [[MCMathFormula alloc] initWithPages:pages page:(size_t)b.page
                                                        descent:descent];
        if (!f) {
            run.done[item.key] = [[MCMathFormula alloc]
                initWithError:@"The formula's page could not be read." retryAfter:nil];
            continue;
        }
        run.done[item.key] = f;
        run.placed.push_back({item.key, batch, (size_t)b.page, descent});
        [index appendFormat:@"%@ %d %.4f\n", item.key, b.page, descent];
    }
    // The PDF as tectonic wrote it, and the index to it, for the next time.
    if (!run.placed.empty()) {
        NSString *cacheDir = MCMathCacheDirectory();
        [fm createDirectoryAtPath:cacheDir withIntermediateDirectories:YES attributes:nil error:nil];
        if ([pdfData writeToFile:MCMathBatchPath(batch, @"pdf") atomically:YES])
            [index writeToFile:MCMathBatchPath(batch, @"idx") atomically:YES
                      encoding:NSUTF8StringEncoding error:nil];
    }
    return run;
}

// A whole batch: runs until every formula is settled, or no run settles any.
static NSDictionary<NSString *, MCMathFormula *> *MCMathRunBatch(
        NSString *tool, const std::vector<MCMathItem> &items, NSString **wholeError,
        std::vector<MCMathPlace> *placed) {
    NSMutableDictionary<NSString *, MCMathFormula *> *done = [NSMutableDictionary dictionary];
    std::vector<size_t> todo;
    for (size_t i = 0; i < items.size(); i++) {
        const std::string why = MathTex::check(items[i].tex);
        if (why.empty()) todo.push_back(i);
        else done[items[i].key] = [[MCMathFormula alloc] initWithError:MCStr(why) retryAfter:nil];
    }
    for (int round = 0; !todo.empty() && round < 16; round++) {
        MCMathRun run = MCMathRunOnce(tool, items, todo);
        [done addEntriesFromDictionary:run.done];
        placed->insert(placed->end(), run.placed.begin(), run.placed.end());
        if (run.wholeError || run.done.count == 0) {
            // Nothing settled: try these again in a while.
            NSString *why = run.wholeError ?: @"tectonic did not typeset the formulas.";
            if (wholeError) *wholeError = why;
            NSDate *retry = [NSDate dateWithTimeIntervalSinceNow:kRetryAfter];
            for (size_t i : run.again.empty() ? todo : run.again)
                done[items[i].key] = [[MCMathFormula alloc] initWithError:why retryAfter:retry];
            break;
        }
        todo = run.again;
    }
    MCMathTrimCache();
    return done;
}

// ------------------------------------------------------------------ requests

static NSMutableDictionary<NSString *, NSArray *> *gQueued;   // key -> @[tex, display]
static NSMutableArray<NSString *> *gQueueOrder;
static NSMutableSet<NSString *> *gUnderWay;
static BOOL gRunning, gFlushScheduled;

static void MCMathFlush(void);

static void MCMathScheduleFlush(NSTimeInterval delay) {
    if (gFlushScheduled || gRunning) return;
    gFlushScheduled = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ MCMathFlush(); });
}

void MCMathTypeset(NSArray<NSArray *> *formulas) {
    if (!gQueued) {
        gQueued = [NSMutableDictionary dictionary];
        gQueueOrder = [NSMutableArray array];
        gUnderWay = [NSMutableSet set];
    }
    NSCache *memory = MCMathMemory();
    for (NSArray *f in formulas) {
        if (f.count != 2 || ![f[0] isKindOfClass:NSString.class]) continue;
        NSString *key = MCMathKey(f[0], [f[1] boolValue]);
        MCMathFormula *known = [memory objectForKey:key];
        if ((known && ![known stale]) || gQueued[key] || [gUnderWay containsObject:key]) continue;
        gQueued[key] = f;
        [gQueueOrder addObject:key];
    }
    if (gQueued.count) MCMathScheduleFlush(0.05);
}

static void MCMathFlush(void) {
    gFlushScheduled = NO;
    if (gRunning || gQueued.count == 0) return;
    NSString *tool = MCTectonicPath();
    std::vector<MCMathItem> items;
    NSMutableSet<NSString *> *keys = [NSMutableSet set];
    for (NSString *key in gQueueOrder) {
        NSArray *f = gQueued[key];
        if (!f) continue;
        items.push_back({key, std::string([f[0] UTF8String] ?: ""), (bool)[f[1] boolValue]});
        [keys addObject:key];
    }
    [gQueued removeAllObjects];
    [gQueueOrder removeAllObjects];
    [gUnderWay unionSet:keys];
    gRunning = YES;
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("MiniCode.math", DISPATCH_QUEUE_SERIAL);
    });
    dispatch_async(queue, ^{
        NSString *wholeError = nil;
        std::vector<MCMathPlace> placed;
        NSDictionary<NSString *, MCMathFormula *> *done;
        if (tool) {
            done = MCMathRunBatch(tool, items, &wholeError, &placed);
        } else {
            wholeError = @"tectonic is not installed.";
            NSMutableDictionary *none = [NSMutableDictionary dictionary];
            NSDate *retry = [NSDate dateWithTimeIntervalSinceNow:kRetryAfter];
            for (const MCMathItem &item : items)
                none[item.key] = [[MCMathFormula alloc] initWithError:wholeError retryAfter:retry];
            done = none;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            NSCache *memory = MCMathMemory();
            [done enumerateKeysAndObjectsUsingBlock:^(NSString *key, MCMathFormula *f, BOOL *stop) {
                (void)stop;
                [memory setObject:f forKey:key];
            }];
            if (gIndex)   // once read, the index learns of the new batch here
                for (const MCMathPlace &p : placed)
                    gIndex[p.key] = @[p.batch, @(p.page), @(p.descent)];
            [gUnderWay minusSet:keys];
            gRunning = NO;
            NSMutableDictionary *info = [@{MCMathKeysKey: keys} mutableCopy];
            if (wholeError) info[MCMathErrorKey] = wholeError;
            [[NSNotificationCenter defaultCenter] postNotificationName:MCMathTypesetNotification
                                                                object:nil userInfo:info];
            if (gQueued.count) MCMathScheduleFlush(0.01);
        });
    });
}

// ---------------------------------------------------------------- attachment

@interface MCMathCell : NSTextAttachmentCell
- (instancetype)initWithFormula:(MCMathFormula *)formula scale:(CGFloat)scale
                          color:(NSColor *)color font:(NSFont *)font;
@end

// The cell's frame is the formula's box, stretched to the font's ascender
// and descender where the formula is shorter: TextKit 1 sizes a line from
// its glyphs, and a line that holds only an attachment would otherwise take
// the formula's height alone and put its baseline higher than text's. The
// formula is drawn on the frame's baseline. Its page's margins left and
// right are left out of the frame and drawn past its edges, or every
// formula would stand a little apart from the comma after it.
@implementation MCMathCell {
    MCMathFormula *_formula;
    CGFloat _scale;
    NSColor *_color;
    CGFloat _minAbove, _minBelow;   // the font's ascender and descender
}

- (instancetype)initWithFormula:(MCMathFormula *)formula scale:(CGFloat)scale
                          color:(NSColor *)color font:(NSFont *)font {
    if ((self = [super init])) {
        _formula = formula;
        _scale = scale;
        _color = color;
        _minAbove = font ? ceil(font.ascender) : 0;
        _minBelow = font ? ceil(-font.descender) : 0;
    }
    return self;
}

// The formula's width without its side margins, in PDF points.
- (CGFloat)boxWidth {
    return MAX(_formula.size.width - 2 * _formula.margin, 1);
}

// The frame at scale `s`, its origin's y being the baseline offset.
- (NSRect)frameAtScale:(CGFloat)s {
    const CGFloat below = _formula.descent * s;
    const CGFloat above = _formula.size.height * s - below;
    const CGFloat frameBelow = MAX(below, _minBelow), frameAbove = MAX(above, _minAbove);
    return NSMakeRect(0, -frameBelow, self.boxWidth * s, frameAbove + frameBelow);
}

- (NSSize)cellSize {
    return [self frameAtScale:_scale].size;
}

- (NSPoint)cellBaselineOffset {
    return [self frameAtScale:_scale].origin;
}

// On the baseline, and no wider than a line: a formula too wide for the
// pane is scaled down whole, so all of it stays in view. One that only
// does not fit in what is left of its line keeps its size, and TextKit
// moves it to the next line as it would a long word.
- (NSRect)cellFrameForTextContainer:(NSTextContainer *)textContainer
               proposedLineFragment:(NSRect)lineFrag
                      glyphPosition:(NSPoint)position
                     characterIndex:(NSUInteger)charIndex {
    const CGFloat room = lineFrag.size.width - 2 * textContainer.lineFragmentPadding;
    CGFloat s = _scale;
    if (self.boxWidth * s > room && room > 24) s = room / self.boxWidth;
    return [self frameAtScale:s];
}

- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    if (_formula.size.width <= 0) return;
    // The scale the frame was made at, from its width; then the formula's
    // whole page around it, on the frame's baseline.
    const CGFloat s = frame.size.width / self.boxWidth;
    const NSRect own = [self frameAtScale:s];
    const CGFloat frameBelow = -own.origin.y;
    const CGFloat below = _formula.descent * s, height = _formula.size.height * s;
    NSRect rect = frame;
    rect.origin.x -= _formula.margin * s;
    rect.size.width = _formula.size.width * s;
    rect.size.height = height;
    if (view.isFlipped)
        rect.origin.y = NSMaxY(frame) - frameBelow + below - height;
    else
        rect.origin.y = NSMinY(frame) + frameBelow - below;
    [_formula drawInRect:rect color:_color flipped:view.isFlipped];
}

- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view
       characterIndex:(NSUInteger)charIndex
        layoutManager:(NSLayoutManager *)layoutManager {
    [self drawWithFrame:frame inView:view];
}

// A click, or a double-click that opens the formula's Markdown, belongs to
// the text view.
- (BOOL)wantsToTrackMouse {
    return NO;
}

@end

@implementation MCMathAttachment {
    MCMathCell *_cell;
}

// Setting attachmentCell in init does not stick (it reads back nil), so the
// cell is handed out here, as MCMarkdownImage does.
- (id<NSTextAttachmentCell>)attachmentCell {
    return _cell;
}

- (instancetype)initWithFormula:(MCMathFormula *)formula scale:(CGFloat)scale
                          color:(NSColor *)color font:(NSFont *)font {
    if ((self = [super initWithData:nil ofType:nil])) {
        _formula = formula;
        _cell = [[MCMathCell alloc] initWithFormula:formula scale:scale color:color font:font];
    }
    return self;
}

@end
