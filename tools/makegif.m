// makegif.m — turn the frames a demo scene recorded into an animated GIF.
//
//   makegif <frames-dir> <out.gif> [--width 960] [--poster out.png] [--hold 2.5]
//
// <frames-dir> is what MINICODE_DEMO writes (see src/Demo.mm): PNG window
// captures plus manifest.txt, which gives each frame's time and any windows
// in front of it. A child window (a popover) is already in the main capture,
// which grows to take it in, so only its position is given; another window
// (the color panel) is captured apart and laid over the frame:
//
//   frame <ms> <file> <width-pt> <height-pt>
//   child <dx-pt> <dy-pt> <width-pt> <height-pt>
//   over <file> <dx-pt> <dy-pt> <width-pt> <height-pt>
//   poster <ms>
//   end <ms>
//
// ImageIO reads the PNGs and writes the poster, CoreGraphics composites and
// scales. The GIF itself is encoded here rather than by ImageIO, whose GIF
// writer stores every frame whole and clears transparent pixels to the
// background (disposal 2), so frames cannot build on each other. A screen
// recording is mostly the same pixels from frame to frame, and only an
// encoder that knows that makes a small file. So: one palette for the
// whole animation (median cut, no dithering, which suits flat UI colors),
// identical frames merged into one longer frame, and each remaining frame
// cut down to the rectangle that changed, with unchanged pixels inside it left
// transparent so they compress to almost nothing.
//
// Build: make build/makegif
#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface Frame : NSObject
@property(nonatomic) long ms;
@property(nonatomic, copy) NSString *file;
@property(nonatomic) double w, h;
// Windows in front: @[file, dx, dy, w, h], in points from the main
// window's top-left corner, back to front.
@property(nonatomic, strong) NSMutableArray<NSArray *> *overs;
// How far child windows stick out past the main window, in points.
@property(nonatomic) BOOL childLeft, childAbove;
@end
@implementation Frame
@end

static void Die(NSString *msg) {
    fprintf(stderr, "makegif: %s\n", msg.UTF8String);
    exit(1);
}

static CGImageRef LoadPNG(NSString *path) {
    CGImageSourceRef src = CGImageSourceCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    if (!src) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    return img;
}

// ------------------------------------------------------------- compositing

// One frame, composited and scaled to W x H, as straight (not premultiplied)
// RGBA. Pixels under half covered become fully transparent (the window's
// rounded corners); the rest become opaque, since GIF has no partial alpha.
static uint8_t *Render(Frame *f, NSString *dir, int W, int H) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    uint8_t *px = calloc((size_t)W * H, 4);
    CGContextRef ctx = CGBitmapContextCreate(px, W, H, 8, (size_t)W * 4, cs,
        (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    double k = W / f.w;   // pixels per point in the output

    CGImageRef main = LoadPNG([dir stringByAppendingPathComponent:f.file]);
    if (!main) Die([NSString stringWithFormat:@"cannot read %@", f.file]);
    // With a popover poking out past the window, the capture is that much
    // bigger; cut the window back out. The pixel scale comes from the side
    // nothing sticks out of.
    size_t iw = CGImageGetWidth(main), ih = CGImageGetHeight(main);
    double scale = f.childLeft ? ih / f.h : iw / f.w;
    if (f.childLeft && f.childAbove) scale = MIN(iw / f.w, ih / f.h);
    size_t mw = (size_t)lround(f.w * scale), mh = (size_t)lround(f.h * scale);
    if (iw > mw || ih > mh) {
        size_t x = f.childLeft && iw > mw ? iw - mw : 0;
        size_t y = f.childAbove && ih > mh ? ih - mh : 0;
        CGImageRef cut = CGImageCreateWithImageInRect(main,
            CGRectMake(x, y, MIN(mw, iw - x), MIN(mh, ih - y)));
        CGImageRelease(main);
        main = cut;
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, W, H), main);
    CGImageRelease(main);

    for (NSArray *o in f.overs) {
        CGImageRef img = LoadPNG([dir stringByAppendingPathComponent:o[0]]);
        if (!img) continue;
        double dx = [o[1] doubleValue], dy = [o[2] doubleValue];
        double ow = [o[3] doubleValue], oh = [o[4] doubleValue];
        CGRect r = CGRectMake(dx * k, H - (dy + oh) * k, ow * k, oh * k);
        CGContextDrawImage(ctx, r, img);
        CGImageRelease(img);
    }
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);

    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint8_t *p = px + i * 4;
        if (p[3] < 128) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
        if (p[3] < 255) {
            for (int c = 0; c < 3; c++) p[c] = (uint8_t)MIN(255, p[c] * 255 / p[3]);
            p[3] = 255;
        }
    }
    return px;
}

