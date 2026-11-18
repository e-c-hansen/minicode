// Generates the mutible app icon. Build via `make icon`.
//
// The mark: "m-", a Fraunces m and the ible.ai family's slanted indigo hyphen,
// on a squircle split along the hyphen's lean into paper (left) and ink
// (right). The split runs down the m's middle stem, so the letter is two
// arches, one on each side: your side and the agents' side of one space. Each
// side draws the m in its own colours, and the hyphen runs off the right edge.
//
// CoreGraphics only, no fonts at run time: the m's outline is embedded below,
// taken once from Fraunces (SIL Open Font License), wght 700, opsz 72. At 32
// and 16 px the m is hinted to whole pixels and the split stands upright; at
// 16 px it sits between the m and the hyphen.
//
//   makeicon out.png              the 1024 px master (what `make icon` uses)
//   makeicon --iconset dir        a full .iconset for iconutil, with the
//                                 hand-tuned 16 and 32 px images
//   makeicon --png size out.png   one size, e.g. for Linux's hicolor theme
//
// Builds with or without ARC: clang -framework Cocoa makeicon.m -o makeicon
#import <Cocoa/Cocoa.h>

// Fraunces 'm', font units (1000/em, y up). Display cut: wght 700, opsz 72.
static const char *M_DISPLAY =
"M,236.0,449.3,L,236.0,63.3,Q,236.0,50.9,239.6,44.5,Q,243.1,38.2,250.4,35.6,L,268.6,30.8,Q,279.6,26.1,279.6,16.2,Q,279.6,0.0,259.2,0.0,L,45.3,0.0,Q,34.9,0.0,30.1,4.2,Q,25.3,8.4,25.3,15.3,Q,25.3,20.6,28.7,24.7,Q,32.0,28.7,38.9,31.2,L,57.8,36.0,Q,65.4,38.4,68.5,44.3,Q,71.7,50.3,71.7,62.4,L,71.7,369.6,Q,71.7,379.2,68.8,383.6,Q,65.9,388.1,59.4,389.1,L,36.4,390.6,Q,30.4,392.1,27.4,395.6,Q,24.4,399.1,24.4,404.3,Q,24.4,410.4,28.1,414.3,Q,31.8,418.2,41.5,421.6,L,173.4,463.8,Q,187.2,469.1,195.7,471.2,Q,204.2,473.4,212.2,473.4,Q,224.1,473.4,230.0,466.5,Q,236.0,459.7,236.0,449.3,Z,"
"M,221.5,337.6,L,204.5,355.0,L,222.2,371.4,Q,282.4,428.1,329.1,450.9,Q,375.9,473.8,415.3,473.8,Q,472.6,473.8,507.5,437.3,Q,542.4,400.9,542.4,333.5,L,542.4,62.0,Q,542.4,49.3,545.8,43.6,Q,549.2,37.8,556.3,35.4,L,571.0,31.0,Q,582.1,25.9,582.1,16.2,Q,582.1,0.0,561.0,0.0,L,355.9,0.0,Q,336.2,0.0,336.2,16.2,Q,336.2,26.1,347.2,30.8,L,363.6,35.2,Q,372.5,38.2,375.9,45.3,Q,379.2,52.4,379.2,66.1,L,379.2,312.3,Q,379.2,348.5,362.2,367.7,Q,345.1,386.8,316.0,386.8,Q,296.1,386.8,277.2,378.8,Q,258.2,370.9,239.6,354.1,Z,"
"M,527.5,337.6,L,510.5,355.0,L,527.1,372.5,Q,579.1,425.6,621.5,449.7,Q,663.9,473.8,704.1,473.8,Q,759.3,473.8,792.7,437.2,Q,826.2,400.6,833.2,333.2,L,863.3,62.8,Q,864.8,49.5,867.8,43.5,Q,870.9,37.4,878.0,35.0,L,892.7,30.8,Q,899.4,28.4,902.5,24.5,Q,905.5,20.7,905.5,15.3,Q,905.5,8.2,900.8,4.1,Q,896.1,0.0,885.9,0.0,L,672.4,0.0,Q,652.7,0.0,652.7,16.2,Q,652.7,26.0,663.7,30.8,L,680.1,35.2,Q,689.0,38.2,693.1,45.5,Q,697.2,52.7,695.7,66.9,L,670.5,312.3,Q,666.6,348.5,651.8,367.6,Q,636.9,386.8,608.9,386.8,Q,592.7,386.8,576.7,378.6,Q,560.6,370.3,544.6,355.2,Z,";

