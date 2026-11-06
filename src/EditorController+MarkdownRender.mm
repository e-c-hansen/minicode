// EditorController+MarkdownRender.mm — EditorController: turning Markdown runs into attributed text: paragraphs, runs, tables.
#import "EditorController+Private.h"
#import "EditorFont.h"
#import "EditorController+MarkdownIvars.h"
#import <objc/runtime.h>

// A task list item's box, drawn rather than the ☐ and ☑ characters, whose
// look depends on the font. Checked, it is filled with the link color and
// carries a check mark.
@interface MCTaskBoxCell : NSTextAttachmentCell
@property(nonatomic) BOOL checked;
@property(nonatomic, strong) NSColor *ink, *fill;
@end

@implementation MCTaskBoxCell
- (NSSize)cellSize { return NSMakeSize(14, 14); }
- (NSPoint)cellBaselineOffset { return NSMakePoint(0, -2); }
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view {
    NSRect box = NSInsetRect(frame, 0.75, 0.75);
    NSBezierPath *outline = [NSBezierPath bezierPathWithRoundedRect:box xRadius:3 yRadius:3];
    if (self.checked) {
        [self.fill setFill];
        [outline fill];
        // The text view is flipped: y grows downwards.
        NSBezierPath *tick = [NSBezierPath bezierPath];
        [tick moveToPoint:NSMakePoint(NSMinX(box) + 3.0, NSMidY(box) + 0.2)];
        [tick lineToPoint:NSMakePoint(NSMinX(box) + 5.6, NSMaxY(box) - 3.2)];
        [tick lineToPoint:NSMakePoint(NSMaxX(box) - 2.8, NSMinY(box) + 3.2)];
        tick.lineWidth = 1.8;
        tick.lineCapStyle = NSLineCapStyleRound;
        tick.lineJoinStyle = NSLineJoinStyleRound;
        [[NSColor whiteColor] setStroke];
        [tick stroke];
    } else {
        outline.lineWidth = 1.3;
        [[self.ink colorWithAlphaComponent:0.75] setStroke];
        [outline stroke];
    }
}
- (void)drawWithFrame:(NSRect)frame inView:(NSView *)view
       characterIndex:(NSUInteger)charIndex layoutManager:(NSLayoutManager *)lm {
    [self drawWithFrame:frame inView:view];
}
@end

// The cell is handed out by an override, since one set in init reads back
// nil (see MarkdownImage).
@interface MCTaskBox : NSTextAttachment
@property(nonatomic, strong) MCTaskBoxCell *box;
@end

@implementation MCTaskBox
- (id<NSTextAttachmentCell>)attachmentCell { return self.box; }
@end

@implementation EditorController (MarkdownRender)

// The preview's vertical rhythm, in points at no zoom: the empty line
// between two blocks (the parser's `gap` runs), and the indent of each list
// level.
static const CGFloat kMarkdownGap = 12;
static const CGFloat kMarkdownListStep = 24;

// A text block as wide as the text container.
static NSTextBlock *MCFullWidthBlock(void) {
    NSTextBlock *b = [NSTextBlock new];
    [b setValue:100 type:NSTextBlockPercentageValueType forDimension:NSTextBlockWidth];
    return b;
}