static void WritePNG(const uint8_t *px, int W, int H, NSString *path) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate((void *)px, W, H, 8, (size_t)W * 4, cs,
        (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path],
        (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    if (!CGImageDestinationFinalize(d)) Die(@"cannot write the poster");
    CFRelease(d);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

// ---------------------------------------------------------------- palette

// Colors are binned at 5 bits per channel; each bin keeps the sum of the
// exact colors in it, so palette entries are true averages.
typedef struct { double n, r, g, b; } Bin;
static inline int Key(const uint8_t *p) {
    return ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3);
}

typedef struct { int lo[3], hi[3]; double n; } Box;

static void Shrink(Box *b, const Bin *bins) {
    int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
    double n = 0;
    for (int r = b->lo[0]; r <= b->hi[0]; r++)
    for (int g = b->lo[1]; g <= b->hi[1]; g++)
    for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
        const Bin *x = &bins[(r << 10) | (g << 5) | bl];
        if (x->n <= 0) continue;
        n += x->n;
        int v[3] = {r, g, bl};
        for (int c = 0; c < 3; c++) { lo[c] = MIN(lo[c], v[c]); hi[c] = MAX(hi[c], v[c]); }
    }
    b->n = n;
    if (n > 0) for (int c = 0; c < 3; c++) { b->lo[c] = lo[c]; b->hi[c] = hi[c]; }
}

// Median cut. The box split next is the one with the widest color range,
// weighted by the square root of its pixel count: plain count-weighting would
// spend the palette on shades of the big flat backgrounds and leave the
// anti-aliased edges of text with too few colors.
static int BuildPalette(const Bin *bins, int maxColors, uint8_t pal[][3]) {
    Box boxes[256];
    int count = 1;
    boxes[0] = (Box){{0, 0, 0}, {31, 31, 31}, 0};
    Shrink(&boxes[0], bins);
    while (count < maxColors) {
        int best = -1, axis = 0;
        double score = 0;
        for (int i = 0; i < count; i++) {
            for (int c = 0; c < 3; c++) {
                int range = boxes[i].hi[c] - boxes[i].lo[c];
                if (range == 0) continue;
                double s = range * sqrt(boxes[i].n);
                if (s > score) { score = s; best = i; axis = c; }
            }
        }
        if (best < 0) break;   // every box is a single bin
        Box *b = &boxes[best];
        // Weighted median along the axis.
        double half = b->n / 2, acc = 0;
        int cut = b->lo[axis];
        for (int v = b->lo[axis]; v < b->hi[axis]; v++) {
            for (int r = b->lo[0]; r <= b->hi[0]; r++)
            for (int g = b->lo[1]; g <= b->hi[1]; g++)
            for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
                int p[3] = {r, g, bl};
                if (p[axis] != v) continue;
                acc += bins[(r << 10) | (g << 5) | bl].n;
            }
            cut = v;
            if (acc >= half) break;
        }
        Box a = *b, c2 = *b;
        a.hi[axis] = cut;
        c2.lo[axis] = cut + 1;
        Shrink(&a, bins);
        Shrink(&c2, bins);
        *b = a;
        boxes[count++] = c2;
    }
    for (int i = 0; i < count; i++) {
        double n = 0, r = 0, g = 0, bl = 0;
        for (int x = boxes[i].lo[0]; x <= boxes[i].hi[0]; x++)
        for (int y = boxes[i].lo[1]; y <= boxes[i].hi[1]; y++)
        for (int z = boxes[i].lo[2]; z <= boxes[i].hi[2]; z++) {
            const Bin *q = &bins[(x << 10) | (y << 5) | z];
            n += q->n; r += q->r; g += q->g; bl += q->b;
        }
        if (n <= 0) n = 1;
        pal[i][0] = (uint8_t)lround(r / n);
        pal[i][1] = (uint8_t)lround(g / n);
        pal[i][2] = (uint8_t)lround(bl / n);
    }
    return count;
}

