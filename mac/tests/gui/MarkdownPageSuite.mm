// MarkdownPageSuite.mm — `make guitest SUITES=markdown_page`: a rendered
// Markdown page draws whole however AppKit asks (the owner's report on
// 1.5.16, 2026-11-09). A thin strip of a table row, as scrolling redraws,
// shows what the whole picture shows (AppKit found no glyphs in one: blank
// bands through uneven rows), in the full preview and the one to the side;
// inline code that wraps has its background only behind its text; a
// page replaced at a new zoom while scrolled down redraws the whole pane,
// margins too (the old page's ink stayed in them); and a page whose file
// another program rewrites (in place, and by renaming over it) while it is
// scrolled down in a wide window, with the window and the sidebar resized
// between, stays where it was, one text view, centered at its measure,
// all of it marked to be drawn again each time (the owner's STATUS.md on
// 1.6.3, 2026-11-18: an older, narrower copy of the page in its left
// margin, cut off where the page's column starts). And the page's edit
// box (D2 on 1.6.3 and 1.6.4): what is typed in it survives a switch to
// another app, a close other than Esc applies it, its button says Apply,
// an agent's rewrite of the file while it is open is kept (the edit lands
// where its paragraph now is, or, that paragraph rewritten, goes to the
// clipboard), and with two editor groups a double-click on the other
// group's page opens it on the first try.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "ControlServer.h"
#import "EditorFont.h"
#import "MarkdownSidePreview.h"
#import "PageMeasure.h"
#import "AppSettings.h"
#import "MarkdownEditPanel.h"
#import "EditorMirror.h"
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

// Marks recorded across steps (a reload lands on a later turn).
void StartMarks(NSTextView *tv) {
    if (gWatched) return;
    gWatched = tv;
    gMarked = [NSMutableArray array];
    gMarkOriginal = method_setImplementation(
        class_getInstanceMethod(NSView.class, @selector(setNeedsDisplayInRect:)), (IMP)MarkRecord);
    gMarkLayoutOriginal = method_setImplementation(
        class_getInstanceMethod(NSTextView.class,
                                @selector(setNeedsDisplayInRect:avoidAdditionalLayout:)),
        (IMP)MarkLayoutRecord);
}
NSArray<NSValue *> *StopMarks(void) {
    if (!gWatched) return @[];
    method_setImplementation(
        class_getInstanceMethod(NSView.class, @selector(setNeedsDisplayInRect:)), gMarkOriginal);
    method_setImplementation(
        class_getInstanceMethod(NSTextView.class,
                                @selector(setNeedsDisplayInRect:avoidAdditionalLayout:)),
        gMarkLayoutOriginal);
    gWatched = nil;
    return gMarked;
}

// A long page shaped like the owner's STATUS.md: headings with their
// rules, tables, numbered lists and paragraphs, `revision` at its end.
NSString *StatusDoc(int revision) {
    NSMutableString *s = [NSMutableString stringWithString:@"# Work status\n\n"];
    for (int k = 0; k < 30; k++) {
        [s appendFormat:@"## Section %d\n\nA paragraph about section %d, long enough to wrap "
                         "once or twice at a narrow width but not at the page's measure.\n\n",
                        k, k];
        if (k % 3 == 0)
            [s appendString:@"| Work | Who | State |\n|---|---|---|\n| a change | an agent | "
                             "building |\n| another | the owner | waiting |\n\n"];
        [s appendString:@"1. first\n3. second\n4. third\n\n"];
    }
    [s appendFormat:@"Revision %d%@.\n", revision,
                    [@"" stringByPaddingToLength:(NSUInteger)revision withString:@"!"
                                 startingAtIndex:0]];
    return s;
}

// Whole-file replace, as Python's open(path, 'w') does: the same file,
// cut to nothing, then written.
void RewriteInPlace(NSString *path, NSString *text) {
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
    [h truncateFileAtOffset:0];
    [h writeData:[text dataUsingEncoding:NSUTF8StringEncoding]];
    [h closeFile];
}

