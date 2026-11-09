// PageMeasureSuite.mm — three of the owner's small things (2026-11-08).
//
// `make guitest SUITES=hints_panel`: the shortcut hints (Shift+Cmd+H) fit
// the window at every size and zoom: two columns of tab stops when they
// fit side by side, else one, never a row broken in the middle, and a
// panel no taller than the window's room that scrolls to its last row.
//
// `make guitest SUITES=markdown_width`: a rendered Markdown page's lines
// keep to markdown.max-width at the zoom, centered in a wider pane, its
// quotes, code boxes and tables inside the same measure (no ink outside
// it), in the full preview and the one to the side; a narrow pane, a high
// zoom and `off` use the whole width, and the source keeps its own inset.
//
// `make guitest SUITES=swatch_escape`: Esc closes the color panel a
// settings swatch opened, from the settings file or the panel, and its
// picks stop; Esc elsewhere leaves it open.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "AppSettings.h"
#import "Appearance.h"
#import "ControlServer.h"
#import "EditorFont.h"
#import "MarkdownSidePreview.h"
#include "PageMeasure.h"
#include "Settings.h"

namespace {

NSString *const kHarnessSettings = @"lsp.enabled = false\n";

void Resize(EditorController *c, CGFloat w, CGFloat h) {
    NSRect fr = c.window.frame;
    fr.size = NSMakeSize(w, h);
    [c.window setFrame:fr display:NO];
    [c.window.contentView layoutSubtreeIfNeeded];
}

// Every paragraph of the hints (a row, or a heading) is laid out on one
// line fragment: nothing wrapped mid-row. Returns the paragraphs that did
// not, as text, for the note.
NSArray<NSString *> *WrappedRows(NSTextView *tv) {
    NSLayoutManager *lm = tv.layoutManager;
    [lm ensureLayoutForTextContainer:tv.textContainer];
    NSString *s = tv.string;
    NSMutableArray *wrapped = [NSMutableArray array];
    [s enumerateSubstringsInRange:NSMakeRange(0, s.length)
                          options:NSStringEnumerationByParagraphs
                       usingBlock:^(NSString *para, NSRange r, NSRange, BOOL *) {
        if (!r.length) return;
        NSRange glyphs = [lm glyphRangeForCharacterRange:r actualCharacterRange:NULL];
        __block int fragments = 0;
        [lm enumerateLineFragmentsForGlyphRange:glyphs
                                     usingBlock:^(NSRect, NSRect, NSTextContainer *, NSRange, BOOL *) {
            fragments++;
        }];
        if (fragments != 1) [wrapped addObject:para];
    }];
    return wrapped;
}

// The panel lies inside the window's room: under the title bar, above the
// status bar, inside the sides.
BOOL PanelFits(EditorController *c) {
    NSView *panel = [c valueForKey:@"hintsPanel"];
    NSView *box = panel.superview;
    const NSRect f = panel.frame;
    return NSMinX(f) >= 0 && NSMaxX(f) <= NSWidth(box.bounds) && NSMinY(f) >= 24 &&
           NSMaxY(f) <= NSHeight(box.bounds) - [c titlebarHeight] + 0.5;
}

// The hints panel at this size and zoom: in its room, no row wrapped, the
// columns expected (0: either), and its last row reachable by scrolling.
void CheckHints(EditorController *c, NSString *name, NSInteger columns) {
    NSView *panel = [c valueForKey:@"hintsPanel"];
    NSScrollView *scroll = [c valueForKey:@"hintsScroll"];
    NSTextView *tv = [c valueForKey:@"hintsTextView"];
    MCGuiNote(@"%@: panel %@ in %@, %ld columns, text %@ in clip %@", name,
              NSStringFromRect(panel.frame), NSStringFromRect(panel.superview.bounds),
              (long)c.hintsColumns, NSStringFromSize(tv.frame.size),
              NSStringFromSize(scroll.contentView.bounds.size));
    GCHECK(!panel.hidden);
    GCHECK(PanelFits(c));
    NSArray *wrapped = WrappedRows(tv);
    if (wrapped.count) MCGuiNote(@"wrapped: %@", wrapped);
    GCHECK(wrapped.count == 0);
    if (columns) GCHECK(c.hintsColumns == columns);
    // The text is all there, and the last row can be scrolled to.
    GCHECK([tv.string containsString:@"Start of the file"]);
    GCHECK([tv.string hasSuffix:@"Hide these hints"]);
    const CGFloat docH = NSHeight(tv.frame), clipH = NSHeight(scroll.contentView.bounds);
    [tv scrollRangeToVisible:NSMakeRange(tv.string.length, 0)];
    const NSRect visible = scroll.contentView.documentVisibleRect;
    GCHECK(NSMaxY(visible) >= docH - 1);
    if (docH > clipH + 1) {
        GCHECK(scroll.hasVerticalScroller);
        GCHECK(NSMinY(visible) > 0);   // it did scroll
    }
    // No text wider than the panel's text area.
    NSLayoutManager *lm = tv.layoutManager;
    const NSRect used = [lm usedRectForTextContainer:tv.textContainer];
    GCHECK(NSMaxX(used) <= tv.textContainer.size.width + 0.5);
    GCHECK(tv.textContainerOrigin.x + NSMaxX(used) <= NSWidth(scroll.contentView.bounds) + 0.5);
    [tv scrollRangeToVisible:NSMakeRange(0, 0)];
    MCGuiSnapshot(c.window.contentView, [@"hints_" stringByAppendingString:name]);
}

// The tab stops of the hints' first shortcut row.
NSArray<NSTextTab *> *RowStops(NSTextView *tv) {
    NSString *s = tv.string;
    NSRange r = [s rangeOfString:@"New file"];
    if (r.location == NSNotFound) return @[];
    NSParagraphStyle *ps = [tv.textStorage attribute:NSParagraphStyleAttributeName
                                             atIndex:r.location effectiveRange:NULL];
    return ps.tabStops ?: @[];
}

// Non-transparent pixels of `view` in the columns [x0, x1), as drawn.
NSUInteger InkIn(NSView *view, CGFloat x0, CGFloat x1) {
    NSRect visible = view.visibleRect;
    NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:visible];
    [view cacheDisplayInRect:visible toBitmapImageRep:rep];
    const CGFloat scale = rep.pixelsWide / NSWidth(visible);
    NSUInteger ink = 0;
    const NSInteger from = (NSInteger)floor((x0 - NSMinX(visible)) * scale);
    const NSInteger to = (NSInteger)ceil((x1 - NSMinX(visible)) * scale);
    for (NSInteger y = 0; y < rep.pixelsHigh; y += 2)
        for (NSInteger x = MAX(0, from); x < MIN(rep.pixelsWide, to); x++)
            if ([rep colorAtX:x y:y].alphaComponent > 0.1) ink++;
    return ink;
}

