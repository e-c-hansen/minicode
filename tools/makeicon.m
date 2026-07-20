// Generates resources/AppIcon.icns for MiniCode. Build via `make icon`.
// Motif: syntax-highlighted lines of code in the app's own Dark+ palette,
// on a dark rounded square.
#import <Cocoa/Cocoa.h>

static NSColor *C(int r, int g, int b) {
    return [NSColor colorWithSRGBRed:r/255.0 green:g/255.0 blue:b/255.0 alpha:1.0];
}

static void bar(CGFloat x, CGFloat y, CGFloat w, CGFloat h, NSColor *c) {
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, y, w, h)
                                                     xRadius:h / 2 yRadius:h / 2];
    [c setFill];
    [p fill];
}

static void renderPNG(int S, NSString *path) {
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:S pixelsHigh:S
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
        colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    NSGraphicsContext *ctx =
        [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:ctx];

    // Rounded-square background with a subtle vertical gradient.
    CGFloat inset = S * 0.055;
    NSRect r = NSMakeRect(inset, inset, S - 2 * inset, S - 2 * inset);
    NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:r
                                                      xRadius:S * 0.185
                                                      yRadius:S * 0.185];
    NSGradient *grad = [[NSGradient alloc] initWithColorsAndLocations:
        C(37, 40, 46), 0.0, C(24, 25, 29), 1.0, nil];
    [grad drawInBezierPath:bg angle:-90];

    // Palette (matches ColorForStyle in the editor).
    NSColor *blue = C(86, 156, 214);    // keyword
    NSColor *gray = C(150, 158, 170);   // plain text
    NSColor *orange = C(206, 145, 120); // string
    NSColor *yellow = C(220, 220, 170); // function
    NSColor *teal = C(78, 201, 176);    // type
    NSColor *green = C(106, 153, 85);   // comment

    CGFloat h = S * 0.050;              // bar thickness
    CGFloat gap = S * 0.058;            // vertical gap
    CGFloat stride = h + gap;
    int rows = 5;
    CGFloat totalH = rows * h + (rows - 1) * gap;
    CGFloat y = S / 2.0 + totalH / 2.0 - h;   // top row, going downward
    CGFloat x0 = S * 0.30;
    CGFloat ind = S * 0.075;            // one indent step
    CGFloat sp = S * 0.020;            // gap between segments on a line

    bar(x0, y, S * 0.12, h, blue);                       // row 0
    bar(x0 + S * 0.12 + sp, y, S * 0.19, h, gray);
    y -= stride;                                         // row 1 (indent)
    bar(x0 + ind, y, S * 0.23, h, orange);
    y -= stride;                                         // row 2 (indent)
    bar(x0 + ind, y, S * 0.11, h, yellow);
    bar(x0 + ind + S * 0.11 + sp, y, S * 0.14, h, teal);
    y -= stride;                                         // row 3 (deeper)
    bar(x0 + 2 * ind, y, S * 0.18, h, green);
    y -= stride;                                         // row 4
    bar(x0, y, S * 0.10, h, blue);
    bar(x0 + S * 0.10 + sp, y, S * 0.22, h, gray);

    [NSGraphicsContext restoreGraphicsState];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                    properties:@{}];
    [png writeToFile:path atomically:YES];
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        renderPNG(1024, [NSString stringWithUTF8String:argv[1]]);
    }
    return 0;
}