// ible.ai's palette (ible.ai/styles.css).
#define PAPER   0xF8FAFC
#define INK     0x0F172A
#define INDIGO  0x6366F1   // the accent on paper
#define INDIGO2 0x818CF8   // the accent on ink
#define LEAN    0.32       // the hyphen's lean, dx per unit of height

static void fillHex(CGContextRef c, unsigned hex, CGFloat a) {
    CGContextSetRGBFillColor(c, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0, (hex & 255) / 255.0, a);
}

static CGPathRef glyphPath(const char *d, CGAffineTransform t) {
    CGMutablePathRef p = CGPathCreateMutable();
    const char *s = d;
    while (*s) {
        char op = *s; s += 2;
        double v[6]; int n = op == 'M' || op == 'L' ? 2 : op == 'Q' ? 4 : op == 'C' ? 6 : 0;
        for (int i = 0; i < n; i++) { char *e; v[i] = strtod(s, &e); s = e + 1; }
        if (op == 'M') CGPathMoveToPoint(p, &t, v[0], v[1]);
        else if (op == 'L') CGPathAddLineToPoint(p, &t, v[0], v[1]);
        else if (op == 'Q') CGPathAddQuadCurveToPoint(p, &t, v[0], v[1], v[2], v[3]);
        else if (op == 'C') CGPathAddCurveToPoint(p, &t, v[0], v[1], v[2], v[3], v[4], v[5]);
        else if (op == 'Z') CGPathCloseSubpath(p);
    }
    return p;
}
static CGPathRef hyphenPath(CGFloat x, CGFloat y, CGFloat w, CGFloat h, CGFloat slant, CGFloat r) {
    CGPoint P[4] = { {x, y}, {x + w, y}, {x + w + h * slant, y + h}, {x + h * slant, y + h} };
    CGMutablePathRef p = CGPathCreateMutable();
    for (int i = 0; i < 4; i++) {
        CGPoint a = P[(i + 3) % 4], b = P[i], c = P[(i + 1) % 4];
        CGFloat la = hypot(b.x - a.x, b.y - a.y), lc = hypot(c.x - b.x, c.y - b.y);
        CGFloat ra = MIN(r, la / 2), rc = MIN(r, lc / 2);
        CGPoint in = { b.x + (a.x - b.x) * ra / la, b.y + (a.y - b.y) * ra / la };
        CGPoint out = { b.x + (c.x - b.x) * rc / lc, b.y + (c.y - b.y) * rc / lc };
        if (i == 0) CGPathMoveToPoint(p, NULL, in.x, in.y); else CGPathAddLineToPoint(p, NULL, in.x, in.y);
        CGPathAddQuadCurveToPoint(p, NULL, b.x, b.y, out.x, out.y);
    }
    CGPathCloseSubpath(p);
    return p;
}
static CGPathRef squirclePath(CGRect r) {
    CGMutablePathRef p = CGPathCreateMutable();
    CGFloat cx = CGRectGetMidX(r), cy = CGRectGetMidY(r), a = r.size.width / 2, b = r.size.height / 2;
    for (int i = 0; i < 720; i++) {
        double t = i * M_PI / 360, c = cos(t), s = sin(t);
        double x = cx + a * copysign(pow(fabs(c), 0.4), c), y = cy + b * copysign(pow(fabs(s), 0.4), s);
        if (i == 0) CGPathMoveToPoint(p, NULL, x, y); else CGPathAddLineToPoint(p, NULL, x, y);
    }
    CGPathCloseSubpath(p);
    return p;
}
static void fillPath(CGContextRef c, CGPathRef p, unsigned color) {
    fillHex(c, color, 1); CGContextAddPath(c, p); CGContextFillPath(c); CGPathRelease(p);
}