// A list item's indent for the quote's bar or the code block's box inside
// it: a text block of its own, with no background or border, around theirs
// (a margin on the box itself still had the box's background drawn under
// it). The box keeps it, so every paragraph of the box gets the same one.
static char kMCIndentKey;
static void MCIndentBlock(NSTextBlock *inner, CGFloat points) {
    NSTextBlock *b = MCFullWidthBlock();
    [b setWidth:points type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
           edge:NSRectEdgeMinX];
    objc_setAssociatedObject(inner, &kMCIndentKey, b, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static void MCAddBlock(NSMutableArray<NSTextBlock *> *blocks, NSTextBlock *b) {
    if (NSTextBlock *indent = objc_getAssociatedObject(b, &kMCIndentKey)) [blocks addObject:indent];
    [blocks addObject:b];
}

// The paragraph style for a paragraph of rendered Markdown, from its first
// run. A block quote draws a bar down its left side, one inside the other
// for a quote in a quote, and a code block a box; `quotes` (one a level)
// and `code` carry those text blocks from one paragraph to the next, so the
// paragraphs of one quote or code block (and a quote's inner gaps) share
// them and each is drawn once around them all. A quote or a code block in
// a list item starts at the item's indent.
- (NSParagraphStyle *)markdownParagraphFor:(const MdRun &)r
                                    quotes:(NSMutableArray<NSTextBlock *> *)quotes
                                      code:(NSTextBlock * __strong *)code {
    // Display math on a line of its own: centred when it is typeset, in a
    // code block's box while it shows as TeX.
    if (r.math == 2 && r.heading == 0 && !r.table && ![self typesetFormulaFor:r]) {
        MdRun box = r;
        box.math = 0;
        box.codeBlock = true;
        return [self markdownParagraphFor:box quotes:quotes code:code];
    }
    AppSettings *cfg = [AppSettings shared];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineSpacing = 3.0;
    if (r.math == 2 && r.heading == 0 && !r.table) {
        ps.alignment = NSTextAlignmentCenter;
        ps.lineSpacing = 0;
        ps.paragraphSpacingBefore = 4;
        ps.paragraphSpacing = 4;
    }
    NSMutableArray<NSTextBlock *> *blocks = [NSMutableArray array];
    const CGFloat step = MCZoomed(kMarkdownListStep);
    // The list levels outside the quote (for its bar) and inside it.
    const NSInteger outside = r.quote ? r.quoteIndent : 0;
    const NSInteger levels = MAX(0, r.listDepth - outside);
    const NSUInteger depth = r.quote ? (NSUInteger)MAX(1, r.quoteDepth) : 0;
    while (quotes.count > depth) [quotes removeLastObject];
    while (quotes.count < depth) {
        NSTextBlock *b = MCFullWidthBlock();
        [b setWidth:3 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
               edge:NSRectEdgeMinX];
        [b setBorderColor:Hex(0x4B5563) forEdge:NSRectEdgeMinX];
        [b setWidth:13 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
               edge:NSRectEdgeMinX];
        if (quotes.count == 0 && outside > 0) MCIndentBlock(b, outside * step);
        [quotes addObject:b];
    }
    for (NSTextBlock *b in quotes) MCAddBlock(blocks, b);
    if (r.codeBlock) {
        if (!*code) {
            NSTextBlock *b = MCFullWidthBlock();
            if (levels > 0) MCIndentBlock(b, levels * step);   // at a list item's indent
            b.backgroundColor = MCColor(cfg.settings.markdownCodeBackground());
            [b setWidth:12 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinX];
            [b setWidth:12 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxX];
            [b setWidth:9 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMinY];
            [b setWidth:9 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxY];
            *code = b;
        }
        MCAddBlock(blocks, *code);
        ps.lineSpacing = 2.0;
    } else {
        *code = nil;
    }
    if (r.gap) {
        ps.minimumLineHeight = ps.maximumLineHeight = MCZoomed(kMarkdownGap);
        ps.lineSpacing = 0;
    } else if (r.heading > 0) {
        ps.paragraphSpacingBefore = r.heading <= 2 ? 10.0 : 4.0;
        ps.paragraphSpacing = 2.0;
        if (r.heading <= 2) {   // a line under the two biggest, as GitHub has
            NSTextBlock *b = MCFullWidthBlock();
            [b setWidth:1 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
                   edge:NSRectEdgeMaxY];
            [b setBorderColor:Hex(0x3F3F46) forEdge:NSRectEdgeMaxY];
            [b setWidth:6 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                   edge:NSRectEdgeMaxY];
            [blocks addObject:b];
        }
    } else if (r.rule) {
        NSTextBlock *b = MCFullWidthBlock();
        [b setWidth:2 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder
               edge:NSRectEdgeMaxY];
        [b setBorderColor:Hex(0x4B5563) forEdge:NSRectEdgeMaxY];
        [blocks addObject:b];
        ps.minimumLineHeight = ps.maximumLineHeight = 4;
        ps.lineSpacing = 0;
    }
    if (levels > 0 && !r.codeBlock) {
        // The text of every line sits at the level's indent; a marker is
        // right-aligned just before it, so numbers of any width line up.
        const CGFloat text = levels * step;
        ps.headIndent = text;
        ps.firstLineHeadIndent = r.marker ? text - step : text;
        ps.tabStops = @[
            [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentRight
                                            location:text - 7 options:@{}],
            [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                            location:text options:@{}],
        ];
        ps.paragraphSpacing = 3.0;
    }
    ps.textBlocks = blocks;
    return ps;
}

// One run of rendered Markdown, set in the paragraph style `ps`. Inside a
// table cell (`inCell`) the text is set in the body font, since a real
// table lines its columns up without monospace.
- (NSAttributedString *)markdownRun:(const MdRun &)r
                              style:(NSParagraphStyle *)ps
                             inCell:(BOOL)inCell {
    NSString *s = [NSString stringWithUTF8String:r.text.c_str()];
    if (!s) return nil;

    // Sizes at no zoom; Zoom In and Out scale the preview with the editor.
    NSFont *body = [NSFont systemFontOfSize:MCZoomed(15)];
    NSFont *mono = MCMonoFont(MCZoomed(13), NSFontWeightRegular);
    AppSettings *cfg = [AppSettings shared];
    NSFont *font = body;
    NSColor *color = [cfg text:Surface::Editor];
    NSMutableDictionary *a = [NSMutableDictionary dictionary];

    if (r.heading > 0) {
        CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
        font = [NSFont boldSystemFontOfSize:MCZoomed(sizes[r.heading])];
        color = [cfg markdown:MarkdownColor::Heading];
    }
    if (r.footnotes) {   // the footnotes at the page's end: smaller, muted
        font = [NSFont systemFontOfSize:MCZoomed(13)];
        color = [cfg markdown:MarkdownColor::Quote];
    }
    if (r.footnote > 0) {   // a reference to one: a raised number
        font = [NSFont systemFontOfSize:MCZoomed(r.footnotes ? 9 : 11)];
        a[NSBaselineOffsetAttributeName] = @(MCZoomed(r.footnotes ? 4 : 6));
    }
    // Display math on a line of its own, shown as TeX, sits in a code box.
    const BOOL boxedMath = r.math == 2 && r.heading == 0 && !r.table;
    if (r.codeBlock || r.code) {
        font = mono;
        color = [cfg markdown:MarkdownColor::Code];
        if (!r.codeBlock && !boxedMath)   // a code block's box is its paragraphs' text block
            a[NSBackgroundColorAttributeName] =
                MCColor(cfg.settings.markdownCodeBackground());
    }
    if (r.table && !inCell) {
        font = mono;   // monospace keeps the padded columns aligned
        if (!r.code) color = r.bold ? [cfg markdown:MarkdownColor::Heading]
                                    : [cfg text:Surface::Editor];
    }
    if (r.table && inCell && r.bold && !r.code)
        color = [cfg markdown:MarkdownColor::Heading];
    if (r.quote && !r.code) color = [cfg markdown:MarkdownColor::Quote];
    if (r.marker) {
        // "  • " from the parser: the tabs of the paragraph style place it.
        NSString *m = [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        s = [NSString stringWithFormat:@"\t%@\t", m];
    }
    // A line break inside a paragraph, not the start of a new one.
    if (r.hardBreak) s = @" ";
    if (r.rule) font = [NSFont systemFontOfSize:2];
    if (r.strike)
        a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    if (r.link) { color = [cfg markdown:MarkdownColor::Link]; a[NSUnderlineStyleAttributeName] =
        @(NSUnderlineStyleSingle); }
    // A footnote's number and its way back are links without the line.
    if (r.footnote > 0 || (r.footnotes && r.url.rfind("#fnref-", 0) == 0))
        [a removeObjectForKey:NSUnderlineStyleAttributeName];
    // A link's target as written; textView:clickedOnLink: resolves it.
    NSString *href = r.link && !r.url.empty()
                         ? [NSString stringWithUTF8String:r.url.c_str()] : nil;
    if (href.length) a[NSLinkAttributeName] = href;
    if (r.line >= 0) a[kMarkdownSourceLine] = @(r.line);
    // A checked task's text: muted and struck through.
    if (r.task == 2 && !r.marker) {
        color = [cfg markdown:MarkdownColor::Done];
        a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    }
    // A task's marker is a box to click, placed by the tabs as a bullet is.
    if (r.marker && r.task && r.line >= 0) {
        MCTaskBox *att = [MCTaskBox new];
        att.box = [MCTaskBoxCell new];
        att.box.checked = r.task == 2;
        att.box.ink = [cfg text:Surface::Editor];
        att.box.fill = [cfg markdown:MarkdownColor::Link];
        NSMutableDictionary *ta = [a mutableCopy];
        ta[NSFontAttributeName] = font;
        ta[NSForegroundColorAttributeName] = color;
        ta[NSParagraphStyleAttributeName] = ps;
        NSMutableAttributedString *m =
            [[NSMutableAttributedString alloc] initWithString:@"\t" attributes:ta];
        NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
            initWithAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
        [pic addAttributes:ta range:NSMakeRange(0, pic.length)];
        [pic addAttribute:NSLinkAttributeName
                    value:[kTaskLinkPrefix stringByAppendingFormat:@"%d", r.line]
                    range:NSMakeRange(0, pic.length)];
        [pic addAttribute:NSToolTipAttributeName
                    value:r.task == 2 ? @"Done: click to clear" : @"Click to tick"
                    range:NSMakeRange(0, pic.length)];
        [m appendAttributedString:pic];
        [m appendAttributedString:[[NSAttributedString alloc] initWithString:@"\t"
                                                                  attributes:ta]];
        return m;
    }
    if (!ps) ps = [NSParagraphStyle defaultParagraphStyle];
    if (r.math && cfg.settings.math()) {
        // Typeset, a picture on the baseline in the color the text around
        // it has. Otherwise its TeX in the code style: while it is being
        // typeset (it is asked for once the page is built), or with TeX's
        // complaint on hover when it failed.
        const BOOL display = r.math == 2;
        MCMathFormula *f = _mathOn ? MCMathLookup(s, display) : nil;
        if (f && !f.failed) {
            NSColor *ink = [cfg text:Surface::Editor];
            NSFont *around = body;
            if (r.heading > 0) {
                const CGFloat sizes[7] = {0, 26, 22, 19, 17, 15, 14};
                around = [NSFont boldSystemFontOfSize:MCZoomed(sizes[r.heading])];
                ink = [cfg markdown:MarkdownColor::Heading];
            }
            if (r.table && r.bold) ink = [cfg markdown:MarkdownColor::Heading];
            if (r.quote) ink = [cfg markdown:MarkdownColor::Quote];
            if (r.link) ink = [cfg markdown:MarkdownColor::Link];
            MCMathAttachment *att = [[MCMathAttachment alloc]
                initWithFormula:f scale:MCMathScaleFor(around.pointSize) color:ink font:around];
            NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
                initWithAttributedString:[NSAttributedString attributedStringWithAttachment:att]];
            NSMutableDictionary *pa = [NSMutableDictionary dictionary];
            pa[NSParagraphStyleAttributeName] = ps;
            pa[NSFontAttributeName] = around;
            if (href.length) pa[NSLinkAttributeName] = href;
            if (r.line >= 0) pa[kMarkdownSourceLine] = @(r.line);
            [pic addAttributes:pa range:NSMakeRange(0, pic.length)];
            return pic;
        }
        if (f.failed) {
            if (f.transient) _mathRunProblem = f.error;
            a[NSToolTipAttributeName] = f.error.length ? f.error : @"TeX could not typeset this.";
            // A dotted red line under it says there is something to hover.
            a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle | NSUnderlinePatternDot);
            a[NSUnderlineColorAttributeName] = Hex(0xF14C4C);
        } else if (_mathOn) {
            NSString *key = MCMathKey(s, display);
            [_awaitedMath addObject:key];
            [_mathWanted addObject:@[s, @(display)]];
            a[kMarkdownMath] = @[key, @(++_mathMarks)];
        }
    }
    if (r.image) {
        NSString *src = [NSString stringWithUTF8String:r.src.c_str()];
        NSImage *picture = MCMarkdownLoadImage(src,
            self.currentPath.stringByDeletingLastPathComponent);
        // An https picture comes from the cache, or is fetched in the
        // background while its alt text stands in; the page is rendered
        // again when it arrives (markdownWebImageArrived:).
        BOOL waiting = NO;
        if (!picture && MCMarkdownIsWebImage(src) && cfg.settings.webImages())
            picture = MCMarkdownWebImage(src, &waiting);
        if (waiting) {
            [_awaitedWebImages addObject:src];
            a[kMarkdownWebImage] = @[src, @(++_webImageMarks)];
        }
        if (picture) {
            MCMarkdownImage *att = [[MCMarkdownImage alloc] initWithPicture:picture];
            NSMutableAttributedString *pic = [[NSMutableAttributedString alloc]
                initWithAttributedString:
                    [NSAttributedString attributedStringWithAttachment:att]];
            [pic addAttribute:NSParagraphStyleAttributeName value:ps
                        range:NSMakeRange(0, pic.length)];
            if (href.length)
                [pic addAttribute:NSLinkAttributeName value:href range:NSMakeRange(0, pic.length)];
            if (r.line >= 0)
                [pic addAttribute:kMarkdownSourceLine value:@(r.line)
                            range:NSMakeRange(0, pic.length)];
            return pic;
        }
        // Missing, still on its way, or a web picture that may not or could
        // not be fetched: its alt text, muted.
        if (!r.link) color = Hex(0x9CA3AF);
        if (s.length == 0) s = src;
    }

    NSFontManager *fm = [NSFontManager sharedFontManager];
    if (r.bold) font = [fm convertFont:font toHaveTrait:NSBoldFontMask];
    if (r.italic) font = [fm convertFont:font toHaveTrait:NSItalicFontMask];

    a[NSFontAttributeName] = font;
    a[NSForegroundColorAttributeName] = color;
    a[NSParagraphStyleAttributeName] = ps;
    return [[NSAttributedString alloc] initWithString:s attributes:a];
}

// How wide a table cell's content must be (its widest word or formula, which
// cannot wrap) and would like to be (all of it on one line).
static void MCMeasureCell(NSAttributedString *s, CGFloat *least, CGFloat *most) {
    *least = *most = 0;
    if (s.length == 0) return;
    *most = ceil([s boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX) options:0].size.width);
    NSString *str = s.string;
    NSCharacterSet *space = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    const NSUInteger n = str.length;
    for (NSUInteger i = 0; i < n;) {
        while (i < n && [space characterIsMember:[str characterAtIndex:i]]) i++;
        NSUInteger j = i;
        while (j < n && ![space characterIsMember:[str characterAtIndex:j]]) j++;
        if (j > i) {
            NSAttributedString *word = [s attributedSubstringFromRange:NSMakeRange(i, j - i)];
            *least = MAX(*least, ceil([word boundingRectWithSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)
                                                        options:0].size.width));
        }
        i = j;
    }
}

