// MarkdownMathSuite.mm — `make guitest SUITES=markdown_math`: a typeset
// formula as the preview draws it, without tectonic. The suite writes a
// batch into the math cache itself (a one-page PDF and the index line
// MarkdownMath.mm reads), opens a Markdown file holding that formula, and
// checks the preview shows an MCMathAttachment of the page's size, drawn in
// the editor's text color and following it when the setting changes, with
// the stand-in tectonic never run. MC_GUI_REAL_TECTONIC=<a tectonic> adds a
// real batch where one formula tries to change the next ($ inside braces
// closes its box early, then \def): it fails, and the formulas after it are
// typeset as they would be alone.
#import "Harness.h"
#import "AppSettings.h"
#import "EditorController.h"
#import "EditorController+Private.h"   // closeTab:prompt:
#import "MarkdownMath.h"
#include <sys/stat.h>

namespace {

NSString *Fixture(NSString *name) {
    return [MCGuiProject() stringByAppendingPathComponent:[@"mdmath" stringByAppendingPathComponent:name]];
}

NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }

// The math pictures on show, in text order, with where each is.
NSArray<NSArray *> *Pictures(EditorController *c) {
    NSTextStorage *st = Text(c).textStorage;
    NSMutableArray *out = [NSMutableArray array];
    [st enumerateAttribute:NSAttachmentAttributeName inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(id v, NSRange r, BOOL *stop) {
        (void)stop;
        if ([v isKindOfClass:MCMathAttachment.class]) [out addObject:@[v, @(r.location)]];
    }];
    return out;
}

// A one-page PDF `size` points big, black ink over the middle half of it.
NSData *InkPage(NSSize size) {
    NSMutableData *data = [NSMutableData data];
    CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData((__bridge CFMutableDataRef)data);
    CGRect box = CGRectMake(0, 0, size.width, size.height);
    CGContextRef ctx = CGPDFContextCreate(consumer, &box, NULL);
    CGPDFContextBeginPage(ctx, NULL);
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextFillRect(ctx, CGRectInset(box, size.width / 4, size.height / 4));
    CGPDFContextEndPage(ctx);
    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    CGDataConsumerRelease(consumer);
    return data;
}