static void hintedM(CGContextRef c, CGFloat ox, CGFloat oy, int sw, int cw, int xh, unsigned ink, CGFloat a) {
    CGMutablePathRef p = CGPathCreateMutable();
    CGFloat t = MAX(1.5, sw * 0.72);         // the shoulder's thickness at the top
    CGFloat r = (sw + cw) * 0.75;            // outer shoulder radius
    CGFloat ri = cw * 0.8;                   // counter radius
    // First stem, full height.
    CGPathAddRect(p, NULL, CGRectMake(ox, oy, sw, xh));
    for (int k = 0; k < 2; k++) {
        CGFloat xa = ox + k * (sw + cw), xb = xa + sw + cw;
        // The outer shape: from the left stem's top across to the right stem,
        // rounded at the top right, square at the top left (the notch is drawn below).
        CGMutablePathRef o = CGPathCreateMutable();
        CGPathMoveToPoint(o, NULL, xa + sw * 0.5, oy);
        CGPathAddLineToPoint(o, NULL, xb + sw, oy);
        CGPathAddLineToPoint(o, NULL, xb + sw, oy + xh - r);
        CGPathAddQuadCurveToPoint(o, NULL, xb + sw, oy + xh, xb + sw - r, oy + xh);
        CGPathAddLineToPoint(o, NULL, xa + sw * 0.5 + r * 0.5, oy + xh);
        CGPathAddQuadCurveToPoint(o, NULL, xa + sw * 0.5, oy + xh, xa + sw * 0.5, oy + xh - r * 0.6);
        CGPathCloseSubpath(o);
        // The counter, rounded at its top.
        CGPathMoveToPoint(o, NULL, xa + sw, oy);
        CGPathAddLineToPoint(o, NULL, xa + sw, oy + xh - t - ri);
        CGPathAddQuadCurveToPoint(o, NULL, xa + sw, oy + xh - t, xa + sw + ri, oy + xh - t);
        CGPathAddLineToPoint(o, NULL, xb - ri, oy + xh - t);
        CGPathAddQuadCurveToPoint(o, NULL, xb, oy + xh - t, xb, oy + xh - t - ri);
        CGPathAddLineToPoint(o, NULL, xb, oy);
        CGPathCloseSubpath(o);
        fillHex(c, ink, a);
        CGContextAddPath(c, o); CGContextEOFillPath(c);
        CGPathRelease(o);
    }
    // The flag: a wedge up and to the left of the first stem's top.
    CGPathMoveToPoint(p, NULL, ox + 0.5, oy + xh);
    CGPathAddLineToPoint(p, NULL, ox - sw * 0.55, oy + xh - sw * 0.45);
    CGPathAddLineToPoint(p, NULL, ox - sw * 0.55, oy + xh - sw * 0.9);
    CGPathAddLineToPoint(p, NULL, ox, oy + xh - sw * 1.2);
    CGPathCloseSubpath(p);
    if (xh >= 11) {   // feet, one pixel tall, a pixel past each stem's outer side
        for (int k = 0; k < 3; k++) {
            CGFloat x = ox + k * (sw + cw);
            CGPathAddRect(p, NULL, CGRectMake(x - (k == 0 ? 1 : 0.5), oy, sw + (k == 2 ? 1 : 0.5) + (k == 0 ? 0.5 : 0), 1));
        }
    }
    fillHex(c, ink, a); CGContextAddPath(c, p); CGContextFillPath(c);
    CGPathRelease(p);
}
static CGFloat hintedWidth(int sw, int cw) { return 3 * sw + 2 * cw; }
// ---- Layout ---------------------------------------------------------------
// All sizes share one description: where the m sits, how it is drawn, the
// hyphen, and the split (x = splitX + lean * (y - splitY)), in pixels, y up.
enum { OUTLINE, HINTED, PIXELS };
typedef struct {
    int kind;
    CGFloat ox, base, s;          // OUTLINE: m origin and scale (font units to px)
    int sw, cw, xh;               // HINTED and PIXELS: stem, counter, x-height
    CGFloat hx, hy, hw, hh;       // the hyphen: lower-left corner before leaning, size
    CGFloat splitX, splitY, lean;
} Layout;

// The Fraunces m's metrics, in its units.
#define M_LEFT 24.0      // flag's left edge
#define M_RIGHT 906.0    // last foot's right edge
#define M_XH 474.0
#define M_STEM2 460.0    // middle of the second stem

// macOS's grid: the body is 824 of 1024, centred. 64 px and below fill the
// square instead, where every pixel counts (tabs, favicons, Finder lists).
static CGRect bodyRect(int S) {
    CGFloat inset = S <= 64 ? S * 0.03 : S * 100.0 / 1024;
    return CGRectMake(inset, inset, S - 2 * inset, S - 2 * inset);
}

