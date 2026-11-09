// MarkdownPageSuite.mm — `make guitest SUITES=markdown_page`: a rendered
// Markdown page draws whole however AppKit asks (the owner's report on
// 1.5.16, 2026-11-09). A thin strip of a table row, as scrolling redraws,
// shows what the whole picture shows (AppKit found no glyphs in one: blank
// bands through uneven rows), in the full preview and the one to the side;
// inline code that wraps has its background only behind its text; and a
// page replaced at a new zoom while scrolled down redraws the whole pane,
// margins too (the old page's ink stayed in them).
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "ControlServer.h"
#import "EditorFont.h"
#import "MarkdownSidePreview.h"
#import <objc/runtime.h>

namespace {

NSBitmapImageRep *DrawRect(NSView *v, NSRect r) {
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:r];
    [v cacheDisplayInRect:r toBitmapImageRep:rep];
    return rep;
}

NSUInteger BrightPixels(NSBitmapImageRep *rep, NSInteger y0, NSInteger y1) {
    NSUInteger n = 0;
    for (NSInteger y = MAX(0, y0); y < MIN(rep.pixelsHigh, y1); y++)
        for (NSInteger x = 0; x < rep.pixelsWide; x++) {
            NSColor *c = [rep colorAtX:x y:y];
            if (c.alphaComponent > 0.5 && c.brightnessComponent > 0.5) n++;
        }
    return n;
}

NSString *TablesDoc(void) {
    NSMutableString *s = [NSMutableString stringWithString:@"# Tables\n\nA line before.\n\n"];
    for (int t = 0; t < 12; t++) {
        [s appendString:@"| Artifact | What it is | Source |\n|---|---|---|\n"];
        for (int i = 0; i < 8; i++) {
            NSMutableString *words = [NSMutableString string];
            for (int k = 0; k < 8 + (i * 5) % 22; k++) [words appendFormat:@"word%d ", k];
            [s appendFormat:@"| row %d | %@ | `api.example.com/orgs/some-name/and/a/longer/path` |\n",
                            i, words];
        }
        [s appendString:@"\nA paragraph between tables, long enough to read as text.\n\n"];
    }
    return s;
}