NSString *LongParagraph(void) {
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < 60; i++)
        [s appendFormat:@"word%d is part of a paragraph long enough to wrap many times ", i];
    return s;
}

// The page's measure in `tv`: the text width, and whether the page sits
// centered (equal room either side) in the view.
CGFloat TextWidth(NSTextView *tv) {
    return tv.textContainer.size.width - 2 * tv.textContainer.lineFragmentPadding;
}
BOOL Centered(NSTextView *tv) {
    return fabs(2 * tv.textContainerInset.width + tv.textContainer.size.width - NSWidth(tv.frame)) <= 1;
}

// Everything laid out in `tv` (paragraphs, quotes, code boxes, tables)
// keeps inside the text container's width.
BOOL InsideMeasure(NSTextView *tv) {
    NSLayoutManager *lm = tv.layoutManager;
    [lm ensureLayoutForTextContainer:tv.textContainer];
    __block BOOL inside = YES;
    const CGFloat w = tv.textContainer.size.width;
    [lm enumerateLineFragmentsForGlyphRange:NSMakeRange(0, lm.numberOfGlyphs)
                                 usingBlock:^(NSRect rect, NSRect used, NSTextContainer *, NSRange, BOOL *stop) {
        if (NSMinX(rect) < -0.5 || NSMaxX(used) > w + 0.5) {
            MCGuiNote(@"outside the measure: %@ (used %@) in %.1f", NSStringFromRect(rect),
                      NSStringFromRect(used), w);
            inside = NO;
            *stop = YES;
        }
    }];
    return inside;
}