// Written beside it, then renamed over it.
void RewriteByRename(NSString *path, NSString *text) {
    NSString *tmp = [path stringByAppendingString:@".tmp"];
    [text writeToFile:tmp atomically:NO encoding:NSUTF8StringEncoding error:nil];
    rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation);
}

// The pane holds one page: the scroll view's document is the editor's
// text view, it has no subviews it did not have before, no other text
// view shows over the editor's slot, and the page is centered at its
// measure.
void CheckOnePage(EditorController *c, NSUInteger subviewsBefore, NSString *when) {
    NSTextView *tv = [c valueForKey:@"textView"];
    NSScrollView *sv = c.editorScroll;
    GCHECK(sv.documentView == tv && !sv.hidden);
    GCHECK(sv.contentView.subviews.count == 1);
    GCHECK(tv.subviews.count == subviewsBefore);
    const NSRect slot = [sv convertRect:sv.bounds toView:nil];
    __block NSUInteger others = 0;
    __block void (^walk)(NSView *) = nil;
    void (^visit)(NSView *) = ^(NSView *v) {
        if (v.hidden || v.alphaValue < 0.01) return;
        if (v != tv && [v isKindOfClass:NSTextView.class] && NSWidth(v.visibleRect) > 0 &&
            NSIntersectsRect([v convertRect:v.visibleRect toView:nil], slot))
            others++;
        for (NSView *sub in v.subviews) walk(sub);
    };
    walk = visit;
    visit(c.window.contentView);
    walk = nil;
    NSTextContainer *tc = tv.textContainer;
    const double most = PageMeasure::maxTextWidth(
        [AppSettings shared].settings.markdownMaxWidth(), MCZoomScale());
    const CGFloat inset = PageMeasure::centeredInset(NSWidth(tv.frame), 8, most,
                                                     tc.lineFragmentPadding);
    const CGFloat left = tv.textContainerOrigin.x;
    const CGFloat right = NSWidth(tv.frame) - left - tc.size.width;
    MCGuiNote(@"%@: frame %.0f wide, inset %.1f (want %.1f), container %.0f, margins %.1f and "
              "%.1f, %lu other text views over the slot, %lu subviews",
              when, NSWidth(tv.frame), tv.textContainerInset.width, inset, tc.size.width, left,
              right, (unsigned long)others, (unsigned long)tv.subviews.count);
    GCHECK(others == 0);
    GCHECK(fabs(tv.textContainerInset.width - inset) < 0.5);
    GCHECK(inset > 100);   // a wide pane: the page has margins to leave ink in
    GCHECK(fabs(left - right) < 1);
}

// Every mark since StartMarks: one of them covers the whole view, so no
// tile the layer kept, on screen or off it, keeps the older page. The
// view's height may have moved since (layout below the pane goes on),
// so "whole" is its full width from its top to well past a screenful
// below the pane; marks of what shows alone stop at the pane.
// The app going to the background and coming back, as AppKit says so.
void SwitchAppAndBack(void) {
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc postNotificationName:NSApplicationWillResignActiveNotification object:NSApp];
    [nc postNotificationName:NSApplicationDidResignActiveNotification object:NSApp];
    [nc postNotificationName:NSApplicationWillBecomeActiveNotification object:NSApp];
    [nc postNotificationName:NSApplicationDidBecomeActiveNotification object:NSApp];
}

// A press on the page at `at` (window coordinates), as a mouse-down of
// `clicks` clicks is handed to `view` (a window never on screen gets no
// mouse-down through sendEvent:).
void PressAt(NSView *view, NSPoint at, NSInteger clicks) {
    NSEvent *down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:at
                                  modifierFlags:0
                                      timestamp:NSProcessInfo.processInfo.systemUptime
                                   windowNumber:view.window.windowNumber context:nil
                                    eventNumber:0 clickCount:clicks pressure:1];
    [view mouseDown:down];
}