// S picks the drawing (16 and 32 px are hand-tuned); body is where it goes.
static Layout layoutFor(int S, CGRect body) {
    Layout L = {0};
    if (S == 16) {
        // Drawn pixel by pixel (M16 below). Here the split moves out of the
        // m to just before the hyphen: a split stem at 2 px reads as mud,
        // while a whole m on paper and the hyphen on ink stays the same mark.
        L.kind = PIXELS; L.ox = 2; L.base = 4;
        L.hx = L.ox + 10; L.hy = L.base + 2; L.hw = 8; L.hh = 2;
        L.splitX = L.ox + 9; L.splitY = 0; L.lean = 0;
        return L;
    }
    if (S == 32) {
        // 4 px stems, 3 px counters, a 12 px x-height; the split down the
        // middle of the second stem.
        L.kind = HINTED; L.sw = 4; L.cw = 3; L.xh = 12; L.ox = 4; L.base = 10;
        L.hx = L.ox + hintedWidth(4, 3) + 2; L.hy = L.base + 4; L.hw = 14; L.hh = 3;
        L.splitX = L.ox + 4 + 3 + 2; L.splitY = 0; L.lean = 0;
        return L;
    }
    CGFloat W = body.size.width;
    L.kind = OUTLINE;
    L.s = W * 0.34 / M_XH;                               // x-height: 34% of the body
    L.ox = body.origin.x + W * 0.09 - M_LEFT * L.s;      // flag 9% in from the left
    L.base = CGRectGetMidY(body) - M_XH * L.s / 2;
    L.hx = L.ox + (M_RIGHT + 55) * L.s; L.hy = L.base + 165 * L.s;
    L.hh = 128 * L.s; L.hw = CGRectGetMaxX(body) - L.hx + W * 0.1;   // runs off the edge
    // The split crosses the middle stem corner to corner at the hyphen's lean.
    L.splitY = L.base + M_XH * L.s / 2;
    L.splitX = L.ox + M_STEM2 * L.s;
    L.lean = LEAN;
    return L;
}

// 16 px: 2 px stems, 1 px counters, a 7 px x-height. '#' full, '+' half.
static const char *M16[] = {
    "####+##+",
    "##+##+##",
    "##.##.##",
    "##.##.##",
    "##.##.##",
    "##.##.##",
    "##.##.##",
};

static void drawLetter(CGContextRef c, Layout L, unsigned ink) {
    if (L.kind == OUTLINE) {
        fillPath(c, glyphPath(M_DISPLAY, CGAffineTransformMake(L.s, 0, 0, L.s, L.ox, L.base)), ink);
    } else if (L.kind == HINTED) {
        hintedM(c, L.ox, L.base, L.sw, L.cw, L.xh, ink, 1);
    } else {
        for (int r = 0; r < 7; r++) for (int k = 0; M16[r][k]; k++) {
            char ch = M16[r][k];
            if (ch == '.') continue;
            fillHex(c, ink, ch == '#' ? 1 : 0.5);
            CGContextFillRect(c, CGRectMake(L.ox + k, L.base + 6 - r, 1, 1));
        }
        fillHex(c, ink, 0.45); CGContextFillRect(c, CGRectMake(L.ox - 1, L.base + 6, 1, 1));
    }
}

static void drawHyphen(CGContextRef c, Layout L, unsigned accent) {
    fillPath(c, hyphenPath(L.hx, L.hy, L.hw, L.hh, LEAN, L.hh * (L.kind == OUTLINE ? 0.22 : 0.25)), accent);
}

// The ink side: everything right of the split, clipped later to the body.
static CGPathRef inkSide(CGRect body, Layout L) {
    CGMutablePathRef p = CGPathCreateMutable();
    CGFloat y0 = CGRectGetMinY(body) - 4, y1 = CGRectGetMaxY(body) + 4, x1 = CGRectGetMaxX(body) + 4;
    CGPathMoveToPoint(p, NULL, L.splitX + L.lean * (y0 - L.splitY), y0);
    CGPathAddLineToPoint(p, NULL, x1, y0);
    CGPathAddLineToPoint(p, NULL, x1, y1);
    CGPathAddLineToPoint(p, NULL, L.splitX + L.lean * (y1 - L.splitY), y1);
    CGPathCloseSubpath(p);
    return p;
}