// Whether a color chosen in the shared color panel now reaches the
// settings file on show (colorPicked: rewrites the swatch's line).
BOOL PickReaches(EditorController *c, CGFloat red) {
    NSColorPanel *panel = [NSColorPanel sharedColorPanel];
    panel.color = [NSColor colorWithSRGBRed:red green:0.2 blue:0.4 alpha:1];
    NSString *hex = [NSString stringWithFormat:@"#%02X3366", (int)lround(red * 255)];
    NSTextView *tv = [c valueForKey:@"textView"];
    return [tv.string containsString:hex];
}

}  // namespace

// ------------------------------------------------------------ hints_panel
MC_GUI_SUITE(hints_panel) {
    __block EditorController *c = nil;

    MCGuiStep(@"a window of the suite's own, the hints shown", ^{
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:MCGuiProject()];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        MCEditorZoomBy(0);
        Resize(c, 1400, 900);
        [c toggleHints:nil];
        GCHECK(c.hintsVisible);
    });
    MCGuiStep(@"wide: two columns on tab stops, nothing wrapped, in the window", ^{
        CheckHints(c, @"1400x900", 2);
        NSTextView *tv = [c valueForKey:@"hintsTextView"];
        NSArray<NSTextTab *> *stops = RowStops(tv);
        GCHECK(stops.count == 3);
        // Aligned by stops, never spaces: no run of two spaces in a row.
        GCHECK(![tv.string containsString:@"  ("] || [tv.string containsString:@"(also"]);
        GCHECK([tv.string rangeOfString:@"\t"].location != NSNotFound);
    });
    MCGuiStep(@"short: the panel stops at the window's room and scrolls", ^{
        Resize(c, 1400, 500);
        CheckHints(c, @"1400x500", 2);
        NSScrollView *scroll = [c valueForKey:@"hintsScroll"];
        NSTextView *tv = [c valueForKey:@"hintsTextView"];
        GCHECK(NSHeight(tv.frame) > NSHeight(scroll.contentView.bounds) + 50);
    });
    MCGuiStep(@"narrow: one column, a shortcut a row", ^{
        Resize(c, 460, 700);
        CheckHints(c, @"460x700", 1);
        NSTextView *tv = [c valueForKey:@"hintsTextView"];
        GCHECK(RowStops(tv).count == 2);
        // The pair "New file | Open folder" is two rows now.
        NSRange nf = [tv.string rangeOfString:@"New file"];
        NSRange of = [tv.string rangeOfString:@"Open folder"];
        GCHECK(nf.location != NSNotFound && of.location != NSNotFound);
        if (nf.location != NSNotFound && of.location != NSNotFound) {
            NSString *between = [tv.string substringWithRange:
                NSMakeRange(NSMaxRange(nf), of.location - NSMaxRange(nf))];
            GCHECK([between containsString:@"\n"]);
        }
    });
    MCGuiStep(@"zoomed in: the text grows and still fits, one or two columns", ^{
        Resize(c, 1400, 900);
        MCEditorZoomBy(6);
        CheckHints(c, @"1400x900_zoom6", 0);
        NSTextView *tv = [c valueForKey:@"hintsTextView"];
        NSFont *f = [tv.textStorage attribute:NSFontAttributeName
                                      atIndex:[tv.string rangeOfString:@"New file"].location
                               effectiveRange:NULL];
        GCHECK(f.pointSize >= 17);
    });
    MCGuiStep(@"zoomed far in on a small window: one column, wrapped under the labels", ^{
        MCEditorZoomBy(14);
        Resize(c, 520, 600);
        NSTextView *tv = [c valueForKey:@"hintsTextView"];
        GCHECK(PanelFits(c));
        GCHECK(c.hintsColumns == 1);
        MCGuiSnapshot(c.window.contentView, @"hints_520x600_zoom20");
        // A row that wraps continues at its label's stop, not under the key.
        NSParagraphStyle *ps = [tv.textStorage attribute:NSParagraphStyleAttributeName
            atIndex:[tv.string rangeOfString:@"New file"].location effectiveRange:NULL];
        GCHECK(ps.headIndent > 0 && ps.tabStops.count >= 1 &&
               fabs(ps.headIndent - ps.tabStops.firstObject.location) < 0.5);
    });
    MCGuiStep(@"the window goes", ^{
        MCEditorZoomBy(0);
        [c toggleHints:nil];
        MCGuiSetKeyWindow(nil);
        [c.window performClose:nil];
    });
}