// Each column's share of a table `available` points wide, as percentages,
// laid out the way a browser lays out a table: every column gets its widest
// piece that cannot wrap, and the room left over goes to the columns by how
// much wider their text would like to be. `chrome` is a cell's padding and
// border. Empty when the pane is too narrow to bother.
static std::vector<CGFloat> MCTableColumnWidths(NSArray<NSAttributedString *> *cells, int rows,
                                                int cols, CGFloat available, CGFloat chrome) {
    std::vector<CGFloat> lo((size_t)cols, 0), hi((size_t)cols, 0);
    for (int row = 0; row < rows; row++)
        for (int col = 0; col < cols; col++) {
            CGFloat least = 0, most = 0;
            MCMeasureCell(cells[(NSUInteger)(row * cols + col)], &least, &most);
            // A point to spare, for the rounding of percentages.
            lo[(size_t)col] = MAX(lo[(size_t)col], least + 1);
            hi[(size_t)col] = MAX(hi[(size_t)col], most + 1);
        }
    const CGFloat room = available - cols * chrome;
    if (room <= 40) return {};
    CGFloat sumLo = 0, sumHi = 0;
    for (int c = 0; c < cols; c++) {
        sumLo += lo[(size_t)c];
        sumHi += hi[(size_t)c];
    }
    std::vector<CGFloat> pct((size_t)cols);
    for (int c = 0; c < cols; c++) {
        const size_t k = (size_t)c;
        CGFloat w;
        if (sumHi <= room)         // everything fits on one line: share out the rest
            w = sumHi > 0 ? hi[k] + (room - sumHi) * hi[k] / sumHi : room / cols;
        else if (sumLo >= room)    // not even the widest pieces fit: scale them all
            w = lo[k] * room / sumLo;
        else
            w = lo[k] + (room - sumLo) * (hi[k] - lo[k]) / (sumHi - sumLo);
        pct[k] = w / available * 100;
    }
    return pct;
}