// Draws the icon's body into `body` of a canvas of side S: a squircle, or a
// plain square (for apple-touch-icon, which the system rounds itself).
static void drawIconIn(CGContextRef c, int S, CGRect body, BOOL squircle, BOOL shadow) {
    Layout L = layoutFor(S, body);
    CGPathRef sq = squircle ? squirclePath(body) : CGPathCreateWithRect(body, NULL);
    if (shadow) {   // the drop shadow of the macOS grid
        CGContextSaveGState(c);
        CGColorRef sh = CGColorCreateGenericRGB(0, 0, 0, 0.30);
        CGContextSetShadowWithColor(c, CGSizeMake(0, -S * 0.012), S * 0.024, sh);
        CGColorRelease(sh);
        fillHex(c, PAPER, 1); CGContextAddPath(c, sq); CGContextFillPath(c);
        CGContextRestoreGState(c);
    }
    CGPathRef ink = inkSide(body, L);
    CGContextSaveGState(c);
    CGContextAddPath(c, sq); CGContextClip(c);
    fillHex(c, PAPER, 1); CGContextAddPath(c, sq); CGContextFillPath(c);
    fillHex(c, INK, 1); CGContextAddPath(c, ink); CGContextFillPath(c);
    // Paper side: an ink m and the indigo hyphen.
    CGContextSaveGState(c);
    CGContextAddRect(c, CGRectInset(body, -4, -4)); CGContextAddPath(c, ink); CGContextEOClip(c);
    drawLetter(c, L, INK); drawHyphen(c, L, INDIGO);
    CGContextRestoreGState(c);
    // Ink side: a paper m and the lighter indigo.
    CGContextAddPath(c, ink); CGContextClip(c);
    drawLetter(c, L, PAPER); drawHyphen(c, L, INDIGO2);
    CGContextRestoreGState(c);
    // A hairline round the body, so the paper side holds on a white ground.
    if (!squircle) { CGPathRelease(ink); CGPathRelease(sq); return; }
    CGContextSaveGState(c);
    CGContextAddPath(c, sq); CGContextClip(c);
    CGContextSetRGBStrokeColor(c, 15 / 255.0, 23 / 255.0, 42 / 255.0, S <= 64 ? 0.16 : 0.10);
    CGContextSetLineWidth(c, S <= 64 ? (S <= 32 ? 2.0 : 3.0) : S * 0.008);
    CGContextAddPath(c, sq); CGContextStrokePath(c);
    CGContextRestoreGState(c);
    CGPathRelease(ink); CGPathRelease(sq);
}

static void drawIcon(CGContextRef c, int S) {
    drawIconIn(c, S, bodyRect(S), YES, S > 64);
}

// ---- Output ---------------------------------------------------------------
static CGContextRef newContext(int W, int H) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, W, H, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    return c;
}

static BOOL writeContextPNG(CGContextRef c, NSString *path) {
    CGImageRef im = CGBitmapContextCreateImage(c);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:im];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    BOOL ok = [png writeToFile:path atomically:YES];
    CGImageRelease(im);
#if !__has_feature(objc_arc)
    [rep release];
#endif
    return ok;
}

static BOOL writeIcon(int S, NSString *path) {
    CGContextRef c = newContext(S, S);
    drawIcon(c, S);
    BOOL ok = writeContextPNG(c, path);
    CGContextRelease(c);
    return ok;
}

#ifndef MAKEICON_NO_MAIN
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 3 && strcmp(argv[1], "--iconset") == 0) {
            NSString *dir = [NSString stringWithUTF8String:argv[2]];
            [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
            int sizes[] = {16, 32, 64, 128, 256, 512};
            for (int i = 0; i < 6; i++) {
                int sz = sizes[i];
                if (!writeIcon(sz, [dir stringByAppendingFormat:@"/icon_%dx%d.png", sz, sz])) return 1;
                if (!writeIcon(sz * 2, [dir stringByAppendingFormat:@"/icon_%dx%d@2x.png", sz, sz])) return 1;
            }
            return 0;
        }
        if (argc == 4 && strcmp(argv[1], "--png") == 0) {
            int S = atoi(argv[2]);
            if (S < 16 || S > 2048) { fprintf(stderr, "makeicon: size %s is out of range\n", argv[2]); return 2; }
            return writeIcon(S, [NSString stringWithUTF8String:argv[3]]) ? 0 : 1;
        }
        if (argc == 2 && argv[1][0] != '-') {
            return writeIcon(1024, [NSString stringWithUTF8String:argv[1]]) ? 0 : 1;
        }
        fprintf(stderr, "usage: makeicon out.png | --iconset dir | --png size out.png\n");
        return 2;
    }
}
#endif