// ------------------------------------------------------------ GIF writing

typedef struct { uint8_t *d; size_t len, cap; } Buf;
static void Put(Buf *b, uint8_t v) {
    if (b->len == b->cap) { b->cap = b->cap ? b->cap * 2 : 1 << 16; b->d = realloc(b->d, b->cap); }
    b->d[b->len++] = v;
}
static void Put16(Buf *b, int v) { Put(b, v & 0xFF); Put(b, (v >> 8) & 0xFF); }

// LZW-compressed image data in 255-byte sub-blocks.
typedef struct { Buf *out; uint8_t block[255]; int blockLen; uint32_t bits; int nbits; } Packer;
static void PackByte(Packer *p, uint8_t v) {
    p->block[p->blockLen++] = v;
    if (p->blockLen == 255) {
        Put(p->out, 255);
        for (int i = 0; i < 255; i++) Put(p->out, p->block[i]);
        p->blockLen = 0;
    }
}
static void PackCode(Packer *p, int code, int size) {
    p->bits |= (uint32_t)code << p->nbits;
    p->nbits += size;
    while (p->nbits >= 8) { PackByte(p, p->bits & 0xFF); p->bits >>= 8; p->nbits -= 8; }
}

static void Lzw(Buf *out, const uint8_t *px, size_t n) {
    const int minSize = 8, clear = 256, eoi = 257;
    static uint16_t dict[4096 * 256];   // dict[code * 256 + byte] = code, 0 = none
    memset(dict, 0, sizeof dict);
    Put(out, minSize);
    Packer p = {out, {0}, 0, 0, 0};
    int size = minSize + 1, next = eoi + 1;
    PackCode(&p, clear, size);
    int cur = px[0];
    for (size_t i = 1; i < n; i++) {
        int k = px[i];
        uint16_t *slot = &dict[cur * 256 + k];
        if (*slot) { cur = *slot; continue; }
        PackCode(&p, cur, size);
        if (next < 4096) {
            if (next == (1 << size)) size++;
            *slot = (uint16_t)next++;
        } else {
            PackCode(&p, clear, size);
            memset(dict, 0, sizeof dict);
            size = minSize + 1;
            next = eoi + 1;
        }
        cur = k;
    }
    PackCode(&p, cur, size);
    PackCode(&p, eoi, size);
    if (p.nbits > 0) PackByte(&p, p.bits & 0xFF);
    if (p.blockLen) {
        Put(out, (uint8_t)p.blockLen);
        for (int i = 0; i < p.blockLen; i++) Put(out, p.block[i]);
    }
    Put(out, 0);
}

// Append a frame whose delay was left open, now that its end is known.
static void Flush(Buf *gif, Buf *frame, size_t delayAt, long startMs, long untilMs) {
    int cs = (int)lround((untilMs - startMs) / 10.0);
    if (cs < 2) cs = 2;
    if (cs > 65535) cs = 65535;
    frame->d[delayAt] = cs & 0xFF;
    frame->d[delayAt + 1] = (cs >> 8) & 0xFF;
    for (size_t i = 0; i < frame->len; i++) Put(gif, frame->d[i]);
}

