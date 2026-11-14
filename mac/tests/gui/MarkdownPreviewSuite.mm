// MarkdownPreviewSuite.mm — `make guitest SUITES=markdown_preview`: the Mac's
// rendered Markdown, checked by its attributes rather than by eye. It opens
// tests/markdown/corpus.md and checks the headings, a real table, a footnote
// and its way there and back, a quote inside a list item's quote, code at a
// list item's indent and an HTML block's text left as written; ticks a task
// box through its minicode-task: link; edits a table cell from the page and
// finds the source changed by that one splice; and an edit made after the
// file changed underneath ends on the clipboard with a note in the status
// bar, not in the bin. Math is checked as its TeX (no tectonic here, or in
// CI); MC_GUI_REAL_TECTONIC=<a tectonic> adds one formula typeset by it.
#import "Harness.h"
#import "EditorController.h"
#import "EditorController+Private.h"
#import "MarkdownEditPanel.h"
#import "MarkdownMath.h"
#import "MarkdownImage.h"
#import "MarkdownSidePreview.h"
#include "MarkdownHtml.h"
#import <objc/runtime.h>

namespace {

NSString *Fixture(NSString *name) {
    return [MCGuiProject() stringByAppendingPathComponent:[@"mdpreview" stringByAppendingPathComponent:name]];
}

NSTextView *Text(EditorController *c) { return [c valueForKey:@"textView"]; }

NSDictionary *ActiveTab(EditorController *c) {
    for (NSDictionary *t in [c controlTabs])
        if ([t[@"active"] boolValue]) return t;
    return nil;
}

// Where `needle` starts on the page (after `from`), or NSNotFound.
NSUInteger At(EditorController *c, NSString *needle, NSUInteger from = 0) {
    NSString *s = Text(c).string;
    if (from >= s.length) return NSNotFound;
    return [s rangeOfString:needle options:0 range:NSMakeRange(from, s.length - from)].location;
}

id Attr(EditorController *c, NSAttributedStringKey key, NSUInteger i) {
    NSTextStorage *st = Text(c).textStorage;
    return i < st.length ? [st attribute:key atIndex:i effectiveRange:NULL] : nil;
}

NSParagraphStyle *Para(EditorController *c, NSUInteger i) {
    return Attr(c, NSParagraphStyleAttributeName, i);
}

// The first character carrying link `href`, or NSNotFound.
NSUInteger LinkAt(EditorController *c, NSString *href) {
    NSTextStorage *st = Text(c).textStorage;
    __block NSUInteger found = NSNotFound;
    [st enumerateAttribute:NSLinkAttributeName inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(id v, NSRange r, BOOL *stop) {
        if ([v isEqual:href]) { found = r.location; *stop = YES; }
    }];
    return found;
}

// Whether character `i` is in the part of the page on show.
BOOL Showing(EditorController *c, NSUInteger i) {
    NSRect r = [c textRectForCharacter:i];
    NSClipView *clip = ((NSScrollView *)[c valueForKey:@"editorScroll"]).contentView;
    return !NSIsEmptyRect(r) && NSMinY(r) >= NSMinY(clip.bounds) &&
           NSMaxY(r) <= NSMaxY(clip.bounds);
}

// The popover's editor (the window is offscreen, so the popover itself never
// shows, but its text view and buttons exist): its text set, then Save, as
// a key press would be, in an undo group of its own.
void SaveEdit(EditorController *c, NSString *text) {
    MCMarkdownEditPanel *panel = [c valueForKey:@"markdownEditPanel"];
    NSTextView *editor = [panel valueForKey:@"_editor"];
    editor.string = text;
    MCGuiUndoGroup(Text(c).undoManager, ^{
        [panel performSelector:@selector(save:) withObject:nil];
    });
}

// How the two strings differ: the one stretch of `a` that `b` replaces.
NSRange Changed(NSString *a, NSString *b, NSString **with) {
    NSUInteger p = 0, s = 0;
    while (p < a.length && p < b.length && [a characterAtIndex:p] == [b characterAtIndex:p]) p++;
    while (s < a.length - p && s < b.length - p &&
           [a characterAtIndex:a.length - 1 - s] == [b characterAtIndex:b.length - 1 - s])
        s++;
    if (with) *with = [b substringWithRange:NSMakeRange(p, b.length - p - s)];
    return NSMakeRange(p, a.length - p - s);
}

// Web pictures without the web: https://pictures.minicode-test.invalid/
// answers here with a tall PNG, through a URL protocol the picture
// loader's session is given (its configuration is made by
// ephemeralSessionConfiguration, swapped for the step that needs it). Every
// address asked for is recorded, so an http one can be seen never to be.
NSMutableArray<NSString *> *gPictureRequests;
NSData *gPicturePng;

}  // namespace