// Where a word of the page on show is, in window coordinates.
NSPoint WindowPointOf(EditorController *c, NSString *word) {
    NSTextView *tv = [c valueForKey:@"textView"];
    const NSRange r = [tv.string rangeOfString:word];
    if (r.location == NSNotFound) return NSMakePoint(-1, -1);
    const NSRect box = [c textRectForCharacter:r.location + 1];
    return [tv convertPoint:NSMakePoint(NSMidX(box), NSMidY(box)) toView:nil];
}

NSTextView *EditBox(EditorController *c) {
    return [[c valueForKey:@"markdownEditPanel"] valueForKey:@"_editor"];
}

void CheckWholeViewMarked(NSTextView *tv, NSString *when) {
    NSArray<NSValue *> *marked = StopMarks();
    const NSRect vis = tv.visibleRect;
    const NSRect want = NSMakeRect(0, 0, NSWidth(tv.bounds),
                                   MIN(NSHeight(tv.bounds), NSMaxY(vis) + NSHeight(vis)));
    NSRect biggest = NSZeroRect;
    for (NSValue *v in marked)
        if (NSHeight(v.rectValue) * NSWidth(v.rectValue) > NSHeight(biggest) * NSWidth(biggest))
            biggest = v.rectValue;
    MCGuiNote(@"%@: %lu rects marked, the largest %@; the view %@, showing %@", when,
              (unsigned long)marked.count, NSStringFromRect(biggest), NSStringFromRect(tv.bounds),
              NSStringFromRect(vis));
    GCHECK(NSMinY(vis) > 0);   // scrolled down, or this means nothing
    GCHECK(AnyHolds(marked, want));
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
    // A page another program rewrites while it is scrolled down, in a
    // wide window, with the window and the sidebar resized between.
    NSString *status = [dir stringByAppendingPathComponent:@"STATUS.md"];
    __block CGFloat keptY = 0;
    __block NSUInteger subviews = 0;
    MCGuiStep(@"a long page, scrolled near its end, in a wide window", ^{
        if ([c sidePreviewShowing]) [c toggleSidePreview:nil];
        MCEditorZoomBy(0);
        MCGuiWriteFile(status, StatusDoc(1));
        NSRect fr = c.window.frame;
        fr.size = NSMakeSize(2300, 1200);
        [c.window setFrame:fr display:NO];
        GCHECK([c showFile:status]);
        if (!c.previewMode) [c togglePreview:nil];
        GCHECK(c.previewMode);
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK([tv.string containsString:@"Revision 1"]);
        [tv.layoutManager ensureLayoutForTextContainer:tv.textContainer];
        NSClipView *clip = c.editorScroll.contentView;
        keptY = NSHeight(tv.frame) - NSHeight(clip.bounds) - 300;
        GCHECK(keptY > 2000);
        [clip scrollToPoint:NSMakePoint(0, keptY)];
        [c.editorScroll reflectScrolledClipView:clip];
        subviews = tv.subviews.count;
        CheckOnePage(c, subviews, @"before");
        StartMarks(tv);
        RewriteInPlace(status, StatusDoc(2));
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"the page rewritten in place, read again", 5, ^BOOL {
        return [[[c valueForKey:@"textView"] string] containsString:@"Revision 2"];
    });
    // Each check after a reload: the place kept, one page, all of it marked.
    void (^afterReload)(NSString *) = ^(NSString *when) {
        NSTextView *tv = [c valueForKey:@"textView"];
        const CGFloat y = c.editorScroll.contentView.bounds.origin.y;
        MCGuiNote(@"%@: scrolled to %.0f, was %.0f", when, y, keptY);
        GCHECK(fabs(y - keptY) < 2);   // where it was, not back at the top
        CheckOnePage(c, subviews, when);
        CheckWholeViewMarked(tv, when);
        MCGuiSnapshot(c.window.contentView, [@"markdown-page-" stringByAppendingString:
            [when stringByReplacingOccurrencesOfString:@" " withString:@"-"]]);
    };
    MCGuiStep(@"it stayed where it was, one page, drawn again whole", ^{
        afterReload(@"rewritten in place");
        StartMarks([c valueForKey:@"textView"]);
        RewriteByRename(status, StatusDoc(3));
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"the page renamed over, read again", 5, ^BOOL {
        return [[[c valueForKey:@"textView"] string] containsString:@"Revision 3"];
    });
    MCGuiStep(@"renamed over: the same", ^{
        afterReload(@"renamed over");
        // Narrower, then wide again: the page moves sideways each time.
        NSTextView *tv = [c valueForKey:@"textView"];
        NSRect fr = c.window.frame;
        fr.size.width = 1300;
        [c.window setFrame:fr display:NO];
        [c.window layoutIfNeeded];
        StartMarks(tv);
        fr.size.width = 2300;
        [c.window setFrame:fr display:NO];
        [c.window layoutIfNeeded];
        CheckOnePage(c, subviews, @"window widened");
        CheckWholeViewMarked(tv, @"window widened");
        // The sidebar narrower: the pane widens and the page recenters.
        const CGFloat side = NSWidth(c.splitView.subviews.firstObject.frame);
        StartMarks(tv);
        [c.splitView setPosition:MAX(170, side - 80) ofDividerAtIndex:0];
        [c.window layoutIfNeeded];
        MCGuiNote(@"sidebar %.0f, now %.0f", side, NSWidth(c.splitView.subviews.firstObject.frame));
        CheckOnePage(c, subviews, @"sidebar resized");
        CheckWholeViewMarked(tv, @"sidebar resized");
        [c.splitView setPosition:side ofDividerAtIndex:0];
        // Read again after the resizes, in place once more.
        keptY = c.editorScroll.contentView.bounds.origin.y;
        GCHECK(keptY > 2000);
        StartMarks(tv);
        RewriteInPlace(status, StatusDoc(4));
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"the page read again after the resizes", 5, ^BOOL {
        return [[[c valueForKey:@"textView"] string] containsString:@"Revision 4"];
    });
    MCGuiStep(@"after the resizes: the same", ^{
        afterReload(@"read after resizes");
        // A numbered list written 1. 3. 4. counts 1, 2, 3.
        NSTextView *tv = [c valueForKey:@"textView"];
        GCHECK([tv.string containsString:@"\t2.\tsecond"] && [tv.string containsString:@"\t3.\tthird"]);
        GCHECK(![tv.string containsString:@"\t4.\tthird"]);
    });
    // The edit box on the page (D2 on 1.6.3 and 1.6.4): switching to
    // another app and back closed it and lost what was typed; its button
    // said Save but only applied the edit.
    NSString *fix = [dir stringByAppendingPathComponent:@"fix.md"];
    NSString *const fixText = @"A paragraph with a wrod to fix.\n\nAnother paragraph here.\n";
    MCGuiStep(@"what is typed in the page's edit box survives a switch to another app", ^{
        StopMarks();
        MCGuiWriteFile(fix, fixText);
        GCHECK([c showFile:fix]);
        if (!c.previewMode) [c togglePreview:nil];
        GCHECK(c.previewMode);
        NSTextView *tv = [c valueForKey:@"textView"];
        const NSRange word = [tv.string rangeOfString:@"wrod"];
        GCHECK(word.location != NSNotFound);
        if (word.location == NSNotFound) return;
        const NSRect r = [c textRectForCharacter:word.location + 1];
        GCHECK([c editMarkdownAtCharacter:word.location + 1
                                    point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        MCMarkdownEditPanel *panel = [c valueForKey:@"markdownEditPanel"];
        NSPopover *pop = [panel valueForKey:@"_popover"];
        // A transient popover closes when the app loses the keyboard.
        MCGuiNote(@"popover behavior %ld", (long)pop.behavior);
        GCHECK(pop.behavior == NSPopoverBehaviorSemitransient);
        // It says what Return does: the edit goes into the document.
        NSMutableArray<NSString *> *buttons = [NSMutableArray array];
        for (NSView *v in pop.contentViewController.view.subviews)
            if ([v isKindOfClass:NSButton.class]) [buttons addObject:((NSButton *)v).title];
        NSString *label = nil;
        for (NSView *v in pop.contentViewController.view.subviews)
            if ([v isKindOfClass:NSTextField.class]) label = ((NSTextField *)v).stringValue;
        MCGuiNote(@"buttons %@, title \"%@\"", buttons, label);
        GCHECK([buttons containsObject:@"Apply"] && ![buttons containsObject:@"Save"]);
        GCHECK([label containsString:@"Return applies"] && ![label containsString:@"saves"]);
        NSTextView *editor = EditBox(c);
        GCHECK_EQ_STR(editor.string, @"A paragraph with a wrod to fix.");
        [editor insertText:@"word" replacementRange:[editor.string rangeOfString:@"wrod"]];
        GCHECK_EQ_STR(editor.string, @"A paragraph with a word to fix.");
        SwitchAppAndBack();
        // Still open with the text, or applied: never lost.
        const BOOL kept = panel.edited && [EditBox(c).string isEqualToString:
                                                @"A paragraph with a word to fix."];
        const BOOL applied = [c.sourceText containsString:@"a word to fix"];
        MCGuiNote(@"after the switch: kept %d, applied %d", kept, applied);
        GCHECK(kept || applied);
        GCHECK(kept);   // semi-transient: AppKit leaves it open
        // Return applies it into the document, which is then unsaved.
        MCGuiUndoGroup([[c valueForKey:@"textView"] undoManager], ^{
            [panel performSelector:@selector(save:) withObject:nil];
        });
        GCHECK([c.sourceText containsString:@"A paragraph with a word to fix."]);
        GCHECK(!panel.edited);
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:fix encoding:NSUTF8StringEncoding error:nil], fixText);   // applied, not saved
    });
    MCGuiStep(@"a box closed by anything but Esc applies what was typed, one undo step", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        NSString *before = [c.sourceText copy];
        const NSRange word = [tv.string rangeOfString:@"Another"];
        GCHECK(word.location != NSNotFound);
        if (word.location == NSNotFound) return;
        NSRect r = [c textRectForCharacter:word.location];
        GCHECK([c editMarkdownAtCharacter:word.location point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        MCMarkdownEditPanel *panel = [c valueForKey:@"markdownEditPanel"];
        EditBox(c).string = @"Another paragraph, changed.";
        // AppKit closing it on its own (a click elsewhere in the window).
        NSPopover *pop = [panel valueForKey:@"_popover"];
        MCGuiUndoGroup(tv.undoManager, ^{
            [panel performSelector:@selector(popoverWillClose:)
                        withObject:[NSNotification notificationWithName:NSPopoverWillCloseNotification
                                                                 object:pop]];
        });
        GCHECK([c.sourceText containsString:@"Another paragraph, changed."]);
        [tv.undoManager undo];
        GCHECK_EQ_STR(c.sourceText, before);
        // Esc (close) drops it, as asked.
        const NSRange again = [tv.string rangeOfString:@"Another"];
        r = [c textRectForCharacter:again.location];
        GCHECK([c editMarkdownAtCharacter:again.location point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        EditBox(c).string = @"Dropped on purpose.";
        [panel close];
        GCHECK_EQ_STR(c.sourceText, before);
        // A save, a close or a quit asks about it with the rest
        // (commitPageEdit): applied first.
        r = [c textRectForCharacter:again.location];
        GCHECK([c editMarkdownAtCharacter:again.location point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        EditBox(c).string = @"Another paragraph, kept for the save.";
        MCGuiUndoGroup(tv.undoManager, ^{ [c commitPageEdit]; });
        GCHECK([c.sourceText containsString:@"Another paragraph, kept for the save."]);
        GCHECK(!panel.edited);
    });
    // An agent rewrites the file while the box is open (round 10's
    // documents defect 3, checked here as on the LaTeX page): the clean
    // tab reads the rewrite in, and Apply puts the edit where its block is
    // now, keeping the rewrite; when the rewrite changed that block, the
    // edit goes to the clipboard and the rewrite stays.
    NSString *agentDoc = [dir stringByAppendingPathComponent:@"agent.md"];
    NSString *const agentText = @"# Notes\n\nThe first paragraph.\n\nA paragraph with a wrod.\n";
    __block NSString *rewrite = nil;
    MCGuiStep(@"the box open on a paragraph, an agent rewrites the file", ^{
        MCGuiWriteFile(agentDoc, agentText);
        GCHECK([c showFile:agentDoc]);
        if (!c.previewMode) [c togglePreview:nil];
        GCHECK(c.previewMode && !c.dirty);
        NSTextView *tv = [c valueForKey:@"textView"];
        const NSRange word = [tv.string rangeOfString:@"wrod"];
        GCHECK(word.location != NSNotFound);
        if (word.location == NSNotFound) return;
        const NSRect r = [c textRectForCharacter:word.location + 1];
        GCHECK([c editMarkdownAtCharacter:word.location + 1 point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        GCHECK_EQ_STR(EditBox(c).string, @"A paragraph with a wrod.");
        [EditBox(c) insertText:@"word" replacementRange:[EditBox(c).string rangeOfString:@"wrod"]];
        rewrite = @"# Notes\n\nAn agent added this line.\n\nThe first paragraph, rewritten.\n\n"
                  @"A paragraph with a wrod.\n\nAnd a closing line.\n";
        MCGuiWriteFile(agentDoc, rewrite);
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"the clean tab reads the rewrite, the box still open", 10, ^BOOL {
        return [c.sourceText isEqualToString:rewrite];
    });
    MCGuiStep(@"Apply: the edit lands in its paragraph, the agent's lines kept", ^{
        MCMarkdownEditPanel *panel = [c valueForKey:@"markdownEditPanel"];
        GCHECK(panel.edited);
        MCGuiUndoGroup([[c valueForKey:@"textView"] undoManager], ^{
            [panel performSelector:@selector(save:) withObject:nil];
        });
        MCGuiNote(@"text now: %@", c.sourceText);
        GCHECK_EQ_STR(c.sourceText, [rewrite stringByReplacingOccurrencesOfString:@"wrod"
                                                                       withString:@"word"]);
        // Saved, so the tab follows the disk again for the next step.
        rewrite = [c.sourceText copy];
        [c saveCurrentFile:nil];
        GCHECK(!c.dirty);
        GCHECK_EQ_STR([NSString stringWithContentsOfFile:agentDoc encoding:NSUTF8StringEncoding
                                                   error:nil], rewrite);
    });
    MCGuiStep(@"the box open again, the agent rewrites that very paragraph", ^{
        GCHECK(!c.dirty);
        NSTextView *tv = [c valueForKey:@"textView"];
        const NSRange word = [tv.string rangeOfString:@"closing"];
        GCHECK(word.location != NSNotFound);
        if (word.location == NSNotFound) return;
        const NSRect r = [c textRectForCharacter:word.location + 1];
        GCHECK([c editMarkdownAtCharacter:word.location + 1 point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        GCHECK_EQ_STR(EditBox(c).string, @"And a closing line.");
        [EditBox(c) insertText:@"final" replacementRange:[EditBox(c).string rangeOfString:@"closing"]];
        rewrite = [rewrite stringByReplacingOccurrencesOfString:@"And a closing line."
                                                     withString:@"The agent's last line."];
        MCGuiWriteFile(agentDoc, rewrite);
        [c checkTabsOnDisk];
    });
    MCGuiWait(@"(read in)", 10, ^BOOL { return [c.sourceText isEqualToString:rewrite]; });
    MCGuiStep(@"Apply changes nothing: the rewrite stays, the draft is on the clipboard", ^{
        MCMarkdownEditPanel *panel = [c valueForKey:@"markdownEditPanel"];
        [panel performSelector:@selector(save:) withObject:nil];
        GCHECK_EQ_STR(c.sourceText, rewrite);
        GCHECK(!c.dirty);
        GCHECK_EQ_STR([NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString],
                      @"And a final line.");
        NSString *note = [[c valueForKey:@"settingsLabel"] stringValue];
        MCGuiNote(@"status: %@", note);
        GCHECK([note containsString:@"your edit was not applied"]);
        GCHECK([c closeTab:[c valueForKey:@"activeTab"] prompt:NO]);
    });
    // Two editor groups, a page in each: a double-click in the group
    // without the keyboard opens the box on the first try.
    NSString *left = [dir stringByAppendingPathComponent:@"left.md"];
    NSString *right = [dir stringByAppendingPathComponent:@"right.md"];
    MCGuiStep(@"two groups: a double-click on the other group's page opens the box at once", ^{
        MCGuiWriteFile(left, @"Left page with a wrod.\n");
        MCGuiWriteFile(right, @"Right page with a recieve.\n");
        [c openFileAtPath:left from:EditorTabs::OpenFrom::Other];
        if (!c.previewMode) [c togglePreview:nil];
        [c splitEditor:nil];   // left.md in both, the right group focused
        GCHECK([c editorSplit] && [c focusedGroup] == 1);
        [c openFileAtPath:right from:EditorTabs::OpenFrom::Other];
        if (!c.previewMode) [c togglePreview:nil];
        // Where left.md's word is on its page, in the left group.
        [c focusFirstEditorGroup:nil];
        GCHECK([c focusedGroup] == 0 && c.previewMode);
        const NSPoint at = WindowPointOf(c, @"wrod");
        [c focusSecondEditorGroup:nil];
        GCHECK([c focusedGroup] == 1 && c.previewMode);
        MCEditorMirror *mirror = [c valueForKey:@"mirror"];
        GCHECK(mirror.showsPicture);   // left.md's page, as a picture
        GCHECK(at.x >= 0);
        if (!mirror.showsPicture || at.x < 0) return;
        // The press arrives with two clicks (the first went to making the
        // window key, or the group): the box opens on the word, at once.
        [c setValue:nil forKey:@"markdownEditPanel"];
        PressAt(mirror.pictureView, at, 2);
        MCGuiNote(@"group %ld, box \"%@\"", (long)[c focusedGroup], EditBox(c).string);
        GCHECK([c focusedGroup] == 0);
        GCHECK_EQ_STR(EditBox(c).string, @"Left page with a wrod.");
        [[c valueForKey:@"markdownEditPanel"] close];
    });
    MCGuiStep(@"two groups: one click, then the second, in the other group's page", ^{
        // Where right.md's word is, in the right group.
        [c focusSecondEditorGroup:nil];
        const NSPoint at = WindowPointOf(c, @"recieve");
        [c focusFirstEditorGroup:nil];
        MCEditorMirror *mirror = [c valueForKey:@"mirror"];
        GCHECK(mirror.showsPicture && at.x >= 0);
        if (!mirror.showsPicture || at.x < 0) return;
        [c setValue:nil forKey:@"markdownEditPanel"];
        PressAt(mirror.pictureView, at, 1);   // the group takes the keyboard
        GCHECK([c focusedGroup] == 1);
        // The second click lands on the page itself, now under the pointer.
        NSTextView *tv = [c valueForKey:@"textView"];
        const NSPoint p = [tv convertPoint:at fromView:nil];
        const NSUInteger i = [tv characterIndexForInsertionAtPoint:p];
        const NSRange word = [tv.string rangeOfString:@"recieve"];
        MCGuiNote(@"the second click at character %lu, the word at %@", (unsigned long)i,
                  NSStringFromRange(word));
        // (Only sent where it opens a box: a double-click that did not would
        // start the text view's own tracking, waiting for a release.)
        const BOOL onWord = c.previewMode && word.location != NSNotFound &&
                            i >= word.location && i <= NSMaxRange(word);
        GCHECK(onWord);
        if (!onWord) return;
        PressAt(tv, at, 2);
        GCHECK_EQ_STR(EditBox(c).string, @"Right page with a recieve.");
        [[c valueForKey:@"markdownEditPanel"] close];
    });
    MCGuiStep(@"the window goes", ^{
        StopMarks();
        MCEditorZoomBy(0);
        if ([c sidePreviewShowing]) [c toggleSidePreview:nil];
        MCGuiSetKeyWindow(nil);
        [c.window close];
    });
}