// ------------------------------------------------------------------ main

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: makegif <frames-dir> <out.gif> [--width N] "
                            "[--poster out.png] [--hold seconds]\n");
            return 2;
        }
        NSString *dir = @(argv[1]);
        NSString *outPath = @(argv[2]);
        int width = 960;
        double hold = 2.5;
        NSString *posterPath = nil;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--width")) width = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "--poster")) posterPath = @(argv[i + 1]);
            else if (!strcmp(argv[i], "--hold")) hold = atof(argv[i + 1]);
            else Die([NSString stringWithFormat:@"unknown option %s", argv[i]]);
        }

        // --- read the manifest
        NSString *manifest = [NSString stringWithContentsOfFile:
            [dir stringByAppendingPathComponent:@"manifest.txt"]
                                                       encoding:NSUTF8StringEncoding
                                                          error:nil];
        if (!manifest) Die(@"no manifest.txt in the frames folder");
        NSMutableArray<Frame *> *frames = [NSMutableArray array];
        long posterMs = -1, endMs = -1;
        for (NSString *line in [manifest componentsSeparatedByString:@"\n"]) {
            NSArray<NSString *> *w = [line componentsSeparatedByString:@" "];
            if ([w[0] isEqualToString:@"frame"] && w.count == 5) {
                Frame *f = [Frame new];
                f.ms = w[1].longLongValue;
                f.file = w[2];
                f.w = w[3].doubleValue;
                f.h = w[4].doubleValue;
                f.overs = [NSMutableArray array];
                [frames addObject:f];
            } else if ([w[0] isEqualToString:@"child"] && w.count == 5 && frames.count) {
                if (w[1].doubleValue < 0) frames.lastObject.childLeft = YES;
                if (w[2].doubleValue < 0) frames.lastObject.childAbove = YES;
            } else if ([w[0] isEqualToString:@"over"] && w.count == 6 && frames.count) {
                [frames.lastObject.overs addObject:@[w[1], @(w[2].doubleValue),
                    @(w[3].doubleValue), @(w[4].doubleValue), @(w[5].doubleValue)]];
            } else if ([w[0] isEqualToString:@"poster"] && w.count == 2) {
                posterMs = w[1].longLongValue;
            } else if ([w[0] isEqualToString:@"end"] && w.count == 2) {
                endMs = w[1].longLongValue;
            }
        }
        if (frames.count == 0) Die(@"the manifest lists no frames");
        if (endMs < frames.lastObject.ms) endMs = frames.lastObject.ms + 100;
        const int W = width;
        const int H = (int)lround(frames[0].h * W / frames[0].w);
        const size_t N = (size_t)W * H;

        // --- pass 1: palette from every frame (every other pixel is plenty)
        Bin *bins = calloc(32768, sizeof(Bin));
        BOOL anyTransparent = NO;
        for (Frame *f in frames) {
            @autoreleasepool {
                uint8_t *px = Render(f, dir, W, H);
                for (size_t i = 0; i < N; i += 2) {
                    uint8_t *p = px + i * 4;
                    if (p[3] == 0) { anyTransparent = YES; continue; }
                    Bin *b = &bins[Key(p)];
                    b->n += 1; b->r += p[0]; b->g += p[1]; b->b += p[2];
                }
                free(px);
            }
        }
        (void)anyTransparent;   // index 255 is always kept for transparency
        uint8_t pal[256][3] = {{0}};
        int colors = BuildPalette(bins, 255, pal);
        free(bins);
        const uint8_t kClear = 255;
        // Nearest palette entry for every 15-bit color.
        uint8_t *lookup = malloc(32768);
        for (int key = 0; key < 32768; key++) {
            int r = ((key >> 10) & 31) * 255 / 31, g = ((key >> 5) & 31) * 255 / 31,
                b = (key & 31) * 255 / 31;
            int best = 0, bestD = INT_MAX;
            for (int i = 0; i < colors; i++) {
                int dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
                int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (d < bestD) { bestD = d; best = i; }
            }
            lookup[key] = (uint8_t)best;
        }

        // --- pass 2: index each frame, merge repeats, write changed rectangles
        Buf gif = {0};
        const char *hdr = "GIF89a";
        for (int i = 0; i < 6; i++) Put(&gif, hdr[i]);
        Put16(&gif, W); Put16(&gif, H);
        Put(&gif, 0xF7);   // global color table, 8 bits, 256 entries
        Put(&gif, 0); Put(&gif, 0);
        for (int i = 0; i < 256; i++) { Put(&gif, pal[i][0]); Put(&gif, pal[i][1]); Put(&gif, pal[i][2]); }
        // Loop forever.
        const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E',
                                '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
        for (size_t i = 0; i < sizeof loop; i++) Put(&gif, loop[i]);

        uint8_t *shown = malloc(N);    // what the viewer shows now
        uint8_t *cur = malloc(N);
        uint8_t *rect = malloc(N);
        BOOL first = YES;
        int written = 0;
        long pendingStart = 0;         // ms at which the pending frame appeared
        // A frame is written once the next different frame (or the end) is
        // known, since its delay is the time until then.
        Buf pendingData = {0};
        BOOL havePending = NO;
        size_t delayAt = 0;            // offset of the pending frame's delay field
        uint8_t *posterPx = NULL;

        for (NSUInteger fi = 0; fi < frames.count; fi++) {
            @autoreleasepool {
                Frame *f = frames[fi];
                uint8_t *px = Render(f, dir, W, H);
                if (posterPath && !posterPx && f.ms >= posterMs && posterMs >= 0)
                    posterPx = px;
                for (size_t i = 0; i < N; i++) {
                    uint8_t *p = px + i * 4;
                    cur[i] = p[3] == 0 ? kClear : lookup[Key(p)];
                }
                if (px != posterPx) free(px);

                int x0 = W, y0 = H, x1 = -1, y1 = -1;
                if (first) { x0 = 0; y0 = 0; x1 = W - 1; y1 = H - 1; }
                else {
                    for (int y = 0; y < H; y++) {
                        const uint8_t *a = cur + (size_t)y * W, *b = shown + (size_t)y * W;
                        if (!memcmp(a, b, W)) continue;
                        y0 = MIN(y0, y); y1 = y;
                        for (int x = 0; x < W; x++)
                            if (a[x] != b[x]) { x0 = MIN(x0, x); x1 = MAX(x1, x); }
                    }
                }
                if (x1 < 0) continue;   // identical: the pending frame just lasts longer

                // A small change to a mostly transparent rectangle can only
                // help; on the first frame, keep real pixels everywhere.
                int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
                for (int y = 0; y < rh; y++) {
                    const uint8_t *a = cur + (size_t)(y0 + y) * W + x0;
                    const uint8_t *b = shown + (size_t)(y0 + y) * W + x0;
                    uint8_t *o = rect + (size_t)y * rw;
                    for (int x = 0; x < rw; x++)
                        o[x] = (!first && a[x] == b[x]) ? kClear : a[x];
                }
                // Pixels that turned transparent cannot be drawn over an
                // opaque one with "leave in place" disposal; they keep what
                // was there, so record that as what is shown.
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) {
                        size_t i = (size_t)y * W + x;
                        if (cur[i] != kClear) shown[i] = cur[i];
                        else if (first) shown[i] = kClear;
                    }

                if (havePending) Flush(&gif, &pendingData, delayAt, pendingStart, f.ms);
                pendingData.len = 0;
                Buf *b = &pendingData;
                Put(b, 0x21); Put(b, 0xF9); Put(b, 4);
                Put(b, 0x05);            // disposal: leave in place; transparency on
                delayAt = b->len;
                Put16(b, 0);             // delay, filled in by flush
                Put(b, kClear);
                Put(b, 0);
                Put(b, 0x2C);
                Put16(b, x0); Put16(b, y0); Put16(b, rw); Put16(b, rh);
                Put(b, 0);               // no local color table, not interlaced
                Lzw(b, rect, (size_t)rw * rh);
                havePending = YES;
                pendingStart = f.ms;
                first = NO;
                written++;
            }
        }
        if (havePending)
            Flush(&gif, &pendingData, delayAt, pendingStart, endMs + (long)(hold * 1000));
        Put(&gif, 0x3B);
        if (![[NSData dataWithBytesNoCopy:gif.d length:gif.len freeWhenDone:NO]
                writeToFile:outPath atomically:YES])
            Die(@"cannot write the GIF");

        if (posterPath) {
            if (!posterPx) posterPx = Render(frames.lastObject, dir, W, H);
            WritePNG(posterPx, W, H, posterPath);
        }
        printf("%s: %d frames (%lu captured), %dx%d, %d colors, %.1fs, %.2f MB\n",
               outPath.lastPathComponent.UTF8String, written,
               (unsigned long)frames.count, W, H, colors,
               (endMs - frames[0].ms) / 1000.0 + hold, gif.len / 1048576.0);
    }
    return 0;
}