// A Markdown table as an NSTextTable: every cell is a paragraph of its own
// that wraps inside its column, so a table wider than the pane stays a
// table. Columns are sized to their content by AppKit, the header row is
// shaded, and each column keeps the alignment its separator row gave it.
// The parser's padding and rules (tableCol -1) are for monospace ports and
// are skipped here.
//
// AppKit's sizing reads text and knows nothing of an attachment's width, so
// a table holding typeset formulas gets its column widths from
// MCTableColumnWidths instead: formulas cannot wrap, and AppKit would make
// the columns equal and leave them to be scaled down.
- (void)appendMarkdownTable:(const std::vector<MdRun> &)runs
                       from:(size_t)first
                         to:(size_t)end
                       into:(NSMutableAttributedString *)out {
    const int cols = runs[first].tableCols;
    int rows = 0;
    for (size_t k = first; k < end; k++) rows = std::max(rows, runs[k].tableRow + 1);
    if (cols <= 0 || rows <= 0) return;

    AppSettings *cfg = [AppSettings shared];
    NSDictionary *plain = @{NSFontAttributeName: [NSFont systemFontOfSize:MCZoomed(15)],
                            NSForegroundColorAttributeName: [cfg text:Surface::Editor]};
    if (out.length && ![out.string hasSuffix:@"\n"])
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                    attributes:plain]];

    NSTextTable *table = [[NSTextTable alloc] init];
    table.numberOfColumns = (NSUInteger)cols;
    table.layoutAlgorithm = NSTextTableAutomaticLayoutAlgorithm;
    table.collapsesBorders = YES;
    table.hidesEmptyCells = NO;
    NSColor *border = Hex(0x444444);
    NSColor *headerBackground = MCColor(cfg.settings.markdownCodeBackground());
    const NSTextAlignment alignments[3] = {NSTextAlignmentLeft, NSTextAlignmentCenter,
                                           NSTextAlignmentRight};

    // The cells' contents first, so the columns can be measured.
    NSMutableArray<NSMutableAttributedString *> *contents = [NSMutableArray array];
    std::vector<int> aligns((size_t)(rows * cols), 0);
    BOOL typesetMath = NO;
    NSParagraphStyle *interim = [NSParagraphStyle defaultParagraphStyle];
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
            for (size_t k = first; k < end; k++) {
                const MdRun &r = runs[k];
                if (r.tableRow != row || r.tableCol != col) continue;
                aligns[(size_t)(row * cols + col)] = r.tableAlign;
                NSAttributedString *piece = [self markdownRun:r style:interim inCell:YES];
                if (!piece) continue;
                [text appendAttributedString:piece];
                if (r.math && piece.length == 1 &&
                    [[piece attribute:NSAttachmentAttributeName atIndex:0 effectiveRange:NULL]
                        isKindOfClass:MCMathAttachment.class])
                    typesetMath = YES;
            }
            [contents addObject:text];
        }
    }
    // Each cell pads its text by 8 points a side and has a 1 point border.
    static const CGFloat kCellChrome = 17;
    std::vector<CGFloat> widths;
    if (typesetMath)
        widths = MCTableColumnWidths(contents, rows, cols,
                                     self.textView.textContainer.size.width -
                                         2 * self.textView.textContainer.lineFragmentPadding,
                                     kCellChrome);

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            NSTextTableBlock *cell =
                [[NSTextTableBlock alloc] initWithTable:table startingRow:row rowSpan:1
                                         startingColumn:col columnSpan:1];
            [cell setWidth:1 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockBorder];
            [cell setBorderColor:border];
            [cell setWidth:8 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMinX];
            [cell setWidth:8 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMaxX];
            [cell setWidth:4 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMinY];
            [cell setWidth:4 type:NSTextBlockAbsoluteValueType forLayer:NSTextBlockPadding
                      edge:NSRectEdgeMaxY];
            if (row == 0) cell.backgroundColor = headerBackground;
            if (!widths.empty())
                [cell setValue:widths[(size_t)col] type:NSTextBlockPercentageValueType
                  forDimension:NSTextBlockWidth];

            NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
            ps.textBlocks = @[cell];
            ps.lineSpacing = 2.0;
            const int align = aligns[(size_t)(row * cols + col)];
            ps.alignment = alignments[std::min(std::max(align, 0), 2)];
            NSMutableAttributedString *text = contents[(NSUInteger)(row * cols + col)];
            NSMutableDictionary *end = [plain mutableCopy];
            end[NSParagraphStyleAttributeName] = ps;
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                         attributes:end]];
            [text addAttribute:NSParagraphStyleAttributeName value:ps
                         range:NSMakeRange(0, text.length)];
            [text addAttribute:kMarkdownSourceLine value:@(runs[first].line)
                         range:NSMakeRange(0, text.length)];
            [text addAttribute:kMarkdownTableCell value:@[@(row), @(col)]
                         range:NSMakeRange(0, text.length)];
            [out appendAttributedString:text];
        }
    }
}

@end