// The color, as drawn, at the middle of the character at `index` in the
// text view, or `left` of its width left of the middle.
NSColor *DrawnAt(NSTextView *tv, NSUInteger index, CGFloat left) {
    NSLayoutManager *lm = tv.layoutManager;
    NSRange glyphs = [lm glyphRangeForCharacterRange:NSMakeRange(index, 1) actualCharacterRange:NULL];
    NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
    r = NSOffsetRect(r, tv.textContainerOrigin.x, tv.textContainerOrigin.y);
    NSRect area = NSIntegralRect(NSInsetRect(r, -4, -4));
    NSBitmapImageRep *rep = [tv bitmapImageRepForCachingDisplayInRect:area];
    if (!rep) return nil;
    [tv cacheDisplayInRect:area toBitmapImageRep:rep];
    const CGFloat sx = rep.pixelsWide / area.size.width;
    const CGFloat x = NSMidX(r) - left * r.size.width - area.origin.x, y = NSMidY(r) - area.origin.y;   // flipped
    return [[rep colorAtX:(NSInteger)(x * sx) y:(NSInteger)(y * sx)]
        colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
}

BOOL Near(NSColor *a, NSColor *b, CGFloat tolerance) {
    a = [a colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    b = [b colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    return a && b && fabs(a.redComponent - b.redComponent) < tolerance &&
           fabs(a.greenComponent - b.greenComponent) < tolerance &&
           fabs(a.blueComponent - b.blueComponent) < tolerance;
}

}  // namespace

MC_GUI_SUITE(markdown_math) {
    __block EditorController *c = nil;
    __block NSString *tectonicBefore = nil;
    NSString *ran = [MCGuiScratch() stringByAppendingPathComponent:@"mdmath-tectonic-ran"];
    NSString *fake = [MCGuiScratch() stringByAppendingPathComponent:@"mdmath-tectonic"];
    NSString *page = Fixture(@"page.md");
    const NSSize size = NSMakeSize(40, 24);
    NSFileManager *fm = [NSFileManager defaultManager];

    MCGuiStep(@"a batch in the cache for $x^2$, a stand-in tectonic, the page open", ^{
        const char *was = getenv("MINICODE_TECTONIC");
        tectonicBefore = was ? @(was) : nil;
        MCGuiWriteFile(fake, [NSString stringWithFormat:@"#!/bin/sh\n/usr/bin/touch '%@'\nexit 1\n", ran]);
        chmod(fake.fileSystemRepresentation, 0755);
        setenv("MINICODE_TECTONIC", fake.fileSystemRepresentation, 1);
        MCGuiSetSettings(@"lsp.enabled = false\n");

        NSString *dir = MCMathCacheDirectory();
        GCHECK([dir hasPrefix:MCGuiScratch()]);   // the scratch world's, never the user's
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *batch = [dir stringByAppendingPathComponent:@"000000000000001-mdmath"];
        GCHECK([InkPage(size) writeToFile:[batch stringByAppendingPathExtension:@"pdf"] atomically:YES]);
        NSString *index = [NSString stringWithFormat:@"minicode-math-index 1\n%@ 1 6.0000\n",
                                                     MCMathKey(@"x^2", NO)];
        GCHECK([index writeToFile:[batch stringByAppendingPathExtension:@"idx"] atomically:YES
                         encoding:NSUTF8StringEncoding error:nil]);
        MCMathReloadCache();

        MCGuiWriteFile(page, @"Inline $x^2$ here.\n");
        c = MCGuiController();
        GCHECK([c showFile:page]);
    });
    MCGuiWait(@"the formula is a picture", 10, ^BOOL { return Pictures(c).count == 1; });
    MCGuiStep(@"the cache's page, its size and baseline, drawn in the text color", ^{
        NSArray *pic = Pictures(c).firstObject;
        MCMathAttachment *att = pic[0];
        GCHECK(!att.formula.failed);
        GCHECK(fabs(att.formula.size.width - size.width) < 0.01 &&
               fabs(att.formula.size.height - size.height) < 0.01);
        GCHECK(fabs(att.formula.descent - 6) < 0.001);
        // The text the formula stood for is gone from the page.
        GCHECK(![Text(c).string containsString:@"x^2"]);
        NSTextView *tv = Text(c);
        [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
        NSColor *ink = [[AppSettings shared] text:Surface::Editor];
        NSColor *bg = [[AppSettings shared] background:Surface::Editor];
        const NSUInteger at = [pic[1] unsignedIntegerValue];
        NSColor *middle = DrawnAt(tv, at, 0);
        // Outside the inked half, still inside the page: the background.
        NSColor *edge = DrawnAt(tv, at, 0.4);
        MCGuiNote(@"ink %@, drawn %@, beside it %@", ink, middle, edge);
        GCHECK(Near(middle, ink, 0.25));
        // The view paints no background of its own (the scroll view's
        // backdrop does), so beside the ink is clear, or the background.
        GCHECK(edge && (edge.alphaComponent < 0.1 || Near(edge, bg, 0.25)));
        GCHECK(!Near(ink, bg, 0.3) && middle.alphaComponent > 0.9);
        MCGuiSetSettings(@"lsp.enabled = false\neditor.text = #FF8000\n");
    });
    MCGuiWait(@"another text color: the formula follows it", 10, ^BOOL {
        NSArray *pic = Pictures(c).firstObject;
        // cacheDisplayInRect: shifts colors a little (0.19 in blue here).
        return pic && Near(DrawnAt(Text(c), [pic[1] unsignedIntegerValue], 0),
                           [NSColor colorWithSRGBRed:1 green:0.5 blue:0 alpha:1], 0.25);
    });
    MCGuiPause(0.5);   // before a negative check: tectonic must not start
    MCGuiStep(@"nothing was typeset: the stand-in tectonic never ran", ^{
        GCHECK(![fm fileExistsAtPath:ran]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
    });

    if (NSString *tectonic = NSProcessInfo.processInfo.environment[@"MC_GUI_REAL_TECTONIC"]) {
        NSString *attack = Fixture(@"attack.md");
        MCGuiStep(@"with a real tectonic: a formula that tries to change the next ones", ^{
            setenv("MINICODE_TECTONIC", tectonic.fileSystemRepresentation, 1);
            MCGuiWriteFile(attack, @"A $\\alpha$ B\n\n"
                                   @"$$\n{$}\\def\\alpha{\\beta}{$}\n$$\n\n"
                                   @"C $\\alpha$ D $\\beta$ E $\\typeout{MC:3:1:99pt:9pt:0pt}y$\n");
            GCHECK([c showFile:attack]);
        });
        MCGuiWait(@"tectonic typesets the page", 240, ^BOOL { return Pictures(c).count >= 4; });
        MCGuiStep(@"the attack failed; \\alpha after it is \\alpha; the forged line counted for nothing", ^{
            NSArray<NSArray *> *pics = Pictures(c);
            NSMutableArray<NSNumber *> *widths = [NSMutableArray array];
            for (NSArray *p in pics) [widths addObject:@(((MCMathAttachment *)p[0]).formula.size.width)];
            MCGuiNote(@"widths: %@", widths);
            GCHECK(pics.count == 4);
            if (pics.count == 4) {
                GCHECK(fabs(widths[0].doubleValue - widths[1].doubleValue) < 0.01);   // \alpha twice
                GCHECK(fabs(widths[0].doubleValue - widths[2].doubleValue) > 0.2);    // \beta
                GCHECK(widths[3].doubleValue < 20);   // y, not the forged 99pt box
            }
            // The attack shows as its TeX, with TeX's complaint on hover.
            const NSRange tex = [Text(c).string rangeOfString:@"\\def\\alpha"];
            GCHECK(tex.location != NSNotFound);
            if (tex.location != NSNotFound)
                GCHECK([Text(c).textStorage attribute:NSToolTipAttributeName
                                              atIndex:tex.location effectiveRange:NULL] != nil);
            // \alpha typeset alone afterwards is the same picture.
            MCMathFormula *alone = MCMathLookup(@"\\alpha", NO);
            GCHECK(alone && !alone.failed && fabs(alone.size.width - widths[0].doubleValue) < 0.01);
            GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
        });
        // Page-level state: the page color, by name or as an environment,
        // would fill every later page of the batch. Both are refused
        // before the run, with the reason on hover, and y is typeset.
        NSString *page2 = Fixture(@"pagecolor.md");
        MCGuiStep(@"with a real tectonic: a formula that sets the page color", ^{
            MCGuiWriteFile(page2, @"A $\\pagecolor{red} p$ B $\\begin{pagecolor}{red}q\\end{pagecolor}$ "
                                  @"C $y$ D\n");
            GCHECK([c showFile:page2]);
        });
        MCGuiWait(@"tectonic typesets y", 240, ^BOOL { return Pictures(c).count >= 1; });
        MCGuiStep(@"the page color is refused; y is typeset", ^{
            NSArray<NSArray *> *pics = Pictures(c);
            GCHECK(pics.count == 1);
            for (NSString *tex in @[@"\\pagecolor{red} p", @"\\begin{pagecolor}"]) {
                const NSRange r = [Text(c).string rangeOfString:tex];
                GCHECK(r.location != NSNotFound);
                if (r.location == NSNotFound) continue;
                NSString *tip = [Text(c).textStorage attribute:NSToolTipAttributeName atIndex:r.location
                                                effectiveRange:NULL];
                MCGuiNote(@"%@: %@", tex, tip);
                GCHECK([tip containsString:@"not allowed"]);
            }
            MCMathFormula *y = MCMathLookup(@"y", NO);
            GCHECK(y && !y.failed);
            GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
        });
    }

    MCGuiStep(@"put everything back", ^{
        if (tectonicBefore) setenv("MINICODE_TECTONIC", tectonicBefore.fileSystemRepresentation, 1);
        else unsetenv("MINICODE_TECTONIC");
        GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
        [fm removeItemAtPath:Fixture(@"") error:nil];
        MCMathReloadCache();
    });
}