@interface MCGuiPictureProtocol : NSURLProtocol
@end

@implementation MCGuiPictureProtocol
+ (BOOL)canInitWithRequest:(NSURLRequest *)request {
    if (![request.URL.host isEqualToString:@"pictures.minicode-test.invalid"]) return NO;
    @synchronized(gPictureRequests) {
        [gPictureRequests addObject:request.URL.absoluteString];
    }
    return YES;
}
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
// Half a second late, as a server would be, so the page shows the alt text
// first however the steps are timed; on the loading thread's run loop.
- (void)startLoading {
    [self performSelector:@selector(answer) withObject:nil afterDelay:0.5];
}
- (void)answer {
    NSHTTPURLResponse *ok = [[NSHTTPURLResponse alloc]
         initWithURL:self.request.URL statusCode:200 HTTPVersion:@"HTTP/1.1"
        headerFields:@{@"Content-Type": @"image/png",
                       @"Content-Length": [NSString stringWithFormat:@"%lu",
                                                    (unsigned long)gPicturePng.length]}];
    [self.client URLProtocol:self didReceiveResponse:ok cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    [self.client URLProtocol:self didLoadData:gPicturePng];
    [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading {
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
}
@end

@interface NSURLSessionConfiguration (MCGuiPictures)
@end

@implementation NSURLSessionConfiguration (MCGuiPictures)
+ (NSURLSessionConfiguration *)mcgui_ephemeralSessionConfiguration {
    NSURLSessionConfiguration *c = [self mcgui_ephemeralSessionConfiguration];   // the original
    c.protocolClasses = [@[MCGuiPictureProtocol.class] arrayByAddingObjectsFromArray:c.protocolClasses ?: @[]];
    return c;
}
@end

namespace {

void SwapEphemeralConfiguration(void) {
    Class meta = object_getClass(NSURLSessionConfiguration.class);
    method_exchangeImplementations(
        class_getClassMethod(meta, @selector(ephemeralSessionConfiguration)),
        class_getClassMethod(meta, @selector(mcgui_ephemeralSessionConfiguration)));
}

// Whether character `i` of `tv` shows a picture rather than text.
BOOL PictureAt(NSTextView *tv, NSUInteger i) {
    NSTextStorage *st = tv.textStorage;
    return i < st.length &&
           [[st attribute:NSAttachmentAttributeName atIndex:i effectiveRange:NULL]
               isKindOfClass:MCMarkdownImage.class];
}

NSUInteger PictureCount(NSTextView *tv) {
    NSTextStorage *st = tv.textStorage;
    __block NSUInteger n = 0;
    [st enumerateAttribute:NSAttachmentAttributeName inRange:NSMakeRange(0, st.length) options:0
                usingBlock:^(id v, NSRange r, BOOL *stop) {
        (void)r; (void)stop;
        if ([v isKindOfClass:MCMarkdownImage.class]) n++;
    }];
    return n;
}

// The text at the top of the part of `tv` on show.
NSString *TopText(NSTextView *tv) {
    const NSRect v = tv.visibleRect;
    NSUInteger i = [tv characterIndexForInsertionAtPoint:NSMakePoint(NSMinX(v) + 20, NSMinY(v) + 2)];
    NSString *s = tv.string;
    if (i >= s.length) return @"";
    const NSRange line = [s lineRangeForRange:NSMakeRange(i, 0)];
    return [[s substringWithRange:line]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

// A page of short paragraphs with pictures among the first: an https one
// (`name`), an http one and a data: one.
NSString *PicturePage(NSString *name) {
    NSMutableString *md = [NSMutableString string];
    for (int k = 0; k < 120; k++) {
        [md appendFormat:@"Para %d of the pictures page.\n\n", k];
        if (k == 3)
            [md appendFormat:@"![a tall picture](https://pictures.minicode-test.invalid/%@)\n\n"
                             @"![plain http](http://pictures.minicode-test.invalid/plain.png)\n\n"
                             @"![inline](data:image/png;base64,%@)\n\n",
                             name, [gPicturePng base64EncodedStringWithOptions:0]];
    }
    return md;
}

}  // namespace

MC_GUI_SUITE(markdown_preview) {
    __block NSString *corpus = nil;
    __block NSString *original = nil;

    MCGuiStep(@"the corpus renders: headings, a table, nested quotes, code in a list item", ^{
        NSString *repo = MCGuiBuildDir().stringByDeletingLastPathComponent;
        original = [NSString stringWithContentsOfFile:
            [repo stringByAppendingPathComponent:@"tests/markdown/corpus.md"]
                                             encoding:NSUTF8StringEncoding error:nil];
        GCHECK(original.length > 1000);
        corpus = Fixture(@"corpus.md");
        MCGuiWriteFile(corpus, original);
        EditorController *c = MCGuiController();
        GCHECK([c showFile:corpus]);
        GCHECK(c.previewMode && !Text(c).editable);

        // The title: a heading, big and bold, with GitHub's anchor.
        NSDictionary *anchors = [c valueForKey:@"markdownAnchors"];
        GCHECK(anchors[@"markdown-corpus"] != nil && anchors[@"tables"] != nil);
        const NSUInteger title = [anchors[@"markdown-corpus"] unsignedIntegerValue];
        GCHECK(At(c, @"Markdown corpus", title) == title);
        NSFont *font = Attr(c, NSFontAttributeName, title);
        GCHECK(font.pointSize > 20 && (font.fontDescriptor.symbolicTraits & NSFontDescriptorTraitBold));

        // A table is a real NSTextTable, a cell to a paragraph.
        const NSUInteger cell = At(c, @"empty cells");
        GCHECK(cell != NSNotFound);
        GCHECK([Para(c, cell).textBlocks.firstObject isKindOfClass:NSTextTableBlock.class]);
        NSArray *rc = Attr(c, kMarkdownTableCell, cell);
        GCHECK(rc.count == 2 && [rc[0] intValue] == 2 && [rc[1] intValue] == 1);

        // A quote inside a quoted list item: two bars, one in the other.
        const NSUInteger inner = At(c, @"with a quote inside the item");
        GCHECK(inner != NSNotFound && Para(c, inner).textBlocks.count == 2);
        const NSUInteger outer = At(c, @"an item in a quote");
        GCHECK(Para(c, outer).textBlocks.count == 1);
        // A quote inside a numbered step: its bar at the step's indent.
        const NSUInteger step = At(c, @"A quote inside the step");
        NSArray<NSTextBlock *> *around = Para(c, step).textBlocks;
        GCHECK(around.count == 2 && !around[0].backgroundColor &&
               [around[0] widthForLayer:NSTextBlockPadding edge:NSRectEdgeMinX] > 10 &&
               [around[1] widthForLayer:NSTextBlockBorder edge:NSRectEdgeMinX] > 1);
        GCHECK(Para(c, step).headIndent < 1);
        // Code in a list item: the box at the item's indent.
        const NSUInteger code = At(c, @"and a code block");
        NSArray<NSTextBlock *> *boxed = Para(c, code).textBlocks;
        GCHECK(boxed.count == 2 && !boxed[0].backgroundColor && boxed[1].backgroundColor &&
               [boxed[0] widthForLayer:NSTextBlockPadding edge:NSRectEdgeMinX] > 10);
        // Its paragraphs share both, so the box is drawn once around them.
        GCHECK(Para(c, code).textBlocks[0] == Para(c, code + 1).textBlocks[0]);
        // An HTML block's text is HTML: its asterisks stay.
        const NSUInteger html = At(c, @"*Not emphasis*");
        GCHECK(html != NSNotFound);
        NSFont *plain = Attr(c, NSFontAttributeName, html + 1);
        GCHECK(!(plain.fontDescriptor.symbolicTraits & NSFontDescriptorTraitItalic));
        // A <details> summary is bold.
        NSFont *summary = Attr(c, NSFontAttributeName, At(c, @"A summary, in bold"));
        GCHECK(summary.fontDescriptor.symbolicTraits & NSFontDescriptorTraitBold);
        // Math shows as its TeX while nothing typesets it.
        GCHECK(At(c, @"e^{i\\pi} + 1 = 0") != NSNotFound);
        // Pictures to look at, with MC_GUI_SNAPSHOTS set.
        MCGuiSnapshot(Text(c), @"markdown-corpus");
        for (NSString *at in @[@"and a code block", @"an item in a quote", @"A quote inside the step",
                               @"*Not emphasis*", @"A claim that needs", @"A second note."]) {
            [c scrollEditorToY:NSMinY([c textRectForCharacter:At(c, at)]) - 200];
            MCGuiSnapshot([c valueForKey:@"editorScroll"], [@"markdown-" stringByAppendingString:
                [at stringByTrimmingCharactersInSet:NSCharacterSet.punctuationCharacterSet]]);
        }
        [c scrollEditorToY:0];
    });

    MCGuiStep(@"a footnote: a raised number, the note at the end, and the way back", ^{
        EditorController *c = MCGuiController();
        const NSUInteger ref = LinkAt(c, @"#fn-1");
        GCHECK(ref != NSNotFound);
        GCHECK([Attr(c, NSBaselineOffsetAttributeName, ref) doubleValue] > 0);
        GCHECK(Attr(c, NSUnderlineStyleAttributeName, ref) == nil);
        GCHECK([[Text(c).string substringWithRange:NSMakeRange(ref, 1)] isEqualToString:@"1"]);
        NSDictionary *anchors = [c valueForKey:@"markdownAnchors"];
        NSNumber *note = anchors[@"fn-1"], *back = anchors[@"fnref-1"];
        GCHECK(note && back && anchors[@"fnref-1-2"] && anchors[@"fn-2"]);
        GCHECK(note.unsignedIntegerValue > At(c, @"Long lines"));   // after the last section
        GCHECK(back.unsignedIntegerValue == ref);
        // The note's text is smaller than the body's.
        const NSUInteger text = At(c, @"The source, with a");
        GCHECK(text > note.unsignedIntegerValue);
        GCHECK([Attr(c, NSFontAttributeName, text) pointSize] <
               [Attr(c, NSFontAttributeName, At(c, @"Open this file")) pointSize]);
        // Following the link scrolls to the note, and ↩ back to the reference.
        NSTextView *tv = Text(c);
        NSClipView *clip = ((NSScrollView *)[c valueForKey:@"editorScroll"]).contentView;
        [clip scrollToPoint:NSZeroPoint];
        GCHECK(!Showing(c, note.unsignedIntegerValue));
        GCHECK([c textView:tv clickedOnLink:@"#fn-1" atIndex:ref]);
        const CGFloat down = clip.bounds.origin.y;
        GCHECK(down > 500 && Showing(c, note.unsignedIntegerValue));
        const NSUInteger home = LinkAt(c, @"#fnref-1");
        GCHECK(home != NSNotFound && home > note.unsignedIntegerValue);
        GCHECK([c textView:tv clickedOnLink:@"#fnref-1" atIndex:home]);
        GCHECK(clip.bounds.origin.y < down && Showing(c, ref));
        // A reference with no definition stays as written.
        GCHECK(At(c, @"[^missing]") != NSNotFound);
    });

    MCGuiStep(@"ticking a task box changes the source, and the tab is unsaved", ^{
        EditorController *c = MCGuiController();
        GCHECK(![ActiveTab(c)[@"dirty"] boolValue]);
        NSString *line = @"- [ ] A task still to do";
        NSArray *sourceLines = [original componentsSeparatedByString:@"\n"];
        const NSUInteger n = [sourceLines indexOfObject:line];
        GCHECK(n != NSNotFound);
        NSString *link = [NSString stringWithFormat:@"minicode-task:%lu", (unsigned long)n];
        const NSUInteger box = LinkAt(c, link);
        GCHECK(box != NSNotFound);
        GCHECK([Attr(c, NSAttachmentAttributeName, box) isKindOfClass:NSTextAttachment.class]);
        MCGuiUndoGroup(Text(c).undoManager, ^{
            GCHECK([c textView:Text(c) clickedOnLink:link atIndex:box]);
        });
        GCHECK([c.sourceText containsString:@"- [x] A task still to do"]);
        GCHECK([ActiveTab(c)[@"dirty"] boolValue]);
        GCHECK(LinkAt(c, link) != NSNotFound);   // shown again, ticked
        GCHECK_EQ_STR([Text(c).undoManager undoActionName], @"Toggle Task");
    });
    MCGuiStep(@"Cmd+Z puts the box back", ^{
        EditorController *c = MCGuiController();
        [Text(c).undoManager undo];
        GCHECK([c.sourceText isEqualToString:original]);
        GCHECK(![ActiveTab(c)[@"dirty"] boolValue]);
    });

    MCGuiStep(@"a double-click edits a table cell, one splice in the source", ^{
        EditorController *c = MCGuiController();
        const NSUInteger cell = At(c, @"empty cells");
        GCHECK(cell != NSNotFound);
        NSRect r = [c textRectForCharacter:cell];
        GCHECK([c editMarkdownAtCharacter:cell point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        MCMarkdownEditPanel *panel = [c valueForKey:@"markdownEditPanel"];
        NSTextView *editor = [panel valueForKey:@"_editor"];
        GCHECK_EQ_STR(editor.string, @"empty cells");
        NSString *before = c.sourceText;
        SaveEdit(c, @"full | cells");
        NSString *with = nil;
        const NSRange changed = Changed(before, c.sourceText, &with);
        GCHECK_EQ_STR([before substringWithRange:changed], @"empty");
        GCHECK_EQ_STR(with, @"full \\|");
        GCHECK([c.sourceText containsString:@"|  | full \\| cells |  |"]);
        GCHECK(At(c, @"full | cells") != NSNotFound);   // shown again, the pipe a pipe
        GCHECK([ActiveTab(c)[@"dirty"] boolValue]);
    });

    MCGuiStep(@"an edit after the file changed underneath goes to the clipboard, and says so", ^{
        EditorController *c = MCGuiController();
        const NSUInteger at = At(c, @"Markdown again, after a blank line.");
        GCHECK(at != NSNotFound);
        NSRect r = [c textRectForCharacter:at];
        GCHECK([c editMarkdownAtCharacter:at point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        // Meanwhile the buffer changes (a reload from disk, an agent's edit).
        NSString *changed = [c.sourceText stringByAppendingString:@"\nAdded meanwhile.\n"];
        [c setValue:changed forKey:@"sourceText"];
        [NSPasteboard.generalPasteboard clearContents];
        SaveEdit(c, @"My careful rewrite.");
        GCHECK([c.sourceText isEqualToString:changed]);   // not applied over it
        GCHECK_EQ_STR([NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString],
                      @"My careful rewrite.");
        NSTextField *note = [c valueForKey:@"settingsLabel"];
        GCHECK([note.stringValue containsString:@"your text is on the clipboard"]);
        // Saving it unchanged is no edit, and loses nothing worth a note.
        [c renderMarkdownKeepingPlace];
        const NSUInteger again = At(c, @"Markdown again, after a blank line.");
        r = [c textRectForCharacter:again];
        GCHECK([c editMarkdownAtCharacter:again point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        [c setValue:[changed stringByAppendingString:@"x"] forKey:@"sourceText"];
        [NSPasteboard.generalPasteboard clearContents];
        SaveEdit(c, @"Markdown again, after a blank line.");
        GCHECK([NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString] == nil);
    });

    MCGuiStep(@"an HTML block edits as its HTML", ^{
        NSString *path = Fixture(@"html.md");
        MCGuiWriteFile(path, @"Intro\n\n<div align=\"center\">\n  *kept* &amp; <b>bold</b>\n</div>\n");
        EditorController *c = MCGuiController();
        GCHECK([c showFile:path]);
        const NSUInteger at = At(c, @"*kept* & ");
        GCHECK(at != NSNotFound);
        NSFont *bold = Attr(c, NSFontAttributeName, At(c, @"bold"));
        GCHECK(bold.fontDescriptor.symbolicTraits & NSFontDescriptorTraitBold);
        NSRect r = [c textRectForCharacter:at];
        GCHECK([c editMarkdownAtCharacter:at point:NSMakePoint(NSMidX(r), NSMidY(r))]);
        NSTextView *editor = [[c valueForKey:@"markdownEditPanel"] valueForKey:@"_editor"];
        GCHECK_EQ_STR(editor.string, @"<div align=\"center\">\n  *kept* &amp; <b>bold</b>\n</div>");
        [[c valueForKey:@"markdownEditPanel"] close];
    });

    MCGuiStep(@"a numbered task keeps its number before its box; a footnote's code is a code box", ^{
        NSString *path = Fixture(@"numbered.md");
        MCGuiWriteFile(path, @"1. [ ] first\n2. [x] second\n\nRun it[^a].\n\n"
                             @"[^a]: Like this:\n\n    ```sh\n    make all\n    ```\n");
        EditorController *c = MCGuiController();
        GCHECK([c showFile:path]);
        const NSUInteger box = LinkAt(c, @"minicode-task:0");
        GCHECK(box != NSNotFound && box >= 3);
        if (box != NSNotFound && box >= 3)
            GCHECK_EQ_STR([Text(c).string substringWithRange:NSMakeRange(box - 3, 3)], @"1. ");
        const NSUInteger second = LinkAt(c, @"minicode-task:1");
        GCHECK(second != NSNotFound && second >= 3 &&
               [[Text(c).string substringWithRange:NSMakeRange(second - 3, 3)] isEqualToString:@"2. "]);
        const NSUInteger code = At(c, @"make all");
        GCHECK(code != NSNotFound && At(c, @"sh make") == NSNotFound);
        NSTextBlock *boxed = Para(c, code).textBlocks.lastObject;
        GCHECK(boxed.backgroundColor != nil);   // the code block's box
        GCHECK(At(c, @"\xE2\x86\xA9") > code);   // the way back, after the code
    });

    __block NSString *topBefore = nil;
    __block CGFloat yBefore = 0;   // where the view was scrolled to
    MCGuiStep(@"web pictures: alt text first, the page scrolled below the picture", ^{
        // A tall picture (40 by 600 pixels) to push what follows it down.
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL pixelsWide:40 pixelsHigh:600 bitsPerSample:8
                     samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        memset(rep.bitmapData, 0x80, (size_t)rep.bytesPerRow * 600);
        gPicturePng = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        gPictureRequests = [NSMutableArray array];
        // The loader's session is made at its first fetch, with the
        // configuration swapped in now; one made before goes.
        SwapEphemeralConfiguration();
        id loader = [NSClassFromString(@"MCWebImageLoader") performSelector:@selector(shared)];
        [[loader valueForKey:@"_session"] invalidateAndCancel];
        [loader setValue:nil forKey:@"_session"];

        NSString *path = Fixture(@"pictures.md");
        MCGuiWriteFile(path, PicturePage(@"tall.png"));
        EditorController *c = MCGuiController();
        GCHECK([c showFile:path] && c.previewMode);
        NSTextView *tv = Text(c);
        GCHECK(At(c, @"a tall picture") != NSNotFound);   // its alt text stands in
        GCHECK(At(c, @"plain http") != NSNotFound);
        GCHECK(PictureCount(tv) == 1);                     // the data: one, decoded in place
        [c scrollEditorToY:NSMinY([c textRectForCharacter:At(c, @"Para 60 of")])];
        topBefore = TopText(tv);
        yBefore = ((NSScrollView *)[c valueForKey:@"editorScroll"]).contentView.bounds.origin.y;
        MCGuiNote(@"at the top before the picture: %@", topBefore);
        GCHECK([topBefore hasPrefix:@"Para 60 of"]);
    });
    MCGuiWait(@"the https picture to arrive and take its alt text's place", 5, ^BOOL {
        EditorController *c = MCGuiController();
        return PictureCount(Text(c)) == 2 && At(c, @"a tall picture") == NSNotFound;
    });
    MCGuiStep(@"the text at the top stays at the top; http was never asked for", ^{
        EditorController *c = MCGuiController();
        NSTextView *tv = Text(c);
        MCGuiNote(@"at the top after: %@; asked for %@", TopText(tv), gPictureRequests);
        GCHECK_EQ_STR(TopText(tv), topBefore);
        // ... though the picture pushed it down the page.
        const CGFloat y = ((NSScrollView *)[c valueForKey:@"editorScroll"]).contentView.bounds.origin.y;
        MCGuiNote(@"scrolled from %.0f to %.0f", yBefore, y);
        GCHECK(y > yBefore + 100);
        GCHECK(At(c, @"plain http") != NSNotFound);   // still its alt text
        @synchronized(gPictureRequests) {
            GCHECK([gPictureRequests containsObject:@"https://pictures.minicode-test.invalid/tall.png"]);
            for (NSString *u in gPictureRequests) GCHECK(![u hasPrefix:@"http:"]);
        }
        // The picture sits where its alt text was, between paragraphs 3 and 4.
        const NSUInteger three = At(c, @"Para 3 of"), four = At(c, @"Para 4 of");
        BOOL between = NO;
        for (NSUInteger i = three; i < four; i++) between = between || PictureAt(tv, i);
        GCHECK(between);
    });

    MCGuiStep(@"the page beside the source: the picture arrives, the page keeps its place", ^{
        NSString *path = Fixture(@"pictures-side.md");
        MCGuiWriteFile(path, PicturePage(@"tall-side.png"));
        EditorController *c = MCGuiController();
        GCHECK([c showFile:path]);
        if (c.previewMode) [c togglePreview:nil];
        if (![c sidePreviewShowing]) [c toggleSidePreview:nil];
        MCMarkdownSidePreview *side = [c valueForKey:@"sidePreview"];
        GCHECK([c sidePreviewShowing]);
        NSTextView *page = side.textView;
        const NSUInteger at = [page.string rangeOfString:@"Para 60 of"].location;
        GCHECK(at != NSNotFound && [page.string containsString:@"a tall picture"]);
        // The user scrolls the page: it leads, so it keeps its place.
        MCScrollTextViewToY(side.scrollView, page, NSMinY(MCTextViewRectForCharacter(page, at)));
        GCHECK(side.pageLeads);
        topBefore = TopText(page);
        yBefore = side.scrollY;
        GCHECK([topBefore hasPrefix:@"Para 60 of"]);
    });
    MCGuiWait(@"the picture to arrive in the page beside the source", 5, ^BOOL {
        MCMarkdownSidePreview *side = [MCGuiController() valueForKey:@"sidePreview"];
        return PictureCount(side.textView) == 2 &&
               ![side.textView.string containsString:@"a tall picture"];
    });
    MCGuiStep(@"its top is still the same text", ^{
        EditorController *c = MCGuiController();
        MCMarkdownSidePreview *side = [c valueForKey:@"sidePreview"];
        MCGuiNote(@"side top after: %@", TopText(side.textView));
        GCHECK_EQ_STR(TopText(side.textView), topBefore);
        GCHECK(side.scrollY > yBefore + 100 && side.pageLeads);
        // Built again in place: the page is what a whole render makes.
        GCHECK_EQ_STR(side.textView.string,
                      [c markdownPageFor:c.sourceText width:side.pageWidth anchors:NULL].string);
        SwapEphemeralConfiguration();   // back as it was
        [c closeSidePreview];
    });

    MCGuiStep(@"Export as HTML… writes the page beside the file, unsaved changes included", ^{
        EditorController *c = MCGuiController();
        GCHECK([c showFile:corpus]);
        NSMenuItem *item = nil;
        for (NSMenuItem *top in NSApp.mainMenu.itemArray)
            for (NSMenuItem *i in top.submenu.itemArray)
                if (i.action == @selector(exportMarkdownHtml:)) item = i;
        GCHECK(item && item.keyEquivalent.length == 0 && [item.menu.title isEqualToString:@"File"]);
        GCHECK([c validateMenuItem:item] && c.canExportMarkdownHtml);
        // Unsaved text goes into the page.
        NSString *edited = [original stringByAppendingString:@"\n## Added before saving\n"];
        [c setValue:edited forKey:@"sourceText"];
        __block NSString *suggested = nil;
        NSString *out = Fixture(@"exported.html");
        MCHtmlExportPathChooser = ^NSString *(NSString *s) {
            suggested = s;
            return out;
        };
        [[NSFileManager defaultManager] removeItemAtPath:out error:nil];
        [c exportMarkdownHtml:item];
        MCHtmlExportPathChooser = nil;
        GCHECK_EQ_STR(suggested, Fixture(@"corpus.html"));
        NSString *page = [NSString stringWithContentsOfFile:out encoding:NSUTF8StringEncoding error:nil];
        const std::string want = MarkdownHtml::page(edited.UTF8String, "corpus.md");
        GCHECK(page && [page isEqualToString:[NSString stringWithUTF8String:want.c_str()]]);
        GCHECK([page containsString:@"<title>Markdown corpus</title>"]);
        GCHECK([page containsString:@"<h2 id=\"added-before-saving\">Added before saving</h2>"]);
        GCHECK([page containsString:@"<li class=\"task-list-item\"><input type=\"checkbox\" disabled>"]);
        NSTextField *note = [c valueForKey:@"settingsLabel"];
        GCHECK([note.stringValue containsString:@"Exported exported.html"]);
        [c setValue:original forKey:@"sourceText"];
        // A file that is not Markdown has nothing to export.
        NSString *py = Fixture(@"plain.py");
        MCGuiWriteFile(py, @"print(1)\n");
        GCHECK([c showFile:py]);
        GCHECK(!c.canExportMarkdownHtml && ![c validateMenuItem:item]);
    });

    if (NSString *tectonic = NSProcessInfo.processInfo.environment[@"MC_GUI_REAL_TECTONIC"]) {
        MCGuiStep(@"with a real tectonic, a formula is typeset", ^{
            setenv("MINICODE_TECTONIC", tectonic.fileSystemRepresentation, 1);
            NSString *path = Fixture(@"math.md");
            MCGuiWriteFile(path, @"Inline $x^2 + y^2$ here.\n");
            GCHECK([MCGuiController() showFile:path]);
        });
        MCGuiWait(@"tectonic typesets the formula", 240, ^BOOL {
            NSTextStorage *st = Text(MCGuiController()).textStorage;
            __block BOOL typeset = NO;
            [st enumerateAttribute:NSAttachmentAttributeName inRange:NSMakeRange(0, st.length)
                           options:0 usingBlock:^(id v, NSRange r, BOOL *stop) {
                (void)r;
                if ([v isKindOfClass:MCMathAttachment.class]) { typeset = YES; *stop = YES; }
            }];
            return typeset;
        });
    }
}