// Strips of each height drawn alone, at each scroll: nothing the whole
// picture shows is missing.
void CheckStrips(NSTextView *tv, NSScrollView *sv, NSString *name) {
    [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
    for (CGFloat top : {0.0, 437.0, 900.0, 1737.0, 3100.0}) {
        [sv.contentView scrollToPoint:NSMakePoint(0, top)];
        [sv reflectScrolledClipView:sv.contentView];
        const NSRect vis = tv.visibleRect;
        NSBitmapImageRep *whole = DrawRect(tv, vis);
        const CGFloat scale = whole.pixelsHigh / NSHeight(vis);
        for (CGFloat height : {13.0, 37.0, 61.0}) {
            NSUInteger seen = 0, missing = 0;
            for (CGFloat y = NSMinY(vis); y + height < NSMaxY(vis); y += height) {
                const NSRect strip = NSMakeRect(NSMinX(vis), y, NSWidth(vis), height);
                NSBitmapImageRep *one = DrawRect(tv, strip);
                const NSUInteger alone = BrightPixels(one, 0, one.pixelsHigh);
                const NSInteger off = (NSInteger)llround((y - NSMinY(vis)) * scale);
                const NSUInteger in = BrightPixels(whole, off, off + one.pixelsHigh);
                seen += in;
                if (alone * 10 < in * 8) missing += in - alone;
            }
            MCGuiNote(@"%@ at %.0f, strips of %.0f: %lu bright pixels, %lu missing", name, top,
                      height, (unsigned long)seen, (unsigned long)missing);
            GCHECK(seen > 1000);
            GCHECK(missing == 0);
        }
    }
}

// Is this pixel of `rep` (in view coordinates, relative to `r`) the color
// behind inline code, as `ref` is, rather than the page's?
BOOL PaintedAs(NSBitmapImageRep *rep, NSRect r, NSPoint p, NSColor *ref) {
    const CGFloat scale = rep.pixelsWide / NSWidth(r);
    NSColor *c = [rep colorAtX:(NSInteger)((p.x - NSMinX(r)) * scale)
                             y:(NSInteger)((p.y - NSMinY(r)) * scale)];
    return fabs(c.redComponent - ref.redComponent) < 0.02 &&
           fabs(c.greenComponent - ref.greenComponent) < 0.02 &&
           fabs(c.blueComponent - ref.blueComponent) < 0.02 &&
           fabs(c.alphaComponent - ref.alphaComponent) < 0.02;
}

// What a text view marks to redraw, while `body` runs.
NSView *gWatched;
NSMutableArray<NSValue *> *gMarked;
IMP gMarkOriginal, gMarkLayoutOriginal;

void MarkRecord(id self, SEL cmd, NSRect r) {
    if (self == gWatched) [gMarked addObject:[NSValue valueWithRect:r]];
    ((void (*)(id, SEL, NSRect))gMarkOriginal)(self, cmd, r);
}
void MarkLayoutRecord(id self, SEL cmd, NSRect r, BOOL avoid) {
    if (self == gWatched) [gMarked addObject:[NSValue valueWithRect:r]];
    ((void (*)(id, SEL, NSRect, BOOL))gMarkLayoutOriginal)(self, cmd, r, avoid);
}

NSArray<NSValue *> *MarkedDuring(NSTextView *tv, void (^body)(void)) {
    Method m = class_getInstanceMethod(NSView.class, @selector(setNeedsDisplayInRect:));
    Method ml = class_getInstanceMethod(NSTextView.class,
                                        @selector(setNeedsDisplayInRect:avoidAdditionalLayout:));
    gWatched = tv;
    gMarked = [NSMutableArray array];
    gMarkOriginal = method_setImplementation(m, (IMP)MarkRecord);
    gMarkLayoutOriginal = method_setImplementation(ml, (IMP)MarkLayoutRecord);
    body();
    method_setImplementation(m, gMarkOriginal);
    method_setImplementation(ml, gMarkLayoutOriginal);
    gWatched = nil;
    return gMarked;
}

// Some one marked rect holds all of `want`.
BOOL AnyHolds(NSArray<NSValue *> *marked, NSRect want) {
    for (NSValue *v in marked)
        if (NSContainsRect(v.rectValue, want)) return YES;
    return NO;
}

void CheckZoomRedraw(NSTextView *tv, NSScrollView *sv, NSString *name) {
    [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
    [sv.contentView scrollToPoint:NSMakePoint(0, 1200)];
    [sv reflectScrolledClipView:sv.contentView];
    GCHECK(NSMinY(tv.visibleRect) > 800);
    NSArray<NSValue *> *marked = MarkedDuring(tv, ^{ MCEditorZoomBy(-2); });
    MCGuiNote(@"%@: visible %@, inset %.0f, %lu rects marked", name,
              NSStringFromRect(tv.visibleRect), tv.textContainerInset.width,
              (unsigned long)marked.count);
    GCHECK(NSMinY(tv.visibleRect) > 300);   // still down the page
    GCHECK(AnyHolds(marked, tv.visibleRect));
    MCEditorZoomBy(0);
}

}  // namespace

MC_GUI_SUITE(markdown_page) {
    __block EditorController *c = nil;
    NSString *dir = MCGuiProjectFile(@"mdpage");
    NSString *md = [dir stringByAppendingPathComponent:@"page.md"];
    NSString *code = [dir stringByAppendingPathComponent:@"code.md"];

    MCGuiStep(@"a page of uneven tables in the full preview", ^{
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        MCGuiWriteFile(md, TablesDoc());
        // Long words, so each line of the code ends well short of the page's edge.
        NSString *word = [@"" stringByPaddingToLength:64 withString:@"longword." startingAtIndex:0];
        MCGuiWriteFile(code, [NSString stringWithFormat:
            @"- A list item with code that wraps: `%@ %@ %@ %@` and some words.\n", word, word, word, word]);
        MCGuiSetSettings(@"lsp.enabled = false\n");
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        MCEditorZoomBy(0);
        NSRect fr = c.window.frame;
        fr.size = NSMakeSize(2000, 1200);
        [c.window setFrame:fr display:NO];
        GCHECK([c showFile:md]);
        GCHECK(c.previewMode);
    });
    MCGuiStep(@"strips of table rows draw what the whole picture shows", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        CheckStrips(tv, tv.enclosingScrollView, @"full preview");
    });
    MCGuiStep(@"a new zoom far down the page redraws the whole pane", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        CheckZoomRedraw(tv, tv.enclosingScrollView, @"full preview");
    });
    MCGuiStep(@"inline code's background stops with its text on each line", ^{
        GCHECK([c showFile:code]);
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK(c.previewMode);
        NSLayoutManager *lm = tv.layoutManager;
        NSTextContainer *tc = tv.textContainer;
        [lm ensureLayoutForTextContainer:tc];
        const NSRange run = [tv.string rangeOfString:@"longword."];
        GCHECK(run.location != NSNotFound);
        NSRange codeRange;
        [tv.textStorage attribute:NSBackgroundColorAttributeName atIndex:run.location
            longestEffectiveRange:&codeRange inRange:NSMakeRange(0, tv.string.length)];
        NSRange glyphs = [lm glyphRangeForCharacterRange:codeRange actualCharacterRange:NULL];
        NSMutableArray<NSValue *> *used = [NSMutableArray array];
        [lm enumerateLineFragmentsForGlyphRange:glyphs usingBlock:^(NSRect, NSRect u,
                                                                    NSTextContainer *, NSRange,
                                                                    BOOL *) {
            [used addObject:[NSValue valueWithRect:u]];
        }];
        GCHECK(used.count >= 2);
        const NSPoint o = tv.textContainerOrigin;
        const NSRect vis = tv.visibleRect;
        NSBitmapImageRep *rep = DrawRect(tv, vis);
        NSRect first = NSOffsetRect(used[0].rectValue, o.x, o.y);
        NSRect second = NSOffsetRect(used[1].rectValue, o.x, o.y);
        // The first line's code box, just left of where its line ends: a
        // pixel between letters could be ink, so several are looked at.
        NSColor *ref = nil;
        NSCountedSet *seen = [NSCountedSet set];
        for (CGFloat x = NSMaxX(first) - 60; x < NSMaxX(first) - 2; x += 1) {
            const CGFloat scale = rep.pixelsWide / NSWidth(vis);
            [seen addObject:[rep colorAtX:(NSInteger)((x - NSMinX(vis)) * scale)
                                        y:(NSInteger)((NSMinY(first) + 2 - NSMinY(vis)) * scale)]];
        }
        NSUInteger best = 0;
        for (NSColor *color in seen)
            if ([seen countForObject:color] > best) { best = [seen countForObject:color]; ref = color; }
        MCGuiNote(@"code background %@; first line %@, second %@, container %.0f wide", ref,
                  NSStringFromRect(first), NSStringFromRect(second), tc.size.width);
        GCHECK(ref != nil && ref.alphaComponent > 0.01);
        // Just past the last letter on the first line, and just left of
        // the second line's first: AppKit's fill reached both.
        __block NSRect ink = NSZeroRect;
        [lm enumerateLineFragmentsForGlyphRange:glyphs usingBlock:^(NSRect, NSRect,
                                                                    NSTextContainer *box,
                                                                    NSRange line, BOOL *stop) {
            ink = [lm boundingRectForGlyphRange:NSIntersectionRange(line, glyphs)
                                inTextContainer:box];
            *stop = YES;
        }];
        const CGFloat afterText = NSMaxX(ink) + o.x + 10;
        MCGuiNote(@"the first line's code ends at %.0f; looking at %.0f and %.0f",
                  NSMaxX(ink) + o.x, afterText, NSMinX(second) - 10);
        GCHECK(afterText < o.x + tc.size.width - 30);   // room past the text, or this means nothing
        GCHECK(!PaintedAs(rep, vis, NSMakePoint(afterText, NSMidY(first)), ref));
        GCHECK(NSMinX(second) > o.x + 20);   // the list's indent
        GCHECK(!PaintedAs(rep, vis, NSMakePoint(NSMinX(second) - 10, NSMidY(second)), ref));
    });
    MCGuiStep(@"the preview to the side", ^{
        GCHECK([c showFile:md]);
        [c togglePreview:nil];
        GCHECK(!c.previewMode);
        [c toggleSidePreview:nil];
        GCHECK([c sidePreviewShowing]);
    });
    MCGuiWait(@"the side page", 5, ^BOOL {
        MCMarkdownSidePreview *side = [c valueForKey:@"sidePreview"];
        return [side.textView.string containsString:@"A line before."];
    });
    MCGuiStep(@"its strips draw whole too", ^{
        MCMarkdownSidePreview *side = [c valueForKey:@"sidePreview"];
        CheckStrips(side.textView, side.scrollView, @"side preview");
    });
    MCGuiStep(@"the window goes", ^{
        MCEditorZoomBy(0);
        if ([c sidePreviewShowing]) [c toggleSidePreview:nil];
        MCGuiSetKeyWindow(nil);
        [c.window close];
    });
}