// --------------------------------------------------------- markdown_width
MC_GUI_SUITE(markdown_width) {
    __block EditorController *c = nil;
    // A folder of its own, so the window is new: nothing earlier suites
    // left open (a browser, a terminal) narrows its editor.
    NSString *dir = MCGuiProjectFile(@"measure");
    NSString *md = [dir stringByAppendingPathComponent:@"measure.md"];
    NSString *text = [NSString stringWithFormat:
        @"# A heading over the page\n\n%@\n\n> %@\n\n```\n%@\n```\n\n"
        @"| name | what it is |\n|---|---|\n| one | %@ |\n\n---\n\nThe end.\n",
        LongParagraph(), LongParagraph(), [@"code " stringByPaddingToLength:900 withString:@"code "
                                                           startingAtIndex:0],
        LongParagraph()];

    MCGuiStep(@"a wide window shows the page centered at markdown.max-width", ^{
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES
                                                   attributes:nil error:nil];
        MCGuiWriteFile(md, text);
        MCGuiSetSettings(kHarnessSettings);
        [(id<MCControlAppHost>)NSApp.delegate controlOpenWindowAtPath:dir];
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        MCEditorZoomBy(0);
        Resize(c, 2000, 900);
        GCHECK([c showFile:md]);
        GCHECK(c.previewMode);
        NSTextView *tv = [c valueForKey:@"textView"];
        MCGuiNote(@"window %@, view %.0f, inset %.1f, text %.1f, browser %d, terminal %d",
                  NSStringFromRect(c.window.frame), NSWidth(tv.frame),
                  tv.textContainerInset.width, TextWidth(tv), c.browserVisible, c.terminalVisible);
        GCHECK(NSWidth(tv.frame) > 1200);
        GCHECK(fabs(TextWidth(tv) - Settings::kDefaultMarkdownMaxWidth) <= 1);
        GCHECK(Centered(tv));
        GCHECK(tv.textContainerInset.width > 100);
        GCHECK(InsideMeasure(tv));
        // No ink left or right of the page: its quote bar, code box, rules
        // and table borders are inside it too.
        const CGFloat left = tv.textContainerOrigin.x;
        const CGFloat right = left + tv.textContainer.size.width;
        GCHECK(InkIn(tv, 0, left - 1) == 0);
        GCHECK(InkIn(tv, right + 1, NSWidth(tv.frame)) == 0);
        GCHECK(InkIn(tv, left, right) > 1000);
        MCGuiSnapshot(c.window.contentView, @"markdown_width_2000");
    });
    MCGuiStep(@"the page's table stays inside the measure", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        NSRange cell = [tv.string rangeOfString:@"what it is"];
        GCHECK(cell.location != NSNotFound);
        NSParagraphStyle *ps = [tv.textStorage attribute:NSParagraphStyleAttributeName
                                                 atIndex:cell.location effectiveRange:NULL];
        NSTextTableBlock *block = (NSTextTableBlock *)ps.textBlocks.lastObject;
        GCHECK([block isKindOfClass:NSTextTableBlock.class]);
        NSLayoutManager *lm = tv.layoutManager;
        NSRange glyphs = [lm glyphRangeForCharacterRange:cell actualCharacterRange:NULL];
        const NSRect r = [lm boundingRectForGlyphRange:glyphs inTextContainer:tv.textContainer];
        GCHECK(NSMinX(r) >= 0 && NSMaxX(r) <= tv.textContainer.size.width + 0.5);
    });
    MCGuiStep(@"a narrower window: the page takes the pane, with the editor's inset", ^{
        Resize(c, 900, 900);
        NSTextView *tv = [c valueForKey:@"textView"];
        MCGuiNote(@"view %.0f, inset %.1f, text %.1f", NSWidth(tv.frame),
                  tv.textContainerInset.width, TextWidth(tv));
        GCHECK(NSWidth(tv.frame) < Settings::kDefaultMarkdownMaxWidth);
        GCHECK(tv.textContainerInset.width == 8);
        GCHECK(Centered(tv) && InsideMeasure(tv));
        Resize(c, 2000, 900);   // and back, by the view's own resize
        GCHECK(fabs(TextWidth(tv) - Settings::kDefaultMarkdownMaxWidth) <= 1 && Centered(tv));
    });
    MCGuiStep(@"zoomed in, the measure grows with the page's fonts", ^{
        MCEditorZoomBy(4);   // 17 / 13 of the size
        NSTextView *tv = [c valueForKey:@"textView"];
        const CGFloat want = Settings::kDefaultMarkdownMaxWidth * MCZoomScale();
        MCGuiNote(@"zoom %.3f: view %.0f, text %.1f, want %.1f", MCZoomScale(),
                  NSWidth(tv.frame), TextWidth(tv), want);
        GCHECK(c.previewMode);
        // Whole-point insets, rounded up either side: up to 2 points short.
        GCHECK(TextWidth(tv) <= want + 0.01 && want - TextWidth(tv) < 2);
        GCHECK(Centered(tv) && InsideMeasure(tv));
    });
    MCGuiStep(@"zoomed far in, the page takes the whole pane and still lays out", ^{
        MCEditorZoomBy(40);
        NSTextView *tv = [c valueForKey:@"textView"];
        MCGuiNote(@"zoom %.3f: view %.0f, inset %.1f, text %.1f", MCZoomScale(),
                  NSWidth(tv.frame), tv.textContainerInset.width, TextWidth(tv));
        GCHECK(MCZoomScale() > 3);
        GCHECK(tv.textContainerInset.width == 8);
        GCHECK(InsideMeasure(tv));
        NSFont *body = [tv.textStorage attribute:NSFontAttributeName
            atIndex:[tv.string rangeOfString:@"word3 "].location effectiveRange:NULL];
        GCHECK(body.pointSize > 45);
        MCGuiSnapshot(c.window.contentView, @"markdown_width_zoomed");
        MCEditorZoomBy(0);
    });
    MCGuiStep(@"markdown.max-width = off: the whole width; a number: that width", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        MCGuiSetSettings([kHarnessSettings stringByAppendingString:@"markdown.max-width = off\n"]);
        GCHECK(tv.textContainerInset.width == 8);
        MCGuiSetSettings([kHarnessSettings stringByAppendingString:@"markdown.max-width = 500\n"]);
        GCHECK(fabs(TextWidth(tv) - 500) <= 1 && Centered(tv));
        MCGuiSetSettings(kHarnessSettings);
        GCHECK(fabs(TextWidth(tv) - Settings::kDefaultMarkdownMaxWidth) <= 1);
    });
    MCGuiStep(@"the source keeps the editor's inset", ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        [c togglePreview:nil];
        GCHECK(!c.previewMode && tv.editable);
        GCHECK(tv.textContainerInset.width == 8);
        Resize(c, 2100, 900);
        GCHECK(tv.textContainerInset.width == 8);
    });
    MCGuiStep(@"the preview to the side keeps the measure in its own pane", ^{
        MCGuiSetSettings([kHarnessSettings stringByAppendingString:@"markdown.max-width = 400\n"]);
        [c toggleSidePreview:nil];
        GCHECK([c sidePreviewShowing]);
    });
    MCGuiWait(@"the side page", 5, ^BOOL {
        MCMarkdownSidePreview *side = [c valueForKey:@"sidePreview"];
        return [side.textView.string containsString:@"The end."];
    });
    MCGuiStep(@"the side page is centered at the width, and follows its pane", ^{
        MCMarkdownSidePreview *side = [c valueForKey:@"sidePreview"];
        NSTextView *tv = side.textView;
        MCGuiNote(@"side view %.0f, inset %.1f, text %.1f", NSWidth(tv.frame),
                  tv.textContainerInset.width, TextWidth(tv));
        GCHECK(NSWidth(tv.frame) > 600);
        GCHECK(fabs(TextWidth(tv) - 400) <= 1 && Centered(tv) && InsideMeasure(tv));
        const CGFloat left = tv.textContainerOrigin.x;
        GCHECK(InkIn(tv, 0, left - 1) == 0);
        GCHECK(InkIn(tv, left + tv.textContainer.size.width + 1, NSWidth(tv.frame)) == 0);
        MCGuiSnapshot(c.window.contentView, @"markdown_width_side");
        Resize(c, 900, 900);   // a narrow pane: the least inset
        MCGuiNote(@"narrow side view %.0f, inset %.1f", NSWidth(tv.frame), tv.textContainerInset.width);
        GCHECK(NSWidth(tv.frame) < 430);
        GCHECK(tv.textContainerInset.width == 14);
        GCHECK(InsideMeasure(tv));
    });
    MCGuiStep(@"the window goes", ^{
        MCGuiSetSettings(kHarnessSettings);
        MCEditorZoomBy(0);
        if ([c sidePreviewShowing]) [c toggleSidePreview:nil];
        MCGuiSetKeyWindow(nil);
        [c.window performClose:nil];
    });
}

// ---------------------------------------------------------- swatch_escape
MC_GUI_SUITE(swatch_escape) {
    __block EditorController *c = nil;
    NSString *settingsPath = NSProcessInfo.processInfo.environment[@"MINICODE_SETTINGS"];
    NSString *withColor = [kHarnessSettings stringByAppendingString:@"editor.ruler-color = #5A5A5A\n"];
    void (^openSwatch)(void) = ^{
        NSTextView *tv = [c valueForKey:@"textView"];
        NSRange key = [tv.string rangeOfString:@"editor.ruler-color = #"];
        GCHECK(key.location != NSNotFound);
        if (key.location == NSNotFound) return;
        [c.window makeFirstResponder:tv];
        [c pickColorAtIndex:NSMaxRange(key) - 1];
    };

    MCGuiStep(@"a swatch in the settings file opens the color panel", ^{
        MCGuiSetSettings(withColor);
        c = MCGuiController();
        MCGuiSetKeyWindow(c.window);
        GCHECK([c showFile:settingsPath]);
        openSwatch();
        GCHECK(MCSwatchColorPanelOpen());
        GCHECK(PickReaches(c, 0.6));   // a color chosen there rewrites the line
    });
    MCGuiStep(@"Esc in the settings file closes it, and its picks stop", ^{
        MCGuiSendKeyToApp(c.window, 53, 0, @"\x1b", @"\x1b");
        GCHECK(!MCSwatchColorPanelOpen());
        GCHECK(!PickReaches(c, 0.8));   // nothing more is written
    });
    MCGuiStep(@"Esc in the panel itself closes it", ^{
        openSwatch();
        GCHECK(MCSwatchColorPanelOpen());
        MCGuiSendKeyToApp([NSColorPanel sharedColorPanel], 53, 0, @"\x1b", @"\x1b");
        GCHECK(!MCSwatchColorPanelOpen());
        GCHECK(!PickReaches(c, 0.4));
    });
    MCGuiStep(@"Esc with the keyboard elsewhere, or with a modifier, leaves it open", ^{
        openSwatch();
        MCGuiSendKeyToApp(c.window, 53, NSEventModifierFlagOption, @"\x1b", @"\x1b");
        GCHECK(MCSwatchColorPanelOpen());
        [c focusTree:nil];
        GCHECK(c.window.firstResponder != [c valueForKey:@"textView"]);
        MCGuiSendKeyToApp(c.window, 53, 0, @"\x1b", @"\x1b");
        GCHECK(MCSwatchColorPanelOpen());
        GCHECK(PickReaches(c, 0.2));
    });
    MCGuiStep(@"its close button stops the picks too", ^{
        // Offscreen the panel was never ordered in, so close says nothing;
        // on screen its close button posts this.
        [[NSNotificationCenter defaultCenter]
            postNotificationName:NSWindowWillCloseNotification
                          object:[NSColorPanel sharedColorPanel]];
        GCHECK(!MCSwatchColorPanelOpen());
        GCHECK(!PickReaches(c, 0.1));
        GCHECK(!MCCloseSwatchPanels());   // nothing left to close
        MCGuiSetSettings(kHarnessSettings);
        MCGuiSetKeyWindow(nil);
    });
}
